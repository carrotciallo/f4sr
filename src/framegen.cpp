#include "sr_status.h"
#include "framegen.h"
#include "frame_journal.h"
#include "capture.h"
#include "frame_camera.h"

#include <atomic>
#include <fstream>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "ffx_runtime.h"
#include "settings.h"
#include "gpu_timing.h"
#include "xefg_presenter.h"
#include "swapchain_inner.h"

#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>

#include <cstring>
#include <format>

namespace
{
	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	Present_t g_origPresent{ nullptr };
	f4sr::FrameGen* g_presentOwner{ nullptr };
	IDXGISwapChain* g_hookedChain{ nullptr };

	// With a swap chain wrapper (ENB), the generated and real frames are
	// presented from the inner chain: the wrapper's Present (ENB effects and
	// per-frame state) runs once per game frame and forwards to the inner chain,
	// where OnPresent takes the finished frame. Presenting the generated frame
	// through the wrapper as well ran ENB's per-frame work twice and broke its
	// lighting (dark shadows, missing ambient).
	Present_t g_innerOrigPresent{ nullptr };
	IDXGISwapChain* g_innerChain{ nullptr };
	thread_local bool tl_inWrapperPresent = false;
	bool g_innerHit{ false };
	bool g_innerDisabled{ false };

	HRESULT PresentOn(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
	{
		return chain == g_innerChain && g_innerOrigPresent ? g_innerOrigPresent(chain, syncInterval, flags) : g_origPresent(chain, syncInterval, flags);
	}

	// iFGPresentPath 2 (diagnostic): the old path, both frames through the wrapper.
	bool InnerRouting() { return g_innerChain != nullptr && !g_innerDisabled && f4sr::GetSettings().fgPresentPath != 2; }

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
	{
		thread_local bool inside = false;
		if (inside || chain != g_hookedChain || g_presentOwner == nullptr) {
			return g_origPresent(chain, syncInterval, flags);
		}
		struct Guard { bool& value; ~Guard() { value = false; } } guard{ inside };
		inside = true;
		// Under XeSS-FG the frame ends at the inner Present (after ENB's work).
		if ((flags & DXGI_PRESENT_TEST) == 0 && !f4sr::xefg::Attached())
			if (auto* data = RE::BSGraphics::RendererData::GetSingleton())
				f4sr::gpu_timing::MarkAt(reinterpret_cast<ID3D11DeviceContext*>(data->context), f4sr::gpu_timing::kPresent);
		if ((flags & DXGI_PRESENT_TEST) == 0 && InnerRouting()) {
			g_innerHit = false;
			tl_inWrapperPresent = true;
			const HRESULT hr = g_origPresent(chain, syncInterval, flags);
			tl_inWrapperPresent = false;
			static std::uint32_t missedInRow = 0;
			if (g_innerHit) { missedInRow = 0; return hr; }
			// The wrapper presented without reaching the inner chain: this
			// frame is shown, but without generation. Give up on the inner
			// route after a run of misses.
			if (++missedInRow >= 60) {
				g_innerDisabled = true;
				F4SE::log::warn("f4sr: FG: the wrapper's Present did not reach the inner swap chain 60 times in a row; presenting through the wrapper again");
			}
			return hr;
		}
		return g_presentOwner->OnPresent(chain, syncInterval, flags);
	}

	HRESULT STDMETHODCALLTYPE InnerPresentThunk(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
	{
		if (!tl_inWrapperPresent || chain != g_innerChain || g_presentOwner == nullptr || (flags & DXGI_PRESENT_TEST) != 0)
			return g_innerOrigPresent(chain, syncInterval, flags);
		tl_inWrapperPresent = false;  // one frame per wrapper Present
		g_innerHit = true;
		return g_presentOwner->OnPresent(chain, syncInterval, flags);
	}

	const char* MenuBlockReason()
	{
		const auto* ui = RE::UI::GetSingleton();
		if (!ui) return "UI unavailable";
		if (ui->freezeFramePause || ui->freezeFrameMenuBG || ui->closingAllMenus) return "UI freeze/transition";
		static const RE::BSFixedString menus[] = {
			"LoadingMenu", "MainMenu", "PauseMenu", "PipboyMenu", "Console", "VATSMenu",
			"DialogueMenu", "LooksMenu", "WorkshopMenu", "TerminalMenu", "FaderMenu"
		};
		for (const auto& menu : menus) if (ui->GetMenuOpen(menu)) return menu.c_str();
		return nullptr;
	}

	std::uint32_t SurfaceFormat(DXGI_FORMAT format)
	{
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		default:
			return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		}
	}
}

namespace f4sr
{
	const char* FrameGenMenuBlockReason() { return MenuBlockReason(); }

