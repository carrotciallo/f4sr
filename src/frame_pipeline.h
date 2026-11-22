#pragma once

#include "reshade_preui.h"
#include "reactive_mask.h"

#include <d3d11.h>

#include <cstdint>

#include "composite_capture.h"
#include "depth_bridge.h"
#include "framegen.h"
#include "xefg_presenter.h"
#include "interop.h"
#include "proxy_targets.h"
#include "refraction_bridge.h"
#include "upscaling.h"
#include "xess_upscaling.h"

namespace RE::BSGraphics
{
	class State;
}

namespace f4sr
{
	struct Settings;
	// Owns the per-frame super-resolution resolve: captures the game's
	// pre-TAA scene inputs at the imagespace seam, runs the FSR dispatch
	// on the private D3D12 device, and writes the result back into kMain
	// before the imagespace chain reads it.
	class FramePipeline
	{
	public:
		// UpdateTemporalData thunk (after the original): capture the
		// frame's jitter source values and dynamic-resolution ratios.
		void BeginTemporalData() { resolvedThisFrame_ = false; }
		void OnTemporalData(RE::BSGraphics::State* state);
		// RenderEffectRange thunk (before the original): run SR and
		// replace kMain. rtManager is the thunk's `this` (ratios live at
		// +0xF88/+0xF8C in the engine layout).
		void OnPreImagespace(void* rtManager);
		void OnPostImagespace(void* rtManager);
		void OnPreInterface(void* rtManager)
		{
			// Effects first, so the HUD-less captures carry them.
			reshade_preui::Render(rtManager);
			frameGen_.CaptureHudless(rtManager);
			xefg::CaptureHudless(rtManager);
		}
		bool EnsureDiagStaging(ID3D11Device* device, std::uint32_t dispW, std::uint32_t dispH);
		bool DumpPointA(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex);
		bool DumpPointB();
		void LogDiagStats(ID3D11DeviceContext* ctx);
		[[nodiscard]] bool SrActive() const { return initialized_ && settingsEnabled_ && interop_.Healthy(); }
		[[nodiscard]] bool SrResolved() const;
		// Composite/SSLR hook gating: the pipeline is driving a
		// sub-native frame and the proxy module is enabled.
		[[nodiscard]] bool ProxyDriving() const { return proxyDrive_; }
		ProxyTargets& Proxies() { return proxies_; }

		// One-shot composite-pass probe: called from the composite
		// thunk at entry (0), after BeginProxyPass (1), after the
		// original pass (2) and after EndProxyPass (3). Dumps the
		// kMain/kMainCopy textures around the pass, once, on the
		// first eligible gameplay composite after a successful resolve.
		void CompositeProbe(int point);

		// End of the deferred composite pass: snapshot of the opaque scene
		// for the reactive mask.
		void OnOpaqueComposited();

	private:
		// OnPreImagespace stages, in order.
		void SelectSlots(RE::BSGraphics::RendererData* data, const Settings& settings);
		bool FrameFlowSettled();
		bool RebuildOnResize(ID3D11Device* device, ID3D11Texture2D* mainTex);
		void LogTargetMapOnce(RE::BSGraphics::RendererData* data);
		ID3D12Resource* CaptureInputs(ID3D11DeviceContext* ctx, const Settings& settings, ID3D11Texture2D* mainTex,
			ID3D11Texture2D* mvTex, ID3D11ShaderResourceView* depthSrv);
		void BuildUpscaleFrame(const Settings& settings, std::uint32_t displayW, std::uint32_t displayH,
			ID3D12Resource* reactiveMask, UpscaleFrame& frame);
		bool DispatchUpscale(ID3D11DeviceContext* ctx, const Settings& settings, std::uint32_t displayW, std::uint32_t displayH,
			ID3D11Texture2D* mainTex, ID3D12Resource* reactiveMask, UpscaleFrame& frame);
		void Writeback(ID3D11DeviceContext* ctx, RE::BSGraphics::RendererData* data, const Settings& settings, ID3D11Texture2D* mainTex,
			ID3D11Texture2D* depthTex, ID3D11DepthStencilView* depthDsv, std::uint32_t displayW, std::uint32_t displayH);
		void SeamDiagnostics(ID3D11DeviceContext* ctx, const Settings& settings, const UpscaleFrame& frame, ID3D11Texture2D* mainTex);
		void HandOffToImagespace(ID3D11DeviceContext* ctx, RE::BSGraphics::RendererData* data, void* rtManager,
			ID3D11Texture2D* mainTex, std::uint32_t displayW, std::uint32_t displayH);

