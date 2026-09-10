/* ab_gl.c — entradas GLES2 vistas pelo convidado.
 *
 * A .so importa 69 símbolos GLES2 e NENHUM GLES3: o encaixe no Utgard é direto
 * e não existe tradutor de shader neste port. Só três entradas recebem float
 * POR VALOR e por isso precisam da borda softfp→hardfp; o resto passa direto
 * pro driver do device.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include "nxgl_gles2.h" /* as bordas softfp chamam os ponteiros provados */
#include <SDL.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include "ab_port.h"

#define AB_SOFTFP __attribute__((pcs("aapcs")))

int ab_gl_provider_exports(void *handle) {
  static const char *const required[] = {
      "glClearColor", "glUniform1f", "glUniform2f", "glUniform3f",
      "glUniform4f", "glClearDepthf", "glDepthRangef", "glLineWidth",
      "glPolygonOffset", "glTexParameterf", "glBlendColor",
      "glSampleCoverage", "glVertexAttrib4f", "glActiveTexture",
      "glAttachShader", "glBindAttribLocation", "glBindBuffer",
      "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture",
      "glBlendEquation", "glBlendEquationSeparate", "glBlendFunc",
      "glBlendFuncSeparate", "glBufferData", "glBufferSubData",
      "glCheckFramebufferStatus", "glClear", "glClearStencil",
      "glColorMask", "glCompileShader", "glCompressedTexImage2D",
      "glCompressedTexSubImage2D", "glCopyTexImage2D",
      "glCopyTexSubImage2D", "glCreateProgram", "glCreateShader",
      "glCullFace", "glDeleteBuffers", "glDeleteFramebuffers",
      "glDeleteProgram", "glDeleteRenderbuffers", "glDeleteShader",
      "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDetachShader",
      "glDisable", "glDisableVertexAttribArray", "glDrawArrays",
      "glDrawElements", "glEnable", "glEnableVertexAttribArray",
      "glFinish", "glFlush", "glFramebufferRenderbuffer",
      "glFramebufferTexture2D", "glFrontFace", "glGenBuffers",
      "glGenFramebuffers", "glGenRenderbuffers", "glGenTextures",
      "glGenerateMipmap", "glGetActiveAttrib", "glGetActiveUniform",
      "glGetAttribLocation", "glGetBooleanv", "glGetError", "glGetFloatv",
      "glGetIntegerv", "glGetProgramInfoLog", "glGetProgramiv",
      "glGetShaderInfoLog", "glGetShaderiv", "glGetString",
      "glGetUniformfv", "glGetUniformiv", "glGetUniformLocation", "glHint",
      "glIsEnabled", "glIsTexture", "glLinkProgram", "glPixelStorei",
      "glReadPixels", "glRenderbufferStorage", "glScissor",
      "glShaderSource", "glStencilFunc", "glStencilMask", "glStencilOp",
      "glTexImage2D", "glTexParameteri", "glTexSubImage2D", "glUniform1i",
      "glUniform1fv", "glUniform2fv", "glUniform3fv", "glUniform4fv",
      "glUniform1iv", "glUniformMatrix3fv", "glUniformMatrix4fv",
      "glUseProgram", "glValidateProgram", "glVertexAttribPointer",
      "glViewport"};
  size_t index;
  if (!handle)
    return 0;
  for (index = 0u; index < sizeof(required) / sizeof(required[0]); ++index) {
    if (!dlsym(handle, required[index]))
      return 0;
  }
  return 1;
}

AB_SOFTFP static void sf_glClearColor(GLclampf r, GLclampf g, GLclampf b,
                                      GLclampf a) {
  glClearColor(r, g, b, a);
}
AB_SOFTFP static void sf_glUniform1f(GLint location, GLfloat v) {
  glUniform1f(location, v);
}
AB_SOFTFP static void sf_glUniform4f(GLint location, GLfloat x, GLfloat y,
                                     GLfloat z, GLfloat w) {
  glUniform4f(location, x, y, z, w);
}
/* Não estão nos UND desta build, mas registrar é grátis e protege uma variante
 * do APK que os use. */
AB_SOFTFP static void sf_glClearDepthf(GLclampf d) { glClearDepthf(d); }
AB_SOFTFP static void sf_glDepthRangef(GLclampf n, GLclampf f) {
  glDepthRangef(n, f);
}
AB_SOFTFP static void sf_glLineWidth(GLfloat w) { glLineWidth(w); }
AB_SOFTFP static void sf_glPolygonOffset(GLfloat factor, GLfloat units) {
  glPolygonOffset(factor, units);
}
AB_SOFTFP static void sf_glTexParameterf(GLenum target, GLenum pname,
                                         GLfloat param) {
  glTexParameterf(target, pname, param);
}
AB_SOFTFP static void sf_glBlendColor(GLclampf r, GLclampf g, GLclampf b,
                                      GLclampf a) {
  glBlendColor(r, g, b, a);
}
AB_SOFTFP static void sf_glSampleCoverage(GLclampf value, GLboolean invert) {
  glSampleCoverage(value, invert);
}
AB_SOFTFP static void sf_glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y,
                                          GLfloat z, GLfloat w) {
  glVertexAttrib4f(index, x, y, z, w);
}
AB_SOFTFP static void sf_glUniform2f(GLint l, GLfloat x, GLfloat y) {
  glUniform2f(l, x, y);
}
AB_SOFTFP static void sf_glUniform3f(GLint l, GLfloat x, GLfloat y, GLfloat z) {
  glUniform3f(l, x, y, z);
}

