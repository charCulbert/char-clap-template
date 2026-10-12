#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "native.h"

extern char** environ;

// Newer than some headers; the same numbers on every architecture but Alpha.
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

// The page is a WebKitGTK view in a helper process, DPF's way: the plugin
// starts the system's dynamic loader with its own binary as the program, and
// the binary's entry point (webview_helper_start, set by webview.cmake) runs
// GTK there. GTK 3 can't share a process with a GTK 2 host (Ardour hangs as
// soon as it starts), and GTK left in a host changes its locale and event
// handling. The helper finds GTK and WebKit at run time, so the plugin loads
// without them and only its page is missing. Plugin and helper talk over a
// socket pair; the page's files still come from the plugin's clap.webview.
// It needs glibc: the plugin finds the loader through glibc's _rtld_global,
// and glibc's loader runs a library as a program.
namespace webview::platform
{

const char* const windowApi = CLAP_WINDOW_API_X11;
const bool needsTimer = true;

namespace
{

constexpr const char* helperFlag = "--webview-helper";
constexpr int helperSocket = 3; // the helper's end of the socket pair, in the helper

// Each message: its length (4 bytes, counting the type), its type, its payload.
enum Message : uint8_t
{
    // plugin to helper
    Script = 1, // run before each document's own scripts
    Load,       // the page's URI
    Attach,     // the host's window (8-byte XID)
    Size,       // width and height in pixels (4 bytes each)
    Visible,    // 1 or 0 (1 byte)
    Evaluate,   // a script
    Reply,      // a request's number (4 bytes); if found, its MIME type, a zero byte, its bytes
    // helper to plugin
    Ready,   // pixels per page point (a float)
    Fetch,   // a request's number (4 bytes) and the path
    Receive, // base64 from window.clapWebviewReceive
};

template <typename T>
std::string bytesOf (const T& value)
{
    return std::string (reinterpret_cast<const char*> (&value), sizeof (T));
}

template <typename T>
T take (std::string_view& data)
{
    T value {};
    if (data.size() < sizeof (T))
        return value;
    std::memcpy (&value, data.data(), sizeof (T));
    data.remove_prefix (sizeof (T));
    return value;
}

// One end of the socket pair. Neither end ever waits for the other: the host's
// main thread mustn't stop because the helper has, and two ends each waiting
// to send (a big Reply against a Receive) would never move again. What the
// socket won't take yet waits in `out` for the next flush.
struct Channel
{
    int socket = -1;
    std::string in, out;
    size_t read = 0, written = 0; // how far each has been handled

    // So much unsent means the other end has stopped reading.
    static constexpr size_t maxWaiting = 64 << 20;

    // Queues a message and sends what the socket takes. False once the other
    // end has gone or stopped reading.
    bool send (Message type, std::string_view payload = {})
    {
        out += bytesOf (uint32_t (payload.size() + 1));
        out += char (type);
        out += payload;
        return flush();
    }

    bool flush()
    {
        while (written < out.size())
        {
            const ssize_t n = ::send (socket, out.data() + written, out.size() - written, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0)
                written += size_t (n);
            else if (n < 0 && errno == EINTR)
                continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return out.size() - written < maxWaiting;
            else
                return false;
        }
        out.clear();
        written = 0;
        return true;
    }

    bool waiting() const
    {
        return written < out.size();
    }

