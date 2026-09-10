#include "../src/present_fbo.h"

#include <stdio.h>

int main(void)
{
    if (sb_present_fbo_known_set_ready(-1) ||
        sb_present_fbo_known_set_ready(0) ||
        !sb_present_fbo_known_set_ready(1) ||
        !sb_present_fbo_known_set_ready(3) ||
        !sb_present_fbo_known_set_ready(4)) {
        fputs("BLOSSOM PRESENT FBO SCAN: FAIL\n", stderr);
        return 1;
    }
    puts("BLOSSOM PRESENT FBO SCAN: PASS zero=wait one/three/four=scan");
    return 0;
}
