/*============================================================================
 * main.c - Game Entry Point (Platform-Independent)
 *
 * TABLE OF CONTENTS (search for "=== SECTION" to jump between sections):
 *
 *   === TYPES & ENUMS ===        Scene, action, player, item, map types
 *   === CONFIGURATION ===        Tile collision, hitbox, physics
 *                                 #include "tunables.h" (tunable_t, T, tvars)
 *   === MAP DATA ===             #include "maps.h"
 *   === GAME STATE ===           All static variables, grouped by system
 *   === FORWARD DECLARATIONS === All forward-declared functions
 *   === COLLISION HELPERS ===    Tile lookup, box-vs-flag tests
 *   === TILE MANAGEMENT ===      Overworld tile build/swap for action scenes
 *   === FRIENDLY NPC DIALOG ===  NPC names, lines, dialog box, effects
 *   === OVERWORLD SCENE ===      Init, camera, movement, encounters
 *   === DUNGEON SYSTEM ===       Room loading, fixed/random init, transitions
 *   === ACTION SCENE ===         Init, physics, movement, collision, combat
 *   === ARROWS & PLACED LADDER = Projectile and placeable item systems
 *   === EQUIPMENT USE ===        BTN4/5/6 dispatch
 *   === INVENTORY ===            Init, bag slots, equip slots
 *   === BAG / PAUSE MENU ===     Bag UI, equip HUD, pause overlay
 *   === DEBUG MENU ===           #include "debug_menu.h" + menu functions
 *   === DEBUG CONSOLE ===        Runtime parameter editor (tunable_t)
 *   === KEYBIND EDITOR ===       Control rebinding UI
 *   === RENDER ===               Full frame draw
 *   === MAIN LOOP ===            Entry point and game loop
 *
 * Scenes:
 *   OVERWORLD  -- top-down scrolling map with spawning events
 *   ACTION     -- sidescrolling platformer (field / dungeon)
 *
 * Action scene types (map_event_type_t):
 *   FIELD   (1)  -- open area with exit tiles on L/R borders.
 *                   Enemies, safe zones, discoveries load here.
 *   DUNGEON_FIXED (2) -- pre-defined room order (type 1).
 *   DUNGEON_RANDOM(3) -- rooms picked from pool, randomly connected (type 2).
 *                   Transition tiles move between rooms.
 *
 * Spawn cycle: timer -> spawn group -> linger 5s -> despawn -> timer resets
 *============================================================================*/

#include "hal/hal.h"
#include "game/rng.h"
#include "game/overworld_events.h"
#include <stdlib.h>

/*==========================================================================
 * === TYPES & ENUMS ===
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

typedef enum {
    ITEM_NONE=0,
    ITEM_BOW,       /* fires arrow in facing direction from upper body */
    ITEM_LADDER,    /* places/retracts a climbable ladder in front */
    ITEM_COUNT
} item_id_t;

/*--- Player hitbox ---
 * Action scenes: 16x32 sprite (two 16x16 slots stacked).
 * Hitbox is inset from the sprite edges.
 * Overworld: uses OW_PLAYER_SIZE/OFFSET (8x8 centred in 16x16). */

#define PLAYER_HB_X_OFFSET  3
#define PLAYER_HB_Y_OFFSET  2
#define PLAYER_HB_W         10
#define PLAYER_HB_H         28   /* tall hitbox for 16x32 sprite */
#define PLAYER_SPRITE_H     32   /* total sprite height in action scenes */
/* Crouch hitbox: one tile tall, aligned with bottom sprite (y+16) */
#define PLAYER_CROUCH_HB_Y_OFFSET  16  /* starts at bottom sprite */
#define PLAYER_CROUCH_HB_H         14  /* one tile minus margin */
#define PATTERN_ACT_TOP      2   /* sprite pattern: action player top half  */
#define PATTERN_ACT_BOT      3   /* sprite pattern: action player bottom    */
#define PATTERN_ACT_CROUCH   14  /* sprite pattern: action player crouching */

typedef struct {
    int16_t x, y, vel_x, vel_y;
    int16_t vel_fx;  /* horizontal velocity in 8.8 fixed-point (action scenes) */
    int16_t vel_fy;  /* vertical velocity in 8.8 fixed-point (action scenes)   */
    uint8_t dir, on_ground, on_ladder, attacking, frame;
    uint8_t crouching;
    int16_t hp;
    uint8_t invuln;
    int16_t jump_vel_fx; /* vel_fx captured at jump, used to limit air speed */
} player_t;

/* World map descriptor */
typedef struct {
    const uint8_t *data;
    uint16_t w, h;
    const char *name;
} world_map_t;
typedef struct {
    const uint8_t *map;
    uint16_t w, h;
    int16_t entry_back_x, entry_back_y;
    int16_t entry_fwd_x, entry_fwd_y;
    const char *name;          /* room display name */
    const char *label_back;    /* entry point label: back/left */
    const char *label_fwd;     /* entry point label: forward/right */
} dungeon_room_def_t;
#define MAX_DUNGEON_ROOMS 8
typedef struct {
    uint8_t num_rooms;
    uint8_t room_order[MAX_DUNGEON_ROOMS];
    uint8_t current_idx;
    uint8_t in_entry;   /* 1 = in type-2 entry room (has exit+transition) */
} dungeon_state_t;

/* Arrow projectile */
typedef struct {
    int16_t x, y, vel_x;
    uint8_t active, timer;
} arrow_t;

#define PLACED_LADDER_MAX_H  6  /* max tiles in any direction */

typedef struct {
    uint8_t active;
    uint16_t tiles_x[PLACED_LADDER_MAX_H]; /* tile x per segment */
    uint16_t tiles_y[PLACED_LADDER_MAX_H]; /* tile y per segment */
    uint8_t  orig[PLACED_LADDER_MAX_H];    /* original tile at each position */
    uint8_t count;                          /* number of ladder tiles placed */
} placed_ladder_t;

/*==========================================================================
 * === CONFIGURATION ===
 *
 * All tunable values in one place. To adjust gameplay feel, edit here.
 *==========================================================================*/

/*--- Tile collision table ---*/
#define NUM_TILE_TYPES  26

static const uint8_t tile_collision[NUM_TILE_TYPES] = {
    /* 0 */ TILE_EMPTY,
    /* 1 */ TILE_SOLID,
    /* 2 */ TILE_SOLID,
    /* 3 */ TILE_SOLID,
    /* 4 */ TILE_PLATFORM,
    /* 5 */ TILE_LADDER,
    /* 6 */ TILE_DAMAGE,
    /* 7 */ TILE_EMPTY,        /* path / decoration / ice in action */
    /* 8 */ TILE_EXIT,         /* exit trigger            */
    /* 9 */ TILE_TRANSITION,   /* room transition / dungeon entrance */
    /*10 */ TILE_WATER,        /* water — passable, bidirectional */
    /*11 */ TILE_EMPTY,        /* town entrance (overworld only) */
    /*12 */ TILE_EMPTY,        /* hidden area entrance     */
    /*13 */ TILE_EMPTY,        /* forest entrance (overworld only) */
    /*14 */ TILE_SOLID,        /* brick        */
    /*15 */ TILE_SOLID,        /* bark         */
    /*16 */ TILE_SOLID,        /* stone_wall   */
    /*17 */ TILE_SOLID,        /* wood_plank   */
    /*18 */ TILE_EMPTY,        /* grass        */
    /*19 */ TILE_EMPTY,        /* sand         */
    /*20 */ TILE_SOLID,        /* roof         */
    /*21 */ TILE_EMPTY,        /* cobblestone  */
    /*22 */ TILE_EMPTY,        /* dirt         */
    /*23 */ TILE_SOLID,        /* window       */
    /*24 */ TILE_EMPTY,        /* world_water (overworld, passable) */
    /*25 */ TILE_EMPTY,        /* world_road (overworld, passable)  */
};

/*----------------------------------------------------------------------
 * Sprite Patterns
 *----------------------------------------------------------------------*/
#define PATTERN_OW_PLAYER    1   /* overworld player        */
#define PATTERN_FRIENDLY     8
#define PATTERN_NPC_MERCHANT  10
#define PATTERN_NPC_HEALER    11
#define PATTERN_NPC_SAGE      12
#define PATTERN_NPC_WANDERER  13

/*----------------------------------------------------------------------
 * Overworld Player — 8x8 visual centred in 16x16 sprite slot.
 * Uses PATTERN_OW_PLAYER with only the centre 8x8 filled.
 *----------------------------------------------------------------------*/
#define OW_PLAYER_SIZE    8
#define OW_PLAYER_OFFSET  4   /* (16-8)/2 -- centering in 16x16 sprite */
#define OW_MOVE_SPEED     1   /* 75% of 2: alternate 1 and 2 px/frame  */

/*----------------------------------------------------------------------
 * Combat / Movement Defaults
 *----------------------------------------------------------------------*/
#define MOVE_SPEED 2
#define CLIMB_SPEED 2
#define ATTACK_DURATION_DEF 12
#define INVULN_TIME_DEF 30
#define PLAYER_START_HP 10

/*----------------------------------------------------------------------
 * Horizontal momentum — values now in tunable_t T (see above)
 *----------------------------------------------------------------------*/
#define FX_SHIFT       8
#define FX_ONE         (1 << FX_SHIFT)           /* 256  */
#define FX_DECEL       9999                       /* instant stop on ground */
#define FX_TO_PX(v)    ((int16_t)((v) / FX_ONE))  /* symmetric toward-zero truncation */

#include "game/tunables.h"

/*----------------------------------------------------------------------
 * Arrow / Projectile Config
 *----------------------------------------------------------------------*/
#define MAX_ARROWS     1
#define ARROW_SPEED    4
#define ARROW_LIFETIME 60   /* frames before despawn */
#define ARROW_SPAWN_Y_STANDING 8   /* px from player top -- upper body */
#define ARROW_SPAWN_Y_CROUCH  20  /* px from player top -- lower body when crouched */

/*----------------------------------------------------------------------
 * Inventory Config
 *----------------------------------------------------------------------*/
#define BAG_SLOTS     8
#define EQUIP_SLOTS   3

/*----------------------------------------------------------------------
 * NPC / Action Scene Config
 *----------------------------------------------------------------------*/
#define ACTION_NPC_SPRITE_BASE 2  /* slots 0,1 = action player (top,bottom) */
#define ACTION_NPC_MAX 8
#define TRANSITION_FRAMES 12  /* ~0.25s at 50Hz */

/*==========================================================================
 * === MAP DATA ===
 * All world maps, field maps, safe zones, dungeons, interiors.
 * Types (world_map_t, dungeon_room_def_t) must be defined above.
 *==========================================================================*/

#include "game/maps.h"

/*==========================================================================
 * === GAME STATE ===
 * All static variables, grouped by subsystem.
 *==========================================================================*/

/*--- Player ---*/
static player_t s_player;
static int16_t s_ow_player_x, s_ow_player_y;

