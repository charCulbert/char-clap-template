#define NOMINMAX // keep std::min usable
#include <windows.h>
#include <ole2.h>
#include <shellapi.h> // DragQueryFileW
#include <shlobj.h>   // SHCreateDataObject
#include "native.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// A child HWND in the host's window. Dawn draws into it with D3D12; a Win32
// timer on it drives frames, and its window procedure forwards the mouse and keys.
namespace gpu::platform {

const char *const windowApi = CLAP_WINDOW_API_WIN32;
const bool needsTimer = false;

struct DropTarget;

struct Window
{
    HWND hwnd = nullptr;
    Callbacks callbacks;
    DropTarget *dropTarget = nullptr;         // files dragged in, where OLE works on this thread
    bool ole = false;                         // OleInitialize succeeded, so OleUninitialize
    std::vector<std::string> dragging;        // files to drag out, from the next WM_APP_DRAG
    bool locked = false;
    POINT anchor = {};       // screen position the cursor is held at while locked
    float x = 0, y = 0;      // pointer position reported to the view (pixels)
    bool tracking = false;   // asked for WM_MOUSELEAVE
    bool pressed = false;    // the left button went down here and hasn't come up
    wchar_t highSurrogate = 0; // the first half of a character beyond U+FFFF, typed
};

static const UINT WM_APP_DRAG = WM_APP + 1; // drag `dragging` out, outside any frame (DoDragDrop is modal)

static std::string toUtf8(const wchar_t *text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(size > 0 ? size - 1 : 0, '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

static std::wstring toWide(const std::string &utf8)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
    return wide;
}

// Files dragged in (Explorer's CF_HDROP), passed to View::files.
struct DropTarget : IDropTarget
{
    Window *window;
    std::vector<std::string> paths; // of the drag over us now
    ULONG references = 1;

    explicit DropTarget(Window *w) : window(w) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
    {
        *object = id == IID_IUnknown || id == IID_IDropTarget ? this : nullptr;
        if (!*object) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --references;
        if (!left) delete this;
        return left;
    }
    DWORD files(View::FileDrag type, POINTL at)
    {
        POINT p = {at.x, at.y};
        ScreenToClient(window->hwnd, &p);
        return window->callbacks.files(type, float(p.x), float(p.y), paths) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL at, DWORD *effect) override
    {
        paths.clear();
        FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        if (SUCCEEDED(data->GetData(&format, &medium)))
        {
            auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
            const UINT count = drop ? DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) : 0;
            for (UINT i = 0; i < count; ++i)
            {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
                DragQueryFileW(drop, i, path.data(), UINT(path.size() + 1));
                paths.push_back(toUtf8(path.c_str()));
            }
            if (drop) GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
        }
        *effect = paths.empty() ? DROPEFFECT_NONE : files(View::FileDrag::over, at);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL at, DWORD *effect) override
    {
        *effect = paths.empty() ? DROPEFFECT_NONE : files(View::FileDrag::over, at);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        window->callbacks.files(View::FileDrag::leave, 0, 0, {});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL at, DWORD *effect) override
    {
        *effect = paths.empty() ? DROPEFFECT_NONE : files(View::FileDrag::drop, at);
        return S_OK;
    }
};

// Files dragged out: the drag goes on while the left button is held.
struct DropSource : IDropSource
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
    {
        *object = id == IID_IUnknown || id == IID_IDropSource ? this : nullptr;
        return *object ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; } // lives on the stack for the drag
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        return escape ? DRAGDROP_S_CANCEL : keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

static void dragOut(Window *window)
{
    std::wstring list; // DROPFILES: each path and a nul, then one more nul
    for (const std::string &path : window->dragging) list += toWide(path) + L'\0';
    window->dragging.clear();
    if (list.empty()) return;
    list += L'\0';
    HGLOBAL memory = GlobalAlloc(GHND, sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    if (!memory) return;
    auto *files = static_cast<DROPFILES *>(GlobalLock(memory));
    files->pFiles = sizeof(DROPFILES);
    files->fWide = TRUE;
    std::memcpy(files + 1, list.data(), list.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    IDataObject *data = nullptr;
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {TYMED_HGLOBAL};
    medium.hGlobal = memory;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data))) || FAILED(data->SetData(&format, &medium, TRUE)))
    {
        if (data) data->Release();
        GlobalFree(memory);
        return;
    }
    DropSource source;
    DWORD effect = DROPEFFECT_NONE;
    DoDragDrop(data, &source, DROPEFFECT_COPY, &effect);
    data->Release();
    // The drag had the mouse, so the view never saw it let go.
    window->callbacks.pointer({View::Pointer::up, window->x, window->y, 0});
}

