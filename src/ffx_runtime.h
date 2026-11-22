#pragma once

#include <cstdint>

#include "ffx_api_loader.h"

namespace f4sr
{
	// Provider debug-message sink (ffxApiMessage): forwards FidelityFX
	// validation complaints to the plugin log.
	void FfxLogMessage(std::uint32_t type, const wchar_t* message);

	// Loads amd_fidelityfx_loader_dx12.dll from the f4sr.dll directory and
	// resolves the ffx-api entry points into a process-wide table.
	// Returns false (and logs) when the loader DLL is missing/unloadable;
	// every FFX context operation requires this to have succeeded.
	// The resolved ffx-api entry points (valid after LoadFfxRuntime()).
	const ffxFunctions& Ffx();
	bool LoadFfxRuntime();
	bool FfxRuntimeReady();

	// Enumerates the upscale provider's versions on the given ID3D12Device
	// and returns the versionId for the requested line:
	// backend 0 -> FSR 3.1.x, backend 1 -> FSR 4.1.x.
	// Returns 0 (= failure; callers must not silently select a default) when enumeration fails or
	// no version matches; outName receives the matched display name.
	std::uint64_t PickUpscaleVersion(void* d3d12Device, int backend, const char** outName);
	std::uint64_t PickFrameGenerationVersion(void* d3d12Device, const char** outName);
}
