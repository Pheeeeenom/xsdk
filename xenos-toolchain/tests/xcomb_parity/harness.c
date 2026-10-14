/* xcomb parity harness.
 *
 * Usage:
 *   harness 1 a b c d aa ab ac ad
 *   harness 2 a b c d aa ab ac ad  a b c d aa ab ac ad
 *
 * Each value is an xcomb_src integer (0..7, see xcomb.h enum order:
 * ZERO ONE TEXEL0 TEXEL1 SHADE PRIM ENV COMBINED). Writes the generated
 * container to stdout as lowercase hex (one line), or prints "ERR" and
 * exits 1 if xcomb_generate reports an error.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../../xcomb/xcomb.h"

static int parse_cycle(char **argv, xcomb_cycle *cy)
{
    int v[8], i;
    for (i = 0; i < 8; i++) {
        char *end = NULL;
        long x = strtol(argv[i], &end, 10);
        if (!end || *end != '\0' || x < -64 || x > 64)
            return 0;
        v[i] = (int)x;
    }
    cy->a  = (xcomb_src)v[0];
    cy->b  = (xcomb_src)v[1];
    cy->c  = (xcomb_src)v[2];
    cy->d  = (xcomb_src)v[3];
    cy->aa = (xcomb_src)v[4];
    cy->ab = (xcomb_src)v[5];
    cy->ac = (xcomb_src)v[6];
    cy->ad = (xcomb_src)v[7];
    return 1;
}

int main(int argc, char **argv)
{
    xcomb_cycle c0, c1;
    int n_cycles;
    unsigned char buf[XCOMB_MAX_CONTAINER];
    size_t n, i;

    if (argc < 2) {
        fprintf(stderr, "usage: harness <1|2> <8 ints> [<8 ints>]\n");
        return 2;
    }
    n_cycles = atoi(argv[1]);
    if ((n_cycles != 1 && n_cycles != 2) || argc != 2 + 8 * n_cycles) {
        fprintf(stderr, "bad argument count\n");
        return 2;
    }
    if (!parse_cycle(argv + 2, &c0) ||
        (n_cycles == 2 && !parse_cycle(argv + 10, &c1))) {
        fprintf(stderr, "bad cycle value\n");
        return 2;
    }

    n = xcomb_generate(&c0, n_cycles == 2 ? &c1 : NULL, buf, sizeof(buf));
    if (n == 0) {
        printf("ERR\n");
        return 1;
    }
    for (i = 0; i < n; i++)
        printf("%02x", buf[i]);
    printf("\n");
    return 0;
}
