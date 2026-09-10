/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_gles2 -- resolucao em tempo de execucao do OpenGL ES 2.0 core.
 *
 * Mesmo motivo e mesmo algoritmo do nxgl_gles1 (0.2.14): nem toda CFW expoe o
 * SONAME "libGLESv2.so", e onde expoe ele pode ser uma biblioteca MORTA com os
 * SONAMEs cruzados (medido no dArkOS e no ROCKNIX do R36S). Um DT_NEEDED
 * libGLESv2.so nao carrega, ou carrega e nao desenha. Este modulo resolve os
 * 142 pontos de entrada do core ES2 por selecao de CONJUNTO com prova de
 * vida (glGetString(GL_RENDERER) do proprio candidato), nunca por nome.
 *
 * Alem dos ponteiros (nxgl_gles2_pfn_*), este modulo publica FUNCOES DE
 * DESPACHO estaveis (nxgl_gles2_dispatch_lookup): um so-loader em que a
 * ENGINE cria a janela/contexto precisa gravar enderecos no GOT da engine
 * ANTES de existir contexto GL. O dispatch e' um simbolo fixo que encaminha
 * pelo ponteiro resolvido depois, no nxgl_gles2_init().
 *
 * Uso no port (engine dona do contexto, ex.: Vector Unit/SDL3):
 *   tabela de imports: { "glClear", (uintptr_t)nxgl_gles2_dispatch_lookup("glClear") }
 *   no wrapper de SDL_GL_CreateContext, depois do contexto corrente:
 *     nxgl_gles2_set_primary_resolver((nxgl_gles2_resolver_fn)SDL_GL_GetProcAddress);
 *     nxgl_gles2_init(&receipt);
 */
#ifndef NXGL_GLES2_H
#define NXGL_GLES2_H

#include <GLES2/gl2.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_GLES2_SYMBOL_COUNT 142u
#define NXGL_GLES2_PROVIDER_MAX 64u
#define NXGL_GLES2_RECEIPT_MAX 256u

typedef struct nxgl_gles2_receipt {
  char provider[NXGL_GLES2_PROVIDER_MAX];
  unsigned resolved;
  unsigned total;
  char first_missing[NXGL_GLES2_PROVIDER_MAX];
  char text[NXGL_GLES2_RECEIPT_MAX];
} nxgl_gles2_receipt;

typedef void *(*nxgl_gles2_resolver_fn)(const char *name);
void nxgl_gles2_set_primary_resolver(nxgl_gles2_resolver_fn resolver);

/* Resolve todos os pontos de entrada. Idempotente. 0 = ok, <0 = faltou algo.
 * Chamar da thread do contexto GL, com o contexto corrente. */
int nxgl_gles2_init(nxgl_gles2_receipt *receipt);

/* Endereco do ponteiro ja' resolvido, ou NULL antes do init. */
void *nxgl_gles2_lookup(const char *name);

/* Endereco da FUNCAO DE DESPACHO estavel para o nome, ou NULL se o nome nao
 * pertence ao core ES2. Valido ANTES do init -- e' para o GOT da engine. */
void *nxgl_gles2_dispatch_lookup(const char *name);

const char *nxgl_gles2_provider(void);
const char *nxgl_gles2_liveness(void);

