#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <gtk/gtk.h>
#include "ui.h"
#include "pipewire.h"

static const double COLORS[MAX_SOURCES][3] = {
    {0.2, 0.8, 0.9},  /* Cyan */
    {0.9, 0.5, 0.2},  /* Orange */
    {0.4, 0.9, 0.4},  /* Green */
    {0.9, 0.3, 0.8}   /* Magenta */
};

void refresh_canvas(AppData *data)
{
    if (data->canvas) {
        gtk_widget_queue_draw(data->canvas);
    }
}

static void stereo_positions(const AppData *data, int idx, double *out_lx, double *out_ly, double *out_rx, double *out_ry)
{
    if (!data->sources[idx].active || !data->sources[idx].is_playing) {
        if (out_lx) *out_lx = 0;
        if (out_ly) *out_ly = 0;
        if (out_rx) *out_rx = 0;
        if (out_ry) *out_ry = 0;
        return;
    }

    float center = data->sources[idx].azimuth;
    float half = data->sources[idx].width * 0.5f;
    float az_l = center - half;
    float az_r = center + half;

    if (az_l < 0.0f) az_l += 360.0f;
    if (az_r >= 360.0f) az_r -= 360.0f;

    float radius_pct = data->sources[idx].radius / 100.0f;
    float radius_px = radius_pct * MAX_RADIUS;

    float angle_l = (az_l - 90.0f) * M_PI / 180.0f;
    float angle_r = (az_r - 90.0f) * M_PI / 180.0f;

    if (out_lx) *out_lx = CENTER_X + radius_px * cos(angle_l);
    if (out_ly) *out_ly = CENTER_Y + radius_px * sin(angle_l);
    if (out_rx) *out_rx = CENTER_X + radius_px * cos(angle_r);
    if (out_ry) *out_ry = CENTER_Y + radius_px * sin(angle_r);
}

static int find_source_at_point(AppData *data, double x, double y)
{
    const double grab_radius = 14.0;
    for (int i = 0; i < MAX_SOURCES; i++) {
        double lx = 0, ly = 0, rx = 0, ry = 0;
        if (!data->sources[i].active || !data->sources[i].is_playing)
            continue;

        stereo_positions(data, i, &lx, &ly, &rx, &ry);

        double dxL = x - lx, dyL = y - ly;
        double dxR = x - rx, dyR = y - ry;
        if (sqrt(dxL * dxL + dyL * dyL) <= grab_radius ||
            sqrt(dxR * dxR + dyR * dyR) <= grab_radius) {
            return i;
        }
    }
    return -1;
}

static void set_source_position_from_point(AppData *data, int source_idx, double x, double y)
{
    float dx = x - CENTER_X;
    float dy = y - CENTER_Y;

    float radius_px = sqrtf(dx * dx + dy * dy);
    float radius_pct = (radius_px / MAX_RADIUS) * 100.0f;
    if (radius_pct > 100.0f) radius_pct = 100.0f;
    if (radius_pct < MIN_RADIUS_PCT) radius_pct = MIN_RADIUS_PCT;

    float azimuth = atan2f(dy, dx) * 180.0f / (float)M_PI + 90.0f;
    if (azimuth < 0) azimuth += 360.0f;

    update_source_position(data, source_idx, azimuth, radius_pct);
}

static void draw_canvas(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    AppData *data = user_data;

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.15);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 0.5);
    cairo_set_line_width(cr, 1.0);

    for (int i = 1; i <= 3; i++) {
        double r = (MAX_RADIUS / 3.0) * i;
        cairo_arc(cr, CENTER_X, CENTER_Y, r, 0, 2 * M_PI);
        cairo_stroke(cr);

        char label[16];
        snprintf(label, sizeof(label), "%dm", i * 33);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_move_to(cr, CENTER_X + r - 20, CENTER_Y - 5);
        cairo_show_text(cr, label);
    }

    cairo_set_source_rgb(cr, 0.5, 0.5, 0.6);
    cairo_set_font_size(cr, 14);

    cairo_move_to(cr, CENTER_X - 5, 20);
    cairo_show_text(cr, "N");
    cairo_move_to(cr, CENTER_X - 5, CANVAS_SIZE - 10);
    cairo_show_text(cr, "S");
    cairo_move_to(cr, 10, CENTER_Y + 5);
    cairo_show_text(cr, "W");
    cairo_move_to(cr, CANVAS_SIZE - 20, CENTER_Y + 5);
    cairo_show_text(cr, "E");

    /* Draw head with ears */
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.95);
    cairo_arc(cr, CENTER_X, CENTER_Y, 9, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, CENTER_X - 13, CENTER_Y, 4, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, CENTER_X + 13, CENTER_Y, 4, 0, 2 * M_PI);
    cairo_fill(cr);

    for (int i = 0; i < MAX_SOURCES; i++) {
        if (!data->sources[i].active || !data->sources[i].is_playing)
            continue;

        double lx=0, ly=0, rx=0, ry=0;
        stereo_positions(data, i, &lx, &ly, &rx, &ry);

        cairo_set_source_rgba(cr, COLORS[i][0], COLORS[i][1], COLORS[i][2], 0.3);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, CENTER_X, CENTER_Y);
        cairo_line_to(cr, lx, ly);
        cairo_move_to(cr, CENTER_X, CENTER_Y);
        cairo_line_to(cr, rx, ry);
        cairo_stroke(cr);

        /* Draw L/R markers; if width is 0 they overlap */
        float elev = data->sources[i].elevation;
        double bright = 0.55 + ((elev + 90.0) / 180.0) * 0.45; /* 0.55..1.0 */
        double radius = 10.5 + (elev / 90.0) * 2.0;            /* +/-2 px */
        const char *label = (data->sources[i].app_label[0] != '\0') ? data->sources[i].app_label : "SPK";

        cairo_set_source_rgb(cr,
            COLORS[i][0] * bright,
            COLORS[i][1] * bright,
            COLORS[i][2] * bright);
        cairo_arc(cr, lx, ly, radius, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, rx, ry, radius, 0, 2 * M_PI);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.08, 0.08, 0.1);
        cairo_arc(cr, lx, ly, radius - 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, rx, ry, radius - 2.0, 0, 2 * M_PI);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_font_size(cr, 10);

        /* pick initials */
        char initials[3] = "SP";
        if (label[0]) {
            initials[0] = label[0];
            initials[1] = label[1] ? label[1] : '\0';
        }

        cairo_move_to(cr, lx - 4, ly + 4);
        cairo_show_text(cr, initials);
        cairo_move_to(cr, rx - 4, ry + 4);
        cairo_show_text(cr, initials);
    }
}

