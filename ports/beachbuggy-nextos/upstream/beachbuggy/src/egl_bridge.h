/* SPDX-License-Identifier: GPL-3.0-only */
/* Ponte EGL resolvida em tempo de execucao (sem DT_NEEDED libEGL). Ver
 * egl_bridge.c. Os tipos vem do header EGL padrao; nenhum simbolo egl* real
 * e' referenciado pelo binario. */
#ifndef EGL_BRIDGE_H
#define EGL_BRIDGE_H

#include <EGL/egl.h>

EGLBoolean nxegl_eglBindAPI(EGLenum api);
EGLContext nxegl_eglCreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share, const EGLint *attrib_list);
EGLSurface nxegl_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                         const EGLint *attrib_list);
EGLSurface nxegl_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                        EGLNativeWindowType win,
                                        const EGLint *attrib_list);
EGLBoolean nxegl_eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean nxegl_eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean nxegl_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                    EGLint attribute, EGLint *value);
EGLBoolean nxegl_eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
                               EGLint config_size, EGLint *num_config);
EGLContext nxegl_eglGetCurrentContext(void);
EGLDisplay nxegl_eglGetDisplay(EGLNativeDisplayType display_id);
__eglMustCastToProperFunctionPointerType
nxegl_eglGetProcAddress(const char *procname);
EGLBoolean nxegl_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean nxegl_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                EGLSurface read, EGLContext ctx);
EGLBoolean nxegl_eglQueryContext(EGLDisplay dpy, EGLContext ctx,
                                 EGLint attribute, EGLint *value);
const char *nxegl_eglQueryString(EGLDisplay dpy, EGLint name);
EGLBoolean nxegl_eglTerminate(EGLDisplay dpy);

#endif /* EGL_BRIDGE_H */
