/* main_angrybirds.c — laço de vida do Angry Birds Classic 8.0.3 no NextOS.
 *
 * A ordem aqui NÃO foi inventada: ela é a das classes Java do próprio APK
 * (decompiladas com jadx em extracted/java/), que são a autoridade sobre esta
 * engine:
 *
 *   MySurfaceView          -> new NativeApplication(view, filesDir)
 *   NativeApplication(...) -> nativeConfig(filesDir)
 *                             nativeGetPossibleOrientations()
 *                             nativeRenderThread()  (escolhe Single/MultiThread)
 *   MyRenderer.onSurfaceCreated  -> EGLWrapper.init(config)
 *   MyRenderer.onSurfaceChanged  -> setOrientation(w>h)
 *                                   initialize(w,h) na primeira vez
 *   SingleThreadWrapper.initialize -> doInit(w,h) + onResize(w,h)
 *   NativeApplication.doInit       -> nativeInit(w,h) [+ nativeResume]
 *   MyRenderer.onDrawFrame         -> updater.onFrame()
 *       single: handleEvents() + nativeUpdate()   (o update TAMBÉM desenha)
 *       multi : nativeRender() na thread de GL, nativeUpdate() na UpdateThread
 *               com contexto EGL compartilhado
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "ab_egl.h"      /* egl* -> tabela ligada pelo nxgl_egl_binding */
#include "nxgl_gles2.h"  /* gl*  -> ponteiros provados pelo nxgl_gles2 */
#include <SDL.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ab_port.h"
#include "ab_locale.h"
#include "nxloader_softfp.h"

#define SF __attribute__((pcs("aapcs")))

nxloader_module *ab_guest;

/* ---------- exports do convidado ---------- */
typedef void (*VoidFn)(void *env, void *obj);
typedef void (*ConfigFn)(void *env, void *obj, void *jstring_path);
typedef int32_t (*IntFn)(void *env, void *obj);
typedef uint8_t (*BoolFn)(void *env, void *obj);
typedef void (*InitFn)(void *env, void *obj, int32_t w, int32_t h);
typedef uint8_t (*ResizeFn)(void *env, void *obj, int32_t w, int32_t h);

static ConfigFn native_config;
static IntFn native_get_orientations;
static BoolFn native_render_thread;
static InitFn native_init;
static ResizeFn native_resize;
static BoolFn native_update;
static BoolFn native_render;
static VoidFn native_pause;
static VoidFn native_resume;
static VoidFn native_deinit;
static VoidFn native_frame_clear;
static VoidFn native_interrupt_render;

/* ---------- estado ---------- */
static SDL_Window *g_window;
static SDL_GLContext g_gl;
static int g_width, g_height;
static void *g_app_object;
static volatile int g_running = 1;
static volatile int g_resumed;
static volatile int g_initialized;
static int g_multithread;
static SDL_Thread *g_update_thread;
static SDL_mutex *g_update_lock;

extern void ab_audio_create(int64_t mixer, int rate, int channels, int bits,
                            int bufbytes);
extern int ab_audio_start(void);
extern void ab_audio_stop(void);
extern void ab_input_flush(void);
extern void ab_input_set_screen(int width, int height);
extern void *ab_jni_new_string(const char *utf);
extern void *ab_jni_new_object(const char *cls);

uintptr_t ab_sym(const char *name) {
  uintptr_t address = 0;
  if (!ab_guest)
    return 0;
  if (nxloader_module_find_export(ab_guest, name, &address) != NXLOADER_OK)
    return 0;
  return address;
}

/* ==================== EGL compartilhado (EGLWrapper) ====================
 * O EGLWrapper do APK fica do lado Java, ou seja o contexto é NOSSO. Ele é
 * usado pela MultiThreadWrapper para dar à UpdateThread um contexto que
 * compartilha objetos com o principal.
 */
#define EGL_SLOTS 8
typedef struct {
  EGLContext context;
  EGLSurface surface;
  int used;
} EglSlot;
static EglSlot g_egl_slots[EGL_SLOTS];
static int g_egl_count;

static EGLConfig current_egl_config(EGLDisplay display) {
  EGLint config_id = 0;
  EGLint attribs[] = {EGL_CONFIG_ID, 0, EGL_NONE};
  EGLConfig config = NULL;
  EGLint found = 0;
  if (!eglQueryContext(display, eglGetCurrentContext(), EGL_CONFIG_ID,
                       &config_id))
    return NULL;
  attribs[1] = config_id;
  if (!eglChooseConfig(display, attribs, &config, 1, &found) || found < 1)
    return NULL;
  return config;
}

