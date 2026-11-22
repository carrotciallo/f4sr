#include "sr_status.h"
#include "xefg_presenter.h"
#include "swapchain_inner.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REL/IAT.h"

#include "frame_camera.h"
#include "fg_overlay.h"
#include "gpu_timing.h"
#include "framegen.h"
#include "interop.h"
#include "settings.h"
#include "xefg_pacing.h"
#include "xefg_unlock.h"

#include "xell/xell.h"
#include "xell/xell_d3d12.h"
#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"

using Microsoft::WRL::ComPtr;

namespace
{
	using namespace f4sr;

	using CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

	CreateDeviceAndSwapChain_t g_origCreate{ nullptr };
	// What the import held when the redirect was first installed (normally the
	// d3d11.dll export). Used if another plugin's hook calls back into ours.
	CreateDeviceAndSwapChain_t g_firstCreate{ nullptr };
	Present_t g_origPresent{ nullptr };
	bool g_redirectUsed{ false };

	// ENB wraps the swap chain (myIDXGISwapChain) and applies its effects in
	// the wrapper's Present before calling the inner DXVK chain's Present.
	// XeSS-FG takes the frame there, after ENB, and the inner Present is not
	// forwarded (a second window never gets its buffers back under gamescope).
	Present_t g_innerOrigPresent{ nullptr };
	IDXGISwapChain* g_innerChain{ nullptr };
	thread_local bool tl_inGamePresent = false;
	bool g_innerHit{ false };

	// ENB binds input to its swap chain's window (our hidden window): it only
	// reacts to keys (Shift+F12, Shift+Enter GUI) while GetForegroundWindow /
	// GetActiveWindow return that window, and maps the cursor with
	// ScreenToClient on it. Its own imports of those are redirected so the
	// real game window counts as the hidden one.
	HWND g_focusReal{}, g_focusHidden{};
	decltype(&GetForegroundWindow) g_origGetForeground{};
	decltype(&GetActiveWindow) g_origGetActive{};
	decltype(&ScreenToClient) g_origScreenToClient{};
	HWND WINAPI GetForegroundHook()
	{
		const HWND h = g_origGetForeground();
		return h && h == g_focusReal ? g_focusHidden : h;
	}
	HWND WINAPI GetActiveHook()
	{
		const HWND h = g_origGetActive();
		return h && h == g_focusReal ? g_focusHidden : h;
	}
	BOOL WINAPI ScreenToClientHook(HWND hwnd, LPPOINT point)
	{
		return g_origScreenToClient(hwnd && hwnd == g_focusHidden ? g_focusReal : hwnd, point);
	}
	void RedirectWrapperFocus(HMODULE wrapper, HWND real, HWND hidden)
	{
		g_focusReal = real;
		g_focusHidden = hidden;
		int patched = 0;
		auto patch = [&](const char* name, void* hook, auto& original) {
			auto* slot = reinterpret_cast<std::uintptr_t*>(REL::GetIATAddr(reinterpret_cast<REX::W32::HMODULE>(wrapper), "USER32.dll", name));
			if (!slot) return;
			original = reinterpret_cast<std::remove_reference_t<decltype(original)>>(*slot);
			REL::safe_write(reinterpret_cast<std::uintptr_t>(slot), reinterpret_cast<std::uintptr_t>(hook));
			++patched;
		};
		patch("GetForegroundWindow", reinterpret_cast<void*>(&GetForegroundHook), g_origGetForeground);
		patch("GetActiveWindow", reinterpret_cast<void*>(&GetActiveHook), g_origGetActive);
		patch("ScreenToClient", reinterpret_cast<void*>(&ScreenToClientHook), g_origScreenToClient);
		F4SE::log::info("f4sr: XeFG: {} focus/cursor imports of the swap chain wrapper redirected to the game window (ENB hotkeys and GUI)", patched);
	}

	using swapchain::FindInnerChain;
	using swapchain::ModulePath;
	using swapchain::Readable;

	// Hang watchdog: OnPresent records which potentially blocking call it is in;
	// a helper thread logs a step that has not returned within 3 s, and a gap of
	// more than 5 s between presents while idle.
	enum class Step : int { Idle, Copy, BeginFrame, EndFrame, ProxyPresent, PresentStatus, FenceOrder, GamePresent, XellSleep };
	const char* StepName(Step s)
	{
		switch (s) {
		case Step::Idle: return "idle (outside the XeFG present path)";
		case Step::Copy: return "back buffer copy / D3D11->D3D12 signal";
		case Step::BeginFrame: return "interop BeginFrame";
		case Step::EndFrame: return "interop EndFrame (D3D12 submit)";
		case Step::ProxyPresent: return "proxy swap chain Present(1,0) (XeSS-FG)";
		case Step::PresentStatus: return "xefgSwapChainGetLastPresentStatus";
		case Step::FenceOrder: return "D3D12->D3D11 fence ordering";
		case Step::GamePresent: return "game swap chain Present (skipped)";
		case Step::XellSleep: return "xellSleep (XeLL low-latency)";
		}
		return "?";
	}
	std::atomic<int> g_step{ 0 };
	std::atomic<std::uint64_t> g_stepTick{ 0 }, g_lastPresentDone{ 0 }, g_presentsDone{ 0 };
	std::atomic<std::uint32_t> g_stepFrame{ 0 };
	void SetStep(Step s, std::uint32_t frame)
	{
		g_stepFrame.store(frame, std::memory_order_relaxed);
		g_stepTick.store(GetTickCount64(), std::memory_order_relaxed);
		g_step.store(static_cast<int>(s), std::memory_order_release);
	}
	void StartWatchdog()
	{
		std::thread([] {
			int reportedStep = -1;
			std::uint64_t reportedTick = 0;
			bool idleReported = false;
			for (;;) {
				Sleep(500);
				const auto now = GetTickCount64();
				const int step = g_step.load(std::memory_order_acquire);
				const auto tick = g_stepTick.load(std::memory_order_relaxed);
				if (step != 0) {
					if (now - tick > 3000 && (step != reportedStep || tick != reportedTick)) {
						F4SE::log::error("f4sr: XeFG watchdog: present path stuck {} ms in {} (frame {}, presents completed {})", now - tick,
							StepName(static_cast<Step>(step)), g_stepFrame.load(), g_presentsDone.load());
						reportedStep = step;
						reportedTick = tick;
					}
					idleReported = false;
				} else if (reportedStep > 0) {
					F4SE::log::warn("f4sr: XeFG watchdog: present path resumed after {}", StepName(static_cast<Step>(reportedStep)));
					reportedStep = -1;
				} else {
					const auto last = g_lastPresentDone.load(std::memory_order_relaxed);
					if (last && now - last > 5000 && !idleReported) {
						F4SE::log::warn("f4sr: XeFG watchdog: no game Present for {} ms (presents completed {}); the stall is outside the XeFG present path",
							now - last, g_presentsDone.load());
						idleReported = true;
					} else if (last && now - last <= 5000) {
						idleReported = false;
					}
				}
			}
		}).detach();
	}

