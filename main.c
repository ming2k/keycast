#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <linux/input.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <libevdev/libevdev.h>
#include "wlr-layer-shell-protocol.h"

/* Default configuration */
#define DEFAULT_MARGIN 20
#define DEFAULT_POSITION POS_BOTTOM_RIGHT
#define FONT_DESC "Sans Bold 20"
#define PADDING 10
#define BACKGROUND_ALPHA 0.3
#define CORNER_RADIUS 6.0
#define KEY_HISTORY_SIZE 100  /* Large buffer, display limited by width */
#define MAX_WIDTH 400  /* Maximum overlay width in pixels */

/* Position enum: 9-point grid positioning */
enum position {
    POS_TOP_LEFT,
    POS_TOP_CENTER,
    POS_TOP_RIGHT,
    POS_CENTER_LEFT,
    POS_CENTER,
    POS_CENTER_RIGHT,
    POS_BOTTOM_LEFT,
    POS_BOTTOM_CENTER,
    POS_BOTTOM_RIGHT
};

/* Input device structure */
struct input_device {
    int fd;
    struct libevdev *evdev;
    char name[256];
};

/* Main state structure */
struct keycast_state {
    /* Wayland core objects */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;

    /* Layer shell objects */
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    /* Input devices (evdev) */
    struct input_device *devices;
    int num_devices;
    struct pollfd *pollfds;

    /* Modifier tracking */
    bool shift_pressed;
    bool ctrl_pressed;
    bool alt_pressed;
    bool super_pressed;

    /* Last pressed key tracking (for release-based display) */
    int last_pressed_key;
    bool last_shift;
    bool last_ctrl;
    bool last_alt;
    bool last_super;

    /* Rendering */
    struct wl_buffer *buffer;
    PangoLayout *pango_layout;

    /* Configuration */
    enum position position;
    int margin;

    /* Key history */
    char key_history[KEY_HISTORY_SIZE][128];
    int history_count;

    /* Window dimensions */
    int width;
    int height;
    bool configured;
};

/* ==================== Forward Declarations ==================== */
static void handle_key_press(struct keycast_state *state, int code);
static void draw_keycast(struct keycast_state *state);

/* ==================== Helper Functions ==================== */

/* Create shared memory file (POSIX portable version) */
static int create_anonymous_file(off_t size) {
    char name[] = "/wl_shm-XXXXXX";
    int retries = 100;
    int fd;

    do {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long r = ts.tv_nsec;
        for (int i = 0; i < 6; ++i) {
            name[(sizeof(name) - 1) - 6 + i] = 'A' + (r & 15) + (r & 16) * 2;
            r >>= 5;
        }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            break;
        }
    } while (--retries > 0 && fd < 0);

    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Calculate anchor flags based on position */
static uint32_t calculate_anchors(enum position pos) {
    switch (pos) {
    case POS_TOP_LEFT:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    case POS_TOP_CENTER:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    case POS_TOP_RIGHT:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    case POS_CENTER_LEFT:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    case POS_CENTER:
        return 0;  /* No anchor, centered */
    case POS_CENTER_RIGHT:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    case POS_BOTTOM_LEFT:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    case POS_BOTTOM_CENTER:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    case POS_BOTTOM_RIGHT:
    default:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    }
}

/* Find and open all keyboard input devices */
static int find_keyboard_devices(struct keycast_state *state) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        fprintf(stderr, "Failed to open /dev/input: %s\n", strerror(errno));
        return -1;
    }

    /* Count devices first */
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) == 0) {
            count++;
        }
    }

    if (count == 0) {
        fprintf(stderr, "No input devices found\n");
        closedir(dir);
        return -1;
    }

    /* Allocate arrays */
    state->devices = calloc(count, sizeof(struct input_device));
    state->pollfds = calloc(count + 1, sizeof(struct pollfd));  /* +1 for Wayland fd */
    state->num_devices = 0;

    /* Open devices */
    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;  /* No permission, skip */
        }

        struct libevdev *dev;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            close(fd);
            continue;
        }

        /* Check if this device has keyboard keys */
        if (!libevdev_has_event_type(dev, EV_KEY) ||
            (!libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
             !libevdev_has_event_code(dev, EV_KEY, KEY_ENTER))) {
            libevdev_free(dev);
            close(fd);
            continue;
        }

        /* Add to our device list */
        state->devices[state->num_devices].fd = fd;
        state->devices[state->num_devices].evdev = dev;
        strncpy(state->devices[state->num_devices].name,
                libevdev_get_name(dev), 255);

        state->pollfds[state->num_devices].fd = fd;
        state->pollfds[state->num_devices].events = POLLIN;

        printf("Found keyboard: %s (%s)\n",
               state->devices[state->num_devices].name, path);

        state->num_devices++;
    }

    closedir(dir);

    if (state->num_devices == 0) {
        fprintf(stderr, "No keyboard devices found. "
                       "Make sure you have permission to read /dev/input/event*\n");
        fprintf(stderr, "Try: sudo usermod -a -G input $USER (then log out and back in)\n");
        free(state->devices);
        free(state->pollfds);
        return -1;
    }

    return 0;
}

