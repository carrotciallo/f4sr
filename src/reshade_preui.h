#pragma once

#include <windows.h>

// ReShade effects before the UI.
//
// When ReShade (with add-on support) is loaded, f4sr registers as a ReShade
// add-on and, with bReShadeBeforeUI, renders the ReShade effects itself at the
// pre-interface point: after upscaling and ENB's post-processing, at display
// resolution, before the UI is drawn and before frame generation captures its
// HUD-less image. ReShade then skips its own present-time rendering for that
// frame, so the UI stays untouched and generated frames carry the effects
// through interpolation instead of UI extraction.
namespace f4sr::reshade_preui
{
	// Registers the add-on when a ReShade module is loaded. Safe to call from
	// F4SEPlugin_Load (no device work happens here).
	void Install(HMODULE self);
	// Pre-interface hook, before the HUD-less captures.
	void Render(void* rtManager);
	// Before every extra present of a game frame (FSR-FG pairs): keeps ReShade
	// from rendering the effects again on top of the UI at that present.
	void SuppressPresentEffects();
	// True while this frame's effects were rendered before the UI.
	bool Active();
}
