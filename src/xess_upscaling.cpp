#include "sr_status.h"
#include "xess_upscaling.h"
#include "settings.h"

#include <windows.h>

#include <filesystem>

#include "F4SE/F4SE.h"

namespace
{
	using PFN_xessD3D12CreateContext = xess_result_t (*)(ID3D12Device*, xess_context_handle_t*);
	using PFN_xessD3D12Init = xess_result_t (*)(xess_context_handle_t, const xess_d3d12_init_params_t*);
	using PFN_xessD3D12Execute = xess_result_t (*)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*);
	using PFN_xessSetVelocityScale = xess_result_t (*)(xess_context_handle_t, float, float);
	using PFN_xessDestroyContext = xess_result_t (*)(xess_context_handle_t);

	struct XessApi
	{
		PFN_xessD3D12CreateContext createContext{ nullptr };
		PFN_xessD3D12Init init{ nullptr };
		PFN_xessD3D12Execute execute{ nullptr };
		decltype(&xessGetOptimalInputResolution) inputRange{};
		PFN_xessSetVelocityScale setVelocityScale{ nullptr };
		PFN_xessDestroyContext destroyContext{ nullptr };
	};

	const XessApi* Api()
	{
		static const XessApi* api = []() -> const XessApi* {
			char path[MAX_PATH]{};
			HMODULE self = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&Api), &self) == 0 ||
				GetModuleFileNameA(self, path, MAX_PATH) == 0) {
				return nullptr;
			}
			const auto dir = std::filesystem::path(path).parent_path();
			auto* module = LoadLibraryA((dir / "libxess.dll").string().c_str());
			if (module == nullptr) {
				F4SE::log::error("f4sr: libxess.dll not found beside f4sr.dll");
				return nullptr;
			}
			auto* a = new XessApi();
			a->createContext = reinterpret_cast<PFN_xessD3D12CreateContext>(GetProcAddress(module, "xessD3D12CreateContext"));
			a->init = reinterpret_cast<PFN_xessD3D12Init>(GetProcAddress(module, "xessD3D12Init"));
			a->execute = reinterpret_cast<PFN_xessD3D12Execute>(GetProcAddress(module, "xessD3D12Execute"));
			a->inputRange = reinterpret_cast<decltype(a->inputRange)>(GetProcAddress(module, "xessGetOptimalInputResolution"));
			a->setVelocityScale = reinterpret_cast<PFN_xessSetVelocityScale>(GetProcAddress(module, "xessSetVelocityScale"));
			a->destroyContext = reinterpret_cast<PFN_xessDestroyContext>(GetProcAddress(module, "xessDestroyContext"));
			if (a->createContext == nullptr || a->init == nullptr || a->execute == nullptr ||
				a->setVelocityScale == nullptr || a->destroyContext == nullptr) {
				F4SE::log::error("f4sr: libxess.dll entry points missing");
				delete a;
				return nullptr;
			}
			return a;
		}();
		return api;
	}

	xess_quality_settings_t QualitySetting(int mode)
	{
		switch (mode) {
		case 1: return XESS_QUALITY_SETTING_QUALITY;
		case 2: return XESS_QUALITY_SETTING_BALANCED;
		case 3: return XESS_QUALITY_SETTING_PERFORMANCE;
		default: return XESS_QUALITY_SETTING_AA;
		}
	}

	bool Ok(xess_result_t rc, const char* what)
	{
		if (rc == XESS_RESULT_SUCCESS) {
			return true;
		}
		F4SE::log::error("f4sr: XeSS {} failed: {}", what, static_cast<std::int32_t>(rc));
		return false;
	}
}

namespace f4sr
{
	bool XessUpscaler::Create(ID3D12Device* device, std::uint32_t outputWidth, std::uint32_t outputHeight, int qualityMode, bool responsiveMask)
	{
		return CreateImpl(device, { outputWidth, outputHeight }, qualityMode, responsiveMask, nullptr);
	}

	bool XessUpscaler::CreatePathB(ID3D12Device* device, xess_2d_t input, xess_2d_t output, int qualityMode)
	{
		return CreateImpl(device, output, qualityMode, false, &input);
	}

