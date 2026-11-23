#pragma once

// Path B: the UI draws at the display size. Between the pre-interface point
// (after the scene has been upscaled into a display-size canvas) and the
// Present, the game's binds of its frame buffer are redirected to the canvas,
// and its viewports and scissor rectangles are scaled to the canvas size.
// The game's depth-stencil target (render size) is swapped for a
// display-size one while the canvas is bound.

#include <d3d11.h>

namespace f4sr::ui_redirect
{
	// Hooks the game's immediate context (once).
	bool Install(ID3D11DeviceContext* gameContext);
	// From now until End, binds of `frameBuffer` go to `canvas` (same format
	// family, display size). Rebinds the current render targets.
	// sceneUnder: the canvas already holds this frame's (upscaled) scene;
	// frame-buffer draws that bypass the redirect then carry only their
	// changed pixels.
	void Begin(ID3D11DeviceContext* ctx, ID3D11Texture2D* frameBuffer, ID3D11Texture2D* canvas, bool sceneUnder);
	void End();
	[[nodiscard]] bool Active();
	void ResetForResize();

	// Private draws in a swapped D3D11 context state must not replace the
	// recorded game viewport or scissor state.
	class ScopedUntracked
	{
	public:
		ScopedUntracked();
		~ScopedUntracked();
		ScopedUntracked(const ScopedUntracked&) = delete;
		ScopedUntracked& operator=(const ScopedUntracked&) = delete;
	private:
		bool previous_;
	};
}
