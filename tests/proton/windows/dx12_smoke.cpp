#include "smoke_common.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <array>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    constexpr UINT frame_count = 2;
    HWND window = ns_create_window(instance, L"NeuroShade DX12 qualification");
    if (window == nullptr) return 20;

    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* swapchain1 = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12Fence* fence = nullptr;
    std::array<ID3D12Resource*, frame_count> targets{};
    HANDLE fence_event = nullptr;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) {
        result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&device));
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(result)) result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = 320;
    swap_desc.Height = 240;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = frame_count;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (SUCCEEDED(result)) {
        result = factory->CreateSwapChainForHwnd(queue, window, &swap_desc, nullptr,
                                                  nullptr, &swapchain1);
    }
    if (SUCCEEDED(result)) result = swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = frame_count;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (SUCCEEDED(result)) result = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    const UINT rtv_size = SUCCEEDED(result)
                              ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
                              : 0;
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (SUCCEEDED(result)) handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < frame_count && SUCCEEDED(result); ++index) {
        result = swapchain->GetBuffer(index, IID_PPV_ARGS(&targets[index]));
        if (SUCCEEDED(result)) device->CreateRenderTargetView(targets[index], nullptr, handle);
        handle.ptr += rtv_size;
    }
    if (SUCCEEDED(result)) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator));
    }
    if (SUCCEEDED(result)) {
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                           nullptr, IID_PPV_ARGS(&commands));
    }
    if (SUCCEEDED(result)) result = commands->Close();
    if (SUCCEEDED(result)) result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                         IID_PPV_ARGS(&fence));
    if (SUCCEEDED(result)) {
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) result = HRESULT_FROM_WIN32(GetLastError());
    }

    UINT64 fence_value = 0;
    for (unsigned frame = 0; frame < 90 && SUCCEEDED(result) && ns_pump_messages(); ++frame) {
        const UINT index = swapchain->GetCurrentBackBufferIndex();
        result = allocator->Reset();
        if (SUCCEEDED(result)) result = commands->Reset(allocator, nullptr);
        D3D12_RESOURCE_BARRIER to_target{};
        to_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_target.Transition.pResource = targets[index];
        to_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (SUCCEEDED(result)) commands->ResourceBarrier(1, &to_target);
        auto target_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        target_handle.ptr += static_cast<SIZE_T>(index) * rtv_size;
        const float color[] = {0.12F, static_cast<float>(frame % 30) / 30.0F, 0.48F, 1.0F};
        if (SUCCEEDED(result)) commands->ClearRenderTargetView(target_handle, color, 0, nullptr);
        D3D12_RESOURCE_BARRIER to_present = to_target;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        if (SUCCEEDED(result)) commands->ResourceBarrier(1, &to_present);
        if (SUCCEEDED(result)) result = commands->Close();
        if (SUCCEEDED(result)) {
            ID3D12CommandList* lists[] = {commands};
            queue->ExecuteCommandLists(1, lists);
            result = swapchain->Present(0, 0);
        }
        if (SUCCEEDED(result)) result = queue->Signal(fence, ++fence_value);
        if (SUCCEEDED(result) && fence->GetCompletedValue() < fence_value) {
            result = fence->SetEventOnCompletion(fence_value, fence_event);
            if (SUCCEEDED(result) && WaitForSingleObject(fence_event, 5000) != WAIT_OBJECT_0) {
                result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
        Sleep(4);
    }

    if (fence_event != nullptr) CloseHandle(fence_event);
    for (auto*& target : targets) ns_release(target);
    ns_release(fence);
    ns_release(commands);
    ns_release(allocator);
    ns_release(rtv_heap);
    ns_release(swapchain);
    ns_release(swapchain1);
    ns_release(queue);
    ns_release(device);
    ns_release(factory);
    DestroyWindow(window);
    return FAILED(result) ? 21 : 0;
}
