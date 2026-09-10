#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

/* GLES2 sem DT_NEEDED: resolvido em runtime pelo nxgl (selecao por medicao,
 * espelhos dArkOS/ROCKNIX). As chamadas gl* deste arquivo viram os ponteiros
 * nxgl_gles2_pfn_* pelo redirect do header; a engine recebe as funcoes de
 * despacho estaveis via so_resolve. EGL idem, pela ponte local. */
#include "nxgl_gles2.h"
#include "nxgl_frame_proof_adapter.h"
#include "egl_bridge.h"

#include "asset_shim.h"
#include "bionic_shims.h"
#include "error.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "pthread_bridge.h"
#include "so_util.h"
#include "util.h"

typedef int jint;

/* 🩹 CANARY BIONIC (fix provado no Dysmantle/GTA SA): a engine bionic le a
 * stack-guard de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD do Android). Sob glibc
 * esse endereco cai em TLS de outra imagem e pode MUDAR durante a execucao ->
 * prologo le X, epilogo le Y -> __stack_chk_fail -> abort. Reservar um pad
 * TLS local NUNCA escrito deixa o slot estavel em toda thread. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

static void *g_vm = NULL;
static void *g_env = NULL;

static void *my_SDL_GetAndroidJNIEnv(void) {
  return jni_get_env();
}

static void *my_SDL_GetAndroidActivity(void) {
  return (void *)0x1;
}

static const char *my_SDL_GetAndroidInternalStoragePath(void) {
  return bb_game_dir();
}

static void *exit_deadline_thread(void *arg) {
  (void)arg;
  usleep(500000);
  _exit(0);
  return NULL;
}

/* Keep the exit hotkey in the host input event path instead of depending on
 * an Android activity or an engine-specific menu action.  A single
 * BACK/SELECT or START remains untouched. */
static SDL_JoystickID g_exit_gamepad_id = 0;
static bool g_exit_select_held = false;
static bool g_exit_start_held = false;

static void exit_from_gamepad_hotkey(void) {
  logPrintf("[input] SELECT+START -> exiting\n");
  nxgl_frame_proof_publish();
  _exit(0);
}

static void track_exit_gamepad_event(const SDL_Event *event) {
  if (!event) return;

  if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
    if (event->gdevice.which == g_exit_gamepad_id) {
      g_exit_gamepad_id = 0;
      g_exit_select_held = false;
      g_exit_start_held = false;
    }
    return;
  }

  if (event->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
      event->type != SDL_EVENT_GAMEPAD_BUTTON_UP) {
    return;
  }

  const SDL_JoystickID which = event->gbutton.which;
  const bool down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;

  if (g_exit_gamepad_id != 0 && which != g_exit_gamepad_id) {
    return;
  }
  if (g_exit_gamepad_id == 0 && down) {
    g_exit_gamepad_id = which;
  }

  if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
    g_exit_select_held = down;
  } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
    g_exit_start_held = down;
  } else {
    return;
  }

  if (g_exit_select_held && g_exit_start_held) {
    exit_from_gamepad_hotkey();
  }
  if (!g_exit_select_held && !g_exit_start_held) {
    g_exit_gamepad_id = 0;
  }
}

static bool my_SDL_PollEvent(SDL_Event *event) {
  const bool result = SDL_PollEvent(event);
  if (result) track_exit_gamepad_event(event);
  return result;
}

/* Recuperacao de provedor portatil, identica a do nxsplash: em algumas
 * firmwares KMSDRM o provedor da GPU real so' existe pelos nomes NAO
 * versionados (libEGL.so/libGLESv2.so) e o soname versionado e' um
 * dispatcher inutil. So' depois do default falhar, e nunca por cima de um
 * provedor explicito. */
