#pragma once
// Helpers shared by frame_pipeline.cpp and frame_pipeline_diag.cpp.
#include <d3d11.h>
#include <cstdint>

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "capture.h"

namespace f4sr::detail
{
	inline std::uint32_t CaptureFrameId()
	{
		return reinterpret_cast<const RE::BSGraphics::State*>(REL::ID(600795).address())->frameCount;
	}

	// Gameplay frame: no main/loading/pause/fader menu, no freeze frame.
	inline bool CaptureGameplay()
	{
		const auto* ui = RE::UI::GetSingleton();
		if (!ui || ui->freezeFramePause || ui->freezeFrameMenuBG || ui->closingAllMenus) return false;
		static const RE::BSFixedString menus[]{ "MainMenu", "LoadingMenu", "PauseMenu", "FaderMenu" };
		for (const auto& menu : menus) if (ui->GetMenuOpen(menu)) return false;
		return true;
	}

	// Copies `src` through the matching staging texture and writes it as a raw capture.
	inline bool DumpRawToFile(ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
		ID3D11Texture2D* stage, const char* name)
	{
		if (!src || !stage) return false;
		D3D11_TEXTURE2D_DESC s{}, d{};
		src->GetDesc(&s);
		stage->GetDesc(&d);
		if (s.Width != d.Width || s.Height != d.Height || s.Format != d.Format || s.SampleDesc.Count != 1) {
			F4SE::log::warn("f4sr: capture {} refused incompatible copy", name);
			return false;
		}
		ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, src, 0, nullptr);
		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map))) {
			return false;
		}
		const bool written = WriteCapture(name, src, map);
		ctx->Unmap(stage, 0);
		return written;
	}
}
