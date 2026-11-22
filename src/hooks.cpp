#include "dynres_audit.h"
#include "hooks.h"

#include "F4SE/F4SE.h"  // IWYU pragma: keep
#include "RE/Fallout.h"  // IWYU pragma: keep
#include "frame_pipeline.h"
#include "gpu_timing.h"
#include "late_draw_trace.h"
#include "proxy_targets.h"
#include "settings.h"

#include <cstdint>
#include <filesystem>

namespace
{
	// Address-library ids (OG 1.10.163) and call-site offsets, cross-checked
	// against the Dear-Modding CommonLibF4 ID table and the
	// community-shaders anchor list.
	constexpr std::uint64_t kDrawWorldBeginId = 502840;
	constexpr std::uint64_t kDrawWorldRenderUiId = 587723;
	constexpr std::uintptr_t kBeginTemporalOffset = 0x3C1;
	constexpr std::uintptr_t kRenderUiEffectRangeOffset = 0x9F;
	constexpr std::uint64_t kStateSingletonId = 600795;
	constexpr std::uintptr_t kBeginDynViewportOffset = 0x26;
	constexpr std::uint64_t kTaaEffectVtableId = 1340364;
	constexpr std::uint64_t kDrawWorldDeferredCompositeId = 728427;
	constexpr std::uintptr_t kSslrBeginOffset = 0x3D9;
	constexpr std::uintptr_t kSslrAcquireOffset = 0x5FB;
	constexpr std::uintptr_t kSslrReleaseOffset = 0x6BA;
	constexpr std::uintptr_t kCompositePassOffset = 0x8DC;

