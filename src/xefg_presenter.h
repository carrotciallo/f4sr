#pragma once

#include <d3d11.h>

#include <cstdint>

namespace f4sr
{
	// Per-frame inputs from the SR pipeline, on the game's D3D11 device. The
	// textures are display-size with the render rect at the top-left; motion
	// vectors are in render-viewport UV units (current -> previous).
	struct XeFGFrameInputs
	{
		ID3D11Texture2D* depth{ nullptr };
		ID3D11Texture2D* motionVectors{ nullptr };
		std::uint32_t renderWidth{ 0 };
		std::uint32_t renderHeight{ 0 };
		float jitterX{ 0.0f };  // pixel units, FSR/XeSS convention
		float jitterY{ 0.0f };
		float frameTimeMs{ 0.0f };
		bool reset{ false };
	};

	// XeSS frame generation (frame-generation engine 1).
	//
	// Presentation: XeSS-FG presents through its own D3D12 proxy swap chain.
	// Under gamescope a window has exactly one live flip chain, so the game's
	// D3D11 swap chain is not created on the game window at all: the import
	// of D3D11CreateDeviceAndSwapChain is redirected, the chain (ENB -> DXVK)
	// is created on a hidden window instead, and the proxy chain is created on
	// the real window. Each game Present copies the finished back buffer into
	// the proxy's back buffer, tags HUD-less color, depth and motion vectors,
	// and presents the proxy chain with sync interval 1 (FIFO); the game chain
	// then presents to the hidden window with interval 0 so ENB keeps its
	// per-frame bookkeeping. XeLL (required by XeSS-FG) is driven with all six
	// markers per frame.
	namespace xefg
	{
		// Plugin load, before the renderer exists. No-op unless engine == 1.
		void InstallSwapChainRedirect();
		// kPostPostLoad: reinstall in front of hooks other plugins added later.
		void ReclaimSwapChainRedirect();
		[[nodiscard]] bool Attached();
		// The swap chain redirect was installed at startup (XeSS-FG selected and
		// frame generation on).
		[[nodiscard]] bool RedirectInstalled();
		// Path B is running: the game renders at its own size and frames are
		// scaled to the proxy chain's (display) size.
		[[nodiscard]] bool OutputScaling();
		// This session runs the UI Native pipeline (Path B), attached or not yet.
		[[nodiscard]] bool UiNativeSession();
		// Render-size depth copy for ReShade (ID3D12Resource*, null until the
		// first frame) and a counter that changes whenever it is recreated.
		[[nodiscard]] void* ReShadeDepth12(std::uint32_t& generation);
		void RequestPathBQuality(int quality);
		// A Path B failure after startup: Classic from the next start, error panel now.
		void ReportPathBFailure(const char* reason);
		void ConfirmPathBQuality();
		[[nodiscard]] const char* PathBQualityNote();
		// Path B: the game's (render) and the display size, once known.
		bool PathBSizes(UINT& renderW, UINT& renderH, UINT& displayW, UINT& displayH);
		[[nodiscard]] UINT OutputWidth();
		[[nodiscard]] UINT OutputHeight();
		// Path B: the display-size frame the UI is drawn on; presented
		// instead of the game's back buffer (null: scale the back buffer).
		void SetComposedFrame(ID3D11Texture2D* frame);
		// Path B: HUD-less copy from a display-size frame.
		void CaptureHudlessFrom(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame);
		// Render start of a frame (XeLL SIMULATION_END / RENDERSUBMIT_START).
		void OnFrameStart();
		// Before the UI draws: copies the HUD-less frame.
		void CaptureHudless(void* rtManager);
		// After the SR dispatch: copies depth and motion vectors (render rect).
		void SetFrameInputs(ID3D11DeviceContext* ctx, const XeFGFrameInputs& inputs);
	}
}
