#include "dynres_audit.h"

#include <d3d11.h>
#include <windows.h>

#include <array>
#include <intrin.h>
#include <utility>
#include <cstdio>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REL/REL.h"
#include "rt_relayout.h"
#include "settings.h"

namespace f4sr::dynres_audit
{
	namespace
	{
		constexpr std::uint64_t kRenderTargetManagerId = 1508457;
		constexpr std::size_t kRatioW = 0xF88, kRatioH = 0xF8C, kActivated = 0xFA8;
		constexpr int kStack = 8;
		// Scene targets are legitimately read in the imagespace range (dynres-aware
		// effects); keep room for the UI phase, where the terminal glow is composited.
		constexpr int kMaxImagespace = 12, kMaxUi = 30;

		enum class Phase : std::uint8_t { kBeforeSeam, kImagespace, kUi };

		struct Write
		{
			std::array<void*, kStack> stack{};
			float vpW{}, vpH{};
			UINT texW{}, texH{};
		};

		bool g_installed{}, g_active{};
		Phase g_phase{ Phase::kBeforeSeam };
		std::unordered_map<ID3D11Resource*, Write> g_written;           // this frame, before the seam
		std::unordered_set<std::uint64_t> g_reported;                    // (resource, phase) pairs logged
		// Targets the UI phase reads: they must be in display layout after the
		// imagespace range. Learned from UI-phase reads.
		std::unordered_set<ID3D11Resource*> g_uiReaders;
		// Targets re-laid out at the seam, before the imagespace range: the
		// terminal's renderTargets[59] (a scene copy written before the seam)
		// is read by an imagespace draw (reader stack exe+28C8CEE, exe+28693C1) that runs after the
		// seam at ratio 1 and so composited the reduced copy 1:1 - the
		// terminal glow (2026-11-22 captures).
		// renderTargets[24] is read by the same HDR pass while the terminal is
		// open (its screen area is a solid mask in the capture), so it is
		// re-laid out at the seam as well (2026-11-22, third capture).
		constexpr int kSeamSlots[]{ 59, 24 };
		RtRelayout g_relayout;

		// One-frame capture (capture key while the audit is active): the UI-read
		// targets before and after the relayout, and every texture bound to the
		// first UI draw that samples one of them. Raw files in the game folder.
		bool g_capturing{}, g_keyDown{};
		std::uint64_t g_captureId{};
		int g_captureFiles{};
		bool g_uiDrawCaptured{};