	constexpr UINT kProxyBuffers = 8;  // 4X bursts queue up to 4 flips per real frame

	struct Api
	{
		HMODULE fg{}, xell{};
		decltype(&xefgSwapChainD3D12CreateContext) createContext{};
		decltype(&xefgSwapChainSetLatencyReduction) setLatencyReduction{};
		decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) initFromDesc{};
		decltype(&xefgSwapChainD3D12GetSwapChainPtr) getSwapChain{};
		decltype(&xefgSwapChainSetEnabled) setEnabled{};
		decltype(&xefgSwapChainSetUiCompositionState) setUiComposition{};  // optional
		decltype(&xefgSwapChainSetNumInterpolatedFrames) setInterpolations{};
		decltype(&xefgSwapChainSetPresentId) setPresentId{};
		decltype(&xefgSwapChainTagFrameConstants) tagConstants{};
		decltype(&xefgSwapChainD3D12TagFrameResource) tagResource{};
		decltype(&xefgSwapChainGetLastPresentStatus) lastPresentStatus{};
		decltype(&xefgSwapChainGetProperties) properties{};
		decltype(&xefgSwapChainSetLoggingCallback) setLogging{};
		decltype(&xefgSwapChainDestroy) destroy{};
		decltype(&xellD3D12CreateContext) xellCreate{};
		decltype(&xellSetSleepMode) xellSetSleepMode{};
		decltype(&xellSleep) xellSleep{};
		decltype(&xellGetFramesReports) xellReports{};  // optional
		decltype(&xellAddMarkerData) xellMarker{};
		decltype(&xellSetLoggingCallback) xellSetLogging{};
		decltype(&xellDestroyContext) xellDestroy{};

