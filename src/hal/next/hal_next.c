/*============================================================================
 * hal_next.c - Spectrum Next HAL Implementation
 *
 * Targets the ZX Spectrum Next via z88dk (zsdcc backend).
 * Uses Next hardware features:
 *   - Hardware tilemap (port 0x6B, 0x6C, 0x6E, 0x6F)
 *   - Hardware sprites (port 0x303B, 0x57)
 *   - AY-3-8910 sound chip
 *   - esxDOS for SD card file I/O
 *   - DMA for fast memory transfers
 *
 * Compile with:
 *   zcc +zxn -vn -SO3 -startup=0 -clib=sdcc_iy
 *       -pragma-include:zpragma.inc hal_next.c -o game -create-app
 *============================================================================*/

#ifdef __Z88DK

#include "../hal.h"
#include <z80.h>
#include <arch/zxn.h>
#include <arch/zxn/esxdos.h>
#include <intrinsic.h>
#include <string.h>

/*--------------------------------------------------------------------------
 * Next hardware register / port constants
 *--------------------------------------------------------------------------*/

/* Sprite ports */
#define SPRITE_SLOT_SEL    0x303Bu
#define SPRITE_ATTR        0x0057u
#define SPRITE_PATTERN     0x005Bu

/* Tilemap Next registers (accent via TBBLUE) */
#define REG_TILEMAP_CTRL       0x6B
#define REG_TILEMAP_DEFAULT    0x6C
#define REG_TILEMAP_BASE       0x6E
#define REG_TILEMAP_TILE_BASE  0x6F
#define REG_TILEMAP_OFFSET_X   0x2F
#define REG_TILEMAP_OFFSET_Y   0x31

/* Palette registers */
#define REG_PALETTE_CTRL   0x43
#define REG_PALETTE_INDEX  0x40
#define REG_PALETTE_VALUE  0x41

/* General */
#define REG_SPRITE_SYSTEM  0x15
#define REG_CLIP_SPRITE    0x19
#define REG_CLIP_TILEMAP   0x1B

/*--------------------------------------------------------------------------
 * Internal state
 *--------------------------------------------------------------------------*/

static uint16_t s_frame_counter;
static uint16_t s_input_current;
static uint16_t s_input_previous;

/* Shadow sprite table — we write here, then flush to hardware */
static sprite_desc_t s_sprites[MAX_SPRITES];

/* Tilemap state */
static uint8_t s_tilemap_buf[128 * 64];
static uint8_t *s_tilemap_data;
static uint16_t s_tilemap_w;
static uint16_t s_tilemap_h;
static int16_t  s_scroll_x;
static int16_t  s_scroll_y;

/*--------------------------------------------------------------------------
 * Z80 inline helpers
 *--------------------------------------------------------------------------*/

/* Write a value to a TBBLUE (Next) register */
static void nextreg(uint8_t reg, uint8_t val) __z88dk_fastcall {
    ZXN_NEXTREG(reg, val);
}

/* Read a TBBLUE register */
static uint8_t nextreg_read(uint8_t reg) __z88dk_fastcall {
    return ZXN_READ_REG(reg);
}

/*--------------------------------------------------------------------------
 * SYSTEM LIFECYCLE
 *--------------------------------------------------------------------------*/

int hal_init(void) {
    /* Disable interrupts during setup */
    intrinsic_di();

    /* Set turbo mode: 28 MHz */
    nextreg(0x07, 0x03);

    /* Enable sprites: sprites visible, sprites over border off */
    nextreg(REG_SPRITE_SYSTEM, 0x01);

    /* Enable tilemap: 40x32, 8-bit entries, tilemap over ULA */
    nextreg(REG_TILEMAP_CTRL, 0xA0);

    /* Tilemap base address in bank 5 (default 0x6C00 -> bits) */
    nextreg(REG_TILEMAP_BASE, 0x6C);

    /* Tile definitions base */
    nextreg(REG_TILEMAP_TILE_BASE, 0x0C);

    /* Clear all sprites */
    hal_sprite_hide_all();

    s_frame_counter = 0;
    s_input_current = 0;
    s_input_previous = 0;

    s_tilemap_data = NULL;
    s_tilemap_w = 0;
    s_tilemap_h = 0;
    s_scroll_x = 0;
    s_scroll_y = 0;

    /* Initialize keyboard config to default layout 1 */
    hal_keys_set_layout(KEY_LAYOUT_1);

    /* Re-enable interrupts */
    intrinsic_ei();

    return 0;
}

