#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib.h>
#include "app.h"
#include "pipewire.h"
#include "ui.h"

static void activate(GtkApplication *app, gpointer user_data)
{
    AppData *data = user_data;
    build_gui(data);
    gtk_window_present(GTK_WINDOW(data->window));
}

int main(int argc, char *argv[])
{
    AppData data;
    init_app_data(&data);

    if (!init_pipewire(&data)) {
        return 1;
    }

    GThread *pw_thread = g_thread_new("pipewire", pipewire_thread, &data);

    GtkApplication *app = gtk_application_new("org.pipewire.mixer3d", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &data);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    pw_main_loop_quit(data.loop);
    g_thread_join(pw_thread);

    shutdown_pipewire(&data);

    g_object_unref(app);

    if (data.links) g_hash_table_destroy(data.links);
    if (data.ports) g_hash_table_destroy(data.ports);

    return status;
}
