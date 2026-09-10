/*
 * gl_state.c -- see gl_state.h for why this exists.
 *
 * Every entry point here is installed in the engine's import table, so the
 * shadow sees the same calls the driver does.  Nothing reads back from GL.
 */
#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <string.h>

#include "egl_shim.h"
#include "gl_state.h"

static gd_gl_state g;

/* ---------------------------------------------------------------- helpers -- */
static unsigned char *cap_slot(GLenum cap) {
  switch (cap) {
  case GL_BLEND:
    return &g.blend;
  case GL_DEPTH_TEST:
    return &g.depth;
  case GL_SCISSOR_TEST:
    return &g.scissor_test;
  case GL_CULL_FACE:
    return &g.cull;
  default:
    return NULL;
  }
}

static int tex_unit(void) {
  int u = (int)(g.active - GL_TEXTURE0);
  return (u >= 0 && u < GD_TEXUNITS) ? u : 0;
}

/* --------------------------------------------------------------- wrappers -- */
void gd_glUseProgram(GLuint p) {
  g.prog = p;
  glUseProgram(p);
}

void gd_glActiveTexture(GLenum t) {
  g.active = t;
  glActiveTexture(t);
}

void gd_glBindTexture(GLenum target, GLuint tex) {
  if (target == GL_TEXTURE_2D)
    g.tex[tex_unit()] = tex;
  glBindTexture(target, tex);
}

void gd_glBindBuffer(GLenum target, GLuint buf) {
  if (target == GL_ARRAY_BUFFER)
    g.arraybuf = buf;
  glBindBuffer(target, buf);
}

void gd_glBindFramebuffer(GLenum target, GLuint fb) {
  g.fbo = fb;
  glBindFramebuffer(target, fb);
}

void gd_glBlendFunc(GLenum s, GLenum d) {
  g.bsrc = s;
  g.bdst = d;
  glBlendFunc(s, d);
}

void gd_glEnable(GLenum cap) {
  unsigned char *s = cap_slot(cap);
  if (s)
    *s = 1;
  glEnable(cap);
}

void gd_glDisable(GLenum cap) {
  unsigned char *s = cap_slot(cap);
  if (s)
    *s = 0;
  glDisable(cap);
}

void gd_glstate_viewport_raw(int x, int y, int w, int h) {
  g.viewport[0] = x;
  g.viewport[1] = y;
  g.viewport[2] = w;
  g.viewport[3] = h;
  glViewport(x, y, w, h);
}

/* The engine may be told a smaller size than the real framebuffer (GD_RES); in
 * that case every rectangle it hands us is scaled up to the panel here. */
static int scale_x(int v) {
  int ew = gd_engine_w(), w = gd_win_w();
  return (ew == w) ? v : (int)((long)v * w / ew);
}
static int scale_y(int v) {
  int eh = gd_engine_h(), h = gd_win_h();
  return (eh == h) ? v : (int)((long)v * h / eh);
}

void gd_glViewport(int x, int y, int w, int h) {
  gd_glstate_viewport_raw(scale_x(x), scale_y(y), scale_x(w), scale_y(h));
}

void gd_glScissor(int x, int y, int w, int h) {
  g.scissor[0] = scale_x(x);
  g.scissor[1] = scale_y(y);
  g.scissor[2] = scale_x(w);
  g.scissor[3] = scale_y(h);
  glScissor(g.scissor[0], g.scissor[1], g.scissor[2], g.scissor[3]);
}

void gd_glEnableVertexAttribArray(GLuint i) {
  if (i < GD_ATTRIBS)
    g.attr[i].on = 1;
  glEnableVertexAttribArray(i);
}

void gd_glDisableVertexAttribArray(GLuint i) {
  if (i < GD_ATTRIBS)
    g.attr[i].on = 0;
  glDisableVertexAttribArray(i);
}

