/*============================================================================
 * main.c - Game Entry Point (Platform-Independent)
 *
 * Scenes:
 *   OVERWORLD  — top-down scrolling map with spawning events
 *   ACTION     — sidescrolling platformer (field / dungeon)
 *
 * Action scene types (map_event_type_t):
 *   FIELD   (1)  — open area with exit tiles on L/R borders.
 *                   Enemies, safe zones, discoveries load here.
 *   DUNGEON_FIXED (2) — pre-defined room order (type 1).
 *   DUNGEON_RANDOM(3) — rooms picked from pool, randomly connected (type 2).
 *                   Transition tiles move between rooms.
 *
 * Spawn cycle: timer -> spawn group -> linger 5s -> despawn -> timer resets
 *============================================================================*/

#include "hal/hal.h"
#include "game/rng.h"
#include "game/overworld_events.h"

/*==========================================================================
 * TILE COLLISION TABLE
 *==========================================================================*/

#define NUM_TILE_TYPES  10

static const uint8_t tile_collision[NUM_TILE_TYPES] = {
    /* 0 */ TILE_EMPTY,
    /* 1 */ TILE_SOLID,
    /* 2 */ TILE_SOLID,
    /* 3 */ TILE_SOLID,
    /* 4 */ TILE_PLATFORM,
    /* 5 */ TILE_LADDER,
    /* 6 */ TILE_DAMAGE,
    /* 7 */ TILE_EMPTY,        /* path / decoration      */
    /* 8 */ TILE_EXIT,         /* exit trigger            */
    /* 9 */ TILE_TRANSITION,   /* room transition trigger */
};

static uint8_t tile_flags(uint8_t tile_idx) {
    if (tile_idx >= NUM_TILE_TYPES) return TILE_EMPTY;
    return tile_collision[tile_idx];
}

/*==========================================================================
 * COLLISION HELPERS
 *==========================================================================*/

static uint8_t point_tile_flags(int16_t px, int16_t py) {
    uint16_t tx = (uint16_t)(px / TILE_SIZE);
    uint16_t ty = (uint16_t)(py / TILE_SIZE);
    return tile_flags(hal_tilemap_get(tx, ty));
}

static uint8_t box_hits_flag(int16_t bx, int16_t by, uint8_t bw, uint8_t bh, uint8_t mask) {
    if (point_tile_flags(bx,          by)          & mask) return 1;
    if (point_tile_flags(bx + bw - 1, by)          & mask) return 1;
    if (point_tile_flags(bx,          by + bh - 1) & mask) return 1;
    if (point_tile_flags(bx + bw - 1, by + bh - 1) & mask) return 1;
    if (point_tile_flags(bx + bw / 2, by)          & mask) return 1;
    if (point_tile_flags(bx + bw / 2, by + bh - 1) & mask) return 1;
    if (point_tile_flags(bx,          by + bh / 2) & mask) return 1;
    if (point_tile_flags(bx + bw - 1, by + bh / 2) & mask) return 1;
    return 0;
}

/*==========================================================================
 * MAPS -- OVERWORLD  (40 x 30 tiles, 4x area of original)
 * 0=grass  1=wall  7=path(safe)  9=dungeon entrance (transition tile)
 *==========================================================================*/

#define OW_MAP_W   40
#define OW_MAP_H   30
#define OW_PATH_TILE  7

