/*
 * main.c — Door Kickers: Action Squad 1.2.4 (engine C++ KillHouse,
 * libAndroidEntryPoint.so, NativeActivity PURA aarch64) so-loader p/ NextOS
 * Mali-450 (Utgard, GLES2 via SDL2).
 *
 * Esqueleto = ports/castleofillusion (port FINALIZADO/PUBLICADO, mesmo lineage
 * aarch64: so_util ELF64, android_app 64-bit, canary bionic tpidr+0x28,
 * pthread_bridge).  Shim de OpenSL ES = ports/retry (FINALIZADO + R2).
 *
 * ⚠️ O FLUXO É DO JOGO, não da referência: no COI a glue do NDK está do lado do
 * loader e o loader chama `android_main`.  Aqui é o inverso — a glue está DENTRO
 * da `.so` e o que o Android chama é `ANativeActivity_onCreate`, que instala os
 * ANativeActivityCallbacks e sobe a thread do app sozinha.  Este loader
 * reproduz o que a NativeActivity.java faz, na ordem dela, e nada mais.
 * Medições que sustentam cada passo: MEDIDAS.md.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "nxgl_frame_proof_adapter.h"

#define GAME_SO      "libAndroidEntryPoint.so"
#define GAME_HEAP_MB 384
#define PACKAGE_NAME "com.khg.actionsquad"

volatile uintptr_t g_load_base = 0;
static char **g_process_argv;

extern DynLibFunction coi_overrides[];
extern const int coi_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern DynLibFunction coi_extra[];
extern const int coi_extra_count;
extern DynLibFunction as_overrides[];
extern const int as_overrides_count;

/* stub do caminho de texbake herdado do esqueleto doador (Action Squad não usa
 * cache de bake: sem nome = sem cache-hit, upload normal). */
const char *bk_last_bmp_name(void) { return ""; }

/* 🩹 CANARY BIONIC: a engine lê a stack-guard de tpidr_el0+0x28 (bionic
 * TLS_SLOT_STACK_GUARD).  Sob glibc esse endereço cai no TLS de outra lib e muda
 * em runtime -> __stack_chk_fail falso.  Pad TLS no início do bloco do exe. */
__attribute__((aligned(16))) static _Thread_local char g_bionic_guard_pad[256];

static volatile sig_atomic_t g_exit_signal;

int as_retry_sdl_provider(const char *stage) {
  const char *provider = getenv("ACTIONSQUAD_SDL_PROVIDER");
  struct stat info;
  void *handle;

  if (!provider || !provider[0] || getenv("ACTIONSQUAD_PROVIDER_RETRY") ||
      getenv("SDL_VIDEO_EGL_DRIVER") || getenv("SDL_VIDEO_GL_DRIVER") ||
      getenv("LD_PRELOAD") || !g_process_argv)
    return 0;
  if (lstat(provider, &info) != 0 || !S_ISREG(info.st_mode) ||
      S_ISLNK(info.st_mode))
    return 0;

  handle = dlopen(provider, RTLD_NOW | RTLD_LOCAL);
  if (!handle)
    return 0;
  if (!dlsym(handle, "eglInitialize") || !dlsym(handle, "glGetString")) {
    dlclose(handle);
    return 0;
  }
  dlclose(handle);

  setenv("SDL_VIDEO_EGL_DRIVER", provider, 1);
  setenv("SDL_VIDEO_GL_DRIVER", provider, 1);
  setenv("LD_PRELOAD", provider, 1);
  setenv("ACTIONSQUAD_PROVIDER_RETRY", "1", 1);
  fprintf(stderr,
          "[video] provider recovery retry stage=%s provider=%s\n",
          stage ? stage : "unknown", provider);
  fflush(NULL);
  execv("/proc/self/exe", g_process_argv);
  fprintf(stderr, "[video] provider recovery exec failed: %s\n",
          strerror(errno));
  return 0;
}

