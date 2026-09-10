/* SPDX-License-Identifier: GPL-3.0-only */
/* Implementacao do resolvedor GLES2. Gerado por tools de nxgl a partir da
 * tabela core ES2; algoritmo identico ao nxgl_gles1.c 0.2.14 (selecao por
 * conjunto com prova de vida). Ver nxgl_gles2.h para o motivo. */
#define NXGL_GLES2_NO_REDIRECT 1

#include "nxgl_gles2.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* A mesma licao do nxgl_gles1: nenhuma ordem fixa de SONAME acerta em todas
 * as firmwares (dArkOS x ROCKNIX tem os nomes cruzados em sentidos opostos).
 * A ordem abaixo so' desempata entre candidatos igualmente completos e VIVOS;
 * quem decide e' a prova de vida. */
static const char *const k_providers[] = {
  "libmali.so",
  "libMali.so",
  "libGLES_mali.so",
  "libGLESv2.so",
  "libmali.so.1",
  "libGLESv2.so.2",
};

void (*nxgl_gles2_pfn_glActiveTexture)(GLenum texture);
void (*nxgl_gles2_pfn_glAttachShader)(GLuint program, GLuint shader);
void (*nxgl_gles2_pfn_glBindAttribLocation)(GLuint program, GLuint index, const GLchar *name);
void (*nxgl_gles2_pfn_glBindBuffer)(GLenum target, GLuint buffer);
void (*nxgl_gles2_pfn_glBindFramebuffer)(GLenum target, GLuint framebuffer);
void (*nxgl_gles2_pfn_glBindRenderbuffer)(GLenum target, GLuint renderbuffer);
void (*nxgl_gles2_pfn_glBindTexture)(GLenum target, GLuint texture);
void (*nxgl_gles2_pfn_glBlendColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void (*nxgl_gles2_pfn_glBlendEquation)(GLenum mode);
void (*nxgl_gles2_pfn_glBlendEquationSeparate)(GLenum modeRGB, GLenum modeAlpha);
void (*nxgl_gles2_pfn_glBlendFunc)(GLenum sfactor, GLenum dfactor);
void (*nxgl_gles2_pfn_glBlendFuncSeparate)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
void (*nxgl_gles2_pfn_glBufferData)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void (*nxgl_gles2_pfn_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
GLenum (*nxgl_gles2_pfn_glCheckFramebufferStatus)(GLenum target);
void (*nxgl_gles2_pfn_glClear)(GLbitfield mask);
void (*nxgl_gles2_pfn_glClearColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void (*nxgl_gles2_pfn_glClearDepthf)(GLclampf d);
void (*nxgl_gles2_pfn_glClearStencil)(GLint s);
void (*nxgl_gles2_pfn_glColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void (*nxgl_gles2_pfn_glCompileShader)(GLuint shader);
void (*nxgl_gles2_pfn_glCompressedTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
void (*nxgl_gles2_pfn_glCompressedTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data);
void (*nxgl_gles2_pfn_glCopyTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void (*nxgl_gles2_pfn_glCopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
GLuint (*nxgl_gles2_pfn_glCreateProgram)(void);
GLuint (*nxgl_gles2_pfn_glCreateShader)(GLenum type);
void (*nxgl_gles2_pfn_glCullFace)(GLenum mode);
void (*nxgl_gles2_pfn_glDeleteBuffers)(GLsizei n, const GLuint *buffers);
void (*nxgl_gles2_pfn_glDeleteFramebuffers)(GLsizei n, const GLuint *framebuffers);
void (*nxgl_gles2_pfn_glDeleteProgram)(GLuint program);
void (*nxgl_gles2_pfn_glDeleteRenderbuffers)(GLsizei n, const GLuint *renderbuffers);
void (*nxgl_gles2_pfn_glDeleteShader)(GLuint shader);
void (*nxgl_gles2_pfn_glDeleteTextures)(GLsizei n, const GLuint *textures);
void (*nxgl_gles2_pfn_glDepthFunc)(GLenum func);
void (*nxgl_gles2_pfn_glDepthMask)(GLboolean flag);
void (*nxgl_gles2_pfn_glDepthRangef)(GLclampf n, GLclampf f);
void (*nxgl_gles2_pfn_glDetachShader)(GLuint program, GLuint shader);
void (*nxgl_gles2_pfn_glDisable)(GLenum cap);
void (*nxgl_gles2_pfn_glDisableVertexAttribArray)(GLuint index);
void (*nxgl_gles2_pfn_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
void (*nxgl_gles2_pfn_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices);
void (*nxgl_gles2_pfn_glEnable)(GLenum cap);
void (*nxgl_gles2_pfn_glEnableVertexAttribArray)(GLuint index);
void (*nxgl_gles2_pfn_glFinish)(void);
void (*nxgl_gles2_pfn_glFlush)(void);
void (*nxgl_gles2_pfn_glFramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void (*nxgl_gles2_pfn_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void (*nxgl_gles2_pfn_glFrontFace)(GLenum mode);
void (*nxgl_gles2_pfn_glGenBuffers)(GLsizei n, GLuint *buffers);
void (*nxgl_gles2_pfn_glGenerateMipmap)(GLenum target);
void (*nxgl_gles2_pfn_glGenFramebuffers)(GLsizei n, GLuint *framebuffers);
void (*nxgl_gles2_pfn_glGenRenderbuffers)(GLsizei n, GLuint *renderbuffers);
void (*nxgl_gles2_pfn_glGenTextures)(GLsizei n, GLuint *textures);
void (*nxgl_gles2_pfn_glGetActiveAttrib)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void (*nxgl_gles2_pfn_glGetActiveUniform)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void (*nxgl_gles2_pfn_glGetAttachedShaders)(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders);
GLint (*nxgl_gles2_pfn_glGetAttribLocation)(GLuint program, const GLchar *name);
void (*nxgl_gles2_pfn_glGetBooleanv)(GLenum pname, GLboolean *data);
void (*nxgl_gles2_pfn_glGetBufferParameteriv)(GLenum target, GLenum pname, GLint *params);
GLenum (*nxgl_gles2_pfn_glGetError)(void);
void (*nxgl_gles2_pfn_glGetFloatv)(GLenum pname, GLfloat *data);
void (*nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv)(GLenum target, GLenum attachment, GLenum pname, GLint *params);
void (*nxgl_gles2_pfn_glGetIntegerv)(GLenum pname, GLint *data);
void (*nxgl_gles2_pfn_glGetProgramiv)(GLuint program, GLenum pname, GLint *params);
void (*nxgl_gles2_pfn_glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void (*nxgl_gles2_pfn_glGetRenderbufferParameteriv)(GLenum target, GLenum pname, GLint *params);
void (*nxgl_gles2_pfn_glGetShaderiv)(GLuint shader, GLenum pname, GLint *params);
void (*nxgl_gles2_pfn_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void (*nxgl_gles2_pfn_glGetShaderPrecisionFormat)(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision);
void (*nxgl_gles2_pfn_glGetShaderSource)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);
void (*nxgl_gles2_pfn_glGetTexParameterfv)(GLenum target, GLenum pname, GLfloat *params);
void (*nxgl_gles2_pfn_glGetTexParameteriv)(GLenum target, GLenum pname, GLint *params);
void (*nxgl_gles2_pfn_glGetUniformfv)(GLuint program, GLint location, GLfloat *params);
void (*nxgl_gles2_pfn_glGetUniformiv)(GLuint program, GLint location, GLint *params);
GLint (*nxgl_gles2_pfn_glGetUniformLocation)(GLuint program, const GLchar *name);
void (*nxgl_gles2_pfn_glGetVertexAttribfv)(GLuint index, GLenum pname, GLfloat *params);
void (*nxgl_gles2_pfn_glGetVertexAttribiv)(GLuint index, GLenum pname, GLint *params);
void (*nxgl_gles2_pfn_glGetVertexAttribPointerv)(GLuint index, GLenum pname, void **pointer);
void (*nxgl_gles2_pfn_glHint)(GLenum target, GLenum mode);
GLboolean (*nxgl_gles2_pfn_glIsBuffer)(GLuint buffer);
GLboolean (*nxgl_gles2_pfn_glIsEnabled)(GLenum cap);
GLboolean (*nxgl_gles2_pfn_glIsFramebuffer)(GLuint framebuffer);
GLboolean (*nxgl_gles2_pfn_glIsProgram)(GLuint program);
GLboolean (*nxgl_gles2_pfn_glIsRenderbuffer)(GLuint renderbuffer);
GLboolean (*nxgl_gles2_pfn_glIsShader)(GLuint shader);
GLboolean (*nxgl_gles2_pfn_glIsTexture)(GLuint texture);
void (*nxgl_gles2_pfn_glLineWidth)(GLfloat width);
void (*nxgl_gles2_pfn_glLinkProgram)(GLuint program);
void (*nxgl_gles2_pfn_glPixelStorei)(GLenum pname, GLint param);
void (*nxgl_gles2_pfn_glPolygonOffset)(GLfloat factor, GLfloat units);
void (*nxgl_gles2_pfn_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
void (*nxgl_gles2_pfn_glReleaseShaderCompiler)(void);
void (*nxgl_gles2_pfn_glRenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void (*nxgl_gles2_pfn_glSampleCoverage)(GLclampf value, GLboolean invert);
void (*nxgl_gles2_pfn_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
void (*nxgl_gles2_pfn_glShaderBinary)(GLsizei count, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length);
void (*nxgl_gles2_pfn_glShaderSource)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
void (*nxgl_gles2_pfn_glStencilFunc)(GLenum func, GLint ref, GLuint mask);
void (*nxgl_gles2_pfn_glStencilFuncSeparate)(GLenum face, GLenum func, GLint ref, GLuint mask);
void (*nxgl_gles2_pfn_glStencilMask)(GLuint mask);
void (*nxgl_gles2_pfn_glStencilMaskSeparate)(GLenum face, GLuint mask);
void (*nxgl_gles2_pfn_glStencilOp)(GLenum fail, GLenum zfail, GLenum zpass);
void (*nxgl_gles2_pfn_glStencilOpSeparate)(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
void (*nxgl_gles2_pfn_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
void (*nxgl_gles2_pfn_glTexParameterf)(GLenum target, GLenum pname, GLfloat param);
void (*nxgl_gles2_pfn_glTexParameterfv)(GLenum target, GLenum pname, const GLfloat *params);
void (*nxgl_gles2_pfn_glTexParameteri)(GLenum target, GLenum pname, GLint param);
void (*nxgl_gles2_pfn_glTexParameteriv)(GLenum target, GLenum pname, const GLint *params);
void (*nxgl_gles2_pfn_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
void (*nxgl_gles2_pfn_glUniform1f)(GLint location, GLfloat v0);
void (*nxgl_gles2_pfn_glUniform1fv)(GLint location, GLsizei count, const GLfloat *value);
void (*nxgl_gles2_pfn_glUniform1i)(GLint location, GLint v0);
void (*nxgl_gles2_pfn_glUniform1iv)(GLint location, GLsizei count, const GLint *value);
void (*nxgl_gles2_pfn_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
void (*nxgl_gles2_pfn_glUniform2fv)(GLint location, GLsizei count, const GLfloat *value);
void (*nxgl_gles2_pfn_glUniform2i)(GLint location, GLint v0, GLint v1);
void (*nxgl_gles2_pfn_glUniform2iv)(GLint location, GLsizei count, const GLint *value);
void (*nxgl_gles2_pfn_glUniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void (*nxgl_gles2_pfn_glUniform3fv)(GLint location, GLsizei count, const GLfloat *value);
void (*nxgl_gles2_pfn_glUniform3i)(GLint location, GLint v0, GLint v1, GLint v2);
void (*nxgl_gles2_pfn_glUniform3iv)(GLint location, GLsizei count, const GLint *value);
void (*nxgl_gles2_pfn_glUniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void (*nxgl_gles2_pfn_glUniform4fv)(GLint location, GLsizei count, const GLfloat *value);
void (*nxgl_gles2_pfn_glUniform4i)(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
void (*nxgl_gles2_pfn_glUniform4iv)(GLint location, GLsizei count, const GLint *value);
void (*nxgl_gles2_pfn_glUniformMatrix2fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void (*nxgl_gles2_pfn_glUniformMatrix3fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void (*nxgl_gles2_pfn_glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void (*nxgl_gles2_pfn_glUseProgram)(GLuint program);
void (*nxgl_gles2_pfn_glValidateProgram)(GLuint program);
void (*nxgl_gles2_pfn_glVertexAttrib1f)(GLuint index, GLfloat x);
void (*nxgl_gles2_pfn_glVertexAttrib1fv)(GLuint index, const GLfloat *v);
void (*nxgl_gles2_pfn_glVertexAttrib2f)(GLuint index, GLfloat x, GLfloat y);
void (*nxgl_gles2_pfn_glVertexAttrib2fv)(GLuint index, const GLfloat *v);
void (*nxgl_gles2_pfn_glVertexAttrib3f)(GLuint index, GLfloat x, GLfloat y, GLfloat z);
void (*nxgl_gles2_pfn_glVertexAttrib3fv)(GLuint index, const GLfloat *v);
void (*nxgl_gles2_pfn_glVertexAttrib4f)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void (*nxgl_gles2_pfn_glVertexAttrib4fv)(GLuint index, const GLfloat *v);
void (*nxgl_gles2_pfn_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
void (*nxgl_gles2_pfn_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
const GLubyte * (*nxgl_gles2_pfn_glGetString)(GLenum name);

struct nxgl_gles2_entry {
  const char *name;
  void **slot;
  void *dispatch;
};

/* Funcoes de despacho estaveis: o GOT da engine aponta para elas antes de
 * existir contexto; elas encaminham pelo ponteiro resolvido no init. */
static void nxgl_gles2_lazy_init(void);
static void nxgl_gles2_d_glActiveTexture(GLenum texture) { if (nxgl_gles2_pfn_glActiveTexture == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glActiveTexture == NULL) return; nxgl_gles2_pfn_glActiveTexture(texture); }
static void nxgl_gles2_d_glAttachShader(GLuint program, GLuint shader) { if (nxgl_gles2_pfn_glAttachShader == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glAttachShader == NULL) return; nxgl_gles2_pfn_glAttachShader(program, shader); }
static void nxgl_gles2_d_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) { if (nxgl_gles2_pfn_glBindAttribLocation == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBindAttribLocation == NULL) return; nxgl_gles2_pfn_glBindAttribLocation(program, index, name); }
static void nxgl_gles2_d_glBindBuffer(GLenum target, GLuint buffer) { if (nxgl_gles2_pfn_glBindBuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBindBuffer == NULL) return; nxgl_gles2_pfn_glBindBuffer(target, buffer); }
static void nxgl_gles2_d_glBindFramebuffer(GLenum target, GLuint framebuffer) { if (nxgl_gles2_pfn_glBindFramebuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBindFramebuffer == NULL) return; nxgl_gles2_pfn_glBindFramebuffer(target, framebuffer); }
static void nxgl_gles2_d_glBindRenderbuffer(GLenum target, GLuint renderbuffer) { if (nxgl_gles2_pfn_glBindRenderbuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBindRenderbuffer == NULL) return; nxgl_gles2_pfn_glBindRenderbuffer(target, renderbuffer); }
static void nxgl_gles2_d_glBindTexture(GLenum target, GLuint texture) { if (nxgl_gles2_pfn_glBindTexture == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBindTexture == NULL) return; nxgl_gles2_pfn_glBindTexture(target, texture); }
static void nxgl_gles2_d_glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { if (nxgl_gles2_pfn_glBlendColor == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBlendColor == NULL) return; nxgl_gles2_pfn_glBlendColor(red, green, blue, alpha); }
static void nxgl_gles2_d_glBlendEquation(GLenum mode) { if (nxgl_gles2_pfn_glBlendEquation == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBlendEquation == NULL) return; nxgl_gles2_pfn_glBlendEquation(mode); }
static void nxgl_gles2_d_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) { if (nxgl_gles2_pfn_glBlendEquationSeparate == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBlendEquationSeparate == NULL) return; nxgl_gles2_pfn_glBlendEquationSeparate(modeRGB, modeAlpha); }
static void nxgl_gles2_d_glBlendFunc(GLenum sfactor, GLenum dfactor) { if (nxgl_gles2_pfn_glBlendFunc == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBlendFunc == NULL) return; nxgl_gles2_pfn_glBlendFunc(sfactor, dfactor); }
static void nxgl_gles2_d_glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) { if (nxgl_gles2_pfn_glBlendFuncSeparate == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBlendFuncSeparate == NULL) return; nxgl_gles2_pfn_glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha); }
static void nxgl_gles2_d_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) { if (nxgl_gles2_pfn_glBufferData == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBufferData == NULL) return; nxgl_gles2_pfn_glBufferData(target, size, data, usage); }
static void nxgl_gles2_d_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) { if (nxgl_gles2_pfn_glBufferSubData == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glBufferSubData == NULL) return; nxgl_gles2_pfn_glBufferSubData(target, offset, size, data); }
static GLenum nxgl_gles2_d_glCheckFramebufferStatus(GLenum target) { if (nxgl_gles2_pfn_glCheckFramebufferStatus == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCheckFramebufferStatus == NULL) return (GLenum)0; return nxgl_gles2_pfn_glCheckFramebufferStatus(target); }
static void nxgl_gles2_d_glClear(GLbitfield mask) { if (nxgl_gles2_pfn_glClear == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glClear == NULL) return; nxgl_gles2_pfn_glClear(mask); }
static void nxgl_gles2_d_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { if (nxgl_gles2_pfn_glClearColor == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glClearColor == NULL) return; nxgl_gles2_pfn_glClearColor(red, green, blue, alpha); }
static void nxgl_gles2_d_glClearDepthf(GLclampf d) { if (nxgl_gles2_pfn_glClearDepthf == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glClearDepthf == NULL) return; nxgl_gles2_pfn_glClearDepthf(d); }
static void nxgl_gles2_d_glClearStencil(GLint s) { if (nxgl_gles2_pfn_glClearStencil == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glClearStencil == NULL) return; nxgl_gles2_pfn_glClearStencil(s); }
static void nxgl_gles2_d_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) { if (nxgl_gles2_pfn_glColorMask == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glColorMask == NULL) return; nxgl_gles2_pfn_glColorMask(red, green, blue, alpha); }
static void nxgl_gles2_d_glCompileShader(GLuint shader) { if (nxgl_gles2_pfn_glCompileShader == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCompileShader == NULL) return; nxgl_gles2_pfn_glCompileShader(shader); }
static void nxgl_gles2_d_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) { if (nxgl_gles2_pfn_glCompressedTexImage2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCompressedTexImage2D == NULL) return; nxgl_gles2_pfn_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data); }
static void nxgl_gles2_d_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) { if (nxgl_gles2_pfn_glCompressedTexSubImage2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCompressedTexSubImage2D == NULL) return; nxgl_gles2_pfn_glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data); }
static void nxgl_gles2_d_glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) { if (nxgl_gles2_pfn_glCopyTexImage2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCopyTexImage2D == NULL) return; nxgl_gles2_pfn_glCopyTexImage2D(target, level, internalformat, x, y, width, height, border); }
static void nxgl_gles2_d_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) { if (nxgl_gles2_pfn_glCopyTexSubImage2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCopyTexSubImage2D == NULL) return; nxgl_gles2_pfn_glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height); }
static GLuint nxgl_gles2_d_glCreateProgram(void) { if (nxgl_gles2_pfn_glCreateProgram == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCreateProgram == NULL) return (GLuint)0; return nxgl_gles2_pfn_glCreateProgram(); }
static GLuint nxgl_gles2_d_glCreateShader(GLenum type) { if (nxgl_gles2_pfn_glCreateShader == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCreateShader == NULL) return (GLuint)0; return nxgl_gles2_pfn_glCreateShader(type); }
static void nxgl_gles2_d_glCullFace(GLenum mode) { if (nxgl_gles2_pfn_glCullFace == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glCullFace == NULL) return; nxgl_gles2_pfn_glCullFace(mode); }
static void nxgl_gles2_d_glDeleteBuffers(GLsizei n, const GLuint *buffers) { if (nxgl_gles2_pfn_glDeleteBuffers == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDeleteBuffers == NULL) return; nxgl_gles2_pfn_glDeleteBuffers(n, buffers); }
static void nxgl_gles2_d_glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) { if (nxgl_gles2_pfn_glDeleteFramebuffers == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDeleteFramebuffers == NULL) return; nxgl_gles2_pfn_glDeleteFramebuffers(n, framebuffers); }
static void nxgl_gles2_d_glDeleteProgram(GLuint program) { if (nxgl_gles2_pfn_glDeleteProgram == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDeleteProgram == NULL) return; nxgl_gles2_pfn_glDeleteProgram(program); }
static void nxgl_gles2_d_glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) { if (nxgl_gles2_pfn_glDeleteRenderbuffers == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDeleteRenderbuffers == NULL) return; nxgl_gles2_pfn_glDeleteRenderbuffers(n, renderbuffers); }
static void nxgl_gles2_d_glDeleteShader(GLuint shader) { if (nxgl_gles2_pfn_glDeleteShader == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDeleteShader == NULL) return; nxgl_gles2_pfn_glDeleteShader(shader); }
static void nxgl_gles2_d_glDeleteTextures(GLsizei n, const GLuint *textures) { if (nxgl_gles2_pfn_glDeleteTextures == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDeleteTextures == NULL) return; nxgl_gles2_pfn_glDeleteTextures(n, textures); }
static void nxgl_gles2_d_glDepthFunc(GLenum func) { if (nxgl_gles2_pfn_glDepthFunc == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDepthFunc == NULL) return; nxgl_gles2_pfn_glDepthFunc(func); }
static void nxgl_gles2_d_glDepthMask(GLboolean flag) { if (nxgl_gles2_pfn_glDepthMask == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDepthMask == NULL) return; nxgl_gles2_pfn_glDepthMask(flag); }
static void nxgl_gles2_d_glDepthRangef(GLclampf n, GLclampf f) { if (nxgl_gles2_pfn_glDepthRangef == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDepthRangef == NULL) return; nxgl_gles2_pfn_glDepthRangef(n, f); }
static void nxgl_gles2_d_glDetachShader(GLuint program, GLuint shader) { if (nxgl_gles2_pfn_glDetachShader == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDetachShader == NULL) return; nxgl_gles2_pfn_glDetachShader(program, shader); }
static void nxgl_gles2_d_glDisable(GLenum cap) { if (nxgl_gles2_pfn_glDisable == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDisable == NULL) return; nxgl_gles2_pfn_glDisable(cap); }
static void nxgl_gles2_d_glDisableVertexAttribArray(GLuint index) { if (nxgl_gles2_pfn_glDisableVertexAttribArray == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDisableVertexAttribArray == NULL) return; nxgl_gles2_pfn_glDisableVertexAttribArray(index); }
static void nxgl_gles2_d_glDrawArrays(GLenum mode, GLint first, GLsizei count) { if (nxgl_gles2_pfn_glDrawArrays == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDrawArrays == NULL) return; nxgl_gles2_pfn_glDrawArrays(mode, first, count); }
static void nxgl_gles2_d_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) { if (nxgl_gles2_pfn_glDrawElements == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glDrawElements == NULL) return; nxgl_gles2_pfn_glDrawElements(mode, count, type, indices); }
static void nxgl_gles2_d_glEnable(GLenum cap) { if (nxgl_gles2_pfn_glEnable == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glEnable == NULL) return; nxgl_gles2_pfn_glEnable(cap); }
static void nxgl_gles2_d_glEnableVertexAttribArray(GLuint index) { if (nxgl_gles2_pfn_glEnableVertexAttribArray == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glEnableVertexAttribArray == NULL) return; nxgl_gles2_pfn_glEnableVertexAttribArray(index); }
static void nxgl_gles2_d_glFinish(void) { if (nxgl_gles2_pfn_glFinish == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glFinish == NULL) return; nxgl_gles2_pfn_glFinish(); }
static void nxgl_gles2_d_glFlush(void) { if (nxgl_gles2_pfn_glFlush == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glFlush == NULL) return; nxgl_gles2_pfn_glFlush(); }
static void nxgl_gles2_d_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) { if (nxgl_gles2_pfn_glFramebufferRenderbuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glFramebufferRenderbuffer == NULL) return; nxgl_gles2_pfn_glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer); }
static void nxgl_gles2_d_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) { if (nxgl_gles2_pfn_glFramebufferTexture2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glFramebufferTexture2D == NULL) return; nxgl_gles2_pfn_glFramebufferTexture2D(target, attachment, textarget, texture, level); }
static void nxgl_gles2_d_glFrontFace(GLenum mode) { if (nxgl_gles2_pfn_glFrontFace == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glFrontFace == NULL) return; nxgl_gles2_pfn_glFrontFace(mode); }
static void nxgl_gles2_d_glGenBuffers(GLsizei n, GLuint *buffers) { if (nxgl_gles2_pfn_glGenBuffers == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGenBuffers == NULL) return; nxgl_gles2_pfn_glGenBuffers(n, buffers); }
static void nxgl_gles2_d_glGenerateMipmap(GLenum target) { if (nxgl_gles2_pfn_glGenerateMipmap == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGenerateMipmap == NULL) return; nxgl_gles2_pfn_glGenerateMipmap(target); }
static void nxgl_gles2_d_glGenFramebuffers(GLsizei n, GLuint *framebuffers) { if (nxgl_gles2_pfn_glGenFramebuffers == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGenFramebuffers == NULL) return; nxgl_gles2_pfn_glGenFramebuffers(n, framebuffers); }
static void nxgl_gles2_d_glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) { if (nxgl_gles2_pfn_glGenRenderbuffers == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGenRenderbuffers == NULL) return; nxgl_gles2_pfn_glGenRenderbuffers(n, renderbuffers); }
static void nxgl_gles2_d_glGenTextures(GLsizei n, GLuint *textures) { if (nxgl_gles2_pfn_glGenTextures == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGenTextures == NULL) return; nxgl_gles2_pfn_glGenTextures(n, textures); }
static void nxgl_gles2_d_glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) { if (nxgl_gles2_pfn_glGetActiveAttrib == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetActiveAttrib == NULL) return; nxgl_gles2_pfn_glGetActiveAttrib(program, index, bufSize, length, size, type, name); }
static void nxgl_gles2_d_glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) { if (nxgl_gles2_pfn_glGetActiveUniform == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetActiveUniform == NULL) return; nxgl_gles2_pfn_glGetActiveUniform(program, index, bufSize, length, size, type, name); }
static void nxgl_gles2_d_glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders) { if (nxgl_gles2_pfn_glGetAttachedShaders == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetAttachedShaders == NULL) return; nxgl_gles2_pfn_glGetAttachedShaders(program, maxCount, count, shaders); }
static GLint nxgl_gles2_d_glGetAttribLocation(GLuint program, const GLchar *name) { if (nxgl_gles2_pfn_glGetAttribLocation == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetAttribLocation == NULL) return (GLint)0; return nxgl_gles2_pfn_glGetAttribLocation(program, name); }
static void nxgl_gles2_d_glGetBooleanv(GLenum pname, GLboolean *data) { if (nxgl_gles2_pfn_glGetBooleanv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetBooleanv == NULL) return; nxgl_gles2_pfn_glGetBooleanv(pname, data); }
static void nxgl_gles2_d_glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetBufferParameteriv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetBufferParameteriv == NULL) return; nxgl_gles2_pfn_glGetBufferParameteriv(target, pname, params); }
static GLenum nxgl_gles2_d_glGetError(void) { if (nxgl_gles2_pfn_glGetError == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetError == NULL) return (GLenum)0; return nxgl_gles2_pfn_glGetError(); }
static void nxgl_gles2_d_glGetFloatv(GLenum pname, GLfloat *data) { if (nxgl_gles2_pfn_glGetFloatv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetFloatv == NULL) return; nxgl_gles2_pfn_glGetFloatv(pname, data); }
static void nxgl_gles2_d_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv == NULL) return; nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv(target, attachment, pname, params); }
static void nxgl_gles2_d_glGetIntegerv(GLenum pname, GLint *data) { if (nxgl_gles2_pfn_glGetIntegerv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetIntegerv == NULL) return; nxgl_gles2_pfn_glGetIntegerv(pname, data); }
static void nxgl_gles2_d_glGetProgramiv(GLuint program, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetProgramiv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetProgramiv == NULL) return; nxgl_gles2_pfn_glGetProgramiv(program, pname, params); }
static void nxgl_gles2_d_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) { if (nxgl_gles2_pfn_glGetProgramInfoLog == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetProgramInfoLog == NULL) return; nxgl_gles2_pfn_glGetProgramInfoLog(program, bufSize, length, infoLog); }
static void nxgl_gles2_d_glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetRenderbufferParameteriv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetRenderbufferParameteriv == NULL) return; nxgl_gles2_pfn_glGetRenderbufferParameteriv(target, pname, params); }
static void nxgl_gles2_d_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetShaderiv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetShaderiv == NULL) return; nxgl_gles2_pfn_glGetShaderiv(shader, pname, params); }
static void nxgl_gles2_d_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) { if (nxgl_gles2_pfn_glGetShaderInfoLog == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetShaderInfoLog == NULL) return; nxgl_gles2_pfn_glGetShaderInfoLog(shader, bufSize, length, infoLog); }
static void nxgl_gles2_d_glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision) { if (nxgl_gles2_pfn_glGetShaderPrecisionFormat == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetShaderPrecisionFormat == NULL) return; nxgl_gles2_pfn_glGetShaderPrecisionFormat(shadertype, precisiontype, range, precision); }
static void nxgl_gles2_d_glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source) { if (nxgl_gles2_pfn_glGetShaderSource == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetShaderSource == NULL) return; nxgl_gles2_pfn_glGetShaderSource(shader, bufSize, length, source); }
static void nxgl_gles2_d_glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) { if (nxgl_gles2_pfn_glGetTexParameterfv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetTexParameterfv == NULL) return; nxgl_gles2_pfn_glGetTexParameterfv(target, pname, params); }
static void nxgl_gles2_d_glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetTexParameteriv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetTexParameteriv == NULL) return; nxgl_gles2_pfn_glGetTexParameteriv(target, pname, params); }
static void nxgl_gles2_d_glGetUniformfv(GLuint program, GLint location, GLfloat *params) { if (nxgl_gles2_pfn_glGetUniformfv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetUniformfv == NULL) return; nxgl_gles2_pfn_glGetUniformfv(program, location, params); }
static void nxgl_gles2_d_glGetUniformiv(GLuint program, GLint location, GLint *params) { if (nxgl_gles2_pfn_glGetUniformiv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetUniformiv == NULL) return; nxgl_gles2_pfn_glGetUniformiv(program, location, params); }
static GLint nxgl_gles2_d_glGetUniformLocation(GLuint program, const GLchar *name) { if (nxgl_gles2_pfn_glGetUniformLocation == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetUniformLocation == NULL) return (GLint)0; return nxgl_gles2_pfn_glGetUniformLocation(program, name); }
static void nxgl_gles2_d_glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params) { if (nxgl_gles2_pfn_glGetVertexAttribfv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetVertexAttribfv == NULL) return; nxgl_gles2_pfn_glGetVertexAttribfv(index, pname, params); }
static void nxgl_gles2_d_glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) { if (nxgl_gles2_pfn_glGetVertexAttribiv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetVertexAttribiv == NULL) return; nxgl_gles2_pfn_glGetVertexAttribiv(index, pname, params); }
static void nxgl_gles2_d_glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer) { if (nxgl_gles2_pfn_glGetVertexAttribPointerv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetVertexAttribPointerv == NULL) return; nxgl_gles2_pfn_glGetVertexAttribPointerv(index, pname, pointer); }
static void nxgl_gles2_d_glHint(GLenum target, GLenum mode) { if (nxgl_gles2_pfn_glHint == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glHint == NULL) return; nxgl_gles2_pfn_glHint(target, mode); }
static GLboolean nxgl_gles2_d_glIsBuffer(GLuint buffer) { if (nxgl_gles2_pfn_glIsBuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsBuffer == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsBuffer(buffer); }
static GLboolean nxgl_gles2_d_glIsEnabled(GLenum cap) { if (nxgl_gles2_pfn_glIsEnabled == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsEnabled == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsEnabled(cap); }
static GLboolean nxgl_gles2_d_glIsFramebuffer(GLuint framebuffer) { if (nxgl_gles2_pfn_glIsFramebuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsFramebuffer == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsFramebuffer(framebuffer); }
static GLboolean nxgl_gles2_d_glIsProgram(GLuint program) { if (nxgl_gles2_pfn_glIsProgram == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsProgram == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsProgram(program); }
static GLboolean nxgl_gles2_d_glIsRenderbuffer(GLuint renderbuffer) { if (nxgl_gles2_pfn_glIsRenderbuffer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsRenderbuffer == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsRenderbuffer(renderbuffer); }
static GLboolean nxgl_gles2_d_glIsShader(GLuint shader) { if (nxgl_gles2_pfn_glIsShader == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsShader == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsShader(shader); }
static GLboolean nxgl_gles2_d_glIsTexture(GLuint texture) { if (nxgl_gles2_pfn_glIsTexture == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glIsTexture == NULL) return (GLboolean)0; return nxgl_gles2_pfn_glIsTexture(texture); }
static void nxgl_gles2_d_glLineWidth(GLfloat width) { if (nxgl_gles2_pfn_glLineWidth == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glLineWidth == NULL) return; nxgl_gles2_pfn_glLineWidth(width); }
static void nxgl_gles2_d_glLinkProgram(GLuint program) { if (nxgl_gles2_pfn_glLinkProgram == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glLinkProgram == NULL) return; nxgl_gles2_pfn_glLinkProgram(program); }
static void nxgl_gles2_d_glPixelStorei(GLenum pname, GLint param) { if (nxgl_gles2_pfn_glPixelStorei == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glPixelStorei == NULL) return; nxgl_gles2_pfn_glPixelStorei(pname, param); }
static void nxgl_gles2_d_glPolygonOffset(GLfloat factor, GLfloat units) { if (nxgl_gles2_pfn_glPolygonOffset == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glPolygonOffset == NULL) return; nxgl_gles2_pfn_glPolygonOffset(factor, units); }
static void nxgl_gles2_d_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) { if (nxgl_gles2_pfn_glReadPixels == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glReadPixels == NULL) return; nxgl_gles2_pfn_glReadPixels(x, y, width, height, format, type, pixels); }
static void nxgl_gles2_d_glReleaseShaderCompiler(void) { if (nxgl_gles2_pfn_glReleaseShaderCompiler == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glReleaseShaderCompiler == NULL) return; nxgl_gles2_pfn_glReleaseShaderCompiler(); }
static void nxgl_gles2_d_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) { if (nxgl_gles2_pfn_glRenderbufferStorage == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glRenderbufferStorage == NULL) return; nxgl_gles2_pfn_glRenderbufferStorage(target, internalformat, width, height); }
static void nxgl_gles2_d_glSampleCoverage(GLclampf value, GLboolean invert) { if (nxgl_gles2_pfn_glSampleCoverage == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glSampleCoverage == NULL) return; nxgl_gles2_pfn_glSampleCoverage(value, invert); }
static void nxgl_gles2_d_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { if (nxgl_gles2_pfn_glScissor == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glScissor == NULL) return; nxgl_gles2_pfn_glScissor(x, y, width, height); }
static void nxgl_gles2_d_glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length) { if (nxgl_gles2_pfn_glShaderBinary == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glShaderBinary == NULL) return; nxgl_gles2_pfn_glShaderBinary(count, shaders, binaryFormat, binary, length); }
static void nxgl_gles2_d_glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) { if (nxgl_gles2_pfn_glShaderSource == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glShaderSource == NULL) return; nxgl_gles2_pfn_glShaderSource(shader, count, string, length); }
static void nxgl_gles2_d_glStencilFunc(GLenum func, GLint ref, GLuint mask) { if (nxgl_gles2_pfn_glStencilFunc == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glStencilFunc == NULL) return; nxgl_gles2_pfn_glStencilFunc(func, ref, mask); }
static void nxgl_gles2_d_glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) { if (nxgl_gles2_pfn_glStencilFuncSeparate == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glStencilFuncSeparate == NULL) return; nxgl_gles2_pfn_glStencilFuncSeparate(face, func, ref, mask); }
static void nxgl_gles2_d_glStencilMask(GLuint mask) { if (nxgl_gles2_pfn_glStencilMask == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glStencilMask == NULL) return; nxgl_gles2_pfn_glStencilMask(mask); }
static void nxgl_gles2_d_glStencilMaskSeparate(GLenum face, GLuint mask) { if (nxgl_gles2_pfn_glStencilMaskSeparate == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glStencilMaskSeparate == NULL) return; nxgl_gles2_pfn_glStencilMaskSeparate(face, mask); }
static void nxgl_gles2_d_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) { if (nxgl_gles2_pfn_glStencilOp == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glStencilOp == NULL) return; nxgl_gles2_pfn_glStencilOp(fail, zfail, zpass); }
static void nxgl_gles2_d_glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) { if (nxgl_gles2_pfn_glStencilOpSeparate == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glStencilOpSeparate == NULL) return; nxgl_gles2_pfn_glStencilOpSeparate(face, sfail, dpfail, dppass); }
static void nxgl_gles2_d_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) { if (nxgl_gles2_pfn_glTexImage2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glTexImage2D == NULL) return; nxgl_gles2_pfn_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels); }
static void nxgl_gles2_d_glTexParameterf(GLenum target, GLenum pname, GLfloat param) { if (nxgl_gles2_pfn_glTexParameterf == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glTexParameterf == NULL) return; nxgl_gles2_pfn_glTexParameterf(target, pname, param); }
static void nxgl_gles2_d_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) { if (nxgl_gles2_pfn_glTexParameterfv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glTexParameterfv == NULL) return; nxgl_gles2_pfn_glTexParameterfv(target, pname, params); }
static void nxgl_gles2_d_glTexParameteri(GLenum target, GLenum pname, GLint param) { if (nxgl_gles2_pfn_glTexParameteri == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glTexParameteri == NULL) return; nxgl_gles2_pfn_glTexParameteri(target, pname, param); }
static void nxgl_gles2_d_glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { if (nxgl_gles2_pfn_glTexParameteriv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glTexParameteriv == NULL) return; nxgl_gles2_pfn_glTexParameteriv(target, pname, params); }
static void nxgl_gles2_d_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) { if (nxgl_gles2_pfn_glTexSubImage2D == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glTexSubImage2D == NULL) return; nxgl_gles2_pfn_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels); }
static void nxgl_gles2_d_glUniform1f(GLint location, GLfloat v0) { if (nxgl_gles2_pfn_glUniform1f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform1f == NULL) return; nxgl_gles2_pfn_glUniform1f(location, v0); }
static void nxgl_gles2_d_glUniform1fv(GLint location, GLsizei count, const GLfloat *value) { if (nxgl_gles2_pfn_glUniform1fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform1fv == NULL) return; nxgl_gles2_pfn_glUniform1fv(location, count, value); }
static void nxgl_gles2_d_glUniform1i(GLint location, GLint v0) { if (nxgl_gles2_pfn_glUniform1i == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform1i == NULL) return; nxgl_gles2_pfn_glUniform1i(location, v0); }
static void nxgl_gles2_d_glUniform1iv(GLint location, GLsizei count, const GLint *value) { if (nxgl_gles2_pfn_glUniform1iv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform1iv == NULL) return; nxgl_gles2_pfn_glUniform1iv(location, count, value); }
static void nxgl_gles2_d_glUniform2f(GLint location, GLfloat v0, GLfloat v1) { if (nxgl_gles2_pfn_glUniform2f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform2f == NULL) return; nxgl_gles2_pfn_glUniform2f(location, v0, v1); }
static void nxgl_gles2_d_glUniform2fv(GLint location, GLsizei count, const GLfloat *value) { if (nxgl_gles2_pfn_glUniform2fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform2fv == NULL) return; nxgl_gles2_pfn_glUniform2fv(location, count, value); }
static void nxgl_gles2_d_glUniform2i(GLint location, GLint v0, GLint v1) { if (nxgl_gles2_pfn_glUniform2i == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform2i == NULL) return; nxgl_gles2_pfn_glUniform2i(location, v0, v1); }
static void nxgl_gles2_d_glUniform2iv(GLint location, GLsizei count, const GLint *value) { if (nxgl_gles2_pfn_glUniform2iv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform2iv == NULL) return; nxgl_gles2_pfn_glUniform2iv(location, count, value); }
static void nxgl_gles2_d_glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) { if (nxgl_gles2_pfn_glUniform3f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform3f == NULL) return; nxgl_gles2_pfn_glUniform3f(location, v0, v1, v2); }
static void nxgl_gles2_d_glUniform3fv(GLint location, GLsizei count, const GLfloat *value) { if (nxgl_gles2_pfn_glUniform3fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform3fv == NULL) return; nxgl_gles2_pfn_glUniform3fv(location, count, value); }
static void nxgl_gles2_d_glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) { if (nxgl_gles2_pfn_glUniform3i == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform3i == NULL) return; nxgl_gles2_pfn_glUniform3i(location, v0, v1, v2); }
static void nxgl_gles2_d_glUniform3iv(GLint location, GLsizei count, const GLint *value) { if (nxgl_gles2_pfn_glUniform3iv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform3iv == NULL) return; nxgl_gles2_pfn_glUniform3iv(location, count, value); }
static void nxgl_gles2_d_glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) { if (nxgl_gles2_pfn_glUniform4f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform4f == NULL) return; nxgl_gles2_pfn_glUniform4f(location, v0, v1, v2, v3); }
static void nxgl_gles2_d_glUniform4fv(GLint location, GLsizei count, const GLfloat *value) { if (nxgl_gles2_pfn_glUniform4fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform4fv == NULL) return; nxgl_gles2_pfn_glUniform4fv(location, count, value); }
static void nxgl_gles2_d_glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) { if (nxgl_gles2_pfn_glUniform4i == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform4i == NULL) return; nxgl_gles2_pfn_glUniform4i(location, v0, v1, v2, v3); }
static void nxgl_gles2_d_glUniform4iv(GLint location, GLsizei count, const GLint *value) { if (nxgl_gles2_pfn_glUniform4iv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniform4iv == NULL) return; nxgl_gles2_pfn_glUniform4iv(location, count, value); }
static void nxgl_gles2_d_glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) { if (nxgl_gles2_pfn_glUniformMatrix2fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniformMatrix2fv == NULL) return; nxgl_gles2_pfn_glUniformMatrix2fv(location, count, transpose, value); }
static void nxgl_gles2_d_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) { if (nxgl_gles2_pfn_glUniformMatrix3fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniformMatrix3fv == NULL) return; nxgl_gles2_pfn_glUniformMatrix3fv(location, count, transpose, value); }
static void nxgl_gles2_d_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) { if (nxgl_gles2_pfn_glUniformMatrix4fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUniformMatrix4fv == NULL) return; nxgl_gles2_pfn_glUniformMatrix4fv(location, count, transpose, value); }
static void nxgl_gles2_d_glUseProgram(GLuint program) { if (nxgl_gles2_pfn_glUseProgram == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glUseProgram == NULL) return; nxgl_gles2_pfn_glUseProgram(program); }
static void nxgl_gles2_d_glValidateProgram(GLuint program) { if (nxgl_gles2_pfn_glValidateProgram == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glValidateProgram == NULL) return; nxgl_gles2_pfn_glValidateProgram(program); }
static void nxgl_gles2_d_glVertexAttrib1f(GLuint index, GLfloat x) { if (nxgl_gles2_pfn_glVertexAttrib1f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib1f == NULL) return; nxgl_gles2_pfn_glVertexAttrib1f(index, x); }
static void nxgl_gles2_d_glVertexAttrib1fv(GLuint index, const GLfloat *v) { if (nxgl_gles2_pfn_glVertexAttrib1fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib1fv == NULL) return; nxgl_gles2_pfn_glVertexAttrib1fv(index, v); }
static void nxgl_gles2_d_glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y) { if (nxgl_gles2_pfn_glVertexAttrib2f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib2f == NULL) return; nxgl_gles2_pfn_glVertexAttrib2f(index, x, y); }
static void nxgl_gles2_d_glVertexAttrib2fv(GLuint index, const GLfloat *v) { if (nxgl_gles2_pfn_glVertexAttrib2fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib2fv == NULL) return; nxgl_gles2_pfn_glVertexAttrib2fv(index, v); }
static void nxgl_gles2_d_glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) { if (nxgl_gles2_pfn_glVertexAttrib3f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib3f == NULL) return; nxgl_gles2_pfn_glVertexAttrib3f(index, x, y, z); }
static void nxgl_gles2_d_glVertexAttrib3fv(GLuint index, const GLfloat *v) { if (nxgl_gles2_pfn_glVertexAttrib3fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib3fv == NULL) return; nxgl_gles2_pfn_glVertexAttrib3fv(index, v); }
static void nxgl_gles2_d_glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { if (nxgl_gles2_pfn_glVertexAttrib4f == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib4f == NULL) return; nxgl_gles2_pfn_glVertexAttrib4f(index, x, y, z, w); }
static void nxgl_gles2_d_glVertexAttrib4fv(GLuint index, const GLfloat *v) { if (nxgl_gles2_pfn_glVertexAttrib4fv == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttrib4fv == NULL) return; nxgl_gles2_pfn_glVertexAttrib4fv(index, v); }
static void nxgl_gles2_d_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) { if (nxgl_gles2_pfn_glVertexAttribPointer == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glVertexAttribPointer == NULL) return; nxgl_gles2_pfn_glVertexAttribPointer(index, size, type, normalized, stride, pointer); }
static void nxgl_gles2_d_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { if (nxgl_gles2_pfn_glViewport == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glViewport == NULL) return; nxgl_gles2_pfn_glViewport(x, y, width, height); }
static const GLubyte * nxgl_gles2_d_glGetString(GLenum name) { if (nxgl_gles2_pfn_glGetString == NULL) nxgl_gles2_lazy_init(); if (nxgl_gles2_pfn_glGetString == NULL) return (const GLubyte *)0; return nxgl_gles2_pfn_glGetString(name); }

