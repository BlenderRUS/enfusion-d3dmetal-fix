// dxgi_fix.cpp - DXGI proxy DLL for Enfusion games (Arma Reforger) on D3DMetal (CrossOver, macOS)
// Copyright 2026 Grigorii S. (BlenderRUS), https://github.com/BlenderRUS/enfusion-d3dmetal-fix
// SPDX-License-Identifier: Apache-2.0
//
// Based on dxgi_hook.cpp from enfusion-dxgi-fix by vec715 (https://github.com/vec715/enfusion-dxgi-fix),
// which fixes EnumOutputs / display mode enumeration. Changes in this version (see NOTICE):
//   * vertex input layout fix: D3DMetal resolves D3D12_APPEND_ALIGNED_ELEMENT offsets wrongly,
//     so Enfusion reads skinning weights/indices and UVs from the wrong bytes (giant stretched
//     polygons, noise textures). The DLL resolves the offsets itself and passes explicit values.
//   * logging is off unless ENF_LOG=1.
//
// Build (macOS, mingw-w64 from Homebrew):  ./build.sh

#define WIN32_LEAN_AND_MEAN
#define DXGI_NO_EXPORTS  // Prevent SDK from declaring the exports
#include <windows.h>

// We need to define DXGI interfaces but not import the factory functions
// So we include the headers but will use our own function implementations
#include <dxgi1_6.h>
#include <d3d12.h>
#include <stdio.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <vector>
using std::min;
using std::max;

using Microsoft::WRL::ComPtr;

// Function pointer types for the real DXGI functions
typedef HRESULT(WINAPI* PFN_CREATE_DXGI_FACTORY)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CREATE_DXGI_FACTORY1)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CREATE_DXGI_FACTORY2)(UINT Flags, REFIID riid, void** ppFactory);

// Debug logging
static FILE* g_logFile = nullptr;

static int g_logEnabled = -1;  // ENF_LOG=1 writes dxgi_hook.log next to the game

void Log(const char* fmt, ...) {
    if (g_logEnabled < 0) {
        char v[8];
        g_logEnabled = GetEnvironmentVariableA("ENF_LOG", v, sizeof(v)) && v[0] == '1';
    }
    if (!g_logEnabled) return;
    if (!g_logFile) {
        g_logFile = fopen("dxgi_hook.log", "a");
    }
    if (g_logFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
        va_end(args);
    }
}

// Original DLL handle and functions
static HMODULE g_realDXGI = nullptr;

static PFN_CREATE_DXGI_FACTORY g_realCreateDXGIFactory = nullptr;
static PFN_CREATE_DXGI_FACTORY1 g_realCreateDXGIFactory1 = nullptr;
static PFN_CREATE_DXGI_FACTORY2 g_realCreateDXGIFactory2 = nullptr;

// ============================================================================
// Fake IDXGIOutput Implementation
// ============================================================================

class FakeOutput : public IDXGIOutput6 {
private:
    LONG m_refCount = 1;
    IDXGIAdapter* m_parentAdapter;
    IDXGIOutput* m_realOutput;  // Can be null for fully fake output
    
public:
    FakeOutput(IDXGIAdapter* parent, IDXGIOutput* realOutput = nullptr) 
        : m_parentAdapter(parent), m_realOutput(realOutput) {
        if (m_parentAdapter) m_parentAdapter->AddRef();
        // Don't AddRef realOutput - we take ownership
        Log("FakeOutput created, wrapping real output: %p", realOutput);
    }
    