void hal_shutdown(void) {
    hal_sprite_hide_all();
    hal_music_stop();

    /* Restore normal CPU speed */
    nextreg(0x07, 0x00);
}

/*--------------------------------------------------------------------------
 * FRAME TIMING
 *--------------------------------------------------------------------------*/

void hal_frame_begin(void) {
    /* Wait for vertical blanking interval (HALT waits for interrupt) */
    intrinsic_halt();
    s_frame_counter++;
}

void hal_frame_end(void) {
    /* Flush sprite attributes to hardware */
    hal_sprites_draw();
}

uint32_t hal_frame_count(void) {
    return (uint32_t)s_frame_counter;
}

/*--------------------------------------------------------------------------
 * INPUT — Rebindable keyboard system
 *
 * The active config stores key_id_t IDs (platform-portable).
 * A resolved array maps each bind to a Z80 keyboard matrix port+mask.
 * Save/load uses esxDOS and the compact binary .cfg format.
 *
 * Kempston joystick is always active (layout-independent).
 *
 * ZX Spectrum keyboard matrix reference:
 *   Port    Bit4  Bit3  Bit2  Bit1  Bit0
 *   0xFEFE:  V     C     X     Z   Shift
 *   0xFDFE:  G     F     D     S     A
 *   0xFBFE:  T     R     E     W     Q
 *   0xF7FE:  5     4     3     2     1
 *   0xEFFE:  6     7     8     9     0
 *   0xDFFE:  Y     U     I     O     P
 *   0xBFFE:  H     J     K     L   Enter
 *   0x7FFE:  B     N     M   Sym   Space
 *--------------------------------------------------------------------------*/

/*--- key_id_t → Z80 matrix port+mask lookup ---
 *
 * Array index = key_id_t enum value.
 * Must match the enum order in hal_keybinds.h exactly.
 * {0,0} means the key has no Z80 matrix representation.
 *---*/

typedef struct {
    uint16_t port;
    uint8_t  mask;
} next_key_hw_t;

static const next_key_hw_t s_keyid_to_hw[KEY_ID_COUNT] = {
    /* KEY_ID_NONE = 0 */  { 0, 0 },
    /* A-Z (1..26) — Spectrum keyboard matrix */
    /* A */ { 0xFDFE, 0x01 },  /* B */ { 0x7FFE, 0x10 },
    /* C */ { 0xFEFE, 0x08 },  /* D */ { 0xFDFE, 0x04 },
    /* E */ { 0xFBFE, 0x04 },  /* F */ { 0xFDFE, 0x08 },
    /* G */ { 0xFDFE, 0x10 },  /* H */ { 0xBFFE, 0x10 },
    /* I */ { 0xDFFE, 0x04 },  /* J */ { 0xBFFE, 0x08 },
    /* K */ { 0xBFFE, 0x04 },  /* L */ { 0xBFFE, 0x02 },
    /* M */ { 0x7FFE, 0x04 },  /* N */ { 0x7FFE, 0x08 },
    /* O */ { 0xDFFE, 0x02 },  /* P */ { 0xDFFE, 0x01 },
    /* Q */ { 0xFBFE, 0x01 },  /* R */ { 0xFBFE, 0x08 },
    /* S */ { 0xFDFE, 0x02 },  /* T */ { 0xFBFE, 0x10 },
    /* U */ { 0xDFFE, 0x08 },  /* V */ { 0xFEFE, 0x10 },
    /* W */ { 0xFBFE, 0x02 },  /* X */ { 0xFEFE, 0x04 },
    /* Y */ { 0xDFFE, 0x10 },  /* Z */ { 0xFEFE, 0x02 },
    /* 0-9 (27..36) */
    /* 0 */ { 0xEFFE, 0x01 },  /* 1 */ { 0xF7FE, 0x01 },
    /* 2 */ { 0xF7FE, 0x02 },  /* 3 */ { 0xF7FE, 0x04 },
    /* 4 */ { 0xF7FE, 0x08 },  /* 5 */ { 0xF7FE, 0x10 },
    /* 6 */ { 0xEFFE, 0x10 },  /* 7 */ { 0xEFFE, 0x08 },
    /* 8 */ { 0xEFFE, 0x04 },  /* 9 */ { 0xEFFE, 0x02 },
    /* Special keys (37..42) — must match enum: ENTER,SPACE,SHIFT,BACKSPACE,TAB,ESCAPE */
    /* ENTER     = 37 */ { 0xBFFE, 0x01 },
    /* SPACE     = 38 */ { 0x7FFE, 0x01 },
    /* SHIFT     = 39 */ { 0xFEFE, 0x01 },
    /* BACKSPACE = 40 */ { 0, 0 },  /* no dedicated key on Spectrum */
    /* TAB       = 41 */ { 0, 0 },  /* no TAB on Spectrum           */
    /* ESCAPE    = 42 */ { 0, 0 },  /* no ESC on Spectrum           */
    /* Arrows (43..46) — no arrow keys in Z80 matrix */
    /* UP    = 43 */ { 0, 0 },
    /* DOWN  = 44 */ { 0, 0 },
    /* LEFT  = 45 */ { 0, 0 },
    /* RIGHT = 46 */ { 0, 0 },
    /* Punctuation (47..50) — require Symbol Shift combos, not mapped */
    /* COMMA     = 47 */ { 0, 0 },
    /* PERIOD    = 48 */ { 0, 0 },
    /* SLASH     = 49 */ { 0, 0 },
    /* SEMICOLON = 50 */ { 0, 0 },
};

