// Copyright 2026 Grigorii S. (BlenderRUS), https://github.com/BlenderRUS/enfusion-d3dmetal-fix
// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// Vertex input layout fix (on by default, ENF_INPUT_LAYOUT_FIX=0 disables)
//
// D3DMetal resolves D3D12_APPEND_ALIGNED_ELEMENT offsets differently from D3D12 for some layouts,
// so vertex attributes are fetched from the wrong bytes. Enfusion declares every element as
// APPEND_ALIGNED. This code resolves the offsets per D3D12 rules (directly after the previous
// element of the same input slot, aligned to min(4, element size)) and passes explicit values to
// ID3D12Device::CreateGraphicsPipelineState / CreatePipelineState.
//
// The device is created by D3DMetal in native macOS memory, where VirtualProtect does not work,
// so its vtable cannot be patched in place. Instead the primary vtable is copied (its length is
// found by scanning for code pointers) and the object's vptr is redirected to the copy.
// ============================================================================

static bool InputLayoutFixEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        char v[8];
        enabled = !(GetEnvironmentVariableA("ENF_INPUT_LAYOUT_FIX", v, sizeof(v)) && v[0] == '0');
        Log("[il] input layout fix %s", enabled ? "enabled" : "disabled by ENF_INPUT_LAYOUT_FIX=0");
    }
    return enabled != 0;
}

static UINT DxgiFormatBytes(DXGI_FORMAT f) {
    UINT v = (UINT)f;
    if (v >= 1 && v <= 4) return 16;    // R32G32B32A32
    if (v >= 5 && v <= 8) return 12;    // R32G32B32
    if (v >= 9 && v <= 22) return 8;    // R16G16B16A16, R32G32, R32G8X24
    if (v >= 23 && v <= 47) return 4;   // R10G10B10A2, R11G11B10, R8G8B8A8, R16G16, R32, R24G8
    if (v >= 48 && v <= 59) return 2;   // R8G8, R16
    if (v >= 60 && v <= 65) return 1;   // R8, A8
    if (v == 67 || v == 68 || v == 69) return 4;
    if (v == 85 || v == 86 || v == 115) return 2;
    if (v >= 87 && v <= 93) return 4;   // B8G8R8A8 family
    return 0;
}

// Fills `out` with explicit offsets; returns false if nothing had to change or a format is unknown.
static bool ResolveInputLayout(const D3D12_INPUT_LAYOUT_DESC& il, std::vector<D3D12_INPUT_ELEMENT_DESC>& out) {
    if (!il.pInputElementDescs || il.NumElements == 0) return false;
    bool changed = false;
    UINT next[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    out.assign(il.pInputElementDescs, il.pInputElementDescs + il.NumElements);
    for (auto& e : out) {
        if (e.InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return false;
        UINT size = DxgiFormatBytes(e.Format);
        if (e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT) {
            if (!size) return false;
            UINT align = size >= 4 ? 4 : size;
            e.AlignedByteOffset = (next[e.InputSlot] + align - 1) / align * align;
            changed = true;
        }
        next[e.InputSlot] = e.AlignedByteOffset + size;
    }
    return changed;
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CREATE_GPS)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CREATE_PS)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
typedef HRESULT(WINAPI* PFN_D3D12_CREATE_DEVICE_RAW)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// COM slots of ID3D12Device (fixed by the D3D12 ABI): IUnknown 0-2, ID3D12Object 3-6, then methods.
static const size_t kSlotCreateGraphicsPipelineState = 10;
static const size_t kSlotCreatePipelineState = 47;  // ID3D12Device2

struct DeviceVtable {
    void** orig;      // D3DMetal's vtable
    void** copy;      // our copy (address point)
    size_t slots;
    PFN_CREATE_GPS createGps;
    PFN_CREATE_PS createPs;
};
static DeviceVtable g_devVtables[8];
static int g_devVtableCount = 0;
static SRWLOCK g_devLock = SRWLOCK_INIT;
static PFN_D3D12_CREATE_DEVICE_RAW g_realD3D12CreateDevice = nullptr;
static LONG g_ilFixedCount = 0;

static const DeviceVtable* VtableFor(void* device) {
    void** vt = *(void***)device;
    for (int i = 0; i < g_devVtableCount; i++)
        if (g_devVtables[i].orig == vt || g_devVtables[i].copy == vt) return &g_devVtables[i];
    return nullptr;
}

static void LogFixedLayout(const std::vector<D3D12_INPUT_ELEMENT_DESC>& elems) {
    LONG n = InterlockedIncrement(&g_ilFixedCount);
    if (n > 5) return;
    std::string s;
    char b[96];
    for (const auto& e : elems) {
        snprintf(b, sizeof(b), "%s%s%u@%u", s.empty() ? "" : " ", e.SemanticName ? e.SemanticName : "?", e.SemanticIndex,
                 e.AlignedByteOffset);
        s += b;
    }
    Log("[il] explicit offsets: %s", s.c_str());
}

static HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                                  REFIID riid, void** pp) {
    const DeviceVtable* dv = VtableFor(self);
    if (!dv) return E_FAIL;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
    if (desc && ResolveInputLayout(desc->InputLayout, elems)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC patched = *desc;
        patched.InputLayout.pInputElementDescs = elems.data();
        LogFixedLayout(elems);
        return dv->createGps(self, &patched, riid, pp);
    }
    return dv->createGps(self, desc, riid, pp);
}

