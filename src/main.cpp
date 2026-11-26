// f4sr - FSR 3.1.6 / 4.1.2 upscaling + FSR frame generation for Fallout 4 (AMD)
// Self-built F4SE plugin: FidelityFX SDK 2.4 (ffx-api) + D3D11/D3D12 interop.

#include "environment_log.h"
#include "log_redact.h"
#include "menu_effects.h"
#include "reshade_preui.h"
#include <algorithm>
#include "F4SE/F4SE.h"
#include "xefg_presenter.h"
#include "RE/Fallout.h"

#include <windows.h>

#include <string>
#include <vector>

#include "ffx_runtime.h"
#include "hooks.h"
#include "settings.h"
#include "sr_status.h"
#include "version.h"
#include "shader_capture.h"
#include "texture_probe.h"

extern "C" __declspec(dllexport) constinit F4SE::PluginVersionData F4SEPlugin_VersionData = [] {
	F4SE::PluginVersionData data{};
	data.PluginVersion(REL::Version{ 1, 1, 0, 0 });
	data.PluginName("f4sr");
	data.AuthorName("Muse for Carrot");
	data.UsesAddressLibrary(true);
	data.CompatibleVersions({ REL::Version{ 1, 10, 163, 0 } });
	return data;
}();

// CommonLibF4's PluginVersionData::GetSingleton looks up this shorter
// name ("F4SEPlugin_Version"); F4SE itself reads the one above.
extern "C" __declspec(dllexport) constinit F4SE::PluginVersionData F4SEPlugin_Version = [] {
	F4SE::PluginVersionData data{};
	data.PluginVersion(REL::Version{ 1, 1, 0, 0 });
	data.PluginName("f4sr");
	data.AuthorName("Muse for Carrot");
	data.UsesAddressLibrary(true);
	data.CompatibleVersions({ REL::Version{ 1, 10, 163, 0 } });
	return data;
}();


namespace f4sr
{
	Settings& GetSettings()
	{
		static Settings settings;
		return settings;
	}

