/* main.c -- KOTOR (Aspyr / Odyssey, armeabi-v7a) so-loader for Mali-450.
 *
 * Unlike the GLSurfaceView LEGO ports, KOTOR is a standard SDL2 game: its entry
 * is SDL_main and it owns the window/GL/event/audio loop through SDL2. We load
 * libKOTOR.so + its native deps (libandroid_port, libfmod, libfreetype,
 * libLzmaLib, libminiz, libhidapi) as bionic modules, resolve them against our
 * shim table + the device's NATIVE SDL2/GLESv2/EGL (dlsym fallback), reproduce
 * the Aspyr Java handshake (mountObb/mountPatchObb), then call SDL_main.
 *
 * libSDL2.so and libstub.so (Play DRM) from the APK are NOT loaded: SDL is the
 * device-native build, and DRM is bypassed by not running the dex.
 *
 * MIT license. See LICENSE.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "jni_fake.h"
#include "softfp_shim.h"
#include "kotor_framework.h"

/* the shim tables */
extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;
extern void update_imports(void);
extern void exidx_register(uintptr_t lo, uintptr_t hi, uintptr_t exidx, int count);
extern void kotor_set_android_compressed_tex(void *function);
extern void kotor_set_win32_set_last_error(void *function);
extern void kotor_bind_fmod_api(void *init_fn, void *set_output_fn,
                                void *get_output_fn);
extern void kotor_bind_fmod_audio_system_api(
    void *create_sound, void *release_sound, void *play_sound,
    void *get_sound_length, void *get_sound_sample_rate, void *create_stream,
    void *close_stream, void *play_stream, void *get_stream_length,
    void *get_is_channel_playing, void *get_channel_position);
extern void kotor_install_fmod_audio_system_hooks(
    DynLibFunction *functions, int count);

/* image range of the main module (libKOTOR) for the crash reporter */
static uintptr_t g_kotor_lo = 0, g_kotor_hi = 0;

/* combined resolution table: base shims + libm softfp + each module's exports */
static DynLibFunction *g_comb;
static int g_comb_n;
typedef int (*jni_onload_fn)(void *vm, void *reserved);

static void comb_append(DynLibFunction *tbl, int n) {
  g_comb = realloc(g_comb, sizeof(DynLibFunction) * (g_comb_n + n));
  memcpy(g_comb + g_comb_n, tbl, sizeof(DynLibFunction) * n);
  g_comb_n += n;
}

/* ---- console / desligamento ------------------------------------------------
 *
 * O SDL2, no caminho KMSDRM, poe o teclado do console em K_OFF e so' restaura
 * por `atexit(kbd_cleanup_atexit)` e pelos handlers de sinal fatal dele
 * (SDL_evdev_kbd.c).  A lista de sinais fatais da SDL nao inclui SIGTERM --
 * "Handlers for SIGTERM and SIGINT are installed in SDL_QuitInit", e la' o
 * SIGTERM apenas POSTA um SDL_QUIT.  Resultado: processo morto por SIGKILL, ou
 * pendurado no teardown, ou saindo por `_exit()` deixa o console em K_OFF e o
 * aparelho inteiro para de responder -- inclusive o botao de power.
 *
 * Nenhum port nosso tinha isso (varredura de 07/08/2026).  E' justamente o
 * furo que a receita de `_exit` do ports/chrono abriria aqui, porque `_exit`
 * pula o atexit da SDL.  Entao: restaurar o console e' NOSSO, e roda antes de
 * qualquer saida.
 *
 * So' ioctl: e' seguro chamar de handler de sinal.
 */
