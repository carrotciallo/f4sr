#include "texture_probe.h"
#include "F4SE/F4SE.h"
#include "settings.h"
#include <d3d11.h>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>

namespace
{
	using CreateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
	CreateFn originals[4]{};
	HANDLE journal = INVALID_HANDLE_VALUE;
	SRWLOCK journalLock = SRWLOCK_INIT;
	std::atomic<std::uint64_t> sequence{0};
	constexpr std::uint64_t capacity = 8192;
	// Fixed little-endian journal: 8192 rotating 128-byte records, no header.
	// phase=0 means entered (the original Buffout handler may terminate).
	struct Record {
		std::uint64_t sequence, tick, callId;
		std::uint32_t site, thread, phase, result;
		D3D11_TEXTURE2D_DESC desc;
		std::uint32_t pitch, slicePitch, flags;
		std::uint64_t initialData;
		std::uint64_t reserved[3];
	};
	static_assert(sizeof(Record) == 128);

	bool Readable(std::uintptr_t address, std::size_t bytes)
	{
		MEMORY_BASIC_INFORMATION info{};
		if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
			info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
		const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
		return address <= end && bytes <= end - address;
	}

	void WriteRecord(const Record& record)
	{
		if (journal == INVALID_HANDLE_VALUE) return;
		AcquireSRWLockExclusive(&journalLock);
		LARGE_INTEGER offset{};
		offset.QuadPart = static_cast<LONGLONG>((record.sequence % capacity) * sizeof(Record));
		DWORD written{};
		// Synchronous WriteFile copies data into the OS cache before the original
		// handler is entered. Process termination does not discard that cache.
		// Do not FlushFileBuffers for every texture or perform logging under this lock.
		const bool ok = SetFilePointerEx(journal, offset, nullptr, FILE_BEGIN) &&
			WriteFile(journal, &record, sizeof(record), &written, nullptr) && written == sizeof(record);
		const auto error = ok ? ERROR_SUCCESS : GetLastError();
		ReleaseSRWLockExclusive(&journalLock);
		if (!ok) {
			static std::atomic<bool> reported{false};
			if (!reported.exchange(true)) F4SE::log::error("f4sr: texture journal write failed error={}", error);
		}
	}

	bool OpenJournal()
	{
		wchar_t executable[MAX_PATH]{};
		if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return false;
		const auto path = std::filesystem::path(executable).parent_path() /
			(L"f4sr-texture-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".bin");
		journal = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (journal != INVALID_HANDLE_VALUE) return true;
		F4SE::log::error("f4sr: texture probe journal creation failed error={}", GetLastError());
		return false;
	}

	template <std::uint32_t Site>
	HRESULT STDMETHODCALLTYPE Probe(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
		const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** output)
	{
		Record record{};
		record.sequence = sequence.fetch_add(1);
		record.callId = record.sequence;
		record.tick = GetTickCount64();
		record.site = Site;
		record.thread = GetCurrentThreadId();
		record.initialData = reinterpret_cast<std::uintptr_t>(initial);
		if (desc && Readable(reinterpret_cast<std::uintptr_t>(desc), sizeof(*desc))) {
			record.desc = *desc;
			record.flags |= 1;
		}
		if (initial && Readable(reinterpret_cast<std::uintptr_t>(initial), sizeof(*initial))) {
			record.pitch = initial->SysMemPitch;
			record.slicePitch = initial->SysMemSlicePitch;
			record.flags |= 2;
		}
		WriteRecord(record); // Must precede Buffout's terminating warning path.
		const auto result = originals[Site](device, desc, initial, output);
		record.phase = 1;
		record.result = static_cast<std::uint32_t>(result);
		// Use a new slot so a slow call cannot overwrite a newer ring entry.
		record.sequence = sequence.fetch_add(1);
		WriteRecord(record);
		if (FAILED(result)) F4SE::log::error("f4sr: texture create failed site={} call={} hr={:#x} size={}x{} mips={} array={} format={} sample={}/{} usage={} bind={:#x} cpu={:#x} misc={:#x} pitch={} slicePitch={} valid={:#x}",
			Site, record.callId, record.result, record.desc.Width, record.desc.Height, record.desc.MipLevels,
			record.desc.ArraySize, static_cast<unsigned>(record.desc.Format), record.desc.SampleDesc.Count,
			record.desc.SampleDesc.Quality, static_cast<unsigned>(record.desc.Usage), record.desc.BindFlags,
			record.desc.CPUAccessFlags, record.desc.MiscFlags, record.pitch, record.slicePitch, record.flags);
		return result; // Preserve the existing handler, including failures.
	}

	HRESULT STDMETHODCALLTYPE CreateDirect(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
		const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** output)
	{
		return device->CreateTexture2D(desc, initial, output);
	}

	// A read-only pointer cell within rel32 range; allocation failure disables
	// diagnostics instead of invoking Trampoline::create's fatal error path.
	void* AllocateCallPointer(std::uintptr_t site)
	{
		SYSTEM_INFO system{}; GetSystemInfo(&system);
		const auto granularity = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
		const auto low = site > 0x7fff0000u ? site - 0x7fff0000u : granularity;
		const auto high = site + 0x7fff0000u;
		auto address = (low + granularity - 1) & ~(granularity - 1);
		while (address < high) {
			MEMORY_BASIC_INFORMATION info{};
			if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) break;
			const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
			if (end <= address) break;
			if (info.State == MEM_FREE && end - address >= granularity && high - address >= granularity) {
				if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(address), granularity,
					MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return memory;
			}
			address = (end + granularity - 1) & ~(granularity - 1);
		}
		return nullptr;
	}

