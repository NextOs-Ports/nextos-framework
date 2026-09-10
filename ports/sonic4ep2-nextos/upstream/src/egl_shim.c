#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>

#include "egl_shim.h"
#include "util.h"

static const char *sonic_env(const char *name) {
  const char *v = getenv(name);
  return (v && *v) ? v : NULL;
}

static int sonic_env_on(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
         strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

/* Resolucao DINAMICA (qualquer device): desktop mode do SDL com fallback
 * 1280x720. Exportada p/ imports.c (ANativeWindow_getWidth/Height — o que o
 * JOGO le) e android_shim.c (clamp do cursor). */
int sonic_screen_w = 1280, sonic_screen_h = 720;
#define SCREEN_WIDTH sonic_screen_w
#define SCREEN_HEIGHT sonic_screen_h

/* A engine (bionic) lê a stack-canary de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD).
 * Sob glibc esse offset colide com uma TLS var que o Mali/SDL escreve no
 * MakeCurrent/CreateContext -> a canary "muda" no meio da função -> stack smash
 * FALSO-POSITIVO. Salvamos/restauramos tpidr+0x28 ao redor das chamadas SDL_GL
 * p/ a engine ver o guard ESTÁVEL. */
/* ARM32: o __stack_chk_fail é no-op (imports.c) e a canary bionic vive em outro
 * slot TLS; não precisamos do save/restore aarch64 — chamadas SDL_GL diretas. */
static int gl_makecurrent(SDL_Window *w, SDL_GLContext c) {
  int (*f)(SDL_Window *, SDL_GLContext) = &SDL_GL_MakeCurrent;
  return f(w, c);
}
static SDL_GLContext gl_createcontext(SDL_Window *w) {
  SDL_GLContext (*f)(SDL_Window *) = &SDL_GL_CreateContext;
  return f(w);
}

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  int swapint_applied;
  int id;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;
static int frame_count = 0;
static int next_context_id = 1;

static _egl_context *current_context = NULL;
static _egl_context *last_context = NULL;
static int has_real_gl = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

/* Lê o GL_VERSION do contexto CURRENT e diz se é GLES (contém "ES"). Usado p/ REJEITAR
   um contexto DESKTOP-GL: no Mesa/Panfrost, pedir ES pode devolver "3.1 Mesa" (desktop) que
   cria OK mas não roda shader GLSL ES -> tela preta. Rejeitando, o loop tenta a próxima
   versão até pegar um "OpenGL ES ..." real. SONIC_ALLOW_DESKTOP_GL desliga a rejeição. */
static int ctx_is_gles(void) {
  const unsigned char *(*p)(unsigned int) =
      (const unsigned char *(*)(unsigned int))SDL_GL_GetProcAddress("glGetString");
  if (!p) return 1;                 /* sem como checar -> aceita (não piora nada) */
  const char *v = (const char *)p(0x1F02); /* GL_VERSION */
  if (!v) return 1;
  return (strstr(v, "ES") != NULL || strstr(v, "es") != NULL);
}

int egl_shim_create_window(void) {
  /* 🟢 FIX ROCKNIX/Mesa "sem video" (default ON; SONIC_NO_FORCE_GLES desliga):
     em devices Mesa/Panfrost (ROCKNIX, handhelds RK open-source) o SDL pode devolver
     um contexto OpenGL DESKTOP (GL_VERSION="3.x Mesa", GLSL "1.40") em vez de GLES ->
     os shaders GLSL ES do jogo nao compilam -> tela preta (so audio). Isto NAO e
     forcar SDL_VIDEODRIVER (regra #6) — e escolher a LIB GL (GLES via EGL vs desktop
     via GLX), que o jogo EXIGE. Lido no load da libGL (1o SDL_CreateWindow OPENGL).
     Inofensivo em devices que ja usam GLES real (Mali libmali/Utgard). */
  if (!sonic_env("SONIC_NO_FORCE_GLES")) {
    SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");   /* carrega libGLESv2 via EGL */
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");/* em X11, EGL em vez de GLX */
    fprintf(stderr, "egl_shim: FORCE_GLES on (SDL_OPENGL_ES_DRIVER=1, X11_FORCE_EGL=1) "
                    "-> driver GLES/EGL p/ Mesa/Panfrost\n");
  }
  /* 🔑 RESOLUÇÃO NATIVA 100% AUTOMÁTICA (TV 1080p, handheld 480p, .79 720p...).
     BUG antigo: SDL_GetDesktopDisplayMode era chamado ANTES do subsistema de vídeo
     existir -> falhava SILENCIOSO -> ficava o fallback 1280x720 (no painel 640x480
     do R36S = ZOOM gigante; no .79 720p coincidia e ninguém via). O CreateWindow
     auto-inicializa o vídeo, a query não. Agora: init explícito + cadeia de fontes
     (desktop mode -> current mode -> display bounds -> DRM sysfs do conector
     conectado -> fb0) e SEMPRE logado p/ diagnosticar device que não temos. */
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    fprintf(stderr, "egl_shim: SDL_InitSubSystem(VIDEO) falhou: %s\n", SDL_GetError());
  {
    SDL_DisplayMode dm;
    int got_w = 0, got_h = 0; const char *src = NULL;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
      got_w = dm.w; got_h = dm.h; src = "desktop-mode";
    } else {
      /* SDL não soube (headless/kmsdrm capenga): pergunta ao KERNEL. DRM: primeiro
         modo (= preferido) do primeiro conector CONECTADO. Depois fb0. */
      char path[128], stat[16], mode[64];
      for (int card = 0; card < 2 && !src; card++) {
        char glob_dir[64];
        snprintf(glob_dir, sizeof glob_dir, "/sys/class/drm");
        DIR *d = opendir(glob_dir);
        if (!d) break;
        struct dirent *e;
        while ((e = readdir(d)) && !src) {
          if (strncmp(e->d_name, "card", 4) != 0 || !strchr(e->d_name, '-')) continue;
          snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
          FILE *f = fopen(path, "r");
          if (!f) continue;
          stat[0] = 0; fgets(stat, sizeof stat, f); fclose(f);
          if (strncmp(stat, "connected", 9) != 0) continue;
          snprintf(path, sizeof path, "/sys/class/drm/%s/modes", e->d_name);
          f = fopen(path, "r");
          if (!f) continue;
          mode[0] = 0; fgets(mode, sizeof mode, f); fclose(f);
          int w, h;
          if (sscanf(mode, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            got_w = w; got_h = h; src = "drm-sysfs";
          }
        }
        closedir(d);
        break;
      }
      if (!src) {
        FILE *f = fopen("/sys/class/graphics/fb0/mode", "r");
        char m[64];
        if (f) {
          m[0] = 0; fgets(m, sizeof m, f); fclose(f);
          int w, h; /* formato "U:640x480p-0" */
          char *x = strchr(m, ':');
          if (x && sscanf(x + 1, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            got_w = w; got_h = h; src = "fb0-mode";
          }
        }
      }
    }
    if (src) {
      sonic_screen_w = got_w; sonic_screen_h = got_h;
      fprintf(stderr, "egl_shim: resolucao nativa %dx%d (fonte: %s)\n",
              got_w, got_h, src);
    } else {
      fprintf(stderr, "egl_shim: AVISO: nenhuma fonte de resolucao respondeu -> "
              "fallback %dx%d\n", sonic_screen_w, sonic_screen_h);
    }
  }
  { const char *e = sonic_env("SONIC_RES"); int w, h; /* override opcional */
    if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      sonic_screen_w = w; sonic_screen_h = h;
      debugPrintf("egl_shim: SONIC_RES override %dx%d\n", w, h);
    } }
  /* Criação RESILIENTE do contexto GL. O blob Mali proprietário (Utgard,
     EmuELEC .79) aceita ES2 + depth24 + stencil8 de primeira. Mas o
     mesa/panfrost (ROCKNIX e handhelds RK open-source) pode rejeitar essa
     combinação EXATA com EGL_BAD_MATCH no eglCreateContext -> SDL falha ->
     jogo fica SEM VÍDEO (só áudio; foi o sintoma relatado num device mesa).
     Tentamos várias combinações de (versão ES, depth, stencil) e usamos a 1ª
     que cria contexto. SEMPRE logado (não-gated) p/ diagnosticar devices que
     não temos. Regra #6: nada de driver hardcoded — só negociação dos
     atributos GL padrão do SDL; um contexto ES3 roda um engine ES2 sem
     problema (compatível pra trás). */
  /* Preferência de versão: ES2 PRIMEIRO (a engine libfox é ES2/GLSL-ES-1.00; é o que
     Crazy Taxi e Bully pedem e é o que dá GLES REAL no Mesa/Panfrost). Cai p/ ES3 depois
     (inofensivo em device ES3-real; ES2 roda lá sem problema). ⚠️ ES3-primeiro fazia o
     Mesa devolver um contexto DESKTOP-GL "3.1 Mesa" que passava como sucesso -> shaders
     GLSL ES não compilam = tela preta (Device A/ROCKNIX). SONIC_GLVER=2 força só ES2;
     =3 força só ES3; sem env = adaptativo ES2->ES3. */
  int vers[2], nver;
  { const char *gv = sonic_env("SONIC_GLVER");
    if (gv && gv[0] == '2')      { vers[0] = 2; nver = 1; }
    else if (gv && gv[0] == '3') { vers[0] = 3; nver = 1; }
    else                          { vers[0] = 2; vers[1] = 3; nver = 2; } }
  /* depth/stencil sao fixados na criacao da JANELA -> 1 janela por combo; as
     versoes ES sao testadas na MESMA janela (sem recriar) p/ nao estressar o
     fbdev Mali do .79 (no caso comum cria uma janela so). */
  static const struct { int depth, stencil; } dsc[] = { {24,8},{16,0},{0,0} };

  egl_window = NULL; egl_share_root = NULL;
  for (int d = 0; d < 3 && !egl_share_root; d++) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, dsc[d].depth);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, dsc[d].stencil);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, vers[0]);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    /* Janela FULLSCREEN_DESKTOP (borderless, sem modeset) — como o Bully. O exclusivo
       (SDL_WINDOW_FULLSCREEN) faz mode-set que pode dar EGL_BAD_MATCH em kmsdrm/wayland.
       SONIC_EXCL_FS=1 volta pro exclusivo (escape p/ o fbdev Mali .79 se precisar). */
    Uint32 fsflag = sonic_env("SONIC_EXCL_FS") ? SDL_WINDOW_FULLSCREEN
                                               : SDL_WINDOW_FULLSCREEN_DESKTOP;
    egl_window = SDL_CreateWindow(
        PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_OPENGL | fsflag);
    if (!egl_window) {
      fprintf(stderr, "egl_shim: depth%d stencil%d: SDL_CreateWindow FALHOU: %s\n",
              dsc[d].depth, dsc[d].stencil, SDL_GetError());
      continue;
    }
    int allow_desktop = sonic_env("SONIC_ALLOW_DESKTOP_GL") != 0;
    for (int v = 0; v < nver && !egl_share_root; v++) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, vers[v]);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      egl_share_root = gl_createcontext(egl_window);
      if (egl_share_root) {
        /* rejeita contexto DESKTOP-GL (não-ES) e tenta a próxima versão -> pega GLES real */
        if (!allow_desktop && !ctx_is_gles()) {
          fprintf(stderr, "egl_shim: ES%d deu contexto DESKTOP-GL (sem 'ES') -> REJEITADO, tenta proxima\n",
                  vers[v]);
          SDL_GL_DeleteContext(egl_share_root);
          egl_share_root = NULL;
          continue;
        }
        fprintf(stderr, "egl_shim: GL context OK -> ES%d depth%d stencil%d (window %dx%d)\n",
                vers[v], dsc[d].depth, dsc[d].stencil, SCREEN_WIDTH, SCREEN_HEIGHT);
        break;
      }
      fprintf(stderr, "egl_shim: try ES%d depth%d stencil%d: context FALHOU: %s\n",
              vers[v], dsc[d].depth, dsc[d].stencil, SDL_GetError());
    }
    if (!egl_share_root) { SDL_DestroyWindow(egl_window); egl_window = NULL; }
  }
  if (!egl_window || !egl_share_root) {
    fprintf(stderr, "egl_shim: NENHUMA combinacao GL criou contexto -> SEM VIDEO\n");
    return -1;
  }
  /* v3.8 diag de VIDEO: identidade GL real do device (o contexto recem-criado
     ja esta current). Crucial p/ devices que nao temos (ROCKNIX mesa/panfrost,
     handhelds RK): diz GPU/driver/versao GLES/GLSL e ajuda a explicar tela
     preta. Sempre visivel. */
  {
    /* glGetString resolvido em runtime (o binario nao linka libGLESv2; GL vem
       do device via dlsym). SDL_GL_GetProcAddress funciona com contexto current. */
    const unsigned char *(*p_glGetString)(unsigned int) =
        (const unsigned char *(*)(unsigned int))SDL_GL_GetProcAddress("glGetString");
    if (p_glGetString) {
      const char *ven = (const char *)p_glGetString(0x1F00); /* GL_VENDOR */
      const char *r   = (const char *)p_glGetString(0x1F01); /* GL_RENDERER */
      const char *v   = (const char *)p_glGetString(0x1F02); /* GL_VERSION */
      const char *s   = (const char *)p_glGetString(0x8B8C); /* GL_SHADING_LANGUAGE_VERSION */
      fprintf(stderr, "egl_shim: GL_VENDOR=%s\n", ven ? ven : "?");
      fprintf(stderr, "egl_shim: GL_RENDERER=%s\n", r ? r : "?");
      fprintf(stderr, "egl_shim: GL_VERSION=%s\n", v ? v : "?");
      fprintf(stderr, "egl_shim: GL_GLSL=%s\n", s ? s : "?");
      /* 🟢 Detecção: um contexto GLES reporta "OpenGL ES x.x". Se vier DESKTOP GL
         (ex.: "3.1 Mesa", sem "ES"), os shaders GLSL ES nao compilam -> tela preta.
         Avisa alto (o FORCE_GLES acima deve evitar; se ainda vier desktop, o device
         nao tem libGLESv2/EGL no caminho do SDL). */
      if (v && !strstr(v, "ES") && !strstr(v, "es")) {
        fprintf(stderr, "egl_shim: *** AVISO: contexto e OpenGL DESKTOP, nao GLES! "
                "(GL_VERSION sem 'ES') -> shaders GLSL ES nao compilam = TELA PRETA. "
                "Device Mesa precisa de libGLESv2+EGL no SDL. ***\n");
      }
    } else {
      fprintf(stderr, "egl_shim: glGetString indisponivel (sem identidade GL)\n");
    }
  }
  /* SONIC_SWAPINT no contexto novo (a engine pode nunca chamar
   * eglSwapInterval; default SDL=vsync 1 + limiter da engine = 30fps). */
  {
    const char *f = sonic_env("SONIC_SWAPINT");
    if (f) {
      SDL_GL_SetSwapInterval(atoi(f));
      debugPrintf("egl_shim: swap interval forçado=%d\n", atoi(f));
    }
  }

  /* 🔑 RESOLUÇÃO AUTOMÁTICA (autoridade final): o tamanho REAL da superfície que
     a GPU vai renderizar pode diferir do que pedimos (fullscreen-desktop usa o
     painel real; compositor pode dar outra coisa). Pegamos o drawable de verdade
     e é ELE que o jogo recebe em setScreenSize/fox.init -> zoom certo em QUALQUER
     tela, sem número fixo. (R36S 640x480 e Mali 1280x720 batem com o que já usavam,
     então não há regressão.) */
  {
    /* wayland negocia o tamanho DEPOIS do create (configure assíncrono do
       compositor): bombear eventos até o drawable estabilizar (~300ms máx). */
    int dw = 0, dh = 0;
    for (int i = 0; i < 30; i++) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) { /* drena: processa os configure do compositor */ }
      SDL_GL_GetDrawableSize(egl_window, &dw, &dh);
      if (dw > 0 && dh > 0 && dw == sonic_screen_w && dh == sonic_screen_h)
        break;                              /* já bate com o pedido: estável */
      if (i == 29) break;
      usleep(10 * 1000);
    }
    if (dw <= 0 || dh <= 0) { SDL_GetWindowSize(egl_window, &dw, &dh); }
    if (dw > 0 && dh > 0 && (dw != sonic_screen_w || dh != sonic_screen_h)) {
      fprintf(stderr, "egl_shim: drawable real %dx%d (ajustado de %dx%d)\n",
              dw, dh, sonic_screen_w, sonic_screen_h);
      sonic_screen_w = dw; sonic_screen_h = dh;
    }
  }

  gl_makecurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
  return 0;
}