static int console_restore(void) {
  /* NAO usar NULL como terminador: o primeiro candidato e' um getenv, e quando
   * CUR_TTY nao existe ele JA' e' NULL -- o laco terminava na entrada zero e
   * nada era restaurado.  Medido no Mali-450: "console devolvido em 0 tty(s)"
   * com o console em K_OFF do lado de fora.  Tamanho fixo + pular vazios. */
  const char *candidates[] = {getenv("CUR_TTY"), "/dev/tty0", "/dev/tty1",
                              "/dev/console"};
  int restored = 0;
  for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
    if (!candidates[i] || !candidates[i][0])
      continue;
    int fd = open(candidates[i], O_RDWR | O_NOCTTY);
    if (fd < 0)
      continue;
    /* Abrir NAO e' prova de nada: CUR_TTY costuma apontar para o pty da
     * sessao (ssh) ou para um /dev/tty qualquer, onde o open passa e o ioctl
     * devolve ENOTTY.  Medido no Mali-450: a versao que dava `return` no
     * primeiro open bem-sucedido saia sem NUNCA tocar no /dev/tty0, e o
     * console ficava em K_OFF do mesmo jeito.  So' o ioctl conta, e todos os
     * candidatos que aceitarem sao restaurados (a SDL mexe em um, mas nao
     * custa nada acertar os outros). */
    if (ioctl(fd, KDSKBMODE, K_UNICODE) == 0) {
      ioctl(fd, KDSETMODE, KD_TEXT);
      restored++;
    }
    close(fd);
  }
  return restored;
}

/* Keep an immutable descriptor for the exact running ELF.  On CFWs where the
 * game user cannot issue KDSKBMODE, a passwordless sudo helper executes this
 * same inode through the still-live parent's /proc fd.  This replaces the
 * legacy run.sh postamble without trusting argv[0] or a mutable ROM path. */
static int g_console_self_fd = -1;
static int g_console_restore_attempted;

static void console_restore_helper_init(void) {
  struct stat status;
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd >= 0 && fstat(fd, &status) == 0 && S_ISREG(status.st_mode)) {
    g_console_self_fd = fd;
    return;
  }
  if (fd >= 0)
    close(fd);
}

static const char *console_sudo_path(void) {
  static const char *const candidates[] = {
      "/usr/bin/sudo", "/bin/sudo", "/usr/local/bin/sudo"};
  size_t index;
  for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index)
    if (access(candidates[index], X_OK) == 0)
      return candidates[index];
  return NULL;
}

static int console_restore_owned(void) {
  const char *sudo_path;
  char self_fd_path[64];
  pid_t child;
  int restored;
  int status;

  if (g_console_restore_attempted)
    return 0;
  g_console_restore_attempted = 1;
  restored = console_restore();
  if (restored > 0 || geteuid() == 0 || g_console_self_fd < 0)
    return restored;

  sudo_path = console_sudo_path();
  if (!sudo_path)
    return 0;
  if (snprintf(self_fd_path, sizeof(self_fd_path), "/proc/%ld/fd/%d",
               (long)getpid(), g_console_self_fd) <= 0)
    return 0;
  child = fork();
  if (child < 0)
    return 0;
  if (child == 0) {
    char *const arguments[] = {(char *)sudo_path, (char *)"-n",
                               self_fd_path, (char *)"--restore-console",
                               NULL};
    execv(sudo_path, arguments);
    _exit(127);
  }
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      return 0;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : 0;
}

/* atexit() so' aceita void(void) */
static void console_restore_atexit(void) { (void)console_restore_owned(); }

/* Saida terminal, sempre com o console devolvido ao dono. */
static void kotor_exit_now(int status) {
  (void)console_restore_owned();
  fflush(NULL);
  _exit(status);
}


/* Depois que a parte segura terminou, um SIGSEGV/SIGBUS no desmonte das
 * threads da engine nao pode virar "o jogo quebrou": receita do
 * ports/chrono (ct_platform.c), medida no R36S. */
static volatile sig_atomic_t g_shutting_down;
static volatile sig_atomic_t g_sdl_main_returned;

static void shutdown_fault(int sig) {
  const char msg[] = "SHUTDOWN: falha no teardown; saindo limpo\n";
  ssize_t ignored = write(2, msg, sizeof msg - 1);
  (void)ignored;
  (void)sig;
  kotor_exit_now(0);
}

/* Criada no arranque e dormindo o tempo todo: um handler de sinal nao pode
 * chamar pthread_create nem sigaction, entao quem faz o trabalho e' esta
 * thread, e o handler so' levanta a flag. */
