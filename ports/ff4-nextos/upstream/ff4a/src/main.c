/*
 * main.c -- FF4 The After Years so-loader (Mali-450, arm64, GLES1).
 *
 * Estratégia (Alternativa B): carregamos o libff4a.so direto (o libjniproxy só
 * fazia dlopen+dlsym), fornecemos contexto SDL2/GLES1 + JNIEnv falso, e dirigimos
 * o loop:  JNI_OnLoad -> setViewport -> resume -> loop render + SwapWindow.
 * A engine inicializa GL/OBB dentro do 1º resume/render e pega AssetManager/paths
 * via upcalls no JNIEnv (logadas p/ recon).
 *
 * Env:
 *   FF4A_DATA    diretório de dados (default ./data)
 *   FF4A_FRAMES  nº de frames a rodar (default 0 = infinito)
 *   FF4A_W/FF4A_H  força tamanho da janela
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nxgl_frame_proof_adapter.h"
#include "nxgl_provider_discovery_adapter.h"
#include "nxgl_gles1.h"
#include "vkbd.h"

#include "crash.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "obb_data.h"
#include "so_util.h"
#include "util.h"

#define SO_NAME "./libff4a.so"
#define MODULE_ARENA (96 * 1024 * 1024)

/* GLES1 fixed-function p/ desenhar o cursor. Os prototipos vem de
 * nxgl_gles1.h: nao ha' bind direto na libGLESv1_CM porque esse SONAME nao
 * existe em toda CFW (dArkOS/ArkOS no Mali G31 so' tem libmali.so). */

/* Saida terminal (receita Chrono/AoZ, provada em Mali).  Depois da parte
 * segura (pause -> save do jogo), NAO se desmonta contexto GL/janela: no Mali
 * o teardown do driver trava DENTRO DO KERNEL, o processo fica vivo ignorando
 * ate' SIGKILL e o frontend nunca volta -- e' isso que o jogador ve' como
 * "SELECT+START nao matou o jogo e travou o device".  Uma falha no desmonte
 * da engine depois do save nao e' crash, e uma thread de driver presa nao
 * pode ficar dona do display: falha e watchdog convergem no _exit(0). */
static void ff4a_shutdown_terminal(int sig) {
  const char *m = (sig == SIGALRM)
      ? "[ff4a] teardown deadline; saindo\n"
      : "[ff4a] falha no teardown apos o save; saindo limpo\n";
  ssize_t ignored = write(2, m, strlen(m));
  (void)ignored;
  _exit(0);
}

static void ff4a_install_shutdown_guards(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = ff4a_shutdown_terminal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGALRM, &sa, NULL);
  /* O watchdog entra FOLGADO porque a parte segura (o save) ainda vai rodar:
   * um alarme curto aqui cortaria a gravacao no meio num cartao lento, o que
   * seria pior do que o travamento que este bloco conserta. Depois que o
   * pause() volta, ff4a_shutdown_deadline() aperta para cinco segundos. */
  alarm(20);
}

/* Save concluido: dali em diante nada mais e' essencial, entao o frontend
 * nunca espera mais que isso pelo desmonte da engine. */
static void ff4a_shutdown_deadline(void) { alarm(5); }

static float g_cursor_x = 0.5f, g_cursor_y = 0.5f;
/* Padrao da casa: MEXEU no analogico direito, a seta aparece; PAROU, some
 * em 3s. Antes ela ficava PERMANENTE na tela enquanto houvesse controle. */
static int g_cursor_pressed = 0;
static unsigned g_cursor_show_until = 0;  /* ms (SDL_GetTicks) */
#define CURSOR_HOLD_MS 3000

/* Cursor no padrao da casa, portado do ff4 (22/08/2026): seta com sombra,
 * contorno e realce ao clicar, escalada pela altura da janela, e com TODO o
 * estado GL salvo/restaurado -- os menus reaproveitam ponteiros entre quadros
 * e uma seta desleixada some com a UI no quadro seguinte. A antiga era um
 * triangulo chapado que ficava PERMANENTE na tela. */
static void gl_restore_cap(GLenum cap, GLboolean enabled) {
    if (enabled) glEnable(cap);
    else glDisable(cap);
}

static void gl_restore_client_cap(GLenum cap, GLboolean enabled) {
    if (enabled) glEnableClientState(cap);
    else glDisableClientState(cap);
}

