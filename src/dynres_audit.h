#pragma once

namespace f4sr::dynres_audit
{
	// Finds render targets that are written before the SR seam with a reduced
	// (dynamic-resolution) viewport and read after it, the pattern behind the
	// terminal glow (2026-11-22: a half-size translucent rectangle in the upper
	// left). Hooks the game context's draw / copy calls (vtable of the
	// immediate context the renderer uses, ENB wrapper included) and is active
	// only while one of the listed menus is open, or always with
	// bDynResAudit=1. Each finding is logged once: the texture, its
	// RendererData slot if any, the reduced viewport, and short call stacks of
	// the writer and the reader (exe RVAs, resolvable with tools/re).
	void OnFrameStart();       // render thread, UpdateTemporalData
	void OnSeam();             // before the imagespace range
	void OnPostImagespace();   // after the imagespace range: UI phase
}
