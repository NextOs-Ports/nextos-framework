/*
 * gl_state.h -- shadow copy of the GL state the cursor overlay disturbs.
 *
 * The engine keeps its own C++ cache of the GL state (ccGLStateCache), so any
 * bind we leave behind desyncs it.  The obvious repair -- calling the engine's
 * ccGLInvalidateStateCache() -- is a trap: on Cocos2d-x 2.x that function starts
 * with kmGLFreeAll(), which releases the kazmath modelview/projection stacks.
 * The projection is only rebuilt by CCDirector::setProjection(), not per frame,
 * so the very next frame draws with an identity projection: black screen, or
 * flat purple/garbage geometry.  That is exactly what moving the cursor did.
 *
 * Instead we mirror every state-setting call the engine makes (all of them are
 * imports we already resolve ourselves), so the overlay can put the state back
 * byte for byte with no glGet* at all -- glGet* stalls the Utgard pipeline and
 * this runs every frame.
 */
#ifndef __GL_STATE_H__
#define __GL_STATE_H__

#include <GLES2/gl2.h>

#define GD_TEXUNITS 8
#define GD_ATTRIBS 8

typedef struct {
  GLint size;
  GLenum type;
  GLboolean norm;
  GLsizei stride;
  const void *ptr;
  GLuint buf; /* ARRAY_BUFFER bound when the pointer was set */
  unsigned char on;
} gd_attr_state;

typedef struct {
  GLuint prog, arraybuf, fbo;
  GLenum active;
  GLuint tex[GD_TEXUNITS];
  GLenum bsrc, bdst;
  GLint viewport[4], scissor[4];
  unsigned char blend, depth, scissor_test, cull;
  gd_attr_state attr[GD_ATTRIBS];
} gd_gl_state;

/* Seed the shadow with the GL defaults; call once, right after the context is
 * current and the framebuffer size is known. */
void gd_glstate_init(int fb_w, int fb_h);

/* Pure memory copy -- issues no GL calls. */
void gd_glstate_save(gd_gl_state *out);

/* Puts the state back, skipping anything that already matches. */
void gd_glstate_restore(const gd_gl_state *s);

/* The overlay drives GL through these so the shadow stays truthful. */
void gd_glUseProgram(GLuint p);
void gd_glActiveTexture(GLenum t);
void gd_glBindTexture(GLenum target, GLuint tex);
void gd_glBindBuffer(GLenum target, GLuint buf);
void gd_glBindFramebuffer(GLenum target, GLuint fb);
void gd_glBlendFunc(GLenum s, GLenum d);
void gd_glEnable(GLenum cap);
void gd_glDisable(GLenum cap);
void gd_glViewport(int x, int y, int w, int h);
void gd_glScissor(int x, int y, int w, int h);
void gd_glEnableVertexAttribArray(GLuint i);
void gd_glDisableVertexAttribArray(GLuint i);
void gd_glVertexAttribPointer(GLuint i, GLint size, GLenum type,
                              GLboolean norm, GLsizei stride, const void *ptr);

/* Raw viewport/scissor in framebuffer pixels -- the wrappers above scale from
 * engine space (GD_RES) and the overlay needs the unscaled form. */
void gd_glstate_viewport_raw(int x, int y, int w, int h);

#endif