/* Modelo GLSurfaceView (Sonic/fox): a engine NÃO cria contexto EGL próprio —
   assume o contexto já current (o GLSurfaceView faria isso). Ligar o share-root
   na thread chamadora (a do DrawFrame) p/ as chamadas GL do engine valerem. */
void egl_shim_bind_main(void) {
  if (egl_window && egl_share_root) {
    gl_makecurrent(egl_window, egl_share_root);
    has_real_gl = 1;
    debugPrintf("egl_shim: share-root current na thread principal (GLSurfaceView)\n");
  }
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  if (has_real_gl)
    return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (!egl_window || !ctx || !ctx->sdl_context)
    return 0;

  int ret = gl_makecurrent(egl_window, ctx->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    current_context = ctx;
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), ctx->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), ctx->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_share_root) {
    SDL_GL_DeleteContext(egl_share_root);
    egl_share_root = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  debugPrintf("egl_shim: eglChooseConfig()\n");
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)strdup("config");
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c)
    return EGL_NO_CONTEXT;

  pthread_mutex_lock(&egl_context_create_mutex);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  if (egl_share_root)
    gl_makecurrent(egl_window, egl_share_root);
  c->sdl_context = gl_createcontext(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  gl_makecurrent(egl_window, NULL);
  pthread_mutex_unlock(&egl_context_create_mutex);

  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    free(c);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d]\n",
              share_context, c, c->id);
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (egl_window) {
      gl_makecurrent(egl_window, NULL);
      /* debugPrintf("egl_shim: GL released [tid=%lx] reason=eglMakeCurrent(NULL)\n",
                    (unsigned long)pthread_self()); */
    }
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  int ret = gl_makecurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    /* SONIC_SWAPINT: intervalo é estado por-contexto; aplica 1x em cada */
    {
      static const char *si = (const char *)-1;
      if (si == (const char *)-1) si = sonic_env("SONIC_SWAPINT");
      if (si && !context->swapint_applied) {
        context->swapint_applied = 1;
        SDL_GL_SetSwapInterval(atoi(si));
      }
    }
    static int acq_log = 0;
    if (acq_log < 20 || mc % 500 == 0) {
      //debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] ACQUIRED [ctx_id=%d]\n",
      //            mc, is_window ? "WINDOW" : "PBUFFER",
      //            (unsigned long)pthread_self(), context->id);
      acq_log++;
    }
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED [ctx_id=%d]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (unsigned long)pthread_self(), context->id, SDL_GetError());
  }

  return EGL_TRUE;
}

