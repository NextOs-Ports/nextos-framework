/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_gles1 -- resolucao em tempo de execucao do OpenGL ES 1.1 fixed-function.
 *
 * Motivo: nem toda CFW expoe o SONAME "libGLESv1_CM.so". O Mali-450 (Utgard)
 * do NextOS/EmuELEC expoe; o dArkOS/ArkOS do R36S (Mali-Bifrost G31) NAO expoe
 * arquivo nenhum com esse nome, embora o blob unificado "libMali.so" defina
 * todos os 45 pontos de entrada GLES1 usados pelos ports desta familia.
 *
 * Um port que declara DT_NEEDED libGLESv1_CM.so nao carrega nesses aparelhos.
 * Este modulo remove o DT_NEEDED: cada entrada e' resolvida em tempo de
 * execucao por uma cadeia de provedores, do mais especifico ao mais generico.
 *
 * Uso no port:
 *   #include "nxgl_gles1.h"          // depois dos headers GLES
 *   if (nxgl_gles1_init(&receipt) != 0) { ... }
 *   glClear(GL_COLOR_BUFFER_BIT);    // vira nxgl_gles1_pfn_glClear(...)
 *
 * Uso no so-loader (tabela de imports da engine Android):
 *   void *addr = nxgl_gles1_lookup("glDrawArrays");
 */
#ifndef NXGL_GLES1_H
#define NXGL_GLES1_H

#include <GLES/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_GLES1_SYMBOL_COUNT 47u
#define NXGL_GLES1_PROVIDER_MAX 64u
#define NXGL_GLES1_RECEIPT_MAX 256u
#define NXGL_GLES1_CANDIDATES_MAX 320u

typedef struct nxgl_gles1_receipt {
  char provider[NXGL_GLES1_PROVIDER_MAX];   /* provedor que resolveu o 1o simbolo */
  unsigned resolved;                        /* quantos dos SYMBOL_COUNT resolveram */
  unsigned total;                           /* == NXGL_GLES1_SYMBOL_COUNT */
  char first_missing[NXGL_GLES1_PROVIDER_MAX]; /* vazio se nada faltou */
  char text[NXGL_GLES1_RECEIPT_MAX];        /* linha de recibo pronta para log */
  /* V3 (aditivo, sempre no FIM da struct): rastro dos candidatos NA ORDEM em
   * que foram medidos, um por token "nome:razao" separado por espaco --
   * razoes: live (escolhido; a medicao para aqui: candidato posterior NAO
   * aparece porque nao foi invocado), dead (conjunto completo, mas o
   * glGetString do proprio candidato nao respondeu), incomplete (nao tem o
   * conjunto todo), absent (dlopen falhou / resolvedor ausente). E' a prova
   * auditavel de POR QUE cada candidato perdeu e de que um saudavel na
   * frente nunca aciona o degrau seguinte. */
  char candidates[NXGL_GLES1_CANDIDATES_MAX];
} nxgl_gles1_receipt;

/* 0.2.14: resolvedor primario injetado pelo port -- a fonte coerente com o
 * contexto VIVO por construcao. Chamar ANTES de nxgl_gles1_init, depois de
 * SDL_GL_CreateContext + MakeCurrent:
 *   nxgl_gles1_set_primary_resolver(
 *       (nxgl_gles1_resolver_fn)SDL_GL_GetProcAddress);
 * O init prova o conjunto com glGetString(GL_RENDERER) do proprio candidato
 * (liveness) e so' aceita provedor que responde; ver nxgl_gles1.c. Opcional:
 * sem o resolvedor, a selecao por medicao continua sobre RTLD_DEFAULT /
 * eglGetProcAddress / cadeia dlopen. */
typedef void *(*nxgl_gles1_resolver_fn)(const char *name);
void nxgl_gles1_set_primary_resolver(nxgl_gles1_resolver_fn resolver);

/* Resolve todos os pontos de entrada. Idempotente. 0 = ok, <0 = faltou algo.
 * receipt pode ser NULL. Contrato de thread: chamar da thread do GL (a mesma
 * do contexto), como os ports ja' fazem. */
int nxgl_gles1_init(nxgl_gles1_receipt *receipt);

/* Endereco de um ponto de entrada ja' resolvido, ou NULL. Nao resolve sozinho:
 * chame nxgl_gles1_init() antes. */
