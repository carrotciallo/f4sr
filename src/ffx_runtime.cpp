#include "ffx_runtime.h"

#include "F4SE/F4SE.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "ffx_framegeneration.h"

namespace
{
	ffxFunctions g_ffx{};
	HMODULE g_loaderModule{ nullptr };
	bool g_ready{ false };
}

namespace f4sr
{
	const ffxFunctions& Ffx()
	{
		return g_ffx;
	}

	bool LoadFfxRuntime()
	{
		if (g_ready) {
			return true;
		}
		// The loader DLL ships next to f4sr.dll (Data/F4SE/Plugins/f4sr/).
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&LoadFfxRuntime), &self);
		std::array<char, MAX_PATH> buf{};
		GetModuleFileNameA(self, buf.data(), MAX_PATH);
		std::string dir{ buf.data() };
		auto pos = dir.find_last_of("\\/");
		if (pos != std::string::npos) {
			dir.resize(pos + 1);
		}
		const std::string dll = dir + "amd_fidelityfx_loader_dx12.dll";
		g_loaderModule = LoadLibraryA(dll.c_str());
		if (g_loaderModule == nullptr) {
			F4SE::log::error("f4sr: failed to load {}", dll);
			return false;
		}
		ffxLoadFunctions(&g_ffx, g_loaderModule);
		if (g_ffx.CreateContext == nullptr || g_ffx.Dispatch == nullptr ||
			g_ffx.Query == nullptr || g_ffx.DestroyContext == nullptr || g_ffx.Configure == nullptr) {
			F4SE::log::error("f4sr: loader DLL missing ffx-api entry points");
			return false;
		}
		g_ready = true;
		F4SE::log::info("f4sr: FidelityFX runtime loaded");
		return true;
	}

	bool FfxRuntimeReady()
	{
		return g_ready;
	}

	static std::uint64_t PickVersion(void* d3d12Device, std::uint64_t type, const char* want, const char** outName)
	{
		if (outName != nullptr) {
			*outName = nullptr;
		}
		if (!g_ready || d3d12Device == nullptr) {
			return 0;
		}
		ffxQueryDescGetVersions query{};
		query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		query.createDescType = type;
		query.device = d3d12Device;
		std::uint64_t count = 0;
		query.outputCount = &count;
		if (Ffx().Query(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0) {
			F4SE::log::error("f4sr: provider enumeration failed type={}", type);
			return 0;
		}
		std::vector<std::uint64_t> ids(count);
		std::vector<const char*> names(count);
		query.versionIds = ids.data();
		query.versionNames = names.data();
		if (Ffx().Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
			return 0;
		}
		for (std::uint64_t i = 0; i < count; ++i) {
			F4SE::log::info("f4sr: provider type={} version={} id={}", type, names[i] ? names[i] : "?", ids[i]);
			if (names[i] != nullptr && std::string{ names[i] }.find(want) != std::string::npos) {
				if (outName != nullptr) {
					*outName = names[i];
				}
				return ids[i];
			}
		}
		F4SE::log::error("f4sr: no provider matching {} for type={}; refusing fallback", want, type);
		return 0;
	}
	std::uint64_t PickUpscaleVersion(void* device, int backend, const char** outName)
	{
		return PickVersion(device, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, backend == 0 ? "3.1.6" : "4.1.2", outName);
	}

	std::uint64_t PickFrameGenerationVersion(void* device, const char** outName)
	{
		return PickVersion(device, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, "3.1.6", outName);
	}
}

namespace f4sr
{
	void FfxLogMessage(std::uint32_t type, const wchar_t* message)
	{
		if (message == nullptr) {
			return;
		}
		std::string narrow;
		for (const wchar_t* p = message; *p != L'\0'; ++p) {
			narrow += (*p >= 0x20 && *p < 0x7F) ? static_cast<char>(*p) : '?';
		}
		F4SE::log::error("f4sr: ffx message (type {}): {}", type, narrow);
	}
}
