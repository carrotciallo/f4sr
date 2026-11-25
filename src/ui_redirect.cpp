#include "ui_redirect.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <windows.h>
#include <wrl/client.h>

#include "F4SE/F4SE.h"

#include "output_scale.h"
#include "scaleform_scale.h"

using Microsoft::WRL::ComPtr;

namespace
{
	// ID3D11DeviceContext vtable slots.
	constexpr int kPSSetShaderResources = 8;
	constexpr int kOMSetRenderTargets = 33;
	constexpr int kOMSetRenderTargetsAndUAVs = 34;
	constexpr int kRSSetViewports = 44;
	constexpr int kRSSetScissorRects = 45;
	constexpr int kCopySubresourceRegion = 46;
	constexpr int kCopyResource = 47;
	constexpr int kClearRenderTargetView = 50;
	constexpr int kClearDepthStencilView = 53;

	using PSSetShaderResources_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
	using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
	using OMSetRenderTargetsAndUAVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
		UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
	using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
	using RSSetScissorRects_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
	using CopySubresourceRegion_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
	using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
	using ClearRenderTargetView_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
	using ClearDepthStencilView_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

	PSSetShaderResources_t g_psSrv{};
	OMSetRenderTargets_t g_omSet{};
	OMSetRenderTargetsAndUAVs_t g_omSetUav{};
	RSSetViewports_t g_rsVp{};
	RSSetScissorRects_t g_rsSc{};
	ClearRenderTargetView_t g_clearRtv{};
	CopySubresourceRegion_t g_copyRegion{};
	CopyResource_t g_copy{};
	f4sr::output_scale::Scaler g_copyScaler;
	ClearDepthStencilView_t g_clearDsv{};

	struct State
	{
		bool installed{};
		bool active{};
		bool redirected{};  // the canvas is currently bound in place of the frame buffer
		ID3D11Texture2D* frameBuffer{};
		ComPtr<ID3D11Texture2D> canvas;
		UINT fbW{}, fbH{}, cW{}, cH{};
		float sx{ 1.0f }, sy{ 1.0f };
		std::vector<std::pair<ID3D11RenderTargetView*, ComPtr<ID3D11RenderTargetView>>> rtvs;
		std::vector<std::pair<ID3D11DepthStencilView*, ComPtr<ID3D11DepthStencilView>>> dsvs;
		std::vector<std::pair<ID3D11Texture2D*, ComPtr<ID3D11Texture2D>>> depthTextures;
		D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT vpN{};
		D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT scN{};
		std::uint64_t copies{};
		bool inside{};  // f4sr's own draws while redirecting: pass through
		bool sceneUnder{};
		int mode{};             // see ViewMode
		bool tracking{};        // g.vp/g.sc follow every game call
		bool fbDirty{}; // the real frame buffer was drawn (mixed bind): carry it to the canvas
		std::uint64_t mixed{};
		// The game's own (unmapped) binding, restored at End.
		ID3D11DeviceContext* ctx{};
		ComPtr<ID3D11RenderTargetView> gameRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
		ComPtr<ID3D11DepthStencilView> gameDsv;
		UINT gameN{};
		// The game's UI texture (Scaleform draws the menus into it at the
		// render size, then composites it): replaced by a display-size twin.
		struct UiTexture
		{
			ID3D11Texture2D* game{};
			ComPtr<ID3D11Texture2D> twin;
			std::vector<std::pair<ID3D11RenderTargetView*, ComPtr<ID3D11RenderTargetView>>> rtvs;
			std::vector<std::pair<ID3D11ShaderResourceView*, ComPtr<ID3D11ShaderResourceView>>> srvs;
		};
		std::vector<UiTexture> ui;  // at most kMaxUi
		std::uint64_t uiReads{};
	} g;