static SDL_Window *my_SDL_CreateWindow(const char *title, int w, int h,
                                       SDL_WindowFlags flags) {
  /* Selecao de provedor EGL/GL da JANELA por MEDICAO (mesma licao do nxgl e
   * da ponte EGL: nenhum nome fixo acerta em todas as firmwares — dArkOS,
   * ArkOS, ROCKNIX, muOS, Knulli, AmberELEC tem cadeias diferentes e ate'
   * cruzadas). A prova de vida e' a propria janela abrir. Ordem: o provedor
   * herdado do frontend (nomes SDL2 traduzidos p/ hints SDL3 — o ES do
   * ArkOS/dArkOS exporta SDL_VIDEO_EGL_DRIVER e o SDL3 nao le), o default
   * da firmware, e entao os nomes portateis/versionados/blob. */
  static const struct {
    const char *egl;
    const char *gl;
    const char *label;
  } k_providers[] = {
    { NULL, NULL, "frontend/default" },
    { "libEGL.so", "libGLESv2.so", "portatil" },
    { "libEGL.so.1", "libGLESv2.so.2", "versionado" },
    { "libmali.so", "libmali.so", "blob-mali" },
    { "libMali.so", "libMali.so", "blob-Mali" },
  };
  const char *env_egl = getenv("SDL_VIDEO_EGL_DRIVER");
  const char *env_gl = getenv("SDL_VIDEO_GL_DRIVER");
  SDL_Window *win = NULL;
  size_t i;

  for (i = 0; i < sizeof(k_providers) / sizeof(k_providers[0]); ++i) {
    const char *egl = k_providers[i].egl;
    const char *gl = k_providers[i].gl;
    if (i == 0) {
      /* Honrar o provedor explicito do frontend, traduzido para o SDL3. */
      if (env_egl != NULL && env_egl[0] != '\0') {
        SDL_SetHint(SDL_HINT_EGL_LIBRARY, env_egl);
        if (env_gl != NULL && env_gl[0] != '\0')
          SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, env_gl);
      } else {
        SDL_ResetHint(SDL_HINT_EGL_LIBRARY);
        SDL_ResetHint(SDL_HINT_OPENGL_LIBRARY);
      }
    } else {
      const char *prev = SDL_GetHint(SDL_HINT_EGL_LIBRARY);
      if (prev != NULL && strcmp(prev, egl) == 0)
        continue; /* identico ao que acabou de falhar */
      SDL_SetHint(SDL_HINT_EGL_LIBRARY, egl);
      SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, gl);
    }
    win = SDL_CreateWindow(title, w, h, flags);
    if (win != NULL) {
      logPrintf("[video] janela aberta com provedor %s (EGL=%s GL=%s)\n",
                k_providers[i].label,
                SDL_GetHint(SDL_HINT_EGL_LIBRARY) != NULL
                    ? SDL_GetHint(SDL_HINT_EGL_LIBRARY) : "default",
                SDL_GetHint(SDL_HINT_OPENGL_LIBRARY) != NULL
                    ? SDL_GetHint(SDL_HINT_OPENGL_LIBRARY) : "default");
      break;
    }
    logPrintf("[video] provedor %s falhou: %s\n", k_providers[i].label,
              SDL_GetError());
  }
  if (win == NULL) {
    SDL_ResetHint(SDL_HINT_EGL_LIBRARY);
    SDL_ResetHint(SDL_HINT_OPENGL_LIBRARY);
    logPrintf("[video] NENHUM provedor abriu a janela\n");
  }
  logPrintf("[video] SDL_CreateWindow -> %p (%s)\n", (void *)win,
            win != NULL ? "ok" : SDL_GetError());
  return win;
}

/* O contexto GL nasce AQUI, dentro da engine. So' depois dele existe uma
 * fonte coerente para resolver GLES2: o SDL_GL_GetProcAddress do proprio
 * contexto (primario), com a cadeia por medicao do nxgl como reserva. */