/*--- Scene / Game ---*/
static scene_t s_scene;
static action_reason_t s_action_reason;
static map_event_type_t s_map_event_type;
static safe_zone_type_t s_safe_type;
static encounter_t s_last_encounter;
static uint8_t s_paused, s_bag_open, s_friendly_dialog;
static uint8_t s_transition_timer;
static uint8_t s_force_reinit;
static uint8_t s_cur_world_map = 0;

/*--- Camera ---*/
static int16_t s_camera_x, s_ow_cam_x, s_ow_cam_y;

/*--- Action Map ---*/
static uint16_t s_act_map_w, s_act_map_h;
static const char *s_act_map_name;

/*--- Building Interior ---*/
static uint8_t s_in_building;
static int16_t s_building_px, s_building_py;
static uint16_t s_building_map_w, s_building_map_h;
static const uint8_t *s_building_outer_map;
static int16_t s_building_cam_x;

/*--- Location Re-entry Prevention ---*/
static uint16_t s_immune_tx, s_immune_ty;
static uint8_t s_immune_active;

/*--- Action NPCs ---*/
static uint8_t s_action_npc_count;
static int16_t s_action_npc_wx[ACTION_NPC_MAX];
static int16_t s_action_npc_wy[ACTION_NPC_MAX];
static uint8_t s_action_npc_pat[ACTION_NPC_MAX];

/*--- Dungeon ---*/
static dungeon_state_t s_dungeon;

/*--- Overworld ---*/
static uint8_t s_ow_frame_toggle;

/*--- Equipment / Inventory ---*/
static item_id_t s_bag[BAG_SLOTS];
static item_id_t s_equip[EQUIP_SLOTS];
static uint8_t s_bag_cursor;

/*--- Arrows ---*/
static arrow_t s_arrows[MAX_ARROWS];

/*--- Placed Ladder ---*/
static placed_ladder_t s_placed_ladder;

/*--- Overworld Tile Patterns ---*/
static const uint8_t s_blank_tile[TILE_SIZE * TILE_SIZE] = {0};
static uint8_t s_ow_tile_town[TILE_SIZE * TILE_SIZE];
static uint8_t s_ow_tile_forest[TILE_SIZE * TILE_SIZE];
static uint8_t s_ow_tiles_built = 0;

/*--- Debug Menu ---*/
static uint8_t s_debug_menu;
static uint8_t s_debug_cursor;
static uint8_t s_debug_scroll;
static uint8_t s_debug_level;
static uint8_t s_debug_cat;
static uint8_t s_debug_sub;
static uint8_t s_debug_room;

/*--- Debug Console ---*/
static uint8_t s_console_open;
static uint8_t s_console_cursor;
static uint8_t s_console_scroll;

/*--- Keybind Editor ---*/
static uint8_t s_keybind_editor;
static uint8_t s_keybind_cursor;
static uint8_t s_keybind_waiting;

/*==========================================================================
 * === FORWARD DECLARATIONS ===
 *==========================================================================*/
static void action_clear_ow_tiles(void);
static void ow_restore_tiles(void);
static void arrows_clear(void);
static void arrows_update(void);
static void arrows_draw(int16_t cam_x);
static void arrow_fire(void);
static void placed_ladder_clear(void);
static void placed_ladder_apply(void);
static void placed_ladder_place(uint16_t input);
static void placed_ladder_remove(void);
static uint8_t player_on_placed_ladder(void);
static void use_equip(uint8_t slot, uint16_t pressed, uint16_t input);
static void inventory_init(void);
static void bag_input(uint16_t pressed);
static void bag_draw(void);
static void equip_hud_draw(void);
static void menu_draw(void);
static void debug_draw(void);
static void debug_input(uint16_t pressed);
static void console_draw(void);
static void console_input(uint16_t pressed, uint16_t input);
static void keybind_draw(void);
static void keybind_input(uint16_t pressed);
static void friendly_dialog_draw(void);
static void render(void);

/*==========================================================================
 * === COLLISION HELPERS ===
 *==========================================================================*/
static uint8_t tile_flags(uint8_t tile_idx) {
    if (tile_idx >= NUM_TILE_TYPES) return TILE_EMPTY;
    return tile_collision[tile_idx];
}

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
 * === TILE MANAGEMENT ===
 *==========================================================================*/
static void ow_tiles_build(void) {
    /* Build once: generate the same patterns the HAL creates at init */
    int px, py;
    if (s_ow_tiles_built) return;
    /* Town tile 11 */
    for (py = 0; py < TILE_SIZE; py++) {
        for (px = 0; px < TILE_SIZE; px++) {
            uint8_t c = ((px+py)&1) ? 0xFD : 0xFD; /* base town checkerboard */
            if (py <= 3 && px >= 3 && px <= 12) c = 0xFC;       /* roof */
            else if (py > 3 && py <= 12 && (px == 3 || px == 12)) c = 0xE8; /* walls */
            else if (py >= 10 && px >= 6 && px <= 9) c = 0x64;  /* door */
            else if (py > 3 && py < 10 && px > 3 && px < 12) c = 0xED; /* wall fill */
            s_ow_tile_town[py * TILE_SIZE + px] = c;
        }
    }
    /* Forest tile 13 */
    for (py = 0; py < TILE_SIZE; py++) {
        for (px = 0; px < TILE_SIZE; px++) {
            uint8_t c = ((px+py)&1) ? 0x04 : 0x28; /* base forest checkerboard */
            {int cx2 = px - 7, cy2 = py - 4;
            if (cx2*cx2 + cy2*cy2 <= 20) c = 0x24;              /* canopy */
            else if (px >= 6 && px <= 9 && py >= 10) c = 0x64;} /* trunk */
            s_ow_tile_forest[py * TILE_SIZE + px] = c;
        }
    }
    s_ow_tiles_built = 1;
}

static void action_clear_ow_tiles(void) {
    hal_tiles_load(s_blank_tile, 11, 1);  /* blank town */
    hal_tiles_load(s_blank_tile, 13, 1);  /* blank forest */
}

static void ow_restore_tiles(void) {
    ow_tiles_build();
    hal_tiles_load(s_ow_tile_town,   11, 1);
    hal_tiles_load(s_ow_tile_forest, 13, 1);
}

/*==========================================================================
 * === FRIENDLY NPC DIALOG ===
 *==========================================================================*/
static const char *item_name(item_id_t id){
    switch(id){case ITEM_BOW:return"Bow";case ITEM_LADDER:return"Ladder";default:return"---";}
}
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
    switch(et){case EVENT_ENEMY_WEAK:return"WEAK FOE";case EVENT_ENEMY_MEDIUM:return"MEDIUM FOE";
    case EVENT_ENEMY_STRONG:return"BOSS FOE";default:return"???";}
}
static const char *safe_zone_name(safe_zone_type_t st) {
    switch(st){case SAFE_LONE_CHARACTER:return"QUIET CLEARING";case SAFE_CARAVAN:return"CARAVAN";
    case SAFE_OASIS_TOWN:return"OASIS TOWN";default:return"SAFE ZONE";}
}

/*==========================================================================
 * === OVERWORLD SCENE ===
 *==========================================================================*/

static void overworld_init(void) {
    const world_map_t *wm=&world_maps[s_cur_world_map];
    hal_sprite_hide_all();
    ow_restore_tiles(); /* reload town/forest tile patterns after action cleared them */
    hal_tilemap_set(wm->data, wm->w, wm->h);
    s_player.x=s_ow_player_x; s_player.y=s_ow_player_y;
    s_player.vel_x=0; s_player.vel_y=0; s_player.vel_fx=0; s_player.vel_fy=0; s_player.dir=0;
    s_player.on_ground=0; s_player.attacking=0; s_player.frame=0;
    s_friendly_dialog=0;s_ow_frame_toggle=0;
    /* Set immunity on the tile player is standing on to prevent re-entry */
    {int16_t cx=s_player.x+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
     int16_t cy=s_player.y+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
     s_immune_tx=(uint16_t)(cx/TILE_SIZE);s_immune_ty=(uint16_t)(cy/TILE_SIZE);
     s_immune_active=1;
     /* Try to spawn a traveling event on re-entry */
     events_try_spawn_traveling(s_immune_tx,s_immune_ty);
    }
    events_init(wm->data,wm->w,wm->h,OW_PATH_TILE,&SPAWN_TABLE_DEFAULT);
}

static void overworld_update_camera(void) {
    const world_map_t *wm=&world_maps[s_cur_world_map];
    int16_t max_x=(int16_t)(wm->w*TILE_SIZE)-SCREEN_W;
    int16_t max_y=(int16_t)(wm->h*TILE_SIZE)-SCREEN_H;
    /* Centre camera on the 8x8 visual centre of the player */
    s_ow_cam_x=s_player.x+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2-SCREEN_W/2;
    s_ow_cam_y=s_player.y+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2-SCREEN_H/2;
    if(s_ow_cam_x<0)s_ow_cam_x=0; if(s_ow_cam_y<0)s_ow_cam_y=0;
    if(s_ow_cam_x>max_x)s_ow_cam_x=max_x; if(s_ow_cam_y>max_y)s_ow_cam_y=max_y;
    hal_tilemap_scroll(s_ow_cam_x,s_ow_cam_y);
}

