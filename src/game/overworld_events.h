/*============================================================================
 * overworld_events.h - Overworld Map Event System
 *
 * Manages entities that spawn and roam the overworld map:
 *   - Enemies (weak / medium / strong) that trigger action scenes
 *   - Safe zones (lone character / caravan / oasis town)
 *   - Friendly characters (merchant, healer, sage, wanderer)
 *   - Discoveries (enter a special/secret action map)
 *
 * Spawn lifecycle:
 *   1. On overworld entry, the spawn timer starts counting down.
 *   2. The system cycles:
 *        WAITING → spawn timer counts down (always, even on path tiles)
 *        SPAWNED → group appears, lingers for 6 seconds
 *        DESPAWN → all events vanish, spawn timer resets → WAITING
 *   3. Enemy contact triggers action scene; non-enemy contact is
 *      handled per type (friendly = dialog, safe zone / discovery = map).
 *
 * Spawn distribution is data-driven via spawn_table_t so multiple
 * tables can exist (e.g. different zones, story progression, etc).
 *============================================================================*/

#ifndef OVERWORLD_EVENTS_H
#define OVERWORLD_EVENTS_H

#include "hal/hal_types.h"

/*--------------------------------------------------------------------------
 * Constants
 *--------------------------------------------------------------------------*/

/* Max simultaneous events on the overworld map.
   Each uses one sprite slot (slots 1..MAX_MAP_EVENTS, slot 0 = player). */
#define MAX_MAP_EVENTS     16

/* Spawn timer range: how long WAITING lasts before a group appears */
#define SPAWN_TIMER_MIN   200   /* 4 seconds  */
#define SPAWN_TIMER_MAX   400   /* 8 seconds  */

/* Linger duration: how long spawned events stay alive (frames) */
#define EVENT_LINGER_TIME 300   /* 6 seconds  */

/* Group size range */
#define GROUP_SIZE_MIN      2
#define GROUP_SIZE_MAX      4

/* Movement: how many frames between direction changes */
#define EVENT_MOVE_INTERVAL_MIN  30
#define EVENT_MOVE_INTERVAL_MAX  90

/* Minimum pixel distance from player when spawning */
#define SPAWN_MIN_DISTANCE  48

/*--------------------------------------------------------------------------
 * Event types
 *--------------------------------------------------------------------------*/

typedef enum {
    EVENT_NONE = 0,

    /* Enemies — trigger combat action scene on contact */
    EVENT_ENEMY_WEAK,
    EVENT_ENEMY_MEDIUM,
    EVENT_ENEMY_STRONG,

    /* Non-enemies */
    EVENT_SAFE_ZONE,       /* enter a peaceful action map           */
    EVENT_FRIENDLY,        /* talk to a friendly NPC                */
    EVENT_DISCOVERY,       /* enter a special/secret action map     */
    EVENT_DUNGEON_FIXED,   /* enter a fixed-layout dungeon          */
    EVENT_DUNGEON_RANDOM,  /* enter a randomly-generated dungeon    */

    EVENT_TYPE_COUNT
} event_type_t;

/*--------------------------------------------------------------------------
 * Safe zone subtypes
 *
 * When an event is EVENT_SAFE_ZONE, the subtype field determines
 * which peaceful action map to load.
 *--------------------------------------------------------------------------*/

typedef enum {
    SAFE_LONE_CHARACTER = 0,   /* a single NPC in a quiet clearing     */
    SAFE_CARAVAN,              /* a traveling group with multiple NPCs  */
    SAFE_OASIS_TOWN,           /* a small town with buildings + NPCs    */
    SAFE_TYPE_COUNT
} safe_zone_type_t;

/*--------------------------------------------------------------------------
 * Friendly NPC subtypes
 *
 * When an event is EVENT_FRIENDLY, the subtype field determines
 * which kind of NPC it is.
 *--------------------------------------------------------------------------*/

typedef enum {
    FRIENDLY_MERCHANT = 0, /* buys/sells items                     */
    FRIENDLY_HEALER,       /* restores HP                          */
    FRIENDLY_SAGE,         /* gives hints / lore                   */
    FRIENDLY_WANDERER,     /* random dialogue / quest hooks        */
    FRIENDLY_TYPE_COUNT
} friendly_type_t;

/*--------------------------------------------------------------------------
 * Map event entity
 *--------------------------------------------------------------------------*/

typedef struct {
    uint8_t      active;        /* 0 = slot is free                    */
    event_type_t type;          /* what kind of event                  */
    uint8_t      subtype;       /* safe_zone_type_t / friendly_type_t  */
    int16_t      x, y;          /* pixel position on the overworld     */
    int8_t       move_dx;       /* current movement direction x (-1,0,1) */
    int8_t       move_dy;       /* current movement direction y (-1,0,1) */
    uint8_t      move_timer;    /* frames until next direction change  */
    uint8_t      move_speed;    /* pixels per frame (1 or 2)           */
    uint8_t      sprite_id;     /* HAL sprite slot (1..MAX_MAP_EVENTS) */
    uint8_t      pattern;       /* sprite pattern index                */
} map_event_t;