static const uint8_t overworld_map[OW_MAP_H * OW_MAP_W] = {
/* r 0*/ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* r 1*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/* r 2*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,7,0,0,0,0,0,0,0,0,0,0,0,0,1,
/* r 3*/ 1,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,7,0,0,0,0,0,0,0,0,0,0,0,0,1,
/* r 4*/ 1,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,
/* r 5*/ 1,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,
/* r 6*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/* r 7*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/* r 8*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/* r 9*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,1,
/*r10*/ 1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,1,
/*r11*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r12*/ 1,0,0,7,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r13*/ 1,0,0,7,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r14*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r15*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r16*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r17*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,1,
/*r18*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r19*/ 1,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r20*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r21*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,1,
/*r22*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,7,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r23*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,7,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r24*/ 1,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r25*/ 1,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,
/*r26*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r27*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r28*/ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
/*r29*/ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

/*==========================================================================
 * FIELD ACTION MAPS -- exit tiles (8) on L/R borders
 *==========================================================================*/

#define FIELD_COMBAT_W  40
#define FIELD_COMBAT_H  12
static const uint8_t field_map_combat[FIELD_COMBAT_H * FIELD_COMBAT_W] = {
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,2,0,0,4,4,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,2,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,2,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,2,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,8,
8,1,1,1,1,1,1,1,1,1,1,0,0,6,6,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
};

#define FIELD_DISC_W  30
#define FIELD_DISC_H  12
static const uint8_t field_map_discovery[FIELD_DISC_H * FIELD_DISC_W] = {
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
};

/*==========================================================================
 * PEACEFUL ACTION MAPS -- with exit borders
 *==========================================================================*/

#define SAFE_LONE_W  16
#define SAFE_LONE_H  12
static const uint8_t safe_map_lone[SAFE_LONE_H * SAFE_LONE_W] = {
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,7,0,0,0,0,7,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
};
#define SAFE_LONE_NPC_X  (8 * TILE_SIZE)
#define SAFE_LONE_NPC_Y  (8 * TILE_SIZE)

#define SAFE_CARAVAN_W  30
#define SAFE_CARAVAN_H  12
static const uint8_t safe_map_caravan[SAFE_CARAVAN_H * SAFE_CARAVAN_W] = {
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,3,3,0,0,0,0,0,0,0,0,7,7,7,0,0,0,0,0,0,0,3,3,3,0,0,0,8,
8,0,0,3,3,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,3,3,3,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
};
#define SAFE_CARAVAN_NPC_COUNT 4
static const int16_t safe_caravan_npc_x[SAFE_CARAVAN_NPC_COUNT] = {6*16,11*16,16*16,21*16};
static const int16_t safe_caravan_npc_y[SAFE_CARAVAN_NPC_COUNT] = {8*16,8*16,8*16,8*16};

#define SAFE_OASIS_W  50
#define SAFE_OASIS_H  12
static const uint8_t safe_map_oasis[SAFE_OASIS_H * SAFE_OASIS_W] = {
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,3,3,3,3,3,0,0,0,0,3,3,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,3,3,3,3,3,3,3,0,0,0,8,
8,0,3,0,0,0,3,0,0,0,0,3,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,3,0,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,8,
8,0,3,0,0,0,3,0,0,0,0,3,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,3,0,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,8,
8,0,3,0,0,0,3,0,0,0,0,3,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,3,0,0,0,0,0,0,3,0,0,0,0,0,3,0,0,0,8,
8,0,3,3,0,3,3,0,0,0,0,3,3,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,3,0,0,0,0,0,0,3,3,3,0,0,3,3,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,
};
#define SAFE_OASIS_NPC_COUNT 7
static const int16_t safe_oasis_npc_x[SAFE_OASIS_NPC_COUNT]={4*16,13*16,21*16,25*16,30*16,42*16,18*16};
static const int16_t safe_oasis_npc_y[SAFE_OASIS_NPC_COUNT]={5*16,5*16,8*16,8*16,5*16,5*16,8*16};
static const uint8_t safe_oasis_npc_type[SAFE_OASIS_NPC_COUNT]={
    FRIENDLY_MERCHANT,FRIENDLY_MERCHANT,FRIENDLY_MERCHANT,
    FRIENDLY_WANDERER,FRIENDLY_HEALER,FRIENDLY_SAGE,FRIENDLY_WANDERER};

/*==========================================================================
 * DUNGEON ROOM DEFINITIONS
 * Transition tiles (9) at edges: left = go back, right = go forward.
 *==========================================================================*/

typedef struct {
    const uint8_t *map;
    uint16_t w, h;
    int16_t entry_back_x, entry_back_y;
    int16_t entry_fwd_x, entry_fwd_y;
} dungeon_room_def_t;

#define DR_W 20
#define DR_H 12
static const uint8_t droom_a[DR_H*DR_W]={
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,5,0,9,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1,
1,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,5,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

#define DR2_W 24
static const uint8_t droom_b[DR_H*DR2_W]={
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,1,
1,0,0,0,0,0,0,0,4,4,0,0,0,0,4,4,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,
1,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,6,6,0,0,0,0,0,0,0,0,0,0,6,6,0,0,0,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

#define DR3_W 16
static const uint8_t droom_c[DR_H*DR3_W]={
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
9,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1,
1,0,0,5,0,0,0,0,0,0,0,0,5,0,0,1,
1,1,1,1,1,0,0,0,0,0,0,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,5,0,0,5,0,0,0,0,0,9,
1,0,0,0,0,0,5,0,0,5,0,0,0,0,0,1,
1,0,0,0,0,0,5,0,0,5,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

static const uint8_t droom_d[DR_H*DR_W]={
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

static const uint8_t droom_e[DR_H*DR_W]={
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,2,2,0,0,0,0,0,0,0,0,2,2,0,0,0,1,
1,0,0,0,2,2,0,0,0,0,0,0,0,0,2,2,0,0,0,1,
1,0,0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,6,6,0,0,0,0,0,0,0,0,0,0,6,6,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

static const uint8_t droom_f[DR_H*DR2_W]={
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
9,0,0,0,0,1,1,1,1,0,0,0,0,0,1,1,1,1,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,
1,0,0,0,0,1,1,1,1,0,0,0,0,0,1,1,1,1,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

#define DUNGEON_ROOM_POOL_SIZE 6
static const dungeon_room_def_t room_pool[DUNGEON_ROOM_POOL_SIZE] = {
    { droom_a, DR_W,  DR_H,  2*16,3*16,  18*16,6*16 },
    { droom_b, DR2_W, DR_H,  2*16,2*16,  22*16,6*16 },
    { droom_c, DR3_W, DR_H,  2*16,2*16,  14*16,6*16 },
    { droom_d, DR_W,  DR_H,  2*16,2*16,  18*16,2*16 },
    { droom_e, DR_W,  DR_H,  2*16,2*16,  18*16,6*16 },
    { droom_f, DR2_W, DR_H,  2*16,3*16,  22*16,6*16 },
};

#define FIXED_DUNGEON_SIZE 4
static const uint8_t fixed_dungeon_order[FIXED_DUNGEON_SIZE] = { 0, 1, 2, 3 };

/*==========================================================================
 * TYPE-2 DUNGEON ENTRY ROOM
 * Exit tiles (8) on L/R edges let the player leave back to overworld.
 * A ladder pit in the centre leads down to a transition tile (9) that
 * drops into the first randomly-connected room.
 *==========================================================================*/

#define ENTRY_W 24
#define ENTRY_H 12
static const uint8_t dungeon_entry_room[ENTRY_H * ENTRY_W] = {
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,
8,1,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,5,5,5,5,1,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,5,5,5,5,1,1,1,1,1,1,1,1,1,8,
8,1,1,1,1,1,1,1,1,1,9,9,9,9,1,1,1,1,1,1,1,1,1,8,
};

/*==========================================================================
 * PLAYER STATE
 *==========================================================================*/

#define PLAYER_HB_X_OFFSET  3
#define PLAYER_HB_Y_OFFSET  2
#define PLAYER_HB_W         10
#define PLAYER_HB_H         14

typedef struct {
    int16_t x, y, vel_x, vel_y;
    int16_t vel_fx;  /* horizontal velocity in 8.8 fixed-point (action scenes) */
    uint8_t dir, on_ground, on_ladder, attacking, frame;
    int16_t hp;
    uint8_t invuln;
} player_t;

static player_t s_player;
static int16_t s_ow_player_x, s_ow_player_y;

/*==========================================================================
 * GAME STATE
 *==========================================================================*/

typedef enum { SCENE_OVERWORLD, SCENE_ACTION } scene_t;

typedef enum {
    ACTION_REASON_NONE=0, ACTION_REASON_COMBAT, ACTION_REASON_SAFE,
    ACTION_REASON_DISCOVERY, ACTION_REASON_DUNGEON_FIXED,
    ACTION_REASON_DUNGEON_RANDOM
} action_reason_t;

typedef enum {
    MAP_EVENT_FIELD=1, MAP_EVENT_DUNGEON_FIXED, MAP_EVENT_DUNGEON_RANDOM
} map_event_type_t;

static scene_t s_scene;
static action_reason_t s_action_reason;
static map_event_type_t s_map_event_type;
static safe_zone_type_t s_safe_type;
static encounter_t s_last_encounter;
static uint8_t s_paused, s_bag_open, s_friendly_dialog;
static int16_t s_camera_x, s_ow_cam_x, s_ow_cam_y;
static uint16_t s_act_map_w, s_act_map_h;

#define ACTION_NPC_SPRITE_BASE 2
#define ACTION_NPC_MAX 8
static uint8_t s_action_npc_count;

#define MAX_DUNGEON_ROOMS 8
typedef struct {
    uint8_t num_rooms;
    uint8_t room_order[MAX_DUNGEON_ROOMS];
    uint8_t current_idx;
    uint8_t in_entry;   /* 1 = in type-2 entry room (has exit+transition) */
} dungeon_state_t;
static dungeon_state_t s_dungeon;

#define GRAVITY 1
#define JUMP_FORCE (-7)
#define MOVE_SPEED 2
#define MAX_FALL_SPEED 6
#define CLIMB_SPEED 2
#define ATTACK_DURATION 12
#define INVULN_TIME 30
#define PLAYER_START_HP 10

/* Overworld player: 8x8 visual centred in 16x16 sprite slot.
 * Uses a dedicated pattern (PATTERN_OW_PLAYER) with only the
 * centre 8x8 filled.  Movement at 75% of action speed. */
#define OW_PLAYER_SIZE    8
#define OW_PLAYER_OFFSET  4   /* (16-8)/2 — centering in 16x16 sprite */
#define OW_MOVE_SPEED     1   /* 75% of 2: alternate 1 and 2 px/frame */
#define PATTERN_OW_PLAYER 1   /* sprite pattern 1 = overworld player   */

/*----------------------------------------------------------------------
 * Horizontal momentum (fixed-point 8.8)
 *
 * Max speed = 2 px/frame = 512 in 8.8.
 * Ramp-up in ~0.25s = 12 frames at 50 Hz.
 * Accel  = 512 / 12 ≈ 43 per frame.
 * Decel (friction) is slightly faster so the player stops crisply.
 *----------------------------------------------------------------------*/
#define FX_SHIFT       8
#define FX_ONE         (1 << FX_SHIFT)           /* 256  */
#define FX_MAX_SPEED   (MOVE_SPEED * FX_ONE)     /* 512  */
#define FX_ACCEL       43                         /* reach max in ~12 frames */
#define FX_DECEL       64                         /* stop in ~8 frames */
#define FX_TO_PX(v)    ((int16_t)((v) >> FX_SHIFT))

/*==========================================================================
 * FRIENDLY NPC DIALOG
 *==========================================================================*/

static const char *friendly_name(friendly_type_t ft) {
    switch(ft){case FRIENDLY_MERCHANT:return"MERCHANT";case FRIENDLY_HEALER:return"HEALER";
    case FRIENDLY_SAGE:return"SAGE";case FRIENDLY_WANDERER:return"WANDERER";default:return"STRANGER";}
}
static const char *friendly_line1(friendly_type_t ft) {
    switch(ft){case FRIENDLY_MERCHANT:return"Want to trade?";case FRIENDLY_HEALER:return"Let me heal you.";
    case FRIENDLY_SAGE:return"I sense danger...";case FRIENDLY_WANDERER:return"Nice day for a walk.";default:return"...";}
}
static const char *friendly_line2(friendly_type_t ft) {
    switch(ft){case FRIENDLY_MERCHANT:return"I have rare items.";case FRIENDLY_HEALER:return"You look tired.";
    case FRIENDLY_SAGE:return"Beware the east.";case FRIENDLY_WANDERER:return"Seen anything odd?";default:return"";}
}
static void friendly_apply_effect(friendly_type_t ft) {
    if(ft==FRIENDLY_HEALER){s_player.hp+=3;if(s_player.hp>PLAYER_START_HP)s_player.hp=PLAYER_START_HP;}
}
static void friendly_dialog_draw(void) {
    hal_draw_rect(24,100,208,80,0x00); hal_draw_rect(26,102,204,76,0x03);
    hal_draw_text(36,108,friendly_name(s_last_encounter.friendly_type),0xFF);
    hal_draw_text(36,124,friendly_line1(s_last_encounter.friendly_type),0xFF);
    hal_draw_text(36,138,friendly_line2(s_last_encounter.friendly_type),0xFF);
    hal_draw_text(36,158,"Btn1=OK",0xFF);
}
static const char *enemy_name(event_type_t et) {
    switch(et){case EVENT_ENEMY_WEAK:return"WEAK FOE";case EVENT_ENEMY_MEDIUM:return"TOUGH FOE";
    case EVENT_ENEMY_STRONG:return"BOSS FOE";default:return"???";}
}
static const char *safe_zone_name(safe_zone_type_t st) {
    switch(st){case SAFE_LONE_CHARACTER:return"QUIET CLEARING";case SAFE_CARAVAN:return"CARAVAN";
    case SAFE_OASIS_TOWN:return"OASIS TOWN";default:return"SAFE ZONE";}
}

/*==========================================================================
 * OVERWORLD SCENE -- scrolling camera
 *==========================================================================*/

static uint8_t s_ow_frame_toggle;  /* alternates 0/1 each frame for 75% speed */

static void overworld_init(void) {
    hal_tilemap_set(overworld_map, OW_MAP_W, OW_MAP_H);
    s_player.x=s_ow_player_x; s_player.y=s_ow_player_y;
    s_player.vel_x=0; s_player.vel_y=0; s_player.vel_fx=0; s_player.dir=0;
    s_player.on_ground=0; s_player.attacking=0; s_player.frame=0;
    s_friendly_dialog=0;s_ow_frame_toggle=0;
    events_init(overworld_map,OW_MAP_W,OW_MAP_H,OW_PATH_TILE,&SPAWN_TABLE_DEFAULT);
}

static void overworld_update_camera(void) {
    int16_t max_x=(int16_t)(OW_MAP_W*TILE_SIZE)-SCREEN_W;
    int16_t max_y=(int16_t)(OW_MAP_H*TILE_SIZE)-SCREEN_H;
    /* Centre camera on the 8x8 visual centre of the player */
    s_ow_cam_x=s_player.x+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2-SCREEN_W/2;
    s_ow_cam_y=s_player.y+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2-SCREEN_H/2;
    if(s_ow_cam_x<0)s_ow_cam_x=0; if(s_ow_cam_y<0)s_ow_cam_y=0;
    if(s_ow_cam_x>max_x)s_ow_cam_x=max_x; if(s_ow_cam_y>max_y)s_ow_cam_y=max_y;
    hal_tilemap_scroll(s_ow_cam_x,s_ow_cam_y);
}

static void overworld_update(uint16_t input, uint16_t pressed) {
    int16_t nx,ny; encounter_t enc;
    int16_t spd;
    if(s_friendly_dialog){if(pressed&INPUT_BTN1){friendly_apply_effect(s_last_encounter.friendly_type);s_friendly_dialog=0;}return;}

    /* 75% of 2 px/frame: alternate 2,1,2,1 → average 1.5 px/frame */
    s_ow_frame_toggle^=1;
    spd=s_ow_frame_toggle?2:1;

    nx=s_player.x; ny=s_player.y;
    if(input&INPUT_UP)ny-=spd; if(input&INPUT_DOWN)ny+=spd;
    if(input&INPUT_LEFT){nx-=spd;s_player.dir=1;} if(input&INPUT_RIGHT){nx+=spd;s_player.dir=0;}

    /* Collision uses 8x8 centre of the 16x16 slot */
    if(!box_hits_flag(nx+OW_PLAYER_OFFSET,s_player.y+OW_PLAYER_OFFSET,OW_PLAYER_SIZE,OW_PLAYER_SIZE,TILE_SOLID))s_player.x=nx;
    if(!box_hits_flag(s_player.x+OW_PLAYER_OFFSET,ny+OW_PLAYER_OFFSET,OW_PLAYER_SIZE,OW_PLAYER_SIZE,TILE_SOLID))s_player.y=ny;

    if(s_player.x<0)s_player.x=0; if(s_player.y<0)s_player.y=0;
    if(s_player.x>(OW_MAP_W*TILE_SIZE)-SPRITE_W)s_player.x=(OW_MAP_W*TILE_SIZE)-SPRITE_W;
    if(s_player.y>(OW_MAP_H*TILE_SIZE)-SPRITE_H)s_player.y=(OW_MAP_H*TILE_SIZE)-SPRITE_H;

    /* Check for fixed dungeon entrance (tile 9) — use 8x8 centre */
    {int16_t cx=s_player.x+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
     int16_t cy=s_player.y+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
     uint16_t tx=(uint16_t)(cx/TILE_SIZE),ty=(uint16_t)(cy/TILE_SIZE);
     if(tx<OW_MAP_W&&ty<OW_MAP_H&&overworld_map[ty*OW_MAP_W+tx]==9){
        s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
        s_action_reason=ACTION_REASON_DUNGEON_FIXED;s_map_event_type=MAP_EVENT_DUNGEON_FIXED;
        events_clear();s_scene=SCENE_ACTION;return;}}

    overworld_update_camera();

    enc=events_update(s_player.x,s_player.y);
    if(enc.kind!=ENCOUNTER_NONE){
        s_last_encounter=enc;s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
        switch(enc.kind){
        case ENCOUNTER_COMBAT:s_action_reason=ACTION_REASON_COMBAT;s_map_event_type=MAP_EVENT_FIELD;events_clear();s_scene=SCENE_ACTION;break;
        case ENCOUNTER_SAFE:s_action_reason=ACTION_REASON_SAFE;s_map_event_type=MAP_EVENT_FIELD;s_safe_type=enc.safe_type;events_clear();s_scene=SCENE_ACTION;break;
        case ENCOUNTER_DISCOVERY:s_action_reason=ACTION_REASON_DISCOVERY;s_map_event_type=MAP_EVENT_FIELD;events_clear();s_scene=SCENE_ACTION;break;
        case ENCOUNTER_FRIENDLY:s_friendly_dialog=1;break;
        case ENCOUNTER_DUNGEON_FIXED:s_action_reason=ACTION_REASON_DUNGEON_FIXED;s_map_event_type=MAP_EVENT_DUNGEON_FIXED;events_clear();s_scene=SCENE_ACTION;break;
        case ENCOUNTER_DUNGEON_RANDOM:s_action_reason=ACTION_REASON_DUNGEON_RANDOM;s_map_event_type=MAP_EVENT_DUNGEON_RANDOM;events_clear();s_scene=SCENE_ACTION;break;
        default:break;}}
}

/*==========================================================================
 * ACTION SCENE
 *==========================================================================*/

#define PATTERN_FRIENDLY 8

static void place_action_npcs(const int16_t *xs,const int16_t *ys,uint8_t count){
    uint8_t i;s_action_npc_count=count;
    for(i=0;i<count&&i<ACTION_NPC_MAX;i++){
        sprite_desc_t npc;npc.id=ACTION_NPC_SPRITE_BASE+i;npc.x=xs[i];npc.y=ys[i];
        npc.pattern=PATTERN_FRIENDLY;npc.palette=0;npc.flags=SPRITE_FLAG_VISIBLE;hal_sprite_set(&npc);}}

static void load_dungeon_room(uint8_t pool_idx,uint8_t from_fwd){
    const dungeon_room_def_t *room=&room_pool[pool_idx];
    hal_sprite_hide_all();s_action_npc_count=0;
    hal_tilemap_set(room->map,room->w,room->h);
    s_act_map_w=room->w;s_act_map_h=room->h;
    if(from_fwd){s_player.x=room->entry_back_x;s_player.y=room->entry_back_y;}
    else{s_player.x=room->entry_fwd_x;s_player.y=room->entry_fwd_y;}
    s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;
    s_camera_x=0;s_dungeon.in_entry=0;
}

static void load_entry_room(void){
    hal_sprite_hide_all();s_action_npc_count=0;
    hal_tilemap_set(dungeon_entry_room,ENTRY_W,ENTRY_H);
    s_act_map_w=ENTRY_W;s_act_map_h=ENTRY_H;
    /* Spawn near centre on the floor, above the pit */
    s_player.x=6*TILE_SIZE;s_player.y=7*TILE_SIZE;
    s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;
    s_camera_x=0;s_dungeon.in_entry=1;
}

static void dungeon_init_fixed(void){
    uint8_t i;s_dungeon.num_rooms=FIXED_DUNGEON_SIZE;
    for(i=0;i<FIXED_DUNGEON_SIZE;i++)s_dungeon.room_order[i]=fixed_dungeon_order[i];
    s_dungeon.current_idx=0;s_dungeon.in_entry=0;
    load_dungeon_room(s_dungeon.room_order[0],1);
}

static void dungeon_init_random(void){
    uint8_t count,i,used[DUNGEON_ROOM_POOL_SIZE];
    count=(uint8_t)rng_range16(3,5);if(count>MAX_DUNGEON_ROOMS)count=MAX_DUNGEON_ROOMS;
    for(i=0;i<DUNGEON_ROOM_POOL_SIZE;i++)used[i]=0;
    s_dungeon.num_rooms=0;
    while(s_dungeon.num_rooms<count){
        uint8_t pick=(uint8_t)rng_range(DUNGEON_ROOM_POOL_SIZE);
        if(!used[pick]){used[pick]=1;s_dungeon.room_order[s_dungeon.num_rooms]=pick;s_dungeon.num_rooms++;}}
    s_dungeon.current_idx=0;
    /* Start in the entry room — player can exit L/R or descend into depths */
    load_entry_room();
}

static void action_init(void){
    hal_sprite_hide_all();s_action_npc_count=0;
    if(s_map_event_type==MAP_EVENT_DUNGEON_FIXED){dungeon_init_fixed();}
    else if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM){dungeon_init_random();}
    else{
        switch(s_action_reason){
        case ACTION_REASON_SAFE:
            switch(s_safe_type){
            case SAFE_LONE_CHARACTER:hal_tilemap_set(safe_map_lone,SAFE_LONE_W,SAFE_LONE_H);s_act_map_w=SAFE_LONE_W;s_act_map_h=SAFE_LONE_H;
                {int16_t nx=SAFE_LONE_NPC_X,ny=SAFE_LONE_NPC_Y;place_action_npcs(&nx,&ny,1);}break;
            case SAFE_CARAVAN:hal_tilemap_set(safe_map_caravan,SAFE_CARAVAN_W,SAFE_CARAVAN_H);s_act_map_w=SAFE_CARAVAN_W;s_act_map_h=SAFE_CARAVAN_H;
                place_action_npcs(safe_caravan_npc_x,safe_caravan_npc_y,SAFE_CARAVAN_NPC_COUNT);break;
            case SAFE_OASIS_TOWN:hal_tilemap_set(safe_map_oasis,SAFE_OASIS_W,SAFE_OASIS_H);s_act_map_w=SAFE_OASIS_W;s_act_map_h=SAFE_OASIS_H;
                place_action_npcs(safe_oasis_npc_x,safe_oasis_npc_y,SAFE_OASIS_NPC_COUNT);break;
            default:hal_tilemap_set(safe_map_lone,SAFE_LONE_W,SAFE_LONE_H);s_act_map_w=SAFE_LONE_W;s_act_map_h=SAFE_LONE_H;break;}break;
        case ACTION_REASON_DISCOVERY:hal_tilemap_set(field_map_discovery,FIELD_DISC_W,FIELD_DISC_H);s_act_map_w=FIELD_DISC_W;s_act_map_h=FIELD_DISC_H;break;
        case ACTION_REASON_COMBAT:default:hal_tilemap_set(field_map_combat,FIELD_COMBAT_W,FIELD_COMBAT_H);s_act_map_w=FIELD_COMBAT_W;s_act_map_h=FIELD_COMBAT_H;break;}
        /* Field spawn: near but not at extreme left, on the ground */
        s_player.x=3*TILE_SIZE;s_player.y=(int16_t)((s_act_map_h-3)*TILE_SIZE);
        s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;}
    s_player.dir=0;s_player.frame=0;s_player.invuln=0;s_player.vel_fx=0;s_camera_x=0;
}

static void dungeon_handle_transition(int16_t pcx){
    int16_t mid=(int16_t)(s_act_map_w*TILE_SIZE/2);
    if(pcx<mid){
        /* Going backward */
        if(s_dungeon.current_idx==0){
            if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM){
                /* Back to entry room */
                load_entry_room();return;
            }
            /* Fixed dungeon: exit to overworld from first room */
            s_scene=SCENE_OVERWORLD;return;
        }
        s_dungeon.current_idx--;load_dungeon_room(s_dungeon.room_order[s_dungeon.current_idx],0);
    }else{
        /* Going forward */
        if(s_dungeon.current_idx>=s_dungeon.num_rooms-1){s_scene=SCENE_OVERWORLD;return;}
        s_dungeon.current_idx++;load_dungeon_room(s_dungeon.room_order[s_dungeon.current_idx],1);
    }
}

static void action_update(uint16_t input,uint16_t pressed){
    int16_t nx,ny,hb_x,hb_y,abs_vfx;

    /* Horizontal momentum: accelerate toward input, decelerate when idle */
    if(input&INPUT_LEFT){
        s_player.vel_fx-=FX_ACCEL;
        if(s_player.vel_fx<-FX_MAX_SPEED)s_player.vel_fx=-FX_MAX_SPEED;
        s_player.dir=1;
    }else if(input&INPUT_RIGHT){
        s_player.vel_fx+=FX_ACCEL;
        if(s_player.vel_fx>FX_MAX_SPEED)s_player.vel_fx=FX_MAX_SPEED;
        s_player.dir=0;
    }else{
        /* Friction: decelerate toward zero */
        if(s_player.vel_fx>0){s_player.vel_fx-=FX_DECEL;if(s_player.vel_fx<0)s_player.vel_fx=0;}
        else if(s_player.vel_fx<0){s_player.vel_fx+=FX_DECEL;if(s_player.vel_fx>0)s_player.vel_fx=0;}
    }
    s_player.vel_x=FX_TO_PX(s_player.vel_fx);
    abs_vfx=s_player.vel_x; if(abs_vfx<0)abs_vfx=-abs_vfx; if(abs_vfx<1&&s_player.vel_fx!=0)abs_vfx=1;

    hb_x=s_player.x+PLAYER_HB_X_OFFSET;hb_y=s_player.y+PLAYER_HB_Y_OFFSET;
    s_player.on_ladder=box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_LADDER);

    if(s_player.on_ladder){s_player.vel_y=0;
        if(input&INPUT_UP)s_player.vel_y=-CLIMB_SPEED;if(input&INPUT_DOWN)s_player.vel_y=CLIMB_SPEED;
    }else{
        if((pressed&INPUT_JUMP)&&s_player.on_ground){s_player.vel_y=JUMP_FORCE;s_player.on_ground=0;}
        s_player.vel_y+=GRAVITY;if(s_player.vel_y>MAX_FALL_SPEED)s_player.vel_y=MAX_FALL_SPEED;}

    if((pressed&INPUT_ATTACK)&&s_player.attacking==0)s_player.attacking=ATTACK_DURATION;
    if(s_player.attacking>0)s_player.attacking--;

    /* Horizontal collision */
    nx=s_player.x+s_player.vel_x;hb_x=nx+PLAYER_HB_X_OFFSET;hb_y=s_player.y+PLAYER_HB_Y_OFFSET;
    if(!box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_SOLID)){s_player.x=nx;}
    else{int16_t step=(s_player.vel_x>0)?1:-1;int16_t tx2=s_player.x;int16_t m=0;
        while(m<abs_vfx){tx2+=step;hb_x=tx2+PLAYER_HB_X_OFFSET;
            if(box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_SOLID)){tx2-=step;break;}m++;}
        s_player.x=tx2;s_player.vel_x=0;s_player.vel_fx=0;}

    /* Vertical collision */
    s_player.on_ground=0;ny=s_player.y+s_player.vel_y;hb_x=s_player.x+PLAYER_HB_X_OFFSET;hb_y=ny+PLAYER_HB_Y_OFFSET;
    if(!box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_SOLID)){
        if(s_player.vel_y>0){int16_t fy=ny+PLAYER_HB_Y_OFFSET+PLAYER_HB_H-1,pf=s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H-1;
            uint16_t fty=(uint16_t)(fy/TILE_SIZE),pty=(uint16_t)(pf/TILE_SIZE);
            if(fty!=pty){uint16_t mtx=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET+PLAYER_HB_W/2)/TILE_SIZE);
                uint8_t tb=hal_tilemap_get(mtx,fty);
                if((tile_flags(tb)&TILE_PLATFORM)&&pf<(int16_t)(fty*TILE_SIZE)){
                    s_player.y=(int16_t)(fty*TILE_SIZE)-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;s_player.vel_y=0;s_player.on_ground=1;goto av;}}}
        s_player.y=ny;
    }else{if(s_player.vel_y>0){int16_t fy=ny+PLAYER_HB_Y_OFFSET+PLAYER_HB_H-1;uint16_t fty=(uint16_t)(fy/TILE_SIZE);
        s_player.y=(int16_t)(fty*TILE_SIZE)-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;s_player.on_ground=1;
    }else{uint16_t hty=(uint16_t)(hb_y/TILE_SIZE);s_player.y=(int16_t)((hty+1)*TILE_SIZE)-PLAYER_HB_Y_OFFSET;}s_player.vel_y=0;}
