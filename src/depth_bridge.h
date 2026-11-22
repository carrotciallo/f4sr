#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace f4sr
{
	// After the sub-native resolve, the engine depth target still holds the
	// render-resolution layout in its top-left rectangle and the clear value
	// elsewhere, while the colour it pairs with is now display layout. Effects
	// after the seam that sample depth by screen UV (the imagespace chain and
	// ENB) then see depth that is half-scale inside the rectangle and "far"
	// outside it. Rewrite the depth plane in display layout from the
	// render-layout copy taken for the upscaler. Stencil is left untouched.
	class DepthBridge
	{
		template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
		Ptr<ID3D11Device> device_;
		Ptr<ID3D11VertexShader> vs_;
		Ptr<ID3D11PixelShader> ps_;
		Ptr<ID3D11DepthStencilState> dss_;
		Ptr<ID3D11RasterizerState> rs_;
		Ptr<ID3D11Buffer> cb_;
		Ptr<ID3D11ShaderResourceView> view_;
		ID3D11Texture2D* viewSource_{};
		UINT w_{}, h_{}, rw_{}, rh_{};
		bool failed_{};

		bool Prepare(ID3D11Device* device, UINT w, UINT h, UINT rw, UINT rh)
		{
			if (device_.Get() == device && w_ == w && h_ == h && rw_ == rw && rh_ == rh) return !failed_;
			*this = {};
			device_ = device; w_ = w; h_ = h; rw_ = rw; rh_ = rh; failed_ = true;
			static constexpr char vsSource[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
 float2 uv = float2((id << 1) & 2, id & 2);
 return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})";
			// Nearest sample: interpolating depth across silhouettes would
			// invent surfaces between foreground and background.
			static constexpr char psSource[] = R"(
Texture2D<float> source : register(t0);
cbuffer Extents : register(b0) { uint2 render; uint2 display; };
float main(float4 p : SV_Position) : SV_Depth {
 uint2 at = min(uint2(p.xy * float2(render) / float2(display)), render - 1);
 return source.Load(int3(at, 0));
})";
			Ptr<ID3DBlob> vsCode, psCode;
			if (FAILED(D3DCompile(vsSource, sizeof(vsSource) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, nullptr)) ||
				FAILED(D3DCompile(psSource, sizeof(psSource) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, nullptr)) ||
				FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
				FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_))) return false;
			D3D11_DEPTH_STENCIL_DESC ds{};
			ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
			D3D11_RASTERIZER_DESC rs{};
			rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = FALSE;
			const UINT dims[]{ rw, rh, w, h };
			D3D11_SUBRESOURCE_DATA init{ dims, 0, 0 };
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = sizeof(dims); cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.Usage = D3D11_USAGE_IMMUTABLE;
			failed_ = FAILED(device->CreateDepthStencilState(&ds, &dss_)) || FAILED(device->CreateRasterizerState(&rs, &rs_)) ||
				FAILED(device->CreateBuffer(&cb, &init, &cb_));
			return !failed_;
		}

		// Everything the draw touches, restored afterwards.
		struct Saved
		{
			ID3D11DeviceContext* ctx;
			Ptr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
			Ptr<ID3D11DepthStencilView> dsv;
			Ptr<ID3D11DepthStencilState> dss; UINT stencilRef{};
			Ptr<ID3D11BlendState> blend; FLOAT blendFactor[4]{}; UINT sampleMask{};
			Ptr<ID3D11RasterizerState> rs;
			D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{}; UINT numVps{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			Ptr<ID3D11InputLayout> layout; D3D11_PRIMITIVE_TOPOLOGY topology{};
			Ptr<ID3D11VertexShader> vs; Ptr<ID3D11PixelShader> ps;
			Ptr<ID3D11GeometryShader> gs; Ptr<ID3D11HullShader> hs; Ptr<ID3D11DomainShader> ds;
			Ptr<ID3D11ShaderResourceView> psSrv; Ptr<ID3D11Buffer> psCb;
			explicit Saved(ID3D11DeviceContext* c) : ctx(c)
			{
				ID3D11RenderTargetView* r[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
				ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, r, &dsv);
				for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) rtvs[i].Attach(r[i]);
				ctx->OMGetDepthStencilState(&dss, &stencilRef);
				ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
				ctx->RSGetState(&rs);
				ctx->RSGetViewports(&numVps, vps);
				ctx->IAGetInputLayout(&layout);
				ctx->IAGetPrimitiveTopology(&topology);
				ctx->VSGetShader(&vs, nullptr, nullptr);
				ctx->PSGetShader(&ps, nullptr, nullptr);
				ctx->GSGetShader(&gs, nullptr, nullptr);
				ctx->HSGetShader(&hs, nullptr, nullptr);
				ctx->DSGetShader(&ds, nullptr, nullptr);
				ctx->PSGetShaderResources(0, 1, &psSrv);
				ctx->PSGetConstantBuffers(0, 1, &psCb);
			}
			~Saved()
			{
				ID3D11RenderTargetView* r[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
				for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) r[i] = rtvs[i].Get();
				ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, r, dsv.Get());
				ctx->OMSetDepthStencilState(dss.Get(), stencilRef);
				ctx->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
				ctx->RSSetState(rs.Get());
				ctx->RSSetViewports(numVps, vps);
				ctx->IASetInputLayout(layout.Get());
				ctx->IASetPrimitiveTopology(topology);
				ctx->VSSetShader(vs.Get(), nullptr, 0);
				ctx->PSSetShader(ps.Get(), nullptr, 0);
				ctx->GSSetShader(gs.Get(), nullptr, 0);
				ctx->HSSetShader(hs.Get(), nullptr, 0);
				ctx->DSSetShader(ds.Get(), nullptr, 0);
				ctx->PSSetShaderResources(0, 1, psSrv.GetAddressOf());
				ctx->PSSetConstantBuffers(0, 1, psCb.GetAddressOf());
			}
		};

	public:
		// source: R32_FLOAT render-layout depth (top-left rw x rh valid).
		// target/dsv: the engine depth texture and its writable view, w x h.
		bool Apply(ID3D11DeviceContext* ctx, ID3D11Texture2D* source, ID3D11Texture2D* target, ID3D11DepthStencilView* dsv,
			UINT w, UINT h, UINT rw, UINT rh)
		{
			if (!ctx || !source || !target || !dsv || !rw || !rh || rw >= w || rh >= h) return false;
			D3D11_TEXTURE2D_DESC td{}; target->GetDesc(&td);
			D3D11_TEXTURE2D_DESC sd{}; source->GetDesc(&sd);
			D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; dsv->GetDesc(&vd);
			Ptr<ID3D11Resource> dsvRes; dsv->GetResource(&dsvRes);
			if (dsvRes.Get() != static_cast<ID3D11Resource*>(target) || td.Width != w || td.Height != h || td.SampleDesc.Count != 1 ||
				vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice || vd.Flags ||
				sd.Format != DXGI_FORMAT_R32_FLOAT || sd.Width < rw || sd.Height < rh) return false;
			Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
			if (!Prepare(dev.Get(), w, h, rw, rh)) return false;
			if (viewSource_ != source || !view_) {
				view_.Reset(); viewSource_ = nullptr;
				if (FAILED(dev->CreateShaderResourceView(source, nullptr, &view_))) return false;
				viewSource_ = source;
			}
			Saved saved{ ctx };
			ctx->OMSetRenderTargets(0, nullptr, dsv);
			ctx->OMSetDepthStencilState(dss_.Get(), 0);
			ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			ctx->RSSetState(rs_.Get());
			const D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
			ctx->RSSetViewports(1, &vp);
			ctx->IASetInputLayout(nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(vs_.Get(), nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->HSSetShader(nullptr, nullptr, 0);
			ctx->DSSetShader(nullptr, nullptr, 0);
			ctx->PSSetShader(ps_.Get(), nullptr, 0);
			ctx->PSSetShaderResources(0, 1, view_.GetAddressOf());
			ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView* none{};
			ctx->PSSetShaderResources(0, 1, &none);
			return true;
		}
	};
}
