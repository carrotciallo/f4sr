#include "menu_effects.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REL/REL.h"
#include "settings.h"

namespace f4sr::menu_effects
{
	namespace
	{
		// RenderTargetManager dynamic-resolution state (see frame_pipeline.cpp).
		constexpr std::uint64_t kRenderTargetManagerId = 1508457;
		constexpr std::size_t kRatioW = 0xF88, kRatioH = 0xF8C, kActivated = 0xFA8;

		enum class Kind : std::uint8_t { kUnknown, kScene, kSeparate };

		struct Effect
		{
			const char* name;
			std::uint64_t vtableId;
			const char* rttiName;  // checked before patching
			using Update_t = bool (*)(void*, void*);
			using Render_t = void (*)(void*, void*, void*);
			Update_t update{};
			Render_t render{};
			Kind kind{ Kind::kUnknown };
			bool loggedForced{};
		};

		// Vtable IDs from the 2026-11-22 static audit (docs/2026-11-22-dynres-static-audit.md).
		std::array<Effect, 5> g_effects{ {
			{ "PipboyScreen", 972075, ".?AVImageSpaceEffectPipboyScreen@@" },
			{ "HUDGlass", 1518400, ".?AVImageSpaceEffectHUDGlass@@" },
			{ "ModMenu", 42572, ".?AVImageSpaceEffectModMenu@@" },
			{ "VatsTarget", 886442, ".?AVImageSpaceEffectVatsTarget@@" },
			{ "FullScreenBlur", 944662, ".?AVImageSpaceEffectFullScreenBlur@@" },
		} };

		std::byte* Manager() { return reinterpret_cast<std::byte*>(REL::ID(kRenderTargetManagerId).address()); }

		bool BeforeSeamSubNative(float& w, float& h, bool& on)
		{
			auto* m = Manager();
			w = *reinterpret_cast<float*>(m + kRatioW);
			h = *reinterpret_cast<float*>(m + kRatioH);
			on = *reinterpret_cast<bool*>(m + kActivated);
			return on && (w < 1.0f || h < 1.0f);
		}

		void WriteState(float w, float h, bool on)
		{
			auto* m = Manager();
			*reinterpret_cast<float*>(m + kRatioW) = w;
			*reinterpret_cast<float*>(m + kRatioH) = h;
			*reinterpret_cast<bool*>(m + kActivated) = on;
		}

		// Ratio 1 for the duration of one call; the guard ignores writes from
		// this module, and the driven values are back before the seam reads them.
		struct NativeScope
		{
			float w{}, h{};
			bool on{}, active{};
			explicit NativeScope(bool apply)
			{
				if (apply && BeforeSeamSubNative(w, h, on)) {
					WriteState(1.0f, 1.0f, false);
					active = true;
				}
			}
			~NativeScope()
			{
				if (active) WriteState(w, h, on);
			}
		};

		ID3D11DeviceContext* Context()
		{
			auto* data = RE::BSGraphics::RendererData::GetSingleton();
			return data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
		}

		// The upscaler's colour input: the RendererData slot the pipeline reads.
		ID3D11Resource* SceneTexture()
		{
			auto* data = RE::BSGraphics::RendererData::GetSingleton();
			const int slot = GetSettings().colorSlot;
			if (!data || slot < 0 || slot >= 101) return nullptr;
			return reinterpret_cast<ID3D11Resource*>(data->renderTargets[slot].texture);
		}

		void Classify(Effect& e, bool beforeSeam, float ratio)
		{
			auto* ctx = Context();
			if (!ctx) return;
			Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
			ctx->OMGetRenderTargets(1, &rtv, nullptr);
			Microsoft::WRL::ComPtr<ID3D11Resource> res;
			UINT w = 0, h = 0;
			if (rtv) {
				rtv->GetResource(&res);
				Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
				if (res && SUCCEEDED(res.As(&tex))) {
					D3D11_TEXTURE2D_DESC d{};
					tex->GetDesc(&d);
					w = d.Width;
					h = d.Height;
				}
			}
			UINT nvp = 1;
			D3D11_VIEWPORT vp{};
			ctx->RSGetViewports(&nvp, &vp);
			const bool scene = res && res.Get() == SceneTexture();
			e.kind = scene ? Kind::kScene : Kind::kSeparate;
			F4SE::log::info("f4sr: menu effect {} first seen {} the seam (ratio {:.3f}): target {} {}x{}, viewport {:.0f}x{:.0f} -> {}",
				e.name, beforeSeam ? "before" : "after", ratio, scene ? "is the upscaler input" : "is separate", w, h, vp.Width, vp.Height,
				!beforeSeam ? "nothing to do" : scene ? "left at the render ratio" : "runs at ratio 1 from now on");
		}

		bool ShouldForce(Effect& e)
		{
			if (!GetSettings().menuEffectFix || e.kind != Kind::kSeparate) return false;
			float w, h;
			bool on;
			if (!BeforeSeamSubNative(w, h, on)) return false;
			if (!e.loggedForced) {
				e.loggedForced = true;
				F4SE::log::info("f4sr: menu effect {} drawn at ratio 1 before the seam (was {:.3f})", e.name, w);
			}
			return true;
		}

