#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace f4sr
{
	struct FgOverlaySample
	{
		std::uint64_t source{}, real{}, generated{}, resets{}, failures{};
		HRESULT statistics{ DXGI_ERROR_UNSUPPORTED };
		bool enabled{};
		const char* reason{ "OFF" };
	};

	// Diagnostic text panel blended (premultiplied alpha, rounded translucent
	// card sized to its text) into the top-left of a swap chain back buffer;
	// falls back to an opaque copy when the back buffer has no render-target view.
	// Two variants (0 and 1, drawn in different colours) are kept so a caller can
	// mark alternating frames, e.g. real vs generated. The text is rebuilt at most
	// once per second; `build` receives the seconds since the previous rebuild
	// (0 on the first one) so callers can turn counters into rates.
	class FgOverlay
	{
	public:
		using Builder = std::function<std::array<std::string, 2>(double seconds)>;
		void Draw(IDXGISwapChain* chain, int variant, const Builder& build);
		void Reset();
	private:
		bool Update(ID3D11Device* device, DXGI_FORMAT format, const std::array<std::string, 2>& text);
		bool CreatePipeline(ID3D11Device* device);
		bool Blend(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, int index);
		Microsoft::WRL::ComPtr<ID3D11Device> device_;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> panels_[2];
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> views_[2];
		UINT panelW_[2]{}, panelH_[2]{};
		Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
		Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
		bool pipelineFailed_{};
		DXGI_FORMAT format_{ DXGI_FORMAT_UNKNOWN };
		ULONGLONG lastTick_{};
	};
}
