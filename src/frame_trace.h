#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>

#include "F4SE/F4SE.h"
#include "shader_capture.h"

namespace f4sr::frame_trace
{
	// Diagnostics only. Records every Draw/DrawIndexed on the system
	// immediate context for one whole frame (frame start to next frame start)
	// with its phase relative to the sub-native seam, outputs, viewport and
	// pixel-shader inputs, to f4sr-frametrace-<pid>-<n>.txt in the game root.
	// Used to find passes after the seam that read textures last written in
	// render layout before it.
	struct State
	{
		bool armed{}, recording{}, postSeam{};
		std::FILE* file{};
		std::uint32_t draws{}, traces{};
		ID3D11DeviceContext* ctx{};
	};
	inline State g;

	inline std::uintptr_t ModuleBase(const void* a, char* name, size_t n)
	{
		HMODULE m{};
		name[0] = 0;
		if (!a || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a), &m)) return 0;
		char path[MAX_PATH]{};
		GetModuleFileNameA(m, path, MAX_PATH);
		const char* s = path;
		for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') s = p + 1;
		snprintf(name, n, "%s", s);
		return reinterpret_cast<std::uintptr_t>(m);
	}

	inline void Arm() { if (!g.recording) g.armed = true; }

	// Frame start: closes a finished trace, opens an armed one.
	inline void FrameStart()
	{
		if (g.recording) {
			std::fprintf(g.file, "end draws=%u\n", g.draws);
			std::fclose(g.file);
			g.file = nullptr;
			g.recording = false;
			F4SE::log::info("f4sr: frame trace {} written ({} draws)", g.traces, g.draws);
		}
		if (!g.armed) return;
		g.armed = false;
		const auto name = std::format("f4sr-frametrace-{}-{}.txt", GetCurrentProcessId(), ++g.traces);
		g.file = std::fopen(name.c_str(), "w");
		if (!g.file) { F4SE::log::warn("f4sr: frame trace could not open {}", name); return; }
		std::fputs("# draw <seq> <pre|post> <12=DrawIndexed|13=Draw> <count> caller=<module+off> vs=<ptr> ps=<ptr> vp=<x,y,w,h>\n"
			"#   rt<i>=<res> <w>x<h> f<fmt> | ds=<res> <w>x<h> f<fmt> | t<slot>=<res> <w>x<h> f<fmt>/v<viewfmt> m<mips>\n", g.file);
		g.recording = true;
		g.postSeam = false;
		g.draws = 0;
		g.ctx = nullptr;
	}

	inline void Seam()
	{
		if (!g.recording) return;
		g.postSeam = true;
		std::fprintf(g.file, "seam after %u draws\n", g.draws);
	}

	inline void Describe(std::string& out, const char* tag, ID3D11Resource* res, DXGI_FORMAT viewFormat, bool view)
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
		if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
			out += std::format(" {}={:p} buf", tag, static_cast<void*>(res));
			return;
		}
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		out += std::format(" {}={:p} {}x{} f{}", tag, static_cast<void*>(res), d.Width, d.Height, static_cast<unsigned>(d.Format));
		if (view) out += std::format("/v{} m{}", static_cast<unsigned>(viewFormat), d.MipLevels);
		if (d.SampleDesc.Count > 1) out += std::format(" ms{}", d.SampleDesc.Count);
		if (d.ArraySize > 1) out += std::format(" a{}", d.ArraySize);
	}

	// ENB wrapper return offsets whose render target is dumped after the draw
	// in a traced frame: the t5 producer, the SSAO chain and the composite.
	inline constexpr std::uintptr_t kDumpCallers[]{ 0x78399, 0x7388b, 0x73b3e, 0x73dfa, 0x7444a, 0x7461a, 0x74bfa, 0x74e6a, 0x78b0a };

	inline void AfterDraw(ID3D11DeviceContext* ctx, const void* caller)
	{
		if (!g.recording || ctx != g.ctx || g.postSeam) return;
		char mod[64]{};
		const auto base = ModuleBase(caller, mod, sizeof(mod));
		if (_stricmp(mod, "d3d11.dll") != 0) return;
		const auto off = reinterpret_cast<std::uintptr_t>(caller) - base;
		bool want = false;
		for (auto c : kDumpCallers) want |= c == off;
		if (!want) return;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
		ctx->OMGetRenderTargets(1, &rtv, nullptr);
		if (!rtv) return;
		D3D11_RENDER_TARGET_VIEW_DESC vd{};
		rtv->GetDesc(&vd);
		Microsoft::WRL::ComPtr<ID3D11Resource> res;
		rtv->GetResource(&res);
		Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
		if (!res || FAILED(res.As(&tex)) || vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		const UINT mip = vd.Texture2D.MipSlice;
		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = (std::max)(1u, d.Width >> mip); sd.Height = (std::max)(1u, d.Height >> mip);
		sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
		sd.Format = d.Format; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (d.SampleDesc.Count != 1 || d.ArraySize != 1) return;
		Microsoft::WRL::ComPtr<ID3D11Device> dev;
		ctx->GetDevice(&dev);
		Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
		if (!dev || FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return;
		ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), mip, nullptr);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
		const auto name = std::format("f4sr-frametrace-{}-{}-d{}-{:x}.raw", GetCurrentProcessId(), g.traces, g.draws, off);
		if (auto* f = std::fopen(name.c_str(), "wb")) {
			std::fprintf(f, "F4SRRAW %u %u %u %u\n", sd.Width, sd.Height, static_cast<unsigned>(vd.Format ? vd.Format : d.Format), m.RowPitch);
			std::fwrite(m.pData, 1, static_cast<size_t>(m.RowPitch) * sd.Height, f);
			std::fclose(f);
		}
		ctx->Unmap(staging.Get(), 0);
	}

	inline void OnDraw(ID3D11DeviceContext* ctx, unsigned method, unsigned count, const void* caller)
	{
		if (!g.recording || !ctx) return;
		if (!g.ctx) g.ctx = ctx;
		if (ctx != g.ctx) return;
		const auto seq = ++g.draws;
		char mod[64]{};
		const auto base = ModuleBase(caller, mod, sizeof(mod));
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
		ctx->VSGetShader(&vs, nullptr, nullptr);
		ctx->PSGetShader(&ps, nullptr, nullptr);
		D3D11_VIEWPORT vp{};
		UINT nvp = 1;
		ctx->RSGetViewports(&nvp, &vp);
		std::string line = std::format("draw {} {} {} {} caller={}+0x{:x} vs={:p} ps={:p} vp={},{},{},{}", seq, g.postSeam ? "post" : "pre", method, count,
			mod, reinterpret_cast<std::uintptr_t>(caller) - base, static_cast<void*>(vs.Get()), static_cast<void*>(ps.Get()),
			vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
		// ENB's own passes (not the forwarded game draws): keep their pixel shader.
		const auto off = reinterpret_cast<std::uintptr_t>(caller) - base;
		if (!_stricmp(mod, "d3d11.dll") && off != 0x6fdcf && off != 0x6f8be) {
			const auto file = shader_capture::Write(ps.Get());
			if (!file.empty()) line += " psfile=" + file;
		}
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
			if (!rtvs[i]) continue;
			Microsoft::WRL::ComPtr<ID3D11Resource> r;
			rtvs[i]->GetResource(&r);
			Describe(line, std::format("rt{}", i).c_str(), r.Get(), DXGI_FORMAT_UNKNOWN, false);
			rtvs[i]->Release();
		}
		if (dsv) {
			Microsoft::WRL::ComPtr<ID3D11Resource> r;
			dsv->GetResource(&r);
			Describe(line, "ds", r.Get(), DXGI_FORMAT_UNKNOWN, false);
			dsv->Release();
		}
		line += " |";
		ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
		ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
		for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
			if (!views[i]) continue;
			Microsoft::WRL::ComPtr<ID3D11Resource> r;
			views[i]->GetResource(&r);
			D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
			views[i]->GetDesc(&vd);
			Describe(line, std::format("t{}", i).c_str(), r.Get(), vd.Format, true);
			views[i]->Release();
		}
		line += '\n';
		std::fputs(line.c_str(), g.file);
	}
}