static int egl_current_context(void) {
  EGLContext context = eglGetCurrentContext();
  for (int i = 1; i < g_egl_count; i++)
    if (g_egl_slots[i].used && g_egl_slots[i].context == context)
      return i;
  if (g_egl_count == 0)
    g_egl_count = 1; /* o índice 0 é reservado, igual ao EGLWrapper do APK */
  if (g_egl_count >= EGL_SLOTS)
    return 0;
  g_egl_slots[g_egl_count].context = context;
  g_egl_slots[g_egl_count].surface = eglGetCurrentSurface(EGL_DRAW);
  g_egl_slots[g_egl_count].used = 1;
  return g_egl_count++;
}

static int egl_create_shared(int handle) {
  EGLDisplay display = eglGetCurrentDisplay();
  EGLConfig config;
  EGLContext shared;
  EGLSurface pbuffer;
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  const EGLint pbuffer_attribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
  if (handle <= 0 || handle >= g_egl_count || !g_egl_slots[handle].used)
    return 0;
  if (g_egl_count >= EGL_SLOTS)
    return 0;
  config = current_egl_config(display);
  if (!config) {
    ab_log("[egl] nao consegui recuperar o EGLConfig atual");
    return 0;
  }
  shared = eglCreateContext(display, config, g_egl_slots[handle].context,
                            context_attribs);
  if (shared == EGL_NO_CONTEXT) {
    ab_log("[egl] eglCreateContext compartilhado falhou (0x%x)", eglGetError());
    return 0;
  }
  pbuffer = eglCreatePbufferSurface(display, config, pbuffer_attribs);
  if (pbuffer == EGL_NO_SURFACE) {
    ab_log("[egl] eglCreatePbufferSurface falhou (0x%x)", eglGetError());
    eglDestroyContext(display, shared);
    return 0;
  }
  g_egl_slots[g_egl_count].context = shared;
  g_egl_slots[g_egl_count].surface = pbuffer;
  g_egl_slots[g_egl_count].used = 1;
  ab_log("[egl] contexto compartilhado %d criado", g_egl_count);
  return g_egl_count++;
}

static void egl_destroy_shared(int handle) {
  EGLDisplay display = eglGetCurrentDisplay();
  if (handle <= 1 || handle >= g_egl_count || !g_egl_slots[handle].used)
    return;
  eglDestroySurface(display, g_egl_slots[handle].surface);
  eglDestroyContext(display, g_egl_slots[handle].context);
  g_egl_slots[handle].used = 0;
}

static int egl_register_thread(int handle) {
  EGLDisplay display = eglGetCurrentDisplay();
  if (handle <= 0 || handle >= g_egl_count || !g_egl_slots[handle].used)
    return 0;
  return eglMakeCurrent(display, g_egl_slots[handle].surface,
                        g_egl_slots[handle].surface,
                        g_egl_slots[handle].context)
             ? 1
             : 0;
}

static void egl_unregister_thread(void) {
  eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);
}

/* ==================== hooks para o falso-JNI ==================== */

static int hook_display_width(void) { return g_width; }
static int hook_display_height(void) { return g_height; }
static int hook_ppi(void) { return 160; }
static void hook_quit(void) { g_running = 0; }

/* ==================== instância única ==================== */

static int lock_single_instance(const char *executable) {
  int fd = open(executable, O_RDONLY);
  if (fd < 0)
    return -1;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ab_log("[main] outra instancia ja esta rodando este binario");
    close(fd);
    return -1;
  }
  return fd; /* mantido aberto pela vida do processo */
}

/* ==================== thread de update (modo multithread) ============== */

static int g_shared_handle;

static int SDLCALL update_thread_main(void *userdata) {
  (void)userdata;
  if (g_shared_handle > 0 && !egl_register_thread(g_shared_handle))
    ab_log("[egl] registerThread(%d) falhou na UpdateThread", g_shared_handle);
  while (g_running) {
    if (!g_resumed || !g_initialized) {
      SDL_Delay(4);
      continue;
    }
    ab_input_flush();
    if (native_update && !native_update(ab_jni_env(), g_app_object)) {
      ab_log("[main] nativeUpdate devolveu false: o jogo pediu saida");
      g_running = 0;
      break;
    }
  }
  return 0;
}

/* ==================== main ==================== */

