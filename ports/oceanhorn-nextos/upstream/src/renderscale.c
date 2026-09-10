/* renderscale.c — render interno em baixa resolução p/ reduzir fill-rate no Mali-450.
 *
 * Diagnóstico (s14): o mapa-múndi roda a ~30fps com só 41 draws/9k verts — não é
 * CPU/animação nem geometria, é FILL-RATE: ~41 quads grandes blended a 1280x720
 * (~1.1 Gpix/s) saturam o Utgard. O menu (7 draws) roda a 60fps. Lever = renderizar a
 * cena numa resolução MENOR e dar upscale p/ os 720p nativos do fb0.
 *
 * Como (GLES2, EGL real do Mali — surface fixa no fb0, sem Screen.SetResolution):
 *  - REDIRECIONA o framebuffer DEFAULT (fb 0 = a tela) p/ um FBO offscreen de baixa res
 *    (g_lo_fbo, ex. 640x360). Toda a renderização "de tela" do Unity cai nele.
 *  - ESCALA o glViewport enquanto a tela (lógico-0) está bound (só aí; os FBOs próprios
 *    do Unity — shadow/RT — passam intactos).
 *  - No eglSwapBuffers: bind o fb 0 REAL, desenha um quad fullscreen amostrando a textura
 *    de cor do FBO lo-res (upscale linear), e então faz o swap real.
 *
 * Gated por CUP_RENDERSCALE=N (divisor; 2 = metade = 640x360). default OFF.
 * Estado salvo/restaurado em volta do blit p/ não corromper o pipeline do Unity.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <dlfcn.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint;
typedef int GLsizei; typedef unsigned char GLboolean; typedef float GLfloat;
typedef char GLchar; typedef ptrdiff_t GLintptr;

#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_DEPTH_COMPONENT16      0x81A5
#define GL_TEXTURE_2D             0x0DE1
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908
#define GL_UNSIGNED_BYTE          0x1401
#define GL_LINEAR                 0x2601
#define GL_NEAREST                0x2600
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_ARRAY_BUFFER           0x8892
#define GL_ARRAY_BUFFER_BINDING   0x8894
#define GL_STATIC_DRAW            0x88E4
#define GL_FLOAT                  0x1406
#define GL_FALSE                  0
#define GL_TRUE                   1
#define GL_TRIANGLE_STRIP         0x0005
#define GL_TEXTURE0               0x84C0
#define GL_DEPTH_TEST             0x0B71
#define GL_CULL_FACE              0x0B44
#define GL_BLEND                  0x0BE2
#define GL_STENCIL_TEST           0x0B90
#define GL_SCISSOR_TEST           0x0C11
#define GL_COLOR_BUFFER_BIT       0x4000
#define GL_DEPTH_BUFFER_BIT       0x0100
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_RENDERBUFFER_BINDING   0x8CA7
#define GL_CURRENT_PROGRAM        0x8B8D
#define GL_ACTIVE_TEXTURE         0x84E0
#define GL_TEXTURE_BINDING_2D     0x8069
#define GL_VIEWPORT               0x0BA2
#define GL_COLOR_WRITEMASK        0x0C23
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED        0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE           0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE         0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE           0x8625
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED     0x886A
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_VERTEX_ATTRIB_ARRAY_POINTER        0x8645

/* ponteiros p/ as funções GLES2 reais (Mali), resolvidos via dlsym */
static struct {
  void (*GenFramebuffers)(GLsizei, GLuint*);
  void (*BindFramebuffer)(GLenum, GLuint);
  void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  GLenum (*CheckFramebufferStatus)(GLenum);
  void (*GenRenderbuffers)(GLsizei, GLuint*);
  void (*BindRenderbuffer)(GLenum, GLuint);
  void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
  void (*GenTextures)(GLsizei, GLuint*);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
  void (*GetIntegerv)(GLenum, GLint*);
  GLuint (*CreateShader)(GLenum);
  void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
  void (*CompileShader)(GLuint);
  void (*GetShaderiv)(GLuint, GLenum, GLint*);
  GLuint (*CreateProgram)(void);
  void (*AttachShader)(GLuint, GLuint);
  void (*BindAttribLocation)(GLuint, GLuint, const GLchar*);
  void (*LinkProgram)(GLuint);
  void (*GetProgramiv)(GLuint, GLenum, GLint*);
  void (*UseProgram)(GLuint);
  GLint (*GetAttribLocation)(GLuint, const GLchar*);
  GLint (*GetUniformLocation)(GLuint, const GLchar*);
  void (*GenBuffers)(GLsizei, GLuint*);
  void (*BindBuffer)(GLenum, GLuint);
  void (*BufferData)(GLenum, GLintptr, const void*, GLenum);
  void (*VertexAttribPointer)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
  void (*EnableVertexAttribArray)(GLuint);
  void (*DisableVertexAttribArray)(GLuint);
  void (*GetVertexAttribiv)(GLuint, GLenum, GLint*);
  void (*GetVertexAttribPointerv)(GLuint, GLenum, void**);
  void (*ActiveTexture)(GLenum);
  void (*Uniform1i)(GLint, GLint);
  void (*Disable)(GLenum);
  void (*Enable)(GLenum);
  void (*DrawArrays)(GLenum, GLint, GLsizei);
  void (*Clear)(GLenum);
  void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
  void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
  GLboolean (*IsEnabled)(GLenum);
} gl;

