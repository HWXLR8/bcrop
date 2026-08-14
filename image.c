#define _GNU_SOURCE

#include "image.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <jpeglib.h>
#include <jerror.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

struct file_data {
    uint8_t *data;
    size_t size;
};

struct marker {
    int code;
    size_t size;
    uint8_t *data;
    struct marker *next;
};

struct jpeg_codec {
    struct file_data file;
    uint8_t *samples;
    int components;
    J_COLOR_SPACE input_space;
    J_COLOR_SPACE jpeg_space;
    int num_components;
    int h_samp[MAX_COMPONENTS];
    int v_samp[MAX_COMPONENTS];
    int quant_no[MAX_COMPONENTS];
    bool quant_present[NUM_QUANT_TBLS];
    UINT16 quant[NUM_QUANT_TBLS][DCTSIZE2];
    bool progressive;
    bool arithmetic;
    unsigned int restart_interval;
    bool jfif;
    int jfif_major, jfif_minor;
    int density_unit, density_x, density_y;
    int mcu_width, mcu_height;
    struct marker *markers;
};

struct png_codec {
    struct file_data file;
    uint8_t *rows;
    size_t rowbytes;
    int bit_depth;
    int color_type;
    int interlace;
    int channels;
    png_color palette[256];
    int palette_size;
    png_byte trans_alpha[256];
    int trans_count;
    png_color_16 trans_color;
    bool have_trans_color;
    bool have_gamma;
    double gamma;
    bool have_srgb;
    int srgb_intent;
    bool have_phys;
    png_uint_32 phys_x, phys_y;
    int phys_unit;
    char *icc_name;
    uint8_t *icc_data;
    png_uint_32 icc_size;
    int icc_compression;
    bool have_chrm;
    double white_x, white_y, red_x, red_y, green_x, green_y, blue_x, blue_y;
    png_text *texts;
    int text_count;
    uint8_t *exif_data;
    png_uint_32 exif_size;
    bool have_time;
    png_time modification_time;
    bool have_background;
    png_color_16 background;
    bool have_sigbits;
    png_color_8 sigbits;
};

struct webp_frame_info {
    bool lossless;
    float quality;
};

struct webp_chunk {
    char fourcc[4];
    uint8_t *data;
    size_t size;
};

struct webp_codec {
    struct file_data file;
    struct webp_frame_info *frames;
    struct webp_chunk chunks[3];
};

struct jpeg_error {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

struct memory_reader {
    const uint8_t *data;
    size_t size;
    size_t offset;
};

static void set_error(char *error, size_t size, const char *message)
{
    if (size)
        snprintf(error, size, "%s", message);
}

static void set_errno_error(char *error, size_t size, const char *prefix)
{
    if (size)
        snprintf(error, size, "%s: %s", prefix, strerror(errno));
}

static uint16_t exif_u16(const uint8_t *data, bool little)
{
    return little ? (uint16_t)data[0] | (uint16_t)data[1] << 8 :
                    (uint16_t)data[0] << 8 | data[1];
}

static uint32_t exif_u32(const uint8_t *data, bool little)
{
    return little ? (uint32_t)data[0] | (uint32_t)data[1] << 8 |
                    (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24 :
                    (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
                    (uint32_t)data[2] << 8 | data[3];
}

static void exif_put_u16(uint8_t *data, bool little, uint16_t value)
{
    if (little) {
        data[0] = (uint8_t)value;
        data[1] = (uint8_t)(value >> 8);
    } else {
        data[0] = (uint8_t)(value >> 8);
        data[1] = (uint8_t)value;
    }
}

static void exif_put_u32(uint8_t *data, bool little, uint32_t value)
{
    if (little) {
        data[0] = (uint8_t)value;
        data[1] = (uint8_t)(value >> 8);
        data[2] = (uint8_t)(value >> 16);
        data[3] = (uint8_t)(value >> 24);
    } else {
        data[0] = (uint8_t)(value >> 24);
        data[1] = (uint8_t)(value >> 16);
        data[2] = (uint8_t)(value >> 8);
        data[3] = (uint8_t)value;
    }
}

static void patch_exif_ifd(uint8_t *tiff, size_t size, uint32_t offset,
                           bool little, uint32_t width, uint32_t height,
                           bool root, int depth)
{
    if (depth > 3 || offset > size || size - offset < 2)
        return;
    uint16_t count = exif_u16(tiff + offset, little);
    size_t entries = (size_t)count * 12;
    if (entries > size - offset - 2 || size - offset - 2 - entries < 4)
        return;
    uint32_t exif_ifd = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t *entry = tiff + offset + 2 + (size_t)i * 12;
        uint16_t tag = exif_u16(entry, little);
        uint16_t type = exif_u16(entry + 2, little);
        uint32_t values = exif_u32(entry + 4, little);
        if (tag == 0x8769 && values == 1)
            exif_ifd = exif_u32(entry + 8, little);
        uint32_t replacement = 0;
        if (tag == 0x0100 || tag == 0xa002)
            replacement = width;
        else if (tag == 0x0101 || tag == 0xa003)
            replacement = height;
        else
            continue;
        if (values != 1)
            continue;
        if (type == 3 && replacement <= UINT16_MAX)
            exif_put_u16(entry + 8, little, (uint16_t)replacement);
        else if (type == 4 || type == 9)
            exif_put_u32(entry + 8, little, replacement);
    }
    if (root)
        exif_put_u32(tiff + offset + 2 + entries, little, 0);
    if (exif_ifd)
        patch_exif_ifd(tiff, size, exif_ifd, little, width, height, false,
                       depth + 1);
}

static void patch_exif(uint8_t *data, size_t size, uint32_t width,
                       uint32_t height)
{
    if (size >= 6 && memcmp(data, "Exif\0\0", 6) == 0) {
        data += 6;
        size -= 6;
    }
    if (size < 8)
        return;
    bool little;
    if (memcmp(data, "II", 2) == 0)
        little = true;
    else if (memcmp(data, "MM", 2) == 0)
        little = false;
    else
        return;
    if (exif_u16(data + 2, little) != 42)
        return;
    patch_exif_ifd(data, size, exif_u32(data + 4, little), little,
                   width, height, true, 0);
}

static int read_file(const char *path, struct file_data *file,
                     char *error, size_t error_size)
{
    FILE *stream = fopen(path, "rb");
    long length;

    if (!stream) {
        set_errno_error(error, error_size, "cannot open input");
        return -1;
    }
    if (fseek(stream, 0, SEEK_END) < 0 || (length = ftell(stream)) < 0 ||
        fseek(stream, 0, SEEK_SET) < 0) {
        set_errno_error(error, error_size, "cannot inspect input");
        fclose(stream);
        return -1;
    }
    if (length == 0 || (unsigned long)length > SIZE_MAX) {
        set_error(error, error_size, "empty or oversized input");
        fclose(stream);
        return -1;
    }
    file->data = malloc((size_t)length);
    if (!file->data) {
        set_error(error, error_size, "out of memory");
        fclose(stream);
        return -1;
    }
    file->size = (size_t)length;
    if (fread(file->data, 1, file->size, stream) != file->size) {
        set_errno_error(error, error_size, "cannot read input");
        fclose(stream);
        free(file->data);
        memset(file, 0, sizeof(*file));
        return -1;
    }
    fclose(stream);
    return 0;
}

static void jpeg_fail(j_common_ptr common)
{
    struct jpeg_error *error = (struct jpeg_error *)common->err;
    (*common->err->format_message)(common, error->message);
    longjmp(error->jump, 1);
}

static void free_markers(struct marker *marker)
{
    while (marker) {
        struct marker *next = marker->next;
        free(marker->data);
        free(marker);
        marker = next;
    }
}

static int copy_jpeg_markers(struct jpeg_codec *codec,
                             const struct jpeg_decompress_struct *decompress)
{
    struct marker **tail = &codec->markers;

    for (jpeg_saved_marker_ptr source = decompress->marker_list; source;
         source = source->next) {
        struct marker *marker = calloc(1, sizeof(*marker));
        if (!marker)
            return -1;
        if (source->data_length)
            marker->data = malloc(source->data_length);
        if (source->data_length && !marker->data) {
            free(marker);
            return -1;
        }
        marker->code = source->marker;
        marker->size = source->data_length;
        if (marker->size)
            memcpy(marker->data, source->data, marker->size);
        *tail = marker;
        tail = &marker->next;
    }
    return 0;
}

static uint8_t multiply_8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((unsigned int)a * b + 127) / 255);
}

