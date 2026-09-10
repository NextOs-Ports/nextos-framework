/*
 * main.c -- Sonic the Hedgehog 4: Episode II (Sega "NN/Ninja" engine + "fox"
 * wrapper, libfox.so, armv7, GLES2) so-loader p/ NextOS armv7 + Mali-450 (fbdev,
 * GLES2 via SDL2).
 *
 * Modelo GLSurfaceView (JNI-driven): a Activity Java dirige a engine. Nós
 * replicamos esse driver aqui chamando os entry points Java_com_mineloader_fox_
 * foxJniLib_*: init -> SetGamePath -> SetLanguageId -> DrawEGLCreated ->
 * loop{ FileProcess, GameProcess, DrawFrame } + input.
 *
 * Framework so_util/egl_shim/jni_shim/imports REUSADO do Shantae (ELF32-ARM).
 * Estudo: ports/sonic4/STUDY.md.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <execinfo.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "egl_shim.h"
#include "jni_shim.h"

#ifdef __aarch64__
#define GAME_SO "lib/arm64-v8a/libfox.so"
#else
#define GAME_SO "lib/armeabi-v7a/libfox.so"
#endif
#define GAME_HEAP_MB 256

/* resolução vem 100% automática do egl_shim (SDL_GL_GetDrawableSize do device);
   sem números fixos aqui — qualquer tela é pega na hora. */

extern DynLibFunction shantae_overrides[];
extern const int shantae_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

volatile uintptr_t g_load_base = 0;
volatile unsigned long sonic_frame_for_imports = 0;
volatile int sonic_game_started = 0;
static volatile int sonic_in_draw_frame = 0;
static int g_fbclear = 0; /* SONIC_FBCLEAR: glClear por frame (fix candidato dos rastros) */
extern int sonic_screen_w, sonic_screen_h; /* resolução real da tela (egl_shim) */
static pthread_t g_main_thread;
static volatile int g_hang_watch_active = 0;
static int g_hang_watch_seconds = 0;
static int env_flag_enabled(const char *name);

/* 🔧 HANDLER DE CRASH (workflow de debug): captura SIGSEGV/ABRT/BUS/ILL/FPE,
   grava backtrace com offset `libfox+0xNNN` (= bt[i]-text_base, resolvível por
   readelf) num `crash.log` PERSISTENTE (append; o log.txt é truncado no relaunch
   pelo launcher, por isso o crash some). Inclui frame atual e se estava no DrawFrame. */
/* 🛡️ chamada protegida do teardown do attract-demo (ver my_ep2_CStartDemo_ReleaseInstance):
   se estamos DENTRO da guarda e vem um SIGSEGV/SIGBUS, recupera via siglongjmp em vez de morrer. */
static sigjmp_buf g_demo_guard_env;
static volatile sig_atomic_t g_demo_guard_active = 0;
static unsigned long g_demo_guard_recovered = 0;
static int g_demoguard_on = 1;                 /* setado em load_module (SONIC_NO_DEMOGUARD desliga) */
/* faixa do destrutor ~CStartDemo (ep2) onde o UAF crasha; epílogo = pop {r4,r5,r6,pc}. */
#define CSD_DTOR_LO 0x4253f8UL
#define CSD_DTOR_HI 0x4254e0UL
#define CSD_DTOR_EPILOGUE 0x4254d8UL
/* Faixas DERIVADAS de símbolo (offsets relativos a text_base) — auto-adaptam entre
 * versões/arquiteturas (v2 armv7 vs v3 arm64). Setadas em load_module a partir dos
 * símbolos ep2::CStartDemo::~ (dtor) e amTexMgrDecRef. 0 = não-derivado (usa #defines
 * do armv7 no fallback). Ver sonic_demoguard_derive_ranges(). */
static unsigned long g_csd_lo = 0, g_csd_hi = 0;          /* dtor ~CStartDemo ep2 */
static unsigned long g_texdec_lo = 0, g_texdec_hi = 0;    /* amTexMgrDecRef (leaf) */

static void sonic_crash_handler(int sig, siginfo_t *si, void *uc) {
  if (g_demo_guard_active && (sig == SIGSEGV || sig == SIGBUS)) {
    g_demo_guard_active = 0;
    g_demo_guard_recovered++;
    siglongjmp(g_demo_guard_env, 1);          /* recupera: pula de volta pro sigsetjmp */
  }
  /* 🛡️ DEMOGUARD GERAL: SIGSEGV/SIGBUS DENTRO do ~CStartDemo (Act Clear->mapa, delete direto...)
     OU do amTexMgrDecRef (teardown de fase pesada) = use-after-free. Recupera retornando via LR
     (pula o resto da função corrompida; vaza o objeto, mas NÃO fecha o jogo). Cobre o que o hook
     do ReleaseInstance/sigsetjmp não pega. Faixas: armv7 usa os #defines (v2); arm64 usa as faixas
     DERIVADAS de simbolo (g_csd e g_texdec, setadas em load_module) - auto-adapta ao v3. */
  if (g_demoguard_on && (sig == SIGSEGV || sig == SIGBUS) && uc && text_base) {
    ucontext_t *u = (ucontext_t *)uc;
    unsigned long newpc = 0, off = 0, lr = 0;
#if defined(__arm__)
    off = u->uc_mcontext.arm_pc - (uintptr_t)text_base;
    lr  = u->uc_mcontext.arm_lr;
    unsigned long csd_lo = g_csd_lo ? g_csd_lo : CSD_DTOR_LO;
    unsigned long csd_hi = g_csd_hi ? g_csd_hi : CSD_DTOR_HI;
    unsigned long tex_lo = g_texdec_lo ? g_texdec_lo : 0x205f20UL;
    unsigned long tex_hi = g_texdec_hi ? g_texdec_hi : 0x205f64UL;
    if (off >= csd_lo && off < csd_hi)
      newpc = (!g_csd_lo && off < 0x425404UL) ? lr
                                              : (uintptr_t)text_base + CSD_DTOR_EPILOGUE;
    else if (off >= tex_lo && off < tex_hi) newpc = lr;
#elif defined(__aarch64__)
    off = u->uc_mcontext.pc - (uintptr_t)text_base;
    lr  = u->uc_mcontext.regs[30];
    /* arm64: recupera retornando via LR (x30). Confiável no amTexMgrDecRef (leaf). No dtor é
       best-effort (o sigsetjmp guard do ReleaseInstance é a camada primária). Só faixas derivadas. */
    if ((g_texdec_lo && off >= g_texdec_lo && off < g_texdec_hi) ||
        (g_csd_lo && off >= g_csd_lo && off < g_csd_hi))
      newpc = lr;
#endif
    if (newpc) {
#if defined(__arm__)
      u->uc_mcontext.arm_pc = newpc;
#elif defined(__aarch64__)
      u->uc_mcontext.pc = newpc;
#endif
      g_demo_guard_recovered++;
      fprintf(stderr, "[DEMOGUARD] SIGSEGV em libfox+0x%lx (in_draw=%d) -> RECUPERADO #%lu\n",
              off, sonic_in_draw_frame, g_demo_guard_recovered);
      return;                                 /* kernel resume no caller -> jogo segue */
    }
    (void)off; (void)lr;
  }
  void *bt[48];
  int n = backtrace(bt, 48);
  /* PC/LR exatos do ucontext — a instrução do crash mesmo sem unwind da libfox. */
  unsigned long pc = 0, lr = 0, sp = 0;
  unsigned long R[13] = {0};
#if defined(__arm__)
  if (uc) { ucontext_t *u = (ucontext_t *)uc;
    pc = u->uc_mcontext.arm_pc; lr = u->uc_mcontext.arm_lr; sp = u->uc_mcontext.arm_sp;
    R[0]=u->uc_mcontext.arm_r0; R[1]=u->uc_mcontext.arm_r1; R[2]=u->uc_mcontext.arm_r2;
    R[3]=u->uc_mcontext.arm_r3; R[4]=u->uc_mcontext.arm_r4; R[5]=u->uc_mcontext.arm_r5;
    R[6]=u->uc_mcontext.arm_r6; R[7]=u->uc_mcontext.arm_r7; R[8]=u->uc_mcontext.arm_r8;
    R[9]=u->uc_mcontext.arm_r9; R[10]=u->uc_mcontext.arm_r10; R[11]=u->uc_mcontext.arm_fp;
    R[12]=u->uc_mcontext.arm_ip; }
#elif defined(__aarch64__)
  if (uc) { ucontext_t *u = (ucontext_t *)uc;
    pc = u->uc_mcontext.pc; lr = u->uc_mcontext.regs[30]; sp = u->uc_mcontext.sp;
    for (unsigned i = 0; i < 13; i++) R[i] = u->uc_mcontext.regs[i]; }
#endif
  uintptr_t tb = (uintptr_t)text_base;
  FILE *fs[2]; fs[0] = stderr; fs[1] = fopen("crash.log", "a");
  for (int k = 0; k < 2; k++) {
    FILE *o = fs[k]; if (!o) continue;
    fprintf(o, "\n==== CRASH sig=%d addr=%p text_base=0x%lx frame=%lu in_draw=%d ====\n",
            sig, si ? si->si_addr : NULL, (unsigned long)tb,
            sonic_frame_for_imports, sonic_in_draw_frame);
    fprintf(o, "  PC=0x%lx libfox+0x%lx   LR=0x%lx libfox+0x%lx   SP=0x%lx\n",
            pc, pc - tb, lr, lr - tb, sp);
#if defined(__aarch64__)
    fprintf(o, "  x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx x4=0x%lx x5=0x%lx x6=0x%lx\n",
            R[0],R[1],R[2],R[3],R[4],R[5],R[6]);
    fprintf(o, "  x7=0x%lx x8=0x%lx x9=0x%lx x10=0x%lx x11=0x%lx x12=0x%lx\n",
            R[7],R[8],R[9],R[10],R[11],R[12]);
#else
    fprintf(o, "  r0=0x%lx r1=0x%lx r2=0x%lx r3=0x%lx r4=0x%lx r5=0x%lx r6=0x%lx\n",
            R[0],R[1],R[2],R[3],R[4],R[5],R[6]);
    fprintf(o, "  r7=0x%lx r8=0x%lx r9=0x%lx r10=0x%lx fp=0x%lx ip=0x%lx\n",
            R[7],R[8],R[9],R[10],R[11],R[12]);
#endif
    for (int i = 0; i < n; i++) {
      long off = (long)((uintptr_t)bt[i] - tb);
      fprintf(o, "  #%-2d %p  libfox+0x%lx\n", i, bt[i], off);
    }
    fflush(o);
  }
  if (fs[1]) fclose(fs[1]);
  signal(sig, SIG_DFL);
  raise(sig);
}
static void sonic_install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = sonic_crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
  for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);
}

static void sonic_dump_addr_module(FILE *o, const char *label, unsigned long addr) {
  FILE *m = fopen("/proc/self/maps", "r");
  char line[512];
  if (!m) return;
  while (fgets(line, sizeof line, m)) {
    unsigned long lo = 0, hi = 0, off = 0;
    char perms[8] = {0}, dev[16] = {0}, path[256] = {0};
    unsigned long inode = 0;
    int n = sscanf(line, "%lx-%lx %7s %lx %15s %lu %255[^\n]",
                   &lo, &hi, perms, &off, dev, &inode, path);
    if (n >= 6 && addr >= lo && addr < hi) {
      fprintf(o, "  %s=0x%lx in %s +0x%lx file+0x%lx\n",
              label, addr, n == 7 ? path : "?", addr - lo, off + (addr - lo));
      break;
    }
  }
  fclose(m);
}

static void sonic_hang_signal_handler(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si;
  unsigned long pc = 0, lr = 0, sp = 0;
#if defined(__aarch64__)
  unsigned long regs[31] = {0};
  unsigned long tls_guard = 0, saved_guard = 0;
#endif
#if defined(__aarch64__)
  if (uc) {
    ucontext_t *u = (ucontext_t *)uc;
    pc = u->uc_mcontext.pc;
    lr = u->uc_mcontext.regs[30];
    sp = u->uc_mcontext.sp;
    memcpy(regs, u->uc_mcontext.regs, sizeof(regs));
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (tp > 0x10000)
      tls_guard = *(volatile unsigned long *)(tp + 40);
    if (regs[29] > 0x10000 && regs[29] > sp && regs[29] - sp < 0x10000)
      saved_guard = *(volatile unsigned long *)(regs[29] - 88);
  }
#elif defined(__arm__)
  if (uc) {
    ucontext_t *u = (ucontext_t *)uc;
    pc = u->uc_mcontext.arm_pc;
    lr = u->uc_mcontext.arm_lr;
    sp = u->uc_mcontext.arm_sp;
  }
#endif
  FILE *fs[2]; fs[0] = stderr; fs[1] = fopen("hang.log", "a");
  for (int k = 0; k < 2; k++) {
    FILE *o = fs[k]; if (!o) continue;
    fprintf(o, "\n==== HANG watchdog frame=%lu in_draw=%d active=%d ====\n",
            sonic_frame_for_imports, sonic_in_draw_frame, g_hang_watch_active);
    fprintf(o, "  PC=0x%lx LR=0x%lx SP=0x%lx text_base=0x%lx\n",
            pc, lr, sp, (unsigned long)(uintptr_t)text_base);
#if defined(__aarch64__)
    fprintf(o, "  x19=%lx x20=%lx x21=%lx x22=%lx x23=%lx x24=%lx "
               "x25=%lx x26=%lx x29=%lx\n",
            regs[19], regs[20], regs[21], regs[22], regs[23], regs[24],
            regs[25], regs[26], regs[29]);
    fprintf(o, "  stack_guard current=%016lx saved(fp-88)=%016lx\n",
            tls_guard, saved_guard);
#endif
    sonic_dump_addr_module(o, "PC", pc);
    sonic_dump_addr_module(o, "LR", lr);
    fflush(o);
  }
  if (fs[1]) fclose(fs[1]);
  _exit(124);
}

static void *sonic_hang_watchdog_thread(void *arg) {
  (void)arg;
  unsigned stalled_samples = 0;
  for (;;) {
    unsigned long before = sonic_frame_for_imports;
    sleep(g_hang_watch_seconds > 0 ? g_hang_watch_seconds : 5);
    if (g_hang_watch_active && sonic_frame_for_imports == before) {
      if (++stalled_samples >= 2)
        pthread_kill(g_main_thread, SIGUSR1);
    } else {
      stalled_samples = 0;
    }
  }
  return NULL;
}

static void sonic_install_hang_watchdog(void) {
  const char *e = getenv("SONIC_HANGALARM");
  if (!env_flag_enabled("SONIC_HANGALARM")) return;
  g_hang_watch_seconds = atoi(e);
  if (g_hang_watch_seconds <= 0) g_hang_watch_seconds = 5;
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = sonic_hang_signal_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  pthread_t th;
  int rc = pthread_create(&th, NULL, sonic_hang_watchdog_thread, NULL);
  if (rc == 0) {
    pthread_detach(th);
    fprintf(stderr, "=== HANG watchdog ligado: %d s ===\n", g_hang_watch_seconds);
  } else {
    fprintf(stderr, "AVISO: HANG watchdog nao iniciou (pthread rc=%d)\n", rc);
  }
}

static struct {
  int pending;
  int done;
  unsigned long uid;
  int (*AoStorageLoadIsFinished)(void);
  int (*AoStorageLoadIsSuccessed)(void);
  int (*AoStorageGetError)(void);
  void (*CopyBackupComp)(unsigned long);
  void (*SetSaveEnable)(unsigned long, long);
  void (*DmBuildSysDataFromBackup)(void);
} g_native_save_load;

static struct {
  int ready;
  int built;
  int missing_logged;
  int (*AoStorageLoadIsFinished)(void);
  int (*AoStorageLoadIsSuccessed)(void);
  int (*AoStorageGetError)(void);
  void (*DmBuildSysDataFromBackup)(void);
  void (*UpdateStageUnlockState)(void);
  int (*IsStageUnlocked)(unsigned long, int);
  int (*IsStageClear)(unsigned long, int);
  void *(*SProgressCreateInstance)(unsigned long);
  int (*GetStageUnlockState)(void *);
  int (*GetSsUnlockState)(void *);
  int (*GetEpMetalUnlockState)(void *);
} g_save_bootstrap;

static DynLibFunction *g_base;
static int g_base_n;
static int env_flag_enabled(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
         strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

/* single-instance: mata qualquer outra instância do MESMO binário ANTES de
   inicializar fb/EGL — 2 jogos juntos travam o device. Mesmo método /proc/PID/exe
   do launcher antigo (readlink casa o caminho real; pkill -x/-f não casa pois o
   exe vira ./sonic4), agora no binário p/ deixar o launcher enxuto/padrão. */
static void sonic_kill_other_instances(void) {
  char self_exe[4096];
  ssize_t n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
  if (n <= 0) return;
  self_exe[n] = '\0';
  pid_t me = getpid();
  for (int pass = 0; pass < 2; pass++) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    int killed = 0;
    while ((e = readdir(d))) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      pid_t pid = (pid_t)atoi(e->d_name);
      if (pid <= 0 || pid == me) continue;
      char path[64], tgt[4096];
      snprintf(path, sizeof(path), "/proc/%d/exe", pid);
      ssize_t tn = readlink(path, tgt, sizeof(tgt) - 1);
      if (tn <= 0) continue;
      tgt[tn] = '\0';
      if (strcmp(tgt, self_exe) != 0) continue;
      fprintf(stderr, "=== matando instância anterior pid %d (%s) [%s] ===\n",
              pid, tgt, pass == 0 ? "TERM" : "KILL");
      kill(pid, pass == 0 ? SIGTERM : SIGKILL);
      killed++;
    }
    closedir(d);
    if (!killed) break;       /* 0 outras instâncias -> confirmado, sai */
    usleep(700 * 1000);       /* dá tempo do TERM antes do KILL */
  }
}

