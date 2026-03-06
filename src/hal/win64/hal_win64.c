/*============================================================================
 * hal_win64.c - Windows x64 HAL Implementation (SDL2)
 *
 * Emulates the Spectrum Next's display model using SDL2:
 *   - 256x192 virtual resolution scaled to a window
 *   - 256-color indexed palettes
 *   - Software sprite compositing
 *   - Tilemap rendering to an offscreen buffer
 *
 * Dependencies:
 *   - SDL2       (rendering, input, timing)
 *   - SDL2_mixer (sound effects, music)
 *
 * Compile with:
 *   cl /O2 hal_win64.c /I<SDL2_include> /link SDL2.lib SDL2_mixer.lib
 *   or via CMake (see build/CMakeLists.txt)
 *============================================================================*/

#ifndef __Z88DK

#include "../hal.h"

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*--------------------------------------------------------------------------
 * Configuration
 *--------------------------------------------------------------------------*/
#define WINDOW_SCALE     4       /* 256x192 * 4 = 1024x768 window       */
#define WINDOW_W         (SCREEN_W * WINDOW_SCALE)
#define WINDOW_H         (SCREEN_H * WINDOW_SCALE)
#define TARGET_FPS       50      /* Match the Next's 50Hz refresh (PAL) */
#define FRAME_TIME_MS    (1000 / TARGET_FPS)

#define AUDIO_FREQ       44100
#define AUDIO_CHANNELS   1
#define AUDIO_CHUNK_SIZE 1024
#define MAX_SFX          32
#define MIX_CHANNELS     8

/*--------------------------------------------------------------------------
 * Internal state
 *--------------------------------------------------------------------------*/

static SDL_Window   *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture  *s_framebuffer;     /* 256x192 streaming texture    */

/* The virtual framebuffer: 256x192, 8-bit indexed color */
static uint8_t s_pixels[SCREEN_W * SCREEN_H];

/* RGBA conversion buffer */
static uint32_t s_rgba[SCREEN_W * SCREEN_H];

/* Two palettes: 0 = tilemap, 1 = sprites */
static uint8_t  s_palette_raw[2][256];
static uint32_t s_palette_rgba[2][256];

/* Frame timing */
static uint32_t s_frame_counter;
static uint32_t s_frame_start_ms;

/* Input — now 16-bit */
static uint16_t s_input_current;
static uint16_t s_input_previous;
static uint8_t  s_last_key_pressed; /* key_id_t of most recent key press */
static SDL_GameController *s_gamepad;

/* Sprites */
static sprite_desc_t s_sprites[MAX_SPRITES];
static uint8_t s_sprite_patterns[256][SPRITE_W * SPRITE_H];

/* Tilemap */
static uint8_t s_tile_patterns[256][TILE_SIZE * TILE_SIZE];
static uint8_t s_tilemap_buf[128 * 64]; /* mutable copy for hal_tilemap_put */
static uint8_t *s_tilemap_data;
static uint16_t s_tilemap_w;
static uint16_t s_tilemap_h;
static int16_t  s_scroll_x;
static int16_t  s_scroll_y;

/* Sound */
static Mix_Chunk *s_sfx[MAX_SFX];
static Mix_Music *s_music;

/*--------------------------------------------------------------------------
 * Palette helpers
 *--------------------------------------------------------------------------*/