    ~FakeOutput() {
        if (m_parentAdapter) m_parentAdapter->Release();
        if (m_realOutput) m_realOutput->Release();
        Log("FakeOutput destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIOutput) ||
            riid == __uuidof(IDXGIOutput1) ||
            riid == __uuidof(IDXGIOutput2) ||
            riid == __uuidof(IDXGIOutput3) ||
            riid == __uuidof(IDXGIOutput4) ||
            riid == __uuidof(IDXGIOutput5) ||
            riid == __uuidof(IDXGIOutput6)) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }
    
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        if (m_realOutput) return m_realOutput->SetPrivateData(Name, DataSize, pData);
        return E_NOTIMPL;
    }
    
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        if (m_realOutput) return m_realOutput->SetPrivateDataInterface(Name, pUnknown);
        return E_NOTIMPL;
    }
    
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        if (m_realOutput) return m_realOutput->GetPrivateData(Name, pDataSize, pData);
        return E_NOTIMPL;
    }
    
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        if (m_parentAdapter) {
            return m_parentAdapter->QueryInterface(riid, ppParent);
        }
        return E_NOINTERFACE;
    }

    // IDXGIOutput
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) override {
        if (!pDesc) return E_POINTER;
        
        Log("FakeOutput::GetDesc called");
        
        // Try real output first
        if (m_realOutput) {
            HRESULT hr = m_realOutput->GetDesc(pDesc);
            Log("Real GetDesc returned 0x%08X", hr);
            if (SUCCEEDED(hr)) {
                Log("  DeviceName: %ls", pDesc->DeviceName);
                Log("  DesktopCoordinates: %d,%d - %d,%d", 
                    pDesc->DesktopCoordinates.left, pDesc->DesktopCoordinates.top,
                    pDesc->DesktopCoordinates.right, pDesc->DesktopCoordinates.bottom);
                Log("  AttachedToDesktop: %d", pDesc->AttachedToDesktop);
                Log("  Rotation: %d", pDesc->Rotation);
                Log("  Monitor: %p", pDesc->Monitor);
                
                // FIX: Ensure valid values
                if (pDesc->DesktopCoordinates.right == 0 || pDesc->DesktopCoordinates.bottom == 0) {
                    Log("  FIXING: Invalid desktop coordinates, setting to 3440x1440");
                    pDesc->DesktopCoordinates.left = 0;
                    pDesc->DesktopCoordinates.top = 0;
                    pDesc->DesktopCoordinates.right = 3440;
                    pDesc->DesktopCoordinates.bottom = 1440;
                }
                if (!pDesc->AttachedToDesktop) {
                    Log("  FIXING: AttachedToDesktop was FALSE, setting to TRUE");
                    pDesc->AttachedToDesktop = TRUE;
                }
                return hr;
            }
        }
        
        // Provide fake data
        Log("Providing fake GetDesc data");
        ZeroMemory(pDesc, sizeof(*pDesc));
        wcscpy_s(pDesc->DeviceName, L"\\\\.\\DISPLAY1");
        pDesc->DesktopCoordinates.left = 0;
        pDesc->DesktopCoordinates.top = 0;
        pDesc->DesktopCoordinates.right = 3440;
        pDesc->DesktopCoordinates.bottom = 1440;
        pDesc->AttachedToDesktop = TRUE;
        pDesc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        pDesc->Monitor = (HMONITOR)0x10001;  // Fake but non-null monitor handle
        
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, 
                                                  UINT* pNumModes, DXGI_MODE_DESC* pDesc) override {
        Log("FakeOutput::GetDisplayModeList format=%d flags=%u pDesc=%p pNumModes=%u", 
            EnumFormat, Flags, pDesc, pNumModes ? *pNumModes : 0);
        
        // Save original buffer size before calling real output
        UINT originalNumModes = pNumModes ? *pNumModes : 0;
        
        // Try real output first
        if (m_realOutput) {
            HRESULT hr = m_realOutput->GetDisplayModeList(EnumFormat, Flags, pNumModes, pDesc);
            Log("Real GetDisplayModeList returned 0x%08X, numModes=%u", hr, pNumModes ? *pNumModes : 0);
            if (SUCCEEDED(hr) && *pNumModes > 0) {
                return hr;
            }
            Log("Real output returned no modes for format %d, providing fake ones", EnumFormat);
        }
        
        // Generate fake modes for the REQUESTED format
        DXGI_MODE_DESC modes[7];
        
        modes[0] = { 3440, 1440, {180, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        modes[1] = { 3440, 1440, {60, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        modes[2] = { 2560, 1440, {180, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        modes[3] = { 2560, 1440, {60, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        modes[4] = { 1920, 1080, {60, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        modes[5] = { 1920, 1080, {120, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        modes[6] = { 1280, 720, {60, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED };
        
        const UINT numModes = 7;
        
        if (!pDesc) {
            *pNumModes = numModes;
            Log("  Returning fake mode count: %u", numModes);
            return S_OK;
        }
        
        // Use the ORIGINAL buffer size, not the one real output stomped on
        UINT count = min(originalNumModes, numModes);
        if (count == 0) count = numModes;  // Fallback if original was also 0
        memcpy(pDesc, modes, count * sizeof(DXGI_MODE_DESC));
        *pNumModes = count;
        Log("  Returned %u fake modes for format %d (buffer was %u)", count, EnumFormat, originalNumModes);
        
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch,
                                                       DXGI_MODE_DESC* pClosestMatch,
                                                       IUnknown* pConcernedDevice) override {
        Log("FakeOutput::FindClosestMatchingMode %ux%u @ %u/%u", 
            pModeToMatch->Width, pModeToMatch->Height,
            pModeToMatch->RefreshRate.Numerator, pModeToMatch->RefreshRate.Denominator);
        
        if (!pClosestMatch) return E_POINTER;
        
        // Try real output first
        if (m_realOutput) {
            HRESULT hr = m_realOutput->FindClosestMatchingMode(pModeToMatch, pClosestMatch, pConcernedDevice);
            Log("Real FindClosestMatchingMode returned 0x%08X", hr);
            if (SUCCEEDED(hr)) {
                Log("  Result: %ux%u @ %u/%u", 
                    pClosestMatch->Width, pClosestMatch->Height,
                    pClosestMatch->RefreshRate.Numerator, pClosestMatch->RefreshRate.Denominator);
                return hr;
            }
        }
        
        // Return the requested mode as-is, or default to 1920x1080
        *pClosestMatch = *pModeToMatch;
        if (pClosestMatch->Width == 0) pClosestMatch->Width = 1920;
        if (pClosestMatch->Height == 0) pClosestMatch->Height = 1080;
        if (pClosestMatch->RefreshRate.Numerator == 0) {
            pClosestMatch->RefreshRate.Numerator = 60;
            pClosestMatch->RefreshRate.Denominator = 1;
        }
        if (pClosestMatch->Format == DXGI_FORMAT_UNKNOWN) {
            pClosestMatch->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        pClosestMatch->ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override {
        Log("FakeOutput::WaitForVBlank");
        if (m_realOutput) return m_realOutput->WaitForVBlank();
        Sleep(16); // ~60fps
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* pDevice, BOOL Exclusive) override {
        Log("FakeOutput::TakeOwnership device=%p exclusive=%d", pDevice, Exclusive);
        if (m_realOutput) return m_realOutput->TakeOwnership(pDevice, Exclusive);
        return S_OK;
    }
    
    void STDMETHODCALLTYPE ReleaseOwnership() override {
        Log("FakeOutput::ReleaseOwnership");
        if (m_realOutput) m_realOutput->ReleaseOwnership();
    }
    
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) override {
        Log("FakeOutput::GetGammaControlCapabilities");
        if (m_realOutput) return m_realOutput->GetGammaControlCapabilities(pGammaCaps);
        if (!pGammaCaps) return E_POINTER;
        ZeroMemory(pGammaCaps, sizeof(*pGammaCaps));
        pGammaCaps->ScaleAndOffsetSupported = FALSE;
        pGammaCaps->MaxConvertedValue = 1.0f;
        pGammaCaps->MinConvertedValue = 0.0f;
        pGammaCaps->NumGammaControlPoints = 256;
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) override {
        Log("FakeOutput::SetGammaControl");
        if (m_realOutput) return m_realOutput->SetGammaControl(pArray);
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* pArray) override {
        Log("FakeOutput::GetGammaControl");
        if (m_realOutput) return m_realOutput->GetGammaControl(pArray);
        if (!pArray) return E_POINTER;
        ZeroMemory(pArray, sizeof(*pArray));
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface* pScanoutSurface) override {
        Log("FakeOutput::SetDisplaySurface");
        if (m_realOutput) return m_realOutput->SetDisplaySurface(pScanoutSurface);
        return E_NOTIMPL;
    }
    
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface* pDestination) override {
        Log("FakeOutput::GetDisplaySurfaceData");
        if (m_realOutput) return m_realOutput->GetDisplaySurfaceData(pDestination);
        return E_NOTIMPL;
    }
    
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
        Log("FakeOutput::GetFrameStatistics");
        if (m_realOutput) return m_realOutput->GetFrameStatistics(pStats);
        if (!pStats) return E_POINTER;
        ZeroMemory(pStats, sizeof(*pStats));
        return S_OK;
    }

    // IDXGIOutput1
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags,
                                                   UINT* pNumModes, DXGI_MODE_DESC1* pDesc) override {
        Log("FakeOutput::GetDisplayModeList1 format=%d flags=%u pDesc=%p pNumModes=%u", 
            EnumFormat, Flags, pDesc, pNumModes ? *pNumModes : 0);
        
        // Save original buffer size before calling real output
        UINT originalNumModes = pNumModes ? *pNumModes : 0;
        
        // Try real output first
        if (m_realOutput) {
            ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(m_realOutput->QueryInterface(IID_PPV_ARGS(&output1)))) {
                HRESULT hr = output1->GetDisplayModeList1(EnumFormat, Flags, pNumModes, pDesc);
                Log("Real GetDisplayModeList1 returned 0x%08X, numModes=%u", hr, pNumModes ? *pNumModes : 0);
                if (SUCCEEDED(hr) && *pNumModes > 0) {
                    return hr;
                }
            }
            Log("Real output returned no modes for format %d, providing fake ones", EnumFormat);
        }
        
        // Generate fake modes for the REQUESTED format
        DXGI_MODE_DESC1 modes[3];
        
        modes[0] = { 3440, 1440, {180, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED, FALSE };
        modes[1] = { 3440, 1440, {60, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED, FALSE };
        modes[2] = { 1920, 1080, {60, 1}, EnumFormat, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED, FALSE };
        
        const UINT numModes = 3;
        
        if (!pDesc) {
            *pNumModes = numModes;
            Log("  Returning fake mode count: %u", numModes);
            return S_OK;
        }
        
        // Use the ORIGINAL buffer size
        UINT count = min(originalNumModes, numModes);
        if (count == 0) count = numModes;
        memcpy(pDesc, modes, count * sizeof(DXGI_MODE_DESC1));
        *pNumModes = count;
        Log("  Returned %u fake modes for format %d (buffer was %u)", count, EnumFormat, originalNumModes);
        
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* pModeToMatch,
                                                        DXGI_MODE_DESC1* pClosestMatch,
                                                        IUnknown* pConcernedDevice) override {
        Log("FakeOutput::FindClosestMatchingMode1");
        if (!pClosestMatch) return E_POINTER;
        
        if (m_realOutput) {
            ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(m_realOutput->QueryInterface(IID_PPV_ARGS(&output1)))) {
                HRESULT hr = output1->FindClosestMatchingMode1(pModeToMatch, pClosestMatch, pConcernedDevice);
                if (SUCCEEDED(hr)) return hr;
            }
        }
        
        *pClosestMatch = *pModeToMatch;
        if (pClosestMatch->Width == 0) pClosestMatch->Width = 1920;
        if (pClosestMatch->Height == 0) pClosestMatch->Height = 1080;
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource* pDestination) override {
        Log("FakeOutput::GetDisplaySurfaceData1");
        if (m_realOutput) {
            ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(m_realOutput->QueryInterface(IID_PPV_ARGS(&output1)))) {
                return output1->GetDisplaySurfaceData1(pDestination);
            }
        }
        return E_NOTIMPL;
    }
    
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown* pDevice, IDXGIOutputDuplication** ppOutputDuplication) override {
        Log("FakeOutput::DuplicateOutput");
        if (m_realOutput) {
            ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(m_realOutput->QueryInterface(IID_PPV_ARGS(&output1)))) {
                return output1->DuplicateOutput(pDevice, ppOutputDuplication);
            }
        }
        return E_NOTIMPL;
    }

    // IDXGIOutput2
    BOOL STDMETHODCALLTYPE SupportsOverlays() override {
        Log("FakeOutput::SupportsOverlays");
        return FALSE;
    }

    // IDXGIOutput3
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT EnumFormat, IUnknown* pConcernedDevice,
                                                   UINT* pFlags) override {
        Log("FakeOutput::CheckOverlaySupport");
        if (pFlags) *pFlags = 0;
        return S_OK;
    }

    // IDXGIOutput4
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT Format,
                                                            DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                            IUnknown* pConcernedDevice,
                                                            UINT* pFlags) override {
        Log("FakeOutput::CheckOverlayColorSpaceSupport");
        if (pFlags) *pFlags = 0;
        return S_OK;
    }

    // IDXGIOutput5
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown* pDevice, UINT Flags,
                                                UINT SupportedFormatsCount,
                                                const DXGI_FORMAT* pSupportedFormats,
                                                IDXGIOutputDuplication** ppOutputDuplication) override {
        Log("FakeOutput::DuplicateOutput1");
        return E_NOTIMPL;
    }

    // IDXGIOutput6
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* pDesc) override {
        if (!pDesc) return E_POINTER;
        
        Log("FakeOutput::GetDesc1 called");
        
        // Try real output first  
        if (m_realOutput) {
            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(m_realOutput->QueryInterface(IID_PPV_ARGS(&output6)))) {
                HRESULT hr = output6->GetDesc1(pDesc);
                Log("Real GetDesc1 returned 0x%08X", hr);
                if (SUCCEEDED(hr)) {
                    Log("  DeviceName: %ls", pDesc->DeviceName);
                    Log("  DesktopCoordinates: %d,%d - %d,%d", 
                        pDesc->DesktopCoordinates.left, pDesc->DesktopCoordinates.top,
                        pDesc->DesktopCoordinates.right, pDesc->DesktopCoordinates.bottom);
                    Log("  AttachedToDesktop: %d", pDesc->AttachedToDesktop);
                    Log("  Monitor: %p", pDesc->Monitor);
                    Log("  BitsPerColor: %u", pDesc->BitsPerColor);
                    
                    // FIX: Ensure valid values
                    if (pDesc->DesktopCoordinates.right == 0 || pDesc->DesktopCoordinates.bottom == 0) {
                        Log("  FIXING: Invalid desktop coordinates");
                        pDesc->DesktopCoordinates.right = 3440;
                        pDesc->DesktopCoordinates.bottom = 1440;
                    }
                    if (!pDesc->AttachedToDesktop) {
                        Log("  FIXING: AttachedToDesktop");
                        pDesc->AttachedToDesktop = TRUE;
                    }
                    if (pDesc->BitsPerColor == 0) {
                        Log("  FIXING: BitsPerColor");
                        pDesc->BitsPerColor = 8;
                    }
                    return hr;
                }
            }
        }
        
        Log("Providing fake GetDesc1 data");
        ZeroMemory(pDesc, sizeof(*pDesc));
        wcscpy_s(pDesc->DeviceName, L"\\\\.\\DISPLAY1");
        pDesc->DesktopCoordinates.left = 0;
        pDesc->DesktopCoordinates.top = 0;
        pDesc->DesktopCoordinates.right = 3440;
        pDesc->DesktopCoordinates.bottom = 1440;
        pDesc->AttachedToDesktop = TRUE;
        pDesc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        pDesc->Monitor = (HMONITOR)0x10001;
        pDesc->BitsPerColor = 8;
        pDesc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        pDesc->RedPrimary[0] = 0.640f; pDesc->RedPrimary[1] = 0.330f;
        pDesc->GreenPrimary[0] = 0.300f; pDesc->GreenPrimary[1] = 0.600f;
        pDesc->BluePrimary[0] = 0.150f; pDesc->BluePrimary[1] = 0.060f;
        pDesc->WhitePoint[0] = 0.3127f; pDesc->WhitePoint[1] = 0.3290f;
        pDesc->MinLuminance = 0.0f;
        pDesc->MaxLuminance = 300.0f;
        pDesc->MaxFullFrameLuminance = 300.0f;
        
        return S_OK;
    }
    
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* pFlags) override {
        Log("FakeOutput::CheckHardwareCompositionSupport");
        if (pFlags) *pFlags = 0;
        return S_OK;
    }
};

// ============================================================================
// Adapter Wrapper - Intercepts EnumOutputs
// ============================================================================

class AdapterWrapper : public IDXGIAdapter4 {
private:
    IDXGIAdapter* m_realAdapter;
    LONG m_refCount = 1;
    
public:
    AdapterWrapper(IDXGIAdapter* real) : m_realAdapter(real) {
        Log("AdapterWrapper created for %p", real);
    }
    
    ~AdapterWrapper() {
        Log("AdapterWrapper destroyed");
        if (m_realAdapter) m_realAdapter->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIAdapter) ||
            riid == __uuidof(IDXGIAdapter1) ||
            riid == __uuidof(IDXGIAdapter2) ||
            riid == __uuidof(IDXGIAdapter3) ||
            riid == __uuidof(IDXGIAdapter4)) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        
        // Forward unknown interfaces to real adapter
        return m_realAdapter->QueryInterface(riid, ppvObject);
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }
    
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_realAdapter->SetPrivateData(Name, DataSize, pData);
    }
    
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_realAdapter->SetPrivateDataInterface(Name, pUnknown);
    }
    
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_realAdapter->GetPrivateData(Name, pDataSize, pData);
    }
    
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return m_realAdapter->GetParent(riid, ppParent);
    }

    // IDXGIAdapter - THE KEY HOOK
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override {
        Log("AdapterWrapper::EnumOutputs called with index %u", Output);
        
        // Try real adapter first
        IDXGIOutput* realOutput = nullptr;
        HRESULT hr = m_realAdapter->EnumOutputs(Output, &realOutput);
        
        Log("Real EnumOutputs returned 0x%08X", hr);
        
        if (SUCCEEDED(hr) && realOutput) {
            // Wrap the real output to fix/log its data
            Log("Wrapping real output %p", realOutput);
            *ppOutput = new FakeOutput(m_realAdapter, realOutput);
            return S_OK;
        }
        
        // If real adapter has no outputs and we're asking for output 0, create fake
        if (hr == DXGI_ERROR_NOT_FOUND && Output == 0) {
            Log("Creating fully fake output for index 0");
            *ppOutput = new FakeOutput(m_realAdapter, nullptr);
            return S_OK;
        }
        
        *ppOutput = nullptr;
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override {
        Log("AdapterWrapper::GetDesc called");
        HRESULT hr = m_realAdapter->GetDesc(pDesc);
        if (SUCCEEDED(hr) && pDesc) {
            Log("  Description: %ls", pDesc->Description);
            Log("  VendorId=0x%X DeviceId=0x%X SubSysId=0x%X Revision=%u", 
                pDesc->VendorId, pDesc->DeviceId, pDesc->SubSysId, pDesc->Revision);
            Log("  DedicatedVideoMemory=%llu DedicatedSystemMemory=%llu SharedSystemMemory=%llu",
                pDesc->DedicatedVideoMemory, pDesc->DedicatedSystemMemory, pDesc->SharedSystemMemory);
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override {
        Log("AdapterWrapper::CheckInterfaceSupport called");
        HRESULT hr = m_realAdapter->CheckInterfaceSupport(InterfaceName, pUMDVersion);
        Log("  CheckInterfaceSupport returned 0x%08X", hr);
        return hr;
    }

    // IDXGIAdapter1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override {
        Log("AdapterWrapper::GetDesc1 called");
        ComPtr<IDXGIAdapter1> adapter1;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter1));
        if (SUCCEEDED(hr)) {
            hr = adapter1->GetDesc1(pDesc);
            if (SUCCEEDED(hr) && pDesc) {
                Log("  Description: %ls", pDesc->Description);
                Log("  VendorId=0x%X DeviceId=0x%X Flags=0x%X", 
                    pDesc->VendorId, pDesc->DeviceId, pDesc->Flags);
                Log("  DedicatedVideoMemory=%llu SharedSystemMemory=%llu",
                    pDesc->DedicatedVideoMemory, pDesc->SharedSystemMemory);
            }
        }
        return hr;
    }

    // IDXGIAdapter2
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) override {
        ComPtr<IDXGIAdapter2> adapter2;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter2));
        if (SUCCEEDED(hr)) {
            return adapter2->GetDesc2(pDesc);
        }
        return hr;
    }

    // IDXGIAdapter3
    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(
        HANDLE hEvent, DWORD* pdwCookie) override {
        ComPtr<IDXGIAdapter3> adapter3;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        if (SUCCEEDED(hr)) {
            return adapter3->RegisterHardwareContentProtectionTeardownStatusEvent(hEvent, pdwCookie);
        }
        return hr;
    }
    
    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) override {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            adapter3->UnregisterHardwareContentProtectionTeardownStatus(dwCookie);
        }
    }
    
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex,
                                                    DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                    DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) override {
        ComPtr<IDXGIAdapter3> adapter3;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        if (SUCCEEDED(hr)) {
            return adapter3->QueryVideoMemoryInfo(NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex,
                                                         DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                         UINT64 Reservation) override {
        ComPtr<IDXGIAdapter3> adapter3;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        if (SUCCEEDED(hr)) {
            return adapter3->SetVideoMemoryReservation(NodeIndex, MemorySegmentGroup, Reservation);
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(
        HANDLE hEvent, DWORD* pdwCookie) override {
        ComPtr<IDXGIAdapter3> adapter3;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        if (SUCCEEDED(hr)) {
            return adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(hEvent, pdwCookie);
        }
        return hr;
    }
    
    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) override {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            adapter3->UnregisterVideoMemoryBudgetChangeNotification(dwCookie);
        }
    }

    // IDXGIAdapter4
    HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* pDesc) override {
        ComPtr<IDXGIAdapter4> adapter4;
        HRESULT hr = m_realAdapter->QueryInterface(IID_PPV_ARGS(&adapter4));
        if (SUCCEEDED(hr)) {
            return adapter4->GetDesc3(pDesc);
        }
        return hr;
    }
};

