#pragma once

#include <windows.h>

inline LRESULT CALLBACK ns_window_proc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

inline HWND ns_create_window(HINSTANCE instance, const wchar_t* title) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = ns_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"NeuroShadeQualificationWindow";
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    HWND window = CreateWindowExW(0, window_class.lpszClassName, title,
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  336, 279, nullptr, nullptr, instance, nullptr);
    if (window != nullptr) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    return window;
}

inline bool ns_pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

template <typename T>
inline void ns_release(T*& object) {
    if (object != nullptr) object->Release();
    object = nullptr;
}
