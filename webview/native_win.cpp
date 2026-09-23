#define NOMINMAX // keep std::min usable
#include <windows.h>
#include "native.h"

// The same steps as CHOC's own DesktopWindow::setContent().
namespace webview::platform {

const char *const windowApi = CLAP_WINDOW_API_WIN32;
const bool needsTimer = false;

void *attach(void *view, const clap_window_t *parent)
{
    HWND child = (HWND)view, host = (HWND)parent->win32;
    if (!IsWindow(host)) return nullptr;
    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    SetWindowLongPtrW(child, GWL_STYLE, (style & ~LONG_PTR(WS_POPUP)) | WS_CHILD | WS_CLIPSIBLINGS);
    SetParent(child, host);
    RECT area = {};
    GetClientRect(host, &area);
    SetWindowPos(child, nullptr, 0, 0, area.right, area.bottom,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    return host;
}

void detach(void *view, void *)
{
    ShowWindow((HWND)view, SW_HIDE);
    SetParent((HWND)view, nullptr);
}

void setSize(void *view, uint32_t width, uint32_t height)
{
    SetWindowPos((HWND)view, nullptr, 0, 0, int(width), int(height),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void setVisible(void *view, bool visible) { ShowWindow((HWND)view, visible ? SW_SHOW : SW_HIDE); }

void pumpEvents() {}

}
