# Development Guide

This document provides technical details for developers who want to understand, modify, or contribute to keycast.

## Tech Stack

### Core Technologies

- **Language**: C (C99)
- **Build System**: GNU Make
- **Compiler**: GCC (also compatible with Clang)

### Libraries

| Library | Purpose | Version Used |
|---------|---------|--------------|
| wayland-client | Wayland client protocol implementation | 1.24.0+ |
| cairo | 2D graphics rendering | 1.18.0+ |
| pango/pangocairo | Text layout and rendering | 1.50.0+ |
| libevdev | Linux input device event handling | 1.13.0+ |

### Wayland Protocols

- **wlr-layer-shell-unstable-v1**: Layer shell protocol for overlay surfaces
- **xdg-shell**: Standard Wayland shell protocol (dependency of wlr-layer-shell)

## Project Structure

```
keycast/
├── main.c                              # Main source code (~1000 lines)
├── Makefile                            # Build configuration
├── wlr-layer-shell-unstable-v1.xml    # Protocol definition (upstream)
├── README.md                           # User documentation
├── DEVELOPMENT.md                      # This file
└── .gitignore                          # Git ignore rules

Generated files (not in git):
├── wlr-layer-shell-protocol.c         # Generated from .xml
├── wlr-layer-shell-protocol.h         # Generated from .xml
├── xdg-shell-protocol.c               # Generated from system protocols
├── xdg-shell-protocol.h               # Generated from system protocols
├── *.o                                 # Object files
└── keycast                             # Final binary
```

## Protocol Files

### wlr-layer-shell-unstable-v1.xml

**Source**: https://gitlab.freedesktop.org/wlroots/wlr-protocols

This is a local copy of the layer shell protocol definition. It defines the interface for creating overlay surfaces that stay on top of regular windows.

**Where it comes from**:
```bash
# Original source
curl -O https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-layer-shell-unstable-v1.xml
```

**When to update**:
- When the protocol is updated upstream
- When you need new features from a protocol version bump
- Rarely needed - the protocol is relatively stable

**How to update**:
1. Download the latest version from the URL above
2. Replace `wlr-layer-shell-unstable-v1.xml`
3. Run `make clean && make` to regenerate bindings
4. Test thoroughly - protocol changes can break compatibility

### xdg-shell Protocol

**Source**: System-installed wayland-protocols package

Located at: `/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml`

**Why it's needed**: The wlr-layer-shell protocol references `xdg_popup` interface from xdg-shell, so we need to generate both protocol bindings.

**When to update**: Automatically updated when you upgrade the `wayland-protocols` package on your system.

## Code Generation

### Protocol Bindings

Wayland protocols are defined in XML files. The `wayland-scanner` tool generates C bindings from these definitions.

**Process**:

1. **wayland-scanner** reads the `.xml` file
2. Generates two files:
   - **Header (`.h`)**: Interface definitions and function declarations
   - **Source (`.c`)**: Protocol implementation code

**Commands used in Makefile**:

```makefile
# Generate header
wayland-scanner client-header protocol.xml protocol.h

# Generate source
wayland-scanner private-code protocol.xml protocol.c
```

**Generated files**:
- `wlr-layer-shell-protocol.{c,h}` - Layer shell bindings
- `xdg-shell-protocol.{c,h}` - XDG shell bindings

These are generated automatically when you run `make`.

## Architecture Overview

### Component Diagram

```
┌─────────────────────────────────────────────────────┐
│                    keycast                          │
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌──────────────┐          ┌──────────────┐       │
│  │ Input Layer  │          │ Display Layer│       │
│  │  (libevdev)  │          │  (Wayland)   │       │
│  └──────┬───────┘          └──────┬───────┘       │
│         │                          │               │
│         │  ┌────────────────────┐ │               │
│         └─▶│  Event Processing  │◀┘               │
│            │   & Key Tracking   │                 │
│            └─────────┬──────────┘                 │
│                      │                             │
│            ┌─────────▼──────────┐                 │
│            │  Text Rendering    │                 │
│            │  (Cairo + Pango)   │                 │
│            └────────────────────┘                 │
│                                                     │
└─────────────────────────────────────────────────────┘
         │                                │
         ▼                                ▼
  /dev/input/event*              Wayland Compositor
   (evdev devices)                  (wlr-layer-shell)
```

### Data Flow

1. **Input Capture**:
   - `libevdev` reads from `/dev/input/event*` devices
   - `poll()` monitors multiple input devices simultaneously
   - Key events are processed in `process_input_event()`

