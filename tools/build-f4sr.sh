#!/usr/bin/env bash
# f4sr build via CMake (vcpkg + CommonLibF4 as subproject). Idempotent.
set -euo pipefail
trap 'rc=$?; echo "F4SR-BUILD-DONE rc=$rc"' EXIT
source "$(dirname "$0")/env.sh"
cd "$(dirname "$0")/.."
build=build
triplet=x64-windows-clang
if [[ ${1:-} == --staticdeps ]]; then
  build=build-sf
  triplet=x64-windows-clang-staticdeps
  shift
fi
target=${1:-f4sr}
if [[ $# -gt 1 || ( $target != f4sr && $target != ffx-replay ) ]]; then
  echo 'Usage: tools/build-f4sr.sh [--staticdeps] [f4sr|ffx-replay]' >&2
  exit 2
fi
ARGS=(-S . -B "$build" -G Ninja
  -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_TARGET_TRIPLET="$triplet"
  -DVCPKG_OVERLAY_TRIPLETS="$F4SR_ROOT/tools/triplets"
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$F4SR_ROOT/cmake/chainload-main.cmake"
  -DVCPKG_MANIFEST_DIR="$F4SR_ROOT/third_party/CommonLibF4"
  -DVCPKG_MANIFEST_FEATURES=
  -DCMAKE_BUILD_TYPE=Release)
rm -f -- "$build/CMakeCache.txt"
cmake "${ARGS[@]}"
python3 tools/fix-vcpkg-dlls.py "$build/vcpkg_installed/$triplet"
cmake "${ARGS[@]}"
cmake --build "$build" -j 4 --target "$target"