static void cursor_draw(int win_w, int win_h, float px, float py, int pressed) {
    float scale = win_h / 720.0f;
    if (scale < 0.85f) scale = 0.85f;
    if (scale > 1.80f) scale = 1.80f;
    static const GLfloat base_fill[] = {
        0, 0,   0, 25,  23, 17,          // cabeca da seta
        7, 18, 12, 31,  18, 28,          // haste, triangulo 1
        7, 18, 18, 28,  13, 16           // haste, triangulo 2
    };
    static const GLfloat base_outline[] = {
        0, 0,  0, 25,  7, 18,  12, 31,  18, 28,  13, 16,  23, 17
    };
    GLfloat fill[sizeof base_fill / sizeof base_fill[0]];
    GLfloat shadow[sizeof base_fill / sizeof base_fill[0]];
    GLfloat outline[sizeof base_outline / sizeof base_outline[0]];
    for (size_t i = 0; i < sizeof fill / sizeof fill[0]; i += 2) {
        fill[i] = px + base_fill[i] * scale;
        fill[i + 1] = py + base_fill[i + 1] * scale;
        shadow[i] = fill[i] + 2.5f * scale;
        shadow[i + 1] = fill[i + 1] + 3.0f * scale;
    }
    for (size_t i = 0; i < sizeof outline / sizeof outline[0]; i += 2) {
        outline[i] = px + base_outline[i] * scale;
        outline[i + 1] = py + base_outline[i + 1] * scale;
    }

    // O overlay roda depois do render nativo, mas o contexto e' o mesmo. Salvar
    // tudo que tocamos e' obrigatorio: menus do FF4 reutilizam estado/pointers
    // entre quadros e uma seta que deixa vertex pointer, scissor ou blend para
    // tras faz a UI desaparecer no quadro seguinte.
    GLint old_matrix_mode = GL_MODELVIEW, old_viewport[4];
    GLint old_blend_src = GL_ONE, old_blend_dst = GL_ZERO;
    GLint old_array_buffer = 0, old_vertex_size = 4;
    GLint old_vertex_type = GL_FLOAT, old_vertex_stride = 0;
    GLfloat old_color[4], old_line_width = 1.0f;
    GLvoid *old_vertex_pointer = NULL;
    GLboolean old_depth, old_texture, old_lighting, old_cull, old_fog;
    GLboolean old_alpha, old_scissor, old_blend;
    GLboolean old_vertex, old_texcoord, old_color_array, old_normal;
    glGetIntegerv(GL_MATRIX_MODE, &old_matrix_mode);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetIntegerv(GL_BLEND_SRC, &old_blend_src);
    glGetIntegerv(GL_BLEND_DST, &old_blend_dst);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &old_vertex_size);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &old_vertex_type);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &old_vertex_stride);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &old_vertex_pointer);
    glGetFloatv(GL_CURRENT_COLOR, old_color);
    glGetFloatv(GL_LINE_WIDTH, &old_line_width);
    glGetBooleanv(GL_DEPTH_TEST, &old_depth);
    glGetBooleanv(GL_TEXTURE_2D, &old_texture);
    glGetBooleanv(GL_LIGHTING, &old_lighting);
    glGetBooleanv(GL_CULL_FACE, &old_cull);
    glGetBooleanv(GL_FOG, &old_fog);
    glGetBooleanv(GL_ALPHA_TEST, &old_alpha);
    glGetBooleanv(GL_SCISSOR_TEST, &old_scissor);
    glGetBooleanv(GL_BLEND, &old_blend);
    glGetBooleanv(GL_VERTEX_ARRAY, &old_vertex);
    glGetBooleanv(GL_TEXTURE_COORD_ARRAY, &old_texcoord);
    glGetBooleanv(GL_COLOR_ARRAY, &old_color_array);
    glGetBooleanv(GL_NORMAL_ARRAY, &old_normal);

    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrthof(0, (GLfloat)win_w, (GLfloat)win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE); glDisable(GL_FOG); glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(2, GL_FLOAT, 0, shadow);
    glColor4f(0.0f, 0.0f, 0.0f, 0.48f);
    glDrawArrays(GL_TRIANGLES, 0, 9);
    glVertexPointer(2, GL_FLOAT, 0, fill);
    if (pressed) glColor4f(1.0f, 0.78f, 0.16f, 1.0f);
    else         glColor4f(0.96f, 0.98f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 9);
    glVertexPointer(2, GL_FLOAT, 0, outline);
    glLineWidth(2.0f * scale);
    glColor4f(0.05f, 0.08f, 0.16f, 1.0f);
    glDrawArrays(GL_LINE_LOOP, 0, 7);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_array_buffer);
    glVertexPointer(old_vertex_size, (GLenum)old_vertex_type, old_vertex_stride,
                    old_vertex_pointer);
    gl_restore_client_cap(GL_VERTEX_ARRAY, old_vertex);
    gl_restore_client_cap(GL_TEXTURE_COORD_ARRAY, old_texcoord);
    gl_restore_client_cap(GL_COLOR_ARRAY, old_color_array);
    gl_restore_client_cap(GL_NORMAL_ARRAY, old_normal);
    glBlendFunc((GLenum)old_blend_src, (GLenum)old_blend_dst);
    glLineWidth(old_line_width);
    glColor4f(old_color[0], old_color[1], old_color[2], old_color[3]);
    gl_restore_cap(GL_DEPTH_TEST, old_depth);
    gl_restore_cap(GL_TEXTURE_2D, old_texture);
    gl_restore_cap(GL_LIGHTING, old_lighting);
    gl_restore_cap(GL_CULL_FACE, old_cull);
    gl_restore_cap(GL_FOG, old_fog);
    gl_restore_cap(GL_ALPHA_TEST, old_alpha);
    gl_restore_cap(GL_SCISSOR_TEST, old_scissor);
    gl_restore_cap(GL_BLEND, old_blend);
    glMatrixMode((GLenum)old_matrix_mode);
}

/* compat: o ff4a chamava draw_cursor(cx,cy) com coordenadas normalizadas */
static void draw_cursor(float cx, float cy) {
  int vp[4] = {0, 0, 1280, 720};
  glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
  int w = vp[2] > 0 ? vp[2] : 1280, h = vp[3] > 0 ? vp[3] : 720;
  cursor_draw(w, h, cx * (float)w, cy * (float)h, g_cursor_pressed);
}