    // Reads whatever has arrived. False once the other end has gone.
    bool receive()
    {
        in.erase (0, read);
        read = 0;
        char chunk[65536];
        for (;;)
        {
            const ssize_t n = recv (socket, chunk, sizeof (chunk), MSG_DONTWAIT);
            if (n > 0)
                in.append (chunk, size_t (n));
            else if (n < 0 && errno == EINTR)
                continue;
            else
                return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }

    bool next (Message& type, std::string& payload)
    {
        if (in.size() - read < 5)
            return false;
        uint32_t length = 0;
        std::memcpy (&length, in.data() + read, 4);
        if (length == 0 || in.size() - read - 4 < length)
            return false;
        type = Message (in[read + 4]);
        payload.assign (in, read + 5, length - 1);
        read += 4 + length;
        return true;
    }
};

// ---- The plugin's side.

float lastScale = 1.0f; // a helper's, for asking before there is a page

std::string loaderPath()
{
    Dl_info info = {};
    void* loaderData = dlsym (RTLD_DEFAULT, "_rtld_global");
    if (!loaderData || !dladdr (loaderData, &info) || !info.dli_fname)
        return {};
    return info.dli_fname;
}

// The file this code is in: the .clap, the VST3's .so, or an executable.
std::string thisBinary()
{
    Dl_info info = {};
    link_map* map = nullptr;
    if (!dladdr1 (reinterpret_cast<const void*> (&thisBinary), &info, reinterpret_cast<void**> (&map), RTLD_DL_LINKMAP)
        || !map)
        return {};
    const char* path = map->l_name && *map->l_name ? map->l_name : "/proc/self/exe";
    char resolved[PATH_MAX];
    return realpath (path, resolved) ? resolved : "";
}

class HelperPage final : public Page
{
public:
    HelperPage (PageClient client_, int socket, pid_t helper_, int pidfd_)
        : client (std::move (client_))
        , helper (helper_)
        , pidfd (pidfd_)
    {
        channel.socket = socket;
    }

    ~HelperPage() override
    {
        close (channel.socket);
        stopHelper(); // its X windows go with it
    }

