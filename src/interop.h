#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace f4sr
{
	// A texture created on the game's D3D11 device with NT-handle sharing
	// and opened on the private D3D12 device. The recipe follows
	// doodlum's FO4 interop. Pixel readbacks verified aliasing on the
	// target stack; resource state and GPU lifetime still need explicit
	// synchronization. This is the only channel FFX work crosses.
	struct SharedTexture
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> tex11;
		Microsoft::WRL::ComPtr<ID3D12Resource> res12;
		D3D11_TEXTURE2D_DESC desc{};
	};

	// Owns the private D3D12 device (created on the game's adapter), its
	// direct queue, double-buffered command lists, and one fence shared
	// with the D3D11 side for cross-API ordering.
	class D3D12Interop
	{
	public:
		bool Create(ID3D11Device* gameDevice11);
		bool Destroy();
		bool Drain();
		[[nodiscard]] bool Created() const { return device12_ != nullptr; }
		[[nodiscard]] bool Healthy() const { return !failed_; }

		ID3D12Device* Device12() const { return device12_.Get(); }
		ID3D12CommandQueue* Queue12() const { return queue12_.Get(); }

		bool CreateSharedTexture(const D3D11_TEXTURE2D_DESC& desc, SharedTexture& out);

		// Frame command list on the D3D12 queue (double-buffered).
		ID3D12GraphicsCommandList* BeginFrame();
		bool EndFrame();
		static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
			D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

		// Cross-API ordering on the single shared fence. Producer calls
		// Signal after submitting its work; consumer Waits before touching
		// the shared textures. One monotonic counter serves both directions.
		bool SignalFromD3D12();
		bool WaitOnD3D11(ID3D11DeviceContext* ctx11);
		bool SignalFromD3D11(ID3D11DeviceContext* ctx11);
		bool WaitOnD3D12();

		// Asynchronous presentation (iXeFGAsyncPresent): one more shared fence
		// per direction, signalled with caller-chosen values (the frame
		// sequence), so the game thread and the present thread never share a
		// counter. "Ready": D3D11 finished writing a hand-off slot. "Retired":
		// D3D12 (including the XeFG Present) finished reading it.
		bool CreateAsyncFences();
		bool SignalReadyFromD3D11(ID3D11DeviceContext* ctx11, std::uint64_t value);
		bool WaitReadyOnD3D12(std::uint64_t value);
		bool SignalRetiredFromD3D12(std::uint64_t value);
		bool WaitRetiredOnD3D11(ID3D11DeviceContext* ctx11, std::uint64_t value);

	private:
		bool CreateSharedFence(Microsoft::WRL::ComPtr<ID3D12Fence>& fence12, Microsoft::WRL::ComPtr<ID3D11Fence>& fence11, const char* what);
		bool Check(HRESULT hr, const char* operation);
		bool WaitForCompletion(std::uint64_t value);
		Microsoft::WRL::ComPtr<ID3D12Fence> completionFence_;
		std::uint64_t completionValue_{ 0 };
		std::uint64_t allocatorCompletion_[2]{};
		HANDLE completionEvent_{ nullptr };
		bool recording_{ false };
		bool failed_{ false };
		Microsoft::WRL::ComPtr<ID3D11Device> device11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[2];
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> lists_[2];
		Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
		Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
		std::uint64_t fenceValue_{ 0 };
		Microsoft::WRL::ComPtr<ID3D12Fence> ready12_, retired12_;
		Microsoft::WRL::ComPtr<ID3D11Fence> ready11_, retired11_;
		std::uint32_t frameIndex_{ 0 };
	};
}
