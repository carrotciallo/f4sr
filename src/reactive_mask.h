#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "F4SE/F4SE.h"
#include "interop.h"

namespace f4sr
{
	// Reactive mask for the upscaler: how much each pixel changed between the
	// end of the deferred composite (opaque lighting only) and the seam (after
	// forward passes: alpha-blended geometry, particles, decals, effects). The
	// same idea as the FidelityFX "generate reactive" helper (tone-mapped
	// per-channel max difference, scaled, thresholded), done here on the game's
	// D3D11 context so it works for FSR and XeSS alike. The result is a shared
	// R8 texture the D3D12 side passes as the reactive / responsive pixel mask.
	class ReactiveMask
	{
	public:
		bool Create(ID3D11Device* device, D3D12Interop& interop, const D3D11_TEXTURE2D_DESC& mainDesc)
		{
			Destroy();
			D3D11_TEXTURE2D_DESC d{};
			d.Width = mainDesc.Width;
			d.Height = mainDesc.Height;
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.Format = mainDesc.Format;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(device->CreateTexture2D(&d, nullptr, opaque_.GetAddressOf()))) return Fail("opaque copy texture");
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = Typed(mainDesc.Format);
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(opaque_.Get(), &sd, opaqueSrv_.GetAddressOf()))) return Fail("opaque copy view");
			format_ = sd.Format;

			d.Format = DXGI_FORMAT_R8_UNORM;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			if (!interop.CreateSharedTexture(d, shared_) || !shared_.tex11) return Fail("shared mask texture");
			D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
			ud.Format = DXGI_FORMAT_R8_UNORM;
			ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateUnorderedAccessView(shared_.tex11.Get(), &ud, uav_.GetAddressOf()))) return Fail("mask view");

			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = 16;
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&bd, nullptr, constants_.GetAddressOf()))) return Fail("constants");

			static constexpr const char* kSource = R"(
Texture2D<float4> Opaque : register(t0);
Texture2D<float4> Final : register(t1);
RWTexture2D<float> Reactive : register(u0);
cbuffer Params : register(b0) { uint2 size; float scale; float threshold; };
float3 Tonemap(float3 c) { c = max(c, 0); return c / (1.0 + max(max(c.r, c.g), c.b)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= size)) return;
	float3 d = abs(Tonemap(Final[id.xy].rgb) - Tonemap(Opaque[id.xy].rgb));
	float r = saturate(max(max(d.r, d.g), d.b) * scale);
	Reactive[id.xy] = r < threshold ? 0.0 : min(r, 0.9);
}
)";
			Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
			if (FAILED(D3DCompile(kSource, std::strlen(kSource), nullptr, nullptr, nullptr, "main", "cs_5_0",
					D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf())) ||
				FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, cs_.GetAddressOf())))
				return Fail("shader");
			F4SE::log::info("f4sr: reactive mask ready ({}x{}, scene format {})", d.Width, d.Height, static_cast<int>(format_));
			return true;
		}

		void Destroy()
		{
			opaque_.Reset(); opaqueSrv_.Reset(); uav_.Reset(); constants_.Reset(); cs_.Reset();
			shared_ = {};
			snapped_ = false;
		}

		[[nodiscard]] bool Ready() const { return cs_ && shared_.res12; }

		// End of the deferred composite: keep the opaque scene's render rect.
		void Snapshot(ID3D11DeviceContext* ctx, ID3D11Texture2D* main, std::uint32_t w, std::uint32_t h)
		{
			if (!Ready() || !main || w == 0 || h == 0) return;
			const D3D11_BOX box{ 0, 0, 0, w, h, 1 };
			ctx->CopySubresourceRegion(opaque_.Get(), 0, 0, 0, 0, main, 0, &box);
			snapped_ = true;
			++snapshots_;
		}

		// At the seam, after the scene colour was copied to `finalSrv`'s
		// texture. Returns the D3D12 mask, or nullptr when there is no
		// snapshot for this frame (the upscaler then runs without a mask).
		ID3D12Resource* Generate(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* finalSrv, std::uint32_t w, std::uint32_t h, float scale)
		{
			const bool have = snapped_;
			snapped_ = false;
			if (!Ready() || !finalSrv || !have || scale <= 0.0f) {
				if (Ready() && scale > 0.0f) ++missed_;
				return nullptr;
			}
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
			struct { std::uint32_t w, h; float scale, threshold; } p{ w, h, scale, 0.04f };
			std::memcpy(m.pData, &p, sizeof(p));
			ctx->Unmap(constants_.Get(), 0);

			ID3D11ShaderResourceView* oldSrv[2]{};
			ID3D11UnorderedAccessView* oldUav{};
			ID3D11Buffer* oldCb{};
			ID3D11ComputeShader* oldCs{};
			ID3D11ClassInstance* inst[256]{};
			UINT nInst = 256;
			ctx->CSGetShaderResources(0, 2, oldSrv);
			ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
			ctx->CSGetConstantBuffers(0, 1, &oldCb);
			ctx->CSGetShader(&oldCs, inst, &nInst);

			ID3D11ShaderResourceView* srvs[2]{ opaqueSrv_.Get(), finalSrv };
			ID3D11UnorderedAccessView* uav = uav_.Get();
			ID3D11Buffer* cb = constants_.Get();
			ctx->CSSetShaderResources(0, 2, srvs);
			ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			ctx->CSSetConstantBuffers(0, 1, &cb);
			ctx->CSSetShader(cs_.Get(), nullptr, 0);
			ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

			ID3D11ShaderResourceView* nullSrv[2]{};
			ctx->CSSetShaderResources(0, 2, nullSrv);
			ctx->CSSetShaderResources(0, 2, oldSrv);
			ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
			ctx->CSSetConstantBuffers(0, 1, &oldCb);
			ctx->CSSetShader(oldCs, inst, nInst);
			for (auto* s : oldSrv) if (s) s->Release();
			if (oldUav) oldUav->Release();
			if (oldCb) oldCb->Release();
			if (oldCs) oldCs->Release();
			for (UINT i = 0; i < nInst; ++i) if (inst[i]) inst[i]->Release();
			++generated_;
			return shared_.res12.Get();
		}

		// An all-zero mask, for a backend that requires one every frame (XeSS
		// created with the responsive-mask flag) when no mask was generated.
		ID3D12Resource* Cleared(ID3D11DeviceContext* ctx)
		{
			if (!Ready()) return nullptr;
			const FLOAT zero[4]{};
			ctx->ClearUnorderedAccessViewFloat(uav_.Get(), zero);
			return shared_.res12.Get();
		}

		[[nodiscard]] std::uint64_t Generated() const { return generated_; }
		[[nodiscard]] std::uint64_t Missed() const { return missed_; }
		[[nodiscard]] std::uint64_t Snapshots() const { return snapshots_; }

		static DXGI_FORMAT Typed(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
			default: return f;
			}
		}

	private:
		bool Fail(const char* what)
		{
			F4SE::log::warn("f4sr: reactive mask unavailable ({} failed); the upscaler runs without it", what);
			Destroy();
			return false;
		}

		Microsoft::WRL::ComPtr<ID3D11Texture2D> opaque_;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> opaqueSrv_;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
		Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
		SharedTexture shared_;
		DXGI_FORMAT format_{};
		bool snapped_{};
		std::uint64_t generated_{}, missed_{}, snapshots_{};
	};
}