static SDL_GLContext my_SDL_GL_CreateContext(SDL_Window *window) {
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  logPrintf("[gl] SDL_GL_CreateContext -> %p\n", (void *)ctx);
  if (ctx != NULL) {
    nxgl_gles2_receipt receipt;
    nxgl_gles2_set_primary_resolver(
        (nxgl_gles2_resolver_fn)SDL_GL_GetProcAddress);
    nxgl_gles2_init(&receipt);
    logPrintf("[nxgl] %s\n", receipt.text);
    nxgl_frame_proof_set_resolver(nxgl_gles2_lookup);
    if (nxgl_gles2_pfn_glGetString != NULL) {
      const char *renderer = (const char *)nxgl_gles2_pfn_glGetString(0x1F01);
      const char *version = (const char *)nxgl_gles2_pfn_glGetString(0x1F02);
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(window, &w, &h);
      logPrintf("[gl] GL_RENDERER=%s GL_VERSION=%s %dx%d\n",
                renderer ? renderer : "?", version ? version : "?", w, h);
      nxgl_frame_proof_set_video_context(w, h, SDL_GetCurrentVideoDriver(),
                                         renderer, version);
    }
  }
  return ctx;
}

static void my_SDL_GL_SwapWindow(SDL_Window *window) {
  GLboolean colorMask[4];
  GLfloat clearColor[4];
  GLint scissorBox[4];
  GLboolean scissorTest;
  GLint curFBO = 0;

  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &curFBO);
  glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
  glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
  scissorTest = glIsEnabled(GL_SCISSOR_TEST);

  if (scissorTest) glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
  glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
  if (scissorTest) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, curFBO);

  /* Prova de imagem continua ANTES do present (pos-swap o backbuffer e'
   * indefinido em GPU tile-based). */
  {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    nxgl_frame_proof_before_present(w, h);
  }
  SDL_GL_SwapWindow(window);
}

static EGLContext my_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list) {
  EGLContext ctx = nxegl_eglCreateContext(dpy, config, share_context, attrib_list);
  logPrintf("[egl] eglCreateContext(dpy=%p, share=%p) -> %p\n", dpy, share_context, ctx);
  return ctx;
}

static EGLSurface my_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
  EGLSurface surf = nxegl_eglCreatePbufferSurface(dpy, config, attrib_list);
  logPrintf("[egl] eglCreatePbufferSurface(dpy=%p) -> %p\n", dpy, surf);
  return surf;
}

static EGLBoolean my_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  EGLBoolean ret = nxegl_eglMakeCurrent(dpy, draw, read, ctx);
  logPrintf("[egl] eglMakeCurrent(draw=%p, read=%p, ctx=%p, tid=%ld) -> %d\n", draw, read, ctx, (long)getpid(), ret);
  return ret;
}

static void my_glCompileShader(GLuint shader) {
  glCompileShader(shader);
  GLint status = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (!status) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    logPrintf("[shader-error] glCompileShader failed: %s\n", log);
  }
}

static void my_glLinkProgram(GLuint program) {
  glLinkProgram(program);
  GLint status = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    char log[1024];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    logPrintf("[program-error] glLinkProgram failed: %s\n", log);
  }
}

/* Builds oficiais recentes vem embrulhados pelo PairIP (protecao da Play
 * Store): a libfmod.so importa `ExecuteProgram` da VM de licenca
 * (libpairipcore.so), que nao existe fora do Android. Sem resolver, o slot
 * fica envenenado e o construtor da lib segfaulta na carga. Stub que retorna
 * sucesso (mesma receita provada no GTA III DE). */
static int g_pairip_calls = 0;
static long stub_ExecuteProgram(void *a, void *b, void *c, void *d,
                                void *e, void *f, void *g, void *h) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
  if (g_pairip_calls < 16)
    logPrintf("[pairip] ExecuteProgram stub call #%d -> 0\n", g_pairip_calls);
  g_pairip_calls++;
  return 0;
}

