#define NOMINMAX // keep std::min usable
#include <windows.h>
#include "native.h"

// A child HWND in the host's window. Dawn draws into it with D3D12; a Win32
// timer on it drives frames, and its window procedure forwards the mouse.
namespace gpu::platform {

const char *const windowApi = CLAP_WINDOW_API_WIN32;
const bool needsTimer = false;

struct Window
{
    HWND hwnd = nullptr;
    Callbacks callbacks;
};

static HINSTANCE thisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *window = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (window)
    {
        const float x = float(short(LOWORD(lParam))), y = float(short(HIWORD(lParam)));
        switch (message)
        {
        case WM_TIMER: window->callbacks.frame(); return 0;
        case WM_LBUTTONDOWN: SetCapture(hwnd); window->callbacks.pointer(View::Pointer::down, x, y); return 0;
        case WM_MOUSEMOVE: if (GetCapture() == hwnd) window->callbacks.pointer(View::Pointer::move, x, y); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); window->callbacks.pointer(View::Pointer::up, x, y); return 0;
        case WM_ERASEBKGND: return 1; // Dawn paints every pixel
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    HWND host = static_cast<HWND>(parent->win32);
    if (!IsWindow(host)) return nullptr;
    static const wchar_t *className = L"simpleCharClapGpuView";
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = thisModule();
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass); // fails harmlessly when already registered

    auto *window = new Window{nullptr, std::move(callbacks)};
    window->hwnd = CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                   int(width), int(height), host, nullptr, thisModule(), nullptr);
    if (!window->hwnd)
    {
        delete window;
        return nullptr;
    }
    SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    SetTimer(window->hwnd, 1, 16, nullptr);
    return window;
}

wgpu::Surface createSurface(const wgpu::Instance &instance, Window *window)
{
    wgpu::SurfaceSourceWindowsHWND source;
    source.hinstance = thisModule();
    source.hwnd = window->hwnd;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    return instance.CreateSurface(&descriptor);
}

void destroy(Window *window)
{
    KillTimer(window->hwnd, 1);
    SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(window->hwnd);
    delete window;
}

// clap.gui sizes on Windows are already physical pixels.
void setSize(Window *window, uint32_t width, uint32_t height)
{
    SetWindowPos(window->hwnd, nullptr, 0, 0, int(width), int(height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void pixelSize(Window *window, uint32_t &width, uint32_t &height)
{
    RECT area = {};
    GetClientRect(window->hwnd, &area);
    width = uint32_t(area.right), height = uint32_t(area.bottom);
}

float scale(Window *window) { return float(GetDpiForWindow(window->hwnd)) / 96.0f; }

void setVisible(Window *window, bool visible) { ShowWindow(window->hwnd, visible ? SW_SHOW : SW_HIDE); }

void pumpEvents(Window *) {}

}