static HINSTANCE thisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

static unsigned modifiers()
{
    auto down = [](int key) { return GetKeyState(key) < 0; };
    return (down(VK_SHIFT) ? View::shift : 0u) | (down(VK_CONTROL) ? View::control : 0u) | (down(VK_MENU) ? View::alt : 0u);
}

static bool keyFor(WPARAM virtualKey, View::Key &key)
{
    switch (virtualKey)
    {
    case VK_LEFT: key = View::Key::left; return true;
    case VK_RIGHT: key = View::Key::right; return true;
    case VK_UP: key = View::Key::up; return true;
    case VK_DOWN: key = View::Key::down; return true;
    case VK_PRIOR: key = View::Key::pageUp; return true;
    case VK_NEXT: key = View::Key::pageDown; return true;
    case VK_HOME: key = View::Key::home; return true;
    case VK_END: key = View::Key::end; return true;
    case VK_TAB: key = View::Key::tab; return true;
    case VK_RETURN: key = View::Key::enter; return true;
    case VK_ESCAPE: key = View::Key::escape; return true;
    case VK_BACK: key = View::Key::backspace; return true;
    case VK_DELETE: key = View::Key::deleteForward; return true;
    }
    return false;
}

static std::string clipboardText(HWND hwnd)
{
    std::string text;
    if (!OpenClipboard(hwnd)) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
        if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(data)))
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (size > 1)
            {
                text.resize(size_t(size));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr);
                text.pop_back(); // the terminating zero
            }
            GlobalUnlock(data);
        }
    CloseClipboard();
    return text;
}

// Keyboard messages for our windows are taken straight from the thread's
// queue and handled here, while any of them is open: some hosts act on keys
// before a focused plugin window sees them. Ableton subclasses the plugin's
// window and keeps any key the plugin's VST3 view doesn't claim, so the key
// goes to our window procedure itself, not through DispatchMessage. JUCE,
// baseview and Visage do the same.
static HHOOK keyHook = nullptr;
static std::vector<HWND> ourWindows;
static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK keyHookProc(int code, WPARAM removed, LPARAM message)
{
    auto *m = reinterpret_cast<MSG *>(message);
    const bool keyboard = m->message == WM_KEYDOWN || m->message == WM_KEYUP || m->message == WM_SYSKEYDOWN
                       || m->message == WM_SYSKEYUP || m->message == WM_CHAR || m->message == WM_SYSCHAR;
    if (code == HC_ACTION && removed == PM_REMOVE && keyboard
        && std::find(ourWindows.begin(), ourWindows.end(), m->hwnd) != ourWindows.end())
    {
        TranslateMessage(m); // WM_CHAR for a WM_KEYDOWN, which comes back through here
        windowProc(m->hwnd, m->message, m->wParam, m->lParam);
        m->message = WM_NULL; // handled: the host's loop sees nothing
        return 0;
    }
    return CallNextHookEx(keyHook, code, removed, message);
}

void textInput(Window *, bool, View::Caret) {}

bool onScreen(Window *window) { return IsWindowVisible(window->hwnd) && !IsIconic(GetAncestor(window->hwnd, GA_ROOT)); }

void dragFiles(Window *window, const std::vector<std::string> &paths)
{
    window->dragging = paths;
    PostMessageW(window->hwnd, WM_APP_DRAG, 0, 0);
}

