#include "late_draw_trace.h"
#include "early_scene_rect.h"
#include "post_depth_remap.h"
#include "frame_trace.h"
#include <intrin.h>
#include <psapi.h>
#include <filesystem>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <d3d11_1.h>
#include <fstream>
#include <string>

namespace f4sr::late_trace
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using Ctx = ID3D11DeviceContext;
		constexpr UINT ny = 54, maxEvents = 64, maxSamples = 160;
		struct Window {
			Ctx* ctx{};
			ComPtr<ID3D11Texture2D> main, earlyCopy;
			std::ofstream events, pixels;
			UINT frame{}, draws{}, samples{};
			bool busy{}, post{}, motion{};
			UINT inspected{}, earlyWrites{};
			std::array<ID3D11PixelShader*,3> witnessed{};
		};
		// Only the render thread owns a window; calls on other contexts/threads
		// go straight to the exact method that was displaced (including ENB).
		thread_local Window* active{};
		Window w;
		void** table{};
		void* originals[59]{};
		bool attempted[3]{}, tapped{}, unsafeToReopen{};
		UINT RowLimit() { return w.post ? 512u : maxSamples; }
		auto address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

		UINT Bytes(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
			case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R32_FLOAT: return 4;
			case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: return 2;
			case DXGI_FORMAT_R8_UNORM: return 1;
			default: return 0;
			}
		}

		// Preserve every X at y=floor(i*height/rows). No filtering, texture
		// rebinding, mip generation, or allocation-sized CPU readback.
		void Rows(UINT seq, const char* role, UINT slot, ID3D11Texture2D* tex, UINT mip, UINT dy=0, Ctx* source=nullptr)
		{
			auto* ctx=source?source:w.ctx;
			D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
			const auto b = Bytes(d.Format);
			if (!b || d.ArraySize != 1 || mip >= d.MipLevels || mip >= 32 || d.SampleDesc.Count != 1 ||
				(d.BindFlags & D3D11_BIND_DEPTH_STENCIL) || d.Width > 8192 || w.samples >= RowLimit()) return;
			const UINT width = (std::max)(1u, d.Width >> mip), height = (std::max)(1u, d.Height >> mip);
			const UINT rows = (std::min)(w.post ? 18u : ny, height);
			D3D11_TEXTURE2D_DESC sd{}; sd.Width = width; sd.Height = rows; sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
			sd.Format = d.Format; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
			ComPtr<ID3D11Texture2D> stage;
			if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) return;
			for (UINT i = 0; i < rows; ++i) {
				const UINT y = (std::min)(height-1, i * height / rows + dy);
				D3D11_BOX box{0, y, 0, width, y+1, 1};
				ctx->CopySubresourceRegion(stage.Get(), 0, 0, i, 0, tex, mip, &box);
			}
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
			const auto offset = w.pixels.tellp();
			if (m.pData && m.RowPitch >= width*b) {
				for (UINT i = 0; i < rows; ++i) w.pixels.write(static_cast<const char*>(m.pData) + i*std::size_t(m.RowPitch), width*b);
			}
			ctx->Unmap(stage.Get(), 0);
			if (w.pixels && w.pixels.tellp()-offset == std::streamoff(rows)*width*b) {
				++w.samples;
				w.events << "rows " << seq << ' ' << role << ' ' << slot << ' ' << address(tex) << ' ' << d.Format << ' ' << mip << ' ' << width << ' ' << height << ' ' << rows << ' ' << b << ' ' << offset << '\n';
			}
		}

		void Stack(UINT seq)
		{
			void* pc[24]{};
			const auto n = CaptureStackBackTrace(1, 24, pc, nullptr);
			for (USHORT i=0; i<n; ++i) {
				HMODULE module{}; char name[MAX_PATH]{};
				if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<const char*>(pc[i]), &module) || !GetModuleFileNameA(module,name,MAX_PATH)) continue;
				w.events << "pc " << seq << ' ' << i << ' ' << address(pc[i])-address(module) << ' ' << name << '\n';
			}
		}

		bool Observe(UINT seq, UINT method)
		{
			auto* ctx=w.ctx;
			w.events << "event " << seq << ' ' << method << '\n'; Stack(seq);
			D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{}; UINT nv=std::size(vp);
			ctx->RSGetViewports(&nv,vp);
			for (UINT i=0;i<nv;++i) w.events << "vp " << seq << ' ' << i << ' ' << vp[i].TopLeftX << ' ' << vp[i].TopLeftY << ' ' << vp[i].Width << ' ' << vp[i].Height << '\n';
			ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps,nullptr,nullptr);
			w.events << "ps " << seq << ' ' << address(ps.Get()) << '\n';
			bool writesMain=false;
			ID3D11RenderTargetView* outs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ctx->OMGetRenderTargets(std::size(outs),outs,nullptr);
			for (UINT i=0;i<std::size(outs);++i) if (outs[i]) {
				ComPtr<ID3D11RenderTargetView> view; view.Attach(outs[i]);
				ComPtr<ID3D11Resource> res; view->GetResource(&res);
				ComPtr<ID3D11Texture2D> tex; res.As(&tex);
				writesMain |= tex.Get()==w.main.Get();
				if (w.post && tex && tex.Get()!=w.main.Get()) {
					D3D11_RENDER_TARGET_VIEW_DESC vd{}; view->GetDesc(&vd);
					if (vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D) Rows(seq,"target-before",i,tex.Get(),vd.Texture2D.MipSlice);
				}
				w.events << "rt " << seq << ' ' << i << ' ' << address(view.Get()) << ' ' << address(tex.Get()) << '\n';
			}
			// Command lists are opaque; an observed difference across one is NOT
			// attributable to the immediate-context PS. Dispatch uses CS bindings.
			const bool compute = method==41 || method==42, opaque=method==58;
			if (writesMain || compute || opaque) Rows(seq,"before",0,w.main.Get(),0);
			if (!opaque) {
				ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
				if (compute) ctx->CSGetShaderResources(0,std::size(views),views);
				else ctx->PSGetShaderResources(0,std::size(views),views);
				for (UINT i=0;i<std::size(views);++i) if (views[i]) {
					ComPtr<ID3D11ShaderResourceView> view; view.Attach(views[i]);
					ComPtr<ID3D11Resource> res; view->GetResource(&res);
					D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; view->GetDesc(&vd);
					ComPtr<ID3D11Texture2D> tex; res.As(&tex); D3D11_TEXTURE2D_DESC d{}; if(tex) tex->GetDesc(&d);
					w.events << "srv " << seq << ' ' << i << ' ' << address(view.Get()) << ' ' << address(res.Get()) << ' ' << address(tex.Get()) << ' ' << vd.Format << ' ' << vd.ViewDimension << ' ' << d.Width << ' ' << d.Height << '\n';
					if(tex && vd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D) Rows(seq,compute?"cs":"ps",i,tex.Get(),vd.Texture2D.MostDetailedMip);
				}
			}
			return writesMain || compute || opaque;
		}

		// Read the bound bytes, not merely the CPU camera cache. 11.1 binding
		// ranges matter for engines that suballocate from a large constant buffer.
		void MotionBuffers(UINT seq, Ctx* ctx, bool vs)
		{
			ComPtr<ID3D11DeviceContext1> c1; ctx->QueryInterface(IID_PPV_ARGS(&c1));
			ComPtr<ID3D11Device> device; ctx->GetDevice(&device);
			ID3D11Buffer* buffers[14]{}; UINT first[14]{}, count[14]{};
			if(c1 && vs) c1->VSGetConstantBuffers1(0,14,buffers,first,count);
			else if(c1) c1->PSGetConstantBuffers1(0,14,buffers,first,count);
			else if(vs) ctx->VSGetConstantBuffers(0,14,buffers); else ctx->PSGetConstantBuffers(0,14,buffers);
			for(UINT i=0;i<14;++i) if(buffers[i]) {
				ComPtr<ID3D11Buffer> cb; cb.Attach(buffers[i]); D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
				const UINT off=first[i]*16; if(off>=d.ByteWidth) continue;
				const UINT sz=(std::min)({65536u,d.ByteWidth-off,c1?count[i]*16:65536u}); if(!sz) continue;
				if(d.ByteWidth>4u*1024*1024) continue;
				D3D11_BUFFER_DESC sd{}; sd.ByteWidth=d.ByteWidth; sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
				ComPtr<ID3D11Buffer> staging; if(FAILED(device->CreateBuffer(&sd,nullptr,&staging))) continue;
				ctx->CopyResource(staging.Get(),cb.Get());
				D3D11_MAPPED_SUBRESOURCE m{}; if(FAILED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m))) continue;
				const auto pos=w.pixels.tellp(); w.pixels.write(static_cast<const char*>(m.pData)+off,sz); ctx->Unmap(staging.Get(),0);
				w.events << "cb " << seq << ' ' << (vs?"vs":"ps") << ' ' << i << ' ' << address(cb.Get()) << ' ' << off << ' ' << sz << ' ' << pos << '\n';
			}
		}
		// One real DrawIndexed only. Record the input state seen by the caller;
		// it need not be the final state used inside a graphics wrapper.
		ComPtr<ID3D11Query> EarlyDrawState(UINT seq, Ctx* ctx, UINT indices, UINT start, INT base)
		{
			D3D11_PRIMITIVE_TOPOLOGY topology{}; ctx->IAGetPrimitiveTopology(&topology);
			w.events << "early-ia " << seq << ' ' << topology << ' ' << indices << ' ' << start << ' ' << base << '\n';
			ComPtr<ID3D11BlendState> blend; FLOAT factors[4]{}; UINT mask{};
			ctx->OMGetBlendState(&blend,factors,&mask);
			D3D11_BLEND_DESC bd{};
			if(blend) blend->GetDesc(&bd);
			const auto& rt=bd.RenderTarget[0];
			w.events << "early-blend " << seq << ' ' << bool(blend.Get()) << ' ' << rt.BlendEnable << ' ' << rt.SrcBlend << ' ' << rt.DestBlend << ' ' << rt.BlendOp << ' ' << UINT(rt.RenderTargetWriteMask) << ' ' << mask;
			for(auto f:factors) w.events << ' ' << f;
			w.events << '\n'; // null object means D3D11 default; ignore zero descriptor fields
			for(UINT slot=0;slot<D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;++slot) {
				ComPtr<ID3D11SamplerState> s; ctx->PSGetSamplers(slot,1,&s); if(!s) continue;
				D3D11_SAMPLER_DESC d{}; s->GetDesc(&d);
				w.events << "early-sampler " << seq << ' ' << slot << ' ' << d.Filter << ' ' << d.AddressU << ' ' << d.AddressV << ' ' << d.MipLODBias << ' ' << d.MinLOD << ' ' << d.MaxLOD << ' ' << d.MaxAnisotropy << '\n';
			}
			// Pair the 0x08 candidate's existing source rows with y+1 so a
			// bilinear half-scale prediction need not invent vertical samples.
			for(UINT slot:{0u,4u,5u,6u}) {
				ComPtr<ID3D11ShaderResourceView> v; ctx->PSGetShaderResources(slot,1,&v); if(!v) continue;
				D3D11_SHADER_RESOURCE_VIEW_DESC d{}; v->GetDesc(&d); if(d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D) continue;
				ComPtr<ID3D11Resource> res; v->GetResource(&res); ComPtr<ID3D11Texture2D> tex;
				if(SUCCEEDED(res.As(&tex))) Rows(seq,"ps-y1",slot,tex.Get(),d.Texture2D.MostDetailedMip,1);
				w.events << "early-mips " << seq << ' ' << slot << ' ' << d.Texture2D.MostDetailedMip << ' ' << d.Texture2D.MipLevels << '\n';
			}
			ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev); ComPtr<ID3D11Query> q;
			D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS,0};
			const auto hr=dev->CreateQuery(&desc,&q);
			w.events << "early-query " << seq << ' ' << hr << '\n';
			if(SUCCEEDED(hr) && q) ctx->Begin(q.Get());
			return q;
		}
		bool Exchange(void** at, void* old, void* next);
