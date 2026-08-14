#ifndef BCROP_IMAGE_H
#define BCROP_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum image_format {
    IMAGE_JPEG,
    IMAGE_PNG,
    IMAGE_WEBP,
};

struct image_frame {
    uint32_t *pixels;
    int duration_ms;
    bool lossless;
    bool opaque;
    float quality;
};

struct image {
    enum image_format format;
    int width;
    int height;
    struct image_frame *frames;
    size_t frame_count;
    size_t current_frame;
    int loop_count;
    uint32_t background;
    void *codec;
};

int image_load(struct image *image, const char *path, char *error, size_t error_size);
void image_free(struct image *image);
const uint32_t *image_pixels(const struct image *image);
bool image_opaque(const struct image *image);
bool image_animated(const struct image *image);
int image_advance(struct image *image);
int image_current_duration(const struct image *image);
const char *image_format_name(enum image_format format);
bool image_extension_matches(enum image_format format, const char *path);
void image_jpeg_mcu(const struct image *image, int *width, int *height);
int image_save_crop(const struct image *image, const char *path,
                    int x, int y, int width, int height, bool require_lossless,
                    char *error, size_t error_size);

#endif