/* ---------------- crash / backtrace ---------------- */

static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0)
    return;
  char buf[8192];
  int n;
  char line[400];
  int li = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e;
        char perm[8];
        char path[256];
        path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >=
                3 &&
            a >= s && a < e) {
          const char *base = strrchr(path, '/');
          base = base ? base + 1 : (path[0] ? path : "?");
          snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
          close(fd);
          return;
        }
        li = 0;
      } else
        line[li++] = c;
    }
  close(fd);
}

static void dump_frames(mcontext_t *m, int limit) {
  char r[300];
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < limit && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp;
    uintptr_t next = p[0], lr = p[1];
    if (!lr)
      break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_load_base && lr >= g_load_base)
      fprintf(stderr, " {" GAME_SO "+0x%lx}", (unsigned long)(lr - g_load_base));
    fprintf(stderr, "\n");
    if (next <= fp)
      break;
    fp = next;
  }
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300];
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(__NR_gettid));
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "  PC=%p %s", (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, "  {" GAME_SO "+0x%lx}", (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(m->regs[30], r, sizeof(r));
  fprintf(stderr, "  LR=%p %s", (void *)m->regs[30], r);
  if (g_load_base && m->regs[30] >= g_load_base)
    fprintf(stderr, "  {" GAME_SO "+0x%lx}",
            (unsigned long)(m->regs[30] - g_load_base));
  fprintf(stderr, "\n");
  for (int i = 0; i < 29; i += 3)
    fprintf(stderr, "  x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n", i,
            (unsigned long)m->regs[i], i + 1, (unsigned long)m->regs[i + 1],
            i + 2, (unsigned long)m->regs[i + 2]);
  fprintf(stderr, "  sp=%lx fp=%lx\n", (unsigned long)m->sp,
          (unsigned long)m->regs[29]);
  dump_frames(m, 24);
  fflush(stderr);
  _exit(128 + sig);
}

/* SIGUSR1: dump da pilha SEM sair (sonda de "onde travou"). */
static void bt_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300], name[32] = "?";
  pthread_getname_np(pthread_self(), name, sizeof(name));
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "\n[BT sig=%d tid=%d %s] PC=%p %s", sig,
          (int)syscall(__NR_gettid), name, (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, " {game+0x%lx}", (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  dump_frames(m, 20);
  fflush(stderr);
}

static void exit_signal_handler(int sig) { g_exit_signal = sig; }

static void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);

  struct sigaction sb;
  memset(&sb, 0, sizeof(sb));
  sb.sa_sigaction = bt_handler;
  sb.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &sb, NULL);

  struct sigaction se;
  memset(&se, 0, sizeof(se));
  se.sa_handler = exit_signal_handler;
  sigaction(SIGINT, &se, NULL);
  sigaction(SIGTERM, &se, NULL);
  sigaction(SIGHUP, &se, NULL);
}

/* AS_MAX_SECONDS: teto SÓ de bancada.  Jogo aberto a pedido roda sem teto. */
static void timeout_exit_handler(int sig) {
  (void)sig;
  const char msg[] = "AS_MAX_SECONDS reached; exiting\n";
  write(STDERR_FILENO, msg, sizeof(msg) - 1);
  _exit(124);
}