/* 🔑 Sessão/runtime FALLBACK: se o frontend (ES) NÃO exportou XDG_RUNTIME_DIR /
   WAYLAND_DISPLAY — acontece em muOS e alguns ROCKNIX — os backends de ÁUDIO
   (pulse/pipewire PRECISAM do runtime-dir; sem ele -> "pw_loop_new can't make
   support.system handle" = MUDO) e de VÍDEO (wayland precisa do socket; sem ele
   -> tela preta) FALHAM. Aqui só APONTAMOS pro que o sistema JÁ criou (não força
   driver nenhum; só preenche se estiver vazio). Em kmsdrm puro (muOS sem wayland)
   não há socket wayland -> WAYLAND_DISPLAY fica vazio -> SDL usa kmsdrm (correto). */
static int dir_ok_rw(const char *p) {
  if (!p || !*p) return 0;
  DIR *d = opendir(p); if (!d) return 0; closedir(d);
  return access(p, W_OK) == 0;    /* precisa ser GRAVAVEL p/ o socket do pipewire/pulse */
}
static void sonic_detect_session_runtime(void) {
  /* Só age se o runtime-dir ATUAL estiver ausente/não-gravável (não mexe num válido da sessão). */
  const char *cur = getenv("XDG_RUNTIME_DIR");
  if (!dir_ok_rw(cur)) {
    char ubuf[64];
    snprintf(ubuf, sizeof(ubuf), "/run/user/%u", (unsigned)getuid());
    const char *cands[] = { "/run/0-runtime-dir", "/var/run/0-runtime-dir",
                            "/run/user/0", "/var/run/user/0", ubuf, NULL };
    const char *chosen = NULL;
    for (int i = 0; cands[i]; i++)
      if (dir_ok_rw(cands[i])) { chosen = cands[i]; break; }
    /* 🔊 muOS/ROCKNIX sem runtime-dir: pipewire/pulse falham ("pw.loop can't make
       support.system handle: No such file or directory") -> sem servidor de som -> cai no
       ALSA cru -> speaker busy -> HDMI (mudo). Se NADA válido existe, CRIA um dir gravável
       0700 p/ o servidor de som conseguir criar o socket. SONIC_NO_RTDIR=1 desliga. */
    static char made[80];
    if (!chosen && !getenv("SONIC_NO_RTDIR")) {
      snprintf(made, sizeof(made), "/tmp/sonic-rt-%u", (unsigned)getuid());
      mkdir(made, 0700);
      if (dir_ok_rw(made)) { chmod(made, 0700); chosen = made; }
    }
    if (chosen) {
      setenv("XDG_RUNTIME_DIR", chosen, 1);
      fprintf(stderr, "=== XDG_RUNTIME_DIR = %s (fallback p/ pipewire/pulse) ===\n", chosen);
    }
  }
  if (!getenv("WAYLAND_DISPLAY")) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    DIR *d = rt ? opendir(rt) : NULL;
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        if (strncmp(e->d_name, "wayland-", 8) == 0 && !strstr(e->d_name, ".lock")) {
          setenv("WAYLAND_DISPLAY", e->d_name, 1);
          fprintf(stderr, "=== WAYLAND_DISPLAY fallback = %s ===\n", e->d_name);
          break;
        }
      }
      closedir(d);
    }
  }
}

static void sonic_check_exit_hotkey(SDL_GameController *pad, const Uint8 *ks) {
  int keyboard_combo = ks && ks[SDL_SCANCODE_ESCAPE] &&
                       ks[SDL_SCANCODE_RETURN];
  int pad_combo = 0;
  if (pad) {
    SDL_GameControllerUpdate();
    pad_combo = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
  }
  if (keyboard_combo || pad_combo) {
    fprintf(stderr, "=== SELECT+START -> exit ===\n");
    fflush(NULL);
    sync();
    _exit(0);
  }
}

static void build_base_table(void) {
  g_base_n = shantae_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, shantae_overrides, sizeof(DynLibFunction) * shantae_overrides_count);
  memcpy(g_base + shantae_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

/* ---- patch "return 0": ARM/Thumb (32-bit) ou A64 (aarch64) ---- */
static void patch_ret0(const char *sym) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch: símbolo %s NÃO encontrado\n", sym); return; }
  uintptr_t a = raw & ~(uintptr_t)1;
  uintptr_t pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch: mprotect %s falhou\n", sym); return;
  }
#ifdef __aarch64__
  ((uint32_t *)a)[0] = 0x52800000; /* mov w0, #0 */
  ((uint32_t *)a)[1] = 0xd65f03c0; /* ret        */
  const char *mode = "A64";
#else
  int thumb = raw & 1;
  if (thumb) {
    ((uint16_t *)a)[0] = 0x2000; /* movs r0,#0 */
    ((uint16_t *)a)[1] = 0x4770; /* bx lr      */
  } else {
    ((uint32_t *)a)[0] = 0xe3a00000; /* mov r0,#0 (ARM) */
    ((uint32_t *)a)[1] = 0xe12fff1e; /* bx lr     (ARM) */
  }
  const char *mode = thumb ? "Thumb" : "ARM";
#endif
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "patch: %s -> return 0 @0x%lx (%s)\n", sym,
          (unsigned long)a, mode);
}

/* patch "return val" (val pequeno) */
static void patch_retval(const char *sym, int val) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = raw & ~(uintptr_t)1, pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
#ifdef __aarch64__
  ((uint32_t *)a)[0] = 0x52800000 | ((uint32_t)(val & 0xffff) << 5); /* movz w0,#val */
  ((uint32_t *)a)[1] = 0xd65f03c0; /* ret */
  const char *mode = "A64";
#else
  int thumb = raw & 1;
  if (thumb) { ((uint16_t *)a)[0] = 0x2000 | (val & 0xff); ((uint16_t *)a)[1] = 0x4770; }
  else { ((uint32_t *)a)[0] = 0xe3a00000 | (val & 0xff); ((uint32_t *)a)[1] = 0xe12fff1e; }
  const char *mode = thumb ? "Thumb" : "ARM";
#endif
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "patch: %s -> return %d @0x%lx (%s)\n", sym, val,
          (unsigned long)a, mode);
}

/* patch de UMA instrução (32-bit) em offset de byte dentro de um símbolo.
   `insn` deve ser do ISA do build (ARM ou A64) — quem chama escolhe via #ifdef. */
static void patch_word_at(const char *sym, unsigned off, uint32_t insn) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch_word: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = (raw & ~(uintptr_t)1) + off, pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch_word: mprotect %s falhou\n", sym); return;
  }
  ((uint32_t *)a)[0] = insn;
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 4);
  fprintf(stderr, "patch_word: %s+0x%x = 0x%08x @0x%lx\n", sym, off, insn,
          (unsigned long)a);
}

#ifdef __aarch64__
static int64_t a64_sign_extend(uint64_t value, unsigned bits) {
  uint64_t sign = UINT64_C(1) << (bits - 1);
  return (int64_t)((value ^ sign) - sign);
}

static uintptr_t a64_bl_target(uintptr_t pc, uint32_t insn) {
  return (uintptr_t)((int64_t)pc +
                     a64_sign_extend(insn & 0x03ffffffU, 26) * 4);
}

static int a64_is_b_cond(uint32_t insn) {
  return (insn & 0xff000010U) == 0x54000000U;
}

static int a64_is_b(uint32_t insn) {
  return (insn & 0xfc000000U) == 0x14000000U;
}

static uintptr_t a64_b_cond_target(uintptr_t pc, uint32_t insn) {
  return (uintptr_t)((int64_t)pc +
                     a64_sign_extend((insn >> 5) & 0x7ffffU, 19) * 4);
}

static uintptr_t a64_b_target(uintptr_t pc, uint32_t insn) {
  return (uintptr_t)((int64_t)pc +
                     a64_sign_extend(insn & 0x03ffffffU, 26) * 4);
}

static uint32_t a64_encode_b(uintptr_t pc, uintptr_t target) {
  int64_t delta = (int64_t)target - (int64_t)pc;
  return 0x14000000U | ((uint32_t)(delta >> 2) & 0x03ffffffU);
}

/* Android/Bionic and glibc do not use the same AArch64 TLS layout. This exact
 * libfox reads tpidr_el0+0x28 as its stack guard; on some glibc releases that
 * slot is mutable runtime state, so valid calls take __stack_chk_fail paths.
 * The imported fail handler is already a compatibility no-op, but those calls
 * are compiled noreturn and therefore fall through into unrelated functions.
 *
 * Validate the complete, known instruction layout before changing anything,
 * then make every recognized guard path use its normal epilogue. The extractor
 * accepts only the matching libfox SHA/BuildID, so a different payload fails
 * closed instead of receiving offsets meant for this release. */
static int sonic_patch_bionic_stack_guards(void) {
  enum {
    EXPECTED_FAIL_CALLS = 2521,
    EXPECTED_BRANCHES_TO_FAIL = 2527,
    EXPECTED_JUMPS_TO_FAIL = 447,
    EXPECTED_DIRECT_FALLTHROUGHS = 338,
    EXPECTED_NORMAL_GUARDS = 785
  };
  static const unsigned char expected_build_note[36] = {
    0x04, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x47, 0x4e, 0x55, 0x00,
    0xfb, 0x0e, 0x88, 0x4d, 0xe8, 0x48, 0x7a, 0x83,
    0x7d, 0x5f, 0xdd, 0xa6, 0xea, 0x35, 0xf7, 0x31,
    0xf2, 0x1c, 0xff, 0x32
  };
  static const uint32_t expected_plt[4] = {
    0x90003f70U, 0xf9430611U, 0x91182210U, 0xd61f0220U
  };
  const size_t expected_text_size = 0xae2210U;
  const size_t code_start_offset = 0x399000U;
  const size_t code_end_offset = 0x926ea8U;
  const uintptr_t base = (uintptr_t)text_base;
  const uintptr_t end = base + expected_text_size;
  const uintptr_t code_start = base + code_start_offset;
  const uintptr_t code_end = base + code_end_offset;
  const uintptr_t fail_plt = base + 0x381610U;
  const size_t insn_count = expected_text_size / sizeof(uint32_t);
  unsigned char *fail_calls = NULL;
  unsigned fail_count = 0, to_fail_count = 0, covered_fail_count = 0;
  unsigned jump_to_fail_count = 0, direct_fallthrough_count = 0;
  unsigned normal_guard_count = 0, invalid_structure_count = 0;

  if (!base || text_size != expected_text_size ||
      base > UINTPTR_MAX - expected_text_size || (base & 3U) ||
      memcmp((const void *)(base + 0x200U), expected_build_note,
             sizeof(expected_build_note)) != 0 ||
      memcmp((const void *)fail_plt, expected_plt, sizeof(expected_plt)) != 0) {
    fprintf(stderr, "ERRO: STACKGUARD64 BuildID/layout/PLT inesperado; payload recusado\n");
    return -1;
  }

  fail_calls = calloc(insn_count, 1);
  if (!fail_calls) {
    fprintf(stderr, "ERRO: STACKGUARD64 sem memoria para validar o codigo\n");
    return -1;
  }

  for (size_t i = code_start_offset / 4; i < code_end_offset / 4; i++) {
    uintptr_t pc = base + i * sizeof(uint32_t);
    uint32_t insn = ((const uint32_t *)base)[i];
    if ((insn & 0xfc000000U) == 0x94000000U &&
        a64_bl_target(pc, insn) == fail_plt) {
      fail_calls[i] = 1;
      fail_count++;
    }
  }

  for (size_t i = code_start_offset / 4; i < code_end_offset / 4; i++) {
    uintptr_t pc = base + i * sizeof(uint32_t);
    uint32_t insn = ((const uint32_t *)base)[i];
    if (a64_is_b_cond(insn)) {
      uintptr_t target = a64_b_cond_target(pc, insn);
      if (target >= base && target < end && ((target - base) & 3U) == 0 &&
          (fail_calls[(target - base) / sizeof(uint32_t)] & 1U)) {
        to_fail_count++;
        if ((insn & 0xfU) != 1U || i == 0)
          invalid_structure_count++;
        size_t target_i = (target - base) / sizeof(uint32_t);
        if (!(fail_calls[target_i] & 2U)) {
          fail_calls[target_i] |= 2U;
          covered_fail_count++;
        }
      }
    }
    if (a64_is_b(insn)) {
      uintptr_t target = a64_b_target(pc, insn);
      if (target >= base && target < end && ((target - base) & 3U) == 0 &&
          (fail_calls[(target - base) / sizeof(uint32_t)] & 1U)) {
        jump_to_fail_count++;
        if (i == 0) {
          invalid_structure_count++;
        } else {
          uint32_t previous = ((const uint32_t *)base)[i - 1];
          uintptr_t previous_pc = pc - sizeof(uint32_t);
          uintptr_t normal_target = a64_is_b_cond(previous)
                                      ? a64_b_cond_target(previous_pc, previous) : 0;
          if (!a64_is_b_cond(previous) || (previous & 0xfU) != 0U ||
              normal_target < code_start || normal_target >= code_end)
            invalid_structure_count++;
          else
            normal_guard_count++;
        }
      }
    }
    if (fail_calls[i] && i > 0) {
      uint32_t previous = ((const uint32_t *)base)[i - 1];
      uintptr_t previous_pc = pc - sizeof(uint32_t);
      if (a64_is_b_cond(previous) && a64_b_cond_target(previous_pc, previous) != pc) {
        uintptr_t normal_target = a64_b_cond_target(previous_pc, previous);
        direct_fallthrough_count++;
        if ((previous & 0xfU) != 0U || normal_target < code_start ||
            normal_target >= code_end)
          invalid_structure_count++;
        else
          normal_guard_count++;
      }
    }
  }

  if (fail_count != EXPECTED_FAIL_CALLS ||
      to_fail_count != EXPECTED_BRANCHES_TO_FAIL ||
      covered_fail_count != EXPECTED_FAIL_CALLS ||
      jump_to_fail_count != EXPECTED_JUMPS_TO_FAIL ||
      direct_fallthrough_count != EXPECTED_DIRECT_FALLTHROUGHS ||
      normal_guard_count != EXPECTED_NORMAL_GUARDS || invalid_structure_count) {
    fprintf(stderr,
            "ERRO: STACKGUARD64 divergente calls=%u/%u to_fail=%u/%u "
            "covered=%u/%u jumps=%u/%u direct=%u/%u normal=%u/%u bad=%u; "
            "nenhum patch aplicado\n",
            fail_count, EXPECTED_FAIL_CALLS,
            to_fail_count, EXPECTED_BRANCHES_TO_FAIL,
            covered_fail_count, EXPECTED_FAIL_CALLS,
            jump_to_fail_count, EXPECTED_JUMPS_TO_FAIL,
            direct_fallthrough_count, EXPECTED_DIRECT_FALLTHROUGHS,
            normal_guard_count, EXPECTED_NORMAL_GUARDS,
            invalid_structure_count);
    free(fail_calls);
    return -1;
  }

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) page_size = 4096;
  uintptr_t map_start = code_start & ~((uintptr_t)page_size - 1U);
  uintptr_t map_end = (code_end + (uintptr_t)page_size - 1U) &
                      ~((uintptr_t)page_size - 1U);
  if (mprotect((void *)map_start, map_end - map_start,
               PROT_READ | PROT_WRITE) != 0) {
    fprintf(stderr, "ERRO: STACKGUARD64 nao conseguiu liberar text para patch\n");
    free(fail_calls);
    return -1;
  }

  for (size_t i = code_start_offset / 4; i < code_end_offset / 4; i++) {
    uintptr_t pc = base + i * sizeof(uint32_t);
    uint32_t insn = ((uint32_t *)base)[i];
    if (!a64_is_b_cond(insn)) continue;
    uintptr_t target = a64_b_cond_target(pc, insn);
    if (target >= base && target < end && ((target - base) & 3U) == 0 &&
        (fail_calls[(target - base) / sizeof(uint32_t)] & 1U))
      ((uint32_t *)base)[i] = 0xd503201fU;
  }
  for (size_t i = code_start_offset / 4; i < code_end_offset / 4; i++) {
    uintptr_t pc = base + i * sizeof(uint32_t);
    uint32_t insn = ((uint32_t *)base)[i];
    if (!a64_is_b(insn)) continue;
    uintptr_t target = a64_b_target(pc, insn);
    if (target >= base && target < end && ((target - base) & 3U) == 0 &&
        (fail_calls[(target - base) / sizeof(uint32_t)] & 1U)) {
      uintptr_t previous_pc = pc - sizeof(uint32_t);
      uint32_t previous = ((uint32_t *)base)[i - 1];
      ((uint32_t *)base)[i - 1] =
          a64_encode_b(previous_pc, a64_b_cond_target(previous_pc, previous));
    }
  }
  for (size_t i = code_start_offset / 4 + 1; i < code_end_offset / 4; i++) {
    if (!fail_calls[i]) continue;
    uintptr_t pc = base + i * sizeof(uint32_t);
    uintptr_t previous_pc = pc - sizeof(uint32_t);
    uint32_t previous = ((uint32_t *)base)[i - 1];
    if (a64_is_b_cond(previous)) {
      uintptr_t target = a64_b_cond_target(previous_pc, previous);
      if (target != pc)
        ((uint32_t *)base)[i - 1] = a64_encode_b(previous_pc, target);
    }
  }

  __builtin___clear_cache((char *)code_start, (char *)code_end);
  if (mprotect((void *)map_start, map_end - map_start,
               PROT_READ | PROT_EXEC) != 0) {
    fprintf(stderr, "ERRO: STACKGUARD64 nao conseguiu restaurar text RX\n");
    free(fail_calls);
    return -1;
  }
  free(fail_calls);
  fprintf(stderr,
          "=== STACKGUARD64: %u calls, %u fail-branches e %u epilogos normalizados ===\n",
          fail_count, to_fail_count, normal_guard_count);
  return 0;
}
#endif

