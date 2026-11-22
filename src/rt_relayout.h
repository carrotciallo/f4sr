#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace f4sr
{
	// Stretches the render rectangle (top-left w x h) of a colour render
	// target over the whole texture, in place, with bilinear filtering.
	// Used for targets written before the SR seam at the dynamic-resolution
	// viewport and read afterwards by passes that assume display layout.
	class RtRelayout
	{
		template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
		Ptr<ID3D11Device> device_;
		Ptr<ID3D11VertexShader> vs_;
		Ptr<ID3D11PixelShader> ps_;
		Ptr<ID3D11RasterizerState> rs_;
		Ptr<ID3D11SamplerState> sampler_;
		Ptr<ID3D11Buffer> cb_;
		Ptr<ID3D11Texture2D> temp_;
		Ptr<ID3D11ShaderResourceView> tempView_;
		D3D11_TEXTURE2D_DESC tempDesc_{};
		DXGI_FORMAT tempViewFormat_{};
		bool failed_{};

		bool Prepare(ID3D11Device* device)
		{
			if (device_.Get() == device) return !failed_;
			*this = {};
			device_ = device;
			failed_ = true;
			static constexpr char vsSource[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
 float2 uv = float2((id << 1) & 2, id & 2);
 return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})";
			static constexpr char psSource[] = R"(
Texture2D source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Extents : register(b0) { float2 rect; float2 full; };
float4 main(float4 p : SV_Position) : SV_Target {
 float2 uv = p.xy / full * rect;
 uv = clamp(uv, 0.5, rect - 0.5) / full;
 return source.SampleLevel(linearClamp, uv, 0);
})";
			Ptr<ID3DBlob> vsCode, psCode;
			if (FAILED(D3DCompile(vsSource, sizeof(vsSource) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, nullptr)) ||
				FAILED(D3DCompile(psSource, sizeof(psSource) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, nullptr)) ||
				FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
				FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_))) return false;
			D3D11_RASTERIZER_DESC rs{};
			rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = 16; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.Usage = D3D11_USAGE_DYNAMIC; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			failed_ = FAILED(device->CreateRasterizerState(&rs, &rs_)) || FAILED(device->CreateSamplerState(&sd, &sampler_)) ||
				FAILED(device->CreateBuffer(&cb, nullptr, &cb_));
			return !failed_;
		}

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
			Ptr<ID3D11ShaderResourceView> psSrv; Ptr<ID3D11Buffer> psCb; Ptr<ID3D11SamplerState> psSampler;
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
				ctx->PSGetSamplers(0, 1, &psSampler);
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
				ctx->PSSetSamplers(0, 1, psSampler.GetAddressOf());
			}
		};

		static DXGI_FORMAT Typed(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			default: return f;
			}
		}

	public:
		// target: the texture; rtv: a render-target view of mip 0 (created
		// here when null). rectW/rectH: the valid top-left extent.
		bool Apply(ID3D11DeviceContext* ctx, ID3D11Texture2D* target, ID3D11RenderTargetView* rtv, float rectW, float rectH)
		{
			if (!ctx || !target) return false;
			D3D11_TEXTURE2D_DESC td{};
			target->GetDesc(&td);
			if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || rectW < 1 || rectH < 1 || rectW >= td.Width || rectH >= td.Height) return false;
			Ptr<ID3D11Device> dev;
			ctx->GetDevice(&dev);
			if (!Prepare(dev.Get())) return false;
			Ptr<ID3D11RenderTargetView> ownRtv;
			if (!rtv) {
				if (!(td.BindFlags & D3D11_BIND_RENDER_TARGET)) return false;
				D3D11_RENDER_TARGET_VIEW_DESC rd{};
				rd.Format = Typed(td.Format);
				rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				if (FAILED(dev->CreateRenderTargetView(target, &rd, &ownRtv))) return false;
				rtv = ownRtv.Get();
			}
			const DXGI_FORMAT viewFormat = Typed(td.Format);
			if (!temp_ || tempDesc_.Width != td.Width || tempDesc_.Height != td.Height || tempDesc_.Format != td.Format || tempViewFormat_ != viewFormat) {
				temp_.Reset();
				tempView_.Reset();
				D3D11_TEXTURE2D_DESC d = td;
				d.MipLevels = 1;
				d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				d.Usage = D3D11_USAGE_DEFAULT;
				d.CPUAccessFlags = 0;
				d.MiscFlags = 0;
				D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
				vd.Format = viewFormat;
				vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				vd.Texture2D.MipLevels = 1;
				if (FAILED(dev->CreateTexture2D(&d, nullptr, &temp_)) || FAILED(dev->CreateShaderResourceView(temp_.Get(), &vd, &tempView_))) {
					temp_.Reset();
					return false;
				}
				tempDesc_ = td;
				tempViewFormat_ = viewFormat;
			}
			ctx->CopySubresourceRegion(temp_.Get(), 0, 0, 0, 0, target, 0, nullptr);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
			const float extents[4]{ rectW, rectH, float(td.Width), float(td.Height) };
			memcpy(m.pData, extents, sizeof(extents));
			ctx->Unmap(cb_.Get(), 0);
			Saved saved{ ctx };
			ID3D11ShaderResourceView* none{};
			ctx->PSSetShaderResources(0, 1, &none);
			ctx->OMSetRenderTargets(1, &rtv, nullptr);
			ctx->OMSetDepthStencilState(nullptr, 0);
			ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			ctx->RSSetState(rs_.Get());
			const D3D11_VIEWPORT vp{ 0, 0, float(td.Width), float(td.Height), 0, 1 };
			ctx->RSSetViewports(1, &vp);
			ctx->IASetInputLayout(nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(vs_.Get(), nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->HSSetShader(nullptr, nullptr, 0);
			ctx->DSSetShader(nullptr, nullptr, 0);
			ctx->PSSetShader(ps_.Get(), nullptr, 0);
			ctx->PSSetShaderResources(0, 1, tempView_.GetAddressOf());
			ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
			ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
			ctx->Draw(3, 0);
			ctx->PSSetShaderResources(0, 1, &none);
			return true;
		}
	};
}
