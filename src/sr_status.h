#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

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
		std::atomic<bool> reactive{};       // a reactive mask went with the last dispatch  // last 5 s average of the SR segment (gpu_timing)
	};
	inline State g;

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
	inline std::string Lines(bool enabled, double seconds, std::uint64_t& previous)
	{
		const auto n = g.dispatched.load(std::memory_order_relaxed);
		const double rate = seconds > 0 && n >= previous ? static_cast<double>(n - previous) / seconds : 0.0;
		previous = n;
		if (!enabled) return "SR\t!off (setting)\n";
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
		if (n == 0 || (seconds > 0 && rate < 1.0))
			out += "\t!not dispatching\n";
		else
			out += std::format("\t{:.0f} runs/s   GPU {}   {}{}\n", rate,
				gpu >= 0 ? std::format("{:.1f} ms", gpu) : std::string("-"),
				g.reactive.load(std::memory_order_relaxed) ? "reactive mask" : "no mask",
				failed ? std::format("   failed {}", failed) : std::string());
		return out;
	}
}
