#include "proxy_targets.h"

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	// RenderTargetManager (OG 1.10.163): the REL address IS the
	// object (round-11 crash lesson). Field offsets are the
	// Dear-Modding engine layout, static-asserted there.
	constexpr std::uint64_t kManagerId = 1508457;
	constexpr std::size_t kRtDataOffset = 0x000;    // RenderTargetProperties[100], stride 0x20
	constexpr std::size_t kRtIdOffset = 0xDC4;      // uint32[100]
	constexpr std::size_t kDsIdOffset = 0xF54;      // uint32[12]
	constexpr std::size_t kRatioWOffset = 0xF88;
	constexpr std::size_t kRatioHOffset = 0xF8C;
	constexpr std::size_t kVpFlagOffset = 0xFA7;    // useDynamicResolutionViewportAsDefaultViewport
	constexpr std::size_t kActivatedOffset = 0xFA8;
	constexpr std::uint64_t kStateId = 600795;      // BSGraphics::State instance, not State**
	constexpr std::uint64_t kCameraNearId = 57985;
	constexpr std::uint64_t kCameraFarId = 958877;

	// The targets the deferred composite and SSLR passes draw at
	// render scale (community-shaders' kProxiedTargets).
	constexpr std::int32_t kProxied[] = {
		f4sr::logical::kGBufferNormal,
		f4sr::logical::kGBufferEnvironment,
		f4sr::logical::kGBufferAmbient,
		f4sr::logical::kAmbientOcclusion,
		f4sr::logical::kGBufferEmissive,
		f4sr::logical::kDiffuseLight,
		f4sr::logical::kDiffuseSpecular,
		f4sr::logical::kAmbientOcclusionHalf,
		f4sr::logical::kMainCopy,
		f4sr::logical::kSSRBlurred1,
		f4sr::logical::kDiffuseLightTiled,
		f4sr::logical::kDiffuseSpecularTiled,
		f4sr::logical::kMain,
		f4sr::logical::kGBufferAlbedoSpecular,
		f4sr::logical::kSSRPrepass,
		f4sr::logical::kSSRDepth,
		f4sr::logical::kSSR,
	};
	constexpr std::int32_t kPooled[] = {
		f4sr::logical::kSSRBlurred0,
	};

	std::byte* Manager()
	{
		return reinterpret_cast<std::byte*>(REL::ID(kManagerId).address());
	}

	float MgrRatioW()
	{
		return *reinterpret_cast<float*>(Manager() + kRatioWOffset);
	}

	float MgrRatioH()
	{
		return *reinterpret_cast<float*>(Manager() + kRatioHOffset);
	}

	void WriteMgrState(float w, float h, bool activated)
	{
		auto* mgr = Manager();
		*reinterpret_cast<float*>(mgr + kRatioWOffset) = w;
		*reinterpret_cast<float*>(mgr + kRatioHOffset) = h;
		*reinterpret_cast<bool*>(mgr + kActivatedOffset) = activated;
	}

	bool Contains(std::initializer_list<std::int32_t> list, std::int32_t value)
	{
		for (const auto v : list) {
			if (v == value) {
				return true;
			}
		}
		return false;
	}

	ID3D11Device* EngineDevice()
	{
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		return data ? reinterpret_cast<ID3D11Device*>(data->device) : nullptr;
	}

	ID3D11DeviceContext* EngineContext()
	{
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		return data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
	}
}

namespace f4sr
{
	static_assert(sizeof(RE::BSGraphics::RenderTarget) == sizeof(RtStruct),
		"CLF4 RenderTarget layout must match the RtStruct mirror");