/* screenshot sob demanda: `touch /dev/shm/sonic_shot` ->
 * RGBA cru do backbuffer em /dev/shm/sonic_shot.raw + .txt WxH (flip vertical
 * na conversao). Roda na thread de render, custo zero sem o trigger. */
static void sonic_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15) return;
  if (access("/dev/shm/sonic_shot", F_OK) != 0) return;
  unlink("/dev/shm/sonic_shot");
  GLint vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/sonic_shot.raw", "wb");
  if (o) { fwrite(buf, 1, (size_t)w * h * 4, o); fclose(o); }
  FILE *t = fopen("/dev/shm/sonic_shot.txt", "w");
  if (t) { fprintf(t, "%d %d\n", w, h); fclose(t); }
  free(buf);
  debugPrintf("[shot] %dx%d salvo\n", w, h);
}

static void perf_note_present(void) {
  static struct timespec last = {0, 0};
  static double sum = 0, mx = 0;
  static unsigned n = 0, s20 = 0, s40 = 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (last.tv_sec) {
    double ms = (now.tv_sec - last.tv_sec) * 1e3 +
                (now.tv_nsec - last.tv_nsec) / 1e6;
    sum += ms;
    n++;
    if (ms > mx) mx = ms;
    if (ms > 20) s20++;
    if (ms > 40) s40++;
    if (sum >= 5000) {
      int log_perf = sonic_env_on("SONIC_PERFLOG") ||
                     sonic_env_on("SONIC_VERBOSE_LOG");
      if (log_perf)
      fprintf(stderr, "[PERF] fps=%.1f avg=%.1fms max=%.0fms >20ms=%u >40ms=%u\n",
              n * 1000.0 / sum, sum / n, mx, s20, s40);
      sum = 0;
      n = 0;
      mx = 0;
      s20 = 0;
      s40 = 0;
    }
  }
  last = now;
}