static void resolve_exports(void) {
#define BIND(variable, type, symbol)                     \
  do {                                                   \
    variable = (type)ab_sym(symbol);                     \
    if (!variable)                                       \
      ab_log("[main] export AUSENTE: %s", symbol);       \
  } while (0)

  BIND(native_config, ConfigFn,
       "Java_com_rovio_fusion_NativeApplication_nativeConfig");
  BIND(native_get_orientations, IntFn,
       "Java_com_rovio_fusion_NativeApplication_nativeGetPossibleOrientations");
  BIND(native_render_thread, BoolFn,
       "Java_com_rovio_fusion_NativeApplication_nativeRenderThread");
  BIND(native_init, InitFn,
       "Java_com_rovio_fusion_NativeApplication_nativeInit");
  BIND(native_resize, ResizeFn,
       "Java_com_rovio_fusion_NativeApplication_nativeResize");
  BIND(native_update, BoolFn,
       "Java_com_rovio_fusion_NativeApplication_nativeUpdate");
  BIND(native_render, BoolFn,
       "Java_com_rovio_fusion_NativeApplication_nativeRender");
  BIND(native_pause, VoidFn,
       "Java_com_rovio_fusion_NativeApplication_nativePause");
  BIND(native_resume, VoidFn,
       "Java_com_rovio_fusion_NativeApplication_nativeResume");
  BIND(native_deinit, VoidFn,
       "Java_com_rovio_fusion_NativeApplication_nativeDeinit");
  BIND(native_frame_clear, VoidFn,
       "Java_com_rovio_fusion_NativeApplication_nativeFrameClear");
  BIND(native_interrupt_render, VoidFn,
       "Java_com_rovio_fusion_NativeApplication_nativeInterruptRender");
#undef BIND
}

static void loader_log(void *userdata, nxloader_log_level level,
                       const char *message) {
  (void)userdata;
  if (level <= NXLOADER_LOG_INFO || ab_env_int("AB_LOADER_LOG", 0))
    ab_log("[nxloader] %s", message);
}

