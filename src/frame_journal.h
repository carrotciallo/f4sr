#pragma once

#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <format>

#include "F4SE/F4SE.h"

namespace f4sr::frame_journal
{
	// Diagnostics only. One CSV row per presented frame with the camera and
	// resolve state of the frame it shows, and the same present index stamped
	// as a block code into the top-right corner of the back buffer, so a
	// screen recording can be matched to rows frame by frame.
	//
	// Stamp layout (display pixels, top row, right-aligned): 26 blocks of
	// kBlock x kBlock. Block 0 white and block 1 black are guards; blocks
	// 2..25 carry the present index, most significant bit first (white = 1).
	inline constexpr UINT kBlock = 32;
	inline constexpr UINT kBits = 24;

	struct Row
	{
		std::uint32_t engineFrame{};
		float frameMs{};
		float position[3]{}, forward[3]{};
		bool camera{};
		float jitterX{}, jitterY{};
		std::uint32_t renderW{}, renderH{};
		bool reset{};
	};

	struct State
	{
		Row pending{};
		bool resolved{};
		std::uint32_t presents{};
		std::int64_t lastQpc{};
		std::FILE* file{};
		bool failed{};
		ID3D11Texture2D* rtvTexture{};
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
	};
	inline State g;

	inline void Record(const Row& row)
	{
		g.pending = row;
		g.resolved = true;
	}

	inline double NowMs(std::int64_t& raw)
	{
		LARGE_INTEGER qpc{}, freq{};
		QueryPerformanceCounter(&qpc);
		QueryPerformanceFrequency(&freq);
		raw = qpc.QuadPart;
		return static_cast<double>(qpc.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
	}

	inline DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
	{
		switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
		}
	}

	inline bool Stamp(IDXGISwapChain* chain, ID3D11DeviceContext* ctx, std::uint32_t index)
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
		if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
		D3D11_TEXTURE2D_DESC d{};
		back->GetDesc(&d);
		if (d.Width < 26 * kBlock || d.Height < kBlock) return false;
		if (g.rtvTexture != back.Get() || !g.rtv) {
			g.rtv.Reset(); g.rtvTexture = nullptr;
			Microsoft::WRL::ComPtr<ID3D11Device> device;
			back->GetDevice(&device);
			D3D11_RENDER_TARGET_VIEW_DESC vd{};
			vd.Format = ViewFormat(d.Format);
			vd.ViewDimension = d.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
			if (!device || FAILED(device->CreateRenderTargetView(back.Get(), &vd, &g.rtv))) return false;
			g.rtvTexture = back.Get();
		}
		Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
		if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return false;
		D3D11_RECT ones[kBits + 1]{}, zeros[kBits + 1]{};
		UINT n1 = 0, n0 = 0;
		const LONG x0 = static_cast<LONG>(d.Width - 26 * kBlock);
		auto block = [&](UINT i) { return D3D11_RECT{ x0 + LONG(i * kBlock), 0, x0 + LONG((i + 1) * kBlock), LONG(kBlock) }; };
		ones[n1++] = block(0);
		zeros[n0++] = block(1);
		for (UINT b = 0; b < kBits; ++b) {
			const bool bit = (index >> (kBits - 1 - b)) & 1u;
			(bit ? ones[n1++] : zeros[n0++]) = block(2 + b);
		}
		const FLOAT white[4]{ 1, 1, 1, 1 }, black[4]{ 0, 0, 0, 1 };
		ctx1->ClearView(g.rtv.Get(), white, ones, n1);
		ctx1->ClearView(g.rtv.Get(), black, zeros, n0);
		return true;
	}

	// Called once per Present of the game swap chain, before the original Present.
	inline void OnPresent(IDXGISwapChain* chain, ID3D11DeviceContext* ctx)
	{
		if (g.failed || !chain || !ctx) return;
		if (!g.file) {
			const auto name = std::format("f4sr-frames-{}.csv", GetCurrentProcessId());
			g.file = std::fopen(name.c_str(), "w");
			if (!g.file) { g.failed = true; F4SE::log::warn("f4sr: frame journal could not open {}", name); return; }
			std::fputs("present,qpc_ms,present_dt_ms,stamped,resolved,engine_frame,frame_ms,camera,px,py,pz,fx,fy,fz,jitter_x,jitter_y,render_w,render_h,reset\n", g.file);
			F4SE::log::info("f4sr: frame journal {} with a {}-bit present stamp in the top-right corner", name, kBits);
		}
		const std::uint32_t index = ++g.presents;
		const bool stamped = Stamp(chain, ctx, index);
		std::int64_t raw{};
		const double now = NowMs(raw);
		LARGE_INTEGER freq{}; QueryPerformanceFrequency(&freq);
		const double dt = g.lastQpc ? static_cast<double>(raw - g.lastQpc) * 1000.0 / static_cast<double>(freq.QuadPart) : 0.0;
		g.lastQpc = raw;
		const auto& r = g.pending;
		std::fprintf(g.file, "%u,%.3f,%.3f,%d,%d,%u,%.3f,%d,%.3f,%.3f,%.3f,%.6f,%.6f,%.6f,%.5f,%.5f,%u,%u,%d\n",
			index, now, dt, stamped ? 1 : 0, g.resolved ? 1 : 0, r.engineFrame, r.frameMs, r.camera ? 1 : 0,
			r.position[0], r.position[1], r.position[2], r.forward[0], r.forward[1], r.forward[2],
			r.jitterX, r.jitterY, r.renderW, r.renderH, r.reset ? 1 : 0);
		if (index % 120 == 0) std::fflush(g.file);
		g.resolved = false;
	}
}
