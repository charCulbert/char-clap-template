#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include "native.h"

// CHOC's Linux WebView is a WebKitGTK widget. It goes in a borderless GTK
// window that is re-parented into the host's X11 window before it is shown,
// and GTK's events are pumped from the host's timer. (A GtkPlug never paints
// there: hosts give a plain window, not an XEmbed socket.)
namespace webview::platform {

const char *const windowApi = CLAP_WINDOW_API_X11;
const bool needsTimer = true;

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

void *attach(void *view, const clap_window_t *parent)
{
    if (!parent->x11) return nullptr;
    GtkWidget *window = nullptr;
    ignoringXErrors([&] {
        GdkWindow *host = gdk_x11_window_foreign_new_for_display(gdk_display_get_default(), Window(parent->x11));
        if (!host) return;
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view)); // CHOC keeps its own reference
        gtk_widget_realize(window);
        gdk_window_reparent(gtk_widget_get_window(window), host, 0, 0);
        g_object_set_data_full(G_OBJECT(window), "host", host, g_object_unref);
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

void setSize(void *view, uint32_t width, uint32_t height)
{
    ignoringXErrors([&] {
        gtk_widget_set_size_request(GTK_WIDGET(view), int(width), int(height));
        GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(view));
        if (GTK_IS_WINDOW(top)) gtk_window_resize(GTK_WINDOW(top), int(width), int(height));
    });
}

void setVisible(void *view, bool visible)
{
    ignoringXErrors([&] {
        if (visible) gtk_widget_show(GTK_WIDGET(view));
        else gtk_widget_hide(GTK_WIDGET(view));
    });
}

float scale(void *) { return 1.0f; } // X11 hosts size in pixels, and GTK draws a pixel per point

void pumpEvents()
{
    // Bounded, so a busy page cannot hold the host's main thread.
    ignoringXErrors([] {
        for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i)
            g_main_context_iteration(nullptr, false);
    });
}

}
