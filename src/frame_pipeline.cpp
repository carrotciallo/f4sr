#include "sr_status.h"
#include "frame_pipeline.h"
#include "vram_budget.h"
#include "frame_pipeline_detail.h"
#include "capture.h"
#include "frame_camera.h"
#include "late_draw_trace.h"
#include "xefg_presenter.h"
#include "post_depth_remap.h"
#include "frame_journal.h"
#include "temporal_witness.h"
#include "dynres_guard.h"
#include "frame_trace.h"

#include <atomic>
#include <fstream>

#include <atomic>

#include <intrin.h>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "settings.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>

namespace
{
	// FO4 render target indices (engine layout, cross-checked against
	// doodlum's FO4 integration and the community-shaders port).
	constexpr std::uint32_t kTargetMain = 4;
	constexpr std::uint32_t kTargetMotionVectors = 29;
	constexpr std::uint32_t kDepthMain = 2;
	// RenderTargetManager dynamic-resolution ratios (engine offsets).
	constexpr std::size_t kDynRatioWOffset = 0xF88;
	constexpr std::size_t kDynRatioHOffset = 0xF8C;
	// Camera near/far globals (OG 1.10.163 address-library ids).
	constexpr std::uint64_t kCameraNearId = 57985;
	constexpr std::uint64_t kCameraFarId = 958877;

	float QualityRatio(int mode)
	{
		switch (mode) {
		case 1: return 1.0f / 1.5f;   // Quality
		case 2: return 1.0f / 1.7f;   // Balanced
		case 3: return 1.0f / 2.0f;   // Performance
		default: return 1.0f;         // Native AA
		}
	}

	// community-shaders' jitter sequence (Halton(2,3), phase count
	// 8 * (display/render)^2), in render pixels, range [-0.5, 0.5).
	float Halton(std::int32_t index, std::int32_t base)
	{
		float fraction = 1.0f;
		float result = 0.0f;
		for (auto i = index; i > 0; i /= base) {
			fraction /= static_cast<float>(base);
			result += fraction * static_cast<float>(i % base);
		}
		return result;
	}
}

namespace f4sr
{
	std::atomic<int> g_dumpStage{ 0 };  // three-point dump sequencing (see DumpPointA/B)

	using detail::CaptureFrameId;
	using detail::CaptureGameplay;
	using detail::DumpRawToFile;

	FramePipeline& Pipeline()
	{
		static FramePipeline pipeline;
		return pipeline;
	}

	bool FramePipeline::CompileDepthShader(ID3D11Device* device)
	{
		// The game's depth lives in a typeless R24G8 target that cannot be
		// CopyResource'd into an R32_FLOAT texture (different format
		// family), so a tiny compute pass converts it through the depth
		// SRV. Same approach as doodlum's CopyDepthToSharedBufferCS.
		static constexpr const char* kSource = R"(
Texture2D<float> DepthTex : register(t0);
RWTexture2D<float> OutDepth : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	OutDepth.GetDimensions(width, height);
	if (id.x < width && id.y < height) OutDepth[id.xy] = DepthTex[id.xy];
}
)";
		Microsoft::WRL::ComPtr<ID3DBlob> blob;
		Microsoft::WRL::ComPtr<ID3DBlob> errors;
		const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), nullptr, nullptr, nullptr,
			"main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
			blob.GetAddressOf(), errors.GetAddressOf());
		if (FAILED(hr)) {
			F4SE::log::error("f4sr: depth conversion shader compile failed");
			return false;
		}
		return SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, depthCs_.GetAddressOf()));
	}

	void FramePipeline::CreateStaging(ID3D11Device* device, ID3D11Texture2D* mainTex, ID3D11Texture2D* mvTex)
	{
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTex->GetDesc(&mainDesc);
		D3D11_TEXTURE2D_DESC mvDesc{};
		mvTex->GetDesc(&mvDesc);

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = mainDesc.Width;
		desc.Height = mainDesc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;

		// UI Native upscales the post-ENB frame (ldrShared_ -> sceneOut_), so
		// the seam's upscale output is never written there; the seam colour
		// copy only feeds the reactive mask, which UI Native XeSS does not use.
		const bool pathB = xefg::OutputScaling();
		const bool seamColor = !(pathB && GetSettings().backend == 2);
		desc.Format = mainDesc.Format;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (seamColor) interop_.CreateSharedTexture(desc, colorShared_);
		if (colorShared_.tex11) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = ReactiveMask::Typed(mainDesc.Format);
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(colorShared_.tex11.Get(), &sd, colorSrv_.ReleaseAndGetAddressOf());
		}
		if (GetSettings().reactiveMask && colorSrv_) reactive_.Create(device, interop_, mainDesc);
		// The upscaler writes the output through a UAV: it needs the
		// unordered-access bind flag or the D3D12 side lacks
		// ALLOW_UNORDERED_ACCESS and FidelityFX rejects every dispatch
		// with ERROR_PARAMETER (2026-11-15 acceptance run).
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (!pathB) interop_.CreateSharedTexture(desc, outputShared_);
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		desc.Format = mvDesc.Format;
		interop_.CreateSharedTexture(desc, mvShared_);

		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		interop_.CreateSharedTexture(desc, depthShared_);
		if (depthShared_.tex11) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(depthShared_.tex11.Get(), &uavDesc, depthUav_.GetAddressOf());
		}
	}

