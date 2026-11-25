#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "settings.h"
#include "version.h"

namespace f4sr::xefg
{
	const char* PathBQualityNote();
	bool OutputScaling();
}
namespace f4sr::reshade_preui { std::string PanelLine(); }

namespace f4sr::sr_status
{
	// What the upscaler is doing, for the diagnostic panels: backend, quality,
	// render -> output size, dispatch rate and failures. Written on the render
	// thread, read when a panel is rebuilt (at most once per second).
	struct State
	{
		std::mutex m;
		std::string backend{ "none" };
		std::atomic<std::uint64_t> dispatched{}, failed{};
		std::atomic<std::uint32_t> renderW{}, renderH{}, outputW{}, outputH{};
		std::atomic<int> quality{ -1 };
		std::atomic<double> gpuMs{ -1.0 };
		std::atomic<bool> reactive{};
		std::atomic<const char*> note{};    // panel hint (static string) or null       // a reactive mask went with the last dispatch  // last 5 s average of the SR segment (gpu_timing)
	};
	inline State g;

	// Errors the player has to see even with the status panel off. A newly
	// raised alert forces the panel on for kAlertMs; active alerts stay listed
	// in the panel until cleared. Lines are static strings in panel format.
	struct Alerts
	{
		std::mutex m;
		std::vector<const char*> lines;
		std::atomic<std::int64_t> lastRaised{};
	};
	inline Alerts alerts;
	inline constexpr std::int64_t kAlertMs = 30'000;

	inline std::int64_t NowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}
	// Forces the panel on for a condition already shown in it (status notes).
	inline void Poke() { alerts.lastRaised.store(NowMs(), std::memory_order_relaxed); }
	inline void Raise(const char* line)
	{
		{
			std::lock_guard lock(alerts.m);
			if (std::find(alerts.lines.begin(), alerts.lines.end(), line) != alerts.lines.end()) return;
			alerts.lines.push_back(line);
		}
		Poke();
	}
	inline void Clear(const char* line)
	{
		std::lock_guard lock(alerts.m);
		std::erase(alerts.lines, line);
	}
	inline bool PanelForced()
	{
		const auto t = alerts.lastRaised.load(std::memory_order_relaxed);
		return t != 0 && NowMs() - t < kAlertMs;
	}
	// `panelSetting`: the Status Panel option, to say why the panel appeared.
	inline std::string AlertLines(bool panelSetting)
	{
		std::string out;
		{
			std::lock_guard lock(alerts.m);
			for (const char* line : alerts.lines) out += line;
		}
		if (!panelSetting && PanelForced()) out += "\t(this panel closes in 30 s; details in f4sr.log)\n";
		return out;
	}

	inline constexpr const char* kAlertSrStart = "Error\t!upscaler failed to start; upscaling is off\n";
	inline constexpr const char* kAlertPathBSr = "Error\t!upscaler failed; the image is only stretched\n";
	inline constexpr const char* kAlertFsr4 = "Error\t!FSR 4.1 unavailable here; using FSR 3.1\n";
	inline constexpr const char* kAlertPipeline = "Error\t!UI Native failed; Classic until a restart\n";
	inline constexpr const char* kAlertPipelineEarly = "Error\t!UI Native unavailable; using Classic\n";
	inline constexpr const char* kAlertPipelinePrevious = "Error\t!UI Native failed last run; switched to Classic\n";
	inline constexpr const char* kAlertSmallIsize = "Error\t!Fallout4Prefs.ini iSize far below the monitor\n";
	inline constexpr const char* kAlertFrameAtDisplay = "Error\t!Render Resolution ignored: game runs at\n\tmonitor size (check HighFPSPhysicsFix [Display])\n";
	inline constexpr const char* kAlertSizeForced = "Error\t!another mod changed the game resolution;\n\tf4sr set it back to the Render Resolution\n";
	inline constexpr const char* kAlertXefg = "Error\t!XeSS-FG failed to start; no frame generation\n";

	inline void SetBackend(std::string name)
	{
		std::lock_guard lock(g.m);
		g.backend = std::move(name);
	}

	inline void OnDispatch(bool ok, int quality, std::uint32_t rw, std::uint32_t rh, std::uint32_t ow, std::uint32_t oh)
	{
		(ok ? g.dispatched : g.failed).fetch_add(1, std::memory_order_relaxed);
		g.quality.store(quality, std::memory_order_relaxed);
		g.renderW.store(rw, std::memory_order_relaxed);
		g.renderH.store(rh, std::memory_order_relaxed);
		g.outputW.store(ow, std::memory_order_relaxed);
		g.outputH.store(oh, std::memory_order_relaxed);
	}

	// Upscaler rows (fg_overlay format: "label<TAB>value", a value starting
	// with '!' is a warning). `rate` is the dispatch rate since the caller's
	// last rebuild.
	inline std::string UpscaleRows(bool enabled, double rate, bool sampled)
	{
		const char* pathBNote = xefg::PathBQualityNote();
		if (!enabled) return std::string("Upscale\toff (MCM)\n") + (pathBNote ? pathBNote : "");
		static constexpr const char* kQuality[]{ "Native AA", "Quality", "Balanced", "Performance" };
		const int q = g.quality.load(std::memory_order_relaxed);
		std::string backend;
		{
			std::lock_guard lock(g.m);
			backend = g.backend;
		}
		const double gpu = g.gpuMs.load(std::memory_order_relaxed);
		std::string out = std::format("Upscale\t{}  {}\n\t{}x{} -> {}x{}{}\n", backend, q >= 0 && q < 4 ? kQuality[q] : "-",
			g.renderW.load(), g.renderH.load(), g.outputW.load(), g.outputH.load(),
			gpu >= 0 ? std::format("   {:.1f} ms", gpu) : std::string());
		if (const char* note = g.note.load(std::memory_order_relaxed)) out += note;
		if (pathBNote) out += pathBNote;
		if (g.dispatched.load(std::memory_order_relaxed) == 0 || (sampled && rate < 1.0)) out += "\t!not running\n";
		if (const auto failed = g.failed.load(std::memory_order_relaxed)) out += std::format("\t!{} failed runs (see f4sr.log)\n", failed);
		return out;
	}

	// The whole status panel, shared by every frame-generation path:
	//   title (version, pipeline), errors, FPS, upscaler, frame generation,
	//   ReShade.
	// `fps` is the FPS value ("" = the upscaler's run rate), `frameGen` the
	// caller's "FrameGen" rows, `titleSuffix` an optional title addition.
	// `previous` is the caller's dispatch count at its last rebuild, `seconds`
	// the time since then.
	inline std::string Panel(const std::string& fps, const std::string& frameGen, double seconds, std::uint64_t& previous,
		const char* titleSuffix = "")
	{
		const auto n = g.dispatched.load(std::memory_order_relaxed);
		const double rate = seconds > 0 && n >= previous ? static_cast<double>(n - previous) / seconds : 0.0;
		previous = n;
		const bool enabled = GetSettings().enableUpscaling;
		std::string out = std::format("{}   {}{}\n", version::kPanelTitle, xefg::OutputScaling() ? "UI Native" : "Classic", titleSuffix);
		out += AlertLines(GetSettings().frameGenOverlay);
		if (!fps.empty()) out += "FPS\t" + fps + "\n";
		else if (enabled && seconds > 0 && rate >= 1.0) out += std::format("FPS\t{:.0f}\n", rate);
		out += UpscaleRows(enabled, rate, seconds > 0);
		out += frameGen;
		out += reshade_preui::PanelLine();
		return out;
	}
}