static void on_canvas_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    (void)gesture;
    (void)n_press;
    AppData *data = user_data;

    int source_idx = find_source_at_point(data, x, y);
    if (source_idx < 0)
        return;

    data->active_source = source_idx;
    set_source_position_from_point(data, source_idx, x, y);
}

static void on_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data)
{
    AppData *data = user_data;
    int source_idx = find_source_at_point(data, start_x, start_y);
    data->active_source = source_idx;
    g_object_set_data(G_OBJECT(gesture), "dragging-source", GINT_TO_POINTER(source_idx));
}

static void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    AppData *data = user_data;
    int source_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "dragging-source"));
    if (source_idx < 0)
        return;

    double start_x = 0.0, start_y = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    set_source_position_from_point(data, source_idx, start_x + offset_x, start_y + offset_y);
}

static void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    AppData *data = user_data;
    int source_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "dragging-source"));
    if (source_idx < 0)
        return;

    double start_x = 0.0, start_y = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    set_source_position_from_point(data, source_idx, start_x + offset_x, start_y + offset_y);
    g_object_set_data(G_OBJECT(gesture), "dragging-source", GINT_TO_POINTER(-1));
}

static void on_elevation_changed(GtkRange *range, gpointer user_data)
{
    int source_idx = GPOINTER_TO_INT(user_data);
    AppData *data = g_object_get_data(G_OBJECT(range), "app_data");

    float elevation = gtk_range_get_value(range);
    data->sources[source_idx].elevation = elevation;

    send_sofa_control(data, source_idx);
    refresh_canvas(data);
}

static void on_width_changed(GtkRange *range, gpointer user_data)
{
    int source_idx = GPOINTER_TO_INT(user_data);
    AppData *data = g_object_get_data(G_OBJECT(range), "app_data");

    float width = gtk_range_get_value(range);
    if (fabsf(width - data->sources[source_idx].width) < 0.2f)
        return;

    data->sources[source_idx].width = width;

    send_sofa_control(data, source_idx);
    refresh_canvas(data);
}

static void on_bypass_toggled(GtkCheckButton *button, gpointer user_data)
{
    int source_idx = GPOINTER_TO_INT(user_data);
    AppData *data = g_object_get_data(G_OBJECT(button), "app_data");
    set_source_bypass(data, source_idx, gtk_check_button_get_active(button));
}

void update_source_position(AppData *data, int source_idx, float azimuth, float radius)
{
    if (source_idx < 0 || source_idx >= MAX_SOURCES) return;

    data->sources[source_idx].azimuth = azimuth;
    data->sources[source_idx].radius = radius;

    send_sofa_control(data, source_idx);
    refresh_canvas(data);
}

