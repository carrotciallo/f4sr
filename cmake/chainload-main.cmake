set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
# Main-project chainload: clang-cl + lld-link against the xwin splat.
# No /LIBPATH here: the main configure runs under tools/env.sh whose LIB
# env var clang-cl reads natively. (/LIBPATH in linker flags breaks the
# clang-cl driver, which treats it as an input file; vcpkg ports are fine
# with it because they link through vs_link_exe -> lld-link directly.)
set(CMAKE_C_COMPILER clang-cl CACHE STRING "")
set(CMAKE_CXX_COMPILER clang-cl CACHE STRING "")
set(_IMSVC "-fuse-ld=lld-link /imsvc/home/carrot/muse/f4sr/tools/msvc/crt/include /imsvc/home/carrot/muse/f4sr/tools/msvc/sdk/include/ucrt /imsvc/home/carrot/muse/f4sr/tools/msvc/sdk/include/um /imsvc/home/carrot/muse/f4sr/tools/msvc/sdk/include/winrt /imsvc/home/carrot/muse/f4sr/tools/msvc/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT "${_IMSVC}")
set(CMAKE_CXX_FLAGS_INIT "${_IMSVC} -fdelayed-template-parsing -DFMT_CONSTEVAL=")