2. **Event Processing**:
   - Modifier state tracked (Shift, Ctrl, Alt, Super)
   - Key codes mapped to human-readable names
   - Key combinations formatted (e.g., "Ctrl+c")
   - Added to circular history buffer (100 keys max)

3. **Rendering**:
   - Pango calculates text dimensions
   - Cairo creates ARGB32 image surface
   - Rounded rectangle background drawn with transparency
   - Text rendered with fade effect (newest brightest)
   - Surface committed to Wayland compositor

4. **Display**:
   - wlr-layer-shell creates overlay surface
   - Surface positioned based on user configuration
   - Overlay stays on top, doesn't steal focus

## Key Functions

### Input Handling

```c
find_keyboard_devices()     // Scan /dev/input for keyboard devices
process_input_event()       // Handle evdev key events
get_key_name()              // Map evdev key codes to names
handle_key_press()          // Format key with modifiers
add_key_to_history()        // Manage circular history buffer
```

### Wayland/Display

```c
registry_handler()          // Bind to Wayland globals
configure_layer_surface()   // Set up layer shell properties
layer_surface_configure()   // Handle compositor configuration
calculate_anchors()         // Compute positioning anchors
```

### Rendering

```c
calculate_text_size()       // Measure text dimensions
draw_keycast()              // Render overlay with Cairo/Pango
create_anonymous_file()     // Create shared memory for buffers
```

## Configuration Constants

All user-facing configuration is defined as constants at the top of `main.c`:

```c
#define DEFAULT_MARGIN 20          // Default margin in pixels
#define DEFAULT_POSITION POS_BOTTOM_RIGHT
#define FONT_DESC "Sans Bold 20"   // Font family and size
#define PADDING 10                 // Internal padding
#define BACKGROUND_ALPHA 0.3       // Background transparency
#define CORNER_RADIUS 6.0          // Rounded corners
#define KEY_HISTORY_SIZE 100       // Max keys in buffer
#define MAX_WIDTH 400              // Maximum overlay width
```

To modify behavior, edit these constants and recompile.

## Build Process

### Dependencies Resolution

```makefile
PKGS = wayland-client cairo pangocairo libevdev
CFLAGS = $(shell pkg-config --cflags $(PKGS)) -g -Wall -Wextra
LIBS = $(shell pkg-config --libs $(PKGS)) -lrt -lm
```

`pkg-config` automatically finds the correct include paths and library flags.

### Compilation Steps

1. **Generate protocol bindings**:
   ```bash
   wayland-scanner client-header wlr-layer-shell-unstable-v1.xml wlr-layer-shell-protocol.h
   wayland-scanner private-code wlr-layer-shell-unstable-v1.xml wlr-layer-shell-protocol.c
   # Same for xdg-shell
   ```

2. **Compile object files**:
   ```bash
   gcc -c main.c -o main.o
   gcc -c wlr-layer-shell-protocol.c -o wlr-layer-shell-protocol.o
   gcc -c xdg-shell-protocol.c -o xdg-shell-protocol.o
   ```

3. **Link binary**:
   ```bash
   gcc -o keycast main.o wlr-layer-shell-protocol.o xdg-shell-protocol.o -lwayland-client -lcairo -lpangocairo -levdev -lrt -lm
   ```

## Debugging

### Enable Debug Output

The code already has debug `printf()` statements. Run in terminal to see logs:

```bash
./keycast -p top-right -m 20
```

Output shows:
- Connected to Wayland display
- Found keyboard devices
- Layer surface configuration
- Key press events
- Overlay drawing events

### Debug with GDB

```bash
# Compile with debug symbols (already enabled with -g)
make clean && make

# Run with GDB
gdb ./keycast
(gdb) run -p center
(gdb) break handle_key_press
(gdb) continue
```

### Memory Leak Detection

```bash
# Check for memory leaks
valgrind --leak-check=full --track-origins=yes ./keycast

# Press a few keys, then Ctrl+C to exit
# Check valgrind report
```

### Input Device Debugging

```bash
# List input devices
ls -la /dev/input/event*

# Check device capabilities
sudo evtest /dev/input/event0

# Monitor evdev events
sudo libinput debug-events
```

## Adding Features

### Add a New Key Mapping

Edit `get_key_name()` function:

```c
case KEY_NEWKEY: return "NewKey";
```

