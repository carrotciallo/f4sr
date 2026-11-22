#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

#include "F4SE/F4SE.h"

namespace f4sr::dynres_guard
{
	// The pipeline drives the render-target manager's dynamic-resolution
	// ratio at frame start and resets it at the seam. Something else in the
	// process occasionally rewrites the ratio in between (observed: 0.528
	// instead of 0.5, roughly every 30 frames, with and without ENB), and that
	// frame is rendered at a scale the upscaler does not know about: a single
	// zoomed "flash" frame. The writer lives in the SteamStub-encrypted game
	// code, so it is caught at run time with a hardware write watchpoint on the
	// two ratio floats of the rendering thread. While armed, a foreign write is
	// undone immediately and its code address is recorded for the log.
	inline constexpr unsigned kFrames = 6;
	inline constexpr unsigned kRawSlots = 96;

	struct Hit
	{
		std::uintptr_t rip{};
		std::uintptr_t stack[kFrames]{};
		std::uintptr_t raw[kRawSlots]{}; // stack words at the hit, for return-address scanning
		float value{};
	};

	struct State
	{
		float* w{};
		float* h{};
		std::atomic<bool> armed{};
		float expectW{}, expectH{};
		std::atomic<std::uint32_t> foreign{}, restored{};
		Hit last{};
		std::atomic<bool> lastPending{};
		DWORD thread{};
		PVOID handler{};
		bool installed{}, failed{};
		std::uintptr_t selfLo{}, selfHi{};
	};
	inline State g;

