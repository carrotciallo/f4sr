# source this before configuring/building (fish: use bash)
F4SR_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export INCLUDE="$F4SR_ROOT/tools/msvc/crt/include;$F4SR_ROOT/tools/msvc/sdk/include/ucrt;$F4SR_ROOT/tools/msvc/sdk/include/um;$F4SR_ROOT/tools/msvc/sdk/include/shared"
export LIB="$F4SR_ROOT/tools/msvc/crt/lib/x86_64;$F4SR_ROOT/tools/msvc/sdk/lib/um/x86_64;$F4SR_ROOT/tools/msvc/sdk/lib/ucrt/x86_64"
export VCPKG_ROOT="$F4SR_ROOT/tools/vcpkg"