Find key codes in `/usr/include/linux/input-event-codes.h`.

### Modify Rendering

Edit `draw_keycast()` function. Key sections:

- **Background**: Lines ~520-535 (rounded rectangle)
- **Text rendering**: Lines ~545-582 (Pango layout)
- **Fade effect**: Line ~552 (opacity calculation)

### Change Positioning

Edit `calculate_anchors()` function to modify anchor points for each position.

### Adjust History Size

Change `KEY_HISTORY_SIZE` constant and recompile.

## Testing

### Manual Testing Checklist

- [ ] All 9 positions work correctly
- [ ] Margin adjustment works
- [ ] Single keys display (a, b, 1, 2, Space, Enter)
- [ ] Modifier combinations work (Ctrl+c, Alt+Tab, Super+d)
- [ ] Multi-modifier combos work (Ctrl+Alt+Delete)
- [ ] Key repeat works (hold a key)
- [ ] Punctuation keys display (/, ,, ., ;, etc.)
- [ ] Arrow keys show correct symbols (↑ ↓ ← →)
- [ ] Overlay stays on top
- [ ] Transparency is correct
- [ ] Rounded corners have no artifacts
- [ ] Text is sharp and readable
- [ ] Dynamic width works (grows and caps at MAX_WIDTH)
- [ ] Overflow is clipped correctly

### Test on Different Compositors

- [ ] Sway
- [ ] Hyprland
- [ ] niri
- [ ] river

## Performance Considerations

### Current Design

- **Event-driven**: Only redraws when keys are pressed
- **Efficient text measurement**: Pango caches font metrics
- **Minimal allocations**: Fixed-size history buffer
- **Shared memory**: Uses wl_shm for zero-copy buffer sharing

### Potential Optimizations

1. **Buffer caching**: Reuse buffers instead of creating new ones
2. **Partial redraws**: Only redraw changed regions
3. **Texture caching**: Cache rendered glyphs
4. **Throttling**: Limit redraw rate for rapid key repeats

Currently not needed - performance is excellent even on low-end hardware.

## Contributing

### Code Style

- **Indentation**: 4 spaces (no tabs)
- **Line length**: Try to keep under 100 characters
- **Naming**: snake_case for functions and variables
- **Comments**: English only, explain "why" not "what"
- **Error handling**: Check return values, print meaningful errors

### Submitting Changes

1. Test your changes thoroughly
2. Update documentation if needed
3. Keep commits focused and atomic
4. Write clear commit messages
5. Ensure `make clean && make` works

## Maintenance

### Regular Tasks

**Update dependencies**:
```bash
# Void Linux
sudo xbps-install -Su

# Arch Linux
sudo pacman -Syu

# Ubuntu/Debian
sudo apt update && sudo apt upgrade
```

**Update protocol files**:
Only needed if upstream changes. Check:
- https://gitlab.freedesktop.org/wlroots/wlr-protocols

### Version Compatibility

**Wayland protocols**: Stable, rarely changes
**libevdev**: Very stable API
**Cairo/Pango**: Stable, backwards compatible

Breaking changes are unlikely, but always test after dependency updates.

## Troubleshooting Development Issues

### "Protocol file not found"

```bash
# Check wayland-protocols location
pkg-config --variable=pkgdatadir wayland-protocols

# Should output: /usr/share/wayland-protocols
```

### "wayland-scanner not found"

```bash
# Install wayland development tools
# Void: sudo xbps-install -S wayland-devel
# Arch: sudo pacman -S wayland
# Ubuntu: sudo apt install wayland-protocols
```

### Linking errors

```bash
# Check if libraries are installed
pkg-config --libs wayland-client cairo pangocairo libevdev

# Should output library flags
```

## Resources

### Documentation

- [Wayland Book](https://wayland-book.com/) - Wayland protocol guide
- [Cairo Manual](https://www.cairographics.org/manual/) - Cairo graphics API
- [Pango Reference](https://docs.gtk.org/Pango/) - Pango text layout
- [libevdev Documentation](https://www.freedesktop.org/software/libevdev/doc/latest/) - Input event handling

### Protocol Specifications

- [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) - wlroots protocol extensions
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) - Standard Wayland protocols

### Similar Projects

- [wshowkeys](https://git.sr.ht/~sircmpwn/wshowkeys) - Similar keycast tool
- [screenkey](https://gitlab.com/screenkey/screenkey) - X11 keycast tool

## License

GPL-3.0-or-later. See README.md for full license text.