static void *shutdown_watchdog(void *arg) {
  (void)arg;
  const struct timespec tick = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
  while (!g_shutting_down)
    nanosleep(&tick, NULL);

  /* Primeiro o caminho LIMPO, o mesmo do SELECT+START: pedir ao jogo que
   * feche.  Ate' a 1.0.3 quem fazia isto era o handler de SIGTERM da propria
   * SDL; como agora o nosso handler e' instalado antes, a SDL nao instala o
   * dela (ela so' assume o sinal quando a disposicao ainda e' SIG_DFL) e o
   * SDL_QUIT teria deixado de existir.  SDL_PushEvent e' thread-safe. */
  if (!g_sdl_main_returned) {
    SDL_Event quit;
    memset(&quit, 0, sizeof quit);
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
  }

  /* daqui pra frente, falha de teardown vira saida limpa (receita do chrono) */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = shutdown_fault;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);

  long seconds = 8;
  const char *env = getenv("KOTOR_SHUTDOWN_DEADLINE");
  if (env && atoi(env) > 0)
    seconds = atoi(env);
  const struct timespec deadline = {.tv_sec = seconds, .tv_nsec = 0};
  nanosleep(&deadline, NULL);

  const char msg[] = "SHUTDOWN: prazo estourado; encerrando o processo\n";
  ssize_t ignored = write(2, msg, sizeof msg - 1);
  (void)ignored;
  kotor_exit_now(0);
  return NULL;
}

static void install_shutdown_watchdog(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, shutdown_watchdog, NULL) == 0)
    pthread_detach(thread);
  else
    debugPrintf("watchdog de desligamento indisponivel (pthread_create)\n");
}

/* Chamado quando o jogo ja' devolveu o controle (SDL_main retornou) ou quando
 * o frontend pediu para sair.  A parte segura ja' rodou; daqui pra frente o
 * unico objetivo e' sair do caminho sem deixar display, audio ou console
 * presos.  Uma atribuicao a sig_atomic_t: chamavel de handler de sinal. */
static void start_shutdown_deadline(void) { g_shutting_down = 1; }

/* SIGTERM/SIGINT: o frontend pediu para fechar.  A SDL so' posta um SDL_QUIT,
 * que o jogo pode nunca consumir se ja' estiver pendurado -- por isso o prazo
 * comeca a contar aqui. */
static void terminate_handler(int sig) {
  (void)sig;
  start_shutdown_deadline();
}

static void install_terminate_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = terminate_handler;
  sigemptyset(&sa.sa_mask);
  /* Handler PERSISTENTE de proposito.  Com SA_RESETHAND, o segundo SIGTERM --
   * que o launcher manda naturalmente quando a trap de EXIT roda depois da
   * trap de TERM -- caia no SIG_DFL e matava o processo no meio do desmonte:
   * medido no .73, status 143 em vez de 0.  Ficar sem SIG_DFL e' seguro
   * porque o watchdog garante a saida dentro do prazo. */
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
}

/* ---- crash handler (armhf sigcontext) ------------------------------------- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr;

  /* FMOD's non-logging Android build uses raise/tgkill(SIGBUS/SIGSEGV) as a
   * debugger assertion.  SI_USER/SI_TKILL have si_code <= 0 and are distinct
   * from actual alignment/address faults (BUS_ADRALN/BUS_ADRERR and
   * SEGV_MAPERR/SEGV_ACCERR, all positive).  Continue only the deliberate
   * signal; real memory faults still take the fatal path below. */
  if ((sig == SIGBUS || sig == SIGSEGV) && info && info->si_code <= 0 &&
      !getenv("KOTOR_NO_ASSERT_IGNORE")) {
    static unsigned int sent_asserts;
    if (sent_asserts++ < 8)
      fprintf(stderr,
              "[FMOD ASSERT] sent signal %d (si_code=%d) ignored at pc=%p\n",
              sig, info->si_code, (void *)pc);
    return;
  }

  fprintf(stderr, "\n=== CRASH sig=%d fault=%p pc=%p lr=%p ===\n", sig,
          info ? info->si_addr : NULL, (void *)pc, (void *)lr);
  if (pc >= g_kotor_lo && pc < g_kotor_hi)
    fprintf(stderr, "PC in libKOTOR +0x%lx\n", (unsigned long)(pc - g_kotor_lo));
  if (lr >= g_kotor_lo && lr < g_kotor_hi)
    fprintf(stderr, "LR in libKOTOR +0x%lx\n", (unsigned long)(lr - g_kotor_lo));
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1, (unsigned long)m->arm_r2,
          (unsigned long)m->arm_r3, (unsigned long)m->arm_r4, (unsigned long)m->arm_r5);
  uintptr_t sp = m->arm_sp; int n = 0;
  for (uintptr_t a = sp; a < sp + 0x3000 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= g_kotor_lo && v < g_kotor_hi) { fprintf(stderr, "  libKOTOR+0x%lx\n", (unsigned long)(v - g_kotor_lo)); n++; }
  }
  fprintf(stderr, "=== END CRASH ===\n");
  fflush(stderr);
  kotor_exit_now(139); /* nunca sair sem devolver o console */
}
static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
}