int main(int argc, char **argv) {
  char executable[PATH_MAX];
  char gamedir[PATH_MAX];
  char guest_path[PATH_MAX + 64];
  nxloader_config config;
  nxloader_registry *registry = NULL;
  nxloader_resolution_report report;
  nxloader_jni_onload_options onload;
  const int32_t accepted[] = {0x00010004, 0x00010006, 0x00010002};
  int32_t jni_version = 0;
  nxloader_result rc;
  ssize_t link_len;
  ab_jni_hooks hooks;
  int orientations;

  link_len = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (link_len <= 0)
    snprintf(executable, sizeof(executable), "%s", argv[0]);
  else
    executable[link_len] = 0;
  snprintf(gamedir, sizeof(gamedir), "%s", executable);
  {
    char *slash = strrchr(gamedir, '/');
    if (slash)
      *slash = 0;
  }
  if (argc > 1 && argv[1][0] == '/')
    snprintf(gamedir, sizeof(gamedir), "%s", argv[1]);

  ab_log_open(gamedir);
  ab_log("=== Angry Birds Classic 8.0.3 · framework universal 1.1.1 ===");
  ab_log("[main] executavel=%s", executable);
  ab_log("[main] gamedir=%s", gamedir);

  if (lock_single_instance(executable) < 0) {
    ab_log("[main] abortando: instancia unica");
    return 1;
  }

  /* Idioma do jogo: NXPORT_LANGUAGE (GAME_LANGUAGE do launcher); "auto" lê o
   * LANG do sistema ANTES do processo fixar o seu proprio locale C. Japones
   * nunca por padrao (regra da casa). */
  ab_locale_init();
  setenv("LANG", "en_US.UTF-8", 1);
  setenv("LC_ALL", "en_US.UTF-8", 1);

  if (!ab_framework_preflight(gamedir)) {
    ab_log("[main] preflight universal falhou");
    return 2;
  }

  if (ab_apk_open(ab_apk_path()) != 0) {
    ab_log("[main] APK do dono ausente em %s", ab_apk_path());
    return 3;
  }

  if (!ab_framework_open_graphics(&g_window, &g_gl, &g_width, &g_height)) {
    ab_log("[main] negociacao de video/GLES2 falhou");
    return 4;
  }
  if (SDL_InitSubSystem(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER |
                        SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
    ab_log("[main] SDL subsistemas falharam: %s", SDL_GetError());
    return 5;
  }
  SDL_GL_SetSwapInterval(1);
  SDL_ShowCursor(SDL_DISABLE);

  /* primeiro frame preto para não deixar lixo do console na tela */
  glViewport(0, 0, g_width, g_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!ab_framework_present()) {
    ab_log("[main] primeiro present falhou");
    return 5;
  }

  /* ---------- carregar o convidado ---------- */
  nxloader_config_init(&config);
  config.expected_arch = NXLOADER_ARCH_ARMV7;
  config.log = loader_log;
  config.trampoline_pool_size = 64 * 1024;
  rc = nxloader_module_create(&config, &ab_guest);
  if (rc != NXLOADER_OK) {
    ab_log("[main] nxloader_module_create: %s", nxloader_result_string(rc));
    return 6;
  }
  /* A .so do convidado sai do APK do dono, em memória: nada de dado de jogo
   * gravado em disco por nossa conta. O arquivo solto só é usado se existir
   * (conveniência de bring-up). */
  snprintf(guest_path, sizeof(guest_path), "%s/libAngryBirdsClassic.so",
           gamedir);
  if (access(guest_path, R_OK) == 0) {
    rc = nxloader_module_load_file(ab_guest, guest_path);
    ab_log("[main] convidado carregado do arquivo solto");
  } else {
    size_t guest_size = 0;
    void *guest_image =
        ab_apk_entry("lib/armeabi-v7a/libAngryBirdsClassic.so", &guest_size);
    if (!guest_image) {
      ab_log("[main] lib/armeabi-v7a/libAngryBirdsClassic.so ausente no APK");
      return 7;
    }
    ab_log("[main] convidado carregado do APK (%zu B)", guest_size);
    rc = nxloader_module_load_memory(ab_guest, guest_image, guest_size,
                                     "libAngryBirdsClassic.so");
    free(guest_image);
  }
  if (rc != NXLOADER_OK) {
    ab_log("[main] load do convidado: %s", nxloader_result_string(rc));
    return 7;
  }
  rc = nxloader_module_relocate(ab_guest);
  if (rc != NXLOADER_OK) {
    ab_log("[main] relocate: %s", nxloader_result_string(rc));
    return 8;
  }

  if (nxloader_registry_create(&registry) != NXLOADER_OK) {
    ab_log("[main] registry_create falhou");
    return 9;
  }
  nxloader_softfp_add_libm(registry, "angrybirds-softfp-libm", 5, NULL);
  ab_add_bionic_provider(registry);
  ab_add_pthread_provider(registry);
  ab_add_stdio_provider(registry);
  ab_add_gl_provider(registry);

  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  rc = nxloader_module_resolve(ab_guest, registry, 0, &report);
  ab_log("[main] resolve: %s (resolvidos=%zu, fracos=%zu, faltando=%zu, "
         "primeiro=%s)",
         nxloader_result_string(rc), report.imports_resolved,
         report.weak_imports_zeroed, report.unresolved_strong,
         report.first_unresolved ? report.first_unresolved : "-");
  if (rc != NXLOADER_OK)
    return 10;

  /* Slingshot.lua consulta um snapshot do pad pela os.clock interna. O hook e
   * os offsets são específicos e verificados para a lib 8.0.3 deste APK. */
  if (!ab_lua_control_install()) {
    ab_log("[control] hook Lua nao instalado; estilingue automatico desativado");
    return 10;
  }

  rc = nxloader_module_finalize(ab_guest);
  if (rc != NXLOADER_OK) {
    ab_log("[main] finalize: %s", nxloader_result_string(rc));
    return 11;
  }

  /* falso-JNI pronto ANTES dos construtores: eles podem chamar de volta */
  ab_jni_init();
  memset(&hooks, 0, sizeof(hooks));
  hooks.display_width = hook_display_width;
  hooks.display_height = hook_display_height;
  hooks.ppi = hook_ppi;
  hooks.audio_create = ab_audio_create;
  hooks.audio_start = ab_audio_start;
  hooks.audio_stop = ab_audio_stop;
  hooks.quit_requested = hook_quit;
  hooks.egl_current_context = egl_current_context;
  hooks.egl_create_shared = egl_create_shared;
  hooks.egl_destroy_shared = egl_destroy_shared;
  hooks.egl_register_thread = egl_register_thread;
  hooks.egl_unregister_thread = egl_unregister_thread;
  ab_jni_set_hooks(&hooks);

  rc = nxloader_module_call_initializers(ab_guest);
  if (rc != NXLOADER_OK) {
    ab_log("[main] call_initializers: %s", nxloader_result_string(rc));
    return 12;
  }
  ab_log("[main] construtores OK");

  memset(&onload, 0, sizeof(onload));
  onload.struct_size = sizeof(onload);
  onload.java_vm = ab_jni_vm();
  onload.accepted_versions = accepted;
  onload.accepted_version_count = sizeof(accepted) / sizeof(accepted[0]);
  onload.flags = NXLOADER_JNI_ONLOAD_OPTIONAL;
  rc = nxloader_module_call_jni_onload(ab_guest, &onload, &jni_version);
  ab_log("[main] JNI_OnLoad: %s (versao=0x%x)", nxloader_result_string(rc),
         (unsigned)jni_version);
  if (rc != NXLOADER_OK)
    return 13;

  resolve_exports();
  g_app_object = ab_jni_new_object("com/rovio/fusion/NativeApplication");
  ab_input_set_screen(g_width, g_height);
  ab_input_init();
  if (!ab_framework_publish_input(ab_input_context()) ||
      !ab_framework_require_ready()) {
    ab_log("[main] requisitos universais nao satisfeitos");
    return 14;
  }

  /* ---------- ordem do NativeApplication do APK ---------- */
  if (native_config) {
    void *files_dir = ab_jni_new_string(ab_filesdir());
    ab_log("[main] nativeConfig(%s)", ab_filesdir());
    native_config(ab_jni_env(), g_app_object, files_dir);
  }
  orientations =
      native_get_orientations
          ? native_get_orientations(ab_jni_env(), g_app_object)
          : 0;
  ab_log("[main] nativeGetPossibleOrientations = 0x%x (landscape=%d)",
         orientations, (orientations & 0x0a) ? 1 : 0);
  g_multithread = native_render_thread
                      ? (native_render_thread(ab_jni_env(), g_app_object) != 0)
                      : 0;
  ab_log("[main] nativeRenderThread = %d (%s)", g_multithread,
         g_multithread ? "MultiThreadWrapper" : "SingleThreadWrapper");

  /* onSurfaceChanged: initialize(w,h) */
  g_update_lock = SDL_CreateMutex();
  if (native_init) {
    ab_log("[main] nativeInit(%d, %d)", g_width, g_height);
    native_init(ab_jni_env(), g_app_object, g_width, g_height);
  }
  if (native_resume) {
    native_resume(ab_jni_env(), g_app_object);
    g_resumed = 1;
  }
  g_initialized = 1;

  if (g_multithread) {
    /* MultiThreadWrapper: contexto compartilhado para a UpdateThread */
    int main_handle = egl_current_context();
    g_shared_handle = egl_create_shared(main_handle);
    ab_log("[main] contexto principal=%d compartilhado=%d", main_handle,
           g_shared_handle);
    g_update_thread = SDL_CreateThread(update_thread_main, "UpdateThread", NULL);
  } else if (native_resize) {
    ab_log("[main] nativeResize(%d, %d)", g_width, g_height);
    native_resize(ab_jni_env(), g_app_object, g_width, g_height);
  }

  /* ---------- laço de frame ---------- */
  {
    Uint32 frames = 0;
    Uint32 last_report = SDL_GetTicks();
    while (g_running) {
      ab_input_pump();
      if (ab_input_quit_requested())
        break;

      if (g_multithread) {
        if (g_resumed && native_render)
          native_render(ab_jni_env(), g_app_object);
      } else {
        ab_input_flush();
        if (native_update &&
            !native_update(ab_jni_env(), g_app_object)) {
          ab_log("[main] nativeUpdate devolveu false: o jogo pediu saida");
          g_running = 0;
        }
      }
      ab_input_draw_cursor();
      if (!ab_framework_present()) {
        ab_log("[main] present falhou; pedindo saida limpa");
        g_running = 0;
      }

      frames++;
      if (SDL_GetTicks() - last_report >= 5000) {
        ab_log("[main] %.1f fps", frames * 1000.0 /
                                      (double)(SDL_GetTicks() - last_report));
        frames = 0;
        last_report = SDL_GetTicks();
      }
    }
  }

  /* ---------- saída limpa ----------
   * Pausa e desmonte nativo primeiro; depois _exit(0) direto, sem descer a
   * pilha de GL/SDL (lição do Chrono: o SIGSEGV mora no teardown, nunca no
   * jogo). */
  ab_log("[main] encerrando");
  g_running = 0;
  if (g_multithread && native_interrupt_render)
    native_interrupt_render(ab_jni_env(), g_app_object);
  if (g_update_thread)
    SDL_WaitThread(g_update_thread, NULL);
  if (native_pause) {
    native_pause(ab_jni_env(), g_app_object);
    g_resumed = 0;
  }
  ab_audio_stop();
  ab_input_shutdown();
  if (native_deinit)
    native_deinit(ab_jni_env(), g_app_object);
  ab_log("[main] saida limpa");
  ab_log_close();
  _exit(0);
}
