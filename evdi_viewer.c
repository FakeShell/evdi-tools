/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <evdi_lib.h>
#include <gtk/gtk.h>

#define BUFFER_ID 0
#define MAX_RECTS 16

typedef struct App {
    evdi_handle evdi;
    evdi_selectable evdi_fd;

    struct evdi_mode mode;
    struct evdi_buffer buffer;
    struct evdi_rect rects[MAX_RECTS];

    bool running;
    bool buffer_registered;
    bool have_frame;

    GtkWidget *window;
    GtkWidget *drawing_area;
} App;

static App app;

static unsigned char*
read_edid_file(const char *path, unsigned int *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror("fopen EDID");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    if (size != 128 && size != 256 && size != 384) {
        fprintf(stderr, "Bad EDID size: %ld. Expected 128, 256, or 384\n", size);
        fclose(file);
        return NULL;
    }

    unsigned char *data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }

    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        perror("fread EDID");
        free(data);
        fclose(file);
        return NULL;
    }

    fclose(file);
    *size_out = (unsigned int)size;
    return data;
}

static void
cleanup_buffer(void)
{
    if (app.buffer_registered) {
        evdi_unregister_buffer(app.evdi, app.buffer.id);
        app.buffer_registered = false;
    }

    free(app.buffer.buffer);
    app.buffer.buffer = NULL;
}

