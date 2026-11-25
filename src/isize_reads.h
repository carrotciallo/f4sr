#pragma once

// Path B: the game's iSize reads, decided without engine state so that
// tools/isize-test can drive the same code against real INI files.
//
// GetPrivateProfileIntA/StringA return the caller's default when a key is
// missing from the file. The game reads iSize from several INI files and
// passes its current value as that default, so after the first file has been
// answered with the render size, every file without the key hands the render
// size back. Only a value that is really in the file is a display size.

#include <Windows.h>

#include <cmath>
#include <cstring>

namespace isize_reads
{
	// The engine's render size for a display size: even, never larger.
	inline unsigned Scale(unsigned display, float ratio)
	{
		if (display == 0 || ratio >= 1.0f) return display;
		return static_cast<unsigned>(std::lround(display * ratio)) & ~1u;
	}

	// Whether `key` is present in `file`, asked through kernel32 directly
	// (this module's import is not hooked) with a default no file contains.
	inline bool KeyInFile(LPCSTR section, LPCSTR key, LPCSTR file)
	{
		static constexpr char kAbsent[] = "\x01" "f4sr-absent" "\x01";
		char buf[64]{};
		GetPrivateProfileStringA(section, key, kAbsent, buf, sizeof(buf), file);
		return std::strcmp(buf, kAbsent) != 0;
	}

	// A size below half the monitor on both axes is worth a warning:
	// 1.1.0-alpha.1 could leave such values behind in Fallout4Prefs.ini.
	// The size is still used as is; only the player can tell a choice from
	// a leftover.
	inline bool Implausible(unsigned width, unsigned height, unsigned monitorWidth, unsigned monitorHeight)
	{
		return width && height && monitorWidth && monitorHeight && width * 2 < monitorWidth && height * 2 < monitorHeight;
	}

	// The display mode of the primary monitor in physical pixels (not DPI
	// virtualised), or 0x0.
	inline void MonitorMode(unsigned& width, unsigned& height)
	{
		DEVMODEA mode{};
		mode.dmSize = sizeof(mode);
		if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
			width = mode.dmPelsWidth;
			height = mode.dmPelsHeight;
		} else {
			width = height = 0;
		}
	}

	// What a write of iSize to `file` should store: the value the file
	// already has, else `fallback` (the display size read this session).
	inline unsigned WriteValue(LPCSTR section, LPCSTR key, LPCSTR file, unsigned fallback)
	{
		if (KeyInFile(section, key, file))
			if (const unsigned v = GetPrivateProfileIntA(section, key, 0, file)) return v;
		return fallback;
	}

	struct Answer
	{
		bool present{};       // the key is in the file; otherwise `render` is the value as read
		bool implausible{};   // the file's pair is below half the monitor
		unsigned fileW{}, fileH{};
		unsigned display{};   // display size for this axis
		unsigned render{};    // what the game is told
	};

	// One iSize read: `which` 1 = width, 2 = height; `value` is what the API
	// returned.
	inline Answer Decide(int which, unsigned value, LPCSTR section, LPCSTR key, LPCSTR file, float ratio, unsigned monitorWidth, unsigned monitorHeight)
	{
		Answer a{};
		a.render = value;
		if (value == 0 || !KeyInFile(section, key, file)) return a;
		a.present = true;
		a.fileW = which == 1 ? value : GetPrivateProfileIntA(section, "iSize W", 0, file);
		a.fileH = which == 2 ? value : GetPrivateProfileIntA(section, "iSize H", 0, file);
		a.display = value;
		a.implausible = Implausible(a.fileW, a.fileH, monitorWidth, monitorHeight);
		a.render = Scale(a.display, ratio);
		return a;
	}
}
