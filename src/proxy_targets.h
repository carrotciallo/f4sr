#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>

namespace f4sr
{
	// Logical render-target ids (Dear-Modding engine layout, OG
	// 1.10.163). NOTE: these are NOT the numbers doodlum's older enum
	// used (kMain=3 / MV=29 there); the manager's ID arrays index by
	// THESE ids. The platform slots they resolve to on this setup
	// (kMain -> 4, kGBufferMotionVectors -> 29, depth kMain -> 2)
	// are exactly the direct slots the seam pins empirically -- the
	// mapping was never broken, the lookups used the wrong enum.
	namespace logical
	{
		inline constexpr std::int32_t kFrameBuffer = 0;
		inline constexpr std::int32_t kMain = 1;
		inline constexpr std::int32_t kMainCopy = 2;
		inline constexpr std::int32_t kSSRPrepass = 4;
		inline constexpr std::int32_t kSSR = 5;
		inline constexpr std::int32_t kSSRBlurred0 = 6;
		inline constexpr std::int32_t kSSRBlurred1 = 7;
		inline constexpr std::int32_t kSSRDepth = 8;
		inline constexpr std::int32_t kGBufferAlbedoSpecular = 26;
		inline constexpr std::int32_t kGBufferNormal = 27;
		inline constexpr std::int32_t kGBufferEnvironment = 29;
		inline constexpr std::int32_t kGBufferAmbient = 30;
		inline constexpr std::int32_t kGBufferEmissive = 31;
		inline constexpr std::int32_t kGBufferMotionVectors = 32;
		inline constexpr std::int32_t kDiffuseLight = 33;
		inline constexpr std::int32_t kDiffuseSpecular = 34;
		inline constexpr std::int32_t kDiffuseLightTiled = 35;
		inline constexpr std::int32_t kDiffuseSpecularTiled = 36;
		inline constexpr std::int32_t kAmbientOcclusion = 37;
		inline constexpr std::int32_t kAmbientOcclusionHalf = 39;
		inline constexpr std::int32_t kSAOCameraZ = 40;
		inline constexpr std::int32_t kCount = 100;
		inline constexpr std::int32_t kDepthMain = 1;  // DepthStencilTargetID
	}

	// BSGraphics::RenderTarget layout (0x30, six view/texture
	// pointers); the engine structs are read and written through
	// this mirror, same as the rest of the plugin treats them.
	struct RtStruct
	{
		void* texture{ nullptr };      // 0x00
		void* copyTexture{ nullptr };  // 0x08
		void* rtView{ nullptr };       // 0x10
		void* srView{ nullptr };       // 0x18
		void* copySRView{ nullptr };   // 0x20
		void* uaView{ nullptr };       // 0x28
	};
	static_assert(sizeof(RtStruct) == 0x30);

	// Render-resolution proxy targets, ported from community-shaders'
	// DynamicResolution module (features/Upscaling). The engine's own
	// dynamic resolution only moves a viewport inside the full-size
	// targets; several stock passes (the deferred composite, the
	// SSLR chain) sample with texture-space UVs that assume the
	// bound target IS the whole image. Around those passes this
	// class swaps the engine's render-target structs for proxies
	// that really are render-sized (contents copied in/out per
	// pass), publishes ratio 1.0 to the engine for the duration,
	// and substitutes a render-size depth for srViewDepth -- so the
	// stock passes run exactly as they do at native resolution.
	class ProxyTargets
	{
	public:
		// (Re)build the proxy set for a render ratio; no-op when the
		// ratio AND engine resource identities are unchanged. A ratio of 1 releases the textures.
		void UpdateRenderTargets(float widthRatio, float heightRatio);
		void Release();

		// The imagespace manager pointer (the RenderEffectRange
		// thunk's `this`); needed for the chain-triangle swap.
		void SetImageSpaceManager(void* ism) { ism_ = ism; }

		[[nodiscard]] bool HasProxies() const { return hasProxies_; }