static int load_jpeg(struct image *image, struct file_data file,
                     char *error, size_t error_size)
{
    struct jpeg_codec *codec = calloc(1, sizeof(*codec));
    struct jpeg_decompress_struct decompress;
    struct jpeg_error jpeg_error;
    tjhandle probe = NULL;
    uint8_t *volatile row = NULL;

    if (!codec) {
        set_error(error, error_size, "out of memory");
        return -1;
    }
    codec->file = file;

    probe = tj3Init(TJINIT_DECOMPRESS);
    if (!probe || tj3DecompressHeader(probe, file.data, file.size) < 0 ||
        tj3Get(probe, TJPARAM_PRECISION) != 8 || tj3Get(probe, TJPARAM_LOSSLESS)) {
        set_error(error, error_size,
                  "unsupported JPEG: only common 8-bit lossy JPEG is accepted");
        if (probe)
            tj3Destroy(probe);
        free(codec);
        return -1;
    }
    tj3Destroy(probe);

    memset(&decompress, 0, sizeof(decompress));
    decompress.err = jpeg_std_error(&jpeg_error.base);
    jpeg_error.base.error_exit = jpeg_fail;
    if (setjmp(jpeg_error.jump)) {
        set_error(error, error_size, jpeg_error.message);
        jpeg_destroy_decompress(&decompress);
        free(row);
        free(codec->samples);
        if (image->frames) {
            free(image->frames[0].pixels);
            free(image->frames);
            image->frames = NULL;
        }
        free_markers(codec->markers);
        free(codec);
        return -1;
    }
    jpeg_create_decompress(&decompress);
    for (int code = 0; code < 16; code++)
        jpeg_save_markers(&decompress, JPEG_APP0 + code, 0xffff);
    jpeg_save_markers(&decompress, JPEG_COM, 0xffff);
    jpeg_mem_src(&decompress, file.data, file.size);
    jpeg_read_header(&decompress, TRUE);

    if (decompress.image_width > INT_MAX || decompress.image_height > INT_MAX) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message),
                 "JPEG dimensions are too large");
        longjmp(jpeg_error.jump, 1);
    }
    image->width = (int)decompress.image_width;
    image->height = (int)decompress.image_height;
    codec->jpeg_space = decompress.jpeg_color_space;
    codec->num_components = decompress.num_components;
    codec->progressive = decompress.progressive_mode;
    codec->arithmetic = decompress.arith_code;
    codec->restart_interval = decompress.restart_interval;
    codec->jfif = decompress.saw_JFIF_marker;
    codec->jfif_major = decompress.JFIF_major_version;
    codec->jfif_minor = decompress.JFIF_minor_version;
    codec->density_unit = decompress.density_unit;
    codec->density_x = decompress.X_density;
    codec->density_y = decompress.Y_density;
    int max_h = 1, max_v = 1;
    for (int i = 0; i < decompress.num_components && i < MAX_COMPONENTS; i++) {
        codec->h_samp[i] = decompress.comp_info[i].h_samp_factor;
        codec->v_samp[i] = decompress.comp_info[i].v_samp_factor;
        codec->quant_no[i] = decompress.comp_info[i].quant_tbl_no;
        if (codec->h_samp[i] > max_h)
            max_h = codec->h_samp[i];
        if (codec->v_samp[i] > max_v)
            max_v = codec->v_samp[i];
    }
    codec->mcu_width = max_h * DCTSIZE;
    codec->mcu_height = max_v * DCTSIZE;
    for (int i = 0; i < NUM_QUANT_TBLS; i++) {
        if (decompress.quant_tbl_ptrs[i]) {
            codec->quant_present[i] = true;
            memcpy(codec->quant[i], decompress.quant_tbl_ptrs[i]->quantval,
                   sizeof(codec->quant[i]));
        }
    }
    if (copy_jpeg_markers(codec, &decompress) < 0) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message), "out of memory");
        longjmp(jpeg_error.jump, 1);
    }

    if (decompress.jpeg_color_space == JCS_GRAYSCALE)
        decompress.out_color_space = JCS_GRAYSCALE;
    else if (decompress.jpeg_color_space == JCS_CMYK ||
             decompress.jpeg_color_space == JCS_YCCK)
        decompress.out_color_space = JCS_CMYK;
    else
        decompress.out_color_space = JCS_RGB;
    codec->input_space = decompress.out_color_space;
    jpeg_start_decompress(&decompress);
    codec->components = decompress.output_components;

    if ((size_t)image->width > SIZE_MAX / (size_t)image->height) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message),
                 "image is too large");
        longjmp(jpeg_error.jump, 1);
    }
    size_t pixel_count = (size_t)image->width * image->height;
    if (pixel_count > SIZE_MAX / sizeof(uint32_t) ||
        pixel_count > SIZE_MAX / (size_t)codec->components) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message),
                 "image is too large");
        longjmp(jpeg_error.jump, 1);
    }
    codec->samples = malloc(pixel_count * (size_t)codec->components);
    image->frames = calloc(1, sizeof(*image->frames));
    if (!codec->samples || !image->frames ||
        !(image->frames[0].pixels = malloc(pixel_count * sizeof(uint32_t)))) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message), "out of memory");
        longjmp(jpeg_error.jump, 1);
    }
    row = malloc((size_t)image->width * codec->components);
    if (!row) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message), "out of memory");
        longjmp(jpeg_error.jump, 1);
    }
    while (decompress.output_scanline < decompress.output_height) {
        JSAMPROW rows[1] = { row };
        size_t y = decompress.output_scanline;
        jpeg_read_scanlines(&decompress, rows, 1);
        memcpy(codec->samples + y * (size_t)image->width * codec->components,
               row, (size_t)image->width * codec->components);
        for (int x = 0; x < image->width; x++) {
            uint8_t r, g, b;
            if (codec->components == 1) {
                r = g = b = row[x];
            } else if (codec->components == 4) {
                uint8_t c = row[x * 4], m = row[x * 4 + 1];
                uint8_t yy = row[x * 4 + 2], k = row[x * 4 + 3];
                if (decompress.saw_Adobe_marker) {
                    r = multiply_8(c, k);
                    g = multiply_8(m, k);
                    b = multiply_8(yy, k);
                } else {
                    r = multiply_8((uint8_t)(255 - c), (uint8_t)(255 - k));
                    g = multiply_8((uint8_t)(255 - m), (uint8_t)(255 - k));
                    b = multiply_8((uint8_t)(255 - yy), (uint8_t)(255 - k));
                }
            } else {
                r = row[x * 3];
                g = row[x * 3 + 1];
                b = row[x * 3 + 2];
            }
            image->frames[0].pixels[y * (size_t)image->width + x] =
                0xff000000U | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }
    }
    free(row);
    jpeg_finish_decompress(&decompress);
    jpeg_destroy_decompress(&decompress);
    image->frame_count = 1;
    image->frames[0].opaque = true;
    image->format = IMAGE_JPEG;
    image->codec = codec;
    return 0;
}

static void png_memory_read(png_structp png, png_bytep output, size_t length)
{
    struct memory_reader *reader = png_get_io_ptr(png);
    if (length > reader->size - reader->offset)
        png_error(png, "truncated PNG");
    memcpy(output, reader->data + reader->offset, length);
    reader->offset += length;
}

static bool png_has_actl(const struct file_data *file)
{
    size_t offset = 8;
    while (offset + 12 <= file->size) {
        uint32_t length = (uint32_t)file->data[offset] << 24 |
                          (uint32_t)file->data[offset + 1] << 16 |
                          (uint32_t)file->data[offset + 2] << 8 |
                          file->data[offset + 3];
        if (memcmp(file->data + offset + 4, "acTL", 4) == 0)
            return true;
        if ((size_t)length + 12 > file->size - offset)
            break;
        offset += (size_t)length + 12;
    }
    return false;
}

