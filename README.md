# bcrop

`bcrop` is a small, Wayland-only interactive image cropper. It supports JPEG,
PNG, and WebP and always writes the same format it opened.

## Build

The build requires a C11 compiler plus the development files for Wayland,
xkbcommon, libjpeg-turbo (including TurboJPEG), libpng, and libwebp.

```sh
make
```

## Use

```text
bcrop [-f] [-o OUTPUT] [-l|--jpeg-lossless] IMAGE
```

Without `-o`, the input path is the output path, so `-f` is required. Output
extensions must match the detected input format. Press `?` in the program for
the controls; Enter saves and exits.

JPEG crops whose top-left corner is naturally MCU-aligned are lossless. Other
JPEG crops are re-encoded with the source quantization tables, chroma sampling,
colorspace, and baseline/progressive mode. `--jpeg-lossless` snaps the crop
origin to the nearest valid MCU boundary and forbids a fallback re-encode.

PNG crops retain their original bit depth, color type, palette, transparency,
interlace mode, color information, density, text, and Exif metadata. APNG is
rejected rather than silently flattened.

Static and animated WebP are supported. Animations play automatically while
cropping, and all frames are cropped while retaining their timing, loop count,
background, lossless/lossy mode, and ICC/Exif/XMP metadata. Lossy WebP quality
is estimated from the VP8 segment quantizers because WebP files do not store an
encoder quality value.
