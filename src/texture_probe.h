#pragma once

namespace f4sr
{
	// Startup-only, opt-in call-site instrumentation. Never patches the device vtable.
	void InstallTextureProbe();
}
