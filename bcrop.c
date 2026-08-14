#define _GNU_SOURCE

#include "image.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"

enum drag_mode {
    DRAG_NONE,
    DRAG_NEW,
    DRAG_MOVE,
    DRAG_PAN,
    DRAG_LEFT = 1 << 4,
    DRAG_RIGHT = 1 << 5,
    DRAG_TOP = 1 << 6,
    DRAG_BOTTOM = 1 << 7,
};

struct rect {
    double x, y, w, h;
};

struct app;

struct buffer {
    struct app *app;
    struct wl_buffer *wl;
    uint32_t *pixels;
    size_t size;
    int width, height;
    bool busy;
    bool retired;
    struct buffer *next;
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    struct buffer *buffers[2];
    struct buffer *retired;
    struct image image;
    int image_width, image_height;
    int width, height;
    int scale;
    bool configured;
    bool fitted;
    bool running;
    bool dirty;
    bool show_help;
    char status[64];

    double zoom;
    double offset_x, offset_y;
    double pointer_x, pointer_y;
    struct rect crop;
    struct rect drag_crop;
    double drag_x, drag_y;
    double drag_anchor_x, drag_anchor_y;
    enum drag_mode drag;
    double aspect;

    const char *input_path;
    const char *output_path;
    bool force;
    bool jpeg_lossless;
    int animation_fd;
    int exit_status;
};

static void redraw(struct app *app);

