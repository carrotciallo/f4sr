#pragma once
#include <d3d11.h>
#include <cstdint>

namespace f4sr::late_trace
{
	// A single CMP10 -> CMP5 window. Inspect actual API draw bindings, never
	// infer the sampled resource from the render-target manager's current slot.
	void Begin(ID3D11DeviceContext* ctx, ID3D11Texture2D* main, std::uint32_t frame, std::uint64_t sessionTick, bool postChain = false);
	// Only the adjacent witness frame, frame start -> first deferred composite.
	void BeginMotion(ID3D11DeviceContext* ctx, ID3D11Texture2D* motion, ID3D11Texture2D* mainCopy, std::uint32_t frame, std::uint64_t sessionTick);
	void EndMotion();
	using EarlyPassFn = void(*)(void*, std::uint32_t, bool);
	void RunEarlyPass(ID3D11DeviceContext* outer, ID3D11Texture2D* mainCopy, EarlyPassFn original, void* pass, std::uint32_t technique, bool flag);
	// Lower render resolutions under ENB rely on cropping ENB's early-scene
	// draws, located in ENB by instruction signature. Unavailable: the call
	// sites were not found, ENB's draws do not reach a supported backend, or
	// the crop never engaged; the pipeline then stays at native resolution.
	enum class SubNative { Pending, Engaged, Unavailable };
	[[nodiscard]] SubNative SubNativeState();
	[[nodiscard]] const char* SubNativeReason();
	void Mark(const char* boundary);
	void End();
}