static unsigned int png_sample(const uint8_t *row, int bit_depth, int index)
{
    if (bit_depth == 16)
        return (unsigned int)row[index * 2] << 8 | row[index * 2 + 1];
    if (bit_depth == 8)
        return row[index];
    int bit = index * bit_depth;
    int shift = 8 - bit_depth - bit % 8;
    return (row[bit / 8] >> shift) & ((1U << bit_depth) - 1U);
}

static uint8_t sample_to_byte(unsigned int value, int bit_depth)
{
    unsigned int maximum = bit_depth == 16 ? 65535U : (1U << bit_depth) - 1U;
    return (uint8_t)((value * 255U + maximum / 2U) / maximum);
}

static char *copy_png_string(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void free_png_texts(struct png_codec *codec)
{
    for (int i = 0; i < codec->text_count; i++) {
        free(codec->texts[i].key);
        free(codec->texts[i].text);
        free(codec->texts[i].lang);
        free(codec->texts[i].lang_key);
    }
    free(codec->texts);
    codec->texts = NULL;
    codec->text_count = 0;
}

static int append_png_texts(struct png_codec *codec, png_structp png,
                            png_infop info)
{
    png_textp source_texts = NULL;
    int text_count = 0;
    if (png_get_text(png, info, &source_texts, &text_count) <= 0 || text_count <= 0)
        return 0;
    int old_count = codec->text_count;
    png_text *texts = realloc(codec->texts,
                              (size_t)(old_count + text_count) * sizeof(*texts));
    if (!texts)
        return -1;
    codec->texts = texts;
    memset(codec->texts + old_count, 0,
           (size_t)text_count * sizeof(*codec->texts));
    codec->text_count += text_count;
    for (int i = 0; i < text_count; i++) {
        png_text *destination = &codec->texts[old_count + i];
        const png_text *source = &source_texts[i];
        size_t text_length = source->text_length;
        if (source->compression == PNG_ITXT_COMPRESSION_NONE ||
            source->compression == PNG_ITXT_COMPRESSION_zTXt)
            text_length = source->itxt_length;
        destination->compression = source->compression;
        destination->key = copy_png_string(source->key, strlen(source->key));
        destination->text = copy_png_string(source->text ? source->text : "",
                                            text_length);
        destination->text_length = source->text_length;
        destination->itxt_length = source->itxt_length;
        destination->lang = copy_png_string(source->lang ? source->lang : "",
                                            strlen(source->lang ? source->lang : ""));
        destination->lang_key = copy_png_string(
            source->lang_key ? source->lang_key : "",
            strlen(source->lang_key ? source->lang_key : ""));
        if (!destination->key || !destination->text || !destination->lang ||
            !destination->lang_key)
            return -1;
    }
    return 0;
}

static int load_png(struct image *image, struct file_data file,
                    char *error, size_t error_size)
{
    struct png_codec *codec = calloc(1, sizeof(*codec));
    png_structp png = NULL;
    png_infop info = NULL;
    png_infop end = NULL;
    struct memory_reader reader = { file.data, file.size, 0 };
    png_bytep *volatile row_pointers = NULL;

    if (!codec) {
        set_error(error, error_size, "out of memory");
        return -1;
    }
    codec->file = file;
    if (png_has_actl(&file)) {
        set_error(error, error_size, "APNG is not supported");
        free(codec);
        return -1;
    }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    end = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || !end) {
        set_error(error, error_size, "cannot initialize libpng");
        goto fail;
    }
    if (setjmp(png_jmpbuf(png))) {
        set_error(error, error_size, "invalid or unsupported PNG");
        goto fail;
    }
    png_set_read_fn(png, &reader, png_memory_read);
    png_read_info(png, info);
    png_uint_32 png_width = png_get_image_width(png, info);
    png_uint_32 png_height = png_get_image_height(png, info);
    if (!png_width || !png_height || png_width > INT_MAX || png_height > INT_MAX)
        png_error(png, "PNG dimensions are too large");
    image->width = (int)png_width;
    image->height = (int)png_height;
    codec->bit_depth = png_get_bit_depth(png, info);
    codec->color_type = png_get_color_type(png, info);
    codec->interlace = png_get_interlace_type(png, info);
    codec->channels = png_get_channels(png, info);

    png_colorp palette = NULL;
    int palette_size = 0;
    if (png_get_PLTE(png, info, &palette, &palette_size)) {
        codec->palette_size = palette_size;
        memcpy(codec->palette, palette, (size_t)palette_size * sizeof(*palette));
    }
    png_bytep alpha = NULL;
    int alpha_count = 0;
    png_color_16p trans_color = NULL;
    if (png_get_tRNS(png, info, &alpha, &alpha_count, &trans_color)) {
        codec->trans_count = alpha_count;
        if (alpha_count)
            memcpy(codec->trans_alpha, alpha, (size_t)alpha_count);
        if (trans_color) {
            codec->trans_color = *trans_color;
            codec->have_trans_color = true;
        }
    }
    codec->have_gamma = png_get_gAMA(png, info, &codec->gamma);
    codec->have_srgb = png_get_sRGB(png, info, &codec->srgb_intent);
    codec->have_phys = png_get_pHYs(png, info, &codec->phys_x, &codec->phys_y,
                                    &codec->phys_unit);
    png_timep modification_time = NULL;
    if (png_get_tIME(png, info, &modification_time)) {
        codec->modification_time = *modification_time;
        codec->have_time = true;
    }
    png_color_16p background = NULL;
    if (png_get_bKGD(png, info, &background)) {
        codec->background = *background;
        codec->have_background = true;
    }
    png_color_8p sigbits = NULL;
    if (png_get_sBIT(png, info, &sigbits)) {
        codec->sigbits = *sigbits;
        codec->have_sigbits = true;
    }
    png_charp icc_name = NULL;
    png_bytep icc_data = NULL;
    int icc_compression = 0;
    png_uint_32 icc_size = 0;
    if (png_get_iCCP(png, info, &icc_name, &icc_compression, &icc_data, &icc_size)) {
        codec->icc_name = strdup(icc_name);
        codec->icc_data = malloc(icc_size);
        if (!codec->icc_name || !codec->icc_data) {
            set_error(error, error_size, "out of memory");
            goto fail;
        }
        memcpy(codec->icc_data, icc_data, icc_size);
        codec->icc_size = icc_size;
        codec->icc_compression = icc_compression;
    }
    codec->have_chrm = png_get_cHRM(png, info, &codec->white_x, &codec->white_y,
                                    &codec->red_x, &codec->red_y,
                                    &codec->green_x, &codec->green_y,
                                    &codec->blue_x, &codec->blue_y);
    if (append_png_texts(codec, png, info) < 0) {
        set_error(error, error_size, "out of memory");
        goto fail;
    }
    png_bytep exif_data = NULL;
    png_uint_32 exif_size = 0;
    if (png_get_eXIf_1(png, info, &exif_size, &exif_data)) {
        codec->exif_data = malloc(exif_size);
        if (!codec->exif_data) {
            set_error(error, error_size, "out of memory");
            goto fail;
        }
        memcpy(codec->exif_data, exif_data, exif_size);
        codec->exif_size = exif_size;
    }

    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    codec->rowbytes = png_get_rowbytes(png, info);
    if ((size_t)image->height > SIZE_MAX / codec->rowbytes) {
        set_error(error, error_size, "image is too large");
        goto fail;
    }
    codec->rows = malloc(codec->rowbytes * (size_t)image->height);
    row_pointers = malloc((size_t)image->height * sizeof(*row_pointers));
    image->frames = calloc(1, sizeof(*image->frames));
    if ((size_t)image->width > SIZE_MAX / (size_t)image->height ||
        (size_t)image->width * image->height > SIZE_MAX / sizeof(uint32_t)) {
        set_error(error, error_size, "image is too large");
        goto fail;
    }
    size_t pixel_count = (size_t)image->width * image->height;
    if (!codec->rows || !row_pointers || !image->frames ||
        !(image->frames[0].pixels = malloc(pixel_count * sizeof(uint32_t)))) {
        set_error(error, error_size, "out of memory");
        goto fail;
    }
    for (int y = 0; y < image->height; y++)
        row_pointers[y] = codec->rows + (size_t)y * codec->rowbytes;
    png_read_image(png, row_pointers);
    png_read_end(png, end);
    if (append_png_texts(codec, png, end) < 0) {
        set_error(error, error_size, "out of memory");
        goto fail;
    }
    if (!codec->have_time && png_get_tIME(png, end, &modification_time)) {
        codec->modification_time = *modification_time;
        codec->have_time = true;
    }

    image->frames[0].opaque = true;
    for (int y = 0; y < image->height; y++) {
        const uint8_t *row_data = row_pointers[y];
        for (int x = 0; x < image->width; x++) {
            unsigned int values[4] = {0, 0, 0, 0xffff};
            uint8_t r, g, b, a = 255;
            if (codec->color_type == PNG_COLOR_TYPE_PALETTE) {
                unsigned int index = png_sample(row_data, codec->bit_depth, x);
                if (index >= (unsigned int)codec->palette_size)
                    index = 0;
                r = codec->palette[index].red;
                g = codec->palette[index].green;
                b = codec->palette[index].blue;
                if (index < (unsigned int)codec->trans_count)
                    a = codec->trans_alpha[index];
            } else {
                for (int c = 0; c < codec->channels; c++)
                    values[c] = png_sample(row_data, codec->bit_depth,
                                           x * codec->channels + c);
                if (codec->color_type == PNG_COLOR_TYPE_GRAY ||
                    codec->color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
                    r = g = b = sample_to_byte(values[0], codec->bit_depth);
                    if (codec->color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
                        a = sample_to_byte(values[1], codec->bit_depth);
                    else if (codec->have_trans_color &&
                             values[0] == codec->trans_color.gray)
                        a = 0;
                } else {
                    r = sample_to_byte(values[0], codec->bit_depth);
                    g = sample_to_byte(values[1], codec->bit_depth);
                    b = sample_to_byte(values[2], codec->bit_depth);
                    if (codec->color_type == PNG_COLOR_TYPE_RGB_ALPHA)
                        a = sample_to_byte(values[3], codec->bit_depth);
                    else if (codec->have_trans_color &&
                             values[0] == codec->trans_color.red &&
                             values[1] == codec->trans_color.green &&
                             values[2] == codec->trans_color.blue)
                        a = 0;
                }
            }
            image->frames[0].pixels[(size_t)y * image->width + x] =
                (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
            if (a != 255)
                image->frames[0].opaque = false;
        }
    }
    free((void *)row_pointers);
    png_destroy_read_struct(&png, &info, &end);
    image->frame_count = 1;
    image->format = IMAGE_PNG;
    image->codec = codec;
    return 0;

fail:
    free((void *)row_pointers);
    if (png)
        png_destroy_read_struct(&png, info ? &info : NULL, end ? &end : NULL);
    free(codec->icc_name);
    free(codec->icc_data);
    free_png_texts(codec);
    free(codec->exif_data);
    free(codec->rows);
    if (image->frames) {
        free(image->frames[0].pixels);
        free(image->frames);
        image->frames = NULL;
    }
    free(codec);
    return -1;
}

static uint32_t rgba_to_argb(const uint8_t *rgba)
{
    return (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 |
           (uint32_t)rgba[1] << 8 | rgba[2];
}

static int copy_webp_chunk(WebPDemuxer *demux, struct webp_chunk *chunk,
                           const char fourcc[4])
{
    WebPChunkIterator iterator;
    memcpy(chunk->fourcc, fourcc, 4);
    if (!WebPDemuxGetChunk(demux, fourcc, 1, &iterator))
        return 0;
    if (iterator.chunk.size)
        chunk->data = malloc(iterator.chunk.size);
    if (iterator.chunk.size && !chunk->data) {
        WebPDemuxReleaseChunkIterator(&iterator);
        return -1;
    }
    if (iterator.chunk.size)
        memcpy(chunk->data, iterator.chunk.bytes, iterator.chunk.size);
    chunk->size = iterator.chunk.size;
    WebPDemuxReleaseChunkIterator(&iterator);
    return 0;
}

struct vp8_bool_reader {
    const uint8_t *data;
    size_t size;
    size_t offset;
    unsigned int range;
    unsigned int value;
    int count;
    bool failed;
};

static void vp8_bool_init(struct vp8_bool_reader *reader,
                          const uint8_t *data, size_t size)
{
    memset(reader, 0, sizeof(*reader));
    reader->data = data;
    reader->size = size;
    reader->range = 255;
    if (size < 2) {
        reader->failed = true;
        return;
    }
    reader->value = (unsigned int)data[0] << 8 | data[1];
    reader->offset = 2;
}

static unsigned int vp8_bool_bit(struct vp8_bool_reader *reader, unsigned int probability)
{
    unsigned int split = 1 + ((reader->range - 1) * probability >> 8);
    unsigned int split_value = split << 8;
    unsigned int bit;

    if (reader->failed)
        return 0;
    if (reader->value >= split_value) {
        bit = 1;
        reader->range -= split;
        reader->value -= split_value;
    } else {
        bit = 0;
        reader->range = split;
    }
    while (reader->range < 128) {
        reader->range <<= 1;
        reader->value <<= 1;
        if (++reader->count == 8) {
            reader->count = 0;
            if (reader->offset >= reader->size) {
                reader->failed = true;
                return 0;
            }
            reader->value |= reader->data[reader->offset++];
        }
    }
    return bit;
}

static unsigned int vp8_bool_value(struct vp8_bool_reader *reader, int bits)
{
    unsigned int value = 0;
    while (bits-- > 0)
        value = value << 1 | vp8_bool_bit(reader, 128);
    return value;
}

static int vp8_signed_value(struct vp8_bool_reader *reader, int bits)
{
    if (vp8_bool_bit(reader, 128)) {
        int value = (int)vp8_bool_value(reader, bits);
        return vp8_bool_bit(reader, 128) ? -value : value;
    }
    return 0;
}

static float vp8_effective_quantizer(const uint8_t *data, size_t size)
{
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0) {
        data += 12;
        size -= 12;
    }
    if (!(size >= 6 && (data[0] & 1) == 0 && data[3] == 0x9d &&
          data[4] == 0x01 && data[5] == 0x2a)) {
        while (size >= 8 && memcmp(data, "VP8 ", 4) != 0) {
            uint32_t chunk_size = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
                                  (uint32_t)data[6] << 16 |
                                  (uint32_t)data[7] << 24;
            size_t skip = 8 + (size_t)chunk_size + (chunk_size & 1U);
            if (skip > size)
                return -1.0f;
            data += skip;
            size -= skip;
        }
    }
    if (size >= 8 && memcmp(data, "VP8 ", 4) == 0) {
        uint32_t chunk_size = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
                              (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
        if (chunk_size > size - 8)
            return -1.0f;
        data += 8;
        size = chunk_size;
    }
    if (size < 10 || (data[0] & 1) != 0 || data[3] != 0x9d ||
        data[4] != 0x01 || data[5] != 0x2a)
        return -1.0f;
    uint32_t partition_size = ((uint32_t)data[0] | (uint32_t)data[1] << 8 |
                               (uint32_t)data[2] << 16) >> 5;
    if (partition_size > size - 10)
        return -1.0f;
    struct vp8_bool_reader reader;
    vp8_bool_init(&reader, data + 10, partition_size);
    (void)vp8_bool_bit(&reader, 128); /* colorspace */
    (void)vp8_bool_bit(&reader, 128); /* clamp */

    bool segmented = vp8_bool_bit(&reader, 128);
    bool absolute = false;
    int segment_quantizer[4] = {0, 0, 0, 0};
    bool have_segment_data = false;
    if (segmented) {
        unsigned int update_map = vp8_bool_bit(&reader, 128);
        if (vp8_bool_bit(&reader, 128)) { /* update data */
            absolute = vp8_bool_bit(&reader, 128);
            have_segment_data = true;
            for (int i = 0; i < 4; i++)
                segment_quantizer[i] = vp8_signed_value(&reader, 7);
            for (int i = 0; i < 4; i++)
                (void)vp8_signed_value(&reader, 6);
        }
        if (update_map) {
            for (int i = 0; i < 3; i++) {
                if (vp8_bool_bit(&reader, 128))
                    (void)vp8_bool_value(&reader, 8);
            }
        }
    }
    (void)vp8_bool_bit(&reader, 128); /* simple filter */
    (void)vp8_bool_value(&reader, 6); /* filter level */
    (void)vp8_bool_value(&reader, 3); /* sharpness */
    if (vp8_bool_bit(&reader, 128) && vp8_bool_bit(&reader, 128)) {
        for (int i = 0; i < 8; i++)
            (void)vp8_signed_value(&reader, 6);
    }
    (void)vp8_bool_value(&reader, 2); /* token partitions */
    int base_quantizer = (int)vp8_bool_value(&reader, 7);
    for (int i = 0; i < 5; i++)
        (void)vp8_signed_value(&reader, 4);
    if (reader.failed)
        return -1.0f;
    if (!segmented || !have_segment_data)
        return (float)base_quantizer;
    float total = 0.0f;
    for (int i = 0; i < 4; i++) {
        int value = absolute ? segment_quantizer[i] :
                              base_quantizer + segment_quantizer[i];
        if (value < 0)
            value = 0;
        if (value > 127)
            value = 127;
        total += value;
    }
    return total / 4.0f;
}

static float infer_webp_quality(const uint8_t *data, size_t size)
{
    static const struct {
        float quantizer;
        float quality;
    } points[] = {
        {127, 0}, {86, 5}, {75, 10}, {68, 15}, {62, 20}, {57, 25},
        {52, 30}, {48, 35}, {45, 40}, {41, 45}, {38, 50}, {36, 55},
        {33, 60}, {30, 65}, {28, 70}, {26, 75}, {19, 80}, {17, 82},
        {14, 85}, {9, 90}, {4, 95}, {0, 100},
    };
    float quantizer = vp8_effective_quantizer(data, size);
    if (quantizer < 0.0f)
        return 75.0f;
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); i++) {
        if (quantizer >= points[i].quantizer) {
            float fraction = (points[i - 1].quantizer - quantizer) /
                (points[i - 1].quantizer - points[i].quantizer);
            return points[i - 1].quality + fraction *
                (points[i].quality - points[i - 1].quality);
        }
    }
    return 100.0f;
}

static int load_webp(struct image *image, struct file_data file,
                     char *error, size_t error_size)
{
    struct webp_codec *codec = calloc(1, sizeof(*codec));
    WebPData data = { file.data, file.size };
    WebPDemuxer *demux = NULL;
    WebPAnimDecoder *decoder = NULL;
    WebPAnimDecoderOptions options;
    WebPAnimInfo info;
    WebPIterator iterator;

    if (!codec) {
        set_error(error, error_size, "out of memory");
        return -1;
    }
    codec->file = file;
    demux = WebPDemux(&data);
    if (!demux) {
        set_error(error, error_size, "invalid WebP");
        goto fail;
    }
    image->width = (int)WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    image->height = (int)WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    image->frame_count = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
    image->loop_count = (int)WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);
    image->background = WebPDemuxGetI(demux, WEBP_FF_BACKGROUND_COLOR);
    if (!image->frame_count)
        image->frame_count = 1;
    image->frames = calloc(image->frame_count, sizeof(*image->frames));
    codec->frames = calloc(image->frame_count, sizeof(*codec->frames));
    if (!image->frames || !codec->frames) {
        set_error(error, error_size, "out of memory");
        goto fail;
    }
    if (!WebPAnimDecoderOptionsInit(&options)) {
        set_error(error, error_size, "cannot initialize WebP animation decoder");
        goto fail;
    }
    options.color_mode = MODE_RGBA;
    decoder = WebPAnimDecoderNew(&data, &options);
    if (!decoder || !WebPAnimDecoderGetInfo(decoder, &info)) {
        set_error(error, error_size, "cannot decode WebP");
        goto fail;
    }
    if (image->width <= 0 || image->height <= 0 ||
        (size_t)image->width > SIZE_MAX / (size_t)image->height ||
        (size_t)image->width * image->height > SIZE_MAX / sizeof(uint32_t)) {
        set_error(error, error_size, "WebP dimensions are too large");
        goto fail;
    }
    size_t pixel_count = (size_t)image->width * image->height;
    int previous_timestamp = 0;
    for (size_t i = 0; i < image->frame_count; i++) {
        uint8_t *rgba = NULL;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(decoder, &rgba, &timestamp) || !rgba) {
            set_error(error, error_size, "truncated WebP animation");
            goto fail;
        }
        image->frames[i].pixels = malloc(pixel_count * sizeof(uint32_t));
        if (!image->frames[i].pixels) {
            set_error(error, error_size, "out of memory");
            goto fail;
        }
        image->frames[i].opaque = true;
        for (size_t p = 0; p < pixel_count; p++) {
            image->frames[i].pixels[p] = rgba_to_argb(rgba + p * 4);
            if (rgba[p * 4 + 3] != 255)
                image->frames[i].opaque = false;
        }
        image->frames[i].duration_ms = timestamp - previous_timestamp;
        if (image->frames[i].duration_ms < 0) {
            set_error(error, error_size, "invalid WebP frame timing");
            goto fail;
        }
        previous_timestamp = timestamp;

        if (WebPDemuxGetFrame(demux, (int)i + 1, &iterator)) {
            WebPBitstreamFeatures features;
            bool lossless = WebPGetFeatures(iterator.fragment.bytes,
                                            iterator.fragment.size,
                                            &features) == VP8_STATUS_OK &&
                            features.format == 2;
            codec->frames[i].lossless = lossless;
            codec->frames[i].quality = lossless ? 100.0f :
                infer_webp_quality(iterator.fragment.bytes, iterator.fragment.size);
            image->frames[i].lossless = codec->frames[i].lossless;
            image->frames[i].quality = codec->frames[i].quality;
            WebPDemuxReleaseIterator(&iterator);
        }
    }
    if (copy_webp_chunk(demux, &codec->chunks[0], "ICCP") < 0 ||
        copy_webp_chunk(demux, &codec->chunks[1], "EXIF") < 0 ||
        copy_webp_chunk(demux, &codec->chunks[2], "XMP ") < 0) {
        set_error(error, error_size, "out of memory");
        goto fail;
    }
    WebPAnimDecoderDelete(decoder);
    WebPDemuxDelete(demux);
    image->format = IMAGE_WEBP;
    image->codec = codec;
    return 0;

