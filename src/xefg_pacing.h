#pragma once

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

#include "F4SE/F4SE.h"
#include "xefg_unlock.h"

namespace f4sr::xefg_pacing
{
	// Flip-queue pacing for XeSS-FG at 2X and above (libxess_fg.dll 1.3.1.78 only;
	// the sites are verified by xefg_unlock::Apply before this installs).
	//
	// The provider presents a burst's generated frames back to back from the
	// loop at 0x2202E8 and the burst's last one from 0x220462, both through the
	// 16-byte thunk at 0x25C0 (jmp 0x21F730 + int3 padding). Above 2X that
	// reaches the flip queue as a clump followed by a gap. The thunk is
	// redirected here (jmp [rip+0] + 8-byte address, no relocation needed); the
	// two generated-frame call sites and the burst's real frame (site D),
	// identified by return address, are held to one wall-clock chain with a
	// spacing of 90 % of the median real frame period divided by the
	// multiplier, and every present is forwarded to the native 0x21F730.
	//
	// Evidence: every 5 s the presented gap statistics are logged, and in
	// short windows every present through the thunk is logged with its call
	// site and in-burst index, so the order real:generated is visible.
	using PresentFn = std::int64_t (*)(void*, std::uint32_t, std::uint32_t, std::uint64_t, void*, void*, std::uint64_t);

	inline constexpr std::int64_t kHeadroomPct = 90;
	inline constexpr int kSamples = 15;
	inline constexpr std::uint32_t kOtherSiteA = 0x220A57; // return of call at 0x220A52
	inline constexpr std::uint32_t kOtherSiteD = 0x220DB2; // return of call at 0x220DAD

	struct State
	{
		std::uint8_t* base{};
		PresentFn native{};
		bool installed{};
		std::atomic<bool> enabled{ false };
		LARGE_INTEGER freq{};
		// Present thread only.
		std::int64_t lastBurst{}, periodNs{}, intervalQpc{}, target{}, blockQpc{};
		std::int64_t samples[kSamples]{};
		int sampleCount{}, samplePos{};
		std::int64_t lastPaced{}, gapSum{}, gapMin{}, gapMax{}, statsDeadline{};
		int gapCount{};
		std::int64_t multiplier{}, pacedFrames{}, clamped{}, lastPaced5s{};
		bool burstPaced{};  // generated frames of the current burst were paced; its real frame follows
		bool loggedFirst{};
		std::atomic<std::int64_t> renderTimeNs{ 0 };
		// Per-present trace windows.
		// site 0 = the application's own Present of real frame `frame` on the
		// proxy chain (game thread); other sites are provider presents.
		struct Entry { std::int64_t qpc; std::uint32_t site; std::uint16_t index, count; std::uint32_t waitUs, frame; };
		std::atomic_flag traceLock{};
		Entry trace[256]{};
		int traceLen{}, traceWindows{};
		std::int64_t traceArmAt{};
		std::uint64_t presents{};
	};
	inline State g;

