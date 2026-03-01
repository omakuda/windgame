/*============================================================================
 * hal.h - Hardware Abstraction Layer Interface
 *
 * This is the ONLY header game code should include for platform services.
 * All functions declared here must be implemented by each platform backend.
 *
 * Design principles:
 *   - Game code never touches hardware directly
 *   - All coordinates are in virtual screen space (256x192)
 *   - Functions are small and Z80-friendly (no large structs by value)
 *   - Error handling via return codes, not exceptions
 *============================================================================*/

#ifndef HAL_H
#define HAL_H

#include "hal_types.h"
#include "hal_keybinds.h"

/*==========================================================================
 * SYSTEM LIFECYCLE
 *
 * Called once at startup and shutdown. The HAL initializes display,
 * audio, input, and any platform-specific subsystems.
 *==========================================================================*/

/* Initialize all subsystems. Returns 0 on success, nonzero on failure. */
int hal_init(void);

/* Shut down all subsystems and free resources. */
void hal_shutdown(void);

/*==========================================================================
 * FRAME TIMING
 *
 * The game runs at a fixed tick rate. hal_frame_begin/end bracket each
 * frame and handle vsync / buffer flipping.
 *==========================================================================*/

/* Call at the start of each game frame. Waits for vsync on Next. */
void hal_frame_begin(void);

/* Call at the end of each game frame. Flips buffers /
   presents the frame on PC. */
void hal_frame_end(void);

/* Returns the number of frames elapsed since hal_init().
   Wraps at 65535 on Z80 (uint16_t), full 32-bit on PC. */
uint32_t hal_frame_count(void);

/*==========================================================================
 * INPUT
 *
 * Polled once per frame. Returns a 16-bit bitmask of INPUT_* flags.
 * Supports 4 movement keys, 6 action buttons, menu, and bag.
 *==========================================================================*/

/* Poll all input devices and return a bitmask of currently held buttons.
   On Next: reads keyboard rows + Kempston joystick.
   On Win64: reads keyboard + gamepad via SDL2. */
uint16_t hal_input_poll(void);

/* Returns buttons that were just pressed THIS frame (edge-triggered).
   Must be called after hal_input_poll(). */
uint16_t hal_input_pressed(void);

/* Returns buttons that were just released THIS frame. */
uint16_t hal_input_released(void);

/* Set the active keyboard layout to one of the 4 built-in defaults.
   Copies the default into the mutable active config.
   Gamepad bindings are unaffected. Takes effect on next hal_input_poll(). */
void hal_keys_set_layout(uint8_t layout_id);

/* Returns the layout ID that was last loaded via hal_keys_set_layout().
   Returns KEY_LAYOUT_CUSTOM (4) if the active config was loaded from
   a .cfg file or modified via hal_keys_rebind(). */
uint8_t hal_keys_get_layout(void);

/* Full rebinding & persistence API — declared in hal_keybinds.h:
 *
 * Querying:
 *   hal_keys_get_config()          — read-only pointer to active config
 *   hal_keys_get_bind(idx, which)  — get key_id for a single slot
 *
 * Modifying:
 *   hal_keys_rebind(idx, pri, alt) — change one binding (marks custom)
 *   hal_keys_set_name(name)        — set config display name
 *   hal_keys_reset_defaults(id)    — restore a built-in default
 *
 * File I/O (.cfg files, 61 bytes each):
 *   hal_keys_save_config(path)     — write active config to file
 *   hal_keys_load_config(path)     — load file into active config
 */

/*==========================================================================
 * PALETTE
 *
 * Both platforms use 256-color palettes. On the Next this maps to
 * hardware palette registers. On Windows the palette is used to
 * convert indexed color to RGBA.
 *==========================================================================*/

/* Load a 256-entry palette. Each entry is RRRGGGBB (8-bit).
   palette_id: 0 = tilemap palette, 1 = sprite palette. */
void hal_palette_set(uint8_t palette_id, const uint8_t *colors, uint8_t count);

/* Set a single palette entry. */
void hal_palette_set_color(uint8_t palette_id, uint8_t index, uint8_t rgb8);

/*==========================================================================
 * TILEMAP
 *
 * The game uses one tilemap layer for backgrounds. The HAL renders
 * visible tiles based on the current scroll position. Tile pattern
 * data must be loaded before drawing.
 *==========================================================================*/

/* Upload tile pattern data (pixel data for tile graphics).
   data:  packed 8-bit indexed pixels, TILE_SIZE*TILE_SIZE bytes per tile.
   first: first tile index to write.
   count: number of tiles to upload. */
void hal_tiles_load(const uint8_t *data, uint8_t first, uint8_t count);

/* Set the current tilemap. Does NOT copy data — the pointer must
   remain valid. map_w and map_h are in tiles. */
void hal_tilemap_set(const uint8_t *data, uint16_t map_w, uint16_t map_h);

/* Set the tilemap scroll position in pixels. The HAL handles wrapping
   and only drawing visible tiles. */
void hal_tilemap_scroll(int16_t scroll_x, int16_t scroll_y);

