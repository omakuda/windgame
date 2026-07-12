/*============================================================================
 * gen_enemy_catalog.c - Generate enemy_catalog.json from enemies.h
 *
 * The map editor reads enemy_catalog.json to populate its enemy-select
 * window. This tool derives that catalog from the SAME enemies.h the game
 * compiles, so the two never drift. Regenerate whenever enemies.h changes:
 *
 *   cc -I../src tools/gen_enemy_catalog.c -o /tmp/gencat
 *   /tmp/gencat > tools/enemy_catalog.json
 *
 * (or from src/:  cc -I. ../tools/gen_enemy_catalog.c -o /tmp/gencat)
 *
 * Roles (editor tabs) are DERIVED from traits, not stored separately:
 *   flying = movement is a fly type; ranged = projectile attack;
 *   ground = anything not flying. Enemies can carry multiple roles.
 * The boss flag is derived from multi-tile size (w>=2 or h>=2).
 *
 * Enemies are referenced by stable string id (str_id), never array index,
 * so reordering the enemies.h table never invalidates saved maps.
 *============================================================================*/
#include <stdio.h>
#include <stdint.h>
#include "game/enemies.h"

/* Derive editor "roles" (tabs) from traits, matching the mockup:
 *   Flying  = movement is a fly type
 *   Ranged  = attack is projectile
 *   Ground  = anything not flying
 * An enemy can carry multiple roles (appears under each matching tab). */
static const char* mv_name(uint8_t m){
    switch(m){case MOVE_NONE:return"none";case MOVE_WALK:return"walk";
    case MOVE_FLY_HORIZ:return"fly_horiz";case MOVE_FLY_SINE:return"fly_sine";
    case MOVE_HOP:return"hop";default:return"?";}
}
static const char* bhv_name(uint8_t b){
    switch(b){case BHV_STATIONARY:return"stationary";case BHV_CHASE:return"chase";
    case BHV_JUMP_IN_PLACE:return"jump";case BHV_SWORD_DUEL:return"duel";
    case BHV_PATROL:return"patrol";default:return"?";}
}
int main(void){
    uint8_t i,j;
    printf("{\n  \"version\": 1,\n  \"enemies\": [\n");
    for(i=0;i<ENEMY_DEF_COUNT;i++){
        const enemy_def_t *d=&enemy_defs[i];
        uint8_t flying=(d->movement==MOVE_FLY_HORIZ||d->movement==MOVE_FLY_SINE);
        uint8_t ranged=(d->attack==ATK_PROJECTILE);
        uint8_t boss=(d->tiles_w>=2||d->tiles_h>=2); /* size => boss/miniboss */
        printf("    {\n");
        printf("      \"id\": \"%s\",\n", d->str_id);
        printf("      \"name\": \"%s\",\n", d->name);
        printf("      \"pattern\": %u,\n", d->pattern);
        printf("      \"palette\": %u,\n", d->palette);
        printf("      \"w\": %u, \"h\": %u,\n", d->tiles_w, d->tiles_h);
        printf("      \"hp\": %d,\n", d->hp);
        printf("      \"movement\": \"%s\",\n", mv_name(d->movement));
        printf("      \"behavior\": \"%s\",\n", bhv_name(d->behavior));
        printf("      \"roles\": [");
        {int first=1;
         if(!flying){printf("\"ground\"");first=0;}
         if(flying){printf("%s\"flying\"",first?"":", ");first=0;}
         if(ranged){printf("%s\"ranged\"",first?"":", ");first=0;}
        }
        printf("],\n");
        printf("      \"boss\": %s,\n", boss?"true":"false");
        printf("      \"vuln\": {");
        {const char* dn[]={"slash","pierce","bludgeon","magic_a","magic_b"};
         for(j=0;j<DMG_TYPE_COUNT;j++)
            printf("%s\"%s\": %u", j?", ":"", dn[j], d->vuln[j]);
        }
        printf("}\n");
        printf("    }%s\n", (i<ENEMY_DEF_COUNT-1)?",":"");
    }
    printf("  ]\n}\n");
    return 0;
}
