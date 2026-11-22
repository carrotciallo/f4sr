#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "F4SE/F4SE.h"

namespace f4sr::xefg_unlock
{
	// In-memory multi-frame unlock for one exact build of libxess_fg.dll
	// (1.3.1.78, PE stamp 0x69CB0F4D, SizeOfImage 0x15ED000) and the paired
	// libxell.dll (1.3.2.10, stamp 0x6A561284, SizeOfImage 0x6A000). The DLLs
	// on disk are never modified; only the loaded image is patched.
	//
	// Everything is verified before anything is written. Any mismatch - other
	// build, different bytes at a site, a paced caller that no longer calls the
	// present thunk - leaves the module untouched and the session at stock 2X,
	// with the failing check logged.
	//
	// Sites (RVA, original -> patched):
	//   U1 0x20DA4F  jne rel32       -> jmp rel32; nop   per-frame count fallback
	//   U2 0x1A5DE4  je +9           -> jmp +6           multi-frame allowed gate
	//   U3 0x1A517D  mov ebx,3       -> mov ebx,N        default ceiling
	//   U4 0x1A45C2  mov [rdi+16Ch],1 -> ...,N           capability-failure override
	//   U5 0x20973B  mov eax,1       -> mov eax,N        reported maximum
	// Verify-only:
	//   P0 0x0025C0  present thunk: jmp 0x21F730 + 11 x int3 (16 bytes the
	//                pacing detour overwrites)
	//   P1 0x2202ED  return site of the burst-loop call to the thunk
	//   P2 0x220467  return site of the last-frame call to the thunk
	//   X1 libxell 0xD1C9  cmp ebx,3; jbe - XeLL accepts up to 3 generated frames
	inline constexpr std::uint32_t kFgStamp = 0x69CB0F4D;
	inline constexpr std::uint32_t kFgSize = 0x015ED000;
	inline constexpr std::uint32_t kXellStamp = 0x6A561284;
	inline constexpr std::uint32_t kXellSize = 0x0006A000;
	inline constexpr std::uint32_t kPresentThunk = 0x25C0;
	inline constexpr std::uint32_t kNativePresent = 0x21F730;
	inline constexpr std::uint32_t kPacedLoopReturn = 0x2202ED;
	inline constexpr std::uint32_t kPacedLastReturn = 0x220467;
	inline constexpr int kMaxInterpolations = 3;

	struct Result
	{
		bool unlocked{ false };      // U1-U5 written
		bool pacingSitesOk{ false }; // P0-P2 verified; the pacing detour may install
		int maxInterpolations{ 1 };  // ceiling to pass to XeFG init
		std::string failure;         // first failed check, empty when unlocked
	};

	inline std::string Hex(const std::uint8_t* p, std::size_t n)
	{
		std::string s;
		for (std::size_t i = 0; i < n; ++i) s += std::format("{}{:02X}", i ? " " : "", p[i]);
		return s;
	}

