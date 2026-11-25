#!/usr/bin/env bash
# Assemble the installable package under package/ (Data tree layout).
set -eu
cd "$HOME/muse/f4sr"
P=package/Data/F4SE/Plugins
cp build/f4sr.dll "$P/"
SDK=sdk/FidelityFX-SDK/Kits/FidelityFX/signedbin
cp "$SDK/amd_fidelityfx_loader_dx12.dll" "$P/"
cp "$SDK/amd_fidelityfx_upscaler_dx12.dll" "$P/"
cp "$SDK/amd_fidelityfx_framegeneration_dx12.dll" "$P/"
cp sdk/xess/bin/libxess.dll "$P/"
# Runtime DLLs from the same build tree f4sr.dll was linked against.
VB=build/vcpkg_installed/x64-windows-clang/bin
cp "$VB/fmt.dll" package/
cp "$VB/spdlog.dll" package/
echo "PACKAGE-DONE"
find package -type f | sort
