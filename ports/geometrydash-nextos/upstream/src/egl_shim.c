/*
 * egl_shim.c -- GLES2 context through SDL2.
 *
 * SDL2's "mali" video driver builds the EGL fbdev surface the Utgard driver
 * actually accepts, so we let it own the window and never touch EGL directly
 * (raw eglCreateWindowSurface on an fbdev window answers BAD_ALLOC here).  The
 * SDL video/audio driver is never forced -- SDL picks it.
 *
 * The framebuffer size is read from the display, never hardcoded.  When the
 * engine is told a smaller size than the real framebuffer (GD_RES), every
 * viewport/scissor rectangle it sets is scaled up here, so the picture still
 * fills the screen.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "egl_shim.h"
#include "gl_state.h"
#include "util.h"

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static int g_w, g_h;         /* real framebuffer */
static int g_ew, g_eh;       /* what the engine believes */

static void parse_engine_size(void) {
  g_ew = g_w;
  g_eh = g_h;
  const char *r = getenv("GD_RES");
  if (!r || !*r)
    return;
  int w = 0, h = 0;
  if (sscanf(r, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
    g_ew = w;
    g_eh = h;
    debugPrintf("[gl] engine roda em %dx%d, framebuffer %dx%d (escala ativa)\n",
                g_ew, g_eh, g_w, g_h);
  }
}

int gd_gl_init(void) {
  if (g_ctx)
    return 1;
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    debugPrintf("[sdl] InitVideo: %s\n", SDL_GetError());
    return 0;
  }

  SDL_DisplayMode dm;
  g_w = 0;
  g_h = 0;
  if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w;
    g_h = dm.h;
  } else if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w;
    g_h = dm.h;
  }
  if (g_w <= 0 || g_h <= 0) {
    debugPrintf("[sdl] nao consegui ler o modo do display: %s\n", SDL_GetError());
    return 0;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  /* O titulo vem do jogo que esta' sendo compilado (o build define GD_TITLE);
   * este arquivo e' comum aos dois ports. */
#ifndef GD_TITLE
#define GD_TITLE "Geometry Dash"
#endif
  g_win = SDL_CreateWindow(GD_TITLE, SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, g_w, g_h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!g_win) {
    debugPrintf("[sdl] CreateWindow: %s\n", SDL_GetError());
    return 0;
  }
  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) {
    debugPrintf("[sdl] GL_CreateContext: %s\n", SDL_GetError());
    return 0;
  }
  SDL_GL_MakeCurrent(g_win, g_ctx);
  SDL_GL_SetSwapInterval(1);
  SDL_ShowCursor(SDL_DISABLE);

  int dw = 0, dh = 0;
  SDL_GL_GetDrawableSize(g_win, &dw, &dh);
  if (dw > 0 && dh > 0) {
    g_w = dw;
    g_h = dh;
  }
  parse_engine_size();
  gd_glstate_init(g_w, g_h);

  const GLubyte *rend = glGetString(GL_RENDERER), *ver = glGetString(GL_VERSION);
  debugPrintf("[gl] %dx%d driver=%s | %s / %s\n", g_w, g_h,
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
              rend ? (const char *)rend : "?", ver ? (const char *)ver : "?");
  /* Mesa (Panfrost, Lima, Freedreno) pode entregar um contexto de OpenGL de
   * desktop mesmo com o perfil ES pedido. A engine so' fala GLES2, e o
   * sintoma de um contexto errado e' tela preta com tudo "funcionando" -- por
   * isso o contrato e' CONFERIDO e escrito no log, nunca presumido. */
  if (ver && !strstr((const char *)ver, "OpenGL ES"))
    debugPrintf("[gl] AVISO: o driver devolveu '%s', que nao e' OpenGL ES; "
                "a engine so' fala GLES2 e o desenho pode sair vazio\n",
                (const char *)ver);
  return 1;
}

int gd_win_w(void) { return g_w; }
int gd_win_h(void) { return g_h; }
int gd_engine_w(void) { return g_ew ? g_ew : g_w; }
int gd_engine_h(void) { return g_eh ? g_eh : g_h; }

void gd_gl_swap(void) {
  if (g_win)
    SDL_GL_SwapWindow(g_win);
}

void gd_gl_shutdown(void) {
  if (g_ctx) {
    SDL_GL_MakeCurrent(g_win, NULL);
    SDL_GL_DeleteContext(g_ctx);
    g_ctx = NULL;
  }
  if (g_win) {
    SDL_DestroyWindow(g_win);
    g_win = NULL;
  }
}

/* ------------------------------------------------------------- cursor ----- */
/* A small arrow drawn on top of the engine frame.  It shows up when the right
 * stick moves it and fades out after a few idle seconds; its position is never
 * forgotten, so a press still lands exactly where it was left. */
static float g_cx, g_cy;
static int g_cvis = 1;
static GLuint g_cprog, g_ctex, g_cbuf;
static int g_cattr_pos, g_cattr_uv, g_cuni_tex;

