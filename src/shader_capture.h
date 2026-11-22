#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <format>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "F4SE/F4SE.h"

namespace f4sr::shader_capture
{
	// Diagnostics only. Keeps the bytecode of every pixel shader created on
	// the system D3D11 device, keyed by the returned object, so a traced frame
	// can write out the shaders of ENB's internal passes (whose sources are
	// not on disk) for offline disassembly.
	using CreatePs = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

	struct State
	{
		CreatePs original{};
		std::mutex lock;
		std::unordered_map<const void*, std::vector<unsigned char>> pixel;
		std::unordered_set<const void*> written;
		std::atomic<bool> installed{}, started{};
	};
	inline State g;

	inline HRESULT STDMETHODCALLTYPE OnCreatePixelShader(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out)
	{
		const HRESULT hr = g.original(self, code, size, linkage, out);
		if (SUCCEEDED(hr) && out && *out && code && size) {
			const auto* b = static_cast<const unsigned char*>(code);
			std::scoped_lock l(g.lock);
			g.pixel[*out].assign(b, b + size);
		}
		return hr;
	}

	// Creating a device needs the process to be fully initialised: F4SE loads
	// plugins while the main thread is still inside process start-up, and a
	// D3D11/Vulkan device created there deadlocks (the driver's worker threads
	// cannot start until start-up finishes). So the probe device is made on a
	// helper thread, which only runs once start-up is done and still well
	// before the game creates its renderer.
	inline DWORD WINAPI InstallWorker(LPVOID)
	{
		wchar_t dir[MAX_PATH]{};
		const auto len = GetSystemDirectoryW(dir, MAX_PATH);
		if (!len || len >= MAX_PATH) return 0;
		HMODULE mod = LoadLibraryW((std::wstring(dir) + L"\\d3d11.dll").c_str());
		if (!mod) { F4SE::log::warn("f4sr: shader capture: system d3d11 not loaded"); return 0; }
		auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(mod, "D3D11CreateDevice"));
		Microsoft::WRL::ComPtr<ID3D11Device> dev;
		if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr)) || !dev) {
			F4SE::log::warn("f4sr: shader capture: no probe device");
			return 0;
		}
		auto** vtable = *reinterpret_cast<void***>(dev.Get());
		DWORD old{};
		if (!VirtualProtect(&vtable[15], sizeof(void*), PAGE_READWRITE, &old)) return 0;
		g.original = reinterpret_cast<CreatePs>(vtable[15]);
		vtable[15] = reinterpret_cast<void*>(&OnCreatePixelShader);
		VirtualProtect(&vtable[15], sizeof(void*), old, &old);
		g.installed.store(true, std::memory_order_release);
		F4SE::log::info("f4sr: shader capture installed on the system device");
		return 0;
	}

	// Safe to call from F4SEPlugin_Load: never creates a device on the calling
	// thread and never waits.
	inline void Install()
	{
		if (g.started.exchange(true)) return;
		if (HANDLE t = CreateThread(nullptr, 0, &InstallWorker, nullptr, 0, nullptr)) CloseHandle(t);
		else F4SE::log::warn("f4sr: shader capture: no helper thread");
	}

	// Writes the bytecode of `ps` once per process; returns the file name or
	// an empty string.
	inline std::string Write(const void* ps)
	{
		if (!g.installed.load(std::memory_order_acquire) || !ps) return {};
		std::vector<unsigned char> code;
		{
			std::scoped_lock l(g.lock);
			if (!g.written.insert(ps).second) return std::format("f4sr-shader-{}-{:p}.dxbc", GetCurrentProcessId(), ps);
			auto it = g.pixel.find(ps);
			if (it == g.pixel.end()) return {};
			code = it->second;
		}
		const auto name = std::format("f4sr-shader-{}-{:p}.dxbc", GetCurrentProcessId(), ps);
		if (auto* f = std::fopen(name.c_str(), "wb")) {
			std::fwrite(code.data(), 1, code.size(), f);
			std::fclose(f);
			return name;
		}
		return {};
	}
}