static uint32_t rgb8_to_argb32(uint8_t c) {
    uint32_t r = ((c >> 5) & 0x07) * 36;
    uint32_t g = ((c >> 2) & 0x07) * 36;
    uint32_t b = ((c >> 0) & 0x03) * 85;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void rebuild_palette_rgba(uint8_t palette_id) {
    int i;
    for (i = 0; i < 256; i++) {
        s_palette_rgba[palette_id][i] = rgb8_to_argb32(s_palette_raw[palette_id][i]);
    }
}

/*--------------------------------------------------------------------------
 * SYSTEM LIFECYCLE
 *--------------------------------------------------------------------------*/

int hal_init(void) {
    int i;

    /* We define our own main() and don't link SDL2main,
       so tell SDL not to expect SDL_main. */
    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    s_window = SDL_CreateWindow(
        "Game (Next HAL)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_SHOWN
    );
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(s_renderer, SCREEN_W, SCREEN_H);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    s_framebuffer = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H);
    if (!s_framebuffer) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    if (Mix_OpenAudio(AUDIO_FREQ, MIX_DEFAULT_FORMAT, AUDIO_CHANNELS, AUDIO_CHUNK_SIZE) != 0) {
        fprintf(stderr, "Mix_OpenAudio failed: %s\n", Mix_GetError());
    }
    Mix_AllocateChannels(MIX_CHANNELS);

    s_gamepad = NULL;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            s_gamepad = SDL_GameControllerOpen(i);
            break;
        }
    }

    for (i = 0; i < 256; i++) {
        s_palette_raw[0][i] = (uint8_t)i;
        s_palette_raw[1][i] = (uint8_t)i;
    }
    rebuild_palette_rgba(0);
    rebuild_palette_rgba(1);

    memset(s_pixels, 0, sizeof(s_pixels));
    memset(s_sprites, 0, sizeof(s_sprites));
    memset(s_sprite_patterns, 0, sizeof(s_sprite_patterns));
    memset(s_tile_patterns, 0, sizeof(s_tile_patterns));
    memset(s_sfx, 0, sizeof(s_sfx));

    /*------------------------------------------------------------------
     * Generate placeholder tile patterns so maps are visible.
     *
     * Tile indices used by the game:
     *   0 = grass/empty (dark green)    4 = platform (brown)
     *   1 = solid wall  (grey)          5 = ladder   (yellow-brown)
     *   2 = brick       (red-brown)     6 = damage   (red)
     *   3 = breakable   (orange)        7 = path     (tan)
     *
     * Color format: RRRGGGBB (3-3-2 palette)
     *------------------------------------------------------------------*/
    {
        /* Base colors for each tile type (RRRGGGBB) */
        static const uint8_t tile_colors[14] = {
            0x0C,  /* 0: dark green   000 011 00 */
            0x6D,  /* 1: grey         011 011 01 */
            0x88,  /* 2: red-brown    100 010 00 */
            0xC8,  /* 3: orange       110 010 00 */
            0x64,  /* 4: brown        011 001 00 */
            0xA4,  /* 5: yellow-brown 101 001 00 */
            0xC0,  /* 6: red          110 000 00 */
            0xAD,  /* 7: tan          101 011 01 */
            0x03,  /* 8: exit - dark blue-green */
            0x5F,  /* 9: transition - purple   */
            0x13,  /* 10: water - blue         */
            0xFC,  /* 11: town - yellow        */
            0x08,  /* 12: hidden - dark green  */
            0x24,  /* 13: forest - green       */
            0xC0,  /* 14: brick                */
            0x64,  /* 15: bark                 */
            0x92,  /* 16: stone_wall           */
            0xA4,  /* 17: wood_plank           */
            0x28,  /* 18: grass                */
            0xED,  /* 19: sand                 */
            0xC0,  /* 20: roof                 */
            0x92,  /* 21: cobblestone          */
            0x64,  /* 22: dirt                 */
            0x9F,  /* 23: window               */
            0x13,  /* 24: world_water          */
            0xA9,  /* 25: world_road           */
        };
        /* Slightly lighter variant for dithering */
        static const uint8_t tile_colors_hi[26] = {
            0x10,  /* 0 */
            0xB6,  /* 1 */
            0xCC,  /* 2 */
            0xE9,  /* 3 */
            0xA8,  /* 4 */
            0xE8,  /* 5 */
            0xE4,  /* 6 */
            0xED,  /* 7 */
            0x17,  /* 8: exit lighter */
            0x9F,  /* 9: transition lighter */
            0x1B,  /* 10: water lighter */
            0xFD,  /* 11: town lighter */
            0x0C,  /* 12: hidden lighter */
            0x28,  /* 13: forest lighter */
            0xE4,  /* 14: brick lighter */
            0x24,  /* 15: bark lighter  */
            0xB6,  /* 16: stone_wall lighter */
            0x64,  /* 17: wood_plank lighter */
            0x2C,  /* 18: grass lighter */
            0xE9,  /* 19: sand lighter  */
            0x80,  /* 20: roof lighter  */
            0x6D,  /* 21: cobblestone lighter */
            0x44,  /* 22: dirt lighter  */
            0xBF,  /* 23: window lighter */
            0x17,  /* 24: world_water lighter */
            0xED,  /* 25: world_road lighter */
        };
        int t, px, py;
        for (t = 0; t < 26; t++) {
            uint8_t c0 = tile_colors[t];
            uint8_t c1 = tile_colors_hi[t];
            for (py = 0; py < TILE_SIZE; py++) {
                for (px = 0; px < TILE_SIZE; px++) {
                    uint8_t c;
                    /* Checkerboard dither for slight texture */
                    if ((px + py) & 1)
                        c = c0;
                    else
                        c = c1;
                    /* Tile border (1px darker edge) */
                    if (t >= 1 && t <= 3) {
                        if (px == 0 || py == 0 || px == TILE_SIZE-1 || py == TILE_SIZE-1)
                            c = c0 & 0x6D; /* darken */
                    }
                    /* Platform: only top 3 rows solid */
                    if (t == 4 && py > 2)
                        c = 0x00; /* transparent/black */
                    /* Ladder: two vertical bars */
                    if (t == 5) {
                        if ((px >= 3 && px <= 4) || (px >= 11 && px <= 12))
                            c = c1;
                        else if (py % 4 == 0)
                            c = c0;  /* rungs */
                        else
                            c = 0x00;
                    }
                    /* Damage: animated-looking spikes */
                    if (t == 6 && py < 8) {
                        int spike_x = px % 8;
                        if (spike_x < py || spike_x > (7 - py))
                            c = 0x00;
                    }
                    /* Exit: pulsing border arrows pointing outward */
                    if (t == 8) {
                        if (px == 0 || px == TILE_SIZE-1 ||
                            py == 0 || py == TILE_SIZE-1)
                            c = c1;
                        else if ((px + py) % 4 == 0)
                            c = c1;
                        else
                            c = c0;
                    }
                    /* Transition: door/portal frame */
                    if (t == 9) {
                        if (px <= 1 || px >= TILE_SIZE-2)
                            c = c1;  /* door frame sides */
                        else if (py <= 1)
                            c = c1;  /* door frame top */
                        else if (py >= TILE_SIZE-2)
                            c = c0;  /* threshold */
                        else
                            c = 0x00; /* dark interior */
                    }
                    /* Water: wavy surface with darker below */
                    if (t == 10) {
                        if (py <= 2)
                            c = 0x1B; /* surface highlight */
                        else if (py <= 4 && ((px + py) & 3) == 0)
                            c = 0x1B; /* wave crests */
                        else
                            c = (py > 10) ? 0x03 : c; /* deep water darker */
                    }
                    /* Town: building shape with door (overworld only — cleared in action) */
                    if (t == 11) {
                        if (py <= 3 && px >= 3 && px <= 12)
                            c = 0xFC; /* roof */
                        else if (py > 3 && py <= 12 && (px == 3 || px == 12))
                            c = 0xE8; /* walls */
                        else if (py >= 10 && px >= 6 && px <= 9)
                            c = 0x64; /* door */
                        else if (py > 3 && py < 10 && px > 3 && px < 12)
                            c = 0xED; /* wall fill */
                    }
                    /* Hidden area: dark with subtle glow */
                    if (t == 12) {
                        if ((px + py * 3) % 7 == 0)
                            c = 0x0C; /* sparse glow dots */
                        else
                            c = (py < 4) ? 0x04 : 0x00;
                    }
                    /* Forest: tree shapes (overworld only — cleared in action) */
                    if (t == 13) {
                        int cx2 = px - 7, cy2 = py - 4;
                        if (cx2*cx2 + cy2*cy2 <= 20)
                            c = 0x24; /* canopy */
                        else if (px >= 6 && px <= 9 && py >= 10)
                            c = 0x64; /* trunk */
                    }
                    /* World water (24): tileable seamless wave pattern */
                    if (t == 24) {
                        int wave = (px * 3 + py * 7) % 11;
                        c = (wave < 3) ? 0x1B : (wave < 6) ? 0x17 : (wave < 9) ? 0x13 : 0x03;
                    }
                    /* World road (25): tileable packed dirt path */
                    if (t == 25) {
                        int noise = (px * 7 + py * 11) % 13;
                        c = (noise < 3) ? 0xA9 : (noise < 7) ? 0xAD : ((px+py)&1) ? 0xED : 0xE9;
                    }
                    s_tile_patterns[t][py * TILE_SIZE + px] = c;
                }
            }
        }
    }

    /*------------------------------------------------------------------
     * Generate placeholder sprite patterns.
     *   0 = player (blue humanoid)
     *   4-6 = enemies (red shades)
     *   7 = safe zone marker (green)
     *   8 = friendly NPC (cyan)
     *   9 = discovery (yellow star)
     *------------------------------------------------------------------*/
    {
        /* Simple filled circle/diamond for each sprite */
        int px, py;
        /* Pattern 0: Player — blue humanoid silhouette */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                int cx = px - 7, cy = py - 7;
                /* Head (top circle) */
                if (py >= 1 && py <= 5 && px >= 5 && px <= 10)
                    c = 0x1B; /* bright blue 000 110 11 */
                /* Body */
                if (py >= 6 && py <= 10 && px >= 4 && px <= 11)
                    c = 0x13; /* blue 000 100 11 */
                /* Legs */
                if (py >= 11 && py <= 14) {
                    if ((px >= 4 && px <= 6) || (px >= 9 && px <= 11))
                        c = 0x0F; /* dark blue */
                }
                /* Eyes */
                if (py == 3 && (px == 6 || px == 9))
                    c = 0xFF; /* white */
                s_sprite_patterns[0][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 4: Weak enemy (red blob) */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                int dx = px - 7, dy = py - 7;
                if (dx*dx + dy*dy <= 42)
                    s_sprite_patterns[4][py * SPRITE_W + px] = 0xC0; /* red */
                if (dx*dx + dy*dy <= 16)
                    s_sprite_patterns[4][py * SPRITE_W + px] = 0xE4; /* bright red */
            }
        }
        /* Pattern 5: Medium enemy (darker red) */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                int dx = px - 7, dy = py - 8;
                if (dx*dx + dy*dy <= 49)
                    s_sprite_patterns[5][py * SPRITE_W + px] = 0x80; /* dark red */
                if (py >= 2 && py <= 4 && (px == 5 || px == 10))
                    s_sprite_patterns[5][py * SPRITE_W + px] = 0xFF; /* eyes */
            }
        }
        /* Pattern 6: Strong enemy (big dark red) */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                int dx = px - 7, dy = py - 7;
                if (dx*dx + dy*dy <= 56)
                    s_sprite_patterns[6][py * SPRITE_W + px] = 0xA0;
                if (py == 5 && px >= 4 && px <= 11)
                    s_sprite_patterns[6][py * SPRITE_W + px] = 0xFF;
            }
        }
        /* Pattern 7: Safe zone (green diamond) */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                int dx = (px > 7) ? px - 7 : 7 - px;
                int dy = (py > 7) ? py - 7 : 7 - py;
                if (dx + dy <= 7)
                    s_sprite_patterns[7][py * SPRITE_W + px] = 0x1C; /* green */
            }
        }
        /* Pattern 8: Friendly NPC (cyan humanoid) */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                if (py >= 1 && py <= 5 && px >= 5 && px <= 10)
                    c = 0x1F; /* cyan */
                if (py >= 6 && py <= 10 && px >= 4 && px <= 11)
                    c = 0x17;
                if (py >= 11 && py <= 14 && ((px >= 4 && px <= 6) || (px >= 9 && px <= 11)))
                    c = 0x13;
                if (py == 3 && (px == 6 || px == 9))
                    c = 0xFF;
                s_sprite_patterns[8][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 9: Discovery (yellow star) */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                int dx = (px > 7) ? px - 7 : 7 - px;
                int dy = (py > 7) ? py - 7 : 7 - py;
                if ((dx <= 1 && dy <= 7) || (dy <= 1 && dx <= 7) ||
                    (dx + dy <= 4))
                    s_sprite_patterns[9][py * SPRITE_W + px] = 0xFC; /* yellow */
            }
        }
        /* Pattern 1: Overworld player — 8x8 blue figure centred in 16x16
         * Offset 4,4 so the visible pixels occupy (4..11, 4..11). */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                int lx = px - 4, ly = py - 4; /* local 0-7 coords */
                if (lx >= 0 && lx < 8 && ly >= 0 && ly < 8) {
                    /* Head: rows 0-2, cols 2-5 */
                    if (ly <= 2 && lx >= 2 && lx <= 5) c = 0x1B;
                    /* Body: rows 3-5, cols 1-6 */
                    if (ly >= 3 && ly <= 5 && lx >= 1 && lx <= 6) c = 0x13;
                    /* Legs: rows 6-7 */
                    if (ly >= 6 && ((lx >= 1 && lx <= 2) || (lx >= 5 && lx <= 6)))
                        c = 0x0F;
                    /* Eyes */
                    if (ly == 1 && (lx == 3 || lx == 4)) c = 0xFF;
                }
                s_sprite_patterns[1][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 2: Action player TOP (head + torso, upper 16x16 of 16x32)
         * Pattern 3: Action player BOTTOM (waist + legs, lower 16x16 of 16x32)
         * Together they form a tall humanoid sprite. */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                /* TOP half: head rows 2-7, shoulders/torso rows 8-15 */
                /* Hair (top of head) */
                if (py >= 1 && py <= 2 && px >= 5 && px <= 10)
                    c = 0x0B; /* dark blue-green hair */
                /* Head */
                if (py >= 3 && py <= 7 && px >= 4 && px <= 11)
                    c = 0x1B; /* bright blue */
                /* Eyes */
                if (py == 5 && (px == 6 || px == 9))
                    c = 0xFF; /* white */
                /* Neck */
                if (py == 8 && px >= 6 && px <= 9)
                    c = 0x1B;
                /* Shoulders */
                if (py >= 9 && py <= 10 && px >= 3 && px <= 12)
                    c = 0x13; /* blue body */
                /* Upper torso */
                if (py >= 11 && py <= 15 && px >= 4 && px <= 11)
                    c = 0x13;
                /* Belt line at bottom */
                if (py == 15 && px >= 4 && px <= 11)
                    c = 0x0B;
                s_sprite_patterns[2][py * SPRITE_W + px] = c;
            }
        }
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                /* BOTTOM half: hips rows 0-3, legs rows 4-13, boots 14-15 */
                /* Hips */
                if (py <= 3 && px >= 4 && px <= 11)
                    c = 0x13;
                /* Left leg */
                if (py >= 4 && py <= 13 && px >= 4 && px <= 6)
                    c = 0x0F; /* dark blue */
                /* Right leg */
                if (py >= 4 && py <= 13 && px >= 9 && px <= 11)
                    c = 0x0F;
                /* Boots */
                if (py >= 13 && py <= 15 && px >= 3 && px <= 7)
                    c = 0x09; /* darker */
                if (py >= 13 && py <= 15 && px >= 8 && px <= 12)
                    c = 0x09;
                s_sprite_patterns[3][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 10: Merchant — gold/brown figure with bag */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                if (py >= 2 && py <= 5 && px >= 5 && px <= 10) c = 0xA8; /* brown head */
                if (py == 3 && (px == 6 || px == 9)) c = 0xFF;
                if (py >= 6 && py <= 12 && px >= 4 && px <= 11) c = 0xE8; /* gold body */
                if (py >= 8 && py <= 11 && px >= 11 && px <= 14) c = 0x64; /* bag */
                if (py >= 13 && py <= 15 && ((px >= 4 && px <= 6)||(px >= 9 && px <= 11))) c = 0xA4;
                s_sprite_patterns[10][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 11: Healer — white/green figure with cross */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                if (py >= 2 && py <= 5 && px >= 5 && px <= 10) c = 0xB6; /* light head */
                if (py == 3 && (px == 6 || px == 9)) c = 0x0C;
                if (py >= 6 && py <= 12 && px >= 4 && px <= 11) c = 0xFF; /* white robe */
                if (py >= 7 && py <= 9 && px == 7) c = 0x10; /* cross vert */
                if (py == 8 && px >= 6 && px <= 8) c = 0x10; /* cross horiz */
                if (py >= 13 && py <= 15 && ((px >= 4 && px <= 6)||(px >= 9 && px <= 11))) c = 0xB6;
                s_sprite_patterns[11][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 12: Sage — purple/dark figure with pointed hat */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                if (py == 0 && px >= 7 && px <= 8) c = 0x5F; /* hat tip */
                if (py == 1 && px >= 6 && px <= 9) c = 0x5F;
                if (py >= 2 && py <= 3 && px >= 5 && px <= 10) c = 0x5F; /* hat brim */
                if (py >= 4 && py <= 6 && px >= 5 && px <= 10) c = 0x9F; /* face */
                if (py == 5 && (px == 6 || px == 9)) c = 0xFF;
                if (py >= 7 && py <= 12 && px >= 4 && px <= 11) c = 0x5F; /* purple robe */
                if (py >= 13 && py <= 15 && ((px >= 4 && px <= 6)||(px >= 9 && px <= 11))) c = 0x53;
                s_sprite_patterns[12][py * SPRITE_W + px] = c;
            }
        }
        /* Pattern 13: Wanderer — brown/green travel cloak */
        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t c = 0;
                if (py >= 1 && py <= 2 && px >= 6 && px <= 9) c = 0x64; /* hood */
                if (py >= 3 && py <= 5 && px >= 5 && px <= 10) c = 0xAD; /* face */
                if (py == 4 && (px == 6 || px == 9)) c = 0xFF;
                if (py >= 6 && py <= 13 && px >= 3 && px <= 12) c = 0x0C; /* green cloak */
                if (py >= 6 && py <= 13 && px >= 5 && px <= 10) c = 0x64; /* inner brown */
                if (py >= 14 && py <= 15 && ((px >= 4 && px <= 6)||(px >= 9 && px <= 11))) c = 0x64;
                s_sprite_patterns[13][py * SPRITE_W + px] = c;
            }
        }
    }

    s_tilemap_data = NULL;
    s_tilemap_w = 0;
    s_tilemap_h = 0;
    s_scroll_x = 0;
    s_scroll_y = 0;

    s_frame_counter = 0;
    s_input_current = 0;
    s_input_previous = 0;
    s_music = NULL;

    /* Initialize keyboard config to default layout 1 */
    hal_keys_set_layout(KEY_LAYOUT_1);

    return 0;
}