/*--- Mutable active config (keyname IDs) ---*/
static keybind_config_t s_active_config;
static uint8_t      s_active_layout_id = KEY_LAYOUT_1;

/*--- Resolved native codes for fast polling ---*/
typedef struct {
    uint16_t port;
    uint8_t  mask;
    uint16_t flag;
} resolved_next_bind_t;

static resolved_next_bind_t s_resolved[BIND_COUNT];

/*--- Resolve all keyname IDs to Z80 matrix port+mask ---*/

/* Alt key resolved in separate arrays to keep struct size small for Z80 */
static uint16_t s_resolved_alt_port[BIND_COUNT];
static uint8_t  s_resolved_alt_mask[BIND_COUNT];

static void resolve_config_full(void) {
    uint8_t i;
    for (i = 0; i < BIND_COUNT; i++) {
        uint8_t pk = s_active_config.primary[i];
        uint8_t ak = s_active_config.alt[i];
        s_resolved[i].port = (pk < KEY_ID_COUNT) ? s_keyid_to_hw[pk].port : 0;
        s_resolved[i].mask = (pk < KEY_ID_COUNT) ? s_keyid_to_hw[pk].mask : 0;
        s_resolved[i].flag = bind_to_flag[i];
        s_resolved_alt_port[i] = (ak < KEY_ID_COUNT) ? s_keyid_to_hw[ak].port : 0;
        s_resolved_alt_mask[i] = (ak < KEY_ID_COUNT) ? s_keyid_to_hw[ak].mask : 0;
    }
}

/* Read a keyboard half-row. Returns bits where 0 = pressed. */
static uint8_t key_row(uint16_t addr) {
    return z80_inp(addr);
}

/*--- Layout management ---*/

void hal_keys_set_layout(uint8_t layout_id) {
    if (layout_id >= KEY_LAYOUT_COUNT) return;
    s_active_config = *keybind_defaults[layout_id];
    s_active_layout_id = layout_id;
    resolve_config_full();
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
    resolve_config_full();
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
    uint8_t i;
    for (i = 0; i < KEYBIND_NAME_LEN - 1 && name[i]; i++) {
        s_active_config.name[i] = name[i];
    }
    s_active_config.name[i] = '\0';
}

/*--- Save/load .cfg files via esxDOS ---*/

