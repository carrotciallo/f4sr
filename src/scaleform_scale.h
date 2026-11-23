#pragma once

// Path B: Scaleform movies drawn to the screen get a display-size viewport,
// so their shapes and glyphs are rasterised at the display size (the UI
// redirect sends their draws to the display-size canvas). Movies drawn into
// their own textures keep their size. Pixel-space input (mouse events, mouse
// state, hit tests) is converted between the game's render-size coordinates
// and the movie's display-size viewport, and GetViewport returns what the
// game set.
namespace f4sr::scaleform_scale
{
	// Plugin load (before any movie exists). Sizes come from
	// xefg::PathBSizes once the game has read its display settings.
	bool Install();
}