	RtStruct* ProxyTargets::ResolveTarget(std::int32_t logical)
	{
		if (logical < 0 || logical >= logical::kCount) {
			return nullptr;
		}
		const auto slot = *reinterpret_cast<const std::uint32_t*>(Manager() + kRtIdOffset + sizeof(std::uint32_t) * logical);
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!data || slot >= 101) {
			return nullptr;
		}
		return reinterpret_cast<RtStruct*>(&data->renderTargets[slot]);
	}

	void* ProxyTargets::ResolveDepthTarget(std::int32_t logical)
	{
		if (logical < 0 || logical >= 12) {
			return nullptr;
		}
		const auto slot = *reinterpret_cast<const std::uint32_t*>(Manager() + kDsIdOffset + sizeof(std::uint32_t) * logical);
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!data || slot >= 13) {
			return nullptr;
		}
		return &data->depthStencilTargets[slot];
	}

	std::uint32_t ProxyTargets::FrameCount()
	{
		// currentFrame at offset zero cycles through 0..7. A proxy not
		// visited for eight frames must not reuse its old color/depth fill.
		return reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(kStateId).address())->frameCount;
	}

	void ProxyTargets::CaptureOriginal(std::int32_t logical, const RtStruct& engineTarget)
	{
		filledValid_[logical] = false;
		const void* fields[] = { engineTarget.texture, engineTarget.copyTexture, engineTarget.rtView,
			engineTarget.srView, engineTarget.copySRView, engineTarget.uaView };
		for (std::size_t i = 0; i < std::size(fields); ++i) {
			originalRefs_[logical][i] = reinterpret_cast<IUnknown*>(const_cast<void*>(fields[i]));
		}
		originals_[logical] = engineTarget;
		proxies_[logical] = originals_[logical];
		proxies_[logical].texture = proxyTex_[logical].Get();
		proxies_[logical].rtView = proxyRtv_[logical].Get();
		proxies_[logical].srView = proxySrv_[logical].Get();
		proxies_[logical].uaView = proxyUav_[logical].Get();
		captured_[logical] = true;
	}

	void ProxyTargets::ReleaseProxy(std::int32_t logical)
	{
		filledValid_[logical] = false;
		proxyTex_[logical].Reset();
		proxyRtv_[logical].Reset();
		proxySrv_[logical].Reset();
		proxyUav_[logical].Reset();
		for (auto& ref : originalRefs_[logical]) ref.Reset();
		originals_[logical] = {};
		captured_[logical] = false;
		proxies_[logical] = {};
	}

	void ProxyTargets::RestoreEngineSlot(std::int32_t logical)
	{
		auto* engineTarget = ResolveTarget(logical);
		if (engineTarget && proxies_[logical].texture && originals_[logical].texture &&
			engineTarget->texture == proxies_[logical].texture) {
			*engineTarget = originals_[logical];
		}
	}

	void ProxyTargets::UpdateRenderTargetOne(std::int32_t logical, float widthRatio, float heightRatio)
	{
		captured_[logical] = false;
		auto* engineTarget = ResolveTarget(logical);
		if (!engineTarget) {
			return;
		}
		CaptureOriginal(logical, *engineTarget);
		if (widthRatio == 1.0f && heightRatio == 1.0f) {
			return;
		}
		auto* origTex = reinterpret_cast<ID3D11Texture2D*>(originals_[logical].texture);
		if (!origTex) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		origTex->GetDesc(&desc);
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		const bool hasRtv = originals_[logical].rtView != nullptr;
		if (hasRtv) {
			reinterpret_cast<ID3D11RenderTargetView*>(originals_[logical].rtView)->GetDesc(&rtvDesc);
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		const bool hasSrv = originals_[logical].srView != nullptr;
		if (hasSrv) {
			reinterpret_cast<ID3D11ShaderResourceView*>(originals_[logical].srView)->GetDesc(&srvDesc);
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		const bool hasUav = originals_[logical].uaView != nullptr;
		if (hasUav) {
			reinterpret_cast<ID3D11UnorderedAccessView*>(originals_[logical].uaView)->GetDesc(&uavDesc);
		}
		desc.Width = static_cast<std::uint32_t>(static_cast<float>(desc.Width) * widthRatio);
		desc.Height = static_cast<std::uint32_t>(static_cast<float>(desc.Height) * heightRatio);
		if (desc.Width == 0 || desc.Height == 0) {
			return;
		}
		auto* device = EngineDevice();
		if (!device) {
			return;
		}
		auto hr = device->CreateTexture2D(&desc, nullptr, proxyTex_[logical].GetAddressOf());
		if (SUCCEEDED(hr) && hasRtv) hr = device->CreateRenderTargetView(proxyTex_[logical].Get(), &rtvDesc, proxyRtv_[logical].GetAddressOf());
		if (SUCCEEDED(hr) && hasSrv) hr = device->CreateShaderResourceView(proxyTex_[logical].Get(), &srvDesc, proxySrv_[logical].GetAddressOf());
		if (SUCCEEDED(hr) && hasUav) hr = device->CreateUnorderedAccessView(proxyTex_[logical].Get(), &uavDesc, proxyUav_[logical].GetAddressOf());
		if (FAILED(hr)) {
			F4SE::log::error("f4sr: proxy target {} creation failed hr=0x{:08X}, {}x{} fmt={} bind=0x{:X}",
				logical, static_cast<std::uint32_t>(hr), desc.Width, desc.Height, static_cast<std::uint32_t>(desc.Format), desc.BindFlags);
			ReleaseProxy(logical);
			return;
		}
		proxies_[logical].texture = proxyTex_[logical].Get();
		proxies_[logical].rtView = proxyRtv_[logical].Get();
		proxies_[logical].srView = proxySrv_[logical].Get();
		proxies_[logical].uaView = proxyUav_[logical].Get();
	}

	void ProxyTargets::UpdateRenderTargets(float widthRatio, float heightRatio)
	{
		if (proxyPass_) return; // Never capture our own installed proxies as originals.
		bool unchanged = prevRatioW_ == widthRatio && prevRatioH_ == heightRatio;
		for (const auto target : kProxied) {
			const auto* live = ResolveTarget(target);
			if (!live || !captured_[target] || std::memcmp(live, &originals_[target], sizeof(RtStruct)) != 0) unchanged = false;
		}
		if (unchanged) return;
		AbortProxyPass();
		hasProxies_ = false;
		prevRatioW_ = widthRatio;
		prevRatioH_ = heightRatio;

		for (const auto target : kPooled) {
			ReleaseProxy(target);
			captured_[target] = false;
			originals_[target] = {};
		}
		for (const auto target : kProxied) {
			ReleaseProxy(target);
			UpdateRenderTargetOne(target, widthRatio, heightRatio);
		}
		depthTex_.Reset();
		depthSrv_.Reset();
		depthUav_.Reset();
		depthCopyValid_ = false;

		auto* mainTarget = ResolveTarget(logical::kMain);
		auto* mainTex = mainTarget ? reinterpret_cast<ID3D11Texture2D*>(mainTarget->texture) : nullptr;
		if (!mainTex) {
			hasProxies_ = false;
			return;
		}
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTex->GetDesc(&mainDesc);
		if (!mapLogged_) {
			mapLogged_ = true;
			const auto* mgr = Manager();
			const auto rtMain = *reinterpret_cast<const std::uint32_t*>(mgr + kRtIdOffset + sizeof(std::uint32_t) * logical::kMain);
			const auto rtMv = *reinterpret_cast<const std::uint32_t*>(mgr + kRtIdOffset + sizeof(std::uint32_t) * logical::kGBufferMotionVectors);
			const auto dsMain = *reinterpret_cast<const std::uint32_t*>(mgr + kDsIdOffset + sizeof(std::uint32_t) * logical::kDepthMain);
			F4SE::log::info("f4sr: proxy map: rtID[kMain=1]={} rtID[kMV=32]={} dsID[kMain=1]={}; kMain {}x{} fmt {}",
				rtMain, rtMv, dsMain, mainDesc.Width, mainDesc.Height,
				static_cast<std::uint32_t>(mainDesc.Format));
		}
		if (widthRatio == 1.0f && heightRatio == 1.0f) {
			return;
		}
		// The depth override texture: render-size R32 depth that
		// replaces srViewDepth while proxied passes run.
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<std::uint32_t>(static_cast<float>(mainDesc.Width) * widthRatio);
		desc.Height = static_cast<std::uint32_t>(static_cast<float>(mainDesc.Height) * heightRatio);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		auto* device = EngineDevice();
		if (!device || desc.Width == 0 || desc.Height == 0 ||
			FAILED(device->CreateTexture2D(&desc, nullptr, depthTex_.GetAddressOf()))) {
			hasProxies_ = false;
			return;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		const auto srvHr = device->CreateShaderResourceView(depthTex_.Get(), &srvDesc, depthSrv_.GetAddressOf());
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		const auto uavHr = device->CreateUnorderedAccessView(depthTex_.Get(), &uavDesc, depthUav_.GetAddressOf());
		if (FAILED(srvHr) || FAILED(uavHr) || !CompileDepthShaders(device)) {
			F4SE::log::error("f4sr: proxy depth setup failed srv=0x{:08X} uav=0x{:08X}; proxy pass disabled",
				static_cast<std::uint32_t>(srvHr), static_cast<std::uint32_t>(uavHr));
			return;
		}
		std::uint32_t built = 0;
		for (const auto target : kProxied) {
			if (proxyTex_[target]) {
				++built;
			}
		}
		hasProxies_ = built == std::size(kProxied);
		F4SE::log::info("f4sr: proxies built for ratio {:.3f}x{:.3f} ({}/{} targets, depth {}x{})",
			widthRatio, heightRatio, built, static_cast<std::uint32_t>(std::size(kProxied)),
			desc.Width, desc.Height);
	}

	void ProxyTargets::OverrideOne(std::int32_t logical, bool doCopy)
	{
		auto* engineTarget = ResolveTarget(logical);
		if (!engineTarget || !originals_[logical].texture || !proxyTex_[logical]) {
			return;
		}
		*engineTarget = proxies_[logical];
		if (!doCopy) {
			filledValid_[logical] = false;
			return;
		}
		if (filledValid_[logical] && filledFrame_[logical] == FrameCount()) {
			return;
		}
		D3D11_TEXTURE2D_DESC proxyDesc{};
		proxyTex_[logical]->GetDesc(&proxyDesc);
		const D3D11_BOX box{ 0, 0, 0, proxyDesc.Width, proxyDesc.Height, 1 };
		auto* ctx = EngineContext();
		if (!ctx) {
			return;
		}
		ctx->CopySubresourceRegion(proxyTex_[logical].Get(), 0, 0, 0, 0,
			reinterpret_cast<ID3D11Texture2D*>(originals_[logical].texture), 0, &box);
		filledFrame_[logical] = FrameCount();
		filledValid_[logical] = true;
	}

	void ProxyTargets::ResetOne(std::int32_t logical, bool doCopy)
	{
		auto* engineTarget = ResolveTarget(logical);
		if (!engineTarget || !originals_[logical].texture || !proxyTex_[logical]) {
			return;
		}
		if (doCopy) {
			filledValid_[logical] = false;
			D3D11_TEXTURE2D_DESC proxyDesc{};
			proxyTex_[logical]->GetDesc(&proxyDesc);
			const D3D11_BOX box{ 0, 0, 0, proxyDesc.Width, proxyDesc.Height, 1 };
			if (auto* ctx = EngineContext(); ctx) {
				ctx->CopySubresourceRegion(
					reinterpret_cast<ID3D11Texture2D*>(originals_[logical].texture), 0, 0, 0, 0,
					proxyTex_[logical].Get(), 0, &box);
			}
		}
		*engineTarget = originals_[logical];
	}

	void ProxyTargets::OverrideRenderTargets(std::initializer_list<std::int32_t> toCopy)
	{
		if (!hasProxies_) {
			return;
		}
		for (const auto target : kProxied) {
			OverrideOne(target, Contains(toCopy, target));
		}
		auto* mgr = Manager();
		const float widthRatio = MgrRatioW();
		const float heightRatio = MgrRatioH();
		// Report the scaled dimensions so callers that query
		// render-target sizes stay correct while overridden.
		if (!propsSaved_) {
			for (std::uint32_t i = 0; i < 100; ++i) {
				savedPropsW_[i] = *reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20);
				savedPropsH_[i] = *reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20 + 4);
			}
			propsSaved_ = true;
		}
		for (std::uint32_t i = 0; i < 100; ++i) {
			*reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20) =
				static_cast<std::uint32_t>(static_cast<float>(savedPropsW_[i]) * widthRatio);
			*reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20 + 4) =
				static_cast<std::uint32_t>(static_cast<float>(savedPropsH_[i]) * heightRatio);
		}
		// Repoint any pixel-shader SRV already bound from an
		// original target to its proxy.
		auto* ctx = EngineContext();
		if (ctx) {
			ID3D11ShaderResourceView* bound[16] = {};
			ctx->PSGetShaderResources(0, 16, bound);
			for (std::uint32_t slot = 0; slot < 16; ++slot) {
				if (!bound[slot]) {
					continue;
				}
				for (const auto target : kProxied) {
					auto* origSrv = reinterpret_cast<ID3D11ShaderResourceView*>(originals_[target].srView);
					auto* proxySrv = proxySrv_[target].Get();
					if (bound[slot] == origSrv && proxySrv) {
						ctx->PSSetShaderResources(slot, 1, &proxySrv);
						break;
					}
				}
				bound[slot]->Release();
			}
		}
		*reinterpret_cast<bool*>(mgr + kVpFlagOffset) = false;
		overridden_ = true;
	}

	void ProxyTargets::ResetRenderTargets(std::initializer_list<std::int32_t> toCopy)
	{
		if (!overridden_) {
			return;
		}
		for (const auto target : kProxied) {
			ResetOne(target, toCopy.size() == 0 || Contains(toCopy, target));
		}
		auto* mgr = Manager();
		if (propsSaved_) {
			for (std::uint32_t i = 0; i < 100; ++i) {
				*reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20) = savedPropsW_[i];
				*reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20 + 4) = savedPropsH_[i];
			}
			propsSaved_ = false;
		}
		// EndProxyPass restores actual bindings after all structs/properties
		// are back, covering every shader stage rather than only PS t0..15.
		*reinterpret_cast<bool*>(mgr + kVpFlagOffset) = true;
		overridden_ = false;
	}

	void ProxyTargets::RestoreBoundViews()
	{
		auto* ctx = EngineContext();
		if (!ctx) return;
		// Engine slot restoration does not change D3D11 bindings. A later
		// pass may keep the same logical target and skip an OM rebind.
		// Replace only our views; preserve unrelated MRTs, DSV and UAVs.
		ID3D11RenderTargetView* heldRtvs[8]{};
		ID3D11RenderTargetView* rtvs[8]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(8, heldRtvs, &dsv);
		bool changed = false;
		UINT count = 0;
		for (UINT i = 0; i < 8; ++i) {
			rtvs[i] = heldRtvs[i];
			if (!rtvs[i]) continue;
			count = i + 1;
			for (const auto target : kProxied) {
				if (captured_[target] && rtvs[i] == proxyRtv_[target].Get()) {
					rtvs[i] = reinterpret_cast<ID3D11RenderTargetView*>(originals_[target].rtView);
					changed = true;
					break;
				}
			}
		}
		if (changed) ctx->OMSetRenderTargetsAndUnorderedAccessViews(count, rtvs, dsv,
			0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
		for (auto* view : heldRtvs) if (view) view->Release();
		if (dsv) dsv->Release();

		// Restore bindings outside PS t0..t15 too, including the depth
		// override. Get* references are released separately from replacements.
		auto restoreSrvs = [&](auto get, auto set) {
			ID3D11ShaderResourceView* held[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
			(ctx->*get)(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, held);
			for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
				if (!held[i]) continue;
				auto* replacement = held[i];
				if (depthOverridden_ && held[i] == depthSrv_.Get()) {
					replacement = reinterpret_cast<ID3D11ShaderResourceView*>(savedDepthSrv_);
				} else {
					for (const auto target : kProxied) {
						if (captured_[target] && held[i] == proxySrv_[target].Get()) {
							replacement = reinterpret_cast<ID3D11ShaderResourceView*>(originals_[target].srView);
							break;
						}
					}
				}
				if (replacement != held[i]) (ctx->*set)(i, 1, &replacement);
				held[i]->Release();
			}
		};
		restoreSrvs(&ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::VSSetShaderResources);
		restoreSrvs(&ID3D11DeviceContext::HSGetShaderResources, &ID3D11DeviceContext::HSSetShaderResources);
		restoreSrvs(&ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::DSSetShaderResources);
		restoreSrvs(&ID3D11DeviceContext::GSGetShaderResources, &ID3D11DeviceContext::GSSetShaderResources);
		restoreSrvs(&ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::PSSetShaderResources);
		restoreSrvs(&ID3D11DeviceContext::CSGetShaderResources, &ID3D11DeviceContext::CSSetShaderResources);
	}

	bool ProxyTargets::CompileDepthShaders(ID3D11Device* device)
	{
		if (overrideDepthCs_ && linearDepthCs_) {
			return true;
		}
		// community-shaders' OverrideDepthCS: region copy of the
		// engine depth into the render-size override texture.
		static constexpr const char* kOverrideSrc = R"(
Texture2D<float> DepthInput : register(t0);
RWTexture2D<float> DepthOutput : register(u0);
cbuffer Upscaling : register(b0)
{
	uint2 ScreenSize;
	uint2 RenderSize;
	float4 CameraData;
};
[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= RenderSize))
		return;
	DepthOutput[dispatchID.xy] = DepthInput[dispatchID.xy];
}
)";
		// Their OverrideLinearDepthCS: linear view depth at screen
		// size (for kSAOCameraZ consumers), sampled from the region.
		static constexpr const char* kLinearSrc = R"(
