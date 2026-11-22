#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

#include "ffx_api.h"
#include "interop.h"
#include "fg_overlay.h"

namespace f4sr
{
	struct FrameGenInputs
	{
		ID3D12Resource* depth{ nullptr };
		ID3D12Resource* motionVectors{ nullptr };
		std::uint32_t renderWidth{ 0 };
		std::uint32_t renderHeight{ 0 };
		float jitterX{ 0.0f };
		float jitterY{ 0.0f };
		float frameTimeMs{ 0.0f };
		float cameraNear{ 0.0f };
		float cameraFar{ 0.0f };
		bool reset{ false };
	};

	// FSR frame generation in manual-present mode. The generated frame is
	// presented on the game's own swapchain (a second Present of the same
	// chain), because that is the only chain the compositor tracks for
	// this window - a generated frame counts only when it is Presented
	// there (presentation research, 2026-11-15). No second presenter, no
	// write-back into another chain.
	// Name of the open menu/UI state that blocks frame generation, or nullptr.
	const char* FrameGenMenuBlockReason();

	class FrameGen
	{
	public:
		// gameChain: the game's IDXGISwapChain (from RendererWindow).
		// Installs the Present vtable hook on success.
		bool Create(D3D12Interop& interop, ID3D12Device* device, IDXGISwapChain* gameChain,
			std::uint32_t displayWidth, std::uint32_t displayHeight, DXGI_FORMAT backBufferFormat);
		bool InstallPresentHook(IDXGISwapChain* gameChain);
		bool Destroy();
		void Invalidate(bool reset = false) { resetNext_ = resetNext_ || prepared_ || reset; prepared_ = false; hudlessReady_ = false; }
		void CaptureHudless(void* rtManager);

		// Records the PrepareV2 dispatch for the frame being rendered.
		// Must be called exactly once per rendered frame, in frame order.
		void Prepare(ID3D12GraphicsCommandList* cmdList, const FrameGenInputs& inputs);

		[[nodiscard]] bool Created() const { return context_ != nullptr; }
		[[nodiscard]] std::uint64_t LastPreparedFrame() const { return frameId_; }

		// Present detour implementation (called by the vtable hook).
		HRESULT OnPresent(IDXGISwapChain* chain, std::uint32_t syncInterval, std::uint32_t flags);

	private:
		// OnPresent stages.
		void DumpPresentCaptures(IDXGISwapChain* chain);
		void LogPresentViewport(IDXGISwapChain* chain);
		void DrawOverlay(IDXGISwapChain* chain, std::uint32_t flags, bool generated);
		bool GenerateFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer);
		void CaptureSamples(ID3D11DeviceContext* ctx);
		void LogPairStats(IDXGISwapChain* chain, std::uint32_t syncInterval, std::uint32_t flags,
			LARGE_INTEGER generatedStart, LARGE_INTEGER generatedEnd, LARGE_INTEGER realEnd);
		D3D12Interop* interop_{ nullptr };
		SharedTexture presentColor_;
		SharedTexture hudlessColor_;
		SharedTexture generated_;
		bool hudlessReady_{ false };
		std::uint32_t hudlessEngineFrame_{ 0 };
		ffxContext context_{ nullptr };
		std::uint64_t frameId_{ 0 };
		bool prepared_{ false };
		// DXGI frame latency counts Presents; a generated pair presents twice
		// per source frame, so the queue is doubled while pairs are active.
		void ApplyFrameLatency(IDXGISwapChain* chain, bool pairs);
		std::uint32_t latencyBase_{ 0 };
		bool latencyRaised_{ false };
		bool resetNext_{ true };
		bool preparedReset_{ true };
		IDXGISwapChain* chain_{ nullptr }; // Non-owning; validated at every Present.
		std::uint64_t preparedTick_{ 0 };
		std::uint32_t preparedEngineFrame_{ 0 };
		std::uint32_t previousEngineFrame_{ 0 };
		float previousPosition_[3]{}, previousForward_[3]{};
		float previousFov_{ 0.0f };
		FgOverlay overlay_;
		FgOverlaySample overlaySample_;
		FgOverlaySample overlayPrevious_;
		unsigned fgCaptureCount_{};
		std::uint64_t skippedFrames_{ 0 };
		std::uint64_t generatedPresents_{ 0 };
		std::uint64_t realPresents_{ 0 };
		std::uint64_t realOnlyPresents_{ 0 };
		std::uint32_t displayWidth_{ 0 };
		std::uint32_t displayHeight_{ 0 };
	};

}