		void Dump(ID3D11DeviceContext* ctx, ID3D11Resource* res, const std::string& tag)
		{
			if (!res || g_captureFiles >= 16) return;
			ID3D11Texture2D* tex{};
			if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || !tex) return;
			D3D11_TEXTURE2D_DESC d{};
			tex->GetDesc(&d);
			D3D11_TEXTURE2D_DESC s = d;
			s.MipLevels = 1; s.ArraySize = 1; s.SampleDesc = { 1, 0 };
			s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
			ID3D11Device* dev{};
			ctx->GetDevice(&dev);
			ID3D11Texture2D* staging{};
			const bool made = d.SampleDesc.Count == 1 && dev && SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &staging));
			if (dev) dev->Release();
			if (!made) { tex->Release(); return; }
			ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
				const auto name = std::format("f4sr-term-{}-{:02}-{}.raw", g_captureId, g_captureFiles, tag);
				if (FILE* f = std::fopen(name.c_str(), "wb")) {
					std::fprintf(f, "F4SRRAW %u %u %u %u\n", d.Width, d.Height, static_cast<unsigned>(d.Format), m.RowPitch);
					std::fwrite(m.pData, 1, static_cast<size_t>(m.RowPitch) * d.Height, f);
					std::fclose(f);
					++g_captureFiles;
					F4SE::log::info("f4sr: dynres capture: {} ({}x{} fmt {})", name, d.Width, d.Height, static_cast<unsigned>(d.Format));
				}
				ctx->Unmap(staging, 0);
			}
			staging->Release();
			tex->Release();
		}
		int g_imagespaceFindings{}, g_uiFindings{};

		using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
		using Draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
		using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
		using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
		using CopySubresourceRegion_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
		using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
		DrawIndexed_t g_drawIndexed{};
		Draw_t g_draw{};
		DrawIndexedInstanced_t g_drawIndexedInstanced{};
		DrawInstanced_t g_drawInstanced{};
		CopySubresourceRegion_t g_copyRegion{};
		CopyResource_t g_copy{};

		// Draws and copies issued by this plugin (relayout, SR plumbing, XeSS-FG
		// captures) are neither findings nor UI readers.
		bool FromSelf(void* returnAddress)
		{
			static const auto range = [] {
				HMODULE self{};
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&FromSelf), &self);
				const auto base = reinterpret_cast<std::uintptr_t>(self);
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
			}();
			const auto a = reinterpret_cast<std::uintptr_t>(returnAddress);
			return a >= range.first && a < range.second;
		}

		bool BeforeSeam()
		{
			auto* m = reinterpret_cast<std::byte*>(REL::ID(kRenderTargetManagerId).address());
			const float w = *reinterpret_cast<float*>(m + kRatioW), h = *reinterpret_cast<float*>(m + kRatioH);
			return *reinterpret_cast<bool*>(m + kActivated) && (w < 1.0f || h < 1.0f);
		}

		ID3D11Resource* SceneTexture()
		{
			auto* data = RE::BSGraphics::RendererData::GetSingleton();
			const int slot = GetSettings().colorSlot;
			return data && slot >= 0 && slot < 101 ? reinterpret_cast<ID3D11Resource*>(data->renderTargets[slot].texture) : nullptr;
		}

		std::string SlotOf(ID3D11Resource* r)
		{
			auto* data = RE::BSGraphics::RendererData::GetSingleton();
			if (!data) return "?";
			for (int i = 0; i < 101; ++i) {
				if (reinterpret_cast<ID3D11Resource*>(data->renderTargets[i].texture) == r) return std::format("renderTargets[{}]", i);
				if (reinterpret_cast<ID3D11Resource*>(data->renderTargets[i].copyTexture) == r) return std::format("renderTargets[{}].copy", i);
			}
			return "not a RendererData target";
		}

		std::string Stack(const std::array<void*, kStack>& s)
		{
			static const auto base = REL::Module::get().base();
			static const auto size = [] {
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				return static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
			}();
			std::string out;
			for (void* p : s) {
				if (!p) break;
				const auto a = reinterpret_cast<std::uintptr_t>(p);
				if (a >= base && a < base + size) {
					out += std::format(" exe+{:X}", a - base);
				} else {
					HMODULE mod{};
					char path[MAX_PATH]{};
					if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(p), &mod) &&
						GetModuleFileNameA(mod, path, MAX_PATH)) {
						std::string name(path);
						if (const auto cut = name.find_last_of("\\/"); cut != std::string::npos) name.erase(0, cut + 1);
						out += std::format(" {}+{:X}", name, a - reinterpret_cast<std::uintptr_t>(mod));
					} else {
						out += std::format(" {:p}", p);
					}
				}
			}
			return out;
		}

		void Capture(std::array<void*, kStack>& s)
		{
			RtlCaptureStackBackTrace(2, kStack, s.data(), nullptr);
		}

		UINT TexSize(ID3D11Resource* r, UINT& h)
		{
			ID3D11Texture2D* t{};
			if (FAILED(r->QueryInterface(IID_PPV_ARGS(&t))) || !t) { h = 0; return 0; }
			D3D11_TEXTURE2D_DESC d{};
			t->GetDesc(&d);
			t->Release();
			h = d.Height;
			return d.Width;
		}

		void OnWriteDraw(ID3D11DeviceContext* ctx)
		{
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, nullptr);
			UINT nvp = 1;
			D3D11_VIEWPORT vp{};
			ctx->RSGetViewports(&nvp, &vp);
			auto* scene = SceneTexture();
			for (auto* rtv : rtvs) {
				if (!rtv) continue;
				ID3D11Resource* res{};
				rtv->GetResource(&res);
				rtv->Release();
				if (!res) continue;
				if (res != scene && nvp > 0) {
					UINT h = 0;
					const UINT w = TexSize(res, h);
					// Reduced viewport into a large target: the dynamic-resolution pattern.
					if (w >= 1280 && vp.Width > 0 && (vp.Width < w * 0.9f || vp.Height < h * 0.9f) && !g_written.contains(res)) {
						Write wr{};
						Capture(wr.stack);
						wr.vpW = vp.Width;
						wr.vpH = vp.Height;
						wr.texW = w;
						wr.texH = h;
						g_written.emplace(res, wr);
					}
				}
				res->Release();
			}
		}

		void Report(ID3D11Resource* res, const char* how)
		{
			const auto it = g_written.find(res);
			if (it == g_written.end()) return;
			int& count = g_phase == Phase::kUi ? g_uiFindings : g_imagespaceFindings;
			if (count >= (g_phase == Phase::kUi ? kMaxUi : kMaxImagespace)) return;
			const auto key = reinterpret_cast<std::uint64_t>(res) ^ (static_cast<std::uint64_t>(g_phase) << 62);
			if (!g_reported.insert(key).second) return;
			++count;
			if (g_phase == Phase::kUi && g_uiReaders.insert(res).second)
				F4SE::log::info("f4sr: dynres audit: {} ({}) is read by the UI; re-laid out to display size from now on", static_cast<void*>(res), SlotOf(res));
			std::array<void*, kStack> reader{};
			Capture(reader);
			const auto& w = it->second;
			F4SE::log::warn("f4sr: dynres audit: {} {}x{} ({}) written before the seam with viewport {:.0f}x{:.0f}, {} {} the seam\n  writer:{}\n  reader:{}",
				static_cast<void*>(res), w.texW, w.texH, SlotOf(res), w.vpW, w.vpH, how,
				g_phase == Phase::kUi ? "in the UI phase after" : "in the imagespace range after",
				Stack(w.stack), Stack(reader));
		}

		void OnReadDraw(ID3D11DeviceContext* ctx)
		{
			if (g_written.empty()) return;
			ID3D11ShaderResourceView* srvs[16]{};
			ctx->PSGetShaderResources(0, 16, srvs);
			for (auto* srv : srvs) {
				if (!srv) continue;
				ID3D11Resource* res{};
				srv->GetResource(&res);
				srv->Release();
				if (!res) continue;
				Report(res, "sampled by a draw");
				res->Release();
			}
		}

		void CaptureUiDraw(ID3D11DeviceContext* ctx)
		{
			ID3D11ShaderResourceView* srvs[16]{};
			ctx->PSGetShaderResources(0, 16, srvs);
			bool hit = false;
			for (auto* srv : srvs) {
				if (!srv) continue;
				ID3D11Resource* r{};
				srv->GetResource(&r);
				if (r) { hit = hit || g_uiReaders.contains(r); r->Release(); }
			}
			if (hit) {
				g_uiDrawCaptured = true;
				std::array<void*, kStack> stack{};
				Capture(stack);
				F4SE::log::info("f4sr: dynres capture: UI draw sampling a re-laid-out target, caller{}", Stack(stack));
				for (UINT i = 0; i < 16; ++i) {
					if (!srvs[i]) continue;
					ID3D11Resource* r{};
					srvs[i]->GetResource(&r);
					UINT h = 0;
					const UINT w = r ? TexSize(r, h) : 0;
					F4SE::log::info("f4sr: dynres capture:   t{} {} {}x{} ({})", i, static_cast<void*>(r), w, h, r ? SlotOf(r) : "-");
					if (r) { Dump(ctx, r, std::format("uidraw-t{}", i)); r->Release(); }
				}
				ID3D11RenderTargetView* rtv{};
				ctx->OMGetRenderTargets(1, &rtv, nullptr);
				if (rtv) {
					ID3D11Resource* r{};
					rtv->GetResource(&r);
					F4SE::log::info("f4sr: dynres capture:   rt0 {} ({})", static_cast<void*>(r), r ? SlotOf(r) : "-");
					if (r) { Dump(ctx, r, "uidraw-rt0-before"); r->Release(); }
					rtv->Release();
				}
			}
			for (auto* srv : srvs) if (srv) srv->Release();
		}

		void OnDraw(ID3D11DeviceContext* ctx)
		{
			if (!g_active) return;
			if (g_capturing && !g_uiDrawCaptured && g_phase == Phase::kUi) CaptureUiDraw(ctx);
			if (BeforeSeam()) {
				if (g_phase == Phase::kBeforeSeam) OnWriteDraw(ctx);
			} else if (g_phase != Phase::kBeforeSeam) {
				OnReadDraw(ctx);
			}
		}

		void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* c, UINT a, UINT b, INT d) { if (!FromSelf(_ReturnAddress())) OnDraw(c); g_drawIndexed(c, a, b, d); }
		void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* c, UINT a, UINT b) { if (!FromSelf(_ReturnAddress())) OnDraw(c); g_draw(c, a, b); }
		void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) { if (!FromSelf(_ReturnAddress())) OnDraw(c); g_drawIndexedInstanced(c, a, b, d, e, f); }
		void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) { if (!FromSelf(_ReturnAddress())) OnDraw(c); g_drawInstanced(c, a, b, d, e); }
		void STDMETHODCALLTYPE CopyRegionHook(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box)
		{
			if (g_active && g_phase != Phase::kBeforeSeam && !BeforeSeam() && !FromSelf(_ReturnAddress())) Report(src, "copied (CopySubresourceRegion)");
			g_copyRegion(c, dst, ds, x, y, z, src, ss, box);
		}
		void STDMETHODCALLTYPE CopyHook(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
		{
			if (g_active && g_phase != Phase::kBeforeSeam && !BeforeSeam() && !FromSelf(_ReturnAddress())) Report(src, "copied (CopyResource)");
			g_copy(c, dst, src);
		}

		template <class T>
		void PatchSlot(void** vtable, int index, T hook, T& original)
		{
			DWORD old = 0;
			if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return;
			original = reinterpret_cast<T>(vtable[index]);
			vtable[index] = reinterpret_cast<void*>(hook);
			VirtualProtect(&vtable[index], sizeof(void*), old, &old);
		}

		void Install(ID3D11DeviceContext* ctx)
		{
			g_installed = true;
			auto** vtable = *reinterpret_cast<void***>(ctx);
			PatchSlot<DrawIndexed_t>(vtable, 12, &DrawIndexedHook, g_drawIndexed);
			PatchSlot<Draw_t>(vtable, 13, &DrawHook, g_draw);
			PatchSlot<DrawIndexedInstanced_t>(vtable, 20, &DrawIndexedInstancedHook, g_drawIndexedInstanced);
			PatchSlot<DrawInstanced_t>(vtable, 21, &DrawInstancedHook, g_drawInstanced);
			PatchSlot<CopySubresourceRegion_t>(vtable, 46, &CopyRegionHook, g_copyRegion);
			PatchSlot<CopyResource_t>(vtable, 47, &CopyHook, g_copy);
			const bool ok = g_drawIndexed && g_draw && g_drawIndexedInstanced && g_drawInstanced && g_copyRegion && g_copy;
			F4SE::log::info("f4sr: dynres audit hooks {} on the game context", ok ? "installed" : "INCOMPLETE");
		}

		bool AuditMenuOpen()
		{
			const auto* ui = RE::UI::GetSingleton();
			if (!ui) return false;
			static const RE::BSFixedString menus[]{ "TerminalMenu", "PipboyMenu", "VATSMenu", "WorkshopMenu", "ExamineMenu", "PowerArmorModMenu" };
			for (const auto& m : menus)
				if (ui->GetMenuOpen(m)) return true;
			return false;
		}
	}

	void OnFrameStart()
	{
		// Menus fade out for a few frames after they report closed; keep the
		// audit (and the UI relayout) running through that.
		static int linger = 0;
		if (AuditMenuOpen()) linger = 90; else if (linger > 0) --linger;
		const bool want = GetSettings().dynResAudit || linger > 0;
		if (want && !g_installed) {
			if (auto* data = RE::BSGraphics::RendererData::GetSingleton(); data && data->context)
				Install(reinterpret_cast<ID3D11DeviceContext*>(data->context));
		}
		if (want != g_active) F4SE::log::info("f4sr: dynres audit {}", want ? "active" : "idle");
		g_active = want && g_installed;
		if (g_capturing) F4SE::log::info("f4sr: dynres capture {} done ({} files)", g_captureId, g_captureFiles);
		g_capturing = false;
		const int key = GetSettings().captureKey;
		const bool down = key > 0 && key < 256 && (GetAsyncKeyState(key) & 0x8000) != 0;
		if (down && !g_keyDown && g_active) {
			g_capturing = true;
			g_uiDrawCaptured = false;
			g_captureFiles = 0;
			g_captureId = GetTickCount64();
			F4SE::log::info("f4sr: dynres capture {} armed for this frame", g_captureId);
		}
		g_keyDown = down;
		g_phase = Phase::kBeforeSeam;
		g_written.clear();
	}

	void OnSeamRelayout();

	void OnSeam()
	{
		if (g_phase != Phase::kBeforeSeam) return;
		g_phase = Phase::kImagespace;
		OnSeamRelayout();
	}

	namespace
	{
		void Relayout(ID3D11DeviceContext* ctx, RE::BSGraphics::RendererData* data, ID3D11Resource* res, const Write& w, const char* when)
		{
			ID3D11RenderTargetView* rtv{};
			for (int i = 0; i < 101; ++i)
				if (reinterpret_cast<ID3D11Resource*>(data->renderTargets[i].texture) == res) rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderTargets[i].rtView);
			const std::string slotTag = [&] { auto s = SlotOf(res); for (auto& c : s) if (c == '[' || c == ']' || c == '.' || c == ' ') c = '_'; return s; }();
			if (g_capturing) Dump(ctx, res, slotTag + "-before-relayout");
			ID3D11Texture2D* tex{};
			bool ok = false;
			if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex))) && tex) {
				ok = g_relayout.Apply(ctx, tex, rtv, w.vpW, w.vpH);
				tex->Release();
			}
			if (g_capturing) Dump(ctx, res, slotTag + "-after-relayout");
			static std::unordered_set<ID3D11Resource*> logged;
			if (logged.insert(res).second)
				F4SE::log::info("f4sr: dynres audit: {} ({}) re-laid out {} {:.0f}x{:.0f} -> {}x{}: {}", static_cast<void*>(res), SlotOf(res), when, w.vpW, w.vpH, w.texW, w.texH, ok ? "ok" : "FAILED");
		}
	}

	void OnSeamRelayout()
	{
		if (!g_active || !GetSettings().uiRelayout || g_written.empty()) return;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!data) return;
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		for (const int slot : kSeamSlots) {
			auto* res = reinterpret_cast<ID3D11Resource*>(data->renderTargets[slot].texture);
			const auto it = res ? g_written.find(res) : g_written.end();
			if (it == g_written.end()) continue;
			const Write w = it->second;
			g_written.erase(it);
			Relayout(ctx, data, res, w, "at the seam");
		}
	}

	void OnPostImagespace()
	{
		g_phase = Phase::kUi;
		if (!g_active || !GetSettings().uiRelayout || g_written.empty()) return;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!data) return;
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		static std::unordered_set<ID3D11Resource*> logged;
		for (auto it = g_written.begin(); it != g_written.end();) {
			auto* res = it->first;
			if (!g_uiReaders.contains(res)) { ++it; continue; }
			ID3D11RenderTargetView* rtv{};
			for (int i = 0; i < 101; ++i)
				if (reinterpret_cast<ID3D11Resource*>(data->renderTargets[i].texture) == res) rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderTargets[i].rtView);
			ID3D11Texture2D* tex{};
			bool ok = false;
			const Write w = it->second;
			it = g_written.erase(it);  // the relayout's own copy and draw are not findings
			const std::string slotTag = [&] { auto s = SlotOf(res); for (auto& c : s) if (c == '[' || c == ']' || c == '.' || c == ' ') c = '_'; return s; }();
			if (g_capturing) Dump(ctx, res, slotTag + "-before-relayout");
			if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex))) && tex) {
				ok = g_relayout.Apply(ctx, tex, rtv, w.vpW, w.vpH);
				tex->Release();
			}
			if (g_capturing) Dump(ctx, res, slotTag + "-after-relayout");
			if (logged.insert(res).second)
				F4SE::log::info("f4sr: dynres audit: {} ({}) re-laid out {:.0f}x{:.0f} -> {}x{}: {}", static_cast<void*>(res), SlotOf(res), w.vpW, w.vpH, w.texW, w.texH, ok ? "ok" : "FAILED");
		}
	}
}