	inline bool Identity(HMODULE m, std::uint32_t stamp, std::uint32_t size, const char* name, std::string& why)
	{
		const auto* base = reinterpret_cast<const std::uint8_t*>(m);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) { why = std::format("{} DOS header", name); return false; }
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) { why = std::format("{} NT header", name); return false; }
		if (nt->FileHeader.TimeDateStamp != stamp) {
			why = std::format("{} build stamp 0x{:08X} (expected 0x{:08X})", name, nt->FileHeader.TimeDateStamp, stamp);
			return false;
		}
		if (nt->OptionalHeader.SizeOfImage != size) {
			why = std::format("{} SizeOfImage 0x{:X} (expected 0x{:X})", name, nt->OptionalHeader.SizeOfImage, size);
			return false;
		}
		return true;
	}

	inline bool Bytes(HMODULE m, std::uint32_t rva, const std::uint8_t* want, std::size_t n, const char* site, std::string& why)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(m) + rva;
		if (std::memcmp(p, want, n) == 0) return true;
		why = std::format("{} at RVA 0x{:X}: found [{}] expected [{}]", site, rva, Hex(p, n), Hex(want, n));
		return false;
	}

	// The 5 bytes before a return address must be `call rel32` to the thunk.
	inline bool CallsThunk(HMODULE m, std::uint32_t retRva, const char* site, std::string& why)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(m) + retRva - 5;
		std::int32_t rel{};
		std::memcpy(&rel, p + 1, 4);
		const auto target = static_cast<std::uint32_t>(static_cast<std::int64_t>(retRva) + rel);
		if (p[0] == 0xE8 && target == kPresentThunk) return true;
		why = std::format("{} at RVA 0x{:X}: found [{}], not call 0x{:X}", site, retRva - 5, Hex(p, 5), kPresentThunk);
		return false;
	}

	inline bool Write(HMODULE m, std::uint32_t rva, const std::uint8_t* bytes, std::size_t n)
	{
		auto* p = reinterpret_cast<std::uint8_t*>(m) + rva;
		DWORD old{};
		if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
		std::memcpy(p, bytes, n);
		DWORD tmp{};
		VirtualProtect(p, n, old, &tmp);
		FlushInstructionCache(GetCurrentProcess(), p, n);
		return true;
	}

	// fg / xell: the loaded modules. n: generated-frame ceiling (2..3).
	inline Result Apply(HMODULE fg, HMODULE xell, int n)
	{
		Result r;
		n = n < 2 ? 2 : (n > kMaxInterpolations ? kMaxInterpolations : n);
		const auto N = static_cast<std::uint8_t>(n);

		struct Site { const char* name; std::uint32_t rva; std::vector<std::uint8_t> before, after; };
		const Site sites[]{
			{ "U1/frame-count-fallback", 0x20DA4F, { 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00 }, { 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 } },
			{ "U2/multi-frame-gate", 0x1A5DE4, { 0x74, 0x09 }, { 0xEB, 0x06 } },
			{ "U3/default-ceiling", 0x1A517D, { 0xBB, 0x03, 0x00, 0x00, 0x00 }, { 0xBB, N, 0x00, 0x00, 0x00 } },
			{ "U4/override-clamp", 0x1A45C2, { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 },
				{ 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, N, 0x00, 0x00, 0x00 } },
			{ "U5/reported-maximum", 0x20973B, { 0xB8, 0x01, 0x00, 0x00, 0x00 }, { 0xB8, N, 0x00, 0x00, 0x00 } },
		};
		static constexpr std::uint8_t thunk[16]{ 0xE9, 0x6B, 0xD1, 0x21, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
		static constexpr std::uint8_t xellCap[5]{ 0x83, 0xFB, 0x03, 0x76, 0x07 };

		std::string why;
		bool ok = fg && xell;
		if (!ok) why = !fg ? "libxess_fg.dll not loaded" : "libxell.dll not loaded";
		ok = ok && Identity(fg, kFgStamp, kFgSize, "libxess_fg.dll", why);
		ok = ok && Identity(xell, kXellStamp, kXellSize, "libxell.dll", why);
		for (const auto& s : sites) ok = ok && Bytes(fg, s.rva, s.before.data(), s.before.size(), s.name, why);
		ok = ok && Bytes(fg, kPresentThunk, thunk, sizeof(thunk), "P0/present-thunk", why);
		ok = ok && CallsThunk(fg, kPacedLoopReturn, "P1/burst-loop-call", why);
		ok = ok && CallsThunk(fg, kPacedLastReturn, "P2/last-frame-call", why);
		ok = ok && Bytes(xell, 0xD1C9, xellCap, sizeof(xellCap), "X1/xell-frame-cap", why);
		if (!ok) {
			r.failure = why;
			F4SE::log::warn("f4sr: XeFG unlock check failed: {} - nothing patched, staying at 2X", why);
			return r;
		}
		r.pacingSitesOk = true;

		for (const auto& s : sites) {
			if (!Write(fg, s.rva, s.after.data(), s.after.size())) {
				// A partial write cannot be trusted; put back what was written.
				for (const auto& u : sites) {
					if (&u == &s) break;
					Write(fg, u.rva, u.before.data(), u.before.size());
				}
				r.failure = std::format("{} VirtualProtect failed ({})", s.name, GetLastError());
				r.pacingSitesOk = false;
				F4SE::log::warn("f4sr: XeFG unlock write failed: {} - reverted, staying at 2X", r.failure);
				return r;
			}
		}
		r.unlocked = true;
		r.maxInterpolations = n;
		F4SE::log::info("f4sr: XeFG unlock applied in memory: libxess_fg 0x{:08X}/0x{:X}, libxell 0x{:08X}/0x{:X}, U1-U5 patched, ceiling {} generated ({}X), pacing sites P0-P2 verified",
			kFgStamp, kFgSize, kXellStamp, kXellSize, n, n + 1);
		return r;
	}
}
