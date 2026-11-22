#pragma once
#include "capture.h"
#include "upscaling.h"
#include <cstring>
#include <iomanip>

namespace f4sr
{
	// Matrices are the world-camera cache at the dispatch seam, NOT assumed
	// shader constants. Retain every matching entry, including nonjittered.
	inline bool WriteTemporalWitness(const char* path, const UpscaleFrame& f, float publishedX, float publishedY,
		float sharpness, int backend, bool cancellation)
	{
		const auto* s=reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address());
		const auto* world=RE::Main::WorldRootCamera();
		std::ofstream out(path); if (!out || !s) return false;
		out << std::setprecision(9) << "identity " << GetCurrentProcessId() << ' ' << CaptureSessionTick() << ' ' << s->frameCount << ' ' << s->currentFrame << '\n'
			<< "extent " << f.renderWidth << ' ' << f.renderHeight << ' ' << f.outputWidth << ' ' << f.outputHeight << '\n'
			<< "state " << publishedX << ' ' << publishedY << ' ' << s->offsetX << ' ' << s->offsetY << '\n'
			<< "dispatch " << f.jitterX << ' ' << f.jitterY << ' ' << f.frameTimeMs << ' ' << f.cameraNear << ' ' << f.cameraFar << ' ' << f.cameraFovY << ' ' << f.reset << ' ' << sharpness << ' ' << backend << ' ' << cancellation << '\n';
		unsigned count=0;
		for (const auto& e:s->cameraDataCache) {
			if (!world || e.referenceCamera!=world) continue;
			out << "world " << count++ << ' ' << e.useJitter << ' ' << reinterpret_cast<std::uintptr_t>(world) << '\n';
			auto matrix=[&](const char* name,const __m128 (&m)[4]) {
				float values[16]; std::memcpy(values,m,sizeof(values)); out << name;
				for (float v:values) out << ' ' << v; out << '\n';
			};
			matrix("view",e.camViewData.viewMat); matrix("projection",e.camViewData.projMat); matrix("vp",e.camViewData.viewProjMat);
			matrix("unjittered",e.camViewData.viewProjUnjittered); matrix("current",e.camViewData.currentViewProjUnjittered); matrix("previous",e.camViewData.previousViewProjUnjittered);
			out << "adjust " << e.posAdjust.x << ' ' << e.posAdjust.y << ' ' << e.posAdjust.z << ' ' << e.currentPosAdjust.x << ' ' << e.currentPosAdjust.y << ' ' << e.currentPosAdjust.z << ' ' << e.previousPosAdjust.x << ' ' << e.previousPosAdjust.y << ' ' << e.previousPosAdjust.z << '\n';
		}
		out << "end " << count << '\n'; out.close(); return out.good() && count!=0;
	}
}