fail:
    if (decoder)
        WebPAnimDecoderDelete(decoder);
    if (demux)
        WebPDemuxDelete(demux);
    if (image->frames) {
        for (size_t i = 0; i < image->frame_count; i++)
            free(image->frames[i].pixels);
        free(image->frames);
        image->frames = NULL;
    }
    for (size_t i = 0; i < 3; i++)
        free(codec->chunks[i].data);
    free(codec->frames);
    free(codec);
    return -1;
}

int image_load(struct image *image, const char *path, char *error, size_t error_size)
{
    struct file_data file = {0};
    int result;

    memset(image, 0, sizeof(*image));
    if (read_file(path, &file, error, error_size) < 0)
        return -1;
    if (file.size >= 3 && file.data[0] == 0xff && file.data[1] == 0xd8 &&
        file.data[2] == 0xff) {
        result = load_jpeg(image, file, error, error_size);
    } else if (file.size >= 8 && png_sig_cmp(file.data, 0, 8) == 0) {
        result = load_png(image, file, error, error_size);
    } else if (file.size >= 12 && memcmp(file.data, "RIFF", 4) == 0 &&
               memcmp(file.data + 8, "WEBP", 4) == 0) {
        result = load_webp(image, file, error, error_size);
    } else {
        set_error(error, error_size, "unsupported format (expected JPEG, PNG, or WebP)");
        result = -1;
    }
    if (result < 0)
        free(file.data);
    return result;
}

