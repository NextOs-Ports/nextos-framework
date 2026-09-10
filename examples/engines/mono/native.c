/* SPDX-License-Identifier: GPL-3.0-only */
/* Narrow original managed/native ABI, not an Android runtime replacement. */
#include <stdint.h>
struct sample { int32_t x, y; };
typedef int32_t (*visitor)(int32_t);
int32_t nextos_transform(struct sample input, struct sample *output, visitor callback) {
    if(!output || !callback || input.x < -1000 || input.x > 1000) return -1;
    output->x=callback(input.x);output->y=input.y;
    return 0;
}
