#include "interop.h"

#include "F4SE/F4SE.h"

#include <dxgi1_6.h>

using Microsoft::WRL::ComPtr;

namespace f4sr
{
	bool D3D12Interop::Create(ID3D11Device* gameDevice11)
	{
		if (gameDevice11 == nullptr) {
			return false;
		}
		if (!Destroy()) {
			return false;
		}
		device11_ = gameDevice11;

		// The D3D12 device must sit on the same adapter as the game's
		// D3D11 device, or shared handles do not resolve.
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		if (FAILED(gameDevice11->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))) ||
			FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
			F4SE::log::error("f4sr: interop: cannot get game adapter");
			return false;
		}
		if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device12_.GetAddressOf())))) {
			F4SE::log::error("f4sr: interop: D3D12CreateDevice failed");
			return false;
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue12_.GetAddressOf())))) {
			F4SE::log::error("f4sr: interop: CreateCommandQueue failed");
			return false;
		}
		for (std::uint32_t i = 0; i < 2; ++i) {
			if (FAILED(device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocators_[i].GetAddressOf()))) ||
				FAILED(device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[i].Get(), nullptr, IID_PPV_ARGS(lists_[i].GetAddressOf())))) {
				F4SE::log::error("f4sr: interop: command list creation failed");
				return false;
			}
			lists_[i]->Close();
		}

		// One fence, shared both ways (D3D12 creates, D3D11 opens).
		if (FAILED(device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence12_.GetAddressOf())))) {
			F4SE::log::error("f4sr: interop: CreateFence failed");
			return false;
		}
		HANDLE sharedHandle = nullptr;
		if (FAILED(device12_->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle))) {
			F4SE::log::error("f4sr: interop: fence CreateSharedHandle failed");
			return false;
		}
		ComPtr<ID3D11Device5> device5;
		HRESULT hr = E_FAIL;
		if (SUCCEEDED(gameDevice11->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf())))) {
			hr = device5->OpenSharedFence(sharedHandle, IID_PPV_ARGS(fence11_.GetAddressOf()));
		}
		CloseHandle(sharedHandle);
		if (FAILED(hr)) {
			F4SE::log::error("f4sr: interop: OpenSharedFence failed");
			return false;
		}
		if (!Check(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(completionFence_.GetAddressOf())), "CreateCompletionFence")) {
			return false;
		}
		completionEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (completionEvent_ == nullptr) {
			return Check(HRESULT_FROM_WIN32(GetLastError()), "CreateCompletionEvent");
		}
		F4SE::log::info("f4sr: D3D12 interop ready (device + queue + shared fence)");
		return true;
	}

	bool D3D12Interop::Destroy()
	{
		if (!Drain()) {
			return false;  // Retain in-flight resources; never free on timeout.
		}
		completionFence_.Reset();
		if (completionEvent_ != nullptr) CloseHandle(completionEvent_);
		completionEvent_ = nullptr;
		completionValue_ = 0;
		allocatorCompletion_[0] = allocatorCompletion_[1] = 0;
		recording_ = false;
		failed_ = false;
		device11_.Reset();
		fence11_.Reset();
		fence12_.Reset();
		for (auto& l : lists_) l.Reset();
		for (auto& a : allocators_) a.Reset();
		queue12_.Reset();
		device12_.Reset();
		fenceValue_ = 0;
		frameIndex_ = 0;
		return true;
	}

	bool D3D12Interop::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& descIn, SharedTexture& out)
	{
		if (device11_ == nullptr || device12_ == nullptr) {
			return false;
		}
		SharedTexture replacement;
		D3D11_TEXTURE2D_DESC desc = descIn;
		desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(device11_->CreateTexture2D(&desc, nullptr, replacement.tex11.GetAddressOf()))) {
			F4SE::log::error("f4sr: interop: CreateTexture2D (shared) failed");
			return false;
		}
		ComPtr<IDXGIResource1> dxgiRes;
		HANDLE handle = nullptr;
		HRESULT hr = replacement.tex11.As(&dxgiRes);
		if (SUCCEEDED(hr)) {
			hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
		}
		if (SUCCEEDED(hr)) {
			hr = device12_->OpenSharedHandle(handle, IID_PPV_ARGS(replacement.res12.GetAddressOf()));
			CloseHandle(handle);
		}
		if (FAILED(hr)) {
			F4SE::log::error("f4sr: interop: shared handle open on D3D12 failed");
			return false;
		}
		replacement.desc = desc;
		out = std::move(replacement);
		return true;
	}

	bool D3D12Interop::Check(HRESULT hr, const char* operation)
	{
		if (SUCCEEDED(hr)) return true;
		failed_ = true;
		F4SE::log::error("f4sr: interop {} failed hr=0x{:08X}", operation, static_cast<std::uint32_t>(hr));
		return false;
	}

	bool D3D12Interop::WaitForCompletion(std::uint64_t value)
	{
		if (value == 0) return true;
		const auto completed = completionFence_->GetCompletedValue();
		if (completed == UINT64_MAX) return Check(DXGI_ERROR_DEVICE_REMOVED, "completion fence");
		if (completed >= value) return true;
		if (!Check(completionFence_->SetEventOnCompletion(value, completionEvent_), "SetEventOnCompletion")) return false;
		const DWORD result = WaitForSingleObject(completionEvent_, 5000);
		if (result != WAIT_OBJECT_0) {
			return Check(result == WAIT_TIMEOUT ? DXGI_ERROR_WAIT_TIMEOUT :
				HRESULT_FROM_WIN32(GetLastError()), "CPU fence wait");
		}
		// A timed-out registration may signal this reusable event later.
		// The event is only a wake-up; the fence value is the retirement proof.
		const auto retired = completionFence_->GetCompletedValue();
		if (retired == UINT64_MAX) return Check(DXGI_ERROR_DEVICE_REMOVED, "completion fence after wait");
		return retired >= value || Check(E_UNEXPECTED, "premature completion event");
	}

	bool D3D12Interop::Drain()
	{
		if (completionFence_ == nullptr) return true; // No work can have been submitted.
		if (FAILED(device12_->GetDeviceRemovedReason())) return true; // Device is no longer executing.
		if (recording_) return Check(E_UNEXPECTED, "Drain while recording");
		ComPtr<ID3D11DeviceContext> ctx;
		device11_->GetImmediateContext(ctx.GetAddressOf());
		// Include D3D11's last reads of the shared outputs, not just D3D12 work.
		if (!SignalFromD3D11(ctx.Get()) || !WaitOnD3D12()) return false;
		if (!Check(queue12_->Signal(completionFence_.Get(), ++completionValue_), "Drain signal")) return false;
		return WaitForCompletion(completionValue_);
	}

	ID3D12GraphicsCommandList* D3D12Interop::BeginFrame()
	{
		if (failed_ || recording_ || !WaitForCompletion(allocatorCompletion_[frameIndex_])) return nullptr;
		auto* alloc = allocators_[frameIndex_].Get();
		auto* list = lists_[frameIndex_].Get();
		if (!Check(alloc->Reset(), "allocator Reset") || !Check(list->Reset(alloc, nullptr), "list Reset")) return nullptr;
		recording_ = true;
		return list;
	}

	bool D3D12Interop::EndFrame()
	{
		if (!recording_) return Check(E_UNEXPECTED, "EndFrame without BeginFrame");
		recording_ = false;
		auto* list = lists_[frameIndex_].Get();
		if (!Check(list->Close(), "list Close")) return false;
		ID3D12CommandList* exec[] = { list };
		queue12_->ExecuteCommandLists(1, exec);
		const auto value = ++completionValue_;
		if (!Check(queue12_->Signal(completionFence_.Get(), value), "allocator retirement signal")) return false;
		allocatorCompletion_[frameIndex_] = value;
		frameIndex_ = (frameIndex_ + 1) & 1u;
		return true;
	}

	void D3D12Interop::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
		list->ResourceBarrier(1, &barrier);
	}

	bool D3D12Interop::SignalFromD3D12()
	{
		return Check(queue12_->Signal(fence12_.Get(), ++fenceValue_), "D3D12 Signal");
	}

	bool D3D12Interop::WaitOnD3D11(ID3D11DeviceContext* ctx11)
	{
		ComPtr<ID3D11DeviceContext4> ctx4;
		return Check(ctx11->QueryInterface(IID_PPV_ARGS(ctx4.GetAddressOf())), "context4 Wait interface") &&
			Check(ctx4->Wait(fence11_.Get(), fenceValue_), "D3D11 Wait");
	}

	bool D3D12Interop::SignalFromD3D11(ID3D11DeviceContext* ctx11)
	{
		ComPtr<ID3D11DeviceContext4> ctx4;
		if (!Check(ctx11->QueryInterface(IID_PPV_ARGS(ctx4.GetAddressOf())), "context4 Signal interface") ||
			!Check(ctx4->Signal(fence11_.Get(), ++fenceValue_), "D3D11 Signal")) return false;
		ctx11->Flush(); // Submit the signal before the other API waits on it.
		return true;
	}

	bool D3D12Interop::WaitOnD3D12()
	{
		return Check(queue12_->Wait(fence12_.Get(), fenceValue_), "D3D12 Wait");
	}
}
