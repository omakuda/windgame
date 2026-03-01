/*============================================================================
 * overworld_events.c - Overworld Map Event System Implementation
 *
 * Spawn lifecycle:
 *   PHASE_WAITING    → spawn timer counts down (always, even on path)
 *   PHASE_SPAWNED    → group is alive and roaming, linger timer (6s)
 *                      counts down. When it expires ALL events despawn
 *                      and we return to PHASE_WAITING with a fresh timer.
 *
 * Touching an event during PHASE_SPAWNED triggers an encounter and
 * immediately clears the batch (caller handles the scene switch).
 *============================================================================*/

#include "game/overworld_events.h"
#include "game/rng.h"
#include "hal/hal.h"

/*==========================================================================
 * Built-in spawn tables
 *==========================================================================*/

const spawn_table_t SPAWN_TABLE_DEFAULT = {
    /* enemy_pct */    90,
    /* weak_pct */     60,
    /* medium_pct */   30,
    /* safe_zone_pct */ 40,
    /* friendly_pct */  40,
    /* group_min */     0,
    /* group_max */     0,
};

const spawn_table_t SPAWN_TABLE_DANGEROUS = {
    /* enemy_pct */    95,
    /* weak_pct */     20,
    /* medium_pct */   45,
    /* safe_zone_pct */ 30,
    /* friendly_pct */  30,
    /* group_min */     3,
    /* group_max */     4,
};

const spawn_table_t SPAWN_TABLE_PEACEFUL = {
    /* enemy_pct */    50,
    /* weak_pct */     80,
    /* medium_pct */   15,
    /* safe_zone_pct */ 30,
    /* friendly_pct */  50,
    /* group_min */     2,
    /* group_max */     3,
};

/*==========================================================================
 * Internal state
 *==========================================================================*/

static map_event_t s_events[MAX_MAP_EVENTS];

/* Map reference (not owned — must remain valid) */
static const uint8_t *s_map_data;
static uint16_t s_map_w;
static uint16_t s_map_h;
static uint8_t  s_path_tile;

/* Current spawn distribution */
static const spawn_table_t *s_table;

/* Phase state machine */
static spawn_phase_t s_phase;
static uint16_t      s_phase_timer;    /* countdown for current phase */

/* Sprite pattern assignments for event types */
#define PATTERN_ENEMY_WEAK     4
#define PATTERN_ENEMY_MEDIUM   5
#define PATTERN_ENEMY_STRONG   6
#define PATTERN_SAFE_ZONE      7
#define PATTERN_FRIENDLY       8
#define PATTERN_DISCOVERY      9

/*==========================================================================
 * Internal helpers
 *==========================================================================*/

static uint8_t tile_is_walkable(uint16_t tx, uint16_t ty) {
    uint8_t tile_idx;
    if (tx >= s_map_w || ty >= s_map_h) return 0;
    tile_idx = s_map_data[ty * s_map_w + tx];
    if (tile_idx == 1) return 0;
    return 1;
}

static int8_t find_free_slot(void) {
    uint8_t i;
    for (i = 0; i < MAX_MAP_EVENTS; i++) {
        if (!s_events[i].active) return (int8_t)i;
    }
    return -1;
}

static uint8_t find_spawn_position(int16_t player_x, int16_t player_y,
                                   int16_t *out_x, int16_t *out_y) {
    uint8_t attempts = 20;
    while (attempts > 0) {
        uint16_t tx = rng_range16(1, s_map_w - 2);
        uint16_t ty = rng_range16(1, s_map_h - 2);
        if (tile_is_walkable(tx, ty)) {
            int16_t px = (int16_t)(tx * TILE_SIZE);
            int16_t py = (int16_t)(ty * TILE_SIZE);
            int16_t dx = px - player_x;
            int16_t dy = py - player_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if ((uint16_t)(dx + dy) >= SPAWN_MIN_DISTANCE) {
                *out_x = px;
                *out_y = py;
                return 1;
            }
        }
        attempts--;
    }
    return 0;
}

