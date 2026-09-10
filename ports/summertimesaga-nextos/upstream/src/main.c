// Summertime Saga (Ren'Py/SDL2 Android) so-loader for NextOS/Mali.
//
// librenpython.so embeds Ren'Py 8.5.x + SDL2. We load it, resolve Android
// imports against host libc/GLES/OpenSLES shims, build a fake JavaVM/JNIEnv,
// feed SDL the screen resolution, then call nativeRunMain() -> SDL_main.
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define SO_NAME "librenpython.so"
#define HEAP_MB 512

// SDL pixel format passed to onNativeResize (RGB888). GL config is chosen by
// SDL/EGL independently; this is just what SDL reports as the display mode.
#define SDL_PIXELFORMAT_RGB888 0x16161804u

typedef int jint;

extern int g_summertime_screen_w;
extern int g_summertime_screen_h;

static jint (*p_JNI_OnLoad)(void *vm, void *reserved);
static void (*p_nativeSetupJNI)(void *env, void *cls);
static void (*p_AudioManager_nativeSetupJNI)(void *env, void *cls);
static void (*p_ControllerManager_nativeSetupJNI)(void *env, void *cls);
static void (*p_PythonSDLActivity_nativeSetEnv)(void *env, void *cls,
                                                void *key, void *value);
static void (*p_onNativeResize)(void *env, void *cls, jint w, jint h, jint fmt,
                                float rate);
static jint (*p_nativeRunMain)(void *env, void *cls, void *library,
                               void *function, void *arg_array);
static int (*p_SDL_main)(int argc, char **argv);

/* CANARY BIONIC fix (proven on Bully/Dysmantle): Android .so is bionic-compiled and
 * reads the stack-guard from tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD). Under glibc
 * that offset lands in a TCB field that changes at runtime -> prologue reads X,
 * epilogue reads Y -> __stack_chk_fail -> abort. Reserving a never-written
 * _Thread_local pad in the loader image shifts the static-TLS layout so
 * tpidr+0x28 falls inside this pad -> stable -> the canary never mismatches.
 * `used` keeps the linker from dropping it; anchored by a volatile read. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

/* Diz a qual mapeamento de /proc/self/maps um endereco pertence (async-safe:
 * so open/read/close e parsing manual). */
static void crash_print_mapping(const char *tag, uintptr_t addr) {
  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0)
    return;
  static char buf[1 << 20];
  size_t total = 0;
  for (;;) {
    ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
    if (n <= 0)
      break;
    total += (size_t)n;
    if (total >= sizeof(buf) - 1)
      break;
  }
  close(fd);
  if (!total)
    return;
  buf[total] = '\0';
  char *line = buf;
  while (line && *line) {
    char *next = strchr(line, '\n');
    if (next)
      *next++ = '\0';
    uintptr_t lo = strtoul(line, NULL, 16);
    char *dash = strchr(line, '-');
    uintptr_t hi = dash ? strtoul(dash + 1, NULL, 16) : 0;
    if (addr >= lo && addr < hi) {
      const char *name = strrchr(line, ' ');
      debugPrintf("  %s em %s (+0x%lx)\n", tag,
                  name && name[1] ? name + 1 : line,
                  (unsigned long)(addr - lo));
      return;
    }
    line = next;
  }
  debugPrintf("  %s sem mapeamento\n", tag);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  uintptr_t fault = (uintptr_t)info->si_addr;
  uintptr_t tb = (uintptr_t)text_base;
  debugPrintf("\n=== CRASH sig=%d addr=%p si_code=%d ===\n", sig,
              info->si_addr, info->si_code);
  if (tb && fault >= tb && fault < tb + text_size)
    debugPrintf("  fault = librenpython+0x%lx\n", (unsigned long)(fault - tb));