/* ---- one module: load into its own RWX heap, resolve, init, snapshot ------ */
static int load_module(const char *name, int heap_mb, int snapshot) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %s failed\n", name); return -1; }
  debugPrintf("--- %s (heap %p, %d MB) ---\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load %s failed\n", name); return -1; }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate %s failed\n", name); return -1; }
  if (so_resolve(g_comb, g_comb_n, 0) < 0) { fprintf(stderr, "so_resolve %s failed\n", name); return -1; }

  /* register this module's exidx for the C++ unwinder */
  uintptr_t lo, hi, exidx; int ecount;
  if (so_current_exidx(&lo, &hi, &exidx, &ecount) == 0)
    exidx_register(lo, hi, exidx, ecount);
  if (!strcmp(name, SO_NAME)) { g_kotor_lo = lo; g_kotor_hi = hi; }

  so_finalize();
  so_flush_caches();
  so_execute_init_array();   /* static C++ constructors (ObbFile, FMod, ...) */

  /* Android's native loader invokes a library's JNI_OnLoad immediately after
   * its ELF constructors, passing the process JavaVM.  FMOD stores that VM and
   * resolves its org/fmod classes here; omitting this native lifecycle step
   * leaves the Android output driver with zero/invalid device metadata. */
  uintptr_t jni_onload_addr = so_find_addr_safe("JNI_OnLoad");
  if (jni_onload_addr) {
    int version = ((jni_onload_fn)jni_onload_addr)(fake_vm, NULL);
    debugPrintf("%s: JNI_OnLoad(%p) -> 0x%x\n", name, fake_vm, version);
    if (version <= 0) {
      fprintf(stderr, "JNI_OnLoad failed for %s (0x%x)\n", name, version);
      return -1;
    }
  }

  if (!strcmp(name, "libfmod.so")) {
    kotor_bind_fmod_api(
        (void *)so_find_addr_safe("_ZN4FMOD6System4initEijPv"),
        (void *)so_find_addr_safe(
            "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE"),
        (void *)so_find_addr_safe(
            "_ZN4FMOD6System9getOutputEP15FMOD_OUTPUTTYPE"));
  }

  if (snapshot) {
    int n = 0;
    DynLibFunction *t = so_snapshot_symbols(&n);
    if (t && n > 0) { comb_append(t, n); debugPrintf("%s: +%d exported symbols\n", name, n); }
  }
  return 0;
}

/* ---- screen geometry ------------------------------------------------------
 * O drawable REAL vence a geometria presumida (video-backend.md). Ordem:
 *   1. KOTOR_W/KOTOR_H  - override manual, so para diagnostico;
 *   2. conector DRM conectado (KMSDRM: R36S e demais RK3326/Panfrost);
 *   3. modo do fb0 (fbdev: Amlogic Mali-450).
 *      Le "modes"/"mode", NUNCA "virtual_size": em fbdev com double buffer o
 *      virtual_size vem com a altura dobrada (as duas metades do pan).
 *   4. fallback compilado, so se nada acima responder.
 * O valor usado e sempre logado uma vez, com a origem.                       */
static int parse_mode_line(const char *s, int *w, int *h) {
  /* aceita "640x480p60", "U:640x480p-0", "1280x720" */
  while (*s && *s != ':' && (*s < '0' || *s > '9')) s++;
  if (*s == ':') s++;
  int a = 0, b = 0;
  while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
  if (*s != 'x' || a <= 0) return 0;
  s++;
  while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
  if (b <= 0) return 0;
  *w = a; *h = b;
  return 1;
}

static int read_mode_file(const char *path, int *w, int *h) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[128];
  int ok = fgets(line, sizeof line, f) && parse_mode_line(line, w, h);
  fclose(f);
  return ok;
}

