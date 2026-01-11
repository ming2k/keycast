CC = gcc
TARGET = keycast

# Dependencies
PKGS = wayland-client cairo pangocairo libevdev
CFLAGS = $(shell pkg-config --cflags $(PKGS)) -g -Wall -Wextra
LIBS = $(shell pkg-config --libs $(PKGS)) -lrt -lm

# Protocol files
PROTO_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WLR_LAYER_XML = wlr-layer-shell-unstable-v1.xml
XDG_XML = $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml

# Generated protocol files
WLR_LAYER_PROTO = wlr-layer-shell-protocol
XDG_PROTO = xdg-shell-protocol

OBJS = main.o $(WLR_LAYER_PROTO).o $(XDG_PROTO).o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LIBS)
	@echo "Build successful! Run with ./$(TARGET)"

main.o: main.c $(WLR_LAYER_PROTO).h $(XDG_PROTO).h
	$(CC) -c $(CFLAGS) -o $@ $<

$(WLR_LAYER_PROTO).o: $(WLR_LAYER_PROTO).c $(WLR_LAYER_PROTO).h $(XDG_PROTO).h
	$(CC) -c $(CFLAGS) -o $@ $<

$(XDG_PROTO).o: $(XDG_PROTO).c $(XDG_PROTO).h
	$(CC) -c $(CFLAGS) -o $@ $<

# Generate protocol headers and source
$(WLR_LAYER_PROTO).h: $(WLR_LAYER_XML)
	wayland-scanner client-header $< $@

$(WLR_LAYER_PROTO).c: $(WLR_LAYER_XML)
	wayland-scanner private-code $< $@

$(XDG_PROTO).h: $(XDG_XML)
	wayland-scanner client-header $< $@

$(XDG_PROTO).c: $(XDG_XML)
	wayland-scanner private-code $< $@

clean:
	rm -f $(TARGET) $(OBJS) $(WLR_LAYER_PROTO).{c,h} $(XDG_PROTO).{c,h}

.PHONY: all clean