static void overworld_update(uint16_t input, uint16_t pressed) {
    const world_map_t *wm=&world_maps[s_cur_world_map];
    int16_t nx,ny; encounter_t enc;
    int16_t spd;
    if(s_friendly_dialog){if(pressed&INPUT_BTN1){friendly_apply_effect(s_last_encounter.friendly_type);s_friendly_dialog=0;}return;}

    /* 75% of 2 px/frame: alternate 2,1,2,1 → average 1.5 px/frame */
    s_ow_frame_toggle=(s_ow_frame_toggle+1)&3;
    spd=(s_ow_frame_toggle==0)?2:1;

    nx=s_player.x; ny=s_player.y;
    if(input&INPUT_UP)ny-=spd; if(input&INPUT_DOWN)ny+=spd;
    if(input&INPUT_LEFT){nx-=spd;s_player.dir=1;} if(input&INPUT_RIGHT){nx+=spd;s_player.dir=0;}

    /* Collision uses 8x8 centre of the 16x16 slot */
    if(!box_hits_flag(nx+OW_PLAYER_OFFSET,s_player.y+OW_PLAYER_OFFSET,OW_PLAYER_SIZE,OW_PLAYER_SIZE,TILE_SOLID))s_player.x=nx;
    if(!box_hits_flag(s_player.x+OW_PLAYER_OFFSET,ny+OW_PLAYER_OFFSET,OW_PLAYER_SIZE,OW_PLAYER_SIZE,TILE_SOLID))s_player.y=ny;

    if(s_player.x<0)s_player.x=0; if(s_player.y<0)s_player.y=0;
    if(s_player.x>(wm->w*TILE_SIZE)-SPRITE_W)s_player.x=(wm->w*TILE_SIZE)-SPRITE_W;
    if(s_player.y>(wm->h*TILE_SIZE)-SPRITE_H)s_player.y=(wm->h*TILE_SIZE)-SPRITE_H;

    /* Check for permanent locations (tile 9=dungeon, 11=town, 12=hidden, 13=forest) */
    {int16_t cx=s_player.x+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
     int16_t cy=s_player.y+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
     uint16_t tx=(uint16_t)(cx/TILE_SIZE),ty=(uint16_t)(cy/TILE_SIZE);
     /* Clear immunity when player leaves the immune tile */
     if(s_immune_active&&(tx!=s_immune_tx||ty!=s_immune_ty))s_immune_active=0;
     if(!s_immune_active&&tx<wm->w&&ty<wm->h){
        uint8_t ot=wm->data[ty*wm->w+tx];
        if(ot==9){
            s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
            s_action_reason=ACTION_REASON_DUNGEON_FIXED;s_map_event_type=MAP_EVENT_DUNGEON_FIXED;
            events_clear();s_scene=SCENE_ACTION;return;}
        if(ot==11){
            s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
            s_action_reason=ACTION_REASON_SAFE;s_map_event_type=MAP_EVENT_FIELD;s_safe_type=SAFE_OASIS_TOWN;
            events_clear();s_scene=SCENE_ACTION;return;}
        if(ot==12){
            s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
            s_action_reason=ACTION_REASON_DISCOVERY;s_map_event_type=MAP_EVENT_FIELD;
            events_clear();s_scene=SCENE_ACTION;return;}
        if(ot==13){
            s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
            s_action_reason=ACTION_REASON_COMBAT;s_map_event_type=MAP_EVENT_FIELD;
            s_last_encounter.kind=ENCOUNTER_COMBAT;s_last_encounter.enemy_type=EVENT_ENEMY_MEDIUM;
            events_clear();s_scene=SCENE_ACTION;return;}
    }}

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

    /* Check for traveling events (invisible tile events) */
    if(s_scene==SCENE_OVERWORLD&&!s_friendly_dialog){
        int16_t pcx=s_player.x+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
        int16_t pcy=s_player.y+OW_PLAYER_OFFSET+OW_PLAYER_SIZE/2;
        uint16_t ptx=(uint16_t)(pcx/TILE_SIZE),pty=(uint16_t)(pcy/TILE_SIZE);
        uint8_t tev=events_check_traveling(ptx,pty);
        if(tev>0){
            /* Traveling event triggered — load a combat stage for now.
               TODO: map event_id to specific stages/pools via spawn zone config */
            s_ow_player_x=s_player.x;s_ow_player_y=s_player.y;
            s_action_reason=ACTION_REASON_COMBAT;s_map_event_type=MAP_EVENT_FIELD;
            s_last_encounter.kind=ENCOUNTER_COMBAT;
            s_last_encounter.enemy_type=(tev%3==0)?EVENT_ENEMY_STRONG:(tev%3==1)?EVENT_ENEMY_MEDIUM:EVENT_ENEMY_WEAK;
            events_clear();s_scene=SCENE_ACTION;
            /* After leaving this event, another spawn check will happen in overworld_init */
        }
    }
}

/*==========================================================================
 * === DUNGEON SYSTEM ===
 *==========================================================================*/
static uint8_t npc_pattern_for_friendly(friendly_type_t ft){
    switch(ft){case FRIENDLY_MERCHANT:return PATTERN_NPC_MERCHANT;case FRIENDLY_HEALER:return PATTERN_NPC_HEALER;
    case FRIENDLY_SAGE:return PATTERN_NPC_SAGE;case FRIENDLY_WANDERER:return PATTERN_NPC_WANDERER;default:return PATTERN_FRIENDLY;}
}

static void place_action_npcs(const int16_t *xs,const int16_t *ys,uint8_t count){
    uint8_t i;s_action_npc_count=count;
    for(i=0;i<count&&i<ACTION_NPC_MAX;i++){
        s_action_npc_wx[i]=xs[i];s_action_npc_wy[i]=ys[i];
        /* Assign unique pattern per NPC type based on index */
        s_action_npc_pat[i]=npc_pattern_for_friendly((friendly_type_t)(i%FRIENDLY_TYPE_COUNT));
        {sprite_desc_t npc;npc.id=ACTION_NPC_SPRITE_BASE+i;npc.x=xs[i];npc.y=ys[i];
        npc.pattern=s_action_npc_pat[i];npc.palette=0;npc.flags=SPRITE_FLAG_VISIBLE;hal_sprite_set(&npc);}}}

static void load_dungeon_room(uint8_t pool_idx,uint8_t from_fwd){
    const dungeon_room_def_t *room=&room_pool[pool_idx];
    hal_sprite_hide_all();s_action_npc_count=0;
    hal_tilemap_set(room->map,room->w,room->h);
    s_act_map_w=room->w;s_act_map_h=room->h;
    if(from_fwd){s_player.x=room->entry_back_x;s_player.y=room->entry_back_y;}
    else{s_player.x=room->entry_fwd_x;s_player.y=room->entry_fwd_y;}
    s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;s_player.crouching=0;
    s_camera_x=0;s_dungeon.in_entry=0;
}