bool FramePipeline::Init(ID3D11Device* device)
	{
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (data == nullptr) {
			return false;
		}
		auto* mainTex = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[slotMain_].texture);
		auto* mvTex = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[slotMV_].texture);
		if (mainTex == nullptr || mvTex == nullptr) {
			return false;
		}
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTex->GetDesc(&mainDesc);
		// Under gamescope the game starts on a small 640x384 surface
		// and resizes to the real display size moments later; kMain
		// follows the surface. Only initialize once kMain matches the
		// swapchain and that size has held for 60 consecutive attempts,
		// at any display size. Later size changes take the rebuild path.
		{
			auto* chain = reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain);
			DXGI_SWAP_CHAIN_DESC chainDesc{};
			if (chain != nullptr && SUCCEEDED(chain->GetDesc(&chainDesc))) {
				static bool gateLogged = false;
				if (mainDesc.Width != sizeCandidateW_ || mainDesc.Height != sizeCandidateH_) {
					sizeCandidateW_ = mainDesc.Width;
					sizeCandidateH_ = mainDesc.Height;
					sizeStableCount_ = 0;
				}
				const bool matches = mainDesc.Width == chainDesc.BufferDesc.Width &&
					mainDesc.Height == chainDesc.BufferDesc.Height;
				if (!matches) sizeStableCount_ = 0;
				if (!matches || ++sizeStableCount_ < 60) {
					if (!gateLogged) {
						gateLogged = true;
						F4SE::log::info("f4sr: waiting for stable targets (kMain {}x{}, display {}x{})",
							mainDesc.Width, mainDesc.Height,
							chainDesc.BufferDesc.Width, chainDesc.BufferDesc.Height);
					}
					return false;
				}
			}
		}
		// Past the waiting gates: a false return from here on is a failure.
		initAttempted_ = true;
		vram_budget::Init(device);
		const auto vramDevice = vram_budget::LocalUsage();
		if (!interop_.Create(device)) {
			return false;
		}
		vram_budget::NoteDelta("pipeline D3D12 interop device", vramDevice);
		CreateStaging(device, mainTex, mvTex);
		const bool pathB = xefg::OutputScaling();
		const bool seamColor = !(pathB && GetSettings().backend == 2);
		if ((seamColor && !colorShared_.res12) || !depthShared_.res12 || !mvShared_.res12 || (!pathB && !outputShared_.res12) || !depthUav_) {
			F4SE::log::error("f4sr: staging texture creation incomplete");
			return false;
		}
		if (!CompileDepthShader(device)) {
			return false;
		}
		useXess_ = GetSettings().backend == 2;
		if (useXess_) {
			// Path B initializes XeSS at the actual pre-UI input/output pair.
			// Path A still upscales the HDR target at the imagespace seam.
			if (!pathB && !xessUpscaler_.Create(interop_.Device12(), mainDesc.Width, mainDesc.Height, GetSettings().qualityMode, reactive_.Ready())) {
				return false;
			}
		} else if (!pathB && !upscaler_.Create(interop_.Device12(), mainDesc.Width, mainDesc.Height,
				mainDesc.Width, mainDesc.Height, GetSettings().backend, GetSettings().debugView)) {
			// UI Native upscales before the UI with upscalerB_ (CreateUpscalerB)
			// and never dispatches the seam context, so it is not created there.
			return false;
		}
		if (auto* chain = reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain); chain != nullptr) {
			frameGen_.InstallPresentHook(chain);
		}
		// FSR-FG only with engine 0; engine 1 presents through the XeSS-FG proxy.
		if (GetSettings().enableFrameGeneration && GetSettings().frameGenEngine == 0) {
			auto* chain = reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain);
			DXGI_SWAP_CHAIN_DESC chainDesc{};
			if (chain != nullptr && SUCCEEDED(chain->GetDesc(&chainDesc))) {
				frameGen_.Create(interop_, interop_.Device12(), chain,
					mainDesc.Width, mainDesc.Height, chainDesc.BufferDesc.Format);
			}
		}
		initialized_ = true;
		builtBackend_ = GetSettings().backend;
		builtQuality_ = GetSettings().qualityMode;
		builtDebugView_ = GetSettings().debugView;
		builtFrameGen_ = GetSettings().enableFrameGeneration;
		latchedW_ = mainDesc.Width;
		latchedH_ = mainDesc.Height;
		F4SE::log::info("f4sr: frame pipeline ready ({}x{})", mainDesc.Width, mainDesc.Height);
		return true;
	}

	bool FramePipeline::Teardown()
	{
		if (!interop_.Drain()) {
			F4SE::log::error("f4sr: refusing pipeline teardown: GPU work not retired");
			return false;
		}
		if (!frameGen_.Destroy()) return false;
		refraction_.End();
		refraction_ = {};
		proxies_.Release();
		proxyDrive_ = false;
		upscaler_.Destroy();
		xessUpscaler_.Destroy();
		upscalerB_.Destroy();
		pathBDecode_ = {};
		pathBEncode_ = {};
		ldrShared_ = {};
		sceneOut_ = {};
		bStage_ = 0;
		bSucceeded_ = false;
		bFailStreak_ = 0;
		pathBXessUnavailable_ = false;
		pendingBReady_ = false;
		pathBTaaBypassed_ = false;
		pathBLastTaaBypassed_ = false;
		pathBLastUpscaled_ = false;
		colorShared_ = {};
		depthShared_ = {};
		mvShared_ = {};
		outputShared_ = {};
		reactive_.Destroy();
		colorSrv_.Reset();
		depthUav_.Reset();
		diagColor_.Reset();
		diagMv_.Reset();
		diagDepth_.Reset();
		diagReady_ = false;
		resetNext_ = true;
		lastQpc_ = 0;
		resolvedThisFrame_ = false;
		initialized_ = false;
		return true;
	}

	void FramePipeline::ResetInitBackoff()
	{
		initAbandoned_ = false;
		initFailures_ = 0;
		nextInitTick_ = 0;
	}

	bool FramePipeline::TryInit(ID3D11Device* device)
	{
		constexpr std::uint32_t kMaxInitFailures = 5;
		if (initAbandoned_ || GetTickCount64() < nextInitTick_) {
			return false;
		}
		initAttempted_ = false;
		if (Init(device)) {
			ResetInitBackoff();
			sr_status::Clear(sr_status::kAlertSrStart);
			return true;
		}
		if (!initAttempted_) {
			return false;  // Still waiting for targets; not a failure.
		}
		++initFailures_;
		// Release the partial pipeline (shared textures, contexts) instead of
		// leaving it for the next attempt to rebuild every frame.
		Teardown();
		if (initFailures_ >= kMaxInitFailures) {
			initAbandoned_ = true;
			interop_.Destroy();
			F4SE::log::error("f4sr: pipeline init failed {} times; super resolution stays off until a setting changes",
				initFailures_);
			sr_status::Raise(sr_status::kAlertSrStart);
			return false;
		}
		const std::uint64_t delayMs = (std::min)(60000ull, 2000ull << (initFailures_ - 1));
		nextInitTick_ = GetTickCount64() + delayMs;
		F4SE::log::warn("f4sr: pipeline init failed (attempt {}/{}); retrying in {} ms",
			initFailures_, kMaxInitFailures, delayMs);
		return false;
	}


	namespace
{
	// Diagnostic shield (2026-11-15): Buffout4's CreateTexture2D
	// warning has killed three save loads today, always attributed
	// to the (verified well-formed) ColorBlackUtility.DDS. The
	// failing calls are made by the game's BSD3DResourceCreator
	// worker with a desc built at runtime, so log-only hook the
	// device vtable slot and record the full desc of every FAILED
	// creation, plus the caller address, to name the bad field.
	// RenderTargetManager dynamic-resolution state (OG offsets,
	// static-asserted in the Dear-Modding engine layout; the manager
	// object lives at REL 1508457 -- the address IS the object, cf.
	// the round-11 crash). Mirrors the game's own
	// SetDynamicResolutionState.
	constexpr std::size_t kDynRatioActivatedOffset = 0xFA8;
	constexpr std::uint64_t kRenderTargetManagerId = 1508457;

	// The engine's own dynamic-resolution controller (ID 1115215) runs on
	// the render thread between frame start and the seam. Every ~31 frames
	// with headroom it adds its step (+0xF98) to the ratio, quantises it and
	// clamps (0.5 -> 0.528: a frame rendered at a scale the upscaler does
	// not know about, seen as a "flash"); without headroom it subtracts
	// +0xF9C down to the floors +0xF90/+0xF94. While the plugin drives the
	// ratio, the controller is neutralised through its own parameters: the
	// step-up threshold (+0xFAC) is maxed so it only counts, both steps are
	// zero and the floors do not exceed the ratio. Restored when released.
	struct ControllerParams { float floorW, floorH, stepUp, stepDown; std::uint32_t threshold; };
	constexpr std::size_t kCtlFloorW = 0xF90, kCtlFloorH = 0xF94, kCtlStepUp = 0xF98, kCtlStepDown = 0xF9C,
		kCtlThreshold = 0xFAC, kCtlCounter = 0xFB0;

