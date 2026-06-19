/*============================================================================
 * enemies.h - Data-Driven Enemy Framework
 *
 * An enemy is a COLLECTION OF TRAITS, not a hardcoded type. Each trait is an
 * independent field so enemies can be mixed, matched, and randomized:
 *
 *   appearance  -> sprite pattern + size (16x16 up to multi-tile)
 *   color       -> palette swap (significant: same sprite, different tint)
 *   movement    -> how it moves through the world
 *   behavior    -> its decision-making (stationary / chase / jump / duel)
 *   attacks     -> what offensive moves it can use
 *   projectiles -> the OBJECTS an attack spawns (arrow vs bomb vs bouncing
 *                  ball that stays vs ball that lands and vanishes)
 *   vulnerabilities -> damage multipliers per damage type
 *
 * SLOT SYSTEM (for maps):
 *   Maps place enemies in abstract SLOTS (A, B, C, ...). What actually
 *   spawns in a slot is decided by enemy_resolve_slot(), which can later be
 *   randomized or themed per-region without editing map data.
 *
 * DEFERRED (future session):
 *   - Multi-screen Kraid-style bosses (size model allows it; the camera /
 *     off-screen culling / segmented collision is its own task)
 *   - Map-editor wiring of slots -> spawn positions export
 *============================================================================*/

#ifndef ENEMIES_H
#define ENEMIES_H

/*--------------------------------------------------------------------------
 * DAMAGE TYPES — slash / pierce / bludgeon + 2 magic schools
 *--------------------------------------------------------------------------*/
typedef enum {
    DMG_SLASH = 0,   /* sword swing, cutting   */
    DMG_PIERCE,      /* arrow, thrust, spear   */
    DMG_BLUDGEON,    /* hammer, blunt impact   */
    DMG_MAGIC_A,     /* magic school A (e.g. fire / arcane) */
    DMG_MAGIC_B,     /* magic school B (e.g. ice / holy)    */
    DMG_TYPE_COUNT
} damage_type_t;

/* Vulnerability multipliers, expressed in 1/4 units so we can do integer math:
 *   VULN_IMMUNE = 0   (no damage)
 *   VULN_HALF   = 2   (x0.5, reduced)
 *   VULN_NORMAL = 4   (x1.0)
 *   VULN_DOUBLE = 8   (x2.0, increased)
 * final_damage = (base_damage * vuln) / 4
 */
#define VULN_IMMUNE  0
#define VULN_HALF    2
#define VULN_NORMAL  4
#define VULN_DOUBLE  8

/*--------------------------------------------------------------------------
 * MOVEMENT TYPES — how the body translates through space
 *--------------------------------------------------------------------------*/
typedef enum {
    MOVE_NONE = 0,       /* doesn't move (turret, plant)        */
    MOVE_WALK,           /* walks along ground, gravity applies */
    MOVE_FLY_HORIZ,      /* hovers, drifts horizontally         */
    MOVE_FLY_SINE,       /* flies in a sine wave                */
    MOVE_HOP,            /* hops along ground (slime)           */
    MOVE_TYPE_COUNT
} movement_type_t;

/*--------------------------------------------------------------------------
 * BEHAVIOR TYPES — decision-making layer (drives movement + attacks)
 *--------------------------------------------------------------------------*/
typedef enum {
    BHV_STATIONARY = 0,  /* holds position, may still attack    */
    BHV_CHASE,           /* slow constant movement toward player */
    BHV_JUMP_IN_PLACE,   /* intermittent jumping, little x-move   */
    BHV_SWORD_DUEL,      /* approach, strike, back off, repeat — "intelligent" */
    BHV_PATROL,          /* walk back and forth, turn at edges/walls */
    BHV_TYPE_COUNT
} behavior_type_t;

/*--------------------------------------------------------------------------
 * ATTACK TYPES — offensive moves a behavior may trigger
 *--------------------------------------------------------------------------*/