static void load_entry_room(void){
    hal_sprite_hide_all();s_action_npc_count=0;
    hal_tilemap_set(dungeon_entry_room,ENTRY_W,ENTRY_H);
    s_act_map_w=ENTRY_W;s_act_map_h=ENTRY_H;
    /* Spawn near centre on the floor, above the pit */
    s_player.x=6*TILE_SIZE;s_player.y=7*TILE_SIZE;
    s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;s_player.crouching=0;
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

/*==========================================================================
 * === ACTION SCENE ===
 *==========================================================================*/
static void action_init(void){
    hal_sprite_hide_all();s_action_npc_count=0;
    arrows_clear();placed_ladder_clear();s_in_building=0;
    action_clear_ow_tiles();
    if(s_map_event_type==MAP_EVENT_DUNGEON_FIXED){dungeon_init_fixed();}
    else if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM){dungeon_init_random();}
    else{
        switch(s_action_reason){
        case ACTION_REASON_SAFE:
            switch(s_safe_type){
            case SAFE_LONE_CHARACTER:hal_tilemap_set(safe_map_lone,SAFE_LONE_W,SAFE_LONE_H);s_act_map_w=SAFE_LONE_W;s_act_map_h=SAFE_LONE_H;s_act_map_name="lone";
                {int16_t nx=SAFE_LONE_NPC_X,ny=SAFE_LONE_NPC_Y;place_action_npcs(&nx,&ny,1);}break;
            case SAFE_CARAVAN:hal_tilemap_set(safe_map_caravan,SAFE_CARAVAN_W,SAFE_CARAVAN_H);s_act_map_w=SAFE_CARAVAN_W;s_act_map_h=SAFE_CARAVAN_H;s_act_map_name="caravan";
                place_action_npcs(safe_caravan_npc_x,safe_caravan_npc_y,SAFE_CARAVAN_NPC_COUNT);break;
            case SAFE_OASIS_TOWN:hal_tilemap_set(safe_map_oasis,SAFE_OASIS_W,SAFE_OASIS_H);s_act_map_w=SAFE_OASIS_W;s_act_map_h=SAFE_OASIS_H;s_act_map_name="oasis";
                place_action_npcs(safe_oasis_npc_x,safe_oasis_npc_y,SAFE_OASIS_NPC_COUNT);break;
            default:hal_tilemap_set(safe_map_lone,SAFE_LONE_W,SAFE_LONE_H);s_act_map_w=SAFE_LONE_W;s_act_map_h=SAFE_LONE_H;s_act_map_name="lone";break;}break;
        case ACTION_REASON_DISCOVERY:hal_tilemap_set(field_map_discovery,FIELD_DISC_W,FIELD_DISC_H);s_act_map_w=FIELD_DISC_W;s_act_map_h=FIELD_DISC_H;s_act_map_name="discovery";break;
        case ACTION_REASON_COMBAT:default:
            if(s_last_encounter.enemy_type==EVENT_ENEMY_STRONG){
                hal_tilemap_set(field_map_tough,FIELD_TOUGH_W,FIELD_TOUGH_H);s_act_map_w=FIELD_TOUGH_W;s_act_map_h=FIELD_TOUGH_H;s_act_map_name="tough";
            }else if(s_last_encounter.enemy_type==EVENT_ENEMY_MEDIUM){
                /* Medium: randomly choose between combat and tough maps */
                if(rng_range(2)){
                    hal_tilemap_set(field_map_tough,FIELD_TOUGH_W,FIELD_TOUGH_H);s_act_map_w=FIELD_TOUGH_W;s_act_map_h=FIELD_TOUGH_H;s_act_map_name="tough";
                }else{
                    hal_tilemap_set(field_map_combat,FIELD_COMBAT_W,FIELD_COMBAT_H);s_act_map_w=FIELD_COMBAT_W;s_act_map_h=FIELD_COMBAT_H;s_act_map_name="combat";
                }
            }else{
                hal_tilemap_set(field_map_combat,FIELD_COMBAT_W,FIELD_COMBAT_H);s_act_map_w=FIELD_COMBAT_W;s_act_map_h=FIELD_COMBAT_H;s_act_map_name="combat";
            }break;}
        /* Field spawn: place feet on the floor (floor = map_h-2 row) */
        s_player.x=3*TILE_SIZE;
        s_player.y=(int16_t)((s_act_map_h-2)*TILE_SIZE)-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;
        s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;s_player.crouching=0;}
    s_player.dir=0;s_player.frame=0;s_player.invuln=0;s_player.vel_fx=0;s_player.vel_fy=0;s_camera_x=0;
}
static void action_update(uint16_t input,uint16_t pressed){
    int16_t nx,ny,hb_x,hb_y,abs_vfx;
    uint8_t on_ice=0;
    int16_t air_max; /* max horizontal speed allowed in air */
    /* Active hitbox: changes when crouching */
    int16_t ahb_yo, ahb_h;
    if(s_player.crouching){
        ahb_yo=PLAYER_CROUCH_HB_Y_OFFSET; ahb_h=PLAYER_CROUCH_HB_H;
    }else{
        ahb_yo=PLAYER_HB_Y_OFFSET; ahb_h=PLAYER_HB_H;
    }

    /* Crouching: friction-decay slide, no new input */
    if(s_player.crouching){
        int16_t dec=FX_ACCEL; /* same friction as ground accel */
        if(s_player.vel_fx>0){s_player.vel_fx-=dec;if(s_player.vel_fx<0)s_player.vel_fx=0;}
        else if(s_player.vel_fx<0){s_player.vel_fx+=dec;if(s_player.vel_fx>0)s_player.vel_fx=0;}
    } else {
    /* Horizontal momentum: accelerate toward input, decelerate when idle. */
    /* Check if player is on an icy tile */
    if(s_player.on_ground){
        uint16_t ftx=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET+PLAYER_HB_W/2)/TILE_SIZE);
        uint16_t fty=(uint16_t)((s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H)/TILE_SIZE);
        if(fty<s_act_map_h&&ftx<s_act_map_w) on_ice=(hal_tilemap_get(ftx,fty)==7)?1:0;
    }
    /* Air speed cap: can't exceed jump speed or standing-jump steering,
     * whichever is greater. Prevents mid-air acceleration beyond launch. */
    air_max=FX_MAX_SPEED;
    if(!s_player.on_ground&&!s_player.on_ladder){
        int16_t jabs=s_player.jump_vel_fx;if(jabs<0)jabs=-jabs;
        air_max=(jabs>FX_AIR_MAX_BASE)?jabs:FX_AIR_MAX_BASE;
        if(air_max>FX_MAX_SPEED)air_max=FX_MAX_SPEED;
    }
    if(input&INPUT_LEFT){
        /* Instant turn: zero momentum when reversing on non-ice ground */
        if(s_player.vel_fx>0&&s_player.on_ground&&!on_ice)s_player.vel_fx=0;
        {int16_t accel=s_player.on_ground?FX_ACCEL:FX_AIR_ACCEL;
         s_player.vel_fx-=accel;}
        if(s_player.vel_fx<-air_max)s_player.vel_fx=-air_max;
        /* Air direction lock: can't reverse past jump direction */
        if(!s_player.on_ground&&!s_player.on_ladder&&s_player.jump_vel_fx>0&&s_player.vel_fx<0)
            s_player.vel_fx=0;
        s_player.dir=1;
    }else if(input&INPUT_RIGHT){
        if(s_player.vel_fx<0&&s_player.on_ground&&!on_ice)s_player.vel_fx=0;
        {int16_t accel=s_player.on_ground?FX_ACCEL:FX_AIR_ACCEL;
         s_player.vel_fx+=accel;}
        if(s_player.vel_fx>air_max)s_player.vel_fx=air_max;
        /* Air direction lock: can't reverse past jump direction */
        if(!s_player.on_ground&&!s_player.on_ladder&&s_player.jump_vel_fx<0&&s_player.vel_fx>0)
            s_player.vel_fx=0;
        s_player.dir=0;
    }else{
        /* Z2-style: instant stop on ground (no slide), gradual in air */
        if(s_player.on_ground&&!on_ice){
            s_player.vel_fx=0;
        } else {
            int16_t dec=on_ice?(FX_ACCEL/4):(FX_ACCEL/2);
            if(s_player.vel_fx>0){s_player.vel_fx-=dec;if(s_player.vel_fx<0)s_player.vel_fx=0;}
            else if(s_player.vel_fx<0){s_player.vel_fx+=dec;if(s_player.vel_fx>0)s_player.vel_fx=0;}
        }
    }
    } /* end of crouch-else movement block */
    s_player.vel_x=FX_TO_PX(s_player.vel_fx);
    /* Fix: sub-pixel velocities must still produce 1px/frame movement.
     * Without this, small vel_fx (e.g. 128 in 8.8) rounds to 0 pixels,
     * making standing jumps immobile in both directions. */
    if(s_player.vel_x==0&&s_player.vel_fx>0)s_player.vel_x=1;
    if(s_player.vel_x==0&&s_player.vel_fx<0)s_player.vel_x=-1;
    abs_vfx=s_player.vel_x; if(abs_vfx<0)abs_vfx=-abs_vfx; if(abs_vfx<1)abs_vfx=1;

    hb_x=s_player.x+PLAYER_HB_X_OFFSET;hb_y=s_player.y+ahb_yo;
    {uint8_t was_on_ladder=s_player.on_ladder;
    uint8_t touching_ladder=box_hits_flag(hb_x,hb_y,PLAYER_HB_W,ahb_h,TILE_LADDER);

    /* Ladder top detection: check if feet are at the top of a ladder column.
     * "Top" means the tile below feet is ladder but the tile AT feet level is not. */
    uint8_t at_ladder_top=0;
    {int16_t foot_y2=s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H;
     uint16_t fty2=(uint16_t)(foot_y2/TILE_SIZE);
     uint16_t ftx2=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET+PLAYER_HB_W/2)/TILE_SIZE);
     /* Tile at feet = ladder, tile above feet = not ladder → we're at top */
     if(fty2<s_act_map_h && ftx2<s_act_map_w){
         uint8_t below_flags=tile_flags(hal_tilemap_get(ftx2,fty2));
         uint8_t above_flags=(fty2>0)?tile_flags(hal_tilemap_get(ftx2,fty2-1)):0;
         if((below_flags&TILE_LADDER)&&!(above_flags&TILE_LADDER)) at_ladder_top=1;
     }
    }

    /* Ladder grab/release logic */
    if(s_player.on_ladder){
        /* Release if no longer touching AND not at ladder top */
        if(!touching_ladder && !at_ladder_top)
            s_player.on_ladder=0;
        /* When climbing up and reaching the top: dismount onto the top */
        if(s_player.on_ladder && at_ladder_top && (input&INPUT_UP)){
            /* Snap player to stand on top of ladder tile */
            int16_t foot_y3=s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H;
            uint16_t snap_ty=(uint16_t)(foot_y3/TILE_SIZE);
            s_player.y=(int16_t)(snap_ty*TILE_SIZE)-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;
            s_player.on_ladder=0;s_player.on_ground=1;
            s_player.vel_fy=0;s_player.vel_y=0;
        }
    } else {
        /* Grab ladder: pressing up/down AND touching ladder */
        if(touching_ladder && (input&(INPUT_UP|INPUT_DOWN))){
            s_player.on_ladder=1;
            s_player.vel_fx=0;s_player.vel_x=0; /* stop horizontal motion */
        }
        /* Standing on ladder top and pressing DOWN → start climbing down */
        if(!s_player.on_ladder && at_ladder_top && s_player.on_ground && (input&INPUT_DOWN)){
            s_player.on_ladder=1;
            s_player.on_ground=0;
            s_player.vel_fx=0;s_player.vel_x=0;
            /* Nudge player down slightly so they're inside the ladder */
            s_player.y+=2;
        }
    }
    /* If player just left a ladder into air, give immediate downward kick */
    if(was_on_ladder&&!s_player.on_ladder&&!s_player.on_ground){
        if(s_player.vel_fy==0) s_player.vel_fy=FY_GRAV*4;
    }}

    /* Ground probe: check if feet are touching solid/platform/ladder-top BEFORE gravity.
     * This ensures on_ground is set even when vel_y==0 (standing still). */
    {int16_t foot_y=s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H;
     uint16_t foot_ty=(uint16_t)(foot_y/TILE_SIZE);
     uint16_t foot_tx_l=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET)/TILE_SIZE);
     uint16_t foot_tx_r=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET+PLAYER_HB_W-1)/TILE_SIZE);
     uint16_t foot_tx_m=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET+PLAYER_HB_W/2)/TILE_SIZE);
     uint8_t ffl=tile_flags(hal_tilemap_get(foot_tx_l,foot_ty));
     uint8_t ffr=tile_flags(hal_tilemap_get(foot_tx_r,foot_ty));
     uint8_t ffm=tile_flags(hal_tilemap_get(foot_tx_m,foot_ty));
     s_player.on_ground=((ffl|ffr|ffm)&(TILE_SOLID|TILE_PLATFORM))?1:0;
     /* Only count platforms if player feet are exactly at tile boundary */
     if(!((ffl|ffr|ffm)&TILE_SOLID)){
         if((foot_y%TILE_SIZE)!=0) s_player.on_ground=0;
     }
     /* Ladder top acts as platform: if feet are exactly at a ladder tile top
      * and not currently climbing, treat it as ground */
     if(!s_player.on_ladder && !s_player.on_ground && (foot_y%TILE_SIZE)==0){
         if((ffl|ffr|ffm)&TILE_LADDER){
             /* Check tile above is NOT ladder (confirming we're at the top) */
             uint8_t above_l=(foot_ty>0)?tile_flags(hal_tilemap_get(foot_tx_l,foot_ty-1)):0;
             uint8_t above_r=(foot_ty>0)?tile_flags(hal_tilemap_get(foot_tx_r,foot_ty-1)):0;
             uint8_t above_m=(foot_ty>0)?tile_flags(hal_tilemap_get(foot_tx_m,foot_ty-1)):0;
             if(!((above_l|above_r|above_m)&TILE_LADDER))
                 s_player.on_ground=1;
         }
     }
    }

    /* Crouch: hold down while on ground */
    if(s_player.on_ground&&!s_player.on_ladder&&(input&INPUT_DOWN)){
        if(!s_player.crouching){
            /* Entering crouch: preserve a small slide (25% of current speed, capped) */
            int16_t slide=s_player.vel_fx/4;
            int16_t slide_cap=FX_MAX_SPEED/6;
            if(slide>slide_cap)slide=slide_cap;
            if(slide<-slide_cap)slide=-slide_cap;
            s_player.vel_fx=slide;
        }
        s_player.crouching=1;
    } else s_player.crouching=0;

    if(s_player.on_ladder){
        /* Jump off ladder */
        if(pressed&INPUT_JUMP){s_player.on_ladder=0;s_player.jump_vel_fx=s_player.vel_fx;s_player.vel_fy=FY_JUMP;s_player.on_ground=0;s_player.vel_y=FX_TO_PX(s_player.vel_fy);}
        else{s_player.vel_fy=0;s_player.vel_y=0;
        if(input&INPUT_UP)s_player.vel_fy=-FY_CLIMB;
        if(input&INPUT_DOWN)s_player.vel_fy=FY_CLIMB;
        s_player.vel_y=FX_TO_PX(s_player.vel_fy);}
    }else{
        if((pressed&INPUT_JUMP)&&s_player.on_ground&&!s_player.crouching){
            s_player.jump_vel_fx=s_player.vel_fx; /* capture for air speed cap */
            s_player.vel_fy=FY_JUMP;
            /* Running jump boost: +0.75 tiles height when moving > 25% max speed */
            {int16_t abs_fx=s_player.vel_fx;if(abs_fx<0)abs_fx=-abs_fx;
            if(abs_fx > FX_MAX_SPEED/4){
                s_player.vel_fy -= 192; /* extra impulse ≈ 0.75 tiles */
            }}
            s_player.on_ground=0;}
        /* Z2-style gravity: lower gravity while jump button held and rising.
         * Normal gravity otherwise. No apex zone or fast-fall — just two rates. */
        {int16_t grav;
         if(s_player.vel_fy<0 && (input&INPUT_JUMP))
             grav=FY_GRAV_HELD;  /* holding jump while rising → floatier */
         else
             grav=FY_GRAV;       /* normal gravity (falling or not holding) */
         s_player.vel_fy+=grav;
         if(s_player.vel_fy>FY_MAX_FALL)s_player.vel_fy=FY_MAX_FALL;}
        s_player.vel_y=FX_TO_PX(s_player.vel_fy);
        /* If on ground and not jumping, clamp downward velocity to avoid float */
        if(s_player.on_ground&&s_player.vel_fy>0){s_player.vel_fy=0;s_player.vel_y=0;}
    }

    if((pressed&INPUT_ATTACK)&&s_player.attacking==0)s_player.attacking=ATTACK_DURATION;
    if(s_player.attacking>0)s_player.attacking--;

    /* Horizontal collision */
    nx=s_player.x+s_player.vel_x;hb_x=nx+PLAYER_HB_X_OFFSET;hb_y=s_player.y+ahb_yo;
    if(!box_hits_flag(hb_x,hb_y,PLAYER_HB_W,ahb_h,TILE_SOLID)){s_player.x=nx;}
    else{int16_t step=(s_player.vel_x>0)?1:-1;int16_t tx2=s_player.x;int16_t m=0;
        while(m<abs_vfx){tx2+=step;hb_x=tx2+PLAYER_HB_X_OFFSET;
            if(box_hits_flag(hb_x,hb_y,PLAYER_HB_W,ahb_h,TILE_SOLID)){tx2-=step;break;}m++;}
        s_player.x=tx2;s_player.vel_x=0;s_player.vel_fx=0;}

    /* Vertical collision (pixel-step when vel_y exceeds a tile to prevent fall-through)
     * Note: on_ground was already set by ground probe above. The collision loop
     * will update it if the player is actually moving vertically. */
    if(s_player.vel_y!=0) s_player.on_ground=0;
    {int16_t vy_remaining=s_player.vel_y;
     while(vy_remaining!=0){
        int16_t step=vy_remaining;
        if(step>TILE_SIZE)step=TILE_SIZE; if(step<-TILE_SIZE)step=-TILE_SIZE;
        ny=s_player.y+step;hb_x=s_player.x+PLAYER_HB_X_OFFSET;hb_y=ny+ahb_yo;
        if(!box_hits_flag(hb_x,hb_y,PLAYER_HB_W,ahb_h,TILE_SOLID)){
            if(step>0){int16_t fy=ny+ahb_yo+ahb_h-1,pf=s_player.y+ahb_yo+ahb_h-1;
                uint16_t fty=(uint16_t)(fy/TILE_SIZE),pty=(uint16_t)(pf/TILE_SIZE);
                if(fty!=pty){uint16_t mtx=(uint16_t)((s_player.x+PLAYER_HB_X_OFFSET+PLAYER_HB_W/2)/TILE_SIZE);
                    uint8_t tb=hal_tilemap_get(mtx,fty);
                    if((tile_flags(tb)&TILE_PLATFORM)&&pf<(int16_t)(fty*TILE_SIZE)){
                        s_player.y=(int16_t)(fty*TILE_SIZE)-ahb_yo-ahb_h;s_player.vel_y=0;s_player.vel_fy=0;s_player.on_ground=1;goto av;}}}
            s_player.y=ny;
        }else{if(step>0){int16_t fy=ny+ahb_yo+ahb_h-1;uint16_t fty=(uint16_t)(fy/TILE_SIZE);
            s_player.y=(int16_t)(fty*TILE_SIZE)-ahb_yo-ahb_h;s_player.on_ground=1;
        }else{uint16_t hty=(uint16_t)(hb_y/TILE_SIZE);s_player.y=(int16_t)((hty+1)*TILE_SIZE)-ahb_yo;}s_player.vel_y=0;s_player.vel_fy=0;break;}
        vy_remaining-=step;
    }}