Texture2D<float> DepthInput : register(t0);
RWTexture2D<float> LinearDepthOutput : register(u0);
cbuffer Upscaling : register(b0)
{
	uint2 ScreenSize;
	uint2 RenderSize;
	float4 CameraData;
};
float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}
[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= ScreenSize))
		return;
	float depth = DepthInput[dispatchID.xy * RenderSize / ScreenSize];
	LinearDepthOutput[dispatchID.xy] = GetScreenDepth(depth);
}
)";
		auto compile = [&](const char* src, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& out) {
			Microsoft::WRL::ComPtr<ID3DBlob> blob;
			Microsoft::WRL::ComPtr<ID3DBlob> errors;
			const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
				"main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				blob.GetAddressOf(), errors.GetAddressOf());
			if (FAILED(hr)) {
				return false;
			}
			return SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out.GetAddressOf()));
		};
		return compile(kOverrideSrc, overrideDepthCs_) && compile(kLinearSrc, linearDepthCs_);
	}

	bool ProxyTargets::CopyDepth()
	{
		auto* ctx = EngineContext();
		auto* mainDepth = static_cast<RE::BSGraphics::DepthStencilTarget*>(ResolveDepthTarget(logical::kDepthMain));
		if (!ctx || !mainDepth || !depthTex_ || !depthUav_) {
			return false;
		}
		auto* depthSrvIn = reinterpret_cast<ID3D11ShaderResourceView*>(mainDepth->srViewDepth);
		if (!depthSrvIn || !overrideDepthCs_ || !linearDepthCs_) {
			return false;
		}
		auto* saoTarget = ResolveTarget(logical::kSAOCameraZ);
		auto* saoUav = saoTarget ? reinterpret_cast<ID3D11UnorderedAccessView*>(saoTarget->uaView) : nullptr;

		const auto* state = reinterpret_cast<const std::byte*>(REL::ID(kStateId).address());
		const std::uint32_t screenW = *reinterpret_cast<const std::uint32_t*>(state + 0x80);
		const std::uint32_t screenH = *reinterpret_cast<const std::uint32_t*>(state + 0x84);
		if (screenW == 0 || screenH == 0) {
			return false;
		}
		const float ratioW = MgrRatioW();
		const float ratioH = MgrRatioH();
		const std::uint32_t renderW = static_cast<std::uint32_t>(static_cast<float>(screenW) * ratioW);
		const std::uint32_t renderH = static_cast<std::uint32_t>(static_cast<float>(screenH) * ratioH);
		if (renderW == 0 || renderH == 0) {
			return false;
		}

		if (!upscalingCb_) {
			auto* device = EngineDevice();
			if (!device) {
				return false;
			}
			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = 32;
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, upscalingCb_.GetAddressOf()))) {
				return false;
			}
		}
		{
			struct CB
			{
				std::uint32_t screen[2];
				std::uint32_t render[2];
				float camera[4];
			};
			const float nearZ = *reinterpret_cast<const float*>(REL::ID(kCameraNearId).address());
			const float farZ = *reinterpret_cast<const float*>(REL::ID(kCameraFarId).address());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(ctx->Map(upscalingCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return false;
			}
			auto* cb = static_cast<CB*>(mapped.pData);
			cb->screen[0] = screenW;
			cb->screen[1] = screenH;
			cb->render[0] = renderW;
			cb->render[1] = renderH;
			// GetScreenDepth(d) = w / (-d*z + x) with x = far,
			// z = far - near, w = near * far (standard projection).
			cb->camera[0] = farZ;
			cb->camera[1] = 0.0f;
			cb->camera[2] = farZ - nearZ;
			cb->camera[3] = nearZ * farZ;
			ctx->Unmap(upscalingCb_.Get(), 0);
		}

		// Save the bindings this disturbs (OM + the CS slots used).
		ID3D11RenderTargetView* savedRtv = nullptr;
		ID3D11DepthStencilView* savedDsv = nullptr;
		ctx->OMGetRenderTargets(1, &savedRtv, &savedDsv);
		ID3D11ComputeShader* savedCs = nullptr;
		ctx->CSGetShader(&savedCs, nullptr, nullptr);
		ID3D11ShaderResourceView* savedSrv = nullptr;
		ctx->CSGetShaderResources(0, 1, &savedSrv);
		ID3D11UnorderedAccessView* savedUav = nullptr;
		ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
		ID3D11Buffer* savedCb = nullptr;
		ctx->CSGetConstantBuffers(0, 1, &savedCb);

		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		auto* cbPtr = upscalingCb_.Get();
		ctx->CSSetConstantBuffers(0, 1, &cbPtr);
		ID3D11ShaderResourceView* views[] = { depthSrvIn };
		ctx->CSSetShaderResources(0, 1, views);
		if (saoUav) {
			ID3D11UnorderedAccessView* uavs[] = { saoUav };
			ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			ctx->CSSetShader(linearDepthCs_.Get(), nullptr, 0);
			ctx->Dispatch((screenW + 7) / 8, (screenH + 7) / 8, 1);
		}
		{
			ID3D11UnorderedAccessView* uavs[] = { depthUav_.Get() };
			ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			ctx->CSSetShader(overrideDepthCs_.Get(), nullptr, 0);
			ctx->Dispatch((renderW + 7) / 8, (renderH + 7) / 8, 1);
		}
		ID3D11ShaderResourceView* nullViews[] = { nullptr };
		ID3D11UnorderedAccessView* nullUavs[] = { nullptr };
		ctx->CSSetShaderResources(0, 1, nullViews);
		ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

		ctx->CSSetShader(savedCs, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &savedSrv);
		ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
		ctx->CSSetConstantBuffers(0, 1, &savedCb);
		ctx->OMSetRenderTargets(1, &savedRtv, savedDsv);
		if (savedRtv) savedRtv->Release();
		if (savedDsv) savedDsv->Release();
		if (savedCs) savedCs->Release();
		if (savedSrv) savedSrv->Release();
		if (savedUav) savedUav->Release();
		if (savedCb) savedCb->Release();
		return true;
	}

	void ProxyTargets::OverrideDepth(bool doCopy)
	{
		if (!depthTex_ || !overrideDepthCs_ || !linearDepthCs_) {
			return;  // fail closed: without the shaders the override is never populated
		}
		auto* mainDepth = static_cast<RE::BSGraphics::DepthStencilTarget*>(ResolveDepthTarget(logical::kDepthMain));
		if (!mainDepth) {
			return;
		}
		savedDepthSrv_ = mainDepth->srViewDepth;
		if (doCopy) {
			if (!(depthCopyValid_ && depthCopyFrame_ == FrameCount())) {
				if (!CopyDepth()) {
					return;
				}
				depthCopyFrame_ = FrameCount();
				depthCopyValid_ = true;
			}
		}
		mainDepth->srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(depthSrv_.Get());
		depthOverridden_ = true;
	}

	void ProxyTargets::ResetDepth()
	{
		if (!depthOverridden_) {
			return;
		}
		depthOverridden_ = false;
		auto* mainDepth = static_cast<RE::BSGraphics::DepthStencilTarget*>(ResolveDepthTarget(logical::kDepthMain));
		if (mainDepth) {
			mainDepth->srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(savedDepthSrv_);
		}
	}

	bool ProxyTargets::ProxyPassAvailable() const
	{
		return !proxyPass_ && hasProxies_ && (MgrRatioW() != 1.0f || MgrRatioH() != 1.0f);
	}

	void ProxyTargets::ApplyTriangleSwap()
	{
		if (triSwapped_ || !ism_) {
			return;
		}
		auto* ism = static_cast<std::byte*>(ism_);
		void* unit = *reinterpret_cast<void**>(ism + 0x28);
		void* dyn = *reinterpret_cast<void**>(ism + 0x38);
		if (unit && dyn && unit != dyn) {
			triSaved_ = dyn;
			*reinterpret_cast<void**>(ism + 0x38) = unit;
			triSwapped_ = true;
		}
	}

	void ProxyTargets::RestoreTriangleSwap()
	{
		if (!triSwapped_ || !ism_) {
			triSwapped_ = false;
			return;
		}
		auto* ism = static_cast<std::byte*>(ism_);
		*reinterpret_cast<void**>(ism + 0x38) = triSaved_;
		triSwapped_ = false;
		triSaved_ = nullptr;
	}

	bool ProxyTargets::BeginProxyPass(std::initializer_list<std::int32_t> toProxy, ProxyTriangle triangle)
	{
		if (!ProxyPassAvailable()) {
			return false;
		}
		// Set first so EndProxyPass unwinds a partially applied override.
		proxyPass_ = true;
		passRatioW_ = MgrRatioW();
		passRatioH_ = MgrRatioH();
		OverrideRenderTargets(toProxy);
		OverrideDepth(true);
		WriteMgrState(1.0f, 1.0f, false);
		if (triangle == ProxyTriangle::kUnit) {
			ApplyTriangleSwap();
		}
		return true;
	}

	void ProxyTargets::EndProxyPass(std::initializer_list<std::int32_t> fromProxy)
	{
		if (!proxyPass_) {
			return;
		}
		proxyPass_ = false;
		RestoreTriangleSwap();
		ResetRenderTargets(fromProxy);
		RestoreBoundViews();
		ResetDepth();
		if (hasProxies_) {
			WriteMgrState(passRatioW_, passRatioH_, true);
		}
	}

	void ProxyTargets::ProxyAcquiredRenderTarget(std::int32_t logical)
	{
		auto* engineTarget = ResolveTarget(logical);
		if (!proxyPass_ || !engineTarget || !engineTarget->texture) {
			return;
		}
		for (const auto target : kProxied) {
			if (proxyTex_[target] && engineTarget->texture == proxyTex_[target].Get()) {
				return;  // already a proxy texture
			}
		}
		if (proxyTex_[logical] && engineTarget->texture == proxyTex_[logical].Get()) {
			return;
		}
		auto* liveTex = reinterpret_cast<ID3D11Texture2D*>(engineTarget->texture);
		D3D11_TEXTURE2D_DESC liveDesc{};
		liveTex->GetDesc(&liveDesc);
		const auto wantW = static_cast<std::uint32_t>(static_cast<float>(liveDesc.Width) * passRatioW_);
		const auto wantH = static_cast<std::uint32_t>(static_cast<float>(liveDesc.Height) * passRatioH_);
		bool reusable = false;
		if (proxyTex_[logical]) {
			D3D11_TEXTURE2D_DESC proxyDesc{};
			proxyTex_[logical]->GetDesc(&proxyDesc);
			reusable = proxyDesc.Width == wantW && proxyDesc.Height == wantH && proxyDesc.Format == liveDesc.Format;
		}
		if (reusable) {
			// Pooled engine fields may differ per acquire; keep our
			// own texture and views.
			CaptureOriginal(logical, *engineTarget);
		} else {
			ReleaseProxy(logical);
			UpdateRenderTargetOne(logical, passRatioW_, passRatioH_);
		}
		OverrideOne(logical, false);
	}

	void ProxyTargets::ReleaseAcquiredRenderTarget(std::int32_t logical)
	{
		RestoreEngineSlot(logical);
		captured_[logical] = false;
		originals_[logical] = {};
		for (auto& ref : originalRefs_[logical]) ref.Reset();
	}

	void ProxyTargets::AbortProxyPass()
	{
		RestoreTriangleSwap();
		for (const auto target : kProxied) {
			RestoreEngineSlot(target);
		}
		for (const auto target : kPooled) {
			RestoreEngineSlot(target);
		}
		if (overridden_) {
			auto* mgr = Manager();
			if (propsSaved_) {
				for (std::uint32_t i = 0; i < 100; ++i) {
					*reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20) = savedPropsW_[i];
					*reinterpret_cast<std::uint32_t*>(mgr + kRtDataOffset + i * 0x20 + 4) = savedPropsH_[i];
				}
				propsSaved_ = false;
			}
			*reinterpret_cast<bool*>(mgr + kVpFlagOffset) = true;
			overridden_ = false;
		}
		proxyPass_ = false;
		for (std::uint32_t i = 0; i < std::size(filledValid_); ++i) {
			filledValid_[i] = false;
		}
		if (depthOverridden_) {
			auto* mainDepth = static_cast<RE::BSGraphics::DepthStencilTarget*>(ResolveDepthTarget(logical::kDepthMain));
			if (mainDepth) {
				mainDepth->srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(savedDepthSrv_);
			}
			depthOverridden_ = false;
		}
	}

	void ProxyTargets::Release()
	{
		AbortProxyPass();
		for (const auto target : kProxied) {
			ReleaseProxy(target);
			captured_[target] = false;
			originals_[target] = {};
		}
		for (const auto target : kPooled) {
			ReleaseProxy(target);
			captured_[target] = false;
			originals_[target] = {};
		}
		savedDepthSrv_ = nullptr;
		depthTex_.Reset();
		depthSrv_.Reset();
		depthUav_.Reset();
		depthCopyValid_ = false;
		prevRatioW_ = -1.0f;
		prevRatioH_ = -1.0f;
		hasProxies_ = false;
	}

	void* ProxyTargets::DebugSlotTexture(std::int32_t logical)
	{
		auto* target = ResolveTarget(logical);
		return target ? target->texture : nullptr;
	}

	void* ProxyTargets::DebugOriginalTexture(std::int32_t logical)
	{
		return captured_[logical] ? originals_[logical].texture : nullptr;
	}

	void* ProxyTargets::DebugProxyTexture(std::int32_t logical)
	{
		return proxyTex_[logical].Get();
	}
}