/* Process input event from evdev */
static void process_input_event(struct keycast_state *state, struct input_event *ev) {
    if (ev->type != EV_KEY) {
        return;
    }

    /* Update modifier state first */
    if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
        state->shift_pressed = (ev->value == 1 || ev->value == 2);
    } else if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) {
        state->ctrl_pressed = (ev->value == 1 || ev->value == 2);
    } else if (ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT) {
        state->alt_pressed = (ev->value == 1 || ev->value == 2);
    } else if (ev->code == KEY_LEFTMETA || ev->code == KEY_RIGHTMETA) {
        state->super_pressed = (ev->value == 1 || ev->value == 2);
    }

    /* Handle key press (value == 1) and key repeat (value == 2) */
    if (ev->value == 1 || ev->value == 2) {
        handle_key_press(state, ev->code);

        /* Re-render overlay */
        if (state->configured) {
            draw_keycast(state);
        }
    }
}

/* Convert evdev key code to readable name */
static const char *get_key_name(int code) {
    switch (code) {
        /* Modifier keys */
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return "Shift";
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL: return "Ctrl";
        case KEY_LEFTALT:
        case KEY_RIGHTALT: return "Alt";
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA: return "Super";

        /* Special keys */
        case KEY_SPACE: return "Space";
        case KEY_ENTER: return "Enter";
        case KEY_BACKSPACE: return "Bksp";
        case KEY_TAB: return "Tab";
        case KEY_ESC: return "Esc";
        case KEY_DELETE: return "Del";
        case KEY_UP: return "↑";
        case KEY_DOWN: return "↓";
        case KEY_LEFT: return "←";
        case KEY_RIGHT: return "→";
        case KEY_PAGEUP: return "PgUp";
        case KEY_PAGEDOWN: return "PgDn";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_INSERT: return "Ins";
        case KEY_CAPSLOCK: return "Caps";

        /* Function keys */
        case KEY_F1: return "F1";
        case KEY_F2: return "F2";
        case KEY_F3: return "F3";
        case KEY_F4: return "F4";
        case KEY_F5: return "F5";
        case KEY_F6: return "F6";
        case KEY_F7: return "F7";
        case KEY_F8: return "F8";
        case KEY_F9: return "F9";
        case KEY_F10: return "F10";
        case KEY_F11: return "F11";
        case KEY_F12: return "F12";

        /* Single character keys */
        case KEY_A: return "a";
        case KEY_B: return "b";
        case KEY_C: return "c";
        case KEY_D: return "d";
        case KEY_E: return "e";
        case KEY_F: return "f";
        case KEY_G: return "g";
        case KEY_H: return "h";
        case KEY_I: return "i";
        case KEY_J: return "j";
        case KEY_K: return "k";
        case KEY_L: return "l";
        case KEY_M: return "m";
        case KEY_N: return "n";
        case KEY_O: return "o";
        case KEY_P: return "p";
        case KEY_Q: return "q";
        case KEY_R: return "r";
        case KEY_S: return "s";
        case KEY_T: return "t";
        case KEY_U: return "u";
        case KEY_V: return "v";
        case KEY_W: return "w";
        case KEY_X: return "x";
        case KEY_Y: return "y";
        case KEY_Z: return "z";

        case KEY_0: return "0";
        case KEY_1: return "1";
        case KEY_2: return "2";
        case KEY_3: return "3";
        case KEY_4: return "4";
        case KEY_5: return "5";
        case KEY_6: return "6";
        case KEY_7: return "7";
        case KEY_8: return "8";
        case KEY_9: return "9";

        /* Punctuation and symbols */
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_LEFTBRACE: return "[";
        case KEY_RIGHTBRACE: return "]";
        case KEY_SEMICOLON: return ";";
        case KEY_APOSTROPHE: return "'";
        case KEY_GRAVE: return "`";
        case KEY_BACKSLASH: return "\\";
        case KEY_COMMA: return ",";
        case KEY_DOT: return ".";
        case KEY_SLASH: return "/";

        default: return NULL;
    }
}