av:
    hb_x=s_player.x+PLAYER_HB_X_OFFSET;hb_y=s_player.y+ahb_yo;
    if(s_player.invuln==0&&box_hits_flag(hb_x,hb_y,PLAYER_HB_W,ahb_h,TILE_DAMAGE)){
        s_player.hp-=1;s_player.invuln=INVULN_TIME;s_player.vel_fy=FY_JUMP/2;s_player.vel_y=FX_TO_PX(s_player.vel_fy);}
    if(s_player.invuln>0)s_player.invuln--;

    /* Fall death */
    if(s_player.y>(int16_t)(s_act_map_h*TILE_SIZE+32)){s_player.hp-=1;
        if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM&&s_dungeon.in_entry)load_entry_room();
        else if(s_map_event_type!=MAP_EVENT_FIELD)load_dungeon_room(s_dungeon.room_order[s_dungeon.current_idx],1);
        else{s_player.x=3*TILE_SIZE;s_player.y=(int16_t)((s_act_map_h-2)*TILE_SIZE)-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;}return;}

    /* EXIT tiles -- field maps or dungeon entry room → back to overworld (1px wider probe) */
    if(box_hits_flag(hb_x-1,hb_y,PLAYER_HB_W+2,ahb_h,TILE_EXIT)){
        if(s_map_event_type==MAP_EVENT_FIELD
         ||(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM&&s_dungeon.in_entry)){
            s_scene=SCENE_OVERWORLD;return;}}

    /* TRANSITION tiles -- dungeon rooms (probe 1px wider to detect wall-embedded tiles) */
    if((s_map_event_type==MAP_EVENT_DUNGEON_FIXED||s_map_event_type==MAP_EVENT_DUNGEON_RANDOM)
       &&(box_hits_flag(hb_x-1,hb_y,PLAYER_HB_W+2,ahb_h,TILE_TRANSITION))){
        if(s_map_event_type==MAP_EVENT_DUNGEON_RANDOM&&s_dungeon.in_entry){
            /* Entry room ladder → descend into first random room */
            s_dungeon.in_entry=0;s_dungeon.current_idx=0;
            load_dungeon_room(s_dungeon.room_order[0],1);
        }else{
            dungeon_handle_transition(s_player.x+SPRITE_W/2);
        }return;}

    /* DOOR ENTER — safe/town maps: press UP on a transition tile (9) to enter building */
    if(s_action_reason==ACTION_REASON_SAFE&&!s_in_building
       &&(pressed&INPUT_UP)
       &&box_hits_flag(hb_x,hb_y,PLAYER_HB_W,ahb_h,TILE_TRANSITION)){
        /* Save outer map state */
        s_building_px=s_player.x;s_building_py=s_player.y;
        s_building_map_w=s_act_map_w;s_building_map_h=s_act_map_h;
        s_building_cam_x=s_camera_x;
        s_in_building=1;
        /* Load interior */
        hal_tilemap_set(interior_room,INTERIOR_W,INTERIOR_H);
        s_act_map_w=INTERIOR_W;s_act_map_h=INTERIOR_H;
        /* Spawn near right side, on the ground */
        s_player.x=3*TILE_SIZE;
        s_player.y=(int16_t)((INTERIOR_H-2)*TILE_SIZE)-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;
        s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;
        s_camera_x=0;hal_tilemap_scroll(0,0);
        return;
    }

    /* DOOR EXIT — inside building: walk left into exit tile (8) → return to outer map */
    if(s_in_building&&box_hits_flag(hb_x-1,hb_y,PLAYER_HB_W+2,ahb_h,TILE_EXIT)){
        s_in_building=0;
        /* Restore outer map */
        switch(s_safe_type){
        case SAFE_OASIS_TOWN:hal_tilemap_set(safe_map_oasis,SAFE_OASIS_W,SAFE_OASIS_H);break;
        case SAFE_CARAVAN:hal_tilemap_set(safe_map_caravan,SAFE_CARAVAN_W,SAFE_CARAVAN_H);break;
        default:hal_tilemap_set(safe_map_lone,SAFE_LONE_W,SAFE_LONE_H);break;}
        s_act_map_w=s_building_map_w;s_act_map_h=s_building_map_h;
        s_player.x=s_building_px;s_player.y=s_building_py;
        s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;
        s_camera_x=s_building_cam_x;hal_tilemap_scroll(s_camera_x,0);
        return;
    }

    /* Camera */
    s_camera_x=s_player.x-SCREEN_W/2+SPRITE_W/2;
    if(s_camera_x<0)s_camera_x=0;
    if(s_camera_x>(int16_t)(s_act_map_w*TILE_SIZE)-SCREEN_W)s_camera_x=(int16_t)(s_act_map_w*TILE_SIZE)-SCREEN_W;
    hal_tilemap_scroll(s_camera_x,0);

    /* Update NPC sprite positions relative to camera (they stay in world space) */
    {uint8_t ni;
     for(ni=0;ni<s_action_npc_count&&ni<ACTION_NPC_MAX;ni++){
         sprite_desc_t ns;ns.id=ACTION_NPC_SPRITE_BASE+ni;
         ns.x=s_action_npc_wx[ni]-s_camera_x;ns.y=s_action_npc_wy[ni];
         ns.pattern=s_action_npc_pat[ni];ns.palette=0;ns.flags=SPRITE_FLAG_VISIBLE;
         hal_sprite_set(&ns);}}

    /* Equipment use (BTN4/5/6) */
    {uint8_t sl; for(sl=0;sl<EQUIP_SLOTS;sl++) use_equip(sl,pressed,input);}

    /* Update projectiles */
    arrows_update();
    /* No menu exit from action scenes */
}

/*==========================================================================
 * === ARROWS & PLACED LADDER ===
 *==========================================================================*/

static void arrows_clear(void){
    uint8_t i; for(i=0;i<MAX_ARROWS;i++) s_arrows[i].active=0;
}
static void arrow_fire(void){
    uint8_t i;
    int16_t spawn_y_off=s_player.crouching?ARROW_SPAWN_Y_CROUCH:ARROW_SPAWN_Y_STANDING;
    for(i=0;i<MAX_ARROWS;i++){
        if(!s_arrows[i].active){
            s_arrows[i].active=1;
            s_arrows[i].timer=ARROW_LIFETIME;
            s_arrows[i].x=s_player.x+(s_player.dir?-4:SPRITE_W);
            s_arrows[i].y=s_player.y+spawn_y_off;
            s_arrows[i].vel_x=s_player.dir?-ARROW_SPEED:ARROW_SPEED;
            break;
        }
    }
}
static void arrows_update(void){
    uint8_t i;
    for(i=0;i<MAX_ARROWS;i++){
        if(!s_arrows[i].active) continue;
        s_arrows[i].x+=s_arrows[i].vel_x;
        s_arrows[i].timer--;
        /* Despawn on timeout or solid tile hit */
        if(s_arrows[i].timer==0){s_arrows[i].active=0;continue;}
        {int16_t ax=s_arrows[i].x+4, ay=s_arrows[i].y+4;
         uint16_t tx=(uint16_t)(ax/TILE_SIZE), ty=(uint16_t)(ay/TILE_SIZE);
         if(tx<s_act_map_w&&ty<s_act_map_h){
             if(tile_flags(hal_tilemap_get(tx,ty))&TILE_SOLID){s_arrows[i].active=0;}}
         else s_arrows[i].active=0;}
    }
}
static void arrows_draw(int16_t cam_x){
    uint8_t i;
    for(i=0;i<MAX_ARROWS;i++){
        if(!s_arrows[i].active) continue;
        /* Draw arrow as small horizontal bar */
        {int16_t sx=s_arrows[i].x-cam_x, sy=s_arrows[i].y;
         if(sx>=-8&&sx<SCREEN_W+8)
            hal_draw_rect(sx,sy,8,2,0xFC); /* yellow */}
    }
}