static int g_rs_div = 0;            /* divisor (0 = off) */
static int g_scr_w = 1280, g_scr_h = 720;
static int g_lo_w, g_lo_h;
static GLuint g_lo_fbo, g_lo_color, g_lo_depth;
static GLuint g_prog, g_vbo;
static GLint g_a_vertex, g_u_tex;
static int g_logical0 = 1;          /* a tela (fb 0 lógico) está bound? */
static int g_inited = 0;
static int g_in_blit = 0;           /* reentrância: não redirecionar durante o nosso blit */
static unsigned long g_n_bind0, g_n_bindN, g_n_vp, g_n_present;  /* diagnóstico */

static void *gsym(const char *n) { return dlsym(RTLD_DEFAULT, n); }

void rs_init(void) {
  const char *e = getenv("CUP_RENDERSCALE");
  if (!e) return;
  g_rs_div = atoi(e);
  if (g_rs_div < 2) g_rs_div = 2;
  /* Resolução REAL do painel: o port-env exporta TER_SCREEN_W/H medidos do
   * fb0/DRM (640x480 no R36S, 1280x720 no Mali-450). O default 1280x720 só
   * vale se nada foi medido; CUP_RS_W/H continuam como override manual. */
  if (getenv("TER_SCREEN_W") && atoi(getenv("TER_SCREEN_W")) > 0)
    g_scr_w = atoi(getenv("TER_SCREEN_W"));
  if (getenv("TER_SCREEN_H") && atoi(getenv("TER_SCREEN_H")) > 0)
    g_scr_h = atoi(getenv("TER_SCREEN_H"));
  if (getenv("CUP_RS_W")) g_scr_w = atoi(getenv("CUP_RS_W"));
  if (getenv("CUP_RS_H")) g_scr_h = atoi(getenv("CUP_RS_H"));
  g_lo_w = g_scr_w / g_rs_div;
  g_lo_h = g_scr_h / g_rs_div;
  fprintf(stderr, "[RS] render-scale 1/%d -> %dx%d (tela %dx%d)\n",
          g_rs_div, g_lo_w, g_lo_h, g_scr_w, g_scr_h);
}
int rs_enabled(void) { return g_rs_div >= 2; }
int rs_logical0(void) { return g_rs_div >= 2 && g_logical0 && !g_in_blit; }

/* ---- modo LO-REPORT (caminho ES3/KMSDRM) -------------------------------
 * No fbdev/GLES2 (Mali-450) a engine resolve gl* pela tabela wrappada e o
 * par redirect+escala-de-viewport funciona com a engine acreditando na
 * resolução CHEIA. No ES3/KMSDRM a Unity NÃO passa pelos wrappers de
 * viewport (medido: vp=0) e descobre o tamanho por fora — ficava meio
 * adaptada: cedo desenhava 640x480 num FBO de 320x240 (quadrado 1/4 na
 * tela). Neste modo a verdade se inverte: TODAS as superfícies de consulta
 * (ANativeWindow, DisplayMetrics, eglQuerySurface) respondem o tamanho
 * LO-RES desde o primeiro frame; a Unity desenha coerente no FBO lo e o
 * blit sobe para o painel real. Escala de viewport/scissor fica DESLIGADA
 * (a engine já emite tudo em lo). */
