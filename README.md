# HAL — Portable 2D Game Engine

A hardware abstraction layer for building retro-style 2D games that run
natively on both the **ZX Spectrum Next** (Z80, z88dk) and
**Windows x64 / Linux** (SDL2).

The game code is fully platform-independent: it calls HAL functions for
display, input, sound, and file I/O. Two backend implementations handle the
actual hardware.

---

## Project Structure

```
hal_engine/
  src/
    hal/                         # Hardware Abstraction Layer
      hal_types.h                # Portable types, screen/tile/sprite constants
      hal_keybinds.h             # key_id_t enum, keybind_config_t, .cfg format
      hal.h                      # Public API (the ONLY header game code needs)
      next/
        hal_next.c               # Spectrum Next backend (z88dk, esxDOS)
      win64/
        hal_win64.c              # Windows/Linux backend (SDL2)
    game/                        # Platform-independent game code
      main.c                     # Entry point, scenes, player, maps, HUD
      overworld_events.h         # Event spawn system types & API
      overworld_events.c         # Spawn lifecycle, AI, encounter detection
      rng.h                      # 16-bit xorshift PRNG (header-only)
  build/
    CMakeLists.txt               # CMake build for Windows/Linux
    Makefile.next                # z88dk build for Spectrum Next
  README.md                      # This file
```

---

## Architecture Overview

### Virtual Screen

All game logic uses a fixed **256×192** pixel virtual resolution (the native
ZX Spectrum display). On Windows/Linux the window is scaled 3× to 768×576.
Coordinates, tile sizes, and sprite positions are always in virtual pixels.

### Tiles

16×16 pixel tiles. Maps are row-major uint8_t arrays where each byte is a
tile index. The engine supports maps up to 256×256 tiles. Tile collision is
table-driven with flags: `TILE_SOLID`, `TILE_PLATFORM` (one-way),
`TILE_LADDER`, `TILE_DAMAGE`, `TILE_WATER`.

### Sprites

16×16 pixel sprites. The Next has 128 hardware sprites; the SDL2 backend
composites them in software. Sprites have position, pattern index, palette,
and flags (visible, mirror X/Y, rotate). Slot 0 is reserved for the player.

### Input

16-bit bitmask polled once per frame: 4 directions + 6 action buttons + Menu
\+ Bag. Supports keyboard (rebindable), gamepad (SDL2 GameController / 
Kempston joystick).

### Keyboard Rebinding System

The engine uses a **portable key_id_t** enum (51 values: A-Z, 0-9, Enter,
Space, Shift, arrows, punctuation). Each platform maps these to native codes
at poll time via a lookup table.

**4 built-in default layouts** are compiled in as `const` data:

| Slot | Name              | Movement     | Actions        |
|------|-------------------|--------------|----------------|
| 0    | EDSF+JKLUIO       | E/D/S/F      | J K L U I O    |
| 1    | Arrows+ZXC/ASD    | Arrow keys   | Z X C A S D    |
| 2    | Custom 3           | (= Layout 0) | (placeholder)  |
| 3    | Custom 4           | (= Layout 0) | (placeholder)  |

Each slot carries a primary + alt key per button (12 buttons × 2 keys = 24
bindings). `hal_keys_set_layout(id)` copies a default into the mutable
active config.

**Runtime rebinding** via `hal_keys_rebind(bind_index, primary_id, alt_id)`
marks the config as "custom" (layout ID = 4).

**Save/load to `.cfg` files** — a compact 61-byte binary format:

```
Offset  Size  Field
0       4     Magic "KCFG"
4       1     Version (1)
5       32    Config name (null-padded)
37      12    Primary key_id per button
49      12    Alt key_id per button
        --
        61 bytes total
```

`.cfg` files are cross-platform: the same file works on both Next (esxDOS)
and Windows (stdio).

### Event Spawn System

The overworld uses a wave-based spawn cycle:

1. **ACTIVATION** — 3–8 second delay on overworld entry before anything spawns.
2. **WAITING** — Spawn timer counts down 3–8 s. Standing on a path tile
   (tile 7) freezes the timer.
3. **SPAWNED** — Group of 2–4 entities appears. They roam for 5 seconds
   (blinking in the last second as a warning), then despawn. Cycle repeats.

Spawn distribution is data-driven via `spawn_table_t` structs (enemy vs
peaceful %, rarity tiers). Three built-in tables: DEFAULT, DANGEROUS,
PEACEFUL.

### Encounter Types

| Type            | Action                              |
|-----------------|-------------------------------------|
| Enemy (weak/med/strong) | Enter combat action scene  |
| Safe Zone       | Enter peaceful map (3 variants)     |
| Friendly NPC    | Dialog overlay on overworld         |
| Discovery       | Enter special action map            |