	bool XessUpscaler::CreateImpl(ID3D12Device* device, xess_2d_t output, int qualityMode, bool responsiveMask, const xess_2d_t* pathBInput)
	{
		const auto* api = Api();
		if (device == nullptr || api == nullptr) {
			return false;
		}
		Destroy();
		if (!Ok(api->createContext(device, &context_), "xessD3D12CreateContext")) {
			context_ = nullptr;
			return false;
		}
		xess_d3d12_init_params_t params{};
		params.outputResolution = output;
		params.qualitySetting = QualitySetting(qualityMode);
		if (pathBInput) {
			// XeSS 1.3+ names for the game's fixed 1.0/1.5/1.7/2.0 ratios.
			// Try successively wider ranges for integer-aligned render sizes.
			constexpr xess_quality_settings_t presets[]{ XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY,
				XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED,
				XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE };
			const bool native = pathBInput->x == output.x && pathBInput->y == output.y;
			bool found = native;
			params.qualitySetting = XESS_QUALITY_SETTING_AA;
			if (api->inputRange && !native && qualityMode >= 1 && qualityMode <= 3) {
				for (auto i = static_cast<std::size_t>(qualityMode); i < std::size(presets); ++i) {
					xess_2d_t optimal{}, min{}, max{};
					if (!Ok(api->inputRange(context_, &output, presets[i], &optimal, &min, &max), "xessGetOptimalInputResolution")) break;
					if (pathBInput->x < min.x || pathBInput->x > max.x || pathBInput->y < min.y || pathBInput->y > max.y) continue;
					params.qualitySetting = presets[i];
					found = true;
					break;
				}
			}
			if (!found) {
				F4SE::log::error("f4sr: Path B XeSS-SR: no supported XeSS input range for {}x{} -> {}x{} (preset {})",
					pathBInput->x, pathBInput->y, output.x, output.y, qualityMode);
				Destroy();
				return false;
			}
		}
		// Path A uses pre-tonemap HDR; Path B uses post-ENB linearized
		// LDR. Both use standard [0..1] depth and render-size UV motion.
		params.initFlags = responsiveMask ? XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK : XESS_INIT_FLAG_NONE;
		if (pathBInput) params.initFlags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
		responsiveMask_ = responsiveMask;
		params.creationNodeMask = 1;
		params.visibleNodeMask = 1;
		if (!Ok(api->init(context_, &params), "xessD3D12Init")) {
			Destroy();
			return false;
		}
		if (pathBInput) {
			F4SE::log::info("f4sr: Path B XeSS-SR: context created {}x{} -> {}x{} (MCM preset {}, XeSS quality {}, LDR)",
				pathBInput->x, pathBInput->y, output.x, output.y, qualityMode, static_cast<int>(params.qualitySetting));
		} else {
			F4SE::log::info("f4sr: XeSS upscaler created (quality {})", qualityMode);
			sr_status::SetBackend("XeSS");
		}
		return true;
	}

	void XessUpscaler::Destroy()
	{
		if (context_ != nullptr) {
			if (const auto* api = Api(); api != nullptr) {
				api->destroyContext(context_);
			}
			context_ = nullptr;
		}
	}

	bool XessUpscaler::Dispatch(ID3D12GraphicsCommandList* cmdList, const UpscaleFrame& frame)
	{
		const auto* api = Api();
		if (context_ == nullptr || cmdList == nullptr || api == nullptr) {
			return false;
		}
		// FO4 motion vectors are UV-space: scale to pixels, as with FSR.
		if (!Ok(api->setVelocityScale(context_, static_cast<float>(frame.renderWidth), static_cast<float>(frame.renderHeight)), "xessSetVelocityScale")) {
			return false;
		}
		xess_d3d12_execute_params_t exec{};
		exec.pColorTexture = frame.color;
		exec.pVelocityTexture = frame.motionVectors;
		exec.pDepthTexture = frame.depth;
		exec.pOutputTexture = frame.output;
		if (responsiveMask_) exec.pResponsivePixelMaskTexture = frame.reactive;
		// XeSS jitter is the pixel-space offset in [-0.5, 0.5]; the frame
		// jitter arrives in the same pixel units (FSR convention).
		exec.jitterOffsetX = frame.jitterX;
		exec.jitterOffsetY = frame.jitterY;
		exec.exposureScale = 1.0f;
		exec.resetHistory = frame.reset ? 1u : 0u;
		exec.inputWidth = frame.renderWidth;
		exec.inputHeight = frame.renderHeight;
		return Ok(api->execute(context_, cmdList, &exec), "xessD3D12Execute");
	}
}
