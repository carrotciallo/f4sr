#pragma once

// f4sr.log is shared publicly in bug reports. After F4SE::Init, every sink
// of the default logger is wrapped so that the game folder, the user profile
// and Documents folders and any "\Users\<name>\" or "\home\<name>\" segment
// are written as <game>, <profile>, <documents> and <user>.
namespace f4sr::log_redact
{
	void Install();
}