void hal_shutdown(void) {
    int i;

    hal_music_stop();

    for (i = 0; i < MAX_SFX; i++) {
        if (s_sfx[i]) {
            Mix_FreeChunk(s_sfx[i]);
            s_sfx[i] = NULL;
        }
    }

    Mix_CloseAudio();

    if (s_gamepad) {
        SDL_GameControllerClose(s_gamepad);
        s_gamepad = NULL;
    }

    if (s_framebuffer) SDL_DestroyTexture(s_framebuffer);
    if (s_renderer)    SDL_DestroyRenderer(s_renderer);
    if (s_window)      SDL_DestroyWindow(s_window);

    SDL_Quit();
}

/*--------------------------------------------------------------------------
 * FRAME TIMING
 *--------------------------------------------------------------------------*/

void hal_frame_begin(void) {
    s_frame_start_ms = SDL_GetTicks();
    memset(s_pixels, 0, sizeof(s_pixels));
}

void hal_frame_end(void) {
    uint32_t elapsed;
    int pitch;
    void *tex_pixels;
    int i;

    for (i = 0; i < SCREEN_W * SCREEN_H; i++) {
        s_rgba[i] = s_palette_rgba[0][s_pixels[i]];
    }

    SDL_LockTexture(s_framebuffer, NULL, &tex_pixels, &pitch);
    memcpy(tex_pixels, s_rgba, SCREEN_W * SCREEN_H * sizeof(uint32_t));
    SDL_UnlockTexture(s_framebuffer);

    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_framebuffer, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    s_frame_counter++;

    elapsed = SDL_GetTicks() - s_frame_start_ms;
    if (elapsed < FRAME_TIME_MS) {
        SDL_Delay(FRAME_TIME_MS - elapsed);
    }
}

