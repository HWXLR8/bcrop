CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS += $(shell pkg-config --cflags wayland-client xkbcommon libjpeg libturbojpeg libpng libwebp libwebpdemux libwebpmux)
LDLIBS += $(shell pkg-config --libs wayland-client xkbcommon libjpeg libturbojpeg libpng libwebp libwebpdemux libwebpmux) -lm

WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
GENERATED := xdg-shell-client-protocol.h xdg-shell-protocol.c

.PHONY: all clean

all: bcrop

bcrop: bcrop.c image.c image.h $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ bcrop.c image.c xdg-shell-protocol.c $(LDLIBS)

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

clean:
	rm -f bcrop $(GENERATED)