void gd_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm,
                              GLsizei stride, const void *ptr) {
  if (i < GD_ATTRIBS) {
    g.attr[i].size = size;
    g.attr[i].type = type;
    g.attr[i].norm = norm;
    g.attr[i].stride = stride;
    g.attr[i].ptr = ptr;
    g.attr[i].buf = g.arraybuf;
  }
  glVertexAttribPointer(i, size, type, norm, stride, ptr);
}

/* ------------------------------------------------------------ save/restore -- */
void gd_glstate_init(int fb_w, int fb_h) {
  memset(&g, 0, sizeof(g));
  g.active = GL_TEXTURE0;
  g.bsrc = GL_ONE;
  g.bdst = GL_ZERO;
  g.viewport[2] = fb_w;
  g.viewport[3] = fb_h;
  g.scissor[2] = fb_w;
  g.scissor[3] = fb_h;
  for (int i = 0; i < GD_ATTRIBS; i++) {
    g.attr[i].size = 4;
    g.attr[i].type = GL_FLOAT;
  }
}

void gd_glstate_save(gd_gl_state *out) { *out = g; }

void gd_glstate_restore(const gd_gl_state *s) {
  /* Vertex arrays first: the pointer call latches whatever ARRAY_BUFFER is
   * bound at that moment, so each attribute is restored under its own buffer
   * and the real ARRAY_BUFFER binding is put back afterwards. */
  for (int i = 0; i < GD_ATTRIBS; i++) {
    const gd_attr_state *a = &s->attr[i], *c = &g.attr[i];
    if (a->size != c->size || a->type != c->type || a->norm != c->norm ||
        a->stride != c->stride || a->ptr != c->ptr || a->buf != c->buf) {
      gd_glBindBuffer(GL_ARRAY_BUFFER, a->buf);
      gd_glVertexAttribPointer((GLuint)i, a->size, a->type, a->norm, a->stride,
                               a->ptr);
    }
    if (a->on != c->on) {
      if (a->on)
        gd_glEnableVertexAttribArray((GLuint)i);
      else
        gd_glDisableVertexAttribArray((GLuint)i);
    }
  }
  if (g.arraybuf != s->arraybuf)
    gd_glBindBuffer(GL_ARRAY_BUFFER, s->arraybuf);
  if (g.fbo != s->fbo)
    gd_glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);

  /* Textures: bind each unit under its own active unit, then put the active
   * unit itself back.  Doing this in one pass with the wrong unit selected is
   * how the engine ends up sampling the cursor texture. */
  for (int u = 0; u < GD_TEXUNITS; u++) {
    if (g.tex[u] != s->tex[u]) {
      gd_glActiveTexture((GLenum)(GL_TEXTURE0 + u));
      gd_glBindTexture(GL_TEXTURE_2D, s->tex[u]);
    }
  }
  if (g.active != s->active)
    gd_glActiveTexture(s->active);

  if (g.prog != s->prog)
    gd_glUseProgram(s->prog);
  if (g.bsrc != s->bsrc || g.bdst != s->bdst)
    gd_glBlendFunc(s->bsrc, s->bdst);

  if (g.blend != s->blend)
    (s->blend ? gd_glEnable : gd_glDisable)(GL_BLEND);
  if (g.depth != s->depth)
    (s->depth ? gd_glEnable : gd_glDisable)(GL_DEPTH_TEST);
  if (g.scissor_test != s->scissor_test)
    (s->scissor_test ? gd_glEnable : gd_glDisable)(GL_SCISSOR_TEST);
  if (g.cull != s->cull)
    (s->cull ? gd_glEnable : gd_glDisable)(GL_CULL_FACE);

  if (memcmp(g.viewport, s->viewport, sizeof(g.viewport)) != 0)
    gd_glstate_viewport_raw(s->viewport[0], s->viewport[1], s->viewport[2],
                            s->viewport[3]);
  if (memcmp(g.scissor, s->scissor, sizeof(g.scissor)) != 0) {
    memcpy(g.scissor, s->scissor, sizeof(g.scissor));
    glScissor(g.scissor[0], g.scissor[1], g.scissor[2], g.scissor[3]);
  }
}