		bool Load()
		{
			HMODULE self{};
			char path[MAX_PATH]{};
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&g_origCreate), &self) || !GetModuleFileNameA(self, path, MAX_PATH)) {
				F4SE::log::error("f4sr: XeFG: cannot locate the plugin directory");
				return false;
			}
			const auto dir = std::filesystem::path(path).parent_path();
			// libxell first: libxess_fg resolves it by module name.
			xell = LoadLibraryExA((dir / "libxell.dll").string().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			fg = xell ? LoadLibraryExA((dir / "libxess_fg.dll").string().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
			if (!xell || !fg) {
				F4SE::log::error("f4sr: XeFG: {} not loadable from {} (win32={})", xell ? "libxess_fg.dll" : "libxell.dll", dir.string(), GetLastError());
				return false;
			}
			bool ok = true;
			auto get = [&](HMODULE m, auto& fn, const char* name) {
				fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
				if (!fn) {
					F4SE::log::error("f4sr: XeFG: export {} missing", name);
					ok = false;
				}
			};
			get(fg, createContext, "xefgSwapChainD3D12CreateContext");
			get(fg, setLatencyReduction, "xefgSwapChainSetLatencyReduction");
			get(fg, initFromDesc, "xefgSwapChainD3D12InitFromSwapChainDesc");
			get(fg, getSwapChain, "xefgSwapChainD3D12GetSwapChainPtr");
			get(fg, setEnabled, "xefgSwapChainSetEnabled");
			setUiComposition = reinterpret_cast<decltype(setUiComposition)>(GetProcAddress(fg, "xefgSwapChainSetUiCompositionState"));
			get(fg, setInterpolations, "xefgSwapChainSetNumInterpolatedFrames");
			get(fg, setPresentId, "xefgSwapChainSetPresentId");
			get(fg, tagConstants, "xefgSwapChainTagFrameConstants");
			get(fg, tagResource, "xefgSwapChainD3D12TagFrameResource");
			get(fg, lastPresentStatus, "xefgSwapChainGetLastPresentStatus");
			get(fg, properties, "xefgSwapChainGetProperties");
			get(fg, setLogging, "xefgSwapChainSetLoggingCallback");
			get(fg, destroy, "xefgSwapChainDestroy");
			get(xell, xellCreate, "xellD3D12CreateContext");
			get(xell, xellSetSleepMode, "xellSetSleepMode");
			get(xell, xellSleep, "xellSleep");
			xellReports = reinterpret_cast<decltype(xellReports)>(GetProcAddress(xell, "xellGetFramesReports"));
			get(xell, xellMarker, "xellAddMarkerData");
			get(xell, xellSetLogging, "xellSetLoggingCallback");
			get(xell, xellDestroy, "xellDestroyContext");
			return ok;
		}
	};

	void XefgLog(const char* message, xefg_swapchain_logging_level_t level, void*)
	{
		if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR) F4SE::log::error("f4sr: libxess_fg: {}", message ? message : "");
		else F4SE::log::warn("f4sr: libxess_fg: {}", message ? message : "");
	}

	void XellLog(const char* message, xell_logging_level_t level)
	{
		if (level >= XELL_LOGGING_LEVEL_ERROR) F4SE::log::error("f4sr: libxell: {}", message ? message : "");
		else F4SE::log::warn("f4sr: libxell: {}", message ? message : "");
	}

	// Positive XeSS-FG / XeLL results are warnings (e.g. WARNING_OLD_DRIVER);
	// only negative results are failures.
	bool Ok(int result, const char* what)
	{
		if (result > 0) F4SE::log::warn("f4sr: XeFG: {} returned warning {}", what, result);
		return result >= 0;
	}

	DXGI_FORMAT NonSrgb(DXGI_FORMAT f)
	{
		switch (f) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		default: return f;
		}
	}

	HWND CreateHiddenWindow(UINT width, UINT height)
	{
		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"f4srXeFGHiddenSwapChain";
		RegisterClassExW(&wc);
		// Never shown and never presented to (see OnPresent): it only gives the
		// game's D3D11 chain a window that is not the visible one.
		(void)width;
		(void)height;
		return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"f4sr hidden swap chain", WS_POPUP,
			-32000, -32000, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
	}

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags);
	HRESULT STDMETHODCALLTYPE InnerPresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags);

	struct Presenter
	{
		Api api;
		D3D12Interop interop;
		xefg_swapchain_handle_t fg{};
		xell_context_handle_t xell{};
		ComPtr<IDXGISwapChain3> proxy;
		ComPtr<ID3D11Device> device11;
		ComPtr<ID3D11DeviceContext> ctx11;
		IDXGISwapChain* gameChain{};
		HWND realWindow{}, hiddenWindow{};
		UINT width{}, height{};
		DXGI_FORMAT format{};
		SharedTexture back, hudless, depth, motion;
		bool attached{};
		int maxInterpolations{ 1 };
		int appliedInterpolations{ 0 };
		bool fgEnabled{};
		bool resetNext{ true };

		// Frame state. The XeLL frame id doubles as the XeFG present id.
		std::uint32_t frameId{ 1 };
		bool simStarted{}, renderStarted{};
		bool inputsReady{}, hudlessReady{};
		XeFGFrameInputs inputs{};
		std::optional<FrameCamera> camera;  // captured with the inputs, mid-frame

		// 5-second statistics.
		std::uint64_t statsTick{};
		std::uint64_t statReal{}, statFgOn{}, statPresented{}, statStatusFailed{}, statGenErrors{}, statGenWarnings{};
		std::uint64_t statHistogram[6]{};
		int lastGenResult{}, lastGenWarning{};
		const char* lastOff{ "not started" };

		// Diagnostics panel (bFrameGenOverlay): cumulative counters turned into rates.
		bool unlockApplied{};
		std::string unlockFailure;
		FgOverlay overlay;
		std::uint64_t totalReal{}, totalPresented{}, overlayReal{}, overlayPresented{};
		std::uint32_t lastFramesPresented{};
		const char* overlayOff{ "not started" };

		void Marker(std::uint32_t id, xell_latency_marker_type_t m)
		{
			if (xell) api.xellMarker(xell, id, m);
		}

		bool CreateTexture(SharedTexture& out, UINT w, UINT h, DXGI_FORMAT f)
		{
			D3D11_TEXTURE2D_DESC d{};
			d.Width = w;
			d.Height = h;
			d.MipLevels = d.ArraySize = 1;
			d.Format = f;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			return interop.CreateSharedTexture(d, out);
		}

		void Teardown()
		{
			if (fg) api.destroy(fg);
			if (xell) api.xellDestroy(xell);
			fg = nullptr;
			xell = nullptr;
			proxy.Reset();
			back = {};
			hudless = {};
			depth = {};
			motion = {};
			interop.Destroy();
			device11.Reset();
			ctx11.Reset();
			attached = false;
		}

		bool Attach(ID3D11Device* device, IDXGISwapChain* chain, HWND real, HWND hidden, UINT w, UINT h, DXGI_FORMAT gameFormat)
		{
			width = w;
			height = h;
			format = NonSrgb(gameFormat);
			realWindow = real;
			hiddenWindow = hidden;
			if (!api.Load()) return false;
			if (!interop.Create(device)) {
				F4SE::log::error("f4sr: XeFG: D3D12 interop device creation failed");
				return false;
			}
			device11 = device;
			device->GetImmediateContext(&ctx11);

			const auto unlock = xefg_unlock::Apply(api.fg, api.xell, xefg_unlock::kMaxInterpolations);
			maxInterpolations = unlock.maxInterpolations;
			unlockApplied = unlock.unlocked;
			unlockFailure = unlock.failure;
			if (unlock.unlocked && unlock.pacingSitesOk) xefg_pacing::Install(api.fg);

			if (!Ok(api.xellCreate(interop.Device12(), &xell), "xellD3D12CreateContext") || !xell) {
				F4SE::log::error("f4sr: XeFG: xellD3D12CreateContext failed (XeSS-FG requires XeLL)");
				xell = nullptr;
				return false;
			}
			api.xellSetLogging(xell, XELL_LOGGING_LEVEL_WARNING, &XellLog);
			xell_sleep_params_t sleep{};
			sleep.bLowLatencyMode = 1;
			if (!Ok(api.xellSetSleepMode(xell, &sleep), "xellSetSleepMode")) F4SE::log::warn("f4sr: XeFG: xellSetSleepMode failed");

			if (!Ok(api.createContext(interop.Device12(), &fg), "xefgSwapChainD3D12CreateContext") || !fg) {
				F4SE::log::error("f4sr: XeFG: xefgSwapChainD3D12CreateContext failed");
				fg = nullptr;
				return false;
			}
			api.setLogging(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, &XefgLog, nullptr);
			if (!Ok(api.setLatencyReduction(fg, xell), "xefgSwapChainSetLatencyReduction")) {
				F4SE::log::error("f4sr: XeFG: xefgSwapChainSetLatencyReduction failed");
				return false;
			}
			ComPtr<IDXGIFactory2> factory;
			if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
				F4SE::log::error("f4sr: XeFG: CreateDXGIFactory2 failed");
				return false;
			}
			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = width;
			desc.Height = height;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = kProxyBuffers;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
			xefg_swapchain_d3d12_init_params_t params{};
			params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;  // standard depth, render-res UV MVs scaled to pixels, unjittered MVs
			params.maxInterpolatedFrames = static_cast<std::uint32_t>(maxInterpolations);
			params.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
			const auto init = api.initFromDesc(fg, real, &desc, nullptr, interop.Queue12(), factory.Get(), &params);
			if (!Ok(init, "xefgSwapChainD3D12InitFromSwapChainDesc")) {
				F4SE::log::error("f4sr: XeFG: xefgSwapChainD3D12InitFromSwapChainDesc failed ({})", static_cast<int>(init));
				return false;
			}
			if (!Ok(api.getSwapChain(fg, IID_PPV_ARGS(&proxy)), "xefgSwapChainD3D12GetSwapChainPtr") || !proxy) {
				F4SE::log::error("f4sr: XeFG: proxy swap chain (IDXGISwapChain3) unavailable");
				return false;
			}
			if (!CreateTexture(back, width, height, format) || !CreateTexture(hudless, width, height, format)) {
				F4SE::log::error("f4sr: XeFG: shared back buffer / HUD-less texture creation failed");
				return false;
			}
			xefg_swapchain_properties_t props{};
			const auto propsResult = api.properties(fg, &props);
			api.setEnabled(fg, 0);
			// uiMode only takes effect with UI composition enabled (off by
			// default): without it the whole back buffer, HUD included, is
			// interpolated and the crosshair moves with the scene.
			if (api.setUiComposition) {
				const auto ui = api.setUiComposition(fg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
				F4SE::log::info("f4sr: XeFG: UI composition enabled (mode back buffer + HUD-less) result {}", static_cast<int>(ui));
			} else {
				F4SE::log::warn("f4sr: XeFG: xefgSwapChainSetUiCompositionState not exported; UI is interpolated with the scene");
			}

			// Present hook on the game chain object (vtable shared per class).
			void** vtable = *reinterpret_cast<void***>(chain);
			DWORD oldProtect{};
			if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				F4SE::log::error("f4sr: XeFG: present hook protection failed ({})", GetLastError());
				return false;
			}
			g_origPresent = reinterpret_cast<Present_t>(vtable[8]);
			vtable[8] = reinterpret_cast<void*>(&PresentThunk);
			DWORD ignored{};
			VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &ignored);
			gameChain = chain;
			int field = -1;
			std::string innerModule;
			if (auto* inner = FindInnerChain(chain, hidden, field, innerModule)) {
				void** innerTable = *reinterpret_cast<void***>(inner);
				if (VirtualProtect(&innerTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
					g_innerOrigPresent = reinterpret_cast<Present_t>(innerTable[8]);
					innerTable[8] = reinterpret_cast<void*>(&InnerPresentThunk);
					VirtualProtect(&innerTable[8], sizeof(void*), oldProtect, &ignored);
					g_innerChain = inner;
					F4SE::log::info("f4sr: XeFG: frames are taken from the inner swap chain {:p} (wrapper field {}, {}), after the wrapper's Present (ENB)",
						static_cast<void*>(inner), field, innerModule);
				}
			}
			if (g_innerChain) {
				HMODULE wrapper{};
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						static_cast<LPCSTR>(*reinterpret_cast<void* const*>(chain)), &wrapper))
					RedirectWrapperFocus(wrapper, real, hidden);
			}
			if (!g_innerChain)
				F4SE::log::warn("f4sr: XeFG: no inner swap chain found behind {} ; frames are taken before the wrapper's Present (ENB effects applied there are missing)",
					ModulePath(*reinterpret_cast<void* const*>(chain)));
			attached = true;
			StartWatchdog();
			F4SE::log::info("f4sr: XeFG attached: proxy chain {}x{} fmt={} buffers={} FLIP_DISCARD on game window {:p} (game chain on hidden window {:p}); "
							"max generated frames {} ({}X, unlock {}), provider reports max {} (result {}); XeLL low-latency on",
				width, height, static_cast<unsigned>(format), kProxyBuffers, static_cast<void*>(real), static_cast<void*>(hidden),
				maxInterpolations, maxInterpolations + 1, unlock.unlocked ? "applied" : ("not applied: " + unlock.failure),
				props.maxSupportedInterpolations, static_cast<int>(propsResult));
			return true;
		}

		void ApplyInterpolations()
		{
			const int want = std::clamp(GetSettings().xefgInterpolations, 1, maxInterpolations);
			if (want == appliedInterpolations) return;
			const auto r = api.setInterpolations(fg, static_cast<std::uint32_t>(want));
			F4SE::log::info("f4sr: XeFG generated frames per real frame -> {} ({}X) result {}{}", want, want + 1, static_cast<int>(r),
				GetSettings().xefgInterpolations > maxInterpolations ? " (setting clamped: unlock not applied)" : "");
			appliedInterpolations = want;
		}

		// Row-vector world->view and unjittered view->clip, stored row-major.
		static void Matrices(const FrameCamera& c, float aspect, float view[16], float proj[16])
		{
			for (int i = 0; i < 3; ++i) {
				view[i * 4 + 0] = c.right[i];
				view[i * 4 + 1] = c.up[i];
				view[i * 4 + 2] = c.forward[i];
				view[i * 4 + 3] = 0.0f;
			}
			auto dot = [&](const float* a) { return c.position[0] * a[0] + c.position[1] * a[1] + c.position[2] * a[2]; };
			view[12] = -dot(c.right);
			view[13] = -dot(c.up);
			view[14] = -dot(c.forward);
			view[15] = 1.0f;
			const float y = 1.0f / std::tan(c.verticalFov * 0.5f);
			const float x = y / aspect;
			const float q = c.farPlane / (c.farPlane - c.nearPlane);
			std::fill(proj, proj + 16, 0.0f);
			proj[0] = x;
			proj[5] = y;
			proj[10] = q;
			proj[11] = 1.0f;
			proj[14] = -q * c.nearPlane;
		}

		void Tag(ID3D12GraphicsCommandList* list, std::uint32_t id, xefg_swapchain_resource_type_t type, ID3D12Resource* res, UINT w, UINT h)
		{
			xefg_swapchain_d3d12_resource_data_t d{};
			d.type = type;
			d.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
			d.resourceBase = { 0, 0 };
			d.resourceSize = { w, h };
			d.pResource = res;
			d.incomingState = D3D12_RESOURCE_STATE_COMMON;
			api.tagResource(fg, list, id, &d);
		}

		// Returns nullptr when the frame can be generated, else why not.
		const char* GenerationBlock()
		{
			if (!GetSettings().enableFrameGeneration) return "disabled in settings";
			if (!inputsReady) return "no SR inputs this frame";
			if (!hudlessReady) return "no HUD-less capture this frame";
			if (const char* menu = FrameGenMenuBlockReason()) return menu;
			if (!camera) return "invalid world camera";
			return nullptr;
		}

		// XeLL's last 64 frame reports: CPU-side latency from simulation start
		// (after xellSleep) to the end of the proxy Present.
		void LogLatency()
		{
			if (!xell || !api.xellReports) return;
			xell_frame_report_t r[64]{};
			if (api.xellReports(xell, r) != XELL_RESULT_SUCCESS) return;
			std::vector<double> total, toPresent;
			for (const auto& f : r) {
				if (!f.m_frame_id || !f.m_sim_start_ts || f.m_present_end_ts < f.m_sim_start_ts || f.m_present_start_ts < f.m_sim_start_ts) continue;
				total.push_back((f.m_present_end_ts - f.m_sim_start_ts) / 1e6);
				toPresent.push_back((f.m_present_start_ts - f.m_sim_start_ts) / 1e6);
			}
			if (total.empty()) return;
			std::sort(total.begin(), total.end());
			std::sort(toPresent.begin(), toPresent.end());
			F4SE::log::info("f4sr: XeLL latency over {} frames: sim start -> present end median {:.2f} ms (max {:.2f}); sim start -> present start median {:.2f} ms",
				total.size(), total[total.size() / 2], total.back(), toPresent[toPresent.size() / 2]);
		}

		void Stats(const xefg_swapchain_present_status_t* status, bool fgFrame)
		{
			++statReal;
			++totalReal;
			if (fgFrame) ++statFgOn;
			if (!status) {
				++statStatusFailed;
			} else {
				totalPresented += status->framesPresented;
				lastFramesPresented = status->framesPresented;
				statPresented += status->framesPresented;
				++statHistogram[(std::min)(status->framesPresented, 5u)];
				if (status->frameGenResult < 0) {
					++statGenErrors;
					lastGenResult = static_cast<int>(status->frameGenResult);
				} else if (status->frameGenResult > 0) {
					++statGenWarnings;
					lastGenWarning = static_cast<int>(status->frameGenResult);
				}
			}
			const auto now = GetTickCount64();
			if (!statsTick) statsTick = now;
			if (now - statsTick < 5000) return;
			F4SE::log::info("f4sr: XeFG 5s: real frames submitted {} ({} with generation on, last off reason: {}); frames presented by the proxy {} "
							"(per real frame: 0:{} 1:{} 2:{} 3:{} 4:{} 5+:{}), status unavailable {}, generation errors {} (last {}), warnings {} (last {}); generated per real frame setting {}/{}",
				statReal, statFgOn, lastOff, statPresented, statHistogram[0], statHistogram[1], statHistogram[2], statHistogram[3], statHistogram[4], statHistogram[5],
				statStatusFailed, statGenErrors, lastGenResult, statGenWarnings, lastGenWarning, appliedInterpolations, maxInterpolations);
			LogLatency();
			statsTick = now;
			statReal = statFgOn = statPresented = statStatusFailed = statGenErrors = statGenWarnings = 0;
			std::fill(std::begin(statHistogram), std::end(statHistogram), 0ull);
		}

		// Drawn into the game back buffer before it is copied to the proxy, so the
		// panel is part of the real frame (it is not in the HUD-less copy, so
		// XeSS-FG treats it like UI).
		std::uint64_t overlaySr{};
		void DrawOverlay(IDXGISwapChain* chain, const char* block)
		{
			overlayOff = block ? block : "-";
			overlay.Draw(chain, 0, [this](double seconds) {
				auto rate = [&](std::uint64_t n, std::uint64_t o) {
					return seconds > 0 && n >= o ? static_cast<double>(n - o) / seconds : 0.0;
				};
				const double real = rate(totalReal, overlayReal);
				const double presented = rate(totalPresented, overlayPresented);
				overlayReal = totalReal;
				overlayPresented = totalPresented;
				const int setting = GetSettings().xefgInterpolations;
				std::string text = "#F4SR\n";
				text += sr_status::Lines(GetSettings().enableUpscaling, seconds, overlaySr);
				text += "\n";
				if (appliedInterpolations <= 0)
					text += "FG\t!XeSS-FG not applied yet\n";
				else if (fgEnabled)
					text += std::format("FG\tXeSS-FG {}X   generating\n", appliedInterpolations + 1);
				else
					text += std::format("FG\tXeSS-FG {}X   !paused: {}\n", appliedInterpolations + 1, overlayOff);
				text += std::format("\t{:.0f} real/s  ->  {:.0f} shown/s\n", real, presented);
				if (setting != appliedInterpolations && appliedInterpolations > 0)
					text += std::format("\t!setting {}X, device max {}X\n", setting + 1, maxInterpolations + 1);
				if (!unlockApplied) text += "\t!multi-frame unlock failed: " + unlockFailure + "\n";
				if (lastGenResult != 0) text += std::format("\t!last generation error {}\n", lastGenResult);
				if (GetSettings().frameGenEngine != 1) text += "\t!engine setting is FSR-FG: restart to switch\n";
				return std::array<std::string, 2>{ text, text };
			});
		}

		// One-shot diagnostic (bDiagnostics): centre 256x256 of the HUD-less copy and of the
		// final frame for four consecutive generated frames, to check whether
		// the crosshair is already in the HUD-less image. Written to the game
		// folder as f4sr-xefg-<pid>-<frame>-{hudless,final}.raw (F4SRRAW header).
		std::uint64_t centreDumpFrames{};
		void DumpCentre(ID3D11Texture2D* tex, std::uint32_t frame, const char* what)
		{
			constexpr UINT kSize = 256;
			if (width < kSize || height < kSize) return;
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			D3D11_TEXTURE2D_DESC sd{};
			sd.Width = sd.Height = kSize;
			sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
			sd.Format = d.Format;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Texture2D> staging;
			if (FAILED(device11->CreateTexture2D(&sd, nullptr, &staging))) return;
			const D3D11_BOX box{ (width - kSize) / 2, (height - kSize) / 2, 0, (width + kSize) / 2, (height + kSize) / 2, 1 };
			ctx11->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, &box);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
			const auto name = std::format("f4sr-xefg-{}-{}-{}.raw", GetCurrentProcessId(), frame, what);
			if (auto* f = std::fopen(name.c_str(), "wb")) {
				std::fprintf(f, "F4SRRAW %u %u %u %u\n", kSize, kSize, static_cast<unsigned>(d.Format), m.RowPitch);
				std::fwrite(m.pData, 1, static_cast<size_t>(m.RowPitch) * kSize, f);
				std::fclose(f);
			}
			ctx11->Unmap(staging.Get(), 0);
			F4SE::log::info("f4sr: XeFG centre dump written: {}", name);
		}

		HRESULT OnPresent(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
		{
			if ((flags & DXGI_PRESENT_TEST) != 0) return g_origPresent(chain, syncInterval, flags);
			if (auto* data = RE::BSGraphics::RendererData::GetSingleton())
				gpu_timing::MarkAt(reinterpret_cast<ID3D11DeviceContext*>(data->context), gpu_timing::kPresent);
			const std::uint32_t id = frameId;
			if (!simStarted) Marker(id, XELL_SIMULATION_START);
			if (!renderStarted) {
				Marker(id, XELL_SIMULATION_END);
				Marker(id, XELL_RENDERSUBMIT_START);
			}
			Marker(id, XELL_RENDERSUBMIT_END);

			const char* block = interop.Healthy() ? GenerationBlock() : "interop failed";

			bool presented = false;
			ComPtr<ID3D11Texture2D> source;
			D3D11_TEXTURE2D_DESC sd{};
			if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&source))) && source) source->GetDesc(&sd);
			if (source && sd.Width == width && sd.Height == height && interop.Healthy()) {
				SetStep(Step::Copy, id);
				if (GetSettings().frameGenOverlay) DrawOverlay(chain, block);
				ctx11->CopyResource(back.tex11.Get(), source.Get());
				if (GetSettings().diagnostics && !block && hudlessReady && ++centreDumpFrames >= 600 && centreDumpFrames < 604) {
					DumpCentre(hudless.tex11.Get(), id, "hudless");
					DumpCentre(back.tex11.Get(), id, "final");
				}
				const bool signalled = interop.SignalFromD3D11(ctx11.Get()) && interop.WaitOnD3D12();
				SetStep(Step::BeginFrame, id);
				auto* list = signalled ? interop.BeginFrame() : nullptr;
				ComPtr<ID3D12Resource> target;
				if (list && SUCCEEDED(proxy->GetBuffer(proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)))) {
					D3D12Interop::Transition(list, target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
					D3D12Interop::Transition(list, back.res12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
					list->CopyResource(target.Get(), back.res12.Get());
					D3D12Interop::Transition(list, back.res12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
					D3D12Interop::Transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
					if (!block) {
						Tag(list, id, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hudless.res12.Get(), width, height);
						Tag(list, id, XEFG_SWAPCHAIN_RES_DEPTH, depth.res12.Get(), inputs.renderWidth, inputs.renderHeight);
						Tag(list, id, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motion.res12.Get(), inputs.renderWidth, inputs.renderHeight);
						xefg_swapchain_frame_constant_data_t c{};
						Matrices(*camera, static_cast<float>(width) / static_cast<float>(height), c.viewMatrix, c.projectionMatrix);
						c.jitterOffsetX = inputs.jitterX;
						c.jitterOffsetY = inputs.jitterY;
						c.motionVectorScaleX = static_cast<float>(inputs.renderWidth);
						c.motionVectorScaleY = static_cast<float>(inputs.renderHeight);
						c.resetHistory = (inputs.reset || resetNext) ? 1u : 0u;
						const double paced = xefg_pacing::RenderTimeMs();
						c.frameRenderTime = paced > 0.0 ? static_cast<float>(paced) : inputs.frameTimeMs;
						api.tagConstants(fg, id, &c);
					}
					SetStep(Step::EndFrame, id);
					presented = interop.EndFrame();
				} else if (list) {
					interop.EndFrame();
				}
			}
			if (!presented && !block) block = "back buffer copy failed";

			const bool fgFrame = presented && !block;
			if (fgFrame != fgEnabled) {
				api.setEnabled(fg, fgFrame ? 1u : 0u);
				fgEnabled = fgFrame;
			}
			if (fgFrame) {
				ApplyInterpolations();
				resetNext = false;
			} else {
				resetNext = true;
				lastOff = block;
			}
			if (presented) {
				api.setPresentId(fg, id);
				Marker(id, XELL_PRESENT_START);
				xefg_pacing::NoteAppSubmit(id);
				SetStep(Step::ProxyPresent, id);
				const HRESULT hr12 = proxy->Present(1, 0);
				Marker(id, XELL_PRESENT_END);
				if (FAILED(hr12)) F4SE::log::warn("f4sr: XeFG proxy Present hr=0x{:08X}", static_cast<std::uint32_t>(hr12));
				xefg_swapchain_present_status_t status{};
				SetStep(Step::PresentStatus, id);
				Stats(api.lastPresentStatus(fg, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS ? &status : nullptr, fgFrame);  // warnings mean no status
				// The next frame's D3D11 writes into the shared copies wait for this frame's D3D12 reads.
				SetStep(Step::FenceOrder, id);
				if (!interop.SignalFromD3D12() || !interop.WaitOnD3D11(ctx11.Get())) F4SE::log::error("f4sr: XeFG fence ordering failed");
			} else {
				Marker(id, XELL_PRESENT_START);
				Marker(id, XELL_PRESENT_END);
				static std::uint64_t warned = 0;
				if ((warned++ % 600) == 0)
					F4SE::log::warn("f4sr: XeFG frame {} not presented on the proxy chain (back buffer {}x{} vs proxy {}x{}, interop {})", id, sd.Width, sd.Height,
						width, height, interop.Healthy() ? "ok" : "failed");
			}
			inputsReady = hudlessReady = false;
			renderStarted = false;

			// The game chain is not presented. Under gamescope only the visible
			// window gets its buffers released, so a Present to the second
			// window blocks once the chain's buffers are used up (watchdog,
			// 2026-11-22: frame 2 unmapped, frame 3 mapped 1x1). Pacing comes
			// from the proxy's FIFO Present and xellSleep. Present-time work of
			// hooks below this one (ENB GUI, other plugins) does not run.
			ctx11->Flush();  // Present would have submitted the frame's D3D11 work
			const HRESULT hr = S_OK;
			if (static bool logged = false; !logged) {
				logged = true;
				F4SE::log::info("f4sr: XeFG: game swap chain Present is skipped (output goes through the proxy chain only)");
			}

			frameId = id + 1;
			SetStep(Step::XellSleep, id);
			if (xell) api.xellSleep(xell, frameId);
			SetStep(Step::Idle, id);
			const auto done = g_presentsDone.fetch_add(1) + 1;
			g_lastPresentDone.store(GetTickCount64(), std::memory_order_relaxed);
			if (done <= 5) F4SE::log::info("f4sr: XeFG present {} completed (frame {})", done, id);
			Marker(frameId, XELL_SIMULATION_START);
			simStarted = true;
			return hr;
		}
	};

	Presenter g_presenter;

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
	{
		thread_local bool inside = false;
		if (inside || chain != g_presenter.gameChain || !g_presenter.attached || (flags & DXGI_PRESENT_TEST) != 0)
			return g_origPresent(chain, syncInterval, flags);
		struct Guard { bool& v; ~Guard() { v = false; } } guard{ inside };
		inside = true;
		if (!g_innerChain) return g_presenter.OnPresent(chain, syncInterval, flags);
		// Let the wrapper (ENB) run; the frame is taken in InnerPresentThunk.
		g_innerHit = false;
		tl_inGamePresent = true;
		const HRESULT hr = g_origPresent(chain, syncInterval, flags);
		tl_inGamePresent = false;
		if (g_innerHit) return hr;
		static std::uint64_t missed = 0;
		if ((missed++ % 600) == 0)
			F4SE::log::warn("f4sr: XeFG: the wrapper's Present did not reach the inner swap chain ({} times); frame taken before the wrapper", missed);
		return g_presenter.OnPresent(chain, syncInterval, flags);
	}

	HRESULT STDMETHODCALLTYPE InnerPresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
	{
		if (!tl_inGamePresent || chain != g_innerChain || (flags & DXGI_PRESENT_TEST) != 0)
			return g_innerOrigPresent(chain, syncInterval, flags);
		tl_inGamePresent = false;  // one frame per wrapper Present
		g_innerHit = true;
		return g_presenter.OnPresent(chain, syncInterval, flags);
	}

	HRESULT WINAPI CreateDeviceAndSwapChainHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
		const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain,
		ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
	{
		thread_local bool inside = false;
		if (inside)  // a plugin hook below us called the import again
			return g_firstCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, desc, chain, device, level, context);
		struct Guard { bool& v; ~Guard() { v = false; } } guard{ inside };
		inside = true;
		F4SE::log::info("f4sr: XeFG: D3D11CreateDeviceAndSwapChain reached the redirect (window {:p}, {}x{})",
			desc ? static_cast<void*>(desc->OutputWindow) : nullptr, desc ? desc->BufferDesc.Width : 0u, desc ? desc->BufferDesc.Height : 0u);
		if (g_redirectUsed || !desc || !chain || !device || !desc->OutputWindow)
			return g_origCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, desc, chain, device, level, context);
		g_redirectUsed = true;
		UINT w = desc->BufferDesc.Width, h = desc->BufferDesc.Height;
		if (!w || !h) {
			RECT rc{};
			GetClientRect(desc->OutputWindow, &rc);
			w = static_cast<UINT>(rc.right - rc.left);
			h = static_cast<UINT>(rc.bottom - rc.top);
		}
		HWND hidden = CreateHiddenWindow(w, h);
		if (!hidden) {
			F4SE::log::error("f4sr: XeFG: hidden window creation failed ({}); stock swap chain", GetLastError());
			return g_origCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, desc, chain, device, level, context);
		}
		DXGI_SWAP_CHAIN_DESC redirected = *desc;
		redirected.OutputWindow = hidden;
		redirected.Windowed = TRUE;
		redirected.BufferDesc.Width = w;
		redirected.BufferDesc.Height = h;
		HRESULT hr = g_origCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, &redirected, chain, device, level, context);
		if (SUCCEEDED(hr) && *chain && *device && g_presenter.Attach(*device, *chain, desc->OutputWindow, hidden, w, h, desc->BufferDesc.Format))
			return hr;
		F4SE::log::error("f4sr: XeFG: attach failed (hr=0x{:08X}); recreating the stock swap chain on the game window", static_cast<std::uint32_t>(hr));
		g_presenter.Teardown();
		if (SUCCEEDED(hr)) {
			if (context && *context) { (*context)->Release(); *context = nullptr; }
			if (*chain) { (*chain)->Release(); *chain = nullptr; }
			if (*device) { (*device)->Release(); *device = nullptr; }
		}
		DestroyWindow(hidden);
		return g_origCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, desc, chain, device, level, context);
	}
}

