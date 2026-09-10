/* ab_egl.h — entradas EGL do proprio loader ligadas pelo nxgl_egl_binding (V4).
 *
 * O loader nunca importa EGL por DT_NEEDED nem por simbolo indefinido com
 * resolucao preguicosa: os onze pontos de entrada que ele usa (contextos
 * compartilhados do EGLWrapper) sao resolvidos UMA vez, depois de o nxgl abrir
 * o contexto, pelo provedor que realmente possui o contexto corrente
 * (nxgl_egl_binding: resolver do SDL + dladdr + prova de imports + prova de
 * contexto corrente). Sob GLVND (ROCKNIX/Mesa) isso cai em libEGL.so.1; no
 * blob Mali cai no proprio blob. Nenhum nome de biblioteca e' cravado.
 *
 * Incluir DEPOIS de <EGL/egl.h>. Sem AB_EGL_NO_REDIRECT, as chamadas egl*
 * do arquivo passam pelos ponteiros.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_EGL_H
#define AB_EGL_H
#include <EGL/egl.h>

extern EGLBoolean (*ab_egl_pfn_eglQueryContext)(EGLDisplay, EGLContext, EGLint, EGLint *);
extern EGLContext (*ab_egl_pfn_eglGetCurrentContext)(void);
extern EGLBoolean (*ab_egl_pfn_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
extern EGLSurface (*ab_egl_pfn_eglGetCurrentSurface)(EGLint);
extern EGLDisplay (*ab_egl_pfn_eglGetCurrentDisplay)(void);
extern EGLContext (*ab_egl_pfn_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
extern EGLint (*ab_egl_pfn_eglGetError)(void);
extern EGLSurface (*ab_egl_pfn_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
extern EGLBoolean (*ab_egl_pfn_eglDestroyContext)(EGLDisplay, EGLContext);
extern EGLBoolean (*ab_egl_pfn_eglDestroySurface)(EGLDisplay, EGLSurface);
extern EGLBoolean (*ab_egl_pfn_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);

/* Liga a tabela ao provedor do contexto corrente. Chamar da thread do
 * contexto, com ele corrente. 1 = ok; 0 = falha (fechada, nada promovido). */
int ab_egl_bind(void);
/* Recibo one-shot (status, provedor, imports) para o log. */
const char *ab_egl_receipt(void);

#ifndef AB_EGL_NO_REDIRECT
#define eglQueryContext ab_egl_pfn_eglQueryContext
#define eglGetCurrentContext ab_egl_pfn_eglGetCurrentContext
#define eglChooseConfig ab_egl_pfn_eglChooseConfig
#define eglGetCurrentSurface ab_egl_pfn_eglGetCurrentSurface
#define eglGetCurrentDisplay ab_egl_pfn_eglGetCurrentDisplay
#define eglCreateContext ab_egl_pfn_eglCreateContext
#define eglGetError ab_egl_pfn_eglGetError
#define eglCreatePbufferSurface ab_egl_pfn_eglCreatePbufferSurface
#define eglDestroyContext ab_egl_pfn_eglDestroyContext
#define eglDestroySurface ab_egl_pfn_eglDestroySurface
#define eglMakeCurrent ab_egl_pfn_eglMakeCurrent
#endif
#endif