typedef enum {
    ATK_NONE = 0,
    ATK_CONTACT,         /* damage on touch (no special move)   */
    ATK_LUNGE,           /* jump/dash at the player             */
    ATK_PROJECTILE,      /* spawn a projectile object           */
    ATK_COUNTER,         /* parries then strikes (duel)         */
    ATK_TYPE_COUNT
} attack_type_t;

/*--------------------------------------------------------------------------
 * PROJECTILE OBJECT TYPES
 *
 * The SAME attack move (ATK_PROJECTILE) can spawn DIFFERENT objects.
 * This is the "arrow vs bomb" / "bouncing ball that stays vs one that
 * lands and disappears" distinction. The object type defines the physics
 * and lifetime, independent of which enemy/attack fired it.
 *--------------------------------------------------------------------------*/
typedef enum {
    PROJ_NONE = 0,
    PROJ_ARROW,          /* straight line, despawns on wall/timeout */
    PROJ_BOMB,           /* arcs (gravity), explodes on land/timeout */
    PROJ_BALL_BOUNCE,    /* bounces off floor, STAYS on screen      */
    PROJ_BALL_LAND,      /* bounces once then lands and DISAPPEARS   */
    PROJ_FIREBALL,       /* straight line, DMG_MAGIC_A, ignores walls a bit */
    PROJ_TYPE_COUNT
} projectile_kind_t;

/* Per-projectile-kind static properties (physics & damage). */
typedef struct {
    int16_t  speed;          /* base travel speed (px/frame)        */
    int16_t  gravity;        /* 8.8 per-frame downward accel (0=none) */
    uint8_t  lifetime;       /* frames before auto-despawn          */
    uint8_t  bounces;        /* number of floor bounces before death (0xFF=infinite) */
    uint8_t  damage;         /* base damage dealt to player         */
    uint8_t  dmg_type;       /* damage_type_t this projectile deals */
    uint8_t  w, h;           /* hit size in px                      */
    uint8_t  color;          /* draw color (palette idx)            */
} projectile_def_t;

/* Indexed by projectile_kind_t. */
static const projectile_def_t projectile_defs[PROJ_TYPE_COUNT] = {
    /*               spd grav  life bounce dmg  dmgtype      w  h  color */
    /* PROJ_NONE   */ {  0,   0,   0,   0,   0, DMG_PIERCE,   0, 0, 0x00 },
    /* PROJ_ARROW  */ {  4,   0,  90,   0,   1, DMG_PIERCE,   8, 2, 0xFC },
    /* PROJ_BOMB   */ {  2,  48, 120,   0,   3, DMG_BLUDGEON, 6, 6, 0xC0 },
    /* PROJ_BALL_BOUNCE*/{3, 40, 255, 0xFF, 1, DMG_BLUDGEON, 6, 6, 0xEC },
    /* PROJ_BALL_LAND */ {3, 40, 180,   1,   1, DMG_BLUDGEON, 6, 6, 0xE8 },
    /* PROJ_FIREBALL */ { 3,   0, 100,   0,   2, DMG_MAGIC_A,  6, 6, 0xE0 },
};

/*--------------------------------------------------------------------------
 * ENEMY DEFINITION — the full trait collection
 *
 * This is what gets randomized: pick an appearance, a palette, a movement,
 * a behavior, attack(s), and a vulnerability profile and you have an enemy.
 *--------------------------------------------------------------------------*/
typedef struct {
    const char *name;

    /* Appearance */
    uint8_t  pattern;        /* base sprite pattern index            */
    uint8_t  palette;        /* palette swap offset (color identity) */
    uint8_t  tiles_w;        /* width  in 16px tiles (1 = 16px)      */
    uint8_t  tiles_h;        /* height in 16px tiles (1 = 16px)      */

    /* Stats */
    int16_t  hp;
    uint8_t  contact_damage; /* damage dealt by touching the body    */
    uint8_t  contact_dmg_type;

    /* Movement & behavior */
    uint8_t  movement;       /* movement_type_t                      */
    uint8_t  behavior;       /* behavior_type_t                      */
    int16_t  move_speed;     /* px/frame for walking/flying          */

    /* Attacks */
    uint8_t  attack;         /* primary attack_type_t                */
    uint8_t  proj_kind;      /* projectile_kind_t (if ATK_PROJECTILE) */
    uint8_t  attack_cooldown;/* frames between attacks               */

    /* Vulnerabilities — multiplier per damage type (VULN_*) */
    uint8_t  vuln[DMG_TYPE_COUNT];
} enemy_def_t;