uint32_t hal_frame_count(void) {
    return s_frame_counter;
}

/*--------------------------------------------------------------------------
 * INPUT — Rebindable keyboard system
 *
 * The active config stores key_id_t IDs (platform-portable).
 * A resolved array maps each bind to an SDL_Scancode for fast polling.
 * Save/load uses a compact binary .cfg format.
 *
 * Gamepad mapping is layout-independent and always active.
 *--------------------------------------------------------------------------*/

/*--- key_id_t → SDL_Scancode lookup table ---
 *
 * Array index = key_id_t enum value.
 * Must match the enum order in hal_keybinds.h exactly.
 *---*/

static const int s_keyid_to_sdl[KEY_ID_COUNT] = {
    /* KEY_ID_NONE */     0,
    /* A-Z (1..26) */
    SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
    SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
    SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
    SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
    SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    /* 0-9 (27..36) */
    SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
    SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_9,
    /* Special keys (37..42) — must match enum: ENTER,SPACE,SHIFT,BACKSPACE,TAB,ESCAPE */
    SDL_SCANCODE_RETURN,      /* KEY_ID_ENTER     = 37 */
    SDL_SCANCODE_SPACE,       /* KEY_ID_SPACE     = 38 */
    SDL_SCANCODE_LSHIFT,      /* KEY_ID_SHIFT     = 39 */
    SDL_SCANCODE_BACKSPACE,   /* KEY_ID_BACKSPACE = 40 */
    SDL_SCANCODE_TAB,         /* KEY_ID_TAB       = 41 */
    SDL_SCANCODE_ESCAPE,      /* KEY_ID_ESCAPE    = 42 */
    /* Arrows (43..46) */
    SDL_SCANCODE_UP,          /* KEY_ID_UP        = 43 */
    SDL_SCANCODE_DOWN,        /* KEY_ID_DOWN      = 44 */
    SDL_SCANCODE_LEFT,        /* KEY_ID_LEFT      = 45 */
    SDL_SCANCODE_RIGHT,       /* KEY_ID_RIGHT     = 46 */
    /* Punctuation (47..50) */
    SDL_SCANCODE_COMMA,       /* KEY_ID_COMMA     = 47 */
    SDL_SCANCODE_PERIOD,      /* KEY_ID_PERIOD    = 48 */
    SDL_SCANCODE_SLASH,       /* KEY_ID_SLASH     = 49 */
    SDL_SCANCODE_SEMICOLON,   /* KEY_ID_SEMICOLON = 50 */
};