/* entry-points da engine (extern C, plain names) */
typedef int (*fn_JNI_OnLoad)(void *vm, void *reserved);
typedef void (*fn_render)(void *env, void *obj);
typedef void (*fn_resume)(void *env, void *obj);
typedef void (*fn_pause)(void *env, void *obj);
typedef void (*fn_release)(void *env, void *obj);
typedef void (*fn_setViewport)(void *env, void *obj, int w, int h);
/* touch(action, count, x0,y0, x1,y1) — coords normalizadas [0,1] */
typedef void (*fn_touch)(void *env, void *obj, int action, int count, float x0,
                         float y0, float x1, float y1);

static SDL_Window *g_repair_window;
static SDL_GLContext g_repair_context;

/* Teardown para o reparo de provedor: o framework recusa o re-exec sem a
 * atestacao de que a pilha grafica que falhou foi realmente fechada.
 *
 * Isto NAO contradiz a saida terminal acima. Sao momentos opostos: la' o jogo
 * acabou e o driver estava SAUDAVEL, e desmonta-lo trava o Mali no kernel;
 * aqui o contexto nunca funcionou -- ou nem chegou a existir -- e o processo
 * vai ser substituido por execv em seguida. Fechar antes de trocar de processo
 * e' condicao do contrato do re-exec. */
