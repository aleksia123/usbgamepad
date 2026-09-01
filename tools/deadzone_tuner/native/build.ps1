# Builds deadzone_native.dll from deadzone_api.cpp/deadzone.hpp.
# Requires a MinGW-w64 g++ on PATH (e.g. MSYS2's ucrt64 toolchain).
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

g++ -shared -O2 -std=c++17 -static -static-libgcc -static-libstdc++ `
    -o deadzone_native.dll deadzone_api.cpp `
    "-Wl,--out-implib,libdeadzone_native.a"

Write-Output "Built $PSScriptRoot\deadzone_native.dll"