static int g_report_lo = 0;
void rs_set_report_lo(int on) {
  g_report_lo = (on && g_rs_div >= 2) ? 1 : 0;
  if (g_report_lo)
    fprintf(stderr, "[RS] modo LO-REPORT: engine enxerga %dx%d, painel %dx%d\n",
            g_lo_w, g_lo_h, g_scr_w, g_scr_h);
}
int rs_report_lo(void) { return g_report_lo; }
int rs_divisor(void) { return g_rs_div >= 2 ? g_rs_div : 1; }
/* Divide uma medida de tela quando o modo LO-REPORT está ativo. */
int rs_lo_adjust(int v) {
  return (g_report_lo && g_rs_div >= 2 && v > 0) ? v / g_rs_div : v;
}

static const char *VS =
  "attribute vec4 aVertex; varying vec2 vUV;"
  "void main(){ vUV=aVertex.zw; gl_Position=vec4(aVertex.xy,0.0,1.0); }";
static const char *FS =
  "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;"
  "void main(){ vec4 c=texture2D(uTex,vUV); gl_FragColor=vec4(c.rgb,1.0); }";

static GLuint mkshader(GLenum t, const char *src) {
  GLuint s = gl.CreateShader(t);
  gl.ShaderSource(s, 1, &src, 0);
  gl.CompileShader(s);
  GLint ok = 0; gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) fprintf(stderr, "[RS] shader %x falhou compilar\n", t);
  return s;
}