static const struct nxgl_gles2_entry k_entries[] = {
  { "glActiveTexture", (void **)&nxgl_gles2_pfn_glActiveTexture, (void *)nxgl_gles2_d_glActiveTexture },
  { "glAttachShader", (void **)&nxgl_gles2_pfn_glAttachShader, (void *)nxgl_gles2_d_glAttachShader },
  { "glBindAttribLocation", (void **)&nxgl_gles2_pfn_glBindAttribLocation, (void *)nxgl_gles2_d_glBindAttribLocation },
  { "glBindBuffer", (void **)&nxgl_gles2_pfn_glBindBuffer, (void *)nxgl_gles2_d_glBindBuffer },
  { "glBindFramebuffer", (void **)&nxgl_gles2_pfn_glBindFramebuffer, (void *)nxgl_gles2_d_glBindFramebuffer },
  { "glBindRenderbuffer", (void **)&nxgl_gles2_pfn_glBindRenderbuffer, (void *)nxgl_gles2_d_glBindRenderbuffer },
  { "glBindTexture", (void **)&nxgl_gles2_pfn_glBindTexture, (void *)nxgl_gles2_d_glBindTexture },
  { "glBlendColor", (void **)&nxgl_gles2_pfn_glBlendColor, (void *)nxgl_gles2_d_glBlendColor },
  { "glBlendEquation", (void **)&nxgl_gles2_pfn_glBlendEquation, (void *)nxgl_gles2_d_glBlendEquation },
  { "glBlendEquationSeparate", (void **)&nxgl_gles2_pfn_glBlendEquationSeparate, (void *)nxgl_gles2_d_glBlendEquationSeparate },
  { "glBlendFunc", (void **)&nxgl_gles2_pfn_glBlendFunc, (void *)nxgl_gles2_d_glBlendFunc },
  { "glBlendFuncSeparate", (void **)&nxgl_gles2_pfn_glBlendFuncSeparate, (void *)nxgl_gles2_d_glBlendFuncSeparate },
  { "glBufferData", (void **)&nxgl_gles2_pfn_glBufferData, (void *)nxgl_gles2_d_glBufferData },
  { "glBufferSubData", (void **)&nxgl_gles2_pfn_glBufferSubData, (void *)nxgl_gles2_d_glBufferSubData },
  { "glCheckFramebufferStatus", (void **)&nxgl_gles2_pfn_glCheckFramebufferStatus, (void *)nxgl_gles2_d_glCheckFramebufferStatus },
  { "glClear", (void **)&nxgl_gles2_pfn_glClear, (void *)nxgl_gles2_d_glClear },
  { "glClearColor", (void **)&nxgl_gles2_pfn_glClearColor, (void *)nxgl_gles2_d_glClearColor },
  { "glClearDepthf", (void **)&nxgl_gles2_pfn_glClearDepthf, (void *)nxgl_gles2_d_glClearDepthf },
  { "glClearStencil", (void **)&nxgl_gles2_pfn_glClearStencil, (void *)nxgl_gles2_d_glClearStencil },
  { "glColorMask", (void **)&nxgl_gles2_pfn_glColorMask, (void *)nxgl_gles2_d_glColorMask },
  { "glCompileShader", (void **)&nxgl_gles2_pfn_glCompileShader, (void *)nxgl_gles2_d_glCompileShader },
  { "glCompressedTexImage2D", (void **)&nxgl_gles2_pfn_glCompressedTexImage2D, (void *)nxgl_gles2_d_glCompressedTexImage2D },
  { "glCompressedTexSubImage2D", (void **)&nxgl_gles2_pfn_glCompressedTexSubImage2D, (void *)nxgl_gles2_d_glCompressedTexSubImage2D },
  { "glCopyTexImage2D", (void **)&nxgl_gles2_pfn_glCopyTexImage2D, (void *)nxgl_gles2_d_glCopyTexImage2D },
  { "glCopyTexSubImage2D", (void **)&nxgl_gles2_pfn_glCopyTexSubImage2D, (void *)nxgl_gles2_d_glCopyTexSubImage2D },
  { "glCreateProgram", (void **)&nxgl_gles2_pfn_glCreateProgram, (void *)nxgl_gles2_d_glCreateProgram },
  { "glCreateShader", (void **)&nxgl_gles2_pfn_glCreateShader, (void *)nxgl_gles2_d_glCreateShader },
  { "glCullFace", (void **)&nxgl_gles2_pfn_glCullFace, (void *)nxgl_gles2_d_glCullFace },
  { "glDeleteBuffers", (void **)&nxgl_gles2_pfn_glDeleteBuffers, (void *)nxgl_gles2_d_glDeleteBuffers },
  { "glDeleteFramebuffers", (void **)&nxgl_gles2_pfn_glDeleteFramebuffers, (void *)nxgl_gles2_d_glDeleteFramebuffers },
  { "glDeleteProgram", (void **)&nxgl_gles2_pfn_glDeleteProgram, (void *)nxgl_gles2_d_glDeleteProgram },
  { "glDeleteRenderbuffers", (void **)&nxgl_gles2_pfn_glDeleteRenderbuffers, (void *)nxgl_gles2_d_glDeleteRenderbuffers },
  { "glDeleteShader", (void **)&nxgl_gles2_pfn_glDeleteShader, (void *)nxgl_gles2_d_glDeleteShader },
  { "glDeleteTextures", (void **)&nxgl_gles2_pfn_glDeleteTextures, (void *)nxgl_gles2_d_glDeleteTextures },
  { "glDepthFunc", (void **)&nxgl_gles2_pfn_glDepthFunc, (void *)nxgl_gles2_d_glDepthFunc },
  { "glDepthMask", (void **)&nxgl_gles2_pfn_glDepthMask, (void *)nxgl_gles2_d_glDepthMask },
  { "glDepthRangef", (void **)&nxgl_gles2_pfn_glDepthRangef, (void *)nxgl_gles2_d_glDepthRangef },
  { "glDetachShader", (void **)&nxgl_gles2_pfn_glDetachShader, (void *)nxgl_gles2_d_glDetachShader },
  { "glDisable", (void **)&nxgl_gles2_pfn_glDisable, (void *)nxgl_gles2_d_glDisable },
  { "glDisableVertexAttribArray", (void **)&nxgl_gles2_pfn_glDisableVertexAttribArray, (void *)nxgl_gles2_d_glDisableVertexAttribArray },
  { "glDrawArrays", (void **)&nxgl_gles2_pfn_glDrawArrays, (void *)nxgl_gles2_d_glDrawArrays },
  { "glDrawElements", (void **)&nxgl_gles2_pfn_glDrawElements, (void *)nxgl_gles2_d_glDrawElements },
  { "glEnable", (void **)&nxgl_gles2_pfn_glEnable, (void *)nxgl_gles2_d_glEnable },
  { "glEnableVertexAttribArray", (void **)&nxgl_gles2_pfn_glEnableVertexAttribArray, (void *)nxgl_gles2_d_glEnableVertexAttribArray },
  { "glFinish", (void **)&nxgl_gles2_pfn_glFinish, (void *)nxgl_gles2_d_glFinish },
  { "glFlush", (void **)&nxgl_gles2_pfn_glFlush, (void *)nxgl_gles2_d_glFlush },
  { "glFramebufferRenderbuffer", (void **)&nxgl_gles2_pfn_glFramebufferRenderbuffer, (void *)nxgl_gles2_d_glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (void **)&nxgl_gles2_pfn_glFramebufferTexture2D, (void *)nxgl_gles2_d_glFramebufferTexture2D },
  { "glFrontFace", (void **)&nxgl_gles2_pfn_glFrontFace, (void *)nxgl_gles2_d_glFrontFace },
  { "glGenBuffers", (void **)&nxgl_gles2_pfn_glGenBuffers, (void *)nxgl_gles2_d_glGenBuffers },
  { "glGenerateMipmap", (void **)&nxgl_gles2_pfn_glGenerateMipmap, (void *)nxgl_gles2_d_glGenerateMipmap },
  { "glGenFramebuffers", (void **)&nxgl_gles2_pfn_glGenFramebuffers, (void *)nxgl_gles2_d_glGenFramebuffers },
  { "glGenRenderbuffers", (void **)&nxgl_gles2_pfn_glGenRenderbuffers, (void *)nxgl_gles2_d_glGenRenderbuffers },
  { "glGenTextures", (void **)&nxgl_gles2_pfn_glGenTextures, (void *)nxgl_gles2_d_glGenTextures },
  { "glGetActiveAttrib", (void **)&nxgl_gles2_pfn_glGetActiveAttrib, (void *)nxgl_gles2_d_glGetActiveAttrib },
  { "glGetActiveUniform", (void **)&nxgl_gles2_pfn_glGetActiveUniform, (void *)nxgl_gles2_d_glGetActiveUniform },
  { "glGetAttachedShaders", (void **)&nxgl_gles2_pfn_glGetAttachedShaders, (void *)nxgl_gles2_d_glGetAttachedShaders },
  { "glGetAttribLocation", (void **)&nxgl_gles2_pfn_glGetAttribLocation, (void *)nxgl_gles2_d_glGetAttribLocation },
  { "glGetBooleanv", (void **)&nxgl_gles2_pfn_glGetBooleanv, (void *)nxgl_gles2_d_glGetBooleanv },
  { "glGetBufferParameteriv", (void **)&nxgl_gles2_pfn_glGetBufferParameteriv, (void *)nxgl_gles2_d_glGetBufferParameteriv },
  { "glGetError", (void **)&nxgl_gles2_pfn_glGetError, (void *)nxgl_gles2_d_glGetError },
  { "glGetFloatv", (void **)&nxgl_gles2_pfn_glGetFloatv, (void *)nxgl_gles2_d_glGetFloatv },
  { "glGetFramebufferAttachmentParameteriv", (void **)&nxgl_gles2_pfn_glGetFramebufferAttachmentParameteriv, (void *)nxgl_gles2_d_glGetFramebufferAttachmentParameteriv },
  { "glGetIntegerv", (void **)&nxgl_gles2_pfn_glGetIntegerv, (void *)nxgl_gles2_d_glGetIntegerv },
  { "glGetProgramiv", (void **)&nxgl_gles2_pfn_glGetProgramiv, (void *)nxgl_gles2_d_glGetProgramiv },
  { "glGetProgramInfoLog", (void **)&nxgl_gles2_pfn_glGetProgramInfoLog, (void *)nxgl_gles2_d_glGetProgramInfoLog },
  { "glGetRenderbufferParameteriv", (void **)&nxgl_gles2_pfn_glGetRenderbufferParameteriv, (void *)nxgl_gles2_d_glGetRenderbufferParameteriv },
  { "glGetShaderiv", (void **)&nxgl_gles2_pfn_glGetShaderiv, (void *)nxgl_gles2_d_glGetShaderiv },
  { "glGetShaderInfoLog", (void **)&nxgl_gles2_pfn_glGetShaderInfoLog, (void *)nxgl_gles2_d_glGetShaderInfoLog },
  { "glGetShaderPrecisionFormat", (void **)&nxgl_gles2_pfn_glGetShaderPrecisionFormat, (void *)nxgl_gles2_d_glGetShaderPrecisionFormat },
  { "glGetShaderSource", (void **)&nxgl_gles2_pfn_glGetShaderSource, (void *)nxgl_gles2_d_glGetShaderSource },
  { "glGetTexParameterfv", (void **)&nxgl_gles2_pfn_glGetTexParameterfv, (void *)nxgl_gles2_d_glGetTexParameterfv },
  { "glGetTexParameteriv", (void **)&nxgl_gles2_pfn_glGetTexParameteriv, (void *)nxgl_gles2_d_glGetTexParameteriv },
  { "glGetUniformfv", (void **)&nxgl_gles2_pfn_glGetUniformfv, (void *)nxgl_gles2_d_glGetUniformfv },
  { "glGetUniformiv", (void **)&nxgl_gles2_pfn_glGetUniformiv, (void *)nxgl_gles2_d_glGetUniformiv },
  { "glGetUniformLocation", (void **)&nxgl_gles2_pfn_glGetUniformLocation, (void *)nxgl_gles2_d_glGetUniformLocation },
  { "glGetVertexAttribfv", (void **)&nxgl_gles2_pfn_glGetVertexAttribfv, (void *)nxgl_gles2_d_glGetVertexAttribfv },
  { "glGetVertexAttribiv", (void **)&nxgl_gles2_pfn_glGetVertexAttribiv, (void *)nxgl_gles2_d_glGetVertexAttribiv },
  { "glGetVertexAttribPointerv", (void **)&nxgl_gles2_pfn_glGetVertexAttribPointerv, (void *)nxgl_gles2_d_glGetVertexAttribPointerv },
  { "glHint", (void **)&nxgl_gles2_pfn_glHint, (void *)nxgl_gles2_d_glHint },
  { "glIsBuffer", (void **)&nxgl_gles2_pfn_glIsBuffer, (void *)nxgl_gles2_d_glIsBuffer },
  { "glIsEnabled", (void **)&nxgl_gles2_pfn_glIsEnabled, (void *)nxgl_gles2_d_glIsEnabled },
  { "glIsFramebuffer", (void **)&nxgl_gles2_pfn_glIsFramebuffer, (void *)nxgl_gles2_d_glIsFramebuffer },
  { "glIsProgram", (void **)&nxgl_gles2_pfn_glIsProgram, (void *)nxgl_gles2_d_glIsProgram },
  { "glIsRenderbuffer", (void **)&nxgl_gles2_pfn_glIsRenderbuffer, (void *)nxgl_gles2_d_glIsRenderbuffer },
  { "glIsShader", (void **)&nxgl_gles2_pfn_glIsShader, (void *)nxgl_gles2_d_glIsShader },
  { "glIsTexture", (void **)&nxgl_gles2_pfn_glIsTexture, (void *)nxgl_gles2_d_glIsTexture },
  { "glLineWidth", (void **)&nxgl_gles2_pfn_glLineWidth, (void *)nxgl_gles2_d_glLineWidth },
  { "glLinkProgram", (void **)&nxgl_gles2_pfn_glLinkProgram, (void *)nxgl_gles2_d_glLinkProgram },
  { "glPixelStorei", (void **)&nxgl_gles2_pfn_glPixelStorei, (void *)nxgl_gles2_d_glPixelStorei },
  { "glPolygonOffset", (void **)&nxgl_gles2_pfn_glPolygonOffset, (void *)nxgl_gles2_d_glPolygonOffset },
  { "glReadPixels", (void **)&nxgl_gles2_pfn_glReadPixels, (void *)nxgl_gles2_d_glReadPixels },
  { "glReleaseShaderCompiler", (void **)&nxgl_gles2_pfn_glReleaseShaderCompiler, (void *)nxgl_gles2_d_glReleaseShaderCompiler },
  { "glRenderbufferStorage", (void **)&nxgl_gles2_pfn_glRenderbufferStorage, (void *)nxgl_gles2_d_glRenderbufferStorage },
  { "glSampleCoverage", (void **)&nxgl_gles2_pfn_glSampleCoverage, (void *)nxgl_gles2_d_glSampleCoverage },
  { "glScissor", (void **)&nxgl_gles2_pfn_glScissor, (void *)nxgl_gles2_d_glScissor },
  { "glShaderBinary", (void **)&nxgl_gles2_pfn_glShaderBinary, (void *)nxgl_gles2_d_glShaderBinary },
  { "glShaderSource", (void **)&nxgl_gles2_pfn_glShaderSource, (void *)nxgl_gles2_d_glShaderSource },
  { "glStencilFunc", (void **)&nxgl_gles2_pfn_glStencilFunc, (void *)nxgl_gles2_d_glStencilFunc },
  { "glStencilFuncSeparate", (void **)&nxgl_gles2_pfn_glStencilFuncSeparate, (void *)nxgl_gles2_d_glStencilFuncSeparate },
  { "glStencilMask", (void **)&nxgl_gles2_pfn_glStencilMask, (void *)nxgl_gles2_d_glStencilMask },
  { "glStencilMaskSeparate", (void **)&nxgl_gles2_pfn_glStencilMaskSeparate, (void *)nxgl_gles2_d_glStencilMaskSeparate },
  { "glStencilOp", (void **)&nxgl_gles2_pfn_glStencilOp, (void *)nxgl_gles2_d_glStencilOp },
  { "glStencilOpSeparate", (void **)&nxgl_gles2_pfn_glStencilOpSeparate, (void *)nxgl_gles2_d_glStencilOpSeparate },
  { "glTexImage2D", (void **)&nxgl_gles2_pfn_glTexImage2D, (void *)nxgl_gles2_d_glTexImage2D },
  { "glTexParameterf", (void **)&nxgl_gles2_pfn_glTexParameterf, (void *)nxgl_gles2_d_glTexParameterf },
  { "glTexParameterfv", (void **)&nxgl_gles2_pfn_glTexParameterfv, (void *)nxgl_gles2_d_glTexParameterfv },
  { "glTexParameteri", (void **)&nxgl_gles2_pfn_glTexParameteri, (void *)nxgl_gles2_d_glTexParameteri },
  { "glTexParameteriv", (void **)&nxgl_gles2_pfn_glTexParameteriv, (void *)nxgl_gles2_d_glTexParameteriv },
  { "glTexSubImage2D", (void **)&nxgl_gles2_pfn_glTexSubImage2D, (void *)nxgl_gles2_d_glTexSubImage2D },
  { "glUniform1f", (void **)&nxgl_gles2_pfn_glUniform1f, (void *)nxgl_gles2_d_glUniform1f },
  { "glUniform1fv", (void **)&nxgl_gles2_pfn_glUniform1fv, (void *)nxgl_gles2_d_glUniform1fv },
  { "glUniform1i", (void **)&nxgl_gles2_pfn_glUniform1i, (void *)nxgl_gles2_d_glUniform1i },
  { "glUniform1iv", (void **)&nxgl_gles2_pfn_glUniform1iv, (void *)nxgl_gles2_d_glUniform1iv },
  { "glUniform2f", (void **)&nxgl_gles2_pfn_glUniform2f, (void *)nxgl_gles2_d_glUniform2f },
  { "glUniform2fv", (void **)&nxgl_gles2_pfn_glUniform2fv, (void *)nxgl_gles2_d_glUniform2fv },
  { "glUniform2i", (void **)&nxgl_gles2_pfn_glUniform2i, (void *)nxgl_gles2_d_glUniform2i },
  { "glUniform2iv", (void **)&nxgl_gles2_pfn_glUniform2iv, (void *)nxgl_gles2_d_glUniform2iv },
  { "glUniform3f", (void **)&nxgl_gles2_pfn_glUniform3f, (void *)nxgl_gles2_d_glUniform3f },
  { "glUniform3fv", (void **)&nxgl_gles2_pfn_glUniform3fv, (void *)nxgl_gles2_d_glUniform3fv },
  { "glUniform3i", (void **)&nxgl_gles2_pfn_glUniform3i, (void *)nxgl_gles2_d_glUniform3i },
  { "glUniform3iv", (void **)&nxgl_gles2_pfn_glUniform3iv, (void *)nxgl_gles2_d_glUniform3iv },
  { "glUniform4f", (void **)&nxgl_gles2_pfn_glUniform4f, (void *)nxgl_gles2_d_glUniform4f },
  { "glUniform4fv", (void **)&nxgl_gles2_pfn_glUniform4fv, (void *)nxgl_gles2_d_glUniform4fv },
  { "glUniform4i", (void **)&nxgl_gles2_pfn_glUniform4i, (void *)nxgl_gles2_d_glUniform4i },
  { "glUniform4iv", (void **)&nxgl_gles2_pfn_glUniform4iv, (void *)nxgl_gles2_d_glUniform4iv },
  { "glUniformMatrix2fv", (void **)&nxgl_gles2_pfn_glUniformMatrix2fv, (void *)nxgl_gles2_d_glUniformMatrix2fv },
  { "glUniformMatrix3fv", (void **)&nxgl_gles2_pfn_glUniformMatrix3fv, (void *)nxgl_gles2_d_glUniformMatrix3fv },
  { "glUniformMatrix4fv", (void **)&nxgl_gles2_pfn_glUniformMatrix4fv, (void *)nxgl_gles2_d_glUniformMatrix4fv },
  { "glUseProgram", (void **)&nxgl_gles2_pfn_glUseProgram, (void *)nxgl_gles2_d_glUseProgram },
  { "glValidateProgram", (void **)&nxgl_gles2_pfn_glValidateProgram, (void *)nxgl_gles2_d_glValidateProgram },
  { "glVertexAttrib1f", (void **)&nxgl_gles2_pfn_glVertexAttrib1f, (void *)nxgl_gles2_d_glVertexAttrib1f },
  { "glVertexAttrib1fv", (void **)&nxgl_gles2_pfn_glVertexAttrib1fv, (void *)nxgl_gles2_d_glVertexAttrib1fv },
  { "glVertexAttrib2f", (void **)&nxgl_gles2_pfn_glVertexAttrib2f, (void *)nxgl_gles2_d_glVertexAttrib2f },
  { "glVertexAttrib2fv", (void **)&nxgl_gles2_pfn_glVertexAttrib2fv, (void *)nxgl_gles2_d_glVertexAttrib2fv },
  { "glVertexAttrib3f", (void **)&nxgl_gles2_pfn_glVertexAttrib3f, (void *)nxgl_gles2_d_glVertexAttrib3f },
  { "glVertexAttrib3fv", (void **)&nxgl_gles2_pfn_glVertexAttrib3fv, (void *)nxgl_gles2_d_glVertexAttrib3fv },
  { "glVertexAttrib4f", (void **)&nxgl_gles2_pfn_glVertexAttrib4f, (void *)nxgl_gles2_d_glVertexAttrib4f },
  { "glVertexAttrib4fv", (void **)&nxgl_gles2_pfn_glVertexAttrib4fv, (void *)nxgl_gles2_d_glVertexAttrib4fv },
  { "glVertexAttribPointer", (void **)&nxgl_gles2_pfn_glVertexAttribPointer, (void *)nxgl_gles2_d_glVertexAttribPointer },
  { "glViewport", (void **)&nxgl_gles2_pfn_glViewport, (void *)nxgl_gles2_d_glViewport },
  { "glGetString", (void **)&nxgl_gles2_pfn_glGetString, (void *)nxgl_gles2_d_glGetString },
};

