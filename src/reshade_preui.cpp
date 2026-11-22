#include "reshade_preui.h"

#include "settings.h"
#include "xefg_presenter.h"

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <vector>

#include <reshade.hpp>

using Microsoft::WRL::ComPtr;
namespace api = reshade::api;

namespace
{
	std::mutex g_lock;
	api::effect_runtime* g_game{};               // D3D11 runtime on the game's chain
	std::vector<api::effect_runtime*> g_outputs; // other runtimes (XeFG's D3D12 output chain)
	bool g_registered{};
	bool g_active{};          // effects rendered before the UI this frame
	bool g_outputsMuted{};    // we turned the other runtimes' effects off
	std::uint64_t g_renders{};

	// Render target views of the frame buffer, created through ReShade's device.
	ID3D11Texture2D* g_viewTexture{};
	api::resource_view g_rtv{}, g_rtvSrgb{};

	ComPtr<ID3DDeviceContextState> g_state;  // clean state block for the effects

	void DestroyViews()
	{
		if (g_game && g_viewTexture) {
			auto* device = g_game->get_device();
			if (g_rtv.handle) device->destroy_resource_view(g_rtv);
			if (g_rtvSrgb.handle && g_rtvSrgb.handle != g_rtv.handle) device->destroy_resource_view(g_rtvSrgb);
		}
		g_rtv = g_rtvSrgb = {};
		g_viewTexture = nullptr;
	}

	void OnInitRuntime(api::effect_runtime* runtime)
	{
		std::lock_guard lock{ g_lock };
		const auto deviceApi = runtime->get_device()->get_api();
		if (deviceApi == api::device_api::d3d11 && g_game == nullptr) {
			g_game = runtime;
			F4SE::log::info("f4sr: ReShade: D3D11 effect runtime {:p} on the game's swap chain", static_cast<void*>(runtime));
		} else {
			g_outputs.push_back(runtime);
			F4SE::log::info("f4sr: ReShade: effect runtime {:p} (API {:#x}) on another swap chain", static_cast<void*>(runtime),
				static_cast<unsigned>(deviceApi));
		}
	}

	void OnDestroyRuntime(api::effect_runtime* runtime)
	{
		std::lock_guard lock{ g_lock };
		if (runtime == g_game) {
			DestroyViews();
			g_state.Reset();
			g_game = nullptr;
		}
		std::erase(g_outputs, runtime);
	}

	void MuteOutputs(bool mute)
	{
		for (auto* runtime : g_outputs) {
			if (mute && runtime->get_effects_state()) runtime->set_effects_state(false);
		}
		if (!mute && g_outputsMuted)
			for (auto* runtime : g_outputs) runtime->set_effects_state(true);
		if (mute != g_outputsMuted && !g_outputs.empty())
			F4SE::log::info("f4sr: ReShade: effects on the other swap chains {}", mute ? "off (rendered before the UI on the game's chain)" : "restored");
		g_outputsMuted = mute;
	}