/* desvio absoluto pra `target`: ARM (8B) ou A64 (16B: ldr x16,#8; br x16; .quad) */
static void patch_arm_jump(const char *sym, void *target) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch_jump: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = raw & ~(uintptr_t)1, pg = a & ~0xFFFUL;
#ifndef __aarch64__
  if (raw & 1) { fprintf(stderr, "patch_jump: %s é Thumb, ignorado\n", sym); return; }
#endif
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch_jump: mprotect %s falhou\n", sym); return;
  }
#ifdef __aarch64__
  ((uint32_t *)a)[0] = 0x58000050;          /* ldr x16, #8 (pc+8) */
  ((uint32_t *)a)[1] = 0xd61f0200;          /* br  x16            */
  *(uint64_t *)(a + 8) = (uint64_t)(uintptr_t)target;
  __builtin___clear_cache((char *)a, (char *)a + 16);
#else
  ((uint32_t *)a)[0] = 0xe51ff004;          /* ldr pc, [pc, #-4] */
  ((uint32_t *)a)[1] = (uint32_t)(uintptr_t)target;
  __builtin___clear_cache((char *)a, (char *)a + 8);
#endif
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  fprintf(stderr, "patch_jump: %s -> %p @0x%lx\n", sym, target,
          (unsigned long)a);
}

/* base/fim do heap da engine (mantido só p/ diagnóstico/log). */
static uintptr_t g_engine_heap_base = 0, g_engine_heap_end = 0;
/* 🔑 VALIDADE FROUXA (corrige a REGRESSÃO pós-v4.0 que travava a abertura da fase):
   as REGISTLIST da engine são malloc'd em endereços tipo 0xab.../0xac... — FORA do
   arena de 256MB do so_load. A checagem ESTRITA por arena [heap_base,heap_end) rejeitava
   TODA lista válida -> nenhuma textura era liberada -> a free-list do amTexMgr esgotava
   -> amTexMgrCreateTexId fazia `str r0,[r3]` com r3=head=NULL -> SIGSEGV (fase não abre).
   A v4.0 e o 1º fix de saída (a03b5fe) usavam range frouxo e funcionavam. Aqui o
   discriminador do caso stale (saída) NÃO é o range e sim o `count` fora de [1,65536]. */
static int sane_engine_ptr(const void *p) {
  uintptr_t v = (uintptr_t)p;
#ifdef __aarch64__
  /* LP64: malloc do glibc vive ACIMA de 4GB (0x7f...). O teto antigo de 32-bit
     (0xfffff000) rejeitava TODA lista válida no arm64 -> nenhuma textura liberada
     -> free-list do amTexMgr (1024 slots) esgotava no reload -> TELA PRETA. */
  return v > 0x10000UL && v < (1UL << 48) && (v & 7) == 0;
#else
  return v > 0x10000UL && v < 0xfffff000UL && (v & 3) == 0; /* userspace + alinhado a 4 */
#endif
}
/* 🛡️ FIX do crash ao SAIR da fase (Return to Stage Select fecha o jogo):
   _amDrawReleaseTexture(node) libera a lista de texturas da cena de forma DIFERIDA
   (enfileirada por GameProcess, executada por DrawFrame via amDrawExecRegist). Na saída,
   o GameProcess enfileira o release E libera os dados da cena na MESMA passada (loop
   single-thread GameProcess->DrawFrame), então quando o DrawFrame drena a fila o ponteiro
   da lista (node+4 = P = {count,array}) já aponta pra memória LIBERADA -> count lixo
   (ex.: 0xfbfd18f0) -> indexa array fora -> SIGSEGV (e às vezes vira corrupção/OOM).
   Nossa versão VALIDA o count/ponteiros antes de iterar: lista válida = libera normal
   (sem vazar); lista corrompida = pula com segurança (os dados já foram liberados pela
   destruição da cena, então não há o que liberar). Idêntica ao original no caminho bom. */
static void (*g_real_TexMgrDecRef)(unsigned) = 0;
static unsigned long g_relsafe_ok = 0, g_relsafe_skip = 0;
#ifdef __aarch64__
/* registro de SNAPSHOTS in-flight do RELSAFE64 (enqueue copia a lista p/ buffer nosso;
   exec reconhece, libera e dá free). Single-thread (GameProcess/DrawFrame no mesmo loop). */
#define RELSNAP_MAX 512
static void *g_relsnap[RELSNAP_MAX];
static int g_relsnap_n = 0;
static void relsnap_track(void *p) {
  if (g_relsnap_n < RELSNAP_MAX) g_relsnap[g_relsnap_n++] = p;
}
static int relsnap_untrack(void *p) {
  for (int i = 0; i < g_relsnap_n; i++)
    if (g_relsnap[i] == p) { g_relsnap[i] = g_relsnap[--g_relsnap_n]; return 1; }
  return 0;
}
#endif
static void my_amDrawReleaseTexture(void *node) {
  if (!node) return;
  void *P = ((void **)node)[1];                    /* P = [node+4] = lista {count, array} */
#ifdef __aarch64__
  if (P && relsnap_untrack(P)) {
    /* snapshot NOSSO (RELSAFE64): sempre válido, layout {u32 count, pad, array=P+16}. */
    unsigned scount = *(unsigned *)P;
    const unsigned char *sa = *(const unsigned char **)((char *)P + 8);
    if (!g_real_TexMgrDecRef)
      g_real_TexMgrDecRef = (void (*)(unsigned))so_find_addr_safe("amTexMgrDecRef");
    for (unsigned i = 0; i < scount; i++) {
      unsigned h = *(const unsigned *)(sa + (size_t)i * 8);
      if (h && h < 4096 && g_real_TexMgrDecRef) g_real_TexMgrDecRef(h);
    }
    free(P);
    g_relsafe_ok++;
    *(unsigned long *)node = 0;
    return;
  }
  /* arm64: TODO release legítimo virou snapshot no enqueue (RELSAFE64). P não-rastreado
     aqui = lista STALE (ou fluxo desconhecido) -> NUNCA DecRef (conteúdo pode ser lixo
     "plausível" e mataria texturas vivas — foi o que sumiu com os sprites do título).
     Pular = leak raro e inofensivo. */
  g_relsafe_skip++;
  if (g_relsafe_skip <= 20)
    fprintf(stderr, "[RELSAFE64] release exec com P nao-rastreado (%p) pulado #%lu\n",
            P, g_relsafe_skip);
  *(unsigned long *)node = 0;
  return;
#endif
  long count = -1; void *array = 0;
  if (P && sane_engine_ptr(P)) {
    count = (long)((unsigned *)P)[0];              /* [P+0] = count */
    array = ((void **)P)[1];                       /* [P+4] = array de elementos de 8 BYTES */
  }
  if (count > 0 && count <= 65536 && array && sane_engine_ptr(array)) {
    if (!g_real_TexMgrDecRef)
      g_real_TexMgrDecRef = (void (*)(unsigned))so_find_addr_safe("amTexMgrDecRef");
    /* 🔑 STRIDE 8 BYTES (igual ao original `ldr [r6, r4, lsl #3]`): cada entrada do array
       e {u32 handle, u32 outro}; o handle e os 4 bytes baixos. Ler como u32[] (passo 4)
       pegava handles ERRADOS -> DecRef em slots errados -> corrompia o texmgr (free-list)
       -> crash em amTexMgrCreateTexId (special stage/transicoes). */
    const unsigned char *a = (const unsigned char *)array;
    for (long i = 0; i < count; i++) {
      unsigned h = *(const unsigned *)(a + (size_t)i * 8);
      if (h && g_real_TexMgrDecRef) g_real_TexMgrDecRef(h);  /* qualquer handle != 0, igual orig */
    }
    g_relsafe_ok++;
  } else {
    g_relsafe_skip++;
    if (g_relsafe_skip <= 20)
      fprintf(stderr, "[RELSAFE] release de textura com lista CORROMPIDA pulado "
              "(count=%ld P=%p array=%p) skip#%lu\n", count, (void *)P,
              (void *)array, g_relsafe_skip);
  }
#ifdef __aarch64__
  *(unsigned long *)node = 0;   /* v3: campo cmd é de 8 bytes (str xzr) — zerar inteiro */
#else
  ((unsigned *)node)[0] = 0;                       /* node->field0 = 0 (igual ao original) */
#endif
}

#ifdef __aarch64__
/* ---- 🛡️ RELSAFE64: fix RAIZ do "return to stage select / restart = TELA PRETA" (v3/arm64) ----
   O release de textura da cena é DIFERIDO: GameProcess enfileira o comando 2
   (_amDrawReleaseTexture) no ring do amDraw via amDrawRegistCommand1 e o DrawFrame drena
   (amDrawExecRegist). O payload (0x58 bytes) é copiado POR VALOR no enqueue, mas ele contém
   um PONTEIRO P = {u32 count, pad, void *array} que aponta pros dados da CENA — e o
   GameProcess libera a cena na MESMA passada (nosso loop é single-thread) -> no exec o P
   está STALE: ou lê count/handles lixo (SIGSEGV storm em amTexMgrDecRef, mascarado pelo
   DEMOGUARD -> DecRef em slots errados) ou o RELSAFE antigo pulava tudo (leak -> a tabela
   de 1024 slots do amTexMgr esgota -> amTexMgrCreateTexId falha no reload -> TELA PRETA).
   FIX: interceptar o ENQUEUE (amDrawRegistCommand1). NESSE instante P ainda é VÁLIDO:
   fazemos os amTexMgrDecRef JÁ (só decrementa refcount numa tabela global; zero GL — seguro
   em qualquer thread) e enfileiramos o comando 0 (_amDrawRegistNop) no lugar, preservando a
   semântica do ring (índice/contador/payload idênticos). O exec nunca mais toca memória
   liberada. Endereços do ctx/tabela são DERIVADOS dos próprios bytes da lib carregada
   (decode ADRP+LDR/ADD) e o hook SÓ arma se a tabela verificar (table[0]=Nop,
   table[2]=ReleaseTexture) — à prova de versão. SONIC_NO_RELSAFE desliga junto. */
static void **g_regist_ctx_pp;
static void (*g_texdecref64)(unsigned);
static unsigned long g_reltex_sync, g_reltex_bad, g_reltex_badhandle;

