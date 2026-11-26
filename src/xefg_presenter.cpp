#include "vram_budget.h"
#include "sr_status.h"
#include "version.h"
#include "xefg_presenter.h"
#include "ui_redirect.h"
#include "scaleform_scale.h"
#include "frame_pipeline.h"
#include "output_scale.h"
#include "swapchain_inner.h"
#include "reshade_preui.h"
#include "isize_reads.h"
#include "hfpf_display.h"

#include <Windows.h>
#include <intrin.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "ffx_antilag2_dx12.h"
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "frame_pipeline_detail.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

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
	using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	ResizeBuffers_t g_origResizeBuffers{ nullptr };
	using SetFullscreenState_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
	SetFullscreenState_t g_origSetFullscreenState{ nullptr };

	// The game chain lives windowed on a hidden window and the proxy chain
	// presents on the game window, so exclusive fullscreen (bFull Screen=1)
	// must not reach it: on Windows it would switch the display mode for the
	// hidden chain. The game window already covers the monitor, so the
	// result is borderless fullscreen.
	HRESULT STDMETHODCALLTYPE SetFullscreenStateThunk(IDXGISwapChain* chain, BOOL fullscreen, IDXGIOutput* target)
	{
		if (fullscreen) {
			static bool told = false;
			if (!told) {
				told = true;
				F4SE::log::info("f4sr: XeFG: exclusive fullscreen request on the game chain kept windowed (runs as borderless fullscreen)");
			}
			return S_OK;
		}
		return g_origSetFullscreenState(chain, fullscreen, target);
	}
	UINT g_gameWidth{}, g_gameHeight{};  // bootstrap size of the game chain on the 1x1 hidden window
	struct RenderSize
	{
		UINT width{}, height{};
		bool operator==(const RenderSize&) const = default;
	};
	struct PathBResizeAttempt
	{
		IDXGISwapChain* chain;
		HWND window;
		UINT width, height;
		unsigned calls{};
		HRESULT result{ E_UNEXPECTED };
	};
	thread_local PathBResizeAttempt* tl_resizeAttempt{};
	IDXGISwapChain* g_gameChainForResize{};  // the game chain whose ResizeBuffers is redirected
	std::atomic<std::uint64_t> g_latchedRenderSize{};  // width in high word, height in low word

	RenderSize CurrentRenderSize()
	{
		// Input/window hooks may run off the render thread. Publish both
		// dimensions together after a resize, never as separate live values.
		if (const auto size = g_latchedRenderSize.load(std::memory_order_acquire))
			return { static_cast<UINT>(size >> 32), static_cast<UINT>(size) };
		return { g_gameWidth, g_gameHeight };
	}

	// Path B owns the game chain's size: another mod (HighFPSPhysicsFix
	// [Display], 2026-11-25 reports) that hands it the display size would
	// make super resolution run as Native AA. The size is forced back and
	// the override is reported once per distinct request.
	void ForceGameChainSize(UINT& w, UINT& h, RenderSize size);  // after EnbBeforeUpscaling

	void ReportForcedSize(const char* where, UINT askedW, UINT askedH, UINT w, UINT h)
	{
		static std::uint64_t lastKey{};
		static int lines{};
		const std::uint64_t key = (static_cast<std::uint64_t>(askedW) << 48) ^ (static_cast<std::uint64_t>(askedH) << 32) ^
			(static_cast<std::uint64_t>(w) << 16) ^ h;
		sr_status::Raise(sr_status::kAlertSizeForced);
		if (key == lastKey || lines >= 8) return;
		lastKey = key;
		++lines;
		F4SE::log::warn("f4sr: Path B: {} asked for {}x{}; forced to the render size {}x{} (another mod changed the game's buffer size). {}",
			where, askedW, askedH, w, h, hfpf::Read().summary);
	}

	// ResizeBuffers(0, 0) sizes the buffers from the output window, which for
	// the game chain is the 1x1 hidden window. Keep the game's own size.
	// During a Path B hot resize every call on the game chain takes the
	// target size: HighFPSPhysicsFix BorderlessUpscale=true replaces the
	// engine's (0, 0) with the real window size (2026-11-25).
	HRESULT STDMETHODCALLTYPE ResizeBuffersThunk(IDXGISwapChain* chain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags)
	{
		if (auto* attempt = tl_resizeAttempt; attempt && chain == attempt->chain) {
			++attempt->calls;
			attempt->result = g_origResizeBuffers(chain, count, attempt->width, attempt->height, format, flags);
			F4SE::log::info("f4sr: Path B: game chain ResizeBuffers {}x{} (asked {}x{}) hr=0x{:08X}",
				attempt->width, attempt->height, w, h, static_cast<std::uint32_t>(attempt->result));
			return attempt->result;
		}
		// Without ENB the patched vtable is ReShade's wrapper class, which
		// also wraps the XeFG output chain: leave every other chain alone.
		if (chain != g_gameChainForResize) return g_origResizeBuffers(chain, count, w, h, format, flags);
		const auto size = CurrentRenderSize();
		UINT rw = w ? w : size.width, rh = h ? h : size.height;
		ForceGameChainSize(rw, rh, size);
		static std::uint32_t logged = 0;
		if (logged++ < 8)
			F4SE::log::info("f4sr: XeFG: game chain ResizeBuffers {}x{} -> {}x{} (format {})", w, h, rw, rh, static_cast<unsigned>(format));
		return g_origResizeBuffers(chain, count, rw, rh, format, flags);
	}
	bool g_redirectUsed{ false };
	bool g_redirectInstalled{ false };

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
	UINT g_focusRenderW{}, g_focusRenderH{};
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
		if (!hwnd || hwnd != g_focusHidden) return g_origScreenToClient(hwnd, point);
		const BOOL ok = g_origScreenToClient(g_focusReal, point);
		RECT r{};
		if (ok && g_focusRenderW && g_focusRenderH && GetClientRect(g_focusReal, &r) && r.right > 0 && r.bottom > 0) {
			const auto size = CurrentRenderSize();
			point->x = MulDiv(point->x, static_cast<int>(size.width), r.right);
			point->y = MulDiv(point->y, static_cast<int>(size.height), r.bottom);
		}
		return ok;
	}
	void RedirectWrapperFocus(HMODULE wrapper, HWND real, HWND hidden, UINT renderW, UINT renderH)
	{
		g_focusReal = real;
		g_focusHidden = hidden;
		g_focusRenderW = renderW;
		g_focusRenderH = renderH;
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
						// The first Presents are followed by the engine's startup
						// loading; a pause there is expected, not a stall.
						const auto done = g_presentsDone.load();
						if (done < 10)
							F4SE::log::info("f4sr: XeFG watchdog: no game Present for {} ms during startup (presents completed {})", now - last, done);
						else
							F4SE::log::warn("f4sr: XeFG watchdog: no game Present for {} ms (presents completed {}); the stall is outside the XeFG present path",
								now - last, done);
						idleReported = true;
					} else if (last && now - last <= 5000) {
						idleReported = false;
					}
				}
			}
		}).detach();
	}

	constexpr UINT kProxyBuffers = 8;  // 4X bursts queue up to 4 flips per real frame
	constexpr UINT kPlainBuffers = 3;  // UI Native without XeSS-FG: one real frame per Present

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

	// Set around the first xellSleep after the low-latency setting switches
	// back to XeLL. While Anti-Lag 2 or Off was in effect xellSleep was not
	// called, so XeLL still expects the frame after the last one it slept on and
	// reports the jump once (it resynchronises on that call).
	std::atomic<bool> g_xellResync{};

	void XellLog(const char* message, xell_logging_level_t level)
	{
		if (message && g_xellResync.load(std::memory_order_relaxed) && std::string_view(message).starts_with("Sleep() called for frame")) {
			F4SE::log::info("f4sr: libxell: {} (expected once after switching back to XeLL)", message);
			return;
		}
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

	UINT g_displayWidth{}, g_displayHeight{};  // Path B output (window) size, once known

	// Set when Path B failed before any of its hooks were installed: this
	// session runs the Classic pipeline instead.
	std::atomic<bool> g_pathBOff{};

	// Path B needs the proxy chain, so it runs with the XeSS-FG engine (frame
	// generation on or off).
	bool EnbBeforeUpscaling()
	{
		const auto& s = GetSettings();
		return s.enbBeforeUpscaling && s.frameGenEngine == 1 && !g_pathBOff.load(std::memory_order_relaxed);
	}

	void ForceGameChainSize(UINT& w, UINT& h, RenderSize size)
	{
		// A Path B hot resize (RebuildEngine) sets its own size.
		if (tl_resizeAttempt || !EnbBeforeUpscaling() || !g_displayWidth || !size.width || !size.height || (w == size.width && h == size.height)) return;
		ReportForcedSize("a game chain ResizeBuffers", w, h, size.width, size.height);
		w = size.width;
		h = size.height;
	}

	// Path B failure: the MCM choice switches to Classic for the next start
	// and the error panel says so. `early`: nothing of Path B is installed yet,
	// so this session continues on the Classic pipeline.
	void FallBackFromPathB(const char* reason, bool early)
	{
		static std::atomic<bool> reported{};
		if (reported.exchange(true)) return;
		if (early) {
			// Window, cursor, INI and Scaleform hooks may already be in place;
			// they check g_pathBOff and pass everything through from now on.
			g_pathBOff = true;
			if (g_displayWidth && g_displayHeight) {
				if (auto* sw = RE::GetINISetting("iSize W:Display")) sw->SetInt(static_cast<int>(g_displayWidth));
				if (auto* sh = RE::GetINISetting("iSize H:Display")) sh->SetInt(static_cast<int>(g_displayHeight));
			}
		}
		F4SE::log::error("f4sr: UI native pipeline (Path B) failed: {}; {}", reason,
			early ? "this session uses the Classic pipeline" : "restart the game, the next start uses the Classic pipeline");
		WriteMcmValue("iPipeline", "0");
		WriteMcmValue("bPipelineFellBack", "1");
		sr_status::Raise(early ? sr_status::kAlertPipelineEarly : sr_status::kAlertPipeline);
	}

	// Path B: the game's window is created at iSize before plugins load, and
	// under gamescope a later resize reaches Win32 but not the presented
	// surface. So iSize stays at the display size (the window), and at plugin
	// load, before the renderer exists, the in-memory iSize is lowered to the
	// render size: the swap chain and every engine target follow it.

	float RenderRatio(int mode)
	{
		switch (mode) {
		case 1: return 1.0f / 1.5f;
		case 2: return 1.0f / 1.7f;
		case 3: return 1.0f / 2.0f;
		default: return 1.0f;
		}
	}

	bool ShrinkGameSize(const char* when)
	{
		if (g_displayWidth) return true;
		auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
		auto* ini = RE::INISettingCollection::GetSingleton();
		auto* sw = RE::GetINISetting("iSize W:Display");
		auto* sh = RE::GetINISetting("iSize H:Display");
		if (!sw || !sh) {
			// The plugin-load attempt usually runs before the INI collections
			// exist; swap chain creation retries, and only that one is an error.
			const bool final = std::strcmp(when, "swap chain creation") == 0;
			if (final)
				F4SE::log::error("f4sr: ENB before upscaling ({}): iSize settings not found (prefs collection {:p}, ini collection {:p}, W {:p}, H {:p})",
					when, static_cast<void*>(prefs), static_cast<void*>(ini), static_cast<void*>(sw), static_cast<void*>(sh));
			else
				F4SE::log::info("f4sr: ENB before upscaling ({}): iSize settings not loaded yet; retried at swap chain creation", when);
			return false;
		}
		const int dw = sw->GetInt(), dh = sh->GetInt();
		const float ratio = RenderRatio(GetSettings().qualityMode);
		if (dw <= 0 || dh <= 0) return false;
		g_displayWidth = static_cast<UINT>(dw);
		g_displayHeight = static_cast<UINT>(dh);
		if (ratio >= 1.0f) {
			F4SE::log::info("f4sr: ENB before upscaling ({}): iSize {}x{}, ratio {:.3f}: the game renders at the display size", when, dw, dh, ratio);
			return true;
		}
		const int rw = static_cast<int>(std::lround(dw * ratio)) & ~1, rh = static_cast<int>(std::lround(dh * ratio)) & ~1;
		sw->SetInt(rw);
		sh->SetInt(rh);
		F4SE::log::info("f4sr: ENB before upscaling ({}): game size {}x{} -> {}x{} (ratio {:.3f}); window and output stay {}x{}", when, dw, dh, rw, rh, ratio, dw, dh);
		return true;
	}

	// Path B: the game sees a virtual display of the game size. Fallout4.exe's
	// own window, screen and cursor queries are answered in game-size
	// coordinates; the real window and the proxy chain stay display-size.
	HWND g_virtualWindow{};
	using GetRect_t = BOOL(WINAPI*)(HWND, LPRECT);
	using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
	using SetCursorPos_t = BOOL(WINAPI*)(int, int);
	using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
	using GetClipCursor_t = BOOL(WINAPI*)(LPRECT);
	using GetSystemMetrics_t = int(WINAPI*)(int);
	GetRect_t g_origGetClientRect{}, g_origGetWindowRect{};
	GetCursorPos_t g_origGetCursorPos{};
	SetCursorPos_t g_origSetCursorPos{};
	ClipCursor_t g_origClipCursor{};
	GetClipCursor_t g_origGetClipCursor{};
	GetSystemMetrics_t g_origGetSystemMetrics{};

	bool Virtual(RenderSize size) { return !g_pathBOff.load(std::memory_order_relaxed) && g_virtualWindow && g_displayWidth && g_displayHeight && size.width && size.height; }
	LONG ScaleCoordinate(LONG x, UINT from, UINT to) { return static_cast<LONG>(std::lround(static_cast<double>(x) * to / from)); }
	LONG ToGameX(LONG x, RenderSize size) { return ScaleCoordinate(x, g_displayWidth, size.width); }
	LONG ToGameY(LONG y, RenderSize size) { return ScaleCoordinate(y, g_displayHeight, size.height); }
	LONG ToRealX(LONG x, RenderSize size) { return ScaleCoordinate(x, size.width, g_displayWidth); }
	LONG ToRealY(LONG y, RenderSize size) { return ScaleCoordinate(y, size.height, g_displayHeight); }

	BOOL WINAPI GetClientRectHook(HWND hwnd, LPRECT rc)
	{
		const BOOL ok = g_origGetClientRect(hwnd, rc);
		if (const auto size = CurrentRenderSize(); ok && rc && Virtual(size) && hwnd == g_virtualWindow)
			*rc = RECT{ 0, 0, ToGameX(rc->right, size), ToGameY(rc->bottom, size) };
		return ok;
	}
	BOOL WINAPI GetWindowRectHook(HWND hwnd, LPRECT rc)
	{
		const BOOL ok = g_origGetWindowRect(hwnd, rc);
		if (const auto size = CurrentRenderSize(); ok && rc && Virtual(size) && hwnd == g_virtualWindow)
			*rc = RECT{ ToGameX(rc->left, size), ToGameY(rc->top, size), ToGameX(rc->right, size), ToGameY(rc->bottom, size) };
		if (const auto* attempt = tl_resizeAttempt; ok && rc && attempt && hwnd == attempt->window) {
			rc->right = rc->left + static_cast<LONG>(attempt->width);
			rc->bottom = rc->top + static_cast<LONG>(attempt->height);
		}
		return ok;
	}
	BOOL WINAPI GetCursorPosHook(LPPOINT p)
	{
		const BOOL ok = g_origGetCursorPos(p);
		if (const auto size = CurrentRenderSize(); ok && p && Virtual(size)) *p = POINT{ ToGameX(p->x, size), ToGameY(p->y, size) };
		return ok;
	}
	BOOL WINAPI SetCursorPosHook(int x, int y)
	{
		const auto size = CurrentRenderSize();
		return Virtual(size) ? g_origSetCursorPos(ToRealX(x, size), ToRealY(y, size)) : g_origSetCursorPos(x, y);
	}
	BOOL WINAPI ClipCursorHook(const RECT* rc)
	{
		const auto size = CurrentRenderSize();
		if (!rc || !Virtual(size)) return g_origClipCursor(rc);
		const RECT real{ ToRealX(rc->left, size), ToRealY(rc->top, size), ToRealX(rc->right, size), ToRealY(rc->bottom, size) };
		return g_origClipCursor(&real);
	}
	BOOL WINAPI GetClipCursorHook(LPRECT rc)
	{
		const BOOL ok = g_origGetClipCursor(rc);
		if (const auto size = CurrentRenderSize(); ok && rc && Virtual(size))
			*rc = RECT{ ToGameX(rc->left, size), ToGameY(rc->top, size), ToGameX(rc->right, size), ToGameY(rc->bottom, size) };
		return ok;
	}
	int WINAPI GetSystemMetricsHook(int index)
	{
		const int v = g_origGetSystemMetrics(index);
		const auto size = CurrentRenderSize();
		if (!Virtual(size)) return v;
		switch (index) {
		case SM_CXSCREEN: case SM_CXVIRTUALSCREEN: case SM_CXFULLSCREEN: case SM_CXMAXIMIZED: return ToGameX(v, size);
		case SM_CYSCREEN: case SM_CYVIRTUALSCREEN: case SM_CYFULLSCREEN: case SM_CYMAXIMIZED: return ToGameY(v, size);
		default: return v;
		}
	}

	// Path B: the game's render resolution is its iSize. The game reads iSize
	// through GetPrivateProfileIntA/StringA when it loads Fallout4Prefs.ini;
	// answering with the render size there makes every system that sizes
	// itself from the screen (renderer, ENB, UI) start at the render size.
	// The value written back keeps the player's display size.
	using GetProfileInt_t = UINT(WINAPI*)(LPCSTR, LPCSTR, INT, LPCSTR);
	using GetProfileString_t = DWORD(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
	using WriteProfileString_t = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
	GetProfileInt_t g_origGetProfileInt{};
	GetProfileString_t g_origGetProfileString{};
	WriteProfileString_t g_origWriteProfileString{};

	int SizeKey(LPCSTR key)  // 1 = width, 2 = height
	{
		if (!key || (reinterpret_cast<std::uintptr_t>(key) >> 16) == 0) return 0;
		if (_stricmp(key, "iSize W") == 0) return 1;
		if (_stricmp(key, "iSize H") == 0) return 2;
		return 0;
	}

	// The display size as found in the player's INI files (index 1 = width,
	// 2 = height); this is what gets written back.
	UINT g_fileDisplay[3]{};

	const char* IniName(LPCSTR file)
	{
		if (!file) return "(win.ini)";
		const char* slash = std::strrchr(file, '\\');
		const char* fwd = std::strrchr(file, '/');
		const char* name = slash > fwd ? slash : fwd;
		return name ? name + 1 : file;
	}

	void LogSizeRead(int which, LPCSTR file, const char* what, UINT read, UINT answer)
	{
		static std::atomic<int> logged{};
		if (logged.fetch_add(1, std::memory_order_relaxed) >= 32) return;
		F4SE::log::info("f4sr: ENB before upscaling: {} from {}: {} {} -> {}", which == 1 ? "iSize W" : "iSize H", IniName(file), what, read, answer);
	}

	// A value the game read from `file`. Only values really in the file are
	// display sizes; a missing key returns the game's own default (its
	// current, already lowered value), which is handed back unchanged.
	UINT RenderValue(int which, UINT value, LPCSTR section, LPCSTR key, LPCSTR file)
	{
		if (value == 0 || g_pathBOff.load(std::memory_order_relaxed)) return value;
		unsigned monW{}, monH{};
		isize_reads::MonitorMode(monW, monH);
		const auto a = isize_reads::Decide(which, value, section, key, file, RenderRatio(GetSettings().qualityMode), monW, monH);
		if (!a.present) {
			LogSizeRead(which, file, "absent, default", value, value);
			return value;
		}
		if (const auto size = g_latchedRenderSize.load(std::memory_order_acquire)) {
			const UINT render = which == 1 ? static_cast<UINT>(size >> 32) : static_cast<UINT>(size);
			LogSizeRead(which, file, "latched", value, render);
			return render;
		}
		g_fileDisplay[which] = value;
		if (a.implausible) {
			static std::atomic<bool> reported{};
			if (!reported.exchange(true)) {
				F4SE::log::warn("f4sr: ENB before upscaling: {} sets iSize {}x{}, below half of the monitor ({}x{}); if that is not your choice, set iSize W/H in Fallout4Prefs.ini to your resolution",
					IniName(file), a.fileW, a.fileH, monW, monH);
				sr_status::Raise(sr_status::kAlertSmallIsize);
			}
		}
		if (a.render != a.display) {
			if (which == 1) { g_displayWidth = a.display; g_gameWidth = a.render; }
			else { g_displayHeight = a.display; g_gameHeight = a.render; }
		}
		LogSizeRead(which, file, "display", value, a.render);
		return a.render;
	}

	UINT WINAPI GetProfileIntHook(LPCSTR section, LPCSTR key, INT def, LPCSTR file)
	{
		const UINT v = g_origGetProfileInt(section, key, def, file);
		const int which = SizeKey(key);
		return which ? RenderValue(which, v, section, key, file) : v;
	}

	DWORD WINAPI GetProfileStringHook(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
	{
		const DWORD n = g_origGetProfileString(section, key, def, out, size, file);
		const int which = SizeKey(key);
		if (!which || !out || n == 0) return n;
		char* end{};
		const unsigned long value = std::strtoul(out, &end, 10);
		if (end == out || value == 0) return n;
		const auto text = std::to_string(RenderValue(which, static_cast<UINT>(value), section, key, file));
		if (text.size() + 1 > size) return n;
		std::memcpy(out, text.c_str(), text.size() + 1);
		return static_cast<DWORD>(text.size());
	}

	// The game writes its in-memory iSize (the render size); the file keeps
	// the display size it had.
	BOOL WINAPI WriteProfileStringHook(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file)
	{
		const int which = SizeKey(key);
		UINT display = which ? g_fileDisplay[which] : 0;
		if (which && !display) display = which == 1 ? g_displayWidth : g_displayHeight;
		if (which && display) display = isize_reads::WriteValue(section, key, file, display);
		if (which && display && value && !g_pathBOff.load(std::memory_order_relaxed)) {
			const auto text = std::to_string(display);
			static std::atomic<int> logged{};
			if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
				F4SE::log::info("f4sr: ENB before upscaling: {} write to {}: {} kept as {}", which == 1 ? "iSize W" : "iSize H", IniName(file), value, text);
			return g_origWriteProfileString(section, key, text.c_str(), file);
		}
		return g_origWriteProfileString(section, key, value, file);
	}

	// The game window is display-size; its size messages and client mouse
	// coordinates are given to the game in render-size units.
	WNDPROC g_gameWndProc{};
	LRESULT CALLBACK GameWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (g_displayWidth && g_displayHeight && !g_pathBOff.load(std::memory_order_relaxed)) {
			const auto size = CurrentRenderSize();
			const UINT rw = size.width ? size.width : static_cast<UINT>(std::lround(g_displayWidth * RenderRatio(GetSettings().qualityMode))) & ~1u;
			const UINT rh = size.height ? size.height : static_cast<UINT>(std::lround(g_displayHeight * RenderRatio(GetSettings().qualityMode))) & ~1u;
			auto scale = [&](LPARAM l) {
				const int x = static_cast<short>(LOWORD(l)), y = static_cast<short>(HIWORD(l));
				return MAKELPARAM(static_cast<int>(std::lround(static_cast<double>(x) * rw / g_displayWidth)),
					static_cast<int>(std::lround(static_cast<double>(y) * rh / g_displayHeight)));
			};
			switch (msg) {
			case WM_SIZE:
				lp = scale(lp);
				break;
			case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDOWN: case WM_XBUTTONUP:
				lp = scale(lp);
				break;
			default: break;
			}
		}
		return CallWindowProcA(g_gameWndProc, hwnd, msg, wp, lp);
	}

	// Path B: the game reads iSize for its window right after loading its INI
	// files, before the renderer and the UI are set up. That window creation is
	// the point to lower iSize in memory (everything initialised later follows
	// it) and to create the window at the display size instead.
	using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
	using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
	CreateWindowExA_t g_origCreateWindowA{};
	SetWindowPos_t g_origSetWindowPos{};
	HWND g_gameWindow{};

	bool DisplayFromMonitor(int& x, int& y, int& w, int& h)
	{
		MONITORINFO mi{ sizeof(mi) };
		if (!GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) return false;
		x = mi.rcMonitor.left;
		y = mi.rcMonitor.top;
		w = g_displayWidth ? static_cast<int>(g_displayWidth) : mi.rcMonitor.right - mi.rcMonitor.left;
		h = g_displayHeight ? static_cast<int>(g_displayHeight) : mi.rcMonitor.bottom - mi.rcMonitor.top;
		return true;
	}

	HWND WINAPI CreateWindowExAHook(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
	{
		static int calls = 0;
		const bool topLevel = !parent && !(style & WS_CHILD);
		const bool first = topLevel && !g_gameWindow;
		if (calls++ < 8)
			F4SE::log::info("f4sr: ENB before upscaling: CreateWindowExA #{} class '{}' title '{}' style 0x{:X} {}x{} at {},{} parent {:p}", calls,
				(reinterpret_cast<std::uintptr_t>(cls) >> 16) ? cls : "(atom)", name ? name : "", style, w, h, x, y, static_cast<void*>(parent));
		if (first) {
			int dx{}, dy{}, dw{}, dh{};
			if (g_displayWidth && DisplayFromMonitor(dx, dy, dw, dh)) {
				x = dx;
				y = dy;
				w = dw;
				h = dh;
				F4SE::log::info("f4sr: ENB before upscaling: game window created at {}x{}", w, h);
			}
		}
		HWND hwnd = g_origCreateWindowA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
		if (first && hwnd) {
			g_gameWindow = hwnd;
			g_virtualWindow = hwnd;
			g_gameWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWindowProc)));
			F4SE::log::info("f4sr: ENB before upscaling: game window {:p} subclassed ({})", static_cast<void*>(hwnd), g_gameWndProc ? "ok" : "failed");
		}
		return hwnd;
	}

	BOOL WINAPI SetWindowPosHook(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
	{
		if (hwnd && hwnd == g_gameWindow && g_displayWidth && !g_pathBOff.load(std::memory_order_relaxed) && !(flags & SWP_NOSIZE)) {
			int dx{}, dy{}, dw{}, dh{};
			if (DisplayFromMonitor(dx, dy, dw, dh)) {
				static int logged = 0;
				if (logged++ < 4) F4SE::log::info("f4sr: ENB before upscaling: game SetWindowPos {}x{} -> {}x{}", cx, cy, dw, dh);
				x = dx;
				y = dy;
				cx = dw;
				cy = dh;
				flags &= ~static_cast<UINT>(SWP_NOMOVE);
			}
		}
		return g_origSetWindowPos(hwnd, after, x, y, cx, cy, flags);
	}

	void InstallVirtualDisplay()
	{
		g_origGetClientRect = reinterpret_cast<GetRect_t>(REL::PatchIAT(&GetClientRectHook, "user32.dll", "GetClientRect"));
		g_origGetWindowRect = reinterpret_cast<GetRect_t>(REL::PatchIAT(&GetWindowRectHook, "user32.dll", "GetWindowRect"));
		g_origGetCursorPos = reinterpret_cast<GetCursorPos_t>(REL::PatchIAT(&GetCursorPosHook, "user32.dll", "GetCursorPos"));
		g_origSetCursorPos = reinterpret_cast<SetCursorPos_t>(REL::PatchIAT(&SetCursorPosHook, "user32.dll", "SetCursorPos"));
		g_origClipCursor = reinterpret_cast<ClipCursor_t>(REL::PatchIAT(&ClipCursorHook, "user32.dll", "ClipCursor"));
		g_origGetClipCursor = reinterpret_cast<GetClipCursor_t>(REL::PatchIAT(&GetClipCursorHook, "user32.dll", "GetClipCursor"));
		g_origGetSystemMetrics = reinterpret_cast<GetSystemMetrics_t>(REL::PatchIAT(&GetSystemMetricsHook, "user32.dll", "GetSystemMetrics"));
		const bool all = g_origGetClientRect && g_origGetWindowRect && g_origGetCursorPos && g_origSetCursorPos && g_origClipCursor && g_origGetClipCursor && g_origGetSystemMetrics;
		F4SE::log::info("f4sr: ENB before upscaling: virtual display hooks {}", all ? "installed" : "INCOMPLETE");
		if (!all) {
			// Restore whatever was patched; a partial virtual display is worse than none.
			if (g_origGetClientRect) REL::PatchIAT(g_origGetClientRect, "user32.dll", "GetClientRect");
			if (g_origGetWindowRect) REL::PatchIAT(g_origGetWindowRect, "user32.dll", "GetWindowRect");
			if (g_origGetCursorPos) REL::PatchIAT(g_origGetCursorPos, "user32.dll", "GetCursorPos");
			if (g_origSetCursorPos) REL::PatchIAT(g_origSetCursorPos, "user32.dll", "SetCursorPos");
			if (g_origClipCursor) REL::PatchIAT(g_origClipCursor, "user32.dll", "ClipCursor");
			if (g_origGetClipCursor) REL::PatchIAT(g_origGetClipCursor, "user32.dll", "GetClipCursor");
			if (g_origGetSystemMetrics) REL::PatchIAT(g_origGetSystemMetrics, "user32.dll", "GetSystemMetrics");
			g_origGetClientRect = nullptr;
		}
	}

	// Path B: the engine sizes viewports (scene, Scaleform) from its record of
	// the window, which is the display size. Keep that record at the game size.
	void KeepEngineWindowSize(const char* when)
	{
		if (!g_displayWidth) return;
		auto* rd = RE::BSGraphics::RendererData::GetSingleton();
		if (!rd) return;
		auto& rw = rd->renderWindow[0];
		const auto size = CurrentRenderSize();
		const int gw = static_cast<int>(size.width), gh = static_cast<int>(size.height);
		if (!gw || !gh || (rw.windowWidth == gw && rw.windowHeight == gh)) return;
		static std::uint32_t logged = 0;
		if (logged++ < 8)
			F4SE::log::info("f4sr: ENB before upscaling ({}): engine window record {}x{} -> {}x{}", when, rw.windowWidth, rw.windowHeight, gw, gh);
		rw.windowWidth = gw;
		rw.windowHeight = gh;
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

	// AMD Anti-Lag 2 on the interop D3D12 device (Settings::lowLatency 2).
	// Frame types come from the provider's present thread through the pacing
	// hook; they are reported only while AL2 is active and generation is on.
	AMD::AntiLag2DX12::Context g_al2{};
	std::atomic<bool> g_al2FrameTypes{ false };
	void Al2FrameType(bool generated)
	{
		if (g_al2FrameTypes.load(std::memory_order_relaxed)) AMD::AntiLag2DX12::SetFrameGenFrameType(&g_al2, generated);
	}

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
		UINT width{}, height{};        // the game chain (render size under Path B)
		UINT outWidth{}, outHeight{};  // the proxy chain (display size)
		bool scaling{};                // Path B composition, including equal-size Native AA
		// Hot preset switching: a verified outer ENB wrapper with a hooked
		// inner Present, or no ENB in the stack (the engine's ResizeBuffers
		// then reaches ReShade or DXGI directly).
		bool enbResizeSupported{};
		output_scale::Scaler scaleBack, scaleHudless, enbDelta;
		ComPtr<ID3D11Texture2D> composed;  // Path B: display-size frame with the UI
		ComPtr<ID3D11Texture2D> enbBefore; // the render-size buffer snapshotted for this wrapper Present
		DXGI_FORMAT format{};
		SharedTexture back, hudless, depth, motion;
		SharedTexture reshadeDepth;  // render-size depth for ReShade's DEPTH (bReShadeDepth)
		std::atomic<std::uint32_t> reshadeDepthGeneration{};
		bool attached{};
		int maxInterpolations{ 1 };
		bool fgLoaded{};              // XeFG context and proxy chain loaded (else a plain chain)
		vram_budget::SdkBytes fgSdkBytes;  // XeFG internal resources (measured)
		int appliedInterpolations{ 0 };
		bool fgEnabled{};
		bool resetNext{ true };

		// Frame state. The XeLL frame id doubles as the XeFG present id.
		std::uint32_t frameId{ 1 };
		bool simStarted{}, renderStarted{};
		bool inputsReady{}, hudlessReady{}, hudlessFailed{};
		XeFGFrameInputs inputs{};
		std::optional<FrameCamera> camera;  // captured with the inputs, mid-frame

		// 5-second statistics.
		std::uint64_t statsTick{};
		std::uint64_t statReal{}, statFgOn{}, statPresented{}, statStatusFailed{}, statGenErrors{}, statGenWarnings{};
		std::uint64_t statHistogram[6]{};
		int lastGenResult{}, lastGenWarning{};
		const char* lastOff{ "not started" };

		// Hot diagnostics for low GPU clocks under the proxy chain (AMD report,
		// 2026-11-25). Re-read from f4sr.ini while running; not in the packaged ini.
		//   iXeLLSleep       1 = low-latency mode + xellSleep every frame (default)
		//                    0 = low-latency mode off, xellSleep not called
		//                    2 = low-latency mode on, xellSleep only while generating
		//                    3 = low-latency mode on, xellSleep never called
		// XeFG reads the mode when generation is enabled and keeps it: with the
		// mode off at that moment it presents no generated frames (no error)
		// until generation is toggled again, so only 0 turns the mode off.
		//   iXeFGGamePresent 1 = also Present the hidden game chain (interval 0,
		//                    DO_NOT_WAIT) so the D3D11 driver sees frame boundaries
		//   iXeFGFenceWait   0 = skip the D3D11 GPU wait on the D3D12 copy fence
		//                    (the next frame may overwrite a copy still being read)
		//   iXeFGPacing      0 = forward provider presents unpaced (xefg_pacing off)
		//   iXeFGBlockComp   1 = take the game thread's proxy Present time out of the pacing period (experiment, default off)
		int diagXellSleep{ 1 }, diagGamePresent{ 0 }, diagFenceWait{ 1 }, diagPacing{ 1 }, diagBlockComp{ 0 };
		double statBlockMs{};
		std::uint64_t statBlockCount{};
		int appliedLowLatency{ 1 };
		std::uint64_t statGamePresentOk{}, statGamePresentBusy{}, statGamePresentFailed{};
		HRESULT lastGamePresentHr{ S_OK };

		bool DiagNonDefault() const { return diagXellSleep != 1 || diagGamePresent != 0 || diagFenceWait != 1 || diagPacing != 1 || diagBlockComp != 0; }

		void ReadDiagnostics()
		{
			auto read = [](const char* key, int& value, int fallback, int lo, int hi) {
				int now = PluginIniInt(key, fallback);
				if (now < lo || now > hi) now = fallback;
				if (now != value) F4SE::log::info("f4sr: diagnostics: {} {} -> {}", key, value, now);
				value = now;
			};
			read("iXeLLSleep", diagXellSleep, 1, 0, 3);
			read("iXeFGGamePresent", diagGamePresent, 0, 0, 1);
			read("iXeFGFenceWait", diagFenceWait, 1, 0, 1);
			read("iXeFGPacing", diagPacing, 1, 0, 1);
			if (xefg_pacing::g.installed) xefg_pacing::g.enabled.store(diagPacing != 0, std::memory_order_relaxed);
			read("iXeFGBlockComp", diagBlockComp, 0, 0, 1);
			xefg_pacing::g.blockComp.store(diagBlockComp != 0, std::memory_order_relaxed);
		}

		// Low-latency mode as iXeLLSleep asks for it this frame; returns whether
		// xellSleep should run.
		bool ApplySleepMode(bool generating)
		{
			if (!xell) return false;
			const int want = diagXellSleep == 0 ? 0 : 1;
			const bool sleep = diagXellSleep == 1 || (diagXellSleep == 2 && generating);
			if (want != appliedLowLatency) {
				xell_sleep_params_t p{};
				p.bLowLatencyMode = static_cast<std::uint32_t>(want);
				const bool ok = Ok(api.xellSetSleepMode(xell, &p), "xellSetSleepMode");
				static std::uint32_t logged = 0;
				if (logged++ < 20) F4SE::log::info("f4sr: diagnostics: XeLL low-latency mode {} ({})", want ? "on" : "off", ok ? "ok" : "failed");
				appliedLowLatency = want;
			}
			return sleep;
		}

		// Low latency (Settings::lowLatency, live). XeLL's low-latency mode stays
		// on for every choice: XeFG reads it when generation is enabled and
		// presents no generated frames if it was off (verified 2026-11-25).
		enum class Latency { Off, XeLL, AntiLag2 };
		Latency latencyApplied{ Latency::XeLL };
		bool xellResyncNext{};  // next xellSleep follows a stretch without one
		bool al2Tried{}, al2Ok{};
		HRESULT al2Hr{ E_PENDING };
		std::uint64_t statAl2Calls{};
		double statAl2Ms{}, statAl2MaxMs{};

		static const char* LatencyName(Latency l)
		{
			return l == Latency::Off ? "off" : (l == Latency::AntiLag2 ? "Anti-Lag 2" : "XeLL");
		}

		static Latency LatencyWanted()
		{
			switch (GetSettings().lowLatency) {
			case 0: return Latency::Off;
			case 2: return Latency::AntiLag2;
			default: return Latency::XeLL;
			}
		}

		bool InitAntiLag2()
		{
			if (al2Tried) return al2Ok;
			al2Tried = true;
			// Provided by the AMD driver, or by vkd3d-proton under Proton. Kept loaded.
			LoadLibraryA("amdxc64.dll");
			al2Hr = interop.Device12() ? AMD::AntiLag2DX12::Initialize(&g_al2, interop.Device12()) : E_POINTER;
			al2Ok = al2Hr == S_OK;
			F4SE::log::info("f4sr: low latency: Anti-Lag 2 on the interop D3D12 device {} (hr 0x{:08X})",
				al2Ok ? "available" : "unavailable, XeLL is used instead", static_cast<std::uint32_t>(al2Hr));
			if (al2Ok) xefg_pacing::g.frameType.store(&Al2FrameType, std::memory_order_relaxed);
			return al2Ok;
		}

		// Switches the backend to the setting (AL2 falls back to XeLL); returns the one in effect.
		Latency ApplyLatency()
		{
			Latency want = LatencyWanted();
			if (want == Latency::AntiLag2 && !InitAntiLag2()) want = Latency::XeLL;
			if (want != latencyApplied) {
				if (latencyApplied == Latency::AntiLag2) AMD::AntiLag2DX12::Update(&g_al2, false, 0);
				if (want == Latency::XeLL) xellResyncNext = true;
				F4SE::log::info("f4sr: low latency: {} -> {}", LatencyName(latencyApplied), LatencyName(want));
				latencyApplied = want;
				statAl2Calls = 0;
				statAl2Ms = statAl2MaxMs = 0.0;
			}
			return want;
		}

		// Panel: only the diagnostic switches that differ from the default.
		std::string DiagPanel() const
		{
			static constexpr const char* kSleep[] = { "off", "always", "while generating", "mode on, no sleep" };
			std::string out;
			auto add = [&](std::string item) { out += (out.empty() ? "" : ", ") + item; };
			if (diagXellSleep != 1) add(std::string("XeLL sleep ") + kSleep[diagXellSleep]);
			if (diagGamePresent != 0) add("game Present on");
			if (diagFenceWait != 1) add("fence wait off");
			if (diagPacing != 1) add("pacing off");
			if (diagBlockComp != 0) add("block comp on");
			return out;
		}

		std::string DiagSummary() const
		{
			static constexpr const char* kSleep[] = { "off", "always", "while generating", "mode on, no sleep" };
			return std::format("XeLL sleep {}, game Present {}, fence wait {}, pacing {}, block comp {}, async present {}", kSleep[diagXellSleep],
				diagGamePresent ? "on" : "off", diagFenceWait ? "on" : "off", diagPacing ? "on" : "off", diagBlockComp ? "on" : "off",
				!asyncWanted ? "off" : (asyncActive ? std::format("on, queue {}", asyncQueue) : std::string("on (paused: ReShade depth)")));
		}

		// Asynchronous presentation (iXeFGAsyncPresent=1, read at attach;
		// experiment, default off). The provider's present thread holds each
		// real frame for the pacing interval and its queue is about one frame
		// deep, so the game thread used to block 6-10 ms in the proxy Present
		// (2026-11-26). With this on, the game thread only copies the frame's
		// shared textures into one of two hand-off slots and queues it; a
		// worker thread records the D3D12 copy, tags the XeFG inputs and calls
		// the proxy Present. The game thread waits when the worker is more than
		// iXeFGAsyncQueue frames behind. The textures the frame writes elsewhere (back
		// buffer, HUD-less, depth, motion) stay D3D11-only in this mode.
		// ReShade's output-chain depth is still read during the proxy Present,
		// so the synchronous path is used while ReShade wants depth. On the
		// Classic pipeline that is the whole session when ReShade Depth is on
		// (no before-upscaling pass; depth is read after the UI), so async
		// present only takes effect there with ReShade Depth off.
		struct AsyncSlot
		{
			SharedTexture back, hudless, depth, motion;
		};
		struct AsyncJob
		{
			std::uint32_t id{};
			std::uint64_t seq{};
			bool generate{};
			bool al2{};
			int interpolations{};
			UINT renderWidth{}, renderHeight{};
			xefg_swapchain_frame_constant_data_t constants{};
		};
		bool asyncWanted{};
		bool asyncActive{};
		// iXeFGAsyncQueue: frames the game thread may run ahead of the worker.
		// 1 = hand off only after the worker finished the previous frame (the
		// worker's Present overlaps the next game frame; little added latency).
		// 2 = one more frame queued (2026-11-26 test: about +25 ms latency).
		int asyncQueue{ 1 };
		AsyncSlot asyncSlots[2];
		std::uint64_t asyncSeq{};  // game thread: last frame handed off
		std::thread asyncThread;
		std::mutex asyncMutex;
		std::condition_variable asyncCv;
		std::optional<AsyncJob> asyncPending;  // guarded by asyncMutex
		bool asyncBusy{};                       // guarded by asyncMutex
		bool asyncStop{};                       // guarded by asyncMutex
		std::uint64_t asyncDone{};              // guarded by asyncMutex: last finished sequence
		std::atomic<bool> asyncDropped{};       // worker could not present a generation frame
		std::atomic<std::uint64_t> asyncWaitNs{}, asyncWaitFrames{}, asyncWaitMaxNs{};

		~Presenter()
		{
			// Static destruction at process exit runs after the OS ended the
			// worker; joining then would hang and destroying a joinable
			// std::thread would terminate.
			if (asyncThread.joinable()) asyncThread.detach();
		}

		void AsyncStart()
		{
			if (asyncThread.joinable()) return;
			{
				std::lock_guard lock(asyncMutex);
				asyncStop = false;
			}
			asyncThread = std::thread([this] { AsyncLoop(); });
		}

		// Blocks until the worker has finished every queued frame.
		void AsyncIdle()
		{
			if (!asyncThread.joinable()) return;
			std::unique_lock lock(asyncMutex);
			asyncCv.wait(lock, [&] { return !asyncPending && !asyncBusy; });
		}

		void AsyncShutdown()
		{
			if (asyncThread.joinable()) {
				{
					std::lock_guard lock(asyncMutex);
					asyncStop = true;
				}
				asyncCv.notify_all();
				asyncThread.join();
			}
			asyncPending.reset();
			asyncBusy = false;
			asyncDone = asyncSeq = 0;
			asyncActive = false;
			for (auto& slot : asyncSlots) slot = {};
		}

		void AsyncLoop()
		{
			SetThreadDescription(GetCurrentThread(), L"f4sr XeFG present");
			for (;;) {
				AsyncJob job;
				{
					std::unique_lock lock(asyncMutex);
					asyncCv.wait(lock, [&] { return asyncStop || asyncPending.has_value(); });
					if (!asyncPending) return;  // stop requested and nothing queued
					job = *asyncPending;
					asyncPending.reset();
					asyncBusy = true;
				}
				asyncCv.notify_all();
				AsyncPresent(job);
				{
					std::lock_guard lock(asyncMutex);
					asyncBusy = false;
					asyncDone = job.seq;
				}
				asyncCv.notify_all();
			}
		}

		// Game thread: async on while wanted and ReShade does not read depth.
		void AsyncSelect()
		{
			const bool want = asyncWanted && !reshade_preui::WantsDepth();
			if (want == asyncActive) return;
			if (want && (!interop.CreateAsyncFences())) {
				F4SE::log::error("f4sr: XeFG async present: shared fences unavailable; staying synchronous");
				asyncWanted = false;
				return;
			}
			AsyncIdle();
			if (want) AsyncStart();
			asyncActive = want;
			F4SE::log::info("f4sr: XeFG async present {}", want ? "on" : "paused (ReShade reads depth during the proxy Present)");
		}

		// Game thread: the slot texture matches the frame's texture. A new size
		// or format retires all in-flight work before the old one is released.
		bool AsyncEnsure(SharedTexture& slot, const SharedTexture& work)
		{
			if (!work.tex11) return false;
			if (slot.tex11 && slot.desc.Width == work.desc.Width && slot.desc.Height == work.desc.Height && slot.desc.Format == work.desc.Format &&
				slot.desc.BindFlags == work.desc.BindFlags)
				return true;
			AsyncIdle();
			if (slot.tex11 && !interop.Drain()) return false;
			slot = {};
			return interop.CreateSharedTexture(work.desc, slot);
		}

		// Game thread: hands the finished frame to the worker. Returns false
		// when nothing was queued (the frame is then not shown).
		bool AsyncSubmit(std::uint32_t id, const char* block)
		{
			const std::uint64_t seq = asyncSeq + 1;
			auto& slot = asyncSlots[seq & 1];
			const bool generate = !block;
			SetStep(Step::ProxyPresent, id);
			const auto waitStart = std::chrono::steady_clock::now();
			{
				// Slot seq & 1 was last used by seq - 2; at most asyncQueue frames in flight.
				std::unique_lock lock(asyncMutex);
				if (!asyncCv.wait_for(lock, std::chrono::seconds(5), [&] { return !asyncPending && asyncDone + asyncQueue >= seq; })) {
					static std::uint32_t warned = 0;
					if (warned++ < 10) F4SE::log::error("f4sr: XeFG async present: worker did not finish frame {} within 5 s", seq - asyncQueue);
					return false;
				}
			}
			const auto waitNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
			asyncWaitNs.fetch_add(waitNs, std::memory_order_relaxed);
			asyncWaitFrames.fetch_add(1, std::memory_order_relaxed);
			if (waitNs > asyncWaitMaxNs.load(std::memory_order_relaxed)) asyncWaitMaxNs.store(waitNs, std::memory_order_relaxed);

			SetStep(Step::Copy, id);
			if (!AsyncEnsure(slot.back, back) ||
				(generate && (!AsyncEnsure(slot.hudless, hudless) || !AsyncEnsure(slot.depth, depth) || !AsyncEnsure(slot.motion, motion)))) {
				static std::uint32_t warned = 0;
				if (warned++ < 10) F4SE::log::error("f4sr: XeFG async present: hand-off texture creation failed");
				return false;
			}
			// The slot's D3D12 reads from frame seq - 2 (its proxy Present
			// included) finish before D3D11 overwrites it.
			if (seq > 2 && !interop.WaitRetiredOnD3D11(ctx11.Get(), seq - 2)) return false;
			ctx11->CopyResource(slot.back.tex11.Get(), back.tex11.Get());
			if (generate) {
				ctx11->CopyResource(slot.hudless.tex11.Get(), hudless.tex11.Get());
				ctx11->CopyResource(slot.depth.tex11.Get(), depth.tex11.Get());
				ctx11->CopyResource(slot.motion.tex11.Get(), motion.tex11.Get());
			}
			if (!interop.SignalReadyFromD3D11(ctx11.Get(), seq)) return false;
			asyncSeq = seq;

			AsyncJob job{};
			job.id = id;
			job.seq = seq;
			job.generate = generate;
			job.al2 = latencyApplied == Latency::AntiLag2;
			job.interpolations = GetSettings().xefgInterpolations;
			if (generate) {
				job.renderWidth = inputs.renderWidth;
				job.renderHeight = inputs.renderHeight;
				auto& c = job.constants;
				Matrices(*camera, static_cast<float>(width) / static_cast<float>(height), c.viewMatrix, c.projectionMatrix);
				c.jitterOffsetX = inputs.jitterX;
				c.jitterOffsetY = inputs.jitterY;
				c.motionVectorScaleX = static_cast<float>(inputs.renderWidth);
				c.motionVectorScaleY = static_cast<float>(inputs.renderHeight);
				const bool dropped = asyncDropped.exchange(false, std::memory_order_relaxed);
				c.resetHistory = (inputs.reset || resetNext || dropped) ? 1u : 0u;
				const double paced = xefg_pacing::RenderTimeMs();
				c.frameRenderTime = paced > 0.0 ? static_cast<float>(paced) : inputs.frameTimeMs;
			}
			{
				std::lock_guard lock(asyncMutex);
				asyncPending = job;
			}
			asyncCv.notify_all();
			return true;
		}

		// Worker thread: what the synchronous path does after the D3D11 copy.
		// Owns fgEnabled, appliedInterpolations and the 5 s statistics while
		// async present is on; the panel reads them without locking.
		void AsyncPresent(const AsyncJob& job)
		{
			const auto& slot = asyncSlots[job.seq & 1];
			bool presented = false;
			if (interop.WaitReadyOnD3D12(job.seq)) {
				auto* list = interop.BeginFrame();
				ComPtr<ID3D12Resource> target;
				if (list && SUCCEEDED(proxy->GetBuffer(proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)))) {
					D3D12Interop::Transition(list, target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
					D3D12Interop::Transition(list, slot.back.res12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
					list->CopyResource(target.Get(), slot.back.res12.Get());
					D3D12Interop::Transition(list, slot.back.res12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
					D3D12Interop::Transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
					if (job.generate) {
						Tag(list, job.id, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, slot.hudless.res12.Get(), outWidth, outHeight);
						Tag(list, job.id, XEFG_SWAPCHAIN_RES_DEPTH, slot.depth.res12.Get(), job.renderWidth, job.renderHeight);
						Tag(list, job.id, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, slot.motion.res12.Get(), job.renderWidth, job.renderHeight);
						auto c = job.constants;
						api.tagConstants(fg, job.id, &c);
					}
					presented = interop.EndFrame();
				} else if (list) {
					interop.EndFrame();
				}
			}
			const bool fgFrame = presented && job.generate;
			if (fg && fgFrame != fgEnabled) {
				api.setEnabled(fg, fgFrame ? 1u : 0u);
				fgEnabled = fgFrame;
			}
			if (fgFrame) ApplyInterpolations(job.interpolations);
			else if (job.generate) asyncDropped.store(true, std::memory_order_relaxed);
			if (presented) {
				if (fg) api.setPresentId(fg, job.id);
				Marker(job.id, XELL_PRESENT_START);
				xefg_pacing::NoteAppSubmit(job.id);
				const bool al2Fg = fgFrame && job.al2;
				if (al2Fg) AMD::AntiLag2DX12::MarkEndOfFrameRendering(&g_al2);
				g_al2FrameTypes.store(al2Fg, std::memory_order_relaxed);
				const auto presentStart = std::chrono::steady_clock::now();
				const HRESULT hr12 = proxy->Present(1, 0);
				const auto blockNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - presentStart).count();
				if (fgFrame) xefg_pacing::NoteAppBlock(blockNs);
				statBlockMs += blockNs / 1e6;
				++statBlockCount;
				Marker(job.id, XELL_PRESENT_END);
				if (FAILED(hr12)) F4SE::log::warn("f4sr: XeFG proxy Present hr=0x{:08X}", static_cast<std::uint32_t>(hr12));
				xefg_swapchain_present_status_t status{};
				Stats(PresentStatus(status) ? &status : nullptr, fgFrame);
			} else {
				Marker(job.id, XELL_PRESENT_START);
				Marker(job.id, XELL_PRESENT_END);
				static std::uint64_t warned = 0;
				if ((warned++ % 600) == 0) F4SE::log::warn("f4sr: XeFG async present: frame {} not presented on the proxy chain", job.id);
			}
			// Always signalled, so the slot is reusable even after a failure.
			if (!interop.SignalRetiredFromD3D12(job.seq)) F4SE::log::error("f4sr: XeFG async present: retired signal failed");
		}

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

		// Game thread. Created on first use: frame generation off never needs it.
		bool EnsureHudless()
		{
			if (hudless.tex11) return true;
			if (hudlessFailed) return false;
			if (!CreateTexture(hudless, outWidth, outHeight, format, scaling)) {
				hudlessFailed = true;
				F4SE::log::error("f4sr: XeFG: shared HUD-less texture creation failed");
				return false;
			}
			vram_budget::Note("XeFG shared HUD-less texture (frame generation on)", outWidth, outHeight, format);
			return true;
		}

		bool CreateTexture(SharedTexture& out, UINT w, UINT h, DXGI_FORMAT f, bool renderTarget = false)
		{
			D3D11_TEXTURE2D_DESC d{};
			d.Width = w;
			d.Height = h;
			d.MipLevels = d.ArraySize = 1;
			d.Format = f;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0u);
			return interop.CreateSharedTexture(d, out);
		}

		void Teardown()
		{
			AsyncShutdown();
			if (fg) api.destroy(fg);
			if (xell) api.xellDestroy(xell);
			fgSdkBytes.Set(0);
			fg = nullptr;
			xell = nullptr;
			proxy.Reset();
			enbBefore.Reset();
			enbDelta = {};
			back = {};
			hudless = {};
			hudlessFailed = false;
			depth = {};
			motion = {};
			reshadeDepth = {};
			reshadeDepthGeneration.fetch_add(1);
			interop.Destroy();
			device11.Reset();
			ctx11.Reset();
			attached = false;
		}

		bool Attach(ID3D11Device* device, IDXGISwapChain* chain, HWND real, HWND hidden, UINT w, UINT h, UINT ow, UINT oh, DXGI_FORMAT gameFormat)
		{
			width = w;
			height = h;
			outWidth = ow;
			outHeight = oh;
			scaling = EnbBeforeUpscaling();
			format = NonSrgb(gameFormat);
			realWindow = real;
			hiddenWindow = hidden;
			if (!api.Load()) return false;
			vram_budget::Init(device);
			const auto vramDevice = vram_budget::LocalUsage();
			if (!interop.Create(device)) {
				F4SE::log::error("f4sr: XeFG: D3D12 interop device creation failed");
				return false;
			}
			vram_budget::NoteDelta("XeFG D3D12 interop device", vramDevice);
			device11 = device;
			device->GetImmediateContext(&ctx11);

			// UI Native presents through this chain with or without frame
			// generation. Started with frame generation off, it uses a plain
			// 3-buffer chain and XeSS-FG is not loaded: at 4K that saves about
			// 1 GB (the 8-buffer proxy chain plus XeFG's internal resources).
			// Turning frame generation on then needs a restart.
			// bXeFGAlwaysLoad=1 restores the previous behaviour.
			const bool alwaysLoad = PluginIniInt("bXeFGAlwaysLoad", 0) == 1;
			fgLoaded = !scaling || GetSettings().enableFrameGeneration || alwaysLoad;
			xefg_unlock::Result unlock{};
			if (fgLoaded) {
				unlock = xefg_unlock::Apply(api.fg, api.xell, xefg_unlock::kMaxInterpolations);
				maxInterpolations = unlock.maxInterpolations;
				unlockApplied = unlock.unlocked;
				unlockFailure = unlock.failure;
				if (unlock.unlocked && unlock.pacingSitesOk) xefg_pacing::Install(api.fg);
			} else {
				unlockApplied = true;
			}

			if (!Ok(api.xellCreate(interop.Device12(), &xell), "xellD3D12CreateContext") || !xell) {
				F4SE::log::error("f4sr: XeFG: xellD3D12CreateContext failed (XeSS-FG requires XeLL)");
				xell = nullptr;
				return false;
			}
			api.xellSetLogging(xell, XELL_LOGGING_LEVEL_WARNING, &XellLog);
			xell_sleep_params_t sleep{};
			sleep.bLowLatencyMode = 1;
			if (!Ok(api.xellSetSleepMode(xell, &sleep), "xellSetSleepMode")) F4SE::log::warn("f4sr: XeFG: xellSetSleepMode failed");

			ComPtr<IDXGIFactory2> factory;
			if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
				F4SE::log::error("f4sr: XeFG: CreateDXGIFactory2 failed");
				return false;
			}
			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = outWidth;
			desc.Height = outHeight;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = fgLoaded ? kProxyBuffers : kPlainBuffers;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
			const auto vramBefore = vram_budget::LocalUsage();
			xefg_swapchain_properties_t props{};
			auto propsResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
			if (fgLoaded) {
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
				propsResult = api.properties(fg, &props);
				vram_budget::NoteDelta("XeFG proxy swap chain + context", vramBefore);
				{
					const std::uint64_t chainBytes = static_cast<std::uint64_t>(outWidth) * outHeight * vram_budget::BytesPerPixel(format) * kProxyBuffers;
					vram_budget::Track(proxy.Get(), vram_budget::Kind::Chain, chainBytes);
					if (vramBefore)
						fgSdkBytes.Set(static_cast<std::int64_t>(vram_budget::LocalUsage()) - static_cast<std::int64_t>(vramBefore) - static_cast<std::int64_t>(chainBytes));
				}
				vram_budget::Note("XeFG proxy swap chain buffers", outWidth, outHeight, format, kProxyBuffers);
				if (propsResult == XEFG_SWAPCHAIN_RESULT_SUCCESS)
					vram_budget::NoteBytes("XeFG temporary heaps (buffers + textures, SDK-reported)", props.tempBufferHeapSize + props.tempTextureHeapSize);
			} else {
				// Same description as the XeFG chain, fewer buffers: no
				// generated frames queue behind the real one.
				ComPtr<IDXGISwapChain1> plain;
				const HRESULT hr = factory->CreateSwapChainForHwnd(interop.Queue12(), real, &desc, nullptr, nullptr, &plain);
				if (FAILED(hr) || FAILED(plain.As(&proxy))) {
					F4SE::log::error("f4sr: UI Native: output swap chain creation failed (hr 0x{:08X})", static_cast<std::uint32_t>(hr));
					return false;
				}
				vram_budget::NoteDelta("UI Native output swap chain", vramBefore);
				vram_budget::Track(proxy.Get(), vram_budget::Kind::Chain,
					static_cast<std::uint64_t>(outWidth) * outHeight * vram_budget::BytesPerPixel(format) * kPlainBuffers);
				vram_budget::Note("UI Native output swap chain buffers", outWidth, outHeight, format, kPlainBuffers);
			}
			// The HUD-less texture is created on the first generated frame
			// (EnsureHudless): with frame generation off it is never read.
			if (!CreateTexture(back, outWidth, outHeight, format, scaling)) {
				F4SE::log::error("f4sr: XeFG: shared back buffer texture creation failed");
				return false;
			}
			vram_budget::Note("XeFG shared back buffer", outWidth, outHeight, format);
			if (fg) {
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
			g_gameWidth = width;
			g_gameHeight = height;
			if (VirtualProtect(&vtable[13], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				g_origResizeBuffers = reinterpret_cast<ResizeBuffers_t>(vtable[13]);
				vtable[13] = reinterpret_cast<void*>(&ResizeBuffersThunk);
				VirtualProtect(&vtable[13], sizeof(void*), oldProtect, &ignored);
			}
			g_gameChainForResize = chain;
			if (!g_origSetFullscreenState && VirtualProtect(&vtable[10], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				g_origSetFullscreenState = reinterpret_cast<SetFullscreenState_t>(vtable[10]);
				vtable[10] = reinterpret_cast<void*>(&SetFullscreenStateThunk);
				VirtualProtect(&vtable[10], sizeof(void*), oldProtect, &ignored);
			}
			gameChain = chain;
			// Frames are taken directly below ENB and that Present is not
			// forwarded (a second window never gets its buffers back under
			// gamescope), so layers below ENB never run under XeSS-FG.
			const auto layers = swapchain::Stack(chain, hidden);
			F4SE::log::info("f4sr: XeFG: swap chain layers: {}", swapchain::Describe(layers));
			const int below = swapchain::BelowEnb(layers);
			if (below > 0) {
				auto* inner = layers[below].chain;
				void** innerTable = *reinterpret_cast<void***>(inner);
				if (VirtualProtect(&innerTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
					g_innerOrigPresent = reinterpret_cast<Present_t>(innerTable[8]);
					innerTable[8] = reinterpret_cast<void*>(&InnerPresentThunk);
					VirtualProtect(&innerTable[8], sizeof(void*), oldProtect, &ignored);
					g_innerChain = inner;
					enbResizeSupported = below == 1 && layers.front().Enb() && g_origResizeBuffers;
					F4SE::log::info("f4sr: XeFG: frames are taken from {} {:p}, after ENB's Present", layers[below].Describe(), static_cast<void*>(inner));
					if (HMODULE enb = swapchain::ModuleOf(*reinterpret_cast<void* const*>(layers[below - 1].chain)))
						RedirectWrapperFocus(enb, real, hidden, scaling ? width : 0, scaling ? height : 0);
				}
			}
			// A wrapper that also hooks the DXGI factory the XeFG output chain
			// was created from runs there, on every frame the proxy presents,
			// generated ones included. ReShade also patches system32 dxgi.dll's
			// exports, so the factory's module alone does not prove that; the
			// effect runtime ReShade reports for the output chain does.
			const auto factoryModule = swapchain::ModulePath(*reinterpret_cast<void* const*>(factory.Get()));
			F4SE::log::info("f4sr: XeFG: output chain factory object from {}", factoryModule);
			auto reportBypassed = [&](const swapchain::Layer& layer) {
				const bool sameModule = !factoryModule.empty() && _stricmp(factoryModule.c_str(), layer.module.c_str()) == 0;
				const bool reshade = reshade_preui::IsReShadeModule(swapchain::ModuleOf(*reinterpret_cast<void* const*>(layer.chain)));
				if (sameModule || (reshade && reshade_preui::OutputRuntimeSeen()))
					F4SE::log::info("f4sr: XeFG: {} is skipped on the game chain but also wraps the XeFG output chain; its effects apply to every presented frame",
						layer.Describe());
				else if (reshade)
					F4SE::log::warn("f4sr: XeFG: ReShade {} has no effect runtime on the XeFG output chain; its effects are not shown under XeSS-FG",
						layer.Describe());
				else
					F4SE::log::warn("f4sr: XeFG: {} is skipped on the game chain; unless it also hooks the system DXGI factory, its effects and overlay "
									"are not shown under XeSS-FG (FSR-FG keeps it)",
						layer.Describe());
			};
			for (std::size_t i = (below > 0 ? below : 1); i + 1 < layers.size(); ++i) reportBypassed(layers[i]);
			if (below < 0 && layers.size() > 1 && !layers[0].Enb()) reportBypassed(layers[0]);
			const bool anyEnb = std::any_of(layers.begin(), layers.end(), [](const swapchain::Layer& l) { return l.Enb(); });
			if (!g_innerChain && anyEnb)
				F4SE::log::warn("f4sr: XeFG: ENB found but no chain below it; frames are taken before ENB's Present (effects applied there are missing)");
			if (!anyEnb && g_origResizeBuffers) enbResizeSupported = true;  // no ENB: the engine resizes the game chain directly
			// Only with XeFG loaded: the plain chain has no pacing to wait out.
			asyncWanted = GetSettings().asyncPresent && fgLoaded;
			asyncQueue = std::clamp(PluginIniInt("iXeFGAsyncQueue", 1), 1, 2);
			if (GetSettings().asyncPresent)
				F4SE::log::info("f4sr: XeFG async present {} (queue {})", asyncWanted ? "requested" : "off: XeSS-FG not loaded", asyncQueue);
			attached = true;
			StartWatchdog();
			if (scaling)
				F4SE::log::info("f4sr: ENB before upscaling: game chain {}x{}, proxy chain {}x{} (pre-UI upscale; bilinear for menus and fallback)",
					width, height, outWidth, outHeight);
			if (fgLoaded)
				F4SE::log::info("f4sr: XeFG attached: proxy chain {}x{} fmt={} buffers={} FLIP_DISCARD on game window {:p} (game chain on hidden window {:p}); "
								"max generated frames {} ({}X, unlock {}), provider reports max {} (result {}); XeLL low-latency on",
					outWidth, outHeight, static_cast<unsigned>(format), kProxyBuffers, static_cast<void*>(real), static_cast<void*>(hidden),
					maxInterpolations, maxInterpolations + 1, unlock.unlocked ? "applied" : ("not applied: " + unlock.failure),
					props.maxSupportedInterpolations, static_cast<int>(propsResult));
			else
				F4SE::log::info("f4sr: UI Native output: plain swap chain {}x{} fmt={} buffers={} FLIP_DISCARD on game window {:p} (game chain on hidden window {:p}); "
								"XeSS-FG not loaded (frame generation off at start; turning it on needs a restart, bXeFGAlwaysLoad=1 loads it anyway); XeLL on",
					outWidth, outHeight, static_cast<unsigned>(format), kPlainBuffers, static_cast<void*>(real), static_cast<void*>(hidden));
			return true;
		}

		void ApplyInterpolations(int setting)
		{
			const int want = std::clamp(setting, 1, maxInterpolations);
			if (!fg || want == appliedInterpolations) return;
			const auto r = api.setInterpolations(fg, static_cast<std::uint32_t>(want));
			F4SE::log::info("f4sr: XeFG generated frames per real frame -> {} ({}X) result {}{}", want, want + 1, static_cast<int>(r),
				setting > maxInterpolations ? " (setting clamped: unlock not applied)" : "");
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
			if (!fg) return "XeSS-FG not loaded (started with frame generation off): restart";
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

		// The plain chain (XeSS-FG not loaded) presents exactly the real frame.
		bool PresentStatus(xefg_swapchain_present_status_t& status)
		{
			if (!fg) {
				status = {};
				status.framesPresented = 1;
				return true;
			}
			return api.lastPresentStatus(fg, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
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
			if (DiagNonDefault() || asyncWanted)
				F4SE::log::info("f4sr: diagnostics 5s: {}; low-latency mode {}; game chain Presents ok {} busy {} failed {} (last hr 0x{:08X})", DiagSummary(),
					appliedLowLatency ? "on" : "off", statGamePresentOk, statGamePresentBusy, statGamePresentFailed,
					static_cast<std::uint32_t>(lastGamePresentHr));
			statGamePresentOk = statGamePresentBusy = statGamePresentFailed = 0;
			if (latencyApplied == Latency::AntiLag2 && statAl2Calls)
				F4SE::log::info("f4sr: low latency 5s: Anti-Lag 2 Update {} calls, delay avg {:.2f} ms max {:.2f} ms", statAl2Calls,
					statAl2Ms / static_cast<double>(statAl2Calls), statAl2MaxMs);
			else if (latencyApplied != Latency::XeLL)
				F4SE::log::info("f4sr: low latency 5s: {}", LatencyName(latencyApplied));
			statAl2Calls = 0;
			statAl2Ms = statAl2MaxMs = 0.0;
			if (statBlockCount)
				F4SE::log::info("f4sr: proxy Present on the game thread: avg {:.2f} ms over {} frames (pacing sees {:.2f} ms)", statBlockMs / static_cast<double>(statBlockCount),
					statBlockCount, xefg_pacing::g.appBlockNs.load(std::memory_order_relaxed) / 1e6);
			statBlockMs = 0.0;
			statBlockCount = 0;
			if (const auto frames = asyncWaitFrames.exchange(0, std::memory_order_relaxed))
				F4SE::log::info("f4sr: XeFG async present: game thread waited for a hand-off slot avg {:.2f} ms max {:.2f} ms over {} frames (worker Present above)",
					asyncWaitNs.exchange(0, std::memory_order_relaxed) / 1e6 / static_cast<double>(frames), asyncWaitMaxNs.exchange(0, std::memory_order_relaxed) / 1e6,
					frames);
			LogLatency();
			statsTick = now;
			statReal = statFgOn = statPresented = statStatusFailed = statGenErrors = statGenWarnings = 0;
			std::fill(std::begin(statHistogram), std::end(statHistogram), 0ull);
		}

		// Drawn into the game back buffer before it is copied to the proxy, so the
		// panel is part of the real frame (it is not in the HUD-less copy, so
		// XeSS-FG treats it like UI).
		std::uint64_t overlaySr{};
		void DrawOverlay(IDXGISwapChain* chain, const char* block, ID3D11Texture2D* target = nullptr)
		{
			overlayOff = block ? block : "-";
			auto builder = [this](double seconds) {
				auto rate = [&](std::uint64_t n, std::uint64_t o) {
					return seconds > 0 && n >= o ? static_cast<double>(n - o) / seconds : 0.0;
				};
				const double real = rate(totalReal, overlayReal);
				const double presented = rate(totalPresented, overlayPresented);
				overlayReal = totalReal;
				overlayPresented = totalPresented;
				const int setting = GetSettings().xefgInterpolations;
				std::string fg, fps;
				if (!fgLoaded) {
					fg = "FrameGen\toff (XeSS-FG not loaded)\n";
				} else if (appliedInterpolations <= 0) {
					fg = "FrameGen\t!XeSS-FG not applied yet\n";
				} else if (fgEnabled) {
					fg = std::format("FrameGen\tXeSS-FG {}X\n", appliedInterpolations + 1);
					fps = std::format("{:.0f} real -> {:.0f} shown", real, presented);
				} else {
					fg = std::format("FrameGen\tXeSS-FG {}X  paused: {}\n", appliedInterpolations + 1, overlayOff);
				}
				if (fps.empty() && real >= 1.0) fps = std::format("{:.0f}", real);
				if (fgLoaded && setting != appliedInterpolations && appliedInterpolations > 0)
					fg += std::format("\t!set to {}X, this GPU allows {}X\n", setting + 1, maxInterpolations + 1);
				if (fgLoaded && !unlockApplied) fg += "\t!multi-frame unlock failed: " + unlockFailure + "\n";
				if (DiagNonDefault()) fg += "Diag\t" + DiagPanel() + "\n";
				if (LatencyWanted() == Latency::AntiLag2 && latencyApplied != Latency::AntiLag2 && al2Tried)
					fg += std::format("Latency\t!Anti-Lag 2 unavailable (0x{:08X}), XeLL\n", static_cast<std::uint32_t>(al2Hr));
				else
					fg += std::format("Latency\t{}{}\n", LatencyName(latencyApplied),
						!asyncWanted ? "" : asyncActive ? ", async present" : ", async present paused (ReShade depth)");
				if (lastGenResult != 0) fg += std::format("\t!last generation error {}\n", lastGenResult);
				const std::string text = sr_status::Panel(fps, fg, seconds, overlaySr);
				return std::array<std::string, 2>{ text, text };
			};
			if (target) overlay.DrawTo(target, 0, builder);
			else overlay.Draw(chain, 0, builder);
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

		// Path B: the game sizes its window from iSize and may restore that size
		// after the swap chain exists. The proxy chain needs the display size.
		void KeepWindowSize()
		{
			if (!scaling || (frameId % 30) != 1) return;
			RECT rc{};
			if (!GetClientRect(realWindow, &rc)) return;
			const UINT cw = static_cast<UINT>(rc.right - rc.left), ch = static_cast<UINT>(rc.bottom - rc.top);
			if (cw == outWidth && ch == outHeight) return;
			MONITORINFO mi{ sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(realWindow, MONITOR_DEFAULTTOPRIMARY), &mi);
			SetWindowPos(realWindow, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, static_cast<int>(outWidth), static_cast<int>(outHeight),
				SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
			static std::uint32_t logged = 0;
			if (logged++ < 10)
				F4SE::log::info("f4sr: ENB before upscaling: game window client {}x{} at frame {}; restored to {}x{}", cw, ch, frameId, outWidth, outHeight);
		}

		void BeforeWrapperPresent()
		{
			if (!scaling || !composed) return;
			// Flush any mixed UI writes into the canvas first, then let ENB
			// run with the game's targets and render-size viewport restored.
			ui_redirect::End();
			D3D11_TEXTURE2D_DESC d{};
			if (SUCCEEDED(g_innerChain->GetBuffer(0, IID_PPV_ARGS(&enbBefore))) && enbBefore)
				enbBefore->GetDesc(&d);
			if (d.Width != width || d.Height != height || d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1 ||
				!enbDelta.Snapshot(ctx11.Get(), enbBefore.Get())) {
				enbBefore.Reset();
				if (static bool warned = false; !warned) {
					warned = true;
					F4SE::log::warn("f4sr: ENB before upscaling: could not snapshot the render-size buffer before ENB Present");
				}
			}
		}

		bool CompositeWrapperPresent(ID3D11Texture2D* source)
		{
			if (!enbBefore) return false;
			ui_redirect::ScopedUntracked untracked;
			if (source == enbBefore.Get() && enbDelta.CompositeDelta(ctx11.Get(), source, composed.Get(), back.tex11.Get())) {
				if (static bool logged = false; !logged) {
					logged = true;
					F4SE::log::info("f4sr: ENB before upscaling: Present changes composited over the display-size canvas");
				}
				return true;
			}
			if (static bool warned = false; !warned) {
				warned = true;
				F4SE::log::warn("f4sr: ENB before upscaling: could not composite Present changes; keeping the display-size canvas");
			}
			return false;
		}

		HRESULT OnPresent(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
		{
			if ((flags & DXGI_PRESENT_TEST) != 0) return g_origPresent(chain, syncInterval, flags);
			if ((frameId % 120) == 1) ReadDiagnostics();
			ui_redirect::End();
			KeepWindowSize();
			KeepEngineWindowSize("present");
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
			AsyncSelect();

			bool presented = false;
			ComPtr<ID3D11Texture2D> source;
			D3D11_TEXTURE2D_DESC sd{};
			if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&source))) && source) source->GetDesc(&sd);
			if (source && sd.Width == width && sd.Height == height && interop.Healthy()) {
				SetStep(Step::Copy, id);
				const bool fromCanvas = scaling && composed;
				const bool panel = GetSettings().frameGenOverlay || sr_status::PanelForced();
				if (panel && !fromCanvas) DrawOverlay(chain, block);
				if (fromCanvas) {
					if (!CompositeWrapperPresent(source.Get())) ctx11->CopyResource(back.tex11.Get(), composed.Get());
					if (panel) DrawOverlay(chain, block, back.tex11.Get());
				} else if (scaling) scaleBack.Scale(ctx11.Get(), source.Get(), back.tex11.Get());
				else ctx11->CopyResource(back.tex11.Get(), source.Get());
				if (GetSettings().diagnostics && !block && hudlessReady && ++centreDumpFrames >= 600 && centreDumpFrames < 604) {
					DumpCentre(hudless.tex11.Get(), id, "hudless");
					DumpCentre(back.tex11.Get(), id, "final");
				}
				const bool signalled = !asyncActive && interop.SignalFromD3D11(ctx11.Get()) && interop.WaitOnD3D12();
				if (asyncActive) presented = AsyncSubmit(id, block);
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
						Tag(list, id, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hudless.res12.Get(), outWidth, outHeight);
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
			// Under async present the worker owns fgEnabled, the interpolation
			// count, the proxy Present and its XeLL present markers.
			if (fg && !asyncActive && fgFrame != fgEnabled) {
				api.setEnabled(fg, fgFrame ? 1u : 0u);
				fgEnabled = fgFrame;
			}
			if (fgFrame) {
				if (!asyncActive) ApplyInterpolations(GetSettings().xefgInterpolations);
				resetNext = false;
			} else {
				resetNext = true;
				lastOff = block;
			}
			if (presented && !asyncActive) {
				if (fg) api.setPresentId(fg, id);
				Marker(id, XELL_PRESENT_START);
				xefg_pacing::NoteAppSubmit(id);
				SetStep(Step::ProxyPresent, id);
				const bool al2Fg = fgFrame && latencyApplied == Latency::AntiLag2;
				if (al2Fg) AMD::AntiLag2DX12::MarkEndOfFrameRendering(&g_al2);
				g_al2FrameTypes.store(al2Fg, std::memory_order_relaxed);
				const auto presentStart = std::chrono::steady_clock::now();
				const HRESULT hr12 = proxy->Present(1, 0);
				const auto blockNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - presentStart).count();
				if (fgFrame) xefg_pacing::NoteAppBlock(blockNs);
				statBlockMs += blockNs / 1e6;
				++statBlockCount;
				Marker(id, XELL_PRESENT_END);
				if (FAILED(hr12)) F4SE::log::warn("f4sr: XeFG proxy Present hr=0x{:08X}", static_cast<std::uint32_t>(hr12));
				xefg_swapchain_present_status_t status{};
				SetStep(Step::PresentStatus, id);
				Stats(PresentStatus(status) ? &status : nullptr, fgFrame);  // warnings mean no status
				// The next frame's D3D11 writes into the shared copies wait for this frame's D3D12 reads.
				SetStep(Step::FenceOrder, id);
				if (!interop.SignalFromD3D12() || (diagFenceWait && !interop.WaitOnD3D11(ctx11.Get())))
					F4SE::log::error("f4sr: XeFG fence ordering failed");
			} else if (!presented) {
				Marker(id, XELL_PRESENT_START);
				Marker(id, XELL_PRESENT_END);
				static std::uint64_t warned = 0;
				if ((warned++ % 600) == 0)
					F4SE::log::warn("f4sr: XeFG frame {} not presented on the proxy chain (back buffer {}x{} vs game chain {}x{}, interop {})", id, sd.Width, sd.Height,
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
			if (diagGamePresent) {
				// Diagnostic: give the D3D11 driver its Present on the hidden game
				// chain, below any wrapper, without ever blocking on it.
				const Present_t present = (g_innerChain && chain == g_innerChain && g_innerOrigPresent) ? g_innerOrigPresent : g_origPresent;
				const HRESULT gp = present(chain, 0, DXGI_PRESENT_DO_NOT_WAIT);
				lastGamePresentHr = gp;
				if (gp == DXGI_ERROR_WAS_STILL_DRAWING) ++statGamePresentBusy;
				else if (FAILED(gp)) ++statGamePresentFailed;
				else ++statGamePresentOk;
			}

			frameId = id + 1;
			SetStep(Step::XellSleep, id);
			// iXeLLSleep other than 1 is a diagnostic override of the setting.
			const bool diagSleep = ApplySleepMode(fgFrame);
			const bool diagOverride = diagXellSleep != 1;
			const Latency latency = xell ? ApplyLatency() : Latency::Off;
			if (diagOverride ? diagSleep : latency == Latency::XeLL) {
				g_xellResync.store(xellResyncNext, std::memory_order_relaxed);
				api.xellSleep(xell, frameId);
				g_xellResync.store(false, std::memory_order_relaxed);
				xellResyncNext = false;
			}
			if (!diagOverride && latency == Latency::AntiLag2) {
				const auto t0 = std::chrono::steady_clock::now();
				AMD::AntiLag2DX12::Update(&g_al2, true, 0);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				++statAl2Calls;
				statAl2Ms += ms;
				statAl2MaxMs = (std::max)(statAl2MaxMs, ms);
			}
			SetStep(Step::Idle, id);
			const auto done = g_presentsDone.fetch_add(1) + 1;
			g_lastPresentDone.store(GetTickCount64(), std::memory_order_relaxed);
			if (done <= 5) F4SE::log::info("f4sr: XeFG present {} completed (frame {})", done, id);
			Marker(frameId, XELL_SIMULATION_START);
			simStarted = true;
			if (scaling) Pipeline().AfterPresentPathB();
			return hr;
		}
	};

	Presenter g_presenter;

	using NativeRendererFn = void (*)(RE::BSGraphics::Renderer*, std::uint32_t);
	NativeRendererFn g_nativeBegin{}, g_nativeResize{};
	std::atomic<DWORD> g_presentThread{};
	thread_local std::uint32_t tl_resizeFrame{};
	thread_local std::uint64_t tl_resizeTick{};

	struct QualityRebuild
	{
		int previousQuality;
		RenderSize previousSize;
		std::uint64_t gameplayMs{};
		std::uint64_t lastTick{};
	};
	std::optional<int> g_requestedQuality;
	std::optional<QualityRebuild> g_qualityRebuild;
	bool g_resizeBlocked{};
	// All transaction state is render-thread-only, except the status text.
	std::atomic<const char*> g_qualityNote{};
	constexpr auto kWaitForGameplay = "\tRender Resolution changes when the menu closes\n";
	constexpr auto kRebuilding = "\tApplying the new Render Resolution\n";
	constexpr auto kRestored = "\t!Render Resolution change failed; previous kept\n";
	// Shown in the panel's restart section (sr_status::RestartRows).
	constexpr auto kRestartBlocked = "\t^Render Resolution change\n"
									 "\t(another mod blocks live resizing, e.g.\n"
									 "\tHighFPSPhysicsFix ResizeBuffersDisable=true)\n";
	constexpr auto kRestartRequired = "\t^Render Resolution change\n";

	RenderSize InnerBufferSize()
	{
		// Below ENB when there is one; otherwise the game chain itself.
		auto* chain = g_innerChain ? g_innerChain : g_presenter.gameChain;
		ComPtr<ID3D11Texture2D> buffer;
		if (!chain || FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer) return {};
		D3D11_TEXTURE2D_DESC desc{};
		buffer->GetDesc(&desc);
		return { desc.Width, desc.Height };  // release the reference before any resize
	}

	bool PathBTargetsMatch(const RE::BSGraphics::RendererData& data, RenderSize size)
	{
		return InnerBufferSize() == size && logical::SceneTargetsMatch(data, logical::ReadSceneSlots(), size.width, size.height);
	}

	void ResizeMenuCursor(RenderSize size)
	{
		auto* cursor = RE::MenuCursor::GetSingleton();
		if (!cursor) return;
		const auto left = cursor->minCursorX, top = cursor->minCursorY;
		const auto right = cursor->maxCursorX, bottom = cursor->maxCursorY;
		if (right <= left || bottom <= top) return;  // menu cursor not initialized yet
		const auto remap = [](int pos, int low, int high, UINT extent) {
			const double fraction = static_cast<double>(std::clamp(pos, low, high) - low) / (high - low);
			return static_cast<std::int32_t>(std::lround(fraction * extent));
		};
		// The engine initializes these bounds to [0, screenWidth/Height] once.
		// Per-menu constraints live in separate percentage fields; leave them
		// intact. Do not call the centering setter or warp the OS pointer.
		cursor->cursorPosX = remap(cursor->cursorPosX, left, right, size.width);
		cursor->cursorPosY = remap(cursor->cursorPosY, top, bottom, size.height);
		cursor->minCursorX = cursor->minCursorY = 0;
		cursor->maxCursorX = static_cast<std::int32_t>(size.width);
		cursor->maxCursorY = static_cast<std::int32_t>(size.height);
		F4SE::log::info("f4sr: Path B: menu cursor bounds ({},{})-({},{}) -> (0,0)-({},{})",
			left, top, right, bottom, size.width, size.height);
	}

	void PublishRenderSize(RE::BSGraphics::RendererWindow& window, RenderSize size)
	{
		auto& p = g_presenter;
		const bool changed = RenderSize{ p.width, p.height } != size;
		p.width = size.width;
		p.height = size.height;
		window.windowWidth = static_cast<int>(size.width);
		window.windowHeight = static_cast<int>(size.height);
		if (changed) ResizeMenuCursor(size);
		g_latchedRenderSize.store((static_cast<std::uint64_t>(size.width) << 32) | size.height, std::memory_order_release);
		if (auto* sw = RE::GetINISetting("iSize W:Display")) sw->SetInt(window.windowWidth);
		if (auto* sh = RE::GetINISetting("iSize H:Display")) sh->SetInt(window.windowHeight);
	}

	unsigned g_lastResizeCalls{};  // ResizeBuffers calls that reached the game chain in the last rebuild

	bool RebuildEngine(RE::BSGraphics::Renderer* renderer, RenderSize size)
	{
		auto& window = renderer->data.renderWindow[0];
		PathBResizeAttempt attempt{ g_presenter.gameChain, g_virtualWindow, size.width, size.height };
		{
			struct ClearAttempt { ~ClearAttempt() { tl_resizeAttempt = nullptr; } } clear;
			tl_resizeAttempt = &attempt;
			// Under the renderer lock, before Begin. The original pending-flag
			// route also changes the real display mode, which is not wanted here.
			g_nativeResize(renderer, 0);
		}
		g_lastResizeCalls = attempt.calls;
		const bool ok = attempt.calls >= 1 && SUCCEEDED(attempt.result) && PathBTargetsMatch(renderer->data, size) &&
			window.windowWidth == static_cast<int>(size.width) && window.windowHeight == static_cast<int>(size.height);
		if (!ok) {
			const auto actual = InnerBufferSize();
			F4SE::log::error("f4sr: Path B: resize {}x{} verification failed (calls={}, hr=0x{:08X}, actual {}x{})",
				size.width, size.height, attempt.calls, static_cast<std::uint32_t>(attempt.result), actual.width, actual.height);
		}
		return ok;
	}

	void RefuseQualityChange(const char* reason, const char* note = kRestartRequired)
	{
		g_resizeBlocked = true;
		g_qualityRebuild.reset();
		g_requestedQuality.reset();
		g_qualityNote.store(note, std::memory_order_relaxed);
		sr_status::Poke();
		F4SE::log::error("f4sr: Path B: {}; restart required", reason);
	}

	void KeepActualSizeWithoutSr(RE::BSGraphics::RendererWindow& window)
	{
		// Never label unverified targets as the requested preset. If the swap
		// chain survived, allow an ordinary bilinear frame until restart.
		const auto actual = InnerBufferSize();
		if (actual.width && actual.height) PublishRenderSize(window, actual);
		Pipeline().BlockPathBSr();
	}

	bool CanResizePathB(const RE::BSGraphics::RendererData& data)
	{
		const auto& p = g_presenter;
		const auto& w = data.renderWindow[0];
		return p.enbResizeSupported && !data.fullScreen && !data.appFullScreen && g_origGetWindowRect &&
			g_virtualWindow && g_virtualWindow == p.realWindow && reinterpret_cast<HWND>(w.hwnd) == g_virtualWindow &&
			reinterpret_cast<IDXGISwapChain*>(w.swapChain) == p.gameChain &&
			(*reinterpret_cast<void***>(p.gameChain))[13] == reinterpret_cast<void*>(&ResizeBuffersThunk);
	}

	void ApplyPathBQuality(RE::BSGraphics::Renderer* renderer, std::uint32_t windowIndex, std::uint64_t tick)
	{
		auto& p = g_presenter;
		auto& data = renderer->data;
		auto& window = data.renderWindow[0];
		if (g_resizeBlocked || windowIndex != 0 || data.requestWindowSizeChange || p.enbBefore || p.composed ||
			ui_redirect::Active() || !detail::CaptureGameplay()) return;
		bool rollback = false;
		if (g_qualityRebuild) {
			if (!GetSettings().enableUpscaling) xefg::ConfirmPathBQuality();
			else {
				auto& rebuild = *g_qualityRebuild;
				// Count only safe gameplay time, not load screens or stalls.
				if (rebuild.lastTick) rebuild.gameplayMs += (std::min)(tick - rebuild.lastTick, std::uint64_t{ 100 });
				rebuild.lastTick = tick;
				rollback = rebuild.gameplayMs >= 12'000;
				if (!rollback) return;
				F4SE::log::error("f4sr: Path B: super resolution did not resume; restoring the previous preset");
			}
		}
		if (!rollback && !g_requestedQuality) return;
		if (!CanResizePathB(data)) {
			RefuseQualityChange("the current window or ENB swap chain does not support hot resize");
			return;
		}
		const int oldQuality = GetSettings().qualityMode;
		const int quality = rollback ? g_qualityRebuild->previousQuality : *g_requestedQuality;
		const RenderSize oldSize{ p.width, p.height };
		const float ratio = RenderRatio(quality);
		const auto scaled = [=](UINT n) { return ratio == 1.0f ? n : static_cast<UINT>(std::lround(n * ratio)) & ~1u; };
		const RenderSize target = rollback ? g_qualityRebuild->previousSize : RenderSize{ scaled(p.outWidth), scaled(p.outHeight) };
		if (!rollback) g_requestedQuality.reset();
		if (!target.width || !target.height || !PathBTargetsMatch(data, oldSize)) {
			RefuseQualityChange("current engine targets do not match the swap chain");
			return;
		}
		p.AsyncIdle();  // the present worker must not touch the proxy or the shared textures during the resize
		if (p.fgEnabled) {
			if (!Ok(p.api.setEnabled(p.fg, 0), "disable XeFG before Path B resize")) {
				RefuseQualityChange("could not pause frame generation");
				return;
			}
			p.fgEnabled = false;
		}
		if (!p.interop.Drain() || !Pipeline().PreparePathBResize()) {
			RefuseQualityChange("could not retire old GPU work");
			return;
		}
		ui_redirect::ResetForResize();
		p.scaleBack = {};
		p.scaleHudless = {};
		p.enbDelta = {};
		p.hudlessReady = p.inputsReady = false;
		p.resetNext = true;
		Pipeline().RebindPathBSceneSlots();
		F4SE::log::info("f4sr: Path B: {} preset {} -> {}, game/ENB {}x{} -> {}x{} (output {}x{})",
			rollback ? "restoring" : "applying", oldQuality, quality, oldSize.width, oldSize.height,
			target.width, target.height, p.outWidth, p.outHeight);
		if (oldSize != target && !RebuildEngine(renderer, target)) {
			const bool blocked = g_lastResizeCalls == 0;
			if (!rollback && (PathBTargetsMatch(data, oldSize) || RebuildEngine(renderer, oldSize))) {
				PublishRenderSize(window, oldSize);
				g_qualityNote.store(kRestored, std::memory_order_relaxed);
				sr_status::Poke();
				F4SE::log::warn("f4sr: Path B: previous resolution {}x{} restored after failed resize", oldSize.width, oldSize.height);
				// The engine's ResizeBuffers never reached the swap chain:
				// another mod swallows it (HighFPSPhysicsFix
				// ResizeBuffersDisable=true). Further attempts cannot work;
				// the MCM choice applies at the next start.
				if (blocked)
					RefuseQualityChange(std::format("ResizeBuffers is blocked by another mod; the new preset applies after a restart. {}",
						hfpf::Read().summary).c_str(), kRestartBlocked);
			} else {
				KeepActualSizeWithoutSr(window);
				RefuseQualityChange("could not restore verified engine targets");
			}
			return;
		}
		PublishRenderSize(window, target);
		GetSettings().qualityMode = quality;
		const auto slots = logical::ReadSceneSlots();
		F4SE::log::info("f4sr: Path B: verified {}x{} game/ENB targets (logical slots color={}, depth={}, mv={})",
			target.width, target.height, slots.color, slots.depth, slots.motion);
		if (rollback) {
			g_qualityRebuild.reset();
			g_qualityNote.store(g_requestedQuality ? kWaitForGameplay : kRestored, std::memory_order_relaxed);
		} else {
			g_qualityRebuild = QualityRebuild{ oldQuality, oldSize };
			g_qualityNote.store(kRebuilding, std::memory_order_relaxed);
			if (!GetSettings().enableUpscaling) xefg::ConfirmPathBQuality();
		}
	}

	void PathBRendererBegin(RE::BSGraphics::Renderer* renderer, std::uint32_t windowIndex)
	{
		const auto now = GetTickCount64();
		const auto tick = std::exchange(tl_resizeTick, 0);
		if (tick && now - tick <= 200 && g_presentThread.load(std::memory_order_acquire) == GetCurrentThreadId() &&
			g_presenter.frameId == tl_resizeFrame && renderer && &renderer->data == RE::BSGraphics::RendererData::GetSingleton())
			ApplyPathBQuality(renderer, windowIndex, now);
		g_nativeBegin(renderer, windowIndex);
	}

	void InstallPathBResize()
	{
		if (REL::Module::get().version() != REL::Version{ 1, 10, 163, 0 }) return;
		const auto site = REL::ID(1075087).address() + 0x3E;
		const auto original = REL::ID(288964).address();
		const auto resize = REL::ID(212827).address();
		const auto calls = [](std::uintptr_t instruction, std::uintptr_t target) {
			std::int32_t rel{};
			std::memcpy(&rel, reinterpret_cast<const void*>(instruction + 1), sizeof(rel));
			return *reinterpret_cast<const std::uint8_t*>(instruction) == 0xE8 && instruction + 5 + rel == target;
		};
		auto& tramp = F4SE::GetTrampoline();
		if (!calls(site, original) || !calls(original + 0x91, resize) || tramp.free_size() < 32) {
			F4SE::log::warn("f4sr: Path B: native Begin/resize call sites not verified; presets require restarting");
			return;
		}
		g_nativeBegin = reinterpret_cast<NativeRendererFn>(original);
		g_nativeResize = reinterpret_cast<NativeRendererFn>(resize);
		tramp.write_call<5>(site, &PathBRendererBegin);
		F4SE::log::info("f4sr: Path B: native preset switching hook installed");
	}

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
	{
		thread_local bool inside = false;
		if (inside || chain != g_presenter.gameChain || !g_presenter.attached || (flags & DXGI_PRESENT_TEST) != 0)
			return g_origPresent(chain, syncInterval, flags);
		struct Guard { bool& v; ~Guard() { v = false; } } guard{ inside };
		inside = true;
		if (!g_innerChain) {
			// No ENB: the frame is taken here, and the end of this Present is
			// the same preset-switch boundary the ENB path marks below.
			tl_resizeTick = 0;
			if (g_nativeBegin && g_presenter.scaling)
				g_presentThread.store(GetCurrentThreadId(), std::memory_order_release);
			const bool boundary = g_nativeBegin && g_presenter.scaling && g_presenter.enbResizeSupported && g_presenter.composed &&
				detail::CaptureGameplay() && GetForegroundWindow() == g_presenter.realWindow;
			const HRESULT hr = g_presenter.OnPresent(chain, syncInterval, flags);
			if (boundary && SUCCEEDED(hr)) {
				tl_resizeFrame = g_presenter.frameId;
				tl_resizeTick = GetTickCount64();
			}
			return hr;
		}
		// Let the wrapper (ENB) run; the frame is taken in InnerPresentThunk.
		g_presenter.BeforeWrapperPresent();
		tl_resizeTick = 0;
		if (g_nativeBegin && g_presenter.scaling)
			g_presentThread.store(GetCurrentThreadId(), std::memory_order_release);
		const bool resizeBoundary = g_nativeBegin && g_presenter.scaling && g_presenter.enbResizeSupported &&
			g_presenter.composed && g_presenter.enbBefore && detail::CaptureGameplay() &&
			GetForegroundWindow() == g_presenter.realWindow;
		g_innerHit = false;
		tl_inGamePresent = true;
		const HRESULT hr = g_origPresent(chain, syncInterval, flags);
		tl_inGamePresent = false;
		g_presenter.enbBefore.Reset();
		if (resizeBoundary && g_innerHit && SUCCEEDED(hr)) {
			tl_resizeFrame = g_presenter.frameId;
			tl_resizeTick = GetTickCount64();
		}
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
		if (EnbBeforeUpscaling() && !g_displayWidth && ShrinkGameSize("swap chain creation") && g_displayWidth) {
			if (auto* sw = RE::GetINISetting("iSize W:Display")) w = static_cast<UINT>(sw->GetInt());
			if (auto* sh = RE::GetINISetting("iSize H:Display")) h = static_cast<UINT>(sh->GetInt());
		}
		// bTestForeignSize=1 in f4sr.ini: act as if another mod had set the
		// swap chain description to the monitor size.
		if (EnbBeforeUpscaling() && g_displayWidth && PluginIniInt("bTestForeignSize", 0) != 0) {
			F4SE::log::warn("f4sr: bTestForeignSize=1: simulating a foreign {}x{} swap chain size", g_displayWidth, g_displayHeight);
			w = g_displayWidth;
			h = g_displayHeight;
		}
		if (EnbBeforeUpscaling() && g_displayWidth) {
			// The INI reads were already shrunk to the render size (Windows):
			// that, not the description, is the size the engine expects.
			const auto* sw = RE::GetINISetting("iSize W:Display");
			const auto* sh = RE::GetINISetting("iSize H:Display");
			const UINT ew = sw ? static_cast<UINT>(sw->GetInt()) : 0, eh = sh ? static_cast<UINT>(sh->GetInt()) : 0;
			if (ew && eh && ew < g_displayWidth && (w != ew || h != eh)) {
				ReportForcedSize("swap chain creation", w, h, ew, eh);
				w = ew;
				h = eh;
			}
		}
		if (!w || !h) {
			RECT rc{};
			GetClientRect(desc->OutputWindow, &rc);
			w = static_cast<UINT>(rc.right - rc.left);
			h = static_cast<UINT>(rc.bottom - rc.top);
		}
		UINT ow = w, oh = h;
		if (EnbBeforeUpscaling()) {
			// The game renders at its own (render) size on the hidden window; the
			// real window and the proxy chain take the display size.
			MONITORINFO mi{ sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(desc->OutputWindow, MONITOR_DEFAULTTOPRIMARY), &mi);
			const auto& s = GetSettings();
			ow = s.outputWidth > 0 ? static_cast<UINT>(s.outputWidth) : g_displayWidth ? g_displayWidth : static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
			oh = s.outputHeight > 0 ? static_cast<UINT>(s.outputHeight) : g_displayHeight ? g_displayHeight : static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
			if (ow < w || oh < h) {
				F4SE::log::warn("f4sr: ENB before upscaling: output {}x{} is smaller than the game size {}x{}; not scaling", ow, oh, w, h);
				ow = w;
				oh = h;
			} else if (ow != w || oh != h) {
				RECT before{};
				GetWindowRect(desc->OutputWindow, &before);
				SetWindowPos(desc->OutputWindow, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, static_cast<int>(ow), static_cast<int>(oh),
					SWP_NOZORDER | SWP_NOACTIVATE);
				RECT after{};
				GetClientRect(desc->OutputWindow, &after);
				F4SE::log::info("f4sr: ENB before upscaling: game window {}x{} -> client {}x{} (output {}x{})",
					before.right - before.left, before.bottom - before.top, after.right, after.bottom, ow, oh);
			}
		}
		HWND hidden = CreateHiddenWindow(w, h);
		if (!hidden) {
			F4SE::log::error("f4sr: XeFG: hidden window creation failed ({}); stock swap chain", GetLastError());
			if (EnbBeforeUpscaling()) FallBackFromPathB("hidden window creation failed", true);
			return g_origCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, desc, chain, device, level, context);
		}
		DXGI_SWAP_CHAIN_DESC redirected = *desc;
		redirected.OutputWindow = hidden;
		redirected.Windowed = TRUE;
		redirected.BufferDesc.Width = w;
		redirected.BufferDesc.Height = h;
		HRESULT hr = g_origCreate(adapter, driverType, software, flags, levels, levelCount, sdkVersion, &redirected, chain, device, level, context);
		// bTestPathBFail=1 in f4sr.ini: simulate an attach failure under Path B
		// to check the fallback and the error panel.
		const bool testFail = EnbBeforeUpscaling() && PluginIniInt("bTestPathBFail", 0) != 0;
		if (testFail) F4SE::log::warn("f4sr: bTestPathBFail=1: simulating a Path B attach failure");
		if (SUCCEEDED(hr) && *chain && EnbBeforeUpscaling()) {
			// A hook below this one may still have changed the size.
			ComPtr<ID3D11Texture2D> buffer;
			D3D11_TEXTURE2D_DESC bd{};
			if (SUCCEEDED((*chain)->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) buffer->GetDesc(&bd);
			buffer.Reset();
			if (bd.Width && (bd.Width != w || bd.Height != h)) {
				const HRESULT rhr = (*chain)->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, redirected.Flags);
				ReportForcedSize("the created swap chain", bd.Width, bd.Height, w, h);
				F4SE::log::warn("f4sr: Path B: swap chain came up {}x{}; ResizeBuffers to {}x{} hr=0x{:08X}", bd.Width, bd.Height, w, h,
					static_cast<std::uint32_t>(rhr));
			}
		}
		if (!testFail && SUCCEEDED(hr) && *chain && *device && g_presenter.Attach(*device, *chain, desc->OutputWindow, hidden, w, h, ow, oh, desc->BufferDesc.Format)) {
			if (g_origGetClientRect && g_displayWidth) {
				g_virtualWindow = desc->OutputWindow;
				F4SE::log::info("f4sr: ENB before upscaling: virtual display {}x{} on window {:p} (real {}x{})", g_gameWidth, g_gameHeight,
					static_cast<void*>(g_virtualWindow), g_displayWidth, g_displayHeight);
			}
			KeepEngineWindowSize("swap chain creation");
			return hr;
		}
		F4SE::log::error("f4sr: XeFG: attach failed (hr=0x{:08X}); recreating the stock swap chain on the game window", static_cast<std::uint32_t>(hr));
		// The stock chain below is created with the game's own (display) size,
		// so the engine continues on the Classic pipeline in this session.
		if (EnbBeforeUpscaling()) FallBackFromPathB("XeSS-FG output chain could not be attached", true);
		else sr_status::Raise(sr_status::kAlertXefg);
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
		// Path B checks what it can before installing anything, so a failure
		// here still leaves this session a working Classic pipeline.
		if (EnbBeforeUpscaling() && !g_presenter.api.Load())
			FallBackFromPathB("the XeSS-FG libraries could not be loaded", true);
		// The swap chain is redirected only when XeSS-FG can actually run, so
		// players who never use frame generation keep the game's own chain.
		// The chain is created once at startup: turning frame generation on
		// later needs a restart before XeSS-FG attaches.
		if (!GetSettings().enableFrameGeneration && !EnbBeforeUpscaling()) {
			F4SE::log::info("f4sr: XeFG: frame generation is off at startup; the swap chain is left as is (turning it on needs a game restart)");
			return;
		}
		const auto original = REL::PatchIAT(&CreateDeviceAndSwapChainHook, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
		g_origCreate = reinterpret_cast<CreateDeviceAndSwapChain_t>(original);
		g_firstCreate = g_origCreate;
		if (!g_origCreate) {
			F4SE::log::error("f4sr: XeFG: d3d11.dll!D3D11CreateDeviceAndSwapChain import not found; XeSS-FG unavailable");
			if (EnbBeforeUpscaling()) FallBackFromPathB("the swap chain creation import was not found", true);
			else sr_status::Raise(sr_status::kAlertXefg);
			return;
		}
		g_redirectInstalled = true;
		F4SE::log::info("f4sr: XeFG: swap chain creation redirected (import of D3D11CreateDeviceAndSwapChain)");
		if (EnbBeforeUpscaling()) {
			g_origGetProfileInt = reinterpret_cast<GetProfileInt_t>(REL::PatchIAT(&GetProfileIntHook, "kernel32.dll", "GetPrivateProfileIntA"));
			g_origGetProfileString = reinterpret_cast<GetProfileString_t>(REL::PatchIAT(&GetProfileStringHook, "kernel32.dll", "GetPrivateProfileStringA"));
			g_origWriteProfileString = reinterpret_cast<WriteProfileString_t>(REL::PatchIAT(&WriteProfileStringHook, "kernel32.dll", "WritePrivateProfileStringA"));
			F4SE::log::info("f4sr: ENB before upscaling: INI hooks int {} string {} write {}", g_origGetProfileInt ? "on" : "missing",
				g_origGetProfileString ? "on" : "missing", g_origWriteProfileString ? "on" : "missing");
			g_origCreateWindowA = reinterpret_cast<CreateWindowExA_t>(REL::PatchIAT(&CreateWindowExAHook, "user32.dll", "CreateWindowExA"));
			g_origSetWindowPos = reinterpret_cast<SetWindowPos_t>(REL::PatchIAT(&SetWindowPosHook, "user32.dll", "SetWindowPos"));
			F4SE::log::info("f4sr: ENB before upscaling: window hooks CreateWindowExA {} SetWindowPos {}", g_origCreateWindowA ? "on" : "missing", g_origSetWindowPos ? "on" : "missing");
			InstallVirtualDisplay();
			scaleform_scale::Install();
			InstallPathBResize();
		}
	}

	bool RedirectInstalled() { return g_redirectInstalled; }

	bool OutputScaling() { return g_presenter.attached && g_presenter.scaling; }
	bool UiNativeSession() { return EnbBeforeUpscaling(); }
	void* ReShadeDepth12(std::uint32_t& generation)
	{
		generation = g_presenter.reshadeDepthGeneration.load();
		return g_presenter.reshadeDepth.res12.Get();
	}
	void ReportPathBFailure(const char* reason) { FallBackFromPathB(reason, false); }

	void RequestPathBQuality(int quality)
	{
		if (quality == GetSettings().qualityMode) {
			g_requestedQuality.reset();
			if (!g_qualityRebuild && !g_resizeBlocked) g_qualityNote.store(nullptr, std::memory_order_relaxed);
			return;
		}
		if (g_resizeBlocked) return;
		if (!g_nativeResize || !g_presenter.enbResizeSupported) {
			RefuseQualityChange("native hot resize or the ENB wrapper is unavailable");
			return;
		}
		if (g_requestedQuality == quality) return;
		g_requestedQuality = quality;
		if (!g_qualityRebuild) g_qualityNote.store(kWaitForGameplay, std::memory_order_relaxed);
		F4SE::log::info("f4sr: Path B: MCM preset {} queued (current {})", quality, GetSettings().qualityMode);
	}

	void ConfirmPathBQuality()
	{
		if (!g_qualityRebuild) return;
		const auto& p = g_presenter;
		F4SE::log::info("f4sr: Path B: MCM preset {} active at {}x{} -> {}x{}{}", GetSettings().qualityMode,
			p.width, p.height, p.outWidth, p.outHeight, GetSettings().enableUpscaling ? " (SR dispatched)" : " (SR disabled)");
		g_qualityRebuild.reset();
		g_qualityNote.store(g_requestedQuality ? kWaitForGameplay : nullptr, std::memory_order_relaxed);
	}

	const char* PathBQualityNote() { return g_qualityNote.load(std::memory_order_relaxed); }
	bool PathBQualityNeedsRestart()
	{
		const char* note = PathBQualityNote();
		return note == kRestartBlocked || note == kRestartRequired;
	}
	bool GenerationNeedsRestart() { return g_presenter.attached ? !g_presenter.fgLoaded : !g_redirectInstalled; }

	UINT OutputWidth() { return g_presenter.outWidth; }

	bool PathBSizes(UINT& renderW, UINT& renderH, UINT& displayW, UINT& displayH)
	{
		if (g_pathBOff.load(std::memory_order_relaxed)) return false;
		const auto size = CurrentRenderSize();
		renderW = size.width;
		renderH = size.height;
		displayW = g_displayWidth;
		displayH = g_displayHeight;
		return renderW && renderH && displayW && displayH;
	}
	UINT OutputHeight() { return g_presenter.outHeight; }
	void SetComposedFrame(ID3D11Texture2D* frame) { g_presenter.composed = frame; }

	void CaptureHudlessFrom(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame)
	{
		auto& p = g_presenter;
		p.hudlessReady = false;
		if (!p.attached || !p.fg || !ctx || !frame || !GetSettings().enableFrameGeneration || !p.EnsureHudless()) return;
		ctx->CopyResource(p.hudless.tex11.Get(), frame);
		p.hudlessReady = true;
	}

	// Plugins loaded after f4sr (e.g. NVIDIA_Reflex) may re-patch the same
	// import and call d3d11.dll directly, skipping our hook. Once every plugin
	// has loaded, take the outermost position again and chain to whatever
	// hook now sits in the import.
	void ReclaimSwapChainRedirect()
	{
		if (GetSettings().frameGenEngine != 1 || !g_firstCreate) return;
		if (EnbBeforeUpscaling()) ShrinkGameSize("after plugin load");
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
		if (!p.attached || !p.fg || !rtManager || !GetSettings().enableFrameGeneration || !p.EnsureHudless()) return;
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
		if (p.scaling) p.scaleHudless.Scale(ctx, source.Get(), p.hudless.tex11.Get());
		else ctx->CopyResource(p.hudless.tex11.Get(), source.Get());
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
			if (!p.CreateTexture(t, d.Width, d.Height, d.Format)) return false;
			vram_budget::Note(&t == &p.depth ? "XeFG depth input (frame generation on)" : "XeFG motion input (frame generation on)", d.Width, d.Height, d.Format);
			return true;
		};
		// Depth and motion copies feed generation only; with frame generation
		// off they are neither created nor copied (ReShade's depth below is
		// separate).
		const bool generate = GetSettings().enableFrameGeneration && p.fg;
		if (generate && (!ensure(p.depth, in.depth) || !ensure(p.motion, in.motionVectors))) {
			F4SE::log::error("f4sr: XeFG depth/motion texture creation failed");
			return;
		}
		const D3D11_BOX box{ 0, 0, 0, in.renderWidth, in.renderHeight, 1 };
		if (generate) ctx->CopySubresourceRegion(p.depth.tex11.Get(), 0, 0, 0, 0, in.depth, 0, &box);
		if (reshade_preui::WantsDepth()) {
			// Exactly the rendered region: Classic's depth is display-size with
			// the frame in its top-left corner, and ReShade samples 0..1 UV.
			D3D11_TEXTURE2D_DESC dd{};
			in.depth->GetDesc(&dd);
			auto& t = p.reshadeDepth;
			if (!t.tex11 || t.desc.Width != in.renderWidth || t.desc.Height != in.renderHeight || t.desc.Format != dd.Format) {
				t = {};
				if (p.CreateTexture(t, in.renderWidth, in.renderHeight, dd.Format)) {
					p.reshadeDepthGeneration.fetch_add(1);
					F4SE::log::info("f4sr: ReShade depth: {}x{} format {} copy for the output runtime", in.renderWidth, in.renderHeight,
						static_cast<unsigned>(dd.Format));
				}
			}
			if (t.tex11) ctx->CopySubresourceRegion(t.tex11.Get(), 0, 0, 0, 0, in.depth, 0, &box);
		}
		if (!generate) return;
		ctx->CopySubresourceRegion(p.motion.tex11.Get(), 0, 0, 0, 0, in.motionVectors, 0, &box);
		p.inputs = in;
		p.inputs.depth = p.inputs.motionVectors = nullptr;
		p.camera = CaptureFrameCamera();
		p.inputsReady = true;
	}
}
