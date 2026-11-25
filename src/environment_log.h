#pragma once

#include "F4SE/F4SE.h"

// Startup block at the top of f4sr.log describing the player's system, so a
// shared log answers the usual first questions (GPU, driver, OS or Wine,
// display mode, game and F4SE versions, f4sr settings, related mods).
// No user name, computer name or user-directory path is written.
namespace f4sr::environment_log
{
	void Write(const F4SE::LoadInterface* f4se);
}
