/*============================================================================
 * tunables.h - Runtime-Editable Game Parameters
 *
 * All player movement, weapon, and physics values live here.
 * The debug console (Pause -> BTN3 -> BTN3) lets you adjust them in-game.
 *
 * Included from main.c after ATTACK_DURATION_DEF, INVULN_TIME_DEF,
 * PLAYER_START_HP are defined.
 *
 * To tweak gameplay feel, edit the T initializer below.
 *============================================================================*/

#ifndef TUNABLES_H
#define TUNABLES_H

/*----------------------------------------------------------------------
 * tunable_t — the struct holding every tweakable value
 *----------------------------------------------------------------------*/

typedef struct {
    /* Vertical physics (8.8 fixed-point) */
    int16_t fy_grav;          /* gravity when falling / not holding jump   */
    int16_t fy_grav_held;     /* gravity while holding jump + rising       */
    int16_t fy_jump;          /* jump impulse (negative = up)              */
    int16_t fy_max_fall;      /* terminal fall velocity                    */
    int16_t fy_climb;         /* ladder climb speed                        */
    int16_t land_crouch_thresh; /* vspeed threshold for crouch on landing  */
    int16_t land_crouch_frames; /* frames of crouch after hard landing     */

    /* Horizontal physics (8.8 fixed-point) */
    int16_t fx_max_speed;     /* max horizontal speed                      */
    int16_t fx_accel;         /* ground acceleration per frame              */
    int16_t fx_air_accel;     /* air acceleration per frame                 */
    int16_t fx_air_max;       /* air speed cap                              */

    /* Combat / items */
    int16_t attack_duration;  /* frames of sword slash                      */
    int16_t invuln_time;      /* iframes after taking damage                */
    int16_t player_hp;        /* starting / max HP                          */
    int16_t arrow_speed;      /* bow projectile speed                       */
    int16_t arrow_lifetime;   /* bow projectile frames alive                */

    /* Overworld */
    int16_t ow_move_speed;    /* overworld pixels per frame                 */
} tunable_t;

/*----------------------------------------------------------------------
 * Default values — edit here to change starting feel
 *----------------------------------------------------------------------*/

static tunable_t T = {
    /* fy_grav */       106,
    /* fy_grav_held */  67,
    /* fy_jump */       -1230,
    /* fy_max_fall */   1535,
    /* fy_climb */      512,
    /* land_crouch */   1280,
    /* land_crouch_f */ 8,
    /* fx_max_speed */  683,
    /* fx_accel */      52,
    /* fx_air_accel */  18,
    /* fx_air_max */    683,
    /* attack_dur */    ATTACK_DURATION_DEF,
    /* invuln_time */   INVULN_TIME_DEF,
    /* player_hp */     PLAYER_START_HP,
    /* arrow_speed */   5,
    /* arrow_life */    60,
    /* ow_move_spd */   1,
};

/*----------------------------------------------------------------------
 * Convenience macros — use these in game code instead of T.field
 *----------------------------------------------------------------------*/

#define FY_GRAV         T.fy_grav
#define FY_GRAV_HELD    T.fy_grav_held
#define FY_JUMP         T.fy_jump
#define FY_MAX_FALL     T.fy_max_fall
#define FY_CLIMB        T.fy_climb
#define LAND_CROUCH_THRESHOLD  T.land_crouch_thresh
#define LAND_CROUCH_FRAMES     T.land_crouch_frames
#define FX_MAX_SPEED    T.fx_max_speed
#define FX_ACCEL        T.fx_accel
#define FX_AIR_ACCEL    T.fx_air_accel
#define FX_AIR_MAX_BASE T.fx_air_max
#define ATTACK_DURATION T.attack_duration
#define INVULN_TIME     T.invuln_time

/*----------------------------------------------------------------------
 * tvar_t — debug console variable descriptor
 *
 * Commands reference:
 *   Open console: pause (Esc) -> BTN3 -> BTN3 again
 *   Navigate:     Up/Down = select variable
 *   Adjust:       Left/Right = decrease/increase by step
 *   Fine adjust:  Hold BTN1 + Left/Right = adjust by 1
 *   Reset:        BTN2 = reset selected variable to default
 *   Exit:         Esc or BTN3
 *----------------------------------------------------------------------*/

typedef struct {
    const char *name;
    int16_t *ptr;
    int16_t step;
    int16_t def;
    int16_t min;
    int16_t max;
} tvar_t;

#define TVAR_COUNT 17
static const tvar_t tvars[TVAR_COUNT] = {
    {"fy_grav",         &T.fy_grav,           10,   106,   1,  500},
    {"fy_grav_held",    &T.fy_grav_held,      5,    67,    1,  500},
    {"fy_jump",         &T.fy_jump,           50,  -1230, -3000, 0},
    {"fy_max_fall",     &T.fy_max_fall,       50,   1535,  256, 5000},
    {"fy_climb",        &T.fy_climb,          32,   512,   64, 2048},
    {"land_crouch_thr", &T.land_crouch_thresh, 64,  1280,  0,  5000},
    {"land_crouch_frm", &T.land_crouch_frames, 1,   8,    0,  60},
    {"fx_max_speed",    &T.fx_max_speed,      50,   683,   64, 4096},
    {"fx_accel",        &T.fx_accel,          5,    52,    1,  500},
    {"fx_air_accel",    &T.fx_air_accel,      5,    18,    1,  500},
    {"fx_air_max",      &T.fx_air_max,        50,   683,   64, 4096},
    {"attack_dur",      &T.attack_duration,   1,    ATTACK_DURATION_DEF, 1, 120},
    {"invuln_time",     &T.invuln_time,       5,    INVULN_TIME_DEF, 0, 255},
    {"player_hp",       &T.player_hp,         1,    PLAYER_START_HP, 1, 99},
    {"arrow_speed",     &T.arrow_speed,       1,    5,     1,  20},
    {"arrow_lifetime",  &T.arrow_lifetime,    5,    60,   10, 255},
    {"ow_move_speed",   &T.ow_move_speed,     1,    1,     1,  8},
};

#endif /* TUNABLES_H */
