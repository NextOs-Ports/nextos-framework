#ifndef SB_PRESENT_FBO_H
#define SB_PRESENT_FBO_H

/* A valid compositor may expose a single generated FBO. In particular,
 * dArkOS/KMSDRM reaches the menu with exactly three; waiting for a fourth
 * silently disables the owner aspect policy and leaves the engine bars. */
static inline int sb_present_fbo_known_set_ready(int known_count)
{
    return known_count > 0;
}

/* Bypass opt-in do display shader Netflix incompatível com o Mali-450. */
void sb_present_fullsize_fbo(int backbuffer_width, int backbuffer_height);

#endif
