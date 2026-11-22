#pragma once

// Swap chain wrappers (ENB's myIDXGISwapChain) keep the chain they forward to
// in one of their fields. ENB applies its effects and advances its per-frame
// state in the wrapper's Present; frame generation presents extra frames on
// the inner chain so the wrapper sees exactly one Present per game frame.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

namespace f4sr::swapchain
{
	inline bool Readable(const void* p, std::size_t bytes)
	{
		if (!p) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
		if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
		const auto end = static_cast<const std::byte*>(mbi.BaseAddress) + mbi.RegionSize;
		return static_cast<const std::byte*>(p) + bytes <= end;
	}

	inline std::string ModulePath(const void* address)
	{
		HMODULE module{};
		char path[MAX_PATH] = "";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(address), &module))
			GetModuleFileNameA(module, path, MAX_PATH);
		return path;
	}

	// The chain a wrapper forwards to: a field of the wrapper that is a COM
	// object with a vtable in a dxgi module (other than the wrapper's own),
	// answers IDXGISwapChain and presents to `window`. Non-owning; nullptr when
	// `outer` is not a wrapper.
	inline IDXGISwapChain* FindInnerChain(IDXGISwapChain* outer, HWND window, int& fieldIndex, std::string& module)
	{
		auto* const* fields = reinterpret_cast<void* const*>(outer);
		const auto outerModule = ModulePath(*reinterpret_cast<void* const*>(outer));
		for (int i = 1; i < 64; ++i) {
			if (!Readable(&fields[i], sizeof(void*))) break;
			void* candidate = fields[i];
			if (candidate == outer || !Readable(candidate, sizeof(void*))) continue;
			void* const* vtable = *reinterpret_cast<void* const* const*>(candidate);
			if (!Readable(vtable, 9 * sizeof(void*))) continue;
			const auto path = ModulePath(vtable);
			if (path.empty() || path == outerModule) continue;
			std::string lower = path;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lower.find("dxgi") == std::string::npos) continue;
			if (ModulePath(vtable[8]).empty()) continue;
			Microsoft::WRL::ComPtr<IDXGISwapChain> chain;
			if (FAILED(static_cast<IUnknown*>(candidate)->QueryInterface(IID_PPV_ARGS(&chain))) || !chain) continue;
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(chain->GetDesc(&desc)) || desc.OutputWindow != window) continue;
			fieldIndex = i;
			module = path;
			return chain.Get();
		}
		return nullptr;
	}
}
