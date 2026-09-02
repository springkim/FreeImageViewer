#include <gtk/gtk.h>

static void on_button_clicked(GtkButton *button, gpointer user_data) {
    static long long count = 1;
    count *= 2;

    char label[64];
    g_snprintf(label, sizeof(label), "클릭 횟수: %lld", count);
    gtk_button_set_label(button, label);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "GTK4 테스트");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 320);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_window_set_child(GTK_WINDOW(window), box);

    GtkWidget *label = gtk_label_new("안녕하세요, GTK4!");
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *button = gtk_button_new_with_label("눌러보세요");
    g_signal_connect(button, "clicked", G_CALLBACK(on_button_clicked), nullptr);
    gtk_box_append(GTK_BOX(box), button);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("org.example.gtk4test",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
