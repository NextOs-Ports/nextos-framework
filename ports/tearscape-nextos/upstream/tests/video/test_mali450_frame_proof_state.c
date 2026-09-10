/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "../../src/shim/nx_gles3_frame_proof_state.h"

static int check(GLenum pname, GLint expected, GLuint fbo, GLuint pbo)
{
    GLint value = -31337;
    return nx_gles3_frame_proof_state_query(pname, &value, fbo, pbo) == 1 &&
           value == expected;
}

int main(void)
{
    GLint untouched = -31337;

    if (!check(GL_FRAMEBUFFER_BINDING, 7, 7, 11) ||
        !check(GL_READ_FRAMEBUFFER_BINDING, 7, 7, 11) ||
        !check(GL_PIXEL_PACK_BUFFER_BINDING, 11, 7, 11) ||
        !check(GL_PACK_ROW_LENGTH, 0, 7, 11) ||
        !check(GL_PACK_SKIP_ROWS, 0, 7, 11) ||
        !check(GL_PACK_SKIP_PIXELS, 0, 7, 11) ||
        nx_gles3_frame_proof_state_query(
            GL_VIEWPORT, &untouched, 7, 11) != 0 ||
        untouched != -31337) {
        fputs("MALI450 FRAME-PROOF STATE: FAIL\n", stderr);
        return 1;
    }

    puts("MALI450 FRAME-PROOF STATE: PASS physical=GLES2 facade=GLES3");
    return 0;
}