	bool InstallBuffout1286()
	{
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Buffout4.dll"));
		if (!base || !Readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000) return false;
		const auto ntAddress = base + dos->e_lfanew;
		if (!Readable(ntAddress, sizeof(IMAGE_NT_HEADERS64))) return false;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(ntAddress);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
			nt->FileHeader.TimeDateStamp != 0x64b4da69 || nt->OptionalHeader.SizeOfImage != 0x358000) return false;
		// Verified against installed 1.28.6 SHA256 978fc8d87d2aa18ed003cc3c90bf55f9b8a3c7ce3df384a153342825f37bd368.
		// Its warning handler has a fifth filename-context argument. Do NOT wrap
		// the whole handler with the four-argument API signature. Replace ONLY
		// mov rax,[rcx]; call [rax+28h] at +ADECA, preserving its frame/context.
		constexpr std::uint8_t expected[]{
			0x40,0x55,0x53,0x57,0x48,0x8d,0x6c,0x24,0xf0,0x48,0x81,0xec,0x10,0x01,0x00,0x00,
			0x48,0x8b,0x05,0x31,0xbe,0x22,0x00,0x48,0x33,0xc4,0x48,0x89,0x45,0x08,0x48,0x8b,
			0x7d,0x50,0xc7,0x44,0x24,0x20,0x00,0x00,0x00,0x00,0x48,0x8b,0x01,0xff,0x50,0x28,
			0x8b,0xd8,0x85,0xc0,0x75,0x17,0x48,0x8b,0x4d,0x08,0x48,0x33,0xcc,0xe8,0x5e,0x04,
			0x0b,0x00,0x48,0x81,0xc4,0x10,0x01,0x00,0x00,0x5f,0x5b,0x5d,0xc3
		};
		if (!Readable(base + 0xadea0, sizeof(expected)) ||
			std::memcmp(reinterpret_cast<void*>(base + 0xadea0), expected, sizeof(expected)) != 0) {
			F4SE::log::error("f4sr: texture probe refused: Buffout 1.28.6 handler signature mismatch");
			return true; // Recognized version: fail closed, never try another ABI.
		}
		const auto site = base + 0xadeca;
		auto* cell = AllocateCallPointer(site);
		if (!cell) { F4SE::log::error("f4sr: texture probe refused: no rel32 pointer cell"); return true; }
		const CreateFn thunk = Probe<3>;
		std::memcpy(cell, &thunk, sizeof(thunk));
		DWORD oldCellProtect{};
		if (!VirtualProtect(cell, sizeof(thunk), PAGE_READONLY, &oldCellProtect) || !OpenJournal()) {
			VirtualFree(cell, 0, MEM_RELEASE); return true;
		}
		std::uint8_t patch[]{0xff, 0x15, 0, 0, 0, 0};
		const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(cell) - static_cast<std::intptr_t>(site + 6));
		std::memcpy(patch + 2, &displacement, sizeof(displacement));
		DWORD oldProtect{};
		if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			F4SE::log::error("f4sr: texture probe refused: handler page protection error={}", GetLastError());
			CloseHandle(journal); journal = INVALID_HANDLE_VALUE; VirtualFree(cell, 0, MEM_RELEASE); return true;
		}
		originals[3] = CreateDirect;
		std::memcpy(reinterpret_cast<void*>(site), patch, sizeof(patch));
		DWORD ignored{};
		const bool restored = VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), oldProtect, &ignored) != 0;
		const bool flushed = FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(patch)) != 0;
		F4SE::log::info("f4sr: texture probe installed inside Buffout 1.28.6 API call; site=3 rva=0xADECA records={} protectionRestored={} cacheFlushed={}", capacity, restored, flushed);
		return true;
	}
}