av:
    hb_x=s_player.x+PLAYER_HB_X_OFFSET;hb_y=s_player.y+PLAYER_HB_Y_OFFSET;
    if(s_player.invuln==0&&box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_DAMAGE)){
        s_player.hp-=1;s_player.invuln=INVULN_TIME;s_player.vel_y=JUMP_FORCE/2;}
    if(s_player.invuln>0)s_player.invuln--;

    /* Fall death */
    if(s_player.y>(int16_t)(s_act_map_h*TILE_SIZE+32)){s_player.hp-=1;
        if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM&&s_dungeon.in_entry)load_entry_room();
        else if(s_map_event_type!=MAP_EVENT_FIELD)load_dungeon_room(s_dungeon.room_order[s_dungeon.current_idx],1);
        else{s_player.x=3*TILE_SIZE;s_player.y=(int16_t)((s_act_map_h-3)*TILE_SIZE);s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;}return;}

    /* EXIT tiles -- field maps or dungeon entry room → back to overworld */
    if(box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_EXIT)){
        if(s_map_event_type==MAP_EVENT_FIELD
         ||(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM&&s_dungeon.in_entry)){
            s_scene=SCENE_OVERWORLD;return;}}

    /* TRANSITION tiles -- dungeon rooms */
    if((s_map_event_type==MAP_EVENT_DUNGEON_FIXED||s_map_event_type==MAP_EVENT_DUNGEON_RANDOM)
       &&box_hits_flag(hb_x,hb_y,PLAYER_HB_W,PLAYER_HB_H,TILE_TRANSITION)){
        if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM&&s_dungeon.in_entry){
            /* Entry room ladder → descend into first random room */
            s_dungeon.in_entry=0;s_dungeon.current_idx=0;
            load_dungeon_room(s_dungeon.room_order[0],1);
        }else{
            dungeon_handle_transition(s_player.x+SPRITE_W/2);
        }return;}

    /* Camera */
    s_camera_x=s_player.x-SCREEN_W/2+SPRITE_W/2;
    if(s_camera_x<0)s_camera_x=0;
    if(s_camera_x>(int16_t)(s_act_map_w*TILE_SIZE)-SCREEN_W)s_camera_x=(int16_t)(s_act_map_w*TILE_SIZE)-SCREEN_W;
    hal_tilemap_scroll(s_camera_x,0);

    if(pressed&INPUT_BTN3){}if(pressed&INPUT_BTN4){}if(pressed&INPUT_BTN5){}if(pressed&INPUT_BTN6){}
    /* No menu exit from action scenes */
}