	bool FrameGen::InstallPresentHook(IDXGISwapChain* gameChain)
	{
		// Installed unconditionally at pipeline init (not only with FG):
		// OnPresent's instrumentation (viewport log, dumps C/D) runs
		// before the FG gate, and the FG path early-outs through
		// g_origPresent when Create never ran.
		if (gameChain == nullptr) return false;
		void** vtable = *reinterpret_cast<void***>(gameChain);
		if (g_origPresent != nullptr && vtable[8] != reinterpret_cast<void*>(&PresentThunk)) {
			F4SE::log::error("f4sr: refusing to replace an unexpected Present hook/chain");
			return false;
		}
		if (g_origPresent == nullptr) {
			DWORD oldProtect = 0;
			if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				F4SE::log::error("f4sr: Present hook protection failed win32={}", GetLastError());
				return false;
			}
			g_origPresent = reinterpret_cast<Present_t>(vtable[8]);
			vtable[8] = reinterpret_cast<void*>(&PresentThunk);
			DWORD ignored = 0;
			if (!VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &ignored)) {
				F4SE::log::error("f4sr: Present hook protection restore failed win32={}", GetLastError());
			}
		}
		g_presentOwner = this;
		g_hookedChain = gameChain;
		F4SE::log::info("f4sr: present hook owner={:p} chain={:p}", static_cast<void*>(this), static_cast<void*>(gameChain));
		{
			// Which module's Present the FSR-FG pairs go through (ENB's d3d11.dll
			// wrapper vs. the system/DXVK dxgi.dll chain).
			char module[MAX_PATH]{ "?" };
			HMODULE owner{};
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(g_origPresent), &owner))
				GetModuleFileNameA(owner, module, MAX_PATH);
			F4SE::log::info("f4sr: hooked Present belongs to {}", module);
		}
		return true;
	}

	void FrameGen::InstallInnerPresentHook(IDXGISwapChain* gameChain)
	{
		if (g_innerChain != nullptr) return;
		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(gameChain->GetDesc(&desc))) return;
		int field = -1;
		std::string module;
		auto* inner = swapchain::FindInnerChain(gameChain, desc.OutputWindow, field, module);
		if (inner == nullptr) {
			F4SE::log::info("f4sr: FG: no swap chain wrapper found; frames are presented on the game chain");
			return;
		}
		void** table = *reinterpret_cast<void***>(inner);
		DWORD oldProtect{}, ignored{};
		if (!VirtualProtect(&table[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			F4SE::log::warn("f4sr: FG: inner Present hook protection failed win32={}; presenting through the wrapper", GetLastError());
			return;
		}
		g_innerOrigPresent = reinterpret_cast<Present_t>(table[8]);
		table[8] = reinterpret_cast<void*>(&InnerPresentThunk);
		VirtualProtect(&table[8], sizeof(void*), oldProtect, &ignored);
		g_innerChain = inner;
		F4SE::log::info("f4sr: FG: frames are presented on the inner swap chain {:p} (wrapper field {}, {}); the wrapper's Present (ENB) runs once per game frame",
			static_cast<void*>(inner), field, module);
	}

	bool FrameGen::Create(D3D12Interop& interop, ID3D12Device* device, IDXGISwapChain* gameChain,
		std::uint32_t displayWidth, std::uint32_t displayHeight, DXGI_FORMAT backBufferFormat)
	{
		if (!FfxRuntimeReady() || device == nullptr || gameChain == nullptr) {
			return false;
		}
		if (!Destroy() || !InstallPresentHook(gameChain)) return false;
		InstallInnerPresentHook(gameChain);
		// This path currently supports SDR only; never silently reinterpret HDR.
		if (backBufferFormat != DXGI_FORMAT_R8G8B8A8_UNORM && backBufferFormat != DXGI_FORMAT_B8G8R8A8_UNORM) {
			F4SE::log::error("f4sr: FG unsupported backbuffer format {}", static_cast<std::uint32_t>(backBufferFormat));
			return false;
		}
		const char* versionName = nullptr;
		const auto versionId = PickFrameGenerationVersion(device, &versionName);
		if (versionId == 0) return false;
		interop_ = &interop;
		chain_ = gameChain;
		displayWidth_ = displayWidth;
		displayHeight_ = displayHeight;

		// Staging for the present color and the generated frame, in the
		// backbuffer's own format/size so D3D11 CopyResource applies.
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = displayWidth;
		desc.Height = displayHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = backBufferFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (!interop.CreateSharedTexture(desc, presentColor_) || !interop.CreateSharedTexture(desc, hudlessColor_)) {
			return false;
		}
		// The generated frame is the frame-generation dispatch's UAV
		// output: same unordered-access requirement as the SR output.
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (!interop.CreateSharedTexture(desc, generated_)) {
			F4SE::log::error("f4sr: FG staging texture creation failed");
			return false;
		}

		ffxCreateBackendDX12Desc backendDesc{};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backendDesc.device = device;

		ffxCreateContextDescFrameGeneration descFg{};
		descFg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		ffxCreateContextDescFrameGenerationVersion version{};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
		version.header.pNext = &backendDesc.header;
		version.version = FFX_FRAMEGENERATION_VERSION;
		ffxOverrideVersion provider{};
		provider.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		provider.header.pNext = &version.header;
		provider.versionId = versionId;
		descFg.header.pNext = &provider.header;
		// Inputs are post-tonemap LDR for color, but depth/MV follow the
		// SR conventions: standard depth, UV-space motion vectors.
		descFg.flags = FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING |
			FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
		descFg.displaySize = { displayWidth, displayHeight };
		descFg.maxRenderSize = { displayWidth, displayHeight };
		descFg.backBufferFormat = SurfaceFormat(backBufferFormat);

		const auto rc = Ffx().CreateContext(&context_, &descFg.header, nullptr);
		if (rc != FFX_API_RETURN_OK) {
			F4SE::log::error("f4sr: ffxCreateContext (frame generation) failed: {}", static_cast<std::uint32_t>(rc));
			context_ = nullptr;
			return false;
		}

		ffxConfigureDescGlobalDebug debug{};
		debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
		debug.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
		debug.fpMessage = &FfxLogMessage;
		debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS | FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
		const auto debugRc = Ffx().Configure(&context_, &debug.header);
		ffxQueryGetProviderVersion selected{};
		selected.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		const auto queryRc = Ffx().Query(&context_, &selected.header);
		F4SE::log::info("f4sr: FG created owner={:p} provider={} id={} api={} debugRc={} queryRc={} selected={} ({}x{} fmt={}); UI/pacing acceptance pending",
			static_cast<void*>(this), versionName, versionId, FFX_FRAMEGENERATION_VERSION,
			static_cast<std::uint32_t>(debugRc), static_cast<std::uint32_t>(queryRc),
			selected.versionName ? selected.versionName : "unknown", displayWidth, displayHeight, static_cast<std::uint32_t>(backBufferFormat));
		if (queryRc != FFX_API_RETURN_OK || selected.versionId != versionId || debugRc != FFX_API_RETURN_OK) {
			Destroy();
			return false;
		}
		return true;
	}

	bool FrameGen::Destroy()
	{
		if (interop_ != nullptr && !interop_->Drain()) return false;
		if (context_ != nullptr) {
			const auto rc = Ffx().DestroyContext(&context_, nullptr);
			if (rc != FFX_API_RETURN_OK) {
				F4SE::log::error("f4sr: FG destroy failed rc={}", static_cast<std::uint32_t>(rc));
				return false;
			}
			context_ = nullptr;
		}
		overlay_.Reset();
		overlayPrevious_ = {};
		fgCaptureCount_ = 0;
		presentColor_ = {};
		hudlessColor_ = {};
		hudlessReady_ = false;
		generated_ = {};
		interop_ = nullptr;
		chain_ = nullptr;
		frameId_ = 0;
		previousFov_ = 0.0f;
		previousEngineFrame_ = 0;
		preparedEngineFrame_ = 0;
		prepared_ = false;
		resetNext_ = true;
		return true;
	}

	void FrameGen::CaptureHudless(void* rtManager)
	{
		hudlessReady_ = false;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (!rtManager || !data || !data->context) return;
		// Dear-Modding logical kFrameBuffer=0, through the manager's rtID map.
		const auto slot = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(rtManager) + 0xDC4);
		if (slot < 0 || slot >= 101) return;
		auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderTargets[slot].rtView);
		if (!rtv) return;
		Microsoft::WRL::ComPtr<ID3D11Resource> resource;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
		rtv->GetResource(&resource);
		if (!resource || FAILED(resource.As(&source))) return;
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1) return;
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		extern std::atomic<int> g_dumpStage;
		if (g_dumpStage.load() == 3) {
			Microsoft::WRL::ComPtr<ID3D11Device> device;
			ctx->GetDevice(&device);
			auto sd = desc;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = sd.MiscFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
			if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &stage))) {
				ctx->CopyResource(stage.Get(), source.Get());
				D3D11_MAPPED_SUBRESOURCE map{};
				if (SUCCEEDED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map))) {
					WriteCapture("f4sr-dump-H-preinterface.raw", source.Get(), map);
					ctx->Unmap(stage.Get(), 0);
				}
			}
		}
		const auto engineFrame = reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address())->frameCount;
		if (!Created() || !prepared_ || preparedEngineFrame_ != engineFrame || !interop_->Healthy()) return;
		if (desc.Width != displayWidth_ || desc.Height != displayHeight_ || desc.Format != hudlessColor_.desc.Format) {
			if ((++skippedFrames_ % 300) == 1)
				F4SE::log::warn("f4sr: FG HUD-less mismatch slot={} size={}x{} fmt={}", slot, desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
			Invalidate(true);
			return;
		}
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv{};
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		ctx->CopyResource(hudlessColor_.tex11.Get(), source.Get());
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
		for (auto* view : rtvs) if (view) view->Release();
		if (dsv) dsv->Release();
		hudlessEngineFrame_ = engineFrame;
		hudlessReady_ = true;
	}

	void FrameGen::Prepare(ID3D12GraphicsCommandList* cmdList, const FrameGenInputs& inputs)
	{
		hudlessReady_ = false;
		if (context_ == nullptr || cmdList == nullptr) {
			return;
		}
		const char* blocked = MenuBlockReason();
		const auto camera = blocked ? std::nullopt : CaptureFrameCamera();
		if (blocked || !camera || !std::isfinite(inputs.frameTimeMs) || inputs.frameTimeMs <= 0.0f || inputs.frameTimeMs >= 100.0f) {
			overlaySample_.reason = blocked ? blocked : (!camera ? "INVALID CAMERA" : "FRAME DISCONTINUITY");
			if ((++skippedFrames_ % 300) == 1)
				F4SE::log::info("f4sr: FG skipped reason={} dt={}", blocked ? blocked : (!camera ? "invalid world camera" : "frame discontinuity"), inputs.frameTimeMs);
			Invalidate(true);
			return;
		}
		float distanceSquared{}, forwardDot{};
		for (int i = 0; i < 3; ++i) {
			const auto delta = camera->position[i] - previousPosition_[i];
			distanceSquared += delta * delta;
			forwardDot += camera->forward[i] * previousForward_[i];
		}
		// Conservative cut heuristics, not a replacement for an engine history-reset event.
		const bool cameraCut = previousFov_ == 0.0f || camera->engineFrame != previousEngineFrame_ + 1 ||
			distanceSquared > 256.0f * 256.0f || forwardDot < 0.5f || std::abs(camera->verticalFov - previousFov_) > 0.1f;
		++frameId_;
		ffxConfigureDescFrameGeneration config{};
		config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		config.frameGenerationEnabled = true;
		config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		config.frameID = frameId_;
		config.generationRect = { 0, 0, static_cast<std::int32_t>(displayWidth_), static_cast<std::int32_t>(displayHeight_) };
		const auto configureRc = Ffx().Configure(&context_, &config.header);
		if (configureRc != FFX_API_RETURN_OK) {
			++overlaySample_.failures;
			overlaySample_.reason = "CONFIGURE FAILED";
			F4SE::log::error("f4sr: FG Configure frame={} rc={}", frameId_, static_cast<std::uint32_t>(configureRc));
			Invalidate(true);
			return;
		}
		ffxDispatchDescFrameGenerationPrepareV2 prepare{};
		prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prepare.frameID = frameId_;
		prepare.flags = 0;
		prepare.commandList = cmdList;
		prepare.renderSize = { inputs.renderWidth, inputs.renderHeight };
		prepare.jitterOffset = { inputs.jitterX, inputs.jitterY };
		prepare.motionVectorScale = { static_cast<float>(inputs.renderWidth), static_cast<float>(inputs.renderHeight) };
		prepare.frameTimeDelta = inputs.frameTimeMs;
		prepare.reset = inputs.reset || resetNext_ || cameraCut;
		prepare.cameraNear = camera->nearPlane;
		prepare.cameraFar = camera->farPlane;
		prepare.cameraFovAngleVertical = camera->verticalFov;
		prepare.viewSpaceToMetersFactor = 0.01428222656f;
		prepare.depth = ffxApiGetResourceDX12(inputs.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		prepare.motionVectors = ffxApiGetResourceDX12(inputs.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		std::memcpy(prepare.cameraPosition, camera->position, sizeof(camera->position));
		std::memcpy(prepare.cameraUp, camera->up, sizeof(camera->up));
		std::memcpy(prepare.cameraRight, camera->right, sizeof(camera->right));
		std::memcpy(prepare.cameraForward, camera->forward, sizeof(camera->forward));

		const auto rc = Ffx().Dispatch(&context_, &prepare.header);
		if (rc != FFX_API_RETURN_OK) {
			++overlaySample_.failures;
			overlaySample_.reason = "PREPARE FAILED";
			F4SE::log::error("f4sr: FG PrepareV2 dispatch failed: {}", static_cast<std::uint32_t>(rc));
			Invalidate(true);
			return;
		}
		preparedReset_ = prepare.reset;
		if (preparedReset_) {
			++overlaySample_.resets;
			if (overlaySample_.resets <= 32 || overlaySample_.resets % 300 == 0) {
				F4SE::log::info("f4sr: FG reset count={} frame={} engineFrame={} previousEngineFrame={} srReset={} invalidated={} firstCamera={} frameGap={} translation={} rotation={} fovChange={} dt={:.3f}",
					overlaySample_.resets, frameId_, camera->engineFrame, previousEngineFrame_, inputs.reset, resetNext_,
					previousFov_ == 0.0f, camera->engineFrame != previousEngineFrame_ + 1,
					distanceSquared > 256.0f * 256.0f, forwardDot < 0.5f, std::abs(camera->verticalFov - previousFov_) > 0.1f, inputs.frameTimeMs);
			}
		}
		overlaySample_.reason = preparedReset_ ? "HISTORY RESET" : "PREPARED";
		resetNext_ = false;
		prepared_ = true;
		preparedTick_ = GetTickCount64();
		preparedEngineFrame_ = previousEngineFrame_ = camera->engineFrame;
		previousFov_ = camera->verticalFov;
		std::memcpy(previousPosition_, camera->position, sizeof(previousPosition_));
		std::memcpy(previousForward_, camera->forward, sizeof(previousForward_));
		if (frameId_ % 300 == 1) {
			F4SE::log::info("f4sr: FG camera engineFrame={} fov={} near={} far={} cut={}",
				camera->engineFrame, camera->verticalFov, camera->nearPlane, camera->farPlane, cameraCut);
			F4SE::log::info("f4sr: FG prepared frame={} reset={} render={}x{} dt={:.3f}",
				frameId_, preparedReset_, inputs.renderWidth, inputs.renderHeight, inputs.frameTimeMs);
		}
	}

	// Without generation the renderer may queue up to N source frames
	// (DXGI default N=3). Frame latency is counted in Presents, and a
	// generated pair is two Presents, so the same limit holds only N/2
	// source frames: the render thread then blocks in every real Present
	// and the GPU drains (observed: ~10 ms per frame, 50 -> 30 source fps).
	// Doubling the limit while pairs are presented restores the depth.
	void FrameGen::ApplyFrameLatency(IDXGISwapChain* chain, bool pairs)
	{
		if (pairs == latencyRaised_ || chain == nullptr) return;
		Microsoft::WRL::ComPtr<IDXGIDevice1> device;
		if (FAILED(chain->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))) { latencyRaised_ = pairs; return; }
		if (pairs) {
			UINT current{};
			if (FAILED(device->GetMaximumFrameLatency(&current)) || current == 0) current = 3;
			latencyBase_ = current;
			const UINT raised = (std::min)(16u, current * 2);
			const HRESULT hr = device->SetMaximumFrameLatency(raised);
			F4SE::log::info("f4sr: FG frame latency {} -> {} hr=0x{:08X}", current, raised, static_cast<std::uint32_t>(hr));
		} else if (latencyBase_ != 0) {
			device->SetMaximumFrameLatency(latencyBase_);
			F4SE::log::info("f4sr: FG frame latency restored to {}", latencyBase_);
		}
		latencyRaised_ = pairs;
	}

	HRESULT FrameGen::OnPresent(IDXGISwapChain* chain, std::uint32_t syncInterval, std::uint32_t flags)
	{
		if (GetSettings().diagnostics && (flags & DXGI_PRESENT_TEST) == 0) {
			if (auto* data = RE::BSGraphics::RendererData::GetSingleton(); data != nullptr && data->context != nullptr)
				frame_journal::OnPresent(chain, reinterpret_cast<ID3D11DeviceContext*>(data->context));
		}
		if ((flags & DXGI_PRESENT_TEST) == 0) DumpPresentCaptures(chain);
		if ((flags & DXGI_PRESENT_TEST) == 0) LogPresentViewport(chain);
		if ((flags & DXGI_PRESENT_TEST) != 0) return PresentOn(chain, syncInterval, flags);
		++overlaySample_.source;
		overlaySample_.enabled = GetSettings().enableFrameGeneration;
		if (overlaySample_.source % 120 == 1) {
			// Diagnostic A/B without a restart: edit f4sr.ini while playing.
			auto& path = GetSettings().fgPresentPath;
			const int now = PluginIniInt("iFGPresentPath", path);
			if (now != path) F4SE::log::info("f4sr: FSR-FG present path {} -> {}", path, now);
			path = now;
		}
		auto drawOverlay = [&](bool generated) { DrawOverlay(chain, flags, generated); };
		// Conservative synchronous FIFO mode: every member of a generated pair
		// requests at least one vblank. A following interval-zero Present could
		// cancel/drop the interpolated frame on a flip queue. This deliberately
		// caps source throughput at refresh/2; it is not an asynchronous scheduler.
		// A reset/missing-input frame holds the REAL image for the same two-vblank
		// budget instead of switching to interval zero + tearing. Never present
		// an invalid interpolation or count a duplicated real image as generated.
		const bool fifoActive = Created() && (chain == chain_ || (chain == g_innerChain && chain_ == g_hookedChain)) && interop_ != nullptr && interop_->Healthy() &&
			GetSettings().enableFrameGeneration && MenuBlockReason() == nullptr &&
			(flags & ~DXGI_PRESENT_ALLOW_TEARING) == 0 && syncInterval <= 1;
		ApplyFrameLatency(chain, fifoActive);
		UINT realInterval = fifoActive ? 2 : syncInterval;
		UINT realFlags = fifoActive ? flags & ~DXGI_PRESENT_ALLOW_TEARING : flags;
		auto presentReal = [&]() {
			drawOverlay(false);
			const auto hr = PresentOn(chain, realInterval, realFlags);
			if (fifoActive && realInterval == 2) {
				++realOnlyPresents_;
				if (realOnlyPresents_ <= 32 || realOnlyPresents_ % 300 == 0) {
					LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
					F4SE::log::info("f4sr: FG real-only frame={} reason={} interval={} flags={} requestedSync={} requestedFlags={} hr=0x{:08X} qpc={}",
						frameId_, overlaySample_.reason, realInterval, realFlags, syncInterval, flags, static_cast<std::uint32_t>(hr), qpc.QuadPart);
				}
			}
			if (hr == S_OK) ++realPresents_;
			else {
				++overlaySample_.failures;
				overlaySample_.reason = "REAL PRESENT FAILED";
				F4SE::log::warn("f4sr: real Present frame={} hr=0x{:08X}", frameId_, static_cast<std::uint32_t>(hr));
				Invalidate(true);
			}
			return hr;
		};
		const bool generate = fifoActive && prepared_ && hudlessReady_ && hudlessEngineFrame_ == preparedEngineFrame_ &&
			GetTickCount64() - preparedTick_ < 250 &&
			preparedEngineFrame_ == reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address())->frameCount;
		if (!generate) {
			if (!GetSettings().enableFrameGeneration) overlaySample_.reason = "OFF";
			else if (!Created()) overlaySample_.reason = "NO FG CONTEXT";
			else if (const auto* menu = MenuBlockReason()) overlaySample_.reason = menu;
			else if (prepared_ && !hudlessReady_) overlaySample_.reason = "HUDLESS MISSING";
			else if (prepared_) overlaySample_.reason = "PRESENT GATE / STALE INPUT";
			Invalidate();
			return presentReal();
		}
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		if (data == nullptr || data->context == nullptr) {
			Invalidate(true);
			return presentReal();
		}
		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		auto hr = chain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
		if (FAILED(hr)) {
			F4SE::log::error("f4sr: FG GetBuffer frame={} hr=0x{:08X}", frameId_, static_cast<std::uint32_t>(hr));
			Invalidate(true);
			return presentReal();
		}
		D3D11_TEXTURE2D_DESC actual{};
		backBuffer->GetDesc(&actual);
		if (actual.Width != displayWidth_ || actual.Height != displayHeight_ ||
			actual.Format != presentColor_.desc.Format || actual.SampleDesc.Count != 1) {
			F4SE::log::warn("f4sr: FG backbuffer changed; suppressing generation until context rebuild");
			Invalidate(true);
			return presentReal();
		}
		struct OutputBindings {
			ID3D11DeviceContext* ctx;
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* dsv{};
			explicit OutputBindings(ID3D11DeviceContext* c) : ctx(c) {
				ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				ctx->OMSetRenderTargets(0, nullptr, nullptr);
			}
			~OutputBindings() {
				ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				for (auto* rtv : rtvs) if (rtv) rtv->Release();
				if (dsv) dsv->Release();
			}
		} bindings{ ctx };
		if (!GenerateFrame(ctx, backBuffer.Get())) {
			Invalidate(true);
			return presentReal();
		}
		// Reset dispatch primes provider history, but must not display a fake interpolation.
		if (preparedReset_) { overlaySample_.reason = "RESET: REAL ONLY"; return presentReal(); }
		if (GetSettings().fgPresentPath == 1) {
			// Diagnostic: the interpolation ran, but only the real frame is
			// presented (one Present per game frame, same two-vblank cadence).
			overlaySample_.reason = "DIAG: GENERATED NOT PRESENTED";
			return presentReal();
		}
		if (GetSettings().frameGenCapture && frameId_ >= 63 && fgCaptureCount_ < 2) CaptureSamples(ctx);
		overlaySample_.reason = "PAIR SUBMISSION (DISPLAY UNVERIFIED)";
		realInterval = 1;
		realFlags = flags & ~DXGI_PRESENT_ALLOW_TEARING;
		LARGE_INTEGER generatedStart{}, generatedEnd{}, realEnd{};
		QueryPerformanceCounter(&generatedStart);
		ctx->CopyResource(backBuffer.Get(), generated_.tex11.Get());
		drawOverlay(true);
		hr = PresentOn(chain, realInterval, realFlags);
		QueryPerformanceCounter(&generatedEnd);
		if (hr != S_OK) {
			ctx->CopyResource(backBuffer.Get(), presentColor_.tex11.Get());
			++overlaySample_.failures;
			overlaySample_.reason = "GENERATED PRESENT FAILED";
			F4SE::log::warn("f4sr: generated Present frame={} hr=0x{:08X}", frameId_, static_cast<std::uint32_t>(hr));
			Invalidate(true);
			return hr;
		}
		++generatedPresents_;
		// D3D11 exposes the current logical backbuffer at index zero. Reacquire
		// after Present rather than assuming the old reference is still current.
		backBuffer.Reset();
		hr = chain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
		if (FAILED(hr)) {
			F4SE::log::error("f4sr: FG restore GetBuffer frame={} hr=0x{:08X}", frameId_, static_cast<std::uint32_t>(hr));
			Invalidate(true);
			return hr;
		}
		ctx->CopyResource(backBuffer.Get(), presentColor_.tex11.Get());
		hr = presentReal();
		QueryPerformanceCounter(&realEnd);
		if (frameId_ % 300 == 1) LogPairStats(chain, syncInterval, flags, generatedStart, generatedEnd, realEnd);
		return hr;
	}

	// Diagnostics (bDiagnostics capture stage 3): back buffer and present-time
	// colour target, once.
	void FrameGen::DumpPresentCaptures(IDXGISwapChain* chain)
	{
		extern std::atomic<int> g_dumpStage;
		if (g_dumpStage.load() == 3) {
			if (auto* data = RE::BSGraphics::RendererData::GetSingleton(); data != nullptr && data->context != nullptr) {
				auto* vctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
				Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
				if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf())))) {
					D3D11_TEXTURE2D_DESC bd{};
					bb->GetDesc(&bd);
					Microsoft::WRL::ComPtr<ID3D11Device> dev;
					vctx->GetDevice(&dev);
					D3D11_TEXTURE2D_DESC sd2{};
					sd2.Width = bd.Width;
					sd2.Height = bd.Height;
					sd2.MipLevels = 1;
					sd2.ArraySize = 1;
					sd2.Format = bd.Format;
					sd2.SampleDesc.Count = 1;
					sd2.Usage = D3D11_USAGE_STAGING;
					sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
					if (dev != nullptr && SUCCEEDED(dev->CreateTexture2D(&sd2, nullptr, &stage))) {
						vctx->CopyResource(stage.Get(), bb.Get());
						D3D11_MAPPED_SUBRESOURCE map{};
						if (SUCCEEDED(vctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map))) {
							WriteCapture("f4sr-dump-C-backbuffer.raw", bb.Get(), map);
							vctx->Unmap(stage.Get(), 0);
							F4SE::log::info("f4sr: dump C written ({}x{} fmt {})", bd.Width, bd.Height,
								static_cast<std::uint32_t>(bd.Format));
							// B is from the preceding frame: consult sidecars before comparing.
							const auto configuredSlot = GetSettings().colorSlot;
							const auto colorSlot = configuredSlot >= 0 && configuredSlot < 101 ? configuredSlot : 4;
							if (auto* rtTex = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[colorSlot].texture); rtTex != nullptr) {
								D3D11_TEXTURE2D_DESC rd{};
								rtTex->GetDesc(&rd);
								D3D11_TEXTURE2D_DESC sd3{};
								sd3.Width = rd.Width;
								sd3.Height = rd.Height;
								sd3.MipLevels = 1;
								sd3.ArraySize = 1;
								sd3.Format = rd.Format;
								sd3.SampleDesc.Count = 1;
								sd3.Usage = D3D11_USAGE_STAGING;
								sd3.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
								Microsoft::WRL::ComPtr<ID3D11Texture2D> stage2;
								if (SUCCEEDED(dev->CreateTexture2D(&sd3, nullptr, &stage2))) {
									vctx->CopyResource(stage2.Get(), rtTex);
									D3D11_MAPPED_SUBRESOURCE map2{};
									if (SUCCEEDED(vctx->Map(stage2.Get(), 0, D3D11_MAP_READ, 0, &map2))) {
										WriteCapture("f4sr-dump-D-present-rt.raw", rtTex, map2);
										vctx->Unmap(stage2.Get(), 0);
										F4SE::log::info("f4sr: dump D written ({}x{} fmt {})", rd.Width, rd.Height,
											static_cast<std::uint32_t>(rd.Format));
									}
								}
							}
							g_dumpStage.store(4);
						}
					}
				}
			}
		}
	}

	void FrameGen::LogPresentViewport(IDXGISwapChain* chain)
	{
		static std::uint64_t s_presentCount = 0;
		if ((++s_presentCount % 600) == 1) {
			if (auto* data = RE::BSGraphics::RendererData::GetSingleton(); data != nullptr && data->context != nullptr) {
				auto* vctx = reinterpret_cast<ID3D11DeviceContext*>(data->context);
				UINT nvp = 8;
				D3D11_VIEWPORT vps[8]{};
				vctx->RSGetViewports(&nvp, vps);
				DXGI_SWAP_CHAIN_DESC sd{};
				chain->GetDesc(&sd);
				F4SE::log::info("f4sr: vp present nvp={} vp0=({:.0f},{:.0f} {:.0f}x{:.0f}) bb={}x{}",
					nvp,
					nvp != 0 ? vps[0].TopLeftX : -1.0f, nvp != 0 ? vps[0].TopLeftY : -1.0f,
					nvp != 0 ? vps[0].Width : -1.0f, nvp != 0 ? vps[0].Height : -1.0f,
					sd.BufferDesc.Width, sd.BufferDesc.Height);
			}
		}
	}

	// Status panel on this chain (FSR-FG; XeSS-FG draws its own).
	void FrameGen::DrawOverlay(IDXGISwapChain* chain, std::uint32_t flags, bool generated)
	{
		if (!GetSettings().frameGenOverlay || (flags & ~DXGI_PRESENT_ALLOW_TEARING) != 0) return;
		// While XeSS-FG is attached its presenter draws its own panel on this chain.
		if (xefg::Attached()) return;
		if (GetSettings().frameGenEngine != 0) {
			// XeSS-FG selected but not running: say so instead of showing FSR-FG counters.
			overlay_.Draw(chain, 0, [](double seconds) {
				static std::uint64_t srPrevious{};
				std::string text = "#F4SR\n" + sr_status::Lines(GetSettings().enableUpscaling, seconds, srPrevious) +
					"\n"
					"FG\t!none active: XeSS-FG selected but not attached\n"
					"\trestart after changing the engine,\n"
					"\tor look for 'XeFG' errors in f4sr.log\n";
				return std::array<std::string, 2>{ text, text };
			});
			return;
		}
		overlaySample_.real = realPresents_;
		overlaySample_.generated = generatedPresents_;
		overlay_.Draw(chain, generated ? 1 : 0, [&](double seconds) {
			const auto& now = overlaySample_;
			const auto& old = overlayPrevious_;
			auto rate = [&](std::uint64_t n, std::uint64_t o) {
				return seconds > 0 && n >= o ? static_cast<double>(n - o) / seconds : 0.0;
			};
			const double source = rate(now.source, old.source);
			const double gen = rate(now.generated, old.generated);
			const double real = rate(now.real, old.real);
			const char* stats = now.statistics == S_OK ? "VALID COUNTERS (NOT SCANOUT)" :
				now.statistics == DXGI_ERROR_FRAME_STATISTICS_DISJOINT ? "DISJOINT / INVALID" : "UNAVAILABLE";
			static std::uint64_t srPrevious{};
			const std::string sr = sr_status::Lines(GetSettings().enableUpscaling, seconds, srPrevious);
			std::array<std::string, 2> text;
			for (int i = 0; i < 2; ++i) {
				text[i] = "#F4SR" + std::string(i ? "   [G] generated frame" : "   [R] real frame") + "\n" + sr + "\n";
				text[i] += std::string("FG\tFSR-FG 2X  ") + (now.enabled ? "generating" : "!paused") +
					(InnerRouting() ? "  (inner chain)" : g_innerChain ? "  (via wrapper)" : "") + "\n";
				if (!now.enabled) text[i] += std::format("\t!{}\n", now.reason);
				text[i] += std::format("\t{:.0f} source fps -> {:.0f} submitted/s\n", source, real + gen);
				if (now.resets || now.failures) text[i] += std::format("\t!resets {}   failures {}\n", now.resets, now.failures);
				if (now.statistics != S_OK) text[i] += std::format("\t!DXGI frame statistics {}\n", stats);
			}
			overlayPrevious_ = now;
			return text;
		});
	}

	// Configure the HUD-less input and dispatch one interpolated frame into
	// generated_. Saves the real frame to presentColor_ first. False on failure.
	bool FrameGen::GenerateFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer)
	{
		ffxConfigureDescFrameGeneration config{};
		config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		config.frameGenerationEnabled = true;
		config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		config.frameID = frameId_;
		config.generationRect = { 0, 0, static_cast<std::int32_t>(displayWidth_), static_cast<std::int32_t>(displayHeight_) };
		config.HUDLessColor = ffxApiGetResourceDX12(hudlessColor_.res12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		const auto configRc = Ffx().Configure(&context_, &config.header);
		if (configRc != FFX_API_RETURN_OK) {
			++overlaySample_.failures;
			overlaySample_.reason = "HUDLESS CONFIGURE FAILED";
			F4SE::log::error("f4sr: FG HUD-less Configure frame={} rc={}", frameId_, static_cast<unsigned>(configRc));
			return false;
		}
		// Save Rn before modifying the swapchain. Do not present Rn before I(n-1,n).
		ctx->CopyResource(presentColor_.tex11.Get(), backBuffer);
		if (!interop_->SignalFromD3D11(ctx) || !interop_->WaitOnD3D12()) {
			return false;
		}
		auto* list = interop_->BeginFrame();
		if (list == nullptr) {
			return false;
		}
		D3D12Interop::Transition(list, hudlessColor_.res12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		D3D12Interop::Transition(list, presentColor_.res12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		D3D12Interop::Transition(list, generated_.res12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ffxDispatchDescFrameGeneration dispatch{};
		dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
		dispatch.commandList = list;
		dispatch.presentColor = ffxApiGetResourceDX12(presentColor_.res12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatch.outputs[0] = ffxApiGetResourceDX12(generated_.res12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		dispatch.numGeneratedFrames = 1;
		dispatch.reset = preparedReset_;
		dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		dispatch.minMaxLuminance[1] = 100.0f;
		dispatch.generationRect = { 0, 0, static_cast<std::int32_t>(displayWidth_), static_cast<std::int32_t>(displayHeight_) };
		dispatch.frameID = frameId_;
		const auto rc = Ffx().Dispatch(&context_, &dispatch.header);
		D3D12Interop::Transition(list, hudlessColor_.res12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		D3D12Interop::Transition(list, presentColor_.res12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		D3D12Interop::Transition(list, generated_.res12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		const bool submitted = interop_->EndFrame() && interop_->SignalFromD3D12() && interop_->WaitOnD3D11(ctx);
		prepared_ = false;
		if (rc != FFX_API_RETURN_OK || !submitted) {
			++overlaySample_.failures;
			overlaySample_.reason = "DISPATCH / SYNC FAILED";
			F4SE::log::error("f4sr: FG Dispatch frame={} rc={} submitted={}", frameId_, static_cast<std::uint32_t>(rc), submitted);
			return false;
		}
		return true;
	}

	void FrameGen::CaptureSamples(ID3D11DeviceContext* ctx)
	{
	// Bounded, unmarked R/G/H evidence. Readback stalls are diagnostic only.
		ID3D11Texture2D* sources[]{ presentColor_.tex11.Get(), generated_.tex11.Get(), hudlessColor_.tex11.Get() };
		const char* names[]{ "R", "G", "H" };
		for (int i = 0; i < 3; ++i) {
			D3D11_TEXTURE2D_DESC sd{}; sources[i]->GetDesc(&sd);
			sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Microsoft::WRL::ComPtr<ID3D11Device> device; sources[i]->GetDevice(device.GetAddressOf());
			Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
			if (FAILED(device->CreateTexture2D(&sd, nullptr, stage.GetAddressOf()))) continue;
			ctx->CopyResource(stage.Get(), sources[i]);
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				const auto name = std::format("f4sr-dump-FG{}-{}.raw", fgCaptureCount_, names[i]);
				WriteCapture(name.c_str(), sources[i], map);
				ctx->Unmap(stage.Get(), 0);
			}
		}
		++fgCaptureCount_;
		F4SE::log::info("f4sr: FG pixel sample {} captured; exclude readback stall from pacing measurements", fgCaptureCount_);
	}

	void FrameGen::LogPairStats(IDXGISwapChain* chain, std::uint32_t syncInterval, std::uint32_t flags,
		LARGE_INTEGER generatedStart, LARGE_INTEGER generatedEnd, LARGE_INTEGER realEnd)
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		DXGI_FRAME_STATISTICS statistics{};
		const auto statsHr = chain->GetFrameStatistics(&statistics);
		overlaySample_.statistics = statsHr;
		F4SE::log::info("f4sr: FG pair frame={} generated={} real={} mode=sync-fifo interval=1 requestedSync={} flags={} genPresentMs={:.3f} realPresentMs={:.3f} statsHr=0x{:08X} presentCount={} refreshCount={} syncQpc={} (submission != display; source capped by two vblanks)",
			frameId_, generatedPresents_, realPresents_, syncInterval, flags,
			1000.0 * (generatedEnd.QuadPart - generatedStart.QuadPart) / frequency.QuadPart,
			1000.0 * (realEnd.QuadPart - generatedEnd.QuadPart) / frequency.QuadPart,
			static_cast<std::uint32_t>(statsHr), statistics.PresentCount, statistics.PresentRefreshCount, statistics.SyncQPCTime.QuadPart);
	}
}
