# Keep fmt and spdlog private to f4sr.dll so the MO2 package needs no
# loose DLLs next to the game executable. All other linkage, including
# the dynamic CRT, follows the base triplet.
include("${CMAKE_CURRENT_LIST_DIR}/x64-windows-clang.cmake")
if(PORT STREQUAL "fmt" OR PORT STREQUAL "spdlog")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()
