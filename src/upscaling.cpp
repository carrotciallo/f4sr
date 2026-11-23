#include "sr_status.h"
#include "upscaling.h"

#include "F4SE/F4SE.h"
#include "ffx_runtime.h"
#include "settings.h"

#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

namespace f4sr
{
	bool FfxUpscaler::Create(ID3D12Device* device, std::uint32_t maxRenderWidth, std::uint32_t maxRenderHeight,
		std::uint32_t maxOutputWidth, std::uint32_t maxOutputHeight, int backend, bool debugView, bool hdr, bool nonLinearColor)
	{
		if (!FfxRuntimeReady() || device == nullptr) {
			return false;
		}
		Destroy();

		const char* versionName = nullptr;
		const std::uint64_t versionId = PickUpscaleVersion(device, backend, &versionName);
		if (versionId == 0) return false;

		ffxCreateBackendDX12Desc backendDesc{};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backendDesc.device = device;

		ffxOverrideVersion overrideDesc{};
		overrideDesc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		overrideDesc.versionId = versionId;

		ffxCreateContextDescUpscale desc{};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		// FO4 color input is pre-tonemap linear HDR; the game renders with
		// dynamic resolution, so render size varies per frame. Depth is
		// standard [0..1] with sky at 1.0 (not inverted): the input layer
		// feeds the R32_FLOAT copy the community-shaders port produces.
		// Motion vectors are UV-space, hence no display-resolution MV flag.
		desc.flags = FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
		// Path B upscales the finished, tonemapped (LDR) frame after ENB.
		if (hdr) desc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		if (backend == 1 && nonLinearColor) desc.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
		// The world draws bind unjittered motion transforms; the captured
		// stationary field lacks the camera jitter delta. Do not subtract it twice.
		if (GetSettings().mvJitterCancellation) desc.flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
		if (debugView) {
			desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
		}
		desc.maxRenderSize = { maxRenderWidth, maxRenderHeight };
		desc.maxUpscaleSize = { maxOutputWidth, maxOutputHeight };
		desc.fpMessage = &FfxLogMessage;

		// Chain: upscale -> override version -> backend DX12.
		// The upscale-version desc is documented as mandatory ("This must
		// be set to FFX_UPSCALER_VERSION") and the official FSR sample
		// chains it on every create; without it the 4.1.2 provider
		// accepted the context but rejected every dispatch with
		// ERROR_PARAMETER (2026-11-15 acceptance runs).
		ffxCreateContextDescUpscaleVersion versionDesc{};
		versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		versionDesc.version = FFX_UPSCALER_VERSION;
		versionDesc.header.pNext = &backendDesc.header;
		if (versionId != 0) {
			desc.header.pNext = &overrideDesc.header;
			overrideDesc.header.pNext = &versionDesc.header;
		} else {
			desc.header.pNext = &versionDesc.header;
		}

		const auto rc = Ffx().CreateContext(&context_, &desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK) {
			F4SE::log::error("f4sr: ffxCreateContext (upscale) failed: {}", static_cast<std::uint32_t>(rc));
			context_ = nullptr;
			return false;
		}
		F4SE::log::info("f4sr: FSR upscaler created (backend {}, version {})",
			backend, versionName ? versionName : "provider default");
		sr_status::SetBackend(std::string("FSR ") + (versionName ? versionName : (backend == 1 ? "4.x" : "3.x")));
		{
			ffxQueryGetProviderVersion pv{};
			pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
			if (Ffx().Query(&context_, &pv.header) == FFX_API_RETURN_OK) {
				F4SE::log::info("f4sr: context provider version: {} (id {})", pv.versionName ? pv.versionName : "<null>", pv.versionId);
			}
			ffxQueryDescUpscaleGetResourceRequirements rr{};
			rr.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
			if (Ffx().Query(&context_, &rr.header) == FFX_API_RETURN_OK) {
				F4SE::log::info("f4sr: resource requirements: required={} optional={}", rr.required_resources, rr.optional_resources);
			}
		}
		return true;
	}

