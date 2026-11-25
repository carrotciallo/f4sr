#pragma once

#include <cctype>
#include <format>
#include <string>

#include <windows.h>

namespace f4sr::hfpf
{
	// HighFPSPhysicsFix [Display] keys that decide the game's buffer size
	// in borderless mode. Reported, never changed.
	struct Display
	{
		bool found{};
		std::string summary;
		bool suspect{};
	};

	inline Display Read()
	{
		Display out;
		char exe[MAX_PATH]{};
		const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
		std::string path(exe, n);
		if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos) path.resize(slash + 1);
		path += "Data\\F4SE\\Plugins\\HighFPSPhysicsFix.ini";
		if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			out.summary = "HighFPSPhysicsFix.ini not found";
			return out;
		}
		out.found = true;
		const auto read = [&](const char* key) {
			char value[64]{};
			GetPrivateProfileStringA("Display", key, "\x01", value, sizeof(value), path.c_str());
			std::string v = value[0] == '\x01' ? std::string() : std::string(value);
			for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return v;
		};
		const auto on = [](const std::string& v) { return v == "true" || v == "1"; };
		out.summary = "HighFPSPhysicsFix.ini [Display]:";
		for (const char* key : { "Fullscreen", "Borderless", "BorderlessUpscale", "Resolution", "ResolutionScale", "ResizeBuffersDisable",
				 "ResizeTargetDisable" }) {
			const auto v = read(key);
			out.summary += std::format(" {}={}", key, v.empty() ? "(unset)" : v);
		}
		const auto upscale = read("BorderlessUpscale");
		out.suspect = !on(upscale) || on(read("ResizeBuffersDisable")) || !read("Resolution").empty() || !read("ResolutionScale").empty();
		return out;
	}
}
