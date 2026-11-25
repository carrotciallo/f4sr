#include "environment_log.h"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <windows.h>
#include <dxgi1_6.h>
#include <shlobj.h>
#include <wrl/client.h>

#include "hfpf_display.h"
#include "settings.h"
#include "version.h"

namespace
{
	using Microsoft::WRL::ComPtr;

	std::string Narrow(const wchar_t* text)
	{
		if (!text || !*text) return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (n <= 1) return {};
		std::string out(static_cast<std::size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
		return out;
	}

	std::string RegString(HKEY root, const char* key, const char* value)
	{
		char buffer[256]{};
		DWORD size = sizeof(buffer);
		if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
		return buffer;
	}

	DWORD RegDword(HKEY root, const char* key, const char* value)
	{
		DWORD data{}, size = sizeof(data);
		return RegGetValueA(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS ? data : 0;
	}

	std::string Trim(std::string s)
	{
		const auto first = s.find_first_not_of(" \t");
		const auto last = s.find_last_not_of(" \t");
		return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
	}

	std::string OperatingSystem()
	{
		const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		std::string os;
		using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
		OSVERSIONINFOW v{ sizeof(v) };
		if (auto* get = ntdll ? reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr; get && get(&v) == 0) {
			const char* name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11" :
							   v.dwMajorVersion == 10                             ? "Windows 10" :
																					"Windows";
			os = std::format("{} {}.{}.{}", name, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
			constexpr auto key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
			if (const DWORD ubr = RegDword(HKEY_LOCAL_MACHINE, key, "UBR")) os += std::format(".{}", ubr);
			if (const auto display = RegString(HKEY_LOCAL_MACHINE, key, "DisplayVersion"); !display.empty()) os += " (" + display + ")";
		}
		// Wine and Proton report a Windows version; name the real host too.
		using WineVersion_t = const char*(CDECL*)();
		using WineHost_t = void(CDECL*)(const char**, const char**);
		if (auto* wine = ntdll ? reinterpret_cast<WineVersion_t>(GetProcAddress(ntdll, "wine_get_version")) : nullptr) {
			os += std::format(" under Wine {}", wine());
			if (auto* host = reinterpret_cast<WineHost_t>(GetProcAddress(ntdll, "wine_get_host_version"))) {
				const char* sys{};
				const char* release{};
				host(&sys, &release);
				if (sys) os += std::format(" on {} {}", sys, release ? release : "");
			}
		}
		return os.empty() ? "unknown" : os;
	}

	std::string Processor()
	{
		auto name = Trim(RegString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString"));
		if (name.empty()) name = "unknown CPU";
		MEMORYSTATUSEX memory{ sizeof(memory) };
		GlobalMemoryStatusEx(&memory);
		return std::format("{}, {} logical processors, {:.1f} GB RAM", name, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
			static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
	}

	const char* Vendor(UINT id)
	{
		switch (id) {
		case 0x1002: return "AMD";
		case 0x10DE: return "NVIDIA";
		case 0x8086: return "Intel";
		case 0x1414: return "Microsoft";
		default: return "other";
		}
	}

	std::string DriverVersion(IDXGIAdapter1* adapter, UINT vendor)
	{
		LARGE_INTEGER umd{};
		if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) return "unknown";
		const auto q = static_cast<std::uint64_t>(umd.QuadPart);
		const unsigned a = static_cast<unsigned>(q >> 48), b = static_cast<unsigned>((q >> 32) & 0xFFFF);
		const unsigned c = static_cast<unsigned>((q >> 16) & 0xFFFF), d = static_cast<unsigned>(q & 0xFFFF);
		std::string out = std::format("{}.{}.{}.{}", a, b, c, d);
		// NVIDIA's public number is the last five digits (31.0.15.5222 -> 552.22).
		if (vendor == 0x10DE) {
			const unsigned n = (c % 10) * 10000 + d;
			out += std::format(" (NVIDIA {}.{:02})", n / 100, n % 100);
		}
		return out;
	}

	void Displays(IDXGIAdapter1* adapter)
	{
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIOutput> output;
			if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND || !output) break;
			DXGI_OUTPUT_DESC desc{};
			output->GetDesc(&desc);
			DEVMODEW mode{};
			mode.dmSize = sizeof(mode);
			const bool hasMode = EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != 0;
			MONITORINFO mi{ sizeof(mi) };
			GetMonitorInfoW(desc.Monitor, &mi);
			std::string hdr = "unknown";
			ComPtr<IDXGIOutput6> output6;
			if (SUCCEEDED(output.As(&output6))) {
				DXGI_OUTPUT_DESC1 d1{};
				if (SUCCEEDED(output6->GetDesc1(&d1)))
					hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? std::format("on, {} bpc", d1.BitsPerColor) :
																					   std::format("off, {} bpc", d1.BitsPerColor);
			}
			const LONG w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
			const LONG h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
			F4SE::log::info("f4sr: env:   display {}: {}x{} at {} Hz{}, HDR {}", i, w, h, hasMode ? mode.dmDisplayFrequency : 0,
				(mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "", hdr);
		}
	}

	void Adapters()
	{
		// The system dxgi.dll, not a ReShade or ENB proxy beside the game.
		wchar_t system[MAX_PATH]{};
		GetSystemDirectoryW(system, MAX_PATH);
		const HMODULE dxgi = LoadLibraryW((std::wstring(system) + L"\\dxgi.dll").c_str());
		using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
		auto* create = dxgi ? reinterpret_cast<CreateFactory_t>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
		ComPtr<IDXGIFactory1> factory;
		if (!create || FAILED(create(IID_PPV_ARGS(&factory)))) {
			F4SE::log::info("f4sr: env: GPU: DXGI factory unavailable");
			return;
		}
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;
			if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
			F4SE::log::info("f4sr: env: GPU {}: {} ({} {:04X}:{:04X}), {} MB VRAM, driver {}", i, Narrow(desc.Description), Vendor(desc.VendorId),
				desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory / (1024 * 1024), DriverVersion(adapter.Get(), desc.VendorId));
			Displays(adapter.Get());
		}
	}

	std::string GameDirectory()
	{
		char exe[MAX_PATH]{};
		const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
		std::string path(exe, n);
		if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos) path.resize(slash + 1);
		return path;
	}

	// ProductName / FileVersion of a DLL beside the game, or "" when absent.
	std::string FileIdentity(const std::string& path)
	{
		if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
		using Size_t = DWORD(WINAPI*)(LPCSTR, LPDWORD);
		using Info_t = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
		using Query_t = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
		const HMODULE version = LoadLibraryW(L"version.dll");
		auto* size = version ? reinterpret_cast<Size_t>(GetProcAddress(version, "GetFileVersionInfoSizeA")) : nullptr;
		auto* info = version ? reinterpret_cast<Info_t>(GetProcAddress(version, "GetFileVersionInfoA")) : nullptr;
		auto* query = version ? reinterpret_cast<Query_t>(GetProcAddress(version, "VerQueryValueA")) : nullptr;
		if (!size || !info || !query) return "present";
		DWORD ignored{};
		const DWORD bytes = size(path.c_str(), &ignored);
		if (!bytes) return "present (no version info)";
		std::vector<char> data(bytes);
		if (!info(path.c_str(), 0, bytes, data.data())) return "present (no version info)";
		struct Translation { WORD language, codePage; }* translation{};
		UINT length{};
		std::string product, fileVersion;
		if (query(data.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &length) && length >= sizeof(Translation)) {
			const auto get = [&](const char* name) {
				char* value{};
				UINT n{};
				const auto key = std::format("\\StringFileInfo\\{:04x}{:04x}\\{}", translation->language, translation->codePage, name);
				return query(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &n) && value ? std::string(value) : std::string();
			};
			product = Trim(get("ProductName"));
			fileVersion = Trim(get("FileVersion"));
		}
		if (product.empty() && fileVersion.empty()) return "present";
		return Trim(product + " " + fileVersion);
	}

	void RelatedMods()
	{
		const auto dir = GameDirectory();
		std::string line;
		for (const char* name : { "d3d11.dll", "dxgi.dll", "d3d12.dll", "dinput8.dll" }) {
			const auto id = FileIdentity(dir + name);
			line += std::format(" {}={}", name, id.empty() ? "-" : "[" + id + "]");
		}
		F4SE::log::info("f4sr: env: game folder proxies:{}", line);
		const bool enb = GetFileAttributesA((dir + "enblocal.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
		const bool reshade = GetFileAttributesA((dir + "ReShade.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
		F4SE::log::info("f4sr: env: ENB {} (enblocal.ini), ReShade {} (ReShade.ini)", enb ? "yes" : "no", reshade ? "yes" : "no");
		F4SE::log::info("f4sr: env: {}", f4sr::hfpf::Read().summary);
	}

	void GamePrefs()
	{
		PWSTR documents{};
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) || !documents) return;
		const std::wstring path = std::wstring(documents) + L"\\My Games\\Fallout4\\Fallout4Prefs.ini";
		CoTaskMemFree(documents);
		const auto read = [&](const wchar_t* key) {
			wchar_t value[32]{};
			GetPrivateProfileStringW(L"Display", key, L"-", value, 32, path.c_str());
			return Narrow(value);
		};
		F4SE::log::info("f4sr: env: Fallout4Prefs.ini [Display]: bFull Screen={} bBorderless={} iSize W={} iSize H={}", read(L"bFull Screen"),
			read(L"bBorderless"), read(L"iSize W"), read(L"iSize H"));
	}

	void LogSettings()
	{
		const auto& s = f4sr::GetSettings();
		static constexpr const char* kBackend[]{ "FSR 3.1", "FSR 4.1", "XeSS" };
		static constexpr const char* kQuality[]{ "Native AA", "Quality", "Balanced", "Performance" };
		const auto pick = [](const char* const* names, int count, int i) { return i >= 0 && i < count ? names[i] : "?"; };
		F4SE::log::info("f4sr: env: settings: pipeline {}, upscaling {} ({}, {}), frame generation {} ({}), ENB below native {}, ReShade before UI {}",
			s.pipeline == 1 ? "UI Native" : "Classic", s.enableUpscaling ? "on" : "off", pick(kBackend, 3, s.backend),
			pick(kQuality, 4, s.qualityMode), s.enableFrameGeneration ? "on" : "off", s.frameGenEngine == 1 ? "XeSS-FG" : "FSR-FG",
			s.enbSubNative ? "on" : "off", s.reshadeBeforeUi ? "on" : "off");
	}
}

namespace f4sr::environment_log
{
	void Write(const F4SE::LoadInterface* f4se)
	{
		F4SE::log::info("f4sr: env: f4sr {} built {} {}; game {}; F4SE {}", version::kName, __DATE__, __TIME__,
			f4se ? f4se->RuntimeVersion().string() : std::string("?"), f4se ? f4se->F4SEVersion().string() : std::string("?"));
		if (!f4sr::GetSettings().logSystemInfo) {
			F4SE::log::info("f4sr: env: system info not logged (MCM Diagnostics > Log System Info is off)");
			return;
		}
		F4SE::log::info("f4sr: env: OS: {}", OperatingSystem());
		F4SE::log::info("f4sr: env: CPU: {}", Processor());
		Adapters();
		GamePrefs();
		RelatedMods();
		LogSettings();
	}
}
