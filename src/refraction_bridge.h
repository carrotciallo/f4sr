#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace f4sr
{
	// Output-space SRV for the refraction pass. Native physical rt1 is never
	// overwritten. Begin runs inside the resolve's OM/CS t0,u0/shader guard.
	class RefractionBridge
	{
		template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
		Ptr<ID3D11Texture2D> output_;
		Ptr<ID3D11UnorderedAccessView> uav_;
		Ptr<ID3D11ShaderResourceView> view_;
		Ptr<ID3D11ComputeShader> shader_;
		Ptr<ID3D11Buffer> cb_;
		Ptr<ID3D11Device> device_;
		ID3D11ShaderResourceView* original_{};
		UINT w_{}, h_{}, rw_{}, rh_{};
		bool failed_{};

		bool Prepare(ID3D11Device* device, UINT w, UINT h, UINT rw, UINT rh)
		{
			if (device_.Get()==device && w_==w && h_==h && rw_==rw && rh_==rh) return !failed_;
			*this = {}; device_=device; w_=w; h_=h; rw_=rw; rh_=rh; failed_=true;
			static constexpr char source[]=R"(
Texture2D<float3> field : register(t0);
RWTexture2D<float3> outputField : register(u0);
cbuffer Extents : register(b0) { uint2 render; uint2 display; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
 if(any(id.xy>=display)) return;
 float2 at=clamp((id.xy+0.5)*float2(render)/float2(display)-0.5,0,float2(render-1));
 uint2 a=(uint2)at, b=min(a+1,render-1); float2 f=frac(at);
 outputField[id.xy]=lerp(lerp(field.Load(int3(a,0)),field.Load(int3(b.x,a.y,0)),f.x),
                        lerp(field.Load(int3(a.x,b.y,0)),field.Load(int3(b,0)),f.x),f.y);
})";
			Ptr<ID3DBlob> code;
			if (FAILED(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,nullptr)) ||
				FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader_))) return false;
			D3D11_TEXTURE2D_DESC d{}; d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
			d.Format=DXGI_FORMAT_R11G11B10_FLOAT; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
			const UINT dims[]{rw,rh,w,h}; D3D11_SUBRESOURCE_DATA init{dims,0,0}; D3D11_BUFFER_DESC cb{};
			cb.ByteWidth=sizeof(dims); cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cb.Usage=D3D11_USAGE_IMMUTABLE;
			failed_=FAILED(device->CreateTexture2D(&d,nullptr,&output_)) || FAILED(device->CreateShaderResourceView(output_.Get(),nullptr,&view_)) ||
				FAILED(device->CreateUnorderedAccessView(output_.Get(),nullptr,&uav_)) || FAILED(device->CreateBuffer(&cb,&init,&cb_));
			return !failed_;
		}
	public:
		void Begin(ID3D11DeviceContext* ctx, RE::BSGraphics::RendererData* data, UINT w, UINT h, UINT rw, UINT rh)
		{
			if (original_ || !rw || !rh || rw>=w || rh>=h) return;
			auto& rt=data->renderTargets[1]; if (!rt.texture || !rt.srView || rt.copyTexture) return;
			auto* tex=reinterpret_cast<ID3D11Texture2D*>(rt.texture); auto* src=reinterpret_cast<ID3D11ShaderResourceView*>(rt.srView);
			D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d); D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; src->GetDesc(&vd);
			Ptr<ID3D11Resource> res; src->GetResource(&res);
			if (res.Get()!=static_cast<ID3D11Resource*>(tex) || d.Width!=w || d.Height!=h || d.Format!=DXGI_FORMAT_R11G11B10_FLOAT ||
				vd.Format!=d.Format || vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || vd.Texture2D.MostDetailedMip!=0 ||
				d.ArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1) return;
			Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
			const bool newLayout=device_.Get()!=dev.Get() || w_!=w || h_!=h || rw_!=rw || rh_!=rh;
			if (!Prepare(dev.Get(),w,h,rw,rh)) { if(newLayout) F4SE::log::warn("f4sr: refraction field bridge unavailable"); return; }
			Ptr<ID3D11Buffer> saved; ctx->CSGetConstantBuffers(0,1,&saved);
			ctx->CSSetConstantBuffers(0,1,cb_.GetAddressOf()); ctx->CSSetShaderResources(0,1,&src);
			ctx->CSSetUnorderedAccessViews(0,1,uav_.GetAddressOf(),nullptr); ctx->CSSetShader(shader_.Get(),nullptr,0);
			ctx->Dispatch((w+7)/8,(h+7)/8,1);
			ID3D11UnorderedAccessView* noUav{}; ID3D11ShaderResourceView* noSrv{};
			ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr); ctx->CSSetShaderResources(0,1,&noSrv); ctx->CSSetConstantBuffers(0,1,saved.GetAddressOf());
			original_=src; rt.srView=reinterpret_cast<decltype(rt.srView)>(view_.Get());
			if(newLayout) F4SE::log::info("f4sr: refraction field rt1 {:p}->{:p} {}x{} -> {}x{}",(void*)tex,(void*)output_.Get(),rw,rh,w,h);
		}
		void End()
		{
			if (!original_) return;
			auto* data=RE::BSGraphics::RendererData::GetSingleton(); auto& rt=data->renderTargets[1];
			if (reinterpret_cast<ID3D11ShaderResourceView*>(rt.srView)==view_.Get()) rt.srView=reinterpret_cast<decltype(rt.srView)>(original_);
			auto* ctx=reinterpret_cast<ID3D11DeviceContext*>(data->context);
			// Avoid leaving a cached PS view from this scope at the observed t1 binding.
			if (ctx) { Ptr<ID3D11ShaderResourceView> bound; ctx->PSGetShaderResources(1,1,&bound);
				if(bound.Get()==view_.Get()) { auto* native=reinterpret_cast<ID3D11ShaderResourceView*>(rt.srView); ctx->PSSetShaderResources(1,1,&native); } }
			original_=nullptr;
		}
	};
}