#if defined(__aarch64__)
  ucontext_t *u = (ucontext_t *)uc;
  uintptr_t pc = u->uc_mcontext.pc;
  uintptr_t lr = u->uc_mcontext.regs[30];
  debugPrintf("  PC=%p%s\n", (void *)pc,
              (tb && pc >= tb && pc < tb + text_size) ? "" : " (fora de librenpython)");
  if (tb && pc >= tb && pc < tb + text_size)
    debugPrintf("  PC = librenpython+0x%lx\n", (unsigned long)(pc - tb));
  else
    crash_print_mapping("PC", pc);
  if (tb && lr >= tb && lr < tb + text_size)
    debugPrintf("  LR = librenpython+0x%lx\n", (unsigned long)(lr - tb));
  else {
    debugPrintf("  LR=%p\n", (void *)lr);
    crash_print_mapping("LR", lr);
  }
  for (int r = 0; r < 8; r++)
    debugPrintf("  x%d=%p%s", r, (void *)u->uc_mcontext.regs[r],
                r == 3 || r == 7 ? "\n" : "");
  debugPrintf("  x19=%p x20=%p x21=%p sp=%p\n",
              (void *)u->uc_mcontext.regs[19],
              (void *)u->uc_mcontext.regs[20],
              (void *)u->uc_mcontext.regs[21],
              (void *)u->uc_mcontext.sp);
  /* Varredura da stack: com a cadeia de fp corrompida (zeros), qualquer
   * palavra da stack que caia dentro do texto do librenpython e um retorno
   * provavel -> backtrace manual. */
  {
    uintptr_t *w = (uintptr_t *)(u->uc_mcontext.sp & ~7UL);
    int found = 0;
    for (int i = -64; i < 4096 && found < 40; i++) {
      uintptr_t v = w[i];
      if (tb && v >= tb + 0xbfc000 && v < tb + text_size) {
        debugPrintf("  stk[%d] = librenpython+0x%lx\n", i,
                    (unsigned long)(v - tb));
        found++;
      }
    }
  }
  /* backtrace pela cadeia de frame-pointer (builds usam
   * -fno-omit-frame-pointer): [x29] = fp anterior, [x29+8] = endereco de
   * retorno. Validamos cada fp contra a stack antes de ler. */
  {
    uintptr_t fp = u->uc_mcontext.regs[29];
    uintptr_t sp = u->uc_mcontext.sp;
    for (int i = 0; i < 12; i++) {
      if (fp < sp || fp - sp > 0x400000 || (fp & 7))
        break;
      uintptr_t ret = ((uintptr_t *)fp)[1];
      uintptr_t next = ((uintptr_t *)fp)[0];
      if (!ret)
        break;
      if (tb && ret >= tb && ret < tb + text_size)
        debugPrintf("  bt#%d = librenpython+0x%lx\n", i,
                    (unsigned long)(ret - tb));
      else {
        debugPrintf("  bt#%d = %p\n", i, (void *)ret);
        crash_print_mapping("   ", ret);
      }
      if (next <= fp)
        break;
      fp = next;
    }
  }
#endif
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
}

/* ======================================================================
 * Self-contained: hooks que antes viviam no launcher .sh agora rodam no
 * binario (igual ao padrao do Dead Space). O .sh fica minimo.
 * ====================================================================== */

/* Instancia unica: mata summertimesaga orfaos (2 disputando a GPU = trava/
 * preto). Antes eram as linhas 46-49 do .sh. */
static void ss_kill_prior_instances(void) {
  pid_t self = getpid();
  DIR *d = opendir("/proc");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    pid_t pid = (pid_t)atoi(e->d_name);
    if (pid <= 0 || pid == self)
      continue;
    char path[64], link[512];
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 0)
      continue;
    link[n] = 0;
    const char *b = strrchr(link, '/');
    b = b ? b + 1 : link;
    if (strcmp(b, "summertimesaga") == 0)
      kill(pid, SIGKILL);
  }
  closedir(d);
}

/* Limpa arquivos de cursor obsoletos no /dev/shm (antes linhas 51-52). */
static void ss_clean_shm(void) {
  unlink("/dev/shm/summertime_vcursor");
  unlink("/dev/shm/summertime_vclick");
  unlink("/dev/shm/summertime_vcursor.tmp");
  unlink("/dev/shm/summertime_vclick.tmp");
  unlink("/dev/shm/summertime_hover");
}

/* Garante os diretorios de estado (antes o mkdir do .sh). O launcher ainda
 * cria logs/ cedo por causa do redirect de log; aqui garantimos o resto. */
static void ss_ensure_dirs(void) {
  const char *home = getenv("HOME");
  if (!home || !*home)
    return;
  char p[1024];
  const char *subs[] = {"saves", "saves/cache", "game", "logs", NULL};
  for (int i = 0; subs[i]; i++) {
    snprintf(p, sizeof(p), "%s/%s", home, subs[i]);
    mkdir(p, 0755);
  }
}

/* Governor performance nos cores: reduz o engasgo do cursor/engine no
 * Amlogic old. Novo hook (o .sh nao fazia isso). Desliga com SS_NO_CPUPERF. */