/* decode ADRP + (LDR imm | ADD imm) nas primeiras insns de `fn` -> endereço do global. */
static uintptr_t a64_find_global(uintptr_t fn, int want_add, int max_insn) {
  if (!fn) return 0;
  uint32_t *p = (uint32_t *)(fn & ~(uintptr_t)1);
  for (int i = 0; i + 1 < max_insn; i++) {
    uint32_t a = p[i];
    if ((a & 0x9f000000u) != 0x90000000u) continue;              /* ADRP rd, page */
    unsigned rd = a & 31;
    int64_t imm21 = ((int64_t)((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3);
    imm21 = (imm21 << 43) >> 43;                                  /* sign-extend 21b */
    uintptr_t page = ((uintptr_t)(p + i) & ~0xfffUL) + ((uintptr_t)imm21 << 12);
    uint32_t b = p[i + 1];
    if (!want_add && (b & 0xffc00000u) == 0xf9400000u && ((b >> 5) & 31) == rd)
      return page + (uintptr_t)(((b >> 10) & 0xfff) << 3);        /* LDR Xt,[rd,#imm] */
    if (want_add && (b & 0xffc00000u) == 0x91000000u && ((b >> 5) & 31) == rd)
      return page + (uintptr_t)((b >> 10) & 0xfff);               /* ADD rd,rd,#imm */
  }
  return 0;
}

static int my_amDrawRegistCommand1(int cmd, void *src, unsigned long size) {
  /* SNAPSHOT do release (cmd 2 = _amDrawReleaseTexture): NO ENQUEUE a lista P ainda é
     válida -> copiamos {count, handles} p/ buffer NOSSO e trocamos o P do payload pelo
     snapshot. O DecRef continua acontecendo NO EXEC (depois dos draws do frame que ainda
     usam as texturas — timing original preservado; DecRef adiantado matava os sprites
     do título/menu), só que lendo memória estável em vez da cena liberada. */
  void *snap = NULL;
  if (cmd == 2 && src) {
    void *P = *(void **)src;
    long count = -1; unsigned char *array = NULL;
    if (P && sane_engine_ptr(P)) {
      count = (long)*(unsigned *)P;            /* [P+0] = count */
      array = *(unsigned char **)((char *)P + 8); /* [P+8] = array (stride 8, handle u32) */
    }
    if (count > 0 && count <= 65536 && array && sane_engine_ptr(array)) {
      snap = malloc(16 + (size_t)count * 8);
      if (snap) {
        *(unsigned *)snap = (unsigned)count;
        *(void **)((char *)snap + 8) = (char *)snap + 16;
        memcpy((char *)snap + 16, array, (size_t)count * 8);
        relsnap_track(snap);
        g_reltex_sync++;
      }
    } else if (P) {
      g_reltex_bad++;
      if (g_reltex_bad <= 20)
        fprintf(stderr, "[RELSAFE64] lista invalida ja no ENQUEUE (P=%p count=%ld) #%lu\n",
                P, count, g_reltex_bad);
    }
  }
  /* réplica fiel do amDrawRegistCommand1 v3 (RE 0x3aaed8) */
  void *ctx = g_regist_ctx_pp ? *g_regist_ctx_pp : NULL;
  if (!ctx) { fprintf(stderr, "[RELSAFE64] ctx NULL no enqueue!\n"); return -1; }
  int idx = *(int *)((char *)ctx + 0x210c);
  char *entry = (char *)ctx + (long)idx * 0xa0;
  *(long *)(entry + 0x2118) = (long)cmd;
  char *dst = entry + 0x2120;
  if (src) { if ((void *)dst != src) memcpy(dst, src, size ? size : 0x58); }
  else memset(dst, 0, 0x58);
  if (snap) *(void **)dst = snap;              /* payload aponta pro snapshot estável */
  *(int *)((char *)ctx + 0x210c) = (idx + 1) & 511;
  *(unsigned *)((char *)ctx + 0x70) += 1;
  return idx;
}

/* deriva ctx/tabela e arma o hook; retorna 1 se armado. */
static int sonic_install_relsafe64(void) {
  uintptr_t reg1 = so_find_addr_safe("_Z20amDrawRegistCommand1iPvm");
  uintptr_t exec = so_find_addr_safe("_Z16amDrawExecRegistv");
  uintptr_t nop  = so_find_addr_safe("_Z16_amDrawRegistNopP14AMS_REGISTLIST");
  uintptr_t rel  = so_find_addr_safe("_Z21_amDrawReleaseTextureP14AMS_REGISTLIST");
  if (!reg1 || !exec || !nop || !rel) {
    fprintf(stderr, "AVISO: RELSAFE64 sem simbolos (reg1=%lx exec=%lx)\n",
            (unsigned long)reg1, (unsigned long)exec);
    return 0;
  }
  uintptr_t ctx_pp = a64_find_global(reg1, 0, 12);
  uintptr_t table  = a64_find_global(exec, 1, 60);
  if (!ctx_pp || !table) {
    fprintf(stderr, "AVISO: RELSAFE64 nao derivou ctx/tabela (ctx_pp=%lx table=%lx)\n",
            (unsigned long)ctx_pp, (unsigned long)table);
    return 0;
  }
  void **tab = (void **)table;
  if ((uintptr_t)tab[0] != (nop & ~(uintptr_t)1) || (uintptr_t)tab[2] != (rel & ~(uintptr_t)1)) {
    fprintf(stderr, "AVISO: RELSAFE64 tabela NAO verificou (t0=%p esperado=%lx, t2=%p esperado=%lx)\n",
            tab[0], (unsigned long)nop, tab[2], (unsigned long)rel);
    return 0;
  }
  g_regist_ctx_pp = (void **)ctx_pp;
  patch_arm_jump("_Z20amDrawRegistCommand1iPvm", (void *)my_amDrawRegistCommand1);
  fprintf(stderr, "=== RELSAFE64: DecRef sincrono no ENQUEUE (cmd2->NOP) armado; "
                  "ctx_pp=%p table=%p ===\n", (void *)ctx_pp, (void *)table);
  return 1;
}
#endif

/* 🔊 FIX som do 1up/jingle (mudo no gameplay) — CIRÚRGICO, DEFAULT ON.
   A engine poll-a MediaPlayerisPlaying(canal) por frame; quando "não toca" seta bit1=stop
   na SCB e DmSoundIsStopJingle manda PARAR o jingle. Patchar ->0 fixo (p/ pular o intro)
   matava TODO jingle na hora (som da caixa de vida/1up, anéis-suficientes, sons da special
   stage que passam pelo caminho DmSound). SOLUÇÃO: devolver o estado REAL só p/ os canais
   de JINGLE (key com "_jin_"); os outros mantêm ->0 (preserva o resto + pula o intro via
   willPlayMovie->0 + videoIsPlaying->0). Isto NÃO tem relação com o crash da fase (era o
   texmgr) — reabilitado após corrigir o guard do RELSAFE. SONIC_NO_JINGLE1UP=1 reverte. */
extern int sonic_audio_jingle_playing(int id);
static int my_MediaPlayerisPlaying(int i) {
  static int off = -1;
  if (off < 0) off = getenv("SONIC_NO_JINGLE1UP") ? 1 : 0;
  if (off) return 0;                                  /* fallback: comportamento v4.0 (mudo) */
  return sonic_audio_jingle_playing(i);               /* 1 só se canal i for jingle tocando */
}

/* 🛡️ FIX crash no teardown do attract-demo (reportado pelo tester na Electric Road, frame ~115k):
   `gm::start_demo::ep2::CStartDemo` é um SINGLETON (s_instance). Se o objeto sofre USE-AFTER-FREE
   (liberado externamente sem zerar s_instance), o ReleaseInstance nativo chama o destrutor virtual
   sobre memória corrompida -> SIGSEGV dentro de ~CStartDemo (ex.: str [this+0x28] em libfox+0x4254b8,
   PC=0x...4254b8). Reimplementamos o ReleaseInstance (fiel: null-check + blx vtable[0] + zera
   s_instance) MAS envolvemos a chamada do destrutor numa CHAMADA PROTEGIDA (sigsetjmp): se crashar,
   o handler faz siglongjmp de volta, a gente loga, zera s_instance e SEGUE (vaza o objeto meio-
   destruído, mas NÃO fecha o jogo). No caminho bom é idêntico ao original. SONIC_NO_DEMOGUARD desliga. */
static void **g_cstartdemo_s_instance = 0;
static void my_ep2_CStartDemo_ReleaseInstance(void) {
  if (!g_cstartdemo_s_instance)
    g_cstartdemo_s_instance =
        (void **)so_find_addr_safe("_ZN2gm10start_demo3ep210CStartDemo10s_instanceE");
  void **pinst = g_cstartdemo_s_instance;
  if (!pinst) return;
  void *inst = *pinst;
  if (!inst) return;                          /* igual ao original: s_instance null -> nada a fazer */
  g_demo_guard_active = 1;
  if (sigsetjmp(g_demo_guard_env, 1) == 0) {
    void (**vt)(void *) = *(void (***)(void *))inst;   /* vtable = *inst (pode crashar se UAF) */
    void (*dtor)(void *) = vt[0];                      /* vtable[0] = destrutor virtual (D0) */
    dtor(inst);                                        /* == blx [[inst]] do ReleaseInstance nativo */
    g_demo_guard_active = 0;
  } else {
    /* recuperado do SIGSEGV dentro do destrutor */
    fprintf(stderr, "[DEMOGUARD] ~CStartDemo crashou (use-after-free) -> RECUPERADO, "
                    "s_instance zerado (#%lu)\n", g_demo_guard_recovered);
  }
  *pinst = 0;                                 /* igual ao original: zera o singleton */
}

static int sonic_amThreadCheckDraw(long unused) {
  (void)unused;
  /* SONIC_THREADDRAW: testa o valor. O original retorna 1 se está na thread de draw
     registrada; single-thread (nossa main = draw thread) => deveria ser 1 SEMPRE.
     =1 força 1 (execute sempre, sem enfileirar — testa o bug de replicação do cassino);
     =0 força 0; vazio = sonic_in_draw_frame (comportamento atual). */
  static int mode = -2;
  if (mode == -2) { const char *m = getenv("SONIC_THREADDRAW");
    mode = m ? (m[0]-'0') : -1; }
  if (mode == 1) return 1;
  if (mode == 0) return 0;
  return sonic_in_draw_frame ? 1 : 0;
}

static void sonic_native_save_load_poll(const char *where) {
  if (!g_native_save_load.pending) return;
  if (g_native_save_load.AoStorageLoadIsFinished &&
      !g_native_save_load.AoStorageLoadIsFinished())
    return;

  int ok = g_native_save_load.AoStorageLoadIsSuccessed ?
      g_native_save_load.AoStorageLoadIsSuccessed() : 0;
  if (ok) {
    fprintf(stderr, "=== native save load OK (%s) ===\n", where);
    if (g_native_save_load.CopyBackupComp)
      g_native_save_load.CopyBackupComp(g_native_save_load.uid);
    if (g_native_save_load.SetSaveEnable)
      g_native_save_load.SetSaveEnable(g_native_save_load.uid, 1);
    if (g_native_save_load.DmBuildSysDataFromBackup)
      g_native_save_load.DmBuildSysDataFromBackup();
  } else {
    int err = g_native_save_load.AoStorageGetError ?
        g_native_save_load.AoStorageGetError() : -1;
    fprintf(stderr, "=== native save load FAIL err=%d (%s) ===\n", err, where);
    if (g_native_save_load.SetSaveEnable)
      g_native_save_load.SetSaveEnable(g_native_save_load.uid, 0);
    if (g_native_save_load.DmBuildSysDataFromBackup)
      g_native_save_load.DmBuildSysDataFromBackup();
  }
  g_native_save_load.pending = 0;
  g_native_save_load.done = 1;
}

static void sonic_save_bootstrap_init(void) {
  memset(&g_save_bootstrap, 0, sizeof(g_save_bootstrap));
  g_save_bootstrap.AoStorageLoadIsFinished =
      (void *)so_find_addr_safe("_Z23AoStorageLoadIsFinishedv");
  g_save_bootstrap.AoStorageLoadIsSuccessed =
      (void *)so_find_addr_safe("_Z24AoStorageLoadIsSuccessedv");
  g_save_bootstrap.AoStorageGetError =
      (void *)so_find_addr_safe("_Z17AoStorageGetErrorv");
  g_save_bootstrap.DmBuildSysDataFromBackup =
      (void *)so_find_addr_safe("_Z24DmBuildSysDataFromBackupv");
  g_save_bootstrap.UpdateStageUnlockState =
      (void *)so_find_addr_safe("_ZN2gs6backup7utility22UpdateStageUnlockStateEv");
  g_save_bootstrap.IsStageUnlocked =
      (void *)so_find_addr_safe("_ZN2gs6backup7utility15IsStageUnlockedEm21tag_GSE_MAIN_STAGE_ID");
  g_save_bootstrap.IsStageClear =
      (void *)so_find_addr_safe("_ZN2gs6backup7utility12IsStageClearEm21tag_GSE_MAIN_STAGE_ID");
  g_save_bootstrap.SProgressCreateInstance =
      (void *)so_find_addr_safe("_ZN2gs6backup9SProgress14CreateInstanceEm");
  g_save_bootstrap.GetStageUnlockState =
      (void *)so_find_addr_safe("_ZN2gs6backup9SProgress19GetStageUnlockStateEv");
  g_save_bootstrap.GetSsUnlockState =
      (void *)so_find_addr_safe("_ZNK2gs6backup9SProgress16GetSsUnlockStateEv");
  g_save_bootstrap.GetEpMetalUnlockState =
      (void *)so_find_addr_safe("_ZN2gs6backup9SProgress21GetEpMetalUnlockStateEv");

  g_save_bootstrap.ready =
      g_save_bootstrap.AoStorageLoadIsFinished &&
      g_save_bootstrap.AoStorageLoadIsSuccessed &&
      g_save_bootstrap.DmBuildSysDataFromBackup;
  if (!g_save_bootstrap.ready)
    fprintf(stderr, "AVISO: save bootstrap incompleto\n");
}

static void sonic_save_bootstrap_log_progress(const char *where) {
  if (!g_save_bootstrap.IsStageUnlocked || !g_save_bootstrap.IsStageClear)
    return;
  void *progress = g_save_bootstrap.SProgressCreateInstance ?
      g_save_bootstrap.SProgressCreateInstance(0) : NULL;
  int unlock_state = progress && g_save_bootstrap.GetStageUnlockState ?
      g_save_bootstrap.GetStageUnlockState(progress) : -1;
  int ss_state = progress && g_save_bootstrap.GetSsUnlockState ?
      g_save_bootstrap.GetSsUnlockState(progress) : -1;
  int epm_state = progress && g_save_bootstrap.GetEpMetalUnlockState ?
      g_save_bootstrap.GetEpMetalUnlockState(progress) : -1;
  fprintf(stderr, "=== save progress %s: unlock_state=%d ss=%d epm=%d ===\n",
          where, unlock_state, ss_state, epm_state);
  for (int sid = 0; sid <= 4; sid++) {
    fprintf(stderr, "=== save progress stage %d: unlocked=%d clear=%d ===\n",
            sid, g_save_bootstrap.IsStageUnlocked(0, sid),
            g_save_bootstrap.IsStageClear(0, sid));
  }
}

static void sonic_save_bootstrap_poll(unsigned long frame) {
  if (!g_save_bootstrap.ready || g_save_bootstrap.built)
    return;
  if (!g_save_bootstrap.AoStorageLoadIsFinished())
    return;

  if (!g_save_bootstrap.AoStorageLoadIsSuccessed()) {
    if (!g_save_bootstrap.missing_logged && frame > 120) {
      int err = g_save_bootstrap.AoStorageGetError ?
          g_save_bootstrap.AoStorageGetError() : -1;
      fprintf(stderr, "=== save bootstrap: load finished without success err=%d ===\n", err);
      g_save_bootstrap.missing_logged = 1;
    }
    return;
  }

  fprintf(stderr, "=== save bootstrap: DmBuildSysDataFromBackup @frame %lu ===\n", frame);
  g_save_bootstrap.DmBuildSysDataFromBackup();
  if (g_save_bootstrap.UpdateStageUnlockState)
    g_save_bootstrap.UpdateStageUnlockState();
  g_save_bootstrap.built = 1;
  sonic_save_bootstrap_log_progress("after-build");
}

static void sonic_native_save_load_start(void) {
  unsigned long uid = 0, account = 0;
  void (*AoAccountSetCurrentIdStart)(unsigned long) =
      (void *)so_find_addr_safe("_Z26AoAccountSetCurrentIdStartm");
  void (*AoStorageClearError)(void) =
      (void *)so_find_addr_safe("_Z19AoStorageClearErrorv");
  void (*AoStorageLoadStart)(unsigned long, void *, unsigned long,
                             unsigned long, unsigned long) =
      (void *)so_find_addr_safe("_Z18AoStorageLoadStartmPvmmm");
  void *(*GetBackup)(unsigned long) =
      (void *)so_find_addr_safe("_ZN2gs4user5CUtil9GetBackupEm");

  memset(&g_native_save_load, 0, sizeof(g_native_save_load));
  g_native_save_load.uid = uid;
  g_native_save_load.AoStorageLoadIsFinished =
      (void *)so_find_addr_safe("_Z23AoStorageLoadIsFinishedv");
  g_native_save_load.AoStorageLoadIsSuccessed =
      (void *)so_find_addr_safe("_Z24AoStorageLoadIsSuccessedv");
  g_native_save_load.AoStorageGetError =
      (void *)so_find_addr_safe("_Z17AoStorageGetErrorv");
  g_native_save_load.CopyBackupComp =
      (void *)so_find_addr_safe("_ZN2gs4user5CUtil14CopyBackupCompEm");
  g_native_save_load.SetSaveEnable =
      (void *)so_find_addr_safe("_ZN2gs4user5CUtil13SetSaveEnableEml");
  g_native_save_load.DmBuildSysDataFromBackup =
      (void *)so_find_addr_safe("_Z24DmBuildSysDataFromBackupv");

  if (!AoStorageClearError || !AoStorageLoadStart || !GetBackup ||
      !g_native_save_load.AoStorageLoadIsFinished ||
      !g_native_save_load.AoStorageLoadIsSuccessed ||
      !g_native_save_load.CopyBackupComp ||
      !g_native_save_load.SetSaveEnable ||
      !g_native_save_load.DmBuildSysDataFromBackup) {
    fprintf(stderr, "AVISO: native save load incompleto, mantendo fluxo normal\n");
    return;
  }

  if (AoAccountSetCurrentIdStart)
    AoAccountSetCurrentIdStart(account);
  void *backup = GetBackup(uid);
  if (!backup) {
    fprintf(stderr, "AVISO: native save load sem buffer de backup\n");
    return;
  }

  fprintf(stderr, "=== native save load start uid=%lu account=%lu backup=%p ===\n",
          uid, account, backup);
  AoStorageClearError();
  AoStorageLoadStart(account, backup, 1536, 0x594, 0x5bc);
  g_native_save_load.pending = 1;
  sonic_native_save_load_poll("start");
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou\n", heap_mb); exit(1); }
  g_engine_heap_base = (uintptr_t)heap;
  g_engine_heap_end = (uintptr_t)heap + hs;
  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load(%s) falhou\n", name); exit(1); }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); exit(1); }
  so_resolve(tbl, n, 0);
  so_finalize();
#ifdef __aarch64__
  if (sonic_patch_bionic_stack_guards() != 0) exit(1);
#endif
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name,
          text_base, text_size, data_base, data_size);
}

/* ---- fox JNI entry points (Java_com_mineloader_fox_foxJniLib_*) ---- */
typedef void *JEnv;
static struct {
  void (*init)(JEnv, void *, void *, void *);
  void (*SetGamePath)(JEnv, void *, void *, void *);
  void (*coreGetLPKFileInfo)(JEnv, void *, void *, void *);
  void (*SetLanguageId)(JEnv, void *, int);
  void (*DrawEGLCreated)(JEnv, void *);
  void (*DrawFrame)(JEnv, void *, int);
  void (*GameProcess)(JEnv, void *);
  void (*FileProcess)(JEnv, void *);
  void (*HasController)(JEnv, void *, int);
  void (*SetPadData)(JEnv, void *, int, int, int, int, int, int);
  void (*SetTPData)(JEnv, void *, int, int, int, int);
  void (*resumeEvent)(JEnv, void *);
  void (*WindowFocusChanged)(JEnv, void *, int);
} fox;

#define RES(f, sym) do { fox.f = (void *)so_find_addr_safe(sym); \
  fprintf(stderr, "resolve %-22s = %p\n", #f, (void *)fox.f); } while (0)

static void *g_env, *g_thiz;
/* thread dedicada de file-system: roda amFS_proc (loop infinito, cond_wait). */
static void *fs_thread_fn(void *arg) {
  (void)arg;
  fox.FileProcess(g_env, g_thiz);
  return NULL;
}

#ifdef __aarch64__
#define SONIC_DATA_MARKER ".sonic4ep2-v6-data.ok"
#define SONIC_LIBFOX_SIZE 12220336LL
#define SONIC_DATA_OBB_SIZE 673277896LL

/* The extractor writes the marker only after full CRC/SHA/ELF validation and
 * an atomic payload commit. Startup still checks exact sizes so a truncated
 * file can never be accepted just because an old marker survived. */
static int sonic_payload_fast_valid(void) {
  struct stat lib_st, obb_st, marker_st;
  char marker[1024];
  FILE *marker_file;
  size_t marker_len;
  if (stat(GAME_SO, &lib_st) != 0 || !S_ISREG(lib_st.st_mode) ||
      (long long)lib_st.st_size != SONIC_LIBFOX_SIZE)
    return 0;
  if (stat("data/data.obb", &obb_st) != 0 || !S_ISREG(obb_st.st_mode) ||
      (long long)obb_st.st_size != SONIC_DATA_OBB_SIZE)
    return 0;
  if (stat(SONIC_DATA_MARKER, &marker_st) != 0 ||
      !S_ISREG(marker_st.st_mode) || marker_st.st_size <= 0)
    return 0;
  marker_file = fopen(SONIC_DATA_MARKER, "r");
  if (!marker_file)
    return 0;
  marker_len = fread(marker, 1, sizeof(marker) - 1, marker_file);
  fclose(marker_file);
  marker[marker_len] = '\0';
  return strstr(marker, "format=1\n") != NULL &&
         strstr(marker, "libfox_size=12220336\n") != NULL &&
         strstr(marker, "libfox_sha256=ca07163ad1e92d767016d43048a2c13eede7b9d6217ed4f032ca4d6d8e342a1a\n") != NULL &&
         strstr(marker, "data_obb_size=673277896\n") != NULL &&
         strstr(marker, "data_obb_sha256=a2c988a0c2b057b27a328053cef1627e16b6491f5b6700f9627cdc141f82012b\n") != NULL;
}

/* The Android wrapper expects this writable local state before libfox starts.
 * O_EXCL guarantees that an existing user file is never replaced. */
static void sonic_ensure_f2f_state(void) {
  static const char initial_state[] =
      "{\"MerchandiseTime\":1782470230}\n";
  const char *data_dir = getenv("SONIC_DATADIR");
  const char *separator;
  char path[1024];
  struct stat state_st;
  size_t offset = 0;
  int fd, length, ok = 1;

  if (!data_dir || !*data_dir)
    data_dir = ".";
  separator = data_dir[strlen(data_dir) - 1] == '/' ? "" : "/";
  length = snprintf(path, sizeof(path), "%s%sSonic4ep2.f2f",
                    data_dir, separator);
  if (length < 0 || length >= (int)sizeof(path) ||
      lstat(path, &state_st) == 0)
    return;

  fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (errno != EEXIST)
      fprintf(stderr, "[state] warning: could not create %s: %s\n",
              path, strerror(errno));
    return;
  }
  while (offset < sizeof(initial_state) - 1) {
    ssize_t written = write(fd, initial_state + offset,
                            sizeof(initial_state) - 1 - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      ok = 0;
      break;
    }
    offset += (size_t)written;
  }
  if (ok && fsync(fd) != 0 && errno != EINVAL && errno != ENOSYS &&
      errno != EOPNOTSUPP)
    ok = 0;
  if (close(fd) != 0)
    ok = 0;
  if (ok) {
    fprintf(stderr, "[state] created %s\n", path);
    return;
  }
  unlink(path);
  fprintf(stderr, "[state] warning: could not create %s\n", path);
}
#endif

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  g_main_thread = pthread_self();
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  /* modo BAKE (tela de setup da extração, estilo Bully v11): standalone, sai no stop-file. */
  if (getenv("SONIC_SETUPSPLASH")) {
    extern int sonic_run_setup_splash(void);
    return sonic_run_setup_splash();
  }
#ifdef __aarch64__
  fprintf(stderr, "=== SONIC 4 EPISODE II V6 (NN/fox) AArch64 so-loader ===\n");
#else
  fprintf(stderr, "=== SONIC 4 EPISODE II (NN/fox) ARM so-loader ===\n");
#endif

  if (!getenv("SONIC_NO_CRASHLOG")) sonic_install_crash_handler();
  sonic_install_hang_watchdog();

  /* mata instância anterior + confirma 0 antes de tocar no fb/EGL (regra do device). */
  sonic_kill_other_instances();

  /* Config que ANTES vinha do launcher — agora no binário (launcher enxuto/padrão
     PortMaster). overwrite=0: se o ambiente já definiu, respeitamos.
     ⚠️ NUNCA forçamos SDL_VIDEODRIVER/SDL_AUDIODRIVER nem driver de GPU: o
     sistema/SDL escolhem wayland/kmsdrm/fbdev e pulse/alsa/pipewire sozinhos. */
  setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);            /* SDL não pisa nos sinais */