static event_type_t pick_event_type(uint8_t is_enemy_group) {
    uint8_t roll;
    if (is_enemy_group) {
        roll = rng_range(100);
        if (roll < s_table->weak_pct)
            return EVENT_ENEMY_WEAK;
        else if (roll < s_table->weak_pct + s_table->medium_pct)
            return EVENT_ENEMY_MEDIUM;
        else
            return EVENT_ENEMY_STRONG;
    } else {
        roll = rng_range(100);
        if (roll < s_table->safe_zone_pct)
            return EVENT_SAFE_ZONE;
        else if (roll < s_table->safe_zone_pct + s_table->friendly_pct)
            return EVENT_FRIENDLY;
        else {
            /* Within the "discovery" slice, 30% chance of random dungeon */
            if (rng_chance(30))
                return EVENT_DUNGEON_RANDOM;
            return EVENT_DISCOVERY;
        }
    }
}

static uint8_t pattern_for_type(event_type_t type) {
    switch (type) {
        case EVENT_ENEMY_WEAK:     return PATTERN_ENEMY_WEAK;
        case EVENT_ENEMY_MEDIUM:   return PATTERN_ENEMY_MEDIUM;
        case EVENT_ENEMY_STRONG:   return PATTERN_ENEMY_STRONG;
        case EVENT_SAFE_ZONE:      return PATTERN_SAFE_ZONE;
        case EVENT_FRIENDLY:       return PATTERN_FRIENDLY;
        case EVENT_DISCOVERY:      return PATTERN_DISCOVERY;
        case EVENT_DUNGEON_FIXED:  return PATTERN_DISCOVERY;  /* reuse star */
        case EVENT_DUNGEON_RANDOM: return PATTERN_DISCOVERY;  /* reuse star */
        default:                   return 0;
    }
}

static void randomize_movement(map_event_t *ev) {
    uint8_t dir = rng_range(5);
    ev->move_dx = 0;
    ev->move_dy = 0;
    switch (dir) {
        case 1: ev->move_dy = -1; break;
        case 2: ev->move_dy =  1; break;
        case 3: ev->move_dx = -1; break;
        case 4: ev->move_dx =  1; break;
        default: break;
    }
    ev->move_timer = (uint8_t)rng_range16(EVENT_MOVE_INTERVAL_MIN,
                                           EVENT_MOVE_INTERVAL_MAX);
}

/*--------------------------------------------------------------------------
 * Spawn a group of events near a random location.
 *--------------------------------------------------------------------------*/
static void spawn_group(int16_t player_x, int16_t player_y, uint8_t group_is_enemy) {
    uint8_t group_min, group_max, group_size;
    int16_t anchor_x, anchor_y;
    uint8_t i;

    group_min = (s_table->group_min > 0) ? s_table->group_min : GROUP_SIZE_MIN;
    group_max = (s_table->group_max > 0) ? s_table->group_max : GROUP_SIZE_MAX;
    group_size = (uint8_t)rng_range16(group_min, group_max);

    if (!find_spawn_position(player_x, player_y, &anchor_x, &anchor_y)) {
        return;
    }

    for (i = 0; i < group_size; i++) {
        int8_t slot = find_free_slot();
        map_event_t *ev;
        int16_t ox, oy;

        if (slot < 0) return;

        ev = &s_events[slot];

        ox = anchor_x + (int16_t)((int8_t)rng_range(3) - 1) * TILE_SIZE;
        oy = anchor_y + (int16_t)((int8_t)rng_range(3) - 1) * TILE_SIZE;

        if (ox < TILE_SIZE) ox = TILE_SIZE;
        if (oy < TILE_SIZE) oy = TILE_SIZE;
        if (ox > (int16_t)((s_map_w - 2) * TILE_SIZE))
            ox = (int16_t)((s_map_w - 2) * TILE_SIZE);
        if (oy > (int16_t)((s_map_h - 2) * TILE_SIZE))
            oy = (int16_t)((s_map_h - 2) * TILE_SIZE);

        {
            uint16_t check_tx = (uint16_t)(ox / TILE_SIZE);
            uint16_t check_ty = (uint16_t)(oy / TILE_SIZE);
            if (!tile_is_walkable(check_tx, check_ty)) {
                ox = anchor_x;
                oy = anchor_y;
            }
        }

        ev->active = 1;
        ev->type = pick_event_type(group_is_enemy);
        ev->x = ox;
        ev->y = oy;
        ev->sprite_id = (uint8_t)(slot + 1);
        ev->pattern = pattern_for_type(ev->type);

        /* Assign subtype */
        if (ev->type == EVENT_FRIENDLY) {
            ev->subtype = rng_range(FRIENDLY_TYPE_COUNT);
        } else if (ev->type == EVENT_SAFE_ZONE) {
            ev->subtype = rng_range(SAFE_TYPE_COUNT);
        } else {
            ev->subtype = 0;
        }

        /* Movement — all events roam at player speed (2 px/frame) */
        ev->move_speed = 2;
        randomize_movement(ev);
    }
}