// Size of each pipeline state stream subobject (type tag + payload, pointer-aligned).
static size_t StreamSubobjectSize(UINT type) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return 16;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return 24;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return 40;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return 336;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return 8;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return 48;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return 56;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return 24;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return 8;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return 8;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return 40;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return 8;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return 16;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return 8;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return 24;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return 8;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return 64;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return 32;
    default: return 0;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(ID3D12Device* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                                          REFIID riid, void** pp) {
    const DeviceVtable* dv = VtableFor(self);
    if (!dv) return E_FAIL;
    if (desc && desc->pPipelineStateSubobjectStream && desc->SizeInBytes) {
        const unsigned char* p = (const unsigned char*)desc->pPipelineStateSubobjectStream;
        for (size_t off = 0; off + 4 <= desc->SizeInBytes;) {
            UINT type = *(const UINT*)(p + off);
            size_t sz = StreamSubobjectSize(type);
            if (!sz) break;  // unknown subobject: leave the stream as it is
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
                std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
                if (!ResolveInputLayout(*(const D3D12_INPUT_LAYOUT_DESC*)(p + off + 8), elems)) break;
                std::vector<unsigned char> stream(p, p + desc->SizeInBytes);
                ((D3D12_INPUT_LAYOUT_DESC*)(stream.data() + off + 8))->pInputElementDescs = elems.data();
                D3D12_PIPELINE_STATE_STREAM_DESC patched = {stream.size(), stream.data()};
                LogFixedLayout(elems);
                return dv->createPs(self, &patched, riid, pp);
            }
            off += sz;
        }
    }
    return dv->createPs(self, desc, riid, pp);
}

static bool IsCodePointer(ULONG_PTR v, ULONG_PTR vtbl) {
    if (v < 0x100000000ULL || v > 0x00007FFFFFFFFFFFULL) return false;  // small/negative offsets
    ULONG_PTR d = v > vtbl ? v - vtbl : vtbl - v;
    return d >= 0x40000;  // pointers right next to the vtable are data (typeinfo of the next vtable)
}

// Copies the primary vtable including up to 16 words before the address point (offset-to-top,
// typeinfo, virtual base/call offsets). Returns the copy's address point or nullptr.
static void** CloneVtable(void** vtbl, size_t* slotsOut) {
    const size_t kPrefix = 16, kMaxSlots = 4096;
    if (IsBadReadPtr(vtbl - kPrefix, (kPrefix + 1) * sizeof(void*))) return nullptr;
    size_t slots = 0;
    while (slots < kMaxSlots && !IsBadReadPtr(&vtbl[slots], sizeof(void*)) && IsCodePointer((ULONG_PTR)vtbl[slots], (ULONG_PTR)vtbl))
        slots++;
    if (slots <= kSlotCreateGraphicsPipelineState) return nullptr;
    void** mem = (void**)VirtualAlloc(nullptr, (kPrefix + slots) * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return nullptr;
    memcpy(mem, vtbl - kPrefix, (kPrefix + slots) * sizeof(void*));
    *slotsOut = slots;
    return mem + kPrefix;
}

static void HookDevice(void* device) {
    if (!device || !InputLayoutFixEnabled()) return;
    AcquireSRWLockExclusive(&g_devLock);
    void** vt = *(void***)device;
    const DeviceVtable* known = VtableFor(device);
    if (known) {
        if (vt != known->copy) *(void***)device = known->copy;
    } else if (g_devVtableCount < (int)(sizeof(g_devVtables) / sizeof(g_devVtables[0]))) {
        size_t slots = 0;
        if (void** copy = CloneVtable(vt, &slots)) {
            DeviceVtable& d = g_devVtables[g_devVtableCount++];
            d.orig = vt;
            d.copy = copy;
            d.slots = slots;
            d.createGps = (PFN_CREATE_GPS)vt[kSlotCreateGraphicsPipelineState];
            copy[kSlotCreateGraphicsPipelineState] = (void*)&Hook_CreateGraphicsPipelineState;
            if (slots > kSlotCreatePipelineState) {
                d.createPs = (PFN_CREATE_PS)vt[kSlotCreatePipelineState];
                copy[kSlotCreatePipelineState] = (void*)&Hook_CreatePipelineState;
            }
            *(void***)device = copy;
            Log("[il] device %p hooked (vtable %p, %zu slots copied)", device, vt, slots);
        } else {
            Log("[il] device %p: vtable %p not recognised, input layout fix inactive", device, vt);
        }
    }
    ReleaseSRWLockExclusive(&g_devLock);
}

static HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** ppDevice) {
    HRESULT hr = g_realD3D12CreateDevice(adapter, level, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) HookDevice(*ppDevice);
    return hr;
}

// Redirects the game's import of d3d12.dll!D3D12CreateDevice (Enfusion imports it by ordinal).
static void InstallInputLayoutFix() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!InputLayoutFixEnabled()) return;
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryA("d3d12.dll");
    if (d3d12) g_realD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE_RAW)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!g_realD3D12CreateDevice) {
        Log("[il] D3D12CreateDevice not found");
        return;
    }
    auto* base = (BYTE*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.VirtualAddress && imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), "d3d12.dll") != 0) continue;
        for (auto* t = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); t->u1.Function; t++) {
            if ((void*)t->u1.Function != (void*)g_realD3D12CreateDevice) continue;
            DWORD old = 0;
            if (VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                t->u1.Function = (ULONG_PTR)&Hook_D3D12CreateDevice;
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
                patched++;
            }
        }
    }
    Log("[il] D3D12CreateDevice import patched in %d place(s)", patched);
}