int hal_keys_save_config(const char *path) {
    uint8_t handle;
    uint8_t buf[KEYBIND_FILE_SIZE];
    uint8_t i;

    /* Build the file buffer */
    buf[0] = 'K';
    buf[1] = 'C';
    buf[2] = 'F';
    buf[3] = 'G';
    buf[4] = KEYBIND_VERSION;

    /* Name (32 bytes, null-padded) */
    for (i = 0; i < KEYBIND_NAME_LEN; i++) {
        buf[5 + i] = (uint8_t)s_active_config.name[i];
    }

    /* Primary key_ids */
    for (i = 0; i < BIND_COUNT; i++) {
        buf[5 + KEYBIND_NAME_LEN + i] = s_active_config.primary[i];
    }

    /* Alt key_ids */
    for (i = 0; i < BIND_COUNT; i++) {
        buf[5 + KEYBIND_NAME_LEN + BIND_COUNT + i] = s_active_config.alt[i];
    }

    handle = esx_f_open(path, ESX_MODE_WRITE | ESX_MODE_OPEN_CREAT_TRUNC);
    if (handle == 0xFF) return -1;

    if (esx_f_write(handle, buf, KEYBIND_FILE_SIZE) != KEYBIND_FILE_SIZE) {
        esx_f_close(handle);
        return -1;
    }

    esx_f_close(handle);
    return 0;
}

int hal_keys_load_config(const char *path) {
    uint8_t handle;
    uint8_t buf[KEYBIND_FILE_SIZE];
    uint8_t i;

    handle = esx_f_open(path, ESX_MODE_READ | ESX_MODE_OPEN_EXIST);
    if (handle == 0xFF) return -1;

    if (esx_f_read(handle, buf, KEYBIND_FILE_SIZE) < KEYBIND_FILE_SIZE) {
        esx_f_close(handle);
        return -1;
    }

    esx_f_close(handle);

    /* Validate magic */
    if (buf[0] != 'K' || buf[1] != 'C' ||
        buf[2] != 'F' || buf[3] != 'G') {
        return -1;
    }

    if (buf[4] != KEYBIND_VERSION) return -1;

    /* Name (32 bytes at offset 5) */
    for (i = 0; i < KEYBIND_NAME_LEN; i++) {
        s_active_config.name[i] = (char)buf[5 + i];
    }
    s_active_config.name[KEYBIND_NAME_LEN - 1] = '\0';

    /* Primary key_ids (BIND_COUNT bytes at offset 37) */
    for (i = 0; i < BIND_COUNT; i++) {
        uint8_t pk = buf[5 + KEYBIND_NAME_LEN + i];
        if (pk >= KEY_ID_COUNT) pk = KEY_ID_NONE;
        s_active_config.primary[i] = pk;
    }

    /* Alt key_ids (BIND_COUNT bytes at offset 49) */
    for (i = 0; i < BIND_COUNT; i++) {
        uint8_t ak = buf[5 + KEYBIND_NAME_LEN + BIND_COUNT + i];
        if (ak >= KEY_ID_COUNT) ak = KEY_ID_NONE;
        s_active_config.alt[i] = ak;
    }

    s_active_layout_id = KEY_LAYOUT_CUSTOM;
    resolve_config_full();
    return 0;
}

/*--- Input polling ---*/

uint16_t hal_input_poll(void) {
    uint16_t state = 0;
    uint8_t row;
    uint8_t i;

    s_input_previous = s_input_current;

    /* Scan all resolved bindings (primary + alt) */
    for (i = 0; i < BIND_COUNT; i++) {
        /* Primary key */
        if (s_resolved[i].port != 0) {
            row = key_row(s_resolved[i].port);
            if (!(row & s_resolved[i].mask)) {
                state |= s_resolved[i].flag;
            }
        }
        /* Alternate key */
        if (s_resolved_alt_port[i] != 0) {
            row = key_row(s_resolved_alt_port[i]);
            if (!(row & s_resolved_alt_mask[i])) {
                state |= s_resolved[i].flag;
            }
        }
    }

    /*
     * Kempston joystick (port 0x1F) — active high, layout-independent
     * Maps to movement + fire = Btn1
     */
    row = z80_inp(0x001F);
    if (row & 0x08) state |= INPUT_UP;
    if (row & 0x04) state |= INPUT_DOWN;
    if (row & 0x02) state |= INPUT_LEFT;
    if (row & 0x01) state |= INPUT_RIGHT;
    if (row & 0x10) state |= INPUT_BTN1;

    s_input_current = state;
    return state;
}

uint16_t hal_input_pressed(void) {
    return s_input_current & ~s_input_previous;
}

uint16_t hal_input_released(void) {
    return ~s_input_current & s_input_previous;
}

/*--------------------------------------------------------------------------
 * PALETTE
 *--------------------------------------------------------------------------*/