/*==========================================================================
 * BAG / PAUSE
 *==========================================================================*/

static void bag_draw(void){
    hal_draw_rect(32,24,192,144,0x00);hal_draw_rect(34,26,188,140,0x02);
    hal_draw_text(80,32,"== BAG ==",0xFF);
    hal_draw_text(44,52,"1. (empty)",0xFF);hal_draw_text(44,64,"2. (empty)",0xFF);
    hal_draw_text(44,76,"3. (empty)",0xFF);hal_draw_text(44,88,"4. (empty)",0xFF);
    hal_draw_text(44,108,"Press B to close",0xFF);
}
static void menu_draw(void){
    hal_draw_rect(64,60,128,72,0x00);hal_draw_rect(66,62,124,68,0x01);
    hal_draw_text(88,70,"PAUSED",0xFF);hal_draw_text(72,90,"Enter=Resume",0xFF);
}

/*==========================================================================
 * RENDER
 *==========================================================================*/

static void render(void){
    sprite_desc_t ps;
    hal_tilemap_draw();
    if(s_scene==SCENE_OVERWORLD)events_draw(s_ow_cam_x,s_ow_cam_y);

    ps.id=0;ps.palette=0;ps.flags=SPRITE_FLAG_VISIBLE;
    if(s_scene==SCENE_OVERWORLD)ps.pattern=PATTERN_OW_PLAYER;
    else ps.pattern=s_player.frame;
    if(s_player.dir)ps.flags|=SPRITE_FLAG_MIRROR_X;
    if(s_player.invuln>0&&(s_player.invuln&0x02))ps.flags&=~SPRITE_FLAG_VISIBLE;

    if(s_scene==SCENE_ACTION){ps.x=s_player.x-s_camera_x;ps.y=s_player.y;}
    else{ps.x=s_player.x-s_ow_cam_x;ps.y=s_player.y-s_ow_cam_y;}
    hal_sprite_set(&ps);hal_sprites_draw();

    if(s_scene==SCENE_OVERWORLD){
        hal_draw_text(2,2,"OVERWORLD",0xFF);
        switch(events_phase()){
        case PHASE_WAITING:{uint16_t s2=events_timer()/50;hal_draw_text(2,12,"Next:",0xFF);hal_draw_number(44,12,(int32_t)(s2+1),0xFF);hal_draw_text(52,12,"s",0xFF);break;}
        case PHASE_SPAWNED:{uint16_t s2=events_timer()/50;hal_draw_text(2,12,"x",0xFF);hal_draw_number(10,12,(int32_t)events_count(),0xFF);
            hal_draw_text(28,12," ",0xFF);hal_draw_number(36,12,(int32_t)(s2+1),0xFF);hal_draw_text(44,12,"s left",0xFF);break;}}
        hal_draw_text(2,SCREEN_H-12,"HP:",0xFF);hal_draw_number(28,SCREEN_H-12,(int32_t)s_player.hp,0xFF);
    }else{
        switch(s_action_reason){
        case ACTION_REASON_COMBAT:hal_draw_text(2,2,"COMBAT:",0xFF);hal_draw_text(60,2,enemy_name(s_last_encounter.enemy_type),0xFF);break;
        case ACTION_REASON_SAFE:hal_draw_text(2,2,safe_zone_name(s_safe_type),0xFF);break;
        case ACTION_REASON_DISCOVERY:hal_draw_text(2,2,"DISCOVERY!",0xFF);break;
        case ACTION_REASON_DUNGEON_FIXED:hal_draw_text(2,2,"DUNGEON Rm:",0xFF);hal_draw_number(92,2,(int32_t)(s_dungeon.current_idx+1),0xFF);
            hal_draw_text(100,2,"/",0xFF);hal_draw_number(108,2,(int32_t)s_dungeon.num_rooms,0xFF);break;
        case ACTION_REASON_DUNGEON_RANDOM:
            if(s_dungeon.in_entry){hal_draw_text(2,2,"DEPTHS ENTRANCE",0xFF);}
            else{hal_draw_text(2,2,"DEPTHS Rm:",0xFF);hal_draw_number(84,2,(int32_t)(s_dungeon.current_idx+1),0xFF);
            hal_draw_text(92,2,"/",0xFF);hal_draw_number(100,2,(int32_t)s_dungeon.num_rooms,0xFF);}break;
        default:hal_draw_text(2,2,"ACTION",0xFF);break;}
        hal_draw_text(2,12,"HP:",0xFF);hal_draw_number(28,12,(int32_t)s_player.hp,0xFF);
        if(s_player.attacking>0)hal_draw_text(SCREEN_W-64,2,"ATK!",0xFF);
    }
    if(s_friendly_dialog)friendly_dialog_draw();
    if(s_bag_open)bag_draw();
    if(s_paused)menu_draw();
}

