#include "fg_overlay.h"
#include "F4SE/F4SE.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace f4sr
{
	namespace
	{
		// Texture size is the upper bound; the card is cut to the text.
		constexpr UINT kMaxW = 960, kMaxH = 420;
		constexpr int kLine = 32, kPadX = 26, kPadTop = 16, kPadBottom = 18, kValueGap = 22, kAccent = 5, kRadius = 14;
		constexpr int kMargin = 16;  // from the back buffer's top-left corner
		constexpr std::uint8_t kBgR = 14, kBgG = 17, kBgB = 24;
		constexpr float kCardAlpha = 0.84f;

		struct Row
		{
			bool title{}, warn{}, gap{};
			std::string label, value;
		};

		// Panel text format: one row per line. A line starting with '#' is the
		// title; otherwise "label<TAB>value" (label may be empty for a
		// continuation row). A value starting with '!' is a warning; an empty
		// line is a small gap.
		std::vector<Row> Parse(const std::string& text)
		{
			std::vector<Row> rows;
			size_t start = 0;
			while (start < text.size()) {
				const size_t end = (std::min)(text.find('\n', start), text.size());
				std::string line = text.substr(start, end - start);
				start = end + 1;
				Row r;
				if (line.empty()) {
					r.gap = true;
				} else if (line[0] == '#') {
					r.title = true;
					r.value = line.substr(1);
				} else {
					const size_t tab = line.find('\t');
					r.label = tab == std::string::npos ? std::string() : line.substr(0, tab);
					r.value = tab == std::string::npos ? line : line.substr(tab + 1);
					if (!r.value.empty() && r.value[0] == '!') {
						r.warn = true;
						r.value.erase(0, 1);
					}
				}
				rows.push_back(std::move(r));
			}
			while (!rows.empty() && rows.back().gap) rows.pop_back();
			return rows;
		}

		int TextW(HDC dc, const std::string& s)
		{
			SIZE size{};
			GetTextExtentPoint32A(dc, s.c_str(), static_cast<int>(s.size()), &size);
			return size.cx;
		}

		// Coverage of a rounded rectangle at pixel centre (x, y), antialiased.
		float RoundRect(int x, int y, int w, int h, int r)
		{
			const float px = x + 0.5f, py = y + 0.5f;
			const float cx = std::clamp(px, static_cast<float>(r), static_cast<float>(w - r));
			const float cy = std::clamp(py, static_cast<float>(r), static_cast<float>(h - r));
			const float d = std::hypot(px - cx, py - cy) - static_cast<float>(r);
			return std::clamp(0.5f - d, 0.0f, 1.0f);
		}

		// Fullscreen strip over the panel's viewport; one texel per pixel.
		constexpr const char* kShader = R"(
Texture2D<float4> Panel : register(t0);
struct V { float4 pos : SV_Position; float2 uv : UV; };
V vs(uint id : SV_VertexID)
{
	float2 t = float2(id & 1, id >> 1);
	V o; o.uv = t; o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1); return o;
}
float4 ps(V i) : SV_Target
{
	uint w, h; Panel.GetDimensions(w, h);
	uint2 p = min(uint2(i.uv * float2(w, h)), uint2(w - 1, h - 1));
	return Panel.Load(int3(p, 0));
}
)";
	}

	void FgOverlay::Reset()
	{
		for (auto& panel : panels_) panel.Reset();
		for (auto& view : views_) view.Reset();
		vs_.Reset(); ps_.Reset(); blend_.Reset(); raster_.Reset(); depth_.Reset();
		pipelineFailed_ = false;
		device_.Reset();
		format_ = DXGI_FORMAT_UNKNOWN;
		lastTick_ = 0;
	}

	bool FgOverlay::CreatePipeline(ID3D11Device* device)
	{
		if (vs_) return true;
		if (pipelineFailed_) return false;
		Microsoft::WRL::ComPtr<ID3DBlob> vsCode, psCode;
		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		D3D11_DEPTH_STENCIL_DESC dd{};
		const bool ok =
			SUCCEEDED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vsCode.GetAddressOf(), nullptr)) &&
			SUCCEEDED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, psCode.GetAddressOf(), nullptr)) &&
			SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs_.GetAddressOf())) &&
			SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps_.GetAddressOf())) &&
			SUCCEEDED(device->CreateBlendState(&bd, blend_.GetAddressOf())) &&
			SUCCEEDED(device->CreateRasterizerState(&rd, raster_.GetAddressOf())) &&
			SUCCEEDED(device->CreateDepthStencilState(&dd, depth_.GetAddressOf()));
		if (!ok) {
			pipelineFailed_ = true;
			vs_.Reset();
			F4SE::log::warn("f4sr: overlay blend pipeline unavailable; using an opaque copy");
		}
		return ok;
	}

	bool FgOverlay::Update(ID3D11Device* device, DXGI_FORMAT format, const std::array<std::string, 2>& texts)
	{
		HDC dc = CreateCompatibleDC(nullptr);
		if (!dc) return false;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = kMaxW;
		info.bmiHeader.biHeight = -static_cast<LONG>(kMaxH);
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* bits{};
		HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		HFONT font = CreateFontW(-24, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, ANSI_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
		HFONT titleFont = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");
		if (!bitmap || !font || !titleFont || !bits) {
			if (bitmap) DeleteObject(bitmap);
			if (font) DeleteObject(font);
			if (titleFont) DeleteObject(titleFont);
			DeleteDC(dc);
			return false;
		}
		const auto oldBitmap = SelectObject(dc, bitmap);
		const auto oldFont = SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		const auto bgBits = static_cast<std::uint32_t>(kBgR) << 16 | static_cast<std::uint32_t>(kBgG) << 8 | kBgB;
		std::vector<std::uint32_t> pixels;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> next[2];
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nextViews[2];
		UINT nextW[2]{}, nextH[2]{};
		HRESULT hr = S_OK;
		for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
			const auto rows = Parse(texts[i]);
			// Layout: the label column is as wide as the widest label.
			int labelW = 0, valueW = 0, titleW = 0, height = kPadTop + kPadBottom;
			for (const auto& r : rows) {
				if (r.gap) { height += kLine / 3; continue; }
				if (r.title) {
					SelectObject(dc, titleFont);
					titleW = (std::max)(titleW, TextW(dc, r.value));
					SelectObject(dc, font);
					height += kLine + 8;
					continue;
				}
				labelW = (std::max)(labelW, TextW(dc, r.label));
				valueW = (std::max)(valueW, TextW(dc, r.value));
				height += kLine;
			}
			const int valueX = kPadX + (labelW > 0 ? labelW + kValueGap : 0);
			const int w = std::clamp((std::max)(valueX + valueW, kPadX + titleW) + kPadX, 160, static_cast<int>(kMaxW));
			const int h = std::clamp(height, 60, static_cast<int>(kMaxH));

			const COLORREF accent = i ? RGB(255, 120, 210) : RGB(90, 225, 150);
			const COLORREF label = RGB(135, 150, 178), value = RGB(232, 236, 242), warn = RGB(255, 176, 80);
			std::fill_n(static_cast<std::uint32_t*>(bits), kMaxW * kMaxH, bgBits);
			int y = kPadTop;
			for (const auto& r : rows) {
				if (y + kLine > h) break;
				if (r.gap) { y += kLine / 3; continue; }
				if (r.title) {
					SelectObject(dc, titleFont);
					SetTextColor(dc, accent);
					TextOutA(dc, kPadX, y - 2, r.value.c_str(), static_cast<int>(r.value.size()));
					SelectObject(dc, font);
					GdiFlush();
					// Hairline under the title.
					auto* px = static_cast<std::uint32_t*>(bits);
					const int ly = y + kLine + 2;
					for (int x = kPadX; x < w - kPadX && ly < h; ++x) px[ly * kMaxW + x] = 0x00323a4au;
					y += kLine + 8;
					continue;
				}
				SetTextColor(dc, label);
				TextOutA(dc, kPadX, y, r.label.c_str(), static_cast<int>(r.label.size()));
				SetTextColor(dc, r.warn ? warn : value);
				TextOutA(dc, valueX, y, r.value.c_str(), static_cast<int>(r.value.size()));
				y += kLine;
			}
			GdiFlush();

			// Premultiplied RGBA: the card is translucent, text and the accent
			// bar are opaque; text coverage is its distance from the card colour.
			pixels.assign(static_cast<size_t>(w) * h, 0);
			const auto* src = static_cast<const std::uint32_t*>(bits);
			const float ar = GetRValue(accent), ag = GetGValue(accent), ab = GetBValue(accent);
			for (int py = 0; py < h; ++py) {
				for (int px = 0; px < w; ++px) {
					const float shape = RoundRect(px, py, w, h, kRadius);
					if (shape <= 0.0f) continue;
					const auto v = src[py * kMaxW + px];
					float r = static_cast<float>((v >> 16) & 0xff), g = static_cast<float>((v >> 8) & 0xff), b = static_cast<float>(v & 0xff);
					const float diff = (std::max)({ std::abs(r - kBgR), std::abs(g - kBgG), std::abs(b - kBgB) });
					float a = kCardAlpha + (1.0f - kCardAlpha) * std::clamp(diff / 96.0f, 0.0f, 1.0f);
					if (px < kAccent) {
						r = ar; g = ag; b = ab; a = 1.0f;
					}
					a *= shape;
					const auto pr = static_cast<std::uint32_t>(r * a + 0.5f), pg = static_cast<std::uint32_t>(g * a + 0.5f),
							   pb = static_cast<std::uint32_t>(b * a + 0.5f), pa = static_cast<std::uint32_t>(a * 255.0f + 0.5f);
					pixels[static_cast<size_t>(py) * w + px] = format == DXGI_FORMAT_R8G8B8A8_UNORM ?
						pa << 24 | pb << 16 | pg << 8 | pr : pa << 24 | pr << 16 | pg << 8 | pb;
				}
			}
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(w); desc.Height = static_cast<UINT>(h); desc.MipLevels = 1; desc.ArraySize = 1;
			desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA data{ pixels.data(), static_cast<UINT>(w) * 4, 0 };
			hr = device->CreateTexture2D(&desc, &data, next[i].GetAddressOf());
			if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(next[i].Get(), nullptr, nextViews[i].GetAddressOf());
			nextW[i] = desc.Width;
			nextH[i] = desc.Height;
		}
		SelectObject(dc, oldFont); SelectObject(dc, oldBitmap);
		DeleteObject(font); DeleteObject(titleFont); DeleteObject(bitmap); DeleteDC(dc);
		if (FAILED(hr)) {
			F4SE::log::warn("f4sr: FG overlay texture failed hr=0x{:08X}", static_cast<std::uint32_t>(hr));
			return false;
		}
		for (int i = 0; i < 2; ++i) {
			panels_[i] = std::move(next[i]);
			views_[i] = std::move(nextViews[i]);
			panelW_[i] = nextW[i];
			panelH_[i] = nextH[i];
		}
		return true;
	}

	bool FgOverlay::Blend(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, int index)
	{
		if (!CreatePipeline(device_.Get())) return false;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
		if (FAILED(device_->CreateRenderTargetView(backbuffer, nullptr, rtv.GetAddressOf()))) return false;

		// Save everything this draw touches.
		ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* oldDsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
		ID3D11BlendState* oldBlend{}; FLOAT oldFactor[4]{}; UINT oldMask{};
		ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
		ID3D11DepthStencilState* oldDepth{}; UINT oldRef{};
		ctx->OMGetDepthStencilState(&oldDepth, &oldRef);
		ID3D11RasterizerState* oldRaster{};
		ctx->RSGetState(&oldRaster);
		D3D11_VIEWPORT oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ctx->RSGetViewports(&numVps, oldVps);
		ID3D11VertexShader* oldVs{}; ID3D11PixelShader* oldPs{}; ID3D11GeometryShader* oldGs{};
		ID3D11HullShader* oldHs{}; ID3D11DomainShader* oldDs{};
		ID3D11ClassInstance* inst[5][256]{}; UINT nInst[5]{ 256, 256, 256, 256, 256 };
		ctx->VSGetShader(&oldVs, inst[0], &nInst[0]);
		ctx->PSGetShader(&oldPs, inst[1], &nInst[1]);
		ctx->GSGetShader(&oldGs, inst[2], &nInst[2]);
		ctx->HSGetShader(&oldHs, inst[3], &nInst[3]);
		ctx->DSGetShader(&oldDs, inst[4], &nInst[4]);
		ID3D11ShaderResourceView* oldSrv{};
		ctx->PSGetShaderResources(0, 1, &oldSrv);
		ID3D11InputLayout* oldLayout{};
		ctx->IAGetInputLayout(&oldLayout);
		D3D11_PRIMITIVE_TOPOLOGY oldTopo{};
		ctx->IAGetPrimitiveTopology(&oldTopo);

		const D3D11_VIEWPORT vp{ static_cast<float>(kMargin), static_cast<float>(kMargin),
			static_cast<float>(panelW_[index]), static_cast<float>(panelH_[index]), 0.0f, 1.0f };
		ID3D11RenderTargetView* target = rtv.Get();
		ctx->OMSetRenderTargets(1, &target, nullptr);
		const FLOAT factor[4]{};
		ctx->OMSetBlendState(blend_.Get(), factor, 0xffffffffu);
		ctx->OMSetDepthStencilState(depth_.Get(), 0);
		ctx->RSSetState(raster_.Get());
		ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		ctx->VSSetShader(vs_.Get(), nullptr, 0);
		ctx->PSSetShader(ps_.Get(), nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ID3D11ShaderResourceView* view = views_[index].Get();
		ctx->PSSetShaderResources(0, 1, &view);
		ctx->Draw(4, 0);

		ctx->PSSetShaderResources(0, 1, &oldSrv);
		ctx->IASetPrimitiveTopology(oldTopo);
		ctx->IASetInputLayout(oldLayout);
		ctx->VSSetShader(oldVs, inst[0], nInst[0]);
		ctx->PSSetShader(oldPs, inst[1], nInst[1]);
		ctx->GSSetShader(oldGs, inst[2], nInst[2]);
		ctx->HSSetShader(oldHs, inst[3], nInst[3]);
		ctx->DSSetShader(oldDs, inst[4], nInst[4]);
		ctx->RSSetViewports(numVps, oldVps);
		ctx->RSSetState(oldRaster);
		ctx->OMSetDepthStencilState(oldDepth, oldRef);
		ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
		for (auto* r : oldRtvs) if (r) r->Release();
		if (oldDsv) oldDsv->Release();
		if (oldBlend) oldBlend->Release();
		if (oldDepth) oldDepth->Release();
		if (oldRaster) oldRaster->Release();
		if (oldVs) oldVs->Release();
		if (oldPs) oldPs->Release();
		if (oldGs) oldGs->Release();
		if (oldHs) oldHs->Release();
		if (oldDs) oldDs->Release();
		if (oldSrv) oldSrv->Release();
		if (oldLayout) oldLayout->Release();
		for (int s = 0; s < 5; ++s)
			for (UINT i = 0; i < nInst[s]; ++i) if (inst[s][i]) inst[s][i]->Release();
		return true;
	}

	void FgOverlay::Draw(IDXGISwapChain* chain, int variant, const Builder& build)
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
		if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) return;
		DrawTo(backbuffer.Get(), variant, build);
	}

	void FgOverlay::DrawTo(ID3D11Texture2D* backbuffer, int variant, const Builder& build)
	{
		if (!backbuffer) return;
		D3D11_TEXTURE2D_DESC desc{}; backbuffer->GetDesc(&desc);
		if ((desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
			desc.SampleDesc.Count != 1 || desc.Width < kMaxW + 2 * kMargin || desc.Height < kMaxH + 2 * kMargin) return;
		Microsoft::WRL::ComPtr<ID3D11Device> device; backbuffer->GetDevice(device.GetAddressOf());
		if (device.Get() != device_.Get() || format_ != desc.Format) {
			Reset(); device_ = device; format_ = desc.Format;
		}
		const auto now = GetTickCount64();
		if (lastTick_ == 0 || now - lastTick_ >= 1000) {
			const double seconds = lastTick_ ? static_cast<double>(now - lastTick_) / 1000.0 : 0.0;
			lastTick_ = now;
			if (!Update(device.Get(), desc.Format, build(seconds))) return;
		}
		const int index = variant ? 1 : 0;
		if (!panels_[index]) return;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx; device->GetImmediateContext(ctx.GetAddressOf());
		if ((desc.BindFlags & D3D11_BIND_RENDER_TARGET) && Blend(ctx.Get(), backbuffer, index)) return;
		// Fallback: opaque copy of the premultiplied card.
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		ctx->CopySubresourceRegion(backbuffer, 0, kMargin, kMargin, 0, panels_[index].Get(), 0, nullptr);
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
		for (auto* rtv : rtvs) if (rtv) rtv->Release();
		if (dsv) dsv->Release();
	}
}
