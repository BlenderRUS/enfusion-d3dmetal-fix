#!/bin/zsh
# Builds dist/dxgi.dll with mingw-w64 (brew install mingw-w64).
set -e
cd "$(dirname $0)"
mkdir -p dist
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
    -o dist/dxgi.dll src/dxgi_fix.cpp src/dxgi.def -luuid -ldxguid
x86_64-w64-mingw32-strip dist/dxgi.dll
shasum -a 256 dist/dxgi.dll