		enum class ProxyTriangle : std::uint8_t
		{
			kEngine,
			kUnit
		};
		// Wrap one stock pass that assumes texture-space UVs: swap
		// the listed targets' contents into their proxies (targets
		// not listed are swapped without a fill), publish ratio 1,
		// optionally swap the chain triangle to the unit one.
		bool BeginProxyPass(std::initializer_list<std::int32_t> toProxy, ProxyTriangle triangle);
		// Copies only the listed targets back to the engine targets,
		// restores everything else, republishes the render ratio.
		void EndProxyPass(std::initializer_list<std::int32_t> fromProxy);
		// Pooled targets (kSSRBlurred0): the platform slot exists
		// only between the engine's acquire and release calls.
		void ProxyAcquiredRenderTarget(std::int32_t logical);
		void ReleaseAcquiredRenderTarget(std::int32_t logical);

		// Proxy textures and ratio=1 are one scope. Never restore the
		// reduced ratio while retaining scaled textures for Forward.
		[[nodiscard]] bool InProxyPass() const { return proxyPass_; }

		// One-shot probe accessors (composite-pass forensics).
		void* DebugSlotTexture(std::int32_t logical);
		void* DebugOriginalTexture(std::int32_t logical);
		void* DebugProxyTexture(std::int32_t logical);

	private:
		void UpdateRenderTargetOne(std::int32_t logical, float widthRatio, float heightRatio);
		void CaptureOriginal(std::int32_t logical, const RtStruct& engineTarget);
		void ReleaseProxy(std::int32_t logical);
		void RestoreEngineSlot(std::int32_t logical);
		void OverrideRenderTargets(std::initializer_list<std::int32_t> toCopy);
		void ResetRenderTargets(std::initializer_list<std::int32_t> toCopy);
		void RestoreBoundViews();
		void OverrideOne(std::int32_t logical, bool doCopy);
		void ResetOne(std::int32_t logical, bool doCopy);
		void OverrideDepth(bool doCopy);
		void ResetDepth();
		bool CopyDepth();
		bool CompileDepthShaders(ID3D11Device* device);
		void AbortProxyPass();
		void ApplyTriangleSwap();
		void RestoreTriangleSwap();
		[[nodiscard]] bool ProxyPassAvailable() const;

		static RtStruct* ResolveTarget(std::int32_t logical);
		static void* ResolveDepthTarget(std::int32_t logical);  // DepthStencilTarget*
		static std::uint32_t FrameCount();

		// Engine structs indexed by logical id. proxies_ mirrors
		// originals_ except texture/rtView/srView/uaView point at
		// the proxy objects owned below (copyTexture/copySRView
		// keep the engine's, so a swap-in never nulls them).
		RtStruct originals_[logical::kCount]{};
		// Keep displaced engine resources/views alive until the scope is restored.
		Microsoft::WRL::ComPtr<IUnknown> originalRefs_[logical::kCount][6];
		RtStruct proxies_[logical::kCount]{};
		bool captured_[logical::kCount]{};
		Microsoft::WRL::ComPtr<ID3D11Texture2D> proxyTex_[logical::kCount];
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> proxyRtv_[logical::kCount];
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> proxySrv_[logical::kCount];
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> proxyUav_[logical::kCount];
		std::uint32_t filledFrame_[logical::kCount]{};
		bool filledValid_[logical::kCount]{};

		// Manager renderTargetData[] width/height are scaled while
		// targets are overridden so size queries stay correct.
		std::uint32_t savedPropsW_[logical::kCount]{};
		std::uint32_t savedPropsH_[logical::kCount]{};
		bool propsSaved_{ false };

		Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex_;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv_;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depthUav_;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> overrideDepthCs_;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> linearDepthCs_;
		Microsoft::WRL::ComPtr<ID3D11Buffer> upscalingCb_;
		void* savedDepthSrv_{ nullptr };
		bool depthOverridden_{ false };
		std::uint32_t depthCopyFrame_{ 0 };
		bool depthCopyValid_{ false };

		void* ism_{ nullptr };
		void* triSaved_{ nullptr };
		bool triSwapped_{ false };

		bool hasProxies_{ false };
		bool overridden_{ false };
		bool proxyPass_{ false };
		float passRatioW_{ 1.0f };
		float passRatioH_{ 1.0f };
		float prevRatioW_{ -1.0f };
		float prevRatioH_{ -1.0f };
		bool mapLogged_{ false };
	};
}
