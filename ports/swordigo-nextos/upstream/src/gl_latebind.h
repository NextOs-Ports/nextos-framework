#ifndef SWORDIGO_GL_LATEBIND_H
#define SWORDIGO_GL_LATEBIND_H

/* Configure driver-specific GLES1 compatibility after a live context exists. */
void gl_latebind_configure(const char *video_driver, const char *renderer,
                           const char *version);

#endif