/* Add key to history */
static void add_key_to_history(struct keycast_state *state, const char *key) {
    /* Shift history down */
    for (int i = KEY_HISTORY_SIZE - 1; i > 0; i--) {
        strncpy(state->key_history[i], state->key_history[i-1], 127);
        state->key_history[i][127] = '\0';
    }

    /* Add new key at the front */
    strncpy(state->key_history[0], key, 127);
    state->key_history[0][127] = '\0';

    /* Update count */
    if (state->history_count < KEY_HISTORY_SIZE) {
        state->history_count++;
    }

    printf("Key added to history: %s (total: %d)\n", key, state->history_count);
}

/* Format key with modifiers and add to history */
static void handle_key_press(struct keycast_state *state, int code) {
    char buf[128] = {0};
    int offset = 0;

    /* Get key name */
    const char *key_name = get_key_name(code);
    if (!key_name) {
        return;  /* Ignore unknown keys */
    }

    /* Check if this is a modifier key itself */
    bool is_modifier = (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
                        code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
                        code == KEY_LEFTALT || code == KEY_RIGHTALT ||
                        code == KEY_LEFTMETA || code == KEY_RIGHTMETA);

    /* Don't show modifier keys when pressed alone - only when combined with other keys */
    if (is_modifier) {
        return;
    }

    /* Build key string with modifiers (use text names, not symbols) */
    if (state->super_pressed && !is_modifier) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "Super+");
    }
    if (state->ctrl_pressed && !is_modifier) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "Ctrl+");
    }
    if (state->alt_pressed && !is_modifier) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "Alt+");
    }
    if (state->shift_pressed && !is_modifier) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "Shift+");
    }

    snprintf(buf + offset, sizeof(buf) - offset, "%s", key_name);

    /* Add to history */
    add_key_to_history(state, buf);
}

/* ==================== Rendering Functions ==================== */

