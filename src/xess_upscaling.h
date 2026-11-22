#pragma once

#include <d3d12.h>

#include <cstdint>

#include "upscaling.h"  // UpscaleFrame
#include "xess/xess_d3d12.h"

namespace f4sr
{
	// XeSS super resolution (DP4a path on RDNA3) behind the same frame
	// inputs as the FSR backend. Selected with iBackend=2.
	//
	// libxess.dll is loaded at runtime by full path from the plugin
	// directory: a static import is resolved through the executable's
	// search path, which does not include Data\F4SE\Plugins - F4SE
	// rejected the whole plugin with Error 126 on 2026-11-15.
	class XessUpscaler
	{
	public:
		bool Create(ID3D12Device* device, std::uint32_t outputWidth, std::uint32_t outputHeight, int qualityMode, bool responsiveMask = false);
		void Destroy();
		bool Dispatch(ID3D12GraphicsCommandList* cmdList, const UpscaleFrame& frame);
		[[nodiscard]] bool Created() const { return context_ != nullptr; }

	private:
		xess_context_handle_t context_{ nullptr };
		bool responsiveMask_{ false };
	public:
		[[nodiscard]] bool WantsMask() const { return responsiveMask_; }
	};
}
