#pragma once

namespace f4sr
{
	// Installs the two call-site hooks (DrawWorld::Begin jitter capture,
	// DrawWorld::Render_UI SR seam). Call once from F4SEPlugin_Load after
	// F4SE::AllocTrampoline.
	void InstallHooks();
}
