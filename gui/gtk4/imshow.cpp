#include <gtk/gtk.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <decoder/image_decoder.h>

#include "imshow.h"

namespace {

constexpr int minimum_frame_delay_ms = 10;

struct DisplayFrame {
    GdkTexture* texture = nullptr;
    guint delay_ms = minimum_frame_delay_ms;
};

struct ViewerState {
    GtkApplication* application = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* picture = nullptr;
    std::vector<DisplayFrame> frames;
    std::size_t frame_index = 0;
    guint animation_source_id = 0;
    int image_width = 0;
    int image_height = 0;
    std::string title;

    ViewerState() = default;
    ViewerState(const ViewerState&) = delete;
    ViewerState& operator=(const ViewerState&) = delete;

    ~ViewerState() {
        if (animation_source_id != 0) {
            g_source_remove(animation_source_id);
        }
        for (DisplayFrame& frame : frames) {
            g_clear_object(&frame.texture);
        }
    }
};

std::size_t required_pixel_bytes(const int width, const int height) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("imshow: invalid image dimensions");
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h / 4) {
        throw std::runtime_error("imshow: image dimensions are too large");
    }
    return w * h * 4;
}

DisplayFrame make_display_frame(const std::vector<std::uint8_t>& rgba,
                                const int width,
                                const int height,
                                const std::size_t byte_count,
                                const int delay_ms) {
    if (rgba.size() < byte_count) {
        throw std::runtime_error("imshow: decoded pixel buffer is too small");
    }

    GBytes* bytes = g_bytes_new(rgba.data(), byte_count);
    GdkTexture* texture = GDK_TEXTURE(gdk_memory_texture_new(
            width, height, GDK_MEMORY_R8G8B8A8, bytes,
            static_cast<std::size_t>(width) * 4));
    g_bytes_unref(bytes);
    if (texture == nullptr) {
        throw std::runtime_error("imshow: failed to create a GTK texture");
    }

    return {texture, static_cast<guint>(std::max(delay_ms, minimum_frame_delay_ms))};
}

void populate_viewer_state(ViewerState& state, const DecodedImage& decoded,
                           const char* image_path) {
    state.image_width = decoded.width;
    state.image_height = decoded.height;

    gchar* basename = g_path_get_basename(image_path);
    state.title = basename != nullptr && basename[0] != '\0'
                          ? basename
                          : "FreeImageViewer";
    g_free(basename);

    const std::size_t byte_count = required_pixel_bytes(decoded.width, decoded.height);
    if (decoded.animated()) {
        state.frames.reserve(decoded.frames.size());
        for (const ImageFrame& frame : decoded.frames) {
            state.frames.push_back(make_display_frame(
                    frame.pixels, decoded.width, decoded.height,
                    byte_count, frame.delay_ms));
        }
    } else {
        state.frames.push_back(make_display_frame(
                decoded.pixels, decoded.width, decoded.height,
                byte_count, minimum_frame_delay_ms));
    }
}

void schedule_next_frame(ViewerState* state);

gboolean show_next_frame(gpointer user_data) {
    auto* state = static_cast<ViewerState*>(user_data);
    state->animation_source_id = 0;
    if (state->frames.size() < 2 || state->picture == nullptr) {
        return G_SOURCE_REMOVE;
    }

    state->frame_index = (state->frame_index + 1) % state->frames.size();
    gtk_picture_set_paintable(
            GTK_PICTURE(state->picture),
            GDK_PAINTABLE(state->frames[state->frame_index].texture));
    schedule_next_frame(state);
    return G_SOURCE_REMOVE;
}

void schedule_next_frame(ViewerState* state) {
    if (state->frames.size() < 2 || state->animation_source_id != 0) {
        return;
    }
    state->animation_source_id = g_timeout_add(
            state->frames[state->frame_index].delay_ms,
            show_next_frame, state);
}

gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint,
                        GdkModifierType, gpointer user_data) {
    if (keyval == GDK_KEY_space) {
        auto* state = static_cast<ViewerState*>(user_data);
        gtk_window_close(GTK_WINDOW(state->window));
        return TRUE;
    }
    return FALSE;
}

void on_window_destroy(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<ViewerState*>(user_data);
    state->window = nullptr;
    state->picture = nullptr;
    if (state->animation_source_id != 0) {
        g_source_remove(state->animation_source_id);
        state->animation_source_id = 0;
    }
}

void on_activate(GtkApplication* application, gpointer user_data) {
    auto* state = static_cast<ViewerState*>(user_data);
    state->application = application;
    state->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state->window), state->title.c_str());
    gtk_window_set_default_size(
            GTK_WINDOW(state->window), state->image_width, state->image_height);

    state->picture = gtk_picture_new_for_paintable(
            GDK_PAINTABLE(state->frames.front().texture));
    gtk_picture_set_can_shrink(GTK_PICTURE(state->picture), TRUE);
#if GTK_CHECK_VERSION(4, 8, 0)
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture), GTK_CONTENT_FIT_CONTAIN);
#else
    gtk_picture_set_keep_aspect_ratio(GTK_PICTURE(state->picture), TRUE);
#endif
    gtk_widget_set_hexpand(state->picture, TRUE);
    gtk_widget_set_vexpand(state->picture, TRUE);
    gtk_window_set_child(GTK_WINDOW(state->window), state->picture);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), state);
    gtk_widget_add_controller(state->window, keys);
    g_signal_connect(state->window, "destroy", G_CALLBACK(on_window_destroy), state);

    gtk_window_present(GTK_WINDOW(state->window));
    schedule_next_frame(state);
}

} // namespace

void imshow(const char* image_path) {
    if (image_path == nullptr || image_path[0] == '\0') {
        throw std::invalid_argument("imshow: image_path is empty");
    }

    const DecodedImage decoded = decode_image(std::string(image_path), true);
    if (!decoded.ok) {
        throw std::runtime_error("imshow: " + decoded.error);
    }
    ViewerState state;
    populate_viewer_state(state, decoded, image_path);

    GtkApplication* application = gtk_application_new(
            "io.github.freeimageviewer.imshow", G_APPLICATION_NON_UNIQUE);
    if (application == nullptr) {
        throw std::runtime_error("imshow: failed to create a GTK application");
    }

    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &state);
    const int status = g_application_run(G_APPLICATION(application), 0, nullptr);
    g_object_unref(application);
    state.application = nullptr;

    if (status != 0) {
        throw std::runtime_error(
                "imshow: GTK application exited with status " + std::to_string(status));
    }
}