/*--------------------------------------------------------------------------
 * Move a single event, respecting map boundaries and solid tiles.
 *--------------------------------------------------------------------------*/
static void move_event(map_event_t *ev) {
    int16_t nx, ny;
    uint16_t check_tx, check_ty;

    if (ev->move_speed == 0) return;

    if (ev->move_timer > 0) {
        ev->move_timer--;
    } else {
        randomize_movement(ev);
    }

    nx = ev->x + (int16_t)(ev->move_dx * ev->move_speed);
    ny = ev->y + (int16_t)(ev->move_dy * ev->move_speed);

    check_tx = (uint16_t)((nx + SPRITE_W / 2) / TILE_SIZE);
    check_ty = (uint16_t)((ny + SPRITE_H / 2) / TILE_SIZE);

    if (tile_is_walkable(check_tx, check_ty)) {
        ev->x = nx;
        ev->y = ny;
    } else {
        randomize_movement(ev);
    }

    if (ev->x < TILE_SIZE) ev->x = TILE_SIZE;
    if (ev->y < TILE_SIZE) ev->y = TILE_SIZE;
    if (ev->x > (int16_t)((s_map_w - 2) * TILE_SIZE))
        ev->x = (int16_t)((s_map_w - 2) * TILE_SIZE);
    if (ev->y > (int16_t)((s_map_h - 2) * TILE_SIZE))
        ev->y = (int16_t)((s_map_h - 2) * TILE_SIZE);
}

/*--------------------------------------------------------------------------
 * AABB overlap between player and event (shrunken hitboxes).
 *--------------------------------------------------------------------------*/
#define EVENT_HB_SHRINK  4

static uint8_t player_touches_event(int16_t px, int16_t py, map_event_t *ev) {
    int16_t p_left   = px + EVENT_HB_SHRINK;
    int16_t p_right  = px + SPRITE_W - EVENT_HB_SHRINK;
    int16_t p_top    = py + EVENT_HB_SHRINK;
    int16_t p_bottom = py + SPRITE_H - EVENT_HB_SHRINK;

    int16_t e_left   = ev->x + EVENT_HB_SHRINK;
    int16_t e_right  = ev->x + SPRITE_W - EVENT_HB_SHRINK;
    int16_t e_top    = ev->y + EVENT_HB_SHRINK;
    int16_t e_bottom = ev->y + SPRITE_H - EVENT_HB_SHRINK;

    if (p_right <= e_left || p_left >= e_right) return 0;
    if (p_bottom <= e_top || p_top >= e_bottom) return 0;
    return 1;
}

/*--------------------------------------------------------------------------
 * Clear all active events and hide their sprites.
 *--------------------------------------------------------------------------*/
static void clear_all_events(void) {
    uint8_t i;
    for (i = 0; i < MAX_MAP_EVENTS; i++) {
        if (s_events[i].active) {
            hal_sprite_show(s_events[i].sprite_id, false);
            s_events[i].active = 0;
        }
    }
}

/*--------------------------------------------------------------------------
 * Transition to PHASE_WAITING with a fresh random timer.
 *--------------------------------------------------------------------------*/
static void enter_waiting(void) {
    s_phase = PHASE_WAITING;
    s_phase_timer = (uint16_t)rng_range16(SPAWN_TIMER_MIN, SPAWN_TIMER_MAX);
}

/*==========================================================================
 * PUBLIC API
 *==========================================================================*/

void events_init(const uint8_t *map_data, uint16_t map_w, uint16_t map_h,
                 uint8_t path_tile, const spawn_table_t *table) {
    uint8_t i;

    s_map_data  = map_data;
    s_map_w     = map_w;
    s_map_h     = map_h;
    s_path_tile = path_tile;
    s_table     = table;

    for (i = 0; i < MAX_MAP_EVENTS; i++) {
        s_events[i].active = 0;
    }

    /* Start in waiting phase with a random spawn timer */
    s_phase = PHASE_WAITING;
    s_phase_timer = (uint16_t)rng_range16(SPAWN_TIMER_MIN, SPAWN_TIMER_MAX);
}

void events_set_table(const spawn_table_t *table) {
    s_table = table;
}