static char g_provider[NXGL_GLES2_PROVIDER_MAX];
static char g_liveness[16];
static int g_done;
static nxgl_gles2_resolver_fn g_primary_resolver;

typedef void *(*nxgl_gles2_eglGetProcAddress_fn)(const char *);

void nxgl_gles2_set_primary_resolver(nxgl_gles2_resolver_fn resolver) {
  g_primary_resolver = resolver;
  /* Uma fonte primaria nova (o contexto SDL vivo) e' mais coerente do que
   * qualquer medicao anterior (ex.: lazy-init no contexto de PROBE da
   * engine): a proxima nxgl_gles2_init re-mede. O GOT da engine aponta para
   * as funcoes de despacho, entao recomprometer os ponteiros e' seguro. */
  g_done = 0;
}

#define NXGL_GLES2_ENTRY_COUNT (sizeof(k_entries) / sizeof(k_entries[0]))
#define NXGL_GLES2_GETSTRING_INDEX (NXGL_GLES2_ENTRY_COUNT - 1u)

typedef const GLubyte *(*nxgl_gles2_get_string_fn)(GLenum name);

static int staging_is_live(void *const *staging) {
  nxgl_gles2_get_string_fn get_string =
      (nxgl_gles2_get_string_fn)staging[NXGL_GLES2_GETSTRING_INDEX];
  const GLubyte *renderer;

  if (get_string == NULL) {
    return 0;
  }
  renderer = get_string(0x1F01 /* GL_RENDERER */);
  return renderer != NULL && renderer[0] != '\0';
}

