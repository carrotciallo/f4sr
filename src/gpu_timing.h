#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>

#include "F4SE/F4SE.h"
#include "sr_status.h"

namespace f4sr::gpu_timing
{
	// GPU time of the main stages of a frame on the game's immediate context,
	// from D3D11 timestamp queries read back a few frames later (no stall).
	// Logged as 5-second averages. A segment is counted only in frames where
	// both of its marks were issued.
	enum Mark : int
	{
		kFrameStart,      // UpdateTemporalData (render start)
		kPreImagespace,   // before SR
		kPostSr,          // SR done, game imagespace + ENB post follow
		kPostImagespace,  // RenderEffectRange returned
		kPreInterface,    // UI starts
		kPresent,         // frame handed to presentation (after ENB's Present work under XeSS-FG)
		kMarkCount
	};

	inline constexpr const char* kSegmentNames[kMarkCount - 1]{
		"scene", "SR", "imagespace+ENB", "post-imagespace->UI", "UI+present work"
	};

	struct Slot
	{
		Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
		Microsoft::WRL::ComPtr<ID3D11Query> stamps[kMarkCount];
		bool issued[kMarkCount]{};
		bool pending{};
	};

	struct State
	{
		static constexpr int kSlots = 16;  // XeSS-FG queues deep: results can lag many frames
		Slot slots[kSlots];
		int current{ -1 };  // slot of the frame being marked, -1 between frames
		int next{};         // slot the next frame starts in
		bool ready{}, failed{};
		ID3D11Device* device{};
		double sum[kMarkCount - 1]{}, total{};
		std::uint64_t count[kMarkCount - 1]{}, frames{};
		ULONGLONG lastLog{}, firstMark{};
		std::uint64_t ended{}, overwritten{}, disjoint{}, dropped{}, notReady{}, failedReads{};
		HRESULT firstFailure{ S_OK };
		bool reportedNone{};
	};
	inline State g;

	inline bool Ensure(ID3D11DeviceContext* ctx)
	{
		if (g.ready || g.failed || !ctx) return g.ready;
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		if (!device) return false;
		D3D11_QUERY_DESC dj{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts{ D3D11_QUERY_TIMESTAMP, 0 };
		for (auto& s : g.slots) {
			if (FAILED(device->CreateQuery(&dj, &s.disjoint))) { g.failed = true; break; }
			for (auto& q : s.stamps)
				if (FAILED(device->CreateQuery(&ts, &q))) { g.failed = true; break; }
		}
		if (g.failed) {
			F4SE::log::warn("f4sr: GPU timing unavailable (query creation failed)");
			return false;
		}
		g.device = device.Get();
		g.ready = true;
		return true;
	}

	inline void Collect(ID3D11DeviceContext* ctx)
	{
		for (auto& s : g.slots) {
			if (!s.pending) continue;
			// Flags 0 (not DONOTFLUSH): under XeSS-FG the game Present is skipped,
			// so nothing else guarantees the queries are submitted.
			auto read = [](ID3D11DeviceContext* c, ID3D11Query* q, void* out, UINT size) {
				const HRESULT hr = c->GetData(q, out, size, 0);
				if (hr == S_FALSE) ++g.notReady;
				else if (FAILED(hr)) {
					++g.failedReads;
					if (g.firstFailure == S_OK) g.firstFailure = hr;
				}
				return hr == S_OK;
			};
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
			if (!read(ctx, s.disjoint.Get(), &dj, sizeof(dj))) continue;
			UINT64 t[kMarkCount]{};
			bool ok = !dj.Disjoint && dj.Frequency != 0;
			for (int m = 0; ok && m < kMarkCount; ++m)
				if (s.issued[m] && !read(ctx, s.stamps[m].Get(), &t[m], sizeof(UINT64))) ok = false;
			if (!ok) {
				// Not ready yet: keep waiting unless the frame was disjoint.
				if (dj.Disjoint) { s.pending = false; ++g.disjoint; }
				continue;
			}
			s.pending = false;
			const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
			for (int m = 0; m + 1 < kMarkCount; ++m) {
				if (!s.issued[m] || !s.issued[m + 1] || t[m + 1] < t[m]) continue;
				g.sum[m] += static_cast<double>(t[m + 1] - t[m]) * toMs;
				++g.count[m];
			}
			if (s.issued[kFrameStart] && s.issued[kPresent] && t[kPresent] >= t[kFrameStart]) {
				g.total += static_cast<double>(t[kPresent] - t[kFrameStart]) * toMs;
				++g.frames;
			}
		}
		const auto now = GetTickCount64();
		if (!g.lastLog) g.lastLog = now;
		if (now - g.lastLog < 5000) return;
		if (g.frames == 0) {
			if (!g.reportedNone && g.ended > 0) {
				g.reportedNone = true;
				F4SE::log::warn("f4sr: GPU timing: no complete frame in 5 s (frames ended {}, slots overwritten unread {}, disjoint {}, frames without present {}, reads not ready {}, reads failed {} first {:#x})",
					g.ended, g.overwritten, g.disjoint, g.dropped, g.notReady, g.failedReads, static_cast<std::uint32_t>(g.firstFailure));
			}
			g.lastLog = now;
			return;
		}
		auto avg = [](double s, std::uint64_t n) { return n ? s / static_cast<double>(n) : 0.0; };
		F4SE::log::info("f4sr: GPU 5s ({} frames): frame start->present {:.2f} ms = {} {:.2f} + {} {:.2f} + {} {:.2f} + {} {:.2f} + {} {:.2f} ms",
			g.frames, avg(g.total, g.frames),
			kSegmentNames[0], avg(g.sum[0], g.count[0]), kSegmentNames[1], avg(g.sum[1], g.count[1]),
			kSegmentNames[2], avg(g.sum[2], g.count[2]), kSegmentNames[3], avg(g.sum[3], g.count[3]),
			kSegmentNames[4], avg(g.sum[4], g.count[4]));
		sr_status::g.gpuMs.store(avg(g.sum[kPreImagespace], g.count[kPreImagespace]), std::memory_order_relaxed);
		std::fill(std::begin(g.sum), std::end(g.sum), 0.0);
		std::fill(std::begin(g.count), std::end(g.count), 0ull);
		g.total = 0.0;
		g.frames = 0;
		g.lastLog = now;
	}

	// Render thread, on the game's immediate context.
	inline void MarkAt(ID3D11DeviceContext* ctx, Mark m)
	{
		if (!Ensure(ctx)) return;
		if (m == kFrameStart) {
			if (g.current >= 0) {
				// The previous frame never reached kPresent: drop it.
				auto& old = g.slots[g.current];
				ctx->End(old.disjoint.Get());
				old.pending = false;
				++g.dropped;
			}
			g.current = g.next;
			g.next = (g.next + 1) % State::kSlots;
			auto& s = g.slots[g.current];
			if (s.pending) {  // still unread after kSlots frames: reuse it
				s.pending = false;
				++g.overwritten;
			}
			std::fill(std::begin(s.issued), std::end(s.issued), false);
			ctx->Begin(s.disjoint.Get());
		}
		if (g.current < 0) return;
		auto& s = g.slots[g.current];
		if (s.issued[m]) return;
		ctx->End(s.stamps[m].Get());
		s.issued[m] = true;
		if (m == kPresent) {
			ctx->End(s.disjoint.Get());
			s.pending = true;
			++g.ended;
			g.current = -1;
			Collect(ctx);
		}
	}
}
