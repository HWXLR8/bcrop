# bcrop

Minimal Wayland image cropper for JPEG, PNG, and WebP.

## Build

Requires Wayland, xkbcommon, libjpeg-turbo, libpng, and libwebp.

```sh
make
```

## Use

```text
bcrop [-f] [-o OUTPUT] [-l|--jpeg-lossless] IMAGE
```

`-f` allows replacing an existing output. Without `-o`, the input is replaced,
so `-f` is required. `--jpeg-lossless` snaps the crop to valid JPEG boundaries.

Press `?` in the program for controls.
