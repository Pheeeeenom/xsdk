/* xcomb — N64-style color-combiner -> Xenos pixel-shader generator.
 *
 * Dependency-free C99 port of xasm/combiner.py (+ the pixel paths of
 * xasm/program.py, xasm/encode.py, xdis/container_emit.py). Produces
 * byte-identical containers to the Python Generator for every valid
 * combiner configuration (proven by tests/test_xcomb_parity.py).
 *
 * Compiles one or two RDP combiner cycles
 *
 *      out = (A - B) * C + D       (RGB and Alpha pipelines independently)
 *
 * to a complete XDK-style pixel-shader container loadable by
 * Xe_LoadShaderFromMemory.
 *
 * Register/constant contract (pairs with the xost_vs2 interpolator layout):
 *      r0 = TEXCOORD0          c0 = PRIM (app-set via SetPixelShaderConstantF)
 *      r1 = SHADE              c1 = ENV
 *      r2 = TEXEL0 (tfetch0)   c2 = (1,1,1,1) for ONE
 *      r3 = scratch
 *      r4 = COMBINED (cycle-0 result, 2-cycle modes)
 *      r5 = TEXEL1 (tfetch1)
 * TEXEL0/TEXEL1 sample texture fetch constants 0/1 at TEXCOORD0.
 *
 * All output is serialized via explicit big-endian byte writes; the result
 * is identical on little- and big-endian hosts. No allocation: the caller
 * provides the output buffer (XCOMB_MAX_CONTAINER bytes always suffice).
 */
#ifndef XCOMB_H
#define XCOMB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XCOMB_ZERO,
    XCOMB_ONE,
    XCOMB_TEXEL0,
    XCOMB_TEXEL1,
    XCOMB_SHADE,
    XCOMB_PRIM,
    XCOMB_ENV,
    XCOMB_COMBINED   /* cycle-0 result; only valid in cycle 1 */
} xcomb_src;

/* (A-B)*C+D selects. a..d drive the RGB pipeline, aa..ad the alpha
 * pipeline; for "alpha same as color" set aa..ad equal to a..d. */
typedef struct {
    xcomb_src a, b, c, d;
    xcomb_src aa, ab, ac, ad;
} xcomb_cycle;

/* Generous upper bound on the container size for any valid configuration
 * (largest observed in fuzzing is well below this). */
#define XCOMB_MAX_CONTAINER 1024

/* Generates the pixel-shader container for a 1-cycle (cycle1 == NULL) or
 * 2-cycle combine into out[0..out_cap).
 *
 * Returns the container length in bytes, or 0 on error:
 *   - out == NULL / cycle0 == NULL,
 *   - out_cap too small,
 *   - invalid combo (XCOMB_COMBINED referenced in cycle 0, or any
 *     out-of-range xcomb_src value). */
size_t xcomb_generate(const xcomb_cycle *cycle0,
                      const xcomb_cycle *cycle1 /* NULL for 1-cycle */,
                      unsigned char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* XCOMB_H */
