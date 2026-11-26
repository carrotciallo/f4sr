#pragma once

// Video memory telemetry: the process's local (dedicated) and non-local
// (shared) usage against the WDDM budget, logged every 5 s and shown on the
// status panel, plus one-line notes for F4SR's own large allocations so a
// log shows what F4SR holds at the display size. When usage passes the
// budget, the OS starts demoting and paging resources and every pass slows
// down (8 GB cards at 4K output).

#include <atomic>
#include <cstdint>
#include <format>
#include <string>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "F4SE/F4SE.h"

namespace f4sr::vram_budget
{
	struct Sample
	{
		std::uint64_t localUsage{}, localBudget{}, nonLocalUsage{}, nonLocalBudget{};
		bool valid{};
	};

	struct State
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
		HANDLE budgetEvent{};
		DWORD budgetCookie{};
		bool triedInit{}, overBudget{};
		std::uint64_t lastLog{}, budgetChanges{}, peakLocal{}, loggedBudget{};
		std::atomic<std::uint32_t> usageMb{}, budgetMb{};  // panel
	};
	inline State g;

	inline constexpr double kMb = 1024.0 * 1024.0;

	// Bytes per pixel of the formats F4SR allocates (0: unknown).
	inline std::uint32_t BytesPerPixel(DXGI_FORMAT f)
	{
		switch (f) {
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return 4;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R8G8_UNORM: return 2;
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM: return 1;
		default: return 0;
		}
	}

	inline double TextureMb(std::uint32_t w, std::uint32_t h, DXGI_FORMAT f, std::uint32_t count = 1)
	{
		return static_cast<double>(w) * h * BytesPerPixel(f) * count / kMb;
	}

	// One log line per large allocation (at creation, not per frame).
	inline void Note(const char* what, std::uint32_t w, std::uint32_t h, DXGI_FORMAT f, std::uint32_t count = 1)
	{
		F4SE::log::info("f4sr: VRAM: {} {}x{} format {}{} ~{:.1f} MB", what, w, h, static_cast<unsigned>(f),
			count > 1 ? std::format(" x{}", count) : std::string(), TextureMb(w, h, f, count));
	}

	inline void NoteBytes(const char* what, std::uint64_t bytes)
	{
		F4SE::log::info("f4sr: VRAM: {} ~{:.1f} MB", what, static_cast<double>(bytes) / kMb);
	}

	// F4SR's own video memory, counted by F4SR (the OS only reports the
	// whole process): textures and swap chains from their size and format
	// (no driver padding or compression metadata, so a little low), SDK
	// contexts from the SDK's report or the measured usage change. A tracked
	// object carries a tracker as private data; destroying the object
	// releases it, which takes its bytes off the count.
	enum class Kind { Texture, Chain, Sdk, Count };
	inline std::atomic<std::int64_t> g_own[static_cast<int>(Kind::Count)]{};

	inline std::int64_t Own(Kind k) { return g_own[static_cast<int>(k)].load(std::memory_order_relaxed); }
	inline std::int64_t OwnTotal() { return Own(Kind::Texture) + Own(Kind::Chain) + Own(Kind::Sdk); }

	// Pairs an SDK context's bytes with its lifetime (Set on create, Set(0) on destroy).
	class SdkBytes
	{
	public:
		SdkBytes() = default;
		SdkBytes(const SdkBytes&) = delete;
		SdkBytes& operator=(const SdkBytes&) = delete;
		void Set(std::int64_t bytes)
		{
			if (bytes < 0) bytes = 0;
			g_own[static_cast<int>(Kind::Sdk)].fetch_add(bytes - bytes_, std::memory_order_relaxed);
			bytes_ = bytes;
		}
		~SdkBytes() { Set(0); }

	private:
		std::int64_t bytes_{};
	};

	class Tracker final : public IUnknown
	{
	public:
		Tracker(Kind kind, std::int64_t bytes) : kind_(kind), bytes_(bytes)
		{
			g_own[static_cast<int>(kind_)].fetch_add(bytes_, std::memory_order_relaxed);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (!out) return E_POINTER;
			if (riid == __uuidof(IUnknown)) {
				*out = static_cast<IUnknown*>(this);
				AddRef();
				return S_OK;
			}
			*out = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG n = --refs_;
			if (n == 0) {
				g_own[static_cast<int>(kind_)].fetch_sub(bytes_, std::memory_order_relaxed);
				delete this;
			}
			return n;
		}

	private:
		std::atomic<ULONG> refs_{ 1 };
		Kind kind_;
		std::int64_t bytes_;
	};

	// {6B7C1F3E-2E8A-4C1B-9F0D-5A4E8D2C7B11}
	inline constexpr GUID kTrackerGuid{ 0x6b7c1f3e, 0x2e8a, 0x4c1b, { 0x9f, 0x0d, 0x5a, 0x4e, 0x8d, 0x2c, 0x7b, 0x11 } };

	inline void Track(IUnknown* object, Kind kind, std::uint64_t bytes)
	{
		if (!object || !bytes) return;
		auto* tracker = new Tracker(kind, static_cast<std::int64_t>(bytes));
		HRESULT hr = E_NOINTERFACE;
		Microsoft::WRL::ComPtr<ID3D11DeviceChild> child;
		Microsoft::WRL::ComPtr<IDXGIObject> dxgi;
		if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&child)))) hr = child->SetPrivateDataInterface(kTrackerGuid, tracker);
		else if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&dxgi)))) hr = dxgi->SetPrivateDataInterface(kTrackerGuid, tracker);
		(void)hr;     // on failure the count drops back when our reference goes
		tracker->Release();
	}

	inline void TrackTexture(ID3D11Texture2D* tex)
	{
		if (!tex) return;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		Track(tex, Kind::Texture, static_cast<std::uint64_t>(d.Width) * d.Height * BytesPerPixel(d.Format) * d.ArraySize * (d.SampleDesc.Count ? d.SampleDesc.Count : 1));
	}

	inline bool Init(ID3D11Device* device)
	{
		if (g.adapter) return true;
		if (g.triedInit || !device) return false;
		g.triedInit = true;
		Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
			FAILED(adapter.As(&g.adapter))) {
			F4SE::log::warn("f4sr: VRAM: video memory budget unavailable (no IDXGIAdapter3 on the game device)");
			g.adapter.Reset();
			return false;
		}
		g.budgetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (g.budgetEvent && FAILED(g.adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(g.budgetEvent, &g.budgetCookie))) {
			CloseHandle(g.budgetEvent);
			g.budgetEvent = nullptr;
		}
		DXGI_ADAPTER_DESC desc{};
		adapter->GetDesc(&desc);
		F4SE::log::info("f4sr: VRAM: adapter dedicated {:.0f} MB, shared {:.0f} MB; budget change notifications {}",
			static_cast<double>(desc.DedicatedVideoMemory) / kMb, static_cast<double>(desc.SharedSystemMemory) / kMb,
			g.budgetEvent ? "on" : "unavailable");
		return true;
	}

	inline Sample Query()
	{
		Sample s{};
		if (!g.adapter) return s;
		DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
		if (FAILED(g.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
			FAILED(g.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) return s;
		s.localUsage = local.CurrentUsage;
		s.localBudget = local.Budget;
		s.nonLocalUsage = nonLocal.CurrentUsage;
		s.nonLocalBudget = nonLocal.Budget;
		s.valid = true;
		return s;
	}

	inline std::uint64_t LocalUsage() { return Query().localUsage; }

	// Usage change around an SDK context creation. Approximate: other threads
	// (the game streaming) allocate at the same time.
	inline void NoteDelta(const char* what, std::uint64_t before)
	{
		if (!g.adapter || before == 0) return;
		const auto after = LocalUsage();
		F4SE::log::info("f4sr: VRAM: {} changed local usage by {:+.1f} MB (approximate)", what,
			(static_cast<double>(after) - static_cast<double>(before)) / kMb);
	}

	// Game thread, once per frame: 5 s log line, budget change and
	// over-budget warnings, panel values.
	inline void Tick(ID3D11Device* device)
	{
		if (!Init(device)) return;
		const auto now = GetTickCount64();
		const bool changed = g.budgetEvent && WaitForSingleObject(g.budgetEvent, 0) == WAIT_OBJECT_0;
		if (changed) ++g.budgetChanges;
		if (!changed && g.lastLog && now - g.lastLog < 5000) return;
		const auto s = Query();
		if (!s.valid) return;
		g.usageMb.store(static_cast<std::uint32_t>(s.localUsage / (1024 * 1024)), std::memory_order_relaxed);
		g.budgetMb.store(static_cast<std::uint32_t>(s.localBudget / (1024 * 1024)), std::memory_order_relaxed);
		if (s.localUsage > g.peakLocal) g.peakLocal = s.localUsage;
		if (!g.loggedBudget) g.loggedBudget = s.localBudget;
		const double pct = s.localBudget ? 100.0 * static_cast<double>(s.localUsage) / static_cast<double>(s.localBudget) : 0.0;
		const bool over = pct >= 95.0;
		// Proton signals a change every ~1.5 s for drifts of a few MB: log only
		// moves of more than 2 % from the last logged budget.
		const auto drift = s.localBudget > g.loggedBudget ? s.localBudget - g.loggedBudget : g.loggedBudget - s.localBudget;
		if (changed && drift * 50 > g.loggedBudget) {
			g.loggedBudget = s.localBudget;
			F4SE::log::info("f4sr: VRAM: budget changed to {:.0f} MB (local usage {:.0f} MB, {:.0f}%)",
				static_cast<double>(s.localBudget) / kMb, static_cast<double>(s.localUsage) / kMb, pct);
		}
		if (over && !g.overBudget) {
			F4SE::log::warn("f4sr: VRAM: local usage {:.0f} MB is {:.0f}% of the {:.0f} MB budget; the GPU slows down sharply above 100%",
				static_cast<double>(s.localUsage) / kMb, pct, static_cast<double>(s.localBudget) / kMb);
		} else if (!over && g.overBudget && pct < 90.0) {
			F4SE::log::info("f4sr: VRAM: local usage back to {:.0f}% of the budget", pct);
		}
		if (over) g.overBudget = true;
		else if (pct < 90.0) g.overBudget = false;
		if (g.lastLog && now - g.lastLog < 5000) return;
		g.lastLog = now;
		F4SE::log::info("f4sr: VRAM 5s: local {:.0f} / {:.0f} MB budget ({:.0f}%, peak {:.0f} MB), non-local {:.0f} / {:.0f} MB, budget changes {}; "
						"F4SR ~{:.0f} MB (textures {:.0f}, swap chain {:.0f}, SDK contexts {:.0f})",
			static_cast<double>(s.localUsage) / kMb, static_cast<double>(s.localBudget) / kMb, pct, static_cast<double>(g.peakLocal) / kMb,
			static_cast<double>(s.nonLocalUsage) / kMb, static_cast<double>(s.nonLocalBudget) / kMb, g.budgetChanges,
			static_cast<double>(OwnTotal()) / kMb, static_cast<double>(Own(Kind::Texture)) / kMb, static_cast<double>(Own(Kind::Chain)) / kMb,
			static_cast<double>(Own(Kind::Sdk)) / kMb);
	}

	// Status panel row ("" until the first sample).
	inline std::string PanelLine()
	{
		const auto usage = g.usageMb.load(std::memory_order_relaxed);
		const auto budget = g.budgetMb.load(std::memory_order_relaxed);
		if (!budget) return {};
		const bool high = usage * 100ull >= budget * 95ull;
		// The budget is what the OS lets this process use before it starts
		// paging, not the card's total: below the dedicated size by the OS
		// reserve and other applications' use.
		return std::format("VRAM\t{}{:.1f} GB used (F4SR ~{:.1f}), budget {:.1f} GB{}\n", high ? "!" : "", usage / 1024.0,
			static_cast<double>(OwnTotal()) / kMb / 1024.0, budget / 1024.0, high ? " (near)" : "");
	}
}
