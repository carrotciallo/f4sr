#pragma once

// Path B (ENB before upscaling): the game, ENB included, renders at the
// render size and the proxy swap chain runs at the display size. Until the
// upscaler moves to the UI boundary, frames are scaled to the display size
// with a bilinear filter here.

#include <cstring>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "vram_budget.h"

namespace f4sr::output_scale
{
	inline DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
	{
		switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
		}
	}

	inline bool IsTypeless(DXGI_FORMAT f)
	{
		switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return true;
		default: return false;
		}
	}

	// Bilinear copy of a texture into a larger (or equal) one. Runs in its own
	// device context state, so the engine's and ENB's state caches never see
	// a change. A source the shader can read in place (shader-resource
	// binding, not the destination, a raw view format) is read directly;
	// others are copied into a shader-readable stage first (swap chain buffers
	// are not always created with shader-resource binding). The stage costs a
	// full copy of the source in VRAM (66 MB for a 4K RGBA16F source).
	class Scaler
	{
	public:
		enum class Transfer { None, DecodeGamma22, EncodeGamma22 };

		// Keeps a copy of `src` for ScaleChanged.
		bool Snapshot(ID3D11DeviceContext* ctx, ID3D11Texture2D* src)
		{
			if (failed_ || !ctx || !src) return false;
			Microsoft::WRL::ComPtr<ID3D11Device> device;
			ctx->GetDevice(&device);
			D3D11_TEXTURE2D_DESC d{};
			src->GetDesc(&d);
			if (!before_ || d.Width != beforeDesc_.Width || d.Height != beforeDesc_.Height || d.Format != beforeDesc_.Format) {
				before_.Reset();
				beforeSrv_.Reset();
				D3D11_TEXTURE2D_DESC s = d;
				s.MipLevels = s.ArraySize = 1;
				s.SampleDesc = { 1, 0 };
				s.Usage = D3D11_USAGE_DEFAULT;
				s.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				s.CPUAccessFlags = s.MiscFlags = 0;
				D3D11_SHADER_RESOURCE_VIEW_DESC v{};
				v.Format = ViewFormat(d.Format);
				v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				v.Texture2D.MipLevels = 1;
				if (!device || FAILED(device->CreateTexture2D(&s, nullptr, &before_)) || FAILED(device->CreateShaderResourceView(before_.Get(), &v, &beforeSrv_))) {
					before_.Reset();
					return false;
				}
				beforeDesc_ = d;
				vram_budget::TrackTexture(before_.Get());
			}
			ctx->CopyResource(before_.Get(), src);
			return true;
		}

		// Scales only where `src` differs from the last Snapshot; the rest of
		// `dst` keeps its content.
		bool ScaleChanged(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst)
		{
			if (!before_) return Scale(ctx, src, dst);
			return Draw(ctx, src, dst, true, nullptr);
		}

		bool Scale(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, Transfer transfer = Transfer::None)
		{
			return Draw(ctx, src, dst, false, nullptr, transfer);
		}

		// Transfers the signed RGB change from the render-size buffers onto
		// the display-size base, rather than replacing its scene or UI samples.
		bool CompositeDelta(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* base, ID3D11Texture2D* dst)
		{
			if (!beforeSrv_ || !base || base == dst) return false;
			return Draw(ctx, src, dst, false, base);
		}

	private:
		bool Draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, bool changedOnly, ID3D11Texture2D* base,
			Transfer transfer = Transfer::None)
		{
			if (failed_ || !ctx || !src || !dst) return false;
			Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
			if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return Fail();
			Microsoft::WRL::ComPtr<ID3D11Device> device;
			ctx->GetDevice(&device);
			if (!device || !Init(device.Get()) || !Stage(device.Get(), src, dst) || !Target(device.Get(), dst)) return false;
			if (base && (stageDesc_.Width != beforeDesc_.Width || stageDesc_.Height != beforeDesc_.Height ||
				stageDesc_.Format != beforeDesc_.Format || !Base(device.Get(), base))) return false;
			ID3D11PixelShader* shader = base ? psDelta_.Get() : changedOnly ? psChanged_.Get() : ps_.Get();
			if (transfer != Transfer::None) {
				if (stageDesc_.Width != dstDesc_.Width || stageDesc_.Height != dstDesc_.Height) return false;
				const bool decode = transfer == Transfer::DecodeGamma22;
				auto& psTransfer = decode ? psDecodeGamma_ : psEncodeGamma_;
				if (!psTransfer) {
					Microsoft::WRL::ComPtr<ID3DBlob> blob;
					if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
						decode ? "psDecodeGamma" : "psEncodeGamma", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, nullptr)) ||
						FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psTransfer))) return Fail();
				}
				shader = psTransfer.Get();
			}

			if (!direct_) ctx->CopyResource(stage_.Get(), src);
			Microsoft::WRL::ComPtr<ID3DDeviceContextState> previous;
			ctx1->SwapDeviceContextState(state_.Get(), &previous);
			const D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(dstDesc_.Width), static_cast<float>(dstDesc_.Height), 0.0f, 1.0f };
			ID3D11RenderTargetView* rtv = rtv_.Get();
			ID3D11ShaderResourceView* srv = srv_.Get();
			ID3D11SamplerState* sampler = sampler_.Get();
			ctx->OMSetRenderTargets(1, &rtv, nullptr);
			ctx->RSSetViewports(1, &vp);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(vs_.Get(), nullptr, 0);
			ctx->PSSetShader(shader, nullptr, 0);
			ID3D11ShaderResourceView* srvs[3]{ srv, changedOnly || base ? beforeSrv_.Get() : nullptr, base ? baseSrv_.Get() : nullptr };
			const UINT srvCount = base ? 3 : 2;
			ctx->PSSetShaderResources(0, srvCount, srvs);
			ctx->PSSetSamplers(0, 1, &sampler);
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView* none[3]{};
			ctx->PSSetShaderResources(0, srvCount, none);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);
			ctx1->SwapDeviceContextState(previous.Get(), nullptr);
			return true;
		}

		bool Fail()
		{
			failed_ = true;
			return false;
		}

		static constexpr char kShader[] = R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	return source.SampleLevel(linearClamp, uv, 0);
}
float4 psDecodeGamma(float4 pos : SV_Position) : SV_Target
{
	float4 c = source.Load(int3(int2(pos.xy), 0));
	return float4(pow(saturate(c.rgb), 2.2), c.a);
}
float4 psEncodeGamma(float4 pos : SV_Position) : SV_Target
{
	float4 c = source.Load(int3(int2(pos.xy), 0));
	return float4(pow(saturate(c.rgb), 1.0 / 2.2), c.a);
}
Texture2D<float4> before : register(t1);
float4 psChanged(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	uint w, h;
	source.GetDimensions(w, h);
	int2 base = int2(floor(uv * float2(w, h) - 0.5));
	bool changed = false;
	[unroll] for (int y = 0; y < 2; ++y)
		[unroll] for (int x = 0; x < 2; ++x) {
			int3 p = int3(clamp(base + int2(x, y), int2(0, 0), int2(w - 1, h - 1)), 0);
			changed = changed || any(source.Load(p) != before.Load(p));
		}
	if (!changed) discard;
	return source.SampleLevel(linearClamp, uv, 0);
}
Texture2D<float4> canvas : register(t2);
float4 psDelta(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float4 base = canvas.Load(int3(int2(pos.xy), 0));
	float3 delta = source.SampleLevel(linearClamp, uv, 0).rgb - before.SampleLevel(linearClamp, uv, 0).rgb;
	return float4(base.rgb + delta, base.a);
})";

		bool Init(ID3D11Device* device)
		{
			if (vs_) return true;
			Microsoft::WRL::ComPtr<ID3DBlob> vsCode, psCode, psChangedCode;
			Microsoft::WRL::ComPtr<ID3D11Device1> device1;
			const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
			D3D11_SAMPLER_DESC s{};
			s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			s.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, nullptr)) ||
				FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, nullptr)) ||
				FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
				FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_)) ||
				FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "psChanged", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psChangedCode, nullptr)) ||
				FAILED(device->CreatePixelShader(psChangedCode->GetBufferPointer(), psChangedCode->GetBufferSize(), nullptr, &psChanged_)) ||
				FAILED(device->CreateSamplerState(&s, &sampler_)) ||
				FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
				FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &state_))) {
				vs_.Reset();
				return Fail();
			}
			return true;
		}

		bool Base(ID3D11Device* device, ID3D11Texture2D* base)
		{
			if (!psDelta_) {
				Microsoft::WRL::ComPtr<ID3DBlob> code;
				if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "psDelta", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) ||
					FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &psDelta_))) return Fail();
			}
			D3D11_TEXTURE2D_DESC d{};
			base->GetDesc(&d);
			if (d.Width != dstDesc_.Width || d.Height != dstDesc_.Height) return false;
			if (base == base_ && baseSrv_) return true;
			baseSrv_.Reset();
			D3D11_SHADER_RESOURCE_VIEW_DESC v{};
			v.Format = ViewFormat(d.Format);
			v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			v.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(base, &v, &baseSrv_))) return Fail();
			base_ = base;
			return true;
		}

		bool Stage(ID3D11Device* device, ID3D11Texture2D* src, ID3D11Texture2D* dst)
		{
			D3D11_TEXTURE2D_DESC d{};
			src->GetDesc(&d);
			const bool readable = src != dst && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 && d.SampleDesc.Count == 1 &&
				d.ArraySize == 1 && (ViewFormat(d.Format) == d.Format || IsTypeless(d.Format));
			if (readable) {
				// srv_ holds a reference to src, so the pointer cannot be reused
				// by another texture while it is cached.
				if (direct_ == src && srv_) return true;
				stage_.Reset();
				srv_.Reset();
				D3D11_SHADER_RESOURCE_VIEW_DESC v{};
				v.Format = ViewFormat(d.Format);
				v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				v.Texture2D.MipLevels = 1;
				if (FAILED(device->CreateShaderResourceView(src, &v, &srv_))) {
					direct_ = nullptr;
					return Fail();
				}
				direct_ = src;
				stageDesc_ = d;
				return true;
			}
			if (direct_) {
				direct_ = nullptr;
				srv_.Reset();
			}
			if (stage_ && d.Width == stageDesc_.Width && d.Height == stageDesc_.Height && d.Format == stageDesc_.Format) return true;
			stage_.Reset();
			srv_.Reset();
			D3D11_TEXTURE2D_DESC s{};
			s.Width = d.Width;
			s.Height = d.Height;
			s.MipLevels = s.ArraySize = 1;
			s.Format = ViewFormat(d.Format);  // raw copy/view, no implicit sRGB transfer
			s.SampleDesc.Count = 1;
			s.Usage = D3D11_USAGE_DEFAULT;
			s.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SHADER_RESOURCE_VIEW_DESC v{};
			v.Format = ViewFormat(d.Format);
			v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			v.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateTexture2D(&s, nullptr, &stage_)) || FAILED(device->CreateShaderResourceView(stage_.Get(), &v, &srv_))) return Fail();
			if (d.Width * d.Height >= 1920u * 1080u) vram_budget::Note("scaler stage copy", d.Width, d.Height, s.Format);
			vram_budget::TrackTexture(stage_.Get());
			stageDesc_ = d;
			return true;
		}

		bool Target(ID3D11Device* device, ID3D11Texture2D* dst)
		{
			if (dst == target_) return true;
			rtv_.Reset();
			D3D11_TEXTURE2D_DESC d{};
			dst->GetDesc(&d);
			D3D11_RENDER_TARGET_VIEW_DESC v{};
			v.Format = ViewFormat(d.Format);
			v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateRenderTargetView(dst, &v, &rtv_))) return Fail();
			target_ = dst;
			dstDesc_ = d;
			return true;
		}

		bool failed_{};
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_, psChanged_, psDelta_, psDecodeGamma_, psEncodeGamma_;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> baseSrv_;
		ID3D11Texture2D* base_{};  // owned by baseSrv_
		Microsoft::WRL::ComPtr<ID3D11Texture2D> before_;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> beforeSrv_;
		D3D11_TEXTURE2D_DESC beforeDesc_{};
		Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
		Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> stage_;
		ID3D11Texture2D* direct_{};  // read in place; referenced by srv_
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
		ID3D11Texture2D* target_{};  // the presenter owns it
		D3D11_TEXTURE2D_DESC stageDesc_{}, dstDesc_{};
	};
}