namespace f4sr::xefg
{
	void InstallSwapChainRedirect()
	{
		if (GetSettings().frameGenEngine != 1) return;
		const auto original = REL::PatchIAT(&CreateDeviceAndSwapChainHook, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
		g_origCreate = reinterpret_cast<CreateDeviceAndSwapChain_t>(original);
		g_firstCreate = g_origCreate;
		if (!g_origCreate) {
			F4SE::log::error("f4sr: XeFG: d3d11.dll!D3D11CreateDeviceAndSwapChain import not found; XeSS-FG unavailable");
			return;
		}
		F4SE::log::info("f4sr: XeFG: swap chain creation redirected (import of D3D11CreateDeviceAndSwapChain)");
	}

	// Plugins loaded after f4sr (e.g. NVIDIA_Reflex) may re-patch the same
	// import and call d3d11.dll directly, skipping our hook. Once every plugin
	// has loaded, take the outermost position again and chain to whatever
	// hook now sits in the import.
	void ReclaimSwapChainRedirect()
	{
		if (GetSettings().frameGenEngine != 1 || !g_firstCreate) return;
		auto* slot = reinterpret_cast<std::uintptr_t*>(REL::GetIATAddr("d3d11.dll", "D3D11CreateDeviceAndSwapChain"));
		if (!slot) return;
		const auto current = *slot;
		if (current == reinterpret_cast<std::uintptr_t>(&CreateDeviceAndSwapChainHook)) {
			F4SE::log::info("f4sr: XeFG: swap chain redirect still outermost after plugin load");
			return;
		}
		char owner[MAX_PATH] = "unknown";
		HMODULE module{};
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(current), &module))
			GetModuleFileNameA(module, owner, MAX_PATH);
		REL::PatchIAT(&CreateDeviceAndSwapChainHook, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
		g_origCreate = reinterpret_cast<CreateDeviceAndSwapChain_t>(current);
		F4SE::log::info("f4sr: XeFG: import was re-patched by {} after f4sr loaded; redirect reinstalled in front of it", owner);
	}