void hal_palette_set(uint8_t palette_id, const uint8_t *colors, uint8_t count) {
    uint8_t i;
    nextreg(REG_PALETTE_CTRL, (palette_id == 1) ? 0x10 : 0x00);
    nextreg(REG_PALETTE_INDEX, 0);
    for (i = 0; i < count; i++) {
        nextreg(REG_PALETTE_VALUE, colors[i]);
    }
}

void hal_palette_set_color(uint8_t palette_id, uint8_t index, uint8_t rgb8) {
    nextreg(REG_PALETTE_CTRL, (palette_id == 1) ? 0x10 : 0x00);
    nextreg(REG_PALETTE_INDEX, index);
    nextreg(REG_PALETTE_VALUE, rgb8);
}

/*--------------------------------------------------------------------------
 * TILEMAP
 *--------------------------------------------------------------------------*/

#define TILEMAP_MEM_BASE  0x6C00u

void hal_tiles_load(const uint8_t *data, uint8_t first, uint8_t count) {
    uint16_t offset = (uint16_t)first * 256u;
    uint16_t size   = (uint16_t)count * 256u;
    uint8_t *dst    = (uint8_t *)(0x4C00u + offset);
    hal_memcpy(dst, data, size);
}

void hal_tilemap_set(const uint8_t *data, uint16_t map_w, uint16_t map_h) {
    uint16_t sz = map_w * map_h;
    if (sz > sizeof(s_tilemap_buf)) sz = sizeof(s_tilemap_buf);
    hal_memcpy(s_tilemap_buf, data, sz);
    s_tilemap_data = s_tilemap_buf;
    s_tilemap_w = map_w;
    s_tilemap_h = map_h;
    s_scroll_x = 0;
    s_scroll_y = 0;
}

void hal_tilemap_set_bg(const uint8_t *data, uint16_t w, uint16_t h, uint8_t repeat) {
    (void)data; (void)w; (void)h; (void)repeat; /* not implemented on Next */
}
void hal_tilemap_clear_bg(void) { /* not implemented on Next */ }

void hal_tilemap_scroll(int16_t scroll_x, int16_t scroll_y) {
    s_scroll_x = scroll_x;
    s_scroll_y = scroll_y;
}