/* Calculate required buffer size for rendering key history */
static void calculate_text_size(struct keycast_state *state, int *width, int *height) {
    if (!state->pango_layout) {
        /* Create temporary Cairo surface for measurement */
        cairo_surface_t *temp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr = cairo_create(temp);
        state->pango_layout = pango_cairo_create_layout(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(temp);
    }

    /* Set font */
    PangoFontDescription *desc = pango_font_description_from_string(FONT_DESC);
    pango_layout_set_font_description(state->pango_layout, desc);
    pango_font_description_free(desc);

    /* Calculate width for all keys to determine content size */
    char combined[1024] = {0};
    int offset = 0;

    /* Concatenate all keys to measure total width */
    for (int i = state->history_count - 1; i >= 0; i--) {
        if (i < state->history_count - 1) {
            offset += snprintf(combined + offset, sizeof(combined) - offset, "  ");
        }
        offset += snprintf(combined + offset, sizeof(combined) - offset, "%s", state->key_history[i]);
    }

    pango_layout_set_text(state->pango_layout, combined, -1);
    int text_width, text_height;
    pango_layout_get_pixel_size(state->pango_layout, &text_width, &text_height);

    /* Dynamic width that fits content, but capped at MAX_WIDTH */
    int content_width = text_width + PADDING * 2;
    *width = (content_width < MAX_WIDTH) ? content_width : MAX_WIDTH;
    *height = text_height + PADDING * 2;
}

/* Draw keycast overlay */
static void draw_keycast(struct keycast_state *state) {
    printf("draw_keycast called: history_count=%d, configured=%d\n", state->history_count, state->configured);

    if (state->history_count == 0 || !state->configured) {
        printf("  Skipping: not ready to draw\n");
        return;
    }

    /* Calculate required size */
    int width, height;
    calculate_text_size(state, &width, &height);
    printf("  Drawing overlay: %dx%d\n", width, height);

    /* Create shared memory buffer */
    int stride = width * 4;
    int size = stride * height;

    int fd = create_anonymous_file(size);
    if (fd < 0) {
        fprintf(stderr, "Failed to create shared memory\n");
        return;
    }

    uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        fprintf(stderr, "mmap failed\n");
        return;
    }

    /* Create wl_buffer */
    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Create Cairo surface */
    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        (unsigned char *)data, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_t *cr = cairo_create(cairo_surface);

    /* Set font options for crisp rendering */
    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_MEDIUM);
    cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(cr, font_options);
    cairo_font_options_destroy(font_options);

    /* Create Pango context with better rendering */
    PangoContext *pango_context = pango_layout_get_context(state->pango_layout);
    pango_cairo_context_set_font_options(pango_context, font_options);

    /* Clear to fully transparent first */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Draw rounded rectangle background with semi-transparent fill */
    double radius = CORNER_RADIUS;
    cairo_new_sub_path(cr);
    cairo_arc(cr, width - radius, radius, radius, -M_PI/2, 0);
    cairo_arc(cr, width - radius, height - radius, radius, 0, M_PI/2);
    cairo_arc(cr, radius, height - radius, radius, M_PI/2, M_PI);
    cairo_arc(cr, radius, radius, radius, M_PI, 3*M_PI/2);
    cairo_close_path(cr);

    /* Fill the rounded rectangle with semi-transparent background */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, BACKGROUND_ALPHA);
    cairo_fill_preserve(cr);

    /* Optionally draw a subtle border */
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* Set clipping region to the rounded rectangle to hide overflow */
    cairo_new_sub_path(cr);
    cairo_arc(cr, width - radius, radius, radius, -M_PI/2, 0);
    cairo_arc(cr, width - radius, height - radius, radius, 0, M_PI/2);
    cairo_arc(cr, radius, height - radius, radius, M_PI/2, M_PI);
    cairo_arc(cr, radius, radius, radius, M_PI, 3*M_PI/2);
    cairo_close_path(cr);
    cairo_clip(cr);

    /* Render text with Pango */
    PangoFontDescription *desc = pango_font_description_from_string(FONT_DESC);
    pango_layout_set_font_description(state->pango_layout, desc);
    pango_font_description_free(desc);

    /* First pass: calculate total content width */
    int total_content_width = 0;
    for (int i = state->history_count - 1; i >= 0; i--) {
        pango_layout_set_text(state->pango_layout, state->key_history[i], -1);
        int text_width, text_height;
        pango_layout_get_pixel_size(state->pango_layout, &text_width, &text_height);
        total_content_width += text_width;
        if (i > 0) {
            total_content_width += 15;  /* spacing */
        }
    }

    /* Calculate starting x position to align rightmost content to the right edge */
    double x_start = PADDING;
    if (total_content_width > (width - PADDING * 2)) {
        /* Content overflows: shift left so rightmost key is visible */
        x_start = width - PADDING - total_content_width;
    }

    /* Draw keys horizontally with fade effect (oldest to newest, left to right) */
    double x_offset = x_start;
    double y_pos = PADDING;

    for (int i = state->history_count - 1; i >= 0; i--) {
        pango_layout_set_text(state->pango_layout, state->key_history[i], -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(state->pango_layout, &text_width, &text_height);

        /* Calculate opacity: newest key (index 0) is most opaque, oldest fades */
        double opacity = 1.0 - (i * 0.15);
        if (opacity < 0.3) opacity = 0.3;

        /* Use integer pixel positions for sharp rendering */
        cairo_move_to(cr, round(x_offset), round(y_pos));
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, opacity);
        pango_cairo_show_layout(cr, state->pango_layout);

        /* Move to next position horizontally */
        x_offset += text_width + 15;  /* 15px spacing between keys */
    }

    /* Cleanup Cairo */
    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);
    munmap(data, size);

    /* Destroy old buffer */
    if (state->buffer) {
        wl_buffer_destroy(state->buffer);
    }
    state->buffer = buffer;

    /* Update layer surface size */
    zwlr_layer_surface_v1_set_size(state->layer_surface, width, height);

    /* Commit to surface */
    wl_surface_attach(state->surface, buffer, 0, 0);
    wl_surface_damage(state->surface, 0, 0, width, height);
    wl_surface_commit(state->surface);

    printf("  Overlay drawn and committed\n");
}

/* ==================== Layer Shell Configuration ==================== */