static void placed_ladder_clear(void){s_placed_ladder.active=0;s_placed_ladder.count=0;}

/* Write ladder tiles (5) into the tilemap */
static void placed_ladder_apply(void){
    uint8_t i;
    if(!s_placed_ladder.active) return;
    for(i=0;i<s_placed_ladder.count;i++)
        hal_tilemap_put(s_placed_ladder.tiles_x[i],s_placed_ladder.tiles_y[i],5);
}

/* Restore original tiles */
static void placed_ladder_remove(void){
    uint8_t i;
    if(!s_placed_ladder.active) return;
    for(i=0;i<s_placed_ladder.count;i++)
        hal_tilemap_put(s_placed_ladder.tiles_x[i],s_placed_ladder.tiles_y[i],s_placed_ladder.orig[i]);
    s_placed_ladder.active=0;s_placed_ladder.count=0;
}

/* Place ladder starting at player tile.
 * No directional input: extends upward.
 * Holding left/right: extends horizontally in that direction.
 * Only places on empty (tile 0) squares. */
static void placed_ladder_place(uint16_t input){
    uint16_t ptx=(uint16_t)((s_player.x+SPRITE_W/2)/TILE_SIZE);
    uint16_t pty=(uint16_t)((s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H-1)/TILE_SIZE);
    int16_t dx=0,dy=0;
    uint8_t h=0;
    uint16_t cx,cy;

    if(input&INPUT_LEFT){dx=-1;dy=0;}
    else if(input&INPUT_RIGHT){dx=1;dy=0;}
    else{dx=0;dy=-1;} /* default: upward */

    cx=ptx;cy=pty;
    while(h<PLACED_LADDER_MAX_H){
        uint8_t existing;
        if(cx>=s_act_map_w||cy>=s_act_map_h) break;
        if((int16_t)cx<0||(int16_t)cy<0) break;
        existing=hal_tilemap_get(cx,cy);
        /* Stop at any non-empty tile */
        if(existing!=0) break;
        s_placed_ladder.tiles_x[h]=cx;
        s_placed_ladder.tiles_y[h]=cy;
        s_placed_ladder.orig[h]=existing;
        h++;
        cx=(uint16_t)((int16_t)cx+dx);
        cy=(uint16_t)((int16_t)cy+dy);
    }
    if(h==0) return;
    s_placed_ladder.active=1;
    s_placed_ladder.count=h;
    placed_ladder_apply();
}

/* Check if player is on the placed ladder */
static uint8_t player_on_placed_ladder(void){
    uint16_t ptx,pty;uint8_t i;
    if(!s_placed_ladder.active) return 0;
    ptx=(uint16_t)((s_player.x+SPRITE_W/2)/TILE_SIZE);
    pty=(uint16_t)((s_player.y+PLAYER_HB_Y_OFFSET+PLAYER_HB_H/2)/TILE_SIZE);
    for(i=0;i<s_placed_ladder.count;i++){
        if(s_placed_ladder.tiles_x[i]==ptx&&s_placed_ladder.tiles_y[i]==pty) return 1;
    }
    return 0;
}

/*==========================================================================
 * === EQUIPMENT USE ===
 *==========================================================================*/

static void use_equip(uint8_t slot, uint16_t pressed, uint16_t input){
    uint16_t btn;
    item_id_t item;
    switch(slot){
        case 0: btn=INPUT_BTN4; break;
        case 1: btn=INPUT_BTN5; break;
        case 2: btn=INPUT_BTN6; break;
        default: return;
    }
    if(!(pressed&btn)) return;
    item=s_equip[slot];
    switch(item){
        case ITEM_BOW: arrow_fire(); break;
        case ITEM_LADDER:
            if(s_placed_ladder.active){
                /* Retract: allowed from anywhere (including while on it) */
                placed_ladder_remove();
            } else if(s_player.on_ground||s_player.on_ladder){
                placed_ladder_place(input);
            }
            break;
        default: break;
    }
}

/*==========================================================================
 * === INVENTORY ===
 *==========================================================================*/

static void inventory_init(void){
    uint8_t i;
    for(i=0;i<BAG_SLOTS;i++) s_bag[i]=ITEM_NONE;
    for(i=0;i<EQUIP_SLOTS;i++) s_equip[i]=ITEM_NONE;
    /* Starter items */
    s_bag[0]=ITEM_BOW;
    s_bag[1]=ITEM_LADDER;
    s_equip[0]=ITEM_BOW;    /* BTN4 = bow */
    s_equip[1]=ITEM_LADDER; /* BTN5 = ladder */
}

/*==========================================================================
 * === BAG / PAUSE MENU ===
 *==========================================================================*/

static void bag_draw(void){
    uint8_t i;
    hal_draw_rect(20,16,216,160,0x00);hal_draw_rect(22,18,212,156,0x02);
    hal_draw_text(72,22,"== BAG ==",0xFF);

    /* Bag inventory slots */
    for(i=0;i<BAG_SLOTS;i++){
        int16_t yy=38+(int16_t)i*12;
        uint8_t col=(i==s_bag_cursor)?0xFC:0xFF; /* yellow highlight */
        hal_draw_number(30,yy,(int32_t)(i+1),col);
        hal_draw_text(38,yy,".",col);
        hal_draw_text(46,yy,item_name(s_bag[i]),col);
    }

    /* Equip slots on the right side */
    hal_draw_text(140,38,"EQUIP:",0xFF);
    for(i=0;i<EQUIP_SLOTS;i++){
        int16_t yy=50+(int16_t)i*12;
        hal_draw_text(140,yy,i==0?"B4:":i==1?"B5:":"B6:",0xAD);
        hal_draw_text(164,yy,item_name(s_equip[i]),0xFF);
    }

    hal_draw_text(30,146,"Up/Dn=Select 4/5/6=Assign B=Close",0xFF);
}

/* Process bag input: navigate cursor, assign items to equip slots */
static void bag_input(uint16_t pressed){
    if(pressed&INPUT_UP){if(s_bag_cursor>0)s_bag_cursor--;}
    if(pressed&INPUT_DOWN){if(s_bag_cursor<BAG_SLOTS-1)s_bag_cursor++;}
    /* BTN4/5/6 assign currently selected bag item to that equip slot.
     * Unassign from any other slot first (one slot per item). */
    {uint8_t tgt=0xFF;
     if(pressed&INPUT_BTN4) tgt=0;
     if(pressed&INPUT_BTN5) tgt=1;
     if(pressed&INPUT_BTN6) tgt=2;
     if(tgt!=0xFF){
        item_id_t item=s_bag[s_bag_cursor];
        uint8_t s;
        /* Clear this item from any existing equip slot */
        for(s=0;s<EQUIP_SLOTS;s++){if(s_equip[s]==item&&item!=ITEM_NONE)s_equip[s]=ITEM_NONE;}
        s_equip[tgt]=item;
     }}
    /* BTN3 clears the selected bag slot */
    if(pressed&INPUT_BTN3){s_bag[s_bag_cursor]=ITEM_NONE;}
}

/* Equip HUD: 3 small boxes at top-right showing equipped items */
static void equip_hud_draw(void){
    uint8_t i;
    for(i=0;i<EQUIP_SLOTS;i++){
        int16_t bx=SCREEN_W-78+(int16_t)i*26;
        int16_t by=2;
        /* Box background */
        hal_draw_rect(bx,by,24,12,0x01);
        /* Button label */
        hal_draw_text(bx+1,by+2,i==0?"4":i==1?"5":"6",0xAD);
        /* Item name (abbreviated to 3 chars) */
        {const char *n=item_name(s_equip[i]);
         char abbr[4]={n[0],n[1]&&n[0]!='-'?n[1]:' ',n[2]&&n[1]&&n[0]!='-'?n[2]:' ',0};
         hal_draw_text(bx+9,by+2,abbr,0xFF);}
    }
}

static void menu_draw(void){
    hal_draw_rect(64,50,128,92,0x00);hal_draw_rect(66,52,124,88,0x01);
    hal_draw_text(88,60,"PAUSED",0xFF);hal_draw_text(72,80,"Enter=Resume",0xFF);
    hal_draw_text(72,92,"K=Controls",0xFF);
    hal_draw_text(72,104,"L=Debug Menu",0xAD);
}

/*==========================================================================
 * === DEBUG MENU ===
 *==========================================================================*/

#include "game/debug_menu.h"

static uint8_t debug_list_count(void){
    if(s_debug_level==0) return DMAIN_ITEMS;
    if(s_debug_level==1) return DCAT_COUNT;
    if(s_debug_level==2){
        switch(s_debug_cat){
            case DCAT_WORLD:   return NUM_WORLD_MAPS;
            case DCAT_ACTION:  return NUM_DEBUG_ACTIONS;
            case DCAT_DUNGEON: return DUNGEON_ROOM_POOL_SIZE;
            case DCAT_EVENT:   return 1;
            default: return 0;
        }
    }
    if(s_debug_level==3) return DENTRY_COUNT; /* entry point selection */
    return 0;
}
static const char *debug_list_label(uint8_t i){
    if(s_debug_level==0) return (i<DMAIN_ITEMS)?dmain_labels[i]:"?";
    if(s_debug_level==1) return (i<DCAT_COUNT)?dcat_labels[i]:"?";
    if(s_debug_level==2){
        switch(s_debug_cat){
            case DCAT_WORLD:   return (i<NUM_WORLD_MAPS)?world_maps[i].name:"?";
            case DCAT_ACTION:  return (i<NUM_DEBUG_ACTIONS)?debug_actions[i].name:"?";
            case DCAT_DUNGEON: return (i<DUNGEON_ROOM_POOL_SIZE)?room_pool[i].name:"?";
            case DCAT_EVENT:   return "Traveling Event";
            default: return "?";
        }
    }
    if(s_debug_level==3){
        const dungeon_room_def_t *r=&room_pool[s_debug_room];
        if(i==DENTRY_BACK) return r->label_back;
        if(i==DENTRY_FWD) return r->label_fwd;
    }
    return "?";
}