	void PinController(std::byte* manager, float widthRatio, float heightRatio, bool pin)
	{
		static ControllerParams saved{};
		static bool held = false;
		auto f = [&](std::size_t o) -> float& { return *reinterpret_cast<float*>(manager + o); };
		auto u = [&](std::size_t o) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(manager + o); };
		if (pin) {
			if (!held) {
				saved = { f(kCtlFloorW), f(kCtlFloorH), f(kCtlStepUp), f(kCtlStepDown), u(kCtlThreshold) };
				held = true;
				F4SE::log::info("f4sr: engine dynres controller pinned (floor {:.3f}x{:.3f} step +{:.3f}/-{:.3f} every {} frames)",
					saved.floorW, saved.floorH, saved.stepUp, saved.stepDown, saved.threshold);
			}
			f(kCtlStepUp) = 0.0f;
			f(kCtlStepDown) = 0.0f;
			f(kCtlFloorW) = (std::min)(saved.floorW, widthRatio);
			f(kCtlFloorH) = (std::min)(saved.floorH, heightRatio);
			u(kCtlThreshold) = 0xFFFFFFFFu;
			u(kCtlCounter) = 0;
		} else if (held) {
			f(kCtlFloorW) = saved.floorW;
			f(kCtlFloorH) = saved.floorH;
			f(kCtlStepUp) = saved.stepUp;
			f(kCtlStepDown) = saved.stepDown;
			u(kCtlThreshold) = saved.threshold;
			held = false;
		}
	}

	void WriteDynResState(float widthRatio, float heightRatio, bool activated)
	{
		auto* manager = reinterpret_cast<std::byte*>(REL::ID(kRenderTargetManagerId).address());
		*reinterpret_cast<float*>(manager + kDynRatioWOffset) = widthRatio;
		*reinterpret_cast<float*>(manager + kDynRatioHOffset) = heightRatio;
		*reinterpret_cast<bool*>(manager + kDynRatioActivatedOffset) = activated;
	}

	void ReadDynResState(float& widthRatio, float& heightRatio, bool& activated)
	{
		auto* manager = reinterpret_cast<std::byte*>(REL::ID(kRenderTargetManagerId).address());
		widthRatio = *reinterpret_cast<float*>(manager + kDynRatioWOffset);
		heightRatio = *reinterpret_cast<float*>(manager + kDynRatioHOffset);
		activated = *reinterpret_cast<bool*>(manager + kDynRatioActivatedOffset);
	}

	// The ratio the pipeline resolves with: quality setting, gated
	// by the ENB Path-A opt-in (mirrors OnPreImagespace).
	// Latched once per frame at frame start (OnTemporalData) so the ratio
	// driven into the engine and the one resolved at the seam agree.
	//
	// The terminal renders its screen glow before the seam, at the dynamic
	// ratio, into a display-size target that is composited after the seam
	// at ratio 1. With sub-native SR the glow shows as a half-size
	// translucent rectangle in the upper left (2026-11-22, ratio 0.5; gone
	// with SR off, ENB irrelevant). Fixed in menu_effects (the glow's
	// ImageSpace effect runs at ratio 1 before the seam); with
	// bNativeTerminal=1 the older fallback renders the whole frame at native
	// size while the terminal is open.
	bool g_nativeForMenu = false;

	bool NativeMenuOpen()
	{
		if (!GetSettings().nativeTerminal) return false;  // fallback only; menu_effects handles the glow
		const auto* ui = RE::UI::GetSingleton();
		if (!ui) return false;
		static const RE::BSFixedString terminal{ "TerminalMenu" };
		return ui->GetMenuOpen(terminal);
	}

	float EffectiveRatio(const Settings& settings)
	{
		float want = QualityRatio(settings.qualityMode);
		const char* note = nullptr;
		// UI Native renders the game and ENB at the render size itself; the
		// Classic ENB crops (and their fallback) do not apply.
		if (ENBPresent() && want < 1.0f && !xefg::UiNativeSession()) {
			if (!settings.enbSubNative) {
				want = 1.0f;
				note = "\t!ENB: held at Native\n\tbENBSubNative=0 in f4sr.ini\n";
			} else if (late_trace::SubNativeState() == late_trace::SubNative::Unavailable) {
				want = 1.0f;
				note = "\t!ENB: held at Native\n\tsee f4sr.log\n";
			}
		}
		if (sr_status::g.note.exchange(note) != note && note) {
			sr_status::Poke();
			F4SE::log::info("f4sr: render resolution {} is held at native with ENB: {}", settings.qualityMode,
				settings.enbSubNative ? late_trace::SubNativeReason() : "bENBSubNative=0 in f4sr.ini");
		}
		if (g_nativeForMenu) {
			want = 1.0f;
		}
		// Path B: the game already renders at the render size; the upscaler
		// does not shrink it further.
		if (xefg::OutputScaling()) want = 1.0f;
		return want;
	}

}

	bool FramePipeline::SrResolved() const
	{
		return SrActive() && resolvedThisFrame_ && resolvedFrame_ == CaptureFrameId();
	}

	void FramePipeline::OnTemporalData(RE::BSGraphics::State* state)
	{
		// Frame start: depth copies are back in render layout until the next resolve.
		post_depth::End();
		frame_trace::FrameStart();
		xefg::OnFrameStart();
		// Restore last frame's chain-triangle swap: passes that run
		// after the seam (final composite included) still sample
		// through the unit triangle, so the swap must outlive the
		// whole Render_UI -- community-shaders restores at
		// EndRenderUI; frame start here is the equivalent point.
		// Restoring right after RenderEffectRange (the first
		// attempt) left the final composite sampling the render
		// region: the screen showed only that corner of the frame.
		if (triSwapped_) {
			auto* ism = static_cast<std::byte*>(triIsm_);
			*reinterpret_cast<void**>(ism + 0x38) = triSaved_;
			triSwapped_ = false;
			triSaved_ = nullptr;
			triIsm_ = nullptr;
		}
		// Apply MCM only before this frame's ratio/jitter and rendering are chosen.
		// A missed seam must unwind the previous proxy scope first.
		if (proxies_.InProxyPass()) proxies_.EndProxyPass({ logical::kMain });
		WatchMcmSettings();
		settingsEnabled_ = GetSettings().enableUpscaling;
		// Drive the engine's own dynamic resolution so sub-native
		// quality modes render the scene smaller (until now the
		// pipeline only kept its own books, so the modes changed no
		// pixels and no frame rate). Written every frame, before
		// Render_PreUI consumes it; the seam resets it to 1.0 after
		// publishing the upscaled image.
		{
			const auto& settings = GetSettings();
			const bool srLive = SrActive();
			{
				const bool native = NativeMenuOpen();
				if (native != g_nativeForMenu)
					F4SE::log::info("f4sr: terminal {}: rendering at {} size", native ? "open" : "closed", native ? "native" : "the quality-mode");
				g_nativeForMenu = native;
			}
			const float ratio = srLive ? EffectiveRatio(settings) : 1.0f;
			if (settings.diagnostics && dynRatioW_ < 1.0f) {
				// The seam leaves 1.0/off; anything else was written by someone else since.
				float w = 0.0f, h = 0.0f; bool on = false;
				ReadDynResState(w, h, on);
				static std::uint32_t foreign{};
				if ((w != 1.0f || h != 1.0f || on) && (++foreign <= 40 || foreign % 100 == 0))
					F4SE::log::info("f4sr: dynres at frame start was {:.3f}x{:.3f} on={} (frame {}, #{})", w, h, on, CaptureFrameId(), foreign);
			}
			WriteDynResState(ratio, ratio, srLive && ratio < 1.0f);
			PinController(reinterpret_cast<std::byte*>(REL::ID(kRenderTargetManagerId).address()), ratio, ratio, srLive && ratio < 1.0f);
			dynres_guard::Report();
			if (settings.dynResGuard && srLive && ratio < 1.0f) {
				auto* manager = reinterpret_cast<std::byte*>(REL::ID(kRenderTargetManagerId).address());
				dynres_guard::Install(reinterpret_cast<float*>(manager + kDynRatioWOffset), reinterpret_cast<float*>(manager + kDynRatioHOffset));
				dynres_guard::Arm(ratio, ratio);
			} else {
				dynres_guard::Disarm();
			}
			proxyDrive_ = srLive && settings.proxyTargets && ratio < 1.0f;
			if (initialized_ && settings.proxyTargets) {
				proxies_.UpdateRenderTargets(ratio, ratio);
			}
			// Own the sequence for every SR mode, including Native AA and
			// runs without proxies. Do not depend on native TAA's sequence
			// while suppressing its resolve. FO4 divides State offsets by
			// the dynamic ratio when constructing the camera projection.
			// The API receives the NEGATIVE of this camera Halton value,
			// as in the SDK camera callback and dispatch pair.
			useHalton_ = false;
			if (srLive && state != nullptr && state->screenWidth > 0 && state->screenHeight > 0) {
				const auto dispW = static_cast<float>(state->screenWidth);
				const auto dispH = static_cast<float>(state->screenHeight);
				const std::uint32_t rendW = (std::max)(1u, static_cast<std::uint32_t>(dispW * ratio));
				const std::uint32_t rendH = (std::max)(1u, static_cast<std::uint32_t>(dispH * ratio));
				// The projection still uses game render pixels. Only the phase
				// count depends on the final output, which is a separate chain in Path B.
				const UINT pathBOutputW = xefg::OutputScaling() ? xefg::OutputWidth() : 0;
				const float upscaleW = pathBOutputW ? static_cast<float>(pathBOutputW) : dispW;
				const std::int32_t phaseCount = (std::max)(1, static_cast<std::int32_t>(
					8.0f * std::pow(upscaleW / static_cast<float>(rendW), 2.0f)));
				if (pathBOutputW) {
					static std::uint32_t lastRenderW{};
					static UINT lastOutputW{};
					if (lastRenderW != rendW || lastOutputW != pathBOutputW) {
						lastRenderW = rendW;
						lastOutputW = pathBOutputW;
						F4SE::log::info("f4sr: Path B jitter: render width {}, output width {}, {} phases", rendW, pathBOutputW, phaseCount);
					}
				}
				const std::int32_t seq = static_cast<std::int32_t>(state->frameCount % static_cast<std::uint32_t>(phaseCount)) + 1;
				haltonJitterX_ = Halton(seq, 2) - 0.5f;
				haltonJitterY_ = Halton(seq, 3) - 0.5f;
				state->offsetX = -2.0f * haltonJitterX_ * ratio / static_cast<float>(rendW);
				state->offsetY = 2.0f * haltonJitterY_ * ratio / static_cast<float>(rendH);
				useHalton_ = true;
			}
		}
		if (state == nullptr) {
			return;
		}
		jitterOffsetX_ = state->offsetX;
		jitterOffsetY_ = state->offsetY;
		screenW_ = state->screenWidth;
		screenH_ = state->screenHeight;
		// currentFrame is the engine's cyclic temporal index (0..7), not a frame ID.
		if (state->frameCount != lastFrame_ + 1 && lastFrame_ != 0) {
			resetNext_ = true;  // discontinuity, not the temporal-index wrap
		}
		lastFrame_ = state->frameCount;
		late_trace::EndMotion();
		if (g_dumpStage.load()==2 && state->frameCount==firstDumpFrame_+1) {
			auto* data=RE::BSGraphics::RendererData::GetSingleton(); const auto slot=GetSettings().mvSlot;
			if(data && slot>=0 && static_cast<unsigned>(slot)<std::size(data->renderTargets))
				late_trace::BeginMotion(reinterpret_cast<ID3D11DeviceContext*>(data->context),reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[slot].texture),static_cast<ID3D11Texture2D*>(proxies_.DebugSlotTexture(logical::kMainCopy)),state->frameCount,CaptureSessionTick());
		}
	}

	void FramePipeline::WatchMcmSettings()
	{
		// MCM writes Data\MCM\Settings\f4sr.ini the moment a setting
		// changes in its UI. Poll the file's write time (every 30 frame-start
		// calls, ~1 s) and re-apply its values live; when a value the
		// live contexts were built with changes (backend, quality,
		// debug view, frame generation), tear down so the normal flow
		// re-inits with the new settings -- no game restart needed.
		if (++watchCounter_ % 30 != 0) {
			return;
		}
		const auto t = McmSettingsWriteTime();
		if (!t || t == mcmWriteTime_) return;
		const auto previous = GetSettings();
		mcmWriteTime_ = t;
		auto s = ReadMcmSettings();
		const int requestedQuality = s.qualityMode;
		if (xefg::OutputScaling()) s.qualityMode = previous.qualityMode;
		GetSettings() = s;
		UpdateRestartPending();
		// A changed context setting deserves a fresh set of init attempts.
		if (s.backend != previous.backend || s.qualityMode != previous.qualityMode ||
			s.debugView != previous.debugView || s.enableFrameGeneration != previous.enableFrameGeneration ||
			s.enableUpscaling != previous.enableUpscaling || s.enbSubNative != previous.enbSubNative) {
			ResetInitBackoff();
		}
		if (initialized_ &&
			(s.backend != builtBackend_ || s.qualityMode != builtQuality_ ||
				s.debugView != builtDebugView_ || s.enableFrameGeneration != builtFrameGen_ ||
				s.enableUpscaling != previous.enableUpscaling || s.enbSubNative != previous.enbSubNative)) {
			F4SE::log::info("f4sr: live settings change (backend {}->{}, quality {}->{}) -- rebuilding pipeline",
				builtBackend_, s.backend, builtQuality_, s.qualityMode);
			if (!Teardown()) {
				GetSettings() = previous;
				F4SE::log::error("f4sr: settings rebuild rejected; retaining previous configuration");
				return;
			}
			if (s.enableUpscaling) {
				auto* data = RE::BSGraphics::RendererData::GetSingleton();
				if (data && data->device) TryInit(reinterpret_cast<ID3D11Device*>(data->device));
			}
		}
		if (xefg::OutputScaling()) xefg::RequestPathBQuality(requestedQuality);
	}

	namespace
	{
		// Saves and restores the OM / viewport / CS state the seam touches.
		struct ContextStateGuard
		{
			ID3D11DeviceContext* ctx;
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* dsv{};
			D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT numVps{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			ID3D11ComputeShader* shader{};
			ID3D11ClassInstance* instances[256]{};
			UINT numInstances{ 256 };
			ID3D11ShaderResourceView* srv{};
			ID3D11UnorderedAccessView* uav{};
			explicit ContextStateGuard(ID3D11DeviceContext* c) : ctx(c)
			{
				ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				ctx->RSGetViewports(&numVps, vps);
				ctx->CSGetShader(&shader, instances, &numInstances);
				ctx->CSGetShaderResources(0, 1, &srv);
				ctx->CSGetUnorderedAccessViews(0, 1, &uav);
			}
			~ContextStateGuard()
			{
				ctx->CSSetShader(shader, instances, numInstances);
				ctx->CSSetShaderResources(0, 1, &srv);
				ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				ctx->RSSetViewports(numVps, vps);
				for (auto* rtv : rtvs) if (rtv) rtv->Release();
				for (UINT i = 0; i < numInstances; ++i) if (instances[i]) instances[i]->Release();
				if (shader) shader->Release();
				if (srv) srv->Release();
				if (uav) uav->Release();
				if (dsv) dsv->Release();
			}
		};
	}

	void FramePipeline::OnPreImagespace(void* rtManager)
	{
		if (static bool checked = false; !checked) {
			// bTestAlert=1 in f4sr.ini: one fake alert on the first scene frame,
			// to check that the error panel appears with the status panel off.
			checked = true;
			if (PluginIniInt("bTestAlert", 0) != 0) sr_status::Raise("Error\t!test alert (bTestAlert=1 in f4sr.ini)\n");
		}
		resolvedThisFrame_ = false;
		pendingBReady_ = false;
		frameGen_.Invalidate();
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (data == nullptr || data->device == nullptr) {
			return;
		}
		auto* device = reinterpret_cast<ID3D11Device*>(data->device);
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		proxies_.SetImageSpaceManager(rtManager);
		if (proxies_.InProxyPass()) {
			// Defensive unwind only; the composite scope normally ends
			// at its own return, before Forward consumes native targets.
			proxies_.EndProxyPass({ logical::kMain });
		}
		if (proxyDrive_) CompositeProbe(4);
		// Settings are latched at frame start, never between rendering and SR.
		const auto& settings = GetSettings();
		settingsEnabled_ = settings.enableUpscaling;
		if (!settingsEnabled_ || !interop_.Healthy()) {
			resetNext_ = true;
			return;
		}
		if (settings.probeOnly) {
			ProbeFrame(data, ctx);
			return;
		}
		if (xefg::OutputScaling() && pathBSrBlocked_) return;
		if (xefg::OutputScaling() && usePathBLogicalSlots_) {
			if (!SelectPathBSceneSlots(*data)) return;
		} else SelectSlots(data, settings);
		if (!FrameFlowSettled()) return;
		if (!initialized_) {
			TryInit(device);
			return; // Render the next frame with the new context's ratio/jitter first.
		}

		auto& mainTarget = data->renderTargets[slotMain_];
		auto& mvTarget = data->renderTargets[slotMV_];
		auto& depthTarget = data->depthStencilTargets[slotDepth_];
		if (!mainTarget.texture || !mvTarget.texture || !depthTarget.texture || !depthTarget.srViewDepth) {
			return;
		}

		auto* mainTex = reinterpret_cast<ID3D11Texture2D*>(mainTarget.texture);
		if (!RebuildOnResize(device, mainTex)) return;

		LogTargetMapOnce(data);

		// f4sr owns the render ratio: it comes from the quality setting
		// (with the ENB Path-A gate), never from the game's own fields -
		// the inputs handed to FSR must describe the resolution WE
		// chose. (An early acceptance build instead trusted the
		// game's own ratio fields, which sat near the dynamic-
		// resolution floor at the first seam frame: renderSize came
		// out 384x216 with jitter far outside [-1, 1] and FSR
		// rejected every dispatch.) The engine's own ratio state IS
		// driven from OnTemporalData with the same value (offsets
		// since verified against the engine layout), so the scene
		// really renders at this ratio; the seam resets it after
		// publishing (see below).
		// Same decision as the ratio driven at frame start (Path A gate,
		// terminal latch).
		const float want = EffectiveRatio(settings);
		// A new render size invalidates the upscaler's history (terminal
		// open/close, live quality change).
		if (want != dynRatioW_ || want != dynRatioH_) resetNext_ = true;
		dynRatioW_ = want;
		dynRatioH_ = want;
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTex->GetDesc(&mainDesc);
		const std::uint32_t displayW = mainDesc.Width;
		const std::uint32_t displayH = mainDesc.Height;
		renderW_ = (std::max)(1u, static_cast<std::uint32_t>(displayW * dynRatioW_));
		renderH_ = (std::max)(1u, static_cast<std::uint32_t>(displayH * dynRatioH_));

		// --- Capture pre-TAA inputs into the shared staging textures ---
		// The chain runs right after us on this same context and may
		// rely on the bindings the renderer left in place, so the
		// OM/viewport state is saved here and restored on exit
		// (the clear-flash probes changed no state at all, which is
		// one difference from the pipeline path under test).
		ContextStateGuard guard{ ctx };
		ID3D12Resource* reactiveMask = CaptureInputs(ctx, settings, mainTex,
			reinterpret_cast<ID3D11Texture2D*>(mvTarget.texture), reinterpret_cast<ID3D11ShaderResourceView*>(depthTarget.srViewDepth));

		if (xefg::OutputScaling()) {
			// Path B: the inputs wait for the finished frame at the
			// pre-interface point; imagespace and ENB run on the raw scene.
			BuildUpscaleFrame(settings, displayW, displayH, reactiveMask, pendingB_);
			pendingBFrame_ = CaptureFrameId();
			pendingBReady_ = true;
			HandOffToImagespace(ctx, data, rtManager, mainTex, displayW, displayH);
			return;
		}
		UpscaleFrame frame{};
		if (!outputShared_.tex11 || !(useXess_ ? xessUpscaler_.Created() : upscaler_.Created())) {
			// Built for UI Native (no seam output or seam context) but the
			// presenter no longer scales the output: not expected in a session.
			static bool logged = false;
			if (!std::exchange(logged, true))
				F4SE::log::error("f4sr: seam upscale skipped: the pipeline was built for the UI Native pipeline; restart the game");
			return;
		}
		if (!DispatchUpscale(ctx, settings, displayW, displayH, mainTex, reactiveMask, frame)) return;

		Writeback(ctx, data, settings, mainTex,
			reinterpret_cast<ID3D11Texture2D*>(depthTarget.texture), reinterpret_cast<ID3D11DepthStencilView*>(depthTarget.dsView[0]), displayW, displayH);
		resolvedFrame_ = CaptureFrameId();
		resolvedThisFrame_ = true;
		++resolveCount_;
		if (resolveCount_ % 600 == 1) {
			F4SE::log::info("f4sr: resolve active (frame {})", resolveCount_);
		}
		SeamDiagnostics(ctx, settings, frame, mainTex);

		HandOffToImagespace(ctx, data, rtManager, mainTex, displayW, displayH);
	}

	void FramePipeline::SelectSlots(RE::BSGraphics::RendererData* data, const Settings& settings)
	{
		// Direct platform slots, pinned empirically on this setup
		// (2026-11-15) and explained on 2026-11-16: they are the
		// platform slots of kMain / kGBufferMotionVectors / depth
		// kMain under the Dear-Modding logical enum (the earlier
		// "mapping is broken" reading indexed the manager's ID
		// arrays with doodlum's enum numbers). Slots stay
		// ini-configurable so trials do not need a rebuild.
		slotMain_ = settings.colorSlot >= 0 && settings.colorSlot < 101 ? static_cast<std::uint32_t>(settings.colorSlot) : 4;
		slotMV_ = settings.mvSlot >= 0 && settings.mvSlot < 101 ? static_cast<std::uint32_t>(settings.mvSlot) : 29;
		slotDepth_ = settings.depthSlot >= 0 && settings.depthSlot < 13 ? static_cast<std::uint32_t>(settings.depthSlot) : 2;
		{
			static bool ptrLogged = false;
			if (!ptrLogged) {
				ptrLogged = true;
				F4SE::log::info("f4sr: direct slots color {} mv {} depth {}; rt tex [1] {:p} [2] {:p} [3] {:p} [4] {:p}",
					slotMain_, slotMV_, slotDepth_,
					reinterpret_cast<const void*>(data->renderTargets[1].texture),
					reinterpret_cast<const void*>(data->renderTargets[2].texture),
					reinterpret_cast<const void*>(data->renderTargets[3].texture),
					reinterpret_cast<const void*>(data->renderTargets[4].texture));
			}
		}
	}

	bool FramePipeline::SelectPathBSceneSlots(const RE::BSGraphics::RendererData& data)
	{
		UINT width{}, height{}, outW{}, outH{};
		if (!xefg::PathBSizes(width, height, outW, outH)) return false;
		const auto slots = logical::ReadSceneSlots();
		if (!logical::SceneTargetsMatch(data, slots, width, height)) return false;
		slotMain_ = slots.color;
		slotDepth_ = slots.depth;
		slotMV_ = slots.motion;
		return true;
	}

	// False while frame flow has not settled since the last load (init is deferred).
	bool FramePipeline::FrameFlowSettled()
	{
		// Load-safety: twice (2026-11-15 13:07 and 21:18) Init landing
		// inside a save-load window was followed within a second by
		// the game's own CreateTexture2D failing for
		// ColorBlackUtility.DDS (Buffout4 warning, game over). Runs
		// where Init completed during the menu never showed it. So
		// Init waits for steady frame flow: 2 s and 120 seam frames
		// since the last >1 s seam gap (a load stalls or pauses the
		// seam; the resume gap itself registers as the spike). This was
		// 15 s / 300 frames; the load crash was later tied to volumetric
		// lighting rather than to Init timing (2026-11-21).
		{
			const auto now = GetTickCount64();
			if (lastSeamTick_ == 0) {
				lastSeamTick_ = now;
				lastSpikeTick_ = now;
			} else if (now - lastSeamTick_ > 1000) {
				lastSeamTick_ = now;
				lastSpikeTick_ = now;
				framesSinceSpike_ = 0;
			} else {
				lastSeamTick_ = now;
				++framesSinceSpike_;
			}
			if (!initialized_ &&
				(now - lastSpikeTick_ <= 2000 || framesSinceSpike_ <= 120)) {
				if (!initDeferLogged_) {
					initDeferLogged_ = true;
					F4SE::log::info("f4sr: deferring init until frame flow settles (load-safety)");
				}
				return false;
			}
		}
		return true;
	}

	// False when the targets changed size and the rebuild failed.
	bool FramePipeline::RebuildOnResize(ID3D11Device* device, ID3D11Texture2D* mainTex)
	{
		// If the game resizes its targets mid-session (the gamescope
		// startup resize is the known case), rebuild the pipeline at
		// the new size instead of copying between mismatched textures.
		{
			D3D11_TEXTURE2D_DESC curDesc{};
			mainTex->GetDesc(&curDesc);
			if (curDesc.Width != latchedW_ || curDesc.Height != latchedH_) {
				F4SE::log::info("f4sr: render size changed {}x{} -> {}x{}, rebuilding pipeline",
					latchedW_, latchedH_, curDesc.Width, curDesc.Height);
				if (!Teardown() || !TryInit(device)) {
					return false;
				}
			}
		}
		return true;
	}

	void FramePipeline::LogTargetMapOnce(RE::BSGraphics::RendererData* data)
	{
		// One-time ground-truth map of every populated target.
		{
			static bool dumped = false;
			if (!dumped) {
				dumped = true;
				for (std::uint32_t i = 0; i < 101; ++i) {
					auto* t = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[i].texture);
					if (t == nullptr) {
						continue;
					}
					D3D11_TEXTURE2D_DESC d{};
					t->GetDesc(&d);
					F4SE::log::info("f4sr: rt[{}] {}x{} fmt {}", i, d.Width, d.Height, static_cast<std::uint32_t>(d.Format));
				}
				for (std::uint32_t i = 0; i < 13; ++i) {
					auto* t = reinterpret_cast<ID3D11Texture2D*>(data->depthStencilTargets[i].texture);
					if (t == nullptr) {
						continue;
					}
					D3D11_TEXTURE2D_DESC d{};
					t->GetDesc(&d);
					F4SE::log::info("f4sr: ds[{}] {}x{} fmt {}", i, d.Width, d.Height, static_cast<std::uint32_t>(d.Format));
				}
			}
		}
	}

	// Copies the render rect of colour and motion vectors into the shared
	// inputs, converts depth, and builds the reactive mask (nullptr: none).
	ID3D12Resource* FramePipeline::CaptureInputs(ID3D11DeviceContext* ctx, const Settings& settings, ID3D11Texture2D* mainTex,
		ID3D11Texture2D* mvTex, ID3D11ShaderResourceView* depthSrv)
	{
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		// The upscaler, frame generation and the depth bridge read only the
		// top-left render rectangle of these display-size inputs, so only
		// that rectangle is copied (a quarter of the traffic at half scale).
		const D3D11_BOX renderBox{ 0, 0, 0, renderW_, renderH_, 1 };
		if (colorShared_.tex11) ctx->CopySubresourceRegion(colorShared_.tex11.Get(), 0, 0, 0, 0, mainTex, 0, &renderBox);
		ctx->CopySubresourceRegion(mvShared_.tex11.Get(), 0, 0, 0, 0, mvTex, 0, &renderBox);
		ID3D12Resource* reactiveMask = settings.reactiveMask ?
			reactive_.Generate(ctx, colorSrv_.Get(), renderW_, renderH_, settings.reactiveScale) : nullptr;
		static std::uint32_t captures = 0;  // Path B never advances resolveCount_
		if (captures++ % 600 == 0 && settings.reactiveMask && colorShared_.tex11)
			F4SE::log::info("f4sr: reactive mask {} (generated {}, no opaque snapshot {}, snapshots {})",
				reactive_.Ready() ? "on" : "unavailable", reactive_.Generated(), reactive_.Missed(), reactive_.Snapshots());
		sr_status::g.reactive.store(reactiveMask != nullptr, std::memory_order_relaxed);
		// XeSS created with the responsive-mask flag needs a mask on every dispatch.
		if (!reactiveMask && useXess_ && xessUpscaler_.WantsMask()) reactiveMask = reactive_.Cleared(ctx);
		{
			ID3D11ShaderResourceView* views[] = { depthSrv };
			ID3D11UnorderedAccessView* uavs[] = { depthUav_.Get() };
			ctx->CSSetShaderResources(0, 1, views);
			ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			ctx->CSSetShader(depthCs_.Get(), nullptr, 0);
			ctx->Dispatch((renderW_ + 7) / 8, (renderH_ + 7) / 8, 1);
			ID3D11ShaderResourceView* nullViews[] = { nullptr };
			ID3D11UnorderedAccessView* nullUavs[] = { nullptr };
			ctx->CSSetShaderResources(0, 1, nullViews);
			ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
			ctx->CSSetShader(nullptr, nullptr, 0);
		}
		return reactiveMask;
	}

	// Upscale on the private D3D12 device, then FSR-FG prepare and XeSS-FG
	// inputs. False when the frame was not upscaled.
	bool FramePipeline::DispatchUpscale(ID3D11DeviceContext* ctx, const Settings& settings, std::uint32_t displayW, std::uint32_t displayH,
		ID3D11Texture2D* mainTex, ID3D12Resource* reactiveMask, UpscaleFrame& frame)
	{
		// --- FSR dispatch on the private D3D12 device ---
		if (!interop_.SignalFromD3D11(ctx) || !interop_.WaitOnD3D12()) {
			resetNext_ = true;
			return false;
		}
		BuildUpscaleFrame(settings, displayW, displayH, reactiveMask, frame);

		auto* list = interop_.BeginFrame();
		if (list == nullptr) {
			resetNext_ = true;
			return false;
		}
		for (auto* input : { frame.color, frame.depth, frame.motionVectors, frame.reactive }) {
			if (input) D3D12Interop::Transition(list, input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		D3D12Interop::Transition(list, frame.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		const bool ok = useXess_ ? xessUpscaler_.Dispatch(list, frame) :
			upscaler_.Dispatch(list, frame, settings.sharpness, settings.debugView);
		{
			D3D11_TEXTURE2D_DESC outDesc{};
			mainTex->GetDesc(&outDesc);
			sr_status::OnDispatch(ok, settings.qualityMode, renderW_, renderH_, outDesc.Width, outDesc.Height);
		}
		if (ok && frameGen_.Created()) {
			FrameGenInputs fgInputs{};
			fgInputs.depth = depthShared_.res12.Get();
			fgInputs.motionVectors = mvShared_.res12.Get();
			fgInputs.renderWidth = renderW_;
			fgInputs.renderHeight = renderH_;
			fgInputs.jitterX = frame.jitterX;
			fgInputs.jitterY = frame.jitterY;
			fgInputs.frameTimeMs = frame.frameTimeMs;
			fgInputs.cameraNear = frame.cameraNear;
			fgInputs.cameraFar = frame.cameraFar;
			fgInputs.reset = frame.reset;
			frameGen_.Prepare(list, fgInputs);
		}
		for (auto* input : { frame.color, frame.depth, frame.motionVectors, frame.reactive }) {
			if (input) D3D12Interop::Transition(list, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		}
		D3D12Interop::Transition(list, frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		const bool submitted = interop_.EndFrame() && interop_.SignalFromD3D12() && interop_.WaitOnD3D11(ctx);
		resetNext_ = !ok || !submitted;
		if (resetNext_) {
			frameGen_.Invalidate(true);
			return false;
		}
		if (xefg::Attached()) {
			XeFGFrameInputs xefgInputs{};
			xefgInputs.depth = depthShared_.tex11.Get();
			xefgInputs.motionVectors = mvShared_.tex11.Get();
			xefgInputs.renderWidth = renderW_;
			xefgInputs.renderHeight = renderH_;
			xefgInputs.jitterX = frame.jitterX;
			xefgInputs.jitterY = frame.jitterY;
			xefgInputs.frameTimeMs = frame.frameTimeMs;
			xefgInputs.reset = frame.reset;
			xefg::SetFrameInputs(ctx, xefgInputs);
		}
		return true;
	}

	// Inputs, jitter, timing and camera for this frame's upscale.
	void FramePipeline::BuildUpscaleFrame(const Settings& settings, std::uint32_t displayW, std::uint32_t displayH,
		ID3D12Resource* reactiveMask, UpscaleFrame& frame)
	{
		LARGE_INTEGER qpc{};
		QueryPerformanceCounter(&qpc);
		float frameMs = 16.6f;
		if (lastQpc_ != 0) {
			LARGE_INTEGER freq{};
			QueryPerformanceFrequency(&freq);
			frameMs = std::clamp(static_cast<float>(qpc.QuadPart - static_cast<std::int64_t>(lastQpc_)) * 1000.0f / static_cast<float>(freq.QuadPart), 0.0f, 100.0f);
		}
		lastQpc_ = static_cast<std::uint64_t>(qpc.QuadPart);

		frame.color = colorShared_.res12.Get();
		frame.depth = depthShared_.res12.Get();
		frame.motionVectors = mvShared_.res12.Get();
		frame.output = outputShared_.res12.Get();
		frame.reactive = reactiveMask;
		frame.renderWidth = renderW_;
		frame.renderHeight = renderH_;
		frame.outputWidth = displayW;
		frame.outputHeight = displayH;
		// SDK Samples/Upscalers/FidelityFX_FSR/internal/fsrapirendermodule.cpp:
		// camera callback = (-2*HaltonX/renderW, +2*HaltonY/renderH),
		// dispatchUpscale and dispatchFgPrep both receive -Halton.
		// Our State offsets produce that projection after FO4's ratio
		// division. Keep the owned and native-offset paths equivalent.
		const float screenW = screenW_ != 0 ? static_cast<float>(screenW_) : static_cast<float>(displayW);
		const float screenH = screenH_ != 0 ? static_cast<float>(screenH_) : static_cast<float>(displayH);
		frame.jitterX = useHalton_ ? -haltonJitterX_ : jitterOffsetX_ * screenW * 0.5f / dynRatioW_;
		frame.jitterY = useHalton_ ? -haltonJitterY_ : -jitterOffsetY_ * screenH * 0.5f / dynRatioH_;
		frame.frameTimeMs = frameMs;
		frame.cameraNear = *reinterpret_cast<float*>(REL::ID(kCameraNearId).address());
		frame.cameraFar = *reinterpret_cast<float*>(REL::ID(kCameraFarId).address());
		// World-camera vertical FOV from the unjittered projection; keep the
		// last good value across frames where the cache has no world entry.
		const auto journalCamera = CaptureFrameCamera();
		if (const auto& camera = journalCamera) {
			lastFovY_ = camera->verticalFov;
		} else if (lastFovY_ <= 0.0f) {
			static bool fovFallbackLogged = false;
			if (!fovFallbackLogged) {
				fovFallbackLogged = true;
				F4SE::log::warn("f4sr: world camera unavailable at the seam; using a 1.0 rad vertical FOV until it appears");
			}
		}
		frame.cameraFovY = lastFovY_ > 0.0f ? lastFovY_ : 1.0f;
		frame.reset = resetNext_ || settings.forceReset;
		if (settings.diagnostics) {
			frame_journal::Row row{};
			row.engineFrame = CaptureFrameId();
			row.frameMs = frameMs;
			if (journalCamera) {
				row.camera = true;
				for (int i = 0; i < 3; ++i) { row.position[i] = journalCamera->position[i]; row.forward[i] = journalCamera->forward[i]; }
			}
			row.jitterX = frame.jitterX; row.jitterY = frame.jitterY;
			row.renderW = renderW_; row.renderH = renderH_;
			row.reset = frame.reset;
			frame_journal::Record(row);
		}
	}

	// The imagespace chain reads the upscaled image; depth in display layout.
	void FramePipeline::Writeback(ID3D11DeviceContext* ctx, RE::BSGraphics::RendererData* data, const Settings& settings, ID3D11Texture2D* mainTex,
		ID3D11Texture2D* depthTex, ID3D11DepthStencilView* depthDsv, std::uint32_t displayW, std::uint32_t displayH)
	{
		// --- Writeback: the imagespace chain now reads our result ---
		ctx->CopyResource(mainTex, outputShared_.tex11.Get());
		frame_trace::Seam();
		if (settings.postDepth && dynRatioW_ < 1.0f) {
			const bool applied = postDepth_.Apply(ctx, depthShared_.tex11.Get(), depthTex,
				depthDsv, displayW, displayH, renderW_, renderH_);
			if (applied) post_depth::Begin(depthTex, renderW_, renderH_);
			if (!postDepthLogged_) {
				postDepthLogged_ = true;
				if (applied) F4SE::log::info("f4sr: post-seam depth in display layout {}x{} -> {}x{} (ds[1]={:p} ds[2]={:p})", renderW_, renderH_, displayW, displayH,
					static_cast<void*>(data->depthStencilTargets[1].texture), static_cast<void*>(data->depthStencilTargets[2].texture));
				else F4SE::log::warn("f4sr: post-seam depth bridge unavailable; effects after the seam keep render-layout depth");
			}
		}
	}

	// Diagnostics after the resolve (bDiagnostics): periodic stats and the
	// IN1/A + IN2/A2 temporal captures.
	void FramePipeline::SeamDiagnostics(ID3D11DeviceContext* ctx, const Settings& settings, const UpscaleFrame& frame, ID3D11Texture2D* mainTex)
	{
		if (settings.diagnostics && dynRatioW_ < 1.0f && resolveCount_ % 300 == 1) {
			LogDiagStats(ctx);
		}
		if (settings.diagnostics && resolveCount_ == 600) frame_trace::Arm();
		auto witness = [&](bool first) {
			// Independent of CMP success: a missing camera must not poison a valid spatial capture.
			const bool camera = WriteTemporalWitness(first ? "f4sr-IN1-camera.txt" : "f4sr-IN2-camera.txt", frame,
				jitterOffsetX_, jitterOffsetY_, settings.sharpness, settings.backend, settings.mvJitterCancellation);
			const bool depth = diagDepth_ && DumpRawToFile(ctx, depthShared_.tex11.Get(), diagDepth_.Get(),
				first ? "f4sr-dump-IN1-depth.raw" : "f4sr-dump-IN2-depth.raw");
			if (!camera || !depth) F4SE::log::warn("f4sr: temporal witness incomplete frame={} world={} depth={}",CaptureFrameId(),camera,depth);
		};
		auto& capture = compositeCapture_;
		if (settings.diagnostics && settings.captureKey > 0 && settings.captureKey < 256) {
			// Re-arm a light IN1/A + IN2/A2 capture on demand, e.g. while the
			// camera turns. The composite (CMP) transaction is not repeated.
			const bool down = (GetAsyncKeyState(settings.captureKey) & 0x8000) != 0;
			const int stage = g_dumpStage.load();
			if (down && !captureKeyDown_ && (stage == 0 || stage == 4) && dynRatioW_ < 1.0f) {
				motionArmed_ = true;
				frame_trace::Arm();
				g_dumpStage.store(0);
				F4SE::log::info("f4sr: capture armed by key at frame={}", CaptureFrameId());
			}
			captureKeyDown_ = down;
		}
		if (settings.diagnostics && g_dumpStage.load() == 0 && dynRatioW_ < 1.0f &&
			(motionArmed_ || capture.ready(CaptureFrameId()) || (!proxyDrive_ && resolveCount_ > 900))) {
			motionArmed_ = false;
			// Keep legacy standalone IN/A with proxies disabled. With proxies on,
			// never consume the one-shot independently of the composite transaction.
			const auto id = CaptureFrameId();
			const bool written = DumpPointA(ctx, mainTex) &&
				DumpRawToFile(ctx, colorShared_.tex11.Get(), diagColor_.Get(), "f4sr-dump-IN1-color.raw") &&
				DumpRawToFile(ctx, mvShared_.tex11.Get(), diagMv_.Get(), "f4sr-dump-IN1-mv.raw");
			capture.active = false;
			if (written && CaptureFrameId() == id) {
				witness(true);
				capture.complete = true;
				firstDumpFrame_ = id;
				g_dumpStage.store(1);
				F4SE::log::info("f4sr: capture committed frame={} composite={} IN1/A", id, proxyDrive_);
			} else F4SE::log::warn("f4sr: IN1/A capture failed frame={}", id);
		}
		if (g_dumpStage.load() == 2 && dynRatioW_ < 1.0f) {
			// Frame 2 (the resolved frame right after frame 1): its
			// inputs + output. Warping IN1 by IN2's motion vectors
			// must reproduce IN2 if the MV contract (units, scale,
			// sign, jitter) is right.
			const auto id = CaptureFrameId();
			if (id != firstDumpFrame_) {
				const bool adjacent = id == firstDumpFrame_ + 1 && diagReady_ &&
					DumpRawToFile(ctx, colorShared_.tex11.Get(), diagColor_.Get(), "f4sr-dump-IN2-color.raw") &&
					DumpRawToFile(ctx, mvShared_.tex11.Get(), diagMv_.Get(), "f4sr-dump-IN2-mv.raw") &&
					DumpRawToFile(ctx, mainTex, diagColor_.Get(), "f4sr-dump-A2-second.raw") && CaptureFrameId() == id;
				if (adjacent) witness(false);
				if (!adjacent) F4SE::log::warn("f4sr: no verified adjacent IN2/A2 first={} current={}", firstDumpFrame_, id);
				g_dumpStage.store(adjacent ? 3 : 4);
			}
		}
	}

	// Ratio 1 for the imagespace chain, dynamic-tri swap, refraction remap.
	void FramePipeline::HandOffToImagespace(ID3D11DeviceContext* ctx, RE::BSGraphics::RendererData* data, void* rtManager,
		ID3D11Texture2D* mainTex, std::uint32_t displayW, std::uint32_t displayH)
	{
		// Output consumers start unjittered for every resolved mode, including Native AA.
		if (auto* state = reinterpret_cast<RE::BSGraphics::State*>(REL::ID(600795).address())) {
			state->offsetX = state->offsetY = 0.0f;
		}
		// Only the dynamic rectangle/triangle transition is sub-native-specific.
		if (dynRatioW_ < 1.0f) {
			float engW = 0.0f, engH = 0.0f;
			bool engOn = false;
			ReadDynResState(engW, engH, engOn);
			{
				// The scene was rendered with whatever the manager held; a value
				// other than the one driven at frame start means this frame's
				// image does not match the render size given to the upscaler.
				static std::uint32_t mismatches{};
				if ((engW != dynRatioW_ || engH != dynRatioH_ || !engOn) && (++mismatches <= 40 || mismatches % 100 == 0))
					F4SE::log::warn("f4sr: dynres changed before the seam: engine {:.3f}x{:.3f} on={} vs driven {:.3f} (frame {}, #{})",
						engW, engH, engOn, dynRatioW_, CaptureFrameId(), mismatches);
			}
			if (resolveCount_ % 600 == 1) {
				F4SE::log::info("f4sr: dynres at seam: engine {:.3f}x{:.3f} on={} (driving {:.3f})",
					engW, engH, engOn, dynRatioW_);
			}
			dynres_guard::Disarm();
			WriteDynResState(1.0f, 1.0f, false);
			// ImageSpaceManager (the thunk's this): screenTriShape
			// @ 0x28, dynamicScreenTriShape @ 0x38 (engine layout).
			auto* ism = static_cast<std::byte*>(rtManager);
			void* unit = *reinterpret_cast<void**>(ism + 0x28);
			void* dyn = *reinterpret_cast<void**>(ism + 0x38);
			if (resolveCount_ % 600 == 1) {
				F4SE::log::info("f4sr: tri swap check: unit={:p} dyn={:p} engaged={}",
					unit, dyn, unit != nullptr && dyn != nullptr && unit != dyn);
			}
			if (unit != nullptr && dyn != nullptr && unit != dyn && !triSwapped_) {
				triIsm_ = rtManager;
				triSaved_ = dyn;
				*reinterpret_cast<void**>(ism + 0x38) = unit;
				triSwapped_ = true;
			}
		}
		// Draw 10 reads color in output coordinates and rt1 refraction at the
		// same UV. Keep the game's original texture in render coordinates.
		if (dynRatioW_ < 1.0f) refraction_.Begin(ctx,data,displayW,displayH,renderW_,renderH_);
		// Only the committed capture's native effect range. End before the B readback.
		if (g_dumpStage.load() == 1 && CaptureFrameId() == firstDumpFrame_)
			late_trace::Begin(ctx, mainTex, firstDumpFrame_, CaptureSessionTick(), true);
	
	}

	void FramePipeline::OnPostImagespace(void* /*rtManager*/)
	{
		if (g_dumpStage.load() == 1) late_trace::End();
		refraction_.End();
		// The triangle swap is restored in the next OnTemporalData, not here.
		if (g_dumpStage.load() == 1) {
			if (CaptureFrameId() != firstDumpFrame_ || !DumpPointB()) F4SE::log::warn("f4sr: B missing for frame={}", firstDumpFrame_);
			g_dumpStage.store(2); // B is not a prerequisite for the adjacent IN2/A2 pair.
		}
		// Observe the actual post-imagespace output, not an assumed HDR slot.
		// This is only a HUD-less candidate: ENB may still process it before C.
		static bool uiCandidateCaptured = false;
		if (g_dumpStage.load() == 3 && !uiCandidateCaptured) {
			uiCandidateCaptured = true;
			auto* data = RE::BSGraphics::RendererData::GetSingleton();
			if (!data || !data->context) return;
			auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
			Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
			ctx->OMGetRenderTargets(1, rtv.GetAddressOf(), nullptr);
			Microsoft::WRL::ComPtr<ID3D11Resource> resource;
			if (rtv) rtv->GetResource(resource.GetAddressOf());
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			if (!resource || FAILED(resource.As(&texture))) return;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1) return;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Microsoft::WRL::ComPtr<ID3D11Device> device;
			ctx->GetDevice(&device);
			Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
			const auto hr = device->CreateTexture2D(&desc, nullptr, stage.GetAddressOf());
			if (SUCCEEDED(hr)) DumpRawToFile(ctx, texture.Get(), stage.Get(), "f4sr-dump-U-postimagespace-candidate.raw");
			else F4SE::log::warn("f4sr: UI candidate capture creation hr=0x{:08X}", static_cast<std::uint32_t>(hr));
		}
	}

	void FramePipeline::OnOpaqueComposited()
	{
		if (!initialized_ || !GetSettings().reactiveMask || !reactive_.Ready()) return;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!data) return;
		auto* main = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[slotMain_].texture);
		if (!main) return;
		D3D11_TEXTURE2D_DESC d{};
		main->GetDesc(&d);
		const auto w = (std::min)(d.Width, static_cast<std::uint32_t>(std::ceil(d.Width * dynRatioW_)));
		const auto h = (std::min)(d.Height, static_cast<std::uint32_t>(std::ceil(d.Height * dynRatioH_)));
		reactive_.Snapshot(reinterpret_cast<ID3D11DeviceContext*>(data->context), main, w, h);
	}
}
