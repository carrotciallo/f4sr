#pragma once
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <d3d11.h>
#include <cstdint>
#include <fstream>
#include <string>

namespace f4sr
{
	inline auto CaptureSessionTick()
	{
		static const auto tick = GetTickCount64();
		return tick;
	}

	// Raw dumps are only comparable when their sidecars agree on session/frame.
	inline bool WriteCapture(const char* name, ID3D11Texture2D* texture, const D3D11_MAPPED_SUBRESOURCE& map)
	{
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		std::uint32_t pixelBytes{};
		switch (desc.Format) {
		case DXGI_FORMAT_R16G16B16A16_FLOAT: pixelBytes = 8; break;
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: pixelBytes = 4; break;
		default:
			F4SE::log::error("f4sr: dump {} refused unsupported format {}", name, static_cast<std::uint32_t>(desc.Format));
			return false;
		}
		const auto rowBytes = static_cast<std::size_t>(desc.Width) * pixelBytes;
		if (!map.pData || map.RowPitch < rowBytes) return false;
		std::ofstream out(name, std::ios::binary);
		std::uint64_t hash = 14695981039346656037ull;
		for (std::uint32_t y = 0; y < desc.Height && out; ++y) {
			const auto* row = static_cast<const unsigned char*>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch;
			for (std::size_t x = 0; x < rowBytes; ++x) hash = (hash ^ row[x]) * 1099511628211ull;
			out.write(reinterpret_cast<const char*>(row), static_cast<std::streamsize>(rowBytes));
		}
		out.close();
		if (!out) return false;
		const auto sessionTick = CaptureSessionTick();
		auto* state = reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address());
		LARGE_INTEGER qpc{};
		QueryPerformanceCounter(&qpc);
		std::ofstream meta(std::string(name) + ".json");
		meta << "{\"pid\":" << GetCurrentProcessId() << ",\"sessionTick\":" << sessionTick
			<< ",\"frame\":" << (state ? state->frameCount : 0)
			<< ",\"temporalIndex\":" << (state ? state->currentFrame : 0) << ",\"qpc\":" << qpc.QuadPart
			<< ",\"width\":" << desc.Width << ",\"height\":" << desc.Height
			<< ",\"format\":" << static_cast<std::uint32_t>(desc.Format) << ",\"pixelBytes\":" << pixelBytes
			<< ",\"texture\":" << reinterpret_cast<std::uintptr_t>(texture) << ",\"fnv1a64\":\"" << std::hex << hash << "\"}\n";
		meta.close();
		F4SE::log::info("f4sr: dump {} frame={} size={}x{} fmt={} metadata={}", name,
			state ? state->frameCount : 0, desc.Width, desc.Height, static_cast<std::uint32_t>(desc.Format), meta.good());
		return meta.good();
	}
}