static GtkWidget *build_source_control(AppData *data, int idx, const char *source_name)
{
    GtkWidget *source_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_top(source_box, 5);
    gtk_widget_set_margin_bottom(source_box, 5);
    data->source_boxes[idx] = source_box;

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(source_box), header_box);

    char label_text[64];
    snprintf(label_text, sizeof(label_text), "Source %d (%s)", idx + 1, source_name);
    data->source_labels[idx] = gtk_label_new(label_text);
    gtk_widget_set_halign(data->source_labels[idx], GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(data->source_labels[idx]), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(data->source_labels[idx], 200, -1);
    gtk_box_append(GTK_BOX(header_box), data->source_labels[idx]);

    GtkWidget *elev_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *elev_label = gtk_label_new("Elevation:");
    gtk_box_append(GTK_BOX(elev_box), elev_label);

    GtkWidget *elev_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -90.0, 90.0, 1.0);
    gtk_range_set_value(GTK_RANGE(elev_slider), 0.0);
    gtk_widget_set_hexpand(elev_slider, TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(elev_slider), GTK_POS_RIGHT);
    g_object_set_data(G_OBJECT(elev_slider), "app_data", data);
    g_signal_connect(elev_slider, "value-changed", G_CALLBACK(on_elevation_changed), GINT_TO_POINTER(idx));
    data->elevation_sliders[idx] = elev_slider;
    gtk_widget_set_sensitive(elev_slider, false);
    gtk_box_append(GTK_BOX(elev_box), elev_slider);

    gtk_box_append(GTK_BOX(source_box), elev_box);

    GtkWidget *width_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *width_label = gtk_label_new("Width:");
    gtk_box_append(GTK_BOX(width_box), width_label);

    GtkWidget *width_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 90.0, 1.0);
    gtk_range_set_value(GTK_RANGE(width_slider), data->sources[idx].width);
    gtk_widget_set_hexpand(width_slider, TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(width_slider), GTK_POS_RIGHT);
    g_object_set_data(G_OBJECT(width_slider), "app_data", data);
    g_signal_connect(width_slider, "value-changed", G_CALLBACK(on_width_changed), GINT_TO_POINTER(idx));
    data->width_sliders[idx] = width_slider;
    gtk_widget_set_sensitive(width_slider, false);
    gtk_box_append(GTK_BOX(width_box), width_slider);

    gtk_box_append(GTK_BOX(source_box), width_box);

    GtkWidget *bypass_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *bypass_check = gtk_check_button_new_with_label("Bypass");
    gtk_widget_set_sensitive(bypass_check, false);
    data->bypass_checkboxes[idx] = bypass_check;
    g_object_set_data(G_OBJECT(bypass_check), "app_data", data);
    g_signal_connect(bypass_check, "toggled", G_CALLBACK(on_bypass_toggled), GINT_TO_POINTER(idx));
    gtk_box_append(GTK_BOX(bypass_box), bypass_check);
    gtk_box_append(GTK_BOX(source_box), bypass_box);

    GtkWidget *playing_label = gtk_label_new("No audio");
    gtk_widget_set_halign(playing_label, GTK_ALIGN_START);
    data->playing_labels[idx] = playing_label;
    gtk_box_append(GTK_BOX(source_box), playing_label);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(source_box), separator);

    return source_box;
}

void build_gui(AppData *data)
{
    data->window = gtk_application_window_new(GTK_APPLICATION(g_application_get_default()));
    gtk_window_set_title(GTK_WINDOW(data->window), "PipeWire 3D Audio Mixer (4-Channel)");
    gtk_window_set_default_size(GTK_WINDOW(data->window), 900, 550);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_window_set_child(GTK_WINDOW(data->window), main_box);
    gtk_widget_set_margin_start(main_box, 10);
    gtk_widget_set_margin_end(main_box, 10);
    gtk_widget_set_margin_top(main_box, 10);
    gtk_widget_set_margin_bottom(main_box, 10);

    GtkWidget *canvas_frame = gtk_frame_new("Spatial View");
    gtk_widget_set_size_request(canvas_frame, CANVAS_SIZE + 20, CANVAS_SIZE + 20);
    gtk_widget_set_valign(canvas_frame, GTK_ALIGN_CENTER);

    data->canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(data->canvas, CANVAS_SIZE, CANVAS_SIZE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(data->canvas), draw_canvas, data, NULL);
    gtk_frame_set_child(GTK_FRAME(canvas_frame), data->canvas);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_canvas_click), data);
    gtk_widget_add_controller(data->canvas, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), data);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), data);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), data);
    g_object_set_data(G_OBJECT(drag), "dragging-source", GINT_TO_POINTER(-1));
    gtk_widget_add_controller(data->canvas, GTK_EVENT_CONTROLLER(drag));

    gtk_box_append(GTK_BOX(main_box), canvas_frame);

    GtkWidget *control_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(main_box), control_box);

    GtkWidget *control_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(control_label), "<b>Audio Sources</b>");
    gtk_box_append(GTK_BOX(control_box), control_label);

    const char *source_names[] = {"spk1/spk2", "spk3/spk4", "spk5/spk6", "spk7/spk8"};

    for (int i = 0; i < MAX_SOURCES; i++) {
        GtkWidget *source_box = build_source_control(data, i, source_names[i]);
        gtk_box_append(GTK_BOX(control_box), source_box);
    }

    GtkWidget *kill_links_btn = gtk_button_new_with_label("Kill spatializer links");
    g_object_set_data(G_OBJECT(kill_links_btn), "app_data", data);
    g_signal_connect_swapped(kill_links_btn, "clicked", G_CALLBACK(unlink_all_filter_inputs), data);
    gtk_box_append(GTK_BOX(control_box), kill_links_btn);
}