static void install_timeout_guard(void) {
  const char *v = getenv("AS_MAX_SECONDS");
  if (!v || !*v)
    return;
  int seconds = atoi(v);
  if (seconds <= 0)
    return;
  signal(SIGALRM, timeout_exit_handler);
  alarm((unsigned)seconds);
  fprintf(stderr, "AS_MAX_SECONDS guard armed: %d s\n", seconds);
}

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void preload_device_libs(void) {
  static const char *libs[] = {"libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so",
                               "libOpenSLES.so",   "libm.so.6",    "libdl.so.2",
                               "libz.so.1",        NULL};
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static DynLibFunction *g_base;
static int g_base_n;

static void build_base_table(void) {
  g_base_n = coi_overrides_count + revc_pthread_count + coi_extra_count +
             as_overrides_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  int o = 0;
  /* as_overrides PRIMEIRO: o resolvedor pega a primeira entrada que casa, e as
     nossas (funopen, sysconf do bionic, stat do OpenSLES) têm de vencer. */
  memcpy(g_base + o, as_overrides, sizeof(DynLibFunction) * as_overrides_count);
  o += as_overrides_count;
  /* ABI pthread antes do scaffold gráfico: pthread_attr_setstacksize também
   * existe na tabela Castle, mas lá recebe o objeto glibc direto. No Action o
   * objeto é Bionic 56B e sempre precisa passar pela ponte. */
  memcpy(g_base + o, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
  o += revc_pthread_count;
  memcpy(g_base + o, coi_overrides,
         sizeof(DynLibFunction) * coi_overrides_count);
  o += coi_overrides_count;
  memcpy(g_base + o, coi_extra, sizeof(DynLibFunction) * coi_extra_count);
}

static void load_game_module(const char *path) {
  size_t heap_size = (size_t)GAME_HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    fprintf(stderr, "mmap %d MB falhou\n", GAME_HEAP_MB);
    exit(1);
  }
  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", path, heap,
          GAME_HEAP_MB);
  if (so_load(path, heap, heap_size) < 0) {
    fprintf(stderr, "so_load(%s) falhou\n", path);
    exit(1);
  }
  if (so_relocate() < 0) {
    fprintf(stderr, "so_relocate(%s) falhou\n", path);
    exit(1);
  }
  so_resolve(g_base, g_base_n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  g_load_base = (uintptr_t)text_base;
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", path, text_base,
          text_size, data_base, data_size);
}

/* ------------- fluxo NativeActivity do JOGO (lido do binário) ------------- */

static void lifecycle_pause(void) { SDL_Delay(16); }

static void activity_callback(const char *name,
                              void (*callback)(ANativeActivity *),
                              ANativeActivity *activity) {
  if (!callback)
    return;
  fprintf(stderr, "NativeActivity: %s\n", name);
  callback(activity);
  lifecycle_pause();
}

/*
 * Ordem da NativeActivity do Android: onStart -> onResume e, quando o surface
 * existe, os callbacks de InputQueue/Window/Focus.  A glue DENTRO da .so
 * converte cada um em APP_CMD_* na thread do app; o loader nunca fabrica estado
 * de engine.
 */
static void start_native_activity(struct android_app *app) {
  ANativeActivity *activity = app->activity;
  ANativeActivityCallbacks *cb = activity->callbacks;
  ANativeWindow *window = android_shim_get_window();

  if (!cb) {
    fprintf(stderr, "ANativeActivity_onCreate não instalou callbacks\n");
    exit(1);
  }

  activity_callback("onStart", cb->onStart, activity);
  activity_callback("onResume", cb->onResume, activity);

  if (cb->onInputQueueCreated) {
    fprintf(stderr, "NativeActivity: onInputQueueCreated\n");
    cb->onInputQueueCreated(activity, app->inputQueue);
    lifecycle_pause();
  }
  if (cb->onNativeWindowCreated) {
    fprintf(stderr, "NativeActivity: onNativeWindowCreated (%dx%d)\n",
            coi_screen_w, coi_screen_h);
    cb->onNativeWindowCreated(activity, window);
    lifecycle_pause();
  }
  /* Resize/content/redraw NÃO fazem parte obrigatória da criação. O framework
   * só os emite quando o surface realmente muda ou pede redraw. Fabricá-los
   * aqui fazia a callback principal esperar a thread do app confirmar um
   * APP_CMD_WINDOW_RESIZED enquanto ela ainda estava dentro da inicialização
   * de GFX, antes do primeiro poll: deadlock. */
  if (cb->onWindowFocusChanged) {
    fprintf(stderr, "NativeActivity: onWindowFocusChanged(1)\n");
    cb->onWindowFocusChanged(activity, 1);
    lifecycle_pause();
  }
}

