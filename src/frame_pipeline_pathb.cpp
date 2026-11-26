// Path B (ENB before upscaling): the game, ENB included, renders at the
// render size. At the pre-interface point the finished, tonemapped frame is
// upscaled into a display-size canvas; the UI then draws on the canvas
// (ui_redirect) and the XeSS-FG proxy chain presents it.

#include "frame_pipeline.h"
#include "vram_budget.h"
#include "frame_pipeline_detail.h"

#include "settings.h"
#include "sr_status.h"
#include "ui_redirect.h"
#include "hfpf_display.h"

#include <cctype>
#include <format>
#include <string>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

namespace f4sr
{
	namespace
	{
		// Path B renders below the display by lowering the game's iSize; if
		// the game's frame buffer still comes up at the output size, the
		// preset silently runs as Native AA (2026-11-25 reports: HFPF
		// BorderlessUpscale=false or ResizeBuffersDisable=true).
		void CheckFrameAtDisplay(int quality, UINT fbW, UINT fbH, UINT outW, UINT outH)
		{
			static bool logged{};
			static std::uint64_t warnedKey{};
			static hfpf::Display hfpf;
			if (!logged) {
				logged = true;
				hfpf = hfpf::Read();
				F4SE::log::info("f4sr: Path B: {}", hfpf.summary);
			}
			const bool atDisplay = quality > 0 && fbW >= outW && fbH >= outH && !xefg::PathBQualityNote();
			if (!atDisplay) {
				if (warnedKey) {
					warnedKey = 0;
					sr_status::Clear(sr_status::kAlertFrameAtDisplay);
				}
				return;
			}
			const std::uint64_t key = (static_cast<std::uint64_t>(quality) << 48) | (static_cast<std::uint64_t>(fbW) << 24) | fbH;
			if (key == warnedKey) return;
			warnedKey = key;
			sr_status::Raise(sr_status::kAlertFrameAtDisplay);
			F4SE::log::warn("f4sr: Path B: preset {} should render below {}x{}, but the game frame is {}x{}: super resolution runs as Native AA "
							"(no FPS gain). Something resized the game's buffers back to the window size. {}{}",
				quality, outW, outH, fbW, fbH, hfpf.summary,
				hfpf.found ? (hfpf.suspect ? ". Known working: [Display] at defaults with BorderlessUpscale=true and ResizeBuffersDisable=false" :
											 ". These values look fine; check other mods that resize the swap chain (ENB [WINDOW], display tools)") :
							 "");
		}

		DXGI_FORMAT Typeless(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
			case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
			case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
			default: return f;
			}
		}

