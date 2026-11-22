// Included inside late_trace's private namespace. One observation on the actual
// immediate context, never unwrap a COM object by reading its implementation.
namespace backend_trace {
	thread_local Ctx* recording{};
	void* forward[48]{}; // Initialized before publication, retained after removal.
	UINT calls{};
	bool once{};
	thread_local early_scene::Pass* scene{};
	thread_local early_scene::Crop crop;
	thread_local early_scene::Crop cropAo;
	thread_local early_scene::Crop cropBase;
	void** fixedTable{};
	bool disabled{};
	const void* producerReturn{};
	const void* consumerReturn{};

	// Ignore the process-local d3d11.dll facade. Require the table and every
	// displaced method to reside in the loaded system32 implementation.
	bool SystemD3D11(const void* at)
	{
		HMODULE mod{}; wchar_t path[MAX_PATH]{}, dir[MAX_PATH]{};
		const auto len=GetSystemDirectoryW(dir,MAX_PATH);
		if(!len || len>=MAX_PATH || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(at),&mod)) return false;
		const auto n=GetModuleFileNameW(mod,path,MAX_PATH);
		return n && n<MAX_PATH && !_wcsicmp(path,(std::wstring(dir)+L"\\d3d11.dll").c_str());
	}

	template<UINT N, class... A> void ObserveDraw(Ctx* ctx,A... a)
	{
		auto orig=reinterpret_cast<void(STDMETHODCALLTYPE*)(Ctx*,A...)>(forward[N]);
		if(!recording || recording!=ctx || calls>=32) { orig(ctx,a...); return; }
		recording=nullptr; const UINT seq=300+calls++;
		D3D11_PRIMITIVE_TOPOLOGY top{}; ctx->IAGetPrimitiveTopology(&top);
		w.events << "inner-draw " << seq << ' ' << N << ' ' << top;
		((w.events << ' ' << a),...); w.events << '\n';
		Stack(seq);
		D3D11_VIEWPORT vp[16]{}; UINT nv=16; ctx->RSGetViewports(&nv,vp);
		for(UINT i=0;i<nv && i<16;++i) w.events << "vp " << seq << ' ' << i << ' ' << vp[i].TopLeftX << ' ' << vp[i].TopLeftY << ' ' << vp[i].Width << ' ' << vp[i].Height << '\n';
		ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps,nullptr,nullptr);
		w.events << "ps " << seq << ' ' << address(ps.Get()) << '\n';
		ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
		ctx->PSGetShaderResources(0,std::size(views),views);
		for(UINT i=0;i<std::size(views);++i) if(views[i]) {
			ComPtr<ID3D11ShaderResourceView> v; v.Attach(views[i]); ComPtr<ID3D11Resource> r; v->GetResource(&r);
			D3D11_SHADER_RESOURCE_VIEW_DESC d{}; v->GetDesc(&d);
			ComPtr<ID3D11Texture2D> input; D3D11_TEXTURE2D_DESC td{};
			if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D && SUCCEEDED(r.As(&input))) input->GetDesc(&td);
			w.events << "inner-input " << seq << ' ' << i << ' ' << address(r.Get()) << ' ' << d.Format << ' ' << d.ViewDimension << ' ' << (d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?d.Texture2D.MostDetailedMip:UINT(-1)) << ' ' << td.Width << ' ' << td.Height << '\n';
		}
		ComPtr<ID3D11RenderTargetView> v; ctx->OMGetRenderTargets(1,&v,nullptr);
		ComPtr<ID3D11Texture2D> tex; UINT mip{};
		if(v) {
			ComPtr<ID3D11Resource> r; v->GetResource(&r);
			D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d);
			if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D) { r.As(&tex); mip=d.Texture2D.MipSlice; }
			w.events << "inner-out " << seq << " 0 " << address(r.Get()) << ' ' << d.Format << ' ' << d.ViewDimension << '\n';
		}
		if(tex) Rows(seq,"inner-before",0,tex.Get(),mip,0,ctx);
		orig(ctx,a...);
		if(tex) Rows(seq,"inner-after",0,tex.Get(),mip,0,ctx);
		recording=ctx;
	}
	template<UINT N, class... A> void STDMETHODCALLTYPE Draw(Ctx* ctx,A... a)
	{
		if constexpr(N==12 || N==13) {
			frame_trace::OnDraw(ctx,N,[](auto x,auto...){return static_cast<unsigned>(x);}(a...),_ReturnAddress());
			post_depth::OnDraw(ctx);
		}
		auto* p=scene;
		if constexpr(N==12 || N==13) {
			if(p && p->ctx==ctx) {
				const void* caller=_ReturnAddress();
				if constexpr(N==12) {
					if(caller==producerReturn) p->Produced(a...);
				} else if(caller==consumerReturn) {
					ComPtr<ID3D11ShaderResourceView> saved;
					// Suppress our region copy from the program's event stream.
					auto* rec=recording; recording=nullptr;
					const bool bound=p->Bind(a...,crop,saved);
					ComPtr<ID3D11ShaderResourceView> savedAo;
					const bool ao=bound && p->BindAo(cropAo,savedAo);
					ComPtr<ID3D11ShaderResourceView> savedBase;
					const bool base=bound && p->BindSlot(0,cropBase,savedBase);
					recording=rec;
					if(bound) {
						ObserveDraw<N>(ctx,a...);
						frame_trace::AfterDraw(ctx,_ReturnAddress());
						ctx->PSSetShaderResources(5,1,saved.GetAddressOf());
						if(ao) ctx->PSSetShaderResources(3,1,savedAo.GetAddressOf());
						if(base) ctx->PSSetShaderResources(0,1,savedBase.GetAddressOf());
						static bool baseLogged{}; if(base && !baseLogged) {
							baseLogged=true; F4SE::log::info("f4sr: early scene t0 cropped to {}x{}",p->active.w,p->active.h);
						}
						static bool aoLogged{}; if(ao && !aoLogged) {
							aoLogged=true; F4SE::log::info("f4sr: early scene t3 (ENB SSAO) cropped to {}x{}",p->active.w,p->active.h);
						}
						static bool logged{}; if(!logged) {
							logged=true; F4SE::log::info("f4sr: early scene t5 active rect {}x{} from {}x{}; original view restored",p->active.w,p->active.h,p->full.w,p->full.h);
						}
						return;
					}
				}
			}
		}
		ObserveDraw<N>(ctx,a...);
		if constexpr(N==12 || N==13) frame_trace::AfterDraw(ctx,_ReturnAddress());
	}
	template<class Fn> void CopyObserved(Ctx* ctx, UINT method, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss,
		UINT x, UINT y, UINT z, const D3D11_BOX* box, Fn invoke)
	{
		if(!recording || recording!=ctx || calls>=32) { invoke(); return; }
		recording=nullptr; const UINT seq=300+calls++;
		w.events << "inner-copy " << seq << ' ' << method << ' ' << address(dst) << ' ' << ds << ' ' << address(src) << ' ' << ss << ' ' << x << ' ' << y << ' ' << z;
		if(box) w.events << ' ' << box->left << ' ' << box->top << ' ' << box->front << ' ' << box->right << ' ' << box->bottom << ' ' << box->back;
		else w.events << " full"; w.events << '\n';
		ComPtr<ID3D11Texture2D> t; if(dst) dst->QueryInterface(IID_PPV_ARGS(&t));
		if(t) Rows(seq,"inner-before",0,t.Get(),ds,0,ctx);
		invoke();
		if(t) Rows(seq,"inner-after",0,t.Get(),ds,0,ctx);
		recording=ctx;
	}
	void STDMETHODCALLTYPE Copy(Ctx* c,ID3D11Resource* d,ID3D11Resource* s) {
		CopyObserved(c,47,d,0,s,0,0,0,0,nullptr,[&]{reinterpret_cast<decltype(&Copy)>(forward[47])(c,d,s);});
	}
	void STDMETHODCALLTYPE Region(Ctx* c,ID3D11Resource* d,UINT ds,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* b) {
		CopyObserved(c,46,d,ds,s,ss,x,y,z,b,[&]{reinterpret_cast<decltype(&Region)>(forward[46])(c,d,ds,x,y,z,s,ss,b);});
	}
	struct Site { UINT slot; void* probe; };
	const Site sites[]{ {12,(void*)&Draw<12,UINT,UINT,INT>},{13,(void*)&Draw<13,UINT,UINT>},
		{20,(void*)&Draw<20,UINT,UINT,UINT,INT,UINT>},{21,(void*)&Draw<21,UINT,UINT,UINT,UINT>},{46,(void*)&Region},{47,(void*)&Copy} };
}
struct BackendScope {
	ComPtr<Ctx> handle; void** vt{}; UINT installed{};
	void Consider(Ctx* c,const char* route) {
		if(handle || !c) return;
		auto** v=*reinterpret_cast<void***>(c);
		const bool ok=c!=w.ctx && v!=table && (!backend_trace::fixedTable || v==backend_trace::fixedTable) && backend_trace::SystemD3D11(v) && c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;
		w.events << "inner-route " << route << ' ' << address(c) << ' ' << ok << '\n';
		if(ok && std::all_of(std::begin(backend_trace::sites),std::end(backend_trace::sites),[&](auto s){return (v==backend_trace::fixedTable && s.slot<=13 && v[s.slot]==s.probe) || backend_trace::SystemD3D11(v[s.slot]);})) { handle=c; vt=v; }
	}
	void Open(Ctx* outer,ID3D11Query* query) {
		using namespace backend_trace;
		if(once) return; once=true;
		ComPtr<ID3D11DeviceContext1> c1; outer->QueryInterface(IID_PPV_ARGS(&c1)); Consider(c1.Get(),"context1");
		auto child=[&](ID3D11DeviceChild* obj,const char* name) {
			if(!obj || handle) return;
			ComPtr<ID3D11Device> d; obj->GetDevice(&d); if(!d) return;
			ComPtr<Ctx> c; d->GetImmediateContext(&c); Consider(c.Get(),name);
			ComPtr<ID3D11Device1> d1; if(!handle && SUCCEEDED(d.As(&d1))) {
				ComPtr<ID3D11DeviceContext1> inner; d1->GetImmediateContext1(&inner); Consider(inner.Get(),"device1");
			}
		};
		child(w.earlyCopy.Get(),"rt3"); child(query,"query");
		if(!handle) { w.events << "inner-unavailable\n"; return; }
		for(auto s:sites) {
			if(vt==fixedTable && s.slot<=13) { ++installed; continue; }
			forward[s.slot]=vt[s.slot];
			if(!SystemD3D11(forward[s.slot]) || !Exchange(vt+s.slot,forward[s.slot],s.probe)) { Close(); w.events << "inner-install-failed\n"; return; }
			++installed;
		}
		calls=0; recording=handle.Get(); w.events << "inner-begin 100\n";
	}
	void Close() {
		using namespace backend_trace;
		if(!installed) return;
		recording=nullptr; bool ok=true;
		while(installed) { auto s=sites[--installed]; if(vt!=fixedTable || s.slot>13) ok=Exchange(vt+s.slot,s.probe,forward[s.slot]) && ok; }
		w.events << "inner-end " << calls << ' ' << ok << '\n';
		if(!ok) unsafeToReopen=true;
	}
	~BackendScope() { Close(); }
};
