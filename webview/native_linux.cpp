#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include "native.h"

// CHOC's Linux WebView is a WebKitGTK widget. A GtkPlug puts it inside the
// host's X11 window, and GTK's events are pumped from the host's timer.
namespace webview::platform {

const char *const windowApi = CLAP_WINDOW_API_X11;
const bool needsTimer = true;

void *attach(void *view, const clap_window_t *parent)
{
    if (!parent->x11) return nullptr;
    GtkWidget *plug = gtk_plug_new(Window(parent->x11));
    gtk_container_add(GTK_CONTAINER(plug), GTK_WIDGET(view)); // CHOC keeps its own reference
    gtk_widget_show_all(plug);
    return plug;
}

void detach(void *view, void *plug)
{
    gtk_container_remove(GTK_CONTAINER(plug), GTK_WIDGET(view));
    gtk_widget_destroy(GTK_WIDGET(plug));
}

void setSize(void *view, uint32_t width, uint32_t height)
{
    gtk_widget_set_size_request(GTK_WIDGET(view), int(width), int(height));
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(view));
    if (GTK_IS_PLUG(top)) gtk_window_resize(GTK_WINDOW(top), int(width), int(height));
}

void setVisible(void *view, bool visible)
{
    if (visible) gtk_widget_show(GTK_WIDGET(view));
    else gtk_widget_hide(GTK_WIDGET(view));
}

void pumpEvents()
{
    // Bounded, so a busy page cannot hold the host's main thread.
    for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i)
        g_main_context_iteration(nullptr, false);
}

}