static double clamp_double(double value, double low, double high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static void mark_dirty(struct app *app)
{
    app->dirty = true;
    redraw(app);
}

static void destroy_buffer(struct buffer *buffer)
{
    if (!buffer)
        return;
    if (buffer->wl)
        wl_buffer_destroy(buffer->wl);
    if (buffer->pixels && buffer->pixels != MAP_FAILED)
        munmap(buffer->pixels, buffer->size);
    free(buffer);
}

static void unlink_retired(struct app *app, struct buffer *buffer)
{
    struct buffer **link = &app->retired;

    while (*link && *link != buffer)
        link = &(*link)->next;
    if (*link)
        *link = buffer->next;
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct buffer *buffer = data;
    struct app *app = buffer->app;
    (void)wl_buffer;

    buffer->busy = false;
    if (buffer->retired) {
        unlink_retired(app, buffer);
        destroy_buffer(buffer);
    } else if (app->dirty) {
        redraw(app);
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct buffer *create_buffer(struct app *app, int width, int height)
{
    struct buffer *buffer;
    struct wl_shm_pool *pool;
    size_t stride, size;
    int fd;

    if (width <= 0 || height <= 0)
        return NULL;
    stride = (size_t)width * 4;
    if (stride > SIZE_MAX / (size_t)height) {
        errno = EOVERFLOW;
        return NULL;
    }
    size = stride * (size_t)height;
    if (size > INT32_MAX) {
        errno = EOVERFLOW;
        return NULL;
    }

    fd = memfd_create("bcrop", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        close(fd);
        return NULL;
    }
    buffer->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->pixels == MAP_FAILED) {
        close(fd);
        free(buffer);
        return NULL;
    }

    pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
    close(fd);
    if (!pool) {
        munmap(buffer->pixels, size);
        free(buffer);
        return NULL;
    }
    buffer->wl = wl_shm_pool_create_buffer(pool, 0, width, height, (int32_t)stride,
                                            WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!buffer->wl) {
        munmap(buffer->pixels, size);
        free(buffer);
        return NULL;
    }

    buffer->app = app;
    buffer->size = size;
    buffer->width = width;
    buffer->height = height;
    wl_buffer_add_listener(buffer->wl, &buffer_listener, buffer);
    return buffer;
}

static void retire_buffers(struct app *app)
{
    for (size_t i = 0; i < 2; i++) {
        struct buffer *buffer = app->buffers[i];
        app->buffers[i] = NULL;
        if (!buffer)
            continue;
        if (buffer->busy) {
            buffer->retired = true;
            buffer->next = app->retired;
            app->retired = buffer;
        } else {
            destroy_buffer(buffer);
        }
    }
}

static bool ensure_buffers(struct app *app)
{
    int width = app->width * app->scale;
    int height = app->height * app->scale;

    if (app->buffers[0] && app->buffers[0]->width == width &&
        app->buffers[0]->height == height)
        return true;

    retire_buffers(app);
    app->buffers[0] = create_buffer(app, width, height);
    app->buffers[1] = create_buffer(app, width, height);
    if (!app->buffers[0] || !app->buffers[1]) {
        fprintf(stderr, "bcrop: cannot allocate Wayland buffers: %s\n", strerror(errno));
        app->running = false;
        app->exit_status = 1;
        return false;
    }
    return true;
}

static void fit_view(struct app *app)
{
    double x_scale, y_scale;

    if (app->width <= 0 || app->height <= 0)
        return;
    x_scale = (double)app->width / app->image_width;
    y_scale = (double)app->height / app->image_height;
    app->zoom = fmin(x_scale, y_scale);
    app->offset_x = ((double)app->width - app->image_width * app->zoom) / 2.0;
    app->offset_y = ((double)app->height - app->image_height * app->zoom) / 2.0;
}

static void screen_to_image(const struct app *app, double sx, double sy,
                            double *ix, double *iy)
{
    *ix = (sx - app->offset_x) / app->zoom;
    *iy = (sy - app->offset_y) / app->zoom;
}

static void snap_lossless_origin(struct app *app, struct rect *rect)
{
    int mcu_width, mcu_height;
    if (!app->jpeg_lossless || app->image.format != IMAGE_JPEG)
        return;
    image_jpeg_mcu(&app->image, &mcu_width, &mcu_height);
    double max_x = app->image_width - rect->w;
    double max_y = app->image_height - rect->h;
    double snapped_x = round(rect->x / mcu_width) * mcu_width;
    double snapped_y = round(rect->y / mcu_height) * mcu_height;
    double last_x = floor(max_x / mcu_width) * mcu_width;
    double last_y = floor(max_y / mcu_height) * mcu_height;
    rect->x = clamp_double(snapped_x, 0.0, fmax(0.0, last_x));
    rect->y = clamp_double(snapped_y, 0.0, fmax(0.0, last_y));
}

static uint32_t *draw_pixels;
static int draw_stride;
static int draw_height;
static uint32_t draw_color;

static void set_color(int red, int green, int blue, int alpha)
{
    draw_color = (uint32_t)alpha << 24 | (uint32_t)red << 16 |
                 (uint32_t)green << 8 | (uint32_t)blue;
}

static uint32_t blend_pixel(uint32_t destination, uint32_t source)
{
    unsigned int alpha = source >> 24;
    if (alpha == 255)
        return source;
    if (alpha == 0)
        return destination;
    unsigned int inverse = 255 - alpha;
    unsigned int red = (((source >> 16) & 255) * alpha +
                        ((destination >> 16) & 255) * inverse + 127) / 255;
    unsigned int green = (((source >> 8) & 255) * alpha +
                          ((destination >> 8) & 255) * inverse + 127) / 255;
    unsigned int blue = ((source & 255) * alpha + (destination & 255) * inverse + 127) / 255;
    return 0xff000000U | red << 16 | green << 8 | blue;
}

static void fill_rect(int x, int y, int width, int height, int max_width, int max_height)
{
    int x2 = clamp_int(x + width, 0, max_width);
    int y2 = clamp_int(y + height, 0, max_height);

    x = clamp_int(x, 0, max_width);
    y = clamp_int(y, 0, max_height);
    if (x2 <= x || y2 <= y)
        return;
    for (int row = y; row < y2; row++) {
        uint32_t *pixel = draw_pixels + (size_t)row * draw_stride + x;
        for (int column = x; column < x2; column++, pixel++)
            *pixel = blend_pixel(*pixel, draw_color);
    }
}

static const uint8_t font[128][7] = {
    [' '] = {0, 0, 0, 0, 0, 0, 0},
    ['+'] = {0, 4, 4, 31, 4, 4, 0},
    [','] = {0, 0, 0, 0, 0, 6, 4},
    ['-'] = {0, 0, 0, 31, 0, 0, 0},
    ['.'] = {0, 0, 0, 0, 0, 6, 6},
    ['/'] = {1, 2, 2, 4, 8, 8, 16},
    ['0'] = {14, 17, 19, 21, 25, 17, 14},
    ['1'] = {4, 12, 4, 4, 4, 4, 14},
    ['2'] = {14, 17, 1, 2, 4, 8, 31},
    ['3'] = {30, 1, 1, 14, 1, 1, 30},
    ['4'] = {2, 6, 10, 18, 31, 2, 2},
    ['5'] = {31, 16, 16, 30, 1, 1, 30},
    ['6'] = {14, 16, 16, 30, 17, 17, 14},
    ['7'] = {31, 1, 2, 4, 8, 8, 8},
    ['8'] = {14, 17, 17, 14, 17, 17, 14},
    ['9'] = {14, 17, 17, 15, 1, 1, 14},
    [':'] = {0, 6, 6, 0, 6, 6, 0},
    ['?'] = {14, 17, 1, 2, 4, 0, 4},
    ['A'] = {14, 17, 17, 31, 17, 17, 17},
    ['B'] = {30, 17, 17, 30, 17, 17, 30},
    ['C'] = {14, 17, 16, 16, 16, 17, 14},
    ['D'] = {30, 17, 17, 17, 17, 17, 30},
    ['E'] = {31, 16, 16, 30, 16, 16, 31},
    ['F'] = {31, 16, 16, 30, 16, 16, 16},
    ['G'] = {14, 17, 16, 23, 17, 17, 15},
    ['H'] = {17, 17, 17, 31, 17, 17, 17},
    ['I'] = {14, 4, 4, 4, 4, 4, 14},
    ['J'] = {7, 2, 2, 2, 2, 18, 12},
    ['K'] = {17, 18, 20, 24, 20, 18, 17},
    ['L'] = {16, 16, 16, 16, 16, 16, 31},
    ['M'] = {17, 27, 21, 21, 17, 17, 17},
    ['N'] = {17, 25, 21, 19, 17, 17, 17},
    ['O'] = {14, 17, 17, 17, 17, 17, 14},
    ['P'] = {30, 17, 17, 30, 16, 16, 16},
    ['Q'] = {14, 17, 17, 17, 21, 18, 13},
    ['R'] = {30, 17, 17, 30, 20, 18, 17},
    ['S'] = {15, 16, 16, 14, 1, 1, 30},
    ['T'] = {31, 4, 4, 4, 4, 4, 4},
    ['U'] = {17, 17, 17, 17, 17, 17, 14},
    ['V'] = {17, 17, 17, 17, 17, 10, 4},
    ['W'] = {17, 17, 17, 21, 21, 21, 10},
    ['X'] = {17, 17, 10, 4, 10, 17, 17},
    ['Y'] = {17, 17, 10, 4, 4, 4, 4},
    ['Z'] = {31, 1, 2, 4, 8, 16, 31},
};

static void draw_text(const char *text, int x, int y, int pixel_scale)
{
    for (; *text; text++, x += 6 * pixel_scale) {
        unsigned char ch = (unsigned char)*text;
        if (ch >= 'a' && ch <= 'z')
            ch = (unsigned char)(ch - 'a' + 'A');
        if (ch >= 128)
            ch = '?';
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (font[ch][row] & (1U << (4 - col)))
                    fill_rect(x + col * pixel_scale, y + row * pixel_scale,
                              pixel_scale, pixel_scale, draw_stride, draw_height);
            }
        }
    }
}