	// Frame buffer (logical kFrameBuffer) at the pre-interface point, as in
	// FrameGen::CaptureHudless.
	ID3D11Texture2D* FrameBuffer(void* rtManager)
	{
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!rtManager || !data) return nullptr;
		const auto slot = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(rtManager) + 0xDC4);
		if (slot < 0 || slot >= 101) return nullptr;
		auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderTargets[slot].rtView);
		if (!rtv) return nullptr;
		ComPtr<ID3D11Resource> resource;
		rtv->GetResource(&resource);
		ComPtr<ID3D11Texture2D> texture;
		if (!resource || FAILED(resource.As(&texture))) return nullptr;
		return texture.Get();  // owned by the engine's target
	}

	bool EnsureViews(ID3D11Texture2D* texture)
	{
		if (texture == g_viewTexture && g_rtv.handle) return true;
		DestroyViews();
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) return false;
		auto* device = g_game->get_device();
		const auto format = static_cast<api::format>(desc.Format);
		const bool typeless = api::format_to_typeless(format) == format;
		const auto linear = typeless ? api::format_to_default_typed(format, 0) : format;
		const auto srgb = typeless ? api::format_to_default_typed(format, 1) : format;
		const api::resource res{ reinterpret_cast<std::uint64_t>(texture) };
		if (!device->create_resource_view(res, api::resource_usage::render_target,
				api::resource_view_desc(api::resource_view_type::texture_2d, linear, 0, 1, 0, 1), &g_rtv))
			return false;
		if (srgb == linear) {
			g_rtvSrgb = g_rtv;
		} else if (!device->create_resource_view(res, api::resource_usage::render_target,
					   api::resource_view_desc(api::resource_view_type::texture_2d, srgb, 0, 1, 0, 1), &g_rtvSrgb)) {
			g_rtvSrgb = g_rtv;
		}
		g_viewTexture = texture;
		F4SE::log::info("f4sr: ReShade: frame buffer views {}x{} format {} (sRGB view {})", desc.Width, desc.Height,
			static_cast<unsigned>(desc.Format), g_rtvSrgb.handle != g_rtv.handle ? "separate" : "same");
		return true;
	}

	// Runs `work` on the native immediate context with a clean state block
	// swapped in, then swaps the application's state back unchanged (the
	// engine's and ENB's state caches never see a change).
	template <class F>
	bool WithCleanState(ID3D11DeviceContext* native, F&& work)
	{
		ComPtr<ID3D11DeviceContext1> ctx1;
		if (FAILED(native->QueryInterface(IID_PPV_ARGS(&ctx1)))) return false;
		if (!g_state) {
			ComPtr<ID3D11Device> device;
			native->GetDevice(&device);
			ComPtr<ID3D11Device1> device1;
			if (!device || FAILED(device.As(&device1))) return false;
			const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
			if (FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_state)))
				return false;
		}
		ComPtr<ID3DDeviceContextState> previous;
		ctx1->SwapDeviceContextState(g_state.Get(), &previous);
		work();
		ctx1->SwapDeviceContextState(previous.Get(), nullptr);
		return true;
	}
}

namespace f4sr::reshade_preui
{
	void Install(HMODULE self)
	{
		if (reshade::internal::get_reshade_module_handle() == nullptr) return;
		if (!reshade::register_addon(self)) {
			F4SE::log::warn("f4sr: ReShade found but add-on registration failed (needs the ReShade build with add-on support, API {})", RESHADE_API_VERSION);
			return;
		}
		reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
		g_registered = true;
		F4SE::log::info("f4sr: registered as a ReShade add-on (API {})", RESHADE_API_VERSION);
	}

	bool Active() { return g_active; }

	void Render(void* rtManager)
	{
		g_active = false;
		if (!g_registered) return;
		std::lock_guard lock{ g_lock };
		const bool wanted = GetSettings().reshadeBeforeUi;
		if (!wanted || g_game == nullptr) {
			if (g_outputsMuted) MuteOutputs(false);
			return;
		}
		if (xefg::Attached()) {
			// The game chain's ReShade Present never runs under XeSS-FG (its
			// frames end below ENB), so its per-frame updates would stall.
			static bool told = false;
			if (!told) F4SE::log::warn("f4sr: ReShade before the UI is not supported with XeSS-FG yet; effects stay at presentation");
			told = true;
			if (g_outputsMuted) MuteOutputs(false);
			return;
		}
		MuteOutputs(true);
		if (!g_game->get_effects_state()) return;  // the user toggled the effects off
		auto* texture = FrameBuffer(rtManager);
		if (!texture) return;
		if (!EnsureViews(texture)) {
			static bool told = false;
			if (!told) F4SE::log::warn("f4sr: ReShade before the UI: could not create render target views of the frame buffer; skipped");
			told = true;
			return;
		}
		auto* cmd = g_game->get_command_queue()->get_immediate_command_list();
		auto* native = reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());
		if (!native) return;
		const bool ok = WithCleanState(native, [&] { g_game->render_effects(cmd, g_rtv, g_rtvSrgb); });
		if (!ok) {
			static bool told = false;
			if (!told) F4SE::log::warn("f4sr: ReShade before the UI: no ID3D11DeviceContext1 state swap available; skipped");
			told = true;
			return;
		}
		g_active = true;
		if (++g_renders == 1 || g_renders % 3600 == 0)
			F4SE::log::info("f4sr: ReShade effects rendered before the UI ({} frames)", g_renders);
	}

	void SuppressPresentEffects()
	{
		if (!g_active) return;
		std::lock_guard lock{ g_lock };
		if (!g_game) return;
		// A null target renders nothing, updates the uniforms and marks the
		// effects as rendered for the coming present.
		auto* cmd = g_game->get_command_queue()->get_immediate_command_list();
		auto* native = reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());
		if (native) WithCleanState(native, [&] { g_game->render_effects(cmd, api::resource_view{}, api::resource_view{}); });
	}
}