static DynLibFunction base_symbols[] = {
  // ctype & errno
  { "_ctype_", (uintptr_t)&_ctype_ },
  { "__errno", (uintptr_t)bionic_errno },
  { "__sF", (uintptr_t)__sF },
  { "__system_property_get", (uintptr_t)bionic_system_property_get },

  // android logging & asserts
  { "__android_log_print", (uintptr_t)__android_log_print },
  { "__android_log_write", (uintptr_t)__android_log_write },
  { "__android_log_vprint", (uintptr_t)__android_log_vprint },
  { "__strncpy_chk2", (uintptr_t)__strncpy_chk2 },
  { "__strlen_chk", (uintptr_t)__strlen_chk },
  { "__FD_SET_chk", (uintptr_t)__FD_SET_chk },
  { "__assert2", (uintptr_t)__assert2 },
  { "android_set_abort_message", (uintptr_t)android_set_abort_message },

  // PairIP (builds recentes da Play Store embrulham a libfmod.so)
  { "ExecuteProgram", (uintptr_t)stub_ExecuteProgram },

  // stdio mappings for __sF
  { "fflush", (uintptr_t)bb_fflush },
  { "fprintf", (uintptr_t)bb_fprintf },
  { "vfprintf", (uintptr_t)bb_vfprintf },
  { "fwrite", (uintptr_t)bb_fwrite },
  { "fread", (uintptr_t)bb_fread },
  { "fputs", (uintptr_t)bb_fputs },
  { "fputc", (uintptr_t)bb_fputc },
  { "ungetc", (uintptr_t)bb_ungetc },
  { "feof", (uintptr_t)bb_feof },
  { "ferror", (uintptr_t)bb_ferror },
  { "fileno", (uintptr_t)bb_fileno },
  { "fseek", (uintptr_t)bb_fseek },
  { "ftell", (uintptr_t)bb_ftell },
  { "fgets", (uintptr_t)bb_fgets },
  { "fclose", (uintptr_t)bb_fclose },
  { "getc", (uintptr_t)bb_getc },
  { "putc", (uintptr_t)bb_putc },
  { "setbuf", (uintptr_t)bb_setbuf },
  { "setvbuf", (uintptr_t)bb_setvbuf },

  // zlib
  { "compress", (uintptr_t)compress },
  { "compressBound", (uintptr_t)compressBound },
  { "uncompress", (uintptr_t)uncompress },
  { "deflateInit_", (uintptr_t)deflateInit_ },
  { "deflate", (uintptr_t)deflate },
  { "deflateEnd", (uintptr_t)deflateEnd },
  { "inflateInit_", (uintptr_t)inflateInit_ },
  { "inflateInit2_", (uintptr_t)inflateInit2_ },
  { "inflate", (uintptr_t)inflate },
  { "inflateEnd", (uintptr_t)inflateEnd },
  { "zlibVersion", (uintptr_t)zlibVersion },

  // signals & setjmp
  { "sigaction", (uintptr_t)bionic_sigaction },
  { "signal", (uintptr_t)signal },
  { "sigprocmask", (uintptr_t)sigprocmask },
  { "sigfillset", (uintptr_t)sigfillset },
  { "sigdelset", (uintptr_t)sigdelset },
  { "setjmp", (uintptr_t)setjmp },
  { "longjmp", (uintptr_t)longjmp },
  { "sigsetjmp", (uintptr_t)__sigsetjmp },
  { "siglongjmp", (uintptr_t)siglongjmp },

  // semaphores
  { "sem_init", (uintptr_t)bionic_sem_init },
  { "sem_destroy", (uintptr_t)bionic_sem_destroy },
  { "sem_wait", (uintptr_t)bionic_sem_wait },
  { "sem_trywait", (uintptr_t)bionic_sem_trywait },
  { "sem_post", (uintptr_t)bionic_sem_post },
  { "sem_getvalue", (uintptr_t)bionic_sem_getvalue },

  // dynamic linking
  { "dlopen", (uintptr_t)bionic_dlopen },
  { "dlsym", (uintptr_t)bionic_dlsym },
  { "dlclose", (uintptr_t)bionic_dlclose },
  { "dlerror", (uintptr_t)bionic_dlerror },
  { "dl_iterate_phdr", (uintptr_t)dl_iterate_phdr },

  // pthread bridge
  { "pthread_mutexattr_init", (uintptr_t)b_mutexattr_init },
  { "pthread_mutexattr_destroy", (uintptr_t)b_mutexattr_destroy },
  { "pthread_mutexattr_settype", (uintptr_t)b_mutexattr_settype },
  { "pthread_mutex_init", (uintptr_t)b_mutex_init },
  { "pthread_mutex_destroy", (uintptr_t)b_mutex_destroy },
  { "pthread_mutex_lock", (uintptr_t)b_mutex_lock },
  { "pthread_mutex_trylock", (uintptr_t)b_mutex_trylock },
  { "pthread_mutex_unlock", (uintptr_t)b_mutex_unlock },
  { "pthread_condattr_init", (uintptr_t)b_condattr_init },
  { "pthread_condattr_destroy", (uintptr_t)b_condattr_destroy },
  { "pthread_cond_init", (uintptr_t)b_cond_init },
  { "pthread_cond_destroy", (uintptr_t)b_cond_destroy },
  { "pthread_cond_signal", (uintptr_t)b_cond_signal },
  { "pthread_cond_broadcast", (uintptr_t)b_cond_broadcast },
  { "pthread_cond_wait", (uintptr_t)b_cond_wait },
  { "pthread_cond_timedwait", (uintptr_t)b_cond_timedwait },
  { "pthread_rwlockattr_init", (uintptr_t)b_rwlockattr_init },
  { "pthread_rwlockattr_destroy", (uintptr_t)b_rwlockattr_destroy },
  { "pthread_rwlock_init", (uintptr_t)b_rwlock_init },
  { "pthread_rwlock_destroy", (uintptr_t)b_rwlock_destroy },
  { "pthread_rwlock_rdlock", (uintptr_t)b_rwlock_rdlock },
  { "pthread_rwlock_tryrdlock", (uintptr_t)b_rwlock_tryrdlock },
  { "pthread_rwlock_wrlock", (uintptr_t)b_rwlock_wrlock },
  { "pthread_rwlock_trywrlock", (uintptr_t)b_rwlock_trywrlock },
  { "pthread_rwlock_unlock", (uintptr_t)b_rwlock_unlock },
  { "pthread_once", (uintptr_t)b_once },
  { "pthread_attr_setstacksize", (uintptr_t)b_attr_setstacksize },
  { "pthread_create", (uintptr_t)pthread_create },
  { "pthread_join", (uintptr_t)pthread_join },
  { "pthread_detach", (uintptr_t)pthread_detach },
  { "pthread_self", (uintptr_t)pthread_self },
  { "pthread_equal", (uintptr_t)pthread_equal },
  { "pthread_exit", (uintptr_t)pthread_exit },
  { "pthread_key_create", (uintptr_t)pthread_key_create },
  { "pthread_key_delete", (uintptr_t)pthread_key_delete },
  { "pthread_setspecific", (uintptr_t)pthread_setspecific },
  { "pthread_getspecific", (uintptr_t)pthread_getspecific },

  // AAssetManager bridge
  { "AAssetManager_fromJava", (uintptr_t)AAssetManager_fromJava },
  { "AAssetManager_open", (uintptr_t)AAssetManager_open },
  { "AAsset_read", (uintptr_t)AAsset_read },
  { "AAsset_seek", (uintptr_t)AAsset_seek },
  { "AAsset_close", (uintptr_t)AAsset_close },
  { "AAsset_getLength", (uintptr_t)AAsset_getLength },
  { "AAsset_getRemainingLength", (uintptr_t)AAsset_getRemainingLength },
  { "AAsset_getBuffer", (uintptr_t)AAsset_getBuffer },
  { "AAsset_openFileDescriptor", (uintptr_t)AAsset_openFileDescriptor },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)ANativeWindow_setBuffersGeometry },

  // SDL3 Android shims
  { "SDL_GetAndroidJNIEnv", (uintptr_t)my_SDL_GetAndroidJNIEnv },
  { "SDL_GetAndroidActivity", (uintptr_t)my_SDL_GetAndroidActivity },
  { "SDL_GetAndroidInternalStoragePath", (uintptr_t)my_SDL_GetAndroidInternalStoragePath },

  // SDL3 native functions
  { "SDL_Init", (uintptr_t)SDL_Init },
  { "SDL_Quit", (uintptr_t)SDL_Quit },
  { "SDL_SetHint", (uintptr_t)SDL_SetHint },
  { "SDL_CreateWindow", (uintptr_t)my_SDL_CreateWindow },
  { "SDL_DestroyWindow", (uintptr_t)SDL_DestroyWindow },
  { "SDL_GetWindowFlags", (uintptr_t)SDL_GetWindowFlags },
  { "SDL_GetWindowSizeInPixels", (uintptr_t)SDL_GetWindowSizeInPixels },
  { "SDL_MinimizeWindow", (uintptr_t)SDL_MinimizeWindow },
  { "SDL_GetPrimaryDisplay", (uintptr_t)SDL_GetPrimaryDisplay },
  { "SDL_GetCurrentDisplayOrientation", (uintptr_t)SDL_GetCurrentDisplayOrientation },
  { "SDL_PollEvent", (uintptr_t)my_SDL_PollEvent },
  { "SDL_PushEvent", (uintptr_t)SDL_PushEvent },
  { "SDL_AddEventWatch", (uintptr_t)SDL_AddEventWatch },
  { "SDL_GL_SetAttribute", (uintptr_t)SDL_GL_SetAttribute },
  { "SDL_GL_CreateContext", (uintptr_t)my_SDL_GL_CreateContext },
  { "SDL_GL_DestroyContext", (uintptr_t)SDL_GL_DestroyContext },
  { "SDL_GL_MakeCurrent", (uintptr_t)SDL_GL_MakeCurrent },
  { "SDL_GL_SwapWindow", (uintptr_t)my_SDL_GL_SwapWindow },
  { "SDL_GL_GetProcAddress", (uintptr_t)SDL_GL_GetProcAddress },
  { "SDL_GetJoysticks", (uintptr_t)SDL_GetJoysticks },
  { "SDL_GetJoystickNameForID", (uintptr_t)SDL_GetJoystickNameForID },
  { "SDL_GetNumJoystickAxes", (uintptr_t)SDL_GetNumJoystickAxes },
  { "SDL_GetNumJoystickHats", (uintptr_t)SDL_GetNumJoystickHats },
  { "SDL_IsGamepad", (uintptr_t)SDL_IsGamepad },
  { "SDL_OpenGamepad", (uintptr_t)SDL_OpenGamepad },
  { "SDL_CloseGamepad", (uintptr_t)SDL_CloseGamepad },
  { "SDL_GamepadConnected", (uintptr_t)SDL_GamepadConnected },
  { "SDL_GetGamepadButton", (uintptr_t)SDL_GetGamepadButton },
  { "SDL_GetGamepadAxis", (uintptr_t)SDL_GetGamepadAxis },
  { "SDL_GetGamepadJoystick", (uintptr_t)SDL_GetGamepadJoystick },
  { "SDL_GetSensors", (uintptr_t)SDL_GetSensors },
  { "SDL_GetSensorTypeForID", (uintptr_t)SDL_GetSensorTypeForID },
  { "SDL_OpenSensor", (uintptr_t)SDL_OpenSensor },
  { "SDL_CloseSensor", (uintptr_t)SDL_CloseSensor },
  { "SDL_GetSensorData", (uintptr_t)SDL_GetSensorData },

  // OpenGL ES 2.0 functions
  { "glCompileShader", (uintptr_t)my_glCompileShader },
  { "glLinkProgram", (uintptr_t)my_glLinkProgram },

  // EGL functions
  { "eglBindAPI", (uintptr_t)nxegl_eglBindAPI },
  { "eglCreateContext", (uintptr_t)my_eglCreateContext },
  { "eglCreatePbufferSurface", (uintptr_t)my_eglCreatePbufferSurface },
  { "eglCreateWindowSurface", (uintptr_t)nxegl_eglCreateWindowSurface },
  { "eglDestroyContext", (uintptr_t)nxegl_eglDestroyContext },
  { "eglDestroySurface", (uintptr_t)nxegl_eglDestroySurface },
  { "eglGetConfigAttrib", (uintptr_t)nxegl_eglGetConfigAttrib },
  { "eglGetConfigs", (uintptr_t)nxegl_eglGetConfigs },
  { "eglGetCurrentContext", (uintptr_t)nxegl_eglGetCurrentContext },
  { "eglGetDisplay", (uintptr_t)nxegl_eglGetDisplay },
  { "eglGetProcAddress", (uintptr_t)nxegl_eglGetProcAddress },
  { "eglInitialize", (uintptr_t)nxegl_eglInitialize },
  { "eglMakeCurrent", (uintptr_t)my_eglMakeCurrent },
  { "eglQueryContext", (uintptr_t)nxegl_eglQueryContext },
  { "eglQueryString", (uintptr_t)nxegl_eglQueryString },
  { "eglTerminate", (uintptr_t)nxegl_eglTerminate },
};