/* cria o FBO lo-res + programa de upscale (precisa de contexto GL current) */
static int rs_lazy_init(void) {
  if (g_inited) return g_lo_fbo != 0;
  g_inited = 1;
  /* resolve todas as funções */
  gl.GenFramebuffers = gsym("glGenFramebuffers");
  gl.BindFramebuffer = gsym("glBindFramebuffer");
  gl.FramebufferTexture2D = gsym("glFramebufferTexture2D");
  gl.FramebufferRenderbuffer = gsym("glFramebufferRenderbuffer");
  gl.CheckFramebufferStatus = gsym("glCheckFramebufferStatus");
  gl.GenRenderbuffers = gsym("glGenRenderbuffers");
  gl.BindRenderbuffer = gsym("glBindRenderbuffer");
  gl.RenderbufferStorage = gsym("glRenderbufferStorage");
  gl.GenTextures = gsym("glGenTextures");
  gl.BindTexture = gsym("glBindTexture");
  gl.TexImage2D = gsym("glTexImage2D");
  gl.TexParameteri = gsym("glTexParameteri");
  gl.Viewport = gsym("glViewport");
  gl.GetIntegerv = gsym("glGetIntegerv");
  gl.CreateShader = gsym("glCreateShader");
  gl.ShaderSource = gsym("glShaderSource");
  gl.CompileShader = gsym("glCompileShader");
  gl.GetShaderiv = gsym("glGetShaderiv");
  gl.CreateProgram = gsym("glCreateProgram");
  gl.AttachShader = gsym("glAttachShader");
  gl.BindAttribLocation = gsym("glBindAttribLocation");
  gl.LinkProgram = gsym("glLinkProgram");
  gl.GetProgramiv = gsym("glGetProgramiv");
  gl.UseProgram = gsym("glUseProgram");
  gl.GetAttribLocation = gsym("glGetAttribLocation");
  gl.GetUniformLocation = gsym("glGetUniformLocation");
  gl.GenBuffers = gsym("glGenBuffers");
  gl.BindBuffer = gsym("glBindBuffer");
  gl.BufferData = gsym("glBufferData");
  gl.VertexAttribPointer = gsym("glVertexAttribPointer");
  gl.EnableVertexAttribArray = gsym("glEnableVertexAttribArray");
  gl.DisableVertexAttribArray = gsym("glDisableVertexAttribArray");
  gl.GetVertexAttribiv = gsym("glGetVertexAttribiv");
  gl.GetVertexAttribPointerv = gsym("glGetVertexAttribPointerv");
  gl.ActiveTexture = gsym("glActiveTexture");
  gl.Uniform1i = gsym("glUniform1i");
  gl.Disable = gsym("glDisable");
  gl.Enable = gsym("glEnable");
  gl.DrawArrays = gsym("glDrawArrays");
  gl.Clear = gsym("glClear");
  gl.ColorMask = gsym("glColorMask");
  gl.IsEnabled = gsym("glIsEnabled");
  if (!gl.GenFramebuffers || !gl.CreateProgram || !gl.BindAttribLocation ||
      !gl.GetVertexAttribiv || !gl.GetVertexAttribPointerv ||
      !gl.IsEnabled || !gl.ColorMask) {
    fprintf(stderr, "[RS] dlsym GLES2 falhou — render-scale OFF\n");
    g_rs_div = 0; return 0;
  }
  GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0, prev_vbo = 0;
  gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
  gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
  gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
  gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  /* textura de cor */
  gl.GenTextures(1, &g_lo_color);
  gl.BindTexture(GL_TEXTURE_2D, g_lo_color);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_lo_w, g_lo_h, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
  /* Upscale por divisor inteiro = fator exato: NEAREST reconstrói o pixel
   * art SEM borrão (o LINEAR aqui foi a rejeição visual do render-scale na
   * fase 1). CUP_RS_FILTER=linear devolve o filtro antigo se algum painel
   * preferir o suavizado. */
  {
    const char *rf = getenv("CUP_RS_FILTER");
    GLint filt = (rf && strcmp(rf, "linear") == 0) ? GL_LINEAR : GL_NEAREST;
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
  }
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  /* depth renderbuffer */
  gl.GenRenderbuffers(1, &g_lo_depth);
  gl.BindRenderbuffer(GL_RENDERBUFFER, g_lo_depth);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, g_lo_w, g_lo_h);
  /* fbo */
  gl.GenFramebuffers(1, &g_lo_fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, g_lo_fbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_lo_color, 0);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_lo_depth);
  GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "[RS] FBO incompleto (0x%x) — render-scale OFF\n", st);
    g_rs_div = 0;
  }
  /* programa de upscale */
  GLuint vs = mkshader(GL_VERTEX_SHADER, VS), fs = mkshader(GL_FRAGMENT_SHADER, FS);
  g_prog = gl.CreateProgram();
  gl.AttachShader(g_prog, vs); gl.AttachShader(g_prog, fs);
  gl.BindAttribLocation(g_prog, 0, "aVertex");
  gl.LinkProgram(g_prog);
  GLint lk = 0; gl.GetProgramiv(g_prog, GL_LINK_STATUS, &lk);
  if (!lk) {
    fprintf(stderr, "[RS] link do upscale falhou — render-scale OFF\n");
    g_rs_div = 0;
  }
  g_a_vertex = gl.GetAttribLocation(g_prog, "aVertex");
  g_u_tex = gl.GetUniformLocation(g_prog, "uTex");
  /* quad fullscreen (x,y,u,v) triangle strip */
  static const GLfloat quad[] = {
    -1,-1, 0,0,   1,-1, 1,0,   -1,1, 0,1,   1,1, 1,1,
  };
  gl.GenBuffers(1, &g_vbo);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  /* restaura */
  gl.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
  gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
  gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
  fprintf(stderr, "[RS] init %s (fbo=%u color=%u prog=%u attr=%d uTex=%d)\n",
          g_rs_div >= 2 ? "OK" : "FALHOU", g_lo_fbo, g_lo_color,
          g_prog, g_a_vertex, g_u_tex);
  return g_rs_div >= 2;
}