static int save_jpeg_lossless(const struct jpeg_codec *codec, const char *path,
                              int x, int y, int width, int height,
                              char *error, size_t error_size)
{
    tjhandle handle = tj3Init(TJINIT_TRANSFORM);
    unsigned char *output = NULL;
    size_t output_size = 0;
    FILE *stream = NULL;
    int result = -1;

    if (!handle || tj3Set(handle, TJPARAM_SAVEMARKERS, 2) < 0 ||
        tj3DecompressHeader(handle, codec->file.data, codec->file.size) < 0) {
        set_error(error, error_size, handle ? tj3GetErrorStr(handle) :
                  "cannot initialize TurboJPEG");
        goto out;
    }
    if (codec->restart_interval &&
        tj3Set(handle, TJPARAM_RESTARTBLOCKS,
               (int)codec->restart_interval) < 0) {
        set_error(error, error_size, tj3GetErrorStr(handle));
        goto out;
    }
    tjtransform transform;
    memset(&transform, 0, sizeof(transform));
    transform.r = (tjregion){x, y, width, height};
    transform.op = TJXOP_NONE;
    transform.options = TJXOPT_CROP | TJXOPT_OPTIMIZE;
    if (tj3Transform(handle, codec->file.data, codec->file.size, 1,
                     &output, &output_size, &transform) < 0) {
        set_error(error, error_size, tj3GetErrorStr(handle));
        goto out;
    }
    size_t read_offset = 2, write_offset = 2;
    while (read_offset + 4 <= output_size) {
        if (output[read_offset] != 0xff || output[read_offset + 1] == 0xff)
            break;
        uint8_t code = output[read_offset + 1];
        if (code == 0xda || code == JPEG_EOI)
            break;
        if (code == 0 || (code >= JPEG_RST0 && code <= JPEG_RST0 + 7))
            break;
        size_t length = (size_t)output[read_offset + 2] << 8 |
                        output[read_offset + 3];
        if (length < 2 || length > output_size - read_offset - 2)
            break;
        size_t total = length + 2;
        uint8_t *payload = output + read_offset + 4;
        size_t payload_size = length - 2;
        bool jfxx = code == JPEG_APP0 && payload_size >= 5 &&
                    memcmp(payload, "JFXX\0", 5) == 0;
        if (jfxx) {
            read_offset += total;
            continue;
        }
        if (code == JPEG_APP0 && payload_size >= 14 &&
            memcmp(payload, "JFIF\0", 5) == 0 && (payload[12] || payload[13])) {
            output[write_offset] = 0xff;
            output[write_offset + 1] = code;
            output[write_offset + 2] = 0;
            output[write_offset + 3] = 16;
            memmove(output + write_offset + 4, payload, 14);
            output[write_offset + 16] = 0;
            output[write_offset + 17] = 0;
            write_offset += 18;
        } else {
            if (code == JPEG_APP0 + 1 && payload_size >= 6 &&
                memcmp(payload, "Exif\0\0", 6) == 0)
                patch_exif(payload, payload_size,
                           (uint32_t)width, (uint32_t)height);
            memmove(output + write_offset, output + read_offset, total);
            write_offset += total;
        }
        read_offset += total;
    }
    if (write_offset != read_offset) {
        memmove(output + write_offset, output + read_offset,
                output_size - read_offset);
        output_size -= read_offset - write_offset;
    }
    stream = fopen(path, "wb");
    if (!stream || fwrite(output, 1, output_size, stream) != output_size) {
        set_errno_error(error, error_size, "cannot write JPEG");
        goto out;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        set_errno_error(error, error_size, "cannot finish JPEG");
        goto out;
    }
    stream = NULL;
    result = 0;
out:
    if (stream)
        fclose(stream);
    if (output)
        tj3Free(output);
    if (handle)
        tj3Destroy(handle);
    return result;
}

