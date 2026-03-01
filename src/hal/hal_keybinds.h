/*============================================================================
 * hal_keybinds.h - Keyboard Layout & Rebinding System
 *
 * Supports 4 built-in default layouts PLUS user-defined configs that
 * can be saved/loaded from .cfg files.
 *
 * Architecture:
 *   - key_id_t: a portable enum of physical keys. Platform backends
 *     map these to native codes (SDL scancodes / Z80 matrix ports).
 *   - keybind_config_t: holds the full set of bindings (12 buttons,
 *     each with primary + alt key). There is exactly ONE mutable
 *     active config at runtime; it's what hal_input_poll() uses.
 *   - The 4 built-in layouts (defaults 0-3) are const tables of
 *     key_id_t values. hal_keys_set_layout(id) copies one into
 *     the active config.
 *   - hal_keys_rebind() modifies a single binding at runtime.
 *   - hal_keys_save_config() / hal_keys_load_config() persist the
 *     active config to/from a binary .cfg file.
 *
 * .cfg file format (61 bytes):
 *   Offset  Size  Description
 *   0       4     Magic "KCFG"
 *   4       1     Version (currently 1)
 *   5       32    Config name (null-padded)
 *   37      12    Primary key_id for each of BIND_COUNT bindings
 *   49      12    Alt key_id for each of BIND_COUNT bindings
 *   ----    --    ----
 *   Total:  61    bytes
 *
 * Built-in layouts:
 *   Layout 1: EDSF move (alt arrows), JKLUIO actions
 *   Layout 2: Arrow move (alt IKJL), ZXC/ASD actions
 *   Layout 3: placeholder (= Layout 1)
 *   Layout 4: placeholder (= Layout 1)
 *============================================================================*/

#ifndef HAL_KEYBINDS_H
#define HAL_KEYBINDS_H

#include "hal_types.h"

/*==========================================================================
 * Portable key identifiers
 *
 * Every physical key the engine can bind is represented here.
 * Platform backends maintain a mapping table:
 *   key_id_t -> SDL_Scancode  (Windows)
 *   key_id_t -> {port, mask}  (ZX Spectrum Next)
 *
 * KEY_ID_NONE (0) means "no key assigned".
 *==========================================================================*/

typedef enum {
    KEY_ID_NONE = 0,

    /* Letters */
    KEY_ID_A,  KEY_ID_B,  KEY_ID_C,  KEY_ID_D,  KEY_ID_E,
    KEY_ID_F,  KEY_ID_G,  KEY_ID_H,  KEY_ID_I,  KEY_ID_J,
    KEY_ID_K,  KEY_ID_L,  KEY_ID_M,  KEY_ID_N,  KEY_ID_O,
    KEY_ID_P,  KEY_ID_Q,  KEY_ID_R,  KEY_ID_S,  KEY_ID_T,
    KEY_ID_U,  KEY_ID_V,  KEY_ID_W,  KEY_ID_X,  KEY_ID_Y,
    KEY_ID_Z,

    /* Digits */
    KEY_ID_0,  KEY_ID_1,  KEY_ID_2,  KEY_ID_3,  KEY_ID_4,
    KEY_ID_5,  KEY_ID_6,  KEY_ID_7,  KEY_ID_8,  KEY_ID_9,

    /* Special keys */
    KEY_ID_ENTER,
    KEY_ID_SPACE,
    KEY_ID_SHIFT,
    KEY_ID_BACKSPACE,
    KEY_ID_TAB,
    KEY_ID_ESCAPE,

    /* Arrow keys (Next: extended keyboard only) */
    KEY_ID_UP,
    KEY_ID_DOWN,
    KEY_ID_LEFT,
    KEY_ID_RIGHT,

    /* Punctuation available on Spectrum (via symbol shift combos)
       and natively on PC */
    KEY_ID_COMMA,       /* , */
    KEY_ID_PERIOD,      /* . */
    KEY_ID_SLASH,       /* / */
    KEY_ID_SEMICOLON,   /* ; */

    KEY_ID_COUNT
} key_id_t;