/*--------------------------------------------------------------------------
 * ENEMY DEFINITION TABLE
 *
 * Concrete enemies built from traits. Several share a base pattern but use
 * different palettes + vulnerabilities to become distinct foes — exactly the
 * palette-swap-is-significant idea.
 *--------------------------------------------------------------------------*/
typedef enum {
    ENEMY_SLIME_GREEN = 0,
    ENEMY_SLIME_RED,
    ENEMY_SKELETON,
    ENEMY_BAT,
    ENEMY_KNIGHT,
    ENEMY_MAGE,
    ENEMY_BOMBER,
    ENEMY_DEF_COUNT
} enemy_def_id_t;

/* Patterns reused: 4/5 = red/dark-red blobs, 6 = brownish, 7 = green.
 * (These already exist in the HAL sprite bank.) Palette offset recolors. */
static const enemy_def_t enemy_defs[ENEMY_DEF_COUNT] = {
  /* ENEMY_SLIME_GREEN: weak hopping blob, hurt extra by bludgeon */
  { "Green Slime", /*pat*/7, /*pal*/0, /*w*/1,/*h*/1, /*hp*/2, /*cdmg*/1, DMG_BLUDGEON,
    MOVE_HOP, BHV_CHASE, /*spd*/1, ATK_CONTACT, PROJ_NONE, /*cd*/0,
    { VULN_NORMAL, VULN_HALF, VULN_DOUBLE, VULN_NORMAL, VULN_NORMAL } },

  /* ENEMY_SLIME_RED: palette-swapped slime, tougher, resists fire (MAGIC_A) */
  { "Red Slime", /*pat*/7, /*pal*/4, /*w*/1,/*h*/1, /*hp*/4, /*cdmg*/2, DMG_BLUDGEON,
    MOVE_HOP, BHV_CHASE, /*spd*/1, ATK_CONTACT, PROJ_NONE, /*cd*/0,
    { VULN_NORMAL, VULN_HALF, VULN_DOUBLE, VULN_HALF, VULN_NORMAL } },

  /* ENEMY_SKELETON: walks, weak to bludgeon, immune to pierce (bones) */
  { "Skeleton", /*pat*/6, /*pal*/0, /*w*/1,/*h*/1, /*hp*/3, /*cdmg*/2, DMG_SLASH,
    MOVE_WALK, BHV_PATROL, /*spd*/1, ATK_CONTACT, PROJ_NONE, /*cd*/0,
    { VULN_NORMAL, VULN_IMMUNE, VULN_DOUBLE, VULN_NORMAL, VULN_DOUBLE } },

  /* ENEMY_BAT: flies sine, fast, low hp, normal to everything */
  { "Bat", /*pat*/5, /*pal*/8, /*w*/1,/*h*/1, /*hp*/1, /*cdmg*/1, DMG_PIERCE,
    MOVE_FLY_SINE, BHV_CHASE, /*spd*/2, ATK_CONTACT, PROJ_NONE, /*cd*/0,
    { VULN_NORMAL, VULN_NORMAL, VULN_NORMAL, VULN_NORMAL, VULN_NORMAL } },

  /* ENEMY_KNIGHT: sword-duel AI, resists slash (armor), weak to bludgeon */
  { "Knight", /*pat*/6, /*pal*/2, /*w*/1,/*h*/1, /*hp*/6, /*cdmg*/2, DMG_SLASH,
    MOVE_WALK, BHV_SWORD_DUEL, /*spd*/1, ATK_COUNTER, PROJ_NONE, /*cd*/45,
    { VULN_HALF, VULN_HALF, VULN_DOUBLE, VULN_NORMAL, VULN_NORMAL } },

  /* ENEMY_MAGE: stationary, throws fireballs (MAGIC_A), weak to pierce */
  { "Mage", /*pat*/5, /*pal*/3, /*w*/1,/*h*/1, /*hp*/3, /*cdmg*/1, DMG_MAGIC_A,
    MOVE_NONE, BHV_STATIONARY, /*spd*/0, ATK_PROJECTILE, PROJ_FIREBALL, /*cd*/70,
    { VULN_NORMAL, VULN_DOUBLE, VULN_NORMAL, VULN_HALF, VULN_NORMAL } },

  /* ENEMY_BOMBER: hops, lobs bombs that arc and explode on landing */
  { "Bomber", /*pat*/4, /*pal*/1, /*w*/1,/*h*/1, /*hp*/3, /*cdmg*/1, DMG_BLUDGEON,
    MOVE_HOP, BHV_JUMP_IN_PLACE, /*spd*/1, ATK_PROJECTILE, PROJ_BOMB, /*cd*/90,
    { VULN_NORMAL, VULN_NORMAL, VULN_NORMAL, VULN_NORMAL, VULN_NORMAL } },
};