void copyText(Window *window, const std::string &utf8)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    HGLOBAL memory = size > 0 ? GlobalAlloc(GMEM_MOVEABLE, size_t(size) * sizeof(wchar_t)) : nullptr;
    if (!memory) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, static_cast<wchar_t *>(GlobalLock(memory)), size);
    GlobalUnlock(memory);
    if (OpenClipboard(window->hwnd))
    {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr; // the clipboard owns it now
        CloseClipboard();
    }
    if (memory) GlobalFree(memory);
}

// Ctrl+Z, Ctrl+Shift+Z or Y, A, X, C and V, for a text field being edited;
// false for the rest, which go on to the host (its own undo, for one).
static bool shortcut(Window *window, WPARAM virtualKey)
{
    if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0) return false;
    View::Key key;
    switch (virtualKey)
    {
    case 'V':
    {
        const std::string text = clipboardText(window->hwnd);
        return !text.empty() && window->callbacks.text(text.c_str());
    }
    case 'Z': key = GetKeyState(VK_SHIFT) < 0 ? View::Key::redo : View::Key::undo; break;
    case 'Y': key = View::Key::redo; break;
    case 'A': key = View::Key::selectAll; break;
    case 'X': key = View::Key::cut; break;
    case 'C': key = View::Key::copy; break;
    default: return false;
    }
    return window->callbacks.key(key, modifiers());
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *window = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!window) return DefWindowProcW(hwnd, message, wParam, lParam);
    auto pointer = [&](View::Pointer type, float wheel = 0) {
        window->callbacks.pointer({type, window->x, window->y, modifiers(), wheel});
    };
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
    {
        if (!window->locked)
            window->x = float(short(LOWORD(lParam))), window->y = float(short(HIWORD(lParam)));
        else if (message == WM_MOUSEMOVE)
        {
            // Move by how far the cursor got from the anchor, then put it back
            // (which sends one more WM_MOUSEMOVE, with no movement).
            POINT cursor;
            GetCursorPos(&cursor);
            if (cursor.x == window->anchor.x && cursor.y == window->anchor.y) return 0;
            window->x += float(cursor.x - window->anchor.x), window->y += float(cursor.y - window->anchor.y);
            SetCursorPos(window->anchor.x, window->anchor.y);
        }
    }
    switch (message)
    {
    case WM_TIMER: window->callbacks.frame(); return 0;
    case WM_APP_DRAG: dragOut(window); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd); // for keys
        SetCapture(hwnd);
        window->pressed = true;
        pointer(View::Pointer::down);
        return 0;
    case WM_MOUSEMOVE:
        if (!window->tracking) // ask to hear when it leaves
        {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            window->tracking = TrackMouseEvent(&track);
        }
        pointer(View::Pointer::move);
        return 0;
    case WM_MOUSELEAVE:
        window->tracking = false;
        if (!window->locked) pointer(View::Pointer::leave);
        return 0;
    case WM_LBUTTONUP:
        window->pressed = false;
        pointer(View::Pointer::up);
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED: // taken mid-drag (Alt+Tab, a host's dialog): the button-up won't come here
        if (window->pressed) window->pressed = false, pointer(View::Pointer::up);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS; // in dialog-based hosts, Tab, arrows and Enter too
    case WM_MOUSEWHEEL:
    {
        POINT at = {short(LOWORD(lParam)), short(HIWORD(lParam))}; // screen coordinates
        ScreenToClient(hwnd, &at);
        window->callbacks.pointer({View::Pointer::wheel, float(at.x), float(at.y), modifiers(),
                                   float(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA});
        return 0;
    }
    case WM_KEYDOWN:
    {
        View::Key key;
        if (shortcut(window, wParam)) return 0;
        if (keyFor(wParam, key) && window->callbacks.key(key, modifiers())) return 0;
        // Not ours: let the host's window have it (its shortcuts, transport).
        PostMessageW(GetParent(hwnd), message, wParam, lParam);
        return 0;
    }
    case WM_CHAR:
    {
        // Typed characters (TranslateMessage makes these from WM_KEYDOWN),
        // UTF-16 in, UTF-8 out. One beyond U+FFFF (an emoji) comes as two
        // messages, a surrogate pair; controls are skipped.
        const wchar_t c = wchar_t(wParam);
        if (c >= 0xD800 && c < 0xDC00)
        {
            window->highSurrogate = c;
            return 0;
        }
        const wchar_t pair[3] = {window->highSurrogate, c, 0};
        const bool paired = c >= 0xDC00 && c < 0xE000 && window->highSurrogate;
        window->highSurrogate = 0;
        if (c < 0x20 || c == 0x7F || (c >= 0xDC00 && c < 0xE000 && !paired)) return 0;
        const std::string utf8 = toUtf8(paired ? pair : pair + 1);
        if (!window->callbacks.text(utf8.c_str())) PostMessageW(GetParent(hwnd), message, wParam, lParam);
        return 0;
    }
    case WM_KEYUP: PostMessageW(GetParent(hwnd), message, wParam, lParam); return 0;
    case WM_ERASEBKGND: return 1; // Dawn paints every pixel
    case WM_PAINT: // uncovered or restored: the next frame draws it
        ValidateRect(hwnd, nullptr);
        window->callbacks.damaged();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    HWND host = static_cast<HWND>(parent->win32);
    if (!IsWindow(host)) return nullptr;
    static const wchar_t *className = L"ClapGpuView"; // per module: other plugins' classes don't clash
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = thisModule();
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass); // fails harmlessly when already registered

    auto *window = new Window{nullptr, std::move(callbacks)};
    // Our window takes its parent's DPI awareness: hosts that scale plugins
    // as DPI-unaware (Ableton's "Auto scale plugin window") would otherwise
    // get a view drawn at the screen's DPI into a 96 DPI window.
    const DPI_AWARENESS_CONTEXT awareness = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(host));
    window->hwnd = CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                   int(width), int(height), host, nullptr, thisModule(), nullptr);
    if (awareness) SetThreadDpiAwarenessContext(awareness);
    if (!window->hwnd)
    {
        delete window;
        return nullptr;
    }
    SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    SetTimer(window->hwnd, 1, 16, nullptr);
    if (ourWindows.empty()) keyHook = SetWindowsHookExW(WH_GETMESSAGE, keyHookProc, nullptr, GetCurrentThreadId());
    ourWindows.push_back(window->hwnd);
    // Files dragged in. OLE needs a single-threaded apartment, which hosts'
    // UI threads are; where it can't start, the view just takes no drops.
    const HRESULT ole = OleInitialize(nullptr);
    window->ole = SUCCEEDED(ole);
    if (window->ole)
    {
        window->dropTarget = new DropTarget(window);
        if (FAILED(RegisterDragDrop(window->hwnd, window->dropTarget))) window->dropTarget->Release(), window->dropTarget = nullptr;
    }
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

void lockPointer(Window *window, bool locked)
{
    if (window->locked == locked) return;
    window->locked = locked;
    if (locked) GetCursorPos(&window->anchor);
    ShowCursor(locked ? FALSE : TRUE);
}

void destroy(Window *window)
{
    lockPointer(window, false);
    std::erase(ourWindows, window->hwnd);
    if (ourWindows.empty() && keyHook) UnhookWindowsHookEx(keyHook), keyHook = nullptr;
    KillTimer(window->hwnd, 1);
    if (window->dropTarget) RevokeDragDrop(window->hwnd), window->dropTarget->Release();
    if (window->ole) OleUninitialize();
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

// Before the view has a window, the system's scale is the best guess.
float scale(Window *window) { return float(window ? GetDpiForWindow(window->hwnd) : GetDpiForSystem()) / 96.0f; }

void setVisible(Window *window, bool visible) { ShowWindow(window->hwnd, visible ? SW_SHOW : SW_HIDE); }

void pumpEvents(Window *) {}

// Settings > Personalization > Colors > "Choose your app mode".
bool darkMode(Window *)
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

}