#ifdef __aarch64__
  /* v5: sob pm_platform_helper (systemd-run) os exports do launcher NAO chegam;
     o fix da Electric Road precisa estar ON por default (overwrite=0 preserva
     o controle do launcher/env quando existir). */
  if (env_flag_enabled("SONIC_NO_CLEARALL"))
    setenv("SONIC_CLEARALL", "0", 1);
  else
    setenv("SONIC_CLEARALL", "1", 0);
#endif
  setenv("SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP", "1", 0);
  setenv("SDL_VIDEO_FULLSCREEN_DESKTOP", "1", 0);
  setenv("SONIC_DATADIR", ".", 0);  /* cwd = GAMEDIR (launcher faz cd); save/dados aqui */
  sonic_detect_session_runtime();   /* acha XDG_RUNTIME_DIR/WAYLAND_DISPLAY se a sessão não exportou (muOS sem som / ROCKNIX sem vídeo) */

#ifdef __aarch64__
  sonic_ensure_f2f_state();
#endif

  jni_shim_set_package("com.sega.sonic4episode2", 22);
  { const char *d = getenv("SONIC_DATADIR"); jni_shim_set_local_path(d ? d : "."); }

#ifdef __aarch64__
  /* FIRST-RUN INTEGRADO: um payload so e pronto quando o extrator transacional
     gravou o marker e os dois arquivos ainda tem os tamanhos exatos. Isso tambem
     migra instalacoes V5 validas criando o marker apos uma validacao completa. */
  if (!sonic_payload_fast_valid()) {
    int extract_rc = -1;
    if (access("tools/sonic4ep2_extract.sh", F_OK) == 0) {
      extern int sonic_run_firstrun_bake(void);
      fprintf(stderr, "=== first-run: extraindo dados (bake integrado) ===\n");
      extract_rc = sonic_run_firstrun_bake();
      if (extract_rc != 0) {
        fprintf(stderr, "ERRO: instalador de dados terminou com status %d\n",
                extract_rc);
        return 1;
      }
    }
    if (!sonic_payload_fast_valid()) {
      fprintf(stderr, "ERRO: dados ausentes ou invalidos (%s / data/data.obb / %s). "
                      "Copie sua versao Android 3.0.0 ARM64 legal para o gamedir.\n",
              GAME_SO, SONIC_DATA_MARKER);
      return 1;
    }
  }
#endif
  build_base_table();
  load_module(GAME_SO, GAME_HEAP_MB, g_base, g_base_n);
  if (getenv("SONIC_DEBUG")) { fprintf(stderr, "== load_module OK, aplicando patches ==\n"); fflush(stderr); }

  /* destravar trial -> jogo completo: GsTrialIsTrial() -> 0 */
  patch_ret0("_Z14GsTrialIsTrialv");
  patch_ret0("_Z21GsTrialIsTrial_VerTwov");
  /* sem camada de vídeo: forçar "vídeo não está tocando" p/ o game passar do
     intro (clMovie poll videoIsPlaying/MediaPlayerisPlaying espera o fim). */
  if (!getenv("SONIC_KEEPVIDEO")) {
    patch_ret0("_Z14videoIsPlayingv");
    /* clMovie::willPlayMovieBeforeGameStart(int) -> 0: pula o movie de intro
       (SEGA logo) de vez, sem precisar da camada de vídeo. */
    patch_ret0("_ZNK2gm5movie7clMovie28willPlayMovieBeforeGameStartEi");
    /* clMovie::isEnd() -> 1: o game espera o movie de intro "terminar". */
    patch_retval("_ZN2gm5movie7clMovie5isEndEv", 1);
  }
  /* 🔊 MediaPlayerisPlaying: estado REAL da música (não mais ->0). Conserta o jingle do
     1up (o poll de estado da BGM usava isto; ->0 matava o jingle no gameplay). O intro já
     é pulado por willPlayMovie->0 + videoIsPlaying->0. SONIC_MP_PLAYING_ZERO=1 volta ao ->0
     antigo (fallback se algum device travar no intro). */
  if (getenv("SONIC_MP_PLAYING_ZERO"))
    patch_ret0("_Z20MediaPlayerisPlayingi");
  else
    patch_arm_jump("_Z20MediaPlayerisPlayingi", (void *)my_MediaPlayerisPlaying);
  /* Sonic4F2F::isGamePause -> 0: o F2F pausa o jogo (ads/consent que não temos)
     e fox_FrameUpdate pula amTaskExecute (state machine) qdo pausado -> preto. */
  if (!getenv("SONIC_KEEPPAUSE"))
    patch_ret0("_ZN9Sonic4F2F11isGamePauseEv");
  /* 🔑 SJni_IsUpshellShow -> 0: chama CallBooleanMethod JNI ("a tela de upsell/ads
     está aberta?"). Nosso jni_shim devolve 1 (true) por default => a engine acha que
     o upshell está SEMPRE aberto e o título trava em CStateInitialize::Next pra sempre
     (o gate 1 nunca libera). Forçar 0 (nenhum upsell) destrava a state machine do título. */
  if (!getenv("SONIC_KEEPUPSHELL"))
    patch_ret0("_Z18SJni_IsUpshellShowv");
  /* 🔑 GATE DO MENU (title -> menu): CStateWaitSignIn::Next só avança se o usuário
     estiver habilitado e o setup tiver terminado. Não forçamos
     GsUserSetupIsCompleted: a task nativa de setup precisa rodar porque ela carrega
     foxsave_0.dat e popula o backup global antes do menu/continue. */
  if (!getenv("SONIC_KEEPSIGNIN")) {
    patch_retval("_Z14GsUserIsEnablem", 1);
    /* GsUserIsSaveEnable precisa ficar real: o native save load abaixo carrega
       foxsave_0.dat e seta o flag via CUtil::SetSaveEnable. Forçar return 1
       mascarava falha de load e fazia o menu seguir com backup vazio. */
  }
  /* 🔑 AD INTERSTICIAL: ao selecionar Start, onMainMenuToWorldMap chama
     showInterstitial (ad fullscreen entre telas). Sem a camada de ad Java, o
     callback que carrega o world map NUNCA dispara → tela laranja travada.
     showInterstitial pula o ad se isUserRemoveAds()!=0 OU getInternetState()==0
     → aí chama o callback direto (carrega o world map). Forçar ambos. */
  if (!getenv("SONIC_KEEPADS")) {
    /* showInterstitial GUARDA o callback (cria o jogo) só se getInternetState!=0
       (path 0x4f1470, com o ad-obj null do nosso stub). getInternetState->1 garante
       que guarda. isUserRemoveAds->0 (ads não removidos) pra não pular. Depois
       disparamos o callback no loop via callbackInterstitialAds. */
    patch_ret0("_ZN12F2FExtension15isUserRemoveAdsEv");      /* ads NÃO removidos */
    patch_retval("_ZN12F2FExtension16getInternetStateEv", 1); /* internet "ON" */
  }
  /* 🔓 SONIC_UNLOCK_ALL (debug/teste): libera TODAS as fases independente do save —
     IsStageUnlocked/IsStageClear -> 1 (gate per-stage que o world map/stage-select usa).
     Pra reproduzir bugs em qualquer fase (ex.: Metal Sonic/Episode Metal) sem precisar
     do save certo. GetEpMetalUnlockState -> alto destrava as fases do Episode Metal. */
  if (getenv("SONIC_UNLOCK_ALL")) {
    patch_retval("_ZN2gs6backup7utility15IsStageUnlockedEm21tag_GSE_MAIN_STAGE_ID", 1);
    patch_retval("_ZN2gs6backup7utility12IsStageClearEm21tag_GSE_MAIN_STAGE_ID", 1);
    patch_retval("_ZN2gs6backup9SProgress21GetEpMetalUnlockStateEv", 8);
    fprintf(stderr, "=== SONIC_UNLOCK_ALL: todas as fases liberadas (IsStageUnlocked/Clear->1, EpMetal->8) ===\n");
  }
  /* 🔑 menu Finalize trava: `CMainMenuStateFinalize::Next` espera o demo manager
     terminar o teardown (vtbl[20]/IsClean), mas nosso fake (dmSoundEffectIsSetUpEnd
     ->1) mantém o recurso "carregado" => IsClean nunca true => menu nunca finaliza
     => próxima tela (world map) nunca carrega (laranja). Patch: `bne` (+0x20) -> `b`
     (sempre avança, pula a espera do teardown). */
  /* ⚠️ ERRADOS (opt-in agora): estes patches roteavam o menu pro EXIT em vez da
     transição Decision→onMainMenuToMainGame. Sem eles, onMainMenuToMainGame É
     chamado (a transição correta "Start"→jogo). */
  if (getenv("SONIC_FORCEMENUFINAL")) {
#ifndef __aarch64__
    patch_word_at("_ZN2dm8mainmenu22CMainMenuStateFinalize4NextEv", 0x20, 0xea000002);
#else
    fprintf(stderr, "AVISO: SONIC_FORCEMENUFINAL nao re-derivado p/ arm64, ignorado\n");
#endif
  }
  if (getenv("SONIC_FORCEEXITEM"))
    patch_ret0("_ZN9Sonic4F2F11isVisibleExENS_12EX_MENU_ITEME");
  /* gate3 do título: CStateInitialize::Next espera CDemoResourceManager IsValid()
     (recursos do attract-demo do evento 3) que nunca valida (id 2 não carrega).
     NOP no `beq` (offset +0x5c) faz a state machine avançar pro Opening/LogoMainFadeIn
     (mostra o título: bg + logo SONIC, que carregam) -> Waiting, SEM o crash que
     forçar IsValid->1 global causava (over-advance pro save). */
  if (!getenv("SONIC_KEEPDEMOGATE"))
#ifdef __aarch64__
    /* arm64 v3: o gate é `cbz w0,<skip>` após IsValid() em +0x54 -> NOP A64. */
    patch_word_at("_ZN2dm5title16CStateInitialize4NextEv", 0x54, 0xd503201f);
#else
    patch_word_at("_ZN2dm5title16CStateInitialize4NextEv", 0x5c, 0xe1a00000);
#endif
  /* fallback opt-in: forçar IsValid->1 global (crasha no save, só p/ depurar). */
  if (getenv("SONIC_FORCEDEMOGATE"))
    patch_retval("_ZThn20_NK2dm8resource20CResourceManagerTask7IsValidENS0_12EDemoEventID4TypeE", 1);
  /* 🔑 fingir SÓ o attract-demo do título (resType 6) como carregado, mantendo os
     outros recursos REAIS (evita o crash do force-global IsValid->1). A função local
     do is-loaded do title-demo = container CManagerState<CTitleViewTask>::Act+0x20
     (0x2522a4): checa o objeto demo global (null)->0. Forçar return 1 deixa o
     demo-gate do título/menu passar SEM o attract-demo (emblema vazio). */
  /* fake-sound do demo: por padrão NÃO aplicado (o launcher antigo setava sempre
     SONIC_NOFAKESOUND=1). Opt-in via SONIC_FAKESOUND p/ depurar o demo-gate:
     dmSoundEffectIsSetUpEnd->1 (o is-loaded que mais falha; sem som no demo é
     inofensivo). */
  if (getenv("SONIC_FAKESOUND")) {
    patch_retval("_ZN2dm2se23dmSoundEffectIsSetUpEndEv", 1);
  }

  /* 🔑 F2F age gate / GDPR consent: ao apertar "Press any button" o jogo chama
     showAgeGate (dialog Java de idade/consent que não temos) e espera a resposta.
     Bypass: isEnoughtAge->1 (idade ok => haveRemoveAgeGate=1) + isConsentCountry->0
     (não é país GDPR => pula o consent). Assim avança do título pro menu sem o dialog. */
  if (!getenv("SONIC_KEEPAGEGATE")) {
    patch_retval("_ZN12F2FExtension12isEnoughtAgeEv", 1);
    patch_ret0("_ZN12F2FExtension16isConsentCountryEv");
    patch_ret0("_ZN12F2FExtension19getIsConsentCountryEv");
#ifdef __aarch64__
    /* v3 refatorou o consent p/ F2FExtension::Legal (isConsentCountry não existe mais).
       O título pol a Legal::isCompleteAllState() (byte __f2f_legal_is_complete_all_state)
       que só vira 1 via callbacks Java do fluxo legal/consent que NÃO temos -> o tap
       do título nunca avança pro menu. Forçar completo + age gate feito. */
    patch_retval("_ZN12F2FExtension5Legal18isCompleteAllStateEv", 1);
    patch_retval("_ZN12F2FExtension3Age15haveDoneAgeGateEv", 1);
    patch_retval("_ZN12F2FExtension28INTERNAL_F2F_haveUserConsentEv", 1);
    patch_ret0("_ZN12F2FExtension5Legal13CONSENT_Legal26NEED_TO_SHOW_POPUP_IMPROVEEv");
#endif
  }

  if (!getenv("SONIC_NOSPLIT_DRAW_PHASE"))
    patch_arm_jump("_Z17amThreadCheckDrawl", (void *)sonic_amThreadCheckDraw);

  /* 🪶 LOWFX de volta como DEFAULT (FULLFX não resolveu os bugs reais — vídeo/crash/rastros
     — e custa perf). Bloom+sombras OFF por default. SONIC_FULLFX=1 restaura tudo;
     SONIC_NOBLOOM/SONIC_NOSHADOW overrides finos. (O fix REAL de rastro = glClear por frame,
     gated por SONIC_FBCLEAR, independente de FX.) */
  int sonic_lowfx = !env_flag_enabled("SONIC_FULLFX");
  /* - bloom (SsConstBloomIsEnable->0): pula extract hi-luminance + blur gaussiano
       + merge (3 passes full-screen por frame). Mantém água/efeitos de cena. */
  if (sonic_lowfx || env_flag_enabled("SONIC_NOBLOOM")) {
    patch_ret0("_Z20SsConstBloomIsEnablev"); /* bloom off */
    fprintf(stderr, "=== LOWFX: bloom desligado (perf) ===\n");
  }
#ifdef __aarch64__
  /* ArkOS/R36S Mali-G31 blob: nnCreatePowerIndexImage retorna com LR apontando
     para nnPtr32Encode e entra num loop no primeiro DrawFrame. Com bloom/lowfx
     desligado, essa LUT de post-effect nao e necessaria. */
  if (!env_flag_enabled("SONIC_KEEP_POWERINDEX")) {
    patch_ret0("nnCreatePowerIndexImage");
    fprintf(stderr, "=== LOWFX64: nnCreatePowerIndexImage desligado (ArkOS hang fix) ===\n");
  }

