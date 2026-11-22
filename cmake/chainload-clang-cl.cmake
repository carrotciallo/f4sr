# vcpkg chainload: clang-cl + lld-link against the xwin splat.
# vcpkg port builds run in a scrubbed environment, so INCLUDE/LIB env vars do
# not reach them; the splat paths must be injected here.
set(CMAKE_C_COMPILER clang-cl CACHE STRING "")
set(CMAKE_CXX_COMPILER clang-cl CACHE STRING "")
get_filename_component(_F4SR_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_IMSVC "-fuse-ld=lld-link /imsvc${_F4SR_ROOT}/tools/msvc/crt/include /imsvc${_F4SR_ROOT}/tools/msvc/sdk/include/ucrt /imsvc${_F4SR_ROOT}/tools/msvc/sdk/include/um /imsvc${_F4SR_ROOT}/tools/msvc/sdk/include/winrt /imsvc${_F4SR_ROOT}/tools/msvc/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT "${_IMSVC}")
set(CMAKE_CXX_FLAGS_INIT "${_IMSVC}")
set(_LIBPATH "/LIBPATH:${_F4SR_ROOT}/tools/msvc/crt/lib/x86_64 /LIBPATH:${_F4SR_ROOT}/tools/msvc/sdk/lib/um/x86_64 /LIBPATH:${_F4SR_ROOT}/tools/msvc/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_LIBPATH}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_LIBPATH}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_LIBPATH}")
set(CMAKE_STATIC_LINKER_FLAGS_INIT "")