static gboolean
draw_cb(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    (void)widget;
    (void)user_data;

    if (!app.buffer.buffer || !app.have_frame) {
        cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        cairo_paint(cr);
        return FALSE;
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(app.buffer.buffer,
                                                                   CAIRO_FORMAT_RGB24,
                                                                   app.buffer.width,
                                                                   app.buffer.height,
                                                                   app.buffer.stride);

    if (!surface)
        return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(app.drawing_area, &alloc);

    double sx = (double)alloc.width / (double)app.buffer.width;
    double sy = (double)alloc.height / (double)app.buffer.height;
    double scale = sx < sy ? sx : sy;

    double draw_w = app.buffer.width * scale;
    double draw_h = app.buffer.height * scale;
    double off_x = (alloc.width - draw_w) / 2.0;
    double off_y = (alloc.height - draw_h) / 2.0;

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    cairo_save(cr);
    cairo_translate(cr, off_x, off_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(surface);
    return FALSE;
}

static void
register_buffer_for_mode(struct evdi_mode mode)
{
    cleanup_buffer();

    app.mode = mode;

    app.buffer.id = BUFFER_ID;
    app.buffer.width = mode.width;
    app.buffer.height = mode.height;
    app.buffer.stride = mode.width * 4;
    app.buffer.buffer = calloc(1, app.buffer.stride * app.buffer.height);
    app.buffer.rects = app.rects;
    app.buffer.rect_count = MAX_RECTS;

    if (!app.buffer.buffer) {
        fprintf(stderr, "Failed to allocate framebuffer\n");
        app.running = false;
        gtk_main_quit();
        return;
    }

    evdi_register_buffer(app.evdi, app.buffer);
    app.buffer_registered = true;
    app.have_frame = false;

    gtk_window_set_default_size(GTK_WINDOW(app.window), 960, 540);
    gtk_widget_queue_draw(app.drawing_area);

    evdi_request_update(app.evdi, app.buffer.id);
}

static void
dpms_handler(int dpms_mode, void *user_data)
{
    (void)user_data;
    printf("DPMS mode: %d\n", dpms_mode);
}

static void
mode_changed_handler(struct evdi_mode mode, void *user_data)
{
    (void)user_data;

    printf("Mode changed: %dx%d @ %dHz, %d bpp, pixel_format=0x%x\n",
           mode.width,
           mode.height,
           mode.refresh_rate,
           mode.bits_per_pixel,
           mode.pixel_format);

    if (mode.width <= 0 || mode.height <= 0)
        return;

    if (mode.bits_per_pixel != 32) {
        fprintf(stderr, "Only 32bpp modes are supported\n");
        app.running = false;
        gtk_main_quit();
        return;
    }

    register_buffer_for_mode(mode);
}

static void
update_ready_handler(int buffer_id, void *user_data)
{
    (void)user_data;

    if (!app.buffer_registered || buffer_id != app.buffer.id)
        return;

    int n_rects = MAX_RECTS;
    evdi_grab_pixels(app.evdi, app.rects, &n_rects);

    if (n_rects > 0) {
        app.have_frame = true;
        gtk_widget_queue_draw(app.drawing_area);
    }

    evdi_request_update(app.evdi, app.buffer.id);
}

static void
crtc_state_handler(int state, void *user_data)
{
    (void)user_data;
    printf("CRTC state: %d\n", state);
}

static void
cursor_set_handler(struct evdi_cursor_set cursor_set, void *user_data)
{
    (void)cursor_set;
    (void)user_data;
}

static void
cursor_move_handler(struct evdi_cursor_move cursor_move, void *user_data)
{
    (void)cursor_move;
    (void)user_data;
}

static void
ddcci_data_handler(struct evdi_ddcci_data ddcci_data, void *user_data)
{
    (void)ddcci_data;
    (void)user_data;

    evdi_ddcci_response(app.evdi, NULL, 0, false);
}

static gboolean
evdi_io_cb(GIOChannel *source, GIOCondition condition, gpointer user_data)
{
    (void)source;
    (void)user_data;

    if (!(condition & G_IO_IN))
        return TRUE;

    struct evdi_event_context evdi_ctx;
    memset(&evdi_ctx, 0, sizeof(evdi_ctx));

    evdi_ctx.dpms_handler = dpms_handler;
    evdi_ctx.mode_changed_handler = mode_changed_handler;
    evdi_ctx.update_ready_handler = update_ready_handler;
    evdi_ctx.crtc_state_handler = crtc_state_handler;
    evdi_ctx.cursor_set_handler = cursor_set_handler;
    evdi_ctx.cursor_move_handler = cursor_move_handler;
    evdi_ctx.ddcci_data_handler = ddcci_data_handler;
    evdi_ctx.user_data = &app;

    evdi_handle_events(app.evdi, &evdi_ctx);

    if (app.buffer_registered) {
        while (evdi_request_update(app.evdi, app.buffer.id)) {
            update_ready_handler(app.buffer.id, NULL);
        }
    }

    return TRUE;
}

static void
destroy_cb(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;

    app.running = false;
    gtk_main_quit();
}

int
main(int argc, char **argv)
{
    int card_number = 1;
    const char *edid_path = "1920x1080.bin";

    if (argc >= 2)
        card_number = atoi(argv[1]);
    if (argc >= 3)
        edid_path = argv[2];

    memset(&app, 0, sizeof(app));
    app.running = true;

    gtk_init(&argc, &argv);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "EVDI GTK Viewer");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 960, 540);
    gtk_window_present(GTK_WINDOW(app.window));

    app.drawing_area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(app.window), app.drawing_area);

    g_signal_connect(app.window, "destroy", G_CALLBACK(destroy_cb), NULL);
    g_signal_connect(app.drawing_area, "draw", G_CALLBACK(draw_cb), NULL);

    gtk_widget_show_all(app.window);
    gtk_window_present(GTK_WINDOW(app.window));

    unsigned int edid_size = 0;
    unsigned char *edid = read_edid_file(edid_path, &edid_size);
    if (!edid)
        return 1;

    enum evdi_device_status status = evdi_check_device(card_number);
    if (status != AVAILABLE) {
        fprintf(stderr, "EVDI card%d is not available, status=%d\n", card_number, status);
        free(edid);
        return 1;
    }

    app.evdi = evdi_open(card_number);
    if (app.evdi == EVDI_INVALID_HANDLE) {
        fprintf(stderr, "Failed to open EVDI card%d\n", card_number);
        free(edid);
        return 1;
    }

    printf("Connecting EVDI card%d using EDID %s, size=%u\n",
           card_number,
           edid_path,
           edid_size);

    evdi_connect2(app.evdi,
                  edid,
                  edid_size,
                  1920 * 1080,
                  1920 * 1080 * 60);

    free(edid);

    app.evdi_fd = evdi_get_event_ready(app.evdi);

    GIOChannel *channel = g_io_channel_unix_new(app.evdi_fd);
    g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, evdi_io_cb, NULL);
    g_io_channel_unref(channel);

    printf("EVDI GTK viewer ready on card%d. Start producer now.\n", card_number);

    gtk_main();

    cleanup_buffer();

    if (app.evdi) {
        evdi_disconnect(app.evdi);
        evdi_close(app.evdi);
    }

    return 0;
}
