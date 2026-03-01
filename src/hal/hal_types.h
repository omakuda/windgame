/*============================================================================
 * hal_types.h - Shared types and constants for the HAL
 *
 * This file provides portable type definitions and game constants
 * that are used by both the HAL interface and game logic.
 *============================================================================*/

#ifndef HAL_TYPES_H
#define HAL_TYPES_H

/*--------------------------------------------------------------------------
 * Portable integer types
 *
 * z88dk doesn't always have <stdint.h> depending on config,
 * so we define our own for maximum portability.
 *--------------------------------------------------------------------------*/
#ifdef __Z88DK
    typedef unsigned char      uint8_t;
    typedef signed char        int8_t;
    typedef unsigned int       uint16_t;
    typedef signed int         int16_t;
    typedef unsigned long      uint32_t;
    typedef signed long        int32_t;
#else
    #include <stdint.h>
    #include <stdbool.h>
#endif

#ifdef __Z88DK
    #ifndef bool
        typedef uint8_t bool;
        #define true  1
        #define false 0
    #endif
#endif

#ifndef NULL
    #define NULL ((void*)0)
#endif

/*--------------------------------------------------------------------------
 * Screen constants
 *
 * The "virtual" resolution the game logic works in.
 * On the Next, this maps to 256x192 (the native ULA/tilemap area).
 * On Windows, this is scaled up to a comfortable window size.
 *--------------------------------------------------------------------------*/
#define SCREEN_W        256
#define SCREEN_H        192
#define TILE_SIZE        16     /* 16x16 pixel tiles */
#define TILES_X         (SCREEN_W / TILE_SIZE)   /* 16 tiles across */
#define TILES_Y         (SCREEN_H / TILE_SIZE)   /* 12 tiles down   */

/*--------------------------------------------------------------------------
 * Sprite limits
 *
 * The Next supports 128 hardware sprites. We reserve a budget
 * the game can rely on across all platforms.
 *--------------------------------------------------------------------------*/
#define MAX_SPRITES      64
#define SPRITE_W         16
#define SPRITE_H         16

/*--------------------------------------------------------------------------
 * Input bitmask (16-bit)
 *
 * hal_input_poll() returns a uint16_t bitmask of these flags.
 * Designed so multiple buttons can be held simultaneously.
 *
 * Layout:
 *   Bits 0-3:   Movement (up, down, left, right)
 *   Bits 4-9:   Action buttons 1-6
 *   Bit 10:     Menu
 *   Bit 11:     Bag (items)
 *   Bits 12-15: Reserved for future use
 *
 * Default keyboard mappings:
 *   Key     | Next        | Win64 primary | Win64 alt | Gamepad
 *   --------+-------------+---------------+-----------+--------
 *   Up      | E           | E             | Arrow Up  | D-pad Up
 *   Down    | D           | D             | Arrow Dn  | D-pad Down
 *   Left    | S           | S             | Arrow Lt  | D-pad Left
 *   Right   | F           | F             | Arrow Rt  | D-pad Right
 *   Btn 1   | J           | J             |           | A
 *   Btn 2   | K           | K             |           | B
 *   Btn 3   | L           | L             |           | X
 *   Btn 4   | U           | U             |           | Y
 *   Btn 5   | I           | I             |           | LB
 *   Btn 6   | O           | O             |           | RB
 *   Menu    | Enter       | Enter         |           | Start
 *   Bag     | B           | B             |           | Back/Select
 *--------------------------------------------------------------------------*/
#define INPUT_UP        0x0001u
#define INPUT_DOWN      0x0002u
#define INPUT_LEFT      0x0004u
#define INPUT_RIGHT     0x0008u
#define INPUT_BTN1      0x0010u   /* action button 1 (J) — jump / confirm   */
#define INPUT_BTN2      0x0020u   /* action button 2 (K) — attack / cancel  */
#define INPUT_BTN3      0x0040u   /* action button 3 (L)                    */
#define INPUT_BTN4      0x0080u   /* action button 4 (U)                    */
#define INPUT_BTN5      0x0100u   /* action button 5 (I)                    */
#define INPUT_BTN6      0x0200u   /* action button 6 (O)                    */
#define INPUT_MENU      0x0400u   /* menu / pause (Enter)                   */
#define INPUT_BAG       0x0800u   /* bag / items (B)                        */