// ============================================================================
// Factory Wrapper - Wraps adapters
// ============================================================================

class FactoryWrapper : public IDXGIFactory7 {
private:
    IDXGIFactory* m_realFactory;
    LONG m_refCount = 1;
    
public:
    FactoryWrapper(IDXGIFactory* real) : m_realFactory(real) {
        Log("FactoryWrapper created for %p", real);
    }
    
    ~FactoryWrapper() {
        Log("FactoryWrapper destroyed");
        if (m_realFactory) m_realFactory->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) ||
            riid == __uuidof(IDXGIFactory2) ||
            riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) ||
            riid == __uuidof(IDXGIFactory5) ||
            riid == __uuidof(IDXGIFactory6) ||
            riid == __uuidof(IDXGIFactory7)) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        
        return m_realFactory->QueryInterface(riid, ppvObject);
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }
    
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_realFactory->SetPrivateData(Name, DataSize, pData);
    }
    
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_realFactory->SetPrivateDataInterface(Name, pUnknown);
    }
    
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_realFactory->GetPrivateData(Name, pDataSize, pData);
    }
    
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return m_realFactory->GetParent(riid, ppParent);
    }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override {
        Log("FactoryWrapper::EnumAdapters index %u", Adapter);
        
        IDXGIAdapter* realAdapter = nullptr;
        HRESULT hr = m_realFactory->EnumAdapters(Adapter, &realAdapter);
        
        if (SUCCEEDED(hr) && realAdapter) {
            *ppAdapter = new AdapterWrapper(realAdapter);
            Log("Wrapped adapter %u", Adapter);
        } else {
            if (ppAdapter) *ppAdapter = nullptr;
        }
        
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override {
        return m_realFactory->MakeWindowAssociation(WindowHandle, Flags);
    }
    
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override {
        return m_realFactory->GetWindowAssociation(pWindowHandle);
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                               IDXGISwapChain** ppSwapChain) override {
        Log("FactoryWrapper::CreateSwapChain device=%p", pDevice);
        if (pDesc) {
            Log("  BufferDesc: %ux%u format=%d refresh=%u/%u", 
                pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->BufferDesc.Format,
                pDesc->BufferDesc.RefreshRate.Numerator, pDesc->BufferDesc.RefreshRate.Denominator);
            Log("  SampleDesc: count=%u quality=%u", pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality);
            Log("  BufferUsage=0x%X BufferCount=%u OutputWindow=%p", 
                pDesc->BufferUsage, pDesc->BufferCount, pDesc->OutputWindow);
            Log("  Windowed=%d SwapEffect=%d Flags=0x%X", 
                pDesc->Windowed, pDesc->SwapEffect, pDesc->Flags);
        }
        HRESULT hr = m_realFactory->CreateSwapChain(pDevice, pDesc, ppSwapChain);
        Log("  CreateSwapChain returned 0x%08X", hr);
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override {
        return m_realFactory->CreateSoftwareAdapter(Module, ppAdapter);
    }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override {
        Log("FactoryWrapper::EnumAdapters1 index %u", Adapter);
        
        ComPtr<IDXGIFactory1> factory1;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory1));
        if (FAILED(hr)) return hr;
        
        IDXGIAdapter1* realAdapter = nullptr;
        hr = factory1->EnumAdapters1(Adapter, &realAdapter);
        
        if (SUCCEEDED(hr) && realAdapter) {
            *ppAdapter = (IDXGIAdapter1*)new AdapterWrapper(realAdapter);
            Log("Wrapped adapter1 %u", Adapter);
        } else {
            if (ppAdapter) *ppAdapter = nullptr;
        }
        
        return hr;
    }
    
    BOOL STDMETHODCALLTYPE IsCurrent() override {
        ComPtr<IDXGIFactory1> factory1;
        if (SUCCEEDED(m_realFactory->QueryInterface(IID_PPV_ARGS(&factory1)))) {
            return factory1->IsCurrent();
        }
        return TRUE;
    }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override {
        ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
            return factory2->IsWindowedStereoEnabled();
        }
        return FALSE;
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd,
                                                      const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                      IDXGIOutput* pRestrictToOutput,
                                                      IDXGISwapChain1** ppSwapChain) override {
        Log("FactoryWrapper::CreateSwapChainForHwnd device=%p hwnd=%p output=%p", pDevice, hWnd, pRestrictToOutput);
        if (pDesc) {
            Log("  Size: %ux%u format=%d stereo=%d", 
                pDesc->Width, pDesc->Height, pDesc->Format, pDesc->Stereo);
            Log("  SampleDesc: count=%u quality=%u", pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality);
            Log("  BufferUsage=0x%X BufferCount=%u Scaling=%d SwapEffect=%d AlphaMode=%d Flags=0x%X", 
                pDesc->BufferUsage, pDesc->BufferCount, pDesc->Scaling, 
                pDesc->SwapEffect, pDesc->AlphaMode, pDesc->Flags);
        }
        if (pFullscreenDesc) {
            Log("  Fullscreen: refresh=%u/%u scanline=%d scaling=%d windowed=%d",
                pFullscreenDesc->RefreshRate.Numerator, pFullscreenDesc->RefreshRate.Denominator,
                pFullscreenDesc->ScanlineOrdering, pFullscreenDesc->Scaling, pFullscreenDesc->Windowed);
        }
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) {
            Log("  Failed to get Factory2: 0x%08X", hr);
            return hr;
        }
        hr = factory2->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
        Log("  CreateSwapChainForHwnd returned 0x%08X swapchain=%p", hr, ppSwapChain ? *ppSwapChain : nullptr);
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                            const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }
    
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->GetSharedResourceAdapterLuid(hResource, pLuid);
    }
    
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
    }
    
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->RegisterStereoStatusEvent(hEvent, pdwCookie);
    }
    
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override {
        ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
            factory2->UnregisterStereoStatus(dwCookie);
        }
    }
    
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
    }
    
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
    }
    
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override {
        ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
            factory2->UnregisterOcclusionStatus(dwCookie);
        }
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice,
                                                             const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                             IDXGIOutput* pRestrictToOutput,
                                                             IDXGISwapChain1** ppSwapChain) override {
        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        if (FAILED(hr)) return hr;
        return factory2->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    }

    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override {
        ComPtr<IDXGIFactory3> factory3;
        if (SUCCEEDED(m_realFactory->QueryInterface(IID_PPV_ARGS(&factory3)))) {
            return factory3->GetCreationFlags();
        }
        return 0;
    }

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override {
        ComPtr<IDXGIFactory4> factory4;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory4));
        if (FAILED(hr)) return hr;
        
        IDXGIAdapter* realAdapter = nullptr;
        hr = factory4->EnumAdapterByLuid(AdapterLuid, __uuidof(IDXGIAdapter), (void**)&realAdapter);
        if (SUCCEEDED(hr) && realAdapter) {
            AdapterWrapper* wrapper = new AdapterWrapper(realAdapter);
            hr = wrapper->QueryInterface(riid, ppvAdapter);
            wrapper->Release();
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override {
        ComPtr<IDXGIFactory4> factory4;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory4));
        if (FAILED(hr)) return hr;
        return factory4->EnumWarpAdapter(riid, ppvAdapter);
    }

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                                   UINT FeatureSupportDataSize) override {
        ComPtr<IDXGIFactory5> factory5;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory5));
        if (FAILED(hr)) return hr;
        return factory5->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    }

    // IDXGIFactory6
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter,
                                                          DXGI_GPU_PREFERENCE GpuPreference,
                                                          REFIID riid, void** ppvAdapter) override {
        Log("FactoryWrapper::EnumAdapterByGpuPreference index %u pref %d", Adapter, GpuPreference);
        
        ComPtr<IDXGIFactory6> factory6;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory6));
        if (FAILED(hr)) return hr;
        
        IDXGIAdapter* realAdapter = nullptr;
        hr = factory6->EnumAdapterByGpuPreference(Adapter, GpuPreference, __uuidof(IDXGIAdapter), (void**)&realAdapter);
        if (SUCCEEDED(hr) && realAdapter) {
            AdapterWrapper* wrapper = new AdapterWrapper(realAdapter);
            hr = wrapper->QueryInterface(riid, ppvAdapter);
            wrapper->Release();
            Log("Wrapped adapter by GPU pref %u", Adapter);
        }
        return hr;
    }

    // IDXGIFactory7
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) override {
        ComPtr<IDXGIFactory7> factory7;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory7));
        if (FAILED(hr)) return hr;
        return factory7->RegisterAdaptersChangedEvent(hEvent, pdwCookie);
    }
    
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override {
        ComPtr<IDXGIFactory7> factory7;
        HRESULT hr = m_realFactory->QueryInterface(IID_PPV_ARGS(&factory7));
        if (FAILED(hr)) return hr;
        return factory7->UnregisterAdaptersChangedEvent(dwCookie);
    }
};