		bool Init(ID3D11Device* device);
		// Init with failure accounting: a failure past the waiting gates
		// releases the partial pipeline and backs off (2 s doubling to 60 s);
		// after kMaxInitFailures the pipeline stays off until a settings change.
		bool TryInit(ID3D11Device* device);
		void ResetInitBackoff();
		bool Teardown();
		void WatchMcmSettings();
		void ProbeFrame(RE::BSGraphics::RendererData* data, ID3D11DeviceContext* ctx);
		void CreateStaging(ID3D11Device* device, ID3D11Texture2D* mainTex, ID3D11Texture2D* mvTex);
		bool CompileDepthShader(ID3D11Device* device);

		D3D12Interop interop_;
		ProxyTargets proxies_;
		RefractionBridge refraction_;
		DepthBridge postDepth_;
		bool postDepthLogged_{};
		bool captureKeyDown_{};
		bool motionArmed_{};
		bool proxyDrive_{ false };
		CompositeCapture compositeCapture_;
		std::uint32_t firstDumpFrame_{};
		// Owned camera jitter for Native AA and sub-native SR alike.
		// The provider receives its negative, matching the SDK's
		// projection-offset / dispatch-jitter convention.
		float haltonJitterX_{ 0.0f };
		float haltonJitterY_{ 0.0f };
		bool useHalton_{ false };
		FfxUpscaler upscaler_;
		XessUpscaler xessUpscaler_;
		bool useXess_{ false };
		FrameGen frameGen_;
		SharedTexture colorShared_;
		SharedTexture depthShared_;
		SharedTexture mvShared_;
		SharedTexture outputShared_;
		ReactiveMask reactive_;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorSrv_;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depthUav_;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> depthCs_;
		bool initialized_{ false };
		bool resolvedThisFrame_{ false };
		std::uint32_t resolvedFrame_{};
		bool settingsEnabled_{ true };
		// Settings the live contexts were built with; the MCM watcher
		// tears the pipeline down when a reload changes any of them,
		// and the normal flow re-inits with the new values.
		int builtBackend_{ -1 };
		int builtQuality_{ -1 };
		bool builtDebugView_{ false };
		bool builtFrameGen_{ false };
		std::uint32_t watchCounter_{ 0 };
		std::uint64_t mcmWriteTime_{ 0 };
		// Direct RendererData slots for the scene color / motion
		// vectors / depth, from the ini (they equal the platform
		// slots the manager's ID arrays give for kMain /
		// kGBufferMotionVectors / depth kMain under the Dear-
		// Modding logical enum; see proxy_targets.h).
		std::uint32_t latchedW_{ 0 };
		std::uint32_t latchedH_{ 0 };
		std::uint32_t slotMain_{ 4 };
		std::uint32_t slotMV_{ 29 };
		std::uint32_t slotDepth_{ 2 };
		// Captured at Begin (same values the frame renders with).
		float dynRatioW_{ 1.0f };
		float dynRatioH_{ 1.0f };
		float jitterOffsetX_{ 0.0f };
		float jitterOffsetY_{ 0.0f };
		std::uint32_t screenW_{ 0 };
		std::uint32_t screenH_{ 0 };
		std::uint32_t renderW_{ 0 };
		std::uint32_t renderH_{ 0 };
		std::uint32_t lastFrame_{ 0 };
		bool resetNext_{ true };
		std::uint64_t lastQpc_{ 0 };
		// Load-safety gate for Init: seam-call timing state. Init's
		// device/context creation must not land inside a save load
		// (see OnPreImagespace).
		std::uint32_t resolveCount_{ 0 };
		void* triSaved_{ nullptr };
		void* triIsm_{ nullptr };
		bool triSwapped_{ false };
		Microsoft::WRL::ComPtr<ID3D11Texture2D> diagColor_;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> diagMv_;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> diagDepth_;
		bool diagReady_{ false };
		std::uint64_t lastSeamTick_{ 0 };
		std::uint64_t lastSpikeTick_{ 0 };
		std::uint32_t framesSinceSpike_{ 0 };
		bool initDeferLogged_{ false };
		// Init failure backoff (see TryInit).
		bool initAttempted_{ false };
		bool initAbandoned_{ false };
		std::uint32_t initFailures_{ 0 };
		std::uint64_t nextInitTick_{ 0 };
		// Target-size stability gate: Init waits until kMain matches the
		// swapchain and has kept that size for a number of attempts.
		std::uint32_t sizeCandidateW_{ 0 };
		std::uint32_t sizeCandidateH_{ 0 };
		std::uint32_t sizeStableCount_{ 0 };
		// Last world-camera vertical FOV (radians) seen at the seam.
		float lastFovY_{ 0.0f };
	};

	FramePipeline& Pipeline();
}
