#include "smoke_common.hpp"

#include <d3d11.h>
#include <dxgi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HWND window = ns_create_window(instance, L"NeuroShade DX11 qualification");
    if (window == nullptr) return 10;

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_desc.BufferDesc.Width = 320;
    swap_desc.BufferDesc.Height = 240;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.OutputWindow = window;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 1,
        D3D11_SDK_VERSION, &swap_desc, &swapchain, &device, &feature, &context);
    if (FAILED(result)) {
        DestroyWindow(window);
        return 11;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    result = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(backbuffer, nullptr, &target);
    ns_release(backbuffer);
    if (FAILED(result)) {
        ns_release(context);
        ns_release(device);
        ns_release(swapchain);
        DestroyWindow(window);
        return 12;
    }

    for (unsigned frame = 0; frame < 90 && ns_pump_messages(); ++frame) {
        const float color[] = {static_cast<float>(frame % 30) / 30.0F, 0.20F, 0.55F, 1.0F};
        context->ClearRenderTargetView(target, color);
        result = swapchain->Present(0, 0);
        if (FAILED(result)) break;
        Sleep(4);
    }

    context->ClearState();
    ns_release(target);
    ns_release(context);
    ns_release(device);
    ns_release(swapchain);
    DestroyWindow(window);
    return FAILED(result) ? 13 : 0;
}