		template <std::size_t I>
		bool UpdateThunk(void* self, void* param)
		{
			auto& e = g_effects[I];
			NativeScope scope(ShouldForce(e));
			return e.update(self, param);
		}

		template <std::size_t I>
		void RenderThunk(void* self, void* geometry, void* param)
		{
			auto& e = g_effects[I];
			if (e.kind == Kind::kUnknown) {
				float w, h;
				bool on;
				const bool before = BeforeSeamSubNative(w, h, on);
				e.render(self, geometry, param);
				Classify(e, before, w);
				return;
			}
			NativeScope scope(ShouldForce(e));
			e.render(self, geometry, param);
		}

		bool RttiMatches(std::uintptr_t* vtable, const char* expected)
		{
			// vtable[-1] -> RTTICompleteObjectLocator (x64: image-relative offsets).
			const auto col = vtable[-1];
			const auto base = REL::Module::get().base();
			const auto typeDesc = base + *reinterpret_cast<const std::uint32_t*>(col + 12);
			return std::strcmp(reinterpret_cast<const char*>(typeDesc + 16), expected) == 0;
		}

		bool Patch(std::uintptr_t* slot, std::uintptr_t value)
		{
			DWORD old = 0;
			if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
			*slot = value;
			VirtualProtect(slot, sizeof(*slot), old, &old);
			return true;
		}

		template <std::size_t I>
		void InstallOne()
		{
			auto& e = g_effects[I];
			auto* vtable = reinterpret_cast<std::uintptr_t*>(REL::ID(e.vtableId).address());
			if (!RttiMatches(vtable, e.rttiName)) {
				F4SE::log::warn("f4sr: menu effect {}: vtable RTTI mismatch, not hooked", e.name);
				return;
			}
			e.render = reinterpret_cast<Effect::Render_t>(vtable[1]);
			e.update = reinterpret_cast<Effect::Update_t>(vtable[9]);
			if (!Patch(&vtable[1], reinterpret_cast<std::uintptr_t>(&RenderThunk<I>)) ||
				!Patch(&vtable[9], reinterpret_cast<std::uintptr_t>(&UpdateThunk<I>))) {
				F4SE::log::error("f4sr: menu effect {}: vtable patch failed", e.name);
			}
		}

		template <std::size_t... I>
		void InstallAll(std::index_sequence<I...>)
		{
			(InstallOne<I>(), ...);
		}

		// Interface3D::Renderer screen pass (REL 917134, RVA 0xAE9020): scales a
		// target width by the dynres ratio (0xAE9605) to pick how its render
		// target and viewport are bound. The terminal glow is drawn at the
		// ratio's scale although every texture it could read is in display
		// layout (2026-11-22 capture), so its geometry is placed with the
		// ratio seen here. Called from three sites; signature from the callers:
		// (rcx, rdx, r8, r9, int [rsp+20], bool [rsp+28], bool [rsp+30]).
		using I3dRender_t = std::uintptr_t (*)(void*, void*, void*, void*, std::int32_t, bool, bool);
		constexpr std::uintptr_t kI3dRenderRva = 0xAE9020;
		constexpr std::uintptr_t kI3dSites[]{ 0xAE862B, 0xAE8B20, 0xAE998E };
		I3dRender_t g_i3dOriginal{};
		bool g_i3dLogged[std::size(kI3dSites)]{};

		template <std::size_t S>
		std::uintptr_t I3dRenderThunk(void* a, void* b, void* c, void* d, std::int32_t e, bool f, bool g)
		{
			float w, h;
			bool on;
			const bool before = BeforeSeamSubNative(w, h, on);
			if (!g_i3dLogged[S]) {
				g_i3dLogged[S] = true;
				F4SE::log::info("f4sr: Interface3D screen pass (site {}) first called {} the seam (ratio {:.3f}){}", S,
					before ? "before" : "after", w, before && GetSettings().menuEffectFix ? ": runs at ratio 1" : "");
			}
			NativeScope scope(before && GetSettings().menuEffectFix);
			return g_i3dOriginal(a, b, c, d, e, f, g);
		}

		template <std::size_t S>
		void InstallI3dSite()
		{
			const auto base = REL::Module::get().base();
			const auto site = base + kI3dSites[S];
			const auto* p = reinterpret_cast<const std::uint8_t*>(site);
			const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
			if (p[0] != 0xE8 || target != base + kI3dRenderRva) {
				F4SE::log::warn("f4sr: Interface3D screen pass site {} does not call 0x{:X}; not hooked", S, kI3dRenderRva);
				return;
			}
			auto& trampoline = F4SE::GetTrampoline();
			const auto original = trampoline.write_call<5>(site, &I3dRenderThunk<S>);
			if (!g_i3dOriginal) g_i3dOriginal = reinterpret_cast<I3dRender_t>(original);
		}
	}

	void Install()
	{
		InstallAll(std::make_index_sequence<g_effects.size()>{});
		if (F4SE::GetTrampoline().free_size() >= 3 * 14) {
			InstallI3dSite<0>();
			InstallI3dSite<1>();
			InstallI3dSite<2>();
		} else {
			F4SE::log::warn("f4sr: Interface3D screen pass not hooked (trampoline space)");
		}
		F4SE::log::info("f4sr: menu effect hooks installed (fix {})", GetSettings().menuEffectFix ? "on" : "off");
	}
}
