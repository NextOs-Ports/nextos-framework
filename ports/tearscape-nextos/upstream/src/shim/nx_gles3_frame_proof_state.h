/* SPDX-License-Identifier: GPL-3.0-only
 * Logical state queried by the frame-proof adapter through the GLES3 facade.
 *
 * The Mali-450 remains a physical GLES2 renderer.  ES3-only queries must not
 * reach that driver: it rejects them with GL_INVALID_ENUM and leaves the
 * caller's sentinel untouched.  The facade already owns these states, so it
 * answers them without changing the physical context, framebuffer or pixels.
 */
#ifndef TEARSCAPE_NX_GLES3_FRAME_PROOF_STATE_H
#define TEARSCAPE_NX_GLES3_FRAME_PROOF_STATE_H

#include <GLES3/gl3.h>

#if GL_PACK_ROW_LENGTH != 0x0D02
#error "unexpected GL_PACK_ROW_LENGTH value"
#endif
#if GL_PACK_SKIP_ROWS != 0x0D03
#error "unexpected GL_PACK_SKIP_ROWS value"
#endif
#if GL_PACK_SKIP_PIXELS != 0x0D04
#error "unexpected GL_PACK_SKIP_PIXELS value"
#endif

static inline int nx_gles3_frame_proof_state_query(
        GLenum pname, GLint *data, GLuint framebuffer, GLuint pixel_pack)
{
    switch (pname) {
    case GL_FRAMEBUFFER_BINDING:
    case GL_READ_FRAMEBUFFER_BINDING:
        *data = (GLint)framebuffer;
        return 1;
    case GL_PIXEL_PACK_BUFFER_BINDING:
        *data = (GLint)pixel_pack;
        return 1;
    case GL_PACK_ROW_LENGTH:
    case GL_PACK_SKIP_ROWS:
    case GL_PACK_SKIP_PIXELS:
        /* The physical GLES2 pack layout has no row/skip extension here. */
        *data = 0;
        return 1;
    default:
        return 0;
    }
}

#endif
