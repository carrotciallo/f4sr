#pragma once

#include <cstdint>

namespace f4sr
{
	struct Settings
	{
		bool enableUpscaling{ true };
		int backend{ 1 };          // 0 = FSR 3.1, 1 = FSR 4.1, 2 = XeSS
		int qualityMode{ 0 };      // 0 = Native AA, 1 = Quality, 2 = Balanced, 3 = Performance
		float sharpness{ 0.5f };
		bool enableFrameGeneration{ false };
		// Frame-generation engine, startup-only (the XeSS-FG presenter is
		// attached when the game creates its swap chain): 0 = FSR-FG, 1 = XeSS-FG.
		int frameGenEngine{ 1 };
		// XeSS-FG generated frames per real frame: 1 = 2X, 2 = 3X, 3 = 4X.
		// Above 1 needs the in-memory multi-frame unlock; otherwise clamped to 1.
		int xefgInterpolations{ 1 };
		// Low latency for the XeSS-FG presenter, live: 0 = off (XeLL low-latency
		// mode stays on, xellSleep is not called), 1 = XeLL, 2 = AMD Anti-Lag 2
		// on the interop D3D12 device (falls back to XeLL when unavailable).
		int lowLatency{ 1 };
		// XeSS-FG asynchronous presentation (MCM bXeFGAsyncPresent; plugin ini
		// iXeFGAsyncPresent=1 before 1.1), startup-only: a worker thread does
		// the proxy Present so the game thread does not wait out XeFG's pacing.
		bool asyncPresent{ false };
		bool frameGenOverlay{ false };
		bool frameGenCapture{ false };
		// FSR-FG diagnostic: how the generated frame is presented.
		// 0 = normal (from the chain behind a wrapper such as ENB when there is
		// one), 1 = generated but not presented (one Present per game frame,
		// same two-vblank cadence), 2 = both frames through the wrapper (the
		// pre-fix path). Re-read from f4sr.ini while running (PluginIniInt).
		int fgPresentPath{ 0 };
		bool enbSubNative{ true };
		// Render pipeline (MCM iPipeline, startup-only): 1 = UI at native
		// resolution (Path B: game and ENB at the render size, upscaled before
		// the UI, XeSS-FG output chain at the display size), 0 = Classic (Path A).
		// Path B needs the XeSS-FG engine; enbBeforeUpscaling is the result.
		int pipeline{ 1 };
		bool pipelineFellBack{ false };  // MCM bPipelineFellBack: Path B failed in an earlier run
		bool enbBeforeUpscaling{ false };
		bool pathBNonLinearColor{ true };  // startup-only; preset outputs gamma-encoded SDR
		int outputWidth{ 0 }, outputHeight{ 0 };  // 0 = automatic
		bool debugView{ false };
		bool forceReset{ false };
		bool mvJitterCancellation{ false };  // startup-only diagnostic control for the FSR context
		bool probeOnly{ false };  // target-identification mode: no SR, just map dumps + clear flashes
		bool textureProbe{ false }; // startup-only, bounded Buffout call-site journal
		// Startup-only master switch for the capture/trace diagnostics: raw
		// target dumps, composite/late-draw traces, periodic readback stats,
		// code snapshots and their hooks. Off for normal play.
		bool diagnostics{ false };
		bool logSystemInfo{ true };  // startup-only: environment block at the top of f4sr.log (environment_log.h)
		// With diagnostics on: virtual-key code that re-arms a light IN1/A +
		// IN2/A2 capture on the next resolved frame (motion checks). 0 = off.
		int captureKey{ 0x79 };  // VK_F10
		// Sub-native: rewrite the engine depth in display layout after the
		// resolve, for effects after the seam (imagespace chain, ENB).
		bool postDepth{ true };
		bool dynResGuard{ true };
		bool suppressTaa{ true };  // thunk-side TAA suppression while SR is active
		bool menuEffectFix{ true };   // menu ImageSpace effects at ratio 1 before the seam (menu_effects.h)
		bool nativeTerminal{ false };  // fallback: render natively while the terminal is open
		bool dynResAudit{ false };     // audit always on (otherwise only while listed menus are open)
		bool uiRelayout{ true };      // stretch UI-read targets written before the seam (dynres_audit)
		// Reactive mask: per-pixel change between the opaque composite and the
		// seam, passed to FSR (reactive) and XeSS (responsive pixel mask).
		bool reactiveMask{ true };
		// ReShade effects rendered by f4sr before the UI (reshade_preui.h).
		bool reshadeBeforeUi{ false };
		// Under XeSS-FG, bind F4SR's render-size depth as ReShade's DEPTH on
		// the output runtime (ReShade sees no game draws there). Live.
		bool reshadeDepth{ true };
		float reactiveScale{ 1.0f };
		bool proxyTargets{ true };  // render-resolution proxy targets around the composite/SSLR passes
		int colorSlot{ 4 };       // direct RendererData slots, pinned empirically;
		int mvSlot{ 29 };         // they equal platform(kMain)/platform(kGBufferMotionVectors)
		int depthSlot{ 2 };
	};

	Settings& GetSettings();
	void LoadSettings();
	// Read live MCM overrides without publishing the new settings.
	Settings ReadMcmSettings();
	// Compares the MCM's startup-only values (and frame generation when
	// XeSS-FG was not loaded) with this session; lists what needs a restart
	// in the status panel.
	void UpdateRestartPending();
	// The MCM file's last-write time (0 when missing).
	std::uint64_t McmSettingsWriteTime();
	// Writes one value into the MCM settings file ([Main]).
	void WriteMcmValue(const char* key, const char* value);
	// One integer from Data\F4SE\Plugins\f4sr.ini [f4sr], read now.
	int PluginIniInt(const char* key, int fallback);
	bool ENBPresent();
}