static int probe_drm(int *w, int *h) {
  DIR *d = opendir("/sys/class/drm");
  if (!d) return 0;
  struct dirent *e;
  int ok = 0;
  while (!ok && (e = readdir(d))) {
    if (strncmp(e->d_name, "card", 4) || !strchr(e->d_name, '-')) continue;
    char p[256], st[32] = {0};
    snprintf(p, sizeof p, "/sys/class/drm/%s/status", e->d_name);
    FILE *f = fopen(p, "r");
    if (!f) continue;
    if (!fgets(st, sizeof st, f)) st[0] = 0;
    fclose(f);
    if (strncmp(st, "connected", 9)) continue;
    snprintf(p, sizeof p, "/sys/class/drm/%s/modes", e->d_name);
    ok = read_mode_file(p, w, h);
  }
  closedir(d);
  return ok;
}

static void detect_screen_geometry(void) {
  const char *w = getenv("KOTOR_W"), *h = getenv("KOTOR_H");
  int dw = 0, dh = 0;

  if (w && h && atoi(w) > 0 && atoi(h) > 0) {
    screen_width = atoi(w); screen_height = atoi(h);
    debugPrintf("screen: %dx%d (override KOTOR_W/KOTOR_H)\n",
                screen_width, screen_height);
    return;
  }
  if (probe_drm(&dw, &dh)) {
    screen_width = dw; screen_height = dh;
    debugPrintf("screen: %dx%d (conector DRM)\n", screen_width, screen_height);
    return;
  }
  if (read_mode_file("/sys/class/graphics/fb0/modes", &dw, &dh) ||
      read_mode_file("/sys/class/graphics/fb0/mode",  &dw, &dh)) {
    screen_width = dw; screen_height = dh;
    debugPrintf("screen: %dx%d (fb0)\n", screen_width, screen_height);
    return;
  }
  debugPrintf("screen: %dx%d (fallback compilado; drawable real nao respondeu)\n",
              screen_width, screen_height);
}

/* ---- entry ---------------------------------------------------------------- */
typedef int  (*sdl_main_fn)(int, char **);
typedef void (*mount_fn)(void *env, void *obj, void *jstr);
typedef void (*lifecycle_fn)(void *env, void *obj);