	inline LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
	{
		if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
		auto* c = info->ContextRecord;
		// Wine does not report DR6 in the exception context, so a write hit
		// is recognised as a single-step trap on the watched thread that is
		// not a trap-flag step.
		const bool hit = (c->Dr6 & 0x3) || (g.installed && GetCurrentThreadId() == g.thread && !(c->EFlags & 0x100));
		if (!hit) return EXCEPTION_CONTINUE_SEARCH;
		c->Dr6 = 0;
		const auto rip = static_cast<std::uintptr_t>(c->Rip);
		if (rip >= g.selfLo && rip < g.selfHi) return EXCEPTION_CONTINUE_EXECUTION;
		if (!g.armed.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_EXECUTION;
		const float w = *g.w, h = *g.h;
		if (w == g.expectW && h == g.expectH) return EXCEPTION_CONTINUE_EXECUTION;
		g.foreign.fetch_add(1, std::memory_order_relaxed);
		if (!g.lastPending.load(std::memory_order_relaxed)) {
			Hit hit{};
			hit.rip = rip;
			hit.value = w != g.expectW ? w : h;
			CONTEXT walk = *c;
			for (unsigned i = 0; i < kFrames; ++i) {
				DWORD64 base{};
				auto* fn = RtlLookupFunctionEntry(walk.Rip, &base, nullptr);
				if (!fn) break;
				PVOID data{}; DWORD64 frame{};
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, walk.Rip, fn, &walk, &data, &frame, nullptr);
				if (!walk.Rip) break;
				hit.stack[i] = static_cast<std::uintptr_t>(walk.Rip);
			}
			// Unwinding the packed executable fails under Wine; keep raw
			// stack words so return addresses can be found by scanning.
			std::memcpy(hit.raw, reinterpret_cast<const void*>(c->Rsp), sizeof(hit.raw));
			g.last = hit;
			g.lastPending.store(true, std::memory_order_release);
		}
		*g.w = g.expectW;
		*g.h = g.expectH;
		g.restored.fetch_add(1, std::memory_order_relaxed);
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	struct Target { HANDLE thread; float* w; float* h; bool ok; };

	inline DWORD WINAPI SetWatch(LPVOID p)
	{
		auto* t = static_cast<Target*>(p);
		if (SuspendThread(t->thread) == static_cast<DWORD>(-1)) return 0;
		CONTEXT c{};
		c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		if (GetThreadContext(t->thread, &c)) {
			// Leave other users of DR2/DR3 alone; DR0/DR1 must be free.
			if (!(c.Dr7 & 0xF)) {
				c.Dr0 = reinterpret_cast<DWORD64>(t->w);
				c.Dr1 = reinterpret_cast<DWORD64>(t->h);
				// L0,L1 enabled; RW=01 (write), LEN=11 (4 bytes) for both.
				c.Dr7 = (c.Dr7 & ~0xFF000Full) | 0x5ull | (0xDull << 16) | (0xDull << 20);
				c.Dr6 = 0;
				t->ok = SetThreadContext(t->thread, &c) != FALSE;
			}
		}
		ResumeThread(t->thread);
		return 0;
	}

	// Call on the rendering thread. Idempotent.
	inline void Install(float* w, float* h)
	{
		if (g.installed || g.failed) return;
		g.failed = true;
		HMODULE self{};
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&OnException), &self)) {
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<std::byte*>(self) + dos->e_lfanew);
			g.selfLo = reinterpret_cast<std::uintptr_t>(self);
			g.selfHi = g.selfLo + nt->OptionalHeader.SizeOfImage;
		}
		g.w = w; g.h = h;
		g.handler = AddVectoredExceptionHandler(1, &OnException);
		if (!g.handler) { F4SE::log::warn("f4sr: dynres guard: no exception handler"); return; }
		HANDLE me{};
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &me,
				THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0)) {
			RemoveVectoredExceptionHandler(g.handler); g.handler = nullptr; return;
		}
		g.thread = GetCurrentThreadId();
		g.installed = true;
		Target t{ me, w, h, false };
		HANDLE helper = CreateThread(nullptr, 0, &SetWatch, &t, 0, nullptr);
		if (helper) { WaitForSingleObject(helper, 2000); CloseHandle(helper); }
		CloseHandle(me);
		if (!t.ok) {
			g.installed = false;
			RemoveVectoredExceptionHandler(g.handler); g.handler = nullptr;
			F4SE::log::warn("f4sr: dynres guard: could not set the write watchpoint (debug registers busy or refused)");
			return;
		}
		g.failed = false;
		F4SE::log::info("f4sr: dynres guard watching {:p}/{:p} on thread {}", static_cast<void*>(w), static_cast<void*>(h), g.thread);
	}

	inline void Arm(float w, float h)
	{
		g.expectW = w; g.expectH = h;
		g.armed.store(g.installed, std::memory_order_release);
	}

	inline void Disarm() { g.armed.store(false, std::memory_order_release); }

	inline void Describe(std::uintptr_t a, char* out, size_t n)
	{
		HMODULE m{};
		if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a), &m)) {
			char path[MAX_PATH]{};
			GetModuleFileNameA(m, path, MAX_PATH);
			const char* name = path;
			for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
			snprintf(out, n, "%s+0x%llx", name, static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(m)));
		} else {
			snprintf(out, n, "0x%llx", static_cast<unsigned long long>(a));
		}
	}

	inline void HexLines(const char* tag, std::uintptr_t from, std::size_t bytes, std::uintptr_t module)
	{
		MEMORY_BASIC_INFORMATION mi{};
		if (!VirtualQuery(reinterpret_cast<void*>(from), &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
			(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
			reinterpret_cast<std::uintptr_t>(mi.BaseAddress) + mi.RegionSize < from + bytes) return;
		const auto* b = reinterpret_cast<const unsigned char*>(from);
		for (std::size_t o = 0; o < bytes; o += 32) {
			std::string line;
			for (std::size_t i = o; i < o + 32 && i < bytes; ++i) line += std::format("{:02x}", b[i]);
			F4SE::log::info("f4sr: dynres code {} +{:#x} {}", tag, from + o - module, line);
		}
	}

	// Once per session, on the first undone write: the writing function's
	// code and the stack words that point into the executable (candidate
	// return addresses, each with the bytes leading up to it), so the
	// writer can be understood offline and replaced by a direct patch.
	inline void DumpOnce(const Hit& hit)
	{
		static bool done{};
		if (done) return;
		done = true;
		const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
		const auto end = exe + nt->OptionalHeader.SizeOfImage;
		if (hit.rip < exe || hit.rip >= end) return;
		HexLines("writer", hit.rip - 0x180, 0x280, exe);
		unsigned shown = 0;
		for (unsigned i = 0; i < kRawSlots && shown < 8; ++i) {
			const auto a = hit.raw[i];
			if (a <= exe + 0x1000 || a >= end) continue;
			MEMORY_BASIC_INFORMATION mi{};
			if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) ||
				!(mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
			F4SE::log::info("f4sr: dynres code stack[{}] = exe+{:#x}", i, a - exe);
			HexLines(std::format("ret{}", shown).c_str(), a - 0x40, 0x40, exe);
			++shown;
		}
	}

	// Call outside the watched window (e.g. at frame start) to log new hits.
	inline void Report()
	{
		if (!g.lastPending.load(std::memory_order_acquire)) return;
		static std::uint32_t logged{};
		const Hit hit = g.last;
		const auto n = g.foreign.load();
		if (++logged <= 20 || logged % 100 == 0) {
			char at[160]{}, s[kFrames][160]{};
			Describe(hit.rip, at, sizeof(at));
			for (unsigned i = 0; i < kFrames; ++i) Describe(hit.stack[i], s[i], sizeof(s[i]));
			F4SE::log::warn("f4sr: dynres guard undid a write of {:.3f} at {} (total {}) via {} <- {} <- {} <- {} <- {} <- {}",
				hit.value, at, n, s[0], s[1], s[2], s[3], s[4], s[5]);
		}
		DumpOnce(hit);
		g.lastPending.store(false, std::memory_order_release);
	}
}