nxloader_result ab_add_gl_provider(nxloader_registry *registry) {
  static nxloader_symbol symbols[112];
  size_t count = 0;
  nxloader_provider provider;

#define ADD(sym_name, sym_addr)                     \
  do {                                              \
    symbols[count].name = (sym_name);               \
    symbols[count].address = (uintptr_t)(sym_addr); \
    symbols[count].flags = 0;                       \
    count++;                                        \
  } while (0)
#define DIRECT(fn)                                                          \
  do {                                                                      \
    void *resolved = SDL_GL_GetProcAddress(#fn);                             \
    if (!resolved) {                                                        \
      ab_log("[gl] simbolo obrigatorio ausente: %s", #fn);                 \
      return NXLOADER_EUNRESOLVED;                                          \
    }                                                                       \
    ADD(#fn, resolved);                                                      \
  } while (0)

  /* borda softfp */
  ADD("glClearColor", sf_glClearColor);
  ADD("glUniform1f", sf_glUniform1f);
  ADD("glUniform2f", sf_glUniform2f);
  ADD("glUniform3f", sf_glUniform3f);
  ADD("glUniform4f", sf_glUniform4f);
  ADD("glClearDepthf", sf_glClearDepthf);
  ADD("glDepthRangef", sf_glDepthRangef);
  ADD("glLineWidth", sf_glLineWidth);
  ADD("glPolygonOffset", sf_glPolygonOffset);
  ADD("glTexParameterf", sf_glTexParameterf);
  ADD("glBlendColor", sf_glBlendColor);
  ADD("glSampleCoverage", sf_glSampleCoverage);
  ADD("glVertexAttrib4f", sf_glVertexAttrib4f);

  /* passagem direta */
  DIRECT(glActiveTexture);
  DIRECT(glAttachShader);
  DIRECT(glBindAttribLocation);
  DIRECT(glBindBuffer);
  DIRECT(glBindFramebuffer);
  DIRECT(glBindRenderbuffer);
  DIRECT(glBindTexture);
  DIRECT(glBlendEquation);
  DIRECT(glBlendEquationSeparate);
  DIRECT(glBlendFunc);
  DIRECT(glBlendFuncSeparate);
  DIRECT(glBufferData);
  DIRECT(glBufferSubData);
  DIRECT(glCheckFramebufferStatus);
  DIRECT(glClear);
  DIRECT(glClearStencil);
  DIRECT(glColorMask);
  DIRECT(glCompileShader);
  DIRECT(glCompressedTexImage2D);
  DIRECT(glCompressedTexSubImage2D);
  DIRECT(glCopyTexImage2D);
  DIRECT(glCopyTexSubImage2D);
  DIRECT(glCreateProgram);
  DIRECT(glCreateShader);
  DIRECT(glCullFace);
  DIRECT(glDeleteBuffers);
  DIRECT(glDeleteFramebuffers);
  DIRECT(glDeleteProgram);
  DIRECT(glDeleteRenderbuffers);
  DIRECT(glDeleteShader);
  DIRECT(glDeleteTextures);
  DIRECT(glDepthFunc);
  DIRECT(glDepthMask);
  DIRECT(glDetachShader);
  DIRECT(glDisable);
  DIRECT(glDisableVertexAttribArray);
  DIRECT(glDrawArrays);
  DIRECT(glDrawElements);
  DIRECT(glEnable);
  DIRECT(glEnableVertexAttribArray);
  DIRECT(glFinish);
  DIRECT(glFlush);
  DIRECT(glFramebufferRenderbuffer);
  DIRECT(glFramebufferTexture2D);
  DIRECT(glFrontFace);
  DIRECT(glGenBuffers);
  DIRECT(glGenFramebuffers);
  DIRECT(glGenRenderbuffers);
  DIRECT(glGenTextures);
  DIRECT(glGenerateMipmap);
  DIRECT(glGetActiveAttrib);
  DIRECT(glGetActiveUniform);
  DIRECT(glGetAttribLocation);
  DIRECT(glGetBooleanv);
  DIRECT(glGetError);
  DIRECT(glGetFloatv);
  DIRECT(glGetIntegerv);
  DIRECT(glGetProgramInfoLog);
  DIRECT(glGetProgramiv);
  DIRECT(glGetShaderInfoLog);
  DIRECT(glGetShaderiv);
  DIRECT(glGetString);
  DIRECT(glGetUniformfv);
  DIRECT(glGetUniformiv);
  DIRECT(glGetUniformLocation);
  DIRECT(glHint);
  DIRECT(glIsEnabled);
  DIRECT(glIsTexture);
  DIRECT(glLinkProgram);
  DIRECT(glPixelStorei);
  DIRECT(glReadPixels);
  DIRECT(glRenderbufferStorage);
  DIRECT(glScissor);
  DIRECT(glShaderSource);
  DIRECT(glStencilFunc);
  DIRECT(glStencilMask);
  DIRECT(glStencilOp);
  DIRECT(glTexImage2D);
  DIRECT(glTexParameteri);
  DIRECT(glTexSubImage2D);
  DIRECT(glUniform1i);
  DIRECT(glUniform1fv);
  DIRECT(glUniform2fv);
  DIRECT(glUniform3fv);
  DIRECT(glUniform4fv);
  DIRECT(glUniform1iv);
  DIRECT(glUniformMatrix3fv);
  DIRECT(glUniformMatrix4fv);
  DIRECT(glUseProgram);
  DIRECT(glValidateProgram);
  DIRECT(glVertexAttribPointer);
  DIRECT(glViewport);
#undef DIRECT
#undef ADD

  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "angrybirds-gles2";
  provider.symbols = symbols;
  provider.symbol_count = count;
  provider.priority = 40;
  return nxloader_registry_add_provider(registry, &provider, NULL);
}
