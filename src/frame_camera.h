#pragma once

#include "RE/Fallout.h"
#include <cmath>
#include <cstring>
#include <optional>

namespace f4sr
{
	struct FrameCamera
	{
		float position[3]{}, right[3]{}, up[3]{}, forward[3]{};
		float nearPlane{}, farPlane{}, verticalFov{};
		std::uint32_t engineFrame{};
	};

	// Same world-camera cache key as CS FrameBuffer: never use the last UI/shadow camera.
	inline std::optional<FrameCamera> CaptureFrameCamera()
	{
		const auto* state = reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address());
		const auto* world = RE::Main::WorldRootCamera();
		if (!state || !world) return std::nullopt;
		for (const auto& entry : state->cameraDataCache) {
			if (entry.referenceCamera != world || !entry.useJitter) continue;
			float view[4][4], projection[4][4], vp[4][4];
			std::memcpy(view, entry.camViewData.viewMat, sizeof(view));
			std::memcpy(projection, entry.camViewData.projMat, sizeof(projection));
			std::memcpy(vp, entry.camViewData.viewProjUnjittered, sizeof(vp));
			for (const auto* matrix : { &view, &projection, &vp })
				for (const auto& row : *matrix)
					for (float value : row) if (!std::isfinite(value)) return std::nullopt;
			// A rigid row-vector view matrix has inverse rotation = transpose.
			// Reject non-rigid data rather than silently using that assumption.
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					float dot{};
					for (int k = 0; k < 3; ++k) dot += view[i][k] * view[j][k];
					if (std::abs(dot - (i == j ? 1.0f : 0.0f)) > 0.01f) return std::nullopt;
				}
				if (std::abs(view[i][3]) > 0.0001f) return std::nullopt;
			}
			if (std::abs(view[3][3] - 1.0f) > 0.0001f) return std::nullopt;
			FrameCamera result{};
			const float adjust[] = { entry.posAdjust.x, entry.posAdjust.y, entry.posAdjust.z };
			for (int i = 0; i < 3; ++i) {
				result.right[i] = view[i][0];
				result.up[i] = view[i][1];
				result.forward[i] = view[i][2];
				result.position[i] = adjust[i];
				for (int k = 0; k < 3; ++k) result.position[i] -= view[3][k] * view[i][k];
				if (!std::isfinite(result.position[i])) return std::nullopt;
			}
			// Recover asymmetric vertical FOV from the unjittered P*V rows
			// (stored transposed by the engine), matching CS's camera contract.
			float forwardLength{}, offset{}, scale{};
			for (int i = 0; i < 3; ++i) forwardLength += vp[i][3] * vp[i][3];
			forwardLength = std::sqrt(forwardLength);
			if (!(forwardLength > 1e-8f)) return std::nullopt;
			for (int i = 0; i < 3; ++i) offset += vp[i][1] * vp[i][3] / (forwardLength * forwardLength);
			for (int i = 0; i < 3; ++i) {
				const float component = (vp[i][1] - offset * vp[i][3]) / forwardLength;
				scale += component * component;
			}
			scale = std::sqrt(scale);
			if (!(scale > 1e-8f)) return std::nullopt;
			result.verticalFov = std::atan((1.0f - offset) / scale) - std::atan((-1.0f - offset) / scale);
			result.nearPlane = -projection[3][2] / projection[2][2];
			result.farPlane = -projection[3][2] / (projection[2][2] - 1.0f);
			result.engineFrame = state->frameCount;
			if (!std::isfinite(result.verticalFov) || !std::isfinite(result.nearPlane) || !std::isfinite(result.farPlane) ||
				result.verticalFov <= 0.0f || result.verticalFov >= 3.141593f ||
				result.nearPlane <= 0.0f || result.farPlane <= result.nearPlane) return std::nullopt;
			return result;
		}
		return std::nullopt;
	}
}
