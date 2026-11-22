#pragma once

// Swap chain wrappers stack on top of the driver's chain: ENB
// (myIDXGISwapChain, d3d11.dll), ReShade (DXGISwapChain, dxgi.dll or
// d3d11.dll), overlays. Each keeps the chain it forwards to in one of its
// fields.
//
// Frame generation presents its frames directly below ENB: ENB renders its
// effects before the UI and advances per-frame state in its Present, so it
// must run once per game frame. Wrappers below ENB (ReShade) post-process
// what is presented, after the UI, and therefore see every presented frame,
// generated ones included. Without ENB, nothing is bypassed.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

	inline HMODULE ModuleOf(const void* address)
	{
		HMODULE module{};
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(address), &module))
			return nullptr;
		return module;
	}

	inline std::string ModulePath(const void* address)
	{
		char path[MAX_PATH] = "";
		if (HMODULE module = ModuleOf(address)) GetModuleFileNameA(module, path, MAX_PATH);
		return path;
	}

	inline std::string FileName(const std::string& path)
	{
		const auto pos = path.find_last_of("\\/");
		return pos == std::string::npos ? path : path.substr(pos + 1);
	}

	// MSVC RTTI class name of a polymorphic object ("myIDXGISwapChain"), or
	// empty. x64 layout: vtable[-1] -> CompleteObjectLocator {signature 1,
	// offset, cdOffset, typeDescriptor RVA, classDescriptor RVA, self RVA};
	// the TypeDescriptor's name (".?AVname@@") starts at +0x10.
	inline std::string ClassName(const void* object)
	{
		if (!Readable(object, sizeof(void*))) return {};
		auto* const* vtable = *static_cast<void* const* const*>(object);
		if (!Readable(vtable - 1, sizeof(void*))) return {};
		const auto* col = static_cast<const std::uint32_t*>(vtable[-1]);
		if (!Readable(col, 6 * sizeof(std::uint32_t)) || col[0] != 1) return {};
		const auto base = reinterpret_cast<std::uintptr_t>(col) - col[5];
		if (ModuleOf(col) != reinterpret_cast<HMODULE>(base)) return {};
		const char* name = reinterpret_cast<const char*>(base + col[3] + 0x10);
		if (!Readable(name, 4) || std::strncmp(name, ".?AV", 4) != 0) return {};
		std::string out;
		for (const char* p = name + 4; out.size() < 128 && Readable(p, 1) && *p && *p != '@'; ++p) out += *p;
		return out;
	}

	struct Layer
	{
		IDXGISwapChain* chain{};  // non-owning
		int field{ -1 };          // field of the layer above that holds it
		std::string module;
		std::string className;
		bool Enb() const { return className.find("myIDXGISwapChain") != std::string::npos; }
		std::string Describe() const
		{
			return FileName(module) + (className.empty() ? std::string() : "(" + className + ")");
		}
	};

	// The chain a wrapper forwards to: a field of `outer` holding an object
	// whose vtable lies in another module, has the IDXGISwapChain shape (at least 15
	// of its 18 methods in one module), answers IDXGISwapChain and presents to `window`.
	inline IDXGISwapChain* FindInnerChain(IDXGISwapChain* outer, HWND window, int& fieldIndex, std::string& module)
	{
		auto* const* fields = reinterpret_cast<void* const*>(outer);
		const HMODULE outerModule = ModuleOf(*reinterpret_cast<void* const*>(outer));
		for (int i = 1; i < 64; ++i) {
			if (!Readable(&fields[i], sizeof(void*))) break;
			void* candidate = fields[i];
			if (candidate == outer || !Readable(candidate, sizeof(void*))) continue;
			void* const* vtable = *reinterpret_cast<void* const* const*>(candidate);
			if (!Readable(vtable, 18 * sizeof(void*))) continue;
			const HMODULE vtableModule = ModuleOf(vtable);
			if (vtableModule == nullptr || vtableModule == outerModule) continue;
			const HMODULE code = ModuleOf(vtable[0]);
			if (code == nullptr) continue;
			// Other hooks may have replaced a few entries (Present above all).
			int same = 0;
			for (int m = 0; m < 18; ++m) same += ModuleOf(vtable[m]) == code;
			if (same < 15) continue;
			Microsoft::WRL::ComPtr<IDXGISwapChain> chain;
			if (FAILED(static_cast<IUnknown*>(candidate)->QueryInterface(IID_PPV_ARGS(&chain))) || !chain) continue;
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(chain->GetDesc(&desc)) || desc.OutputWindow != window) continue;
			fieldIndex = i;
			module = ModulePath(vtable);
			return chain.Get();
		}
		return nullptr;
	}

	// Outermost (the game's chain) first, driver chain last.
	inline std::vector<Layer> Stack(IDXGISwapChain* outer, HWND window)
	{
		std::vector<Layer> layers;
		Layer top;
		top.chain = outer;
		top.module = ModulePath(*reinterpret_cast<void* const*>(outer));
		top.className = ClassName(outer);
		layers.push_back(top);
		while (layers.size() < 8) {
			Layer next;
			next.chain = FindInnerChain(layers.back().chain, window, next.field, next.module);
			if (next.chain == nullptr) break;
			if (std::any_of(layers.begin(), layers.end(), [&](const Layer& l) { return l.chain == next.chain; })) break;
			next.className = ClassName(next.chain);
			layers.push_back(next);
		}
		return layers;
	}

	inline std::string Describe(const std::vector<Layer>& layers)
	{
		std::string out;
		for (const auto& layer : layers) {
			char address[32]{};
			std::snprintf(address, sizeof(address), "%p", static_cast<void*>(layer.chain));
			out += (out.empty() ? "" : " -> ") + layer.Describe() + " [" + layer.module + " " + address + "]";
		}
		return out;
	}

	// Index of the layer frame generation presents on: the one directly below
	// ENB, or -1 (no ENB, or nothing below it).
	inline int BelowEnb(const std::vector<Layer>& layers)
	{
		for (std::size_t i = 0; i + 1 < layers.size(); ++i)
			if (layers[i].Enb()) return static_cast<int>(i + 1);
		return -1;
	}
}