// ============================================================================
// DLL Entry Point and Exports
// ============================================================================

#include "input_layout_fix.inc"

BOOL LoadRealDXGI() {
    if (g_realDXGI) return TRUE;
    
    // Try to load the renamed original
    g_realDXGI = LoadLibraryA("dxgi_original.dll");
    if (!g_realDXGI) {
        // Fall back to system32
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\dxgi.dll");
        g_realDXGI = LoadLibraryA(path);
    }
    
    if (!g_realDXGI) {
        Log("ERROR: Could not load real dxgi.dll!");
        return FALSE;
    }
    
    g_realCreateDXGIFactory = (PFN_CREATE_DXGI_FACTORY)GetProcAddress(g_realDXGI, "CreateDXGIFactory");
    g_realCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(g_realDXGI, "CreateDXGIFactory1");
    g_realCreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY2)GetProcAddress(g_realDXGI, "CreateDXGIFactory2");
    
    Log("Loaded real DXGI: Factory=%p Factory1=%p Factory2=%p",
        g_realCreateDXGIFactory, g_realCreateDXGIFactory1, g_realCreateDXGIFactory2);
    
    return TRUE;
}

// ============================================================================
// DLL Entry Point and Exports
// Use Hook_ prefix internally, .def file exports them as the real names
// ============================================================================