/*--- Mutable active config (keyname IDs) ---*/
static keybind_config_t s_active_config;
static uint8_t      s_active_layout_id = KEY_LAYOUT_1;

/*--- Resolved native codes for fast polling ---*/
typedef struct {
    int primary;     /* SDL_Scancode */
    int alt;         /* SDL_Scancode (0 = unbound) */
    uint16_t flag;   /* INPUT_* bitmask */
} resolved_bind_t;

static resolved_bind_t s_resolved[BIND_COUNT];

/*--- Resolve all keyname IDs in the active config to SDL scancodes ---*/
static void resolve_config(void) {
    int i;
    for (i = 0; i < BIND_COUNT; i++) {
        uint8_t pk = s_active_config.primary[i];
        uint8_t ak = s_active_config.alt[i];
        s_resolved[i].primary = (pk < KEY_ID_COUNT) ? s_keyid_to_sdl[pk] : 0;
        s_resolved[i].alt     = (ak < KEY_ID_COUNT) ? s_keyid_to_sdl[ak] : 0;
        s_resolved[i].flag    = bind_to_flag[i];
    }
}

/*--- Layout management ---*/

void hal_keys_set_layout(uint8_t layout_id) {
    if (layout_id >= KEY_LAYOUT_COUNT) return;
    s_active_config = *keybind_defaults[layout_id];
    s_active_layout_id = layout_id;
    resolve_config();
}

uint8_t hal_keys_get_layout(void) {
    return s_active_layout_id;
}

/*--- Rebinding ---*/

void hal_keys_rebind(uint8_t bind_idx, uint8_t primary, uint8_t alt) {
    if (bind_idx >= BIND_COUNT) return;
    s_active_config.primary[bind_idx] = primary;
    s_active_config.alt[bind_idx]     = alt;
    s_active_layout_id = KEY_LAYOUT_CUSTOM;  /* mark as custom */
    resolve_config();
}

void hal_keys_reset_defaults(uint8_t layout_id) {
    hal_keys_set_layout(layout_id);
}

uint8_t hal_keys_get_bind(uint8_t bind_idx, uint8_t which) {
    if (bind_idx >= BIND_COUNT) return KEY_ID_NONE;
    return which ? s_active_config.alt[bind_idx]
                 : s_active_config.primary[bind_idx];
}

const keybind_config_t *hal_keys_get_config(void) {
    return &s_active_config;
}

void hal_keys_set_name(const char *name) {
    int i;
    for (i = 0; i < KEYBIND_NAME_LEN - 1 && name[i]; i++) {
        s_active_config.name[i] = name[i];
    }
    s_active_config.name[i] = '\0';
}

/*--- Save/load .cfg files ---*/

int hal_keys_save_config(const char *path) {
    FILE *f;
    uint8_t version = KEYBIND_VERSION;
    char name_buf[KEYBIND_NAME_LEN];

    f = fopen(path, "wb");
    if (!f) return -1;

    /* Magic */
    if (fwrite(KEYBIND_MAGIC, 1, KEYBIND_MAGIC_LEN, f) != KEYBIND_MAGIC_LEN)
        goto fail;

    /* Version */
    if (fwrite(&version, 1, 1, f) != 1)
        goto fail;

    /* Name (fixed 32 bytes, null-padded) */
    memset(name_buf, 0, KEYBIND_NAME_LEN);
    memcpy(name_buf, s_active_config.name, KEYBIND_NAME_LEN);
    if (fwrite(name_buf, 1, KEYBIND_NAME_LEN, f) != KEYBIND_NAME_LEN)
        goto fail;

    /* Primary key_ids */
    if (fwrite(s_active_config.primary, 1, BIND_COUNT, f) != BIND_COUNT)
        goto fail;

    /* Alt key_ids */
    if (fwrite(s_active_config.alt, 1, BIND_COUNT, f) != BIND_COUNT)
        goto fail;

    fclose(f);
    return 0;

fail:
    fclose(f);
    return -1;
}

