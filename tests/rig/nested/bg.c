// Background-layer wallpaper for nested tests (gtk4-layer-shell).
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
static void act(GtkApplication* app, gpointer p) {
    GtkWidget* w = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(w));
    gtk_layer_set_layer(GTK_WINDOW(w), GTK_LAYER_SHELL_LAYER_BACKGROUND);
    for (int e = 0; e < 4; ++e) gtk_layer_set_anchor(GTK_WINDOW(w), e, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(w), -1);
    GtkWidget* pic = gtk_picture_new_for_filename((const char*)p);
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_window_set_child(GTK_WINDOW(w), pic);
    gtk_window_present(GTK_WINDOW(w));
}
int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("test.nested.bg", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(act), argv[1]);
    return g_application_run(G_APPLICATION(app), 1, argv);
}