/*--------------------------------------------------------------------------
 * Spawn phase
 *
 * The event system cycles through two phases:
 *   PHASE_WAITING    → countdown to next group spawn
 *   PHASE_SPAWNED    → events are alive, linger timer counting down
 *--------------------------------------------------------------------------*/

typedef enum {
    PHASE_WAITING = 0,     /* timer counting down to next spawn     */
    PHASE_SPAWNED,         /* events are active, lingering          */
} spawn_phase_t;

/*--------------------------------------------------------------------------
 * Spawn distribution table
 *
 * Defines the probability of each category when a group spawns.
 * Percentages should sum to 100 within each tier.
 *
 * enemy_pct + nonenemy_pct = 100  (what kind of group)
 * weak_pct + medium_pct + strong_pct = 100  (within enemy groups)
 *
 * Multiple tables can exist for different zones or progression stages.
 *--------------------------------------------------------------------------*/

typedef struct {
    /* Group category distribution */
    uint8_t enemy_pct;          /* % chance the group is enemies      */
    /* nonenemy_pct is implicit: 100 - enemy_pct */

    /* Enemy rarity within an enemy group */
    uint8_t weak_pct;           /* % of enemies that are weak         */
    uint8_t medium_pct;         /* % that are medium                  */
    /* strong_pct is implicit: 100 - weak_pct - medium_pct */

    /* Non-enemy distribution */
    uint8_t safe_zone_pct;      /* % of non-enemies that are safe zones */
    uint8_t friendly_pct;       /* % that are friendly NPCs           */
    /* discovery_pct is implicit: 100 - safe_zone_pct - friendly_pct  */

    /* Group size override (0 = use global defaults) */
    uint8_t group_min;
    uint8_t group_max;
} spawn_table_t;

/*--------------------------------------------------------------------------
 * Encounter result
 *
 * When the player touches an event, the system fills this struct
 * so the game knows what action scene to load.
 *--------------------------------------------------------------------------*/

typedef enum {
    ENCOUNTER_NONE = 0,
    ENCOUNTER_COMBAT,      /* fight enemies                        */
    ENCOUNTER_SAFE,        /* safe zone — peaceful action map      */
    ENCOUNTER_FRIENDLY,    /* NPC interaction (no action scene)    */
    ENCOUNTER_DISCOVERY,   /* special action map                   */
    ENCOUNTER_DUNGEON_FIXED,  /* enter fixed-layout dungeon        */
    ENCOUNTER_DUNGEON_RANDOM, /* enter random dungeon              */
} encounter_kind_t;

typedef struct {
    encounter_kind_t kind;
    event_type_t     enemy_type;      /* for COMBAT: which enemy       */
    uint8_t          enemy_subtype;   /* variant / level               */
    friendly_type_t  friendly_type;   /* for FRIENDLY: which NPC       */
    safe_zone_type_t safe_type;       /* for SAFE: which map variant   */
} encounter_t;

/*--------------------------------------------------------------------------
 * API
 *--------------------------------------------------------------------------*/

/* Initialize / reset the event system. Call when entering the overworld. */
void events_init(const uint8_t *map_data, uint16_t map_w, uint16_t map_h,
                 uint8_t path_tile, const spawn_table_t *table);

/* Set a new spawn table (e.g. when entering a different zone). */
void events_set_table(const spawn_table_t *table);

/* Tick the event system once per frame.
   Returns an encounter_t. If kind == ENCOUNTER_NONE, nothing happened. */
encounter_t events_update(int16_t player_x, int16_t player_y);

/* Draw all active event sprites (cam_x/cam_y = overworld scroll). */
void events_draw(int16_t cam_x, int16_t cam_y);

/* Force-despawn all events (e.g. on scene transition). */
void events_clear(void);

/* Query: current spawn phase. */
spawn_phase_t events_phase(void);

/* Query: how many active events are on the map? */
uint8_t events_count(void);

/* Query: frames remaining in current phase timer. */
uint16_t events_timer(void);

/*--------------------------------------------------------------------------
 * Built-in spawn tables
 *--------------------------------------------------------------------------*/

extern const spawn_table_t SPAWN_TABLE_DEFAULT;
extern const spawn_table_t SPAWN_TABLE_DANGEROUS;
extern const spawn_table_t SPAWN_TABLE_PEACEFUL;

#endif /* OVERWORLD_EVENTS_H */
