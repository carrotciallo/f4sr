#include "scaleform_scale.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <windows.h>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include "xefg_presenter.h"

namespace
{
	using RE::Scaleform::GFx::Movie;
	using RE::Scaleform::GFx::Viewport;

	// Scaleform event layout as seen in this build: { EventType Type;
	// 4 bytes (0); float x, y; float ScrollDelta; unsigned Button; unsigned
	// MouseIndex; } for mouse events.
	struct EventHeader
	{
		std::uint32_t type;
		std::uint32_t size;
	};
	constexpr std::uint32_t kMouseMove = 1, kMouseWheel = 4;
	constexpr std::uint32_t kMouseEventSize = 0x1C;

	// Movie vtable slots (CommonLibF4 GFx_Player.h).
	constexpr int kSetViewport = 0x0C;
	constexpr int kGetViewport = 0x0D;
	constexpr int kHandleEvent = 0x23;
	constexpr int kGetMouseState = 0x24;
	constexpr int kNotifyMouseState = 0x25;
	constexpr int kHitTest = 0x26;

	using SetViewport_t = void (*)(Movie*, const Viewport&);
	using GetViewport_t = void (*)(const Movie*, Viewport*);
	using HandleEvent_t = std::uint32_t (*)(Movie*, const void*);
	using GetMouseState_t = void (*)(Movie*, std::uint32_t, float*, float*, std::uint32_t*);
	using NotifyMouseState_t = void (*)(Movie*, float, float, std::uint32_t, std::uint32_t);
	using HitTest_t = bool (*)(Movie*, float, float, std::uint32_t, std::uint32_t);

	SetViewport_t g_setViewport{};
	GetViewport_t g_getViewport{};
	HandleEvent_t g_handleEvent{};
	GetMouseState_t g_getMouseState{};
	NotifyMouseState_t g_notifyMouseState{};
	HitTest_t g_hitTest{};

	std::mutex g_lock;
	std::unordered_map<const Movie*, Viewport> g_scaled;  // movie -> viewport the game set
	std::uint64_t g_scaledCount{}, g_events{};

	bool Scale(float& sx, float& sy)
	{
		UINT rw{}, rh{}, dw{}, dh{};
		if (!f4sr::xefg::PathBSizes(rw, rh, dw, dh) || !rw || !rh || (rw == dw && rh == dh)) return false;
		sx = static_cast<float>(dw) / static_cast<float>(rw);
		sy = static_cast<float>(dh) / static_cast<float>(rh);
		return true;
	}

	bool IsScaled(const Movie* movie)
	{
		std::lock_guard lock{ g_lock };
		return g_scaled.contains(movie);
	}

	void SetViewportHook(Movie* movie, const Viewport& v)
	{
		float sx{}, sy{};
		UINT rw{}, rh{}, dw{}, dh{};
		const bool sizes = f4sr::xefg::PathBSizes(rw, rh, dw, dh) && Scale(sx, sy);
		const bool screen = sizes && !v.flags.any(RE::Scaleform::Render::Viewport::Flag::kIsRenderTexture) &&
			v.bufferWidth == static_cast<std::int32_t>(rw) && v.bufferHeight == static_cast<std::int32_t>(rh);
		if (!screen) {
			{
				std::lock_guard lock{ g_lock };
				g_scaled.erase(movie);
			}
			return g_setViewport(movie, v);
		}
		Viewport s = v;
		s.bufferWidth = static_cast<std::int32_t>(dw);
		s.bufferHeight = static_cast<std::int32_t>(dh);
		s.left = static_cast<std::int32_t>(v.left * sx + 0.5f);
		s.top = static_cast<std::int32_t>(v.top * sy + 0.5f);
		s.width = static_cast<std::int32_t>(v.width * sx + 0.5f);
		s.height = static_cast<std::int32_t>(v.height * sy + 0.5f);
		s.scissorLeft = static_cast<std::int32_t>(v.scissorLeft * sx + 0.5f);
		s.scissorTop = static_cast<std::int32_t>(v.scissorTop * sy + 0.5f);
		s.scissorWidth = static_cast<std::int32_t>(v.scissorWidth * sx + 0.5f);
		s.scissorHeight = static_cast<std::int32_t>(v.scissorHeight * sy + 0.5f);
		{
			std::lock_guard lock{ g_lock };
			g_scaled[movie] = v;
			++g_scaledCount;
		}
		if (g_scaledCount <= 12)
			F4SE::log::info("f4sr: UI at display size: movie {:p} viewport {}x{} ({},{} {}x{}) -> {}x{} ({},{} {}x{})", static_cast<void*>(movie),
				v.bufferWidth, v.bufferHeight, v.left, v.top, v.width, v.height, s.bufferWidth, s.bufferHeight, s.left, s.top, s.width, s.height);
		g_setViewport(movie, s);
	}

	void GetViewportHook(const Movie* movie, Viewport* out)
	{
		g_getViewport(movie, out);
		if (!out) return;
		std::lock_guard lock{ g_lock };
		if (const auto it = g_scaled.find(movie); it != g_scaled.end()) *out = it->second;
	}