#endif
  /* 🌈 SONIC_FORCETONEMAP: força o TONE-MAP (HDR->LDR Reinhard) LIGADO. O tone-map é
     SEPARADO do bloom (SsConstTonemapIsEnable). Desligar o bloom pode ter matado o
     tone-map junto -> o HDR das luzes do cassino estoura pra branco. Forçar o tone-map
     ON (mantendo bloom off) deve corrigir a exposição SEM o blur caro. */
  if (env_flag_enabled("SONIC_FORCETONEMAP")) {
    patch_retval("_Z23SsConstTonemapIsEnablev", 1);
    fprintf(stderr, "=== SONIC_FORCETONEMAP: tone-map HDR->LDR forcado ON ===\n");
  }
  /* 🛡️ FIX crash ao sair da fase (default ON; SONIC_NO_RELSAFE desliga): redireciona
     _amDrawReleaseTexture pra nossa versão que valida a lista antes de liberar. */
  if (!getenv("SONIC_NO_RELSAFE")) {
    patch_arm_jump("_Z21_amDrawReleaseTextureP14AMS_REGISTLIST",
                   (void *)my_amDrawReleaseTexture);
    fprintf(stderr, "=== RELSAFE: _amDrawReleaseTexture protegido (stale-P do exit) ===\n");
#ifdef __aarch64__
    /* v3/arm64: o fix raiz é no ENQUEUE (RELSAFE64). O hook do exec acima vira só
       cinto-de-segurança (com o enqueue transformado em NOP ele nem é chamado). */
    sonic_install_relsafe64();
#endif
  }
  /* 🛡️ DEMOGUARD (default ON; SONIC_NO_DEMOGUARD desliga): protege o teardown do CStartDemo contra
     use-after-free. DUAS camadas: (1) hook do ep2::ReleaseInstance (release limpo p/ esse caminho);
     (2) recuperação GERAL por faixa de PC no crash handler (pega o destrutor por QUALQUER caminho —
     Act Clear->mapa, delete direto etc; foi o que faltava no crash do tester). */
  g_demoguard_on = getenv("SONIC_NO_DEMOGUARD") == NULL;
  if (g_demoguard_on) {
    patch_arm_jump("_ZN2gm10start_demo3ep210CStartDemo15ReleaseInstanceEv",
                   (void *)my_ep2_CStartDemo_ReleaseInstance);
    fprintf(stderr, "=== DEMOGUARD: ~CStartDemo protegido (hook ReleaseInstance + recuperacao geral por PC) ===\n");
#ifdef __aarch64__
    /* arm64/v3: as faixas de PC da recuperacao geral NAO sao os #defines v2 -> derivar
       dos simbolos (auto-adapta). dtor ep2 = span dos D0/D1/D2; amTexMgrDecRef = leaf. */
    {
      uintptr_t tb = (uintptr_t)text_base, lo = 0, hi = 0;
      const char *dts[] = { "_ZN2gm10start_demo3ep210CStartDemoD1Ev",
                            "_ZN2gm10start_demo3ep210CStartDemoD0Ev",
                            "_ZN2gm10start_demo3ep210CStartDemoD2Ev" };
      for (unsigned i = 0; i < 3; i++) {
        uintptr_t a = so_find_addr_safe(dts[i]) & ~(uintptr_t)1;
        if (a) { uintptr_t o = a - tb; if (!lo || o < lo) lo = o; if (o > hi) hi = o; }
      }
      if (lo) { g_csd_lo = lo; g_csd_hi = hi + 0x260; } /* +0x260 cobre o D2 (o maior) */
      uintptr_t td = so_find_addr_safe("amTexMgrDecRef") & ~(uintptr_t)1;
      if (td) { g_texdec_lo = td - tb; g_texdec_hi = g_texdec_lo + 0x80; }
      fprintf(stderr, "=== DEMOGUARD arm64: csd=[0x%lx,0x%lx) texdec=[0x%lx,0x%lx) ===\n",
              g_csd_lo, g_csd_hi, g_texdec_lo, g_texdec_hi);
    }
#endif
  }
  /* 🧪 SONIC_SIMDEMOCRASH: SIMULA o crash do tester chamando o ~CStartDemo DIRETO com um objeto
     garbage (o caminho REAL do tester NÃO passa pelo ReleaseInstance — ex.: teardown do Act Clear).
     O crash cai DENTRO do destrutor -> exercita a RECUPERAÇÃO GERAL por faixa de PC:
       - guarda ON (default): RECUPERA (redireciona pro epílogo/caller) e segue -> dá pra JOGAR.
       - guarda OFF (+SONIC_NO_DEMOGUARD): crash CRU (jogo fecha) -> prova que é a guarda que salva. */
  if (getenv("SONIC_SIMDEMOCRASH")) {
    int guard_off = getenv("SONIC_NO_DEMOGUARD") != NULL;
    uintptr_t dtor = so_find_addr_safe("_ZTv0_n12_N2gm10start_demo3ep210CStartDemoD0Ev");
    fprintf(stderr, "=== SIMDEMOCRASH: ~CStartDemo@0x%lx com this=garbage (guarda=%s) ===\n",
            (unsigned long)dtor, guard_off ? "OFF (deve FECHAR)" : "ON (deve RECUPERAR)");
    if (dtor) {
      void (*d)(void *) = (void (*)(void *))(dtor & ~(uintptr_t)1);
      d((void *)0x1);                           /* crash DENTRO do destrutor; guarda ON -> recupera */
      fprintf(stderr, "=== SIMDEMOCRASH: SOBREVIVI (recuperado=%lu) -> segue ===\n",
              g_demo_guard_recovered);
    }
  }
  /* 🔆 SONIC_FREEZETONEMAP (SUSPEITO #1 do cassino "Electric Road"): CONGELA a
     AUTO-EXPOSIÇÃO. ChangeToneMapParam(midgray,lwhite) é chamado por frame pela
     adaptação de exposição — seta os destinos s_tonemap_*_dst+flags. No gameplay essa
     adaptação dispara e a exposição vai pro errado -> fundo ESTOURA pra branco; no
     PAUSE a lógica congela -> exposição para -> correto (fundo preto, luzes finas).
     No-opar ChangeToneMapParam congela a exposição no valor de Reset (fixo) = igual
     ao pause durante o gameplay. Se o cassino ficar correto -> era a auto-exposição. */
  if (env_flag_enabled("SONIC_FREEZETONEMAP")) {
    patch_ret0("_ZN2gm3pfx7CPfxSys18ChangeToneMapParamEff");
    fprintf(stderr, "=== SONIC_FREEZETONEMAP: auto-exposicao CONGELADA (exposicao fixa) ===\n");
  }
  /* 🌑 no-op nos draws de sombra de objeto/motion: pula o passe de sombra
     (shadow-map / blob) — ganho de fillrate+drawcall no Mali.
     NAO toca em GmShadowBuildCheck (gate de loading -> travaria). */
  if (sonic_lowfx || env_flag_enabled("SONIC_NOSHADOW")) {
    patch_ret0("_Z18SsDrawObjectShadowmP10NNS_OBJECTP12_NNS_TEXLISTy");
    patch_ret0("_Z18SsDrawObjectShadowP10NNS_OBJECTP12_NNS_TEXLISTy");
    patch_ret0("_Z24SsDrawMotionObjectShadowmP10AMS_MOTIONP12_NNS_TEXLISTy");
    patch_ret0("_Z24SsDrawMotionObjectShadowP10AMS_MOTIONP12_NNS_TEXLISTy");
    fprintf(stderr, "=== LOWFX: sombras de objeto desligadas (perf) ===\n");
  }
  /* 💡 SONIC_NOLIGHTMASK (DIAG/teste do bug do cassino): desliga o post-effect de
     máscara de luz (amPostEFLightMaskDraw) + distortion. O "blob branco estourado" do
     cassino é esse light-mask. Se sumir com isso, é ele. (GmPlyPostEfctLightMaskColGet
     lê uma intensidade clampada a 0xff -> luz branca máxima quando alto.) */
  if (getenv("SONIC_NOLIGHTMASK")) {
    patch_ret0("_Z21amPostEFLightMaskDrawmPA16_fS0_");
    patch_ret0("_Z22amPostEFDistortionDrawmPA16_fS0_");
    fprintf(stderr, "=== SONIC_NOLIGHTMASK: light-mask/distortion post-FX desligados ===\n");
  }
  /* 💡 SONIC_NOPOSTFX (DIAG do cassino estourado): mata o EXECUTOR de post-effect
     inteiro (_amPostEFExecEffect + amPostEFUpdate). Se as faixas/blow-up do cassino
     sumirem -> é o sistema de post-effect (feixe de luz) acumulando. Isola a raiz. */
  if (getenv("SONIC_NOPOSTFX")) {
    patch_ret0("_Z19_amPostEFExecEffectl");
    patch_ret0("_Z14amPostEFUpdatev");
    fprintf(stderr, "=== SONIC_NOPOSTFX: executor de post-effect desligado ===\n");
  }
  /* 🌟 SONIC_NOGODRAY: desliga o GOD RAY (raios de luz do fundo, gm::mapfar::C_MGR).
     O cassino tem god-ray com radial blur que está ESTOURANDO em faixas brancas gigantes
     (gsGxGetGodRayRadialBlur). Pular o draw remove as faixas. Se sumir, é o god-ray. */
  if (getenv("SONIC_NOGODRAY")) {
    patch_ret0("_ZN2gm6mapfar5C_MGR14FuncDrawGodrayEP16_OBS_OBJECT_WORK");
    fprintf(stderr, "=== SONIC_NOGODRAY: god-ray (faixas de luz do fundo) desligado ===\n");
  }
  /* 💧 SONIC_NOWATERFX (opt-in, NÃO no LOWFX por default — mexe mais no visual):
     no-op nos EFEITOS extras de água (ripple/waterfall-split), mantendo a SUPERFÍCIE
     (a água não some). Reduz overdraw de alpha das cenas de água. */
  if (env_flag_enabled("SONIC_NOWATERFX")) {
    patch_ret0("GmEffectWaterRippleBuild");
    patch_ret0("GmEffectWaterRippleFlush");
    patch_ret0("GmGmkWaterfallSplitBuild");
    patch_ret0("GmGmkWaterfallSplitFlush");
    fprintf(stderr, "=== SONIC_NOWATERFX: ripple/waterfall desligados (perf) ===\n");
  }

  void *env = NULL, *vm = NULL;
  jni_shim_init(&vm, &env);
  void *thiz = (void *)0x53000001; /* fake jobject/jclass */
  g_env = env; g_thiz = thiz;

  /* JNI_OnLoad: o Android chamaria isso ao carregar o .so, setando o global
     JavaVM (f2fextension GetJNIEnv lê esse global -> NULL deref sem isso). */
  { int (*jniOnLoad)(void *, void *) = (void *)so_find_addr_safe("JNI_OnLoad");
    if (jniOnLoad) { int v = jniOnLoad(vm, NULL);
      fprintf(stderr, "JNI_OnLoad(vm) = 0x%x\n", v); }
    else fprintf(stderr, "AVISO: JNI_OnLoad não encontrado\n"); }

  if (egl_shim_create_window() != 0) {
    fprintf(stderr, "ERRO: nenhum contexto OpenGL ES compativel foi criado\n");
    return 1;
  }
  egl_shim_bind_main();  /* GLSurfaceView: contexto current na thread do DrawFrame */

  RES(init,               "Java_com_mineloader_fox_foxJniLib_init");
  RES(SetGamePath,        "Java_com_mineloader_fox_foxJniLib_SetGamePath");
  RES(coreGetLPKFileInfo, "Java_com_mineloader_fox_foxJniLib_coreGetLPKFileInfo");
  RES(SetLanguageId,      "Java_com_mineloader_fox_foxJniLib_SetLanguageId");
  RES(DrawEGLCreated,     "Java_com_mineloader_fox_foxJniLib_DrawEGLCreated");
  RES(DrawFrame,          "Java_com_mineloader_fox_foxJniLib_DrawFrame");
  RES(GameProcess,        "Java_com_mineloader_fox_foxJniLib_GameProcess");
  RES(FileProcess,        "Java_com_mineloader_fox_foxJniLib_FileProcess");
  RES(HasController,      "Java_com_mineloader_fox_foxJniLib_HasController");
  RES(SetPadData,         "Java_com_mineloader_fox_foxJniLib_SetPadData");
  RES(SetTPData,          "Java_com_mineloader_fox_foxJniLib_SetTPData");
  RES(resumeEvent,        "Java_com_mineloader_fox_foxJniLib_resumeEvent");

  const char *gamedir = getenv("SONIC_DATADIR");
  if (!gamedir) gamedir = ".";
  const char *lpk = getenv("SONIC_LPK");
  /* default esperto: o v3/arm64 usa data/data.obb; v2 usa o main.22. Importante
     porque no NextOS novo (pm_platform_helper/systemd-run) os envs do launcher
     NAO chegam ao binario — o default precisa acertar sozinho. */
  if (!lpk) {
    if (access("data/data.obb", F_OK) == 0) lpk = "data/data.obb";
    else lpk = "data/main.22.com.sega.sonic4episode2.obb"; /* o OBB = LPK */
  }

  /* SetGamePath(env, thiz, int id, String path) -> tsSetFileRootPath(id,path,0):
     id==255(0xff) => tsInitFileRootLPK(path) = fopen+indexa o LPK (o OBB!);
     id<254       => guarda 'path' como root de arquivos soltos.
     PRECISA vir ANTES do init (init lê font.nft de dentro do LPK). */
  fprintf(stderr, "=== fox: SetGamePath(255, LPK=%s) ===\n", lpk);
  if (fox.SetGamePath) fox.SetGamePath(env, thiz, (void *)255, jni_shim_new_string(lpk));

  fprintf(stderr, "=== fox: SetGamePath(0, dir=%s) ===\n", gamedir);
  if (fox.SetGamePath) fox.SetGamePath(env, thiz, (void *)0, jni_shim_new_string(gamedir));

  /* idioma: tabela lang_name_tbl = 0:JP 1:US(inglês) 2:FR 3:IT 4:GE 5:SP 6:KO 7:CH 8:TA.
     Id 1 = US/inglês (id 0 carregava as variantes _JP japonesas). */
  fprintf(stderr, "=== fox: SetLanguageId(1=US/EN) ===\n");
  if (fox.SetLanguageId) fox.SetLanguageId(env, thiz, 1);
  /* 🔑 SetLanguageId seta o global do SetAndroidLanguage (usado no TÍTULO), mas o
     MENU lê de OUTRO global via GsEnvGetLanguage() (default 0=JP) -> menu em japonês!
     Forçar GsEnvGetLanguage()->1 (US) deixa o menu/UI em inglês também. */
  if (!getenv("SONIC_KEEPJP"))
    patch_retval("_Z16GsEnvGetLanguagev", 1);

  /* f2fextension (camada F2F/ads): precisa do context/JavaVM senão getF2FJavaVM()
     retorna NULL -> crash em Android_getLocalPath. Chamar os setups JNI. */
  void (*f2f_setCtx)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_nativeSetContext");
  void (*f2f_setObj)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_SetJavaObj");
  void (*f2f_setApk)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_nativeSetApkPath");
  if (f2f_setObj) { fprintf(stderr, "f2f SetJavaObj\n");    f2f_setObj(env, thiz, thiz); }
  if (f2f_setCtx) { fprintf(stderr, "f2f nativeSetContext\n"); f2f_setCtx(env, thiz, thiz); }
  if (f2f_setApk) { fprintf(stderr, "f2f nativeSetApkPath\n");
                    f2f_setApk(env, thiz, jni_shim_new_string("sonic4ep2.apk")); }

  /* 🪶 SONIC_RENDERSCALE=N (50..99, default 100=off): downscale interno. O engine
     renderiza em N% e o present faz upscale p/ a janela cheia. Maior lever de GPU
     (fillrate ~ pixel²), custo = nitidez. OPT-IN. ⚠️ se sair no canto (não upscale),
     precisa do override de viewport no present. */
  int render_w = sonic_screen_w, render_h = sonic_screen_h;
  { const char *rs = getenv("SONIC_RENDERSCALE"); int pct = rs ? atoi(rs) : 100;
    if (pct >= 50 && pct < 100) {
      render_w = (sonic_screen_w * pct / 100) & ~1;
      render_h = (sonic_screen_h * pct / 100) & ~1;
      fprintf(stderr, "=== SONIC_RENDERSCALE %d%%: render %dx%d (janela %dx%d) ===\n",
              pct, render_w, render_h, sonic_screen_w, sonic_screen_h);
    } }

  /* 🔑 setScreenSize: a engine (foxShaderInit -> amRenderCreate) lê a resolução de
     tela de um global (2 floats) p/ dimensionar os FBOs/render targets. O Java
     chamaria setScreenSize(w,h) no onSurfaceChanged; SEM isso o global fica 0.0/0.0
     => FBO 0x0 INCOMPLETE => glDraw* falham (GL_INVALID_FRAMEBUFFER_OPERATION) =>
     TELA PRETA. setScreenSize faz `stm {w,h}` cru => são jfloat em regs CORE (JNI
     softfp) => passamos os BITS do float em r2/r3 (declarar args como unsigned, NÃO
     float, senão o ABI hardfp manda em s0/s1). */
  {
    extern int sonic_screen_w, sonic_screen_h;
#ifdef __aarch64__
    /* AArch64 (AAPCS64): jfloat vai em registrador FP (s0/s1) — passar float REAL. */
    void (*setScreenSize)(void *, void *, float, float) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenSize");
    void (*setScreenScaleDesity)(void *, void *, float) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenScaleDesity");
    if (setScreenSize) {
      fprintf(stderr, "=== setScreenSize(%d x %d) [A64 fp] ===\n",
              render_w, render_h);
      setScreenSize(env, thiz, (float)render_w, (float)render_h);
    } else {
      fprintf(stderr, "=== setScreenSize ausente na v3; fox.init recebe %dx%d ===\n",
              render_w, render_h);
    }
    if (setScreenScaleDesity) setScreenScaleDesity(env, thiz, 1.0f);
#else
    /* armhf softfp: jfloat vem em regs CORE — passar os BITS do float. */
    void (*setScreenSize)(void *, void *, unsigned, unsigned) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenSize");
    void (*setScreenScaleDesity)(void *, void *, unsigned) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenScaleDesity");
    if (setScreenSize) {
      union { float f; unsigned u; } w, h;
      w.f = (float)render_w; h.f = (float)render_h;
      fprintf(stderr, "=== setScreenSize(%d x %d) bits=%08x %08x ===\n",
              render_w, render_h, w.u, h.u);
      setScreenSize(env, thiz, w.u, h.u);
    } else fprintf(stderr, "AVISO: setScreenSize não encontrado\n");
    if (setScreenScaleDesity) {
      union { float f; unsigned u; } s; s.f = 1.0f;
      setScreenScaleDesity(env, thiz, s.u);  /* densidade/escala = 1.0 */
    }
#endif
  }

  /* save path: stsSavePathData (buffer global) é o prefixo do save; vazio => o save
     vira "/foxsave_0.dat" no root (sem permissão) => save falha, e o Story Mode pode
     gatear nisso. Setar p/ o dir gravável (SONIC_DATADIR). */
  if (!getenv("SONIC_KEEPSAVEPATH")) {
    char *sp = (char *)so_find_addr_safe("stsSavePathData");
    const char *dd = getenv("SONIC_DATADIR"); if (!dd) dd = ".";
    if (sp) {
      size_t n = strlen(dd);
      snprintf(sp, 196, "%s%s", dd, (n > 0 && dd[n - 1] == '/') ? "" : "/");
      fprintf(stderr, "=== save path = %s ===\n", sp);
    }
  }

  /* 🔑🔑 init(env, thiz, WIDTH, HEIGHT): a JNI init repassa args 3/4 p/ fox_Init(w,h)
     -> amDrawInitVideo(w,h) que dimensiona _am_draw_video (os FBOs/render targets).
     Passávamos NULL,NULL = 0,0 => FBO 0x0 INCOMPLETE => glDraw* falham => TELA PRETA.
     Passar a resolução REAL (1280x720) faz os FBOs ficarem completos e renderizar. */
  fprintf(stderr, "=== fox: init(w=%d h=%d) ===\n", render_w, render_h);
  if (fox.init) fox.init(env, thiz, (void *)(intptr_t)render_w,
                         (void *)(intptr_t)render_h);

  fprintf(stderr, "=== fox: DrawEGLCreated ===\n");
  if (fox.DrawEGLCreated) fox.DrawEGLCreated(env, thiz);

  /* resumeEvent: o jogo começa PAUSADO (isGamePause); sem resume, fox_FrameUpdate
     retorna cedo e PULA amTaskExecute (a state machine) -> nada avança -> preto. */
  fprintf(stderr, "=== fox: resumeEvent (unpause) ===\n");
  if (fox.resumeEvent) fox.resumeEvent(env, thiz);
  sonic_save_bootstrap_init();

  /* FileProcess = amFS_proc = loop da THREAD de file-system (cond_wait quando
     ocioso). Roda na PRÓPRIA thread; o game thread enfileira requests e sinaliza. */
  if (fox.FileProcess) {
    pthread_t fs;
    pthread_create(&fs, NULL, fs_thread_fn, NULL);
    fprintf(stderr, "=== FS thread iniciada (FileProcess) ===\n");
  }

  if (env_flag_enabled("SONIC_FORCE_NATIVE_SAVE_LOAD"))
    sonic_native_save_load_start();

  if (getenv("SONIC_USEUSERSETUP")) {
    void (*GsUserSetupStart)(unsigned long, unsigned long) =
        (void *)so_find_addr_safe("_Z16GsUserSetupStartmm");
    if (GsUserSetupStart) {
      fprintf(stderr, "=== GsUserSetupStart(uid=0, account=0) ===\n");
      GsUserSetupStart(0, 0);
    } else {
      fprintf(stderr, "AVISO: GsUserSetupStart nao encontrado\n");
    }
  }

  /* intro video: a engine chama Android_playIntroVideo e espera o callback
     callBackIntroVideo (Java tocaria o mp4 e sinalizaria o fim). Sem vídeo,
     sinalizamos "terminado" cedo p/ a engine seguir pro título/menu. */
  void (*introCB)(JEnv, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_callBackIntroVideo");
#ifdef __aarch64__
  /* v3: NÃO existe callBackIntroVideo. O intro video SETA o bit PAUSE_INTRO_VIDEO (0x80)
     no mask de pause do F2F App (App::activeGame(false, 0x80), visto no ALOG) e o Java
     tocaria o .mp4 e chamaria F2FAndroidJNI_activeGame(true, 0x80) ao terminar. Sem a
     camada de vídeo o bit fica PRESO -> F2F App "pausado" -> título não vai pro menu.
     Fix: retomar nós mesmos (mask &= ~0x80) periodicamente nos primeiros frames. */
  void (*f2f_app_active)(int, unsigned) = (void *)so_find_addr_safe(
      "_ZN12F2FExtension3App10activeGameEbNS0_12REASON_PAUSEE");
  if (!f2f_app_active)
    fprintf(stderr, "AVISO: App::activeGame nao encontrado (resume intro-video off)\n");
#endif
  /* callback do ad intersticial: ao selecionar Start, onMainMenuToMainGame chama
     showInterstitial que GUARDA um callback (que cria o jogo/world map) e espera o
     ad fechar (callbackInterstitialAds do Java). Sem ad Java, disparamos nós: chamar
     callbackInterstitialAds(type=0, callback=0) dispara o callback guardado -> jogo. */
  void (*interCB)(JEnv, void *, int, int) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_callbackInterstitialAds");

  /* input: abrir o 1º gamepad SDL (se houver) */
  SDL_GameController *pad = NULL;
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
      if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
    fprintf(stderr, "=== gamepad: %s ===\n", pad ? "aberto" : "nenhum");
  }
  /* 🔧 fox pad bits = MAPA REAL do jogo (decompilado de foxJniLib.s_remapKey, o keycode->bit
     do próprio jogo). gmPadValue = (1<<bitindex). Antes os bits de Y e ombros estavam ERRADOS
     (Y=0x100 era na verdade L1 -> Y "virava left" no world map e sua função nunca disparava).
     Xbox: A=96 X=99 B=97 Y=100 START=108 SELECT=109 L1=102 L2=104 R1=103 R2=105 L3=106 R3=107. */
  #define FOX_UP     0x0001  /* bit0  DPAD_UP   19 */
  #define FOX_DOWN   0x0002  /* bit1  DPAD_DOWN 20 */
  #define FOX_LEFT   0x0004  /* bit2  DPAD_LEFT 21 */
  #define FOX_RIGHT  0x0008  /* bit3  DPAD_RIGHT 22 */
  #define FOX_Y      0x0010  /* bit4  BUTTON_Y  100  (era 0x100 = L1, ERRADO) */
  #define FOX_A_GAME 0x0020  /* bit5  BUTTON_A  96  (decide/jump) */
  #define FOX_X      0x0040  /* bit6  BUTTON_X  99 */
  #define FOX_B      0x0080  /* bit7  BUTTON_B  97  (cancel) */
  #define FOX_L1     0x0100  /* bit8  BUTTON_L1 102 */
  #define FOX_L2     0x0200  /* bit9  BUTTON_L2 104 */
  #define FOX_L3     0x0400  /* bit10 THUMBL    106 */
  #define FOX_R1     0x0800  /* bit11 BUTTON_R1 103 */
  #define FOX_R2     0x1000  /* bit12 BUTTON_R2 105 */
  #define FOX_R3     0x2000  /* bit13 THUMBR    107 */
  #define FOX_BACK   0x4000  /* bit14 BUTTON_SELECT 109 / BACK */
  #define FOX_START  0x8000  /* bit15 BUTTON_START 108 (confirm título/pause) */
  #define FOX_A_MENU (FOX_A_GAME|FOX_START)  /* 0x8020 = A(decide)+START(confirm título) */
  #define FOX_PAUSE  FOX_START               /* pause in-game = START (check do engine é pad&0xC000) */

  /* 🔑 demo-resource: o gate do título (CDemoResourceManager::IsValid evt3) trava
     pq o recurso "MenuDraw" (tipo 2) não está setado (dmMenuDrawIsSetUpEnd=0).
     dmMenuDrawSetUp() cria o singleton MenuDraw -> o is-loaded passa. A engine não
     o chama no nosso fluxo; chamar aqui pode destravar o gate NATURALMENTE (sem o
     NOP do beq) e deixar o menu renderizar. */
  if (!getenv("SONIC_NOSETUPS")) {  /* default-on: cria os singletons do demo set —
       o IsValid quer os DADOS buildados (o build assíncrono do manager não completa;
       type 6=attract-demo precisa de stage). Default = título via bypass do beq. */
    const char *setups[] = {
      "_ZN2dm10menucommon17dmMenuCommonSetUpEv", /* type 1 */
      "_ZN2dm8menudraw15dmMenuDrawSetUpEv",      /* type 2 */
      "_ZN2dm2se18dmSoundEffectSetUpEv",         /* type 3 */
      "_ZN2dm7message11SystemSetUpEv",           /* message */
    };
    for (unsigned i = 0; i < sizeof(setups)/sizeof(setups[0]); i++) {
      void (*fn)(void) = (void *)so_find_addr_safe(setups[i]);
      if (fn) { fprintf(stderr, "=== setup: %s ===\n", setups[i]); fn(); }
      else fprintf(stderr, "AVISO: setup %s não encontrado\n", setups[i]);
    }
  }

  /* "conectar" o pad: HasController(env, thiz, 1) chama Sonic4F2F::setController(true);
     SetPadData(-2/-5) replica os toggles que o Java fazia para o wrapper fox. */
  if (fox.HasController) fox.HasController(env, thiz, 1);
  if (fox.SetPadData) {
    fox.SetPadData(env, thiz, -2, 0, 0, 0, 0, 0);
    fox.SetPadData(env, thiz, -5, 0, 0, 0, 0, 0);
  }

  /* 🔎 SONIC_STATELOG: loga por segundo os gates do fluxo título->menu (debug). */
  int (*gs_setup_done)(unsigned) = (void *)so_find_addr_safe("_Z22GsUserSetupIsCompletedj");
  if (!gs_setup_done) gs_setup_done = (void *)so_find_addr_safe("_Z22GsUserSetupIsCompletedm");
  int (*gs_user_enable)(unsigned) = (void *)so_find_addr_safe("_Z14GsUserIsEnablem");
  int (*sjni_upshell)(void) = (void *)so_find_addr_safe("_Z18SJni_IsUpshellShowv");

  fprintf(stderr, "=== entrando no loop principal (GameProcess/DrawFrame) ===\n");
  unsigned long frame = 0;
  int prev_a = 0;             /* borda de A p/ disparar interCB 1x por seleção */
  long inter_fire_at = -1;    /* frame agendado p/ 1 disparo de interCB */
  const char *interat = getenv("SONIC_INTERAT"); /* override: 1 disparo no frame N */
  const char *ar = getenv("SONIC_AUTORIGHT_AFTER");
  long autoright_after = ar ? atol(ar) : -1;
  const char *aj = getenv("SONIC_AUTOJUMP_AT");
  long autojump_at = aj ? atol(aj) : -1;
  const char *ap = getenv("SONIC_AUTOPAUSE_AT");
  long autopause_at = ap ? atol(ap) : -1;
  int autopause_state = 0;
  int start_was_down = 0;
  long inter_last_fire_frame = -1000000;
  int inter_gameplay_ignored = 0;
  int prev_mask = 0;
  const char *fs = getenv("SONIC_FRAME_SLEEP_US");
  long frame_sleep_us = fs ? atol(fs) : 0;
  fprintf(stderr, "=== frame sleep us: %ld ===\n", frame_sleep_us);
  int sonic_looplog = env_flag_enabled("SONIC_LOOPLOG");
  g_fbclear = getenv("SONIC_FBCLEAR") != NULL;
  if (g_fbclear) fprintf(stderr, "=== SONIC_FBCLEAR: glClear por frame LIGADO ===\n");
  int gameplay_start_delay_done = 0;
  /* 🔑 CONTINUE/SAVE-COM-PROGRESSO: ao reabrir com save que tem fase interrompida
     (qualquer mundo/mapa já jogado), o título entra em CStateWaitViewPausing:
     OnEnter chama SetContinueShow() + SetContinueStart(0) e Next() fica polando
     isContinueStart() esperando 1 (continuar a fase) ou 2 (ir pro world map).
     Esse 1/2 só viria do diálogo Java SetContinueFlag (que não temos) -> trava
     eterna no título, menu nunca aparece. A flag de continue (global lida SÓ dentro
     de CStateWaitViewPausing::Next) é dirigida aqui: default 2 = caminho nativo
     ClearInterruptionData -> world map/menu (progresso de fases preservado em
     SProgress). SONIC_CONTINUE_MODE=1 resume direto na fase interrompida. */
  int (*sonic_isContinueStart)(void) =
      (void *)so_find_addr_safe("_Z15isContinueStartv");
  void (*sonic_SetContinueStart)(int) =
      (void *)so_find_addr_safe("_Z16SetContinueStarti");
  int sonic_continue_mode = 2;
  { const char *cm = getenv("SONIC_CONTINUE_MODE"); if (cm && *cm) sonic_continue_mode = atoi(cm); }
  /* DrawFrame(JNIEnv*, jobject, jint) grava seu terceiro argumento diretamente
     em continueFlag antes de renderizar. O loader antigo declarava apenas dois
     argumentos no arm64; x2 ficava com lixo e corrompia o fluxo do titulo. Use a
     API exportada para o estado inicial e passe zero explicitamente a cada frame. */
  if (sonic_SetContinueStart) {
    sonic_SetContinueStart(0);
    fprintf(stderr, "=== continue flag inicializada pela API -> 0 ===\n");
  }
  int sonic_continue_disabled = getenv("SONIC_NO_CONTINUE_DRIVE") != NULL;
  long sonic_continue_log_n = 0;
  /* 🗺️ SONIC_WARP_STAGE=N: warp direto pra fase N. Força sm_select_stage_id=N (o ID
     que o world map carrega no confirm) + injeta confirm no world map. Enumerar N até
     cair na Electric Road (Episode Metal). Debug only. */
  int *wm_sel_id = (int *)so_find_addr_safe("_ZN2dm9world_map4CFix18sm_select_stage_idE");
  long warp_stage = getenv("SONIC_WARP_STAGE") ? atol(getenv("SONIC_WARP_STAGE")) : -1;
  if (warp_stage >= 0) fprintf(stderr, "=== SONIC_WARP_STAGE=%ld (sel_id=%p) ===\n", warp_stage, (void*)wm_sel_id);
  short *gm_direct = (short *)so_find_addr_safe("gmPaddirectFromPlayer0");
  short *gm_lx = (short *)so_find_addr_safe("gmPadAnalogLXFromPlayer0");
  short *gm_ly = (short *)so_find_addr_safe("gmPadAnalogLYFromPlayer0");
  if (getenv("SONIC_INPUTLOG"))
    fprintf(stderr, "=== gmPad globals direct=%p lx=%p ly=%p ===\n",
            (void *)gm_direct, (void *)gm_lx, (void *)gm_ly);
  /* 🔎 DIAG Y=Left (SONIC_KEYDUMP=1): o bug "Y age como Left no level select" não é
     colisão de bit no nosso FOX mask (Y=0x100, LEFT=0x4 no .data). Hipótese: o engine
     REMAPEIA os bits de tecla de menu (g_gs_env_key_*) em runtime (keymap OUYA/alt).
     Dump dos valores REAIS dessas globais em runtime fecha a questão: se LEFT != 0x4
     (ou == 0x100), é remap. Só LÊ globais (zero efeito no jogo). */
  short *gk_right  = (short *)so_find_addr_safe("g_gs_env_key_right");
  short *gk_left   = (short *)so_find_addr_safe("g_gs_env_key_left");
  short *gk_down   = (short *)so_find_addr_safe("g_gs_env_key_down");
  short *gk_up     = (short *)so_find_addr_safe("g_gs_env_key_up");
  short *gk_decide = (short *)so_find_addr_safe("g_gs_env_key_decide");
  short *gk_cancel = (short *)so_find_addr_safe("g_gs_env_key_cancel");
  long keydump_last = -100000;
  /* Special/bonus stage (moedas): sinal por-frame determinístico.
     g_SsMain = ss::CMain::s_main (singleton da special stage) @vaddr 0x99e480.
     Não é símbolo exportado, então resolvo via o vizinho exportado
     ss::CNet::s_instance (0x99e4a4) e subtraio 0x24. *pp != NULL => estamos na
     special stage. Necessário porque a special stage carrega por
     EvSpecialStageStart/CSSLoadingTask, caminho que NÃO loga marcador de
     "game start" -> sonic_game_started fica preso em 0 -> o A vira FOX_A_MENU
     (bit 0x8000) e o check de pausa da special stage (pad & 0xC000) faz o pulo
     PAUSAR. Em fases normais o log já seta started=1 (por isso só a bônus falha).
     FOX_A_GAME (0x20) mantém o bit "decide menu" (0x20), só dropa o 0x8000 do
     pause: confirma o resultado da bônus normalmente E acaba com a pausa. */
  void **sonic_ss_main_pp = NULL;
  {
    uintptr_t cnet = so_find_addr_safe("_ZN2ss4CNet10s_instanceE");
    /* g_SsMain = ss::CMain::s_main (não exportado): fica ADJACENTE a CNet::s_instance no .bss,
       mas o OFFSET é por-versão. v2 armv7: CNet@0x99e4a4, s_main@0x99e480 (−0x24). No v3 arm64
       o layout muda (CNet@0xd240c0) e o −0x24 daria um global ALEATÓRIO → se não-nulo, o jogo
       acha que está SEMPRE em special-stage e quebra a confirmação de menu. Então só computo
       quando reconheço o layout v2; senão NULL (fallback = sonic_game_started, seguro). */
    if (cnet) {
      uintptr_t cnet_off = cnet - (uintptr_t)text_base;
      if (cnet_off == 0x99e4a4UL)                  /* v2 armv7 */
        sonic_ss_main_pp = (void **)(cnet - 0x24);
      /* TODO v3 arm64: achar o s_main real (RE com gameplay-test) e setar o offset certo. */
    }
    if (getenv("SONIC_INPUTLOG"))
      fprintf(stderr, "=== g_SsMain pp=%p (cnet_off=0x%lx) ===\n", (void *)sonic_ss_main_pp,
              cnet ? (unsigned long)(cnet - (uintptr_t)text_base) : 0UL);
  }
  for (;;) {
    sonic_frame_for_imports = frame;
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] begin\n", frame);
    /* --- input: drenar eventos SDL + montar a máscara fox + SetPadData --- */
    SDL_Event ev; while (SDL_PollEvent(&ev)) { /* drena (quit etc) */ }
    int mask = 0;
    int lx = 0, ly = 0;
    int rx = 0, ry = 0;
    int lt = 0, rt = 0;
    int start_down = 0;
    /* gameplay = fase normal (started por log) OU special/bonus stage ativa. */
    int sonic_in_gameplay = sonic_game_started ||
        (sonic_ss_main_pp && *sonic_ss_main_pp);
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    sonic_check_exit_hotkey(pad, ks);
    if (ks) {
      if (ks[SDL_SCANCODE_UP])    { mask |= FOX_UP;    ly = -32768; }
      if (ks[SDL_SCANCODE_DOWN])  { mask |= FOX_DOWN;  ly =  32767; }
      if (ks[SDL_SCANCODE_LEFT])  { mask |= FOX_LEFT;  lx = -32768; }
      if (ks[SDL_SCANCODE_RIGHT]) { mask |= FOX_RIGHT; lx =  32767; }
      if (ks[SDL_SCANCODE_W])     { mask |= FOX_UP;    ly = -32768; }
      if (ks[SDL_SCANCODE_S])     { mask |= FOX_DOWN;  ly =  32767; }
      if (ks[SDL_SCANCODE_A])     { mask |= FOX_LEFT;  lx = -32768; }
      if (ks[SDL_SCANCODE_D])     { mask |= FOX_RIGHT; lx =  32767; }
      if (ks[SDL_SCANCODE_SPACE]||ks[SDL_SCANCODE_Z])
        mask |= sonic_in_gameplay ? FOX_A_GAME : FOX_A_MENU;
      if (ks[SDL_SCANCODE_C])     mask |= FOX_B;
      if (ks[SDL_SCANCODE_X])     mask |= FOX_X;
      if (ks[SDL_SCANCODE_V] || ks[SDL_SCANCODE_Y]) mask |= FOX_Y;
      if (ks[SDL_SCANCODE_Q])     mask |= FOX_L1;
      if (ks[SDL_SCANCODE_E])     mask |= FOX_R1;
      if (ks[SDL_SCANCODE_1])     { mask |= FOX_L2; lt = 32767; }
      if (ks[SDL_SCANCODE_3])     { mask |= FOX_R2; rt = 32767; }
      /* L3/R3 (THUMBL/THUMBR) ficam fora do mask: não são ações no Sonic 4.
         (Com o mapa de bits corrigido já não colidem com pause/confirm; omitir é só
         pra não injetar input espúrio de stick-click.) */
      (void)0;
      if (ks[SDL_SCANCODE_ESCAPE]) mask |= FOX_BACK;
      if (ks[SDL_SCANCODE_RETURN]) {
        start_down = 1;
        if (!sonic_in_gameplay) mask |= FOX_A_MENU | FOX_START;
        else mask |= FOX_PAUSE;
      }
    }
    if (pad) {
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))    { mask |= FOX_UP;    ly = -32768; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  { mask |= FOX_DOWN;  ly =  32767; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  { mask |= FOX_LEFT;  lx = -32768; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) { mask |= FOX_RIGHT; lx =  32767; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A))
        mask |= sonic_in_gameplay ? FOX_A_GAME : FOX_A_MENU;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) mask |= FOX_B;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) mask |= FOX_X;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) mask |= FOX_Y;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) mask |= FOX_L1;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= FOX_R1;
      /* LEFTSTICK/RIGHTSTICK (L3/R3) NÃO entram no mask: colidem com PAUSE(0x4000)/
         não são ações no Sonic 4. Ver nota acima. */
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) mask |= FOX_BACK;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
        start_down = 1;
        if (!sonic_in_gameplay) mask |= FOX_A_MENU | FOX_START;
        else mask |= FOX_PAUSE;
      }
      Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
      Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      Sint16 arx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
      Sint16 ary = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
      Sint16 alt = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
      Sint16 art = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
      if (ax < -12000) { mask |= FOX_LEFT;  lx = ax; }
      else if (ax > 12000) { mask |= FOX_RIGHT; lx = ax; }
      if (ay < -12000) { mask |= FOX_UP;    ly = ay; }
      else if (ay > 12000) { mask |= FOX_DOWN; ly = ay; }
      if (arx < -12000 || arx > 12000) rx = arx;
      if (ary < -12000 || ary > 12000) ry = ary;
      if (alt > 12000) { mask |= FOX_L2; lt = alt; }
      if (art > 12000) { mask |= FOX_R2; rt = art; }
    }
    /* auto-press de teste: o trigger é a borda 0->1 (1 frame), então alterna
       0x8000/0 a cada frame após o título carregar -> trigger frequente p/ vencer
       a corrida com o poll do CStateWaiting::Next (SONIC_AUTOSTART). */
    /* press ÚNICO de teste (não contínuo): aperta confirm uma vez ~frame 420 por
       ~5 frames e SOLTA (pressionar contínuo reseta a sequência de saída do título). */
    if (env_flag_enabled("SONIC_AUTOSTART")) {
      /* Pulsos curtos e espaçados: o primeiro sai do título; o segundo confirma
         Start/New Game. Pulsos extras ficam opt-in, porque depois que a fase
         carrega eles podem acionar flows de ad/menu e bagunçar o gameplay. */
      int extra = getenv("SONIC_AUTOSTART_EXTRA") != NULL;
      int autopulse =
          (frame >= 600 && frame < 606) ||
          (frame >= 900 && frame < 906) ||
          (extra && frame >= 1300 && frame < 1306) ||
          (extra && frame >= 1700 && frame < 1706);
      if (autopulse) {
        if (frame == 600 || frame == 900 || frame == 1300 || frame == 1700)
          fprintf(stderr, "=== AUTOSTART A pulse @frame %lu ===\n", frame);
        mask |= FOX_A_MENU;
      }
    }
    /* 🗺️ WARP: no world map, força o stage-id selecionado = warp_stage e injeta confirm
       (FOX_A_MENU) numa janela -> carrega a fase N direto. */
    if (warp_stage >= 0 && !sonic_game_started) {
      if (wm_sel_id) *wm_sel_id = (int)warp_stage;
      /* confirm pra ENTRAR na fase = decide (A=0x20), NÃO FOX_A_MENU (0x8020 tem START
         0x8000 que pode pausar). Pulsos espaçados (borda) p/ disparar o decide 1x. */
      if ((frame >= 1150 && frame < 1154) || (frame >= 1180 && frame < 1184) ||
          (frame >= 1210 && frame < 1214)) {
        if (frame == 1150) fprintf(stderr, "=== WARP confirm stage=%ld (decide 0x20) @frame %lu ===\n", warp_stage, frame);
        mask |= FOX_A_GAME;
      }
    }
    /* 🔎 SONIC_INPUTSEQ="F:MASK:LEN,F:MASK:LEN,..." (debug): injeta MASK (bits FOX)
       do frame F por LEN frames. Roteiro determinístico p/ reproduzir fluxos
       (entrar na fase, pausar, navegar o pause menu) sem controle físico. */
    {
      static int seq_n = -1;
      static long seq_f[48], seq_len[48]; static int seq_m[48];
      if (seq_n < 0) {
        seq_n = 0;
        const char *s = getenv("SONIC_INPUTSEQ");
        if (s) {
          char *dup = strdup(s), *save = NULL;
          for (char *tk = strtok_r(dup, ",", &save); tk && seq_n < 48;
               tk = strtok_r(NULL, ",", &save)) {
            long f0 = 0, ln = 4; unsigned mm = 0;
            if (sscanf(tk, "%ld:%i:%ld", &f0, &mm, &ln) >= 2) {
              seq_f[seq_n] = f0; seq_m[seq_n] = (int)mm;
              seq_len[seq_n] = ln > 0 ? ln : 4; seq_n++;
            }
          }
          free(dup);
          fprintf(stderr, "=== INPUTSEQ: %d passos ===\n", seq_n);
        }
      }
      for (int i = 0; i < seq_n; i++)
        if ((long)frame >= seq_f[i] && (long)frame < seq_f[i] + seq_len[i]) {
          if ((long)frame == seq_f[i])
            fprintf(stderr, "=== INPUTSEQ passo %d mask=0x%04x @frame %lu ===\n",
                    i, seq_m[i], frame);
          mask |= seq_m[i];
        }
    }
    /* 🔎 SONIC_INPUTFILE=/dev/shm/sonic_pad (debug): mask dinâmico via arquivo.
       Conteúdo "0xNNNN" -> OR no mask a cada frame enquanto o arquivo existir.
       Permite dirigir o jogo interativamente por ssh (echo 0x20 > f; rm f). */
    {
      static int inpf = -1; static const char *inpath;
      if (inpf < 0) { inpath = getenv("SONIC_INPUTFILE"); inpf = inpath ? 1 : 0; }
      if (inpf) {
        FILE *f = fopen(inpath, "r");
        if (f) { unsigned mm = 0;
          if (fscanf(f, "%i", &mm) == 1 && mm) {
            mask |= (int)mm;
            static unsigned last_mm = 0;
            if (mm != last_mm) { last_mm = mm;
              fprintf(stderr, "=== INPUTFILE mask=0x%04x @frame %lu ===\n", mm, frame); }
          }
          fclose(f);
        }
      }
    }
    /* 🔎 DIAG Y=Left: SONIC_TESTBIT=0xNNNN injeta esse bit FOX no mask em pulsos
       de 4 frames a cada 60, a partir de SONIC_TESTBIT_AT (default 1500). Determinístico
       (sem uinput). Ex.: TESTBIT=0x0010 (FOX_Y novo) vs 0x0100 (L1, o Y antigo errado). */
    {
      static long testbit = -2; static long testat = 0;
      if (testbit == -2) { const char *e = getenv("SONIC_TESTBIT");
        testbit = e ? strtol(e, NULL, 0) : -1;
        const char *a = getenv("SONIC_TESTBIT_AT"); testat = a ? atol(a) : 1500; }
      if (testbit > 0 && (long)frame >= testat) {
        long ph = ((long)frame - testat) % 60;
        if (ph < 4) {
          if (ph == 0) fprintf(stderr, "=== TESTBIT 0x%04lx pulse @frame %lu ===\n", testbit, frame);
          mask |= (int)testbit;
        }
      }
    }
    if (sonic_game_started && autoright_after >= 0 && (long)frame >= autoright_after) {
      mask |= FOX_RIGHT;
      lx = 32767;
    }
    if (sonic_game_started && autojump_at >= 0 &&
        (long)frame >= autojump_at && (long)frame < autojump_at + 8)
      mask |= FOX_A_GAME;
    if (sonic_game_started && start_down && !start_was_down) {
      fprintf(stderr, "=== START native pause key @frame %lu ===\n", frame);
    }
    start_was_down = start_down;
    if (sonic_game_started && autopause_at >= 0 &&
        (long)frame >= autopause_at && (long)frame < autopause_at + 6) {
      if (autopause_state == 0)
        fprintf(stderr, "=== AUTOPAUSE native pause key @frame %lu ===\n", frame);
      mask |= FOX_PAUSE;
      autopause_state = 1;
    }
    /* (O hack antigo de suprimir Y/X fora do gameplay foi REMOVIDO: a causa real era o
       bit errado do Y; agora FOX_Y=0x10 (mapa real s_remapKey) -> Y dispara sua função
       própria e não aliasa mais pra L1/left.) */
    if (fox.SetPadData) fox.SetPadData(env, thiz, mask, 0, 0, 0, 0, 0);
    if (gm_direct) *gm_direct = (short)mask;
    if (gm_lx) *gm_lx = (short)lx;
    if (gm_ly) *gm_ly = (short)ly;
    if (getenv("SONIC_INPUTLOG") && mask != prev_mask)
      fprintf(stderr, "[input-change f%lu] started=%d mask=%04x prev=%04x\n",
              frame, sonic_game_started, mask & 0xffff, prev_mask & 0xffff);
    prev_mask = mask;
    if (getenv("SONIC_INPUTLOG") && sonic_game_started && (frame % 60) == 0)
      fprintf(stderr, "[input f%lu] mask=%04x lx=%d ly=%d rx=%d ry=%d lt=%d rt=%d\n",
              frame, mask & 0xffff, lx, ly, rx, ry, lt, rt);
    /* 🔎 SONIC_KEYDUMP: dump dos bits de tecla de menu REAIS em runtime (a cada ~2s)
       p/ confirmar/descartar remap do keymap (bug Y=Left). */
    if (getenv("SONIC_KEYDUMP") && (frame - keydump_last) >= 120) {
      keydump_last = frame;
      fprintf(stderr, "[keydump f%lu] L=%04x R=%04x U=%04x D=%04x decide=%04x cancel=%04x  (FOX_Y=0010 FOX_LEFT=0004)\n",
              frame,
              gk_left   ? (*gk_left   & 0xffff) : 0xdead,
              gk_right  ? (*gk_right  & 0xffff) : 0xdead,
              gk_up     ? (*gk_up     & 0xffff) : 0xdead,
              gk_down   ? (*gk_down   & 0xffff) : 0xdead,
              gk_decide ? (*gk_decide & 0xffff) : 0xdead,
              gk_cancel ? (*gk_cancel & 0xffff) : 0xdead);
    }

    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] before save polls\n", frame);
    sonic_native_save_load_poll("frame");
    sonic_save_bootstrap_poll(frame);
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] after save polls\n", frame);
    /* dirige o continue do título (save com fase interrompida) — só fora do gameplay;
       a flag é lida exclusivamente por CStateWaitViewPausing::Next, então forçar
       o valor quando está 0 é seguro e usa o fluxo nativo. */
    if (!sonic_continue_disabled && !sonic_game_started &&
        sonic_isContinueStart && sonic_SetContinueStart &&
        sonic_isContinueStart() == 0) {
      sonic_SetContinueStart(sonic_continue_mode);
      if (sonic_continue_log_n < 3) {
        fprintf(stderr, "=== continue-drive: SetContinueStart(%d) @frame %lu ===\n",
                sonic_continue_mode, frame);
        sonic_continue_log_n++;
      }
    }
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] before GameProcess\n", frame);
    sonic_in_draw_frame = 0;
    if (fox.GameProcess) {
      g_hang_watch_active = 1;
      fox.GameProcess(env, thiz);
      g_hang_watch_active = 0;
    }
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] after GameProcess\n", frame);
    if (sonic_game_started && !gameplay_start_delay_done) {
      const char *d = getenv("SONIC_GAMEPLAY_START_DELAY_MS");
      int ms = d ? atoi(d) : 0;
      gameplay_start_delay_done = 1;
      if (ms > 0) {
        fprintf(stderr, "=== gameplay start delay %d ms @frame %lu ===\n", ms, frame);
        usleep((useconds_t)ms * 1000);
      }
    }
    sonic_in_draw_frame = 1;
    /* 🔧 FIX RASTROS (confirmado por FBO-STATS): o FBO 0 (tela final) é desenhado mas
       NUNCA limpo pelo engine (só FBO 2 é) -> a composição acumula -> smear. Ligamos
       o FBO 0 EXPLICITAMENTE e limpamos antes do DrawFrame (o engine recompõe a cena
       fresca por cima). O clear antigo (sem bind) pegava o FBO errado e não resolvia. */
    if (g_fbclear) {
      extern void glBindFramebuffer(unsigned int, unsigned int);
      extern void glClear(unsigned int);
      glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, 0); /* FBO 0 = tela */
      glClear(0x4100 /* COLOR | DEPTH */);
    }
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] before DrawFrame\n", frame);
    if (fox.DrawFrame) {
      g_hang_watch_active = 1;
      fox.DrawFrame(env, thiz, 0);
      g_hang_watch_active = 0;
    }
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] after DrawFrame\n", frame);
    sonic_in_draw_frame = 0;
    if (getenv("SONIC_TESTCLEAR")) {  /* diagnóstico: present/contexto OK? */
      extern void glClearColor(float, float, float, float);
      extern void glClear(unsigned int);
      glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
      glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    }
    /* PIXDIAG: localizar onde está o conteúdo (FB0 vs FBO interno). Lê o pixel
       central do framebuffer default (0) e de alguns FBOs antes do present. */
    if (getenv("SONIC_PIXDIAG") && (frame % 60) == 0 && frame > 120) {
      extern void glBindFramebuffer(unsigned, unsigned);
      extern void glReadPixels(int,int,int,int,unsigned,unsigned,void*);
      unsigned char px[4]; int fb;
      for (fb = 0; fb <= 3; fb++) {
        glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, (unsigned)fb);
        px[0]=px[1]=px[2]=px[3]=0;
        int cx = sonic_screen_w > 0 ? sonic_screen_w / 2 : 0;
        int cy = sonic_screen_h > 0 ? sonic_screen_h / 2 : 0;
        glReadPixels(cx, cy, 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*UBYTE*/, px);
        fprintf(stderr, "[PIXDIAG f%lu] FB%d center=%02x %02x %02x %02x\n",
                frame, fb, px[0], px[1], px[2], px[3]);
      }
      glBindFramebuffer(0x8D40, 0);
    }
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] before present\n", frame);
    egl_shim_present();
    if (sonic_looplog && frame < 6)
      fprintf(stderr, "[loop f%lu] after present\n", frame);
    /* sinaliza intro-video done nos primeiros segundos */
    if (introCB && frame >= 30 && frame < 120 && (frame % 15) == 0) introCB(env, thiz);
