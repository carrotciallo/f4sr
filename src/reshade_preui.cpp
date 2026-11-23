#include "reshade_preui.h"

#include "settings.h"
#include "xefg_presenter.h"
#include "post_depth_remap.h"

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <d3d12.h>
#include <format>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>
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

	// F4SR's depth bound as DEPTH on the XeSS-FG output runtime.
	struct DepthBinding
	{
		api::effect_runtime* runtime{};
		void* resource{};
		std::uint32_t generation{};
		api::resource_view srv{};
		bool bound{};
	};
	DepthBinding g_depth;

	// UI Native: effects at the ENB position (render size, before upscaling)
	// on the output runtime, recorded into F4SR's own D3D12 command list.
	std::atomic<bool> g_preMode{};     // effects rendered before upscaling; presents only mark them done
	bool g_preFailed{};                // could not run this session (see log); after the UI instead
	std::uint64_t g_preRenders{};
	std::atomic<std::uint32_t> g_preWidth{};  // effect width before upscaling (BUFFER_WIDTH)
	thread_local bool tl_expectList{};
	thread_local api::command_list* tl_capturedList{};
	thread_local void* tl_preDepth{};  // DEPTH for the pre-upscale pass (ID3D12Resource*)
	thread_local bool tl_inPreRender{};
	thread_local bool tl_holdingFrame{};
	// Held by a presenting thread from the present event to reshade_present
	// (ReShade's update, overlay and uniform work) and by the game thread
	// while it records the effects, which ReShade never expects concurrently.
	std::timed_mutex g_frameLock;
	struct ColorViews
	{
		api::effect_runtime* runtime{};
		void* resource{};
		api::resource_view rtv{}, rtvSrgb{};
	};
	ColorViews g_preViews;

	void ReleasePreViews()
	{
		if (g_preViews.runtime) {
			auto* device = g_preViews.runtime->get_device();
			if (g_preViews.rtv.handle) device->destroy_resource_view(g_preViews.rtv);
			if (g_preViews.rtvSrgb.handle && g_preViews.rtvSrgb.handle != g_preViews.rtv.handle) device->destroy_resource_view(g_preViews.rtvSrgb);
		}
		g_preViews = {};
	}

	void PreFailed(const char* why)
	{
		if (!g_preFailed) {
			g_preFailed = true;
			F4SE::log::error("f4sr: ReShade before upscaling: {}; effects render after the UI instead", why);
		}
		g_preMode = false;
	}

	void OnResetCommandList(api::command_list* cmd)
	{
		if (tl_expectList) tl_capturedList = cmd;
	}

	bool IsOutputChain(api::swapchain* swapchain, api::effect_runtime** runtime)
	{
		std::lock_guard lock{ g_lock };
		for (auto* r : g_outputs)
			if (r->get_device()->get_api() != api::device_api::d3d11 && r->get_native() == swapchain->get_native()) {
				*runtime = r;
				return true;
			}
		return false;
	}

	// Every Present on the output chain, real or generated: in pre-upscale
	// mode the effects are already in the image, so only mark them rendered
	// (a null target updates the uniforms and renders nothing).
	void OnPresentEvent(api::command_queue*, api::swapchain* swapchain, const api::rect*, const api::rect*, std::uint32_t, const api::rect*)
	{
		if (tl_holdingFrame) {  // reshade_present did not follow the last one
			tl_holdingFrame = false;
			g_frameLock.unlock();
		}
		api::effect_runtime* runtime{};
		if (!IsOutputChain(swapchain, &runtime)) return;
		g_frameLock.lock();
		tl_holdingFrame = true;
		if (g_preMode.load(std::memory_order_acquire))
			runtime->render_effects(runtime->get_command_queue()->get_immediate_command_list(), api::resource_view{}, api::resource_view{});
	}

	void OnReShadePresent(api::effect_runtime*)
	{
		if (tl_holdingFrame) {
			tl_holdingFrame = false;
			g_frameLock.unlock();
		}
	}

	// The view bound to the first ReShade.fxh DEPTH texture (DepthBufferTex),
	// and whether any loaded effect has one.
	bool CurrentDepthBinding(api::effect_runtime* runtime, std::uint64_t& handle)
	{
		struct Found
		{
			bool any;
			std::uint64_t handle;
		} found{ false, 0 };
		runtime->enumerate_texture_variables(nullptr, [](api::effect_runtime* rt, api::effect_texture_variable var, void* user) {
			auto& f = *static_cast<Found*>(user);
			if (f.any) return;
			char name[64]{};
			rt->get_texture_variable_name(var, name);
			if (std::strcmp(name, "DepthBufferTex") != 0) return;  // ReShade.fxh's DEPTH texture
			api::resource_view srv{}, srgb{};
			rt->get_texture_binding(var, &srv, &srgb);
			f.any = true;
			f.handle = srv.handle;
		}, &found);
		handle = found.handle;
		return found.any;
	}

	api::effect_runtime* OutputRuntimeLocked()
	{
		for (auto* runtime : g_outputs)
			if (runtime->get_device()->get_api() != api::device_api::d3d11) return runtime;
		return nullptr;
	}

	// Classic (D3D11 runtime on the game's chain): after a sub-native resolve
	// the engine depth is already in display layout (post_depth), but
	// ReShade's own depth detection does not pick it. Render thread only (the
	// game's runtime renders and presents there), so no lock.
	struct ClassicDepth
	{
		api::effect_runtime* runtime{};
		ID3D11Texture2D* texture{};
		api::resource_view srv{};
		bool bound{};
	};
	ClassicDepth g_classicDepth;

	void BindClassicDepth(api::effect_runtime* runtime)
	{
		auto& c = g_classicDepth;
		const auto& pd = f4sr::post_depth::g;
		ID3D11Texture2D* texture = f4sr::GetSettings().reshadeDepth && pd.active ? pd.source : nullptr;
		if (!texture) return;  // native layout: leave ReShade's own detection alone
		auto* device = runtime->get_device();
		if (runtime != c.runtime || texture != c.texture) {
			if (c.srv.handle && c.runtime) c.runtime->get_device()->destroy_resource_view(c.srv);
			c = { runtime, texture, {}, false };
			D3D11_TEXTURE2D_DESC d{};
			texture->GetDesc(&d);
			const api::resource res{ reinterpret_cast<std::uint64_t>(texture) };
			if (!device->create_resource_view(res, api::resource_usage::shader_resource,
					api::resource_view_desc(api::resource_view_type::texture_2d, api::format::r24_unorm_x8_uint, 0, 1, 0, 1), &c.srv)) {
				F4SE::log::warn("f4sr: ReShade depth (Classic): view of the {}x{} engine depth could not be created", d.Width, d.Height);
				c.srv = {};
				return;
			}
			F4SE::log::info("f4sr: ReShade depth (Classic): engine depth {}x{} in display layout bound as DEPTH", d.Width, d.Height);
		}
		if (!c.srv.handle) return;
		// Bound every pass: ReShade's own depth detection may rebind DEPTH.
		runtime->update_texture_bindings("DEPTH", c.srv, c.srv);
		c.bound = true;
	}

	void ReleaseDepthView()
	{
		if (g_depth.srv.handle && g_depth.runtime) g_depth.runtime->get_device()->destroy_resource_view(g_depth.srv);
		g_depth.srv = {};
	}

	// Each effect pass on the output runtime (any presenting thread): keep
	// DEPTH pointing at the current copy, or cleared when the option is off.
	void OnBeginEffects(api::effect_runtime* runtime, api::command_list*, api::resource_view, api::resource_view)
	{
		if (runtime->get_device()->get_api() == api::device_api::d3d11) {
			BindClassicDepth(runtime);
			return;
		}
		std::lock_guard lock{ g_lock };
		std::uint32_t generation{};
		void* resource = nullptr;
		if (f4sr::GetSettings().reshadeDepth) resource = tl_inPreRender ? tl_preDepth : f4sr::xefg::ReShadeDepth12(generation);
		if (runtime == g_depth.runtime && resource == g_depth.resource && generation == g_depth.generation) {
			// Effects reloaded after the bind (the pre-upscale size switch,
			// 2026-11-23) came up without it: check now and then, rebind.
			static std::uint32_t passes = 0;
			if (!g_depth.bound || ++passes % 30 != 0) return;
			std::uint64_t current{};
			if (!CurrentDepthBinding(runtime, current) || current == g_depth.srv.handle) return;
			static std::uint32_t taken = 0;
			if (++taken <= 3 || taken % 1000 == 0)
				F4SE::log::info("f4sr: ReShade depth: DEPTH lost its binding (view {:#x}, e.g. after an effect reload); bound F4SR's again ({} times)",
					current, taken);
			runtime->update_texture_bindings("DEPTH", g_depth.srv, g_depth.srv);
			return;
		}
		const bool wasBound = g_depth.bound;
		ReleaseDepthView();
		g_depth = { runtime, resource, generation, {}, false };
		if (!resource) {
			if (wasBound) {
				runtime->update_texture_bindings("DEPTH", api::resource_view{}, api::resource_view{});
				F4SE::log::info("f4sr: ReShade depth: unbound");
			}
			return;
		}
		auto* device = runtime->get_device();
		const api::resource res{ reinterpret_cast<std::uint64_t>(resource) };
		const auto desc = device->get_resource_desc(res);
		if (!device->create_resource_view(res, api::resource_usage::shader_resource,
				api::resource_view_desc(api::resource_view_type::texture_2d, api::format::r32_float, 0, 1, 0, 1), &g_depth.srv)) {
			F4SE::log::warn("f4sr: ReShade depth: view of the {}x{} copy could not be created", desc.texture.width, desc.texture.height);
			return;
		}
		runtime->update_texture_bindings("DEPTH", g_depth.srv, g_depth.srv);
		g_depth.bound = true;
		F4SE::log::info("f4sr: ReShade depth: F4SR's {}x{} depth bound as DEPTH on the output runtime ({})", desc.texture.width, desc.texture.height,
			tl_inPreRender ? "before upscaling, this frame's depth" : "after the UI");
	}

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
		if (runtime == g_depth.runtime) {
			ReleaseDepthView();
			g_depth = {};
		}
		if (runtime == g_preViews.runtime) ReleasePreViews();
		if (runtime == g_classicDepth.runtime) {
			if (g_classicDepth.srv.handle) runtime->get_device()->destroy_resource_view(g_classicDepth.srv);
			g_classicDepth = {};
		}
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
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
		reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetCommandList);
		reshade::register_event<reshade::addon_event::present>(&OnPresentEvent);
		reshade::register_event<reshade::addon_event::reshade_present>(&OnReShadePresent);
		g_registered = true;
		F4SE::log::info("f4sr: registered as a ReShade add-on (API {})", RESHADE_API_VERSION);
	}

	bool Active() { return g_active; }

	bool Registered() { return g_registered; }

	bool PreUpscaleWanted()
	{
		if (!g_registered || g_preFailed || !GetSettings().reshadeBeforeUi) return false;
		std::lock_guard lock{ g_lock };
		// Not tied to the effects toggle: switching modes reloads every
		// effect at the other size, and a disabled runtime renders nothing.
		return OutputRuntimeLocked() != nullptr;
	}

	void ExpectCommandList(bool on)
	{
		tl_expectList = on;
		if (on) tl_capturedList = nullptr;
	}

	bool RenderPreUpscale(void* colorResource, void* depthResource)
	{
		api::effect_runtime* runtime{};
		{
			std::lock_guard lock{ g_lock };
			runtime = OutputRuntimeLocked();
		}
		auto* cmd = std::exchange(tl_capturedList, nullptr);
		if (!runtime || !colorResource) return false;
		if (!cmd) {
			PreFailed("F4SR's D3D12 command list is not visible to ReShade (device created outside ReShade's hooks)");
			return false;
		}
		auto* device = runtime->get_device();
		if (cmd->get_device() != device) {
			PreFailed("F4SR's D3D12 device differs from the output runtime's device");
			return false;
		}
		if (g_preViews.runtime != runtime || g_preViews.resource != colorResource) {
			ReleasePreViews();
			const api::resource res{ reinterpret_cast<std::uint64_t>(colorResource) };
			const auto desc = device->get_resource_desc(res);
			const auto typeless = api::format_to_typeless(desc.texture.format);
			const auto linear = api::format_to_default_typed(typeless, 0);
			const auto srgb = api::format_to_default_typed(typeless, 1);
			g_preViews.runtime = runtime;
			g_preViews.resource = colorResource;
			if (!device->create_resource_view(res, api::resource_usage::render_target,
					api::resource_view_desc(api::resource_view_type::texture_2d, linear, 0, 1, 0, 1), &g_preViews.rtv)) {
				ReleasePreViews();
				PreFailed("render target view of the render-size frame could not be created");
				return false;
			}
			if (srgb == linear || !device->create_resource_view(res, api::resource_usage::render_target,
									  api::resource_view_desc(api::resource_view_type::texture_2d, srgb, 0, 1, 0, 1), &g_preViews.rtvSrgb))
				g_preViews.rtvSrgb = g_preViews.rtv;
			g_preWidth = desc.texture.width;
			const auto displayW = xefg::OutputWidth();
			F4SE::log::info("f4sr: ReShade before upscaling: {}x{} format {} target on the output runtime (effects reload at this size)",
				desc.texture.width, desc.texture.height, static_cast<unsigned>(desc.texture.format));
			if (displayW && displayW != desc.texture.width)
				F4SE::log::info("f4sr: ReShade before upscaling: effects sized in pixels (Layer, Border, ASCII, CRT, grain) appear {:.2f}x larger "
								"than at {} wide; multiply their size settings by {:.3f} (e.g. Layer_Scale) to match",
					static_cast<double>(displayW) / desc.texture.width, displayW, static_cast<double>(desc.texture.width) / displayW);
		}
		if (!g_frameLock.try_lock_for(std::chrono::milliseconds(100))) {
			static std::uint64_t busy = 0;
			if (++busy <= 3 || busy % 600 == 0)
				F4SE::log::warn("f4sr: ReShade before upscaling: output present busy for 100 ms; effects skipped this frame ({} so far)", busy);
			return false;
		}
		tl_inPreRender = true;
		tl_preDepth = depthResource;
		runtime->render_effects(cmd, g_preViews.rtv, g_preViews.rtvSrgb);
		tl_inPreRender = false;
		tl_preDepth = nullptr;
		g_preMode.store(true, std::memory_order_release);
		g_frameLock.unlock();
		g_active = true;
		if (++g_preRenders == 1 || g_preRenders % 3600 == 0)
			F4SE::log::info("f4sr: ReShade effects rendered before upscaling ({} frames)", g_preRenders);
		return true;
	}

	void ReleasePreUpscaleTargets()
	{
		std::lock_guard frame{ g_frameLock };  // no present is using the runtime
		g_preMode = false;
		ReleasePreViews();
		std::lock_guard lock{ g_lock };
		if (g_depth.runtime && g_depth.bound) {
			// The next effects pass binds the after-UI depth (or none).
			g_depth.runtime->update_texture_bindings("DEPTH", api::resource_view{}, api::resource_view{});
			ReleaseDepthView();
			g_depth = {};
		}
	}

	bool WantsDepth()
	{
		if (!g_registered || !GetSettings().reshadeDepth || g_preMode.load()) return false;
		std::lock_guard lock{ g_lock };
		return OutputRuntimeLocked() != nullptr;
	}

	std::string PanelLine()
	{
		if (!g_registered) return {};
		std::lock_guard lock{ g_lock };
		const bool output = OutputRuntimeLocked() != nullptr;
		std::string out = "RS\t";
		if (output) {
			const bool pre = g_preMode.load();
			out += pre ? "ReShade with ENB (before upscaling)" : "ReShade after UI";
			if (!GetSettings().reshadeDepth) out += "   depth off";
			else out += g_depth.bound ? "   depth F4SR" : "   !depth waiting";
			out += "\n";
			const auto preW = g_preWidth.load(), displayW = xefg::OutputWidth();
			if (pre && preW && displayW && preW != displayW)
				out += std::format("\tpixel-sized effects x{:.2f} (scale them by {:.2f})\n", static_cast<double>(displayW) / preW,
					static_cast<double>(preW) / displayW);
			if (GetSettings().reshadeBeforeUi && !pre)
				out += g_preFailed ? "\t!before UI failed: see f4sr.log\n" : (xefg::OutputScaling() ? "" : "\t!before UI: needs UI Native\n");
		} else {
			out += g_active ? "ReShade before UI" : "ReShade after UI";
			if (!GetSettings().reshadeDepth) out += "   depth off";
			else if (g_classicDepth.bound) out += "   depth F4SR";
			out += "\n";
		}
		return out;
	}

	bool IsReShadeModule(HMODULE module)
	{
		return g_registered && module != nullptr && module == reshade::internal::get_reshade_module_handle();
	}

	bool OutputRuntimeSeen()
	{
		std::lock_guard lock{ g_lock };
		return std::any_of(g_outputs.begin(), g_outputs.end(),
			[](api::effect_runtime* runtime) { return runtime->get_device()->get_api() != api::device_api::d3d11; });
	}

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
			if (!told) F4SE::log::warn("f4sr: ReShade before the UI with XeSS-FG needs the UI Native pipeline; effects stay at presentation");
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

	void AfterPathB(bool renderedBeforeUpscale)
	{
		g_active = renderedBeforeUpscale;
		if (!g_registered) return;
		static bool wasPre = false;
		if (renderedBeforeUpscale) wasPre = true;
		else if (wasPre && !PreUpscaleWanted()) {
			wasPre = false;
			g_preMode = false;  // option turned off: ReShade renders at presentation again
			F4SE::log::info("f4sr: ReShade before upscaling off; effects render after the UI (they reload at display size)");
		}
		std::lock_guard lock{ g_lock };
		if (g_outputsMuted) MuteOutputs(false);
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