static void debug_draw(void){
    uint8_t i, count=debug_list_count();
    const char *title;
    hal_draw_rect(12,6,232,182,0x00);hal_draw_rect(14,8,228,178,0x02);
    if(s_debug_level==0) title="== DEBUG ==";
    else if(s_debug_level==1) title="LOAD MAP";
    else if(s_debug_level==3) title="ENTRY POINT";
    else title=dcat_labels[s_debug_cat];
    hal_draw_text(64,10,title,0xFF);
    for(i=0;i<DEBUG_VISIBLE_ROWS&&(i+s_debug_scroll)<count;i++){
        uint8_t idx=i+s_debug_scroll;
        uint8_t col=(idx==s_debug_cursor)?0xFC:0xFF;
        hal_draw_text(24,24+(int16_t)i*11,debug_list_label(idx),col);
        if(idx==s_debug_cursor) hal_draw_text(16,24+(int16_t)i*11,">",0xE0);
    }
    /* Scroll indicators */
    if(s_debug_scroll>0) hal_draw_text(120,16,"^ more ^",0xAD);
    if(s_debug_scroll+DEBUG_VISIBLE_ROWS<count) hal_draw_text(110,24+(int16_t)DEBUG_VISIBLE_ROWS*11,"v more v",0xAD);
    /* Footer */
    if(s_debug_level>0) hal_draw_text(24,178,"Btn2=Back  Jump=Select",0xAD);
    else hal_draw_text(24,178,"Up/Dn=Sel  Jump=Select",0xAD);
}

static void debug_ensure_scroll(void){
    uint8_t count=debug_list_count();
    if(s_debug_cursor>=count&&count>0) s_debug_cursor=count-1;
    if(s_debug_cursor<s_debug_scroll) s_debug_scroll=s_debug_cursor;
    if(s_debug_cursor>=s_debug_scroll+DEBUG_VISIBLE_ROWS) s_debug_scroll=s_debug_cursor-DEBUG_VISIBLE_ROWS+1;
}

static void debug_input(uint16_t pressed){
    uint8_t count=debug_list_count();
    if(pressed&INPUT_UP){if(s_debug_cursor>0)s_debug_cursor--;debug_ensure_scroll();}
    if(pressed&INPUT_DOWN){if(s_debug_cursor<count-1)s_debug_cursor++;debug_ensure_scroll();}

    /* Back button */
    if(pressed&INPUT_BTN2){
        if(s_debug_level>0){s_debug_level--;s_debug_cursor=0;s_debug_scroll=0;return;}
    }

    if(pressed&INPUT_JUMP){
        if(s_debug_level==0){
            switch(s_debug_cursor){
            case DMENU_LOAD_MAP:
                s_debug_level=1;s_debug_cursor=0;s_debug_scroll=0;return;
            case DMENU_CHAR_SET:
                s_console_open=1;s_console_cursor=0;s_console_scroll=0;return;
            case DMENU_BACK_OW:
                s_debug_menu=0;s_force_reinit=0;s_scene=SCENE_OVERWORLD;return;
            case DMENU_CONTROLS:
                s_debug_menu=0;s_force_reinit=0;s_keybind_editor=1;s_keybind_cursor=0;s_keybind_waiting=0;return;
            case DMENU_HEAL:
                s_force_reinit=0;s_player.hp=PLAYER_START_HP;s_debug_menu=0;return;
            case DMENU_EXIT:
                hal_shutdown();exit(0);return;
            }
        }
        else if(s_debug_level==1){
            /* Selected a category — go to level 2 */
            s_debug_cat=s_debug_cursor;s_debug_level=2;s_debug_cursor=0;s_debug_scroll=0;return;
        }
        else if(s_debug_level==2){
            /* Selected a specific map/item */
            s_debug_menu=0;s_force_reinit=1;
            switch(s_debug_cat){
            case DCAT_WORLD:
                /* Load a world map */
                s_cur_world_map=s_debug_cursor;
                s_ow_player_x=2*TILE_SIZE;s_ow_player_y=2*TILE_SIZE;
                s_force_reinit=0;s_scene=SCENE_OVERWORLD;return;
            case DCAT_ACTION:{
                const debug_action_entry_t *a=&debug_actions[s_debug_cursor];
                s_action_reason=a->reason;s_map_event_type=a->map_type;
                if(a->reason==ACTION_REASON_SAFE)s_safe_type=a->safe_type;
                if(a->reason==ACTION_REASON_COMBAT){s_last_encounter.kind=ENCOUNTER_COMBAT;s_last_encounter.enemy_type=a->enemy_type;}
                s_scene=SCENE_ACTION;return;}
            case DCAT_DUNGEON:
                /* Selected a room — go to entry point selection */
                s_debug_room=s_debug_cursor;
                s_debug_level=3;s_debug_cursor=0;s_debug_scroll=0;
                s_debug_menu=1;s_force_reinit=0;return;
            case DCAT_EVENT:
                /* Trigger a traveling event combat */
                s_action_reason=ACTION_REASON_COMBAT;s_map_event_type=MAP_EVENT_FIELD;
                s_last_encounter.kind=ENCOUNTER_COMBAT;s_last_encounter.enemy_type=EVENT_ENEMY_MEDIUM;
                s_scene=SCENE_ACTION;return;
            }
        }
        else if(s_debug_level==3){
            /* Selected an entry point in a dungeon room — spawn directly */
            s_debug_menu=0;s_force_reinit=1;
            {const dungeon_room_def_t *r=&room_pool[s_debug_room];
            int16_t ex,ey;
            if(s_debug_cursor==DENTRY_BACK){ex=r->entry_back_x;ey=r->entry_back_y;}
            else{ex=r->entry_fwd_x;ey=r->entry_fwd_y;}
            /* Load the room as a standalone action map */
            hal_sprite_hide_all();s_action_npc_count=0;
            action_clear_ow_tiles();
            hal_tilemap_set(r->map,r->w,r->h);
            s_act_map_w=r->w;s_act_map_h=r->h;s_act_map_name=r->name;
            s_action_reason=ACTION_REASON_DUNGEON_FIXED;s_map_event_type=MAP_EVENT_FIELD;
            s_player.x=ex;s_player.y=ey-PLAYER_HB_Y_OFFSET-PLAYER_HB_H;
            s_player.vel_x=0;s_player.vel_y=0;s_player.vel_fx=0;s_player.vel_fy=0;
            s_player.on_ground=0;s_player.on_ladder=0;s_player.attacking=0;s_player.crouching=0;
            s_player.dir=0;s_player.frame=0;s_player.invuln=0;s_camera_x=0;
            s_in_building=0;
            s_scene=SCENE_ACTION;return;}
        }
    }
}

/*==========================================================================
 * === DEBUG CONSOLE ===
 *
 * Access: Pause -> BTN3 (debug menu) -> BTN3 again (console)
 * Navigate: Up/Down = select variable
 * Adjust: Left/Right = change by step, BTN1+Left/Right = change by 1
 * Reset: BTN2 = reset to default
 * Exit: Escape/Menu or BTN3
 *==========================================================================*/
#define CONSOLE_VISIBLE 14

static void console_draw(void){
    uint8_t i;
    hal_draw_rect(4,2,248,188,0x00);hal_draw_rect(6,4,244,184,0x02);
    hal_draw_text(50,5,"== PARAM CONSOLE ==",0xFF);
    hal_draw_text(8,16,"Name",0xAD);hal_draw_text(130,16,"Value",0xAD);hal_draw_text(180,16,"Def",0xAD);
    for(i=0;i<CONSOLE_VISIBLE&&(i+s_console_scroll)<TVAR_COUNT;i++){
        uint8_t idx=i+s_console_scroll;
        const tvar_t *v=&tvars[idx];
        uint8_t col=(idx==s_console_cursor)?0xFC:0xFF;
        int16_t yy=27+(int16_t)i*11;
        hal_draw_text(8,yy,v->name,col);
        hal_draw_number(130,yy,(int32_t)*v->ptr,col);
        hal_draw_number(180,yy,(int32_t)v->def,0xAD);
        if(idx==s_console_cursor) hal_draw_text(2,yy,">",0xE0);
    }
    if(s_console_scroll>0) hal_draw_text(120,19,"^",0xAD);
    if(s_console_scroll+CONSOLE_VISIBLE<TVAR_COUNT) hal_draw_text(120,27+(int16_t)CONSOLE_VISIBLE*11,"v",0xAD);
    hal_draw_text(6,178,"L/R=adj Btn1+L/R=fine Btn2=reset",0xAD);
}

static void console_input(uint16_t pressed, uint16_t input){
    if(pressed&INPUT_UP){if(s_console_cursor>0)s_console_cursor--;}
    if(pressed&INPUT_DOWN){if(s_console_cursor<TVAR_COUNT-1)s_console_cursor++;}
    if(s_console_cursor<s_console_scroll)s_console_scroll=s_console_cursor;
    if(s_console_cursor>=s_console_scroll+CONSOLE_VISIBLE)s_console_scroll=s_console_cursor-CONSOLE_VISIBLE+1;

    {const tvar_t *v=&tvars[s_console_cursor];
    int16_t step=(input&INPUT_JUMP)?1:v->step; /* fine adjust with BTN1 held */
    if(pressed&INPUT_LEFT){*v->ptr-=step;if(*v->ptr<v->min)*v->ptr=v->min;}
    if(pressed&INPUT_RIGHT){*v->ptr+=step;if(*v->ptr>v->max)*v->ptr=v->max;}
    if(pressed&INPUT_BTN2){*v->ptr=v->def;} /* reset to default */
    }

    if(pressed&INPUT_BTN3){s_console_open=0;} /* exit console */
    if(pressed&INPUT_MENU){s_console_open=0;s_debug_menu=0;} /* exit all */
}

/*==========================================================================
 * === KEYBIND EDITOR ===
 *
 * Accessible from debug menu. Shows all bindings, press Jump to rebind.
 * Press any key to assign it to the selected binding slot.
 *==========================================================================*/

static void keybind_draw(void){
    uint8_t i;
    const keybind_config_t *cfg=hal_keys_get_config();
    hal_draw_rect(4,2,248,188,0x00);hal_draw_rect(6,4,244,184,0x02);
    hal_draw_text(60,6,"== CONTROLS ==",0xFF);
    hal_draw_text(8,18,"Binding",0xAD);hal_draw_text(108,18,"Key",0xAD);hal_draw_text(178,18,"Alt",0xAD);
    for(i=0;i<BIND_COUNT;i++){
        int16_t yy=28+(int16_t)i*12;
        uint8_t col=(i==s_keybind_cursor)?0xFC:0xFF;
        uint8_t pk=cfg->primary[i], ak=cfg->alt[i];
        hal_draw_text(8,yy,bind_names[i],col);
        if(s_keybind_waiting==1&&i==s_keybind_cursor)
            hal_draw_text(108,yy,"[...]",0xE0);
        else
            hal_draw_text(108,yy,(pk<KEY_ID_COUNT)?key_id_names[pk]:"-",col);
        if(s_keybind_waiting==2&&i==s_keybind_cursor)
            hal_draw_text(178,yy,"[...]",0xE0);
        else
            hal_draw_text(178,yy,(ak<KEY_ID_COUNT&&ak)?key_id_names[ak]:"-",col);
    }
    {int16_t by=28+(int16_t)BIND_COUNT*12+2;
    hal_draw_text(8,by, s_keybind_waiting?"Press key (Esc=cancel)":"J=Bind K=BindAlt",0xAD);
    hal_draw_text(8,by+10,"Layout:",0xAD);hal_draw_text(66,by+10,cfg->name,0xFF);
    hal_draw_text(8,by+22,"L=Reset 4=Lay1 5=Lay2 6=Save",0xAD);
    hal_draw_text(8,by+34,"Esc=Exit",0xE0);}
}