/*--------------------------------------------------------------------------
 * SLOT SYSTEM
 *
 * Maps place enemies in slots A,B,C,... Each slot resolves to an enemy_def
 * via enemy_resolve_slot(). Today it's a fixed mapping; later it can be
 * randomized, themed per-region, or scaled by difficulty WITHOUT touching
 * map data — maps only ever reference the slot letter + a spawn position.
 *--------------------------------------------------------------------------*/
typedef enum {
    ENEMY_SLOT_A = 0,
    ENEMY_SLOT_B,
    ENEMY_SLOT_C,
    ENEMY_SLOT_COUNT
} enemy_slot_t;

/* Default fixed mapping. Replace the body with a randomizer / table lookup
 * keyed on region, difficulty, RNG, etc. when you want variety. */
static uint8_t enemy_resolve_slot(uint8_t slot) {
    switch (slot) {
        case ENEMY_SLOT_A: return ENEMY_SKELETON;
        case ENEMY_SLOT_B: return ENEMY_BAT;
        case ENEMY_SLOT_C: return ENEMY_MAGE;
        default:           return ENEMY_SLIME_GREEN;
    }
}

/*--------------------------------------------------------------------------
 * RUNTIME ENEMY INSTANCE
 *--------------------------------------------------------------------------*/
typedef struct {
    uint8_t  active;
    uint8_t  def_id;         /* index into enemy_defs[]              */
    int16_t  x, y;           /* world position (top-left)           */
    int16_t  vel_fx, vel_fy; /* 8.8 fixed-point velocity            */
    int16_t  hp;
    uint8_t  on_ground;
    uint8_t  dir;            /* 0 = facing right, 1 = facing left   */
    uint8_t  hurt_timer;     /* iframes / flash after being hit     */
    uint16_t atk_timer;      /* counts down to next attack          */
    uint8_t  state;          /* behavior-specific sub-state         */
    int16_t  anim;           /* generic animation / phase counter   */
} enemy_t;

#define MAX_ENEMIES 8

/*--------------------------------------------------------------------------
 * RUNTIME PROJECTILE INSTANCE (enemy-fired)
 *--------------------------------------------------------------------------*/
typedef struct {
    uint8_t  active;
    uint8_t  kind;           /* projectile_kind_t                   */
    int16_t  x, y;
    int16_t  vel_fx, vel_fy; /* 8.8 fixed                           */
    uint8_t  timer;
    uint8_t  bounces_left;
} eproj_t;

#define MAX_EPROJ 6

#endif /* ENEMIES_H */