namespace f4sr
{
	void InstallTextureProbe()
	{
		if (!GetSettings().textureProbe) return;
		static bool attempted = false;
		if (attempted) return;
		attempted = true;
		if (InstallBuffout1286()) return;
		// Only accept the known Buffout call->argument-fixup->C++ handler chain.
		// No guessed instruction lengths, device-vtable hooks or runtime reinstallation.
		const std::uint64_t ids[] = {678241, 1165600, 367479};
		const std::ptrdiff_t offsets[] = {0x147, 0x4c, 0xAA};
		const std::array<std::uint8_t, 5> prefixes[] = {
			{0x4c, 0x8d, 0x4d, 0x08, 0}, {0x4c, 0x8d, 0x4f, 0x08, 0}, {0x48, 0x8d, 0x54, 0x24, 0x40}
		};
		const CreateFn thunks[] = {Probe<0>, Probe<1>, Probe<2>};
		std::uintptr_t sites[3]{};
		std::array<std::array<std::uint8_t, 17>, 3> shims{};
		for (std::size_t i = 0; i < 3; ++i) {
			sites[i] = REL::ID(ids[i]).address() + offsets[i];
			const auto* call = reinterpret_cast<const std::uint8_t*>(sites[i]);
			if (!Readable(sites[i], 6) || call[0] != 0xff || call[1] != 0x15) {
				F4SE::log::error("f4sr: texture probe refused: site {} is not a known Buffout indirect call", i);
				return;
			}
			std::int32_t displacement{};
			std::memcpy(&displacement, call + 2, sizeof(displacement));
			const auto pointer = sites[i] + 6 + displacement;
			std::uintptr_t stub{};
			if (!Readable(pointer, sizeof(stub))) return;
			std::memcpy(&stub, reinterpret_cast<void*>(pointer), sizeof(stub));
			const auto prefixSize = i == 2 ? 5u : 4u;
			if (!Readable(stub, prefixSize + 12)) return;
			const auto* code = reinterpret_cast<const std::uint8_t*>(stub);
			if (std::memcmp(code, prefixes[i].data(), prefixSize) != 0 || code[prefixSize] != 0x48 ||
				code[prefixSize + 1] != 0xb8 || code[prefixSize + 10] != 0xff || code[prefixSize + 11] != 0xe0) {
				F4SE::log::error("f4sr: texture probe refused: unknown argument-fixup stub at site {}", i);
				return;
			}
			std::memcpy(&originals[i], code + prefixSize + 2, sizeof(CreateFn));
			HMODULE owner{};
			wchar_t ownerPath[MAX_PATH]{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(originals[i]), &owner) || !GetModuleFileNameW(owner, ownerPath, MAX_PATH) ||
				_wcsicmp(std::filesystem::path(ownerPath).filename().c_str(), L"Buffout4.dll") != 0) {
				F4SE::log::error("f4sr: texture probe refused: handler is not owned by Buffout4.dll");
				return;
			}
			std::memcpy(shims[i].data(), code, prefixSize + 12);
			std::memcpy(shims[i].data() + prefixSize + 2, &thunks[i], sizeof(CreateFn));
		}
		if (!OpenJournal()) return;
		// All three sites preflighted before any patch; startup-only PostPostLoad.
		auto& trampoline = F4SE::GetTrampoline();
		std::uintptr_t targets[3]{};
		for (std::size_t i = 0; i < 3; ++i) {
			auto* code = trampoline.allocate(shims[i].size());
			std::memcpy(code, shims[i].data(), shims[i].size());
			targets[i] = reinterpret_cast<std::uintptr_t>(code);
		}
		for (std::size_t i = 0; i < 3; ++i) trampoline.write_call<6>(sites[i], targets[i]);
		F4SE::log::info("f4sr: texture probe installed at 3 Buffout call sites; bounded journal={} records, pid={}", capacity, GetCurrentProcessId());
	}
}