static void keybind_input(uint16_t pressed){
    if(s_keybind_waiting){
        uint8_t k=hal_input_last_key();
        if(k==KEY_ID_ESCAPE){s_keybind_waiting=0;return;}
        if(k!=KEY_ID_NONE){
            uint8_t pk=hal_keys_get_bind(s_keybind_cursor,0);
            uint8_t ak=hal_keys_get_bind(s_keybind_cursor,1);
            if(s_keybind_waiting==1) pk=k; else ak=k;
            hal_keys_rebind(s_keybind_cursor,pk,ak);
            s_keybind_waiting=0;
        }
        return;
    }
    if(pressed&INPUT_UP){if(s_keybind_cursor>0)s_keybind_cursor--;}
    if(pressed&INPUT_DOWN){if(s_keybind_cursor<BIND_COUNT-1)s_keybind_cursor++;}
    if(pressed&INPUT_ATTACK){s_keybind_waiting=1;} /* rebind primary (J/Btn1) */
    if(pressed&INPUT_JUMP){s_keybind_waiting=2;}   /* rebind alt (K/Btn2) */
    if(pressed&INPUT_BTN3){hal_keys_reset_defaults(hal_keys_get_layout());} /* reset */
    if(pressed&INPUT_BTN4){hal_keys_set_layout(0);} /* layout 1 */
    if(pressed&INPUT_BTN5){hal_keys_set_layout(1);} /* layout 2 */
    if(pressed&INPUT_BTN6){hal_keys_save_config("controls.cfg");} /* save */
    {uint8_t k=hal_input_last_key();
    if(k==KEY_ID_ESCAPE){s_keybind_editor=0;s_debug_menu=0;}}
}

/*==========================================================================
 * === RENDER ===
 *==========================================================================*/

static void render(void){
    sprite_desc_t ps;
    hal_tilemap_draw();
    if(s_scene==SCENE_OVERWORLD)events_draw(s_ow_cam_x,s_ow_cam_y);

    {uint8_t pflags=SPRITE_FLAG_VISIBLE;
    if(s_player.dir)pflags|=SPRITE_FLAG_MIRROR_X;
    if(s_player.invuln>0&&(s_player.invuln&0x02))pflags&=~SPRITE_FLAG_VISIBLE;

    if(s_scene==SCENE_ACTION){
        if(s_player.crouching){
            /* Crouched: show crouch pattern at bottom sprite position */
            ps.id=0;ps.pattern=PATTERN_ACT_TOP;ps.palette=0;ps.flags=0; /* hide top slot */
            ps.x=s_player.x-s_camera_x;ps.y=s_player.y;
            hal_sprite_set(&ps);
            ps.id=1;ps.pattern=PATTERN_ACT_CROUCH;ps.flags=pflags;
            ps.y=s_player.y+SPRITE_H; /* same position as bottom sprite */
            hal_sprite_set(&ps);
        }else{
            /* Top half (head/torso) — sprite slot 0 */
            ps.id=0;ps.pattern=PATTERN_ACT_TOP;ps.palette=0;ps.flags=pflags;
            ps.x=s_player.x-s_camera_x;ps.y=s_player.y;
            hal_sprite_set(&ps);
            /* Bottom half (legs) — sprite slot 1 */
            ps.id=1;ps.pattern=PATTERN_ACT_BOT;
            ps.y=s_player.y+SPRITE_H;
            hal_sprite_set(&ps);
        }
    }else{
        /* Overworld: single 8x8-in-16x16 sprite */
        ps.id=0;ps.pattern=PATTERN_OW_PLAYER;ps.palette=0;ps.flags=pflags;
        ps.x=s_player.x-s_ow_cam_x;ps.y=s_player.y-s_ow_cam_y;
        hal_sprite_set(&ps);
    }}
    hal_sprites_draw();

    if(s_scene==SCENE_OVERWORLD){
        hal_draw_text(2,2,world_maps[s_cur_world_map].name,0xFF);
        switch(events_phase()){
        case PHASE_WAITING:{uint16_t s2=events_timer()/50;hal_draw_text(2,12,"Next:",0xFF);hal_draw_number(44,12,(int32_t)(s2+1),0xFF);hal_draw_text(52,12,"s",0xFF);break;}
        case PHASE_SPAWNED:{uint16_t s2=events_timer()/50;hal_draw_text(2,12,"x",0xFF);hal_draw_number(10,12,(int32_t)events_count(),0xFF);
            hal_draw_text(28,12," ",0xFF);hal_draw_number(36,12,(int32_t)(s2+1),0xFF);hal_draw_text(44,12,"s left",0xFF);break;}}
        hal_draw_text(2,SCREEN_H-12,"HP:",0xFF);hal_draw_number(28,SCREEN_H-12,(int32_t)s_player.hp,0xFF);
    }else{
        switch(s_action_reason){
        case ACTION_REASON_COMBAT:hal_draw_text(2,2,"COMBAT:",0xFF);hal_draw_text(60,2,enemy_name(s_last_encounter.enemy_type),0xFF);
            if(s_act_map_name){hal_draw_text(2,12,"Map:",0xAD);hal_draw_text(36,12,s_act_map_name,0xAD);}break;
        case ACTION_REASON_SAFE:
            if(s_in_building)hal_draw_text(2,2,"INSIDE",0xFF);
            else hal_draw_text(2,2,safe_zone_name(s_safe_type),0xFF);
            break;
        case ACTION_REASON_DISCOVERY:hal_draw_text(2,2,"DISCOVERY!",0xFF);break;
        case ACTION_REASON_DUNGEON_FIXED:hal_draw_text(2,2,"DUNGEON Rm:",0xFF);hal_draw_number(92,2,(int32_t)(s_dungeon.current_idx+1),0xFF);
            hal_draw_text(100,2,"/",0xFF);hal_draw_number(108,2,(int32_t)s_dungeon.num_rooms,0xFF);break;
        case ACTION_REASON_DUNGEON_RANDOM:
            if(s_dungeon.in_entry){hal_draw_text(2,2,"DEPTHS ENTRANCE",0xFF);}
            else{hal_draw_text(2,2,"DEPTHS Rm:",0xFF);hal_draw_number(84,2,(int32_t)(s_dungeon.current_idx+1),0xFF);
            hal_draw_text(92,2,"/",0xFF);hal_draw_number(100,2,(int32_t)s_dungeon.num_rooms,0xFF);}break;
        default:hal_draw_text(2,2,"ACTION",0xFF);break;}
        hal_draw_text(2,12,"HP:",0xFF);hal_draw_number(28,12,(int32_t)s_player.hp,0xFF);
        if(s_player.attacking>0)hal_draw_text(SCREEN_W-90,14,"ATK!",0xFF);
        arrows_draw(s_camera_x);
        equip_hud_draw();
    }
    if(s_friendly_dialog)friendly_dialog_draw();
    if(s_bag_open)bag_draw();
    if(s_paused)menu_draw();
    if(s_debug_menu)debug_draw();
    if(s_console_open)console_draw();
    if(s_keybind_editor)keybind_draw();
    /* Transition blackout overlay */
    if(s_transition_timer>0){hal_draw_rect(0,0,128,192,0x00);hal_draw_rect(128,0,128,192,0x00);}
}

/*==========================================================================
 * === MAIN LOOP ===
 *==========================================================================*/

int main(void){
    uint16_t input,pressed;scene_t last_scene;
    if(hal_init()!=0)return 1;
    /* Try to load saved controls; if not found, defaults are already set */
    hal_keys_load_config("controls.cfg");
    rng_seed((uint16_t)(hal_frame_count()+12345));
    s_scene=SCENE_OVERWORLD;s_action_reason=ACTION_REASON_NONE;
    s_map_event_type=MAP_EVENT_FIELD;s_safe_type=SAFE_LONE_CHARACTER;
    s_paused=0;s_bag_open=0;s_friendly_dialog=0;s_transition_timer=0;s_debug_menu=0;s_force_reinit=0;s_immune_active=0;s_keybind_editor=0;s_console_open=0;
    s_camera_x=0;s_ow_cam_x=0;s_ow_cam_y=0;
    s_ow_player_x=2*TILE_SIZE;s_ow_player_y=2*TILE_SIZE;
    s_player.hp=PLAYER_START_HP;s_action_npc_count=0;
    s_dungeon.num_rooms=0;s_dungeon.current_idx=0;s_dungeon.in_entry=0;
    s_bag_cursor=0;
    inventory_init();
    overworld_init();last_scene=s_scene;

    for(;;){
        hal_frame_begin();input=hal_input_poll();pressed=hal_input_pressed();
        /* Transition blackout countdown */
        if(s_transition_timer>0){s_transition_timer--;render();hal_frame_end();rng_next();continue;}
        if((pressed&INPUT_BAG)&&!s_friendly_dialog&&!s_debug_menu){s_bag_open=!s_bag_open;if(s_bag_open)s_bag_cursor=0;}
        if((pressed&INPUT_MENU)&&!s_bag_open&&!s_friendly_dialog){
            if(s_debug_menu)s_debug_menu=0; else s_paused=!s_paused;}
        /* BTN3 toggles debug menu when paused */
        if(s_paused&&(pressed&INPUT_BTN3)){
            s_debug_menu=!s_debug_menu;
            if(s_debug_menu){s_debug_level=0;s_debug_cursor=0;s_debug_scroll=0;}
            s_paused=0;}
        /* BTN2 (K) opens control bind editor from pause menu */
        if(s_paused&&(pressed&INPUT_BTN2)){s_keybind_editor=1;s_keybind_cursor=0;s_keybind_waiting=0;s_paused=0;}
        if(s_keybind_editor){keybind_input(pressed);}
        else if(s_console_open){console_input(pressed,input);}
        else if(s_debug_menu){
            /* BTN3 from debug menu opens parameter console */
            if(pressed&INPUT_BTN3){s_console_open=1;s_console_cursor=0;s_console_scroll=0;}
            else debug_input(pressed);
        }
        else if(s_bag_open){bag_input(pressed);}
        else if(!s_paused){switch(s_scene){case SCENE_OVERWORLD:overworld_update(input,pressed);break;case SCENE_ACTION:action_update(input,pressed);break;}}
        if(s_scene!=last_scene||s_force_reinit){s_paused=0;s_bag_open=0;s_transition_timer=TRANSITION_FRAMES;s_force_reinit=0;
            if(s_scene==SCENE_ACTION)action_init();else overworld_init();last_scene=s_scene;}
        hal_music_update();render();hal_frame_end();rng_next();}
#ifdef _MSC_VER
    __assume(0);
#endif
    return 0;
}