#ifdef __aarch64__
    /* v3: limpa o bit PAUSE_INTRO_VIDEO (0x80) que o playIntroVideo deixou preso
       (equivale ao activeGame(true, 0x80) que o Java faria ao fim do vídeo). */
    if (f2f_app_active && frame >= 60 && frame < 4000 && (frame % 120) == 0)
      f2f_app_active(1, 0x80);
#endif
    /* 🔑 dispara o callback do ad intersticial (sem ad Java) p/ destravar a transição
       Start->world map. callbackInterstitialAds(type=0, result=0) invoca o callback que
       showInterstitial ARMAZENOU. ⚠️ NÃO disparar a cada frame: callBackInterestitial NÃO
       limpa o callback após invocar -> re-disparo contínuo RE-INVOCA o "ir pro world map"
       sem parar -> reinicia a criação do world map (createFile/Tex/Mdl/Act) eternamente ->
       nunca completa -> tela azul. Disparo ÚNICO: o jni_shim seta jni_inter_pending no
       INSTANTE em que o engine chama o método Java showInterstitial(I)V (callback já
       armazenado) -> aqui disparamos callbackInterstitialAds 1x (simula ad fechado). */
    {
      extern volatile int jni_inter_pending;
      (void)prev_a; (void)inter_fire_at; (void)interat;
      if (interCB && jni_inter_pending) {
        jni_inter_pending = 0;
        if (sonic_game_started && !env_flag_enabled("SONIC_ALLOW_GAMEPLAY_INTERCB")) {
          if (inter_gameplay_ignored < 8) {
            fprintf(stderr, "=== interCB IGNORE @frame %lu (gameplay showInterstitial) ===\n", frame);
            inter_gameplay_ignored++;
          }
        } else if ((long)frame - inter_last_fire_frame < 30) {
          fprintf(stderr, "=== interCB IGNORE @frame %lu (debounce) ===\n", frame);
        } else {
          inter_last_fire_frame = (long)frame;
          fprintf(stderr, "=== interCB FIRE @frame %lu (showInterstitial->ad closed) ===\n", frame);
          interCB(env, thiz, 0, 0);
        }
      }
    }
    if ((frame % 60) == 0 && env_flag_enabled("SONIC_FRAMELOG"))
      fprintf(stderr, "[frame %lu]\n", frame);
    if ((frame % 60) == 0 && env_flag_enabled("SONIC_STATELOG"))
      fprintf(stderr, "[state f%lu] setup_done=%d user_enable=%d upshell=%d continue=%d started=%d\n",
              frame,
              gs_setup_done ? gs_setup_done(0) : -1,
              gs_user_enable ? gs_user_enable(0) : -1,
              sjni_upshell ? sjni_upshell() : -1,
              sonic_isContinueStart ? sonic_isContinueStart() : -1,
              sonic_game_started);
    frame++;
    if (frame_sleep_us > 0) usleep((useconds_t)frame_sleep_us);
  }
  return 0;
}