void *nxgl_gles1_lookup(const char *name);

/* Nome do provedor efetivo ("libGLESv1_CM.so.1", "libMali.so",
 * "primary-resolver", ...) ou "" se init ainda nao rodou. */
const char *nxgl_gles1_provider(void);

/* Veredito de vida do provedor escolhido: "ok" (glGetString do candidato
 * respondeu com contexto corrente), "dead" (nenhum candidato completo
 * respondeu; seguiu com o primeiro completo, comportamento v1), "mixed"
 * (montagem por simbolo de fontes distintas, ultimo recurso) ou "" antes do
 * init. "dead"/"mixed" sao sinal de tela preta provavel -- o recibo grita. */
const char *nxgl_gles1_liveness(void);

extern void (*nxgl_gles1_pfn_glAlphaFunc)(GLenum func, GLclampf ref);
extern void (*nxgl_gles1_pfn_glBindBuffer)(GLenum target, GLuint buffer);
extern void (*nxgl_gles1_pfn_glBindTexture)(GLenum target, GLuint texture);
extern void (*nxgl_gles1_pfn_glBlendFunc)(GLenum sfactor, GLenum dfactor);
extern void (*nxgl_gles1_pfn_glClear)(GLbitfield mask);
extern void (*nxgl_gles1_pfn_glClearColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
extern void (*nxgl_gles1_pfn_glColor4f)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
extern void (*nxgl_gles1_pfn_glColor4ub)(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
extern void (*nxgl_gles1_pfn_glColorPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
extern void (*nxgl_gles1_pfn_glCullFace)(GLenum mode);
extern void (*nxgl_gles1_pfn_glDeleteTextures)(GLsizei n, const GLuint *textures);
extern void (*nxgl_gles1_pfn_glDepthFunc)(GLenum func);
extern void (*nxgl_gles1_pfn_glDepthMask)(GLboolean flag);
extern void (*nxgl_gles1_pfn_glDisable)(GLenum cap);
extern void (*nxgl_gles1_pfn_glDisableClientState)(GLenum array);
extern void (*nxgl_gles1_pfn_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
extern void (*nxgl_gles1_pfn_glEnable)(GLenum cap);
extern void (*nxgl_gles1_pfn_glEnableClientState)(GLenum array);
extern void (*nxgl_gles1_pfn_glFogf)(GLenum pname, GLfloat param);
extern void (*nxgl_gles1_pfn_glFogfv)(GLenum pname, const GLfloat *params);
extern void (*nxgl_gles1_pfn_glGenTextures)(GLsizei n, GLuint *textures);
extern void (*nxgl_gles1_pfn_glGetBooleanv)(GLenum pname, GLboolean *params);
extern GLenum (*nxgl_gles1_pfn_glGetError)(void);
extern void (*nxgl_gles1_pfn_glGetFloatv)(GLenum pname, GLfloat *params);
extern void (*nxgl_gles1_pfn_glGetIntegerv)(GLenum pname, GLint *params);
extern void (*nxgl_gles1_pfn_glGetPointerv)(GLenum pname, void **params);
extern void (*nxgl_gles1_pfn_glLightfv)(GLenum light, GLenum pname, const GLfloat *params);
extern void (*nxgl_gles1_pfn_glLineWidth)(GLfloat width);
extern void (*nxgl_gles1_pfn_glLoadIdentity)(void);
extern void (*nxgl_gles1_pfn_glLoadMatrixf)(const GLfloat *m);
extern void (*nxgl_gles1_pfn_glMaterialfv)(GLenum face, GLenum pname, const GLfloat *params);
extern void (*nxgl_gles1_pfn_glMatrixMode)(GLenum mode);
extern void (*nxgl_gles1_pfn_glMultMatrixf)(const GLfloat *m);
extern void (*nxgl_gles1_pfn_glNormalPointer)(GLenum type, GLsizei stride, const void *pointer);
extern void (*nxgl_gles1_pfn_glOrthof)(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
extern void (*nxgl_gles1_pfn_glPopMatrix)(void);
extern void (*nxgl_gles1_pfn_glPushMatrix)(void);
extern void (*nxgl_gles1_pfn_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
extern void (*nxgl_gles1_pfn_glTexCoordPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
extern void (*nxgl_gles1_pfn_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
extern void (*nxgl_gles1_pfn_glTexParameteri)(GLenum target, GLenum pname, GLint param);
extern void (*nxgl_gles1_pfn_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
extern void (*nxgl_gles1_pfn_glTranslatef)(GLfloat x, GLfloat y, GLfloat z);
extern void (*nxgl_gles1_pfn_glVertexPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
extern void (*nxgl_gles1_pfn_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
extern void (*nxgl_gles1_pfn_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
extern const GLubyte * (*nxgl_gles1_pfn_glGetString)(GLenum name);

/* Redirecionamento das chamadas do port. Definido depois das declaracoes
 * acima para nao renomear os proprios ponteiros. */
#ifndef NXGL_GLES1_NO_REDIRECT
#define glAlphaFunc nxgl_gles1_pfn_glAlphaFunc
#define glBindBuffer nxgl_gles1_pfn_glBindBuffer
#define glBindTexture nxgl_gles1_pfn_glBindTexture
#define glBlendFunc nxgl_gles1_pfn_glBlendFunc
#define glClear nxgl_gles1_pfn_glClear
#define glClearColor nxgl_gles1_pfn_glClearColor
#define glColor4f nxgl_gles1_pfn_glColor4f
#define glColor4ub nxgl_gles1_pfn_glColor4ub
#define glColorPointer nxgl_gles1_pfn_glColorPointer
#define glCullFace nxgl_gles1_pfn_glCullFace
#define glDeleteTextures nxgl_gles1_pfn_glDeleteTextures
#define glDepthFunc nxgl_gles1_pfn_glDepthFunc
#define glDepthMask nxgl_gles1_pfn_glDepthMask
#define glDisable nxgl_gles1_pfn_glDisable
#define glDisableClientState nxgl_gles1_pfn_glDisableClientState
#define glDrawArrays nxgl_gles1_pfn_glDrawArrays
#define glEnable nxgl_gles1_pfn_glEnable
#define glEnableClientState nxgl_gles1_pfn_glEnableClientState
#define glFogf nxgl_gles1_pfn_glFogf
#define glFogfv nxgl_gles1_pfn_glFogfv
#define glGenTextures nxgl_gles1_pfn_glGenTextures
#define glGetBooleanv nxgl_gles1_pfn_glGetBooleanv
#define glGetError nxgl_gles1_pfn_glGetError
#define glGetFloatv nxgl_gles1_pfn_glGetFloatv
#define glGetIntegerv nxgl_gles1_pfn_glGetIntegerv
#define glGetPointerv nxgl_gles1_pfn_glGetPointerv
#define glLightfv nxgl_gles1_pfn_glLightfv
#define glLineWidth nxgl_gles1_pfn_glLineWidth
#define glLoadIdentity nxgl_gles1_pfn_glLoadIdentity
#define glLoadMatrixf nxgl_gles1_pfn_glLoadMatrixf
#define glMaterialfv nxgl_gles1_pfn_glMaterialfv
#define glMatrixMode nxgl_gles1_pfn_glMatrixMode
#define glMultMatrixf nxgl_gles1_pfn_glMultMatrixf
#define glNormalPointer nxgl_gles1_pfn_glNormalPointer
#define glOrthof nxgl_gles1_pfn_glOrthof
#define glPopMatrix nxgl_gles1_pfn_glPopMatrix
#define glPushMatrix nxgl_gles1_pfn_glPushMatrix
#define glScissor nxgl_gles1_pfn_glScissor
#define glTexCoordPointer nxgl_gles1_pfn_glTexCoordPointer
#define glTexImage2D nxgl_gles1_pfn_glTexImage2D
#define glTexParameteri nxgl_gles1_pfn_glTexParameteri
#define glTexSubImage2D nxgl_gles1_pfn_glTexSubImage2D
#define glTranslatef nxgl_gles1_pfn_glTranslatef
#define glVertexPointer nxgl_gles1_pfn_glVertexPointer
#define glViewport nxgl_gles1_pfn_glViewport
#define glReadPixels nxgl_gles1_pfn_glReadPixels
#define glGetString nxgl_gles1_pfn_glGetString
#endif /* NXGL_GLES1_NO_REDIRECT */

#ifdef __cplusplus
}
#endif

#endif /* NXGL_GLES1_H */
