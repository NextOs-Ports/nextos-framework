/*
 * main.c -- CASTLE OF ILLUSION: Starring Mickey Mouse (Sega "oz" engine,
 * libViewer_GP.so, NativeActivity native aarch64, FMOD Ex audio) so-loader
 * p/ NextOS aarch64 + Mali-450 (Utgard, GLES2 via SDL2).
 *
 * Base = NextOS so-loader scaffold (working aarch64 port lineage: so_util ELF64,
 * android_app 64-bit, canary bionic tpidr+0x28, pthread_bridge). Segredos do
 * port Switch NaGaa95/coi_nx (mesmo jogo, funcional): pacote real
 * "com.sega.ssa.COI", getObbPath, isFireTV=true (modo gamepad), FMOD Ex
 * reusado (nao reimplementado). arm64 = ABI unificada, SEM softfp.
 *
 * Modulos custom-loaded (so_util ELF64):  libfmodex.so -> libViewer_GP.so.
 * COI tem C++ ESTATICO (sem libc++_shared). Resolve via: coi_overrides
 * (imports.c) + revc_pthread_table + snapshot(fmodex) + dlsym(RTLD_DEFAULT).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"
#include "android_shim.h"
#include "egl_shim.h"
#include "jni_shim.h"

#define FMOD_SO  "lib/libfmodex.so"
#define GAME_SO  "lib/libViewer_GP.so"
#define FMOD_HEAP_MB  32
#define GAME_HEAP_MB 256

/* vaddrs no libViewer_GP.so arm64 (readelf) */
#define ANDROID_MAIN_VADDR 0x3da10c
#define EH_FRAME_VADDR     0x306db0

volatile uintptr_t g_load_base = 0;

/* stub for the ETC1-texbake path inherited from the donor scaffold (COI does not use
 * the bake cache -> no name = no cache-hit, normal upload). */
const char *bk_last_bmp_name(void) { return ""; }

extern DynLibFunction coi_overrides[];
extern const int coi_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern DynLibFunction coi_extra[];
extern const int coi_extra_count;

/* 🩹 CANARY BIONIC (donor-scaffold trick): a engine le a stack-guard de
 * tpidr_el0+0x28 (bionic TLS_SLOT_STACK_GUARD). Sob glibc esse endereco cai no
 * TLS de outra lib e muda em runtime -> __stack_chk_fail falso. Pad TLS no
 * inicio do bloco do exe -> tpidr+0x28 fica estavel (zero). */
__attribute__((aligned(16))) static _Thread_local char g_bionic_guard_pad[256];

static DynLibFunction *g_base;
static int g_base_n;
static void build_base_table(void) {
  g_base_n = coi_overrides_count + revc_pthread_count + coi_extra_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  int o = 0;
  memcpy(g_base + o, coi_overrides, sizeof(DynLibFunction) * coi_overrides_count);
  o += coi_overrides_count;
  memcpy(g_base + o, revc_pthread_table, sizeof(DynLibFunction) * revc_pthread_count);
  o += revc_pthread_count;
  memcpy(g_base + o, coi_extra, sizeof(DynLibFunction) * coi_extra_count);
}

/* ---- simbolicacao no crash ---- */
static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0) return;
  char buf[8192]; int n; char line[400]; int li = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e; char perm[8]; char path[256]; path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >= 3 &&
            a >= s && a < e) {
          const char *base = strrchr(path, '/');
          base = base ? base + 1 : (path[0] ? path : "?");
          snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
          close(fd); return;
        }
        li = 0;
      } else line[li++] = c;
    }
  close(fd);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(__NR_gettid));
  char r[300];
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "  PC=%p %s", (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", GAME_SO, (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(m->regs[30], r, sizeof(r));
  fprintf(stderr, "  LR=%p %s", (void *)m->regs[30], r);
  if (g_load_base && m->regs[30] >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", GAME_SO, (unsigned long)(m->regs[30] - g_load_base));
  fprintf(stderr, "\n");
  for (int i = 0; i < 29; i += 3)
    fprintf(stderr, "  x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
            i, (unsigned long)m->regs[i], i+1, (unsigned long)m->regs[i+1],
            i+2, (unsigned long)m->regs[i+2]);
  fprintf(stderr, "  sp=%lx fp=%lx\n", (unsigned long)m->sp, (unsigned long)m->regs[29]);
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < 24 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp; uintptr_t next = p[0], lr = p[1];
    if (!lr) break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_load_base && lr >= g_load_base)
      fprintf(stderr, "  {%s+0x%lx}", GAME_SO, (unsigned long)(lr - g_load_base));
    fprintf(stderr, "\n");
    if (next <= fp) break; fp = next;
  }
  fflush(stderr);
  _exit(128 + sig);
}

/* SIGUSR1: dump da pilha SEM sair (sonda de "onde travou"). */
static void bt_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300];
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "\n[BT sig=%d tid=%d] PC=%p %s", sig,
          (int)syscall(__NR_gettid), (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, " {game+0x%lx}", (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < 20 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp; uintptr_t next = p[0], lr = p[1];
    if (!lr) break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_load_base && lr >= g_load_base)
      fprintf(stderr, " {game+0x%lx}", (unsigned long)(lr - g_load_base));
    fprintf(stderr, "\n");
    if (next <= fp) break; fp = next;
  }
  fflush(stderr);
}

static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
  struct sigaction sb; memset(&sb, 0, sizeof(sb));
  sb.sa_sigaction = bt_handler; sb.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &sb, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so",
      "libOpenSLES.so", "libm.so.6", "libdl.so.2", "libz.so.1", NULL };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

