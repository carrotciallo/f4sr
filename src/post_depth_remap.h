#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <iterator>

#include "F4SE/F4SE.h"

namespace f4sr::post_depth
{
	// Between the sub-native resolve and the next frame start, renderer passes
	// may sample private copies of scene depth (ENB keeps one) that were
	// written before the seam, in render layout: valid in the top-left
	// render rectangle, clear value elsewhere. Depth-based effects after the
	// seam then work on half-scale depth inside that rectangle only.
	//
	// On the system context's draws in that window:
	//  - an R24G8-family display-size texture is refreshed from the engine
	//    depth, which is already in display layout;
	//  - a single-channel R32/R16 display-size texture that no draw has
	//    written since the seam is re-laid out in place (nearest sample of
	//    its own render rectangle, stretched to the display size), whatever
	//    it stores (raw or linear depth).
	// Each texture is handled at most once per frame.
	struct State
	{
		ID3D11Texture2D* source{};  // engine depth, already in display layout
		D3D11_TEXTURE2D_DESC desc{};
		UINT renderW{}, renderH{};
		ID3D11Resource* done[16]{};
		unsigned doneCount{};
		ID3D11Resource* written[48]{};
		unsigned writtenCount{};
		bool active{};
	};
	inline State g;

	struct Relayout
	{
		template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
		Ptr<ID3D11Device> device;
		Ptr<ID3D11ComputeShader> shader;
		Ptr<ID3D11Buffer> cb;
		UINT cbDims[4]{};
		Ptr<ID3D11Texture2D> src, dst;
		D3D11_TEXTURE2D_DESC desc{};
		DXGI_FORMAT viewFormat{};
		Ptr<ID3D11ShaderResourceView> srv;
		Ptr<ID3D11UnorderedAccessView> uav;
		bool failed{};
	};
	inline Relayout r;

	inline bool DepthFamily(DXGI_FORMAT f)
	{
		return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
			f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
	}

	// Single-channel formats used for depth copies; typeless group plus the
	// typed view format the relayout shader reads and writes through.
	inline bool SingleChannel(DXGI_FORMAT res, DXGI_FORMAT view, DXGI_FORMAT& group, DXGI_FORMAT& typed)
	{
		switch (res) {
		case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT:
			group = DXGI_FORMAT_R32_TYPELESS; typed = DXGI_FORMAT_R32_FLOAT; return true;
		case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM:
			group = DXGI_FORMAT_R16_TYPELESS;
			typed = (view == DXGI_FORMAT_R16_FLOAT || res == DXGI_FORMAT_R16_FLOAT) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R16_UNORM;
			return true;
		default: return false;
		}
	}