static const int base_symbols_count = sizeof(base_symbols) / sizeof(base_symbols[0]);

static DynLibFunction *combine_tables(DynLibFunction *t1, int n1, DynLibFunction *t2, int n2, int *out_total) {
  int total = n1 + n2;
  DynLibFunction *res = (DynLibFunction *)malloc(sizeof(DynLibFunction) * total);
  if (!res) {
    if (out_total) *out_total = 0;
    return NULL;
  }
  memcpy(res, t1, sizeof(DynLibFunction) * n1);
  memcpy(res + n1, t2, sizeof(DynLibFunction) * n2);
  if (out_total) *out_total = total;
  return res;
}

static void resolve_so_path(char *dst, size_t dst_sz, const char *gamedir, const char *soname) {
  snprintf(dst, dst_sz, "%s/lib/arm64-v8a/%s", gamedir, soname);
  if (access(dst, F_OK) == 0) return;
  snprintf(dst, dst_sz, "%s/%s", gamedir, soname);
  if (access(dst, F_OK) == 0) return;
  snprintf(dst, dst_sz, "%s", soname);
}


int main(int argc, char *argv[]) {
  { volatile char c = g_bionic_guard_pad[0]; (void)c; } /* ancora o TLS pad */
  logPrintf("\n=== Beach Buggy Racing (Vector Unit) — NextOS ===\n");
  nxgl_frame_proof_launch_receipt();

  char gamedir[PATH_MAX];
  if (argc > 1 && argv[1] && argv[1][0] != '-') {
    snprintf(gamedir, sizeof(gamedir), "%s", argv[1]);
  } else if (access("gamedata/lib", F_OK) == 0) {
    /* Layout do pacote (nxbootstrap roda com cwd no diretorio do port). */
    snprintf(gamedir, sizeof(gamedir), "gamedata");
  } else {
    snprintf(gamedir, sizeof(gamedir), ".");
  }
  setenv("BB_GAMEDIR", gamedir, 1);

  // Initialize shims
  asset_shim_init(gamedir);
  jni_shim_init(&g_vm, &g_env);

  char path[PATH_MAX];

  // Load Module 1: libc++_shared.so
  resolve_so_path(path, sizeof(path), gamedir, "libc++_shared.so");
  logPrintf("[loader] Loading %s ...\n", path);
  int libcxx_sym_count = 0;
  DynLibFunction *libcxx_syms = NULL;
  if (so_load(path, NULL, 0) != 0) {
    logPrintf("[loader] Warning: Could not load %s via so_load\n", path);
  } else {
    so_relocate();
    so_resolve(base_symbols, base_symbols_count, 0);
    so_execute_init_array();
    so_make_text_executable();
    libcxx_syms = so_snapshot_symbols(&libcxx_sym_count);
    logPrintf("[loader] libc++_shared.so loaded and exported %d symbols\n", libcxx_sym_count);
  }

  int t0_count = 0;
  DynLibFunction *t0 = combine_tables(base_symbols, base_symbols_count, libcxx_syms ? libcxx_syms : base_symbols, libcxx_syms ? libcxx_sym_count : 0, &t0_count);

  // Load Module 2: libfmod.so
  resolve_so_path(path, sizeof(path), gamedir, "libfmod.so");
  logPrintf("[loader] Loading %s ...\n", path);
  if (so_load(path, NULL, 0) != 0) {
    logPrintf("[loader] Error loading %s\n", path);
  } else {
    so_relocate();
    so_resolve(t0, t0_count, 0);
    so_execute_init_array();
    so_make_text_executable();
    logPrintf("[loader] libfmod.so base: %p loaded and relocated OK\n", text_base);
  }

  int fmod_sym_count = 0;
  DynLibFunction *fmod_syms = so_snapshot_symbols(&fmod_sym_count);
  logPrintf("[loader] FMOD exported %d symbols\n", fmod_sym_count);

  uintptr_t fmod_onload = so_find_addr_safe("JNI_OnLoad");
  if (fmod_onload) {
    jint (*onload)(void *vm, void *reserved) = (void *)fmod_onload;
    jint ver = onload(g_vm, NULL);
    logPrintf("[loader] FMOD JNI_OnLoad returned 0x%x\n", ver);
  }

  // Load Module 3: libfmodstudio.so
  resolve_so_path(path, sizeof(path), gamedir, "libfmodstudio.so");
  logPrintf("[loader] Loading %s ...\n", path);
  int t1_count = 0;
  DynLibFunction *t1 = combine_tables(t0, t0_count, fmod_syms, fmod_sym_count, &t1_count);

  if (so_load(path, NULL, 0) != 0) {
    logPrintf("[loader] Error loading %s\n", path);
  } else {
    so_relocate();
    so_resolve(t1, t1_count, 0);
    so_execute_init_array();
    so_make_text_executable();
    logPrintf("[loader] libfmodstudio.so base: %p loaded and relocated OK\n", text_base);
  }

  int fmodstudio_sym_count = 0;
  DynLibFunction *fmodstudio_syms = so_snapshot_symbols(&fmodstudio_sym_count);
  logPrintf("[loader] FMOD Studio exported %d symbols\n", fmodstudio_sym_count);

  // Combine all symbols for libmain.so
  int t2_count = 0;
  DynLibFunction *t2 = combine_tables(t1, t1_count, fmodstudio_syms, fmodstudio_sym_count, &t2_count);

  // Load Module 4: libmain.so (Game Engine)
  resolve_so_path(path, sizeof(path), gamedir, "libmain.so");
  logPrintf("[loader] Loading %s ...\n", path);
  if (so_load(path, NULL, 0) != 0) {
    fatal_error("Failed to load %s", path);
  }
  so_relocate();
  so_resolve(t2, t2_count, 1);
  so_execute_init_array();
  so_make_text_executable();
  logPrintf("[loader] libmain.so base: %p loaded and initialized OK!\n", text_base);

  uintptr_t age_signal = so_find_addr_safe("Java_com_vectorunit_VuAgeHelper_nativeOnAgeSignalNotUnderAge");
  uintptr_t gs_signin = so_find_addr_safe("Java_com_vectorunit_VuGameServicesHelper_onSignInSuccess");
  uintptr_t ad_init = so_find_addr_safe("Java_com_vectorunit_VuAdHelper_nativeInitializationComplete");
  uintptr_t bill_item = so_find_addr_safe("Java_com_vectorunit_VuBillingHelper_addOwnedItem");
  jni_shim_set_callbacks((void *)age_signal, (void *)gs_signin, (void *)ad_init, (void *)bill_item);

  uintptr_t sdl_main_addr = so_find_addr("SDL_main");
  if (!sdl_main_addr) {
    fatal_error("SDL_main symbol not found in libmain.so");
  }
  logPrintf("[loader] SDL_main found at %p -> Starting engine...\n", (void *)sdl_main_addr);

  typedef int (*sdl_main_fn)(int argc, char *argv[]);
  sdl_main_fn game_main = (sdl_main_fn)sdl_main_addr;

  char *game_argv[] = { "beachbuggy", NULL };
  int ret = game_main(1, game_argv);
  logPrintf("[loader] SDL_main returned %d -> Exiting\n", ret);
  nxgl_frame_proof_publish();

  pthread_t d;
  if (pthread_create(&d, NULL, exit_deadline_thread, NULL) == 0) {
    pthread_detach(d);
  }
  _exit(ret);
  return ret;
}
