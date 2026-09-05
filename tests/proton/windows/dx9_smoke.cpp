#include "smoke_common.hpp"

#include <d3d9.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HWND window = ns_create_window(instance, L"NeuroShade DX9 qualification");
    if (window == nullptr) return 10;

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d == nullptr) {
        DestroyWindow(window);
        return 11;
    }

    const auto parameters = [window](UINT width, UINT height) {
        D3DPRESENT_PARAMETERS value{};
        value.BackBufferWidth = width;
        value.BackBufferHeight = height;
        value.BackBufferFormat = D3DFMT_X8R8G8B8;
        value.BackBufferCount = 1;
        value.SwapEffect = D3DSWAPEFFECT_DISCARD;
        value.hDeviceWindow = window;
        value.Windowed = TRUE;
        value.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        return value;
    };
    auto present = parameters(320, 240);
    IDirect3DDevice9* device = nullptr;
    HRESULT result = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device);
    if (FAILED(result)) {
        ns_release(d3d);
        DestroyWindow(window);
        return 12;
    }

    unsigned frames = 0;
    for (; frames < 90 && ns_pump_messages(); ++frames) {
        if (frames == 45) {
            // Reset may modify the parameters; always supply a fresh structure.
            present = parameters(400, 300);
            result = device->Reset(&present);
            if (FAILED(result)) break;
        }
        result = device->Clear(0, nullptr, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB((frames % 30) * 8, 51, 140), 1.0F, 0);
        if (FAILED(result)) break;
        result = device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(result)) break;
        Sleep(4);
    }

    ns_release(device);
    ns_release(d3d);
    DestroyWindow(window);
    if (FAILED(result) || frames != 90) return 13;
    std::puts("d3d9_smoke=pass presents=90 resets=1");
    return 0;
}