/* carrega um modulo custom + resolve + init_array. Retorna snapshot p/ encadear. */
static DynLibFunction *load_module(const char *name, int heap_mb,
                                   DynLibFunction *tbl, int n, int *out_n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB failed (%s)\n", heap_mb, name); exit(1); }
  fprintf(stderr, "== loading %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load(%s) failed\n", name); exit(1); }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate(%s) failed\n", name); exit(1); }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name, text_base, text_size, data_base, data_size);
  if (out_n) return so_snapshot_symbols(out_n);
  return NULL;
}

static DynLibFunction *tbl_concat(DynLibFunction *a, int an, DynLibFunction *b,
                                  int bn, int *out_n) {
  DynLibFunction *c = malloc(sizeof(DynLibFunction) * (an + bn));
  memcpy(c, a, sizeof(DynLibFunction) * an);
  memcpy(c + an, b, sizeof(DynLibFunction) * bn);
  *out_n = an + bn;
  return c;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  fprintf(stderr, "=== CASTLE OF ILLUSION (Sega oz) so-loader / NextOS aarch64 Mali-450 ===\n");

  { uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t slot = tp + 0x28, lo = (uintptr_t)g_bionic_guard_pad;
    fprintf(stderr, "TLS guard: slot+0x28=0x%lx pad=[0x%lx..0x%lx] %s\n",
            (unsigned long)slot, (unsigned long)lo,
            (unsigned long)(lo + sizeof(g_bionic_guard_pad)),
            (slot >= lo && slot + 8 <= lo + sizeof(g_bionic_guard_pad)) ? "DENTRO" : "FORA(!)"); }

  /* pacote REAL (coi_nx): com.sega.ssa.COI, obb main.154 */
  jni_shim_set_package("com.sega.ssa.COI", 154);

  preload_device_libs();
  build_base_table();

  /* Modulo A: libfmodex.so (FMOD Ex, self-contido; a engine cross-resolve nele) */
  int fmod_n = 0;
  DynLibFunction *fmod = load_module(FMOD_SO, FMOD_HEAP_MB, g_base, g_base_n, &fmod_n);
  fprintf(stderr, "libfmodex: %d simbolos\n", fmod_n);
  int t1n; DynLibFunction *t1;
  if (fmod) t1 = tbl_concat(g_base, g_base_n, fmod, fmod_n, &t1n);
  else { t1 = g_base; t1n = g_base_n; }

  /* Modulo B: libViewer_GP.so (engine) */
  load_module(GAME_SO, GAME_HEAP_MB, t1, t1n, NULL);

  uintptr_t am = so_find_addr("android_main");
  if (!am) { fprintf(stderr, "android_main NAO encontrado\n"); exit(1); }
  g_load_base = am - ANDROID_MAIN_VADDR;
  fprintf(stderr, "android_main @ 0x%lx  load_base=0x%lx\n",
          (unsigned long)am, (unsigned long)g_load_base);

  /* Perf: corta a espiral de sub-passos do Bullet a baixo fps (COI_PHYS_CAP)
   * e/ou mede (dt, maxSubSteps) com COI_PHYS_LOG. Sem env, nao toca em nada. */
  { extern void coi_physics_install(uintptr_t base);
    coi_physics_install(g_load_base); }

  /* registra .eh_frame do jogo no unwinder C++ (modulo custom-loaded) */
  { extern void __register_frame(void *);
    __register_frame((void *)(g_load_base + EH_FRAME_VADDR));
    fprintf(stderr, "__register_frame eh_frame @ 0x%lx\n",
            (unsigned long)(g_load_base + EH_FRAME_VADDR)); }

  struct android_app *app = android_shim_init();
  if (!app) { fprintf(stderr, "android_shim_init failed\n"); exit(1); }

  /* SIGTERM do frontend entra no MESMO caminho do SELECT+START (pause/save/sair).
   * Depois do android_shim_init porque o handler empurra comandos no pipe. */
  { extern void android_shim_install_exit_signals(void);
    android_shim_install_exit_signals(); }

  { uintptr_t gp = so_find_addr_safe("g_pAndroidApp");
    if (gp) { *(void **)gp = app; fprintf(stderr, "g_pAndroidApp @0x%lx set\n", (unsigned long)gp); } }

  /* JNI_OnLoad(vm): a runtime Java o chama ANTES de tudo (coi_nx). A engine
   * guarda o JavaVM num global proprio que so o JNI_OnLoad seta; sem isso o
   * AttachCurrentThread interno do android_main pega vm=lixo -> crash. */
  { uintptr_t ol = so_find_addr_safe("JNI_OnLoad");
    if (ol) {
      int (*jni_onload)(void *, void *) = (int (*)(void *, void *))ol;
      int v = jni_onload(app->activity->vm, NULL);
      fprintf(stderr, "JNI_OnLoad(vm=%p) -> 0x%x\n", app->activity->vm, v);
    } else fprintf(stderr, "WARNING: JNI_OnLoad not found\n"); }

  egl_shim_create_window();

  android_shim_send_cmd(app, APP_CMD_START);
  android_shim_send_cmd(app, APP_CMD_RESUME);
  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);

  fprintf(stderr, "=== calling android_main ===\n");
  void (*android_main_func)(struct android_app *) = (void (*)(struct android_app *))am;
  android_main_func(app);

  fprintf(stderr, "=== android_main retornou ===\n");
  _exit(0);
}