	using UpdateTemporalData_t = void (*)(RE::BSGraphics::State*);
	using RenderEffectRange_t = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);

	UpdateTemporalData_t g_origTemporal{ nullptr };
	RenderEffectRange_t g_origEffectRange{ nullptr };

	using SetDynViewport_t = void (*)(void*, bool);
	using TaaIsActive_t = bool (*)(void*);

	SetDynViewport_t g_origDynViewport{ nullptr };
	SetDynViewport_t g_origPreInterface{ nullptr };

	ID3D11DeviceContext* GameContext()
	{
		auto* data = RE::BSGraphics::RendererData::GetSingleton();
		return data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
	}

	void PreInterfaceThunk(void* manager, bool enabled)
	{
		f4sr::gpu_timing::MarkAt(GameContext(), f4sr::gpu_timing::kPreInterface);
		g_origPreInterface(manager, enabled);
		f4sr::Pipeline().OnPreInterface(manager);
	}
	TaaIsActive_t g_origTaaIsActive{ nullptr };

	void TemporalThunk(RE::BSGraphics::State* state)
	{
		auto& pipe = f4sr::Pipeline();
		f4sr::gpu_timing::MarkAt(GameContext(), f4sr::gpu_timing::kFrameStart);
		f4sr::dynres_audit::OnFrameStart();
		pipe.BeginTemporalData();  // the native update may query TAA before advancing frameCount
		// Enable the producer only during its update. Never leave a forced
		// user setting behind if SR fails, is disabled, or a menu intervenes.
		if (state && pipe.SrActive() && state->taaState.get() == RE::BSGraphics::TAA_STATE::kDisabled) {
			state->taaState = RE::BSGraphics::TAA_STATE::kEnabled;
			g_origTemporal(state);
			state->taaState = RE::BSGraphics::TAA_STATE::kDisabled;
		} else g_origTemporal(state);
		pipe.OnTemporalData(state);
	}

	RE::BSGraphics::State* GameState()
	{
		// REL::ID(600795) resolves to the State instance itself (a static
		// object in the exe), not to a pointer variable: CommonLibF4's
		// State::GetSingleton() returns *REL::Relocation<State*>{ ID(600795) }.
		// Reading the address as State** dereferences the object's first
		// field (currentFrame) as if it were a pointer - the 2026-11-15
		// crash, with the "pointer" equal to the frame counter (7).
		return reinterpret_cast<RE::BSGraphics::State*>(REL::ID(600795).address());
	}

	void EffectRangeThunk(void* rtManager, std::uint32_t first, std::uint32_t last, std::uint32_t a4, std::uint32_t a5)
	{
		auto& pipe = f4sr::Pipeline();
		f4sr::gpu_timing::MarkAt(GameContext(), f4sr::gpu_timing::kPreImagespace);
		pipe.OnPreImagespace(rtManager);
		f4sr::dynres_audit::OnSeam();
		f4sr::gpu_timing::MarkAt(GameContext(), f4sr::gpu_timing::kPostSr);
		// Suppress the game's own TAA resolve for this range: FSR already
		// resolved the frame into kMain (State::taaState @ 0xA8).
		RE::BSGraphics::State* state = GameState();
		RE::BSGraphics::TAA_STATE saved{};
		const bool suppress = pipe.SrResolved() && state != nullptr && f4sr::GetSettings().suppressTaa;
		if (suppress) {
			saved = state->taaState.get();
			state->taaState = RE::BSGraphics::TAA_STATE::kDisabled;
		}
		g_origEffectRange(rtManager, first, last, a4, a5);
		if (suppress) {
			state->taaState = saved;
		}
		f4sr::gpu_timing::MarkAt(GameContext(), f4sr::gpu_timing::kPostImagespace);
		pipe.OnPostImagespace(rtManager);
		f4sr::dynres_audit::OnPostImagespace();
	}

	void DynViewportThunk(void* manager, bool enabled)
	{
		// community-shaders forces the dynamic viewport as the default
		// while an upscaler drives the frame; the engine's own choice at
		// Begin is not the configuration their handoff is built on. With
		// the flag on, default-viewport consumers follow the ratio
		// state: region during the scene, full after the seam resets it.
		g_origDynViewport(manager, f4sr::Pipeline().SrActive() ? true : enabled);
	}

	bool TaaIsActiveThunk(void* effect)
	{
		// The native TAA imagespace effect must not run over the
		// upscaled frame: it samples the publish target through the
		// dynamic triangle (region UVs) and blends its accumulation
		// over it - a fainter second copy of the scene at render
		// scale. community-shaders disables the effect through this
		// same vfunc while driving an upscaler; the taaState field
		// suppression in EffectRangeThunk does not stop the effect
		// itself (2026-11-16 ENB/dynamic-resolution analysis).
		if (f4sr::Pipeline().SrResolved() && f4sr::GetSettings().suppressTaa) {
			return false;
		}
		return g_origTaaIsActive(effect);
	}

	using SetCurrentRenderTarget_t = void (*)(void*, std::int32_t, std::int32_t, std::int32_t);
	using AcquireRenderTarget_t = void (*)(void*, std::int32_t);
	using ReleaseRenderTarget_t = void (*)(void*, std::int32_t);
	using RenderPassImmediately_t = void (*)(void*, std::uint32_t, bool);

	SetCurrentRenderTarget_t g_origSslrBegin{ nullptr };
	AcquireRenderTarget_t g_origSslrAcquire{ nullptr };
	ReleaseRenderTarget_t g_origSslrRelease{ nullptr };
	RenderPassImmediately_t g_origCompositePass{ nullptr };
	RenderPassImmediately_t g_origEarlyPass{};
	void EarlyPassThunk(void* pass,std::uint32_t technique,bool flag)
	{
		auto& pipe=f4sr::Pipeline();
		if(pipe.ProxyDriving() && f4sr::GetSettings().enbSubNative) {
			auto* data=RE::BSGraphics::RendererData::GetSingleton();
			if(data) return f4sr::late_trace::RunEarlyPass(reinterpret_cast<ID3D11DeviceContext*>(data->context),
				static_cast<ID3D11Texture2D*>(pipe.Proxies().DebugSlotTexture(f4sr::logical::kMainCopy)),g_origEarlyPass,pass,technique,flag);
		}
		g_origEarlyPass(pass,technique,flag);
	}

	// The deferred composite and the SSLR chain sample with
	// texture-space UVs that assume the bound target is the whole
	// image; at sub-native the engine targets only hold the render
	// region. community-shaders wraps these exact call sites with
	// render-resolution proxy passes (see proxy_targets.cpp).
	void SslrBeginThunk(void* manager, std::int32_t slot, std::int32_t logical, std::int32_t mode)
	{
		auto& pipe = f4sr::Pipeline();
		if (pipe.ProxyDriving()) {
			pipe.Proxies().BeginProxyPass(
				{ f4sr::logical::kGBufferNormal, f4sr::logical::kGBufferEnvironment, f4sr::logical::kMainCopy },
				f4sr::ProxyTargets::ProxyTriangle::kUnit);
		}
		g_origSslrBegin(manager, slot, logical, mode);
	}

	void SslrAcquireThunk(void* manager, std::int32_t logical)
	{
		g_origSslrAcquire(manager, logical);
		auto& pipe = f4sr::Pipeline();
		if (pipe.ProxyDriving()) {
			pipe.Proxies().ProxyAcquiredRenderTarget(f4sr::logical::kSSRBlurred0);
		}
	}

	void SslrReleaseThunk(void* manager, std::int32_t logical)
	{
		auto& pipe = f4sr::Pipeline();
		if (pipe.ProxyDriving()) {
			// The pooled slot must be restored while it is still
			// mapped; water then samples the results in the
			// engine's region layout (community-shaders' order).
			pipe.Proxies().ReleaseAcquiredRenderTarget(f4sr::logical::kSSRBlurred0);
			pipe.Proxies().EndProxyPass({ f4sr::logical::kSSR, f4sr::logical::kSSRBlurred1 });
		}
		g_origSslrRelease(manager, logical);
	}

	void CompositePassThunk(void* pass, std::uint32_t a2, bool a3)
	{
		auto& pipe = f4sr::Pipeline();
		const bool driving = pipe.ProxyDriving();
		bool scopeStarted = false;
		if (driving) {
			pipe.CompositeProbe(0);
			scopeStarted = pipe.Proxies().BeginProxyPass(
				{ f4sr::logical::kGBufferNormal, f4sr::logical::kAmbientOcclusion,
					f4sr::logical::kGBufferEnvironment, f4sr::logical::kGBufferAmbient,
					f4sr::logical::kGBufferEmissive, f4sr::logical::kDiffuseLight,
					f4sr::logical::kDiffuseSpecular, f4sr::logical::kMainCopy,
					f4sr::logical::kDiffuseLightTiled, f4sr::logical::kDiffuseSpecularTiled,
					f4sr::logical::kAmbientOcclusionHalf },
				f4sr::ProxyTargets::ProxyTriangle::kEngine);
			pipe.CompositeProbe(1);
		}
		g_origCompositePass(pass, a2, a3);
		if (scopeStarted) {
			pipe.CompositeProbe(2);
			// Only this fullscreen pass uses render-sized texture-space
			// inputs. Forward needs native allocations + reduced viewport,
			// not scaled allocations with the reduced ratio applied again.
			// EndProxyPass also repoints actual bound RTV/SRVs, not just
			// engine structs, so cached logical bindings cannot retain a proxy.
			pipe.Proxies().EndProxyPass({ f4sr::logical::kMain });
			pipe.CompositeProbe(3);
		}
		pipe.OnOpaqueComposited();
	}

	// Runtime snapshots from the 05:47 session verify a second conditional
	// RenderPassImmediately call AFTER the existing +0x8DC probe. Do not
	// conflate that first pass with the end of DeferredComposite.
	RenderPassImmediately_t g_origLateCompositePass{ nullptr };
	void LateCompositePassThunk(void* pass, std::uint32_t technique, bool flag)
	{
		auto& pipe = f4sr::Pipeline();
		if (pipe.ProxyDriving()) pipe.CompositeProbe(6);
		g_origLateCompositePass(pass, technique, flag);
		if (pipe.ProxyDriving()) pipe.CompositeProbe(7);
	}

	using RenderStage_t = void (*)();
	RenderStage_t g_origWindowStages[3]{};
	template <int Index>
	void RenderWindowStageThunk()
	{
		g_origWindowStages[Index]();
		auto& pipe = f4sr::Pipeline();
		if (pipe.ProxyDriving()) pipe.CompositeProbe(8 + Index);
	}

	// The callback slot and virtual target are mutable. Resolve them for each
	// invocation, not at installation. The +26B transfer is a tail JMP after
	// stack teardown, so the replacement must also be entered via JMP.
	using Lighting = void(*)(void*, void*, void*);
	using Helper = void(*)(void*);
	using Tail = std::uintptr_t(*)(void*, unsigned);
	Lighting originalLight{};
	Helper originalHelper{}, *callbackSlot{};
	void TraceLighting(void* self, void* flags, void* xyz)
	{
		f4sr::late_trace::Mark("23a-in"); originalLight(self,flags,xyz); f4sr::late_trace::Mark("23a-out");
	}
	template<unsigned Offset> void TraceHelper(void* self)
	{
		const auto fn = Offset==0x252 ? *callbackSlot : originalHelper;
		f4sr::late_trace::Mark(Offset==0x252 ? "252-in" : Offset==0x246 ? "246-in" : "1ec-in");
		fn(self);
		f4sr::late_trace::Mark(Offset==0x252 ? "252-out" : Offset==0x246 ? "246-out" : "1ec-out");
	}
	std::uintptr_t TraceTail(void* self, unsigned flag)
	{
		const auto fn = (*reinterpret_cast<Tail**>(self))[0x168/sizeof(void*)];
		f4sr::late_trace::Mark("26b-in"); const auto result=fn(self,flag); f4sr::late_trace::Mark("26b-out");
		return result;
	}

	// Snapshot only known executable windows, once on the first sub-native
	// UI boundary. The disk EXE is packed; these bytes permit offline call
	// tracing without attaching a debugger or changing render behavior.
	void CaptureRenderWindowCode()
	{
		static bool attempted = false;
		if (attempted) return;
		attempted = true;
		wchar_t executable[MAX_PATH]{};
		if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return;
		const auto root = std::filesystem::path(executable).parent_path();
		const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto tick = GetTickCount64();
		struct Window { std::uint64_t id; std::size_t bytes; };
		constexpr Window windows[]{ {728427, 0xA00}, {656535, 0x1000},
			{984743, 0x800}, {587723, 0x120}, {408683, 0x240}, {572620, 0x220} };
		for (const auto& window : windows) {
			const auto start = REL::ID(window.id).address();
			const auto end = start + window.bytes;
			auto cursor = start;
			while (cursor < end) {
				MEMORY_BASIC_INFORMATION info{};
				if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT ||
					(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
					!(info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) break;
				const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
				if (next <= cursor) break;
				cursor = next;
			}
			if (cursor < end) { F4SE::log::warn("f4sr: code snapshot rejected id={}", window.id); continue; }
			const auto name = L"f4sr-code-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(tick) +
				L"-id" + std::to_wstring(window.id) + L"-rva" + std::to_wstring(start - module) + L".bin";
			const auto file = CreateFileW((root / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			DWORD written{};
			const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, reinterpret_cast<void*>(start),
				static_cast<DWORD>(window.bytes), &written, nullptr) && written == window.bytes;
			if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
			F4SE::log::info("f4sr: code snapshot id={} rva={:#x} bytes={} ok={} pid={} tick={}",
				window.id, start - module, written, ok, GetCurrentProcessId(), tick);
		}
	}

	using RenderUi_t = void (*)();
	RenderUi_t g_origRenderUi{ nullptr };
	void RenderUiEntryThunk()
	{
		auto& pipe = f4sr::Pipeline();
		if (pipe.ProxyDriving()) {
			pipe.CompositeProbe(5);
			if (f4sr::GetSettings().diagnostics) CaptureRenderWindowCode();
		}
		g_origRenderUi();
	}

	// Sanity check: the site must currently hold a relative call (0xE8).
	bool IsRelativeCall(std::uintptr_t site)
	{
		return *reinterpret_cast<const std::uint8_t*>(site) == 0xE8;
	}
}

namespace f4sr
{
	void InstallHooks()
	{
		auto& trampoline = F4SE::GetTrampoline();

		// Preflight all mandatory paired sites before installing any of them.
		const auto beginBase = REL::ID(kDrawWorldBeginId).address();
		const auto uiBase = REL::ID(kDrawWorldRenderUiId).address();
		const auto deferredBase = REL::ID(kDrawWorldDeferredCompositeId).address();
		for (const auto at : { beginBase + kBeginTemporalOffset, beginBase + kBeginDynViewportOffset,
			uiBase + kRenderUiEffectRangeOffset, uiBase + 0xE1,
			deferredBase + kSslrBeginOffset, deferredBase + kSslrAcquireOffset,
			deferredBase + kSslrReleaseOffset, deferredBase + kCompositePassOffset }) {
			if (!IsRelativeCall(at)) {
				F4SE::log::error("f4sr: hook preflight failed at {:X}; no pipeline hooks installed", at);
				return;
			}
		}
		// CS OG pre-interface anchor calls the same viewport setter as Begin.
		auto callTarget = [](std::uintptr_t site) {
			std::int32_t displacement{};
			std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
			return site + 5 + displacement;
		};
		if (callTarget(uiBase + 0xE1) != callTarget(beginBase + kBeginDynViewportOffset)) {
			F4SE::log::error("f4sr: pre-interface callee mismatch; no pipeline hooks installed");
			return;
		}
		// Diagnostic-only hooks (CMP5, CMP6..10, late trace boundaries) are
		// installed only when bDiagnostics is set; normal play keeps these
		// call sites untouched.
		const bool diagnostics = GetSettings().diagnostics;
		// Optional diagnostic boundary from CS EnsureImagespaceInstalled.
		// OG callers target a jmp thunk (1130087), not Render_UI's body.
		const std::uintptr_t uiEntries[]{ REL::ID(408683).address() + 0x163,
			REL::ID(572620).address() + 0x103, REL::ID(572620).address() + 0x1A7 };
		const auto uiCallee = REL::ID(1130087).address();
		bool entriesValid = diagnostics && trampoline.free_size() >= 128;
		for (const auto site : uiEntries) entriesValid = entriesValid && IsRelativeCall(site) && callTarget(site) == uiCallee;
		if (entriesValid) {
			g_origRenderUi = reinterpret_cast<RenderUi_t>(uiCallee);
			for (const auto site : uiEntries) trampoline.write_call<5>(site, &RenderUiEntryThunk);
			F4SE::log::info("f4sr: Render_UI entry diagnostic installed at 3 verified call sites (CMP5)");
		} else if (diagnostics) {
			F4SE::log::warn("f4sr: CMP5 diagnostic unavailable; Render_UI caller validation or trampoline budget failed");
		}
		g_origPreInterface = reinterpret_cast<SetDynViewport_t>(trampoline.write_call<5>(uiBase + 0xE1, &PreInterfaceThunk));
		const std::uintptr_t temporalSite = beginBase + kBeginTemporalOffset;
		if (!IsRelativeCall(temporalSite)) {
			F4SE::log::error("f4sr: UpdateTemporalData call site is not a relative call; hooks not installed");
			return;
		}
		// write_call returns the trampoline stub that invokes the original.
		g_origTemporal = reinterpret_cast<UpdateTemporalData_t>(trampoline.write_call<5>(temporalSite, &TemporalThunk));

		const std::uintptr_t effectSite = REL::ID(kDrawWorldRenderUiId).address() + kRenderUiEffectRangeOffset;
		if (!IsRelativeCall(effectSite)) {
			F4SE::log::error("f4sr: RenderEffectRange call site is not a relative call; SR seam not installed");
			return;
		}
		g_origEffectRange = reinterpret_cast<RenderEffectRange_t>(trampoline.write_call<5>(effectSite, &EffectRangeThunk));

		const std::uintptr_t dynVpSite = REL::ID(kDrawWorldBeginId).address() + kBeginDynViewportOffset;
		if (!IsRelativeCall(dynVpSite)) {
			F4SE::log::error("f4sr: SetUseDynamicResolutionViewport call site is not a relative call; flag hook not installed");
		} else {
			g_origDynViewport = reinterpret_cast<SetDynViewport_t>(trampoline.write_call<5>(dynVpSite, &DynViewportThunk));
		}

		// Vtable patch in the exe's .rdata: needs a protection change,
		// unlike the call-site hooks above.
		auto* taaSlot = reinterpret_cast<std::uintptr_t*>(REL::ID(kTaaEffectVtableId).address() + 0x40);
		g_origTaaIsActive = reinterpret_cast<TaaIsActive_t>(*taaSlot);
		DWORD oldProtect = 0;
		if (VirtualProtect(taaSlot, sizeof(std::uintptr_t), PAGE_READWRITE, &oldProtect)) {
			*taaSlot = reinterpret_cast<std::uintptr_t>(&TaaIsActiveThunk);
			VirtualProtect(taaSlot, sizeof(std::uintptr_t), oldProtect, &oldProtect);
			F4SE::log::info("f4sr: TAA IsActive hook installed");
		} else {
			F4SE::log::error("f4sr: TAA IsActive hook failed (VirtualProtect)");
		}

		// DeferredComposite proxy-pass hooks (community-shaders
		// anchors, OG offsets). A site that fails its sanity check
		// only loses its proxy wrap; the rest of the plugin runs.
		const std::uintptr_t compositeBase = REL::ID(kDrawWorldDeferredCompositeId).address();
		struct Site
		{
			std::uintptr_t offset;
			void* thunk;
			void** orig;
			const char* name;
		};
		const Site sites[] = {
			{ kSslrBeginOffset, reinterpret_cast<void*>(&SslrBeginThunk), reinterpret_cast<void**>(&g_origSslrBegin), "SSLR begin" },
			{ kSslrAcquireOffset, reinterpret_cast<void*>(&SslrAcquireThunk), reinterpret_cast<void**>(&g_origSslrAcquire), "SSLR blur acquire" },
			{ kSslrReleaseOffset, reinterpret_cast<void*>(&SslrReleaseThunk), reinterpret_cast<void**>(&g_origSslrRelease), "SSLR blur release" },
			{ kCompositePassOffset, reinterpret_cast<void*>(&CompositePassThunk), reinterpret_cast<void**>(&g_origCompositePass), "composite pass" },
		};
		for (const auto& site : sites) {
			const std::uintptr_t at = compositeBase + site.offset;
			if (!IsRelativeCall(at)) {
				F4SE::log::error("f4sr: {} call site is not a relative call; proxy hook skipped", site.name);
				continue;
			}
			*site.orig = reinterpret_cast<void*>(trampoline.write_call<5>(at, site.thunk));
		}
		F4SE::log::info("f4sr: composite/SSLR proxy hooks installed");

		// Optional, read-only diagnostics. Validate the complete group AFTER
		// allocating mandatory hooks; reject all four sites on any mismatch.
		// Each callee/offset was decoded from this executable's live bytes,
		// not from the packed on-disk image or an inferred draw order.
		const auto preUiBase = REL::ID(984743).address();
		const auto lateCompositeSite = compositeBase + 0x989;
		const std::uintptr_t stageSites[]{ preUiBase + 0x1C4, preUiBase + 0x1C9, preUiBase + 0x1CE };
		const std::uintptr_t stageTargets[]{ compositeBase, REL::ID(656535).address(), REL::ID(1572250).address() };
		bool windowValid = diagnostics && trampoline.free_size() >= 128 && IsRelativeCall(lateCompositeSite) &&
			callTarget(lateCompositeSite) == REL::ID(244233).address();
		for (std::size_t i = 0; i < 3; ++i) {
			windowValid = windowValid && IsRelativeCall(stageSites[i]) && callTarget(stageSites[i]) == stageTargets[i];
		}
		if (windowValid) {
			g_origLateCompositePass = reinterpret_cast<RenderPassImmediately_t>(
				trampoline.write_call<5>(lateCompositeSite, &LateCompositePassThunk));
			g_origWindowStages[0] = reinterpret_cast<RenderStage_t>(trampoline.write_call<5>(stageSites[0], &RenderWindowStageThunk<0>));
			g_origWindowStages[1] = reinterpret_cast<RenderStage_t>(trampoline.write_call<5>(stageSites[1], &RenderWindowStageThunk<1>));
			g_origWindowStages[2] = reinterpret_cast<RenderStage_t>(trampoline.write_call<5>(stageSites[2], &RenderWindowStageThunk<2>));
			F4SE::log::info("f4sr: render-window diagnostics installed at 4 verified call sites (CMP6..10)");
		} else if (diagnostics) {
			F4SE::log::warn("f4sr: CMP6..10 unavailable; render-window callee validation or trampoline budget failed");
		}

		// OG runtime bytes, including call [rip+disp] and the epilogue leading
		// into the virtual JMP. Leave the entire optional group alone on mismatch.
		constexpr unsigned char cb[]{0xff,0x15,0xb8,0xbb,0xec,0x03};
		constexpr unsigned char ep[]{0x48,0x8b,0x01,0x48,0x83,0xc4,0x70,0x5e,0x48,0xff,0xa0,0x68,0x01,0,0};
		const auto light=REL::ID(291127).address(), helper=REL::ID(158420).address();
		if (windowValid && entriesValid && trampoline.free_size()>=96 &&
			IsRelativeCall(preUiBase+0x23a) && callTarget(preUiBase+0x23a)==light &&
			IsRelativeCall(preUiBase+0x246) && callTarget(preUiBase+0x246)==helper &&
			IsRelativeCall(preUiBase+0x1ec) && callTarget(preUiBase+0x1ec)==helper &&
			!std::memcmp(reinterpret_cast<const void*>(preUiBase+0x252),cb,sizeof(cb)) &&
			!std::memcmp(reinterpret_cast<const void*>(preUiBase+0x263),ep,sizeof(ep))) {
			originalLight=reinterpret_cast<Lighting>(light); originalHelper=reinterpret_cast<Helper>(helper);
			callbackSlot=reinterpret_cast<Helper*>(preUiBase+0x258+0x03ecbbb8);
			trampoline.write_call<5>(preUiBase+0x1ec,&TraceHelper<0x1ec>);
			trampoline.write_call<5>(preUiBase+0x23a,&TraceLighting);
			trampoline.write_call<5>(preUiBase+0x246,&TraceHelper<0x246>);
			trampoline.write_call<6>(preUiBase+0x252,&TraceHelper<0x252>);
			trampoline.write_branch<5>(preUiBase+0x26b,&TraceTail);
			F4SE::log::info("f4sr: late trace boundaries verified and installed");
		} else if (diagnostics) F4SE::log::warn("f4sr: late trace boundaries skipped: preflight failed");

		// The live OG capture puts the early three-argument call at +1f4;
		// the two later calls may already point into our trampoline.
		const auto early=compositeBase+0x1f4;
		if(trampoline.free_size()>=16 && IsRelativeCall(early) && callTarget(early)==compositeBase+0x29910) {
			g_origEarlyPass=reinterpret_cast<RenderPassImmediately_t>(trampoline.write_call<5>(early,&EarlyPassThunk));
		} else F4SE::log::warn("f4sr: early scene gate unavailable; not changing the pass");
		F4SE::log::info("f4sr: hooks installed (jitter capture + SR seam)");
	}
}