extern void (*nxgl_gles2_pfn_glActiveTexture)(GLenum texture);
extern void (*nxgl_gles2_pfn_glAttachShader)(GLuint program, GLuint shader);
extern void (*nxgl_gles2_pfn_glBindAttribLocation)(GLuint program, GLuint index, const GLchar *name);
extern void (*nxgl_gles2_pfn_glBindBuffer)(GLenum target, GLuint buffer);
extern void (*nxgl_gles2_pfn_glBindFramebuffer)(GLenum target, GLuint framebuffer);
extern void (*nxgl_gles2_pfn_glBindRenderbuffer)(GLenum target, GLuint renderbuffer);
extern void (*nxgl_gles2_pfn_glBindTexture)(GLenum target, GLuint texture);
extern void (*nxgl_gles2_pfn_glBlendColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
extern void (*nxgl_gles2_pfn_glBlendEquation)(GLenum mode);
extern void (*nxgl_gles2_pfn_glBlendEquationSeparate)(GLenum modeRGB, GLenum modeAlpha);
extern void (*nxgl_gles2_pfn_glBlendFunc)(GLenum sfactor, GLenum dfactor);
extern void (*nxgl_gles2_pfn_glBlendFuncSeparate)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
extern void (*nxgl_gles2_pfn_glBufferData)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
extern void (*nxgl_gles2_pfn_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
extern GLenum (*nxgl_gles2_pfn_glCheckFramebufferStatus)(GLenum target);
extern void (*nxgl_gles2_pfn_glClear)(GLbitfield mask);
extern void (*nxgl_gles2_pfn_glClearColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
extern void (*nxgl_gles2_pfn_glClearDepthf)(GLclampf d);
extern void (*nxgl_gles2_pfn_glClearStencil)(GLint s);
extern void (*nxgl_gles2_pfn_glColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
extern void (*nxgl_gles2_pfn_glCompileShader)(GLuint shader);
extern void (*nxgl_gles2_pfn_glCompressedTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
extern void (*nxgl_gles2_pfn_glCompressedTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data);
extern void (*nxgl_gles2_pfn_glCopyTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
extern void (*nxgl_gles2_pfn_glCopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
extern GLuint (*nxgl_gles2_pfn_glCreateProgram)(void);
extern GLuint (*nxgl_gles2_pfn_glCreateShader)(GLenum type);
extern void (*nxgl_gles2_pfn_glCullFace)(GLenum mode);
extern void (*nxgl_gles2_pfn_glDeleteBuffers)(GLsizei n, const GLuint *buffers);
extern void (*nxgl_gles2_pfn_glDeleteFramebuffers)(GLsizei n, const GLuint *framebuffers);
extern void (*nxgl_gles2_pfn_glDeleteProgram)(GLuint program);
extern void (*nxgl_gles2_pfn_glDeleteRenderbuffers)(GLsizei n, const GLuint *renderbuffers);
extern void (*nxgl_gles2_pfn_glDeleteShader)(GLuint shader);
extern void (*nxgl_gles2_pfn_glDeleteTextures)(GLsizei n, const GLuint *textures);
extern void (*nxgl_gles2_pfn_glDepthFunc)(GLenum func);
extern void (*nxgl_gles2_pfn_glDepthMask)(GLboolean flag);
extern void (*nxgl_gles2_pfn_glDepthRangef)(GLclampf n, GLclampf f);
extern void (*nxgl_gles2_pfn_glDetachShader)(GLuint program, GLuint shader);
extern void (*nxgl_gles2_pfn_glDisable)(GLenum cap);
extern void (*nxgl_gles2_pfn_glDisableVertexAttribArray)(GLuint index);
extern void (*nxgl_gles2_pfn_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
extern void (*nxgl_gles2_pfn_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices);
extern void (*nxgl_gles2_pfn_glEnable)(GLenum cap);
extern void (*nxgl_gles2_pfn_glEnableVertexAttribArray)(GLuint index);
extern void (*nxgl_gles2_pfn_glFinish)(void);
extern void (*nxgl_gles2_pfn_glFlush)(void);
extern void (*nxgl_gles2_pfn_glFramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
extern void (*nxgl_gles2_pfn_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
extern void (*nxgl_gles2_pfn_glFrontFace)(GLenum mode);
extern void (*nxgl_gles2_pfn_glGenBuffers)(GLsizei n, GLuint *buffers);
extern void (*nxgl_gles2_pfn_glGenerateMipmap)(GLenum target);
extern void (*nxgl_gles2_pfn_glGenFramebuffers)(GLsizei n, GLuint *framebuffers);
extern void (*nxgl_gles2_pfn_glGenRenderbuffers)(GLsizei n, GLuint *renderbuffers);
extern void (*nxgl_gles2_pfn_glGenTextures)(GLsizei n, GLuint *textures);
extern void (*nxgl_gles2_pfn_glGetActiveAttrib)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
extern void (*nxgl_gles2_pfn_glGetActiveUniform)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
extern void (*nxgl_gles2_pfn_glGetAttachedShaders)(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders);
extern GLint (*nxgl_gles2_pfn_glGetAttribLocation)(GLuint program, const GLchar *name);
extern void (*nxgl_gles2_pfn_glGetBooleanv)(GLenum pname, GLboolean *data);
extern void (*nxgl_gles2_pfn_glGetBufferParameteriv)(GLenum target, GLenum pname, GLint *params);
extern GLenum (*nxgl_gles2_pfn_glGetError)(void);
extern void (*nxgl_gles2_pfn_glGetFloatv)(GLenum pname, GLfloat *data);
extern void (*nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv)(GLenum target, GLenum attachment, GLenum pname, GLint *params);
extern void (*nxgl_gles2_pfn_glGetIntegerv)(GLenum pname, GLint *data);
extern void (*nxgl_gles2_pfn_glGetProgramiv)(GLuint program, GLenum pname, GLint *params);
extern void (*nxgl_gles2_pfn_glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
extern void (*nxgl_gles2_pfn_glGetRenderbufferParameteriv)(GLenum target, GLenum pname, GLint *params);
extern void (*nxgl_gles2_pfn_glGetShaderiv)(GLuint shader, GLenum pname, GLint *params);
extern void (*nxgl_gles2_pfn_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
extern void (*nxgl_gles2_pfn_glGetShaderPrecisionFormat)(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision);
extern void (*nxgl_gles2_pfn_glGetShaderSource)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);
extern void (*nxgl_gles2_pfn_glGetTexParameterfv)(GLenum target, GLenum pname, GLfloat *params);
extern void (*nxgl_gles2_pfn_glGetTexParameteriv)(GLenum target, GLenum pname, GLint *params);
extern void (*nxgl_gles2_pfn_glGetUniformfv)(GLuint program, GLint location, GLfloat *params);
extern void (*nxgl_gles2_pfn_glGetUniformiv)(GLuint program, GLint location, GLint *params);
extern GLint (*nxgl_gles2_pfn_glGetUniformLocation)(GLuint program, const GLchar *name);
extern void (*nxgl_gles2_pfn_glGetVertexAttribfv)(GLuint index, GLenum pname, GLfloat *params);
extern void (*nxgl_gles2_pfn_glGetVertexAttribiv)(GLuint index, GLenum pname, GLint *params);
extern void (*nxgl_gles2_pfn_glGetVertexAttribPointerv)(GLuint index, GLenum pname, void **pointer);
extern void (*nxgl_gles2_pfn_glHint)(GLenum target, GLenum mode);
extern GLboolean (*nxgl_gles2_pfn_glIsBuffer)(GLuint buffer);
extern GLboolean (*nxgl_gles2_pfn_glIsEnabled)(GLenum cap);
extern GLboolean (*nxgl_gles2_pfn_glIsFramebuffer)(GLuint framebuffer);
extern GLboolean (*nxgl_gles2_pfn_glIsProgram)(GLuint program);
extern GLboolean (*nxgl_gles2_pfn_glIsRenderbuffer)(GLuint renderbuffer);
extern GLboolean (*nxgl_gles2_pfn_glIsShader)(GLuint shader);
extern GLboolean (*nxgl_gles2_pfn_glIsTexture)(GLuint texture);
extern void (*nxgl_gles2_pfn_glLineWidth)(GLfloat width);
extern void (*nxgl_gles2_pfn_glLinkProgram)(GLuint program);
extern void (*nxgl_gles2_pfn_glPixelStorei)(GLenum pname, GLint param);
extern void (*nxgl_gles2_pfn_glPolygonOffset)(GLfloat factor, GLfloat units);
extern void (*nxgl_gles2_pfn_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
extern void (*nxgl_gles2_pfn_glReleaseShaderCompiler)(void);
extern void (*nxgl_gles2_pfn_glRenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
extern void (*nxgl_gles2_pfn_glSampleCoverage)(GLclampf value, GLboolean invert);
extern void (*nxgl_gles2_pfn_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
extern void (*nxgl_gles2_pfn_glShaderBinary)(GLsizei count, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length);
extern void (*nxgl_gles2_pfn_glShaderSource)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
extern void (*nxgl_gles2_pfn_glStencilFunc)(GLenum func, GLint ref, GLuint mask);
extern void (*nxgl_gles2_pfn_glStencilFuncSeparate)(GLenum face, GLenum func, GLint ref, GLuint mask);
extern void (*nxgl_gles2_pfn_glStencilMask)(GLuint mask);
extern void (*nxgl_gles2_pfn_glStencilMaskSeparate)(GLenum face, GLuint mask);
extern void (*nxgl_gles2_pfn_glStencilOp)(GLenum fail, GLenum zfail, GLenum zpass);
extern void (*nxgl_gles2_pfn_glStencilOpSeparate)(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
extern void (*nxgl_gles2_pfn_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
extern void (*nxgl_gles2_pfn_glTexParameterf)(GLenum target, GLenum pname, GLfloat param);
extern void (*nxgl_gles2_pfn_glTexParameterfv)(GLenum target, GLenum pname, const GLfloat *params);
extern void (*nxgl_gles2_pfn_glTexParameteri)(GLenum target, GLenum pname, GLint param);
extern void (*nxgl_gles2_pfn_glTexParameteriv)(GLenum target, GLenum pname, const GLint *params);
extern void (*nxgl_gles2_pfn_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
extern void (*nxgl_gles2_pfn_glUniform1f)(GLint location, GLfloat v0);
extern void (*nxgl_gles2_pfn_glUniform1fv)(GLint location, GLsizei count, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUniform1i)(GLint location, GLint v0);
extern void (*nxgl_gles2_pfn_glUniform1iv)(GLint location, GLsizei count, const GLint *value);
extern void (*nxgl_gles2_pfn_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
extern void (*nxgl_gles2_pfn_glUniform2fv)(GLint location, GLsizei count, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUniform2i)(GLint location, GLint v0, GLint v1);
extern void (*nxgl_gles2_pfn_glUniform2iv)(GLint location, GLsizei count, const GLint *value);
extern void (*nxgl_gles2_pfn_glUniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
extern void (*nxgl_gles2_pfn_glUniform3fv)(GLint location, GLsizei count, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUniform3i)(GLint location, GLint v0, GLint v1, GLint v2);
extern void (*nxgl_gles2_pfn_glUniform3iv)(GLint location, GLsizei count, const GLint *value);
extern void (*nxgl_gles2_pfn_glUniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
extern void (*nxgl_gles2_pfn_glUniform4fv)(GLint location, GLsizei count, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUniform4i)(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
extern void (*nxgl_gles2_pfn_glUniform4iv)(GLint location, GLsizei count, const GLint *value);
extern void (*nxgl_gles2_pfn_glUniformMatrix2fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUniformMatrix3fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
extern void (*nxgl_gles2_pfn_glUseProgram)(GLuint program);
extern void (*nxgl_gles2_pfn_glValidateProgram)(GLuint program);
extern void (*nxgl_gles2_pfn_glVertexAttrib1f)(GLuint index, GLfloat x);
extern void (*nxgl_gles2_pfn_glVertexAttrib1fv)(GLuint index, const GLfloat *v);
extern void (*nxgl_gles2_pfn_glVertexAttrib2f)(GLuint index, GLfloat x, GLfloat y);
extern void (*nxgl_gles2_pfn_glVertexAttrib2fv)(GLuint index, const GLfloat *v);
extern void (*nxgl_gles2_pfn_glVertexAttrib3f)(GLuint index, GLfloat x, GLfloat y, GLfloat z);
extern void (*nxgl_gles2_pfn_glVertexAttrib3fv)(GLuint index, const GLfloat *v);
extern void (*nxgl_gles2_pfn_glVertexAttrib4f)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
extern void (*nxgl_gles2_pfn_glVertexAttrib4fv)(GLuint index, const GLfloat *v);
extern void (*nxgl_gles2_pfn_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
extern void (*nxgl_gles2_pfn_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
extern const GLubyte * (*nxgl_gles2_pfn_glGetString)(GLenum name);

/* Redirecionamento das chamadas do proprio port. */
#ifndef NXGL_GLES2_NO_REDIRECT
#define glActiveTexture nxgl_gles2_pfn_glActiveTexture
#define glAttachShader nxgl_gles2_pfn_glAttachShader
#define glBindAttribLocation nxgl_gles2_pfn_glBindAttribLocation
#define glBindBuffer nxgl_gles2_pfn_glBindBuffer
#define glBindFramebuffer nxgl_gles2_pfn_glBindFramebuffer
#define glBindRenderbuffer nxgl_gles2_pfn_glBindRenderbuffer
#define glBindTexture nxgl_gles2_pfn_glBindTexture
#define glBlendColor nxgl_gles2_pfn_glBlendColor
#define glBlendEquation nxgl_gles2_pfn_glBlendEquation
#define glBlendEquationSeparate nxgl_gles2_pfn_glBlendEquationSeparate
#define glBlendFunc nxgl_gles2_pfn_glBlendFunc
#define glBlendFuncSeparate nxgl_gles2_pfn_glBlendFuncSeparate
#define glBufferData nxgl_gles2_pfn_glBufferData
#define glBufferSubData nxgl_gles2_pfn_glBufferSubData
#define glCheckFramebufferStatus nxgl_gles2_pfn_glCheckFramebufferStatus
#define glClear nxgl_gles2_pfn_glClear
#define glClearColor nxgl_gles2_pfn_glClearColor
#define glClearDepthf nxgl_gles2_pfn_glClearDepthf
#define glClearStencil nxgl_gles2_pfn_glClearStencil
#define glColorMask nxgl_gles2_pfn_glColorMask
#define glCompileShader nxgl_gles2_pfn_glCompileShader
#define glCompressedTexImage2D nxgl_gles2_pfn_glCompressedTexImage2D
#define glCompressedTexSubImage2D nxgl_gles2_pfn_glCompressedTexSubImage2D
#define glCopyTexImage2D nxgl_gles2_pfn_glCopyTexImage2D
#define glCopyTexSubImage2D nxgl_gles2_pfn_glCopyTexSubImage2D
#define glCreateProgram nxgl_gles2_pfn_glCreateProgram
#define glCreateShader nxgl_gles2_pfn_glCreateShader
#define glCullFace nxgl_gles2_pfn_glCullFace
#define glDeleteBuffers nxgl_gles2_pfn_glDeleteBuffers
#define glDeleteFramebuffers nxgl_gles2_pfn_glDeleteFramebuffers
#define glDeleteProgram nxgl_gles2_pfn_glDeleteProgram
#define glDeleteRenderbuffers nxgl_gles2_pfn_glDeleteRenderbuffers
#define glDeleteShader nxgl_gles2_pfn_glDeleteShader
#define glDeleteTextures nxgl_gles2_pfn_glDeleteTextures
#define glDepthFunc nxgl_gles2_pfn_glDepthFunc
#define glDepthMask nxgl_gles2_pfn_glDepthMask
#define glDepthRangef nxgl_gles2_pfn_glDepthRangef
#define glDetachShader nxgl_gles2_pfn_glDetachShader
#define glDisable nxgl_gles2_pfn_glDisable
#define glDisableVertexAttribArray nxgl_gles2_pfn_glDisableVertexAttribArray
#define glDrawArrays nxgl_gles2_pfn_glDrawArrays
#define glDrawElements nxgl_gles2_pfn_glDrawElements
#define glEnable nxgl_gles2_pfn_glEnable
#define glEnableVertexAttribArray nxgl_gles2_pfn_glEnableVertexAttribArray
#define glFinish nxgl_gles2_pfn_glFinish
#define glFlush nxgl_gles2_pfn_glFlush
#define glFramebufferRenderbuffer nxgl_gles2_pfn_glFramebufferRenderbuffer
#define glFramebufferTexture2D nxgl_gles2_pfn_glFramebufferTexture2D
#define glFrontFace nxgl_gles2_pfn_glFrontFace
#define glGenBuffers nxgl_gles2_pfn_glGenBuffers
#define glGenerateMipmap nxgl_gles2_pfn_glGenerateMipmap
#define glGenFramebuffers nxgl_gles2_pfn_glGenFramebuffers
#define glGenRenderbuffers nxgl_gles2_pfn_glGenRenderbuffers
#define glGenTextures nxgl_gles2_pfn_glGenTextures
#define glGetActiveAttrib nxgl_gles2_pfn_glGetActiveAttrib
#define glGetActiveUniform nxgl_gles2_pfn_glGetActiveUniform
#define glGetAttachedShaders nxgl_gles2_pfn_glGetAttachedShaders
#define glGetAttribLocation nxgl_gles2_pfn_glGetAttribLocation
#define glGetBooleanv nxgl_gles2_pfn_glGetBooleanv
#define glGetBufferParameteriv nxgl_gles2_pfn_glGetBufferParameteriv
#define glGetError nxgl_gles2_pfn_glGetError
#define glGetFloatv nxgl_gles2_pfn_glGetFloatv
#define glGetFramebufferAttachmentParameteriv nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv
#define glGetIntegerv nxgl_gles2_pfn_glGetIntegerv
#define glGetProgramiv nxgl_gles2_pfn_glGetProgramiv
#define glGetProgramInfoLog nxgl_gles2_pfn_glGetProgramInfoLog
#define glGetRenderbufferParameteriv nxgl_gles2_pfn_glGetRenderbufferParameteriv
#define glGetShaderiv nxgl_gles2_pfn_glGetShaderiv
#define glGetShaderInfoLog nxgl_gles2_pfn_glGetShaderInfoLog
#define glGetShaderPrecisionFormat nxgl_gles2_pfn_glGetShaderPrecisionFormat
#define glGetShaderSource nxgl_gles2_pfn_glGetShaderSource
#define glGetTexParameterfv nxgl_gles2_pfn_glGetTexParameterfv
#define glGetTexParameteriv nxgl_gles2_pfn_glGetTexParameteriv
#define glGetUniformfv nxgl_gles2_pfn_glGetUniformfv
#define glGetUniformiv nxgl_gles2_pfn_glGetUniformiv
#define glGetUniformLocation nxgl_gles2_pfn_glGetUniformLocation
#define glGetVertexAttribfv nxgl_gles2_pfn_glGetVertexAttribfv
#define glGetVertexAttribiv nxgl_gles2_pfn_glGetVertexAttribiv
#define glGetVertexAttribPointerv nxgl_gles2_pfn_glGetVertexAttribPointerv
#define glHint nxgl_gles2_pfn_glHint
#define glIsBuffer nxgl_gles2_pfn_glIsBuffer
#define glIsEnabled nxgl_gles2_pfn_glIsEnabled
#define glIsFramebuffer nxgl_gles2_pfn_glIsFramebuffer
#define glIsProgram nxgl_gles2_pfn_glIsProgram
#define glIsRenderbuffer nxgl_gles2_pfn_glIsRenderbuffer
#define glIsShader nxgl_gles2_pfn_glIsShader
#define glIsTexture nxgl_gles2_pfn_glIsTexture
#define glLineWidth nxgl_gles2_pfn_glLineWidth
#define glLinkProgram nxgl_gles2_pfn_glLinkProgram
#define glPixelStorei nxgl_gles2_pfn_glPixelStorei
#define glPolygonOffset nxgl_gles2_pfn_glPolygonOffset
#define glReadPixels nxgl_gles2_pfn_glReadPixels
#define glReleaseShaderCompiler nxgl_gles2_pfn_glReleaseShaderCompiler
#define glRenderbufferStorage nxgl_gles2_pfn_glRenderbufferStorage
#define glSampleCoverage nxgl_gles2_pfn_glSampleCoverage
#define glScissor nxgl_gles2_pfn_glScissor
#define glShaderBinary nxgl_gles2_pfn_glShaderBinary
#define glShaderSource nxgl_gles2_pfn_glShaderSource
#define glStencilFunc nxgl_gles2_pfn_glStencilFunc
#define glStencilFuncSeparate nxgl_gles2_pfn_glStencilFuncSeparate
#define glStencilMask nxgl_gles2_pfn_glStencilMask
#define glStencilMaskSeparate nxgl_gles2_pfn_glStencilMaskSeparate
#define glStencilOp nxgl_gles2_pfn_glStencilOp
#define glStencilOpSeparate nxgl_gles2_pfn_glStencilOpSeparate
#define glTexImage2D nxgl_gles2_pfn_glTexImage2D
#define glTexParameterf nxgl_gles2_pfn_glTexParameterf
#define glTexParameterfv nxgl_gles2_pfn_glTexParameterfv
#define glTexParameteri nxgl_gles2_pfn_glTexParameteri
#define glTexParameteriv nxgl_gles2_pfn_glTexParameteriv
#define glTexSubImage2D nxgl_gles2_pfn_glTexSubImage2D
#define glUniform1f nxgl_gles2_pfn_glUniform1f
#define glUniform1fv nxgl_gles2_pfn_glUniform1fv
#define glUniform1i nxgl_gles2_pfn_glUniform1i
#define glUniform1iv nxgl_gles2_pfn_glUniform1iv
#define glUniform2f nxgl_gles2_pfn_glUniform2f
#define glUniform2fv nxgl_gles2_pfn_glUniform2fv
#define glUniform2i nxgl_gles2_pfn_glUniform2i
#define glUniform2iv nxgl_gles2_pfn_glUniform2iv
#define glUniform3f nxgl_gles2_pfn_glUniform3f
#define glUniform3fv nxgl_gles2_pfn_glUniform3fv
#define glUniform3i nxgl_gles2_pfn_glUniform3i
#define glUniform3iv nxgl_gles2_pfn_glUniform3iv
#define glUniform4f nxgl_gles2_pfn_glUniform4f
#define glUniform4fv nxgl_gles2_pfn_glUniform4fv
#define glUniform4i nxgl_gles2_pfn_glUniform4i
#define glUniform4iv nxgl_gles2_pfn_glUniform4iv
#define glUniformMatrix2fv nxgl_gles2_pfn_glUniformMatrix2fv
#define glUniformMatrix3fv nxgl_gles2_pfn_glUniformMatrix3fv
#define glUniformMatrix4fv nxgl_gles2_pfn_glUniformMatrix4fv
#define glUseProgram nxgl_gles2_pfn_glUseProgram
#define glValidateProgram nxgl_gles2_pfn_glValidateProgram
#define glVertexAttrib1f nxgl_gles2_pfn_glVertexAttrib1f
#define glVertexAttrib1fv nxgl_gles2_pfn_glVertexAttrib1fv
#define glVertexAttrib2f nxgl_gles2_pfn_glVertexAttrib2f
#define glVertexAttrib2fv nxgl_gles2_pfn_glVertexAttrib2fv
#define glVertexAttrib3f nxgl_gles2_pfn_glVertexAttrib3f
#define glVertexAttrib3fv nxgl_gles2_pfn_glVertexAttrib3fv
#define glVertexAttrib4f nxgl_gles2_pfn_glVertexAttrib4f
#define glVertexAttrib4fv nxgl_gles2_pfn_glVertexAttrib4fv
#define glVertexAttribPointer nxgl_gles2_pfn_glVertexAttribPointer
#define glViewport nxgl_gles2_pfn_glViewport
#define glGetString nxgl_gles2_pfn_glGetString
#endif /* NXGL_GLES2_NO_REDIRECT */

#ifdef __cplusplus
}
#endif

#endif /* NXGL_GLES2_H */
