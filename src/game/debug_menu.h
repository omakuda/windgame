/*============================================================================
 * debug_menu.h - Debug Menu Data & Action Registry
 *
 * Contains:
 *   Main menu item defines + labels
 *   Load sub-category defines + labels
 *   Action map registry for debug spawning
 *   Entry point and display constants
 *
 * Included from main.c after action_reason_t, map_event_type_t,
 * safe_zone_type_t, and event_type_t are defined.
 *============================================================================*/

#ifndef DEBUG_MENU_H
#define DEBUG_MENU_H

/*----------------------------------------------------------------------
 * Main menu items
 *----------------------------------------------------------------------*/
#define DMENU_LOAD_MAP    0
#define DMENU_CHAR_SET    1
#define DMENU_BACK_OW     2
#define DMENU_CONTROLS    3
#define DMENU_HEAL        4
#define DMENU_EXIT        5
#define DMAIN_ITEMS       6
static const char *dmain_labels[DMAIN_ITEMS]={
    "Load Map...","Character Settings...","Back to Overworld",
    "Controls","Heal Full","Exit Game"
};

/*----------------------------------------------------------------------
 * Load sub-categories
 *----------------------------------------------------------------------*/
#define DCAT_WORLD    0
#define DCAT_ACTION   1
#define DCAT_DUNGEON  2
#define DCAT_EVENT    3
#define DCAT_COUNT    4
static const char *dcat_labels[DCAT_COUNT]={
    "World Maps","Action Maps","Dungeons","Events"
};

/*----------------------------------------------------------------------
 * Action map registry for debug loading
 *
 * Each entry defines an action scene the debug menu can spawn:
 * reason + map_type select the scene, safe_type/enemy_type configure it.
 *----------------------------------------------------------------------*/
typedef struct {
    const char *name;
    uint8_t reason;
    uint8_t map_type;
    uint8_t safe_type;
    uint8_t enemy_type;
} debug_action_entry_t;

#define NUM_DEBUG_ACTIONS 10
static const debug_action_entry_t debug_actions[NUM_DEBUG_ACTIONS]={
    {"Combat: Weak",   ACTION_REASON_COMBAT,    MAP_EVENT_FIELD,0,EVENT_ENEMY_WEAK},
    {"Combat: Medium", ACTION_REASON_COMBAT,    MAP_EVENT_FIELD,0,EVENT_ENEMY_MEDIUM},
    {"Combat: Strong", ACTION_REASON_COMBAT,    MAP_EVENT_FIELD,0,EVENT_ENEMY_STRONG},
    {"Safe: Lone NPC", ACTION_REASON_SAFE,      MAP_EVENT_FIELD,SAFE_LONE_CHARACTER,0},
    {"Safe: Caravan",  ACTION_REASON_SAFE,      MAP_EVENT_FIELD,SAFE_CARAVAN,0},
    {"Safe: Oasis",    ACTION_REASON_SAFE,      MAP_EVENT_FIELD,SAFE_OASIS_TOWN,0},
    {"Discovery",      ACTION_REASON_DISCOVERY, MAP_EVENT_FIELD,0,0},
    {"Dungeon Fixed",  ACTION_REASON_DUNGEON_FIXED,MAP_EVENT_DUNGEON_FIXED,0,0},
    {"Dungeon Random", ACTION_REASON_DUNGEON_RANDOM,MAP_EVENT_DUNGEON_RANDOM,0,0},
    {"Dungeon Entry",  ACTION_REASON_DUNGEON_RANDOM,MAP_EVENT_DUNGEON_RANDOM,0,0},
};

/*----------------------------------------------------------------------
 * Entry point options per dungeon room
 *----------------------------------------------------------------------*/
#define DENTRY_BACK  0
#define DENTRY_FWD   1
#define DENTRY_COUNT 2

/*----------------------------------------------------------------------
 * Display constants
 *----------------------------------------------------------------------*/
#define DEBUG_VISIBLE_ROWS 13  /* max rows visible at once */

#endif /* DEBUG_MENU_H */