static void write_jpeg_marker(struct jpeg_compress_struct *compress,
                              const struct marker *marker,
                              int width, int height)
{
    if (marker->code == JPEG_APP0 + 1 && marker->size >= 6 &&
        memcmp(marker->data, "Exif\0\0", 6) == 0) {
        uint8_t *copy = malloc(marker->size);
        if (!copy)
            ERREXIT(compress, JERR_OUT_OF_MEMORY);
        memcpy(copy, marker->data, marker->size);
        patch_exif(copy, marker->size, (uint32_t)width, (uint32_t)height);
        jpeg_write_marker(compress, marker->code, copy,
                          (unsigned int)marker->size);
        free(copy);
    } else {
        jpeg_write_marker(compress, marker->code, marker->data,
                          (unsigned int)marker->size);
    }
}

static int save_jpeg(const struct image *image, const char *path,
                     int x, int y, int width, int height, bool require_lossless,
                     char *error, size_t error_size)
{
    const struct jpeg_codec *codec = image->codec;
    bool aligned = x % codec->mcu_width == 0 && y % codec->mcu_height == 0;
    if (aligned)
        return save_jpeg_lossless(codec, path, x, y, width, height,
                                  error, error_size);
    if (require_lossless) {
        set_error(error, error_size, "JPEG crop origin is not MCU-aligned");
        return -1;
    }

    struct jpeg_compress_struct compress;
    struct jpeg_error jpeg_error;
    FILE *stream = fopen(path, "wb");
    uint8_t *volatile row = NULL;
    if (!stream) {
        set_errno_error(error, error_size, "cannot create JPEG");
        return -1;
    }
    memset(&compress, 0, sizeof(compress));
    compress.err = jpeg_std_error(&jpeg_error.base);
    jpeg_error.base.error_exit = jpeg_fail;
    if (setjmp(jpeg_error.jump)) {
        set_error(error, error_size, jpeg_error.message);
        jpeg_destroy_compress(&compress);
        free(row);
        fclose(stream);
        return -1;
    }
    jpeg_create_compress(&compress);
    jpeg_stdio_dest(&compress, stream);
    compress.image_width = (JDIMENSION)width;
    compress.image_height = (JDIMENSION)height;
    compress.input_components = codec->components;
    compress.in_color_space = codec->input_space;
    jpeg_set_defaults(&compress);
    jpeg_set_colorspace(&compress, codec->jpeg_space);
    for (const struct marker *marker = codec->markers; marker; marker = marker->next) {
        if (marker->code == JPEG_APP0 + 14) {
            compress.write_Adobe_marker = FALSE;
            break;
        }
    }
    for (int i = 0; i < NUM_QUANT_TBLS; i++) {
        if (!codec->quant_present[i])
            continue;
        if (!compress.quant_tbl_ptrs[i])
            compress.quant_tbl_ptrs[i] = jpeg_alloc_quant_table((j_common_ptr)&compress);
        memcpy(compress.quant_tbl_ptrs[i]->quantval, codec->quant[i],
               sizeof(codec->quant[i]));
        compress.quant_tbl_ptrs[i]->sent_table = FALSE;
    }
    for (int i = 0; i < compress.num_components && i < codec->num_components; i++) {
        compress.comp_info[i].h_samp_factor = codec->h_samp[i];
        compress.comp_info[i].v_samp_factor = codec->v_samp[i];
        compress.comp_info[i].quant_tbl_no = codec->quant_no[i];
    }
    compress.optimize_coding = TRUE;
    compress.arith_code = codec->arithmetic;
    compress.restart_interval = codec->restart_interval;
    compress.write_JFIF_header = codec->jfif;
    compress.JFIF_major_version = (UINT8)codec->jfif_major;
    compress.JFIF_minor_version = (UINT8)codec->jfif_minor;
    compress.density_unit = (UINT8)codec->density_unit;
    compress.X_density = (UINT16)codec->density_x;
    compress.Y_density = (UINT16)codec->density_y;
    if (codec->progressive)
        jpeg_simple_progression(&compress);
    jpeg_start_compress(&compress, TRUE);
    for (const struct marker *marker = codec->markers; marker; marker = marker->next) {
        if (marker->code != JPEG_APP0)
            write_jpeg_marker(&compress, marker, width, height);
    }
    row = malloc((size_t)width * codec->components);
    if (!row) {
        snprintf(jpeg_error.message, sizeof(jpeg_error.message), "out of memory");
        longjmp(jpeg_error.jump, 1);
    }
    for (int output_y = 0; output_y < height; output_y++) {
        const uint8_t *source = codec->samples +
            ((size_t)(y + output_y) * image->width + x) * codec->components;
        memcpy(row, source, (size_t)width * codec->components);
        JSAMPROW rows[1] = { row };
        jpeg_write_scanlines(&compress, rows, 1);
    }
    jpeg_finish_compress(&compress);
    jpeg_destroy_compress(&compress);
    free(row);
    if (fclose(stream) != 0) {
        set_errno_error(error, error_size, "cannot finish JPEG");
        return -1;
    }
    return 0;
}