/*==========================================================================
 * MAIN
 *==========================================================================*/

int main(void){
    uint16_t input,pressed;scene_t last_scene;
    if(hal_init()!=0)return 1;
    rng_seed((uint16_t)(hal_frame_count()+12345));
    s_scene=SCENE_OVERWORLD;s_action_reason=ACTION_REASON_NONE;
    s_map_event_type=MAP_EVENT_FIELD;s_safe_type=SAFE_LONE_CHARACTER;
    s_paused=0;s_bag_open=0;s_friendly_dialog=0;
    s_camera_x=0;s_ow_cam_x=0;s_ow_cam_y=0;
    s_ow_player_x=2*TILE_SIZE;s_ow_player_y=2*TILE_SIZE;
    s_player.hp=PLAYER_START_HP;s_action_npc_count=0;
    s_dungeon.num_rooms=0;s_dungeon.current_idx=0;s_dungeon.in_entry=0;
    overworld_init();last_scene=s_scene;

    for(;;){
        hal_frame_begin();input=hal_input_poll();pressed=hal_input_pressed();
        if((pressed&INPUT_BAG)&&!s_friendly_dialog)s_bag_open=!s_bag_open;
        if((pressed&INPUT_MENU)&&!s_bag_open&&!s_friendly_dialog)s_paused=!s_paused;
        if(!s_paused&&!s_bag_open){switch(s_scene){case SCENE_OVERWORLD:overworld_update(input,pressed);break;case SCENE_ACTION:action_update(input,pressed);break;}}
        if(s_scene!=last_scene){s_paused=0;s_bag_open=0;if(s_scene==SCENE_ACTION)action_init();else overworld_init();last_scene=s_scene;}
        hal_music_update();render();hal_frame_end();rng_next();}
#ifdef _MSC_VER
    __assume(0);
#endif
    return 0;
}
