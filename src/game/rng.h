/*============================================================================
 * rng.h - Lightweight Portable Random Number Generator
 *
 * 16-bit xorshift PRNG. Fast on Z80 (no multiply/divide), produces
 * reasonable distribution for game use. NOT cryptographic.
 *
 * Usage:
 *   rng_seed(some_value);         // seed once at startup
 *   uint16_t r = rng_next();     // get next random 0-65535
 *   uint8_t  d = rng_range(6);   // get 0..5
 *============================================================================*/

#ifndef RNG_H
#define RNG_H

#include "hal/hal_types.h"

/*--------------------------------------------------------------------------
 * State — a single 16-bit value. Seed must be nonzero.
 *--------------------------------------------------------------------------*/
static uint16_t s_rng_state = 1;

/*--------------------------------------------------------------------------
 * Seed the generator. Value must not be 0.
 *--------------------------------------------------------------------------*/
static void rng_seed(uint16_t seed) {
    s_rng_state = seed ? seed : 1;
}

/*--------------------------------------------------------------------------
 * 16-bit xorshift. Returns 1..65535 (never 0).
 * Period: 65535.
 *--------------------------------------------------------------------------*/
static uint16_t rng_next(void) {
    s_rng_state ^= s_rng_state << 7;
    s_rng_state ^= s_rng_state >> 9;
    s_rng_state ^= s_rng_state << 8;
    return s_rng_state;
}

/*--------------------------------------------------------------------------
 * Returns a random number in [0, max). Uses modulo bias but acceptable
 * for game purposes where max is small.
 *--------------------------------------------------------------------------*/
static uint8_t rng_range(uint8_t max) {
    if (max == 0) return 0;
    return (uint8_t)(rng_next() % max);
}

/*--------------------------------------------------------------------------
 * Returns a random number in [min, max] inclusive.
 *--------------------------------------------------------------------------*/
static uint16_t rng_range16(uint16_t min, uint16_t max) {
    if (min >= max) return min;
    return min + (rng_next() % (max - min + 1));
}

/*--------------------------------------------------------------------------
 * Returns 1 with probability pct/100, 0 otherwise.
 * pct should be 0..100.
 *--------------------------------------------------------------------------*/
static uint8_t rng_chance(uint8_t pct) {
    return (rng_range(100) < pct) ? 1 : 0;
}

#endif /* RNG_H */