int hal_keys_load_config(const char *path) {
    FILE *f;
    char magic[KEYBIND_MAGIC_LEN];
    uint8_t version;
    keybind_config_t temp;
    int i;

    f = fopen(path, "rb");
    if (!f) return -1;

    /* Read and validate magic */
    if (fread(magic, 1, KEYBIND_MAGIC_LEN, f) != KEYBIND_MAGIC_LEN) {
        fclose(f); return -1;
    }
    if (magic[0] != 'K' || magic[1] != 'C' ||
        magic[2] != 'F' || magic[3] != 'G') {
        fclose(f); return -1;
    }

    /* Version */
    if (fread(&version, 1, 1, f) != 1) { fclose(f); return -1; }
    if (version != KEYBIND_VERSION) { fclose(f); return -1; }

    /* Name */
    memset(temp.name, 0, KEYBIND_NAME_LEN);
    if (fread(temp.name, 1, KEYBIND_NAME_LEN, f) != KEYBIND_NAME_LEN) {
        fclose(f); return -1;
    }
    temp.name[KEYBIND_NAME_LEN - 1] = '\0';

    /* Primary key_ids */
    if (fread(temp.primary, 1, BIND_COUNT, f) != BIND_COUNT) {
        fclose(f); return -1;
    }

    /* Alt key_ids */
    if (fread(temp.alt, 1, BIND_COUNT, f) != BIND_COUNT) {
        fclose(f); return -1;
    }

    fclose(f);

    /* Validate key_ids */
    for (i = 0; i < BIND_COUNT; i++) {
        if (temp.primary[i] >= KEY_ID_COUNT) temp.primary[i] = KEY_ID_NONE;
        if (temp.alt[i] >= KEY_ID_COUNT)     temp.alt[i] = KEY_ID_NONE;
    }

    /* Apply */
    memcpy(&s_active_config, &temp, sizeof(keybind_config_t));
    s_active_layout_id = KEY_LAYOUT_CUSTOM;
    resolve_config();
    return 0;
}

/*--- Input polling ---*/

uint16_t hal_input_poll(void) {
    SDL_Event ev;
    const uint8_t *keys;
    uint16_t state = 0;
    int i;

    s_input_previous = s_input_current;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            hal_shutdown();
            exit(0);
        }
    }

    keys = SDL_GetKeyboardState(NULL);

    /* Scan all resolved bindings */
    for (i = 0; i < BIND_COUNT; i++) {
        if (s_resolved[i].primary != 0 && keys[s_resolved[i].primary]) {
            state |= s_resolved[i].flag;
        }
        if (s_resolved[i].alt != 0 && keys[s_resolved[i].alt]) {
            state |= s_resolved[i].flag;
        }
    }

    /* Gamepad (layout-independent) */
    if (s_gamepad) {
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP))    state |= INPUT_UP;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  state |= INPUT_DOWN;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  state |= INPUT_LEFT;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) state |= INPUT_RIGHT;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_A))          state |= INPUT_BTN1;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_B))          state |= INPUT_BTN2;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_X))          state |= INPUT_BTN3;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_Y))          state |= INPUT_BTN4;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  state |= INPUT_BTN5;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) state |= INPUT_BTN6;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_START))      state |= INPUT_MENU;
        if (SDL_GameControllerGetButton(s_gamepad, SDL_CONTROLLER_BUTTON_BACK))       state |= INPUT_BAG;

        /* Left stick as d-pad */
        {
            int16_t lx = SDL_GameControllerGetAxis(s_gamepad, SDL_CONTROLLER_AXIS_LEFTX);
            int16_t ly = SDL_GameControllerGetAxis(s_gamepad, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx < -16000) state |= INPUT_LEFT;
            if (lx >  16000) state |= INPUT_RIGHT;
            if (ly < -16000) state |= INPUT_UP;
            if (ly >  16000) state |= INPUT_DOWN;
        }
    }

    s_input_current = state;

    /* Detect last key pressed (for rebind UI) — scan all key IDs */
    s_last_key_pressed = KEY_ID_NONE;
    {
        static uint8_t prev_keys[KEY_ID_COUNT];
        uint8_t k;
        for (k = 1; k < KEY_ID_COUNT; k++) {
            int sc = s_keyid_to_sdl[k];
            uint8_t down = (sc && keys[sc]) ? 1 : 0;
            if (down && !prev_keys[k]) s_last_key_pressed = k;
            prev_keys[k] = down;
        }
    }

    return state;
}

uint16_t hal_input_pressed(void) {
    return s_input_current & ~s_input_previous;
}

uint16_t hal_input_released(void) {
    return ~s_input_current & s_input_previous;
}

uint8_t hal_input_last_key(void) {
    return s_last_key_pressed;
}

/*--------------------------------------------------------------------------
 * PALETTE
 *--------------------------------------------------------------------------*/

void hal_palette_set(uint8_t palette_id, const uint8_t *colors, uint8_t count) {
    if (palette_id > 1) return;
    memcpy(s_palette_raw[palette_id], colors, count);
    rebuild_palette_rgba(palette_id);
}

void hal_palette_set_color(uint8_t palette_id, uint8_t index, uint8_t rgb8) {
    if (palette_id > 1) return;
    s_palette_raw[palette_id][index] = rgb8;
    s_palette_rgba[palette_id][index] = rgb8_to_argb32(rgb8);
}

/*--------------------------------------------------------------------------
 * TILEMAP
 *--------------------------------------------------------------------------*/

void hal_tiles_load(const uint8_t *data, uint8_t first, uint8_t count) {
    memcpy(&s_tile_patterns[first], data,
           (size_t)count * TILE_SIZE * TILE_SIZE);
}

void hal_tilemap_set(const uint8_t *data, uint16_t map_w, uint16_t map_h) {
    size_t sz = (size_t)map_w * map_h;
    if (sz > sizeof(s_tilemap_buf)) sz = sizeof(s_tilemap_buf);
    memcpy(s_tilemap_buf, data, sz);
    s_tilemap_data = s_tilemap_buf;
    s_tilemap_w = map_w;
    s_tilemap_h = map_h;
    s_scroll_x = 0;
    s_scroll_y = 0;
}

void hal_tilemap_scroll(int16_t scroll_x, int16_t scroll_y) {
    s_scroll_x = scroll_x;
    s_scroll_y = scroll_y;
}