encounter_t events_update(int16_t player_x, int16_t player_y) {
    encounter_t result;
    uint8_t i;

    result.kind = ENCOUNTER_NONE;
    result.enemy_type = EVENT_NONE;
    result.enemy_subtype = 0;
    result.friendly_type = FRIENDLY_MERCHANT;
    result.safe_type = SAFE_LONE_CHARACTER;

    /*==================================================================
     * PHASE_WAITING: countdown to next spawn (always ticks)
     *==================================================================*/
    if (s_phase == PHASE_WAITING) {
        if (s_phase_timer > 0) {
            s_phase_timer--;
        } else {
            /* Timer expired — spawn a group and enter SPAWNED */
            uint8_t is_enemy = rng_chance(s_table->enemy_pct);
            spawn_group(player_x, player_y, is_enemy);

            s_phase = PHASE_SPAWNED;
            s_phase_timer = EVENT_LINGER_TIME;
        }
        return result;
    }

    /*==================================================================
     * PHASE_SPAWNED: events are alive, linger timer counting down
     *==================================================================*/
    if (s_phase == PHASE_SPAWNED) {
        /* Count down the linger timer */
        if (s_phase_timer > 0) {
            s_phase_timer--;
        }

        /* If linger expired, despawn everything and reset */
        if (s_phase_timer == 0) {
            clear_all_events();
            enter_waiting();
            return result;
        }

        /* Update all active events: move + check player collision */
        for (i = 0; i < MAX_MAP_EVENTS; i++) {
            map_event_t *ev = &s_events[i];

            if (!ev->active) continue;

            move_event(ev);

            /* Check collision with player */
            if (player_touches_event(player_x, player_y, ev)) {
                switch (ev->type) {
                    case EVENT_ENEMY_WEAK:
                    case EVENT_ENEMY_MEDIUM:
                    case EVENT_ENEMY_STRONG:
                        result.kind = ENCOUNTER_COMBAT;
                        result.enemy_type = ev->type;
                        result.enemy_subtype = ev->subtype;
                        break;

                    case EVENT_SAFE_ZONE:
                        result.kind = ENCOUNTER_SAFE;
                        result.safe_type = (safe_zone_type_t)ev->subtype;
                        break;

                    case EVENT_FRIENDLY:
                        result.kind = ENCOUNTER_FRIENDLY;
                        result.friendly_type = (friendly_type_t)ev->subtype;
                        break;

                    case EVENT_DISCOVERY:
                        result.kind = ENCOUNTER_DISCOVERY;
                        break;

                    case EVENT_DUNGEON_FIXED:
                        result.kind = ENCOUNTER_DUNGEON_FIXED;
                        break;

                    case EVENT_DUNGEON_RANDOM:
                        result.kind = ENCOUNTER_DUNGEON_RANDOM;
                        break;

                    default:
                        break;
                }

                /* Clear entire batch on any encounter */
                clear_all_events();
                enter_waiting();
                return result;
            }
        }
    }

    return result;
}

void events_draw(int16_t cam_x, int16_t cam_y) {
    uint8_t i;

    for (i = 0; i < MAX_MAP_EVENTS; i++) {
        map_event_t *ev = &s_events[i];
        sprite_desc_t spr;

        if (!ev->active) {
            hal_sprite_show((uint8_t)(i + 1), false);
            continue;
        }

        spr.id      = ev->sprite_id;
        spr.x       = ev->x - cam_x;
        spr.y       = ev->y - cam_y;
        spr.pattern = ev->pattern;
        spr.palette = 0;
        spr.flags   = SPRITE_FLAG_VISIBLE;

        /* Blink events during last second of linger (visual warning) */
        if (s_phase == PHASE_SPAWNED && s_phase_timer < 50 &&
            (s_phase_timer & 0x04)) {
            spr.flags &= ~SPRITE_FLAG_VISIBLE;
        }

        if (ev->move_dx < 0) {
            spr.flags |= SPRITE_FLAG_MIRROR_X;
        }

        hal_sprite_set(&spr);
    }
}

void events_clear(void) {
    clear_all_events();
    enter_waiting();
}

spawn_phase_t events_phase(void) {
    return s_phase;
}

uint8_t events_count(void) {
    uint8_t i, count = 0;
    for (i = 0; i < MAX_MAP_EVENTS; i++) {
        if (s_events[i].active) count++;
    }
    return count;
}

uint16_t events_timer(void) {
    return s_phase_timer;
}