	inline bool Prepare(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& d, DXGI_FORMAT group, DXGI_FORMAT typed)
	{
		if (r.device.Get() != device) { r = {}; r.device = device; }
		if (r.failed) return false;
		if (!r.shader) {
			static constexpr char source[] = R"(
Texture2D<float> field : register(t0);
RWTexture2D<float> outputField : register(u0);
cbuffer Extents : register(b0) { uint2 render; uint2 display; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
 if (any(id.xy >= display)) return;
 uint2 at = min(uint2((id.xy + 0.5) * float2(render) / float2(display)), render - 1);
 outputField[id.xy] = field.Load(int3(at, 0));
})";
			Microsoft::WRL::ComPtr<ID3DBlob> code;
			if (FAILED(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) ||
				FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &r.shader))) { r.failed = true; return false; }
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = 16; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.Usage = D3D11_USAGE_DEFAULT;
			if (FAILED(device->CreateBuffer(&cb, nullptr, &r.cb))) { r.failed = true; return false; }
		}
		if (r.src && r.desc.Width == d.Width && r.desc.Height == d.Height && r.desc.Format == group && r.viewFormat == typed) return true;
		r.src.Reset(); r.dst.Reset(); r.srv.Reset(); r.uav.Reset();
		D3D11_TEXTURE2D_DESC t{};
		t.Width = d.Width; t.Height = d.Height; t.MipLevels = t.ArraySize = t.SampleDesc.Count = 1;
		t.Format = group;
		t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = typed; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
		uv.Format = typed; uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		if (FAILED(device->CreateTexture2D(&t, nullptr, &r.src)) || FAILED(device->CreateShaderResourceView(r.src.Get(), &sv, &r.srv))) return false;
		t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(device->CreateTexture2D(&t, nullptr, &r.dst)) || FAILED(device->CreateUnorderedAccessView(r.dst.Get(), &uv, &r.uav))) { r.src.Reset(); return false; }
		r.desc = t; r.desc.Format = group; r.viewFormat = typed;
		return true;
	}

	// In-place relayout of `tex` (mip 0) through two scratch copies.
	inline bool RunRelayout(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, DXGI_FORMAT group, DXGI_FORMAT typed)
	{
		if (d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1) return false;
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		if (!device || !Prepare(device.Get(), d, group, typed)) return false;
		const UINT dims[4]{ g.renderW, g.renderH, d.Width, d.Height };
		ctx->UpdateSubresource(r.cb.Get(), 0, nullptr, dims, 0, 0);
		ctx->CopyResource(r.src.Get(), tex);
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> savedCs;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> savedSrv;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> savedUav;
		Microsoft::WRL::ComPtr<ID3D11Buffer> savedCb;
		ctx->CSGetShader(&savedCs, nullptr, nullptr);
		ctx->CSGetShaderResources(0, 1, &savedSrv);
		ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
		ctx->CSGetConstantBuffers(0, 1, &savedCb);
		ctx->CSSetShader(r.shader.Get(), nullptr, 0);
		ctx->CSSetConstantBuffers(0, 1, r.cb.GetAddressOf());
		ctx->CSSetShaderResources(0, 1, r.srv.GetAddressOf());
		ctx->CSSetUnorderedAccessViews(0, 1, r.uav.GetAddressOf(), nullptr);
		ctx->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
		ID3D11UnorderedAccessView* noUav{};
		ID3D11ShaderResourceView* noSrv{};
		ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
		ctx->CSSetShaderResources(0, 1, &noSrv);
		ctx->CSSetShader(savedCs.Get(), nullptr, 0);
		ctx->CSSetShaderResources(0, 1, savedSrv.GetAddressOf());
		ctx->CSSetUnorderedAccessViews(0, 1, savedUav.GetAddressOf(), nullptr);
		ctx->CSSetConstantBuffers(0, 1, savedCb.GetAddressOf());
		ctx->CopyResource(tex, r.dst.Get());
		return true;
	}

	inline void Begin(ID3D11Texture2D* displayDepth, UINT renderW, UINT renderH)
	{
		g = {};  // scratch resources in `r` persist across frames
		if (!displayDepth || !renderW || !renderH) return;
		displayDepth->GetDesc(&g.desc);
		if (renderW >= g.desc.Width || renderH >= g.desc.Height) return;
		g.source = displayDepth;
		g.renderW = renderW; g.renderH = renderH;
		g.active = DepthFamily(g.desc.Format) && g.desc.SampleDesc.Count == 1 && g.desc.ArraySize == 1;
		if (g.active) g.written[g.writtenCount++] = displayDepth;
	}

	inline void End() { g.active = false; g.source = nullptr; }

	inline bool In(ID3D11Resource* const* set, unsigned count, ID3D11Resource* res)
	{
		for (unsigned i = 0; i < count; ++i) if (set[i] == res) return true;
		return false;
	}

	inline void MarkWritten(ID3D11Resource* res)
	{
		if (res && !In(g.written, g.writtenCount, res) && g.writtenCount < std::size(g.written)) g.written[g.writtenCount++] = res;
	}

	// Called before each DrawIndexed/Draw on the system immediate context.
	inline void OnDraw(ID3D11DeviceContext* ctx)
	{
		if (!g.active || !ctx) return;
		ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
		ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
		for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
			if (!views[slot]) continue;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
			view.Attach(views[slot]);
			if (g.doneCount >= std::size(g.done)) continue;
			Microsoft::WRL::ComPtr<ID3D11Resource> res;
			view->GetResource(&res);
			if (!res || In(g.written, g.writtenCount, res.Get()) || In(g.done, g.doneCount, res.Get())) continue;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
			if (FAILED(res.As(&tex))) continue;
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			if (d.Width != g.desc.Width || d.Height != g.desc.Height || d.SampleDesc.Count != 1 || d.ArraySize != 1) continue;
			D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
			view->GetDesc(&vd);
			DXGI_FORMAT group{}, typed{};
			bool ok = false;
			const char* how = "";
			if (DepthFamily(d.Format)) {
				if (d.MipLevels != g.desc.MipLevels) continue;
				ctx->CopyResource(tex.Get(), g.source);
				ok = true; how = "copied from engine depth";
			} else if (SingleChannel(d.Format, vd.Format, group, typed)) {
				ok = RunRelayout(ctx, tex.Get(), d, group, typed);
				how = ok ? "re-laid out in place" : "relayout unavailable";
			} else continue;
			g.done[g.doneCount++] = res.Get();
			static unsigned logged{};
			if (logged < 6) {
				++logged;
				F4SE::log::info("f4sr: post-seam depth copy {:p} ({}x{} fmt {} view {}) at t{}: {}",
					static_cast<void*>(tex.Get()), d.Width, d.Height, static_cast<std::uint32_t>(d.Format),
					static_cast<std::uint32_t>(vd.Format), slot, how);
			}
		}
		// Outputs of this draw are display layout from here on.
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		for (auto* v : rtvs) if (v) { Microsoft::WRL::ComPtr<ID3D11Resource> res; v->GetResource(&res); MarkWritten(res.Get()); v->Release(); }
		if (dsv) { Microsoft::WRL::ComPtr<ID3D11Resource> res; dsv->GetResource(&res); MarkWritten(res.Get()); dsv->Release(); }
	}
}