#include "early_backend_trace.inl"
		bool MotionDraw(Ctx* ctx, UINT method)
		{
			if(w.draws>=3 || w.inspected>=8192 || (method!=12 && method!=20 && method!=13 && method!=21)) return false;
			++w.inspected; ID3D11RenderTargetView* rts[8]{}; ctx->OMGetRenderTargets(8,rts,nullptr); int slot=-1;
			for(UINT i=0;i<8;++i) if(rts[i]) { ComPtr<ID3D11RenderTargetView> v; v.Attach(rts[i]); ComPtr<ID3D11Resource> r; v->GetResource(&r); if(r.Get()==w.main.Get()) slot=i; }
			if(slot<0) return false;
			ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps,nullptr,nullptr);
			if(!ps || std::find(w.witnessed.begin(),w.witnessed.end(),ps.Get())!=w.witnessed.end()) return false;
			ComPtr<ID3D11VertexShader> vs; ctx->VSGetShader(&vs,nullptr,nullptr);
			w.witnessed[w.draws]=ps.Get(); const auto seq=++w.draws;
			w.events << "motionDraw " << seq << ' ' << method << ' ' << slot << ' ' << address(vs.Get()) << ' ' << address(ps.Get()) << '\n';
			MotionBuffers(seq,ctx,true); MotionBuffers(seq,ctx,false); Rows(seq,"mv-before",slot,w.main.Get(),0); return true;
		}
		template<UINT N, class... Args>
		void STDMETHODCALLTYPE Tap(Ctx* ctx, Args... args)
		{
			using Fn=void(STDMETHODCALLTYPE*)(Ctx*,Args...);
			const auto orig=reinterpret_cast<Fn>(originals[N]);
			if (!active || ctx!=w.ctx || w.busy || w.draws>=maxEvents ||
				reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address())->frameCount!=w.frame) { orig(ctx,args...); return; }
			w.busy=true;
			if(w.motion) {
				const bool sampled=MotionDraw(ctx,N);
				bool writer=false;
				if(w.earlyCopy && w.earlyWrites<16 && N!=41 && N!=42 && N!=58) {
					ComPtr<ID3D11RenderTargetView> rt; ctx->OMGetRenderTargets(1,&rt,nullptr); ComPtr<ID3D11Resource> res;
					if(rt) rt->GetResource(&res); writer=res.Get()==w.earlyCopy.Get();
				}
				const UINT seq=100+w.earlyWrites;
				ComPtr<ID3D11Query> stats;
				BackendScope backend;
				if(writer) {
					++w.earlyWrites; Rows(seq,"early-before",0,w.earlyCopy.Get(),0); Observe(seq,N);
					if constexpr (N==12) {
						void* pcs[16]{}; const auto n=CaptureStackBackTrace(0,16,pcs,nullptr);
						const auto site=reinterpret_cast<void*>(REL::ID(728427).address()+0x1f9);
						// Observe only: a shader returned by the wrapped live context
						// is not safe to bind to a separate replay context.
						if(std::find(pcs,pcs+n,site)!=pcs+n && seq==100) {
							MotionBuffers(seq,ctx,false);
							stats=EarlyDrawState(seq,ctx,args...);
							backend.Open(ctx,stats.Get());
						}
					}
				}
				orig(ctx,args...);
				backend.Close();
				if(stats) ctx->End(stats.Get());
				if(writer) Rows(seq,"early-after",0,w.earlyCopy.Get(),0);
				if(stats) {
					D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
					const auto hr=ctx->GetData(stats.Get(),&s,sizeof(s),D3D11_ASYNC_GETDATA_DONOTFLUSH);
					w.events << "early-stats " << seq << ' ' << hr;
					if(hr==S_OK) w.events << ' ' << s.IAVertices << ' ' << s.IAPrimitives << ' ' << s.VSInvocations << ' ' << s.PSInvocations << ' ' << s.CSInvocations;
					w.events << '\n'; // S_FALSE is missing evidence, not zero invocations
				}
				if(sampled) Rows(w.draws,"mv-after",0,w.main.Get(),0); w.busy=false; return;
			}
			const auto seq=++w.draws;
			const bool after=Observe(seq,N);
			orig(ctx,args...);
			if(after) Rows(seq,"after",0,w.main.Get(),0);
			if (w.post && N != 41 && N != 42 && N != 58) {
				ID3D11RenderTargetView* views[8]{}; ctx->OMGetRenderTargets(8,views,nullptr);
				for (UINT i=0;i<8;++i) if (views[i]) {
					ComPtr<ID3D11RenderTargetView> view; view.Attach(views[i]); ComPtr<ID3D11Resource> res; view->GetResource(&res);
					ComPtr<ID3D11Texture2D> tex; D3D11_RENDER_TARGET_VIEW_DESC vd{}; view->GetDesc(&vd);
					if (vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && SUCCEEDED(res.As(&tex)) && tex.Get()!=w.main.Get()) Rows(seq,"target-after",i,tex.Get(),vd.Texture2D.MipSlice);
				}
			}
			w.busy=false;
		}
		// Observe texture transfers without mistaking our own staging copies for game work.
		template<class Invoke>
		void Transfer(Ctx* ctx, UINT slot, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, UINT x, UINT y, UINT z, const D3D11_BOX* box, Invoke invoke)
		{
			if (!active || (!w.post && !(w.motion && w.earlyCopy && dst==w.earlyCopy.Get() && w.earlyWrites<16)) || w.busy || ctx!=w.ctx || w.draws>=maxEvents ||
				reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address())->frameCount!=w.frame) { invoke(); return; }
			ComPtr<ID3D11Texture2D> from,to;
			if (!src || !dst || FAILED(src->QueryInterface(IID_PPV_ARGS(&from))) || FAILED(dst->QueryInterface(IID_PPV_ARGS(&to)))) { invoke(); return; }
			w.busy=true; const UINT seq=w.motion?100+w.earlyWrites++:++w.draws;
			w.events << "event " << seq << ' ' << slot << '\n'; Stack(seq);
			w.events << "copy " << seq << ' ' << address(from.Get()) << ' ' << ss << ' ' << address(to.Get()) << ' ' << ds << '\n';
			w.events << "region " << seq << ' ' << x << ' ' << y << ' ' << z;
			if(box) w.events << ' ' << box->left << ' ' << box->top << ' ' << box->front << ' ' << box->right << ' ' << box->bottom << ' ' << box->back;
			else w.events << " full";
			w.events << '\n';
			Rows(seq,"copy-src",0,from.Get(),ss); Rows(seq,"copy-before",0,to.Get(),ds);
			invoke(); Rows(seq,"copy-after",0,to.Get(),ds); w.busy=false;
		}
		void STDMETHODCALLTYPE Copy(Ctx* c,ID3D11Resource* d,ID3D11Resource* s) {
			Transfer(c,47,d,0,s,0,0,0,0,nullptr,[&]{reinterpret_cast<decltype(&Copy)>(originals[47])(c,d,s);});
		}
		void STDMETHODCALLTYPE Region(Ctx* c,ID3D11Resource* d,UINT ds,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box) {
			Transfer(c,46,d,ds,s,ss,x,y,z,box,[&]{reinterpret_cast<decltype(&Region)>(originals[46])(c,d,ds,x,y,z,s,ss,box);});
		}
		// Published ID3D11DeviceContext ABI (d3d11.h). Never clone a vtable:
		// the device may expose longer context interfaces at the same address.
		struct Site { UINT slot; void* tap; };
		const Site sites[]{ {12,(void*)&Tap<12,UINT,UINT,INT>}, {13,(void*)&Tap<13,UINT,UINT>},
			{20,(void*)&Tap<20,UINT,UINT,UINT,INT,UINT>}, {21,(void*)&Tap<21,UINT,UINT,UINT,UINT>},
			{38,(void*)&Tap<38>}, {39,(void*)&Tap<39,ID3D11Buffer*,UINT>}, {40,(void*)&Tap<40,ID3D11Buffer*,UINT>},
			{41,(void*)&Tap<41,UINT,UINT,UINT>}, {42,(void*)&Tap<42,ID3D11Buffer*,UINT>}, {58,(void*)&Tap<58,ID3D11CommandList*,BOOL>}, {46,(void*)&Region}, {47,(void*)&Copy} };

		bool Exchange(void** at, void* old, void* next)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (!VirtualQuery(at,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
				(info.Protect!=PAGE_READONLY && info.Protect!=PAGE_READWRITE && info.Protect!=PAGE_WRITECOPY)) return false;
			DWORD prot{};
			if (!VirtualProtect(at,sizeof(void*),PAGE_READWRITE,&prot)) return false;
			const auto prev=InterlockedCompareExchangePointer(at,next,old);
			DWORD ignored{}; if (!VirtualProtect(at,sizeof(void*),prot,&ignored)) F4SE::log::error("f4sr: late trace could not restore table protection");
			return prev==old;
		}
	}

	static void Open(Ctx* ctx, ID3D11Texture2D* main, std::uint32_t frame, std::uint64_t tick, bool postChain, bool motion)
	{
		const UINT mode=motion?2u:UINT(postChain);
		if (active || unsafeToReopen || attempted[mode] || !ctx || !main || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
		attempted[mode]=true; w=Window{}; w.motion=motion; tapped=false; w.ctx=ctx; w.main=main; w.frame=frame; w.post=postChain;
		const auto stem=std::format("f4sr-{}-{}-{}-{}",motion?"motion":(postChain?"chain":"late"),GetCurrentProcessId(),tick,frame);
		w.events.open(stem+".txt"); w.pixels.open(stem+".rows",std::ios::binary);
		if (!w.events || !w.pixels) { w.main.Reset(); return; }
		w.events << "begin " << GetCurrentProcessId() << ' ' << tick << ' ' << frame << ' ' << address(main) << '\n';
		auto* data=RE::BSGraphics::RendererData::GetSingleton();
		for(UINT i=0;i<std::size(data->renderTargets);++i) {
			const auto& r=data->renderTargets[i]; if(r.texture || r.copyTexture) w.events << "engine " << i << ' ' << address(r.texture) << ' ' << address(r.copyTexture) << '\n';
		}
		table=*reinterpret_cast<void***>(ctx);
		UINT installed=0;
		const auto count=std::size(sites)-((postChain||motion)?0:2);
		for(std::size_t i=0;i<count;++i) {
			const auto& s=sites[i]; originals[s.slot]=table[s.slot];
			if(!originals[s.slot] || !Exchange(table+s.slot,originals[s.slot],s.tap)) break;
			++installed;
		}
		if(installed!=count) {
			while(installed) { const auto& s=sites[--installed]; if (!Exchange(table+s.slot,s.tap,originals[s.slot])) { unsafeToReopen=true; F4SE::log::error("f4sr: late trace install rollback failed slot={}",s.slot); } }
			w.events << "bindings_unavailable\n"; // stage markers can still localize a write
		} else tapped=true;
		active=&w;
	}

	void Begin(Ctx* ctx, ID3D11Texture2D* main, std::uint32_t frame, std::uint64_t tick, bool postChain) { Open(ctx,main,frame,tick,postChain,false); }
	void BeginMotion(Ctx* ctx, ID3D11Texture2D* mv, ID3D11Texture2D* mainCopy, std::uint32_t frame, std::uint64_t tick) {
		Open(ctx,mv,frame,tick,false,true);
		if(active && w.motion && mainCopy) { w.earlyCopy=mainCopy; w.busy=true; Rows(99,"early-entry",0,mainCopy,0); w.busy=false; }
	}
	void EndMotion() { if(active && w.motion) End(); }

	void RunEarlyPass(Ctx* outer,ID3D11Texture2D* mainCopy,EarlyPassFn original,void* pass,std::uint32_t technique,bool flag)
	{
		namespace bt=backend_trace;
		if(!outer || !mainCopy || bt::disabled || bt::scene || unsafeToReopen) { original(pass,technique,flag); return; }
		ComPtr<ID3D11DeviceContext1> inner; outer->QueryInterface(IID_PPV_ARGS(&inner));
		auto** v=inner?*reinterpret_cast<void***>(inner.Get()):nullptr;
		if(!bt::fixedTable) {
			wchar_t exe[MAX_PATH]{}; auto n=GetModuleFileNameW(nullptr,exe,MAX_PATH);
			auto mod=n && n<MAX_PATH ? GetModuleHandleW((std::filesystem::path(exe).parent_path()/L"d3d11.dll").c_str()):nullptr;
			MODULEINFO mi{}; const auto base=reinterpret_cast<std::uintptr_t>(mod);
			// Verified game-directory wrapper calls: DrawIndexed -> vslot 12,
			// Draw -> vslot 13. Reject other builds instead of guessing offsets.
			constexpr unsigned char producer[]{0x48,0x8b,0x01,0x8b,0xd5,0xff,0x50,0x60};
			constexpr unsigned char consumer[]{0x48,0x8b,0x01,0x41,0x8d,0x50,0x06,0xff,0x50,0x68};
			if(!mod || !K32GetModuleInformation(GetCurrentProcess(),mod,&mi,sizeof(mi)) || mi.SizeOfImage!=0x3b7000 ||
				memcmp(reinterpret_cast<void*>(base+0x78399-sizeof(producer)),producer,sizeof(producer)) ||
				memcmp(reinterpret_cast<void*>(base+0x78b0a-sizeof(consumer)),consumer,sizeof(consumer)) ||
				!v || inner.Get()==outer || !bt::SystemD3D11(v) || !bt::SystemD3D11(v[12]) || !bt::SystemD3D11(v[13])) {
				bt::disabled=true; F4SE::log::warn("f4sr: early scene t5 wrapper/backend mismatch; leaving original pass intact");
			} else {
				bt::producerReturn=reinterpret_cast<void*>(base+0x78399); bt::consumerReturn=reinterpret_cast<void*>(base+0x78b0a);
				bt::forward[12]=v[12]; bt::forward[13]=v[13];
				if(Exchange(v+12,bt::forward[12],bt::sites[0].probe)) {
					if(Exchange(v+13,bt::forward[13],bt::sites[1].probe)) bt::fixedTable=v;
					else if(!Exchange(v+12,bt::sites[0].probe,bt::forward[12])) unsafeToReopen=true;
				}
				bt::disabled=!bt::fixedTable;
			}
		}
		if(!v || v!=bt::fixedTable || v[12]!=bt::sites[0].probe || v[13]!=bt::sites[1].probe) { original(pass,technique,flag); return; }
		D3D11_TEXTURE2D_DESC d{}; mainCopy->GetDesc(&d);
		early_scene::Pass scope{inner.Get(),mainCopy,{d.Width,d.Height}};
		{
			bt::scene=&scope; struct Reset { ~Reset(){backend_trace::scene=nullptr;} } reset;
			original(pass,technique,flag);
		}
		// Live outcome counters: whether the t5 crop engaged in each early pass.
		static std::uint64_t passes{}, engaged{}, producerOnly{}, none{}, multi{}, misses[6]{};
		++passes;
		if(scope.used) ++engaged; else if(scope.indexed) ++producerOnly; else ++none;
		if(scope.indexed>1) ++multi;
		if(!scope.used && scope.indexed) ++misses[static_cast<unsigned>(scope.miss)];
		if(passes==1 || passes%600==0)
			F4SE::log::info("f4sr: early scene t5 crop passes={} engaged={} producer-only={} no-producer={} multi-producer={} miss[noConsumer={} noSource={} shape={} view={} crop={}] producer rejects[shape={} viewport={} notReduced={} output={}] fractional viewports={}",
				passes,engaged,producerOnly,none,multi,misses[1],misses[2],misses[3],misses[4],misses[5],
				early_scene::rejects.shape,early_scene::rejects.viewport,early_scene::rejects.notReduced,early_scene::rejects.output,early_scene::rejects.fractional);
	}

	void Mark(const char* boundary)
	{
		if (!active || w.busy) return;
		w.events << "mark " << boundary << '\n';
		Rows(0,boundary,0,w.main.Get(),0);
	}

	void End()
	{
		if (!active) return;
		if(w.motion && w.earlyCopy) { w.busy=true; Rows(199,"early-exit",0,w.earlyCopy.Get(),0); w.busy=false; }
		active=nullptr; bool restored=true;
		if(tapped) for(std::size_t i=0;i<std::size(sites)-((w.post||w.motion)?0:2);++i) { const auto& s=sites[i]; restored=Exchange(table+s.slot,s.tap,originals[s.slot]) && restored; }
		unsafeToReopen |= !restored;
		const auto id=reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address())->frameCount;
		w.pixels.close();
		w.events << "end " << id << ' ' << w.draws << ' ' << w.samples << ' ' << restored << ' ' << w.pixels.good() << '\n';
		w.events.close(); w.main.Reset(); w.earlyCopy.Reset();
		F4SE::log::info("f4sr: late draw trace frame={}/{} events={} samples={} restored={} files={}",w.frame,id,w.draws,w.samples,restored,w.events.good()&&w.pixels.good());
	}
}
