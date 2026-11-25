#pragma once

#include <windows.h>

#include <string>

// ReShade effects before the UI.
//
// When ReShade (with add-on support) is loaded, f4sr registers as a ReShade
// add-on and, with bReShadeBeforeUI, renders the ReShade effects itself at the
// pre-interface point: after upscaling and ENB's post-processing, at display
// resolution, before the UI is drawn and before frame generation captures its
// HUD-less image. ReShade then skips its own present-time rendering for that
// frame, so the UI stays untouched and generated frames carry the effects
// through interpolation instead of UI extraction.
namespace f4sr::reshade_preui
{
	// Registers the add-on when a ReShade module is loaded. Safe to call from
	// F4SEPlugin_Load (no device work happens here).
	void Install(HMODULE self);
	// Pre-interface hook, before the HUD-less captures.
	void Render(void* rtManager);
	// Path B composed the frame; renderedBeforeUpscale when the effects ran
	// at the ENB position this frame (RenderPreUpscale).
	void AfterPathB(bool renderedBeforeUpscale);
	// UI Native (XeSS-FG output runtime): bReShadeBeforeUI is on, the output
	// runtime exists, and the mode has not failed.
	bool PreUpscaleWanted();
	// Around D3D12Interop::BeginFrame on the game thread: ReShade reports its
	// handle of the command list being reset, which RenderPreUpscale records into.
	void ExpectCommandList(bool on);
	// Records the effects onto colorResource (ID3D12Resource*, render size,
	// in RENDER_TARGET state) with depthResource (ID3D12Resource*, R32_FLOAT,
	// shader-readable, or null) as DEPTH. Presents then only mark the effects
	// as rendered, so they never run again on the display-size output.
	bool RenderPreUpscale(void* colorResource, void* depthResource);
	// Before the pre-upscale targets are released: presents render the
	// effects again, ReShade's views of the targets are destroyed and DEPTH
	// is unbound. The caller still has to drain the GPU before releasing.
	void ReleasePreUpscaleTargets();
	// Before every extra present of a game frame (FSR-FG pairs): keeps ReShade
	// from rendering the effects again on top of the UI at that present.
	void SuppressPresentEffects();
	// True while this frame's effects were rendered before the UI.
	bool Active();
	// f4sr is registered as a ReShade add-on.
	bool Registered();
	// The module is the loaded ReShade module.
	bool IsReShadeModule(HMODULE module);
	// Under XeSS-FG: bReShadeDepth is on and ReShade has an output runtime
	// to bind F4SR's depth to.
	bool WantsDepth();
	// Status panel row(s) for ReShade, or empty when ReShade is not loaded.
	std::string PanelLine();
	// ReShade created an effect runtime on a non-D3D11 swap chain (the XeSS-FG
	// output chain): its effects run on every presented frame there.
	bool OutputRuntimeSeen();
}