	std::uint32_t HandleEventHook(Movie* movie, const void* event)
	{
		const auto* h = static_cast<const EventHeader*>(event);
		float sx{}, sy{};
		if (!h || h->type < kMouseMove || h->type > kMouseWheel || !IsScaled(movie) || !Scale(sx, sy))
			return g_handleEvent(movie, event);
		if (++g_events <= 3)
			F4SE::log::info("f4sr: UI at display size: mouse event type {} size {:#x} at ({:.0f},{:.0f})", h->type, h->size,
				reinterpret_cast<const float*>(h + 1)[0], reinterpret_cast<const float*>(h + 1)[1]);
		// The second header word is not a class size in this build (0); the
		// mouse position sits at +8 (log above).
		alignas(8) std::uint8_t copy[kMouseEventSize];
		std::memcpy(copy, event, kMouseEventSize);
		auto* xy = reinterpret_cast<float*>(copy + sizeof(EventHeader));
		xy[0] *= sx;
		xy[1] *= sy;
		return g_handleEvent(movie, copy);
	}

	void GetMouseStateHook(Movie* movie, std::uint32_t index, float* x, float* y, std::uint32_t* buttons)
	{
		g_getMouseState(movie, index, x, y, buttons);
		float sx{}, sy{};
		if (!IsScaled(movie) || !Scale(sx, sy)) return;
		if (x) *x /= sx;
		if (y) *y /= sy;
	}

	void NotifyMouseStateHook(Movie* movie, float x, float y, std::uint32_t buttons, std::uint32_t index)
	{
		float sx{}, sy{};
		if (IsScaled(movie) && Scale(sx, sy)) {
			x *= sx;
			y *= sy;
		}
		g_notifyMouseState(movie, x, y, buttons, index);
	}

	bool HitTestHook(Movie* movie, float x, float y, std::uint32_t cond, std::uint32_t controller)
	{
		float sx{}, sy{};
		if (IsScaled(movie) && Scale(sx, sy)) {
			x *= sx;
			y *= sy;
		}
		return g_hitTest(movie, x, y, cond, controller);
	}

	template <class T>
	bool Patch(void** table, int slot, T hook, T& original)
	{
		DWORD old{};
		if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
		original = reinterpret_cast<T>(table[slot]);
		table[slot] = reinterpret_cast<void*>(hook);
		VirtualProtect(&table[slot], sizeof(void*), old, &old);
		return true;
	}

	bool PatchVtable(void** table);

	// BSScaleformManager::InitMovieViewport (REL 206895) sets each loaded
	// movie's viewport. Its callers are redirected here: the first movie
	// yields the MovieImpl vtable (the class carries no RTTI in this build).
	using InitMovieViewport_t = void (*)(void*, Movie&, float, float);
	InitMovieViewport_t g_initViewport{};
	bool g_patched{};

	void InitMovieViewportThunk(void* manager, Movie& movie, float wRatio, float hRatio)
	{
		g_initViewport(manager, movie, wRatio, hRatio);
		if (!g_patched) {
			g_patched = true;
			if (!PatchVtable(*reinterpret_cast<void***>(&movie))) return;
		}
		// Movies whose viewport was set before the vtable patch.
		if (!IsScaled(&movie)) {
			Viewport v{};
			g_getViewport(&movie, &v);
			SetViewportHook(&movie, v);
		}
	}

	bool PatchVtable(void** table)
	{
		const bool ok = Patch(table, kSetViewport, &SetViewportHook, g_setViewport) &&
			Patch(table, kGetViewport, &GetViewportHook, g_getViewport) &&
			Patch(table, kHandleEvent, &HandleEventHook, g_handleEvent) &&
			Patch(table, kGetMouseState, &GetMouseStateHook, g_getMouseState) &&
			Patch(table, kNotifyMouseState, &NotifyMouseStateHook, g_notifyMouseState) &&
			Patch(table, kHitTest, &HitTestHook, g_hitTest);
		F4SE::log::info("f4sr: UI at display size: Scaleform movie hooks {} (vtable {:p})", ok ? "installed" : "FAILED", static_cast<void*>(table));
		return ok;
	}
}

namespace f4sr::scaleform_scale
{
	bool Install()
	{
		static bool done = false;
		if (done) return true;
		done = true;
		const auto target = REL::ID(206895).address();
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* sec = IMAGE_FIRST_SECTION(nt);
		std::uintptr_t sites[16]{};
		int count = 0;
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
			const auto start = base + sec[i].VirtualAddress;
			const std::size_t n = sec[i].Misc.VirtualSize;
			for (std::size_t o = 0; o + 5 <= n; ++o) {
				const auto* p = reinterpret_cast<const std::uint8_t*>(start + o);
				if (p[0] != 0xE8) continue;
				std::int32_t rel;
				std::memcpy(&rel, p + 1, 4);
				if (start + o + 5 + rel == target && count < 16) sites[count++] = start + o;
			}
		}
		auto& trampoline = F4SE::GetTrampoline();
		if (count == 0 || trampoline.free_size() < static_cast<std::size_t>(count) * 14) {
			F4SE::log::warn("f4sr: UI at display size: {} InitMovieViewport call sites, trampoline {} bytes free; the UI stays at the render size",
				count, trampoline.free_size());
			return false;
		}
		for (int i = 0; i < count; ++i) {
			const auto original = trampoline.write_call<5>(sites[i], &InitMovieViewportThunk);
			if (!g_initViewport) g_initViewport = reinterpret_cast<InitMovieViewport_t>(original);
		}
		F4SE::log::info("f4sr: UI at display size: {} InitMovieViewport call sites redirected", count);
		return true;
	}
}