/*--------------------------------------------------------------------------
 * Human-readable key names (for UI / config display)
 *--------------------------------------------------------------------------*/

static const char * const key_id_names[KEY_ID_COUNT] = {
    "---",
    "A","B","C","D","E","F","G","H","I","J",
    "K","L","M","N","O","P","Q","R","S","T",
    "U","V","W","X","Y","Z",
    "0","1","2","3","4","5","6","7","8","9",
    "Enter","Space","Shift","BkSpc","Tab","Esc",
    "Up","Down","Left","Right",
    ",",".","/",";",
};

/*==========================================================================
 * Layout slot IDs
 *==========================================================================*/

#define KEY_LAYOUT_1      0    /* EDSF + JKLUIO (default)             */
#define KEY_LAYOUT_2      1    /* Arrows + ZXC/ASD (alt: IKJL move)   */
#define KEY_LAYOUT_3      2    /* placeholder                         */
#define KEY_LAYOUT_4      3    /* placeholder                         */
#define KEY_LAYOUT_COUNT  4
#define KEY_LAYOUT_CUSTOM 4    /* returned by hal_keys_get_layout()
                                  if the active config was loaded from
                                  file or manually rebound             */

/*==========================================================================
 * Logical button indices (bind slots)
 *==========================================================================*/

#define BIND_UP      0
#define BIND_DOWN    1
#define BIND_LEFT    2
#define BIND_RIGHT   3
#define BIND_BTN1    4
#define BIND_BTN2    5
#define BIND_BTN3    6
#define BIND_BTN4    7
#define BIND_BTN5    8
#define BIND_BTN6    9
#define BIND_MENU   10
#define BIND_BAG    11
#define BIND_COUNT  12

/* Convert a bind index to its INPUT_* flag */
static const uint16_t bind_to_flag[BIND_COUNT] = {
    INPUT_UP, INPUT_DOWN, INPUT_LEFT, INPUT_RIGHT,
    INPUT_BTN1, INPUT_BTN2, INPUT_BTN3,
    INPUT_BTN4, INPUT_BTN5, INPUT_BTN6,
    INPUT_MENU, INPUT_BAG
};

/* Human-readable button names (for rebind UI) */
static const char * const bind_names[BIND_COUNT] = {
    "Move Up", "Move Down", "Move Left", "Move Right",
    "Action 1 (J)", "Action 2 (K)", "Action 3 (L)",
    "Action 4 (U)", "Action 5 (I)", "Action 6 (O)",
    "Menu", "Bag",
};

/*==========================================================================
 * Keybind configuration
 *
 * The runtime uses exactly one of these as the "active config".
 * It can be populated from a built-in default, loaded from a .cfg
 * file, or modified per-binding at runtime.
 *==========================================================================*/

#define KEYBIND_NAME_LEN  32   /* max config name including null */

typedef struct {
    char     name[KEYBIND_NAME_LEN];   /* human-readable name           */
    uint8_t  primary[BIND_COUNT];      /* key_id_t for primary key      */
    uint8_t  alt[BIND_COUNT];          /* key_id_t for alt key (0=none) */
} keybind_config_t;

/*==========================================================================
 * Built-in default layouts (key_id values)
 *
 * These are const. hal_keys_set_layout(id) copies one into the
 * mutable active config.
 *==========================================================================*/