void hal_tilemap_draw(void) {
    int vx, vy, px, py;
    int tile_start_x, tile_start_y;
    int offset_x, offset_y;
    int map_tx, map_ty;
    uint8_t tile_idx;
    const uint8_t *tile;
    int sx, sy;

    if (s_tilemap_data == NULL) return;

    offset_x = s_scroll_x & (TILE_SIZE - 1);
    offset_y = s_scroll_y & (TILE_SIZE - 1);

    tile_start_x = s_scroll_x / TILE_SIZE;
    tile_start_y = s_scroll_y / TILE_SIZE;

    for (vy = -1; vy <= TILES_Y; vy++) {
        for (vx = -1; vx <= TILES_X; vx++) {
            map_tx = tile_start_x + vx;
            map_ty = tile_start_y + vy;

            if (map_tx < 0 || map_tx >= (int)s_tilemap_w ||
                map_ty < 0 || map_ty >= (int)s_tilemap_h) {
                continue;
            }

            tile_idx = s_tilemap_data[map_ty * s_tilemap_w + map_tx];
            tile = s_tile_patterns[tile_idx];

            sx = vx * TILE_SIZE - offset_x;
            sy = vy * TILE_SIZE - offset_y;

            for (py = 0; py < TILE_SIZE; py++) {
                int dy = sy + py;
                if (dy < 0 || dy >= SCREEN_H) continue;

                for (px = 0; px < TILE_SIZE; px++) {
                    int dx = sx + px;
                    if (dx < 0 || dx >= SCREEN_W) continue;

                    uint8_t color = tile[py * TILE_SIZE + px];
                    if (color != 0) {
                        s_pixels[dy * SCREEN_W + dx] = color;
                    }
                }
            }
        }
    }
}

uint8_t hal_tilemap_get(uint16_t tile_x, uint16_t tile_y) {
    if (s_tilemap_data == NULL) return 0;
    if (tile_x >= s_tilemap_w || tile_y >= s_tilemap_h) return 0;
    return s_tilemap_data[tile_y * s_tilemap_w + tile_x];
}

void hal_tilemap_put(uint16_t tile_x, uint16_t tile_y, uint8_t tile_idx) {
    if (s_tilemap_data == NULL) return;
    if (tile_x >= s_tilemap_w || tile_y >= s_tilemap_h) return;
    s_tilemap_data[tile_y * s_tilemap_w + tile_x] = tile_idx;
}

/*--------------------------------------------------------------------------
 * SPRITES
 *--------------------------------------------------------------------------*/

void hal_sprite_patterns_load(const uint8_t *data, uint8_t first, uint8_t count) {
    memcpy(&s_sprite_patterns[first], data,
           (size_t)count * SPRITE_W * SPRITE_H);
}

void hal_sprite_set(const sprite_desc_t *desc) {
    if (desc->id >= MAX_SPRITES) return;
    s_sprites[desc->id] = *desc;
}

void hal_sprite_show(uint8_t id, bool visible) {
    if (id >= MAX_SPRITES) return;
    if (visible) {
        s_sprites[id].flags |= SPRITE_FLAG_VISIBLE;
    } else {
        s_sprites[id].flags &= ~SPRITE_FLAG_VISIBLE;
    }
}

void hal_sprite_hide_all(void) {
    memset(s_sprites, 0, sizeof(s_sprites));
}

void hal_sprites_draw(void) {
    int i, px, py;

    for (i = 0; i < MAX_SPRITES; i++) {
        sprite_desc_t *sp = &s_sprites[i];
        const uint8_t *pattern;
        int src_x, src_y;
        int dx, dy;

        if (!(sp->flags & SPRITE_FLAG_VISIBLE)) continue;

        pattern = s_sprite_patterns[sp->pattern];

        for (py = 0; py < SPRITE_H; py++) {
            for (px = 0; px < SPRITE_W; px++) {
                uint8_t color;

                src_x = (sp->flags & SPRITE_FLAG_MIRROR_X) ? (SPRITE_W - 1 - px) : px;
                src_y = (sp->flags & SPRITE_FLAG_MIRROR_Y) ? (SPRITE_H - 1 - py) : py;

                if (sp->flags & SPRITE_FLAG_ROTATE) {
                    int tmp = src_x;
                    src_x = SPRITE_H - 1 - src_y;
                    src_y = tmp;
                }

                color = pattern[src_y * SPRITE_W + src_x];
                if (color == 0) continue;

                color = (uint8_t)((color + sp->palette * 16) & 0xFF);

                dx = sp->x + px;
                dy = sp->y + py;

                if (dx < 0 || dx >= SCREEN_W || dy < 0 || dy >= SCREEN_H) continue;

                s_pixels[dy * SCREEN_W + dx] = color;
            }
        }
    }
}

/*--------------------------------------------------------------------------
 * DRAWING PRIMITIVES
 *--------------------------------------------------------------------------*/

void hal_draw_rect(int16_t x, int16_t y, uint8_t w, uint8_t h, color_t color) {
    int px, py;
    for (py = y; py < y + h; py++) {
        if (py < 0 || py >= SCREEN_H) continue;
        for (px = x; px < x + w; px++) {
            if (px < 0 || px >= SCREEN_W) continue;
            s_pixels[py * SCREEN_W + px] = color;
        }
    }
}

/* Complete 8x8 bitmap font for ASCII 32-127 (96 glyphs).
   Each glyph is 8 bytes, one per row, MSB = leftmost pixel. */