static void png_write_sample(uint8_t *row, int bit_depth, int index,
                             unsigned int value)
{
    if (bit_depth == 16) {
        row[index * 2] = (uint8_t)(value >> 8);
        row[index * 2 + 1] = (uint8_t)value;
    } else if (bit_depth == 8) {
        row[index] = (uint8_t)value;
    } else {
        int bit = index * bit_depth;
        int shift = 8 - bit_depth - bit % 8;
        unsigned int mask = ((1U << bit_depth) - 1U) << shift;
        row[bit / 8] = (uint8_t)((row[bit / 8] & ~mask) | value << shift);
    }
}

static int save_png(const struct image *image, const char *path,
                    int x, int y, int width, int height,
                    char *error, size_t error_size)
{
    const struct png_codec *codec = image->codec;
    FILE *stream = fopen(path, "wb");
    png_structp png = NULL;
    png_infop info = NULL;
    uint8_t *volatile rows = NULL;
    uint8_t *volatile exif = NULL;
    png_bytep *volatile pointers = NULL;
    int result = -1;

    if (!stream) {
        set_errno_error(error, error_size, "cannot create PNG");
        return -1;
    }
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        set_error(error, error_size, "cannot initialize libpng");
        goto out;
    }
    if (setjmp(png_jmpbuf(png))) {
        set_error(error, error_size, "cannot encode PNG");
        goto out;
    }
    png_init_io(png, stream);
    png_set_compression_level(png, 9);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height,
                 codec->bit_depth, codec->color_type, codec->interlace,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    if (codec->palette_size)
        png_set_PLTE(png, info, (png_colorp)codec->palette, codec->palette_size);
    if (codec->trans_count || codec->have_trans_color)
        png_set_tRNS(png, info, codec->trans_count ? (png_bytep)codec->trans_alpha : NULL,
                     codec->trans_count,
                     codec->have_trans_color ? (png_color_16p)&codec->trans_color : NULL);
    if (codec->have_gamma)
        png_set_gAMA(png, info, codec->gamma);
    if (codec->have_srgb)
        png_set_sRGB(png, info, codec->srgb_intent);
    if (codec->have_phys)
        png_set_pHYs(png, info, codec->phys_x, codec->phys_y, codec->phys_unit);
    if (codec->have_time)
        png_set_tIME(png, info, (png_timep)&codec->modification_time);
    if (codec->have_background)
        png_set_bKGD(png, info, (png_color_16p)&codec->background);
    if (codec->have_sigbits)
        png_set_sBIT(png, info, (png_color_8p)&codec->sigbits);
    if (codec->icc_data)
        png_set_iCCP(png, info, codec->icc_name, codec->icc_compression,
                     codec->icc_data, codec->icc_size);
    if (codec->have_chrm)
        png_set_cHRM(png, info, codec->white_x, codec->white_y,
                     codec->red_x, codec->red_y, codec->green_x,
                     codec->green_y, codec->blue_x, codec->blue_y);
    if (codec->text_count)
        png_set_text(png, info, codec->texts, codec->text_count);
    if (codec->exif_data) {
        exif = malloc(codec->exif_size);
        if (!exif) {
            set_error(error, error_size, "out of memory");
            goto out;
        }
        memcpy(exif, codec->exif_data, codec->exif_size);
        patch_exif(exif, codec->exif_size, (uint32_t)width, (uint32_t)height);
        png_set_eXIf_1(png, info, codec->exif_size, exif);
    }

    size_t bits_per_pixel = (size_t)codec->channels * codec->bit_depth;
    size_t rowbytes = ((size_t)width * bits_per_pixel + 7) / 8;
    rows = calloc((size_t)height, rowbytes);
    pointers = malloc((size_t)height * sizeof(*pointers));
    if (!rows || !pointers) {
        set_error(error, error_size, "out of memory");
        goto out;
    }
    for (int output_y = 0; output_y < height; output_y++) {
        const uint8_t *source = codec->rows + (size_t)(y + output_y) * codec->rowbytes;
        uint8_t *destination = rows + (size_t)output_y * rowbytes;
        pointers[output_y] = destination;
        if (codec->bit_depth >= 8) {
            size_t bytes_per_pixel = bits_per_pixel / 8;
            memcpy(destination, source + (size_t)x * bytes_per_pixel,
                   (size_t)width * bytes_per_pixel);
        } else {
            for (int output_x = 0; output_x < width; output_x++) {
                unsigned int value = png_sample(source, codec->bit_depth, x + output_x);
                png_write_sample(destination, codec->bit_depth, output_x, value);
            }
        }
    }
    png_write_info(png, info);
    int passes = png_set_interlace_handling(png);
    for (int pass = 0; pass < passes; pass++) {
        for (int output_y = 0; output_y < height; output_y++)
            png_write_row(png, pointers[output_y]);
    }
    png_write_end(png, info);
    result = 0;
out:
    free((void *)rows);
    free((void *)pointers);
    free((void *)exif);
    if (png)
        png_destroy_write_struct(&png, info ? &info : NULL);
    if (fclose(stream) != 0 && result == 0) {
        set_errno_error(error, error_size, "cannot finish PNG");
        result = -1;
    }
    return result;
}

static uint8_t *crop_rgba(const struct image *image, size_t frame,
                          int x, int y, int width, int height)
{
    uint8_t *rgba = malloc((size_t)width * height * 4);
    if (!rgba)
        return NULL;
    const uint32_t *pixels = image->frames[frame].pixels;
    for (int output_y = 0; output_y < height; output_y++) {
        for (int output_x = 0; output_x < width; output_x++) {
            uint32_t pixel = pixels[(size_t)(y + output_y) * image->width + x + output_x];
            uint8_t *destination = rgba +
                ((size_t)output_y * width + output_x) * 4;
            destination[0] = (uint8_t)(pixel >> 16);
            destination[1] = (uint8_t)(pixel >> 8);
            destination[2] = (uint8_t)pixel;
            destination[3] = (uint8_t)(pixel >> 24);
        }
    }
    return rgba;
}

static int apply_webp_chunks(WebPMux *mux, const struct webp_codec *codec,
                             int width, int height)
{
    for (size_t i = 0; i < 3; i++) {
        if (!codec->chunks[i].data)
            continue;
        uint8_t *copy = NULL;
        const uint8_t *bytes = codec->chunks[i].data;
        if (memcmp(codec->chunks[i].fourcc, "EXIF", 4) == 0) {
            copy = malloc(codec->chunks[i].size);
            if (!copy)
                return -1;
            memcpy(copy, bytes, codec->chunks[i].size);
            patch_exif(copy, codec->chunks[i].size,
                       (uint32_t)width, (uint32_t)height);
            bytes = copy;
        }
        WebPData data = { bytes, codec->chunks[i].size };
        WebPMuxError mux_error = WebPMuxSetChunk(mux, codec->chunks[i].fourcc,
                                                 &data, 1);
        free(copy);
        if (mux_error != WEBP_MUX_OK)
            return -1;
    }
    return 0;
}