/* Configure layer surface: set anchors, margins, layer and keyboard interactivity */
static void configure_layer_surface(struct keycast_state *state) {
    uint32_t anchors = calculate_anchors(state->position);

    /* Set anchors */
    zwlr_layer_surface_v1_set_anchor(state->layer_surface, anchors);

    /* Set margins */
    zwlr_layer_surface_v1_set_margin(state->layer_surface,
                                      state->margin,  /* top */
                                      state->margin,  /* right */
                                      state->margin,  /* bottom */
                                      state->margin); /* left */

    /* No keyboard interactivity needed - we use evdev */
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    /* Initial size (will be adjusted dynamically) */
    zwlr_layer_surface_v1_set_size(state->layer_surface, 300, 100);

    /* Commit configuration */
    wl_surface_commit(state->surface);

    printf("Layer surface configured\n");
}

/* Layer surface configure event handler */
static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct keycast_state *state = data;

    /* Acknowledge configure */
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    /* Record dimensions */
    state->width = width;
    state->height = height;

    printf("Layer surface configured: %ux%u (serial=%u)\n", width, height, serial);

    /* Mark as configured */
    if (!state->configured) {
        state->configured = true;
    }

    /* Draw keycast if we have key history */
    if (state->history_count > 0) {
        draw_keycast(state);
    }
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
    (void)data; (void)surface;
    printf("Layer surface closed\n");
    exit(0);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ==================== Registry and Seat Handlers ==================== */

/* Registry handler: bind global interfaces */
static void registry_handler(void *data, struct wl_registry *registry,
                             uint32_t id, const char *interface, uint32_t version) {
    struct keycast_state *state = data;
    (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
        printf("Bound to compositor\n");
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
        printf("Bound to shm\n");
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, id,
                                               &zwlr_layer_shell_v1_interface, 1);
        printf("Bound to layer_shell\n");
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t id) {
    (void)data; (void)registry; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler,
    .global_remove = registry_remove,
};

/* Seat capabilities handler: get keyboard */
/* ==================== Command-Line Argument Parsing ==================== */

/* Parse position argument */
static enum position parse_position(const char *str) {
    if (strcmp(str, "top-left") == 0) return POS_TOP_LEFT;
    if (strcmp(str, "top-center") == 0) return POS_TOP_CENTER;
    if (strcmp(str, "top-right") == 0) return POS_TOP_RIGHT;
    if (strcmp(str, "center-left") == 0) return POS_CENTER_LEFT;
    if (strcmp(str, "center") == 0) return POS_CENTER;
    if (strcmp(str, "center-right") == 0) return POS_CENTER_RIGHT;
    if (strcmp(str, "bottom-left") == 0) return POS_BOTTOM_LEFT;
    if (strcmp(str, "bottom-center") == 0) return POS_BOTTOM_CENTER;
    if (strcmp(str, "bottom-right") == 0) return POS_BOTTOM_RIGHT;

    fprintf(stderr, "Unknown position: %s\n", str);
    fprintf(stderr, "Valid options: top-left, top-center, top-right, "
                    "center-left, center, center-right, "
                    "bottom-left, bottom-center, bottom-right\n");
    exit(1);
}

/* Print usage information */
static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n\n", prog_name);
    printf("Options:\n");
    printf("  -p, --position POSITION  Set display position (default: bottom-right)\n");
    printf("                           Valid values:\n");
    printf("                           top-left, top-center, top-right,\n");
    printf("                           center-left, center, center-right,\n");
    printf("                           bottom-left, bottom-center, bottom-right\n");
    printf("  -m, --margin PIXELS      Set margin in pixels (default: 20)\n");
    printf("  -h, --help               Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --position top-right --margin 50\n", prog_name);
    printf("  %s -p bottom-left -m 30\n", prog_name);
}