static const uint8_t s_font[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 space */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 33 ! */
    {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, /* 34 " */
    {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00}, /* 35 # */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* 36 $ */
    {0xC6,0xCC,0x18,0x30,0x60,0xC6,0x86,0x00}, /* 37 % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* 38 & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* 39 ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* 40 ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* 41 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 42 * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* 43 + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* 44 , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 45 - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 46 . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* 47 / */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 48 0 */
    {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00}, /* 49 1 */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* 50 2 */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 51 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 52 4 */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 53 5 */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 54 6 */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 55 7 */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 56 8 */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 57 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* 58 : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* 59 ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 60 < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* 61 = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 62 > */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* 63 ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* 64 @ */
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 65 A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 66 B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 67 C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 68 D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 69 E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 70 F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 71 G */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 72 H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 73 I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 74 J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 75 K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 76 L */
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, /* 77 M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 78 N */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 79 O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 80 P */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0E}, /* 81 Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 82 R */
    {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, /* 83 S */
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 84 T */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 85 U */
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, /* 86 V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* 87 W */
    {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00}, /* 88 X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* 89 Y */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 90 Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* 91 [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 92 \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* 93 ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* 94 ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 95 _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* 96 ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 97 a */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* 98 b */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* 99 c */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /*100 d */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /*101 e */
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00}, /*102 f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /*103 g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /*104 h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /*105 i */
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C}, /*106 j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /*107 k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /*108 l */
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00}, /*109 m */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /*110 n */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /*111 o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /*112 p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /*113 q */
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /*114 r */
    {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00}, /*115 s */
    {0x30,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /*116 t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /*117 u */
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /*118 v */
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00}, /*119 w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /*120 x */
    {0x00,0x00,0xC6,0xC6,0xCE,0x76,0x06,0xFC}, /*121 y */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /*122 z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /*123 { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /*124 | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /*125 } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /*126 ~ */
    {0x00,0x10,0x38,0x6C,0xC6,0xFE,0x00,0x00}, /*127 del */
};

uint8_t hal_draw_char(int16_t x, int16_t y, char ch, color_t color) {
    int row, col;
    const uint8_t *glyph;

    if (ch < 32 || ch > 127) return 8;
    glyph = s_font[ch - 32];

    for (row = 0; row < 8; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;

        for (col = 0; col < 8; col++) {
            if (glyph[row] & (0x80 >> col)) {
                int dx = x + col;
                if (dx >= 0 && dx < SCREEN_W) {
                    s_pixels[dy * SCREEN_W + dx] = color;
                }
            }
        }
    }
    return 8;
}

uint16_t hal_draw_text(int16_t x, int16_t y, const char *str, color_t color) {
    uint16_t advance = 0;
    while (*str) {
        hal_draw_char(x + advance, y, *str, color);
        advance += 8;
        str++;
    }
    return advance;
}

void hal_draw_number(int16_t x, int16_t y, int32_t value, color_t color) {
    char buf[12];
    int i = 0;
    uint32_t v;
    int neg = 0;

    if (value < 0) {
        neg = 1;
        v = (uint32_t)(-(value + 1)) + 1u;
    } else {
        v = (uint32_t)value;
    }

    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v > 0) {
            buf[i++] = '0' + (char)(v % 10);
            v /= 10;
        }
    }
    if (neg) buf[i++] = '-';

    while (i > 0) {
        i--;
        hal_draw_char(x, y, buf[i], color);
        x += 8;
    }
}

/*--------------------------------------------------------------------------
 * SOUND
 *--------------------------------------------------------------------------*/

void hal_sfx_load(sfx_id_t id, const uint8_t *data, uint16_t size) {
    SDL_RWops *rw;
    if (id >= MAX_SFX) return;

    if (s_sfx[id]) {
        Mix_FreeChunk(s_sfx[id]);
    }

    rw = SDL_RWFromConstMem(data, size);
    if (rw) {
        s_sfx[id] = Mix_LoadWAV_RW(rw, 1);
    }
}

void hal_sfx_play(sfx_id_t id, uint8_t channel) {
    if (id >= MAX_SFX || !s_sfx[id]) return;

    if (channel == 0xFF) {
        Mix_PlayChannel(-1, s_sfx[id], 0);
    } else {
        Mix_PlayChannel(channel, s_sfx[id], 0);
    }
}

void hal_sfx_stop(uint8_t channel) {
    Mix_HaltChannel(channel);
}

void hal_music_play(music_id_t id, const uint8_t *data, uint16_t size) {
    SDL_RWops *rw;
    (void)id;

    hal_music_stop();

    rw = SDL_RWFromConstMem(data, size);
    if (rw) {
        s_music = Mix_LoadMUS_RW(rw, 1);
        if (s_music) {
            Mix_PlayMusic(s_music, -1);
        }
    }
}

void hal_music_stop(void) {
    Mix_HaltMusic();
    if (s_music) {
        Mix_FreeMusic(s_music);
        s_music = NULL;
    }
}

void hal_music_volume(uint8_t vol) {
    Mix_VolumeMusic(vol / 2);
}

void hal_music_update(void) {
    /* SDL_mixer handles music in its own thread */
}

/*--------------------------------------------------------------------------
 * FILE I/O
 *--------------------------------------------------------------------------*/

#define MAX_FILE_HANDLES 8
static FILE *s_files[MAX_FILE_HANDLES];

int hal_file_open(const char *path) {
    int i;
    for (i = 0; i < MAX_FILE_HANDLES; i++) {
        if (s_files[i] == NULL) {
            s_files[i] = fopen(path, "rb");
            if (s_files[i]) return i;
            return -1;
        }
    }
    return -1;
}

uint16_t hal_file_read(int handle, void *buffer, uint16_t size) {
    if (handle < 0 || handle >= MAX_FILE_HANDLES || !s_files[handle]) return 0;
    return (uint16_t)fread(buffer, 1, size, s_files[handle]);
}

void hal_file_seek(int handle, uint32_t offset) {
    if (handle < 0 || handle >= MAX_FILE_HANDLES || !s_files[handle]) return;
    fseek(s_files[handle], (long)offset, SEEK_SET);
}

void hal_file_close(int handle) {
    if (handle < 0 || handle >= MAX_FILE_HANDLES || !s_files[handle]) return;
    fclose(s_files[handle]);
    s_files[handle] = NULL;
}

/*--------------------------------------------------------------------------
 * MEMORY HELPERS
 *--------------------------------------------------------------------------*/

void hal_memcpy(void *dst, const void *src, uint16_t size) {
    memcpy(dst, src, size);
}

void hal_memset(void *dst, uint8_t val, uint16_t size) {
    memset(dst, val, size);
}

/*--------------------------------------------------------------------------
 * DEBUG
 *--------------------------------------------------------------------------*/

#ifdef DEBUG
void hal_debug_print(const char *msg) {
    fprintf(stdout, "[DEBUG] %s\n", msg);
    fflush(stdout);
}

void hal_debug_number(const char *label, int32_t value) {
    fprintf(stdout, "[DEBUG] %s: %d\n", label, (int)value);
    fflush(stdout);
}
#endif

#endif /* !__Z88DK */
