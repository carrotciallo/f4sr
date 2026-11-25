#pragma once

#include <d3d12.h>

#include <cstdint>

#include "ffx_api.h"

namespace f4sr
{
	// One frame of upscaling inputs, all resources on the private D3D12
	// device (interop layer owns the D3D11->D3D12 copies and guarantees
	// states: inputs in COMPUTE_READ, output in COMPUTE_WRITE).
	struct UpscaleFrame
	{
		ID3D12Resource* color{ nullptr };
		ID3D12Resource* depth{ nullptr };
		ID3D12Resource* motionVectors{ nullptr };
		ID3D12Resource* output{ nullptr };
		ID3D12Resource* reactive{ nullptr };  // optional R8 mask, render size (reactive_mask.h)
		std::uint32_t renderWidth{ 0 };
		std::uint32_t renderHeight{ 0 };
		std::uint32_t outputWidth{ 0 };
		std::uint32_t outputHeight{ 0 };
		// Jitter already converted to the FSR convention by the caller:
		// negated and divided by the dynamic-resolution ratio (the form
		// doodlum's FO4 integration and the game hook both produce).
		float jitterX{ 0.0f };
		float jitterY{ 0.0f };
		float frameTimeMs{ 0.0f };
		float cameraNear{ 0.0f };
		float cameraFar{ 0.0f };
		float cameraFovY{ 0.0f };
		bool reset{ false };  // camera cut / scene load: clear temporal history
	};

	// FSR upscaling through the ffx-api unified interface. The runtime DLL
	// selects the provider version (FSR 3.1.6 or FSR 4.1.2 INT8 on RDNA3)
	// from the override version id resolved at Create time.
	class FfxUpscaler
	{
	public:
		// backend: 0 = FSR 3.1, 1 = FSR 4.1 (see settings). debugView allows
		// the FFX debug visualization output used by the acceptance checks.
		bool Create(ID3D12Device* device, std::uint32_t maxRenderWidth, std::uint32_t maxRenderHeight,
			std::uint32_t maxOutputWidth, std::uint32_t maxOutputHeight, int backend, bool debugView,
			bool hdr = true, bool nonLinearColor = false);
		void Destroy();
		bool Dispatch(ID3D12GraphicsCommandList* cmdList, const UpscaleFrame& frame, float sharpness, bool debugView);
		[[nodiscard]] bool Created() const { return context_ != nullptr; }

	private:
		ffxContext context_{ nullptr };
	};
}