	void Remember(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
	{
		g.gameN = n;
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g.gameRtv[i] = (rtvs && i < n) ? rtvs[i] : nullptr;
		g.gameDsv = dsv;
	}

	constexpr std::size_t kMaxUi = 8;

	// 1: the frame buffer (-> canvas), 2 + i: UI texture i (-> its twin), 0: other.
	int Promoted(ID3D11View* view)
	{
		if (!view) return 0;
		ComPtr<ID3D11Resource> r;
		view->GetResource(&r);
		if (g.frameBuffer && r.Get() == g.frameBuffer) return 1;
		for (std::size_t i = 0; i < g.ui.size(); ++i)
			if (r.Get() == static_cast<ID3D11Resource*>(g.ui[i].game)) return static_cast<int>(2 + i);
		return 0;
	}

	ID3D11RenderTargetView* UiRtv(int kind, ID3D11RenderTargetView* game)
	{
		auto& u = g.ui[kind - 2];
		for (auto& [k, v] : u.rtvs)
			if (k == game) return v.Get();
		D3D11_RENDER_TARGET_VIEW_DESC d{};
		game->GetDesc(&d);
		ComPtr<ID3D11Device> device;
		game->GetDevice(&device);
		ComPtr<ID3D11RenderTargetView> view;
		if (!device || FAILED(device->CreateRenderTargetView(u.twin.Get(), &d, &view))) {
			F4SE::log::warn("f4sr: UI redirect: UI twin view (format {}) failed", static_cast<unsigned>(d.Format));
			return nullptr;
		}
		u.rtvs.emplace_back(game, view);
		return view.Get();
	}

	ID3D11ShaderResourceView* UiSrv(int kind, ID3D11ShaderResourceView* game)
	{
		auto& u = g.ui[kind - 2];
		for (auto& [k, v] : u.srvs)
			if (k == game) return v.Get();
		D3D11_SHADER_RESOURCE_VIEW_DESC d{};
		game->GetDesc(&d);
		ComPtr<ID3D11Device> device;
		game->GetDevice(&device);
		ComPtr<ID3D11ShaderResourceView> view;
		if (!device || FAILED(device->CreateShaderResourceView(u.twin.Get(), &d, &view))) return nullptr;
		u.srvs.emplace_back(game, view);
		return view.Get();
	}

	ID3D11RenderTargetView* CanvasRtv(ID3D11RenderTargetView* game);

	ID3D11RenderTargetView* TwinRtv(int kind, ID3D11RenderTargetView* game)
	{
		return kind == 1 ? CanvasRtv(game) : kind >= 2 ? UiRtv(kind, game) : nullptr;
	}

	// The game texture behind a resource, if it is a UI texture.
	State::UiTexture* UiOf(ID3D11Resource* r)
	{
		for (auto& u : g.ui)
			if (r == static_cast<ID3D11Resource*>(u.game)) return &u;
		return nullptr;
	}

	// The texture bound now gets a display-size twin (see State::ui).
	bool PromoteUi(ID3D11DeviceContext* ctx)
	{
		if (g.ui.size() >= kMaxUi || g.gameN == 0 || !g.gameRtv[0] || Promoted(g.gameRtv[0].Get())) return false;
		ComPtr<ID3D11Resource> r;
		g.gameRtv[0]->GetResource(&r);
		ComPtr<ID3D11Texture2D> tex;
		if (!r || FAILED(r.As(&tex))) return false;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		if (d.Width != g.fbW || d.Height != g.fbH || d.SampleDesc.Count != 1 || d.ArraySize != 1) return false;
		D3D11_TEXTURE2D_DESC t = d;
		t.Width = g.cW;
		t.Height = g.cH;
		t.MipLevels = 1;
		t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		t.MiscFlags = 0;
		t.CPUAccessFlags = 0;
		t.Usage = D3D11_USAGE_DEFAULT;
		ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		State::UiTexture u;
		if (!device || FAILED(device->CreateTexture2D(&t, nullptr, &u.twin))) {
			F4SE::log::warn("f4sr: UI redirect: UI twin {}x{} format {} failed", g.cW, g.cH, static_cast<unsigned>(d.Format));
			return false;
		}
		u.game = tex.Get();
		g.ui.push_back(std::move(u));
		const int kind = static_cast<int>(1 + g.ui.size());
		if (auto* v = UiRtv(kind, g.gameRtv[0].Get())) {
			const FLOAT zero[4]{};
			g_clearRtv(ctx, v, zero);
		}
		F4SE::log::info("f4sr: UI redirect: UI texture #{} {:p} {}x{} format {} now drawn at {}x{}", g.ui.size(), static_cast<void*>(tex.Get()),
			d.Width, d.Height, static_cast<unsigned>(d.Format), g.cW, g.cH);
		return true;
	}

	ID3D11RenderTargetView* CanvasRtv(ID3D11RenderTargetView* game)
	{
		for (auto& [k, v] : g.rtvs)
			if (k == game) return v.Get();
		D3D11_RENDER_TARGET_VIEW_DESC d{};
		game->GetDesc(&d);
		ComPtr<ID3D11Device> device;
		game->GetDevice(&device);
		ComPtr<ID3D11RenderTargetView> view;
		if (!device || FAILED(device->CreateRenderTargetView(g.canvas.Get(), &d, &view))) {
			F4SE::log::warn("f4sr: UI redirect: canvas view (format {}) failed", static_cast<unsigned>(d.Format));
			return nullptr;
		}
		g.rtvs.emplace_back(game, view);
		return view.Get();
	}

	// The game's depth-stencil views at the frame buffer size get a
	// display-size twin (same format), used while the canvas is bound.
	ID3D11DepthStencilView* CanvasDsv(ID3D11DepthStencilView* game)
	{
		for (auto& [k, v] : g.dsvs)
			if (k == game) return v.Get();
		ComPtr<ID3D11Resource> r;
		game->GetResource(&r);
		ComPtr<ID3D11Texture2D> tex;
		if (!r || FAILED(r.As(&tex))) return nullptr;
		D3D11_TEXTURE2D_DESC td{};
		tex->GetDesc(&td);
		if (td.Width != g.fbW || td.Height != g.fbH) {
			g.dsvs.emplace_back(game, nullptr);  // another size: leave it
			return nullptr;
		}
		ComPtr<ID3D11Device> device;
		game->GetDevice(&device);
		ComPtr<ID3D11Texture2D> twin;
		for (auto& [k, v] : g.depthTextures)
			if (k == tex.Get()) twin = v;
		if (!twin) {
			D3D11_TEXTURE2D_DESC d = td;
			d.Width = g.cW;
			d.Height = g.cH;
			d.MipLevels = 1;
			d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			d.MiscFlags = 0;
			if (!device || FAILED(device->CreateTexture2D(&d, nullptr, &twin))) {
				F4SE::log::warn("f4sr: UI redirect: display-size depth target (format {}) failed", static_cast<unsigned>(td.Format));
				g.dsvs.emplace_back(game, nullptr);
				return nullptr;
			}
			g.depthTextures.emplace_back(tex.Get(), twin);
		}
		D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
		game->GetDesc(&dd);
		ComPtr<ID3D11DepthStencilView> view;
		if (FAILED(device->CreateDepthStencilView(twin.Get(), &dd, &view))) {
			g.dsvs.emplace_back(game, nullptr);
			return nullptr;
		}
		g.dsvs.emplace_back(game, view);
		return view.Get();
	}

	void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);

