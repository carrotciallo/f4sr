#pragma once

namespace f4sr::menu_effects
{
	// Menu ImageSpace effects driven by Interface3D renderers (Pip-Boy and
	// terminal screens, power armour HUD glass, workbench mod menu, VATS
	// target highlight) plus the full-screen menu blur. Their UpdateParams
	// (vtable slot 9) and the viewport helpers scale by the engine's dynamic
	// resolution ratio. Under sub-native SR a call that happens before the
	// seam sees the reduced ratio; when the effect draws into a display-size
	// target that is not the upscaler's input, the result lands at the
	// reduced size in the upper left (2026-11-22, terminal glow).
	//
	// The vtable hooks classify each effect by the target it leaves bound
	// after Render: the upscaler's colour input ("scene", left alone) or
	// anything else ("separate"). Separate effects then run their
	// UpdateParams and Render at ratio 1 while the frame is still before the
	// seam. Every effect logs its first sighting.
	void Install();
}
