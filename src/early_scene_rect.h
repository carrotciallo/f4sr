#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <iterator>

namespace f4sr::early_scene {
using Microsoft::WRL::ComPtr;
using Ctx=ID3D11DeviceContext;
struct Rect { UINT w{},h{}; };
// Why a producer draw was not recorded (logged with the t5 crop counters).
inline struct Rejects { unsigned long long shape{}, viewport{}, notReduced{}, output{}, fractional{}; } rejects;
inline bool Same(Rect a,Rect b) { return a.w==b.w && a.h==b.h; }
inline Rect Viewport(Ctx* c) {
	D3D11_VIEWPORT v[2]{}; UINT n=2; c->RSGetViewports(&n,v);
	if(n!=1 || v[0].TopLeftX!=0 || v[0].TopLeftY!=0 || !(v[0].Width>=1 && v[0].Width<=16384 && v[0].Height>=1 && v[0].Height<=16384)) return {};
	// Ratios other than 1/2 (Balanced 1/1.7, Quality 1/1.5) can give a
	// fractional dynamic viewport; the covered pixel extent is its ceiling.
	Rect r{UINT(std::ceil(v[0].Width)),UINT(std::ceil(v[0].Height))};
	if(r.w!=v[0].Width || r.h!=v[0].Height) ++rejects.fractional;
	return r;
}
inline ComPtr<ID3D11Texture2D> Output(Ctx* c) {
	ComPtr<ID3D11RenderTargetView> v; c->OMGetRenderTargets(1,&v,nullptr); if(!v) return {};
	D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d);
	if(d.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || d.Texture2D.MipSlice || d.Format!=DXGI_FORMAT_R11G11B10_FLOAT) return {};
	ComPtr<ID3D11Resource> r; v->GetResource(&r); ComPtr<ID3D11Texture2D> t; r.As(&t); return t;
}
inline bool SceneTexture(ID3D11Texture2D* tex,Rect extent) {
	if(!tex) return false;
	D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
	return d.Width==extent.w && d.Height==extent.h && d.Format==DXGI_FORMAT_R11G11B10_FLOAT && d.ArraySize==1 && d.SampleDesc.Count==1 && d.MipLevels>=1;
}
struct Crop {
	ComPtr<ID3D11Device> owner;
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> view;
	Rect extent{};
	DXGI_FORMAT format{}, viewFormat{};
	bool Prepare(Ctx* c,Rect r,DXGI_FORMAT res=DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT typed=DXGI_FORMAT_R11G11B10_FLOAT) {
		ComPtr<ID3D11Device> dev; c->GetDevice(&dev); if(!dev) return false;
		if(owner.Get()==dev.Get() && view && Same(r,extent) && format==res && viewFormat==typed) return true;
		D3D11_TEXTURE2D_DESC d{}; d.Width=r.w; d.Height=r.h; d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
		d.Format=res; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format=typed; vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels=1;
		ComPtr<ID3D11Texture2D> t; ComPtr<ID3D11ShaderResourceView> v;
		if(FAILED(dev->CreateTexture2D(&d,nullptr,&t)) || FAILED(dev->CreateShaderResourceView(t.Get(),&vd,&v))) return false;
		owner=dev; texture=t; view=v; extent=r; format=res; viewFormat=typed; return true;
	}
};
// Lifetime is one verified early RenderPassImmediately invocation. A texture
// seen last frame, or an unrelated six-vertex post pass, is not a producer.
// The producer site can run more than once in a pass (observed as rare
// "producer-only" passes, more frequent under camera motion); keep every
// qualifying output and let the consumer's t5 resource pick the one it reads.
struct Pass {
	Ctx* ctx{}; ID3D11Texture2D* dst{}; Rect full{},active{};
	struct Source { ComPtr<ID3D11Texture2D> tex; Rect rect{}; };
	Source sources[4]{};
	UINT count{};
	UINT indexed{}; bool used{};
	// Why the consumer was not rebound (last reason in this pass), for counters.
	enum class Miss : unsigned char { none, noConsumer, noSource, shape, view, crop } miss{Miss::noConsumer};
	void Produced(UINT indices,UINT start,INT base) {
		++indexed; if(indices!=3 || start || base) { ++rejects.shape; return; }
		const auto vp=Viewport(ctx); if(!vp.w) { ++rejects.viewport; return; }
		if(vp.w>=full.w || vp.h>=full.h) { ++rejects.notReduced; return; }
		auto t=Output(ctx); if(t.Get()==dst || !SceneTexture(t.Get(),full)) { ++rejects.output; return; }
		for(UINT i=0;i<count;++i) if(sources[i].tex.Get()==t.Get()) { sources[i].rect=vp; return; }
		if(count<std::size(sources)) { sources[count].tex=t; sources[count].rect=vp; ++count; }
	}
	bool Bind(UINT vertices,UINT start,Crop& cache,ComPtr<ID3D11ShaderResourceView>& saved) {
		if(used) return false;
		if(!count) { miss=Miss::noSource; return false; }
		if(vertices!=6 || start || Output(ctx).Get()!=dst) { miss=Miss::shape; return false; }
		D3D11_PRIMITIVE_TOPOLOGY top{}; ctx->IAGetPrimitiveTopology(&top);
		if(top!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) { miss=Miss::shape; return false; }
		ComPtr<ID3D11ShaderResourceView> v; ctx->PSGetShaderResources(5,1,&v); if(!v) { miss=Miss::view; return false; }
		ComPtr<ID3D11Resource> r; v->GetResource(&r);
		const Source* src=nullptr;
		for(UINT i=0;i<count;++i) if(r.Get()==sources[i].tex.Get()) src=&sources[i];
		const auto vp=Viewport(ctx);
		if(!src || !Same(src->rect,vp)) { miss=Miss::view; return false; }
		D3D11_SHADER_RESOURCE_VIEW_DESC d{}; v->GetDesc(&d);
		// Any view starting at mip 0 is replaced by the single-mip crop; a view
		// created with MipLevels -1 ("all") must not silently skip the fix.
		if(d.Format!=DXGI_FORMAT_R11G11B10_FLOAT || d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || d.Texture2D.MostDetailedMip) { miss=Miss::view; return false; }
		if(!cache.Prepare(ctx,src->rect)) { miss=Miss::crop; return false; }
		active=src->rect;
		D3D11_BOX box{0,0,0,active.w,active.h,1};
		ctx->CopySubresourceRegion(cache.texture.Get(),0,0,0,0,src->tex.Get(),0,&box);
		ctx->PSSetShaderResources(5,1,cache.view.GetAddressOf());
		saved=v; used=true; miss=Miss::none; return true;
	}
	// ENB's SSAO result reaches the same consumer at t3: a display-size
	// texture written with a full viewport, so its render-layout content sits
	// in the top-left active rectangle, while the consumer samples it relative
	// to its render-size viewport like t5. Without SSAO, t3 is a 1x1 constant
	// and is left alone. Call only after Bind succeeded for this draw.
	bool BindAo(Crop& cache,ComPtr<ID3D11ShaderResourceView>& saved) { return BindSlot(3,cache,saved); }
	// The composite also samples t0 (a G-buffer target the game wrote with the
	// render viewport; ENB derives the SSAO blend weight from its brightness)
	// relative to the viewport. Uncropped, the AO is skipped in a half-scale
	// copy of bright first-person geometry ("hand shadow"). Same treatment.
	bool BindSlot(UINT slot,Crop& cache,ComPtr<ID3D11ShaderResourceView>& saved) {
		ComPtr<ID3D11ShaderResourceView> v; ctx->PSGetShaderResources(slot,1,&v); if(!v) return false;
		ComPtr<ID3D11Resource> r; v->GetResource(&r); ComPtr<ID3D11Texture2D> t; if(!r || FAILED(r.As(&t))) return false;
		D3D11_TEXTURE2D_DESC td{}; t->GetDesc(&td);
		if(td.Width!=full.w || td.Height!=full.h || td.ArraySize!=1 || td.SampleDesc.Count!=1 || (td.BindFlags&D3D11_BIND_DEPTH_STENCIL)) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC d{}; v->GetDesc(&d);
		if(d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || d.Texture2D.MostDetailedMip) return false;
		if(!cache.Prepare(ctx,active,td.Format,d.Format)) return false;
		D3D11_BOX box{0,0,0,active.w,active.h,1};
		ctx->CopySubresourceRegion(cache.texture.Get(),0,0,0,0,t.Get(),0,&box);
		ctx->PSSetShaderResources(slot,1,cache.view.GetAddressOf());
		saved=v; return true;
	}
};
}