/* hook de glBindFramebuffer: fb 0 (a tela) -> nosso FBO lo-res */
void rs_BindFramebuffer(GLenum target, GLuint fb) {
  if (!gl.BindFramebuffer) gl.BindFramebuffer = gsym("glBindFramebuffer");
  if (g_in_blit) { gl.BindFramebuffer(target, fb); return; }
  if (!g_inited) rs_lazy_init();
  if (g_rs_div >= 2 && fb == 0) {
    g_n_bind0++;
    g_logical0 = 1;
    gl.BindFramebuffer(target, g_lo_fbo);
    /* o Unity NÃO re-seta o viewport ao voltar p/ a tela (assume full-screen do
     * pass anterior, 1280x720) -> num FBO de 640x360 a cena sai cortada. Força o
     * viewport p/ o tamanho do FBO lo-res. (glViewport posterior do Unity, se houver,
     * passa por rs_Viewport e é escalado corretamente.) */
    if (!gl.Viewport) gl.Viewport = gsym("glViewport");
    gl.Viewport(0, 0, g_lo_w, g_lo_h);
  } else {
    g_n_bindN++;
    g_logical0 = 0;
    gl.BindFramebuffer(target, fb);
  }
}

/* hook de glViewport: escala enquanto a tela lógica está bound */
void rs_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (!gl.Viewport) gl.Viewport = gsym("glViewport");
  if (!g_in_blit && g_rs_div >= 2 && g_logical0 && !g_report_lo) {
    g_n_vp++;
    gl.Viewport(x / g_rs_div, y / g_rs_div, w / g_rs_div, h / g_rs_div);
  } else {
    gl.Viewport(x, y, w, h);
  }
}

/* hook de glScissor: escala enquanto a tela lógica está bound (UI/masking) */
void rs_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (!gl.Scissor) gl.Scissor = gsym("glScissor");
  if (!g_in_blit && g_rs_div >= 2 && g_logical0 && !g_report_lo) {
    gl.Scissor(x / g_rs_div, y / g_rs_div, w / g_rs_div, h / g_rs_div);
  } else {
    gl.Scissor(x, y, w, h);
  }
}

typedef struct {
  GLint enabled, size, type, normalized, stride, buffer;
  void *pointer;
} rs_attrib_state;

/* chamado ANTES do eglSwapBuffers real: upscale do FBO lo-res p/ a tela real */
void rs_present(void) {
  if (g_rs_div < 2 || !g_inited || !g_lo_fbo) return;
  if (++g_n_present % 120 == 1)
    fprintf(stderr, "[RS] present#%lu bind0=%lu bindN=%lu vp=%lu\n",
            g_n_present, g_n_bind0, g_n_bindN, g_n_vp);
  g_in_blit = 1;

  /*
   * Salva todo estado tocado pelo passe. O código antigo restaurava só
   * program/VBO/textura/viewport, mas deixava blend/depth/cull/scissor
   * desabilitados, sobrescrevia os ponteiros dos vertex attributes e ainda
   * consultava GL_ACTIVE_TEXTURE com o enum incorreto (0x84e7). A cache de
   * estado da Unity então acreditava que o pipeline anterior seguia ativo:
   * o primeiro splash podia aparecer e os quadros seguintes ficavam pretos.
   */
  GLint pv[4], pprog = 0, pvbo = 0, ptex0 = 0, pact = GL_TEXTURE0;
  GLint pfbo = 0;
  GLint color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  const GLenum caps[] = {
    GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST
  };
  GLboolean cap_enabled[sizeof caps / sizeof caps[0]];
  rs_attrib_state attr = {
    .enabled = 0, .size = 4, .type = GL_FLOAT, .normalized = 0,
    .stride = 0, .buffer = 0, .pointer = NULL
  };
  int logical_before = g_logical0;

  gl.GetIntegerv(GL_VIEWPORT, pv);
  if (g_n_present % 120 == 1)
    fprintf(stderr, "[RS] viewport no present = %d,%d %dx%d (lo=%dx%d)\n",
            pv[0], pv[1], pv[2], pv[3], g_lo_w, g_lo_h);
  gl.GetIntegerv(GL_CURRENT_PROGRAM, &pprog);
  gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &pvbo);
  gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &pfbo);
  gl.GetIntegerv(GL_ACTIVE_TEXTURE, &pact);
  gl.GetIntegerv(GL_COLOR_WRITEMASK, color_mask);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &ptex0);
  for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++)
    cap_enabled[i] = gl.IsEnabled(caps[i]);
  if (g_a_vertex >= 0) {
    GLuint index = (GLuint)g_a_vertex;
    gl.GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr.enabled);
    gl.GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attr.size);
    gl.GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attr.type);
    gl.GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                         &attr.normalized);
    gl.GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attr.stride);
    gl.GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                         &attr.buffer);
    gl.GetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                               &attr.pointer);
  }

  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);             /* FBO 0 REAL */
  gl.Viewport(0, 0, g_scr_w, g_scr_h);
  /*
   * O compositor OSD do Amlogic mistura o fb0 pelo alpha de cada pixel. A Unity
   * pode deixar o canal A mascarado ao terminar a cena; nesse caso o quad copia
   * RGB correto, mas o scanout continua com A=0 e aparece totalmente preto.
   * Force RGBA durante o blit e restaure a máscara da engine em seguida.
   */
  gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++)
    gl.Disable(caps[i]);
  if (getenv("CUP_RS_DBGCLEAR")) {  /* teste: magenta na tela real antes do quad */
    if (!gl.ClearColor) gl.ClearColor = gsym("glClearColor");
    gl.ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
  }
  gl.UseProgram(g_prog);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.EnableVertexAttribArray((GLuint)g_a_vertex);
  gl.VertexAttribPointer((GLuint)g_a_vertex, 4, GL_FLOAT, GL_FALSE, 16,
                         (void*)0);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, g_lo_color);
  gl.Uniform1i(g_u_tex, 0);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