static void draw_help(struct app *app, int width, int height)
{
    static const char *lines[] = {
        "F FREE   O ORIGINAL   1 SQUARE",
        "4 4:3   6 16:9   R RESET   0 FIT",
        "+/- ZOOM   ENTER SAVE   Q/ESC QUIT",
        "LEFT CROP/MOVE/RESIZE   MIDDLE PAN   WHEEL ZOOM",
    };
    int glyph_scale = app->scale;
    int pad = 8 * app->scale;
    int line_height = 10 * glyph_scale;
    int box_width = 0;
    int box_height = (int)(sizeof(lines) / sizeof(lines[0])) * line_height + 2 * pad;

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        int line_width = (int)strlen(lines[i]) * 6 * glyph_scale;
        if (line_width > box_width)
            box_width = line_width;
    }
    box_width += 2 * pad;
    if (box_width > width - 2 * pad)
        box_width = width - 2 * pad;

    set_color(0, 0, 0, 220);
    fill_rect(pad, height - box_height - pad, box_width, box_height, width, height);
    set_color(255, 255, 255, 255);
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        draw_text(lines[i], 2 * pad,
                  height - box_height + (int)i * line_height + pad / 2,
                  glyph_scale);
}

static void draw_status(struct app *app, int width, int height)
{
    int glyph_scale = app->scale;
    int pad = 8 * app->scale;
    int text_width = (int)strlen(app->status) * 6 * glyph_scale;
    int box_width = clamp_int(text_width + 2 * pad, 0, width);
    int box_height = 7 * glyph_scale + 2 * pad;
    (void)height;

    set_color(0, 0, 0, 220);
    fill_rect(0, 0, box_width, box_height, width, height);
    set_color(255, 255, 255, 255);
    draw_text(app->status, pad, pad, glyph_scale);
}

