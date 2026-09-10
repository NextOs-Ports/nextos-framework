#ifndef SDV_MONOGAME_GL_POLICY_H
#define SDV_MONOGAME_GL_POLICY_H

#include <string.h>

/*
 * MonoGame probes eglBindAPI through its libGL handle before selecting the
 * GLES library.  This port deliberately rejects libGL because SDL already
 * created the real OpenGL ES context.  On glibc, however, dlsym(NULL, ...)
 * searches the global namespace; Mesa's global eglBindAPI then accepts
 * EGL_OPENGL_API and MonoGame misclassifies the existing GLES context as
 * desktop GL.
 *
 * Hide only that leaked lookup, and only after the SDL/EGL bridge is ready.
 * MonoGame's own missing-entrypoint fallback selects GLES and then resolves
 * the core FBO functions through libGLESv2/SDL_GL_GetProcAddress.
 */
static inline int sdv_monogame_hide_global_egl_bind_api(
    void *handle, const char *name, int existing_gles_context_ready)
{
    return existing_gles_context_ready && handle == NULL && name != NULL &&
           strcmp(name, "eglBindAPI") == 0;
}

#endif