int main(int argc, char *argv[]) {
  /* Modo de servico: devolver o console e sair.  Existe porque o ioctl
   * KDSKBMODE exige privilegio -- medido na ArkOS, o jogo rodando como usuario
   * comum leva EPERM e nao consegue se restaurar. O processo principal usa
   * este mesmo modo por sudo -n quando necessario, sem wrapper intermediario.
   * E' mais portavel que depender do `kbd_mode` (que recusa sair de K_OFF sem
   * -f e nao toca no KDSETMODE). */
  if (argc > 1 && strcmp(argv[1], "--restore-console") == 0) {
    const int n = console_restore();
    printf("console devolvido em %d tty(s)\n", n);
    return n > 0 ? 0 : 1;
  }
  (void)argc; (void)argv;
  console_restore_helper_init();
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  install_terminate_handlers();
  install_shutdown_watchdog();
  /* Rede de seguranca para toda saida normal: o atexit da SDL restaura o
   * teclado do console, mas so' quando o processo chega ao exit() de verdade
   * e so' quando a SDL chegou a inicializar o caminho evdev. */
  atexit(console_restore_atexit);
  debugPrintf("=== Star Wars: KOTOR -> Mali-450 (Linux/SDL2/GLES2) ===\n");

  /* Defaults owned by this exact adapter. Generic firmware selection, ARMHF
   * audio paths and controller database discovery remain nxcompat policy. */
  (void)setenv("KOTOR_HIGH_RES", "1", 0);
  (void)setenv("KOTOR_FMOD_OPENSL", "1", 0);
  if (!getenv("KOTOR_SYSVOL") &&
      (access("/var/run/batocera-pending-volume", F_OK) == 0 ||
       access("/userdata/system/batocera.conf", F_OK) == 0))
    (void)setenv("KOTOR_SYSVOL", "1", 0);

  char game_dir[PATH_MAX];
  const char *configured_game_dir = getenv("NXCOMPAT_GAME_DIR");
  if (!configured_game_dir || configured_game_dir[0] != '/') {
    if (!getcwd(game_dir, sizeof(game_dir))) {
      fprintf(stderr, "FATAL: cannot resolve game directory\n");
      return 1;
    }
    configured_game_dir = game_dir;
  }
  if (kotor_framework_preflight(configured_game_dir) != 0) {
    fprintf(stderr, "FATAL: NextOS framework preflight failed\n");
    return 1;
  }

  detect_screen_geometry();

  struct stat st;
  if (stat(SO_NAME, &st) < 0) { fprintf(stderr, "FATAL: missing %s in cwd\n", SO_NAME); return 1; }

  /* combined table: base shims + libm softfp thunks (module snapshots appended
   * as each dep loads). */
  g_comb_n = (int)dynlib_numfunctions;
  g_comb = malloc(sizeof(DynLibFunction) * (g_comb_n + softfp_table_count() + 8));
  memcpy(g_comb, dynlib_functions, sizeof(DynLibFunction) * g_comb_n);
  g_comb_n += softfp_fill_table(g_comb + g_comb_n);

  update_imports();
  jni_init();

  /* dependency order: leaves first, each becomes a symbol source for the next */
  if (load_module("libLzmaLib.so",     8,  1) < 0 ||
      kotor_framework_android_module_initialized(0u) != 0) return 1;
  if (load_module("libminiz.so",       8,  1) < 0 ||
      kotor_framework_android_module_initialized(1u) != 0) return 1;
  if (load_module("libfreetype.so",    16, 1) < 0 ||
      kotor_framework_android_module_initialized(2u) != 0) return 1;
  if (load_module("libfmod.so",        24, 1) < 0 ||
      kotor_framework_android_module_initialized(3u) != 0 ||
      kotor_framework_android_module_jni(3u) != 0) return 1;
  load_module("libhidapi.so",          8,  1);   /* optional (native SDL handles pads) */
  if (load_module("libandroid_port.so",32, 1) < 0 ||
      kotor_framework_android_module_initialized(4u) != 0) return 1;
  kotor_set_android_compressed_tex(
      (void *)so_find_addr_safe("android_port_glCompressedTexImage2D"));
  /* FindFirstFileA/FindNextFileA fakes reportam fim de enumeracao no
   * last-error do layer Win32 da Aspyr (fix do freeze do Quick Save). */
  kotor_set_win32_set_last_error(
      (void *)so_find_addr_safe("_Z12SetLastErrorm"));
  /*
   * Bind the original Aspyr audio wrapper while libandroid_port is still the
   * current module, then replace only its entries in the combined table.  Its
   * own relocations remain untouched; libKOTOR, loaded next, gets transparent
   * diagnostic wrappers that always delegate to the native implementation.
   */
  kotor_bind_fmod_audio_system_api(
      (void *)so_find_addr_safe(
          "_ZN15FModAudioSystem11CreateSoundEPciPvmii"),
      (void *)so_find_addr_safe("_ZN15FModAudioSystem12ReleaseSoundEi"),
      (void *)so_find_addr_safe("_ZN15FModAudioSystem9PlaySoundEi"),
      (void *)so_find_addr_safe("_ZN15FModAudioSystem14GetSoundLengthEi"),
      (void *)so_find_addr_safe(
          "_ZN15FModAudioSystem18GetSoundSampleRateEi"),
      (void *)so_find_addr_safe(
          "_ZN15FModAudioSystem12CreateStreamEPcP9SDL_RWopsiiiii"),
      (void *)so_find_addr_safe("_ZN15FModAudioSystem11CloseStreamEm"),
      (void *)so_find_addr_safe("_ZN15FModAudioSystem10PlayStreamEmi"),
      (void *)so_find_addr_safe("_ZN15FModAudioSystem15GetStreamLengthEm"),
      (void *)so_find_addr_safe(
          "_ZN15FModAudioSystem19GetIsChannelPlayingEm"),
      (void *)so_find_addr_safe(
          "_ZN15FModAudioSystem26GetChannelPlaybackPositionEm"));
  kotor_install_fmod_audio_system_hooks(g_comb, g_comb_n);
  if (load_module(SO_NAME,             96, 1) < 0 ||
      kotor_framework_android_module_initialized(5u) != 0) return 1;

  debugPrintf("all modules loaded (%d combined symbols)\n", g_comb_n);

  sdl_main_fn sdl_main = (sdl_main_fn)so_find_addr_safe("SDL_main");
  mount_fn mountObb   = (mount_fn)so_find_addr_safe("Java_com_aspyr_kotor_KOTOR_mountObb");
  mount_fn mountPatch = (mount_fn)so_find_addr_safe("Java_com_aspyr_kotor_KOTOR_mountPatchObb");
  lifecycle_fn createMutex = (lifecycle_fn)so_find_addr_safe(
      "Java_com_aspyr_kotor_KOTOR_nativeCreateMutex");
  lifecycle_fn onResume = (lifecycle_fn)so_find_addr_safe(
      "Java_com_aspyr_kotor_KOTOR_nativeOnResume");
  debugPrintf("entry: SDL_main=%p mountObb=%p mountPatchObb=%p "
              "createMutex=%p onResume=%p\n",
              (void *)sdl_main, (void *)mountObb, (void *)mountPatch,
              (void *)createMutex, (void *)onResume);
  if (!sdl_main) { fprintf(stderr, "FATAL: SDL_main not found\n"); return 1; }

  /* seed the Aspyr-SDL surface geometry globals (the game also queries SDL). */
  extern int g_SDL_BufferGeometry_w, g_SDL_BufferGeometry_h;
  g_SDL_BufferGeometry_w = screen_width;
  g_SDL_BufferGeometry_h = screen_height;

  /* we are the game's main; SDL_main will do SDL_Init itself. */
  SDL_SetMainReady();

  /* KOTOR.onCreate() performs this native handshake before delegating to
   * SDLActivity.onCreate().  It creates the UI mutex/condition pair used by
   * native-to-Java callbacks; leaving them NULL can park a live loading loop
   * forever.  Once SDLActivity has a focused surface, KOTOR.onResume() clears
   * the native pause flag before the SDL main thread is allowed to run. */
  if (createMutex) {
    debugPrintf("nativeCreateMutex()\n");
    createMutex(fake_env, (void *)0x41435431);
  }
  if (kotor_framework_android_activity_created() != 0)
    return 1;
  if (onResume) {
    debugPrintf("nativeOnResume()\n");
    onResume(fake_env, (void *)0x41435431);
  }
  if (kotor_framework_android_resumed() != 0)
    return 1;

  /* Aspyr Java handshake: mount the OBBs (ObbFile indexes the zip). */
  if (mountObb && access(OBB_MAIN, R_OK) == 0) {
    debugPrintf("mountObb(%s)\n", OBB_MAIN);
    mountObb(fake_env, (void *)0x41435431, jni_make_string(OBB_MAIN));
  } else {
    debugPrintf("WARN: %s missing or mountObb unresolved\n", OBB_MAIN);
  }
  if (mountPatch && access(OBB_PATCH, R_OK) == 0) {
    debugPrintf("mountPatchObb(%s)\n", OBB_PATCH);
    mountPatch(fake_env, (void *)0x41435431, jni_make_string(OBB_PATCH));
  }

  char *av[] = { (char *)"kotor", NULL };
  debugPrintf("calling SDL_main...\n");
  int rc = 1;
  if (kotor_framework_run_delegated(sdl_main, 1, av, &rc) != 0) {
    fprintf(stderr, "FATAL: delegated SDL_main contract failed\n");
    return 1;
  }
  g_sdl_main_returned = 1;
  debugPrintf("SDL_main returned %d\n", rc);

  /* O jogo saiu: a parte segura acabou.  O desmonte de uma engine Android
   * carregada por so-loader nao tem garantia de terminar -- threads de som, de
   * recurso e do driver GL seguem vivas.  Prazo ligado e saida terminal com o
   * console devolvido; ficar pendurado aqui e' o que deixa o aparelho morto.
   */
  start_shutdown_deadline();
  /* logado para o teste de campo poder PROVAR a devolucao (um `open` que passa
   * num pty nao restaura nada; so' o ioctl conta) */
  debugPrintf("console devolvido em %d caminho(s)\n",
              console_restore_owned());
  kotor_exit_now(rc);
  return rc; /* inalcancavel */
}
