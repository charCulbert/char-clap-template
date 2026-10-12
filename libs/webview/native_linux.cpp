#include <dlfcn.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <link.h>
#include <string>
#include <string_view>
#include "native.h"

// CHOC's Linux WebView is a WebKitGTK widget. It goes in a borderless GTK
// window that is re-parented into the host's X11 window before it is shown,
// and GTK's events are pumped from the host's timer. (A GtkPlug never paints
// there: hosts give a plain window, not an XEmbed socket.)
namespace webview::platform {

const char *const windowApi = CLAP_WINDOW_API_X11;
const bool needsTimer = true;

// gtk_init sets GDK's one event handler to GTK's. In a host that draws with
// GDK but not GTK (REAPER, through SWELL), that takes every event from the
// host's own windows, and the host stops drawing and responding. SWELL can
// start GTK itself and keep its handler, passing GTK the events, so ask it to
// before CHOC starts GTK. SWELL is loaded privately, so look it up by path.
void prepare()
{
    static bool done = false;
    if (done) return;
    done = true;
    std::string swell;
    dl_iterate_phdr([](dl_phdr_info *info, size_t, void *out) {
        const std::string_view path = info->dlpi_name ? info->dlpi_name : "";
        if (!path.ends_with("/libSwell.so")) return 0;
        *static_cast<std::string *>(out) = path;
        return 1;
    }, &swell);
    if (swell.empty()) return;
    void *library = dlopen(swell.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!library) return;
    using GetFunc = void *(*)(const char *);
    using ExtendedApi = void *(*)(const char *, void *);
    if (const auto getFunc = reinterpret_cast<GetFunc>(dlsym(library, "SWELLAPI_GetFunc")))
        if (const auto extendedApi = reinterpret_cast<ExtendedApi>(getFunc("SWELL_ExtendedAPI")))
            extendedApi("LOAD_GTK", nullptr);
    dlclose(library);
}

// Hosts may destroy their window before clap.gui.destroy (clap-wrapper's VST3
// view does), so GTK can still talk to a window that no longer exists. GDK
// treats that X error as fatal unless our requests are inside an error trap.
template <typename Fn>
static void ignoringXErrors(Fn &&fn)
{
    GdkDisplay *display = gdk_display_get_default();
    gdk_x11_display_error_trap_push(display);
    fn();
    gdk_x11_display_error_trap_pop_ignored(display);
}

// No window manager gives an embedded window the keyboard, so a click takes it.
static gboolean takeFocus(GtkWidget *, GdkEvent *, gpointer window)
{
    ignoringXErrors([&] {
        GdkWindow *gdkWindow = gtk_widget_get_window(GTK_WIDGET(window));
        XSetInputFocus(GDK_WINDOW_XDISPLAY(gdkWindow), GDK_WINDOW_XID(gdkWindow), RevertToParent, CurrentTime);
    });
    return FALSE;
}

// WebKitGTK 2.52's GTK 3 view paints the part of the page a redraw covers
// upside down: a strip at the bottom gets the rows from the top. A host that
// shows more of the window after it first drew (REAPER, when a taller plugin
// takes a shorter one's place) then leaves that strip empty. So any redraw
// redraws the whole window.
static void invalidateAll(GdkWindow *window, cairo_region_t *region)
{
    const cairo_rectangle_int_t all = {0, 0, gdk_window_get_width(window), gdk_window_get_height(window)};
    cairo_region_union_rectangle(region, &all);
}

void *attach(void *view, const clap_window_t *parent)
{
    if (!parent->x11) return nullptr;
    GtkWidget *window = nullptr;
    ignoringXErrors([&] {
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view)); // CHOC keeps its own reference
        gtk_widget_realize(window);
        GdkWindow *gdkWindow = gtk_widget_get_window(window);
        // Frame sync waits for the window manager to say each frame is drawn,
        // which it never does for a window inside another: GTK stops drawing.
        gdk_x11_window_set_frame_sync_enabled(gdkWindow, FALSE);
        gdk_window_set_invalidate_handler(gdkWindow, invalidateAll);
        // Re-parented behind GDK's back, so it stays a toplevel to GTK. In a
        // host that is itself GTK (clap-host), gdk_window_reparent would make
        // it a child of the host's own window, and the window then took
        // seconds longer to show.
        XReparentWindow(GDK_WINDOW_XDISPLAY(gdkWindow), GDK_WINDOW_XID(gdkWindow), Window(parent->x11), 0, 0);
        g_signal_connect(view, "button-press-event", G_CALLBACK(takeFocus), window);
        gtk_widget_show_all(window);
    });
    return window;
}

void detach(void *view, void *window)
{
    ignoringXErrors([&] {
        g_signal_handlers_disconnect_by_func(view, (gpointer)takeFocus, window);
        gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(view));
        gtk_widget_destroy(GTK_WIDGET(window));
    });
}

// X11 hosts size in pixels, and GTK in its own units: one per pixel, or two
// on a desktop scaled to 200%.
void setSize(void *view, uint32_t width, uint32_t height)
{
    ignoringXErrors([&] {
        const uint32_t factor = uint32_t(gtk_widget_get_scale_factor(GTK_WIDGET(view)));
        const int w = int((width + factor - 1) / factor), h = int((height + factor - 1) / factor);
        gtk_widget_set_size_request(GTK_WIDGET(view), w, h);
        GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(view));
        if (GTK_IS_WINDOW(top)) gtk_window_resize(GTK_WINDOW(top), w, h);
    });
}

void setVisible(void *view, bool visible)
{
    ignoringXErrors([&] {
        if (visible) gtk_widget_show(GTK_WIDGET(view));
        else gtk_widget_hide(GTK_WIDGET(view));
    });
}

// WebKit draws a CSS pixel as one GTK unit, times the font DPI over 96. At
// 200% a GTK host has two pixels per unit at 96 DPI; REAPER keeps GDK at one
// pixel per unit, and its DPI is 192.
float scale(void *view)
{
    GdkScreen *screen = gdk_screen_get_default();
    if (!screen) return 1.0f;
    const double dpi = gdk_screen_get_resolution(screen);
    GdkMonitor *monitor = gdk_display_get_monitor(gdk_screen_get_display(screen), 0);
    const int factor = view ? gtk_widget_get_scale_factor(GTK_WIDGET(view)) : monitor ? gdk_monitor_get_scale_factor(monitor) : 1;
    return float(factor * (dpi > 0 ? dpi / 96 : 1.0));
}

void pumpEvents()
{
    // Bounded, so a busy page cannot hold the host's main thread.
    ignoringXErrors([] {
        for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i)
            g_main_context_iteration(nullptr, false);
    });
}

}