    // Sends the page, then waits a little for GTK to start in the helper and
    // give the display's scale. A slower helper carries on in the background,
    // with the last one's scale. False if the helper has quit (no WebKit, say).
    bool start()
    {
        for (const auto& script : client.scripts)
            send (Script, script);
        send (Load, client.uri);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (1);
        while (!ready && !gone)
        {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds> (deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                break;
            pollfd waiting = { channel.socket, short (POLLIN | (channel.waiting() ? POLLOUT : 0)), 0 };
            if (poll (&waiting, 1, int (left.count())) < 0 && errno != EINTR)
                return false;
            pumpEvents();
        }
        starting = false;
        return !gone;
    }

    bool attach (const clap_window_t* parent) override
    {
        if (attached || !parent->x11)
            return false;
        attached = true;
        return send (Attach, bytesOf (uint64_t (parent->x11)));
    }

    void setSize (uint32_t width, uint32_t height) override
    {
        send (Size, bytesOf (width) + bytesOf (height));
    }
    void setVisible (bool visible) override
    {
        send (Visible, bytesOf (uint8_t (visible)));
    }
    float scale() const override
    {
        return pixelScale;
    }
    bool evaluate (const std::string& script) override
    {
        return send (Evaluate, script);
    }
    bool lost() const override
    {
        return gone;
    }

    void pumpEvents() override
    {
        if (gone)
            return;
        const bool open = channel.flush() && channel.receive();
        Message type;
        std::string payload;
        while (channel.next (type, payload))
            handle (type, payload);
        if (!open)
            quit();
    }

private:
    bool send (Message type, std::string_view payload = {})
    {
        if (!gone && !channel.send (type, payload))
            quit();
        return !gone;
    }

    void handle (Message type, const std::string& payload)
    {
        std::string_view data = payload;
        if (type == Ready)
        {
            ready = true;
            const float scale = take<float> (data);
            if (scale <= 0)
                return;
            lastScale = scale;
            if (starting)
                pixelScale = scale; // later, the host has sized the window already
        }
        else if (type == Fetch)
        {
            const auto request = take<uint32_t> (data);
            std::string reply = bytesOf (request);
            if (const auto resource = client.fetch (std::string (data)))
                reply += resource->mime + '\0' + resource->bytes;
            send (Reply, reply);
        }
        else if (type == Receive)
            client.receive (payload);
    }

    // The helper has quit, crashed or stopped reading.
    void quit()
    {
        gone = true;
        stopHelper();
    }

    // By its pidfd where the kernel has them (Linux 5.3), so that a helper the
    // host has reaped (a SIGCHLD handler can) never takes another process's
    // number with it.
    void stopHelper()
    {
        if (helper <= 0)
            return;
        if (pidfd >= 0)
        {
            syscall (SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
            pollfd exited = { pidfd, POLLIN, 0 };
            poll (&exited, 1, 1000);
            siginfo_t info;
            if (waitid (idtype_t (3) /* P_PIDFD, Linux 5.4 */, id_t (pidfd), &info, WEXITED | WNOHANG) < 0
                && errno == EINVAL)
                waitpid (helper, nullptr, WNOHANG);
            close (pidfd);
        }
        else if (waitpid (helper, nullptr, WNOHANG) == 0) // still ours, and running
        {
            kill (helper, SIGKILL);
            while (waitpid (helper, nullptr, 0) < 0 && errno == EINTR)
            {
            }
        }
        helper = -1;
        pidfd = -1;
    }

    PageClient client;
    Channel channel;
    pid_t helper;
    int pidfd; // -1 before Linux 5.3
    float pixelScale = lastScale;
    bool starting = true, ready = false, attached = false, gone = false;
};

// ---- The helper's side: WebKitGTK, through functions looked up at run time.

struct Rectangle
{
    int x, y, width, height;
};

using SocketCallback = int (*) (int, unsigned, void*);
using RequestCallback = void (*) (void*, void*);

// The GTK, GDK, GLib, WebKit and Xlib functions the helper uses.
#define WEBVIEW_FUNCTIONS(F)                                                                            \
    F (void, gdk_set_allowed_backends, const char*)                                                     \
    F (int, gtk_init_check, int*, char***)                                                              \
    F (void, gtk_main, void)                                                                            \
    F (void, gtk_main_quit, void)                                                                       \
    F (void*, gtk_window_new, int)                                                                      \
    F (void, gtk_window_set_decorated, void*, int)                                                      \
    F (void, gtk_window_resize, void*, int, int)                                                        \
    F (void, gtk_container_add, void*, void*)                                                           \
    F (void, gtk_widget_realize, void*)                                                                 \
    F (void, gtk_widget_show, void*)                                                                    \
    F (void, gtk_widget_show_all, void*)                                                                \
    F (void, gtk_widget_hide, void*)                                                                    \
    F (void, gtk_widget_set_size_request, void*, int, int)                                              \
    F (int, gtk_widget_get_scale_factor, void*)                                                         \
    F (void*, gtk_widget_get_window, void*)                                                             \
    F (void*, gdk_display_get_default, void)                                                            \
    F (void*, gdk_screen_get_default, void)                                                             \
    F (double, gdk_screen_get_resolution, void*)                                                        \
    F (void*, gdk_get_default_root_window, void)                                                        \
    F (int, gdk_window_get_scale_factor, void*)                                                         \
    F (int, gdk_window_get_width, void*)                                                                \
    F (int, gdk_window_get_height, void*)                                                               \
    F (void, gdk_window_set_invalidate_handler, void*, void (*) (void*, void*))                         \
    F (void, gdk_x11_window_set_frame_sync_enabled, void*, int)                                         \
    F (unsigned long, gdk_x11_window_get_xid, void*)                                                    \
    F (void*, gdk_x11_display_get_xdisplay, void*)                                                      \
    F (void, gdk_x11_display_error_trap_push, void*)                                                    \
    F (int, gdk_x11_display_error_trap_pop, void*)                                                      \
    F (void, gdk_x11_display_error_trap_pop_ignored, void*)                                             \
    F (int, cairo_region_union_rectangle, void*, const Rectangle*)                                      \
    F (int, XReparentWindow, void*, unsigned long, unsigned long, int, int)                             \
    F (int, XSetInputFocus, void*, unsigned long, int, unsigned long)                                   \
    F (unsigned long, g_signal_connect_data, void*, const char*, void*, void*, void*, int)              \
    F (unsigned, g_unix_fd_add, int, unsigned, SocketCallback, void*)                                   \
    F (void*, g_object_ref, void*)                                                                      \
    F (void, g_object_unref, void*)                                                                     \
    F (void, g_free, void*)                                                                             \
    F (void*, g_bytes_new, const void*, size_t)                                                         \
    F (void, g_bytes_unref, void*)                                                                      \
    F (void*, g_memory_input_stream_new, void)                                                          \
    F (void*, g_memory_input_stream_new_from_bytes, void*)                                              \
    F (void*, soup_message_headers_new, int)                                                            \
    F (void, soup_message_headers_append, void*, const char*, const char*)                              \
    F (void*, webkit_web_context_get_default, void)                                                     \
    F (void, webkit_web_context_register_uri_scheme, void*, const char*, RequestCallback, void*, void*) \
    F (const char*, webkit_uri_scheme_request_get_path, void*)                                          \
    F (void, webkit_uri_scheme_request_finish_with_response, void*, void*)                              \
    F (void*, webkit_uri_scheme_response_new, void*, int64_t)                                           \
    F (void, webkit_uri_scheme_response_set_status, void*, unsigned, const char*)                       \
    F (void, webkit_uri_scheme_response_set_content_type, void*, const char*)                           \
    F (void, webkit_uri_scheme_response_set_http_headers, void*, void*)                                 \
    F (void*, webkit_user_content_manager_new, void)                                                    \
    F (int, webkit_user_content_manager_register_script_message_handler, void*, const char*)            \
    F (void, webkit_user_content_manager_add_script, void*, void*)                                      \
    F (void*, webkit_user_script_new, const char*, int, int, const char* const*, const char* const*)    \
    F (void, webkit_user_script_unref, void*)                                                           \
    F (void*, webkit_web_view_new_with_user_content_manager, void*)                                     \
    F (void*, webkit_web_view_get_settings, void*)                                                      \
    F (void, webkit_settings_set_javascript_can_access_clipboard, void*, int)                           \
    F (void, webkit_settings_set_enable_developer_extras, void*, int)                                   \
    F (void, webkit_web_view_load_uri, void*, const char*)                                              \
    F (void*, webkit_javascript_result_get_js_value, void*)                                             \
    F (char*, jsc_value_to_string, void*)

struct Helper
{
#define WEBVIEW_POINTER(result, name, ...) result (*name) (__VA_ARGS__) = nullptr;
    WEBVIEW_FUNCTIONS (WEBVIEW_POINTER)
#undef WEBVIEW_POINTER
    // WebKitGTK 2.40 renamed this from webkit_web_view_run_javascript.
    void (*evaluate) (void*, const char*, ssize_t, const char*, const char*, void*, void*, void*) = nullptr;
    void (*runJavascript) (void*, const char*, void*, void*, void*) = nullptr;

    Channel channel;
    bool writing = false; // waiting for the socket to take more
    void *display = nullptr, *window = nullptr, *view = nullptr, *scripts = nullptr;
    uint32_t nextRequest = 0;
    std::map<uint32_t, void*> requests; // WebKit's, waiting for the plugin's reply

    bool load()
    {
        if (!dlopen ("libwebkit2gtk-4.1.so.0", RTLD_NOW | RTLD_GLOBAL)
            && !dlopen ("libwebkit2gtk-4.0.so.37", RTLD_NOW | RTLD_GLOBAL))
            return false;
#define WEBVIEW_LOOKUP(result, name, ...)                                          \
    if (!(name = reinterpret_cast<decltype (name)> (dlsym (RTLD_DEFAULT, #name)))) \
        return false;
        WEBVIEW_FUNCTIONS (WEBVIEW_LOOKUP)
#undef WEBVIEW_LOOKUP
        evaluate = reinterpret_cast<decltype (evaluate)> (dlsym (RTLD_DEFAULT, "webkit_web_view_evaluate_javascript"));
        runJavascript =
            reinterpret_cast<decltype (runJavascript)> (dlsym (RTLD_DEFAULT, "webkit_web_view_run_javascript"));
        return evaluate || runJavascript;
    }

    // The host may destroy its window before the plugin's page goes, so GTK
    // can still talk to a window that no longer exists. GDK treats that X
    // error as fatal unless the requests are inside an error trap.
    template <typename Fn>
    void ignoringXErrors (Fn&& fn)
    {
        gdk_x11_display_error_trap_push (display);
        fn();
        gdk_x11_display_error_trap_pop_ignored (display);
    }

    void* xDisplay()
    {
        return gdk_x11_display_get_xdisplay (display);
    }
    unsigned long xWindow()
    {
        return gdk_x11_window_get_xid (gtk_widget_get_window (window));
    }

    void reply (Message type, std::string_view payload)
    {
        if (!channel.send (type, payload))
            gtk_main_quit();
        else if (channel.waiting() && !writing)
        {
            writing = true;
            g_unix_fd_add (channel.socket, 4 /* G_IO_OUT */, onWritable, this);
        }
    }

    // A borderless GTK window, re-parented into the host's X11 window once
    // the plugin has it. (A GtkPlug never paints there: hosts give a plain
    // window, not an XEmbed socket.)
    int run (int socket)
    {
        channel.socket = socket;
        gdk_set_allowed_backends ("x11"); // inside an X11 window, also under Wayland
        if (!gtk_init_check (nullptr, nullptr))
            return 1;
        display = gdk_display_get_default();

        // WebKit draws a CSS pixel as one GTK unit, times the font DPI over 96:
        // two pixels at 200%, from GTK's scale or (as REAPER sets it) the DPI.
        // The plugin waits for this, and the page can follow.
        const double dpi = gdk_screen_get_resolution (gdk_screen_get_default());
        const int factor = gdk_window_get_scale_factor (gdk_get_default_root_window());
        reply (Ready, bytesOf (float (factor * (dpi > 0 ? dpi / 96 : 1.0))));

        void* context = webkit_web_context_get_default();
        webkit_web_context_register_uri_scheme (context, "choc", onRequest, this, nullptr);
        scripts = webkit_user_content_manager_new();
        g_signal_connect_data (
            scripts, "script-message-received::external", reinterpret_cast<void*> (onScriptMessage), this, nullptr, 0);
        webkit_user_content_manager_register_script_message_handler (scripts, "external");
        addScript (
            "window.clapWebviewReceive = message => window.webkit.messageHandlers.external.postMessage(message);");
        view = webkit_web_view_new_with_user_content_manager (scripts);
        void* settings = webkit_web_view_get_settings (view);
        webkit_settings_set_javascript_can_access_clipboard (settings, true);
#ifndef NDEBUG
        webkit_settings_set_enable_developer_extras (settings, true); // right-click > Inspect
#endif

        ignoringXErrors ([&] {
            window = gtk_window_new (0 /* GTK_WINDOW_TOPLEVEL */);
            gtk_window_set_decorated (window, false);
            gtk_container_add (window, view);
            gtk_widget_realize (window);
            void* gdkWindow = gtk_widget_get_window (window);
            // Frame sync waits for the window manager to say each frame is drawn,
            // which it never does for a window inside another: GTK stops drawing.
            gdk_x11_window_set_frame_sync_enabled (gdkWindow, false);
            gdk_window_set_invalidate_handler (gdkWindow, invalidateAll);
        });
        g_signal_connect_data (view, "button-press-event", reinterpret_cast<void*> (takeFocus), this, nullptr, 0);

        g_unix_fd_add (socket, 1 | 8 | 16 /* G_IO_IN | G_IO_ERR | G_IO_HUP */, onSocket, this);
        gtk_main();
        return 0;
    }

    void addScript (const char* source)
    {
        void* script = webkit_user_script_new (source, 1 /* top frame */, 0 /* at document start */, nullptr, nullptr);
        webkit_user_content_manager_add_script (scripts, script);
        webkit_user_script_unref (script);
    }

    void handle (Message type, const std::string& payload)
    {
        std::string_view data = payload;
        switch (type)
        {
            case Script:
                addScript (payload.c_str());
                break;
            case Load:
                webkit_web_view_load_uri (view, payload.c_str());
                break;
            case Attach:
            {
                // Re-parented behind GDK's back, so it stays a toplevel to GTK.
                // Into a window that has gone, the page would show loose on the
                // desktop, so the helper quits instead. (Ardour's goes with the
                // page in it, when that page's helper dies.)
                gdk_x11_display_error_trap_push (display);
                XReparentWindow (xDisplay(), xWindow(), take<uint64_t> (data), 0, 0);
                if (gdk_x11_display_error_trap_pop (display) != 0)
                {
                    gtk_main_quit();
                    break;
                }
                ignoringXErrors ([&] { gtk_widget_show_all (window); });
                break;
            }
            case Size:
            {
                const auto width = take<uint32_t> (data), height = take<uint32_t> (data);
                // X11 hosts size in pixels, and GTK in its own units: one per
                // pixel, or two on a desktop scaled to 200%.
                const uint32_t factor = uint32_t (gtk_widget_get_scale_factor (view));
                const int w = int ((width + factor - 1) / factor), h = int ((height + factor - 1) / factor);
                ignoringXErrors ([&] {
                    gtk_widget_set_size_request (view, w, h);
                    gtk_window_resize (window, w, h);
                });
                break;
            }
            case Visible:
                ignoringXErrors ([&] {
                    if (take<uint8_t> (data))
                        gtk_widget_show (view);
                    else
                        gtk_widget_hide (view);
                });
                break;
            case Evaluate:
                if (evaluate)
                    evaluate (
                        view, payload.data(), ssize_t (payload.size()), nullptr, nullptr, nullptr, nullptr, nullptr);
                else
                    runJavascript (view, payload.c_str(), nullptr, nullptr, nullptr);
                break;
            case Reply:
            {
                const auto found = requests.find (take<uint32_t> (data));
                if (found == requests.end())
                    break;
                finish (found->second, data);
                requests.erase (found);
                break;
            }
            default:
                break;
        }
    }

    // Answers WebKit's request with the plugin's reply: a MIME type, a zero
    // byte and the file, or nothing for a 404.
    void finish (void* request, std::string_view reply)
    {
        const size_t split = reply.find ('\0');
        void *stream = nullptr, *response = nullptr;
        if (split == std::string_view::npos)
        {
            stream = g_memory_input_stream_new();
            response = webkit_uri_scheme_response_new (stream, -1);
            webkit_uri_scheme_response_set_status (response, 404, nullptr);
        }
        else
        {
            const std::string mime (reply.substr (0, split));
            const auto bytes = reply.substr (split + 1);
            void* data = g_bytes_new (bytes.data(), bytes.size());
            stream = g_memory_input_stream_new_from_bytes (data);
            g_bytes_unref (data);
            response = webkit_uri_scheme_response_new (stream, int64_t (bytes.size()));
            webkit_uri_scheme_response_set_status (response, 200, nullptr);
            webkit_uri_scheme_response_set_content_type (response, mime.c_str());
            void* headers = soup_message_headers_new (1 /* SOUP_MESSAGE_HEADERS_RESPONSE */);
            soup_message_headers_append (headers, "Cache-Control", "no-store");
            soup_message_headers_append (headers, "Access-Control-Allow-Origin", "*");
            webkit_uri_scheme_response_set_http_headers (response, headers); // takes the headers
        }
        webkit_uri_scheme_request_finish_with_response (request, response);
        g_object_unref (stream);
        g_object_unref (response);
        g_object_unref (request);
    }

    static int onSocket (int, unsigned, void* data)
    {
        auto& helper = *static_cast<Helper*> (data);
        const bool open = helper.channel.receive();
        Message type;
        std::string payload;
        while (helper.channel.next (type, payload))
            helper.handle (type, payload);
        if (!open)
            helper.gtk_main_quit(); // the plugin closed the page
        return open;
    }

    static int onWritable (int, unsigned, void* data)
    {
        auto& helper = *static_cast<Helper*> (data);
        if (!helper.channel.flush())
            helper.gtk_main_quit();
        helper.writing = helper.channel.waiting();
        return helper.writing;
    }

    // The page's files come from the plugin, which answers on its timer.
    static void onRequest (void* request, void* data)
    {
        auto& helper = *static_cast<Helper*> (data);
        const uint32_t number = ++helper.nextRequest;
        helper.requests[number] = helper.g_object_ref (request);
        helper.reply (Fetch, bytesOf (number) + helper.webkit_uri_scheme_request_get_path (request));
    }

    static void onScriptMessage (void*, void* result, void* data)
    {
        auto& helper = *static_cast<Helper*> (data);
        char* text = helper.jsc_value_to_string (helper.webkit_javascript_result_get_js_value (result));
        helper.reply (Receive, text);
        helper.g_free (text);
    }

    // No window manager gives an embedded window the keyboard, so a click takes it.
    static int takeFocus (void*, void*, void* data)
    {
        auto& helper = *static_cast<Helper*> (data);
        helper.ignoringXErrors (
            [&] { helper.XSetInputFocus (helper.xDisplay(), helper.xWindow(), 2 /* RevertToParent */, 0); });
        return false;
    }

    // WebKitGTK 2.52's GTK 3 view paints the part of the page a redraw covers
    // upside down: a strip at the bottom gets the rows from the top. A host
    // that shows more of the window after it first drew (REAPER, when a taller
    // plugin takes a shorter one's place) then leaves that strip empty. So any
    // redraw redraws the whole window.
    static void invalidateAll (void* window, void* region);
};

Helper* theHelper = nullptr;

void Helper::invalidateAll (void* gdkWindow, void* region)
{
    const Rectangle all = { 0, 0, theHelper->gdk_window_get_width (gdkWindow),
        theHelper->gdk_window_get_height (gdkWindow) };
    theHelper->cairo_region_union_rectangle (region, &all);
}

[[noreturn]] void runHelper (const char* parent)
{
    // Gone with the plugin's process, even if that crashes. (Strictly, with
    // the thread that started it: the host's main thread.)
    prctl (PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != pid_t (std::atol (parent)))
        _exit (0);
    // Nothing of the host's but the socket, where the spawn couldn't see to
    // that itself (before glibc 2.34).
    syscall (SYS_close_range, helperSocket + 1, ~0u, 0);
    static Helper helper;
    theHelper = &helper;
    _exit (helper.load() ? helper.run (helperSocket) : 1);
}

// In an executable (a standalone app) the loader starts main, not the entry
// point below, so catch the helper's arguments before main runs.
__attribute__ ((constructor)) void helperInExecutable (int argc, char** argv, char**)
{
    if (argc == 3 && std::strcmp (argv[1], helperFlag) == 0)
        runHelper (argv[2]);
}

} // namespace

float defaultScale()
{
    return lastScale;
}

std::unique_ptr<Page> openPage (PageClient client)
{
    const std::string loader = loaderPath(), binary = thisBinary();
    if (loader.empty() || binary.empty())
        return nullptr;
    // Both ends close on exec, so a fork on another of the host's threads
    // can't take one. The spawn gives the helper its end, at a fixed number,
    // and nothing else.
    int ends[2];
    if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) != 0)
        return nullptr;
    if (ends[1] == helperSocket) // a dup2 onto itself would leave it closing on exec
    {
        const int moved = fcntl (ends[1], F_DUPFD_CLOEXEC, helperSocket + 1);
        close (ends[1]);
        ends[1] = moved;
    }
    posix_spawn_file_actions_t actions;
    if (ends[1] < 0 || posix_spawn_file_actions_init (&actions) != 0)
    {
        close (ends[0]);
        if (ends[1] >= 0)
            close (ends[1]);
        return nullptr;
    }
    posix_spawn_file_actions_adddup2 (&actions, ends[1], helperSocket);
    using CloseFrom = int (*) (posix_spawn_file_actions_t*, int);
    if (auto closeFrom = reinterpret_cast<CloseFrom> (dlsym (RTLD_DEFAULT, "posix_spawn_file_actions_addclosefrom_np")))
        closeFrom (&actions, helperSocket + 1); // glibc 2.34; before that, the helper does it
    // Nor the host's blocked or ignored signals (a SIGCHLD the host ignores
    // would break GLib's child watches).
    posix_spawnattr_t attributes;
    posix_spawnattr_init (&attributes);
    sigset_t none, all;
    sigemptyset (&none);
    sigfillset (&all);
    posix_spawnattr_setsigmask (&attributes, &none);
    posix_spawnattr_setsigdefault (&attributes, &all);
    posix_spawnattr_setflags (&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const std::string parent = std::to_string (getpid());
    const char* arguments[] = { loader.c_str(), binary.c_str(), helperFlag, parent.c_str(), nullptr };
    // Without the host's library path or preloads: a GLib or GTK of the
    // host's own must not stand in for the system's.
    std::vector<char*> environment;
    for (char** variable = environ; *variable; ++variable)
        if (std::strncmp (*variable, "LD_LIBRARY_PATH=", 16) != 0 && std::strncmp (*variable, "LD_PRELOAD=", 11) != 0)
            environment.push_back (*variable);
    environment.push_back (nullptr);
    pid_t helper = -1;
    const int error = posix_spawn (
        &helper, loader.c_str(), &actions, &attributes, const_cast<char**> (arguments), environment.data());
    posix_spawnattr_destroy (&attributes);
    posix_spawn_file_actions_destroy (&actions);
    close (ends[1]);
    if (error != 0)
    {
        close (ends[0]);
        return nullptr;
    }
    const int pidfd = int (syscall (SYS_pidfd_open, helper, 0)); // -1 before Linux 5.3
    auto page = std::make_unique<HelperPage> (std::move (client), ends[0], helper, pidfd);
    if (!page->start())
        return nullptr;
    return page;
}

} // namespace webview::platform

// The helper's entry point, where the dynamic loader starts the plugin's
// binary run as a program. Nothing has set up argc and argv for C here (nor
// run this binary's static constructors), so the arguments come from /proc.
#if defined(__x86_64__) || defined(__i386__)
#define WEBVIEW_ENTRY __attribute__ ((used, force_align_arg_pointer)) // no return address on the stack
#else
#define WEBVIEW_ENTRY __attribute__ ((used))
#endif
extern "C" [[noreturn]] WEBVIEW_ENTRY void webview_helper_start()
{
    std::string line;
    const int file = open ("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    char chunk[4096];
    for (ssize_t n; file >= 0 && (n = read (file, chunk, sizeof (chunk))) > 0;)
        line.append (chunk, size_t (n));
    if (file >= 0)
        close (file);
    std::vector<const char*> arguments;
    for (size_t at = 0; at < line.size(); at += std::strlen (line.c_str() + at) + 1)
        arguments.push_back (line.c_str() + at);
    for (size_t i = 0; i + 1 < arguments.size(); ++i)
        if (std::strcmp (arguments[i], webview::platform::helperFlag) == 0)
            webview::platform::runHelper (arguments[i + 1]);
    _exit (1);
}