/* Present do nosso loop (modelo GLSurfaceView: engine desenha em DrawFrame, NÓS
   damos o swap, como o GLSurfaceView faz após onDrawFrame). */
void egl_shim_present(void) {
  if (egl_window) {
    if (has_real_gl) sonic_maybe_screenshot();
    SDL_GL_SwapWindow(egl_window);
    perf_note_present();
  }
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    sonic_maybe_screenshot();
    SDL_GL_SwapWindow(egl_window);
    perf_note_present();
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      //debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
      //            fc, (unsigned long)pthread_self());
    }
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) {
    if (context->sdl_context)
      SDL_GL_DeleteContext(context->sdl_context);
    free(context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value) *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  debugPrintf("egl_shim: eglGetConfigAttrib(attr=0x%x)\n", attribute);
  if (!value) return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 8; break;
  case 0x3021: *value = 8; break;
  case 0x3022: *value = 8; break;
  case 0x3023: *value = 0; break;
  case 0x3025: *value = 24; break;
  case 0x3026: *value = 8; break;
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  /* Override GL: a engine resolve glGetString via procaddress; devolvemos NOSSA
   * versão (strings curtas) p/ evitar stack-smash com a lista de extensões do Mali. */
  extern void *sonic_gl_proc_override(const char *name);
  void *ov = sonic_gl_proc_override(procname);
  if (ov) { debugPrintf("egl_shim: proc override %s\n", procname); return ov; }

  void *ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case 0x3053: return "NextOS";      /* EGL_VENDOR */
  case 0x3054: return "1.4 NextOS";  /* EGL_VERSION */
  case 0x3055: return "";            /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";   /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  /* SONIC_SWAPINT força o intervalo (teste do double-pacing: engine dorme
   * ~16ms + vsync = 2 períodos = trava em 30fps; =0 deixa a engine ditar). */
  const char *f = sonic_env("SONIC_SWAPINT");
  if (f) interval = atoi(f);
  debugPrintf("egl_shim: SwapInterval(%d)%s\n", (int)interval, f ? " [forçado]" : "");
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  (void)readdraw;
  return (EGLSurface)"window";
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