static void ss_cpu_performance(void) {
  if (getenv("SS_NO_CPUPERF"))
    return;
  for (int i = 0; i < 8; i++) {
    char p[128];
    snprintf(p, sizeof(p),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
    int fd = open(p, O_WRONLY);
    if (fd >= 0) {
      (void)!write(fd, "performance", 11);
      close(fd);
    }
  }
}

static void call_native_setenv(void *env, void *cls, const char *key,
                               const char *value) {
  if (!key || !value)
    return;
  setenv(key, value, 1);
  if (p_PythonSDLActivity_nativeSetEnv) {
    void *j_key = jni_shim_make_jstring(key);
    void *j_value = jni_shim_make_jstring(value);
    p_PythonSDLActivity_nativeSetEnv(env, cls, j_key, j_value);
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  { volatile char c = g_bionic_guard_pad[0]; (void)c; } // anchor TLS pad
  install_crash_handler();
  // Hooks self-contained (antes no .sh): instancia unica + shm + dirs +
  // governor. Rodam antes de qualquer init de video.
  ss_kill_prior_instances();
  ss_clean_shm();
  ss_ensure_dirs();
  ss_cpu_performance();
  debugPrintf("=== Summertime Saga Ren'Py so-loader (NextOS/Mali) ===\n");

  // Video via the DEVICE's SDL2 (egl_shim): it auto-picks the backend — fbdev on
  // Mali-450, KMSDRM/Wayland on R36S — and gives the NATIVE resolution. The
  // game's static SDL renders through our egl_shim (dlopen libEGL -> egl_shim)
  // into this window. One binary works on any device. No hardcoded 720p.
  /*
   * Proven SOR4/Horizon Chase policy: let SDL choose the device backend, but
   * require its EGL/OpenGL-ES path. On Mesa/Panfrost, omitting this hint can
   * return desktop GL even when the hardware supports GLES 3.
   */
  SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
  SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
  /* SS_DEBUG_HEADLESS=1: reproducao de crash de BOOT fora do device (qemu):
   * pula video/GL por completo. Nunca ligado em producao. */
  if (getenv("SS_DEBUG_HEADLESS")) {
    debugPrintf("DEBUG HEADLESS: pulando SDL video/EGL\n");
  } else {
  if (SDL_Init(SDL_INIT_VIDEO) != 0)
    fatal_error("device SDL_Init(VIDEO) failed: %s", SDL_GetError());
  if (egl_shim_create_window() != 0)
    fatal_error("device EGL/GLES initialization failed: %s", SDL_GetError());
  }
  extern int summertime_screen_w, summertime_screen_h;
  g_summertime_screen_w = summertime_screen_w;
  g_summertime_screen_h = summertime_screen_h;
  debugPrintf("Screen: %dx%d (via device SDL2)\n", g_summertime_screen_w,
              g_summertime_screen_h);

  // PortMaster's control.txt exports SDL_VIDEODRIVER=wayland / SDL_AUDIODRIVER=
  // pulseaudio for the DEVICE SDL2 — which already initialized above and grabbed
  // the right backend (wayland/kmsdrm on R36S, fbdev on Mali-450). The game's
  // STATIC SDL, however, only has the "android" backends; with those env vars it
  // would look for wayland/pulseaudio drivers it doesn't have and SDL_Init would
  // fail ("failed to initialize SDL subsystem" -> NULL deref crash). Force the
  // game's SDL to "android" now that the device window exists. Device-agnostic:
  // the device SDL2 keeps whatever it already picked.
  setenv("SDL_VIDEODRIVER", "android", 1);
  setenv("SDL_AUDIODRIVER", "android", 1);
  extern void summertime_audio_init(void);
  summertime_audio_init();

  // Pull libz (and GLES/EGL) into the global symbol scope so so_resolve's
  // dlsym(RTLD_DEFAULT) fallback can resolve inflate/crc32/gl*/egl* etc.
  const char *globlibs[] = {"libz.so.1", "libz.so", "libGLESv2.so",
                            "libGLESv1_CM.so", NULL};
  for (int i = 0; globlibs[i]; i++)
    dlopen(globlibs[i], RTLD_NOW | RTLD_GLOBAL);

  size_t heap_size = (size_t)HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %d MB failed", HEAP_MB);

  if (so_load(SO_NAME, heap, heap_size) < 0)
    fatal_error("so_load(%s) failed", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base,
              text_size, data_base, data_size);

  if (so_relocate() < 0)
    fatal_error("so_relocate failed");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0)
    fatal_error("so_resolve failed");

  so_finalize();
  so_flush_caches();
  so_execute_init_array();

  p_JNI_OnLoad = (void *)so_find_addr_safe("JNI_OnLoad");
  p_nativeSetupJNI =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_nativeSetupJNI");
  p_AudioManager_nativeSetupJNI = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLAudioManager_nativeSetupJNI");
  p_ControllerManager_nativeSetupJNI = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_nativeSetupJNI");
  p_PythonSDLActivity_nativeSetEnv = (void *)so_find_addr_safe(
      "Java_org_renpy_android_PythonSDLActivity_nativeSetEnv");
  p_onNativeResize =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeResize");
  p_nativeRunMain =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_nativeRunMain");
  p_SDL_main = (void *)so_find_addr_safe("SDL_main");

  if (!p_nativeSetupJNI || (!p_SDL_main && !p_nativeRunMain))
    fatal_error("missing SDL entry points (SDL_main/nativeRunMain/nativeSetupJNI)");

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);

  static int fake_activity_class;
  void *cls = &fake_activity_class;

  char cwd[1024];
  if (!getcwd(cwd, sizeof(cwd)))
    snprintf(cwd, sizeof(cwd), ".");
  const char *game_dir = getenv("ANDROID_APP_PATH");
  if (!game_dir || !game_dir[0])
    game_dir = cwd;
  const char *assets_dir = getenv("SUMMERTIME_ASSETS");
  if (!assets_dir || !assets_dir[0])
    assets_dir = "./assets";
  const char *game_assets_dir = getenv("SUMMERTIME_GAME_DIR");
  const char *common_dir = getenv("SUMMERTIME_COMMON_DIR");
  const char *log_dir = getenv("SUMMERTIME_LOG_DIR");
  /*
   * ANDROID_APK is not used by the filesystem-backed android.apk adapter, but
   * keep forwarding it when a launcher or diagnostic session supplied the
   * actual source. Never manufacture a versioned filename here: NXExtract
   * deliberately accepts renamed packages and compatible future builds.
   */
  const char *apk_path = getenv("SUMMERTIME_APK");

  call_native_setenv(fake_env, cls, "ANDROID_PRIVATE", game_dir);
  call_native_setenv(fake_env, cls, "ANDROID_PUBLIC", game_dir);
  call_native_setenv(fake_env, cls, "ANDROID_OLD_PUBLIC", game_dir);
  call_native_setenv(fake_env, cls, "ANDROID_ARGUMENT", game_dir);
  call_native_setenv(fake_env, cls, "ANDROID_APP_PATH", game_dir);
  if (apk_path && apk_path[0])
    call_native_setenv(fake_env, cls, "ANDROID_APK", apk_path);
  call_native_setenv(fake_env, cls, "SUMMERTIME_ASSETS", assets_dir);
  if (game_assets_dir && game_assets_dir[0])
    call_native_setenv(fake_env, cls, "SUMMERTIME_GAME_DIR", game_assets_dir);
  if (common_dir && common_dir[0])
    call_native_setenv(fake_env, cls, "SUMMERTIME_COMMON_DIR", common_dir);
  if (log_dir && log_dir[0])
    call_native_setenv(fake_env, cls, "SUMMERTIME_LOG_DIR", log_dir);
  call_native_setenv(fake_env, cls, "RENPY_PLATFORM", "android");

  debugPrintf("JNI_OnLoad...\n");
  if (p_JNI_OnLoad)
    p_JNI_OnLoad(fake_vm, NULL);

  debugPrintf("nativeSetupJNI...\n");
  p_nativeSetupJNI(fake_env, cls);
  if (p_AudioManager_nativeSetupJNI)
    p_AudioManager_nativeSetupJNI(fake_env, cls);
  if (p_ControllerManager_nativeSetupJNI)
    p_ControllerManager_nativeSetupJNI(fake_env, cls);

  if (p_onNativeResize) {
    debugPrintf("onNativeResize(%d,%d)...\n", g_summertime_screen_w,
                g_summertime_screen_h);
    p_onNativeResize(fake_env, cls, g_summertime_screen_w, g_summertime_screen_h,
                     SDL_PIXELFORMAT_RGB888, 60.0f);
  }

  extern void summertime_input_start(void *env, void *cls);
  summertime_input_start(fake_env, cls);

  /* O bionic do librenpython instala o handler do debuggerd durante o
   * JNI_OnLoad; ele captura o SIGSEGV real e re-emite via tgkill
   * (si_code=SI_TKILL), escondendo a falha. Reinstalar o NOSSO handler
   * DEPOIS dele garante o registro do fault verdadeiro (licao do SDL3). */
  install_crash_handler();

  jint rc = 0;
  if (p_SDL_main && !getenv("SUMMERTIME_NATIVE_RUNMAIN")) {
    debugPrintf("SDL_main direct ...\n");
    char *main_argv[] = {"summertimesaga", NULL};
    rc = p_SDL_main(1, main_argv);
    debugPrintf("SDL_main returned %d\n", (int)rc);
  } else {
    void *j_lib = jni_shim_make_jstring(SO_NAME);
    void *j_fn = jni_shim_make_jstring("SDL_main");
    debugPrintf("nativeRunMain -> SDL_main ...\n");
    rc = p_nativeRunMain(fake_env, cls, j_lib, j_fn, NULL);
    debugPrintf("nativeRunMain returned %d\n", (int)rc);
  }

  return 0;
}