	bool Attached() { return g_presenter.attached; }

	void OnFrameStart()
	{
		auto& p = g_presenter;
		if (!p.attached || p.renderStarted) return;
		if (!p.simStarted) {
			p.Marker(p.frameId, XELL_SIMULATION_START);
			p.simStarted = true;
		}
		p.Marker(p.frameId, XELL_SIMULATION_END);
		p.Marker(p.frameId, XELL_RENDERSUBMIT_START);
		p.renderStarted = true;
	}

	void CaptureHudless(void* rtManager)
	{
		auto& p = g_presenter;
		p.hudlessReady = false;
		if (!p.attached || !rtManager || !GetSettings().enableFrameGeneration) return;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!data || !data->context) return;
		// Logical kFrameBuffer through the manager's rtID map (as FrameGen::CaptureHudless).
		const auto slot = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(rtManager) + 0xDC4);
		if (slot < 0 || slot >= 101) return;
		auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderTargets[slot].rtView);
		if (!rtv) return;
		ComPtr<ID3D11Resource> resource;
		ComPtr<ID3D11Texture2D> source;
		rtv->GetResource(&resource);
		if (!resource || FAILED(resource.As(&source))) return;
		D3D11_TEXTURE2D_DESC d{};
		source->GetDesc(&d);
		if (d.Width != p.width || d.Height != p.height || NonSrgb(d.Format) != p.format || d.SampleDesc.Count != 1) {
			static std::uint64_t warned = 0;
			if ((warned++ % 600) == 0)
				F4SE::log::warn("f4sr: XeFG HUD-less source {}x{} fmt={} does not match the proxy {}x{} fmt={}", d.Width, d.Height,
					static_cast<unsigned>(d.Format), p.width, p.height, static_cast<unsigned>(p.format));
			return;
		}
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		ctx->CopyResource(p.hudless.tex11.Get(), source.Get());
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
		for (auto* v : rtvs) if (v) v->Release();
		if (dsv) dsv->Release();
		p.hudlessReady = true;
	}

	void SetFrameInputs(ID3D11DeviceContext* ctx, const XeFGFrameInputs& in)
	{
		auto& p = g_presenter;
		p.inputsReady = false;
		if (!p.attached || !ctx || !in.depth || !in.motionVectors || !in.renderWidth || !in.renderHeight) return;
		auto ensure = [&](SharedTexture& t, ID3D11Texture2D* src) {
			D3D11_TEXTURE2D_DESC d{};
			src->GetDesc(&d);
			if (t.tex11 && t.desc.Width == d.Width && t.desc.Height == d.Height && t.desc.Format == d.Format) return true;
			t = {};
			return p.CreateTexture(t, d.Width, d.Height, d.Format);
		};
		if (!ensure(p.depth, in.depth) || !ensure(p.motion, in.motionVectors)) {
			F4SE::log::error("f4sr: XeFG depth/motion texture creation failed");
			return;
		}
		const D3D11_BOX box{ 0, 0, 0, in.renderWidth, in.renderHeight, 1 };
		ctx->CopySubresourceRegion(p.depth.tex11.Get(), 0, 0, 0, 0, in.depth, 0, &box);
		ctx->CopySubresourceRegion(p.motion.tex11.Get(), 0, 0, 0, 0, in.motionVectors, 0, &box);
		p.inputs = in;
		p.inputs.depth = p.inputs.motionVectors = nullptr;
		p.camera = CaptureFrameCamera();
		p.inputsReady = true;
	}
}