static int configure_webp(WebPConfig *config, const struct webp_frame_info *source)
{
    if (!WebPConfigInit(config))
        return -1;
    config->lossless = source->lossless;
    config->quality = roundf(source->quality);
    config->method = 6;
    config->exact = 1;
    config->alpha_quality = 100;
    return WebPValidateConfig(config) ? 0 : -1;
}

static int save_webp(const struct image *image, const char *path,
                     int x, int y, int width, int height,
                     char *error, size_t error_size)
{
    const struct webp_codec *codec = image->codec;
    WebPData encoded = {0};
    int result = -1;

    if (image->frame_count == 1) {
        uint8_t *rgba = crop_rgba(image, 0, x, y, width, height);
        WebPPicture picture;
        WebPConfig config;
        WebPMemoryWriter writer;
        if (!rgba || configure_webp(&config, &codec->frames[0]) < 0 ||
            !WebPPictureInit(&picture)) {
            free(rgba);
            set_error(error, error_size, "cannot initialize WebP encoder");
            return -1;
        }
        picture.width = width;
        picture.height = height;
        WebPMemoryWriterInit(&writer);
        picture.writer = WebPMemoryWrite;
        picture.custom_ptr = &writer;
        if (!WebPPictureImportRGBA(&picture, rgba, width * 4) ||
            !WebPEncode(&config, &picture)) {
            set_error(error, error_size, "cannot encode WebP");
            WebPPictureFree(&picture);
            WebPMemoryWriterClear(&writer);
            free(rgba);
            return -1;
        }
        encoded.bytes = writer.mem;
        encoded.size = writer.size;
        WebPPictureFree(&picture);
        free(rgba);
    } else {
        WebPAnimEncoderOptions options;
        if (!WebPAnimEncoderOptionsInit(&options)) {
            set_error(error, error_size, "cannot initialize WebP animation encoder");
            return -1;
        }
        options.anim_params.bgcolor = image->background;
        options.anim_params.loop_count = image->loop_count;
        options.minimize_size = 1;
        WebPAnimEncoder *encoder = WebPAnimEncoderNew(width, height, &options);
        if (!encoder) {
            set_error(error, error_size, "cannot initialize WebP animation encoder");
            return -1;
        }
        int timestamp = 0;
        for (size_t i = 0; i < image->frame_count; i++) {
            uint8_t *rgba = crop_rgba(image, i, x, y, width, height);
            WebPPicture picture;
            WebPConfig config;
            if (!rgba || configure_webp(&config, &codec->frames[i]) < 0 ||
                !WebPPictureInit(&picture)) {
                free(rgba);
                set_error(error, error_size, "cannot prepare WebP frame");
                WebPAnimEncoderDelete(encoder);
                return -1;
            }
            picture.width = width;
            picture.height = height;
            if (!WebPPictureImportRGBA(&picture, rgba, width * 4) ||
                !WebPAnimEncoderAdd(encoder, &picture, timestamp, &config)) {
                set_error(error, error_size, WebPAnimEncoderGetError(encoder));
                WebPPictureFree(&picture);
                free(rgba);
                WebPAnimEncoderDelete(encoder);
                return -1;
            }
            WebPPictureFree(&picture);
            free(rgba);
            if (image->frames[i].duration_ms > INT_MAX - timestamp) {
                set_error(error, error_size, "WebP animation is too long");
                WebPAnimEncoderDelete(encoder);
                return -1;
            }
            timestamp += image->frames[i].duration_ms;
        }
        if (!WebPAnimEncoderAdd(encoder, NULL, timestamp, NULL) ||
            !WebPAnimEncoderAssemble(encoder, &encoded)) {
            set_error(error, error_size, WebPAnimEncoderGetError(encoder));
            WebPAnimEncoderDelete(encoder);
            return -1;
        }
        WebPAnimEncoderDelete(encoder);
    }

    WebPMux *mux = WebPMuxCreate(&encoded, 1);
    WebPData assembled = {0};
    if (!mux || apply_webp_chunks(mux, codec, width, height) < 0 ||
        WebPMuxAssemble(mux, &assembled) != WEBP_MUX_OK) {
        set_error(error, error_size, "cannot attach WebP metadata");
        if (mux)
            WebPMuxDelete(mux);
        WebPDataClear(&encoded);
        return -1;
    }
    WebPMuxDelete(mux);
    WebPDataClear(&encoded);
    FILE *stream = fopen(path, "wb");
    if (!stream || fwrite(assembled.bytes, 1, assembled.size, stream) != assembled.size) {
        if (stream)
            fclose(stream);
        set_errno_error(error, error_size, "cannot write WebP");
    } else if (fclose(stream) != 0) {
        set_errno_error(error, error_size, "cannot finish WebP");
    } else {
        result = 0;
    }
    WebPDataClear(&assembled);
    return result;
}

int image_save_crop(const struct image *image, const char *path,
                    int x, int y, int width, int height, bool require_lossless,
                    char *error, size_t error_size)
{
    if (!image || !image->codec || !path || x < 0 || y < 0 || width <= 0 ||
        height <= 0 || x > image->width - width || y > image->height - height) {
        set_error(error, error_size, "invalid crop rectangle");
        return -1;
    }
    if (image->format == IMAGE_JPEG)
        return save_jpeg(image, path, x, y, width, height, require_lossless,
                         error, error_size);
    if (image->format == IMAGE_PNG)
        return save_png(image, path, x, y, width, height, error, error_size);
    return save_webp(image, path, x, y, width, height, error, error_size);
}

void image_free(struct image *image)
{
    if (!image)
        return;
    for (size_t i = 0; i < image->frame_count; i++)
        free(image->frames[i].pixels);
    free(image->frames);
    if (image->format == IMAGE_JPEG) {
        struct jpeg_codec *codec = image->codec;
        if (codec) {
            free(codec->file.data);
            free(codec->samples);
            free_markers(codec->markers);
            free(codec);
        }
    } else if (image->format == IMAGE_PNG) {
        struct png_codec *codec = image->codec;
        if (codec) {
            free(codec->file.data);
            free(codec->rows);
            free(codec->icc_name);
            free(codec->icc_data);
            free_png_texts(codec);
            free(codec->exif_data);
            free(codec);
        }
    } else if (image->format == IMAGE_WEBP) {
        struct webp_codec *codec = image->codec;
        if (codec) {
            free(codec->file.data);
            free(codec->frames);
            for (size_t i = 0; i < 3; i++)
                free(codec->chunks[i].data);
            free(codec);
        }
    }
    memset(image, 0, sizeof(*image));
}

const uint32_t *image_pixels(const struct image *image)
{
    return image->frames[image->current_frame].pixels;
}

bool image_opaque(const struct image *image)
{
    return image->frame_count && image->frames[image->current_frame].opaque;
}

bool image_animated(const struct image *image)
{
    return image->frame_count > 1;
}

int image_advance(struct image *image)
{
    image->current_frame = (image->current_frame + 1) % image->frame_count;
    return image_current_duration(image);
}

int image_current_duration(const struct image *image)
{
    if (!image->frame_count)
        return 0;
    return image->frames[image->current_frame].duration_ms;
}

const char *image_format_name(enum image_format format)
{
    if (format == IMAGE_JPEG)
        return "JPEG";
    if (format == IMAGE_PNG)
        return "PNG";
    return "WebP";
}

bool image_extension_matches(enum image_format format, const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash + 1 : path, '.');
    if (!dot)
        return false;
    dot++;
    if (format == IMAGE_JPEG)
        return strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0 ||
               strcasecmp(dot, "jpe") == 0;
    if (format == IMAGE_PNG)
        return strcasecmp(dot, "png") == 0;
    return strcasecmp(dot, "webp") == 0;
}

void image_jpeg_mcu(const struct image *image, int *width, int *height)
{
    *width = *height = 1;
    if (image->format == IMAGE_JPEG) {
        const struct jpeg_codec *codec = image->codec;
        *width = codec->mcu_width;
        *height = codec->mcu_height;
    }
}