static unsigned resolve_set_from_dlsym(void *handle, void **staging) {
  unsigned resolved = 0;
  size_t i;

  for (i = 0; i < NXGL_GLES2_ENTRY_COUNT; ++i) {
    staging[i] = dlsym(handle, k_entries[i].name);
    if (staging[i] != NULL) {
      ++resolved;
    }
  }
  return resolved;
}

static unsigned resolve_set_from_callback(void *(*resolver)(const char *),
                                          void **staging) {
  unsigned resolved = 0;
  size_t i;

  for (i = 0; i < NXGL_GLES2_ENTRY_COUNT; ++i) {
    staging[i] = resolver(k_entries[i].name);
    if (staging[i] != NULL) {
      ++resolved;
    }
  }
  return resolved;
}

static void commit_staging(void *const *staging) {
  size_t i;

  for (i = 0; i < NXGL_GLES2_ENTRY_COUNT; ++i) {
    *k_entries[i].slot = staging[i];
  }
}

struct nxgl_gles2_candidate {
  char name[NXGL_GLES2_PROVIDER_MAX];
  void *staging[NXGL_GLES2_ENTRY_COUNT];
};

int nxgl_gles2_init(nxgl_gles2_receipt *receipt) {
  void *handles[sizeof(k_providers) / sizeof(k_providers[0])];
  static struct nxgl_gles2_candidate candidate; /* fora da pilha do port */
  static struct nxgl_gles2_candidate first_complete;
  nxgl_gles2_eglGetProcAddress_fn get_proc;
  const char *missing = "";
  unsigned resolved = 0;
  unsigned rejected_dead = 0;
  int have_complete = 0;
  int committed = 0;
  size_t provider_count = sizeof(k_providers) / sizeof(k_providers[0]);
  size_t entry_count = NXGL_GLES2_ENTRY_COUNT;
  size_t i;

  if (g_done) {
    if (receipt != NULL) {
      memset(receipt, 0, sizeof(*receipt));
      snprintf(receipt->provider, sizeof(receipt->provider), "%s", g_provider);
      receipt->resolved = (unsigned)entry_count;
      receipt->total = (unsigned)entry_count;
      snprintf(receipt->text, sizeof(receipt->text),
               "GLES2: provider=%s resolved=%u/%u liveness=%s (cache)",
               g_provider, receipt->resolved, receipt->total, g_liveness);
    }
    return 0;
  }

  for (i = 0; i < provider_count; ++i) {
    handles[i] = dlopen(k_providers[i], RTLD_NOW | RTLD_LOCAL);
  }
  get_proc = (nxgl_gles2_eglGetProcAddress_fn)dlsym(RTLD_DEFAULT,
                                                   "eglGetProcAddress");

  /* Candidatos, na ordem de coerencia. O laco para no primeiro VIVO. */
  for (i = 0; i < 3u + provider_count && !committed; ++i) {
    unsigned count = 0;

    candidate.name[0] = '\0';
    if (i == 0u) {
      if (g_primary_resolver == NULL) {
        continue;
      }
      snprintf(candidate.name, sizeof(candidate.name), "primary-resolver");
      count = resolve_set_from_callback(
          (void *(*)(const char *))g_primary_resolver, candidate.staging);
    } else if (i == 1u) {
      snprintf(candidate.name, sizeof(candidate.name), "RTLD_DEFAULT");
      count = resolve_set_from_dlsym(RTLD_DEFAULT, candidate.staging);
    } else if (i == 2u) {
      if (get_proc == NULL) {
        continue;
      }
      snprintf(candidate.name, sizeof(candidate.name), "eglGetProcAddress");
      count = resolve_set_from_callback(
          (void *(*)(const char *))get_proc, candidate.staging);
    } else {
      if (handles[i - 3u] == NULL) {
        continue;
      }
      snprintf(candidate.name, sizeof(candidate.name), "%s",
               k_providers[i - 3u]);
      count = resolve_set_from_dlsym(handles[i - 3u], candidate.staging);
    }

    if (count != (unsigned)entry_count) {
      continue; /* conjunto incompleto nao e' candidato */
    }
    if (!have_complete) {
      have_complete = 1;
      first_complete = candidate;
    }
    if (staging_is_live(candidate.staging)) {
      commit_staging(candidate.staging);
      snprintf(g_provider, sizeof(g_provider), "%s", candidate.name);
      snprintf(g_liveness, sizeof(g_liveness), "ok");
      resolved = count;
      committed = 1;
    } else {
      ++rejected_dead;
    }
  }

  if (!committed && have_complete) {
    commit_staging(first_complete.staging);
    snprintf(g_provider, sizeof(g_provider), "%s", first_complete.name);
    snprintf(g_liveness, sizeof(g_liveness), "dead");
    resolved = (unsigned)entry_count;
    committed = 1;
  }

  if (!committed) {
    /* Ultimo recurso: montagem mista por simbolo, fontes distintas. */
    const char *first_origin = "";

    for (i = 0; i < entry_count; ++i) {
      const char *origin = "";
      void *addr = dlsym(RTLD_DEFAULT, k_entries[i].name);

      if (addr != NULL) {
        origin = "RTLD_DEFAULT";
      }
      if (addr == NULL && get_proc != NULL) {
        addr = get_proc(k_entries[i].name);
        if (addr != NULL) {
          origin = "eglGetProcAddress";
        }
      }
      if (addr == NULL) {
        size_t j;
        for (j = 0; j < provider_count; ++j) {
          if (handles[j] == NULL) {
            continue;
          }
          addr = dlsym(handles[j], k_entries[i].name);
          if (addr != NULL) {
            origin = k_providers[j];
            break;
          }
        }
      }
      if (addr == NULL) {
        if (missing[0] == '\0') {
          missing = k_entries[i].name;
        }
        continue;
      }
      *k_entries[i].slot = addr;
      if (first_origin[0] == '\0') {
        first_origin = origin;
      }
      ++resolved;
    }
    snprintf(g_provider, sizeof(g_provider), "%s",
             first_origin[0] != '\0' ? first_origin : "nenhum");
    snprintf(g_liveness, sizeof(g_liveness), "mixed");
  }

  if (receipt != NULL) {
    memset(receipt, 0, sizeof(*receipt));
    snprintf(receipt->provider, sizeof(receipt->provider), "%s", g_provider);
    receipt->resolved = resolved;
    receipt->total = (unsigned)entry_count;
    snprintf(receipt->first_missing, sizeof(receipt->first_missing), "%s",
             missing);
    if (missing[0] != '\0') {
      snprintf(receipt->text, sizeof(receipt->text),
               "GLES2: provider=%s resolved=%u/%u first_missing=%s",
               g_provider, resolved, receipt->total, missing);
    } else {
      snprintf(receipt->text, sizeof(receipt->text),
               "GLES2: provider=%s resolved=%u/%u liveness=%s%s%u",
               g_provider, resolved, receipt->total, g_liveness,
               " rejected-dead=", rejected_dead);
    }
  }

  if (strcmp(g_liveness, "mixed") != 0) {
    for (i = 0; i < provider_count; ++i) {
      if (handles[i] != NULL && strcmp(g_provider, k_providers[i]) != 0) {
        dlclose(handles[i]);
      }
    }
  }

  if (resolved != (unsigned)entry_count) {
    return -1;
  }
  g_done = 1;
  return 0;
}