### Peaceful Action Maps

Three safe zone variants, each a side-scrolling platformer map with NPCs:

- **Quiet Clearing** (16×12) — One NPC, flat ground, decorative blocks.
- **Caravan** (30×12) — 4 NPCs, wagon blocks, campfire path tiles.
- **Oasis Town** (50×12) — 7 NPCs, 4 buildings with doorway entrances, road.

### RNG

16-bit xorshift PRNG (header-only in `rng.h`). No multiply/divide for Z80
efficiency. Period 65535. Seeded from frame counter at startup, stirred every
frame.

---

## Building

### Windows / Linux (SDL2 + CMake)

**Prerequisites:**
- CMake 3.16+
- SDL2 development libraries
- SDL2_mixer (optional, for music)
- A C99 compiler (MSVC, GCC, or Clang)

**Install SDL2:**
```bash
# Ubuntu/Debian
sudo apt install libsdl2-dev libsdl2-mixer-dev

# macOS
brew install sdl2 sdl2_mixer

# Windows (vcpkg)
vcpkg install sdl2 sdl2-mixer
```

**Build:**
```bash
cd build

# Visual Studio
cmake -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release

# MinGW
cmake -B out -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build out

# Linux / macOS
cmake -B out -DCMAKE_BUILD_TYPE=Release
cmake --build out

# Debug build (enables hal_debug_print)
cmake -B out -DCMAKE_BUILD_TYPE=Debug
cmake --build out
```

The executable is `out/game` (Linux) or `out/Release/game.exe` (Windows).

**SDL2 include path note:** The SDL2 backend uses `#include <SDL2/SDL.h>`.
If your SDL2 installation puts headers directly in the include path (no
`SDL2/` prefix), you may need to adjust or add a symlink.

### ZX Spectrum Next (z88dk)