static void ff4a_video_teardown(void) {
  if (g_repair_context) { SDL_GL_DeleteContext(g_repair_context); g_repair_context = NULL; }
  if (g_repair_window) { SDL_DestroyWindow(g_repair_window); g_repair_window = NULL; }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  log_open("ff4a.log");
  crash_init();
  debugPrintf("=== FF4 so-loader boot ===\n");

  const char *data = getenv("FF4A_DATA");
  if (!data) data = "./data";
  long frames = getenv("FF4A_FRAMES") ? atol(getenv("FF4A_FRAMES")) : 0;

  /* ---- SDL2 + contexto GLES1 (config Mali-450: RGB565/depth16, ES major 1) ---- */
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0)
    fatal_error("SDL_Init: %s", SDL_GetError());

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  int w = getenv("FF4A_W") ? atoi(getenv("FF4A_W")) : 0;
  int h = getenv("FF4A_H") ? atoi(getenv("FF4A_H")) : 0;
  if (w <= 0 || h <= 0) {
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
    else { w = 1280; h = 720; }
  }
  /* Recibo do ambiente grafico HERDADO: no dArkOSRE o unit do frontend
   * exporta SDL_VIDEO_EGL_DRIVER e essa unica variavel separa jogo renderizado
   * de tela preta com som. Sem esta linha, o log nao tinha como contar. */
  debugPrintf("VIDEO ENV: SDL_VIDEODRIVER=%s SDL_VIDEO_EGL_DRIVER=%s "
              "SDL_VIDEO_GL_DRIVER=%s reparo-aplicado=%s\n",
              getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "-",
              getenv("SDL_VIDEO_EGL_DRIVER") ? getenv("SDL_VIDEO_EGL_DRIVER") : "-",
              getenv("SDL_VIDEO_GL_DRIVER") ? getenv("SDL_VIDEO_GL_DRIVER") : "-",
              getenv("NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED") ? "sim" : "nao");
  debugPrintf("janela %dx%d\n", w, h);

  SDL_Window *win = NULL;
  int window_attempt;
  for (window_attempt = 0; window_attempt < 2 && win == NULL;
       window_attempt++) {
    win = SDL_CreateWindow("Final Fantasy IV", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, w, h,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (win == NULL && window_attempt == 0) {
      /* 0.2.12: se este processo ja' nasceu de um re-exec com par de provedor
       * aplicado e a TENTATIVA REAL falhou, desamarra o par e repete pela
       * pilha normal da firmware -- o erro final volta a ser o verdadeiro. */
      nxgl_provider_repair_receipt rollback_receipt;
      debugPrintf("SDL_CreateWindow: %s\n", SDL_GetError());
      if (nxgl_provider_precontext_rollback(&rollback_receipt)) {
        debugPrintf("%s\n", rollback_receipt.text);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
          break;
        }
        continue;
      }
    }
  }
  if (!win) {
    debugPrintf("SDL_CreateWindow: %s\n", SDL_GetError());
    {
      /* A janela nem chegou a existir: nao ha renderer para medir. E' o outro
       * sintoma do provedor cruzado, e o framework tem um plano proprio para
       * ele. Sem reparo autorizado, o erro segue fatal como antes. */
      static const char *const k_gles[] = {"glOrthof", "glDrawArrays",
                                           "glTexImage2D", "glClear"};
      nxgl_provider_repair_options repair;
      nxgl_provider_repair_receipt repair_receipt;

      nxgl_provider_repair_options_init(&repair);
      repair.video_backend = SDL_GetCurrentVideoDriver();
      repair.required_gles_symbols = k_gles;
      repair.required_gles_symbol_count = sizeof k_gles / sizeof k_gles[0];
      repair.teardown = ff4a_video_teardown;
      repair.argv = argv;
      nxgl_provider_repair_precontext(&repair,
                                      NXGL_OPEN_STAGE_V2_WINDOW_CREATE,
                                      NXGL_OPEN_REASON_V2_WINDOW_FAILED,
                                      &repair_receipt);
      debugPrintf("%s\n", repair_receipt.text);
    }
    fatal_error("SDL_CreateWindow: %s", SDL_GetError());
  }
  SDL_GLContext glc = SDL_GL_CreateContext(win);
  if (!glc)
    fatal_error("SDL_GL_CreateContext (GLES1): %s", SDL_GetError());
  SDL_GL_MakeCurrent(win, glc);
  if (SDL_GL_SetSwapInterval(1) != 0)
    SDL_GL_SetSwapInterval(0);

  /* GLES1 e' resolvido AGORA, com o contexto ja' corrente: o binario nao
   * declara DT_NEEDED libGLESv1_CM.so porque esse SONAME nao existe em toda
   * CFW (dArkOS/ArkOS no Mali G31 so' tem o blob libmali.so). Ver
   * framework/nxgl/include/nxgl_gles1.h. */
  {
    nxgl_gles1_receipt gles1;
    /* 0.2.14: a fonte primaria e' o contexto que a SDL acabou de criar -- no
     * ROCKNIX o dlopen por nome achava o blob kbase ORFAO (libmali.so) e o
     * jogo virava no-op (tela preta com som). O resolvedor prova cada
     * candidato com glGetString antes de aceitar. */
    nxgl_gles1_set_primary_resolver(
        (nxgl_gles1_resolver_fn)SDL_GL_GetProcAddress);
    if (nxgl_gles1_init(&gles1) != 0)
      fatal_error("%s", gles1.text);
    debugPrintf("%s\n", gles1.text);
  }

  /* Prova de imagem medida por dentro do contexto: um port que nao desenha e'
   * indistinguivel de um saudavel em todo sinal que o launcher enxerga. O
   * resolvedor do nxgl entra como fonte de simbolos porque, num port
   * so-loader, o glReadPixels real nao existe em mais lugar nenhum. */
  nxgl_frame_proof_set_resolver(nxgl_gles1_lookup);
  nxgl_frame_proof_launch_receipt();

  /* ---- controle nativo (SDL GameController via gamecontrollerdb) ---- */
  SDL_GameController *gc = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      gc = SDL_GameControllerOpen(i);
      if (gc) {
        debugPrintf("pad: %s\n", SDL_GameControllerName(gc));
        break;
      }
    }
  }
  if (!gc)
    debugPrintf("nenhum GameController (usando teclado)\n");

  const char *(*p_glGetString)(unsigned) =
      (void *)SDL_GL_GetProcAddress("glGetString");
  const char *gl_renderer = p_glGetString ? p_glGetString(0x1F01) : NULL;
  if (p_glGetString) {
    debugPrintf("GL_VENDOR   = %s\n", p_glGetString(0x1F00));
    debugPrintf("GL_RENDERER = %s\n", gl_renderer);
    debugPrintf("GL_VERSION  = %s\n", p_glGetString(0x1F02));
  }

  /* Renderer vazio aqui e' a assinatura do SONAME cruzado: contexto que aceita
   * tudo e nao desenha nada. Quem decide e' o framework, e ele nao faz nada
   * quando o renderer esta' saudavel. */
  {
    static const char *const k_gles[] = {"glOrthof", "glDrawArrays",
                                         "glTexImage2D", "glClear"};
    nxgl_provider_repair_options repair;
    nxgl_provider_repair_receipt repair_receipt;
    int pw = 0, ph = 0;

    /* O guardiao mede o renderer PELO CAMINHO DA ENGINE (o resolvedor do
     * nxgl), nao pelo da SDL: no ROCKNIX a SDL respondia "Panfrost" saudavel
     * enquanto a engine desenhava por um provedor morto, e o reparo nunca
     * disparava. */
    const char *engine_renderer =
        nxgl_gles1_pfn_glGetString != NULL
            ? (const char *)nxgl_gles1_pfn_glGetString(0x1F01)
            : NULL;
    SDL_GL_GetDrawableSize(win, &pw, &ph);
    g_repair_window = win;
    g_repair_context = glc;
    nxgl_provider_repair_options_init(&repair);
    repair.renderer = engine_renderer;
    repair.video_backend = SDL_GetCurrentVideoDriver();
    repair.window_opened = 1;
    repair.context_current = 1;
    repair.drawable_positive = (pw > 0 && ph > 0);
    repair.required_gles_symbols = k_gles;
    repair.required_gles_symbol_count = sizeof k_gles / sizeof k_gles[0];
    repair.teardown = ff4a_video_teardown;
    repair.argv = argv;
    nxgl_provider_repair_if_renderer_broken(&repair, &repair_receipt);
    debugPrintf("%s\n", repair_receipt.text);

    /* Recibo VIDEO completo: sem isto a linha final saia window=? driver=?
     * renderer=? -- inutil como primeira leitura de um relato de campo. */
    nxgl_frame_proof_set_video_context(
        pw, ph, SDL_GetCurrentVideoDriver(), engine_renderer,
        nxgl_gles1_pfn_glGetString != NULL
            ? (const char *)nxgl_gles1_pfn_glGetString(0x1F02)
            : NULL);
  }

  /* clear inicial p/ provar o present GLES1 arm64 antes de plugar a engine */
  void (*p_glClearColor)(float, float, float, float) =
      (void *)SDL_GL_GetProcAddress("glClearColor");
  void (*p_glClear)(unsigned) = (void *)SDL_GL_GetProcAddress("glClear");
  for (int i = 0; i < 3; i++) {
    if (p_glClearColor) p_glClearColor(i == 0, i == 1, i == 2, 1.0f);
    if (p_glClear) p_glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    SDL_GL_SwapWindow(win);
    SDL_Delay(120);
  }
  debugPrintf("present GLES1 OK (clear animado)\n");

  /* ---- carrega o libff4a ---- */
  void *arena = mmap(NULL, MODULE_ARENA, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (arena == MAP_FAILED)
    fatal_error("mmap arena falhou");

  if (so_load(SO_NAME, arena, MODULE_ARENA) != 0)
    fatal_error("so_load(%s) falhou", SO_NAME);
  so_relocate();
  int missing = so_resolve(ff4a_imports, ff4a_imports_count);
  debugPrintf("so_resolve: %d imports faltando\n", missing);
  so_record_phdr("libff4a.so");
  so_finalize();
  so_execute_init_array();
  so_free_temp();

  /* ---- resolve entry-points ---- */
  fn_JNI_OnLoad JNI_OnLoad = (fn_JNI_OnLoad)so_find_addr_safe("JNI_OnLoad");
  fn_render render = (fn_render)so_find_addr("render");
  fn_resume resume = (fn_resume)so_find_addr("resume");
  fn_setViewport setViewport = (fn_setViewport)so_find_addr("setViewport");
  fn_pause pause = (fn_pause)so_find_addr_safe("pause");
  fn_release release = (fn_release)so_find_addr_safe("release");
  fn_touch touch = (fn_touch)so_find_addr_safe("touch");
  (void)pause; (void)release;
  debugPrintf("entry-points: JNI_OnLoad=%p render=%p resume=%p setViewport=%p\n",
              (void *)JNI_OnLoad, (void *)render, (void *)resume,
              (void *)setViewport);

  /* FIX "direção infinita": render() faz `cont |= getKeyEvent()` todo frame e
   * NADA no caminho de pad limpa o acumulador — no celular era o TOUCH que
   * zerava (touch() zera cont em touch+0x148). Com pad puro, 1 press ficava
   * acumulado p/ sempre → contF/PAD_Read/CPad viam "held" eterno → menu com
   * repeat infinito (CONTINUE↔SELECT TALE ciclando sozinho). Solução: ao
   * SOLTAR um botão, limpar esses bits direto em cont/contF (símbolos B
   * exportados pelo .so). */
  volatile unsigned *eng_cont  = (volatile unsigned *)so_find_addr_safe("cont");
  volatile unsigned *eng_contF = (volatile unsigned *)so_find_addr_safe("contF");
  debugPrintf("input-fix: cont=%p contF=%p\n", (void *)eng_cont, (void *)eng_contF);

  /* ---- dados do OBB (U4) ---- */
  char obbpath[512];
  snprintf(obbpath, sizeof(obbpath), "%s/main.obb", data);
  int obr = obb_init(obbpath);
  debugPrintf("obb_init(%s) -> %d\n", obbpath, obr);
  if (obr != 0)
    fatal_error("obb_init falhou (%d) — main.obb ausente/inválido?", obr);

  /* ---- JNIEnv falso + boot ---- */
  void *vm = NULL, *env = NULL;
  jni_shim_init(&vm, &env);
  jni_set_screen(w, h);
  setenv("FF4A_DATADIR", data, 1);

  if (JNI_OnLoad) {
    debugPrintf("chamando JNI_OnLoad...\n");
    int jr = JNI_OnLoad(vm, NULL);
    debugPrintf("JNI_OnLoad -> %d (0x%x)\n", jr, jr);
  }
  debugPrintf("setViewport(%d,%d)...\n", w, h);
  setViewport(env, ff4a_fake_obj, w, h);
  debugPrintf("resume()...\n");
  resume(env, ff4a_fake_obj);

  /* ---- loop de render ---- */
  debugPrintf("entrando no loop de render\n");
  /* Teclado virtual (mesmo do ff4): atlas montado com o contexto GL vivo.
   * O After Years ainda nao chama createEditText, entao ele fica pronto e
   * so aparece quando o engine pedir -- ou com FF4A_VKBD_DEMO=1. */
  vkbd_init(getenv("FF4A_FONT"));
  if (getenv("FF4A_VKBD_DEMO")) vkbd_activate("Ceodore", 8);

  long frame = 0;
  int running = 1;
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        running = 0;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
        running = 0;
    }
    /* ---- input -> bitmask do jogo (tabela K decompilada) ---- */
    enum { K_A=1, K_B=2, K_RIGHT=16, K_LEFT=32, K_UP=64, K_DOWN=128,
           K_R=256, K_L=512, K_Y=1024, K_X=2048, K_SELECT=4096 };
    int mask = 0;
    if (gc) {
#define BTN(b) SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_##b)
      if (BTN(A)) mask |= K_A;
      /* B = cancel. A ligacao controle -> slot fica numa linha so' porque e'
       * ela que o contrato declarativo de acoes audita (input-contract.json);
       * o bit extra do BACK Android vem logo abaixo, com o seu proprio porque.
       * As duas linhas juntas setam exatamente os mesmos bits que a versao
       * anterior, sob a mesma condicao. */
      if (BTN(B)) mask |= K_B;   /* isPadCancel dos widgets */
      if (BTN(B)) {
        /* Alem do K_B, o BACK Android no modo corrente (assignBackButton) --
         * e' ele que VOLTA nas telas mobile (menu in-game, opcoes, etc). */
        int bm = jni_get_back_mode();
        mask |= (bm == 1 ? 0x8000 : bm >= 2 ? 0x4000 : 0);
      }
      if (BTN(X)) mask |= K_X;
      if (BTN(Y)) mask |= K_Y;
      if (BTN(DPAD_RIGHT)) mask |= K_RIGHT;
      if (BTN(DPAD_LEFT)) mask |= K_LEFT;
      if (BTN(DPAD_UP)) mask |= K_UP;
      if (BTN(DPAD_DOWN)) mask |= K_DOWN;
      if (BTN(LEFTSHOULDER)) mask |= K_L;
      if (BTN(RIGHTSHOULDER)) mask |= K_R;
      if (BTN(BACK)) mask |= K_SELECT;

      /* Chord de saida obrigatorio de release: SELECT+START por ESTADO.
       * Le' direto o par BACK+START da SDL (nao o mask do jogo) porque START
       * e' consumido pela rota de menu abaixo e nunca chegaria aqui. */
      if (BTN(BACK) && BTN(START)) {
        debugPrintf("[ff4a] SELECT+START -> saindo\n");
        running = 0;
      }
#undef BTN
      /* Left stick -> D-pad com deadzone GRANDE. Adaptadores PS2 baratos
       * (Twin USB) tem drift forte (~12-16k) que gerava cima/baixo/esq SOZINHO.
       * O D-pad e' HAT (h0.*, digital, imune a drift) e ja' cobre o movimento
       * acima. FF4A_DEADZONE tuna; =0 DESLIGA o analogico (so D-pad hat). */
      static int dz = -2;
      if (dz == -2) { const char *e = getenv("FF4A_DEADZONE"); dz = e ? atoi(e) : 9200; }
      if (dz > 0) {
        int axx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int axy = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        /* Deadzone RADIAL (circulo), nao por eixo: a diagonal deixa de exigir
         * o limiar em CADA eixo e o piso cai de 12000 (37%) para 9200 (28%),
         * ainda acima do drift medido (~12-16k so' aparece com o eixo cravado,
         * e o hat digital segue cobrindo o movimento). Histerese 5/8 entre os
         * eixos para o passo em grade nao ziguezaguear. FF4A_DEADZONE tuna. */
        long amx = axx < 0 ? -(long)axx : axx, amy = axy < 0 ? -(long)axy : axy;
        if (amx * amx + amy * amy > (long)dz * dz) {
          if (amx * 8 >= amy * 5) { if (axx > 0) mask |= K_RIGHT; else mask |= K_LEFT; }
          if (amy * 8 >= amx * 5) { if (axy > 0) mask |= K_DOWN;  else mask |= K_UP; }
        }
        if (getenv("FF4A_INPUT_DEBUG") && (frame % 60) == 0)
          debugPrintf("[INPUT] Lstick=(%d,%d) dz=%d mask=0x%x\n", axx, axy, dz, mask);
      }
      /* diagnostico: loga TODA mudanca de mask com as fontes cruas (botao
       * mapeado vs eixo) p/ separar "SDL segura" de "engine retem". */
      if (getenv("FF4A_INPUT_DEBUG")) {
        static int lastm = -1;
        if (mask != lastm) {
          debugPrintf("[INPUT] change mask=0x%x btnD=%d btnU=%d L=(%d,%d) R=(%d,%d) f=%ld\n",
                      mask,
                      SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN),
                      SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP),
                      (int)SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                      (int)SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                      (int)SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                      (int)SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY),
                      frame);
          lastm = mask;
        }
      }
    }
    /* FF4A_TAPKEY="mask:frame:len" — pulso UNICO de mask direto na engine,
     * sem SDL/pad/gptokeyb no caminho (isola onde o "down infinito" nasce). */
    {
      static int tk_mask = -1, tk_len = 0; static long tk_frame = 0;
      if (tk_mask == -1) {
        const char *e = getenv("FF4A_TAPKEY");
        tk_mask = 0;
        if (e) { long f=0; int m=0,l=9;
          if (sscanf(e, "%d:%ld:%d", &m, &f, &l) >= 2) { tk_mask=m; tk_frame=f; tk_len=l; }
        }
      }
      if (tk_mask > 0 && frame >= tk_frame && frame < tk_frame + tk_len)
        mask |= tk_mask;
    }
    /* teclado: SO' quando NAO ha' pad. Com pad conectado, ignorar o teclado
     * evita que o gptokeyb (que emula setas a partir do MESMO stick com drift)
     * reinjete direcoes fantasma por cima do input ja' deadzonado. */
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (!gc) {
      if (ks[SDL_SCANCODE_RIGHT]) mask |= K_RIGHT;
      if (ks[SDL_SCANCODE_LEFT]) mask |= K_LEFT;
      if (ks[SDL_SCANCODE_UP]) mask |= K_UP;
      if (ks[SDL_SCANCODE_DOWN]) mask |= K_DOWN;
      if (ks[SDL_SCANCODE_Z] || ks[SDL_SCANCODE_RETURN]) mask |= K_A;
      if (ks[SDL_SCANCODE_X] || ks[SDL_SCANCODE_BACKSPACE]) mask |= K_B;
    }
    /* auto-input p/ teste headless. FF4A_AUTOKEY=<bit> segura esse bit pulsado;
     * FF4A_HOLD=<mask> segura constante. Default pulsa A. */
    if (getenv("FF4A_AUTOKEY")) {
      const char *ak = getenv("FF4A_AUTOKEY");
      int bit = (ak[0] >= '0' && ak[0] <= '9') ? atoi(ak) : K_A;
      if ((frame % 24) < 6) mask |= bit; /* pulso ~1x/segundo */
    }
    /* walk test: depois do frame N (já no mapa). FF4A_WALKDIR fixa a direção
     * (64=UP p/ rumo aos NPCs/capitão); default = círculos. A-pulse interage. */
    if (getenv("FF4A_WALK") && frame > atol(getenv("FF4A_WALK"))) {
      if (getenv("FF4A_WALKDIR")) {
        int dir = atoi(getenv("FF4A_WALKDIR"));
        long ph = (frame / 30) % 3; /* anda 2/3, para 1/3 p/ o A-pulse interagir */
        if (ph < 2) mask |= dir;
      } else {
        long w = (frame / 40) % 4;
        mask |= (w == 0) ? K_UP : (w == 1) ? K_RIGHT : (w == 2) ? K_DOWN : K_LEFT;
      }
    }
    if (getenv("FF4A_HOLD")) mask |= atoi(getenv("FF4A_HOLD"));
    /* release-clear do acumulador da engine: limpa SO' os bits que acabamos de
     * soltar (nao mexe em bits sinteticos que a propria engine escreva). */
    {
      static int prev_mask = 0;
      int cleared = prev_mask & ~mask;
      if (cleared) {
        if (eng_cont)  *eng_cont  &= ~(unsigned)cleared;
        if (eng_contF) *eng_contF &= ~(unsigned)cleared;
        if (getenv("FF4A_INPUT_DEBUG"))
          debugPrintf("[INPUT] release-clear 0x%x (cont=0x%x)\n", cleared,
                      eng_cont ? *eng_cont : 0);
      }
      prev_mask = mask;
    }
    /* Teclado virtual ativo: consome d-pad/A/B (nada vai para o jogo) e
     * digita. Enquanto o After Years nao chamar createEditText isto nunca
     * roda -- fica pronto para quando chamar. */
    {
      static unsigned kbd_prev = 0;
      if (vkbd_is_active()) {
        unsigned km = (unsigned)mask;
        unsigned ke = km & ~kbd_prev;
        int ok = vkbd_handle(ke & K_LEFT, ke & K_RIGHT, ke & K_UP, ke & K_DOWN,
                             ke & K_A, ke & K_B);
        kbd_prev = km;
        if (ok) {
          debugPrintf("[vkbd] OK -> \"%s\"\n", vkbd_text());
          vkbd_deactivate();
        }
        mask = 0;
      } else {
        kbd_prev = 0;
      }
    }
    jni_set_keystate(mask);

    /* ---- cursor+tap (stick direito move, A = tap) p/ menus mobile touch ---- */
    static float cx = 0.5f, cy = 0.5f;
    static int prevA = 0;
    if (gc && touch) {
      int rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
      int ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
      /* deadzone GRANDE tb no stick direito: senao o cursor dos menus "anda
       * sozinho" com o drift do adaptador. FF4A_RDEADZONE tuna (default 22000). */
      static int rdz = -2;
      if (rdz == -2) { const char *e = getenv("FF4A_RDEADZONE"); rdz = e ? atoi(e) : 9000; }
      if (rx > rdz || rx < -rdz) {
        cx += (rx / 32768.0f) * 0.025f;
        g_cursor_show_until = SDL_GetTicks() + CURSOR_HOLD_MS;
      }
      if (ry > rdz || ry < -rdz) {
        cy += (ry / 32768.0f) * 0.025f;
        g_cursor_show_until = SDL_GetTicks() + CURSOR_HOLD_MS;
      }
      if (cx < 0) cx = 0; if (cx > 1) cx = 1;
      if (cy < 0) cy = 0; if (cy > 1) cy = 1;
      /* Toque do cursor = SO' o R3 (clique do stick direito). O A NAO entra
       * aqui: ele ja' e' o "confirmar" NATIVO (K_A no keystate, acima). Incluir
       * o A tambem gerava um toque sintetico na posicao do cursor, entao um
       * unico A avancava o dialogo DUAS vezes (relato de campo 22/08 -- dialogos
       * pulando dobrado). O cursor e' movido pelo analogico direito, entao o R3
       * e' o clique natural dele. FF4A_A_TAP=1 reativa o A no toque, se algum
       * menu so'-toque precisar. */
      static int a_tap = -1;
      if (a_tap < 0) a_tap = getenv("FF4A_A_TAP") ? 1 : 0;
      int aNow = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)
               || (a_tap && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A));
      g_cursor_pressed = aNow;
      if (aNow) g_cursor_show_until = SDL_GetTicks() + CURSOR_HOLD_MS;
      if (aNow && !prevA) {
        debugPrintf("[TOUCH] pad DOWN (%.3f,%.3f) f=%ld\n", cx, cy, frame);
        touch(env, ff4a_fake_obj, 0, 1, cx, cy, 0, 0);
      } else if (!aNow && prevA) {
        debugPrintf("[TOUCH] pad UP (%.3f,%.3f) f=%ld\n", cx, cy, frame);
        touch(env, ff4a_fake_obj, 1, 1, cx, cy, 0, 0);
      }
      prevA = aNow;
    }
    /* ---- B = VOLTAR nos menus mobile: tap sintetico no pill "Back" ----
     * O caminho de pad-cancel da engine (isPadCancel/edge) e' natimorto: com
     * multiplos parts o CPad::read roda 2+ vezes por frame e o edge morre em
     * microssegundos. As telas mobile so' voltam por TOUCH no pill Back
     * (canto inferior direito, ~0.90,0.92 em todas). Quando um menu esta'
     * aberto a engine anuncia assignBackButton(k>=1) — usamos isso de gate. */
    if (gc && touch) {
      static int prevB = 0, backtap_up = 0;
      int bNow = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B);
      if (bNow && !prevB && jni_get_back_mode() >= 1) {
        debugPrintf("[TOUCH] B->Back tap (0.90,0.92) f=%ld\n", frame);
        touch(env, ff4a_fake_obj, 0, 1, 0.90f, 0.92f, 0, 0);
        backtap_up = 4; /* solta em ~4 frames */
      }
      if (backtap_up > 0 && --backtap_up == 0)
        touch(env, ff4a_fake_obj, 1, 1, 0.90f, 0.92f, 0, 0);
      prevB = bNow;
    }
    /* teclado: IJKL move cursor, ESPAÇO tap (teste sem pad) */
    if (ks[SDL_SCANCODE_L]) cx += 0.015f;
    if (ks[SDL_SCANCODE_J]) cx -= 0.015f;
    if (ks[SDL_SCANCODE_K]) cy += 0.015f;
    if (ks[SDL_SCANCODE_I]) cy -= 0.015f;
    if (cx < 0) cx = 0; if (cx > 1) cx = 1;
    if (cy < 0) cy = 0; if (cy > 1) cy = 1;
    {
      static int prevSp = 0;
      int sp = ks[SDL_SCANCODE_SPACE];
      if (touch && sp && !prevSp) touch(env, ff4a_fake_obj, 0, 1, cx, cy, 0, 0);
      else if (touch && !sp && prevSp) touch(env, ff4a_fake_obj, 1, 1, cx, cy, 0, 0);
      prevSp = sp;
    }
    g_cursor_x = cx; g_cursor_y = cy;

    /* auto-tap p/ teste headless: fases de taps repetidos (tolera timing) */
    if (getenv("FF4A_TAPSCRIPT") && touch) {
      long f = frame;
      float tx = -1, ty = -1;
      /* fase 1: SELECT TALE (300-700) -> abre a lista */
      if (f >= 300 && f < 700) { tx = 0.49f; ty = 0.69f; }
      /* fase 2: Ceodore's Tale (750-1400) -> janela larga p/ pegar a lista */
      else if (f >= 750 && f < 1400) { tx = 0.25f; ty = 0.16f; }
      /* fase 3 (1450+): SEM taps -> deixa a tale carregar/tocar */
      if (tx >= 0) {
        int ph = f % 70;
        if (ph == 0) touch(env, ff4a_fake_obj, 0, 1, tx, ty, 0, 0);
        else if (ph == 8) touch(env, ff4a_fake_obj, 1, 1, tx, ty, 0, 0);
      }
      /* fase 4: já no mapa -> taps repetidos no botão "Menu" (party/status/itens) */
      if (getenv("FF4A_MENU")) {
        long mf = atol(getenv("FF4A_MENU"));
        if (f >= mf && f < mf + 500) {
          int ph = (f - mf) % 90;
          if (ph == 0) touch(env, ff4a_fake_obj, 0, 1, 0.945f, 0.055f, 0, 0);
          else if (ph == 6) touch(env, ff4a_fake_obj, 1, 1, 0.945f, 0.055f, 0, 0);
        }
      }
    }
    if (getenv("FF4A_AUTOTAP") && touch) {
      float tx = getenv("FF4A_TAPX") ? atof(getenv("FF4A_TAPX")) : 0.5f;
      float ty = getenv("FF4A_TAPY") ? atof(getenv("FF4A_TAPY")) : 0.5f;
      int ph = frame % 40;
      if (ph == 0) touch(env, ff4a_fake_obj, 0, 1, tx, ty, 0.0f, 0.0f);
      else if (ph == 6) touch(env, ff4a_fake_obj, 1, 1, tx, ty, 0.0f, 0.0f);
    }
    jni_set_frame(frame);
    render(env, ff4a_fake_obj);
    {
      static int force = -1;
      if (force < 0) force = getenv("FF4A_CURSOR") ? 1 : 0;
      if (force || SDL_GetTicks() < g_cursor_show_until)
        draw_cursor(g_cursor_x, g_cursor_y);
    }
    {  /* teclado virtual por cima (so' quando ativo) */
      int vw = 1280, vh = 720, vp[4] = {0, 0, 1280, 720};
      glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
      if (vp[2] > 0) vw = vp[2];
      if (vp[3] > 0) vh = vp[3];
      vkbd_render(vw, vh);
    }
    /* Prova de imagem CONTINUA, ANTES do present (pos-swap o backbuffer e'
     * indefinido em Mali tile-based). O adapter amostra no cronograma,
     * publica o veredito e grita IMAGE PROOF se o painel ficar preto com o
     * jogo vivo. */
    {
      int pw = 0, ph = 0;
      SDL_GL_GetDrawableSize(win, &pw, &ph);
      nxgl_frame_proof_before_present(pw, ph);
    }
    SDL_GL_SwapWindow(win);
    if (frame < 10 || (frame % 60) == 0)
      debugPrintf("frame %ld\n", frame);
    frame++;
    if (frames > 0 && frame >= frames)
      running = 0;
  }

  debugPrintf("=== saindo (frames=%ld) ===\n", frame);
  ff4a_install_shutdown_guards();
  if (pause) pause(env, ff4a_fake_obj); /* save do jogo -- parte segura */
  ff4a_shutdown_deadline();
  debugPrintf("[ff4a] teardown seguro concluido; saindo\n");
  log_close();
  _exit(0);
}