#if HC_DEV_DIAGNOSTICS
  /* Diag-only: captura a TELA REAL logo após o blit (fb 0 de verdade bound),
   * disparada por /tmp/rsshot. É a única forma de fotografar o upscale — o
   * shot normal lê o FBO lo-res por causa do redirect de bind. */
  {
    if (access("/tmp/rsshot", 0) == 0) {
      unlink("/tmp/rsshot");
      static void (*p_rp)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
      if (!p_rp) p_rp = gsym("glReadPixels");
      unsigned char *buf = malloc((size_t)g_scr_w * g_scr_h * 4);
      if (p_rp && buf) {
        p_rp(0, 0, g_scr_w, g_scr_h, 0x1908 /*RGBA*/, 0x1401, buf);
        FILE *fo = fopen("rs-shot.ppm", "wb");
        if (fo) {
          fprintf(fo, "P6\n%d %d\n255\n", g_scr_w, g_scr_h);
          for (int y = g_scr_h - 1; y >= 0; y--)
            for (int x = 0; x < g_scr_w; x++)
              fwrite(buf + ((size_t)y * g_scr_w + x) * 4, 1, 3, fo);
          fclose(fo);
          fprintf(stderr, "[RS] rs-shot.ppm %dx%d gravado (tela real)\n",
                  g_scr_w, g_scr_h);
        }
      }
      free(buf);
    }
  }
#endif

  /* Restaura o atributo antes do ARRAY_BUFFER global, pois o binding faz
     parte do próprio glVertexAttribPointer. */
  if (g_a_vertex >= 0) {
    GLuint index = (GLuint)g_a_vertex;
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)attr.buffer);
    if (attr.size >= 1 && attr.size <= 4)
      gl.VertexAttribPointer(index, attr.size, (GLenum)attr.type,
                             (GLboolean)attr.normalized, attr.stride,
                             attr.pointer);
    if (attr.enabled)
      gl.EnableVertexAttribArray(index);
    else
      gl.DisableVertexAttribArray(index);
  }
  gl.BindTexture(GL_TEXTURE_2D, (GLuint)ptex0);
  gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)pvbo);
  gl.UseProgram((GLuint)pprog);
  gl.ActiveTexture((GLuint)pact);
  gl.ColorMask((GLboolean)color_mask[0], (GLboolean)color_mask[1],
               (GLboolean)color_mask[2], (GLboolean)color_mask[3]);
  for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) {
    if (cap_enabled[i]) gl.Enable(caps[i]); else gl.Disable(caps[i]);
  }
  gl.Viewport(pv[0], pv[1], pv[2], pv[3]);
  gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)pfbo);
  g_logical0 = logical_before;
  g_in_blit = 0;
}