extern "C" {

HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    InstallInputLayoutFix();
    Log("CreateDXGIFactory called");
    
    if (!LoadRealDXGI() || !g_realCreateDXGIFactory) {
        return E_FAIL;
    }
    
    IDXGIFactory* realFactory = nullptr;
    HRESULT hr = g_realCreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&realFactory);
    
    if (SUCCEEDED(hr) && realFactory) {
        FactoryWrapper* wrapper = new FactoryWrapper(realFactory);
        hr = wrapper->QueryInterface(riid, ppFactory);
        wrapper->Release();
        Log("Wrapped factory created");
    }
    
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    InstallInputLayoutFix();
    Log("CreateDXGIFactory1 called");
    
    if (!LoadRealDXGI() || !g_realCreateDXGIFactory1) {
        return E_FAIL;
    }
    
    IDXGIFactory1* realFactory = nullptr;
    HRESULT hr = g_realCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&realFactory);
    
    if (SUCCEEDED(hr) && realFactory) {
        FactoryWrapper* wrapper = new FactoryWrapper(realFactory);
        hr = wrapper->QueryInterface(riid, ppFactory);
        wrapper->Release();
        Log("Wrapped factory1 created");
    }
    
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    InstallInputLayoutFix();
    Log("CreateDXGIFactory2 called with flags %u", Flags);
    
    if (!LoadRealDXGI() || !g_realCreateDXGIFactory2) {
        return E_FAIL;
    }
    
    IDXGIFactory2* realFactory = nullptr;
    HRESULT hr = g_realCreateDXGIFactory2(Flags, __uuidof(IDXGIFactory2), (void**)&realFactory);
    
    if (SUCCEEDED(hr) && realFactory) {
        FactoryWrapper* wrapper = new FactoryWrapper(realFactory);
        hr = wrapper->QueryInterface(riid, ppFactory);
        wrapper->Release();
        Log("Wrapped factory2 created");
    }
    
    return hr;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        Log("=== DXGI Hook DLL Loaded ===");
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        Log("=== DXGI Hook DLL Unloaded ===");
        if (g_logFile) fclose(g_logFile);
        if (g_realDXGI) FreeLibrary(g_realDXGI);
        break;
    }
    return TRUE;
}
