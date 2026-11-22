// FramePipeline diagnostics: probe-only frames, the three-point frame dump,
// periodic input statistics and the composite-pass probe. All of it runs only
// with bDiagnostics / bProbeOnly (or the proxy-driving probe points).
#include "frame_pipeline.h"
#include "frame_pipeline_detail.h"
#include "capture.h"
#include "late_draw_trace.h"
#include "frame_trace.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "settings.h"

namespace f4sr
{
	extern std::atomic<int> g_dumpStage;
	using detail::CaptureFrameId;
	using detail::CaptureGameplay;
	using detail::DumpRawToFile;

	void FramePipeline::ProbeFrame(RE::BSGraphics::RendererData* data, ID3D11DeviceContext* ctx)
	{
		static std::uint32_t frame = 0;
		++frame;
		if (frame == 1 || frame == 1500 || frame == 4500) {
			const auto mgr = RE::BSGraphics::RenderTargetManager::GetSingleton();
			for (std::uint32_t i = 0; i < 24; ++i) {
				F4SE::log::info("f4sr: map rtID[{}]={}", i, mgr.renderTargetID[i]);
			}
			for (std::uint32_t i = 0; i < 12; ++i) {
				F4SE::log::info("f4sr: map dsID[{}]={}", i, mgr.depthStencilTargetID[i]);
			}
			for (std::uint32_t i = 0; i < 101; ++i) {
				auto* t = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[i].texture);
				if (t == nullptr) {
					continue;
				}
				D3D11_TEXTURE2D_DESC d{};
				t->GetDesc(&d);
				F4SE::log::info("f4sr: rt[{}] {}x{} fmt {}", i, d.Width, d.Height, static_cast<std::uint32_t>(d.Format));
			}
			for (std::uint32_t i = 0; i < 13; ++i) {
				auto* t = reinterpret_cast<ID3D11Texture2D*>(data->depthStencilTargets[i].texture);
				if (t == nullptr) {
					continue;
				}
				D3D11_TEXTURE2D_DESC d{};
				t->GetDesc(&d);
				F4SE::log::info("f4sr: ds[{}] {}x{} fmt {}", i, d.Width, d.Height, static_cast<std::uint32_t>(d.Format));
			}
			F4SE::log::info("f4sr: probe snapshot at frame {}", frame);
		}
		// Rotation of one-frame full-target clears over every scene-
		// format candidate, each with its own color, through RTVs
		// created here from the textures themselves (the struct's
		// rtView field is not trusted). Whichever color flashes on
		// screen is the target the chain actually displays.
		const std::uint32_t phase = frame % 600;
		const bool winA = phase < 10;
		const bool winB = phase >= 300 && phase < 310;
		const bool winC = phase >= 450 && phase < 460;
		if (winA || winB || winC) {
			struct Cand { std::uint32_t slot; bool copy; const char* name; float rgba[4]; };
			static const Cand cands[] = {
				{ 2, false, "BLACK rt2", { 0.0f, 0.0f, 0.0f, 1.0f } },
				{ 4, false, "WHITE rt4", { 1.0f, 1.0f, 1.0f, 1.0f } },
				{ 3, true, "BLUE rt3-copy", { 0.0f, 0.0f, 1.0f, 1.0f } },
			};
			const auto& cand = cands[winA ? 0 : (winB ? 1 : 2)];
			auto* tex = reinterpret_cast<ID3D11Texture2D*>(cand.copy ? data->renderTargets[cand.slot].copyTexture : data->renderTargets[cand.slot].texture);
			if (tex != nullptr) {
				static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[101];
				static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> copyRtv3;
				auto& slot = cand.copy ? copyRtv3 : rtvs[cand.slot];
				if (slot == nullptr) {
					auto* dev = reinterpret_cast<ID3D11Device*>(data->device);
					dev->CreateRenderTargetView(tex, nullptr, &slot);
				}
				if (slot != nullptr) {
					ctx->ClearRenderTargetView(slot.Get(), cand.rgba);
					if (phase == 0 || phase == 300 || phase == 450) {
						F4SE::log::info("f4sr: probe clear {} sustained (frame {})", cand.name, frame);
					}
				}
			}
		}
	}

	// ---- Sub-native input diagnostics (temporary) ----------------
	// On diag frames, read back the render region of the captured
	// inputs and the output and log plain statistics. The motion
	// vector magnitudes identify the units the game writes at
	// sub-native ratios (UV ~1e-3..1e-2 vs pixels ~1e0+), which
	// decides motionVectorScale; color/depth stats sanity-check
	// the region capture itself.
	namespace
	{
		float HalfToFloat(std::uint16_t h)
		{
			const std::uint32_t sign = (h >> 15) & 1;
			const std::uint32_t exp = (h >> 10) & 0x1F;
			const std::uint32_t frac = h & 0x3FF;
			float out;
			if (exp == 0) {
				out = frac * 5.9604644775390625e-8f;  // 2^-24
			} else if (exp == 0x1F) {
				out = frac != 0 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
			} else {
				const std::uint32_t bits = (sign << 31) | ((exp - 15 + 127) << 23) | (frac << 13);
				std::memcpy(&out, &bits, sizeof(out));
				return out;
			}
			return sign != 0 ? -out : out;
		}
	}


	// ---- Three-point frame dump (temporary diagnostics) --------
	// A: rt<colorSlot> right after our writeback. B: the same
	// target after the imagespace chain ran. C: the backbuffer at
	// the next Present (what ENB produced). Raw packed pixels to
	// the game root; decoded offline.
	// (g_dumpStage is defined near the top of this file.)


	bool FramePipeline::DumpPointA(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex)
	{
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		if (device == nullptr || !EnsureDiagStaging(device.Get(), d.Width, d.Height)) {
			return false;
		}
		return DumpRawToFile(ctx, tex, diagColor_.Get(), "f4sr-dump-A-publish.raw");
	}

	bool FramePipeline::DumpPointB()
	{
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (data == nullptr || data->context == nullptr) {
			return false;
		}
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		const auto& target = data->renderTargets[slotMain_];
		if (target.texture == nullptr) {
			return false;
		}
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		auto* tex = reinterpret_cast<ID3D11Texture2D*>(target.texture);
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		if (device == nullptr || !EnsureDiagStaging(device.Get(), d.Width, d.Height)) {
			return false;
		}
		return DumpRawToFile(ctx, tex, diagColor_.Get(), "f4sr-dump-B-postchain.raw");
	}

	bool FramePipeline::EnsureDiagStaging(ID3D11Device* device, std::uint32_t dispW, std::uint32_t dispH)
	{
		if (diagReady_) {
			return true;
		}
		auto mk = [&](DXGI_FORMAT fmt, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out) {
			D3D11_TEXTURE2D_DESC d{};
			d.Width = dispW;
			d.Height = dispH;
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Format = fmt;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_STAGING;
			d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &out));
		};
		diagReady_ = mk(DXGI_FORMAT_R11G11B10_FLOAT, diagColor_) &&
			mk(DXGI_FORMAT_R16G16_FLOAT, diagMv_) &&
			mk(DXGI_FORMAT_R32_FLOAT, diagDepth_);
		if (!diagReady_) {
			F4SE::log::warn("f4sr: diag staging creation failed");
		}
		return diagReady_;
	}

	void FramePipeline::LogDiagStats(ID3D11DeviceContext* ctx)
	{
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		D3D11_TEXTURE2D_DESC od{};
		outputShared_.tex11->GetDesc(&od);
		const std::uint32_t dispW = od.Width, dispH = od.Height;
		if (device == nullptr || dispW == 0 || !EnsureDiagStaging(device.Get(), dispW, dispH)) {
			return;
		}
		const std::uint32_t rw = renderW_, rh = renderH_;
		if (rw == 0 || rh == 0) {
			return;
		}
		D3D11_BOX box{ 0, 0, 0, rw, rh, 1 };
		auto grab = [&](ID3D11Texture2D* src, ID3D11Texture2D* stage, auto&& scan) {
			ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, src, 0, &box);
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map))) {
				scan(static_cast<const std::byte*>(map.pData), map.RowPitch);
				ctx->Unmap(stage, 0);
			}
		};
		// Color input (R11G11B10 packed): channel means prove the
		// region holds a live scene.
		grab(colorShared_.tex11.Get(), diagColor_.Get(), [&](const std::byte* base, std::uint32_t pitch) {
			double sr = 0, sg = 0, sb = 0;
			std::uint64_t n = 0;
			for (std::uint32_t y = 0; y < rh; y += 16) {
				const auto* row = reinterpret_cast<const std::uint32_t*>(base + static_cast<size_t>(y) * pitch);
				for (std::uint32_t x = 0; x < rw; x += 16) {
					const std::uint32_t v = row[x];
					sr += v & 0x7FF;
					sg += (v >> 11) & 0x7FF;
					sb += (v >> 22) & 0x3FF;
					++n;
				}
			}
			if (n != 0) {
				F4SE::log::info("f4sr: diag color-in mean r={:.1f} g={:.1f} b={:.1f} (region {}x{})",
					sr / n, sg / n, sb / n, rw, rh);
			}
		});
		// Motion vectors (R16G16 half floats): magnitude census.
		grab(mvShared_.tex11.Get(), diagMv_.Get(), [&](const std::byte* base, std::uint32_t pitch) {
			double sumAbs = 0;
			float maxAbs = 0;
			std::uint64_t n = 0, gt001 = 0, gt01 = 0, gt1 = 0;
			for (std::uint32_t y = 0; y < rh; y += 8) {
				const auto* row = reinterpret_cast<const std::uint16_t*>(base + static_cast<size_t>(y) * pitch);
				for (std::uint32_t x = 0; x < rw; x += 8) {
					const float fx = HalfToFloat(row[x * 2]);
					const float fy = HalfToFloat(row[x * 2 + 1]);
					const float m = std::sqrt(fx * fx + fy * fy);
					sumAbs += m;
					maxAbs = (std::max)(maxAbs, m);
					gt001 += m > 0.001f ? 1 : 0;
					gt01 += m > 0.01f ? 1 : 0;
					gt1 += m > 0.1f ? 1 : 0;
					++n;
				}
			}
			if (n != 0) {
				F4SE::log::info("f4sr: diag mv meanAbs={:.5f} maxAbs={:.3f} frac>0.001={:.2f} >0.01={:.2f} >0.1={:.2f}",
					sumAbs / n, maxAbs, static_cast<double>(gt001) / n, static_cast<double>(gt01) / n, static_cast<double>(gt1) / n);
			}
		});
		// Depth input (R32_FLOAT after conversion): scene depth
		// should sit in (0,1), sky at 1.
		grab(depthShared_.tex11.Get(), diagDepth_.Get(), [&](const std::byte* base, std::uint32_t pitch) {
			double sum = 0;
			float mn = 1e9f, mx = -1e9f;
			std::uint64_t n = 0, atOne = 0;
			for (std::uint32_t y = 0; y < rh; y += 16) {
				const auto* row = reinterpret_cast<const float*>(base + static_cast<size_t>(y) * pitch);
				for (std::uint32_t x = 0; x < rw; x += 16) {
					const float v = row[x];
					sum += v;
					mn = (std::min)(mn, v);
					mx = (std::max)(mx, v);
					atOne += v >= 0.9999f ? 1 : 0;
					++n;
				}
			}
			if (n != 0) {
				F4SE::log::info("f4sr: diag depth mean={:.4f} min={:.4f} max={:.4f} fracSky={:.2f}",
					sum / n, mn, mx, static_cast<double>(atOne) / n);
			}
		});
		// Output (R11G11B10): the upscaled full frame.
		{
			D3D11_BOX full{ 0, 0, 0, dispW, dispH, 1 };
			ctx->CopySubresourceRegion(diagColor_.Get(), 0, 0, 0, 0, outputShared_.tex11.Get(), 0, &full);
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(ctx->Map(diagColor_.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				const auto* base = static_cast<const std::byte*>(map.pData);
				double sr = 0, sg = 0, sb = 0;
				std::uint64_t n = 0;
				for (std::uint32_t y = 0; y < dispH; y += 16) {
					const auto* row = reinterpret_cast<const std::uint32_t*>(base + static_cast<size_t>(y) * map.RowPitch);
					for (std::uint32_t x = 0; x < dispW; x += 16) {
						const std::uint32_t v = row[x];
						sr += v & 0x7FF;
						sg += (v >> 11) & 0x7FF;
						sb += (v >> 22) & 0x3FF;
						++n;
					}
				}
				ctx->Unmap(diagColor_.Get(), 0);
				if (n != 0) {
					F4SE::log::info("f4sr: diag color-out mean r={:.1f} g={:.1f} b={:.1f}",
						sr / n, sg / n, sb / n);
				}
			}
		}
	}


	void FramePipeline::CompositeProbe(int point)
	{
		if (point == 5 || point == 0 || point == 4) late_trace::End();
		auto& c = compositeCapture_;
		if (c.complete || (!c.active && point != 0)) return;
		const auto id = CaptureFrameId();
		if (point == 0) {
			if (!GetSettings().diagnostics || !c.canBegin(id) || !proxyDrive_ || !resolveCount_ || !CaptureGameplay()) return;
			if (c.active) F4SE::log::warn("f4sr: incomplete capture frame={} progress={}", c.frame, c.next);
			c.begin(id);
			F4SE::log::info("f4sr: capture start frame={} resolve={} attempt={}/{}", id, resolveCount_, c.attempts, c.maxAttempts);
		}
		if (!c.active || c.frame != id) return;
		const bool optional = point == 1 || point == 6 || point == 7;
		if (!optional && !c.expects(id, point)) {
			F4SE::log::warn("f4sr: rejecting capture frame={} unexpected CMP{} progress={}", id, point, c.next);
			c.active = false;
			return;
		}
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (data == nullptr || data->context == nullptr) {
			return;
		}
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		if (device == nullptr) {
			return;
		}
		bool written = true;
		auto dump = [&](void* texPtr, const char* name) {
			auto* tex = static_cast<ID3D11Texture2D*>(texPtr);
			if (tex == nullptr) {
				written = false;
				F4SE::log::info("f4sr: composite probe {} skipped (null texture)", name);
				return;
			}
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			D3D11_TEXTURE2D_DESC sd{};
			sd.Width = d.Width;
			sd.Height = d.Height;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.Format = d.Format;
			sd.SampleDesc.Count = 1;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
			if (FAILED(device->CreateTexture2D(&sd, nullptr, stage.GetAddressOf()))) {
				written = false;
				F4SE::log::error("f4sr: composite probe {} staging failed", name);
				return;
			}
			if (!DumpRawToFile(ctx, tex, stage.Get(), name)) written = false;
		};
		switch (point) {
		case 0:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP0-main.raw");
			dump(proxies_.DebugSlotTexture(logical::kMainCopy), "f4sr-dump-CMP0-maincopy.raw");
			break;
		case 1:
			dump(proxies_.DebugProxyTexture(logical::kMainCopy), "f4sr-dump-CMP1-proxycopy.raw");
			dump(proxies_.DebugProxyTexture(logical::kGBufferNormal), "f4sr-dump-CMP1-proxynormal.raw");
			break;
		case 2:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP2-slotmain.raw");
			dump(proxies_.DebugOriginalTexture(logical::kMain), "f4sr-dump-CMP2-origmain.raw");
			break;
		case 3:
			// Copyback now happens at the composite return, BEFORE Forward.
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP3-main.raw");
			break;
		case 4:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP4-main-seam.raw");
			break;
		case 5:
			// Before Render_UI itself, not merely before its effect-range call.
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP5-main-ui-entry.raw");
			break;
		case 6:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP6-before-late-composite.raw");
			break;
		case 7:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP7-after-late-composite.raw");
			break;
		case 8:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP8-deferred-composite-end.raw");
			break;
		case 9:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP9-forward-end.raw");
			break;
		case 10:
			dump(proxies_.DebugSlotTexture(logical::kMain), "f4sr-dump-CMP10-post-forward-end.raw");
			if (written) late_trace::Begin(ctx, static_cast<ID3D11Texture2D*>(proxies_.DebugSlotTexture(logical::kMain)), id, CaptureSessionTick());
			break;
		default:
			return;
		}
		if (!optional) {
			if (written && CaptureFrameId() == id) ++c.next;
			else {
				F4SE::log::warn("f4sr: incomplete capture frame={} failed CMP{} attempt={}", id, point, c.attempts);
				c.active = false;
			}
		}
	}

}