#define CUR_W 24
#define CUR_H 32

static const char *VS =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "varying vec2 vUV;\n"
    "void main(){ gl_FragColor = texture2D(uTex, vUV); }\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    debugPrintf("[cursor] shader: %s\n", log);
  }
  return s;
}

static void cursor_init(void) {
  unsigned char px[CUR_H][CUR_W][4];
  memset(px, 0, sizeof(px));
  /* Classic arrow: a filled triangle with a white body and a dark outline. */
  for (int y = 0; y < CUR_H; y++) {
    int span = (y * CUR_W) / CUR_H;
    if (y > CUR_H * 3 / 4)
      span = (CUR_W * 3) / 4 - (y - CUR_H * 3 / 4) * 2;
    if (span < 0)
      span = 0;
    for (int x = 0; x <= span && x < CUR_W; x++) {
      int edge = (x >= span - 1) || (y >= CUR_H - 2) || x == 0;
      px[y][x][0] = edge ? 20 : 255;
      px[y][x][1] = edge ? 20 : 255;
      px[y][x][2] = edge ? 20 : 255;
      px[y][x][3] = 255;
    }
  }

  glGenTextures(1, &g_ctex);
  gd_glBindTexture(GL_TEXTURE_2D, g_ctex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CUR_W, CUR_H, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, px);

  GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
  g_cprog = glCreateProgram();
  glAttachShader(g_cprog, vs);
  glAttachShader(g_cprog, fs);
  glLinkProgram(g_cprog);
  g_cattr_pos = glGetAttribLocation(g_cprog, "aPos");
  g_cattr_uv = glGetAttribLocation(g_cprog, "aUV");
  g_cuni_tex = glGetUniformLocation(g_cprog, "uTex");
  glGenBuffers(1, &g_cbuf);
  glDeleteShader(vs);
  glDeleteShader(fs);
}

void gd_cursor_set(float x, float y, int visible) {
  g_cx = x;
  g_cy = y;
  g_cvis = visible;
}

int gd_cursor_draw(void) {
  /* GD_CURSOR=0 turns the arrow off entirely (bring-up escape hatch). */
  static int mode = -1;
  if (mode < 0) {
    const char *e = getenv("GD_CURSOR");
    mode = (e && *e) ? atoi(e) : 1;
  }
  if (!mode || !g_cvis)
    return 0;

  /* engine-space position -> normalised device coords of the real fb */
  float sw = (float)CUR_W * 2.0f / (float)g_w;
  float sh = (float)CUR_H * 2.0f / (float)g_h;
  float nx = (g_cx / (float)gd_engine_w()) * 2.0f - 1.0f;
  float ny = 1.0f - (g_cy / (float)gd_engine_h()) * 2.0f;

  const float v[] = {
      nx,      ny,      0.0f, 0.0f, /**/
      nx + sw, ny,      1.0f, 0.0f, /**/
      nx,      ny - sh, 0.0f, 1.0f, /**/
      nx + sw, ny - sh, 1.0f, 1.0f,
  };

  /* The engine mirrors the GL state in C++ globals, so anything we leave
   * changed silently desyncs it.  The state comes from our own shadow (see
   * gl_state.c) -- no glGet*, which would stall Utgard every frame -- and goes
   * back through the same wrappers, so engine and driver stay in agreement.
   * We never call ccGLInvalidateStateCache(): it frees the kazmath matrix
   * stacks and takes the projection with them. */
  gd_gl_state s;
  gd_glstate_save(&s);

  /* built after the snapshot, so its own binds are undone by the restore */
  if (!g_cprog)
    cursor_init();

  gd_glBindFramebuffer(GL_FRAMEBUFFER, 0);
  gd_glDisable(GL_DEPTH_TEST);
  gd_glDisable(GL_CULL_FACE);
  gd_glDisable(GL_SCISSOR_TEST);
  gd_glEnable(GL_BLEND);
  gd_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gd_glstate_viewport_raw(0, 0, g_w, g_h);

  gd_glUseProgram(g_cprog);
  gd_glBindBuffer(GL_ARRAY_BUFFER, g_cbuf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
  gd_glEnableVertexAttribArray((GLuint)g_cattr_pos);
  gd_glVertexAttribPointer((GLuint)g_cattr_pos, 2, GL_FLOAT, GL_FALSE,
                           4 * sizeof(float), (void *)0);
  gd_glEnableVertexAttribArray((GLuint)g_cattr_uv);
  gd_glVertexAttribPointer((GLuint)g_cattr_uv, 2, GL_FLOAT, GL_FALSE,
                           4 * sizeof(float), (void *)(2 * sizeof(float)));
  gd_glActiveTexture(GL_TEXTURE0);
  gd_glBindTexture(GL_TEXTURE_2D, g_ctex);
  glUniform1i(g_cuni_tex, 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  gd_glstate_restore(&s);
  return 1;
}