/* Layout 1: EDSF + JKLUIO, arrows as alt movement */
static const keybind_config_t KEYBIND_DEFAULT_1 = {
    "EDSF+JKLUIO",
    /*          UP      DOWN    LEFT    RIGHT   B1      B2      B3      B4      B5      B6      MENU        BAG     */
    { KEY_ID_E, KEY_ID_D, KEY_ID_S, KEY_ID_F, KEY_ID_J, KEY_ID_K, KEY_ID_L, KEY_ID_U, KEY_ID_I, KEY_ID_O, KEY_ID_ENTER, KEY_ID_B },
    { KEY_ID_UP, KEY_ID_DOWN, KEY_ID_LEFT, KEY_ID_RIGHT, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* Layout 2: Arrows + ZXC/ASD, IKJL as alt movement */
static const keybind_config_t KEYBIND_DEFAULT_2 = {
    "Arrows+ZXC/ASD",
    { KEY_ID_UP, KEY_ID_DOWN, KEY_ID_LEFT, KEY_ID_RIGHT, KEY_ID_Z, KEY_ID_X, KEY_ID_C, KEY_ID_A, KEY_ID_S, KEY_ID_D, KEY_ID_ENTER, KEY_ID_B },
    { KEY_ID_I, KEY_ID_K, KEY_ID_J, KEY_ID_L, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* Layout 3: placeholder (= Layout 1) */
static const keybind_config_t KEYBIND_DEFAULT_3 = {
    "Custom 3",
    { KEY_ID_E, KEY_ID_D, KEY_ID_S, KEY_ID_F, KEY_ID_J, KEY_ID_K, KEY_ID_L, KEY_ID_U, KEY_ID_I, KEY_ID_O, KEY_ID_ENTER, KEY_ID_B },
    { KEY_ID_UP, KEY_ID_DOWN, KEY_ID_LEFT, KEY_ID_RIGHT, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* Layout 4: placeholder (= Layout 1) */
static const keybind_config_t KEYBIND_DEFAULT_4 = {
    "Custom 4",
    { KEY_ID_E, KEY_ID_D, KEY_ID_S, KEY_ID_F, KEY_ID_J, KEY_ID_K, KEY_ID_L, KEY_ID_U, KEY_ID_I, KEY_ID_O, KEY_ID_ENTER, KEY_ID_B },
    { KEY_ID_UP, KEY_ID_DOWN, KEY_ID_LEFT, KEY_ID_RIGHT, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* Array of default config pointers for indexed access */
static const keybind_config_t * const keybind_defaults[KEY_LAYOUT_COUNT] = {
    &KEYBIND_DEFAULT_1,
    &KEYBIND_DEFAULT_2,
    &KEYBIND_DEFAULT_3,
    &KEYBIND_DEFAULT_4,
};

/*==========================================================================
 * .cfg file format constants
 *==========================================================================*/

#define KEYBIND_FILE_EXT     ".cfg"
#define KEYBIND_MAGIC        "KCFG"
#define KEYBIND_MAGIC_LEN    4
#define KEYBIND_VERSION      1
#define KEYBIND_FILE_SIZE    (KEYBIND_MAGIC_LEN + 1 + KEYBIND_NAME_LEN + BIND_COUNT + BIND_COUNT)
                             /* 4 + 1 + 32 + 12 + 12 = 61 bytes */

/*==========================================================================
 * Rebinding & config API
 *
 * Implemented per platform in hal_win64.c / hal_next.c.
 *==========================================================================*/

/* --- Querying --- */

/* Get a read-only pointer to the current active config. */
const keybind_config_t *hal_keys_get_config(void);

/* Get the key_id bound to a specific slot.
   which: 0 = primary, 1 = alt. */
uint8_t hal_keys_get_bind(uint8_t bind_index, uint8_t which);

/* --- Modifying at runtime --- */

/* Change a single binding.  Marks the config as "custom".
   bind_index: BIND_UP .. BIND_BAG.
   primary:    key_id_t for primary key.
   alt:        key_id_t for alt key (KEY_ID_NONE = none). */
void hal_keys_rebind(uint8_t bind_index, uint8_t primary, uint8_t alt);

/* Set the config name (for display / file saving). */
void hal_keys_set_name(const char *name);

/* Reset the active config to one of the 4 built-in defaults.
   Same as hal_keys_set_layout() but expresses intent more clearly. */
void hal_keys_reset_defaults(uint8_t layout_id);

/* --- File I/O --- */

/* Save the active config to a .cfg file.
   path: full filename (e.g. "controls/player1.cfg").
   Returns 0 on success, -1 on failure. */
int hal_keys_save_config(const char *path);

/* Load a config from a .cfg file into the active config.
   Returns 0 on success, -1 on failure (file not found, bad magic, etc).
   On failure, the active config is unchanged. */
int hal_keys_load_config(const char *path);

#endif /* HAL_KEYBINDS_H */