void *nxgl_gles2_lookup(const char *name) {
  size_t entry_count = sizeof(k_entries) / sizeof(k_entries[0]);
  size_t i;

  if (name == NULL) {
    return NULL;
  }
  for (i = 0; i < entry_count; ++i) {
    if (strcmp(k_entries[i].name, name) == 0) {
      return *k_entries[i].slot;
    }
  }
  return NULL;
}

void *nxgl_gles2_dispatch_lookup(const char *name) {
  size_t entry_count = sizeof(k_entries) / sizeof(k_entries[0]);
  size_t i;

  if (name == NULL) {
    return NULL;
  }
  for (i = 0; i < entry_count; ++i) {
    if (strcmp(k_entries[i].name, name) == 0) {
      return k_entries[i].dispatch;
    }
  }
  return NULL;
}

/* Lazy-init dos despachos: uma engine que faz probe GL ANTES da janela SDL
 * (contexto EGL proprio) chega aqui com os ponteiros ainda nulos. Medir na
 * hora, sem resolvedor primario, e' correto: o contexto corrente e' o do
 * probe, e a prova de vida escolhe um provedor coerente com ele. */
static void nxgl_gles2_lazy_init(void) {
  nxgl_gles2_receipt receipt;
  nxgl_gles2_init(&receipt);
  fprintf(stderr, "[nxgl] lazy-init: %s\n", receipt.text);
}

const char *nxgl_gles2_provider(void) { return g_provider; }

const char *nxgl_gles2_liveness(void) { return g_liveness; }