static void stop_native_activity(struct android_app *app) {
  ANativeActivity *activity = app->activity;
  ANativeActivityCallbacks *cb = activity->callbacks;
  ANativeWindow *window = android_shim_get_window();
  if (!cb)
    return;
  if (cb->onWindowFocusChanged)
    cb->onWindowFocusChanged(activity, 0);
  /* onPause primeiro: a engine salva no pause, igual ao Android. */
  if (cb->onPause)
    cb->onPause(activity);
  if (cb->onNativeWindowDestroyed)
    cb->onNativeWindowDestroyed(activity, window);
  if (cb->onInputQueueDestroyed)
    cb->onInputQueueDestroyed(activity, app->inputQueue);
  if (cb->onStop)
    cb->onStop(activity);
}

/*
 * SetHaveController(JNIEnv*, jobject, jint count, jstring name, jint deviceId)
 * — assinatura lida do binário.  SwitchToJoystick descarta o registro enquanto
 * g_appState < 5 (splash/loading), por isso o Java do Android reporta a cada
 * evento do InputManager e nós repetimos: a chamada é idempotente.
 */
typedef void (*set_have_controller_t)(void *env, void *obj, int count,
                                      void *name, int device_id);

int main(int argc, char **argv) {
  /* Emitted before anything can fail: the launch context is what a reader
   * needs to know whether a startup failure says anything about the port. */
  nxgl_frame_proof_launch_receipt();
  (void)argc;
  g_process_argv = argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  install_signal_handlers();
  install_timeout_guard();
  fprintf(stderr,
          "=== DOOR KICKERS: ACTION SQUAD so-loader / NextOS aarch64 "
          "Mali-450 ===\n");

  {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t slot = tp + 0x28, lo = (uintptr_t)g_bionic_guard_pad;
    fprintf(stderr, "TLS guard: slot+0x28=0x%lx pad=[0x%lx..0x%lx] %s\n",
            (unsigned long)slot, (unsigned long)lo,
            (unsigned long)(lo + sizeof(g_bionic_guard_pad)),
            (slot >= lo && slot + 8 <= lo + sizeof(g_bionic_guard_pad))
                ? "DENTRO"
                : "FORA(!)");
  }

  char port_root[4096];
  const char *root_env = getenv("AS_ROOT");
  if (root_env && root_env[0])
    snprintf(port_root, sizeof(port_root), "%s", root_env);
  else if (!getcwd(port_root, sizeof(port_root))) {
    fprintf(stderr, "não consegui determinar a raiz do port\n");
    exit(1);
  }
  setenv("AS_ROOT", port_root, 1);
  /*
   * cwd = RAIZ do port: a engine acha asset por AAssetManager (mapeado em
   * $AS_ROOT/assets/) e grava as opções no relativo "./gamedata/".  Entrar em
   * assets/ só quebraria o lado gravável.
   */
  char gamedata[4200], userdata[4200];
  snprintf(gamedata, sizeof(gamedata), "%s/gamedata", port_root);
  snprintf(userdata, sizeof(userdata), "%s/userdata", port_root);
  mkdir(gamedata, 0755);
  mkdir(userdata, 0755);
  if (chdir(port_root) != 0) {
    fprintf(stderr, "não consegui entrar em %s\n", port_root);
    exit(1);
  }
  fprintf(stderr, "AS_ROOT=%s\n", port_root);

  jni_shim_set_package(PACKAGE_NAME, 0);

  preload_device_libs();
  build_base_table();

  char so_path[4300];
  snprintf(so_path, sizeof(so_path), "%s/" GAME_SO, port_root);
  load_game_module(so_path);

  struct android_app *app = android_shim_init();
  if (!app) {
    fprintf(stderr, "android_shim_init falhou\n");
    exit(1);
  }
  egl_shim_create_window();

  int (*jni_onload)(void *, void *) =
      (int (*)(void *, void *))so_find_addr_safe("JNI_OnLoad");
  void (*activity_oncreate)(ANativeActivity *, void *, size_t) =
      (void (*)(ANativeActivity *, void *, size_t))so_find_addr_safe(
          "ANativeActivity_onCreate");
  set_have_controller_t set_have_controller =
      (set_have_controller_t)so_find_addr_safe(
          "Java_com_khg_actionsquad_MyNativeActivity_SetHaveController");
  int *game_state = (int *)so_find_addr_safe("g_gameState");
  fprintf(stderr,
          "entry: JNI_OnLoad=%p ANativeActivity_onCreate=%p "
          "SetHaveController=%p g_gameState=%p\n",
          (void *)jni_onload, (void *)activity_oncreate,
          (void *)set_have_controller, (void *)game_state);
  if (!activity_oncreate) {
    fprintf(stderr, "ANativeActivity_onCreate não achado em " GAME_SO "\n");
    exit(1);
  }

  if (jni_onload) {
    int v = jni_onload(app->activity->vm, NULL);
    fprintf(stderr, "JNI_OnLoad(vm=%p) -> 0x%x\n", app->activity->vm, v);
  }

  fprintf(stderr, "=== ANativeActivity_onCreate ===\n");
  activity_oncreate(app->activity, NULL, 0);
  fprintf(stderr, "=== onCreate retornou (glue do jogo no ar) ===\n");
  struct android_app *native_app =
      (struct android_app *)app->activity->instance;
  if (!native_app || native_app == app) {
    fprintf(stderr, "NativeActivity não publicou o android_app nativo\n");
    exit(1);
  }
  fprintf(stderr, "NativeActivity: android_app nativo=%p scaffold=%p\n",
          (void *)native_app, (void *)app);
  android_shim_bind_app(native_app);
  start_native_activity(app);

  uint64_t next_report = monotonic_ns();
  int reports = 0;
  int controller_reported_in_live_state = 0;
  void *pad_jstring = NULL;
  int pad_id = 0;
  while (!native_app->destroyRequested && !g_exit_signal) {
    uint64_t now = monotonic_ns();
    if (set_have_controller && !controller_reported_in_live_state &&
        now >= next_report) {
      if (!pad_jstring) {
        const char *pad = android_shim_get_gamepad_name();
        if (pad) {
          pad_jstring = jni_shim_make_string(pad);
          pad_id = android_shim_get_gamepad_id();
          fprintf(stderr, "MyNativeActivity: pad \"%s\" id=%d\n", pad, pad_id);
        }
      }
      if (pad_jstring) {
        set_have_controller(app->activity->env, app->activity->clazz, 1,
                            pad_jstring, pad_id);
        reports++;
        /* O Java real chama SetHaveController quando a lista do InputManager
         * muda, não a cada segundo para sempre. Chamadas anteriores ao estado
         * 5 são descartadas pela engine; a primeira chamada em MAINMENU ou
         * adiante é a notificação válida e deve ser a última até hotplug. */
        if (game_state && *game_state >= 5) {
          android_shim_expect_controller_confirmation();
          controller_reported_in_live_state = 1;
          fprintf(stderr,
                  "MyNativeActivity: controller reported once at gameState=%d\n",
                  *game_state);
        }
      }
      next_report = now + 1000000000ULL;
    }
    SDL_Delay(4);
  }
  fprintf(stderr, "NativeActivity: %d reports de pad\n", reports);

  if (g_exit_signal)
    fprintf(stderr, "NativeActivity: sinal %d pediu saída\n", g_exit_signal);
  stop_native_activity(app);
  /* Save saiu no onPause; nunca desmontar o contexto GL (regra da casa). */
  _exit(0);
}
