/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>

#define OTR_EGL_STUB(name) void *name(void) { return (void *)(uintptr_t)1u; }

#if defined(OTR_EGL_FIXTURE_MISSING_SWAP)
OTR_EGL_STUB(otr_egl_fixture_incomplete_marker)
#elif defined(OTR_EGL_FIXTURE_NULL_CONTEXT)
OTR_EGL_STUB(otr_egl_fixture_null_context_marker)
#else
OTR_EGL_STUB(otr_egl_fixture_complete_marker)
#endif

OTR_EGL_STUB(eglChooseConfig)
OTR_EGL_STUB(eglCreateContext)
OTR_EGL_STUB(eglCreateWindowSurface)
OTR_EGL_STUB(eglDestroyContext)
OTR_EGL_STUB(eglDestroySurface)
OTR_EGL_STUB(eglGetConfigAttrib)

void *eglGetCurrentContext(void) {
#ifdef OTR_EGL_FIXTURE_NULL_CONTEXT
  return 0;
#else
  return (void *)(uintptr_t)0x1234u;
#endif
}

OTR_EGL_STUB(eglGetDisplay)
OTR_EGL_STUB(eglInitialize)
OTR_EGL_STUB(eglMakeCurrent)
OTR_EGL_STUB(eglQueryString)
#ifndef OTR_EGL_FIXTURE_MISSING_SWAP
OTR_EGL_STUB(eglSwapBuffers)
#endif
OTR_EGL_STUB(eglSwapInterval)
OTR_EGL_STUB(eglTerminate)
