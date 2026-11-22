#!/usr/bin/env bash
# f4sr build via CMake (vcpkg + CommonLibF4 as subproject). Idempotent.
set -euo pipefail
trap 'rc=$?; echo "F4SR-BUILD-DONE rc=$rc"' EXIT
source "$(dirname "$0")/env.sh"
cd "$(dirname "$0")/.."
ARGS=(-S . -B build -G Ninja
  -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_TARGET_TRIPLET=x64-windows-clang
  -DVCPKG_OVERLAY_TRIPLETS="$F4SR_ROOT/tools/triplets"
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$F4SR_ROOT/cmake/chainload-main.cmake"
  -DVCPKG_MANIFEST_DIR="$F4SR_ROOT/third_party/CommonLibF4"
  -DVCPKG_MANIFEST_FEATURES=
  -DCMAKE_BUILD_TYPE=Release)
rm -f build/CMakeCache.txt
cmake "${ARGS[@]}"
python3 tools/fix-vcpkg-dlls.py
cmake "${ARGS[@]}" || exit 1
case "${1:-f4sr}" in
  f4sr|ffx-replay) cmake --build build -j 4 --target "${1:-f4sr}" ;;
  *) echo 'expected f4sr or ffx-replay' >&2; exit 2 ;;
esac