static uint32_t bilinear_sample(const uint32_t *source, int width, int height,
                                double x, double y)
{
    x = clamp_double(x, 0.0, width - 1.0);
    y = clamp_double(y, 0.0, height - 1.0);
    int x0 = clamp_int((int)floor(x), 0, width - 1);
    int y0 = clamp_int((int)floor(y), 0, height - 1);
    int x1 = clamp_int(x0 + 1, 0, width - 1);
    int y1 = clamp_int(y0 + 1, 0, height - 1);
    double fx = x - floor(x);
    double fy = y - floor(y);
    double weights[4] = {(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
                         (1.0 - fx) * fy, fx * fy};
    uint32_t pixels[4] = {source[(size_t)y0 * width + x0],
                          source[(size_t)y0 * width + x1],
                          source[(size_t)y1 * width + x0],
                          source[(size_t)y1 * width + x1]};
    double alpha = 0.0, red = 0.0, green = 0.0, blue = 0.0;
    for (size_t i = 0; i < 4; i++) {
        double a = (pixels[i] >> 24) * weights[i];
        alpha += a;
        red += ((pixels[i] >> 16) & 255) * a;
        green += ((pixels[i] >> 8) & 255) * a;
        blue += (pixels[i] & 255) * a;
    }
    if (alpha <= 0.0)
        return 0;
    return (uint32_t)lrint(alpha) << 24 |
           (uint32_t)lrint(red / alpha) << 16 |
           (uint32_t)lrint(green / alpha) << 8 |
           (uint32_t)lrint(blue / alpha);
}

static void draw_scaled_image(struct app *app, int x, int y, int width, int height)
{
    const uint32_t *source = image_pixels(&app->image);
    int left = clamp_int(x, 0, draw_stride);
    int top = clamp_int(y, 0, draw_height);
    int right = clamp_int(x + width, 0, draw_stride);
    int bottom = clamp_int(y + height, 0, draw_height);

    if (width <= 0 || height <= 0)
        return;
    for (int destination_y = top; destination_y < bottom; destination_y++) {
        double source_y = ((destination_y - y + 0.5) * app->image_height / height) - 0.5;
        for (int destination_x = left; destination_x < right; destination_x++) {
            double source_x = ((destination_x - x + 0.5) * app->image_width / width) - 0.5;
            uint32_t pixel = bilinear_sample(source, app->image_width,
                                             app->image_height, source_x, source_y);
            uint32_t *destination = draw_pixels +
                (size_t)destination_y * draw_stride + destination_x;
            *destination = blend_pixel(*destination, pixel);
        }
    }
}

static void render(struct app *app, struct buffer *buffer)
{
    int physical_scale = app->scale;
    int width = buffer->width;
    int height = buffer->height;
    int image_x = (int)lrint(app->offset_x * physical_scale);
    int image_y = (int)lrint(app->offset_y * physical_scale);
    int image_w = (int)lrint(app->image_width * app->zoom * physical_scale);
    int image_h = (int)lrint(app->image_height * app->zoom * physical_scale);
    int crop_x = (int)lrint((app->offset_x + app->crop.x * app->zoom) * physical_scale);
    int crop_y = (int)lrint((app->offset_y + app->crop.y * app->zoom) * physical_scale);
    int crop_w = (int)lrint(app->crop.w * app->zoom * physical_scale);
    int crop_h = (int)lrint(app->crop.h * app->zoom * physical_scale);
    int line = physical_scale > 1 ? physical_scale : 1;
    int handle = 7 * physical_scale;
    draw_pixels = buffer->pixels;
    draw_stride = width;
    draw_height = height;
    set_color(25, 25, 25, 255);
    fill_rect(0, 0, width, height, width, height);
    draw_scaled_image(app, image_x, image_y, image_w, image_h);

    set_color(0, 0, 0, 150);
    fill_rect(0, 0, width, crop_y, width, height);
    fill_rect(0, crop_y + crop_h, width, height - crop_y - crop_h, width, height);
    fill_rect(0, crop_y, crop_x, crop_h, width, height);
    fill_rect(crop_x + crop_w, crop_y, width - crop_x - crop_w, crop_h, width, height);

    set_color(255, 255, 255, 255);
    fill_rect(crop_x, crop_y, crop_w, line, width, height);
    fill_rect(crop_x, crop_y + crop_h - line, crop_w, line, width, height);
    fill_rect(crop_x, crop_y, line, crop_h, width, height);
    fill_rect(crop_x + crop_w - line, crop_y, line, crop_h, width, height);

    int points[8][2] = {
        {crop_x, crop_y}, {crop_x + crop_w / 2, crop_y}, {crop_x + crop_w, crop_y},
        {crop_x, crop_y + crop_h / 2}, {crop_x + crop_w, crop_y + crop_h / 2},
        {crop_x, crop_y + crop_h}, {crop_x + crop_w / 2, crop_y + crop_h},
        {crop_x + crop_w, crop_y + crop_h},
    };
    for (size_t i = 0; i < 8; i++)
        fill_rect(points[i][0] - handle / 2, points[i][1] - handle / 2,
                  handle, handle, width, height);

    if (app->show_help)
        draw_help(app, width, height);
    if (app->status[0])
        draw_status(app, width, height);

}

static void redraw(struct app *app)
{
    struct buffer *buffer = NULL;

    if (!app->running || !app->configured || !app->dirty)
        return;
    if (!ensure_buffers(app))
        return;
    for (size_t i = 0; i < 2; i++) {
        if (!app->buffers[i]->busy) {
            buffer = app->buffers[i];
            break;
        }
    }
    if (!buffer)
        return;

    render(app, buffer);
    buffer->busy = true;
    wl_surface_set_buffer_scale(app->surface, app->scale);
    wl_surface_attach(app->surface, buffer->wl, 0, 0);
    if (wl_proxy_get_version((struct wl_proxy *)app->surface) >= 4)
        wl_surface_damage_buffer(app->surface, 0, 0, buffer->width, buffer->height);
    else
        wl_surface_damage(app->surface, 0, 0, app->width, app->height);
    wl_surface_commit(app->surface);
    app->dirty = false;
}

static void apply_aspect(struct app *app, double aspect)
{
    double cx = app->crop.x + app->crop.w / 2.0;
    double cy = app->crop.y + app->crop.h / 2.0;
    double width = app->crop.w;
    double height = app->crop.h;

    app->aspect = aspect;
    if (aspect <= 0.0) {
        mark_dirty(app);
        return;
    }
    if (width / height > aspect)
        width = height * aspect;
    else
        height = width / aspect;
    app->crop.w = fmax(1.0, width);
    app->crop.h = fmax(1.0, height);
    app->crop.x = clamp_double(cx - app->crop.w / 2.0, 0.0,
                               app->image_width - app->crop.w);
    app->crop.y = clamp_double(cy - app->crop.h / 2.0, 0.0,
                               app->image_height - app->crop.h);
    snap_lossless_origin(app, &app->crop);
    mark_dirty(app);
}

static void zoom_at(struct app *app, double factor, double sx, double sy)
{
    double ix, iy, minimum, maximum, new_zoom;

    screen_to_image(app, sx, sy, &ix, &iy);
    minimum = fmin((double)app->width / app->image_width,
                   (double)app->height / app->image_height) * 0.1;
    maximum = 64.0;
    new_zoom = clamp_double(app->zoom * factor, fmax(minimum, 0.001), maximum);
    app->offset_x = sx - ix * new_zoom;
    app->offset_y = sy - iy * new_zoom;
    app->zoom = new_zoom;
    mark_dirty(app);
}

static enum drag_mode hit_test(const struct app *app, double x, double y)
{
    double left = app->offset_x + app->crop.x * app->zoom;
    double top = app->offset_y + app->crop.y * app->zoom;
    double right = left + app->crop.w * app->zoom;
    double bottom = top + app->crop.h * app->zoom;
    const double threshold = 9.0;
    enum drag_mode mode = DRAG_NONE;

    if (fabs(x - left) <= threshold && y >= top - threshold && y <= bottom + threshold)
        mode = (enum drag_mode)(mode | DRAG_LEFT);
    else if (fabs(x - right) <= threshold && y >= top - threshold && y <= bottom + threshold)
        mode = (enum drag_mode)(mode | DRAG_RIGHT);
    if (fabs(y - top) <= threshold && x >= left - threshold && x <= right + threshold)
        mode = (enum drag_mode)(mode | DRAG_TOP);
    else if (fabs(y - bottom) <= threshold && x >= left - threshold && x <= right + threshold)
        mode = (enum drag_mode)(mode | DRAG_BOTTOM);
    if (mode != DRAG_NONE)
        return mode;
    if (x >= left && x <= right && y >= top && y <= bottom)
        return DRAG_MOVE;
    return DRAG_NEW;
}

static void fixed_rect_from_corner(struct app *app, double ax, double ay,
                                   double px, double py, struct rect *rect)
{
    double sign_x = px >= ax ? 1.0 : -1.0;
    double sign_y = py >= ay ? 1.0 : -1.0;
    double width = fabs(px - ax);
    double height = fabs(py - ay);
    double max_width = sign_x > 0 ? app->image_width - ax : ax;
    double max_height = sign_y > 0 ? app->image_height - ay : ay;

    if (width / fmax(height, 0.0001) > app->aspect)
        height = width / app->aspect;
    else
        width = height * app->aspect;
    width = fmin(width, max_width);
    height = width / app->aspect;
    if (height > max_height) {
        height = max_height;
        width = height * app->aspect;
    }
    width = fmax(width, 1.0);
    height = fmax(height, 1.0);
    rect->x = sign_x > 0 ? ax : ax - width;
    rect->y = sign_y > 0 ? ay : ay - height;
    rect->w = width;
    rect->h = height;
}

static void drag_fixed_edge(struct app *app, double ix, double iy, struct rect *rect)
{
    struct rect old = app->drag_crop;

    if (app->drag & (DRAG_LEFT | DRAG_RIGHT)) {
        double anchor_x = app->drag & DRAG_LEFT ? old.x + old.w : old.x;
        double width = fabs(ix - anchor_x);
        double height = width / app->aspect;
        double center_y = old.y + old.h / 2.0;
        double max_width = app->drag & DRAG_LEFT ? anchor_x : app->image_width - anchor_x;
        double max_height = 2.0 * fmin(center_y, app->image_height - center_y);
        width = fmin(fmax(width, 1.0), fmin(max_width, max_height * app->aspect));
        height = width / app->aspect;
        rect->x = app->drag & DRAG_LEFT ? anchor_x - width : anchor_x;
        rect->y = center_y - height / 2.0;
        rect->w = width;
        rect->h = height;
    } else {
        double anchor_y = app->drag & DRAG_TOP ? old.y + old.h : old.y;
        double height = fabs(iy - anchor_y);
        double width = height * app->aspect;
        double center_x = old.x + old.w / 2.0;
        double max_height = app->drag & DRAG_TOP ? anchor_y : app->image_height - anchor_y;
        double max_width = 2.0 * fmin(center_x, app->image_width - center_x);
        height = fmin(fmax(height, 1.0), fmin(max_height, max_width / app->aspect));
        width = height * app->aspect;
        rect->x = center_x - width / 2.0;
        rect->y = app->drag & DRAG_TOP ? anchor_y - height : anchor_y;
        rect->w = width;
        rect->h = height;
    }
}

static void update_drag(struct app *app, double x, double y)
{
    double ix, iy;
    struct rect rect = app->drag_crop;

    if (app->drag == DRAG_NONE)
        return;
    if (app->drag == DRAG_PAN) {
        app->offset_x += x - app->drag_x;
        app->offset_y += y - app->drag_y;
        app->drag_x = x;
        app->drag_y = y;
        mark_dirty(app);
        return;
    }

    screen_to_image(app, x, y, &ix, &iy);
    ix = clamp_double(ix, 0.0, app->image_width);
    iy = clamp_double(iy, 0.0, app->image_height);
    if (app->drag == DRAG_MOVE) {
        double dx = (x - app->drag_x) / app->zoom;
        double dy = (y - app->drag_y) / app->zoom;
        rect.x = clamp_double(app->drag_crop.x + dx, 0.0,
                              app->image_width - rect.w);
        rect.y = clamp_double(app->drag_crop.y + dy, 0.0,
                              app->image_height - rect.h);
    } else if (app->drag == DRAG_NEW) {
        if (app->aspect > 0.0) {
            fixed_rect_from_corner(app, app->drag_anchor_x, app->drag_anchor_y,
                                   ix, iy, &rect);
        } else {
            rect.x = fmin(app->drag_anchor_x, ix);
            rect.y = fmin(app->drag_anchor_y, iy);
            rect.w = fmax(1.0, fabs(ix - app->drag_anchor_x));
            rect.h = fmax(1.0, fabs(iy - app->drag_anchor_y));
        }
    } else if (app->aspect > 0.0) {
        bool horizontal = (app->drag & (DRAG_LEFT | DRAG_RIGHT)) != 0;
        bool vertical = (app->drag & (DRAG_TOP | DRAG_BOTTOM)) != 0;
        if (horizontal && vertical) {
            double ax = app->drag & DRAG_LEFT ? app->drag_crop.x + app->drag_crop.w
                                               : app->drag_crop.x;
            double ay = app->drag & DRAG_TOP ? app->drag_crop.y + app->drag_crop.h
                                              : app->drag_crop.y;
            fixed_rect_from_corner(app, ax, ay, ix, iy, &rect);
        } else {
            drag_fixed_edge(app, ix, iy, &rect);
        }
    } else {
        double left = rect.x;
        double right = rect.x + rect.w;
        double top = rect.y;
        double bottom = rect.y + rect.h;
        if (app->drag & DRAG_LEFT)
            left = fmin(ix, right - 1.0);
        if (app->drag & DRAG_RIGHT)
            right = fmax(ix, left + 1.0);
        if (app->drag & DRAG_TOP)
            top = fmin(iy, bottom - 1.0);
        if (app->drag & DRAG_BOTTOM)
            bottom = fmax(iy, top + 1.0);
        rect.x = left;
        rect.y = top;
        rect.w = right - left;
        rect.h = bottom - top;
    }
    rect.w = clamp_double(rect.w, 1.0, app->image_width);
    rect.h = clamp_double(rect.h, 1.0, app->image_height);
    rect.x = clamp_double(rect.x, 0.0, app->image_width - rect.w);
    rect.y = clamp_double(rect.y, 0.0, app->image_height - rect.h);
    snap_lossless_origin(app, &rect);
    app->crop = rect;
    mark_dirty(app);
}

static int save_crop(struct app *app)
{
    const char *destination = app->output_path ? app->output_path : app->input_path;
    char *temporary = NULL;
    const char *slash;
    struct stat old_stat;
    bool had_destination = lstat(destination, &old_stat) == 0;
    int x, y, width, height, fd;
    int result = -1;
    char encode_error[256];

    x = clamp_int((int)floor(app->crop.x), 0, app->image_width - 1);
    y = clamp_int((int)floor(app->crop.y), 0, app->image_height - 1);
    width = clamp_int((int)ceil(app->crop.x + app->crop.w) - x, 1,
                      app->image_width - x);
    height = clamp_int((int)ceil(app->crop.y + app->crop.h) - y, 1,
                       app->image_height - y);

    slash = strrchr(destination, '/');
    size_t directory_length = slash ? (size_t)(slash - destination + 1) : 0;
    static const char suffix[] = ".bcrop.tmp.XXXXXX";
    temporary = malloc(directory_length + sizeof(suffix));
    if (!temporary) {
        fprintf(stderr, "bcrop: out of memory\n");
        goto out;
    }
    if (directory_length)
        memcpy(temporary, destination, directory_length);
    memcpy(temporary + directory_length, suffix, sizeof(suffix));
    fd = mkstemp(temporary);
    if (fd < 0) {
        fprintf(stderr, "bcrop: cannot create temporary file: %s\n", strerror(errno));
        goto out;
    }
    close(fd);

    if (image_save_crop(&app->image, temporary, x, y, width, height,
                        app->jpeg_lossless, encode_error, sizeof(encode_error)) < 0) {
        fprintf(stderr, "bcrop: cannot save %s: %s\n", destination, encode_error);
        goto out;
    }

    if (had_destination) {
        if (chmod(temporary, old_stat.st_mode & 0777) < 0) {
            fprintf(stderr, "bcrop: cannot preserve destination permissions: %s\n",
                    strerror(errno));
            goto out;
        }
    } else {
        mode_t mask = umask(0);
        umask(mask);
        if (chmod(temporary, 0666 & ~mask) < 0) {
            fprintf(stderr, "bcrop: cannot set destination permissions: %s\n",
                    strerror(errno));
            goto out;
        }
    }

    fd = open(temporary, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fsync(fd) < 0) {
        fprintf(stderr, "bcrop: cannot flush cropped image: %s\n", strerror(errno));
        if (fd >= 0)
            close(fd);
        goto out;
    }
    close(fd);

    if (app->force) {
        if (rename(temporary, destination) < 0) {
            fprintf(stderr, "bcrop: cannot replace %s: %s\n", destination,
                    strerror(errno));
            goto out;
        }
    } else {
        if (link(temporary, destination) < 0) {
            fprintf(stderr, "bcrop: cannot create %s: %s\n", destination,
                    strerror(errno));
            goto out;
        }
        if (unlink(temporary) < 0)
            fprintf(stderr, "bcrop: warning: cannot remove %s: %s\n", temporary,
                    strerror(errno));
    }
    result = 0;

out:
    if (result != 0 && temporary)
        unlink(temporary);
    free(temporary);
    return result;
}

static void handle_key(struct app *app, xkb_keysym_t symbol)
{
    switch (symbol) {
    case XKB_KEY_f:
    case XKB_KEY_F:
        apply_aspect(app, 0.0);
        break;
    case XKB_KEY_o:
    case XKB_KEY_O:
        apply_aspect(app, (double)app->image_width / app->image_height);
        break;
    case XKB_KEY_1:
    case XKB_KEY_KP_1:
        apply_aspect(app, 1.0);
        break;
    case XKB_KEY_4:
    case XKB_KEY_KP_4:
        apply_aspect(app, 4.0 / 3.0);
        break;
    case XKB_KEY_6:
    case XKB_KEY_KP_6:
        apply_aspect(app, 16.0 / 9.0);
        break;
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
        zoom_at(app, 1.2, app->width / 2.0, app->height / 2.0);
        break;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
        zoom_at(app, 1.0 / 1.2, app->width / 2.0, app->height / 2.0);
        break;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
        fit_view(app);
        mark_dirty(app);
        break;
    case XKB_KEY_r:
    case XKB_KEY_R:
        app->crop = (struct rect){0, 0, app->image_width, app->image_height};
        if (app->aspect > 0.0)
            apply_aspect(app, app->aspect);
        else
            mark_dirty(app);
        break;
    case XKB_KEY_question:
        app->show_help = !app->show_help;
        mark_dirty(app);
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (save_crop(app) == 0) {
            app->exit_status = 0;
            app->running = false;
        } else {
            snprintf(app->status, sizeof(app->status), "SAVE FAILED - SEE STDERR");
            mark_dirty(app);
        }
        break;
    case XKB_KEY_q:
    case XKB_KEY_Q:
    case XKB_KEY_Escape:
        app->exit_status = 0;
        app->running = false;
        break;
    default:
        break;
    }
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    struct app *app = data;
    (void)pointer;
    (void)serial;
    (void)surface;
    app->pointer_x = wl_fixed_to_double(x);
    app->pointer_y = wl_fixed_to_double(y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y)
{
    struct app *app = data;
    (void)pointer;
    (void)time;
    app->pointer_x = wl_fixed_to_double(x);
    app->pointer_y = wl_fixed_to_double(y);
    update_drag(app, app->pointer_x, app->pointer_y);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    struct app *app = data;
    (void)pointer;
    (void)serial;
    (void)time;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (button == BTN_LEFT) {
            app->drag = hit_test(app, app->pointer_x, app->pointer_y);
            app->drag_crop = app->crop;
            app->drag_x = app->pointer_x;
            app->drag_y = app->pointer_y;
            if (app->drag == DRAG_NEW) {
                screen_to_image(app, app->pointer_x, app->pointer_y,
                                &app->drag_anchor_x, &app->drag_anchor_y);
                app->drag_anchor_x = clamp_double(app->drag_anchor_x, 0.0,
                                                  app->image_width);
                app->drag_anchor_y = clamp_double(app->drag_anchor_y, 0.0,
                                                  app->image_height);
            }
        } else if (button == BTN_MIDDLE) {
            app->drag = DRAG_PAN;
            app->drag_x = app->pointer_x;
            app->drag_y = app->pointer_y;
        }
    } else if ((button == BTN_LEFT && app->drag != DRAG_PAN) ||
               (button == BTN_MIDDLE && app->drag == DRAG_PAN)) {
        app->drag = DRAG_NONE;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    struct app *app = data;
    double amount = wl_fixed_to_double(value);
    (void)pointer;
    (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        zoom_at(app, amount < 0.0 ? 1.15 : 1.0 / 1.15,
                app->pointer_x, app->pointer_y);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size)
{
    struct app *app = data;
    char *mapping;
    (void)keyboard;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        return;
    xkb_keymap_unref(app->xkb_keymap);
    xkb_state_unref(app->xkb_state);
    app->xkb_keymap = xkb_keymap_new_from_string(app->xkb_context, mapping,
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mapping, size);
    app->xkb_state = app->xkb_keymap ? xkb_state_new(app->xkb_keymap) : NULL;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    struct app *app = data;
    xkb_keysym_t symbol;
    (void)keyboard;
    (void)serial;
    (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !app->xkb_state)
        return;
    symbol = xkb_state_key_get_one_sym(app->xkb_state, key + 8);
    handle_key(app, symbol);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    struct app *app = data;
    (void)keyboard;
    (void)serial;
    if (app->xkb_state)
        xkb_state_update_mask(app->xkb_state, depressed, latched, locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct app *app = data;

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && app->pointer) {
        wl_pointer_destroy(app->pointer);
        app->pointer = NULL;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_preferred_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    struct app *app = data;
    (void)surface;
    if (factor > 0 && factor != app->scale) {
        app->scale = factor;
        mark_dirty(app);
    }
}

static void surface_preferred_transform(void *data, struct wl_surface *surface,
                                        uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_scale,
    .preferred_buffer_transform = surface_preferred_transform,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(surface, serial);
    app->configured = true;
    if (!app->fitted) {
        fit_view(app);
        app->fitted = true;
    }
    app->dirty = true;
    redraw(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states)
{
    struct app *app = data;
    (void)toplevel;
    (void)states;
    if (width > 0 && height > 0) {
        app->width = width;
        app->height = height;
        app->dirty = true;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct app *app = data;
    (void)toplevel;
    app->running = false;
    app->exit_status = 0;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                     struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct app *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = version < 6 ? version : 6;
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                           bind_version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !app->seat) {
        uint32_t bind_version = version < 3 ? version : 3;
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, bind_version);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void cleanup(struct app *app)
{
    if (app->toplevel)
        xdg_toplevel_destroy(app->toplevel);
    if (app->xdg_surface)
        xdg_surface_destroy(app->xdg_surface);
    if (app->surface)
        wl_surface_destroy(app->surface);
    for (size_t i = 0; i < 2; i++)
        destroy_buffer(app->buffers[i]);
    while (app->retired) {
        struct buffer *next = app->retired->next;
        destroy_buffer(app->retired);
        app->retired = next;
    }
    if (app->pointer)
        wl_pointer_destroy(app->pointer);
    if (app->keyboard)
        wl_keyboard_destroy(app->keyboard);
    if (app->seat)
        wl_seat_destroy(app->seat);
    if (app->wm_base)
        xdg_wm_base_destroy(app->wm_base);
    if (app->shm)
        wl_shm_destroy(app->shm);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display)
        wl_display_disconnect(app->display);
    xkb_state_unref(app->xkb_state);
    xkb_keymap_unref(app->xkb_keymap);
    xkb_context_unref(app->xkb_context);
    if (app->animation_fd >= 0)
        close(app->animation_fd);
    image_free(&app->image);
}

static int arm_animation(struct app *app)
{
    int duration = image_current_duration(&app->image);
    struct itimerspec timer = {0};
    if (duration < 10)
        duration = 10;
    timer.it_value.tv_sec = duration / 1000;
    timer.it_value.tv_nsec = (long)(duration % 1000) * 1000000L;
    return timerfd_settime(app->animation_fd, 0, &timer, NULL);
}

static int dispatch_events(struct app *app)
{
    struct pollfd descriptors[2] = {
        {wl_display_get_fd(app->display), POLLIN, 0},
        {app->animation_fd, POLLIN, 0},
    };
    nfds_t count = app->animation_fd >= 0 ? 2 : 1;

    while (wl_display_prepare_read(app->display) != 0) {
        if (wl_display_dispatch_pending(app->display) < 0)
            return -1;
    }
    if (wl_display_flush(app->display) < 0) {
        if (errno == EAGAIN)
            descriptors[0].events |= POLLOUT;
        else {
            wl_display_cancel_read(app->display);
            return -1;
        }
    }
    int ready = poll(descriptors, count, -1);
    if (ready < 0) {
        wl_display_cancel_read(app->display);
        return errno == EINTR ? 0 : -1;
    }
    if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        wl_display_cancel_read(app->display);
        errno = EPIPE;
        return -1;
    }
    if (descriptors[0].revents & POLLOUT && wl_display_flush(app->display) < 0 &&
        errno != EAGAIN) {
        wl_display_cancel_read(app->display);
        return -1;
    }
    if (descriptors[0].revents & POLLIN) {
        if (wl_display_read_events(app->display) < 0)
            return -1;
    } else {
        wl_display_cancel_read(app->display);
    }
    if (wl_display_dispatch_pending(app->display) < 0)
        return -1;
    if (count == 2 && descriptors[1].revents & POLLIN) {
        uint64_t expirations;
        if (read(app->animation_fd, &expirations, sizeof(expirations)) ==
            (ssize_t)sizeof(expirations)) {
            (void)expirations;
            image_advance(&app->image);
            if (arm_animation(app) < 0)
                return -1;
            mark_dirty(app);
        }
    }
    if (count == 2 && descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static void usage(FILE *stream)
{
    fprintf(stream, "usage: bcrop [-f] [-o OUTPUT] [-l|--jpeg-lossless] IMAGE\n");
}

int main(int argc, char **argv)
{
    struct app app = {
        .width = 1024,
        .height = 768,
        .scale = 1,
        .running = true,
        .dirty = true,
        .animation_fd = -1,
        .exit_status = 1,
    };
    int option;
    char load_error[256];
    struct stat destination_stat;
    static const struct option long_options[] = {
        {"jpeg-lossless", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    while ((option = getopt_long(argc, argv, "flo:h", long_options, NULL)) != -1) {
        switch (option) {
        case 'f':
            app.force = true;
            break;
        case 'o':
            app.output_path = optarg;
            break;
        case 'l':
            app.jpeg_lossless = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind + 1 != argc) {
        usage(stderr);
        return 2;
    }
    app.input_path = argv[optind];
    const char *destination = app.output_path ? app.output_path : app.input_path;
    if (!app.force && lstat(destination, &destination_stat) == 0) {
        fprintf(stderr, "bcrop: destination exists; use -f to replace it: %s\n",
                destination);
        return 2;
    }
    if (!app.force && errno != ENOENT) {
        fprintf(stderr, "bcrop: cannot inspect destination %s: %s\n", destination,
                strerror(errno));
        return 2;
    }

    if (image_load(&app.image, app.input_path, load_error, sizeof(load_error)) < 0) {
        fprintf(stderr, "bcrop: cannot load %s: %s\n", app.input_path, load_error);
        return 1;
    }
    if (!image_extension_matches(app.image.format, destination)) {
        fprintf(stderr, "bcrop: output must use the same %s format as the input\n",
                image_format_name(app.image.format));
        cleanup(&app);
        return 2;
    }
    if (app.jpeg_lossless && app.image.format != IMAGE_JPEG) {
        fprintf(stderr, "bcrop: --jpeg-lossless requires JPEG input\n");
        cleanup(&app);
        return 2;
    }
    app.image_width = app.image.width;
    app.image_height = app.image.height;
    app.crop = (struct rect){0, 0, app.image_width, app.image_height};
    app.width = app.image_width < 1024 ? app.image_width : 1024;
    app.height = app.image_height < 768 ? app.image_height : 768;
    if (app.width < 320)
        app.width = 320;
    if (app.height < 240)
        app.height = 240;
    if (image_animated(&app.image)) {
        app.animation_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (app.animation_fd < 0 || arm_animation(&app) < 0) {
            fprintf(stderr, "bcrop: cannot create animation timer: %s\n", strerror(errno));
            cleanup(&app);
            return 1;
        }
    }

    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app.xkb_context) {
        fprintf(stderr, "bcrop: cannot initialize xkbcommon\n");
        cleanup(&app);
        return 1;
    }
    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "bcrop: cannot connect to Wayland display\n");
        cleanup(&app);
        return 1;
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0 || !app.compositor || !app.shm ||
        !app.wm_base) {
        fprintf(stderr, "bcrop: compositor lacks required Wayland globals\n");
        cleanup(&app);
        return 1;
    }

    app.surface = wl_compositor_create_surface(app.compositor);
    wl_surface_add_listener(app.surface, &surface_listener, &app);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.toplevel, "bcrop");
    xdg_toplevel_set_app_id(app.toplevel, "bcrop");
    wl_surface_commit(app.surface);

    while (app.running) {
        if (dispatch_events(&app) < 0) {
            fprintf(stderr, "bcrop: Wayland connection failed: %s\n", strerror(errno));
            app.exit_status = 1;
            break;
        }
    }

    cleanup(&app);
    return app.exit_status;
}