	// Size of the render target bound now (first target, else depth).
	bool BoundSize(ID3D11DeviceContext* ctx, UINT& w, UINT& h)
	{
		ID3D11RenderTargetView* rtv{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(1, &rtv, &dsv);
		ID3D11View* view = rtv ? static_cast<ID3D11View*>(rtv) : static_cast<ID3D11View*>(dsv);
		bool ok = false;
		if (view) {
			ComPtr<ID3D11Resource> r;
			view->GetResource(&r);
			ComPtr<ID3D11Texture2D> t;
			if (r && SUCCEEDED(r.As(&t))) {
				D3D11_TEXTURE2D_DESC d{};
				t->GetDesc(&d);
				w = d.Width;
				h = d.Height;
				ok = true;
			}
		}
		if (rtv) rtv->Release();
		if (dsv) dsv->Release();
		return ok;
	}

	// How the game's viewports map now: 0 as set, 1 render -> display size
	// (frame buffer redirected to the canvas), -1 display -> render size (a
	// display-size movie drawing into a render-size texture).
	int ViewMode(ID3D11DeviceContext* ctx)
	{
		if (!g.vpN || !g.fbW) return 0;
		const auto& v = g.vp[0];
		const bool big = v.Width > g.fbW + 0.5f || v.Height > g.fbH + 0.5f;
		if (g.active && g.redirected) return big ? 0 : 1;  // big: already display size
		if (!big) return 0;
		UINT bw{}, bh{};
		if (!BoundSize(ctx, bw, bh) || (v.Width <= bw + 0.5f && v.Height <= bh + 0.5f)) return 0;
		if (g.active && PromoteUi(ctx)) {
			// Bind the twin in its place; the viewport then fits as set.
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			for (UINT i = 0; i < g.gameN; ++i) rtvs[i] = g.gameRtv[i].Get();
			ComPtr<ID3D11DepthStencilView> dsv = g.gameDsv;
			OMSetRenderTargetsHook(ctx, g.gameN, rtvs, dsv.Get());
			return 0;
		}
		if (static bool logged = false; !logged) {
			logged = true;
			F4SE::log::info("f4sr: UI redirect: display-size viewport {:.0f}x{:.0f} on a {}x{} target; drawn at the target size",
				v.Width, v.Height, bw, bh);
		}
		return -1;
	}

	void ApplyViewports(ID3D11DeviceContext* ctx, bool viewports = true, bool scissors = true)
	{
		if (viewports) {
			g.mode = ViewMode(ctx);
			// Diagnostics (2026-11-25, Interface3D menu hitboxes): each new
			// combination of viewport mapping, game viewport and target.
			if (g.vpN) {
				UINT bw{}, bh{};
				BoundSize(ctx, bw, bh);
				const std::uint64_t key = (static_cast<std::uint64_t>(g.mode + 2) << 60) ^ (static_cast<std::uint64_t>(g.redirected) << 59) ^
					(static_cast<std::uint64_t>(g.vp[0].Width) << 44) ^ (static_cast<std::uint64_t>(g.vp[0].Height) << 30) ^
					(static_cast<std::uint64_t>(bw) << 15) ^ bh;
				// Seen combinations reset whenever a movie viewport changes
				// (a menu opened), so each menu gets its own sample.
				static std::vector<std::uint64_t> seen;
				static std::uint32_t epoch{};
				static int total{};
				if (const auto now = f4sr::scaleform_scale::DiagEpoch(); now != epoch) {
					epoch = now;
					seen.clear();
				}
				if (total < 200 && seen.size() < 24 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
					seen.push_back(key);
					++total;
					F4SE::log::info("f4sr: UI diag: redirect viewport mode {} redirected {} game viewport ({:.0f},{:.0f} {:.0f}x{:.0f}) on {}x{} target",
						g.mode, g.redirected, g.vp[0].TopLeftX, g.vp[0].TopLeftY, g.vp[0].Width, g.vp[0].Height, bw, bh);
				}
			}
		}
		const float fx = g.mode > 0 ? g.sx : g.mode < 0 ? 1.0f / g.sx : 1.0f;
		const float fy = g.mode > 0 ? g.sy : g.mode < 0 ? 1.0f / g.sy : 1.0f;
		if (viewports && g.vpN) {
			D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			for (UINT i = 0; i < g.vpN; ++i) {
				v[i] = g.vp[i];
				v[i].TopLeftX *= fx;
				v[i].TopLeftY *= fy;
				v[i].Width *= fx;
				v[i].Height *= fy;
			}
			g_rsVp(ctx, g.vpN, v);
		}
		if (scissors && g.scN) {
			D3D11_RECT r[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			for (UINT i = 0; i < g.scN; ++i) {
				r[i] = g.sc[i];
				if (g.mode != 0) {
					r[i].left = static_cast<LONG>(r[i].left * fx);
					r[i].top = static_cast<LONG>(r[i].top * fy);
					r[i].right = static_cast<LONG>(r[i].right * fx + 0.5f);
					r[i].bottom = static_cast<LONG>(r[i].bottom * fy + 0.5f);
				}
			}
			g_rsSc(ctx, g.scN, r);
		}
	}

	f4sr::output_scale::Scaler g_carryScaler;

	// Frame buffer content drawn at the render size goes under the canvas
	// before the next draw onto the canvas (or at End).
	void CarryFrameBuffer(ID3D11DeviceContext* ctx)
	{
		if (!g.fbDirty || !ctx || !g.frameBuffer || !g.canvas) return;
		g.fbDirty = false;
		g.inside = true;
		if (g.sceneUnder) g_carryScaler.ScaleChanged(ctx, g.frameBuffer, g.canvas.Get());
		else g_carryScaler.Scale(ctx, g.frameBuffer, g.canvas.Get());
		g.inside = false;
	}

	// Maps the render targets and depth view; returns whether the canvas is bound.
	bool Map(UINT n, ID3D11RenderTargetView* const* in, ID3D11DepthStencilView* dsv,
		ID3D11RenderTargetView** out, ID3D11DepthStencilView** dsvOut)
	{
		bool canvas = false;
		bool fb = false, other = false;
		int kind[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		for (UINT i = 0; i < n; ++i) {
			out[i] = in ? in[i] : nullptr;
			if (!out[i]) continue;
			kind[i] = Promoted(out[i]);
			if (kind[i]) fb = true;
			else other = true;
		}
		*dsvOut = dsv;
		if (fb && other) {
			// First mixed bind since the last carry: remember what the frame
			// buffer held, so only what this pass draws is carried.
			if (!g.fbDirty && g.ctx && g.sceneUnder && g.frameBuffer) {
				g.inside = true;
				g_carryScaler.Snapshot(g.ctx, g.frameBuffer);
				g.inside = false;
			}
			// The frame buffer together with another render-size target
			// (loading-screen model): sizes must match, so it draws to the
			// real frame buffer and is carried over later.
			g.fbDirty = true;
			if (++g.mixed == 1) F4SE::log::info("f4sr: UI redirect: frame buffer bound with other targets; drawn at the render size and carried to the canvas");
			return false;
		}
		if (!fb) return false;
		for (UINT i = 0; i < n; ++i) {
			if (!out[i] || !kind[i]) continue;
			if (auto* c = TwinRtv(kind[i], out[i])) {
				out[i] = c;
				canvas = true;
			}
		}
		if (canvas && dsv)
			if (auto* twin = CanvasDsv(dsv)) *dsvOut = twin;
		return canvas;
	}

	void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
	{
		if (!g.active || g.inside) return g_omSet(ctx, n, rtvs, dsv);
		ID3D11RenderTargetView* mapped[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* mappedDsv{};
		const UINT count = (std::min)(n, static_cast<UINT>(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT));
		Remember(count, rtvs, dsv);
		const bool canvas = Map(count, rtvs, dsv, mapped, &mappedDsv);
		if (canvas) CarryFrameBuffer(ctx);
		g_omSet(ctx, count, rtvs ? mapped : nullptr, mappedDsv);
		if (canvas != g.redirected) {
			g.redirected = canvas;
			ApplyViewports(ctx);
		}
	}

	void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVsHook(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
		UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
	{
		if (!g.active || g.inside || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
			return g_omSetUav(ctx, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
		ID3D11RenderTargetView* mapped[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* mappedDsv{};
		const UINT count = (std::min)(n, static_cast<UINT>(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT));
		Remember(count, rtvs, dsv);
		const bool canvas = Map(count, rtvs, dsv, mapped, &mappedDsv);
		if (canvas) CarryFrameBuffer(ctx);
		g_omSetUav(ctx, count, rtvs ? mapped : nullptr, mappedDsv, uavStart, uavCount, uavs, counts);
		if (canvas != g.redirected) {
			g.redirected = canvas;
			ApplyViewports(ctx);
		}
	}

	void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* v)
	{
		if (g.inside || !v) return g_rsVp(ctx, n, v);
		g.vpN = (std::min)(n, static_cast<UINT>(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
		for (UINT i = 0; i < g.vpN; ++i) g.vp[i] = v[i];
		// Outside the UI window only a display-size viewport needs a look.
		if (!g.active && !(g.vpN && (v[0].Width > g.fbW + 0.5f || v[0].Height > g.fbH + 0.5f))) {
			g.mode = 0;
			return g_rsVp(ctx, n, v);
		}
		ApplyViewports(ctx, true, false);
	}

	void STDMETHODCALLTYPE RSSetScissorRectsHook(ID3D11DeviceContext* ctx, UINT n, const D3D11_RECT* r)
	{
		if (g.inside || !r) return g_rsSc(ctx, n, r);
		g.scN = (std::min)(n, static_cast<UINT>(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
		for (UINT i = 0; i < g.scN; ++i) g.sc[i] = r[i];
		if (g.mode == 0) return g_rsSc(ctx, n, r);
		ApplyViewports(ctx, false, true);
	}

	void STDMETHODCALLTYPE ClearRenderTargetViewHook(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT color[4])
	{
		if (g.active && !g.inside && rtv)
			if (const int k = Promoted(rtv))
				if (auto* c = TwinRtv(k, rtv)) return g_clearRtv(ctx, c, color);
		g_clearRtv(ctx, rtv, color);
	}

	void STDMETHODCALLTYPE ClearDepthStencilViewHook(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
	{
		g_clearDsv(ctx, dsv, flags, depth, stencil);
		// The game may clear before binding the frame buffer: keep the
		// display-size twin in step whatever is bound.
		if (g.active && !g.inside && dsv)
			if (auto* twin = CanvasDsv(dsv)) g_clearDsv(ctx, twin, flags, depth, stencil);
	}

	// Copies into the frame buffer bypass the bind redirect: carry the
	// result over to the canvas.
	void CopiedToFrameBuffer(ID3D11DeviceContext* ctx, ID3D11Resource* dst)
	{
		if (!g.active || g.inside || !dst || dst != static_cast<ID3D11Resource*>(g.frameBuffer)) return;
		g.inside = true;
		g_copyScaler.Scale(ctx, g.frameBuffer, g.canvas.Get());
		g.inside = false;
		if (++g.copies == 1) F4SE::log::info("f4sr: UI redirect: copy into the frame buffer carried to the canvas");
	}

	void STDMETHODCALLTYPE CopyResourceHook(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
	{
		g_copy(ctx, dst, src);
		if (auto* u = g.active && !g.inside ? UiOf(src) : nullptr) {
			// The UI lives in its twin: copy from there.
			ComPtr<ID3D11Texture2D> d;
			if (dst && SUCCEEDED(dst->QueryInterface(IID_PPV_ARGS(&d)))) {
				g.inside = true;
				if (dst == static_cast<ID3D11Resource*>(g.frameBuffer)) g_copy(ctx, g.canvas.Get(), u->twin.Get());
				else if (auto* du = UiOf(dst)) g_copy(ctx, du->twin.Get(), u->twin.Get());
				else g_copyScaler.Scale(ctx, u->twin.Get(), d.Get());
				g.inside = false;
			}
			if (++g.copies <= 3) F4SE::log::info("f4sr: UI redirect: copy from the UI texture taken from its twin");
			return;
		}
		CopiedToFrameBuffer(ctx, dst);
	}

	void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* srvs)
	{
		if (!g.active || g.inside || g.ui.empty() || !srvs || n == 0 || n > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
			return g_psSrv(ctx, start, n, srvs);
		ID3D11ShaderResourceView* mapped[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
		bool any = false;
		for (UINT i = 0; i < n; ++i) {
			mapped[i] = srvs[i];
			if (!mapped[i]) continue;
			if (const int k = Promoted(mapped[i]); k >= 2)
				if (auto* t = UiSrv(k, mapped[i])) {
					mapped[i] = t;
					any = true;
				}
		}
		if (any && ++g.uiReads == 1) F4SE::log::info("f4sr: UI redirect: the UI texture is read from its display-size twin");
		g_psSrv(ctx, start, n, any ? mapped : srvs);
	}

	void STDMETHODCALLTYPE CopySubresourceRegionHook(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
		ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
	{
		g_copyRegion(ctx, dst, dstSub, x, y, z, src, srcSub, box);
		CopiedToFrameBuffer(ctx, dst);
	}

	template <class T>
	bool Patch(void** table, int slot, T hook, T& original)
	{
		DWORD old{};
		if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
		original = reinterpret_cast<T>(table[slot]);
		table[slot] = reinterpret_cast<void*>(hook);
		DWORD ignored{};
		VirtualProtect(&table[slot], sizeof(void*), old, &ignored);
		return true;
	}
}

namespace f4sr::ui_redirect
{
	bool Install(ID3D11DeviceContext* gameContext)
	{
		if (g.installed) return true;
		if (!gameContext) return false;
		auto** table = *reinterpret_cast<void***>(gameContext);
		const bool ok = Patch(table, kOMSetRenderTargets, &OMSetRenderTargetsHook, g_omSet) &&
			Patch(table, kOMSetRenderTargetsAndUAVs, &OMSetRenderTargetsAndUAVsHook, g_omSetUav) &&
			Patch(table, kRSSetViewports, &RSSetViewportsHook, g_rsVp) &&
			Patch(table, kRSSetScissorRects, &RSSetScissorRectsHook, g_rsSc) &&
			Patch(table, kClearRenderTargetView, &ClearRenderTargetViewHook, g_clearRtv) &&
			Patch(table, kCopyResource, &CopyResourceHook, g_copy) &&
			Patch(table, kCopySubresourceRegion, &CopySubresourceRegionHook, g_copyRegion) &&
			Patch(table, kClearDepthStencilView, &ClearDepthStencilViewHook, g_clearDsv) &&
			Patch(table, kPSSetShaderResources, &PSSetShaderResourcesHook, g_psSrv);
		g.installed = ok;
		F4SE::log::info("f4sr: UI redirect: context hooks {}", ok ? "installed" : "FAILED");
		return ok;
	}

	void Begin(ID3D11DeviceContext* ctx, ID3D11Texture2D* frameBuffer, ID3D11Texture2D* canvas, bool sceneUnder)
	{
		if (!g.installed || !ctx || !frameBuffer || !canvas) return;
		End();
		g.ctx = ctx;
		g.sceneUnder = sceneUnder;
		D3D11_TEXTURE2D_DESC fd{}, cd{};
		frameBuffer->GetDesc(&fd);
		canvas->GetDesc(&cd);
		if (g.canvas.Get() != canvas || g.frameBuffer != frameBuffer) {
			g.rtvs.clear();
			g.dsvs.clear();
			g.canvas = canvas;
			g.frameBuffer = frameBuffer;
		}
		g.fbW = fd.Width;
		g.fbH = fd.Height;
		g.cW = cd.Width;
		g.cH = cd.Height;
		g.sx = static_cast<float>(cd.Width) / static_cast<float>(fd.Width);
		g.sy = static_cast<float>(cd.Height) / static_cast<float>(fd.Height);
		// The hooks track what the game set; the context may hold mapped values.
		if (!g.tracking) {
			g.tracking = true;
			g.vpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ctx->RSGetViewports(&g.vpN, g.vp);
			g.scN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ctx->RSGetScissorRects(&g.scN, g.sc);
		}
		g.active = true;
		g.redirected = false;
		ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, &dsv);
		UINT n = 0;
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
			if (bound[i]) n = i + 1;
		OMSetRenderTargetsHook(ctx, n, bound, dsv);
		for (auto* v : bound) if (v) v->Release();
		if (dsv) dsv->Release();
	}

	void End()
	{
		if (!g.active) return;
		CarryFrameBuffer(g.ctx);
		g.active = false;
		if (g.redirected && g.ctx) {
			// Back to the game's own targets and viewports.
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			for (UINT i = 0; i < g.gameN; ++i) rtvs[i] = g.gameRtv[i].Get();
			g_omSet(g.ctx, g.gameN, rtvs, g.gameDsv.Get());
			g.redirected = false;
			ApplyViewports(g.ctx);
		}
		g.redirected = false;
		for (auto& v : g.gameRtv) v.Reset();
		g.gameDsv.Reset();
	}

	bool Active() { return g.active; }

	void ResetForResize()
	{
		End();
		const bool installed = g.installed;
		g = {};
		g.installed = installed;
		g_copyScaler = {};
	}

	ScopedUntracked::ScopedUntracked() : previous_(g.inside) { g.inside = true; }
	ScopedUntracked::~ScopedUntracked() { g.inside = previous_; }
}
