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
		if (!panelSetting && PanelForced()) out += "\t(error panel: hides in 30 s; details in f4sr.log)\n";
		return out;
	}

	inline constexpr const char* kAlertSrStart = "ERR\t!super resolution failed to start; it stays off\n";
	inline constexpr const char* kAlertPathBSr = "ERR\t!Path B upscaler unavailable: bilinear only\n";
	inline constexpr const char* kAlertFsr4 = "ERR\t!FSR 4.1 unavailable here: using FSR 3.1\n";
	inline constexpr const char* kAlertPipeline = "ERR\t!UI native pipeline failed: restart (now Classic)\n";
	inline constexpr const char* kAlertPipelineEarly = "ERR\t!UI native pipeline unavailable: using Classic\n";
	inline constexpr const char* kAlertPipelinePrevious = "ERR\t!Classic pipeline: UI native failed last run\n";
	inline constexpr const char* kAlertXefg = "ERR\t!XeSS-FG failed to start: no frame generation\n";

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

	// Panel rows (fg_overlay format: "label<TAB>value", '!' marks a warning).
	// `previous` is the caller's dispatch count at its last rebuild, `seconds`
	// the time since then.
	inline std::string StatusLines(bool enabled, double seconds, std::uint64_t& previous)
	{
		const auto n = g.dispatched.load(std::memory_order_relaxed);
		const double rate = seconds > 0 && n >= previous ? static_cast<double>(n - previous) / seconds : 0.0;
		previous = n;
		const char* pathBNote = xefg::PathBQualityNote();
		if (!enabled) return std::string("SR\t!off (setting)\n") + (pathBNote ? pathBNote : "");
		static constexpr const char* kQuality[]{ "Native AA", "Quality", "Balanced", "Performance" };
		const int q = g.quality.load(std::memory_order_relaxed);
		std::string backend;
		{
			std::lock_guard lock(g.m);
			backend = g.backend;
		}
		const double gpu = g.gpuMs.load(std::memory_order_relaxed);
		const auto failed = g.failed.load();
		std::string out = std::format("SR\t{}  {}\n\t{}x{} -> {}x{}\n",
			backend, q >= 0 && q < 4 ? kQuality[q] : "-",
			g.renderW.load(), g.renderH.load(), g.outputW.load(), g.outputH.load());
		if (const char* note = g.note.load(std::memory_order_relaxed)) out += note;
		if (pathBNote) out += pathBNote;
		if (n == 0 || (seconds > 0 && rate < 1.0))
			out += "\t!not dispatching\n";
		else
			out += std::format("\t{:.0f} runs/s   GPU {}   {}{}\n", rate,
				gpu >= 0 ? std::format("{:.1f} ms", gpu) : std::string("-"),
				g.reactive.load(std::memory_order_relaxed) ? "reactive mask" : "no mask",
				failed ? std::format("   failed {}", failed) : std::string());
		return out;
	}

	// Status rows followed by the active alerts (every panel goes through here).
	inline std::string Lines(bool enabled, double seconds, std::uint64_t& previous)
	{
		const std::string mode = xefg::OutputScaling() ? "Mode\tUI Native\n" : "Mode\tClassic\n";
		return mode + StatusLines(enabled, seconds, previous) + reshade_preui::PanelLine() + AlertLines(GetSettings().frameGenOverlay);
	}
}
