# clang-cl + xwin toolchain (proven 2026-11-15: hello DLL compiles and links)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(MSVC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../tools/msvc")
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
add_compile_options(-fuse-ld=lld-link)
include_directories(SYSTEM
  "${MSVC_ROOT}/crt/include"
  "${MSVC_ROOT}/sdk/include/ucrt"
  "${MSVC_ROOT}/sdk/include/um"
  "${MSVC_ROOT}/sdk/include/shared")
link_directories(
  "${MSVC_ROOT}/crt/lib/x86_64"
  "${MSVC_ROOT}/sdk/lib/um/x86_64"
  "${MSVC_ROOT}/sdk/lib/ucrt/x86_64")