/* Convenience aliases — use these in game code for readability */
#define INPUT_JUMP      INPUT_BTN1
#define INPUT_ATTACK    INPUT_BTN2
#define INPUT_CONFIRM   INPUT_BTN1
#define INPUT_CANCEL    INPUT_BTN2

/* Masks for testing groups of buttons */
#define INPUT_MOVE_MASK   (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT)
#define INPUT_ACTION_MASK (INPUT_BTN1 | INPUT_BTN2 | INPUT_BTN3 | INPUT_BTN4 | INPUT_BTN5 | INPUT_BTN6)
#define INPUT_ALL_MASK    0x0FFFu

/*--------------------------------------------------------------------------
 * Tilemap constants
 *--------------------------------------------------------------------------*/
#define TILEMAP_MAX_W    256   /* max map width in tiles  */
#define TILEMAP_MAX_H    256   /* max map height in tiles */

/*--------------------------------------------------------------------------
 * Tile collision flags
 *
 * Each tile type can have collision properties stored in a separate
 * table indexed by tile index. The game checks these during movement.
 *--------------------------------------------------------------------------*/
#define TILE_EMPTY       0x00   /* air / passable                       */
#define TILE_SOLID       0x01   /* blocks movement from all sides       */
#define TILE_PLATFORM    0x02   /* solid only from above (one-way)      */
#define TILE_LADDER      0x04   /* climbable                            */
#define TILE_DAMAGE      0x08   /* hurts player on contact              */
#define TILE_WATER       0x10   /* swimming physics                     */
#define TILE_EXIT        0x20   /* exit trigger — leave action scene    */
#define TILE_TRANSITION  0x40   /* room transition — door / ladder      */

/*--------------------------------------------------------------------------
 * Color type
 *
 * 8-bit palette index on Next (RRRGGGBB).
 * On Win64 we map this to full RGBA internally.
 *--------------------------------------------------------------------------*/
typedef uint8_t color_t;

/*--------------------------------------------------------------------------
 * Rectangle (used for collision, dirty rects, etc.)
 *--------------------------------------------------------------------------*/
typedef struct {
    int16_t x, y;
    uint8_t w, h;
} rect_t;

/*--------------------------------------------------------------------------
 * Sprite descriptor
 *
 * A lightweight handle the game passes to hal_sprite_set().
 * The HAL maps this to hardware sprites or software blits.
 *--------------------------------------------------------------------------*/
typedef struct {
    uint8_t  id;           /* sprite slot (0 .. MAX_SPRITES-1)    */
    int16_t  x, y;         /* screen position (can be negative)   */
    uint8_t  pattern;      /* pattern/frame index in sprite sheet */
    uint8_t  flags;        /* see SPRITE_FLAG_* below             */
    uint8_t  palette;      /* palette offset (Next: 0-15)         */
} sprite_desc_t;

#define SPRITE_FLAG_VISIBLE   0x01
#define SPRITE_FLAG_MIRROR_X  0x02
#define SPRITE_FLAG_MIRROR_Y  0x04
#define SPRITE_FLAG_ROTATE    0x08  /* 90 degree rotation */

/*--------------------------------------------------------------------------
 * Tilemap descriptor
 *
 * Describes one loaded tilemap layer. The game fills this and hands
 * it to the HAL for rendering.
 *--------------------------------------------------------------------------*/
typedef struct {
    const uint8_t *data;        /* tile indices, row-major          */
    uint16_t       map_w;       /* map width in tiles               */
    uint16_t       map_h;       /* map height in tiles              */
    int16_t        scroll_x;    /* pixel scroll offset              */
    int16_t        scroll_y;    /* pixel scroll offset              */
} tilemap_desc_t;

/*--------------------------------------------------------------------------
 * Sound effect and music IDs
 *
 * The game references sounds by ID. The HAL maps these to
 * platform-specific playback.
 *--------------------------------------------------------------------------*/
typedef uint8_t sfx_id_t;
typedef uint8_t music_id_t;

#endif /* HAL_TYPES_H */