**Prerequisites:**
- [z88dk](https://github.com/z88dk/z88dk) installed and on PATH
- CSpect or ZEsarUX emulator for testing

**Build:**
```bash
cd build
make -f Makefile.next            # release build
make -f Makefile.next debug      # debug build (UART output)
make -f Makefile.next clean      # clean build artifacts
```

Produces `game.nex` — load in CSpect or copy to SD card for real hardware.

**Run in CSpect:**
```bash
CSpect -w3 -tv -mmc=./sdcard/ game.nex
```

---

## HAL API Reference

Game code includes only `"hal/hal.h"`, which pulls in `hal_types.h` and
`hal_keybinds.h`.

### System Lifecycle
```c
int  hal_init(void);           // init all subsystems, returns 0 on success
void hal_shutdown(void);       // free all resources
```

### Frame Timing
```c
void     hal_frame_begin(void);   // start of frame (waits for vsync on Next)
void     hal_frame_end(void);     // end of frame (flips buffers)
uint32_t hal_frame_count(void);   // frames since init (wraps at 65535 on Z80)
```

### Input
```c
uint16_t hal_input_poll(void);      // poll all devices, returns INPUT_* bitmask
uint16_t hal_input_pressed(void);   // edge-triggered: just pressed this frame
uint16_t hal_input_released(void);  // edge-triggered: just released this frame
```

### Keyboard Config
```c
void    hal_keys_set_layout(uint8_t id);         // load built-in default 0-3
uint8_t hal_keys_get_layout(void);               // current layout or CUSTOM (4)

void    hal_keys_rebind(uint8_t bind, uint8_t primary, uint8_t alt);
void    hal_keys_set_name(const char *name);
void    hal_keys_reset_defaults(uint8_t id);

int     hal_keys_save_config(const char *path);  // write active config to .cfg
int     hal_keys_load_config(const char *path);  // load .cfg into active config

const keybind_config_t *hal_keys_get_config(void);
uint8_t hal_keys_get_bind(uint8_t bind, uint8_t which);
```

### Tilemap
```c
void    hal_tiles_load(const uint8_t *data, uint8_t first, uint8_t count);
void    hal_tilemap_set(const uint8_t *data, uint16_t w, uint16_t h);
void    hal_tilemap_scroll(int16_t sx, int16_t sy);
void    hal_tilemap_draw(void);
uint8_t hal_tilemap_get(uint16_t tx, uint16_t ty);
```

### Sprites
```c
void hal_sprite_patterns_load(const uint8_t *data, uint8_t first, uint8_t count);
void hal_sprite_set(const sprite_desc_t *desc);
void hal_sprite_show(uint8_t id, bool visible);
void hal_sprite_hide_all(void);
void hal_sprites_draw(void);
```

### Drawing Primitives
```c
void     hal_draw_rect(int16_t x, int16_t y, uint8_t w, uint8_t h, color_t c);
uint8_t  hal_draw_char(int16_t x, int16_t y, char ch, color_t c);
uint16_t hal_draw_text(int16_t x, int16_t y, const char *str, color_t c);
void     hal_draw_number(int16_t x, int16_t y, int32_t value, color_t c);
```

### Sound
```c
void hal_sfx_load(sfx_id_t id, const uint8_t *data, uint16_t size);
void hal_sfx_play(sfx_id_t id, uint8_t channel);
void hal_sfx_stop(uint8_t channel);
void hal_music_play(music_id_t id, const uint8_t *data, uint16_t size);
void hal_music_stop(void);
void hal_music_volume(uint8_t vol);
void hal_music_update(void);   // call every frame
```

### File I/O
```c
int      hal_file_open(const char *path);
uint16_t hal_file_read(int handle, void *buf, uint16_t size);
void     hal_file_seek(int handle, uint32_t offset);
void     hal_file_close(int handle);
```

---

## Input Map

```
Button    key_id_t         Layout 1         Layout 2         Gamepad
--------  ---------------  ---------------  ---------------  --------
Up        BIND_UP    = 0   E (alt: Up)      Up (alt: I)      D-pad Up
Down      BIND_DOWN  = 1   D (alt: Down)    Down (alt: K)    D-pad Down
Left      BIND_LEFT  = 2   S (alt: Left)    Left (alt: J)    D-pad Left
Right     BIND_RIGHT = 3   F (alt: Right)   Right (alt: L)   D-pad Right
Action 1  BIND_BTN1  = 4   J                Z                A
Action 2  BIND_BTN2  = 5   K                X                B
Action 3  BIND_BTN3  = 6   L                C                X
Action 4  BIND_BTN4  = 7   U                A                Y
Action 5  BIND_BTN5  = 8   I                S                LB
Action 6  BIND_BTN6  = 9   O                D                RB
Menu      BIND_MENU  = 10  Enter            Enter            Start
Bag       BIND_BAG   = 11  B                B                Back
```

---

## Constants Reference

| Constant            | Value  | Description                    |
|---------------------|--------|--------------------------------|
| SCREEN_W/H          | 256/192| Virtual resolution             |
| TILE_SIZE           | 16     | Pixels per tile                |
| MAX_SPRITES         | 64     | Sprite budget                  |
| SPRITE_W/H          | 16/16  | Sprite dimensions              |
| BIND_COUNT          | 12     | Keyboard bindings per config   |
| KEY_LAYOUT_COUNT    | 4      | Built-in default layouts       |
| KEY_ID_COUNT        | 51     | Portable key identifiers       |
| KEYBIND_FILE_SIZE   | 61     | .cfg file size in bytes        |
| MAX_MAP_EVENTS      | 16     | Overworld event entity limit   |
| EVENT_LINGER_TIME   | 250    | Spawn linger (5 s at 50 Hz)    |
| SPAWN_TIMER_MIN/MAX | 150/400| Spawn wait range (3-8 s)       |
| GRAVITY             | 1      | px/frame² (action scenes)      |
| JUMP_FORCE          | -7     | Initial jump velocity          |
| MOVE_SPEED          | 2      | Horizontal walk speed          |
| MAX_FALL_SPEED      | 6      | Terminal fall velocity          |

---

## Status / TODO

Working:
- HAL interface (all functions declared and implemented on both platforms)
- Input polling with 4 rebindable keyboard layouts + gamepad
- Save/load keyboard configs to .cfg files (cross-platform binary format)
- Tilemap rendering and scroll
- Sprite rendering
- Tile collision (solid, platform, ladder, damage)
- Action scene physics (gravity, jump, climb, move, attack timer, invuln)
- Overworld event spawn lifecycle (activation → waiting → spawned → despawn)
- 3 peaceful action map variants with NPC placement
- Friendly NPC dialog system
- Combat/safe/discovery encounter routing
- HUD with phase indicators, HP, and scene labels
- Pause menu and bag overlay

Not yet implemented:
- Sprite/tile pattern data (window opens but graphics are placeholder)
- Layer 2 bank paging for drawing primitives on Next
- Sound system (PT3/VT2 integration on Next, SDL_mixer stubs on PC)
- Font bitmap (only 3 glyphs filled in win64 backend)
- Weapon hitbox logic during attack state
- Inventory/item system for bag overlay
- File-based map loading (currently hardcoded arrays)
- Breakable block logic (tile 3 marked but no handler)
- Special move implementations for Btn3-6
- Enemy AI in action combat scenes
- Merchant shop UI and quest system for NPCs
- NPC interaction/collision in peaceful maps
- Building entrance detection in oasis town
- Camera offset for NPC sprites in wide maps
- Custom definitions for Layout 3 and 4 slots
- Multiple overworld zones with different spawn tables