/* Parse command-line arguments */
static void parse_arguments(int argc, char *argv[], enum position *pos, int *margin) {
    /* Default values */
    *pos = DEFAULT_POSITION;
    *margin = DEFAULT_MARGIN;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--position") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
                exit(1);
            }
            *pos = parse_position(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--margin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
                exit(1);
            }
            *margin = atoi(argv[++i]);
            if (*margin < 0) {
                fprintf(stderr, "Error: margin must be non-negative\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }
}

/* ==================== Main Function ==================== */

int main(int argc, char *argv[]) {
    /* Parse command-line arguments */
    enum position position;
    int margin;
    parse_arguments(argc, argv, &position, &margin);

    /* Initialize state structure */
    struct keycast_state state = {0};
    state.position = position;
    state.margin = margin;

    /* Connect to Wayland display */
    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "Cannot connect to Wayland display\n");
        return 1;
    }
    printf("Connected to Wayland display\n");

    /* Find and open keyboard input devices */
    if (find_keyboard_devices(&state) < 0) {
        fprintf(stderr, "Failed to find keyboard devices\n");
        fprintf(stderr, "Make sure you have permission to read /dev/input/event*\n");
        fprintf(stderr, "Try: sudo usermod -a -G input $USER (then log out and in)\n");
        return 1;
    }
    printf("Found %d keyboard device(s)\n\n", state.num_devices);

    /* Get registry and bind global objects */
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);

    /* Check required interfaces */
    if (!state.compositor || !state.shm || !state.layer_shell) {
        fprintf(stderr, "Missing required Wayland interfaces\n");
        fprintf(stderr, "compositor: %s\n", state.compositor ? "OK" : "MISSING");
        fprintf(stderr, "shm: %s\n", state.shm ? "OK" : "MISSING");
        fprintf(stderr, "layer_shell: %s\n", state.layer_shell ? "OK" : "MISSING");
        return 1;
    }

    /* Create surface */
    state.surface = wl_compositor_create_surface(state.compositor);
    if (!state.surface) {
        fprintf(stderr, "Cannot create surface\n");
        return 1;
    }
    printf("Surface created\n");

    /* Create layer surface */
    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "keycast");

    if (!state.layer_surface) {
        fprintf(stderr, "Cannot create layer surface\n");
        return 1;
    }
    printf("Layer surface created\n");

    /* Configure layer surface */
    zwlr_layer_surface_v1_add_listener(state.layer_surface,
                                        &layer_surface_listener, &state);
    configure_layer_surface(&state);

    /* Ensure configure event is received */
    wl_display_roundtrip(state.display);

    printf("\nKeycast started successfully\n");
    printf("Position: %d, Margin: %d\n", position, margin);
    printf("Monitoring keyboard input from evdev devices\n");
    printf("Press Ctrl+C in this terminal to exit\n\n");

    /* Set up polling for Wayland and input devices */
    state.pollfds[state.num_devices].fd = wl_display_get_fd(state.display);
    state.pollfds[state.num_devices].events = POLLIN;

    /* Event loop: poll both Wayland and input devices */
    while (1) {
        /* Flush pending Wayland requests */
        while (wl_display_prepare_read(state.display) != 0) {
            wl_display_dispatch_pending(state.display);
        }
        wl_display_flush(state.display);

        /* Wait for events */
        int ret = poll(state.pollfds, state.num_devices + 1, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(state.display);
                continue;
            }
            perror("poll");
            break;
        }

        /* Check Wayland events */
        if (state.pollfds[state.num_devices].revents & POLLIN) {
            wl_display_read_events(state.display);
            wl_display_dispatch_pending(state.display);
        } else {
            wl_display_cancel_read(state.display);
        }

        /* Check input device events */
        for (int i = 0; i < state.num_devices; i++) {
            if (state.pollfds[i].revents & POLLIN) {
                struct input_event ev;
                int rc;

                while ((rc = libevdev_next_event(state.devices[i].evdev,
                                                  LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                    process_input_event(&state, &ev);
                }

                if (rc != 0 && rc != -EAGAIN) {
                    fprintf(stderr, "Error reading from device %d: %s\n",
                            i, strerror(-rc));
                }
            }
        }
    }

    /* Cleanup resources */
    for (int i = 0; i < state.num_devices; i++) {
        if (state.devices[i].evdev) {
            libevdev_free(state.devices[i].evdev);
        }
        if (state.devices[i].fd >= 0) {
            close(state.devices[i].fd);
        }
    }
    free(state.devices);
    free(state.pollfds);

    if (state.pango_layout) {
        g_object_unref(state.pango_layout);
    }
    if (state.buffer) {
        wl_buffer_destroy(state.buffer);
    }
    if (state.layer_surface) {
        zwlr_layer_surface_v1_destroy(state.layer_surface);
    }
    if (state.surface) {
        wl_surface_destroy(state.surface);
    }
    if (state.layer_shell) {
        zwlr_layer_shell_v1_destroy(state.layer_shell);
    }
    if (state.shm) {
        wl_shm_destroy(state.shm);
    }
    if (state.compositor) {
        wl_compositor_destroy(state.compositor);
    }
    if (state.registry) {
        wl_registry_destroy(state.registry);
    }
    wl_display_disconnect(state.display);

    return 0;
}