	inline std::int64_t NsFromQpc(std::int64_t d) { return g.freq.QuadPart ? d * 1000000000LL / g.freq.QuadPart : 0; }
	inline std::int64_t QpcFromNs(std::int64_t ns) { return g.freq.QuadPart ? ns * g.freq.QuadPart / 1000000000LL : 0; }
	inline double Ms(std::int64_t qpc) { return g.freq.QuadPart ? qpc * 1000.0 / g.freq.QuadPart : 0.0; }
	inline std::int64_t Now() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }

	// Milliseconds of the measured real frame not spent blocked in pacing, or
	// 0 before a burst has been measured. Usable as frameRenderTime.
	inline double RenderTimeMs() { return g.renderTimeNs.load(std::memory_order_relaxed) / 1e6; }

	inline void ReportGaps()
	{
		if (g.gapCount <= 0) return;
		F4SE::log::info("f4sr: XeFG pacing {}X: real frame {:.2f} ms ({:.1f} fps), target {:.2f} ms/frame; presented gap {:.2f} avg / {:.2f} min / {:.2f} max ms over {} paced frames, {} clamped",
			g.multiplier, g.periodNs / 1e6, g.periodNs > 0 ? 1e9 / g.periodNs : 0.0, Ms(g.intervalQpc),
			Ms(g.gapSum / g.gapCount), Ms(g.gapMin), Ms(g.gapMax), g.gapCount, g.clamped);
	}

	inline void RecordGap(std::int64_t end)
	{
		if (g.lastPaced) {
			const auto gap = end - g.lastPaced;
			g.gapSum += gap;
			if (!g.gapCount || gap < g.gapMin) g.gapMin = gap;
			g.gapMax = (std::max)(g.gapMax, gap);
			++g.gapCount;
		}
		g.lastPaced = end;
		if (!g.statsDeadline || end >= g.statsDeadline) {
			if (g.statsDeadline) ReportGaps();
			g.gapSum = g.gapMin = g.gapMax = g.clamped = 0;
			g.gapCount = 0;
			g.statsDeadline = end + QpcFromNs(5000000000LL);
		}
	}

	inline void PushPeriod(std::int64_t ns)
	{
		if (ns <= 0) return;
		g.samples[g.samplePos] = ns;
		g.samplePos = (g.samplePos + 1) % kSamples;
		g.sampleCount = (std::min)(g.sampleCount + 1, kSamples);
		std::int64_t s[kSamples];
		std::memcpy(s, g.samples, sizeof(std::int64_t) * g.sampleCount);
		std::sort(s, s + g.sampleCount);
		g.periodNs = s[g.sampleCount / 2];
	}

	// index 1 opens a burst: the time between burst openings is one real frame.
	inline void NoteBurst(std::uint64_t count, std::int64_t now)
	{
		if (g.lastBurst) PushPeriod(NsFromQpc(now - g.lastBurst));
		if (g.periodNs > 0) {
			const auto block = NsFromQpc(g.blockQpc);
			g.renderTimeNs.store(g.periodNs > block ? g.periodNs - block : 0, std::memory_order_relaxed);
			g.intervalQpc = QpcFromNs(g.periodNs * kHeadroomPct / 100 / (static_cast<std::int64_t>(count) + 1));
		}
		g.blockQpc = 0;
		g.lastBurst = now;
	}

	inline void WaitUntil(std::int64_t target)
	{
		const auto yieldBelow = QpcFromNs(200000);
		for (auto now = Now(); now < target; now = Now()) {
			if (target - now > yieldBelow) Sleep(0);
			else YieldProcessor();
		}
	}

	// Returns the microseconds waited.
	inline std::uint32_t PaceFrame(std::uint64_t index, std::uint64_t count)
	{
		g.multiplier = static_cast<std::int64_t>(count) + 1;
		// 2X is paced too: the provider does not space it (2026-11-22 trace:
		// real frame 0.09 ms after the generated one in 83/83 bursts).
		const auto now = Now();
		if (index <= 1) {
			NoteBurst(count, now);
			if (g.periodNs <= 0 || g.intervalQpc <= 0) return 0;
			// One chain across bursts: the first generated frame follows the
			// previous real frame's slot by one interval (now, if already late).
			g.target = (std::max)(now, g.target + g.intervalQpc);
			if (!g.loggedFirst) {
				g.loggedFirst = true;
				F4SE::log::info("f4sr: XeFG pacing active at {}X: real frame {:.3f} ms -> {:.3f} ms per presented frame",
					g.multiplier, g.periodNs / 1e6, Ms(g.intervalQpc));
			}
		} else {
			if (g.intervalQpc <= 0) return 0;
			g.target += g.intervalQpc;
		}
		// A whole frame behind after a hitch: drop the catch-up, do not stall.
		const auto latest = now + QpcFromNs(g.periodNs);
		if (g.target > latest) { g.target = latest; ++g.clamped; }
		WaitUntil(g.target);
		const auto end = Now();
		g.blockQpc += end - now;
		++g.pacedFrames;
		RecordGap(end);
		return static_cast<std::uint32_t>(NsFromQpc(end - now) / 1000);
	}

	// The burst's real frame (provider site D, after gen-last): one more
	// interval on the same chain, so the N+1 presents of a real frame are
	// evenly spaced instead of the real frame trailing gen-last by < 1 ms.
	inline std::uint32_t PaceReal()
	{
		if (!g.burstPaced || g.intervalQpc <= 0) return 0;
		g.burstPaced = false;
		const auto now = Now();
		g.target += g.intervalQpc;
		const auto latest = now + QpcFromNs(g.periodNs);
		if (g.target > latest) { g.target = latest; ++g.clamped; }
		if (g.target < now) g.target = now;
		WaitUntil(g.target);
		const auto end = Now();
		g.blockQpc += end - now;
		++g.pacedFrames;
		RecordGap(end);
		return static_cast<std::uint32_t>(NsFromQpc(end - now) / 1000);
	}

	// Last frame: the provider's own limiter holds it when ctx[0x340] is set,
	// the burst's field 0x28 is 0 and count > 2; leave it to the provider then.
	inline bool ProviderPacesLast(const std::uint8_t* ctx, const std::uint8_t* burst, std::uint64_t count)
	{
		return count > 2 && ctx[0x340] != 0 && *reinterpret_cast<const std::uint32_t*>(burst + 0x28) == 0;
	}

	inline void Trace(std::uint32_t site, std::uint64_t index, std::uint64_t count, std::uint32_t waitUs, std::uint32_t frame = 0)
	{
		const auto now = Now();
		while (g.traceLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
		struct Unlock { ~Unlock() { g.traceLock.clear(std::memory_order_release); } } unlock;
		++g.presents;
		if (!g.traceArmAt) g.traceArmAt = now + QpcFromNs(10000000000LL); // first window 10 s in
		if (g.traceLen == 0 && now < g.traceArmAt) return;
		g.trace[g.traceLen++] = { now, site, static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(count), waitUs, frame };
		if (g.traceLen < static_cast<int>(std::size(g.trace))) return;
		// Window full: write it out (one hitch on this thread, once per minute).
		++g.traceWindows;
		F4SE::log::info("f4sr: XeFG present trace window {} ({} entries; app-submit = real frame handed to the proxy chain, gen = provider present of generated frame k of N in the burst, other = provider present of a non-generated frame):",
			g.traceWindows, g.traceLen);
		for (int i = 0; i < g.traceLen; ++i) {
			const auto& e = g.trace[i];
			const auto dt = i ? Ms(e.qpc - g.trace[i - 1].qpc) : 0.0;
			if (e.site == 0) {
				F4SE::log::info("f4sr: XeFG present #{} +{:.3f} ms app-submit frame {}", i, dt, e.frame);
				continue;
			}
			const char* kind = e.site == xefg_unlock::kPacedLoopReturn ? "gen-loop" :
				e.site == xefg_unlock::kPacedLastReturn ? "gen-last" :
				e.site == kOtherSiteA ? "other-A" : e.site == kOtherSiteD ? "other-D" : "other";
			if (e.count)
				F4SE::log::info("f4sr: XeFG present #{} +{:.3f} ms {} {}/{} site=0x{:X} paceWait={}us", i, dt, kind, e.index, e.count, e.site, e.waitUs);
			else
				F4SE::log::info("f4sr: XeFG present #{} +{:.3f} ms {} site=0x{:X} paceWait={}us", i, dt, kind, e.site, e.waitUs);
		}
		g.traceLen = 0;
		g.traceArmAt = now + QpcFromNs(60000000000LL);
	}

	// Game thread: stamps the real frame's Present on the proxy chain into
	// the same trace, as the anchor for the provider presents that follow.
	inline void NoteAppSubmit(std::uint32_t frame)
	{
		if (g.installed) Trace(0, 0, 0, 0, frame);
	}

	inline std::int64_t Detour(void* ctx, std::uint32_t a2, std::uint32_t a3, std::uint64_t a4, void* arg5, void* arg6, std::uint64_t arg7)
	{
		const auto ret = static_cast<std::uint32_t>(reinterpret_cast<std::uint8_t*>(_ReturnAddress()) - g.base);
		std::uint64_t index = 0, count = 0;
		std::uint32_t waited = 0;
		const bool loop = ret == xefg_unlock::kPacedLoopReturn, last = ret == xefg_unlock::kPacedLastReturn;
		if ((loop || last) && arg5 && static_cast<std::uint8_t>(arg7) == (loop ? 1 : 0)) {
			auto* burst = static_cast<std::uint8_t*>(arg5) - 0x38;
			count = *reinterpret_cast<std::uint64_t*>(burst + 8);
			if (count >= 1 && count <= 5) {
				index = count;
				bool valid = true;
				if (loop) {
					auto* frames = *reinterpret_cast<std::uint8_t**>(burst);
					const auto* p = static_cast<std::uint8_t*>(arg6);
					valid = frames && p >= frames && (p - frames) % 8 == 0;
					index = valid ? static_cast<std::uint64_t>(p - frames) / 8 : 0;
					valid = valid && index >= 1 && index < count;
				}
				if (valid && g.enabled.load(std::memory_order_relaxed) &&
					!(last && ProviderPacesLast(static_cast<std::uint8_t*>(ctx), burst, count))) {
					waited = PaceFrame(index, count);
					if (last) g.burstPaced = g.intervalQpc > 0;
				}
				if (!valid) count = index = 0;
			} else {
				count = 0;
			}
		}
		if (ret == kOtherSiteD && g.enabled.load(std::memory_order_relaxed)) waited = PaceReal();
		else if (!loop && !last) g.burstPaced = false;
		Trace(ret, index, count, waited);
		return g.native(ctx, a2, a3, a4, arg5, arg6, arg7);
	}

	// Call only after xefg_unlock::Apply reported pacingSitesOk for `fg`.
	inline bool Install(HMODULE fg)
	{
		if (g.installed) return true;
		QueryPerformanceFrequency(&g.freq);
		g.base = reinterpret_cast<std::uint8_t*>(fg);
		g.native = reinterpret_cast<PresentFn>(g.base + xefg_unlock::kNativePresent);
		std::uint8_t jmp[14]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
		const auto target = reinterpret_cast<std::uint64_t>(&Detour);
		std::memcpy(jmp + 6, &target, 8);
		if (!xefg_unlock::Write(fg, xefg_unlock::kPresentThunk, jmp, sizeof(jmp))) {
			F4SE::log::warn("f4sr: XeFG pacing detour not installed: VirtualProtect failed ({})", GetLastError());
			return false;
		}
		g.installed = true;
		g.enabled.store(true);
		F4SE::log::info("f4sr: XeFG pacing detour installed at thunk RVA 0x{:X} -> native 0x{:X}; paced sites 0x{:X} (loop) 0x{:X} (last)",
			xefg_unlock::kPresentThunk, xefg_unlock::kNativePresent, xefg_unlock::kPacedLoopReturn, xefg_unlock::kPacedLastReturn);
		return true;
	}
}
