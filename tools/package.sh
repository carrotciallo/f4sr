#!/usr/bin/env bash
# Assemble a MO2 archive from an explicit static-deps build directory.
set -euo pipefail

fail() { printf 'package: %s\n' "$*" >&2; exit 1; }
if [[ $# != 2 || $2 != *.zip ]]; then
    echo 'Usage: tools/package.sh STATICDEPS_BUILD_DIR OUTPUT.zip' >&2
    exit 2
fi

build=$(realpath -e -- "$1")
output=$(realpath -m -s -- "$2")
[[ ! -e $output && ! -L $output ]] || fail "refusing to overwrite $output"
cd "$(dirname "$0")/.."

# Inspect all shipped PEs, including delay imports, before publishing the ZIP.
check_dll() {
    local dll=$1 pe export
    pe=$(LC_ALL=C llvm-readobj --file-headers --coff-imports --coff-exports "$dll") || fail "cannot inspect $dll"
    grep -q 'Machine: IMAGE_FILE_MACHINE_AMD64 ' <<< "$pe" || fail "not an x64 PE: $dll"
    if grep -Eiq '^[[:space:]]*Name:[[:space:]]+(fmt|spdlog)\.dll[[:space:]]*$' <<< "$pe"; then
        fail "unexpected dynamic fmt/spdlog dependency in $dll"
    fi
    if [[ ${dll##*/} == f4sr.dll ]]; then
        for export in Load Query Version VersionData; do
            grep -Eq "^[[:space:]]*Name: F4SEPlugin_${export}[[:space:]]*$" <<< "$pe" || fail "missing export F4SEPlugin_${export} in $dll"
        done
    fi
}
check_dll "$build/f4sr.dll"

mkdir -p -- "$(dirname "$output")"
work=$(mktemp -d -- "$(dirname "$output")/.f4sr-package.XXXXXXXX")
trap 'rm -r -- "$work"' EXIT
stage=$work/stage
plugins=$stage/F4SE/Plugins
mcm=$stage/MCM/Config/f4sr
licenses=$stage/licenses
mkdir -p -- "$plugins" "$mcm" "$licenses" "$stage/fomod"

cp -- "$build/f4sr.dll" package/Data/F4SE/Plugins/f4sr.ini "$plugins/"
cp -- package/Data/MCM/Config/f4sr/{config.json,settings.ini} "$mcm/"
cp -- README.md README_CN.md LICENSE "$stage/"
cp -- package/fomod/{info.xml,ModuleConfig.xml} "$stage/fomod/"

amd=sdk/FidelityFX-SDK
intel=sdk/xess
cp -- "$amd"/Kits/FidelityFX/signedbin/amd_fidelityfx_{loader,upscaler,framegeneration}_dx12.dll "$plugins/"
cp -- "$intel"/bin/{libxess,libxess_fg,libxell}.dll "$plugins/"
for dll in "$plugins/"*.dll; do check_dll "$dll"; done

cp -- "$amd/docs/license.md" "$licenses/amd-fidelityfx-LICENSE.md"
cp -- "$amd/3rdpartynotice.md" "$licenses/amd-fidelityfx-third-party.md"
cp -- "$intel/LICENSE.txt" "$licenses/intel-xess-LICENSE.txt"
cp -- "$intel/third-party-programs.txt" "$licenses/intel-xess-third-party.txt"
share=$build/vcpkg_installed/x64-windows-clang-staticdeps/share
for port in fmt spdlog; do
    cp -- "$share/$port/copyright" "$licenses/$port-LICENSE.txt"
done

(cd "$stage" && zip -q -r "$work/artifact.zip" F4SE MCM licenses README.md README_CN.md LICENSE fomod)
unzip -tq "$work/artifact.zip"
ln -T -- "$work/artifact.zip" "$output"
printf 'PACKAGE-DONE %s\n' "$output"
