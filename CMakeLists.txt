cmake_minimum_required(VERSION 3.21)
project(f4sr VERSION 1.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# CommonLibF4 brings its own flags/PCH machinery via cmake/common.cmake;
# consuming it as a subproject is the configuration its headers are
# maintained against (direct clang-cl invocation diverges on template
# parsing and diagnostic severities).
add_subdirectory(third_party/CommonLibF4)

add_library(f4sr SHARED src/main.cpp src/ffx_runtime.cpp src/upscaling.cpp src/interop.cpp src/frame_pipeline.cpp src/frame_pipeline_diag.cpp src/proxy_targets.cpp src/hooks.cpp src/framegen.cpp src/xess_upscaling.cpp src/texture_probe.cpp src/fg_overlay.cpp src/late_draw_trace.cpp src/xefg_presenter.cpp src/menu_effects.cpp src/dynres_audit.cpp src/reshade_preui.cpp src/ui_redirect.cpp src/frame_pipeline_pathb.cpp src/scaleform_scale.cpp src/environment_log.cpp src/log_redact.cpp)
target_link_libraries(f4sr PRIVATE d3d11 d3d12 dxgi d3dcompiler gdi32 advapi32 shell32 ole32)
target_include_directories(f4sr PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/sdk/FidelityFX-SDK/Kits/FidelityFX/api/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/sdk/FidelityFX-SDK/Kits/FidelityFX/upscalers/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/sdk/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/sdk/xess/inc"
	"${CMAKE_CURRENT_SOURCE_DIR}/sdk/antilag2"
	"${CMAKE_CURRENT_SOURCE_DIR}/sdk/reshade/include")
target_link_libraries(f4sr PRIVATE CommonLibF4)
target_precompile_headers(f4sr PRIVATE src/PCH.h)

# Offline measurement only; never package this executable in the game.
add_executable(ffx-replay EXCLUDE_FROM_ALL tools/ffx-replay.cpp)
target_include_directories(ffx-replay PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/FidelityFX-SDK/Kits/FidelityFX/api/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/FidelityFX-SDK/Kits/FidelityFX/upscalers/include")
target_link_libraries(ffx-replay PRIVATE d3d12 dxgi)