/* Draw the tilemap. Called once per frame between hal_frame_begin
   and hal_frame_end. On Next this may be a no-op if the hardware
   tilemap is configured — the scroll position is enough. */
void hal_tilemap_draw(void);

/* Read a single tile from the current tilemap at tile coordinates.
   Returns the tile index, or 0 if out of bounds. */
uint8_t hal_tilemap_get(uint16_t tile_x, uint16_t tile_y);

/*==========================================================================
 * SPRITES
 *
 * Hardware sprites on Next, software blits on PC. The game sets
 * sprite properties, then the HAL draws them all at frame end.
 *==========================================================================*/

/* Upload sprite pattern data.
   Each pattern is SPRITE_W * SPRITE_H bytes of 8-bit indexed pixels.
   first: first pattern index.
   count: number of patterns to upload. */
void hal_sprite_patterns_load(const uint8_t *data, uint8_t first, uint8_t count);

/* Set all properties of a sprite slot in one call.
   This is the primary way the game positions sprites each frame. */
void hal_sprite_set(const sprite_desc_t *desc);

/* Convenience: show/hide a sprite without changing other properties. */
void hal_sprite_show(uint8_t id, bool visible);

/* Hide ALL sprites. Useful on scene transitions. */
void hal_sprite_hide_all(void);

/* Draw all visible sprites. On Next this commits the sprite attribute
   table to hardware. On Win64 this blits each sprite to the backbuffer. */
void hal_sprites_draw(void);

/*==========================================================================
 * DRAWING PRIMITIVES
 *
 * For UI elements, text, health bars, etc. These draw into the
 * display buffer directly. Keep usage minimal on Z80 — prefer
 * tiles and sprites for game content.
 *==========================================================================*/

/* Fill a rectangle with a palette color. */
void hal_draw_rect(int16_t x, int16_t y, uint8_t w, uint8_t h, color_t color);

/* Draw a single character at pixel position using the built-in font.
   Returns the x advance (typically 8 pixels). */
uint8_t hal_draw_char(int16_t x, int16_t y, char ch, color_t color);

/* Draw a null-terminated string. Returns total x advance. */
uint16_t hal_draw_text(int16_t x, int16_t y, const char *str, color_t color);

/* Draw a formatted number (useful for score, HP, etc.).
   Avoids printf on Z80 — just renders decimal digits. */
void hal_draw_number(int16_t x, int16_t y, int32_t value, color_t color);

/*==========================================================================
 * SOUND
 *
 * Simple sound effect and music playback. On Next this drives the
 * AY-3-8910 or NextDAC. On Win64 this uses SDL2_mixer.
 *==========================================================================*/

/* Load a sound effect from a data blob. The format is platform-specific
   (raw PCM / AY register dumps / etc). The sfx_id is chosen by the game. */
void hal_sfx_load(sfx_id_t id, const uint8_t *data, uint16_t size);

/* Play a loaded sound effect. channel = 0..3 (or HAL picks if 0xFF). */
void hal_sfx_play(sfx_id_t id, uint8_t channel);

/* Stop a channel. */
void hal_sfx_stop(uint8_t channel);

/* Load and start background music. Only one track plays at a time. */
void hal_music_play(music_id_t id, const uint8_t *data, uint16_t size);

/* Stop music. */
void hal_music_stop(void);

/* Set music volume (0 = silent, 255 = max). */
void hal_music_volume(uint8_t vol);

/* Must be called each frame to pump the music driver. */
void hal_music_update(void);

/*==========================================================================
 * DATA / RESOURCE LOADING
 *
 * Platform-specific file I/O. On Next this reads from SD card (esxDOS).
 * On Win64 this reads from the filesystem.
 *==========================================================================*/

/* Open a file for reading. Returns a handle (>=0) or -1 on error.
   Path uses '/' separators on all platforms. */
int hal_file_open(const char *path);

/* Read up to 'size' bytes into buffer. Returns bytes actually read. */
uint16_t hal_file_read(int handle, void *buffer, uint16_t size);

/* Seek to a byte offset from the start of the file. */
void hal_file_seek(int handle, uint32_t offset);

/* Close a file handle. */
void hal_file_close(int handle);

/*==========================================================================
 * MEMORY HELPERS
 *
 * On Z80, standard memcpy/memset may not exist or may be slow.
 * The HAL provides optimized versions.
 *==========================================================================*/

void hal_memcpy(void *dst, const void *src, uint16_t size);
void hal_memset(void *dst, uint8_t val, uint16_t size);

/*==========================================================================
 * DEBUG / DEVELOPMENT
 *
 * Available in debug builds. On Next, prints to UART or CSpect debug.
 * On Win64, prints to stdout / debug console.
 *==========================================================================*/
#ifdef DEBUG
void hal_debug_print(const char *msg);
void hal_debug_number(const char *label, int32_t value);
#else
#define hal_debug_print(msg)        ((void)0)
#define hal_debug_number(lbl, val)  ((void)0)
#endif

#endif /* HAL_H */