void hal_tilemap_draw(void) {
    uint8_t *dst;
    uint16_t src_tx, src_ty;
    uint8_t vx, vy;
    uint16_t map_tx, map_ty;

    if (s_tilemap_data == NULL) return;

    nextreg(REG_TILEMAP_OFFSET_X, (uint8_t)(s_scroll_x & 0x0F));
    nextreg(REG_TILEMAP_OFFSET_Y, (uint8_t)(s_scroll_y & 0x0F));

    src_tx = (uint16_t)(s_scroll_x >> 4);
    src_ty = (uint16_t)(s_scroll_y >> 4);

    dst = (uint8_t *)TILEMAP_MEM_BASE;

    for (vy = 0; vy < 32; vy++) {
        map_ty = src_ty + vy;
        if (map_ty >= s_tilemap_h) {
            hal_memset(dst, 0, 40);
        } else {
            for (vx = 0; vx < 40; vx++) {
                map_tx = src_tx + vx;
                if (map_tx < s_tilemap_w) {
                    dst[vx] = s_tilemap_data[map_ty * s_tilemap_w + map_tx];
                } else {
                    dst[vx] = 0;
                }
            }
        }
        dst += 40;
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
    uint16_t i;
    uint16_t total_bytes = (uint16_t)count * 256u;
    IO_SPRITE_SLOT = first;
    for (i = 0; i < total_bytes; i++) {
        IO_SPRITE_PATTERN = data[i];
    }
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
    uint8_t i;
    IO_SPRITE_SLOT = 0;
    for (i = 0; i < 128; i++) {
        IO_SPRITE_ATTR = 0;
        IO_SPRITE_ATTR = 0;
        IO_SPRITE_ATTR = 0;
        IO_SPRITE_ATTR = 0;
    }
    hal_memset(s_sprites, 0, sizeof(s_sprites));
}

void hal_sprites_draw(void) {
    uint8_t i;
    sprite_desc_t *sp;
    uint8_t attr2, attr3;

    IO_SPRITE_SLOT = 0;

    for (i = 0; i < MAX_SPRITES; i++) {
        sp = &s_sprites[i];

        if (!(sp->flags & SPRITE_FLAG_VISIBLE)) {
            IO_SPRITE_ATTR = 0;
            IO_SPRITE_ATTR = 0;
            IO_SPRITE_ATTR = 0;
            IO_SPRITE_ATTR = 0;
            continue;
        }

        IO_SPRITE_ATTR = (uint8_t)(sp->x & 0xFF);
        IO_SPRITE_ATTR = (uint8_t)(sp->y & 0xFF);

        attr2 = (sp->palette & 0x0F) << 4;
        if (sp->flags & SPRITE_FLAG_MIRROR_X) attr2 |= 0x08;
        if (sp->flags & SPRITE_FLAG_MIRROR_Y) attr2 |= 0x04;
        if (sp->flags & SPRITE_FLAG_ROTATE)   attr2 |= 0x02;
        if (sp->x & 0x100)                    attr2 |= 0x01;
        IO_SPRITE_ATTR = attr2;

        attr3 = 0x80 | (sp->pattern & 0x3F);
        IO_SPRITE_ATTR = attr3;
    }
}

/*--------------------------------------------------------------------------
 * DRAWING PRIMITIVES
 *--------------------------------------------------------------------------*/

void hal_draw_rect(int16_t x, int16_t y, uint8_t w, uint8_t h, color_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

uint8_t hal_draw_char(int16_t x, int16_t y, char ch, color_t color) {
    (void)x; (void)y; (void)ch; (void)color;
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

#define AY_REG_SELECT  0xFFFD
#define AY_REG_WRITE   0xBFFD

static const uint8_t *s_sfx_data[16];
static uint16_t       s_sfx_size[16];
static const uint8_t *s_music_data;
static uint16_t       s_music_size;
static uint16_t       s_music_pos;
static uint8_t        s_music_vol;

void hal_sfx_load(sfx_id_t id, const uint8_t *data, uint16_t size) {
    if (id < 16) {
        s_sfx_data[id] = data;
        s_sfx_size[id] = size;
    }
}

void hal_sfx_play(sfx_id_t id, uint8_t channel) {
    (void)id; (void)channel;
}

void hal_sfx_stop(uint8_t channel) {
    uint8_t vol_reg = 8 + channel;
    z80_outp(AY_REG_SELECT, vol_reg);
    z80_outp(AY_REG_WRITE, 0);
}

void hal_music_play(music_id_t id, const uint8_t *data, uint16_t size) {
    (void)id;
    s_music_data = data;
    s_music_size = size;
    s_music_pos  = 0;
}

void hal_music_stop(void) {
    uint8_t i;
    s_music_data = NULL;
    for (i = 0; i < 3; i++) {
        hal_sfx_stop(i);
    }
}

void hal_music_volume(uint8_t vol) {
    s_music_vol = vol;
}

void hal_music_update(void) {
    if (s_music_data == NULL) return;
}

/*--------------------------------------------------------------------------
 * FILE I/O  (esxDOS)
 *--------------------------------------------------------------------------*/

int hal_file_open(const char *path) {
    return (int)esx_f_open(path, ESX_MODE_READ | ESX_MODE_OPEN_EXIST);
}

uint16_t hal_file_read(int handle, void *buffer, uint16_t size) {
    return esx_f_read((uint8_t)handle, buffer, size);
}

void hal_file_seek(int handle, uint32_t offset) {
    esx_f_seek((uint8_t)handle, offset, ESX_SEEK_SET);
}

void hal_file_close(int handle) {
    esx_f_close((uint8_t)handle);
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
    while (*msg) {
        z80_outp(0x0001, *msg);
        msg++;
    }
    z80_outp(0x0001, '\n');
}

void hal_debug_number(const char *label, int32_t value) {
    char buf[12];
    int i = 11;
    uint32_t v;
    hal_debug_print(label);
    buf[i] = 0;
    if (value < 0) {
        z80_outp(0x0001, '-');
        v = (uint32_t)(-(value + 1)) + 1u;
    } else {
        v = (uint32_t)value;
    }
    if (v == 0) { buf[--i] = '0'; }
    while (v > 0) {
        buf[--i] = '0' + (char)(v % 10);
        v /= 10;
    }
    hal_debug_print(&buf[i]);
}
#endif

#endif /* __Z88DK */