		ID3D11Texture2D* FrameBufferTexture(void* rtManager)
		{
			auto* data = RE::BSGraphics::RendererData::GetSingleton();
			if (!data || !rtManager) return nullptr;
			const auto slot = *reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(rtManager) + 0xDC4);
			if (slot < 0 || slot >= 101) return nullptr;
			auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderTargets[slot].rtView);
			if (!rtv) return nullptr;
			Microsoft::WRL::ComPtr<ID3D11Resource> resource;
			rtv->GetResource(&resource);
			Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
			if (!resource || FAILED(resource.As(&tex))) return nullptr;
			return tex.Get();  // owned by the engine
		}
	}

	bool FramePipeline::EnsurePathB(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& fb, UINT outW, UINT outH)
	{
		D3D11_TEXTURE2D_DESC cd{};
		if (canvas_) canvas_->GetDesc(&cd);
		if (!canvas_ || cd.Width != outW || cd.Height != outH || cd.Format != Typeless(fb.Format)) {
			canvas_.Reset();
			canvasRtv_.Reset();
			D3D11_TEXTURE2D_DESC d{};
			d.Width = outW;
			d.Height = outH;
			d.MipLevels = d.ArraySize = 1;
			d.Format = Typeless(fb.Format);
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(device->CreateTexture2D(&d, nullptr, &canvas_))) {
				F4SE::log::error("f4sr: ENB before upscaling: canvas {}x{} format {} creation failed", outW, outH, static_cast<unsigned>(d.Format));
				return false;
			}
			vram_budget::Note("UI Native canvas", outW, outH, d.Format);
			vram_budget::TrackTexture(canvas_.Get());
			F4SE::log::info("f4sr: ENB before upscaling: canvas {}x{} format {} (frame buffer {}x{} format {})", outW, outH,
				static_cast<unsigned>(d.Format), fb.Width, fb.Height, static_cast<unsigned>(fb.Format));
		}
		return true;
	}

	// Upscaler configurations tried in order until a dispatch succeeds:
	// 0 the selected backend with HDR flags, 1 the same in LDR mode,
	// 2 HDR flags with the seam's unused context released, 3 FSR 3.1
	// with HDR flags. FSR 4.1 rejected LDR dispatches in testing.
	bool FramePipeline::CreateUpscalerB(UINT w, UINT h, UINT outW, UINT outH, DXGI_FORMAT typed)
	{
		const auto& settings = GetSettings();
		upscalerB_.Destroy();
		if (useXess_) {
			return ldrShared_.res12 && sceneOut_.res12 &&
				xessUpscaler_.CreatePathB(interop_.Device12(), { w, h }, { outW, outH }, settings.qualityMode);
		}
		int backend = settings.backend;
		bool hdr = false;
		if (bStage_ == 0 || bStage_ == 2 || bStage_ == 3) hdr = true;
		if (bStage_ >= 2) upscaler_.Destroy();
		if (bStage_ == 3) backend = 0;
		const bool nonLinearColor = backend == 1 && settings.pathBNonLinearColor;
		if (!ldrShared_.res12 || !sceneOut_.res12 ||
			!upscalerB_.Create(interop_.Device12(), w, h, outW, outH, backend, false, hdr, nonLinearColor)) {
			F4SE::log::error("f4sr: ENB before upscaling: upscaler {}x{} -> {}x{} unavailable (stage {})", w, h, outW, outH, bStage_);
			return false;
		}
		F4SE::log::info("f4sr: ENB before upscaling: upscaler {}x{} -> {}x{} (stage {}: backend {}, {}, format {}, non-linear input {})",
			w, h, outW, outH, bStage_, backend, hdr ? "HDR flags" : "LDR", static_cast<unsigned>(typed), nonLinearColor);
		return true;
	}

	bool FramePipeline::PathBInputsReady() const
	{
		return xefg::OutputScaling() && !pathBFailed_ && SrActive() && pendingBReady_ &&
			pendingBFrame_ == detail::CaptureFrameId() && pendingB_.depth && pendingB_.motionVectors;
	}

	bool FramePipeline::TryBypassPathBTaa()
	{
		// Keep the native resolve during init, menus, and recovery after an
		// unsuccessful dispatch. Path B does not resolve until pre-interface.
		if (!pathBLastUpscaled_ || !PathBInputsReady() || !(useXess_ ? xessUpscaler_.Created() : upscalerB_.Created()) ||
			!ldrShared_.res12 || !sceneOut_.res12 || ldrShared_.desc.Width != pendingB_.renderWidth ||
			ldrShared_.desc.Height != pendingB_.renderHeight) return false;
		pathBTaaBypassed_ = true;
		return true;
	}

	bool FramePipeline::PreparePathBResize()
	{
		if (!Teardown()) return false;
		ResetInitBackoff();
		scaleB_ = {};
		return true;
	}

	void FramePipeline::AfterPresentPathB()
	{
		const bool composed = composedThisFrame_;
		composedThisFrame_ = false;
		pendingBReady_ = false;
		if (!composed) pathBLastUpscaled_ = false;
		xefg::SetComposedFrame(nullptr);
		if (composed || pathBFailed_ || !xefg::OutputScaling()) return;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
		auto* chain = data ? reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain) : nullptr;
		if (!ctx || !chain) return;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> fbTex;
		if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&fbTex))) || !fbTex) return;
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		D3D11_TEXTURE2D_DESC fb{};
		fbTex->GetDesc(&fb);
		const UINT outW = xefg::OutputWidth(), outH = xefg::OutputHeight();
		if (!device || !outW || !outH || !EnsurePathB(device.Get(), fb, outW, outH)) return;
		static bool hooked = false;
		if (!hooked) {
			hooked = true;
			if (!ui_redirect::Install(ctx)) {
				pathBFailed_ = true;
				xefg::ReportPathBFailure("UI redirect hooks could not be installed");
				return;
			}
		}
		if (!canvasRtv_) {
			D3D11_RENDER_TARGET_VIEW_DESC rd{};
			rd.Format = output_scale::ViewFormat(fb.Format);
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateRenderTargetView(canvas_.Get(), &rd, &canvasRtv_))) return;
		}
		const float black[4]{ 0, 0, 0, 1 };
		ctx->ClearRenderTargetView(canvasRtv_.Get(), black);
		xefg::SetComposedFrame(canvas_.Get());
		ui_redirect::Begin(ctx, fbTex.Get(), canvas_.Get(), false);
	}

	bool FramePipeline::ApplyReShadePreUpscale(ID3D11DeviceContext* ctx, ID3D11Texture2D* fbTex, const D3D11_TEXTURE2D_DESC& fb)
	{
		if (!reshade_preui::PreUpscaleWanted()) {
			if (fxColor_.tex11) {
				// Leaving the mode: ReShade drops its views of these targets,
				// and the GPU must be done with the last effects pass before
				// they are released (releasing them in flight removed the
				// device, 2026-11-23).
				reshade_preui::ReleasePreUpscaleTargets();
				interop_.Drain();
				fxColor_ = {};
				fxDepth_ = {};
				fxDepthValid_ = false;
			}
			return false;
		}
		if (!interop_.Created()) return false;  // menus before the first scene frame
		if (!fxColor_.tex11 || fxColor_.desc.Width != fb.Width || fxColor_.desc.Height != fb.Height) {
			D3D11_TEXTURE2D_DESC d{};
			d.Width = fb.Width;
			d.Height = fb.Height;
			d.MipLevels = d.ArraySize = 1;
			d.Format = output_scale::ViewFormat(fb.Format);
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (fxColor_.tex11) interop_.Drain();  // preset switch: the last effects pass may still use them
			fxColor_ = {};
			fxDepth_ = {};
			fxDepthValid_ = false;
			interop_.CreateSharedTexture(d, fxColor_);
			d.Format = DXGI_FORMAT_R32_FLOAT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			interop_.CreateSharedTexture(d, fxDepth_);
			if (!fxColor_.res12 || !fxDepth_.res12) {
				F4SE::log::error("f4sr: ReShade before upscaling: {}x{} shared textures could not be created", fb.Width, fb.Height);
				fxColor_ = {};
				fxDepth_ = {};
				return false;
			}
		}
		ctx->CopyResource(fxColor_.tex11.Get(), fbTex);
		// This frame's depth (the scene's; menus keep the last one).
		if (PathBInputsReady() && depthShared_.tex11 && depthShared_.desc.Width >= fb.Width && depthShared_.desc.Height >= fb.Height) {
			const D3D11_BOX box{ 0, 0, 0, fb.Width, fb.Height, 1 };
			ctx->CopySubresourceRegion(fxDepth_.tex11.Get(), 0, 0, 0, 0, depthShared_.tex11.Get(), 0, &box);
			fxDepthValid_ = true;
		}
		if (!interop_.SignalFromD3D11(ctx) || !interop_.WaitOnD3D12()) return false;
		reshade_preui::ExpectCommandList(true);
		auto* list = interop_.BeginFrame();
		reshade_preui::ExpectCommandList(false);
		if (!list) return false;
		auto* color = fxColor_.res12.Get();
		auto* depth = fxDepthValid_ ? fxDepth_.res12.Get() : nullptr;
		constexpr auto kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		D3D12Interop::Transition(list, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (depth) D3D12Interop::Transition(list, depth, D3D12_RESOURCE_STATE_COMMON, kRead);
		const bool rendered = reshade_preui::RenderPreUpscale(color, depth);
		D3D12Interop::Transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
		if (depth) D3D12Interop::Transition(list, depth, kRead, D3D12_RESOURCE_STATE_COMMON);
		if (!interop_.EndFrame() || !interop_.SignalFromD3D12() || !interop_.WaitOnD3D11(ctx)) return false;
		if (rendered) ctx->CopyResource(fbTex, fxColor_.tex11.Get());
		return rendered;
	}

	bool FramePipeline::ComposePathB(void* rtManager)
	{
		pathBLastUpscaled_ = false;
		pathBReShadeFx_ = false;
		if (!xefg::OutputScaling() || pathBFailed_) return false;
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
		ID3D11Texture2D* fbTex = FrameBufferTexture(rtManager);
		if (!ctx || !fbTex) return false;
		// A redirect from a frame without a scene ends before f4sr's own draws.
		ui_redirect::End();
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		ctx->GetDevice(&device);
		D3D11_TEXTURE2D_DESC fb{};
		fbTex->GetDesc(&fb);
		const UINT outW = xefg::OutputWidth(), outH = xefg::OutputHeight();
		if (!device || !outW || !outH || !EnsurePathB(device.Get(), fb, outW, outH)) {
			pathBFailed_ = true;
			xefg::ReportPathBFailure("the display-size canvas could not be created");
			return false;
		}
		if (!ui_redirect::Install(ctx)) {
			pathBFailed_ = true;
			xefg::ReportPathBFailure("UI redirect hooks could not be installed");
			return false;
		}
		// ReShade at the ENB position: on the finished render-size frame,
		// before both the upscale and the bilinear menu path read it.
		pathBReShadeFx_ = ApplyReShadePreUpscale(ctx, fbTex, fb);
		const auto& settings = GetSettings();
		const bool convertGamma = useXess_ && settings.pathBNonLinearColor;
		using Transfer = output_scale::Scaler::Transfer;
		const auto transferColor = [ctx](output_scale::Scaler& scaler, ID3D11Texture2D* source, ID3D11Texture2D* target, Transfer transfer) {
			// The private viewport belongs to the swapped context state, not
			// to the game state that the UI redirect restores at Present.
			ui_redirect::ScopedUntracked untracked;
			return scaler.Scale(ctx, source, target, transfer);
		};
		bool upscaled = false;
		if (PathBInputsReady() && fb.Width == pendingB_.renderWidth && fb.Height == pendingB_.renderHeight) {
			const DXGI_FORMAT typed = output_scale::ViewFormat(fb.Format);
			if (!pathBXessUnavailable_ && bStage_ < 4 && !ldrShared_.tex11) {
				D3D11_TEXTURE2D_DESC d{};
				d.Width = fb.Width;
				d.Height = fb.Height;
				d.MipLevels = d.ArraySize = 1;
				d.Format = convertGamma ? DXGI_FORMAT_R16G16B16A16_FLOAT : typed;
				d.SampleDesc.Count = 1;
				d.Usage = D3D11_USAGE_DEFAULT;
				d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (convertGamma ? D3D11_BIND_RENDER_TARGET : 0);
				ldrShared_ = {};
				interop_.CreateSharedTexture(d, ldrShared_);
				d.Width = outW;
				d.Height = outH;
				d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				sceneOut_ = {};
				interop_.CreateSharedTexture(d, sceneOut_);
				vram_budget::Note("UI Native upscaler input", fb.Width, fb.Height, d.Format);
				vram_budget::Note("UI Native upscaler output", outW, outH, d.Format);
				if (!CreateUpscalerB(fb.Width, fb.Height, outW, outH, typed)) {
					if (useXess_) {
						pathBXessUnavailable_ = true;
						sr_status::SetBackend("XeSS-SR unavailable");
						sr_status::Raise(sr_status::kAlertPathBSr);
						F4SE::log::error("f4sr: Path B XeSS-SR: initialization failed; using bilinear until the pipeline is rebuilt");
					} else if (++bStage_ < 4) {
						// A context that cannot be created (FSR 4.1 without a
						// matching provider, e.g. outside RDNA 4 on Windows)
						// moves on to the next configuration, ending at FSR 3.1.
						F4SE::log::warn("f4sr: ENB before upscaling: upscaler creation failed; trying fallback stage {}", bStage_);
					} else {
						sr_status::SetBackend("FSR unavailable");
						sr_status::Raise(sr_status::kAlertPathBSr);
						F4SE::log::error("f4sr: ENB before upscaling: no FSR configuration could be created; using bilinear until the pipeline is rebuilt");
					}
					ldrShared_ = {};
					sceneOut_ = {};
				}
			}
			if (sceneOut_.res12 && (useXess_ ? xessUpscaler_.Created() : upscalerB_.Created()) && !pathBXessUnavailable_) {
				const bool colorReady = !convertGamma || transferColor(pathBDecode_, fbTex, ldrShared_.tex11.Get(), Transfer::DecodeGamma22);
				if (!convertGamma) ctx->CopyResource(ldrShared_.tex11.Get(), fbTex);
				UpscaleFrame frame = pendingB_;
				if (useXess_) frame.reactive = nullptr;  // Path B XeSS context has no responsive mask.
				frame.color = ldrShared_.res12.Get();
				frame.output = sceneOut_.res12.Get();
				frame.outputWidth = outW;
				frame.outputHeight = outH;
				// The input switches between TAA-filtered and raw colour.
				// Do not blend their histories across that transition.
				frame.reset |= pathBTaaBypassed_ != pathBLastTaaBypassed_;
				bool ok = false;
				if (colorReady && interop_.SignalFromD3D11(ctx) && interop_.WaitOnD3D12()) {
					if (auto* list = interop_.BeginFrame()) {
						for (auto* input : { frame.color, frame.depth, frame.motionVectors, frame.reactive })
							if (input) D3D12Interop::Transition(list, input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
						D3D12Interop::Transition(list, frame.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
						ok = useXess_ ? xessUpscaler_.Dispatch(list, frame) : upscalerB_.Dispatch(list, frame, settings.sharpness, false);
						for (auto* input : { frame.color, frame.depth, frame.motionVectors, frame.reactive })
							if (input) D3D12Interop::Transition(list, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
						D3D12Interop::Transition(list, frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
						ok = interop_.EndFrame() && ok && interop_.SignalFromD3D12() && interop_.WaitOnD3D11(ctx);
					}
				}
				if (ok) {
					if (convertGamma) ok = transferColor(pathBEncode_, sceneOut_.tex11.Get(), canvas_.Get(), Transfer::EncodeGamma22);
					else ctx->CopyResource(canvas_.Get(), sceneOut_.tex11.Get());
				}
				if (ok) {
					sr_status::OnDispatch(true, settings.qualityMode, fb.Width, fb.Height, outW, outH);
					CheckFrameAtDisplay(settings.qualityMode, fb.Width, fb.Height, outW, outH);
					xefg::ConfirmPathBQuality();
				} else sr_status::g.failed.fetch_add(1, std::memory_order_relaxed);
				resetNext_ = !ok;
				if (ok && !bSucceeded_) {
					bSucceeded_ = true;
					sr_status::Clear(sr_status::kAlertPathBSr);
					// The selected FSR 4.1 could not run; stage 3 is FSR 3.1.
					if (!useXess_ && bStage_ == 3 && settings.backend == 1) sr_status::Raise(sr_status::kAlertFsr4);
					if (useXess_) {
						sr_status::SetBackend("XeSS-SR");
						F4SE::log::info("f4sr: Path B XeSS-SR: first pre-UI upscale succeeded ({}x{} -> {}x{}, gamma conversion {})",
							fb.Width, fb.Height, outW, outH, convertGamma);
					} else F4SE::log::info("f4sr: ENB before upscaling: first upscale succeeded (stage {})", bStage_);
				}
				bFailStreak_ = ok ? 0 : bFailStreak_ + 1;
				if (useXess_ && bFailStreak_ >= 30) {
					pathBXessUnavailable_ = true;
					sr_status::SetBackend("XeSS-SR unavailable");
					sr_status::Raise(sr_status::kAlertPathBSr);
					F4SE::log::error("f4sr: Path B XeSS-SR: dispatch/output keeps failing; using bilinear until the pipeline is rebuilt");
				} else if (!useXess_ && !bSucceeded_ && bFailStreak_ >= 30) {
					// Next configuration (see CreateUpscalerB).
					bFailStreak_ = 0;
					++bStage_;
					upscalerB_.Destroy();
					ldrShared_ = {};
					sceneOut_ = {};
					F4SE::log::warn("f4sr: ENB before upscaling: upscale keeps failing; advancing fallback stage to {}", bStage_);
					if (bStage_ >= 4) {
						sr_status::SetBackend("FSR unavailable");
						sr_status::Raise(sr_status::kAlertPathBSr);
						F4SE::log::error("f4sr: ENB before upscaling: every FSR configuration failed; using bilinear until the pipeline is rebuilt");
					}
				}
				if (ok) {
					upscaled = true;
					pathBLastTaaBypassed_ = pathBTaaBypassed_;
					if (pathBTaaBypassed_) {
						static bool logged{};
						if (!logged) {
							logged = true;
							F4SE::log::info("f4sr: Path B: native TAA bypass and pre-UI upscale succeeded on the same frame");
						}
					}
					XeFGFrameInputs in{};
					in.depth = depthShared_.tex11.Get();
					in.motionVectors = mvShared_.tex11.Get();
					in.renderWidth = renderW_;
					in.renderHeight = renderH_;
					in.jitterX = frame.jitterX;
					in.jitterY = frame.jitterY;
					in.frameTimeMs = frame.frameTimeMs;
					in.reset = frame.reset;
					xefg::SetFrameInputs(ctx, in);
				}
			}
		}
		pendingBReady_ = false;
		pathBLastUpscaled_ = upscaled;
		composedThisFrame_ = true;
		if (!upscaled) {
			// Menus and loading screens (no scene inputs): bilinear.
			resetNext_ = true;
			scaleB_.Scale(ctx, fbTex, canvas_.Get());
		}
		xefg::CaptureHudlessFrom(ctx, canvas_.Get());
		xefg::SetComposedFrame(canvas_.Get());
		ui_redirect::Begin(ctx, fbTex, canvas_.Get(), true);
		return true;
	}
}