	bool ENBPresent()
	{
		if (GetModuleHandleA("enbseries.dll") != nullptr) {
			return true;
		}
		// ENB also replaces d3d11.dll in the game directory.
		HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
		if (d3d11 == nullptr) {
			return false;
		}
		char path[MAX_PATH]{};
		GetModuleFileNameA(d3d11, path, MAX_PATH);
		char exePath[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		std::string dir{ exePath };
		const auto pos = dir.find_last_of("\\/");
		if (pos != std::string::npos) {
			dir.resize(pos + 1);
		}
		return std::string{ path }.starts_with(dir);
	}

	static std::string McmIniPath()
	{
		char exePath[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		std::string path{ exePath };
		if (const auto pos = path.find_last_of("\\/"); pos != std::string::npos) {
			path.resize(pos + 1);
		}
		path += "Data\\MCM\\Settings\\f4sr.ini";
		return path;
	}

	void WriteMcmValue(const char* key, const char* value)
	{
		const std::string mcmIni = McmIniPath();
		if (!WritePrivateProfileStringA("Main", key, value, mcmIni.c_str()))
			F4SE::log::error("f4sr: could not write {}={} to {} ({})", key, value, mcmIni, GetLastError());
	}

	// Values the MCM page saved override the plugin ini. `startup` also reads
	// the keys that only apply at launch (frame-generation engine, FSR
	// jitter-cancellation flag). Clamps the ranged values.
	// This session's settings as loaded at startup (for UpdateRestartPending).
	static Settings g_startup;

	static void ApplyMcmSettings(Settings& s, bool startup)
	{
		const std::string mcmIni = McmIniPath();
		auto read = [&](const char* key, std::string& out) {
			char b[32]{};
			if (GetPrivateProfileStringA("Main", key, "", b, sizeof(b), mcmIni.c_str()) <= 0) return false;
			out = b;
			return true;
		};
		std::string v;
		auto mcmInt = [&](const char* key, int& out) { if (read(key, v)) out = std::atoi(v.c_str()); };
		auto mcmBool = [&](const char* key, bool& out) { if (read(key, v)) out = std::atoi(v.c_str()) != 0; };
		auto mcmFloat = [&](const char* key, float& out) { if (read(key, v)) out = static_cast<float>(std::atof(v.c_str())); };
		mcmBool("bEnableUpscaling", s.enableUpscaling);
		mcmInt("iBackend", s.backend);
		mcmInt("iQualityMode", s.qualityMode);
		mcmFloat("fSharpness", s.sharpness);
		mcmBool("bEnableFrameGeneration", s.enableFrameGeneration);
		if (startup) mcmInt("iFrameGenEngine", s.frameGenEngine);
		if (startup) mcmInt("iPipeline", s.pipeline);
		if (startup) mcmBool("bXeFGAsyncPresent", s.asyncPresent);
		if (startup) mcmBool("bPipelineFellBack", s.pipelineFellBack);
		mcmInt("iXeFGInterpolations", s.xefgInterpolations);
		int mode = -1;  // MCM dropdown: 0 = 2X, 1 = 3X, 2 = 4X (supersedes iXeFGInterpolations)
		mcmInt("iXeFGMode", mode);
		if (mode >= 0) s.xefgInterpolations = mode + 1;
		mcmInt("iLowLatency", s.lowLatency);
		mcmBool("bReactiveMask", s.reactiveMask);
		mcmBool("bReShadeBeforeUI", s.reshadeBeforeUi);
		mcmBool("bReShadeDepth", s.reshadeDepth);
		// bENBSubNative is plugin-ini only: Native in Render Resolution covers
		// the opt-out, and an MCM value saved by RC1-RC3 (default off) must not
		// keep holding ENB users at native.
		mcmBool("bDebugView", s.debugView);
		mcmBool("bForceReset", s.forceReset);
		mcmBool("bFrameGenOverlay", s.frameGenOverlay);
		mcmBool("bFrameGenCapture", s.frameGenCapture);
		if (startup) mcmBool("bMVJitterCancellation", s.mvJitterCancellation);
		if (startup) mcmBool("bLogSystemInfo", s.logSystemInfo);
		s.qualityMode = std::clamp(s.qualityMode, 0, 3);
		s.frameGenEngine = std::clamp(s.frameGenEngine, 0, 1);
		s.xefgInterpolations = std::clamp(s.xefgInterpolations, 1, 3);
		s.lowLatency = std::clamp(s.lowLatency, 0, 2);
	}

	// The plugin lives in Data\F4SE\Plugins; settings sit next to the exe
	// under Data\F4SE\Plugins\f4sr.ini.
	static std::string PluginIniPath()
	{
		char exePath[MAX_PATH]{};
		GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		std::string ini{ exePath };
		const auto pos = ini.find_last_of("\\/");
		if (pos != std::string::npos) {
			ini.resize(pos + 1);
		}
		ini += "Data\\F4SE\\Plugins\\f4sr.ini";
		return ini;
	}

	int PluginIniInt(const char* key, int fallback)
	{
		return static_cast<int>(GetPrivateProfileIntA("f4sr", key, fallback, PluginIniPath().c_str()));
	}

	void LoadSettings()
	{
		const std::string ini = PluginIniPath();

		auto& s = GetSettings();
		s.enableUpscaling = GetPrivateProfileIntA("f4sr", "bEnableUpscaling", 1, ini.c_str()) != 0;
		s.backend = GetPrivateProfileIntA("f4sr", "iBackend", 1, ini.c_str());
		s.qualityMode = GetPrivateProfileIntA("f4sr", "iQualityMode", 0, ini.c_str());
		char buf[32]{};
		GetPrivateProfileStringA("f4sr", "fSharpness", "0.5", buf, sizeof(buf), ini.c_str());
		s.sharpness = static_cast<float>(std::atof(buf));
		s.enableFrameGeneration = GetPrivateProfileIntA("f4sr", "bEnableFrameGeneration", 0, ini.c_str()) != 0;
		s.frameGenEngine = GetPrivateProfileIntA("f4sr", "iFrameGenEngine", 1, ini.c_str());
		s.xefgInterpolations = GetPrivateProfileIntA("f4sr", "iXeFGInterpolations", 1, ini.c_str());
		s.lowLatency = GetPrivateProfileIntA("f4sr", "iLowLatency", 1, ini.c_str());
		s.frameGenOverlay = GetPrivateProfileIntA("f4sr", "bFrameGenOverlay", 0, ini.c_str()) != 0;
		s.frameGenCapture = GetPrivateProfileIntA("f4sr", "bFrameGenCapture", 0, ini.c_str()) != 0;
		s.fgPresentPath = GetPrivateProfileIntA("f4sr", "iFGPresentPath", 0, ini.c_str());
		s.enbSubNative = GetPrivateProfileIntA("f4sr", "bENBSubNative", 1, ini.c_str()) != 0;
		// bENBBeforeUpscaling (RC packages shipped it as 0) is no longer read:
		// the MCM Render Pipeline choice replaces it.
		s.pipeline = GetPrivateProfileIntA("f4sr", "iPipeline", 1, ini.c_str());
		s.pathBNonLinearColor = GetPrivateProfileIntA("f4sr", "bPathBNonLinearColor", 1, ini.c_str()) != 0;
		s.outputWidth = static_cast<int>(GetPrivateProfileIntA("f4sr", "iOutputWidth", 0, ini.c_str()));
		s.outputHeight = static_cast<int>(GetPrivateProfileIntA("f4sr", "iOutputHeight", 0, ini.c_str()));
		s.debugView = GetPrivateProfileIntA("f4sr", "bDebugView", 0, ini.c_str()) != 0;
		s.forceReset = GetPrivateProfileIntA("f4sr", "bForceReset", 0, ini.c_str()) != 0;
		s.probeOnly = GetPrivateProfileIntA("f4sr", "bProbeOnly", 0, ini.c_str()) != 0;
		s.textureProbe = GetPrivateProfileIntA("f4sr", "bTextureProbe", 0, ini.c_str()) != 0;
		s.diagnostics = GetPrivateProfileIntA("f4sr", "bDiagnostics", 0, ini.c_str()) != 0;
		s.logSystemInfo = GetPrivateProfileIntA("f4sr", "bLogSystemInfo", 1, ini.c_str()) != 0;
		s.captureKey = GetPrivateProfileIntA("f4sr", "iCaptureKey", 0x79, ini.c_str());
		s.postDepth = GetPrivateProfileIntA("f4sr", "bPostDepth", 1, ini.c_str()) != 0;
		s.dynResGuard = GetPrivateProfileIntA("f4sr", "bDynResGuard", 1, ini.c_str()) != 0;
		s.mvJitterCancellation = GetPrivateProfileIntA("f4sr", "bMVJitterCancellation", 0, ini.c_str()) != 0;
		s.suppressTaa = GetPrivateProfileIntA("f4sr", "bSuppressTAA", 1, ini.c_str()) != 0;
	s.proxyTargets = GetPrivateProfileIntA("f4sr", "bProxyTargets", 1, ini.c_str()) != 0;
		s.menuEffectFix = GetPrivateProfileIntA("f4sr", "bMenuEffectFix", 1, ini.c_str()) != 0;
		s.nativeTerminal = GetPrivateProfileIntA("f4sr", "bNativeTerminal", 0, ini.c_str()) != 0;
		s.dynResAudit = GetPrivateProfileIntA("f4sr", "bDynResAudit", 0, ini.c_str()) != 0;
		s.uiRelayout = GetPrivateProfileIntA("f4sr", "bUiRelayout", 1, ini.c_str()) != 0;
		s.reactiveMask = GetPrivateProfileIntA("f4sr", "bReactiveMask", 1, ini.c_str()) != 0;
		s.asyncPresent = GetPrivateProfileIntA("f4sr", "iXeFGAsyncPresent", 0, ini.c_str()) == 1;
		s.reshadeBeforeUi = GetPrivateProfileIntA("f4sr", "bReShadeBeforeUI", 0, ini.c_str()) != 0;
		s.reshadeDepth = GetPrivateProfileIntA("f4sr", "bReShadeDepth", 1, ini.c_str()) != 0;
		{
			char rb[32]{};
			GetPrivateProfileStringA("f4sr", "fReactiveScale", "1.0", rb, sizeof(rb), ini.c_str());
			s.reactiveScale = static_cast<float>(std::atof(rb));
		}
		s.colorSlot = GetPrivateProfileIntA("f4sr", "iColorSlot", 4, ini.c_str());
		s.mvSlot = GetPrivateProfileIntA("f4sr", "iMVSlot", 29, ini.c_str());
		s.depthSlot = GetPrivateProfileIntA("f4sr", "iDepthSlot", 2, ini.c_str());

		// MCM writes its values to Data\MCM\Settings\f4sr.ini, section
		// [Main] (the page's control ids are "<key>:Main"). When that
		// file has a key, it wins over the plugin ini defaults above.
		ApplyMcmSettings(s, true);
		s.pipeline = std::clamp(s.pipeline, 0, 1);
		s.enbBeforeUpscaling = s.pipeline == 1 && s.frameGenEngine == 1;
		F4SE::log::info("f4sr: render pipeline {} (setting {}, engine {}){}", s.enbBeforeUpscaling ? "UI at native resolution (Path B)" : "Classic (Path A)",
			s.pipeline, s.frameGenEngine == 1 ? "XeSS-FG" : "FSR-FG: Classic only", s.pipelineFellBack ? (s.pipeline == 0 ? ", UI Native failed in an earlier run" : ", UI Native chosen again after an earlier failure") : "");
		if (s.pipelineFellBack) {
			if (s.pipeline == 0) sr_status::Raise(sr_status::kAlertPipelinePrevious);
			else WriteMcmValue("bPipelineFellBack", "0");  // the player chose UI native again: try it
		}
		F4SE::log::info("f4sr: FSR-FG present path {}", s.fgPresentPath);
		F4SE::log::info("f4sr: diagnostics overlay={} fgCapture={} captures={} captureKey=0x{:02X} textureProbe={} postDepth={} dynResGuard={}",
			s.frameGenOverlay, s.frameGenCapture, s.diagnostics, s.captureKey, s.textureProbe, s.postDepth, s.dynResGuard);

		// Carry the pre-1.1 plugin-ini switch into the MCM toggle, so it shows what runs.
		if (s.asyncPresent && GetPrivateProfileIntA("Main", "bXeFGAsyncPresent", -1, McmIniPath().c_str()) == -1)
			WriteMcmValue("bXeFGAsyncPresent", "1");
		g_startup = s;
		s.reactiveScale = std::clamp(s.reactiveScale, 0.0f, 4.0f);
		F4SE::log::info("f4sr: reactive mask {} (scale {:.2f})", s.reactiveMask ? "on" : "off", s.reactiveScale);
		F4SE::log::info("f4sr settings: upscaling={} backend={} quality={} sharpness={} frameGen={} fgEngine={} xefgInterpolations={} lowLatency={} enbSubNative={} debugView={} enbPresent={} mvJitterCancellation={}",
			s.enableUpscaling, s.backend, s.qualityMode, s.sharpness, s.enableFrameGeneration, s.frameGenEngine, s.xefgInterpolations, s.lowLatency, s.enbSubNative, s.debugView, ENBPresent(), s.mvJitterCancellation);
	}

	Settings ReadMcmSettings()
	{
		auto s = GetSettings();
		ApplyMcmSettings(s, false);
		F4SE::log::info("f4sr: diagnostics overlay={} fgCapture={}", s.frameGenOverlay, s.frameGenCapture);
		F4SE::log::info("f4sr: MCM settings requested: upscaling={} backend={} quality={} sharpness={} frameGen={} xefgInterpolations={} lowLatency={} debugView={} forceReset={}",
			s.enableUpscaling, s.backend, s.qualityMode, s.sharpness, s.enableFrameGeneration, s.xefgInterpolations, s.lowLatency, s.debugView, s.forceReset);
		return s;
	}

	void UpdateRestartPending()
	{
		const std::string mcmIni = McmIniPath();
		auto mcm = [&](const char* key, int fallback) {
			return static_cast<int>(GetPrivateProfileIntA("Main", key, fallback, mcmIni.c_str()));
		};
		const Settings& start = g_startup;
		std::vector<std::string> items;
		// A UI Native failure writes iPipeline=0 itself; its alert explains that.
		const int pipeline = std::clamp(mcm("iPipeline", start.pipeline), 0, 1);
		if (pipeline != start.pipeline && mcm("bPipelineFellBack", 0) == 0)
			items.push_back(std::string("Pipeline: ") + (pipeline == 1 ? "UI Native" : "Classic"));
		const int engine = std::clamp(mcm("iFrameGenEngine", start.frameGenEngine), 0, 1);
		if (engine != start.frameGenEngine) items.push_back(std::string("Frame generation engine: ") + (engine == 1 ? "XeSS-FG" : "FSR-FG"));
		if (start.frameGenEngine == 1 && engine == 1 && GetSettings().enableFrameGeneration && xefg::GenerationNeedsRestart())
			items.push_back("Frame generation on");
		// The mask texture is created with the upscaler at startup.
		if (!start.reactiveMask && GetSettings().reactiveMask) items.push_back("Transparency Mask on");
		if (start.frameGenEngine == 1) {
			const bool async = mcm("bXeFGAsyncPresent", start.asyncPresent ? 1 : 0) != 0;
			if (async != start.asyncPresent) items.push_back(std::string("Async present ") + (async ? "on" : "off"));
		}
		std::string lines;
		for (const auto& item : items) lines += "\t^" + item + "\n";
		sr_status::SetRestartLines(std::move(lines));
	}

	std::uint64_t McmSettingsWriteTime()
	{
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (!GetFileAttributesExA(McmIniPath().c_str(), GetFileExInfoStandard, &data)) {
			return 0;
		}
		return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
		       data.ftLastWriteTime.dwLowDateTime;
	}
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "f4sr";
	a_info->version = 1;
	return !a_f4se->IsEditor();
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	f4sr::log_redact::Install();
	F4SE::AllocTrampoline(1024);
	F4SE::log::info("f4sr {}{}", f4sr::version::kName, f4sr::version::kExperimental ? " (experimental build)" : "");
	f4sr::LoadSettings();
	f4sr::environment_log::Write(a_f4se);
	f4sr::xefg::InstallSwapChainRedirect();
	if (f4sr::GetSettings().diagnostics) f4sr::shader_capture::Install();
	f4sr::LoadFfxRuntime();
	f4sr::InstallHooks();
	f4sr::menu_effects::Install();
	{
		HMODULE self{};
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&F4SEPlugin_Load), &self);
		f4sr::reshade_preui::Install(self);
	}
	if (!F4SE::GetMessagingInterface()->RegisterListener([](F4SE::MessagingInterface::Message* message) {
		if (message && message->type == F4SE::MessagingInterface::kPostPostLoad) {
			f4sr::xefg::ReclaimSwapChainRedirect();
			f4sr::InstallTextureProbe();
		}
	})) F4SE::log::error("f4sr: could not register startup texture probe listener");
	return true;
}
