#!/usr/bin/env bash
# CommonLibF4 cross-build driver (clang-cl + xwin via vcpkg). Idempotent.
# The configure runs twice: vcpkg's manifest install lands the ports during
# the first configure, but its DLL fixup points imported targets at
# tools/<port>/<dll> while the DLLs sit in bin/; fix-vcpkg-dlls.py bridges
# that between the two configures.
set -u
source "$(dirname "$0")/env.sh"
cd "$(dirname "$0")/.."
ARGS=(-S third_party/CommonLibF4 -B build-clf4 -G Ninja
  -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_TARGET_TRIPLET=x64-windows-clang
  -DVCPKG_OVERLAY_TRIPLETS="$F4SR_ROOT/tools/triplets"
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$F4SR_ROOT/cmake/chainload-main.cmake"
  -DVCPKG_MANIFEST_FEATURES=
  -DCMAKE_BUILD_TYPE=Release)
rm -f build-clf4/CMakeCache.txt
rm -rf build-clf4/CMakeFiles
cmake "${ARGS[@]}"
python3 tools/fix-vcpkg-dlls.py
cmake "${ARGS[@]}" || exit 1
cmake --build build-clf4 -j 4 --target CommonLibF4
echo "CLF4-BUILD-DONE rc=$?"
