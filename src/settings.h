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
		bool frameGenOverlay{ false };
		bool frameGenCapture{ false };
		// FSR-FG diagnostic: how the generated frame is presented.
		// 0 = normal (from the chain behind a wrapper such as ENB when there is
		// one), 1 = generated but not presented (one Present per game frame,
		// same two-vblank cadence), 2 = both frames through the wrapper (the
		// pre-fix path). Re-read from f4sr.ini while running (PluginIniInt).
		int fgPresentPath{ 0 };
		bool enbSubNative{ true };
		bool debugView{ false };
		bool forceReset{ false };
		bool mvJitterCancellation{ false };  // startup-only diagnostic control for the FSR context
		bool probeOnly{ false };  // target-identification mode: no SR, just map dumps + clear flashes
		bool textureProbe{ false }; // startup-only, bounded Buffout call-site journal
		// Startup-only master switch for the capture/trace diagnostics: raw
		// target dumps, composite/late-draw traces, periodic readback stats,
		// code snapshots and their hooks. Off for normal play.
		bool diagnostics{ false };
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
		float reactiveScale{ 1.0f };
		bool proxyTargets{ true };  // render-resolution proxy targets around the composite/SSLR passes
		int colorSlot{ 4 };       // direct RendererData slots, pinned empirically;
		int mvSlot{ 29 };         // they equal platform(kMain)/platform(kGBufferMotionVectors)
		int depthSlot{ 2 };
	};

	Settings& GetSettings();
	void LoadSettings();
	// Live switching: re-apply the MCM settings pass (Data\MCM\
	// Settings\f4sr.ini) onto the live Settings, and the MCM file's
	// last-write time (0 when missing) for change detection.
	void ReloadMcmSettings();
	std::uint64_t McmSettingsWriteTime();
	// One integer from Data\F4SE\Plugins\f4sr.ini [f4sr], read now.
	int PluginIniInt(const char* key, int fallback);
	bool ENBPresent();
}