	void FfxUpscaler::Destroy()
	{
		if (context_ != nullptr) {
			Ffx().DestroyContext(&context_, nullptr);
			context_ = nullptr;
		}
	}

	bool FfxUpscaler::Dispatch(ID3D12GraphicsCommandList* cmdList, const UpscaleFrame& frame, float sharpness, bool debugView)
	{
		if (context_ == nullptr || cmdList == nullptr) {
			return false;
		}
		ffxDispatchDescUpscale dispatch{};
		dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatch.commandList = cmdList;
		dispatch.color = ffxApiGetResourceDX12(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatch.depth = ffxApiGetResourceDX12(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatch.motionVectors = ffxApiGetResourceDX12(frame.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatch.output = ffxApiGetResourceDX12(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		if (frame.reactive) dispatch.reactive = ffxApiGetResourceDX12(frame.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatch.jitterOffset = { frame.jitterX, frame.jitterY };
		// FO4 motion vectors are UV-space [0..1]: scale by render size to
		// convert to pixels (matches doodlum's FO4 FSR integration).
		dispatch.motionVectorScale = { static_cast<float>(frame.renderWidth), static_cast<float>(frame.renderHeight) };
		dispatch.renderSize = { frame.renderWidth, frame.renderHeight };
		dispatch.upscaleSize = { frame.outputWidth, frame.outputHeight };
		dispatch.enableSharpening = sharpness > 0.0f;
		dispatch.sharpness = sharpness;
		dispatch.frameTimeDelta = frame.frameTimeMs;
		dispatch.preExposure = 1.0f;  // game exposes no exposure value; FSR requires > 0
		dispatch.reset = frame.reset;
		dispatch.cameraNear = frame.cameraNear;
		dispatch.cameraFar = frame.cameraFar;
		dispatch.cameraFovAngleVertical = frame.cameraFovY;
		dispatch.viewSpaceToMetersFactor = 0.01428222656f;  // Creation engine unit = 1.428 cm
		dispatch.flags = debugView ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0u;

		const auto rc = Ffx().Dispatch(&context_, &dispatch.header);
		if (rc != FFX_API_RETURN_OK) {
			static int failures = 0;
			++failures;
			if (failures == 1) {
				F4SE::log::error("f4sr: ffxDispatch (upscale) failed: {} - inputs: render {}x{} upscale {}x{} jitter ({}, {}) frameMs {} near {} far {} fov {} color {} depth {} mv {} out {}",
					static_cast<std::uint32_t>(rc), frame.renderWidth, frame.renderHeight, frame.outputWidth, frame.outputHeight,
					frame.jitterX, frame.jitterY, frame.frameTimeMs, frame.cameraNear, frame.cameraFar, frame.cameraFovY,
					frame.color != nullptr, frame.depth != nullptr, frame.motionVectors != nullptr, frame.output != nullptr);
				F4SE::log::error("f4sr: resource descs: color [type {} fmt {} {}x{}] depth [type {} fmt {} {}x{}] mv [type {} fmt {} {}x{}] out [type {} fmt {} {}x{}]",
					dispatch.color.description.type, dispatch.color.description.format, dispatch.color.description.width, dispatch.color.description.height,
					dispatch.depth.description.type, dispatch.depth.description.format, dispatch.depth.description.width, dispatch.depth.description.height,
					dispatch.motionVectors.description.type, dispatch.motionVectors.description.format, dispatch.motionVectors.description.width, dispatch.motionVectors.description.height,
					dispatch.output.description.type, dispatch.output.description.format, dispatch.output.description.width, dispatch.output.description.height);
			} else if (failures % 600 == 0) {
				F4SE::log::error("f4sr: ffxDispatch (upscale) still failing: {} ({} times)", static_cast<std::uint32_t>(rc), failures);
			}
			return false;
		}
		return true;
	}
}
