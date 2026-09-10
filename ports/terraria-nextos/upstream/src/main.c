/*
 * main.c — Terraria Android (Unity 2021.3 IL2CPP) compatibility loader for
 * AArch64 Linux handhelds (SDL2, EGL and GLES2).
 *
 * Receita Unity baseada no port re4 (Unity 2018 Mono), adaptada p/ arm64 + IL2CPP:
 *   - dlopen libz/libGLESv2/libEGL RTLD_GLOBAL (Unity resolve via dlsym RTLD_DEFAULT)
 *   - so_load libunity.so (engine) -> imports overrides -> init_array
 *   - so_load libil2cpp.so (lógica do jogo C#) + global-metadata.dat   [fase seguinte]
 *   - JNI_OnLoad -> janela GLES2 -> lifecycle (initJni/nativeRender)    [fase seguinte]
 * Alvo GLES2: passar -force-gles20 ao Unity (args via initJni/command line).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <SDL2/SDL.h>

#include "so_util.h"
#include "android_compat.h"
#include "imports.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "util.h"
#include <link.h>

#define HEAP_MB 96

/* ---- dl_iterate_phdr custom (INTERPÕE o da libc) ----
 * O unwinder C++ da libgcc acha o .eh_frame de cada lib via dl_iterate_phdr. Nossos
 * módulos (libunity/libil2cpp) são mapeados à mão -> invisíveis ao dl_iterate_phdr da
 * libc -> exceção C++ não acha o landing pad -> std::terminate -> abort (asset loading).
 * Como o EXE é -rdynamic e carrega 1º, este símbolo INTERPÕE o da libc: reportamos os
 * módulos do dynamic linker (via o real, RTLD_NEXT) + os NOSSOS (g_so_mods). */
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
  static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
  if (!real) real = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
  int r = real ? real(cb, data) : 0;
  if (r) return r;
  for (int i = 0; i < g_so_nmods; i++) {
    struct dl_phdr_info info; memset(&info, 0, sizeof info);
    info.dlpi_addr = (ElfW(Addr))g_so_mods[i].base;
    info.dlpi_name = g_so_mods[i].name;
    info.dlpi_phdr = (const ElfW(Phdr) *)g_so_mods[i].ph;
    info.dlpi_phnum = (ElfW(Half))g_so_mods[i].phnum;
    r = cb(&info, sizeof info, data);
    if (r) return r;
  }
  return r;
}

/* canary bionic: libunity lê o stack-guard de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD
 * do bionic); sob glibc esse offset cai no TLS de outra lib e MUDA em runtime →
 * __stack_chk_fail espúrio (e o "SEGV após neutralizar" era o no-op retornando em
 * código adjacente — noreturn). Pad TLS no exe (1º bloco após o TCB de 16B) cobre
 * offset 16..272 e NUNCA é escrito → slot estável. (causa-raiz achada no Dysmantle) */
/* = {1} → .tdata: fica ANTES das TLS .tbss do egl_shim (link order) no template,
 * senão o pad desliza p/ +0x30 e o slot +0x28 cai fora (visto no device). */
__attribute__((aligned(16))) static _Thread_local char g_bionic_guard_pad[256] = {1};

/* fsync(stderr→debug.log): garante que o log sobrevive a hang/power-cycle */
static void dbg_sync(void) { fsync(2); }

/* offsets de diagnostico do mapa (LocalUserGameState) */
size_t g_mapdiag_off[10];

/* ---- build identity ----
 * O NXExtract aceita qualquer payload Terraria Unity/IL2CPP AArch64 realmente
 * compativel; SHA, tamanho, versionCode e texto exato de versao sao apenas
 * procedencia. O manifest .terraria-data.json marca se o payload e o build de
 * referencia ("14564").
 * Patches por RVA sem verificacao de bytes so podem rodar no build de
 * referencia; num build desconhecido eles cairiam em opcode arbitrario. Um
 * manifest formato-1 (sem a chave) so era gravado sob hash exato -> referencia. */
static uintptr_t ter_i2sym(const char *name, unsigned long ref_rva);

static const char *ter_selected_android_package(void) {
  static const char fallback[] = "com.and.games505.TerrariaPaid";
  const char *dir = getenv("TER_GAMEDIR");
  char path[512];
  char blob[8192];
  const char *selected = NULL;
  FILE *f;
  size_t got;

  snprintf(path, sizeof path, "%s/.nxextract-terraria.json", dir ? dir : ".");
  f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr,
            "[PACKAGE] marcador NXExtract ausente; usando identidade historica %s\n",
            fallback);
    return fallback;
  }
  got = fread(blob, 1, sizeof blob - 1, f);
  fclose(f);
  blob[got] = 0;
  selected = ter_android_package_from_marker(blob);
  if (!selected) {
    fprintf(stderr,
            "[PACKAGE] marcador sem package_id Terraria valido; usando identidade "
            "historica %s\n",
            fallback);
    return fallback;
  }
  fprintf(stderr, "[PACKAGE] identidade Android selecionada: %s\n", selected);
  return selected;
}

static int ter_known_build(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  cached = 0;
  const char *dir = getenv("TER_GAMEDIR");
  char path[512];
  snprintf(path, sizeof path, "%s/.terraria-data.json", dir ? dir : ".");
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "[BUILD] manifest ausente (%s); tratando como build desconhecido\n", path);
    return cached;
  }
  static char blob[65536];
  size_t got = fread(blob, 1, sizeof blob - 1, f);
  fclose(f);
  blob[got] = 0;
  if (strstr(blob, "\"known_build\": false")) {
    const char *id = strstr(blob, "\"build_id\": \"");
    int idlen = 0;
    if (id) { id += 13; const char *q = strchr(id, '"'); idlen = q ? (int)(q - id) : 32; if (idlen > 32) idlen = 32; }
    fprintf(stderr, "[BUILD] build desconhecido%s%.*s -- hooks por RVA fixo desativados; "
            "apenas patches com assinatura verificada\n",
            id ? ": " : "", idlen, id ? id : "");
  } else {
    cached = 1;
    fprintf(stderr, "[BUILD] build de referencia (14564): patches completos habilitados\n");
  }
  return cached;
}

/* Hotkey, SDL_QUIT and Unix termination signals all converge here. The render
 * loop owns the Android lifecycle shutdown; signal handlers only set a flag. */
static volatile sig_atomic_t g_ter_quit_requested;
/* sync() e async-signal-safe: mesmo num teardown por timeout, os .wld/.map/
 * .plr que o jogo acabou de escrever saem do page cache antes do _exit —
 * um .map truncado e exatamente o save que "so abre com o minimapa off". */
static void ter_shutdown_timeout(int sig) { (void)sig; sync(); _exit(124); }
void ter_request_quit(void) {
  if (g_ter_quit_requested) return;
  g_ter_quit_requested = 1;
  /* nativeRender normally returns immediately and reaches focus-loss/pause.
   * If a vendor driver or managed callback wedges during that final frame,
   * SIGALRM still guarantees the frontend gets the process back. */
  alarm(3);
}
static void ter_term_handler(int sig) {
  (void)sig;
  g_ter_quit_requested = 1;
  alarm(3);
}
static void ter_install_term_handlers(void) {
  struct sigaction st;
  memset(&st, 0, sizeof(st));
  st.sa_handler = ter_term_handler;
  sigemptyset(&st.sa_mask);
  sigaction(SIGINT, &st, NULL);
  sigaction(SIGTERM, &st, NULL);
  st.sa_handler = ter_shutdown_timeout;
  sigaction(SIGALRM, &st, NULL);
}

/* Legacy diagnostics still use these helpers directly. Guest sem/pthread
 * imports use the audited Prizefighters 2 bridge below. */
extern int g_main_tid;
extern void sh_tick_preload(void);
extern void sh_sem_set_poll(int ms);
static void set_import(const char *name, void *fn);
static int patch_got(const char *name, void *fn);

/* Android arm64 sigset_t is 8 bytes; glibc sigset_t is 128. A direct host
 * sigemptyset call was confirmed to zero nativeRecreateGfxState's saved return
 * address on ArkOS. This bridge is the audited path used by the working
 * Horizon Chase multi-firmware loader. */
extern int ter_bionic_sigemptyset(unsigned long *);
extern int ter_bionic_sigfillset(unsigned long *);
extern int ter_bionic_sigaddset(unsigned long *, int);
extern int ter_bionic_sigdelset(unsigned long *, int);
extern int ter_bionic_sigsuspend(const unsigned long *);
#define BIONIC_SIGSET_LIST(X) \
  X("sigemptyset", ter_bionic_sigemptyset) \
  X("sigfillset", ter_bionic_sigfillset) \
  X("sigaddset", ter_bionic_sigaddset) \
  X("sigdelset", ter_bionic_sigdelset) \
  X("sigsuspend", ter_bionic_sigsuspend)
static void install_bionic_sigset_shim(void) {
#define BIONIC_SIGSET_SET(name, function) set_import(name, (void *)function);
  BIONIC_SIGSET_LIST(BIONIC_SIGSET_SET)
#undef BIONIC_SIGSET_SET
}
static void patch_bionic_sigset_shim(void) {
#define BIONIC_SIGSET_PATCH(name, function) patch_got(name, (void *)function);
  BIONIC_SIGSET_LIST(BIONIC_SIGSET_PATCH)
#undef BIONIC_SIGSET_PATCH
}

/* Complete bionic object/attribute bridge adapted from the working
 * Prizefighters 2 loader. In particular pthread_create translates the Android
 * pthread_attr_t instead of handing its incompatible storage to glibc. */
struct terraria_pthread_import {
  const char *name;
  void *function;
};
extern const struct terraria_pthread_import *
terraria_pthread_bridge_imports(size_t *count);
extern int pthread_sigmask_fake(int, const void *, void *);

static void install_pthread_shim(void) {
  if (getenv("TER_NOPTSHIM"))
    return;
  size_t count = 0;
  const struct terraria_pthread_import *imports =
      terraria_pthread_bridge_imports(&count);
  for (size_t index = 0; index < count; index++)
    set_import(imports[index].name, imports[index].function);
  set_import("pthread_sigmask", (void *)pthread_sigmask_fake);
}

static void patch_pthread_shim(void) {
  if (getenv("TER_NOPTSHIM"))
    return;
  size_t count = 0;
  const struct terraria_pthread_import *imports =
      terraria_pthread_bridge_imports(&count);
  for (size_t index = 0; index < count; index++)
    patch_got(imports[index].name, imports[index].function);
  patch_got("pthread_sigmask", (void *)pthread_sigmask_fake);
}

/* ---------- crash handler (arm64) ---------- */
static uintptr_t g_unity_base, g_il2cpp_base, g_unity_data;
static uintptr_t g_i2heap_base, g_i2heap_size;

/* Terraria 1.4.5.6.4's Microsoft.Xna.Framework.Game.Exit is a four-byte
 * IL2CPP stub containing only `ret` (RVA 0xE0EEA0).  QuitGame deliberately
 * calls it after SaveSettings and SocialAPI.Shutdown, but Android normally
 * relies on its Java Activity to turn that no-op into process teardown.
 *
 * A regular hook_arm64 would overwrite 16 bytes and corrupt the next IL2CPP
 * method, which starts at RVA +4.  Instead, replace only the stub's `ret` with
 * an AArch64 B to a tiny branch island in the unused tail of the loader's
 * reserved libil2cpp mapping.  B/BR preserve x30, so the C hook returns to the
 * original QuitGame caller after requesting our guarded teardown. */
#define TER_GAME_EXIT_RVA 0xE0EEA0UL
#define TER_QUIT_GAME_TAIL_RVA 0xFBC664UL
#define TER_AARCH64_RET 0xD65F03C0u
#define TER_GAME_WINDOW_FIRST 0xF81E0FF4u

static void ter_game_exit_hook(void *game, void *method) {
  (void)game;
  (void)method;
  /* Arm the watchdog before touching the log: slow or wedged removable
   * storage must not be able to turn Quit Game back into a hang. */
  ter_request_quit();
  fprintf(stderr, "[QUIT] Game.Exit solicitou encerramento\n");
}

static int ter_install_game_exit_hook(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || (page_size & (page_size - 1)) != 0)
    page_size = 4096;

  uintptr_t target = g_il2cpp_base + TER_GAME_EXIT_RVA;
  uintptr_t quit_tail = g_il2cpp_base + TER_QUIT_GAME_TAIL_RVA;
  uintptr_t island = (g_i2heap_base + g_i2heap_size - (uintptr_t)page_size) &
                     ~((uintptr_t)page_size - 1);
  uintptr_t loaded_end = (uintptr_t)data_base + data_size;
  uintptr_t text_end = (uintptr_t)text_base + text_size;
  if (text_end > loaded_end)
    loaded_end = text_end;
  loaded_end = (loaded_end + (uintptr_t)page_size - 1) &
               ~((uintptr_t)page_size - 1);

  if (!g_il2cpp_base || g_i2heap_base == (uintptr_t)MAP_FAILED ||
      g_il2cpp_base != g_i2heap_base ||
      g_i2heap_size != 96UL * 1024 * 1024) {
    fprintf(stderr, "[QUIT] layout do libil2cpp inesperado\n");
    return 0;
  }
  uint32_t exit_opcode = *(const uint32_t *)target;
  uint32_t next_opcode = *(const uint32_t *)(target + 4);
  if (exit_opcode != TER_AARCH64_RET ||
      next_opcode != TER_GAME_WINDOW_FIRST) {
    fprintf(stderr,
            "[QUIT] Game.Exit inesperado em libil2cpp+0x%lx "
            "(opcode=%08x proximo=%08x)\n",
            (unsigned long)TER_GAME_EXIT_RVA, exit_opcode, next_opcode);
    return 0;
  }
  static const uint32_t expected_quit_tail[] = {
      0xF9400268u, /* ldr x8, [x19] */
      0xAA1303E0u, /* mov x0, x19 */
      0xA9417BF3u, /* restore x19/x30 */
      0xA95D8502u, /* load virtual Exit target and MethodInfo */
      0xA8C253F5u, /* restore x21/x20 and stack */
      0xD61F0040u, /* br x2 -- tail-dispatch Game.Exit */
  };
  if (memcmp((const void *)quit_tail, expected_quit_tail,
             sizeof(expected_quit_tail)) != 0) {
    fprintf(stderr,
            "[QUIT] assinatura de Terraria.Main.QuitGame inesperada\n");
    return 0;
  }
  if (island < loaded_end + (uintptr_t)page_size) {
    fprintf(stderr,
            "[QUIT] sem pagina livre para a ponte (fim=%p ilha=%p)\n",
            (void *)loaded_end, (void *)island);
    return 0;
  }

  int64_t delta = (int64_t)island - (int64_t)target;
  if ((delta & 3) != 0 || delta < -0x08000000LL ||
      delta > 0x07FFFFFCLL) {
    fprintf(stderr, "[QUIT] ponte fora do alcance AArch64 (delta=0x%llx)\n",
            (unsigned long long)delta);
    return 0;
  }

  /* The tail belongs to the anonymous reservation and is untouched by the
   * ELF.  Refuse to reuse it if that invariant ever changes. */
  const uint64_t *empty = (const uint64_t *)island;
  for (size_t index = 0;
       index < (size_t)page_size / sizeof(*empty); index++) {
    if (empty[index] != 0) {
      fprintf(stderr, "[QUIT] pagina da ponte ja esta ocupada\n");
      return 0;
    }
  }
  if (mprotect((void *)island, (size_t)page_size,
               PROT_READ | PROT_WRITE) != 0) {
    fprintf(stderr, "[QUIT] mprotect RW da ponte falhou: %s\n",
            strerror(errno));
    return 0;
  }

  uint32_t *code = (uint32_t *)island;
  code[0] = 0x58000051u; /* ldr x17, [pc, #8] */
  code[1] = 0xD61F0220u; /* br x17 */
  *(uint64_t *)(code + 2) = (uint64_t)(uintptr_t)ter_game_exit_hook;
  __builtin___clear_cache((char *)island,
                          (char *)island + (size_t)page_size);
  if (mprotect((void *)island, (size_t)page_size,
               PROT_READ | PROT_EXEC) != 0) {
    fprintf(stderr, "[QUIT] mprotect da ponte falhou: %s\n", strerror(errno));
    return 0;
  }

  uintptr_t target_page = target & ~((uintptr_t)page_size - 1);
  if (mprotect((void *)target_page, (size_t)page_size,
               PROT_READ | PROT_WRITE) != 0) {
    fprintf(stderr, "[QUIT] mprotect RW do alvo falhou: %s\n",
            strerror(errno));
    return 0;
  }
  int64_t words = delta / 4;
  *(uint32_t *)target =
      0x14000000u | ((uint32_t)words & 0x03FFFFFFu); /* b island */
  __builtin___clear_cache((char *)target, (char *)target + sizeof(uint32_t));
  if (mprotect((void *)target_page, (size_t)page_size,
               PROT_READ | PROT_EXEC) != 0) {
    fprintf(stderr, "[QUIT] mprotect RX do alvo falhou: %s\n",
            strerror(errno));
    return 0;
  }
  fprintf(stderr,
          "[QUIT] Game.Exit conectado ao teardown (alvo=%p ponte=%p)\n",
          (void *)target, (void *)island);
  return 1;
}
/* exposto p/ pthread_fake.c (TER_JOBLOG: symbolizar start_routine dos workers) */
uintptr_t ter_unity_base(void) { return g_unity_base; }
uintptr_t ter_il2cpp_base(void) { return g_il2cpp_base; }

/* TER_INLINETASK: FINGE a conclusão do per-object future-task NA MAIN. A main constrói o future
   (0x2f3680), submete o functor a um pool, e espera em 0x2f37a4 que um worker rode o functor e
   chame a conclusão 0x2f3a98 (que seta o nó obj+88 + incrementa o contador GLOBAL c10360 que o
   WaitForJobGroup da frame 3 espera). O dispatch p/ os workers está quebrado no so-loader (eles
   ficam ociosos). Aqui, no topo do loop de espera, a própria main faz o bookkeeping da conclusão:
   seta node->next!=0 (sai da espera) + incrementa c10360 (destrava a frame 3). O TRABALHO de
   serialização em si é pulado (já era tolerado como warning "missing script"). Chamado pelo
   trampolim instalado em TER_INLINETASK. */
static volatile int g_inlinetask_n = 0;
void ter_inline_task(void *obj) {
  if (!obj) return;
  void *node = *(void **)((char *)obj + 88);    /* obj+0x58 = node */
  if (node) *(void **)node = (void *)1;          /* node->next = 1 → satisfaz `cbnz` em 0x2f37b0 */
  if (g_unity_base) {
    uint32_t *cnt = (uint32_t *)(g_unity_base + 0xc10360);
    __atomic_add_fetch(cnt, 1, __ATOMIC_SEQ_CST);
  }
  int n = __atomic_add_fetch(&g_inlinetask_n, 1, __ATOMIC_RELAXED);
  if (n <= 5 || (n % 50) == 0) { fprintf(stderr, "[INLINETASK] #%d obj=%p node=%p c10360++\n", n, obj, node); fsync(2); }
}

/* TER_NUKEKB: patcha métodos il2cpp que lançam exceção TODA FRAME e ABORTAM o ExecuteFrame
   ANTES do Draw (KeyboardInput.Update lê o campo Java 'PressedStates' via reflection que falha
   no nosso JNI fake). Usa a API il2cpp REAL (exportada) p/ achar a classe+método e patchar o
   methodPointer p/ `ret` (no-op). Roda lazy do swap-hook até achar (il2cpp já inicializado). */
static void ter_nuke_methods(void) {
  static int done = 0;
  int want_kb = getenv("TER_NUKEKB") ? 1 : 0;
  int want_nanpart = getenv("TER_FIXNANPART") ? 1 : 0;
  if (done || !g_il2cpp_base || (!want_kb && !want_nanpart)) { if (!want_kb && !want_nanpart) done = 1; return; }
  static int tries = 0; if (tries++ > 600) { done = 1; return; }
  void *(*dom_get)(void) = (void *)(ter_i2sym("il2cpp_domain_get", 0x73c860));
  const void **(*dom_asms)(void *, size_t *) = (void *)(ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c));
  void *(*asm_img)(const void *) = (void *)(ter_i2sym("il2cpp_assembly_get_image", 0x73c22c));
  void *(*cls_from_name)(void *, const char *, const char *) = (void *)(ter_i2sym("il2cpp_class_from_name", 0x73c264));
  void *(*cls_method)(void *, const char *, int) = (void *)(ter_i2sym("il2cpp_class_get_method_from_name", 0x73c28c));
  void *domain = dom_get(); if (!domain) return;
  size_t na = 0; const void **asms = dom_asms(domain, &na); if (!asms || !na) return;
  struct nuke_target { const char *env, *ns, *cn, *mn; int argc; };
  static const struct nuke_target targets[] = {
    { "TER_NUKEKB", "", "KeyboardInput", "Update", 0 },
    { "TER_FIXNANPART", "Terraria.Graphics.Renderers", "LittleFlyingCritterParticle", "Update", 1 },
  };
  static unsigned patched_mask;
  int patched = 0;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(asms[i]); if (!img) continue;
    for (unsigned t = 0; t < sizeof targets/sizeof targets[0]; t++) {
      if (!getenv(targets[t].env) || (patched_mask & (1u << t))) continue;
      void *cls = cls_from_name(img, targets[t].ns, targets[t].cn); if (!cls) continue;
      void *m = cls_method(cls, targets[t].mn, targets[t].argc); if (!m) continue;
      void *mp = *(void **)m;   /* MethodInfo.methodPointer @ off 0 */
      if (!mp) continue;
      long pgsz = sysconf(_SC_PAGESIZE);
      void *pa = (void *)((uintptr_t)mp & ~((uintptr_t)pgsz - 1));
      mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
      *(uint32_t *)mp = 0xD65F03C0u;   /* ret */
      mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
      __builtin___clear_cache((char *)pa, (char *)pa + pgsz * 2);
      patched_mask |= (1u << t);
      fprintf(stderr, "[NUKE] %s%s%s.%s @%p -> ret (asm %zu)\n",
              targets[t].ns, targets[t].ns[0] ? "." : "", targets[t].cn, targets[t].mn, mp, i);
      fsync(2);
      patched++;
    }
  }
  int all = 1;
  for (unsigned t = 0; t < sizeof targets/sizeof targets[0]; t++)
    if (getenv(targets[t].env) && !(patched_mask & (1u << t))) all = 0;
  if (all) done = 1;
  (void)patched;
}

/* 🖤 TER_FIXSP: tela preta ao clicar Single Player. SelectSinglePlayer→Main.LoadPlayers→
 * OldSaveSynchronise.CopyOldSaves→get_OldSaveRoot lança NullReferenceException (migração de
 * saves antigos do Android: path/JNI nulo no so-loader) → a tela de seleção de player não
 * carrega → preto. Neutralizamos CopyOldSaves (-> ret): não há saves antigos pra migrar. */
static void ter_fix_singleplayer(void) {
  static int done = 0; if (done || !g_il2cpp_base || !getenv("TER_FIXSP")) { if (!getenv("TER_FIXSP")) done = 1; return; }
  static int tries = 0; if (tries++ > 400) { done = 1; return; }
  long pgsz0 = sysconf(_SC_PAGESIZE);
  /* 🖤 GUILowDiskSpacePopup.CheckDiskSpace mostra "low on storage" se DiskSpace()<=~50MB.
     DiskSpace() (il2cpp+0xd158ac) usa statfs nativo que retorna pouco no so-loader (espaço real
     = 93GB). Patchamos DiskSpace p/ retornar 1GB (movz x0,#0; movk x0,#0x4000,lsl16; ret). */
  { static int dsdone=0; if(!dsdone){ uint32_t*c=(uint32_t*)(g_il2cpp_base+0xd158ac);
      void*pa=(void*)((uintptr_t)c & ~((uintptr_t)pgsz0-1));
      mprotect(pa,pgsz0*2,PROT_READ|PROT_WRITE|PROT_EXEC);
      c[0]=0xD2800000u; c[1]=0xF2A80000u; c[2]=0xD65F03C0u;   /* return 0x40000000 (1GB) */
      mprotect(pa,pgsz0*2,PROT_READ|PROT_EXEC); __builtin___clear_cache((char*)pa,(char*)pa+12);
      fprintf(stderr,"[FIXSP] GUILowDiskSpacePopup.DiskSpace -> 1GB\n"); fsync(2); dsdone=1; } }
  void *(*dom_get)(void) = (void *)(ter_i2sym("il2cpp_domain_get", 0x73c860));
  const void **(*dom_asms)(void *, size_t *) = (void *)(ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c));
  void *(*asm_img)(const void *) = (void *)(ter_i2sym("il2cpp_assembly_get_image", 0x73c22c));
  void *(*cls_from_name)(void *, const char *, const char *) = (void *)(ter_i2sym("il2cpp_class_from_name", 0x73c264));
  void *(*cls_method)(void *, const char *, int) = (void *)(ter_i2sym("il2cpp_class_get_method_from_name", 0x73c28c));
  void *domain = dom_get(); if (!domain) return;
  size_t na = 0; const void **asms = dom_asms(domain, &na); if (!asms || !na) return;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(asms[i]); if (!img) continue;
    void *cls = cls_from_name(img, "Terraria.IO", "OldSaveSynchronise"); if (!cls) continue;
    void *m = cls_method(cls, "CopyOldSaves", 0); if (!m) { continue; }
    void *mp = *(void **)m; if (!mp) { done = 1; return; }
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)((uintptr_t)mp & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)mp = 0xD65F03C0u;   /* ret */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + 8);
    fprintf(stderr, "[FIXSP] OldSaveSynchronise.CopyOldSaves @%p -> ret (asm %zu)\n", mp, i); fsync(2);
    done = 1; return;
  }
}

/* TER_JOBWORKERS0: chama JobsUtility.JobWorkerCount=0 (e ActiveThreadCount=0) via il2cpp_runtime_invoke
   → Unity roda os jobs INLINE na própria thread (dispatch pros worker threads está quebrado no
   so-loader). Fix CORRETO (vs fingir com INLINETASK/SKIPJOBWAIT). Lazy do swap-hook até conseguir. */
static void ter_jobworkers0(void) {
  static int done = 0; if (done || !g_il2cpp_base || !getenv("TER_JOBWORKERS0")) { if (!getenv("TER_JOBWORKERS0")) done = 1; return; }
  static int tries = 0; if (tries++ > 240) { done = 1; return; }
  void *(*dom_get)(void) = (void *)(ter_i2sym("il2cpp_domain_get", 0x73c860));
  const void **(*dom_asms)(void *, size_t *) = (void *)(ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c));
  void *(*asm_img)(const void *) = (void *)(ter_i2sym("il2cpp_assembly_get_image", 0x73c22c));
  void *(*cls_from_name)(void *, const char *, const char *) = (void *)(ter_i2sym("il2cpp_class_from_name", 0x73c264));
  void *(*cls_method)(void *, const char *, int) = (void *)(ter_i2sym("il2cpp_class_get_method_from_name", 0x73c28c));
  void *(*rt_invoke)(void *, void *, void **, void **) = (void *)(ter_i2sym("il2cpp_runtime_invoke", 0x73cc7c));
  void *domain = dom_get(); if (!domain) return;
  size_t na = 0; const void **asms = dom_asms(domain, &na); if (!asms || !na) return;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(asms[i]); if (!img) continue;
    void *cls = cls_from_name(img, "Unity.Jobs.LowLevel.Unsafe", "JobsUtility"); if (!cls) continue;
    static int enum_once = 0;
    if (getenv("TER_JOBENUM") && !enum_once && ++enum_once) { void (*cls_init)(void *) = (void *)(ter_i2sym("il2cpp_runtime_class_init", 0x73cc80)); cls_init(cls);
      fprintf(stderr, "[JOBWORKERS0] JobsUtility achada (asm %zu) — métodos:\n", i);
      void *(*cls_methods)(void *, void **) = (void *)(ter_i2sym("il2cpp_class_get_methods", 0x73c288));
      const char *(*meth_name)(void *) = (void *)(ter_i2sym("il2cpp_method_get_name", 0x73cb9c));
      unsigned (*meth_pc)(void *) = (void *)(ter_i2sym("il2cpp_method_get_param_count", 0x73cbac));
      void *it = NULL, *mm; int cnt = 0;
      while ((mm = cls_methods(cls, &it)) && cnt++ < 60) fprintf(stderr, "   %s/%u\n", meth_name(mm), meth_pc(mm));
      fsync(2);
    }
    int zero = 0; void *params[1] = { &zero }; void *exc = NULL;
    const char *setters[] = { "set_JobWorkerCount", "SetJobQueueMaximumActiveThreadCount", "SetJobQueueMaximumWarpThreadCount" };
    int any = 0;
    for (unsigned s = 0; s < sizeof setters/sizeof setters[0]; s++) {
      void *m = cls_method(cls, setters[s], 1); if (!m) continue;
      exc = NULL; rt_invoke(m, NULL, params, &exc);
      fprintf(stderr, "[JOBWORKERS0] %s(0) invoked exc=%p\n", setters[s], exc); fsync(2); any = 1;
    }
    if (any) { done = 1; return; }
  }
  if (tries > 30) done = 1;   /* desiste do retry (evita spam) se não achou os setters */
}
extern size_t text_size;
/* /proc/self/maps lido UMA vez (sem malloc — open/read/parse manual; fopen não é
 * async-signal-safe e re-faulta no handler). Buffer estático grande o bastante. */
static char g_maps_buf[64 * 1024];
static int g_maps_len;
static void maps_snapshot(void) {
  int fd = open("/proc/self/maps", O_RDONLY);
  g_maps_len = 0;
  if (fd < 0) return;
  int n; char *p = g_maps_buf;
  while (g_maps_len < (int)sizeof(g_maps_buf) - 1 &&
         (n = read(fd, p + g_maps_len, sizeof(g_maps_buf) - 1 - g_maps_len)) > 0)
    g_maps_len += n;
  g_maps_buf[g_maps_len] = 0;
  close(fd);
}
/* acha a linha de maps que contém 'a'; preenche lo/hi/perm; retorna ptr p/ a linha
 * (NUL-terminada temporariamente) ou NULL. Parse manual sobre o snapshot. */
static const char *maps_find(uintptr_t a, uintptr_t *lo_o, uintptr_t *hi_o, char perm_o[5]) {
  const char *s = g_maps_buf;
  while (s < g_maps_buf + g_maps_len) {
    const char *eol = s; while (*eol && *eol != '\n') eol++;
    uintptr_t lo = 0, hi = 0; const char *q = s;
    while (*q && *q != '-') { lo = lo * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); q++; }
    if (*q == '-') q++;
    while (*q && *q != ' ') { hi = hi * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); q++; }
    if (a >= lo && a < hi) {
      if (lo_o) *lo_o = lo; if (hi_o) *hi_o = hi;
      if (perm_o) { const char *pp = q + 1; for (int i = 0; i < 4; i++) perm_o[i] = pp[i]; perm_o[4] = 0; }
      static char line[256]; int len = (int)(eol - s); if (len > 255) len = 255;
      for (int i = 0; i < len; i++) line[i] = s[i]; line[len] = 0;
      return line;
    }
    s = (*eol == '\n') ? eol + 1 : eol;
  }
  return NULL;
}
static int addr_readable(uintptr_t a) {
  char perm[5]; uintptr_t lo, hi;
  return maps_find(a, &lo, &hi, perm) && perm[0] == 'r';
}
/* imprime a linha de maps que contém 'a' + classifica vs nossas bases */
static void crash_classify(const char *tag, uintptr_t a) {
  fprintf(stderr, "[CR] %s=0x%lx", tag, (unsigned long)a);
  if (g_unity_base && a >= g_unity_base && a < g_unity_base + text_size)
    fprintf(stderr, " (libunity+0x%lx)", a - g_unity_base);
  else if (g_il2cpp_base && a >= g_il2cpp_base && a < g_il2cpp_base + 0x3000000)
    fprintf(stderr, " (libil2cpp+0x%lx)", a - g_il2cpp_base);
  else if (g_i2heap_base && a >= g_i2heap_base && a < g_i2heap_base + g_i2heap_size)
    fprintf(stderr, " (i2heap+0x%lx)", a - g_i2heap_base);
  char perm[5]; uintptr_t lo, hi;
  const char *line = maps_find(a, &lo, &hi, perm);
  if (line) fprintf(stderr, "  | %s", line);
  fprintf(stderr, "\n"); dbg_sync();
}
static void crash_dump_qwords(const char *tag, uintptr_t base, int n) {
  if (!addr_readable(base)) { fprintf(stderr, "[CR] %s @0x%lx ILEGÍVEL\n", tag, (unsigned long)base); dbg_sync(); return; }
  for (int k = 0; k < n; k += 2)
    fprintf(stderr, "[CR] %s +%02x: %016lx %016lx\n", tag, k * 8,
            (unsigned long)((uintptr_t *)base)[k], (unsigned long)((uintptr_t *)base)[k + 1]);
  dbg_sync();
}

static volatile int g_crashing = 0;
#define ARENA_LO 0x7f10000000UL
#define ARENA_HI 0x7f10200000UL
static volatile unsigned long g_skipbad_n = 0;
static int g_skipbad = 0;  /* lido 1× no startup (getenv não é async-signal-safe) */
/* recovery por-frame: sigsetjmp antes de nativeRender; on_crash siglongjmp de volta
   (só se o crash for na THREAD de render — longjmp cross-thread é UB). Pula o frame
   corrompido e continua → renderiza apesar das chamadas de método C# corrompidas. */
#include <setjmp.h>
/* GC stop-the-world: SIGPWR suspende a thread (espera o restart SIGXCPU); SIGXCPU
   é no-op (sua chegada acorda o sigsuspend). Mantém nossas threads vivas durante a
   coleta (sem isso, SIGPWR default mata o processo -> exit 158). */
void gc_suspend_handler(int sig);
void gc_suspend_handler(int sig) {
  (void)sig;
  /* CUP_GCSUSP=wait: protocolo real (suspende até SIGXCPU). Default: RETORNA imediato
     (não suspende) — o stop-the-world do GC está quebrado (handler corrompido) e nunca
     manda o restart, então suspender CONGELA a render. Retornar deixa a thread seguir
     (coleta vira racy, mas a render avança → imagem). */
  if (getenv("CUP_GCSUSP")) {
    sigset_t m; sigfillset(&m);
    sigdelset(&m, SIGXCPU); sigdelset(&m, SIGSEGV); sigdelset(&m, SIGBUS);
    sigsuspend(&m);
  }
}
void gc_restart_handler(int sig);
void gc_restart_handler(int sig) { (void)sig; }
static sigjmp_buf g_render_jmp;
static volatile int g_render_jmp_armed = 0;
static int g_render_tid = 0;
static volatile unsigned long g_recover_n = 0;
static void on_crash(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc0 = (ucontext_t *)uc_;
  uintptr_t pc0 = uc0->uc_mcontext.pc, lr0 = uc0->uc_mcontext.regs[30];
  /* 🔎 dump async-signal-safe (write(2) cru, imune a stdio/FILE-lock): garante
     PC/fault/lr mesmo quando o dump rico via fprintf se perde. offsets p/ libunity
     (g_unity_base) e libil2cpp (g_il2cpp_base) p/ casar com objdump no host. */
  {
    char b[256]; int n = 0;
    static const char hx[] = "0123456789abcdef";
    #define _EMIT_S(s) do { const char *p=(s); while(*p&&n<240) b[n++]=*p++; } while(0)
    #define _EMIT_H(v) do { unsigned long _v=(unsigned long)(v); b[n++]='0'; b[n++]='x'; \
        for(int _i=60;_i>=0;_i-=4) b[n++]=hx[(_v>>_i)&0xf]; } while(0)
    _EMIT_S("\n[CR!] sig="); b[n++]=hx[sig&0xf];
    _EMIT_S(" fault="); _EMIT_H((unsigned long)si->si_addr);
    _EMIT_S(" pc="); _EMIT_H(pc0);
    if (g_unity_base && pc0>=g_unity_base && pc0<g_unity_base+0x2000000){ _EMIT_S(" unity+"); _EMIT_H(pc0-g_unity_base); }
    if (g_il2cpp_base && pc0>=g_il2cpp_base && pc0<g_il2cpp_base+0x4000000){ _EMIT_S(" il2cpp+"); _EMIT_H(pc0-g_il2cpp_base); }
    _EMIT_S(" lr="); _EMIT_H(lr0);
    if (g_unity_base && lr0>=g_unity_base && lr0<g_unity_base+0x2000000){ _EMIT_S(" unity+"); _EMIT_H(lr0-g_unity_base); }
    _EMIT_S("\n"); if(n<256){ ssize_t _w=write(2,b,n); (void)_w; }
    #undef _EMIT_S
    #undef _EMIT_H
  }
  /* recovery: crash na thread de render (qualquer fault, não só arena) → volta pro
     loop e pula o frame. Só se armado e na thread certa. */
  if (g_render_jmp_armed && (int)syscall(SYS_gettid) == g_render_tid) {
    g_recover_n++;
    siglongjmp(g_render_jmp, 1);
  }
  /* skipbad: crash em thread NÃO-render (worker/job) → estaciona a thread em vez de
     matar o processo (mantém o jogo vivo p/ a render continuar). */
  if (g_skipbad && sig == SIGSEGV) {
    static volatile unsigned long parked = 0;
    if (parked++ < 40)
      fprintf(stderr, "[PARK] worker tid=%d crashou (pc=0x%lx) — estacionado\n",
              (int)syscall(SYS_gettid), (unsigned long)pc0);
    dbg_sync();
    for (;;) pause();
  }
  /* CUP_SKIPBAD: o ponteiro de método genérico corrompido (→ arena 2MB) é chamado
     em vários sites. Se o pc cai na arena (chamou o lixo), PULA a chamada: retoma
     no lr com retorno null (x0=0). Se as chamadas não forem críticas, o jogo passa
     e renderiza. Hack p/ destravar a imagem (não é fix definitivo). */
  if (g_skipbad && sig == SIGSEGV && pc0 >= ARENA_LO && pc0 < ARENA_HI) {
    if (lr0 && lr0 != pc0) {
      uc0->uc_mcontext.pc = lr0;        /* retoma no retorno */
      uc0->uc_mcontext.regs[0] = 0;     /* valor de retorno = null/0 */
      if (g_skipbad_n++ < 60)
        fprintf(stderr, "[SKIPBAD] #%lu pc=arena -> pula p/ lr=0x%lx\n",
                g_skipbad_n, (unsigned long)lr0);
      if ((g_skipbad_n & 0x3ff) == 0) dbg_sync();
      return;  /* resume */
    }
  }
  /* reentrância: se outra thread já está dumpando (vtable corrompido faz várias
     threads crasharem juntas), esta espera p/ não interleavar/re-faultar o dump. */
  if (__sync_lock_test_and_set(&g_crashing, 1)) {
    fprintf(stderr, "[CR] (2ª thread crashou sig=%d tid=%d — aguardando)\n",
            sig, (int)syscall(SYS_gettid));
    dbg_sync();
    for (;;) pause();
  }
  ucontext_t *uc = (ucontext_t *)uc_;
  uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
  uintptr_t tb = (uintptr_t)text_base;
  maps_snapshot();   /* sem malloc — antes de qualquer parse */
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p pc=0x%lx", sig, si->si_addr,
          (unsigned long)pc);
  if (pc >= tb && pc < tb + text_size) fprintf(stderr, " (libunity+0x%lx)", pc - tb);
  fprintf(stderr, " lr=0x%lx", (unsigned long)lr);
  if (lr >= tb && lr < tb + text_size) fprintf(stderr, " (lr unity+0x%lx)", lr - tb);
  fprintf(stderr, " ===\n"); dbg_sync();
  for (int i = 0; i < 31; i++) {
    fprintf(stderr, " x%-2d=0x%016lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
    if (i % 3 == 2) fprintf(stderr, "\n");
  }
  dbg_sync();
  /* stack scan limitado à região mapeada da pilha desta thread (evita ler além
     do fim do mapping e re-faultar dentro do handler). */
  fprintf(stderr, "[stack scan]\n");
  uintptr_t sp = uc->uc_mcontext.sp;
  uintptr_t slo = 0, shi = 0; char sperm[5];
  maps_find(sp, &slo, &shi, sperm);
  uintptr_t send = shi ? shi : sp + 400 * 8;
  for (uintptr_t a = sp, hits = 0; a + 8 <= send && hits < 32; a += 8) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= tb && v < tb + text_size) { fprintf(stderr, "  [sp+0x%lx] libunity+0x%lx\n", a - sp, v - tb); hits++; }
    else if (g_il2cpp_base && v >= g_il2cpp_base && v < g_il2cpp_base + 0x3000000)
      { fprintf(stderr, "  [sp+0x%lx] libil2cpp+0x%lx\n", a - sp, v - g_il2cpp_base); hits++; }
  }
  dbg_sync();
  /* 🔎 unwind por frame-pointer (x29): [x29]=próximo x29, [x29+8]=lr salvo.
     Reconstrói o backtrace REAL mesmo com pc=0/lr=0 (call-site imediato perdido). */
  {
    fprintf(stderr, "[FP] backtrace via x29:\n");
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 24 && fp; i++) {
      uintptr_t flo=0, fhi=0; char fperm[5]; maps_find(fp, &flo, &fhi, fperm);
      if (!fhi || fp + 16 > fhi) break;
      uintptr_t nfp = *(uintptr_t *)fp, ret = *(uintptr_t *)(fp + 8);
      fprintf(stderr, "[FP]  #%d ret=0x%lx", i, (unsigned long)ret);
      if (ret >= tb && ret < tb + text_size) fprintf(stderr, " libunity+0x%lx", ret - tb);
      else if (g_il2cpp_base && ret >= g_il2cpp_base && ret < g_il2cpp_base + 0x3000000)
        fprintf(stderr, " libil2cpp+0x%lx", ret - g_il2cpp_base);
      fprintf(stderr, "\n");
      if (nfp <= fp || nfp - fp > 0x100000) break;  /* cadeia inválida */
      fp = nfp;
    }
    dbg_sync();
  }

  /* ---- dump rico do crash 0x7f10000004 (vtable/delegate corrompido) ---- */
  uintptr_t fault = (uintptr_t)si->si_addr;
  fprintf(stderr, "[CR] ==== diagnóstico de corrupção ====\n");
  crash_classify("pc", pc);
  crash_classify("fault", fault);
  /* região do ponteiro-lixo (pc=0x7f10000004): o que é 0x7f10000000? */
  crash_classify("pc_region", pc & ~0xFFFUL);
  crash_dump_qwords("pc_target", pc & ~0xFUL, 8);
  /* singleton: *(libunity_data + 0xd18) → método[0] foi p/ o lixo */
  if (g_unity_data) {
    uintptr_t pslot = g_unity_data + 0xd18;
    crash_classify("singleton_slot(d18)", pslot);
    if (addr_readable(pslot)) {
      uintptr_t sgl = *(uintptr_t *)pslot;
      crash_classify("singleton_obj", sgl);
      crash_dump_qwords("singleton", sgl, 16);
    }
  }
  /* dispatcher std::function/delegate: x19 é o objeto; lê [x19+248/256/264] */
  uintptr_t x19 = uc->uc_mcontext.regs[19];
  crash_classify("x19(dispatch_obj)", x19);
  crash_dump_qwords("x19", x19, 40);   /* cobre +0..+312 (inclui 248/256/264) */
  /* x8 = ponteiro de função chamado (= pc no blr x8); x21 = this provável */
  crash_classify("x8", uc->uc_mcontext.regs[8]);
  crash_classify("x21", uc->uc_mcontext.regs[21]);
  /* x20/x22/x23/x24: candidatos a 'this'/objeto pai */
  crash_classify("x20", uc->uc_mcontext.regs[20]);
  crash_classify("x22", uc->uc_mcontext.regs[22]);
  /* SITE DA CHAMADA: lr = retorno após o `blr` que pulou p/ 0x7f10000004.
     Classifica lr e dumpa as 4 instruções em lr-12..lr (acha o blr Xn + o ldr
     que carregou o ponteiro lixo: revela DE ONDE vem 0x7f10000004). */
  crash_classify("lr(call-site)", lr);
  if (addr_readable((lr - 16) & ~0x3UL)) {
    fprintf(stderr, "[CR] insns @lr-16..lr:\n");
    for (uintptr_t a = (lr - 16) & ~0x3UL; a <= lr; a += 4)
      fprintf(stderr, "[CR]   0x%lx: %08x%s\n", (unsigned long)a,
              *(uint32_t *)a, a == lr - 4 ? "  <- blr (chamou o lixo)" : "");
    dbg_sync();
  }
  /* alvo dos ponteiros da singleton (campos = 0x7f..cXX espaçados 4B): o que há lá? */
  if (g_unity_data && addr_readable(g_unity_data + 0xd18)) {
    uintptr_t sgl = *(uintptr_t *)(g_unity_data + 0xd18);
    if (addr_readable(sgl)) {
      uintptr_t tgt = *(uintptr_t *)sgl;       /* singleton[0] = 1º ponteiro */
      crash_classify("singleton[0]_target", tgt);
      crash_dump_qwords("sgl[0]_tgt", tgt & ~0xFUL, 8);
    }
  }
  /* x3/x9/x27: ponteiros 0x7f14.. recorrentes — que região? */
  crash_classify("x3", uc->uc_mcontext.regs[3]);
  crash_classify("x9", uc->uc_mcontext.regs[9]);
  crash_classify("x17", uc->uc_mcontext.regs[17]);
  fprintf(stderr, "[CR] ==== fim ====\n");
  dbg_sync();
  sync(); /* nao deixar um save meio-escrito preso no page cache ao morrer */
  _exit(128 + sig);
}

/* ---------- overrides bionic->glibc (do re4) ---------- */
/* sysconf: Unity lê _SC_* com constantes BIONIC (≠ glibc) → page/nproc/phys errados. */
/* sincos/sincosf reais (o stub gerado não escrevia as saídas — NaN nas rotações) */
extern void sincos(double, double *, double *);
extern void sincosf(float, float *, float *);
static void my_sincos(double x, double *s, double *c) { sincos(x, s, c); }
static void my_sincosf(float x, float *s, float *c) { sincosf(x, s, c); }
static long my_sysconf(int name) {
  switch (name) {
    case 0x27: return getpagesize();                 /* _SC_PAGESIZE */
    case 0x61: return sysconf(_SC_NPROCESSORS_CONF); /* _SC_NPROCESSORS_CONF */
    case 0x62: return sysconf(_SC_NPROCESSORS_ONLN); /* _SC_NPROCESSORS_ONLN */
    case 0x0a: return sysconf(_SC_OPEN_MAX);
    default: return sysconf(name);
  }
}
/* Diagnostic-only affinity override retained outside the public launcher. */
static int my_sched_getaffinity(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (mask && setsize >= sizeof(unsigned long)) {
    memset(mask, 0, setsize);
    *(unsigned long *)mask = 1UL;   /* só CPU 0 */
    return 0;
  }
  return -1;
}
/* mmap spy: a arena de 2MB @ 0x7f10000000 (onde os vtables corrompidos apontam)
 * é um mmap de 0x200000. Logamos alocações desse tamanho + o caller (RA→libunity/
 * il2cpp offset) p/ identificar QUAL alocador/subsistema cria a arena. CUP_MMAPLOG. */
static int g_mmaplog;
extern void *mmap(void *, size_t, int, int, int, long);  /* glibc real */
static void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  void *r = mmap(addr, len, prot, flags, fd, off);
  if (g_mmaplog && (len == 0x200000 || (len >= 0x100000 && len <= 0x400000))) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    const char *lib = "?"; uintptr_t off2 = ra;
    if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + text_size) { lib = "libunity"; off2 = ra - g_unity_base; }
    else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000) { lib = "libil2cpp"; off2 = ra - g_il2cpp_base; }
    fprintf(stderr, "[MMAP] len=0x%zx prot=%d -> %p  caller=%s+0x%lx\n",
            len, prot, r, lib, (unsigned long)off2);
    fsync(2);
  }
  return r;
}
/* /proc/cpuinfo + /sys/.../cpu: Unity conta cores p/ dimensionar job workers. */
static int g_dllog;
static const char *asset_redirect(const char *p, char *buf, size_t bufsz);
static FILE *my_fopen(const char *p, const char *m) {
  if (p && !strcmp(p, "/proc/meminfo")) {
    FILE *t = tmpfile(); if (t) { fputs("MemTotal:      524288 kB\nMemFree:       262144 kB\nMemAvailable:  262144 kB\n", t); rewind(t); return t; }
  }
  if (p && (!strcmp(p, "/sys/devices/system/cpu/possible") || !strcmp(p, "/sys/devices/system/cpu/present") || !strcmp(p, "/sys/devices/system/cpu/online"))) {
    FILE *t = tmpfile(); if (t) { fputs(getenv("CUP_1CORE") ? "0\n" : "0-3\n", t); rewind(t); return t; }
  }
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r) {
    if (g_dllog) fprintf(stderr, "[fopen-redir] %s -> %s\n", p, r);
    return fopen(r, m);
  }
  return fopen(p, m);
}
/* redirect genérico de assets: o engine monta paths de dados com bases erradas
   (APK inexistente, filesdir). Mapeia qualquer tentativa p/ os arquivos REAIS
   deployados em bin/Data (mesma receita do global-metadata.dat, generalizada:
   pega o sufixo após "bin/Data/", senão o basename de arquivos conhecidos do
   engine — globalgamemanagers, level*, sharedassets*, *.assets/.resS/.resource). */
static const char *asset_redirect(const char *p, char *buf, size_t bufsz) {
  if (!p) return NULL;
  char data_root[4096];
  if (ter_game_path(data_root, sizeof(data_root), "bin/Data/") != 0) return NULL;
  /* /data/local/tmp -> /tmp (writable tmpfs). O jogo faz um CASESENSITIVETEST criando
     um arquivo em /data/local/tmp; nosso / é squashfs RO e /data nem existe -> a criação
     falha -> exceção C++ -> (dl_iterate_phdr stubado) std::terminate -> abort. Redireciona
     pro /tmp gravável. SEM access-check (é p/ CRIAR arquivo novo). */
  if (!strncmp(p, "/data/local/tmp", 15)) {
    snprintf(buf, bufsz, "/tmp%s", p + 15);
    return buf;
  }
  /* TER_1CPU: Unity lê /sys/devices/system/cpu/{present,possible,online} p/ contar cores e
     cria (nº cores - 1) Job.Worker threads. O job-system NÃO despacha trabalho pros workers no
     nosso so-loader (eles ficam ociosos; main trava em WaitForJobGroup, counter=0). Reportando
     1 core (string "0"), Unity cria 0 Job.Worker → roda os jobs INLINE na própria thread. */
  if (getenv("TER_1CPU") && !strncmp(p, "/sys/devices/system/cpu/", 24)) {
    const char *leaf = p + 24;
    if (!strcmp(leaf, "present") || !strcmp(leaf, "possible") || !strcmp(leaf, "online")) {
      static const char *fake = "/tmp/ter_cpu0";
      int fd = open(fake, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) { if (write(fd, "0\n", 2) < 0) {} close(fd); }
      snprintf(buf, bufsz, "%s", fake);
      return buf;
    }
  }
  /* QUALQUER path .../AssetBundles/<nome> -> /storage/cuphead-sa/AssetBundles/<nome>.
     Resolve o load do DLC (base-path vinha lixo: "Шестигранные врата 1/AssetBundles/..")
     e qualquer base estranha; o path correto redireciona p/ si mesmo (anti-loop). */
  const char *ab = strstr(p, "/AssetBundles/");
  /* path RELATIVO "AssetBundles/<nome>" (cutscene do livro monta sem base) */
  if (!ab && !strncmp(p, "AssetBundles/", 13)) ab = p - 1;
  if (ab) {
    const char *sap = getenv("CUP_SAPATH"); if (!sap) sap = "/storage/cuphead-sa";
    snprintf(buf, bufsz, "%s/AssetBundles/%s", sap, ab + 14);
    if (strcmp(buf, p) != 0 && access(buf, F_OK) == 0) return buf;
    return NULL;
  }
  /* anti-loop: só pula o que JÁ aponta pro alvo (bin/Data real); paths de
     userdata/ sob a base ainda precisam de redirect (il2cpp/Metadata) */
  if (!strncmp(p, data_root, strlen(data_root))) return NULL;
  const char *sub = strstr(p, "bin/Data/");
  if (sub) {
    char rel[1024];
    if (snprintf(rel, sizeof(rel), "bin/Data/%s", sub + 9) < (int)sizeof(rel) &&
        ter_game_path(buf, bufsz, rel) == 0 && access(buf, F_OK) == 0) return buf;
  }
  const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
  if (!strcmp(base, "global-metadata.dat")) {
    return ter_game_path(buf, bufsz,
                         "bin/Data/Managed/Metadata/global-metadata.dat") == 0 ? buf : NULL;
  }
  /* il2cpp procura <userdata>/il2cpp/Resources/*-resources.dat */
  if (strstr(base, "-resources.dat")) {
    char rel[1024];
    if (snprintf(rel, sizeof(rel), "bin/Data/Managed/Resources/%s", base) < (int)sizeof(rel) &&
        ter_game_path(buf, bufsz, rel) == 0 && access(buf, F_OK) == 0) return buf;
  }
  if (!strncmp(base, "level", 5) || !strncmp(base, "sharedassets", 12) ||
      !strncmp(base, "globalgamemanagers", 18) || strstr(base, ".assets") ||
      strstr(base, ".resS") || strstr(base, ".resource") ||
      !strcmp(base, "data.unity3d") || !strcmp(base, "boot.config") ||
      !strcmp(base, "unity default resources") || !strcmp(base, "unity_builtin_extra")) {
    char rel[1024];
    if (snprintf(rel, sizeof(rel), "bin/Data/%s", base) < (int)sizeof(rel) &&
        ter_game_path(buf, bufsz, rel) == 0 && access(buf, F_OK) == 0) return buf;
    if (snprintf(rel, sizeof(rel), "bin/Data/Resources/%s", base) < (int)sizeof(rel) &&
        ter_game_path(buf, bufsz, rel) == 0 && access(buf, F_OK) == 0) return buf;
  }
  return NULL;
}
/* command line do Unity: lido de /proc/<pid>/cmdline (args separados por \0).
   Injeta -force-gfx-st (single-threaded GFX) p/ matar o GfxDeviceWorker e o
   deadlock main<->worker no boot. CUP_GFXARGS sobrescreve. */
static int cmdline_fd(void) {
  const char *extra = getenv("CUP_GFXARGS");
  char buf[256]; int n = 0;
  n += sprintf(buf + n, "cuphead") + 1;
  if (extra && *extra) {
    /* CUP_GFXARGS="-a -b" -> cada token \0-terminado */
    char tmp[200]; strncpy(tmp, extra, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char *t = strtok(tmp, " "); t; t = strtok(NULL, " ")) n += sprintf(buf + n, "%s", t) + 1;
  } else {
    /* -force-gfx-direct = render DIRETO na main thread (sem GfxDeviceWorker). O nome antigo
       "-force-gfx-st" NÃO é arg real do Unity (era ignorado → worker MT continuava vivo →
       deadlock main<->worker no boot). */
    n += sprintf(buf + n, "-force-gfx-direct") + 1;
    n += sprintf(buf + n, "-force-gles20") + 1;
  }
  FILE *t = tmpfile();
  if (!t) return -1;
  fwrite(buf, 1, n, t); fflush(t);
  int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET);
  fprintf(stderr, "[CMDLINE] injetado (%d bytes): force-gfx-st\n", n);
  return fd;
}
/* TER_GUIDLOG: rastreia o fd do unity_app_guid p/ ver COMO o engine lê (read/
 * lseek/fstat/mmap/close) — diagnóstico do "guid is empty". */
static int g_guidlog;
static int g_guid_fd = -1;
static int my_open(const char *p, int fl, ...) {
  if (p && !strcmp(p, "/proc/cpuinfo")) {
    int nc = getenv("CUP_1CORE") ? 1 : 4;
    FILE *t = tmpfile();
    if (t) { for (int i = 0; i < nc; i++) fprintf(t, "processor\t: %d\nCPU implementer\t: 0x41\nCPU architecture: 8\n\n", i);
      fflush(t); int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET); return fd; }
  }
  if (p && strstr(p, "cmdline") && !getenv("CUP_NOGFXARGS")) return cmdline_fd();
  char rb[512];
  const char *r = asset_redirect(p, rb, sizeof rb);
  if (r) {
    int rmode = 0;
    if (fl & O_CREAT) { va_list ap; va_start(ap, fl); rmode = va_arg(ap, int); va_end(ap); }
    int fd = open(r, fl, rmode);
    if (g_dllog) fprintf(stderr, "[open-redir%s] %s -> %s\n", fd < 0 ? "-MISS" : "", p, r);
    if (g_guidlog && p && strstr(p, "unity_app_guid")) {
      g_guid_fd = fd;
      struct stat sb; int sr = fstat(fd, &sb);
      fprintf(stderr, "[GUID] open '%s' fl=0x%x -> fd=%d (fstat rc=%d st_size=%lld)\n",
              p, fl, fd, sr, sr == 0 ? (long long)sb.st_size : -1LL);
      fsync(2);
    }
    return fd;
  }
  va_list ap; va_start(ap, fl); int mo = va_arg(ap, int); va_end(ap);
  int fd = open(p, fl, mo);
  if (g_dllog && p) fprintf(stderr, "[open%s] %s\n", fd < 0 ? "-MISS" : "", p);
  if (g_guidlog && p && strstr(p, "unity_app_guid")) {
    g_guid_fd = fd;
    fprintf(stderr, "[GUID] open(noredir) '%s' fl=0x%x -> fd=%d\n", p, fl, fd);
    fsync(2);
  }
  return fd;
}
extern ssize_t read(int, void *, size_t);
extern off64_t lseek64(int, off64_t, int);
extern int fstat64(int, struct stat64 *);
extern void *mmap64(void *, size_t, int, int, int, off64_t);
static ssize_t my_read(int fd, void *buf, size_t n) {
  ssize_t r = read(fd, buf, n);
  if (g_guidlog && fd == g_guid_fd) {
    fprintf(stderr, "[GUID] read(fd=%d, n=%zu) -> %zd  first='%.40s'\n",
            fd, n, r, r > 0 ? (char *)buf : "");
    fsync(2);
  }
  return r;
}
static off64_t my_lseek64(int fd, off64_t off, int wh) {
  off64_t r = lseek64(fd, off, wh);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] lseek64(fd=%d, off=%lld, wh=%d) -> %lld\n",
            fd, (long long)off, wh, (long long)r), fsync(2);
  return r;
}
static int my_fstat64(int fd, struct stat64 *st) {
  int r = fstat64(fd, st);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] fstat64(fd=%d) -> rc=%d st_size=%lld\n",
            fd, r, r == 0 ? (long long)st->st_size : -1LL), fsync(2);
  return r;
}
/* builds mais novos (NDK novo) importam `fstat` direto — símbolo que a glibc só
   exporta a partir da 2.33; sem shim o slot fica UNRESOLVED (lixo) e o primeiro
   fstat do init da Unity vira SIGSEGV (visto no build Play 301544). Em aarch64
   struct stat == struct stat64, então delega no fstat64 do host. */
static int my_fstat(int fd, void *st) {
  return fstat64(fd, (struct stat64 *)st);
}
/* DNS desligado no so-loader: devolve EAI_NONAME (8, valor bionic) com *res
   NULL — nunca "sucesso" sem resultado (ver comentário no set_import). */
static int my_getaddrinfo(const char *node, const char *svc, const void *hints,
                          void **res) {
  (void)svc; (void)hints;
  if (res) *res = NULL;
  static int n;
  if (n++ < 4)
    fprintf(stderr, "[NET] getaddrinfo('%s') -> EAI_NONAME (DNS off)\n",
            node ? node : "?");
  return 8;
}
static void *my_mmap64(void *a, size_t len, int prot, int fl, int fd, off64_t off) {
  void *r = mmap64(a, len, prot, fl, fd, off);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] mmap64(fd=%d, len=%zu, off=%lld) -> %p  first='%.40s'\n",
            fd, len, (long long)off, r,
            (r && r != MAP_FAILED && (prot & PROT_READ)) ? (char *)r : ""), fsync(2);
  return r;
}
static FILE *my_fdopen(int fd, const char *mode) {
  FILE *r = fdopen(fd, mode);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] fdopen(fd=%d, '%s') -> %p\n", fd, mode ? mode : "?", (void *)r), fsync(2);
  return r;
}
/* stat/lstat/access com o mesmo redirect — o engine checa existência antes de
   abrir ("No GlobalGameManagers file" pode vir de um stat, não do open).
   Layout de struct stat arm64 = kernel em bionic E glibc → pass-through ok. */
static int my_stat(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r && g_dllog) fprintf(stderr, "[stat-redir] %s -> %s\n", p, r);
  int rc = stat(r ? r : p, (struct stat *)st);
  if (g_dllog && rc < 0 && p) fprintf(stderr, "[stat-MISS] %s\n", p);
  return rc;
}
static int my_lstat(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  return lstat(r ? r : p, (struct stat *)st);
}
/* 🔑 stat64: libunity importa stat64 (NÃO stat). O leitor de arquivos (ReadAllBytes
   @0x21db60 -> GetFileSize @0x22b7c0) pega o TAMANHO via stat64(path); sem redirect,
   o path "assets/bin/Data/unity_app_guid" não existe em disco -> stat64 falha -> size 0
   -> lê 0 bytes -> guid "is empty" -> re-extract -> "Unable to initialize". O open()
   funcionava (redirecionado) mas o size não. arm64: struct stat == struct stat64. */
static int my_stat64(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r && g_dllog) fprintf(stderr, "[stat64-redir] %s -> %s\n", p, r);
  int rc = stat64(r ? r : p, (struct stat64 *)st);
  if (g_dllog && rc < 0 && p) fprintf(stderr, "[stat64-MISS] %s\n", p);
  return rc;
}
static int my_lstat64(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  return lstat64(r ? r : p, (struct stat64 *)st);
}
/* === Enlighten allocator (GI) === FIX do null-deref no HLRTManager/GeoArray.
 * O allocator do Enlighten é um singleton em libunity+0xc886a0. init-A (0x32ea38) instala
 * um allocator VÁLIDO no boot (confirmado: SetMemoryManager(0x7f60...)), MAS algo o ZERA
 * (teardown-B 0x32ec10 = ÚNICO outro writer) antes da criação do HLRTManager (realtime GI da
 * cena 2D). Com singleton NULL, o wrapper de alloc (0x861928) retorna NULL -> ctor do GeoArray
 * faz `str x8,[NULL]` -> SIGSEGV. FIX: substituir o wrapper 0x861928 (52B, cabe trampolim 16B)
 * por my_enl_alloc: usa o allocator REAL quando o singleton é válido (idêntico ao original) e
 * cai p/ posix_memalign quando NULL (evita o crash). */
static int g_enllog;
extern void so_make_text_writable(void), so_make_text_executable(void), so_flush_caches(void);
static void patch_tramp(uintptr_t off, void *fn) {
  uint32_t *p = (uint32_t *)(g_unity_base + off);
  so_make_text_writable();
  p[0] = 0x58000050u;            /* ldr x16, #8  (carrega o .quad abaixo) */
  p[1] = 0xd61f0200u;            /* br  x16      */
  *(uint64_t *)(p + 2) = (uint64_t)fn;  /* .quad fn (ocupa p[2],p[3]) */
  so_make_text_executable(); so_flush_caches();
}
/* assinatura na entrada de 0x861928: (w0=size, w1=align, x2=a2, w3=label, x4=name) -> ptr */
static void *my_enl_alloc(unsigned long size, unsigned long align, void *a2, int label, void *name) {
  void *mm = g_unity_base ? *(void **)(g_unity_base + 0xc886a0) : 0;
  void *r = 0;
  if (mm) {
    /* allocator REAL: vtable[+0x10](this, size, align, a2, label, name) — idêntico ao original */
    void *vt = *(void **)mm;
    void *(*real)(void *, unsigned long, unsigned long, void *, int, void *) =
        *(void *(**)(void *, unsigned long, unsigned long, void *, int, void *))((char *)vt + 0x10);
    r = real(mm, size, align, a2, label, name);
  }
  if (!r) {
    /* singleton NULL OU allocator real devolveu NULL: fallback malloc alinhado (evita o crash) */
    if (align < 8 || (align & (align - 1))) align = 16;
    void *p = NULL;
    if (posix_memalign(&p, align, size ? size : 1) == 0) r = p;
  }
  if (g_enllog) { fprintf(stderr, "[ENL] alloc size=%lu align=%lu label=%d mm=%p -> %p\n", size, align, label, mm, r); fsync(2); }
  return r;
}
static int my_access(const char *p, int m) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r && g_dllog) fprintf(stderr, "[access-redir] %s -> %s\n", p, r);
  return access(r ? r : p, m);
}
/* statfs64: Unity checa espaço livre via statfs64(path) p/ "instalar resources".
   O path que ele passa pode ser Android (/data/...) inexistente -> erro -> 0 livre ->
   "Not enough storage space". Ignoramos o path e medimos o NOSSO GAMEDIR real (93GB).
   glibc preenche o buffer no layout do kernel statfs64 = o que o bionic espera. */
/* FORTIFY do bionic (__*_chk): glibc não tem esses símbolos -> viram stub (NÃO copiam)
   -> corrupção de heap. Implementações reais (ignoram o arg de bounds-check). */
static void *my_memmove_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
static void *my_memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
static void *my_memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }
static size_t my_strlen_chk(const char *s, size_t mn) { (void)mn; return strlen(s); }
static char *my_strcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return strcpy(d, s); }
static char *my_strcat_chk(char *d, const char *s, size_t dn) { (void)dn; return strcat(d, s); }
static int my_vsnprintf_chk(char *str, size_t sz, int flag, size_t slen, const char *fmt, va_list ap) {
  (void)flag; (void)slen; return vsnprintf(str, sz, fmt, ap); }
static int my_snprintf_chk(char *str, size_t sz, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen; va_list ap; va_start(ap, fmt); int r = vsnprintf(str, sz, fmt, ap); va_end(ap); return r; }
static void my_FD_SET_chk(int fd, fd_set *s, size_t n) { (void)n; if (fd >= 0) FD_SET(fd, s); }
/* strlcpy/strlcat (bionic) — o regex de passthrough não cobre (vira stub que NÃO copia
   -> buffer com lixo -> heap corruption "free(): invalid size"). Implementação real. */
static unsigned long my_strlcpy(char *dst, const char *src, unsigned long sz) {
  unsigned long n = strlen(src);
  if (sz) { unsigned long c = n < sz - 1 ? n : sz - 1; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
/* 🔑 memalign: estava STUBADO (única fn de alloc não-passthrough) -> retornava NULL.
   libunity E libil2cpp importam memalign; o allocator do Enlighten (GI) usa memalign p/
   memória alinhada -> NULL -> ctor do HLRTManager/GeoArray recebe `this`=NULL -> SIGSEGV.
   Impl real via posix_memalign (memalign do glibc é deprecated). Alinhamento >= sizeof(void*)
   e potência de 2 (exigência do posix_memalign). */
/* Android arm64 and Linux arm64 use the same kernel syscall numbers.  Preserve
   Unity's native synchronization and signal flow by forwarding the complete
   argument list unchanged.  In particular, do not rewrite futex waits or
   rt_sigprocmask calls here. */
extern long syscall(long, ...);
static long my_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
  if (n == 98 && getenv("TER_FUTEXTRACE")) { /* arm64 SYS_futex */
    int operation = (int)a2 & 0x7f;
    int is_wait = operation == 0 || operation == 9;
    int is_wake = operation == 1 || operation == 10;
    static unsigned events;
    static _Thread_local long last_wait_address;
    if ((is_wake || (is_wait && last_wait_address != a1)) &&
        __atomic_fetch_add(&events, 1, __ATOMIC_RELAXED) < 768) {
      if (is_wait) last_wait_address = a1;
      fprintf(stderr,
              "[futex] %s tid=%ld addr=%p value=%ld op=%d caller=%p\n",
              is_wait ? "wait" : "wake", syscall(SYS_gettid),
              (void *)a1, a3, operation, __builtin_return_address(0));
    }
  }
  return syscall(n, a1, a2, a3, a4, a5, a6);
}
/* TER_PKLOG: loga pthread_kill (quem o GC sinaliza p/ suspender + qual sinal) — diagnóstico
   do stop-the-world travado (nenhuma thread dá ACK). */
extern int pthread_kill(pthread_t, int);
extern const char *ter_thread_comm(pthread_t t);
static int my_pthread_kill(pthread_t t, int sig) {
  static int n;
  if (getenv("TER_PKLOG") && n++ < 60) { fprintf(stderr, "[PKILL] -> %s sig=%d\n", ter_thread_comm(t), sig); fsync(2); }
  /* TER_NOSUSPEND: ENGOLE os sinais de stop-the-world do GC (SIGPWR=30 suspend / SIGXCPU=24 restart).
     Nenhuma thread é suspensa → o GC (com GCOFF, sem scan) só precisa que os WAITs retornem (NOGCWAIT
     + patch do restart-wait). Neutraliza o STW inteiro sem alcançar as threads bionic-static. */
  if (getenv("TER_NOSUSPEND") && (sig == 30 || sig == 24)) return 0;
  /* 🔑 TER_FAKEACK: a thread bionic-static que o GC quer suspender bloqueia SIGPWR e nunca dá ACK.
     O semáforo de ACK que o WaitForThreadsToSuspend espera é o NOSSO sem_shim em il2cpp+0x31666a0.
     Então POSTAMOS o sem no lugar da thread (fake ACK) + ENGOLIMOS o sinal (a thread não suspende) →
     o GC conta o ACK e segue o fluxo NORMAL (≠ NOGCWAIT). Usar com CUP_GCOFF (sem scan de stack viva). */
  if (getenv("TER_FAKEACK") && (sig == 30 || sig == 24) && g_il2cpp_base) {
    extern int sh_sem_post(void *);
    sh_sem_post((void *)(g_il2cpp_base + 0x31666a0));   /* ACK do suspend (sem do WaitForThreadsToSuspend) */
    return 0;
  }
  return pthread_kill(t, sig);
}
/* fsync-on-close: Terraria escreve .wld/.map/.plr sem fsync; morte abrupta
   (OOM/crash/power) descarta o fim do arquivo do page cache -> save que "so
   abre com o minimapa desligado". Um fsync por arquivo GRAVAVEL fechado e
   barato (saves sao eventuais) e fecha essa janela para sempre. */
static int my_close(int fd) {
  if (fd > 2) {
    int flags = fcntl(fd, F_GETFL);
    if (flags != -1 && (flags & O_ACCMODE) != O_RDONLY) {
      struct stat st;
      if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) fsync(fd);
    }
  }
  return close(fd);
}
static void *my_memalign(unsigned long alignment, unsigned long size) {
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  if (alignment & (alignment - 1)) { unsigned long a = sizeof(void *); while (a < alignment) a <<= 1; alignment = a; }
  void *p = NULL;
  if (posix_memalign(&p, alignment, size ? size : 1) != 0) return NULL;
  return p;
}
static unsigned long my_strlcat(char *dst, const char *src, unsigned long sz) {
  unsigned long dl = strnlen(dst, sz), sl = strlen(src);
  if (dl == sz) return sz + sl;
  unsigned long c = (sl < sz - dl - 1) ? sl : sz - dl - 1;
  memcpy(dst + dl, src, c); dst[dl + c] = 0;
  return dl + sl;
}
static int my_statfs64(const char *p, void *buf) {
  static int (*real)(const char *, void *);
  if (!real) { real = (void *)dlsym(RTLD_DEFAULT, "statfs64");
               if (!real) real = (void *)dlsym(RTLD_DEFAULT, "statfs"); }
  int rc = real ? real(ter_game_dir(), buf) : -1;
  if (g_dllog) fprintf(stderr, "[statfs64] path=%s -> GAMEDIR rc=%d\n", p ? p : "?", rc);
  return rc;
}
/* exit() do jogo: loga QUEM chamou (lr) + stack antes de morrer — a morte
   silenciosa pos-FMOD não deixava rastro. */
static void my_exit(int code) {
  fprintf(stderr, "[EXIT] exit(%d) chamado! lr=%p\n", code, __builtin_return_address(0));
  uintptr_t tb = (uintptr_t)g_unity_base;
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  if (tb && lr >= tb) fprintf(stderr, "[EXIT] (libunity+0x%lx)\n", lr - tb);
  fsync(2);
  _exit(code);
}
/* __system_property_get: FMOD checa ro.build.version.sdk antes de usar OpenSLES
   (vazio→SDK 0→desiste sem nem dar dlsym; receita Dysmantle: "25"). Resto vazio. */
static int my_sysprop(const char *name, char *value) {
  if (!value) return 0;
  if (name && strstr(name, "version.sdk")) { strcpy(value, "25"); return 2; }
  value[0] = 0; return 0;
}
/* __android_log -> stderr. Unity can retry a failed graphics recreation every
 * frame and repeat both EGL_BAD_NATIVE_WINDOW and a multi-kilobyte extension
 * dump indefinitely. Preserve the first diagnostic copies, then bound only
 * those exact repeats so one failed run cannot consume hundreds of megabytes. */
struct ter_alog_repeat {
  uint64_t hash;
  size_t length;
  unsigned count;
};
static struct ter_alog_repeat g_alog_repeats[32];
static pthread_mutex_t g_alog_repeat_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t ter_alog_hash(const char *message, size_t length) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t index = 0; index < length; ++index) {
    hash ^= (unsigned char)message[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static void ter_alog_emit(int prio, const char *tag, const char *message) {
  if (!message) message = "";
  size_t length = strlen(message);
  int bad_window = strstr(message, "EGL_BAD_NATIVE_WINDOW") != NULL;
  int long_gl_dump = length > 2048 && strstr(message, "GL_") != NULL;
  unsigned limit = bad_window ? 6u : 1u;
  unsigned occurrence = 1;

  if (bad_window || long_gl_dump) {
    uint64_t hash = ter_alog_hash(message, length);
    pthread_mutex_lock(&g_alog_repeat_lock);
    struct ter_alog_repeat *free_slot = NULL;
    for (size_t index = 0;
         index < sizeof(g_alog_repeats) / sizeof(g_alog_repeats[0]); ++index) {
      struct ter_alog_repeat *slot = &g_alog_repeats[index];
      if (!slot->hash && !free_slot) free_slot = slot;
      if (slot->hash == hash && slot->length == length) {
        occurrence = ++slot->count;
        free_slot = NULL;
        break;
      }
    }
    if (free_slot) {
      free_slot->hash = hash;
      free_slot->length = length;
      free_slot->count = 1;
    }
    pthread_mutex_unlock(&g_alog_repeat_lock);

    if (occurrence > limit) {
      if (occurrence == limit + 1) {
        fprintf(stderr,
                "[ALOG:%d %s] duplicate message suppressed after %u copies "
                "(length=%zu)\n",
                prio, tag ? tag : "?", limit, length);
      }
      return;
    }
  }

  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", message);
}

static int ter_alog_format(int prio, const char *tag, const char *fmt,
                           va_list arguments) {
  if (!fmt) {
    ter_alog_emit(prio, tag, "");
    return 0;
  }
  va_list measure;
  va_copy(measure, arguments);
  int needed = vsnprintf(NULL, 0, fmt, measure);
  va_end(measure);
  if (needed < 0) return 0;
  char *message = malloc((size_t)needed + 1);
  if (!message) return 0;
  va_list render;
  va_copy(render, arguments);
  vsnprintf(message, (size_t)needed + 1, fmt, render);
  va_end(render);
  ter_alog_emit(prio, tag, message);
  free(message);
  return 0;
}

static int my_alog_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int result = ter_alog_format(prio, tag, fmt, ap);
  va_end(ap);
  return result;
}
static int my_alog_write(int prio, const char *tag, const char *msg) {
  ter_alog_emit(prio, tag, msg);
  return 0;
}
/* __android_log_vprint é o canal do PLAYER LOG do Unity — jamais stubar */
static int my_alog_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  return ter_alog_format(prio, tag, fmt, ap);
}
/* ANativeWindow: Unity espera window !=NULL (nativeRecreateGfxState) senão trava p/ sempre.
   Os egl* do libunity são imports PLT que resolvem no libEGL REAL do Mali (dlopen GLOBAL);
   no fbdev a EGLNativeWindowType é só struct {u16 w, u16 h} → entrega uma DE VERDADE e o
   Unity cria a window surface direto no fb0 (sem shim). CUP_SHIMEGL=1 volta pro fake int. */
static struct { unsigned short w, h; } g_fbdev_win = {0, 0};
static int cup_use_kmsdrm(void);  /* fwd: decide fbdev vs kmsdrm (def. abaixo) */
static int g_anw = 0xA11;
static int ter_env_positive_int_main(const char *name) {
  const char *s = getenv(name);
  if (!s || !*s) return 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  return (end != s && v > 0 && v < 32768) ? (int)v : 0;
}
static int ter_read_screen_pair_main(const char *path, int *w, int *h) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char buf[128];
  int ok = fgets(buf, sizeof(buf), f) != NULL;
  fclose(f);
  if (!ok) return 0;
  int a = 0, b = 0;
  if (sscanf(buf, "%d,%d", &a, &b) != 2 &&
      sscanf(buf, "%dx%d", &a, &b) != 2 &&
      sscanf(buf, "%*[^0-9]%dx%d", &a, &b) != 2) return 0;
  if (a <= 0 || b <= 0 || a >= 32768 || b >= 32768) return 0;
  *w = a; *h = b;
  return 1;
}
static int ter_native_screen_size_main(int *w, int *h) {
  int sw = ter_env_positive_int_main("TER_SCREEN_W");
  int sh = ter_env_positive_int_main("TER_SCREEN_H");
  if (!sw) sw = ter_env_positive_int_main("TER_SCREEN_WIDTH");
  if (!sh) sh = ter_env_positive_int_main("TER_SCREEN_HEIGHT");
  if (sw > 0 && sh > 0) { *w = sw; *h = sh; return 1; }
  if (ter_read_screen_pair_main("/sys/class/graphics/fb0/mode", &sw, &sh) ||
      ter_read_screen_pair_main("/sys/class/graphics/fb0/modes", &sw, &sh) ||
      ter_read_screen_pair_main("/sys/class/graphics/fb0/virtual_size", &sw, &sh)) {
    *w = sw; *h = sh; return 1;
  }
  return 0;
}
static int ter_window_w(void) {
  int sw = 0, sh = 0;
  if (g_fbdev_win.w > 0) return g_fbdev_win.w;
  return ter_native_screen_size_main(&sw, &sh) ? sw : 0;
}
static int ter_window_h(void) {
  int sw = 0, sh = 0;
  if (g_fbdev_win.h > 0) return g_fbdev_win.h;
  return ter_native_screen_size_main(&sw, &sh) ? sh : 0;
}
static void *my_aw_fromSurface(void *e, void *s) { (void)e; (void)s;
  /* kmsdrm: ANativeWindow fake (egl_shim ignora a window). fbdev: struct {w,h} real. */
  if (cup_use_kmsdrm()) return (void *)&g_anw;
  int sw = ter_window_w();
  int sh = ter_window_h();
  if (sw > 0 && sh > 0) {
    g_fbdev_win.w = (unsigned short)sw;
    g_fbdev_win.h = (unsigned short)sh;
  }
  return (void *)&g_fbdev_win;
}
static int my_aw_setgeom(void *w, int a, int b, int c) {
  (void)w; (void)c;
  if (a > 0 && b > 0) {
    g_fbdev_win.w = (unsigned short)a;
    g_fbdev_win.h = (unsigned short)b;
  }
  return 0;
}
static int my_aw_getWidth(void *w) { (void)w; return ter_window_w(); }
static int my_aw_getHeight(void *w) { (void)w; return ter_window_h(); }
static int my_aw_getFormat(void *w) { (void)w; return 1; }
static void my_aw_noop(void *w) { (void)w; }
/* dlopen/dlsym: Unity dlopen libGLESv2/EGL/OpenSLES + dlsym em runtime */
/* ---------- egl_shim (janela GLES2 via SDL2, proven re4) ---------- */
extern void egl_shim_create_window(void);
extern void *egl_shim_GetDisplay(void *);
extern unsigned egl_shim_Initialize(void *, int *, int *);
extern unsigned egl_shim_Terminate(void *);
extern unsigned egl_shim_ChooseConfig(void *, const int *, void **, int, int *);
extern void *egl_shim_CreateWindowSurface(void *, void *, void *, const int *);
extern void *egl_shim_CreatePbufferSurface(void *, void *, const int *);
extern void *egl_shim_CreateContext(void *, void *, void *, const int *);
extern unsigned egl_shim_MakeCurrent(void *, void *, void *, void *);
extern unsigned egl_shim_SwapBuffers(void *, void *);
extern unsigned egl_shim_DestroySurface(void *, void *);
extern unsigned egl_shim_DestroyContext(void *, void *);
extern unsigned egl_shim_QuerySurface(void *, void *, int, int *);
extern unsigned egl_shim_GetConfigAttrib(void *, void *, int, int *);
extern int egl_shim_GetError(void);
extern void *egl_shim_GetProcAddress(const char *);
extern unsigned egl_shim_BindAPI(unsigned);
extern const char *egl_shim_QueryString(void *, int);
extern unsigned egl_shim_SwapInterval(void *, int);
extern void *egl_shim_GetCurrentContext(void);
extern void *egl_shim_GetCurrentSurface(int);
extern unsigned egl_shim_SurfaceAttrib(void *, void *, int, int);
static void *egl_route(const char *nm) {
  struct { const char *n; void *f; } m[] = {
    {"eglGetDisplay", egl_shim_GetDisplay}, {"eglInitialize", egl_shim_Initialize},
    {"eglTerminate", egl_shim_Terminate}, {"eglChooseConfig", egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", egl_shim_CreatePbufferSurface},
    {"eglCreateContext", egl_shim_CreateContext}, {"eglMakeCurrent", egl_shim_MakeCurrent},
    {"eglSwapBuffers", egl_shim_SwapBuffers}, {"eglDestroySurface", egl_shim_DestroySurface},
    {"eglDestroyContext", egl_shim_DestroyContext}, {"eglQuerySurface", egl_shim_QuerySurface},
    {"eglGetConfigAttrib", egl_shim_GetConfigAttrib}, {"eglGetError", egl_shim_GetError},
    {"eglGetProcAddress", egl_shim_GetProcAddress}, {"eglBindAPI", egl_shim_BindAPI},
    {"eglQueryString", egl_shim_QueryString}, {"eglSwapInterval", egl_shim_SwapInterval},
    {"eglGetCurrentContext", egl_shim_GetCurrentContext},
    {"eglGetCurrentSurface", egl_shim_GetCurrentSurface},
    {"eglGetCurrentDisplay", egl_shim_GetDisplay}, {"eglSurfaceAttrib", egl_shim_SurfaceAttrib},
    {0, 0}
  };
  for (int i = 0; m[i].n; i++) if (!strcmp(m[i].n, nm)) return m[i].f;
  return NULL;
}

/* ---------- capability-based video ownership ----------
 * The launcher leaves SDL's inherited/default driver untouched. Once SDL has
 * initialized, fbdev-class drivers keep Unity on the vendor EGL path; every
 * other successful SDL backend owns the window through egl_shim. Merely having
 * /dev/dri/card0 is not evidence that KMSDRM is usable. */
static const char *ter_probe_egl_vendor(void) {
  static int probed;
  static char vendor[160];
  if (probed) return vendor;
  probed = 1;

  typedef void *(*get_display_fn)(void *);
  typedef unsigned (*initialize_fn)(void *, int *, int *);
  typedef const char *(*query_string_fn)(void *, int);
  typedef unsigned (*terminate_fn)(void *);
  get_display_fn get_display =
      (get_display_fn)dlsym(RTLD_DEFAULT, "eglGetDisplay");
  initialize_fn initialize =
      (initialize_fn)dlsym(RTLD_DEFAULT, "eglInitialize");
  query_string_fn query_string =
      (query_string_fn)dlsym(RTLD_DEFAULT, "eglQueryString");
  terminate_fn terminate =
      (terminate_fn)dlsym(RTLD_DEFAULT, "eglTerminate");
  if (!get_display || !initialize || !query_string) return vendor;

  void *display = get_display(NULL);
  int major = 0, minor = 0;
  if (!display || !initialize(display, &major, &minor)) return vendor;
  const char *reported = query_string(display, 0x3053 /* EGL_VENDOR */);
  if (reported) {
    snprintf(vendor, sizeof(vendor), "%s", reported);
  }
  if (terminate) terminate(display);
  return vendor;
}

static int cup_use_kmsdrm(void) {
  static int dec = -1;
  if (dec >= 0) return dec;
  const char *driver = SDL_GetCurrentVideoDriver();
  const char *vendor = ter_probe_egl_vendor();
  dec = ter_video_use_sdl_owner(driver, getenv("CUP_VIDEO"),
                                getenv("CUP_SHIMEGL") != NULL, vendor);
  fprintf(stderr, "[F2] video owner=%s driver=%s egl_vendor=%s\n",
          dec ? "sdl" : "vendor-fbdev", driver ? driver : "(none)",
          vendor && *vendor ? vendor : "(unavailable)");
  return dec;
}

/* Re-roteia os egl* da libunity (hoje bindados no libEGL REAL pelo so_resolve)
 * para o egl_shim. ELO QUE FALTAVA do caminho kmsdrm: sem isto a janela SDL e'
 * criada mas a Unity continua chamando o libEGL real (sem fbdev no Valhall -> nao
 * renderiza). Chamar com o contexto do libunity ativo (so_use(g_m_unity)). */
static int egl_patch_unity_got(void) {
  static const char *names[] = {
    "eglGetDisplay", "eglInitialize", "eglTerminate", "eglChooseConfig",
    "eglCreateWindowSurface", "eglCreatePbufferSurface", "eglCreateContext",
    "eglMakeCurrent", "eglSwapBuffers", "eglDestroySurface", "eglDestroyContext",
    "eglQuerySurface", "eglGetConfigAttrib", "eglGetError", "eglGetProcAddress",
    "eglBindAPI", "eglQueryString", "eglSwapInterval", "eglGetCurrentContext",
    "eglGetCurrentSurface", "eglGetCurrentDisplay", "eglSurfaceAttrib", NULL };
  int total = 0;
  for (int i = 0; names[i]; i++) {
    void *f = (!strcmp(names[i], "eglGetCurrentDisplay")) ? (void *)egl_shim_GetDisplay
                                                          : egl_route(names[i]);
    if (f) total += so_patch_got(names[i], (uintptr_t)f);
  }
  return total;
}

/* glGetString wrapper (proven re4): o preprocessador de shader do Unity chama
 * glGetString(RENDERER/VERSION/EXT) numa thread sem contexto GL current -> real
 * devolve NULL -> parse char-a-char de NULL estoura o buffer (stack smash em
 * nativeRecreateGfxState). Cache + defaults Mali; NUNCA NULL. */
static const unsigned char *(*r_glGetString)(unsigned) = NULL;
static const unsigned char *g_glcache[5] = {0,0,0,0,0};
static int glstr_idx(unsigned n){ switch(n){case 0x1F00:return 0;case 0x1F01:return 1;case 0x1F02:return 2;case 0x1F03:return 3;case 0x8B8C:return 4;} return -1; }
/* GL_EXTENSIONS curado curto: a string real do Mali-450 é longa e o parser do
 * Unity pode estourar um buffer fixo (stack smash em nativeRecreateGfxState). */
static const char *GL_EXT_SHORT =
  "GL_OES_depth24 GL_OES_element_index_uint GL_OES_texture_npot "
  "GL_OES_rgb8_rgba8 GL_OES_packed_depth_stencil GL_OES_vertex_array_object "
  "GL_EXT_texture_format_BGRA8888 GL_OES_standard_derivatives";
static const unsigned char *my_glGetString(unsigned n){
  if(n==0x1F03) return (const unsigned char*)GL_EXT_SHORT;   /* GL_EXTENSIONS curto */
  if(!r_glGetString) r_glGetString=(const unsigned char*(*)(unsigned))dlsym(RTLD_DEFAULT,"glGetString");
  const unsigned char *s = r_glGetString ? r_glGetString(n) : NULL;
  int i = glstr_idx(n);
  if(s){ if(i>=0 && !g_glcache[i]) g_glcache[i]=(const unsigned char*)strdup((const char*)s); }
  else if(i>=0 && g_glcache[i]) s=g_glcache[i];
  else if(i>=0) s=(const unsigned char*)(n==0x1F00?"ARM":n==0x1F01?"Mali-450 MP":n==0x1F02?"OpenGL ES 2.0":n==0x8B8C?"OpenGL ES GLSL ES 1.00":"");
  return s;
}

/* ---- wrappers GL de shader (diagnóstico: shader falha/trava no Mali?) ---- */
static void (*r_glCompileShader)(unsigned);
static void (*r_glGetShaderiv)(unsigned, unsigned, int *);
static void (*r_glLinkProgram)(unsigned);
static void (*r_glGetProgramiv)(unsigned, unsigned, int *);
static void (*r_glGetShaderInfoLog)(unsigned, int, int *, char *);
static int g_shN, g_prN;
static void my_glCompileShader(unsigned sh) {
  if (!r_glCompileShader) r_glCompileShader = dlsym(RTLD_DEFAULT, "glCompileShader");
  if (!r_glGetShaderiv) r_glGetShaderiv = dlsym(RTLD_DEFAULT, "glGetShaderiv");
  if (!r_glGetShaderInfoLog) r_glGetShaderInfoLog = dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
  r_glCompileShader(sh);
  int st = -1; if (r_glGetShaderiv) r_glGetShaderiv(sh, 0x8B81, &st); /* COMPILE_STATUS */
  if (st != 1 && g_shN < 100) {
    char log[768] = {0}; if (r_glGetShaderInfoLog) r_glGetShaderInfoLog(sh, sizeof log - 1, NULL, log);
    fprintf(stderr, "[SHADER] compile FALHOU sh=%u status=%d LOG=%s\n", sh, st, log); dbg_sync();
    g_shN++;
  }
}
/* CUP_SHADERDUMP: loga o fonte GLSL na SUBMISSÃO (glGetShaderSource no Mali volta vazio) */
extern volatile int g_render_frame;
static int strstr2_any(const char **string, int count, const char *tok) {
  for (int i = 0; i < count && string[i]; i++)
    if (strstr(string[i], tok)) return 1;
  return 0;
}
static void (*r_glShaderSource)(unsigned, int, const char **, const int *);
static void my_glShaderSource(unsigned sh, int count, const char **string, const int *length) {
  if (!r_glShaderSource) r_glShaderSource = dlsym(RTLD_DEFAULT, "glShaderSource");
  if (getenv("CUP_SHADERDUMP") && string) {
    fprintf(stderr, "[SHSRC] shader=%u count=%d f=%d:\n", sh, count, g_render_frame);
    size_t tot = 0;
    for (int i = 0; i < count && string[i] && tot < 6000; i++) {
      int len = length ? length[i] : (int)strlen(string[i]);
      if (len > (int)(6000 - tot)) len = (int)(6000 - tot);
      fwrite(string[i], 1, len, stderr);
      tot += len;
    }
    fprintf(stderr, "\n[SHSRC] ---fim shader=%u---\n", sh); fsync(2);
  }
  /* CUP_ALPHAFIX: sprites/cenário/chefes INVISÍVEIS — o variant ETC1-split-alpha
   * sampleia _AlphaTex (bound num dummy 4x4) com _EnableExternalAlpha=1:
   *   alpha = mix(_MainTex.a, _AlphaTex.x, _EnableExternalAlpha) -> 0 -> transparente.
   * Os atlases aqui sobem DESCOMPRIMIDOS (RGBA com alpha real no .a — o player prova).
   * Patch: remove a declaração e troca usos de _EnableExternalAlpha por 0.0
   * (força o caminho interno _MainTex.a). */
  if (getenv("CUP_ALPHAFIX") && string &&
      (strstr2_any(string, count, "_EnableExternalAlpha") ||
       strstr2_any(string, count, "_RendererColor"))) {
    /* tokens neutralizados (substituição com fronteira de identificador):
     *   _EnableExternalAlpha -> 0.0       (força alpha interno _MainTex.a)
     *   _RendererColor/_Color -> vec4(1.0) (uniform não-setado = 0 em GLES2 -> cor*0 = invisível) */
    static const struct { const char *tok, *rep; } T[] = {
      {"_EnableExternalAlpha", "0.0"},
      {"_RendererColor", "vec4(1.0)"},
      {"_Color", "vec4(1.0)"},
    };
    size_t tot = 0;
    for (int i = 0; i < count && string[i]; i++)
      tot += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    char *buf = tot < 65536 ? malloc(tot + 1) : NULL;
    if (buf) {
      size_t o = 0;
      for (int i = 0; i < count && string[i]; i++) {
        size_t l = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(buf + o, string[i], l); o += l;
      }
      buf[o] = 0;
      char *out = malloc(o * 2 + 64);
      if (out) {
        size_t w = 0;
        for (char *p = buf; *p; ) {
          char *nl = strchr(p, '\n');
          size_t ll = nl ? (size_t)(nl - p + 1) : strlen(p);
          int drop = 0;
          if (memmem(p, ll, "uniform", 7))
            for (unsigned t = 0; t < sizeof T / sizeof T[0] && !drop; t++)
              if (memmem(p, ll, T[t].tok, strlen(T[t].tok))) drop = 1;  /* corta declaração */
          if (drop) { p += ll; continue; }
          for (size_t k = 0; k < ll; ) {
            int hit = 0;
            for (unsigned t = 0; t < sizeof T / sizeof T[0]; t++) {
              size_t tl = strlen(T[t].tok);
              if (k + tl <= ll && !memcmp(p + k, T[t].tok, tl)) {
                char b = k ? p[k - 1] : ' ', a = (k + tl < ll) ? p[k + tl] : ' ';
                if (!(isalnum(b) || b == '_') && !(isalnum(a) || a == '_')) {  /* fronteira */
                  size_t rl = strlen(T[t].rep);
                  memcpy(out + w, T[t].rep, rl); w += rl; k += tl; hit = 1; break;
                }
              }
            }
            if (!hit) out[w++] = p[k++];
          }
          p += ll;
        }
        out[w] = 0;
        fprintf(stderr, "[ALPHAFIX] shader=%u: ExternalAlpha->0 + _Color/_RendererColor->vec4(1)\n", sh);
        fsync(2);
        r_glShaderSource(sh, 1, (const char **)&out, NULL);
        free(out); free(buf);
        return;
      }
      free(buf);
    }
  }
  r_glShaderSource(sh, count, string, length);
}
static void my_glLinkProgram(unsigned pr) {
  if (!r_glLinkProgram) r_glLinkProgram = dlsym(RTLD_DEFAULT, "glLinkProgram");
  if (!r_glGetProgramiv) r_glGetProgramiv = dlsym(RTLD_DEFAULT, "glGetProgramiv");
  r_glLinkProgram(pr);
  {
    int (*gul)(unsigned, const char *) = dlsym(RTLD_DEFAULT, "glGetUniformLocation");
    if (gul && pr < 4096 && gul(pr, "_AlphaTex") >= 0) {
      extern unsigned char g_extalpha_prog[];
      g_extalpha_prog[pr] = 1;
      fprintf(stderr, "[EXTALPHA] prog=%u marcado (variant _AlphaTex)\n", pr); fsync(2);
    }
  }
  int st = -1; if (r_glGetProgramiv) r_glGetProgramiv(pr, 0x8B82, &st); /* LINK_STATUS */
  if (st != 1 && g_prN < 100) {
    char log[768] = {0}; if (r_glGetShaderInfoLog) { void (*gpil)(unsigned,int,int*,char*) = dlsym(RTLD_DEFAULT,"glGetProgramInfoLog"); if (gpil) gpil(pr, sizeof log-1, NULL, log); }
    fprintf(stderr, "[SHADER] link FALHOU pr=%u status=%d LOG=%s\n", pr, st, log); dbg_sync(); g_prN++;
  }
}

/* ===== CUP_DRAWSPY: ring dos últimos draws p/ achar o que wedga o Utgard =====
 * O bt do wedge mostra a main presa no frame-builder lock do Mali no draw
 * SEGUINTE ao culpado (o GPU não termina o job já submetido) → registramos os
 * últimos DS_RING draws (programa/textura/FBO/count) num ring; um watchdog
 * detecta o stall (seq parado >6s) e dumpa o ring. ⚠️ sem glGetError/glFinish
 * por draw (glFinish satura o Utgard). Bisseção: CUP_SKIPFBO=1 pula draws com
 * FBO!=0 (render-to-texture); CUP_SKIPPROG=a,b,c pula programas específicos. */
static int g_drawspy = 0;       /* roteamento de gl* ligado (TEXHALF e/ou DRAWSPY) */
static int g_drawdiag = 0;      /* DIAGNÓSTICO dos DRAWS (ring + glGetIntegerv/draw) — SÓ com CUP_DRAWSPY.
                                 * ⚠️ ds_enter faz 4 glGetIntegerv POR DRAW = sync CPU↔GPU no Mali =
                                 * mata a performance. NUNCA em produção (TEXHALF sozinho NÃO liga isto). */
volatile int g_render_frame = -1;          /* setado no render loop (F2) */
static void (*ds_r_DrawElements)(unsigned, int, unsigned, const void *);
static void (*ds_r_DrawArrays)(unsigned, int, int);
static void (*ds_r_TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*ds_r_CompTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *);
static void (*ds_r_GetIntegerv)(unsigned, int *);

#define DS_RING 128
typedef struct {
  unsigned seq; int frame; unsigned char kind, in_progress; /* kind 0=elems 1=arrays */
  unsigned mode, type; int count, prog, tex, fbo, unit, texw, texh, tex0, tex1;
} ds_rec;
static ds_rec ds_ring[DS_RING];
static volatile unsigned ds_seq = 0;
static volatile unsigned ds_skipped = 0;

#define DS_MAXTEXID 32768
static unsigned short ds_tw[DS_MAXTEXID], ds_th[DS_MAXTEXID];

static int g_skipfbo = 0;
static int g_skipprog[8], g_nskipprog = 0;

static int ds_geti(unsigned pname) {
  int v = 0;
  if (!ds_r_GetIntegerv) ds_r_GetIntegerv = dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (ds_r_GetIntegerv) ds_r_GetIntegerv(pname, &v);
  return v;
}
static ds_rec *ds_enter(int kind, unsigned mode, int count, unsigned type) {
  unsigned s = __atomic_fetch_add(&ds_seq, 1, __ATOMIC_RELAXED);
  ds_rec *r = &ds_ring[s % DS_RING];
  r->in_progress = 0; r->seq = s; r->frame = g_render_frame;
  r->kind = (unsigned char)kind; r->mode = mode; r->count = count; r->type = type;
  r->prog = ds_geti(0x8B8D);          /* GL_CURRENT_PROGRAM */
  r->unit = ds_geti(0x84E0) - 0x84C0; /* GL_ACTIVE_TEXTURE - GL_TEXTURE0 */
  r->tex  = ds_geti(0x8069);          /* GL_TEXTURE_BINDING_2D */
  r->fbo  = ds_geti(0x8CA6);          /* GL_FRAMEBUFFER_BINDING */
  r->texw = (r->tex > 0 && r->tex < DS_MAXTEXID) ? ds_tw[r->tex] : 0;
  r->texh = (r->tex > 0 && r->tex < DS_MAXTEXID) ? ds_th[r->tex] : 0;
  r->tex0 = r->tex1 = 0;
  r->in_progress = 1;
  return r;
}
/* probe ARMÁVEL de estado por-draw (depth/blend/attachment do FBO + t0/t1):
 * touch /tmp/dsdump arma N draws via watchdog — pega o quadro completo EM GAMEPLAY */
static volatile int g_probe_arm = 0;
static void ds_probe_state(ds_rec *r) {
  static void (*at)(unsigned);
  static unsigned char (*ise)(unsigned);
  static void (*gfap)(unsigned, unsigned, unsigned, int *);
  if (!at) at = dlsym(RTLD_DEFAULT, "glActiveTexture");
  if (!ise) ise = dlsym(RTLD_DEFAULT, "glIsEnabled");
  if (!gfap) gfap = dlsym(RTLD_DEFAULT, "glGetFramebufferAttachmentParameteriv");
  if (!at || !ise) return;
  at(0x84C0); r->tex0 = ds_geti(0x8069);
  at(0x84C1); r->tex1 = ds_geti(0x8069);
  at(0x84C0 + (r->unit >= 0 && r->unit < 32 ? r->unit : 0));
  int dtest = ise(0x0B71), blend = ise(0x0BE2);
  int dmask = ds_geti(0x0B72), dfunc = ds_geti(0x0B74);
  int datt = -1;
  if (gfap && r->fbo != 0) gfap(0x8D40, 0x8D00, 0x8CD0, &datt);  /* FBO/DEPTH_ATT/OBJ_TYPE */
  /* colorMask (4 bools) + stencil completo — fragments podem estar sendo descartados */
  int cm[4] = {-1, -1, -1, -1};
  static void (*gbv)(unsigned, unsigned char *);
  if (!gbv) gbv = dlsym(RTLD_DEFAULT, "glGetBooleanv");
  if (gbv) { unsigned char b[4] = {9, 9, 9, 9}; gbv(0x0C23, b); cm[0] = b[0]; cm[1] = b[1]; cm[2] = b[2]; cm[3] = b[3]; }
  int stest = ise(0x0B90), sfunc = ds_geti(0x0B92), sref = ds_geti(0x0B97),
      svmask = ds_geti(0x0B93), swmask = ds_geti(0x0B98);
  int sciss = ise(0x0C11);
  int sbox[4] = {0}, vp[4] = {0};
  static void (*giv)(unsigned, int *);
  if (!giv) giv = dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (giv) { giv(0x0C10, sbox); giv(0x0BA2, vp); }
  fprintf(stderr, "[SPROBE] f=%d prog=%d fbo=%d u%d cnt=%d t0=%d t1=%d(%dx%d) depth{test=%d mask=%d func=0x%X att=0x%X} blend=%d color=%d%d%d%d sten{test=%d func=0x%X ref=%d vm=0x%X wm=0x%X} sciss=%d[%d,%d,%d,%d] vp=[%d,%d,%d,%d]\n",
          r->frame, r->prog, r->fbo, r->unit, r->count, r->tex0,
          r->tex1, r->tex1 > 0 && r->tex1 < DS_MAXTEXID ? ds_tw[r->tex1] : 0,
          r->tex1 > 0 && r->tex1 < DS_MAXTEXID ? ds_th[r->tex1] : 0,
          dtest, dmask, dfunc, datt, blend, cm[0], cm[1], cm[2], cm[3],
          stest, sfunc, sref, svmask, swmask,
          sciss, sbox[0], sbox[1], sbox[2], sbox[3], vp[0], vp[1], vp[2], vp[3]);
  fsync(2);
}
/* programas com _AlphaTex (variant sprite ext-alpha) marcados no link */
unsigned char g_extalpha_prog[4096];
static volatile int g_cur_prog;
static void (*r_glUseProgram)(unsigned);
static void my_glUseProgram(unsigned p) {
  if (!r_glUseProgram) r_glUseProgram = dlsym(RTLD_DEFAULT, "glUseProgram");
  g_cur_prog = (int)p;
  r_glUseProgram(p);
}
/* log de matrizes (translação+escala) enquanto o probe está armado */
static void (*r_glUniformMatrix4fv)(int, int, unsigned char, const float *);
static void my_glUniformMatrix4fv(int loc, int cnt, unsigned char tr, const float *m) {
  if (!r_glUniformMatrix4fv) r_glUniformMatrix4fv = dlsym(RTLD_DEFAULT, "glUniformMatrix4fv");
  if (g_probe_arm > 0 && m) {
    fprintf(stderr, "[MAT] prog=%d loc=%d n=%d diag=(%.3f %.3f %.3f %.3f) trans=(%.2f %.2f %.2f)\n",
            g_cur_prog, loc, cnt, m[0], m[5], m[10], m[15], m[12], m[13], m[14]);
    fsync(2);
  }
  r_glUniformMatrix4fv(loc, cnt, tr, m);
}
/* PROGSPY: na 1ª vez que um programa desenha, loga samplers->unit + fonte GLSL */
static void ds_progspy(int prog) {
  static unsigned char seen[256];
  if (prog <= 0 || prog >= 256 || seen[prog]) return;
  seen[prog] = 1;
  void (*gau)(unsigned, unsigned, int, int *, int *, unsigned *, char *) = dlsym(RTLD_DEFAULT, "glGetActiveUniform");
  int (*gul)(unsigned, const char *) = dlsym(RTLD_DEFAULT, "glGetUniformLocation");
  void (*guiv)(unsigned, int, int *) = dlsym(RTLD_DEFAULT, "glGetUniformiv");
  void (*gas)(unsigned, int, int *, unsigned *) = dlsym(RTLD_DEFAULT, "glGetAttachedShaders");
  void (*gss)(unsigned, int, int *, char *) = dlsym(RTLD_DEFAULT, "glGetShaderSource");
  void (*gpiv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetProgramiv");
  if (!gau || !gul || !guiv || !gas || !gss || !gpiv) return;
  int nu = 0; gpiv(prog, 0x8B86, &nu);  /* GL_ACTIVE_UNIFORMS */
  fprintf(stderr, "[PROGSPY] prog=%d uniforms=%d f=%d\n", prog, nu, g_render_frame);
  for (int i = 0; i < nu && i < 64; i++) {
    char nm[128] = {0}; int sz = 0; unsigned ty = 0;
    gau(prog, i, sizeof nm - 1, NULL, &sz, &ty, nm);
    if (ty == 0x8B5E || ty == 0x8B60) {  /* SAMPLER_2D / SAMPLER_CUBE */
      int loc = gul(prog, nm), unit = -1;
      if (loc >= 0) guiv(prog, loc, &unit);
      fprintf(stderr, "[PROGSPY]   sampler %s -> unit %d\n", nm, unit);
    }
  }
  unsigned shs[4] = {0}; int ns = 0;
  gas(prog, 4, &ns, shs);
  for (int i = 0; i < ns; i++) {
    static char src[4096]; int len = 0; src[0] = 0;
    gss(shs[i], sizeof src - 1, &len, src);
    fprintf(stderr, "[PROGSPY] prog=%d shader[%d] len=%d SRC:\n%.2400s\n[PROGSPY] ---fim---\n", prog, i, len, src);
  }
  fsync(2);
}
static int ds_skip(const ds_rec *r) {
  if (g_skipfbo && r->fbo != 0) return 1;
  for (int i = 0; i < g_nskipprog; i++) if (r->prog == g_skipprog[i]) return 1;
  return 0;
}
volatile unsigned long g_frame_draws, g_frame_verts, g_draws_lo;   /* CUP_DRAWCOUNT: carga de desenho/frame */
extern int rs_logical0(void); extern int rs_enabled(void);
static void ds_draw_noscissor(void (*draw)(unsigned, int, unsigned, const void *),
                              unsigned mode, int count, unsigned type, const void *idx) {
  static void (*dis)(unsigned), (*ena)(unsigned);
  static unsigned char (*ise2)(unsigned);
  if (!dis) { dis = dlsym(RTLD_DEFAULT, "glDisable"); ena = dlsym(RTLD_DEFAULT, "glEnable"); ise2 = dlsym(RTLD_DEFAULT, "glIsEnabled"); }
  int was = ise2 ? ise2(0x0C11) : 0;
  if (was && dis) dis(0x0C11);
  draw(mode, count, type, idx);
  if (was && ena) ena(0x0C11);
}
static int g_noscissor = -1;
static void my_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
  g_frame_draws++; g_frame_verts += (unsigned)count;
  if (rs_enabled() && rs_logical0()) g_draws_lo++;
  if (g_noscissor < 0) g_noscissor = getenv("CUP_NOSCISSOR") ? 1 : 0;
  if (!g_drawdiag) {  /* fast path: SEM glGetIntegerv (NOSCISSOR só olha o prog atual) */
    if (g_noscissor && g_cur_prog > 0 && g_cur_prog < 4096 && g_extalpha_prog[g_cur_prog]) {
      ds_draw_noscissor(ds_r_DrawElements, mode, count, type, idx);
      return;
    }
    ds_r_DrawElements(mode, count, type, idx);
    return;
  }
  ds_rec *r = ds_enter(0, mode, count, type);
  ds_progspy(r->prog);
  if (g_probe_arm > 0) { g_probe_arm--; ds_probe_state(r); }
  if (r->seq < 8) fprintf(stderr, "[DS] draw#%u f=%d ELM cnt=%d prog=%d fbo=%d tex=%d(%dx%d)\n",
                          r->seq, r->frame, count, r->prog, r->fbo, r->tex, r->texw, r->texh);
  if (ds_skip(r)) { r->in_progress = 0; ds_skipped++; return; }
  /* CUP_NOSCISSOR: testa se o scissor está cortando os sprites ext-alpha */
  if (g_noscissor && r->prog > 0 && r->prog < 4096 && g_extalpha_prog[r->prog]) {
    ds_draw_noscissor(ds_r_DrawElements, mode, count, type, idx);
    r->in_progress = 0;
    return;
  }
  ds_r_DrawElements(mode, count, type, idx);
  r->in_progress = 0;
}
/* CUP_DRAWCOUNT: conta glClear + loga a clear-color (diag de "Draw roda mas 0 draws") */
volatile unsigned long g_clear_count;
static void (*ds_r_Clear)(unsigned);
static void (*ds_r_ClearColor)(float, float, float, float);
static void my_glClear(unsigned mask) {
  g_clear_count++;
  if (ds_r_Clear) ds_r_Clear(mask);
}
static void my_glClearColor(float r, float g, float b, float a) {
  static int n = 0; if (n++ < 8) { fprintf(stderr, "[CLEARCOL] %.3f %.3f %.3f %.3f\n", r, g, b, a); fsync(2); }
  if (ds_r_ClearColor) ds_r_ClearColor(r, g, b, a);
}
static void my_glDrawArrays(unsigned mode, int first, int count) {
  g_frame_draws++; g_frame_verts += (unsigned)count;
  if (!g_drawdiag) { ds_r_DrawArrays(mode, first, count); return; }  /* fast path */
  ds_rec *r = ds_enter(1, mode, count, 0);
  if (ds_skip(r)) { r->in_progress = 0; ds_skipped++; return; }
  ds_r_DrawArrays(mode, first, count);
  r->in_progress = 0;
}
static void ds_rectex(int w, int h, const char *what) {
  int t = ds_geti(0x8069);
  if (t > 0 && t < DS_MAXTEXID) {
    if ((unsigned short)w > ds_tw[t]) ds_tw[t] = (unsigned short)w;
    if ((unsigned short)h > ds_th[t]) ds_th[t] = (unsigned short)h;
  }
  if (w >= 1024 || h >= 1024) { fprintf(stderr, "[DS] BIG TEX %s id=%d %dx%d f=%d\n", what, t, w, h, g_render_frame); fsync(2); }
}
/* CUP_TEXHALF=N: downscale (nearest) das texturas nivel-0 não-comprimidas até
 * caberem no teto N (max(w,h) <= N). N=512 → 2048 vira 512 (1/16 da RAM/VRAM!).
 * Reduz drasticamente p/ o load de assets persistentes caber em 832MB + evita o
 * limite do Utgard. Receita Bully/Castlevania (agressiva). px!=NULL só. */
static int g_texhalf = 0;
static int gl_bpp(unsigned fmt, unsigned type) {
  switch (type) {
    case 0x8033: case 0x8034: case 0x8363: return 2;  /* 4444 / 5551 / 565 */
    case 0x1401:                                       /* UNSIGNED_BYTE */
      switch (fmt) { case 0x1908: return 4;            /* RGBA */
                     case 0x1907: return 3;            /* RGB */
                     case 0x190A: return 2;            /* LUMINANCE_ALPHA */
                     case 0x1909: case 0x1906: return 1; } /* LUMINANCE / ALPHA */
  }
  return 0;  /* desconhecido -> não mexe */
}
static unsigned char ds_shift[DS_MAXTEXID];  /* fator de downscale (log2) por tex id */
static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int b, unsigned fmt, unsigned type, const void *px) {
  if (lvl == 0 && tgt == 0x0DE1) ds_rectex(w, h, "tex");
  if (g_drawdiag && lvl == 0 && tgt == 0x0DE1 && w >= 256) {
    fprintf(stderr, "[TEXFMT] id=%d %dx%d ifmt=0x%X fmt=0x%X type=0x%X px=%c f=%d\n",
            ds_geti(0x8069), w, h, ifmt, fmt, type, px ? 'Y' : 'N', g_render_frame); fsync(2);
  }
  /* CUP_TEXSTAT: o canal alpha dos atlases grandes é real ou zerado? */
  if (getenv("CUP_TEXSTAT") && lvl == 0 && tgt == 0x0DE1 && w >= 1024 && px && fmt == 0x1908 && type == 0x1401) {
    const unsigned char *q = px;
    size_t n = (size_t)w * h, z = 0, ff = 0;
    for (size_t i = 0; i < n; i += 7) {            /* amostra 1/7 dos pixels */
      unsigned char a = q[i * 4 + 3];
      if (a == 0) z++; else if (a == 255) ff++;
    }
    size_t s = (n + 6) / 7;
    fprintf(stderr, "[TEXSTAT] id=%d %dx%d alpha: zero=%zu%% cheio=%zu%% (amostra %zu) f=%d\n",
            ds_geti(0x8069), w, h, z * 100 / s, ff * 100 / s, s, g_render_frame); fsync(2);
  }
  int tid = (g_texhalf && tgt == 0x0DE1) ? ds_geti(0x8069) : 0;
  int shift = 0;
  if (g_texhalf && tgt == 0x0DE1 && px && gl_bpp(fmt, type) > 0) {
    if (lvl == 0) {
      int mw = w, mh = h;
      while ((mw > g_texhalf || mh > g_texhalf) && mw > 1 && mh > 1) { mw >>= 1; mh >>= 1; shift++; }
      if (tid > 0 && tid < DS_MAXTEXID) ds_shift[tid] = (unsigned char)shift;
    } else if (tid > 0 && tid < DS_MAXTEXID) {
      shift = ds_shift[tid];                          /* mesma chain do nivel-0 */
      while (shift > 0 && ((w >> shift) < 1 || (h >> shift) < 1)) shift--;
    }
  }
  if (shift > 0) {
    int bpp = gl_bpp(fmt, type);
    int nw = w >> shift, nh = h >> shift, st = 1 << shift;
    unsigned char *dst = malloc((size_t)nw * nh * bpp);
    if (dst) {
      const unsigned char *src = px;
      for (int y = 0; y < nh; y++) {
        const unsigned char *srow = src + (size_t)(y * st) * w * bpp;
        unsigned char *drow = dst + (size_t)y * nw * bpp;
        for (int x = 0; x < nw; x++)
          memcpy(drow + x * bpp, srow + (size_t)(x * st) * bpp, bpp);
      }
      static int n; if (n++ < 40) { fprintf(stderr, "[TEXHALF] tex=%d %dx%d -> %dx%d (/%d lvl%d)\n", tid, w, h, nw, nh, st, lvl); fsync(2); }
      ds_r_TexImage2D(tgt, lvl, ifmt, nw, nh, b, fmt, type, dst);
      free(dst);
      return;
    }
  }
  ds_r_TexImage2D(tgt, lvl, ifmt, w, h, b, fmt, type, px);
}
static void my_glCompTexImage2D(unsigned tgt, int lvl, unsigned ifmt, int w, int h, int b, int sz, const void *px) {
  if (lvl == 0 && tgt == 0x0DE1) ds_rectex(w, h, "ctex");
  ds_r_CompTexImage2D(tgt, lvl, ifmt, w, h, b, sz, px);
}
/* ---- renderbuffer/FBO: log + retry de formato de DEPTH não suportado ----
 * Hipótese chefes-invisíveis: Unity pede DEPTH_COMPONENT24/32 (ext OES) que o blob
 * Utgard antigo não tem -> glRenderbufferStorage falha SILENCIOSO -> FBO da cena sem
 * depth -> passe opaco front-to-back sem oclusão -> céu (desenhado depois) soterra
 * os sprites. Retry com DEPTH_COMPONENT16 (sempre suportado em GLES2). */
static void (*r_glRenderbufferStorage)(unsigned, unsigned, int, int);
static unsigned (*r_glGetError2)(void);
static unsigned (*r_glCheckFBStatus)(unsigned);
static void my_glRenderbufferStorage(unsigned tgt, unsigned ifmt, int w, int h) {
  if (!r_glRenderbufferStorage) r_glRenderbufferStorage = dlsym(RTLD_DEFAULT, "glRenderbufferStorage");
  if (!r_glGetError2) r_glGetError2 = dlsym(RTLD_DEFAULT, "glGetError");
  if (r_glGetError2) r_glGetError2();                     /* limpa erro pendente */
  r_glRenderbufferStorage(tgt, ifmt, w, h);
  unsigned err = r_glGetError2 ? r_glGetError2() : 0;
  if (g_drawspy || err) /* fora do spy: só erro (ds_geti faz glGet* = stall Mali) */
    fprintf(stderr, "[RBSTOR] rb=%d ifmt=0x%X %dx%d err=0x%X\n", ds_geti(0x8CA7), ifmt, w, h, err);
  if (err) {
    unsigned fb = 0;
    if (ifmt == 0x81A6 || ifmt == 0x81A7) fb = 0x81A5;          /* DEPTH24/32 -> DEPTH16 */
    else if (ifmt == 0x88F0) fb = 0x81A5;                       /* D24S8 -> DEPTH16 (sem stencil) */
    if (fb) {
      r_glRenderbufferStorage(tgt, fb, w, h);
      unsigned e2 = r_glGetError2 ? r_glGetError2() : 0;
      fprintf(stderr, "[RBSTOR] retry ifmt=0x%X -> err=0x%X %s\n", fb, e2, e2 ? "FALHOU" : "OK");
    }
    fsync(2);
  }
}
/* wiring dos FBOs: qual textura/RB em cada attachment (mapa cena->composição) */
static void (*r_glFBTex2D)(unsigned, unsigned, unsigned, unsigned, int);
static void my_glFramebufferTexture2D(unsigned tgt, unsigned att, unsigned textgt, unsigned tex, int lvl) {
  if (!r_glFBTex2D) r_glFBTex2D = dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
  fprintf(stderr, "[FBWIRE] fbo=%d att=0x%X tex=%u(%dx%d)\n", ds_geti(0x8CA6), att, tex,
          tex > 0 && tex < DS_MAXTEXID ? ds_tw[tex] : 0, tex > 0 && tex < DS_MAXTEXID ? ds_th[tex] : 0);
  fsync(2);
  r_glFBTex2D(tgt, att, textgt, tex, lvl);
}
static void (*r_glFBRb)(unsigned, unsigned, unsigned, unsigned);
static void my_glFramebufferRenderbuffer(unsigned tgt, unsigned att, unsigned rbtgt, unsigned rb) {
  if (!r_glFBRb) r_glFBRb = dlsym(RTLD_DEFAULT, "glFramebufferRenderbuffer");
  fprintf(stderr, "[FBWIRE] fbo=%d att=0x%X rb=%u\n", ds_geti(0x8CA6), att, rb); fsync(2);
  r_glFBRb(tgt, att, rbtgt, rb);
}
static unsigned my_glCheckFramebufferStatus(unsigned tgt) {
  if (!r_glCheckFBStatus) r_glCheckFBStatus = dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
  unsigned st = r_glCheckFBStatus ? r_glCheckFBStatus(tgt) : 0;
  if (st != 0x8CD5) { fprintf(stderr, "[FBSTAT] fbo=%d status=0x%X (INCOMPLETO)\n", ds_geti(0x8CA6), st); fsync(2); }
  return st;
}
static void ds_dump(void) {
  unsigned end = ds_seq;
  unsigned n = end < DS_RING ? end : DS_RING;
  fprintf(stderr, "[DS] ===== DUMP ring (seq=%u frame=%d skipped=%u) =====\n", end, g_render_frame, ds_skipped);
  for (unsigned i = end - n; i != end; i++) {
    ds_rec *r = &ds_ring[i % DS_RING];
    if (r->seq != i) continue;  /* slot já sobrescrito (race benigna) */
    fprintf(stderr, "[DS] #%u f=%d %s mode=%u cnt=%d prog=%d fbo=%d u%d tex=%d(%dx%d) t0=%d t1=%d%s\n",
            r->seq, r->frame, r->kind ? "ARR" : "ELM", r->mode, r->count,
            r->prog, r->fbo, r->unit, r->tex, r->texw, r->texh, r->tex0, r->tex1,
            r->in_progress ? "  <== IN-PROGRESS (bloqueado no driver)" : "");
  }
  fsync(2);
}
static void *ds_watchdog(void *a) {
  (void)a;
  unsigned last = 0; int still = 0, dumped = 0, beat = 0;
  for (;;) {
    sleep(2);
    unsigned s = ds_seq;
    if (++beat % 30 == 0) { fprintf(stderr, "[DS] alive seq=%u f=%d skipped=%u\n", s, g_render_frame, ds_skipped); fsync(2); }
    if (access("/tmp/dsdump", F_OK) == 0) { unlink("/tmp/dsdump"); ds_dump(); g_probe_arm = 60; }
    /* liga/desliga o diag PESADO em runtime (boot/nav leves, diag só na fase) */
    if (access("/tmp/dson", F_OK) == 0) { unlink("/tmp/dson"); g_drawdiag = 1; fprintf(stderr, "[DS] drawdiag ON (runtime)\n"); fsync(2); }
    if (access("/tmp/dsoff", F_OK) == 0) { unlink("/tmp/dsoff"); g_drawdiag = 0; fprintf(stderr, "[DS] drawdiag OFF (runtime)\n"); fsync(2); }
    if (s != last) { last = s; still = 0; dumped = 0; continue; }
    if (s == 0) continue;
    if (++still >= 3 && !dumped) {
      fprintf(stderr, "[DS] STALL: draws parados ha %ds (seq=%u)\n", still * 2, s);
      ds_dump(); dumped = 1;
    }
  }
  return NULL;
}
/* render-scale (renderscale.c): redireciona a tela p/ um FBO lo-res + upscale */
extern void rs_init(void); extern int rs_enabled(void);
extern void rs_BindFramebuffer(unsigned, unsigned);
extern void rs_Viewport(int, int, int, int);
extern void rs_Scissor(int, int, int, int);
extern void rs_present(void);
static unsigned (*r_eglSwapBuffers)(void *, void *);
/* Internal-only screenshot diagnostic. Public builds compile it out. */
static void ter_screenshot_maybe(void) {
#ifdef TER_PUBLIC_BUILD
  return;
#else
  static long n = 0; n++;
  const char *s = getenv("TER_SHOT");
  /* TER_SHOTLIVE: captura SOB DEMANDA — quando /tmp/tershot existir, grava shot.ppm e remove o
     gatilho. Permite tirar print a qualquer momento navegando o menu por ssh (depurar Settings). */
  int live = getenv("TER_SHOTLIVE") && access("/tmp/tershot", F_OK) == 0;
  if (live) unlink("/tmp/tershot");
  if (!s && !live) return;
  if (s && !live) { long target = atoi(s); if (target <= 0) target = 1; if (n != target) return; }
  static int (*p_gi)(unsigned, int*); static void (*p_rp)(int,int,int,int,unsigned,unsigned,void*);
  if (!p_gi) p_gi = (void*)dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (!p_rp) p_rp = (void*)dlsym(RTLD_DEFAULT, "glReadPixels");
  if (!p_gi || !p_rp) { fprintf(stderr, "[SHOT] sem glGetIntegerv/glReadPixels\n"); return; }
  int vp[4] = {0,0,0,0}; p_gi(0x0BA2 /*GL_VIEWPORT*/, vp);
  int w = vp[2], h = vp[3]; if (w <= 0 || h <= 0) { w = g_fbdev_win.w; h = g_fbdev_win.h; }
  if (w <= 0 || h <= 0) { fprintf(stderr, "[SHOT] viewport 0\n"); return; }
  unsigned char *buf = malloc((size_t)w*h*4); if (!buf) return;
  p_rp(0,0,w,h,0x1908/*GL_RGBA*/,0x1401/*GL_UNSIGNED_BYTE*/,buf);
  char shot_path[4096];
  FILE *f = ter_game_path(shot_path, sizeof(shot_path), "shot.ppm") == 0
              ? fopen(shot_path, "wb") : NULL;
  if (f) { fprintf(f,"P6\n%d %d\n255\n", w, h);
    for (int y=h-1;y>=0;y--) for (int x=0;x<w;x++){ unsigned char*p=buf+((size_t)y*w+x)*4; fwrite(p,1,3,f);}
    fclose(f);
    /* conta pixels não-pretos + draws acumulados p/ diagnóstico de tela preta */
    long nb = 0; for (size_t i = 0; i < (size_t)w*h; i++) if (buf[i*4]+buf[i*4+1]+buf[i*4+2] > 24) nb++;
    extern volatile unsigned long g_frame_draws, g_frame_verts, g_clear_count;
    fprintf(stderr,"[SHOT] gravado shot.ppm %dx%d (swap #%ld) nao-pretos=%ld/%d draws_acum=%lu verts=%lu clears=%lu\n",
            w,h,n, nb, w*h, g_frame_draws, g_frame_verts, g_clear_count); }
  free(buf);
#endif
}
static void ter_nuke_methods(void);
static void ter_jobworkers0(void);
void ter_before_present(void);

/* native_pad.c: estado do pad (0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start 8=L3 9=R3 10..13=dpad U D L R) */
extern int np_btn(int b), np_btn_down(int b);
static int g_vkbd_swallow, g_vkbd_sel, g_vkbd_force_frames;
static int g_vkbd_uppercase, g_vkbd_latch;
static char g_vkbd_force_text[128];
static volatile int g_vkbd_commit_pending;
static char g_vkbd_commit_text[128];
enum {
  VK_CTX_NONE,
  VK_CTX_PLAYER_FIELD,
  VK_CTX_WORLD_FIELD,
  VK_CTX_PLAYER_SCREEN,
  VK_CTX_WORLD_SCREEN,
};
static volatile int g_vkbd_context;
static void ter_vkbd_update(void);
static void ter_vkbd_draw(void);
static void ter_vkbd_maybe_open(void);
static int ter_vkbd_finish_pending(void);
static void ter_name_hooks_install(void);
static void ter_name_force_text(const char *text);
static void ter_name_commit_text(const char *text);
static void ter_force_main_player_name(const char *text);
static void ter_player_name_menu_force_text(const char *text);
static const char *ter_vkbd_effective_name(const char *fallback);
int ter_vkbd_blocking(void) {
  if (getenv("TER_NOVKBD")) return 0;
  return jni_softinput_active() || g_vkbd_swallow > 0;
}
enum {
  VK_CHARACTER,
  VK_BACKSPACE,
  VK_SHIFT,
  VK_SPACE,
  VK_DONE,
};
struct vk_key {
  int x, y, w, h;          /* Prizefighters 2 design space: 1280x720 */
  const char *label;
  char lower, upper;
  int action;
};
/* QWERTY layout and interaction model ported from the approved
   Prizefighters 2 controller keyboard. */
static const struct vk_key vk_keys[] = {
  { 171,382, 86,52,"Q",'q','Q',VK_CHARACTER },
  { 265,382, 86,52,"W",'w','W',VK_CHARACTER },
  { 359,382, 86,52,"E",'e','E',VK_CHARACTER },
  { 453,382, 86,52,"R",'r','R',VK_CHARACTER },
  { 547,382, 86,52,"T",'t','T',VK_CHARACTER },
  { 641,382, 86,52,"Y",'y','Y',VK_CHARACTER },
  { 735,382, 86,52,"U",'u','U',VK_CHARACTER },
  { 829,382, 86,52,"I",'i','I',VK_CHARACTER },
  { 923,382, 86,52,"O",'o','O',VK_CHARACTER },
  {1017,382, 86,52,"P",'p','P',VK_CHARACTER },
  { 218,444, 86,52,"A",'a','A',VK_CHARACTER },
  { 312,444, 86,52,"S",'s','S',VK_CHARACTER },
  { 406,444, 86,52,"D",'d','D',VK_CHARACTER },
  { 500,444, 86,52,"F",'f','F',VK_CHARACTER },
  { 594,444, 86,52,"G",'g','G',VK_CHARACTER },
  { 688,444, 86,52,"H",'h','H',VK_CHARACTER },
  { 782,444, 86,52,"J",'j','J',VK_CHARACTER },
  { 876,444, 86,52,"K",'k','K',VK_CHARACTER },
  { 970,444, 86,52,"L",'l','L',VK_CHARACTER },
  { 265,506, 86,52,"Z",'z','Z',VK_CHARACTER },
  { 359,506, 86,52,"X",'x','X',VK_CHARACTER },
  { 453,506, 86,52,"C",'c','C',VK_CHARACTER },
  { 547,506, 86,52,"V",'v','V',VK_CHARACTER },
  { 641,506, 86,52,"B",'b','B',VK_CHARACTER },
  { 735,506, 86,52,"N",'n','N',VK_CHARACTER },
  { 829,506, 86,52,"M",'m','M',VK_CHARACTER },
  { 923,506,187,52,"DEL",0,0,VK_BACKSPACE },
  { 171,568,188,58,"SHIFT",0,0,VK_SHIFT },
  { 371,568,458,58,"SPACE",0,0,VK_SPACE },
  { 841,568,269,58,"DONE",0,0,VK_DONE },
};
#define VK_NKEYS ((int)(sizeof(vk_keys) / sizeof(vk_keys[0])))

static int vk_glyph(char c, unsigned char r[7]) {
  memset(r, 0, 7);
  #define VG(a,b,c,d,e,f,g) do { unsigned char v[7]={a,b,c,d,e,f,g}; memcpy(r,v,7); return 1; } while(0)
  switch (c) {
    case 'A': VG(14,17,17,31,17,17,17); case 'B': VG(30,17,17,30,17,17,30);
    case 'C': VG(14,17,16,16,16,17,14); case 'D': VG(30,17,17,17,17,17,30);
    case 'E': VG(31,16,16,30,16,16,31); case 'F': VG(31,16,16,30,16,16,16);
    case 'G': VG(14,17,16,23,17,17,14); case 'H': VG(17,17,17,31,17,17,17);
    case 'I': VG(14,4,4,4,4,4,14);      case 'J': VG(7,2,2,2,18,18,12);
    case 'K': VG(17,18,20,24,20,18,17); case 'L': VG(16,16,16,16,16,16,31);
    case 'M': VG(17,27,21,21,17,17,17); case 'N': VG(17,25,21,19,17,17,17);
    case 'O': VG(14,17,17,17,17,17,14); case 'P': VG(30,17,17,30,16,16,16);
    case 'Q': VG(14,17,17,17,21,18,13); case 'R': VG(30,17,17,30,20,18,17);
    case 'S': VG(15,16,16,14,1,1,30);   case 'T': VG(31,4,4,4,4,4,4);
    case 'U': VG(17,17,17,17,17,17,14); case 'V': VG(17,17,17,17,17,10,4);
    case 'W': VG(17,17,17,21,21,21,10); case 'X': VG(17,17,10,4,10,17,17);
    case 'Y': VG(17,17,10,4,4,4,4);     case 'Z': VG(31,1,2,4,8,16,31);
    case 'a': VG(0,0,14,1,15,17,15);    case 'b': VG(16,16,30,17,17,17,30);
    case 'c': VG(0,0,14,17,16,17,14);   case 'd': VG(1,1,15,17,17,17,15);
    case 'e': VG(0,0,14,17,31,16,14);   case 'f': VG(6,9,8,28,8,8,8);
    case 'g': VG(0,0,15,17,15,1,14);    case 'h': VG(16,16,30,17,17,17,17);
    case 'i': VG(4,0,12,4,4,4,14);      case 'j': VG(2,0,6,2,2,18,12);
    case 'k': VG(16,16,18,20,24,20,18); case 'l': VG(12,4,4,4,4,4,14);
    case 'm': VG(0,0,26,21,21,17,17);   case 'n': VG(0,0,30,17,17,17,17);
    case 'o': VG(0,0,14,17,17,17,14);   case 'p': VG(0,0,30,17,30,16,16);
    case 'q': VG(0,0,15,17,15,1,1);     case 'r': VG(0,0,22,25,16,16,16);
    case 's': VG(0,0,15,16,14,1,30);    case 't': VG(8,8,28,8,8,9,6);
    case 'u': VG(0,0,17,17,17,19,13);   case 'v': VG(0,0,17,17,17,10,4);
    case 'w': VG(0,0,17,17,21,21,10);   case 'x': VG(0,0,17,10,4,10,17);
    case 'y': VG(0,0,17,17,15,1,14);    case 'z': VG(0,0,31,2,4,8,31);
    case '0': VG(14,17,19,21,25,17,14); case '1': VG(4,12,4,4,4,4,14);
    case '2': VG(14,17,1,2,4,8,31);     case '3': VG(30,1,1,14,1,1,30);
    case '4': VG(2,6,10,18,31,2,2);     case '5': VG(31,16,16,30,1,1,30);
    case '6': VG(14,16,16,30,17,17,14); case '7': VG(31,1,2,4,8,8,8);
    case '8': VG(14,17,17,14,17,17,14); case '9': VG(14,17,17,15,1,1,14);
    case '-': VG(0,0,0,31,0,0,0);       case '_': VG(0,0,0,0,0,0,31);
    case ':': VG(0,4,4,0,4,4,0);        case ' ': return 1;
  }
  return 0;
  #undef VG
}

static int vk_gl_begin(int *sw, int *sh, int old_scissor[4], float old_clear[4],
                       unsigned char old_mask[4], int *old_enabled) {
  static void (*p_en)(unsigned), (*p_dis)(unsigned), (*p_sc)(int,int,int,int);
  static void (*p_cc)(float,float,float,float), (*p_cm)(unsigned char,unsigned char,unsigned char,unsigned char);
  static void (*p_cl)(unsigned), (*p_gb)(unsigned,unsigned char*);
  static void (*p_gi)(unsigned,int*), (*p_gf)(unsigned,float*);
  static unsigned char (*p_ie)(unsigned);
  if (!p_en) {
    p_en=dlsym(RTLD_DEFAULT,"glEnable"); p_dis=dlsym(RTLD_DEFAULT,"glDisable");
    p_sc=dlsym(RTLD_DEFAULT,"glScissor"); p_cc=dlsym(RTLD_DEFAULT,"glClearColor");
    p_cm=dlsym(RTLD_DEFAULT,"glColorMask"); p_gb=dlsym(RTLD_DEFAULT,"glGetBooleanv");
    p_cl=dlsym(RTLD_DEFAULT,"glClear"); p_gi=dlsym(RTLD_DEFAULT,"glGetIntegerv");
    p_gf=dlsym(RTLD_DEFAULT,"glGetFloatv"); p_ie=dlsym(RTLD_DEFAULT,"glIsEnabled");
  }
  if (!p_en || !p_dis || !p_sc || !p_cc || !p_cm || !p_cl || !p_gi ||
      !p_gf || !p_gb || !p_ie) return 0;
  int framebuffer = 0, viewport[4] = {0,0,0,0};
  p_gi(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &framebuffer);
  if (framebuffer != 0) return 0;
  p_gi(0x0BA2 /* GL_VIEWPORT */, viewport);
  *sw = viewport[2] > 0 ? viewport[2] : ter_window_w();
  *sh = viewport[3] > 0 ? viewport[3] : ter_window_h();
  if (*sw <= 0 || *sh <= 0) return 0;
  p_gi(0x0C10 /* GL_SCISSOR_BOX */, old_scissor);
  p_gf(0x0C22 /* GL_COLOR_CLEAR_VALUE */, old_clear);
  p_gb(0x0C23 /* GL_COLOR_WRITEMASK */, old_mask);
  *old_enabled = p_ie(0x0C11 /* GL_SCISSOR_TEST */);
  p_en(0x0C11);
  p_cm(1,1,1,1);
  return 1;
}
static void vk_rect(int sw, int sh, int x, int y, int w, int h, float r, float g, float b, float a) {
  static void (*p_sc)(int,int,int,int), (*p_cc)(float,float,float,float), (*p_cl)(unsigned);
  if (!p_sc) { p_sc=dlsym(RTLD_DEFAULT,"glScissor"); p_cc=dlsym(RTLD_DEFAULT,"glClearColor"); p_cl=dlsym(RTLD_DEFAULT,"glClear"); }
  if (!p_sc || !p_cc || !p_cl) return;
  if (w <= 0 || h <= 0 || x >= sw || y >= sh || x + w <= 0 || y + h <= 0) return;
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x; if (y + h > sh) h = sh - y;
  p_sc(x, sh - y - h, w, h); p_cc(r,g,b,a); p_cl(0x00004000);
}
static void vk_gl_end(const int old_scissor[4], const float old_clear[4],
                      const unsigned char old_mask[4], int old_enabled) {
  void (*p_sc)(int,int,int,int)=dlsym(RTLD_DEFAULT,"glScissor");
  void (*p_cc)(float,float,float,float)=dlsym(RTLD_DEFAULT,"glClearColor");
  void (*p_cm)(unsigned char,unsigned char,unsigned char,unsigned char)=dlsym(RTLD_DEFAULT,"glColorMask");
  void (*p_en)(unsigned)=dlsym(RTLD_DEFAULT,"glEnable");
  void (*p_dis)(unsigned)=dlsym(RTLD_DEFAULT,"glDisable");
  if (p_sc) p_sc(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
  if (p_cc) p_cc(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  if (p_cm) p_cm(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
  if (old_enabled) { if (p_en) p_en(0x0C11); } else { if (p_dis) p_dis(0x0C11); }
}
static void vk_design_rect(int sw, int sh, int x, int y, int w, int h,
                           float r, float g, float b, float a) {
  int left = x * sw / 1280, right = (x + w) * sw / 1280;
  int top = y * sh / 720, bottom = (y + h) * sh / 720;
  vk_rect(sw, sh, left, top, right-left, bottom-top, r,g,b,a);
}
static void vk_text(int sw, int sh, int x, int y, const char *s, int scale, float r, float g, float b) {
  for (int n=0; s && s[n]; n++) {
    unsigned char rows[7];
    if (!vk_glyph(s[n], rows)) continue;
    for (int yy=0; yy<7; yy++) for (int xx=0; xx<5;) {
      while (xx < 5 && !(rows[yy] & (1 << (4-xx)))) xx++;
      int start = xx;
      while (xx < 5 && (rows[yy] & (1 << (4-xx)))) xx++;
      if (start < xx)
        vk_design_rect(sw, sh, x+n*6*scale+start*scale, y+yy*scale,
                       (xx-start)*scale, scale, r,g,b,1.0f);
    }
  }
}
static int vk_text_width(const char *text, int scale) {
  return text ? (int)strlen(text) * 6 * scale : 0;
}
static void vk_append_char(char c) {
  char buf[128];
  snprintf(buf, sizeof buf, "%s", jni_softinput_text());
  size_t n = strlen(buf), lim = (size_t)jni_softinput_limit();
  if (lim >= sizeof buf) lim = sizeof buf - 1;
  if (n < lim) {
    buf[n] = c; buf[n+1] = 0;
    jni_softinput_set_text(buf);
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", buf);
    g_vkbd_uppercase = c == ' ';
  }
}
static void vk_backspace(void) {
  char buf[128]; snprintf(buf, sizeof buf, "%s", jni_softinput_text());
  size_t n = strlen(buf); if (n > 0) {
    buf[n-1] = 0;
    jni_softinput_set_text(buf);
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", buf);
    n--;
    g_vkbd_uppercase = n == 0 || buf[n-1] == ' ';
  }
}
static void vk_commit_text(void) {
  if (g_vkbd_commit_pending) return;
  char text[128];
  snprintf(text, sizeof text, "%s", jni_softinput_text());
  if (!text[0]) {
    const char *fallback = getenv("TER_VK_DEFAULT");
    snprintf(text, sizeof text, "%s", fallback && *fallback ? fallback : "Player");
  }
  snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", text);
  snprintf(g_vkbd_commit_text, sizeof g_vkbd_commit_text, "%s", text);
  g_vkbd_force_frames = 180;
  g_vkbd_commit_pending = 1;
  fprintf(stderr, "[VKBD] DONE agendado na thread do jogo: \"%s\"\n", text);
  fsync(2);
}
static void vk_move_selection(int dx, int dy) {
  const struct vk_key *current = &vk_keys[g_vkbd_sel];
  int current_x = current->x + current->w/2;
  int current_y = current->y + current->h/2;
  int best = -1, best_score = INT32_MAX;
  for (int i=0; i<VK_NKEYS; i++) {
    if (i == g_vkbd_sel) continue;
    const struct vk_key *candidate = &vk_keys[i];
    int candidate_x = candidate->x + candidate->w/2;
    int candidate_y = candidate->y + candidate->h/2;
    int along = dx ? (candidate_x-current_x)*dx : (candidate_y-current_y)*dy;
    if (along <= 0) continue;
    int across = dx ? abs(candidate_y-current_y) : abs(candidate_x-current_x);
    int score = dx ? along + across*8 : along*4 + across;
    if (score < best_score) { best_score = score; best = i; }
  }
  if (best >= 0) g_vkbd_sel = best;
}
static void vk_activate_key(int index) {
  if (index < 0 || index >= VK_NKEYS) return;
  const struct vk_key *key = &vk_keys[index];
  switch (key->action) {
    case VK_CHARACTER:
      vk_append_char(g_vkbd_uppercase ? key->upper : key->lower);
      break;
    case VK_BACKSPACE: vk_backspace(); break;
    case VK_SHIFT: g_vkbd_uppercase = !g_vkbd_uppercase; break;
    case VK_SPACE: vk_append_char(' '); break;
    case VK_DONE: vk_commit_text(); break;
  }
}
static void ter_vkbd_maybe_open(void) {
  if (getenv("TER_NOVKBD")) return;
  if (!getenv("TER_OSK") || jni_softinput_active()) return;
  if (access("/tmp/tervkbd", F_OK) == 0) {
    unlink("/tmp/tervkbd");
    jni_softinput_open(getenv("TER_VK_TEXT") ? getenv("TER_VK_TEXT") : "", 32);
    return;
  }
  if ((np_btn_down(3) && np_btn(9)) || (np_btn_down(9) && np_btn(3))) {   /* Y+R3 */
    jni_softinput_open("", 32);
  }
}
static void ter_vkbd_update(void) {
  if (getenv("TER_NOVKBD")) return;
  static int rep, was, auto_frames;
  if (!jni_softinput_active()) { was = 0; return; }
  if (!was) {
    was = 1; g_vkbd_sel = 0; rep = 0; auto_frames = 0;
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", jni_softinput_text());
    size_t length = strlen(g_vkbd_force_text);
    g_vkbd_uppercase = length == 0 || g_vkbd_force_text[length-1] == ' ';
    g_vkbd_latch = 1;
    fprintf(stderr, "[VKBD] teclado QWERTY ON (A=seleciona B=apaga X=shift Start=conclui Select=cancela)\n"); fsync(2);
  }
  if (g_vkbd_latch) {
    if (!np_btn(0) && !np_btn(1) && !np_btn(2) && !np_btn(6) && !np_btn(7) &&
        !np_btn(9) && !np_btn(10) && !np_btn(11) && !np_btn(12) && !np_btn(13))
      g_vkbd_latch = 0;
    return;
  }
  if (getenv("TER_VK_AUTOTEXT")) {
    const char *t = getenv("TER_VK_AUTOTEXT");
    if (auto_frames == 5) jni_softinput_set_text(t && *t ? t : "PLAYER");
    if (auto_frames++ == 25) { vk_commit_text(); return; }
  }
  int dx = np_btn(13) ? 1 : (np_btn(12) ? -1 : 0);   /* dpad R/L */
  int dy = np_btn(11) ? 1 : (np_btn(10) ? -1 : 0);   /* dpad D/U */
  int edge_move = (np_btn_down(10) || np_btn_down(11) || np_btn_down(12) || np_btn_down(13));
  if ((dx || dy) && (edge_move || rep <= 0)) {
    if (dx) vk_move_selection(dx, 0);
    if (dy) vk_move_selection(0, dy);
    rep = edge_move ? 14 : 8;
  }
  if (rep > 0) rep--;
  if (np_btn_down(0) || np_btn_down(9)) vk_activate_key(g_vkbd_sel);    /* A/R3 = seleciona */
  if (np_btn_down(1)) vk_backspace();                                   /* B = apaga */
  if (np_btn_down(2)) g_vkbd_uppercase = !g_vkbd_uppercase;             /* X = shift */
  if (np_btn_down(6)) { g_vkbd_context=VK_CTX_NONE; jni_softinput_cancel(); g_vkbd_swallow=18; }
  if (np_btn_down(7)) vk_commit_text();                                 /* Start = conclui */
}
static void ter_vkbd_draw(void) {
  if (!jni_softinput_active()) return;
  int sw=0, sh=0, osc[4], oen=0; float occ[4]; unsigned char ocm[4];
  if (!vk_gl_begin(&sw, &sh, osc, occ, ocm, &oen)) return;
  /* Terraria-style inventory palette: midnight blue stone, wood edging and
     the warm gold used by selected controller slots. */
  vk_design_rect(sw,sh,132,268,1016,452,0.018f,0.025f,0.055f,1.0f);
  vk_design_rect(sw,sh,140,276,1000,444,0.44f,0.29f,0.075f,1.0f);
  vk_design_rect(sw,sh,148,284,984,436,0.035f,0.070f,0.155f,1.0f);
  vk_text(sw,sh,171,290,"ENTER NAME",2,0.98f,0.88f,0.48f);
  vk_design_rect(sw,sh,167,306,947,62,0.66f,0.46f,0.11f,1.0f);
  vk_design_rect(sw,sh,173,312,935,50,0.025f,0.055f,0.125f,1.0f);
  char shown[130]; snprintf(shown, sizeof shown, "%s_", jni_softinput_text());
  vk_text(sw,sh,190,323,shown,4,0.98f,0.91f,0.58f);
  for (int i=0; i<VK_NKEYS; i++) {
    const struct vk_key *key = &vk_keys[i];
    char dynamic_label[8]; const char *label = key->label;
    if (key->action == VK_CHARACTER) {
      dynamic_label[0] = g_vkbd_uppercase ? key->upper : key->lower;
      dynamic_label[1] = 0; label = dynamic_label;
    } else if (key->action == VK_SHIFT) {
      snprintf(dynamic_label, sizeof dynamic_label, "%s", g_vkbd_uppercase ? "UPPER" : "LOWER");
      label = dynamic_label;
    }
    int highlighted = i == g_vkbd_sel || (key->action == VK_SHIFT && g_vkbd_uppercase);
    vk_design_rect(sw,sh,key->x-3,key->y-3,key->w+6,key->h+6,
                   highlighted?1.00f:0.34f, highlighted?0.78f:0.23f,
                   highlighted?0.18f:0.065f,1.0f);
    vk_design_rect(sw,sh,key->x,key->y,key->w,key->h,
                   highlighted?0.18f:0.085f, highlighted?0.34f:0.17f,
                   highlighted?0.62f:0.34f,1.0f);
    int scale = strlen(label) > 3 ? 2 : 3;
    int tx = key->x + (key->w-vk_text_width(label,scale))/2;
    int ty = key->y + (key->h-7*scale)/2;
    vk_text(sw,sh,tx,ty,label,scale,0.98f,0.91f,0.58f);
  }
  vk_text(sw,sh,171,650,"A SELECT  B DELETE  X SHIFT  START DONE",2,0.82f,0.86f,0.94f);
  vk_gl_end(osc, occ, ocm, oen);
}
/* relocador de ADRP p/ trampolins (corrige o page-relative ao copiar p/ outro endereço) */
static uint32_t ter_reloc_insn(uint32_t insn, uintptr_t opc, uintptr_t npc) {
  if ((insn & 0x9F000000u) == 0x90000000u) {   /* ADRP */
    uint32_t immlo=(insn>>29)&3, immhi=(insn>>5)&0x7FFFF;
    int64_t imm=(int64_t)((immhi<<2)|immlo); if(imm&(1<<20)) imm-=(1<<21);
    uintptr_t target=(opc & ~0xFFFUL)+((uintptr_t)imm<<12);
    int64_t nimm=((int64_t)(target & ~0xFFFUL)-(int64_t)(npc & ~0xFFFUL))>>12;
    uint32_t nlo=nimm&3, nhi=(nimm>>2)&0x7FFFF;
    return (insn & 0x9F00001Fu)|(nlo<<29)|(nhi<<5);
  }
  return insn;  /* (stp/sub/mov etc. são position-independent) */
}
/* hook inline genérico: copia 4 instrs (relocando adrp) p/ um trampolim que segue p/ target+16,
   e patcha a entrada do alvo p/ saltar p/ fn. Retorna o trampolim (=original) em *orig_out. */
int ter_install_hook4(unsigned long off, void* fn, void** orig_out) {
  uintptr_t target=g_il2cpp_base+off; uint32_t*o=(uint32_t*)target; long pgsz=sysconf(_SC_PAGESIZE);
  uint32_t*tr=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(tr==MAP_FAILED) return 0;
  for(int i=0;i<4;i++) tr[i]=ter_reloc_insn(o[i], target+i*4, (uintptr_t)tr+i*4);
  tr[4]=0x58000050u; tr[5]=0xD61F0200u; *(uint64_t*)(tr+6)=(uint64_t)(target+16);
  __builtin___clear_cache((char*)tr,(char*)tr+32);
  *orig_out=(void*)tr;
  void*pa=(void*)(target & ~((uintptr_t)pgsz-1));
  mprotect(pa,pgsz*2,PROT_READ|PROT_WRITE|PROT_EXEC);
  o[0]=0x58000050u; o[1]=0xD61F0200u; *(uint64_t*)(o+2)=(uint64_t)(uintptr_t)fn;
  mprotect(pa,pgsz*2,PROT_READ|PROT_EXEC); __builtin___clear_cache((char*)pa,(char*)pa+16);
  return 1;
}
static volatile void *g_player_create_menu_inst, *g_world_create_menu_inst, *g_player_name_menu_inst;
static void ter_invoke0(const char *ns, const char *cn, const char *mn, void *obj);
static volatile void *g_world_name_menu_inst;
static volatile int g_world_name_menu_frame = -999999;
static volatile void *g_menu_nameedit_inst;
static volatile int g_player_create_menu_frame = -999999, g_world_create_menu_frame = -999999,
                    g_player_name_menu_frame = -999999, g_menu_nameedit_frame = -999999;
static void (*g_orig_player_create_draw)(void*, void*);
static void (*g_orig_player_name_draw)(void*, void*);
static void (*g_orig_world_create_draw)(void*, void*);
static void (*g_orig_nameedit_enable)(void*, void*, void*);
static void (*g_orig_player_create_save)(void*, void*);
static void (*g_orig_player_create_player)(void*, void*);
static void (*g_orig_world_create)(void*, void*);

static void ter_player_create_draw_hook(void *self, void *mi) {
  g_player_create_menu_inst = self;
  g_player_create_menu_frame = g_render_frame;
  int committed_context = ter_vkbd_finish_pending();
  if (committed_context == VK_CTX_PLAYER_FIELD) {
    ter_name_force_text(g_vkbd_force_text);
    ter_force_main_player_name(g_vkbd_force_text);
    ter_invoke0("", "GUIPlayerCreateMenu", "CloseNameEdit", self);
    ter_name_force_text(g_vkbd_force_text);
    ter_force_main_player_name(g_vkbd_force_text);
    fprintf(stderr, "[VKBD] nome do campo confirmado por CloseNameEdit nativo\n");
    fsync(2);
  }
  const char *live = jni_softinput_active() ? jni_softinput_text() : g_vkbd_force_text;
  if (live && live[0] && (jni_softinput_active() || g_vkbd_force_frames > 0))
    ter_name_force_text(live);
  if (g_orig_player_create_draw) g_orig_player_create_draw(self, mi);
}
/* 🔑 A tela de nome usa o caminho de texto ESTILO PC: GetInputText(oldString, region, ...)
   @0xFCD98C — le o teclado FNA (morto no so-loader), ZERA Main.inputTextEnter a cada
   chamada (por isso setar a flag de fora nunca funcionou) e devolve o texto digitado
   (por isso o OSK nao aparecia no campo). Hook com call-through: quando o auto-enter
   esta armado (g_name_enter_frames>0, setado pelo pump), devolvemos NOSSO nome e
   ligamos inputTextEnter DEPOIS do original (que acabou de zerar) -> a tela aceita
   nativamente (CreateAndSave/CreateWorld + transicao). Roda na thread do jogo. */
static volatile int g_name_enter_frames;
static void *(*g_orig_getinputtext)(long,long,long,long,long,long,long,long,long,long);
static void *ter_getinputtext_hook(long a0,long a1,long a2,long a3,long a4,
                                   long a5,long a6,long a7,long a8,long a9) {
  void *r = g_orig_getinputtext ? (void *)g_orig_getinputtext(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9)
                                : (void *)a0;
  if (g_name_enter_frames > 0 && g_il2cpp_base) {
    g_name_enter_frames--;
    const char *nm = g_vkbd_force_text[0] ? g_vkbd_force_text :
                     (getenv("TER_VK_DEFAULT") ? getenv("TER_VK_DEFAULT") : "Player");
    void *(*isn)(const char *) = (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
    /* RVAs CONFERIDOS no dump: set_chatText=0xF74E44, set_inputTextEnter=0xF74EF0
       (0xf74e9c/0xf74f4c eram os GETTERS — a 1a tentativa era no-op). Estamos na
       thread do jogo (chamados pelo Draw) -> thread-static ok. */
    void (*set_chat)(void *, void *) = (void (*)(void *, void *))(g_il2cpp_base + 0xF74E44);
    void (*set_enter)(int, void *) = (void (*)(int, void *))(g_il2cpp_base + 0xF74EF0);
    void *ns = isn(nm);
    set_chat(ns, NULL);
    set_enter(1, NULL);
    static int logged;
    if (logged++ < 8) { fprintf(stderr, "[AUTONAME] GetInputText -> \"%s\" + ENTER\n", nm); fsync(2); }
    return ns;
  }
  return r;
}
/* Preserva EnterName -> menu nativo -> GetInputText -> Create*. O hook fornece
   somente texto e confirmacao, que seriam entregues pelo teclado Android. */
static void ter_il2cpp_set_string_field(void *obj, size_t off, void *s);

static void ter_copy_managed_string(char *output, size_t output_size,
                                    void *managed, const char *fallback) {
  if (!output || output_size == 0) return;
  output[0] = '\0';
  if (managed) {
    int32_t length = *(int32_t *)((char *)managed + 0x10);
    const uint16_t *chars = (const uint16_t *)((char *)managed + 0x14);
    if (length > 0 && length < 128) {
      size_t count = (size_t)length < output_size - 1
                       ? (size_t)length : output_size - 1;
      for (size_t i = 0; i < count; i++)
        output[i] = chars[i] >= 0x20 && chars[i] < 0x7f ? (char)chars[i] : '_';
      output[count] = '\0';
    }
  }
  if (!output[0] && fallback)
    snprintf(output, output_size, "%s", fallback);
}

static void (*g_orig_player_entername)(void*, void*);
static void (*g_orig_world_entername)(void*, void*);
static void ter_player_entername_hook(void *self, void *mi) {
  const char *fallback = getenv("TER_VK_DEFAULT");
  ter_copy_managed_string(g_vkbd_force_text, sizeof g_vkbd_force_text,
                          self ? *(void **)((char *)self + 0x80) : NULL,
                          fallback && *fallback ? fallback : "Player");
  fprintf(stderr, "[VKBD] Criar(player): seguindo EnterName nativo com \"%s\"\n",
          g_vkbd_force_text); fsync(2);
  g_vkbd_context = VK_CTX_PLAYER_FIELD;
  if (g_orig_player_entername) g_orig_player_entername(self, mi);
}
static void ter_world_entername_hook(void *self, void *mi) {
  ter_copy_managed_string(g_vkbd_force_text, sizeof g_vkbd_force_text,
                          self ? *(void **)((char *)self + 0x70) : NULL,
                          "World");
  fprintf(stderr, "[VKBD] Criar(mundo): seguindo EnterName nativo com \"%s\"\n",
          g_vkbd_force_text); fsync(2);
  g_vkbd_context = VK_CTX_WORLD_FIELD;
  if (g_orig_world_entername) g_orig_world_entername(self, mi);
}
static void (*g_orig_world_name_draw)(void*, void*);
/* RENOMEAR: GUIPlayerSelectMenu (tela "Selecionar Personagem", botao Renomear).
   OpenNameEdit abre o GUIMenuNameEdit; CloseNameEditAndSave@0xD3899C pega
   _editedName e chama PlayerFileData.Rename. O teclado PC esta morto -> escrevemos
   _editedName (0x10) em TEMPO REAL (aparece na tela) e, no OK, disparamos o save. */
static volatile void *g_player_select_inst; static volatile int g_player_select_frame = -999999;
static volatile int g_pending_rename = -1;   /* frame do OK; save dispara no Draw (thread do jogo) */
static void (*g_orig_player_select_draw)(void*, void*);
static void ter_player_select_draw_hook(void *self, void *mi) {
  g_player_select_inst = self;
  g_player_select_frame = g_render_frame;
  ter_vkbd_finish_pending();
  /* tempo real: enquanto o OSK esta aberto num rename, reflete o texto no editor */
  if (jni_softinput_active() && g_menu_nameedit_inst &&
      g_render_frame - g_menu_nameedit_frame < 600 && g_il2cpp_base) {
    const char *t = jni_softinput_text();
    if (t && t[0]) {
      void *(*isn)(const char *) = (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
      ter_il2cpp_set_string_field((void *)g_menu_nameedit_inst, 0x10, isn(t));  /* _editedName */
    }
  }
  if (g_pending_rename >= 0 && !ter_vkbd_blocking() && g_render_frame - g_pending_rename > 8) {
    g_pending_rename = -1;
    if (g_menu_nameedit_inst && g_vkbd_force_text[0] && g_il2cpp_base) {
      void *(*isn)(const char *) = (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
      ter_il2cpp_set_string_field((void *)g_menu_nameedit_inst, 0x10,
                                  isn(g_vkbd_force_text));
    }
    fprintf(stderr, "[VKBD] rename: CloseNameEditAndSave (grava .plr)\n"); fsync(2);
    ter_invoke0("", "GUIPlayerSelectMenu", "CloseNameEditAndSave", self);
  }
  if (g_orig_player_select_draw) g_orig_player_select_draw(self, mi);
}
static void ter_world_name_draw_hook(void *self, void *mi) {
  g_world_name_menu_inst = self;
  g_world_name_menu_frame = g_render_frame;
  ter_vkbd_finish_pending();
  { static int visit_frames, opened, last_seen = -999999;
    if (g_render_frame - last_seen > 30) { visit_frames = 0; opened = 0; }
    last_seen = g_render_frame;
    if (!jni_softinput_active()) visit_frames++;
    if (!opened && visit_frames >= 20) {
      opened = 1;
      if (getenv("TER_OSK")) {
        g_vkbd_context = VK_CTX_WORLD_SCREEN;
        jni_softinput_open(ter_vkbd_effective_name("World"), 20);
        fprintf(stderr, "[VKBD] tela nativa de nome do mundo: teclado aberto\n");
      } else {
        if (!g_vkbd_force_text[0])
          snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s",
                   ter_vkbd_effective_name("World"));
        g_name_enter_frames = 12;
        fprintf(stderr, "[AUTONAME] tela nativa de nome do mundo: texto+Enter\n");
      }
      fsync(2);
    } }
  if (g_orig_world_name_draw) g_orig_world_name_draw(self, mi);
}
static void ter_player_name_draw_hook(void *self, void *mi) {
  g_player_name_menu_inst = self;
  g_player_name_menu_frame = g_render_frame;
  ter_vkbd_finish_pending();
  /* Preserve the game's own EnterName -> GUIPlayerNameMenu.Draw ->
     CreateAndSave sequence. Our overlay only supplies text/Enter where the
     original Android touch keyboard would have done so. */
  { static int visit_frames, opened, last_seen = -999999;
    if (g_render_frame - last_seen > 30) {
      visit_frames = 0;
      opened = 0;
    }
    last_seen = g_render_frame;
    if (!jni_softinput_active()) visit_frames++;
    if (!opened && visit_frames >= 20) {
      opened = 1;
      if (getenv("TER_OSK")) {
        g_vkbd_context = VK_CTX_PLAYER_SCREEN;
        jni_softinput_open(ter_vkbd_effective_name("Player"), 20);
        fprintf(stderr, "[VKBD] tela nativa de nome do jogador: teclado aberto\n");
      } else {
        if (!g_vkbd_force_text[0])
          snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s",
                   ter_vkbd_effective_name("Player"));
        ter_force_main_player_name(g_vkbd_force_text);
        g_name_enter_frames = 12;
        fprintf(stderr, "[AUTONAME] tela nativa de nome do jogador: texto+Enter\n");
      }
      fsync(2);
    } }
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("Player"));
  ter_player_name_menu_force_text(g_vkbd_force_text);
  if (g_orig_player_name_draw) g_orig_player_name_draw(self, mi);
  ter_player_name_menu_force_text(g_vkbd_force_text);
}
static void ter_world_create_draw_hook(void *self, void *mi) {
  g_world_create_menu_inst = self;
  g_world_create_menu_frame = g_render_frame;
  int committed_context = ter_vkbd_finish_pending();
  if (committed_context == VK_CTX_WORLD_FIELD) {
    ter_name_force_text(g_vkbd_force_text);
    ter_invoke0("", "GUIWorldCreateMenu", "CloseNameEdit", self);
    ter_name_force_text(g_vkbd_force_text);
    fprintf(stderr, "[VKBD] nome do mundo confirmado por CloseNameEdit nativo\n");
    fsync(2);
  }
  const char *live = jni_softinput_active() ? jni_softinput_text() : g_vkbd_force_text;
  if (live && live[0] && (jni_softinput_active() || g_vkbd_force_frames > 0))
    ter_name_force_text(live);
  if (g_orig_world_create_draw) g_orig_world_create_draw(self, mi);
}
static void ter_nameedit_enable_hook(void *self, void *arg, void *mi) {
  g_menu_nameedit_inst = self;
  g_menu_nameedit_frame = g_render_frame;
  if (g_orig_nameedit_enable) g_orig_nameedit_enable(self, arg, mi);
}
static const char *ter_vkbd_effective_name(const char *fallback) {
  if (g_vkbd_force_text[0]) return g_vkbd_force_text;
  const char *env = getenv("TER_VK_DEFAULT");
  return (env && *env) ? env : fallback;
}
static void ter_force_player_create_menu_name(void *self, const char *text) {
  if (!self || !g_il2cpp_base || !text || !text[0]) return;
  void *(*string_new)(const char *) =
      (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
  void *value = string_new(text);
  ter_il2cpp_set_string_field(self, 0x80, value); /* _playerName */
  ter_il2cpp_set_string_field(self, 0x88, value); /* editPlayerName */
}
static void ter_force_world_create_menu_name(void *self, const char *text) {
  if (!self || !g_il2cpp_base || !text || !text[0]) return;
  void *(*string_new)(const char *) =
      (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
  ter_il2cpp_set_string_field(self, 0x70, string_new(text)); /* _worldName */
}
static void ter_player_create_save_hook(void *self, void *mi) {
  g_player_create_menu_inst = self;
  g_player_create_menu_frame = g_render_frame;
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("Player"));
  ter_force_player_create_menu_name(self, g_vkbd_force_text);
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
  fprintf(stderr, "[VKBD] CreateAndSave forcou nome \"%s\"\n", g_vkbd_force_text); fsync(2);
  if (g_orig_player_create_save) g_orig_player_create_save(self, mi);
  ter_force_player_create_menu_name(self, g_vkbd_force_text);
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
}
static void ter_player_create_player_hook(void *self, void *mi) {
  g_player_create_menu_inst = self;
  g_player_create_menu_frame = g_render_frame;
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("Player"));
  ter_force_player_create_menu_name(self, g_vkbd_force_text);
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
  fprintf(stderr, "[VKBD] CreatePlayer forcou nome \"%s\"\n", g_vkbd_force_text); fsync(2);
  if (g_orig_player_create_player) g_orig_player_create_player(self, mi);
  ter_force_player_create_menu_name(self, g_vkbd_force_text);
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
}
static void ter_world_create_hook(void *self, void *mi) {
  g_world_create_menu_inst = self;
  g_world_create_menu_frame = g_render_frame;
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("MUNDO"));
  ter_force_world_create_menu_name(self, g_vkbd_force_text);
  ter_name_force_text(g_vkbd_force_text);
  fprintf(stderr, "[VKBD] CreateWorld forcou nome \"%s\"\n", g_vkbd_force_text); fsync(2);
  if (g_orig_world_create) g_orig_world_create(self, mi);
}
unsigned long ter_method_off(const char *ns, const char *cn, const char *mn, int argc) {
  if (!g_il2cpp_base) return 0;
  void *(*dom_get)(void) = (void*)(ter_i2sym("il2cpp_domain_get", 0x73c860));
  const void **(*dom_asms)(void*, size_t*) = (void*)(ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c));
  void *(*asm_img)(const void*) = (void*)(ter_i2sym("il2cpp_assembly_get_image", 0x73c22c));
  void *(*cls_from_name)(void*, const char*, const char*) = (void*)(ter_i2sym("il2cpp_class_from_name", 0x73c264));
  void *(*cls_method)(void*, const char*, int) = (void*)(ter_i2sym("il2cpp_class_get_method_from_name", 0x73c28c));
  void *domain = dom_get(); if (!domain) return 0;
  size_t na=0; const void **as = dom_asms(domain, &na); if (!as) return 0;
  for (size_t i=0; i<na; i++) {
    void *img = asm_img(as[i]); if (!img) continue;
    void *cls = cls_from_name(img, ns, cn); if (!cls) continue;
    void *m = cls_method(cls, mn, argc); if (!m) return 0;
    void *mp = *(void**)m; if (!mp) return 0;
    return (unsigned long)((uintptr_t)mp - g_il2cpp_base);
  }
  return 0;
}
static void ter_name_hooks_install(void) {
  static int done, tries;
  if (done || !g_il2cpp_base || (!getenv("TER_OSK") && !getenv("TER_AUTONAME"))) return;
  if (tries++ > 600) { done = 1; return; }
  unsigned long po = ter_method_off("", "GUIPlayerCreateMenu", "Draw", 0);
  unsigned long pno = ter_method_off("", "GUIPlayerNameMenu", "Draw", 0);
  unsigned long wo = ter_method_off("", "GUIWorldCreateMenu", "Draw", 0);
  unsigned long psd = ter_method_off("", "GUIPlayerSelectMenu", "Draw", 0);
  if (psd && !g_orig_player_select_draw &&
      ter_install_hook4(psd, (void*)ter_player_select_draw_hook, (void**)&g_orig_player_select_draw))
    fprintf(stderr, "[VKBD] GUIPlayerSelectMenu.Draw hookado @0x%lx (rename)\n", psd);
  unsigned long wnn = ter_method_off("", "GUIWorldNameMenu", "Draw", 0);
  if (!g_orig_getinputtext && ter_known_build() &&
      ter_install_hook4(0xFCD98C, (void*)ter_getinputtext_hook, (void**)&g_orig_getinputtext))
    fprintf(stderr, "[VKBD] GetInputText @0xFCD98C hookado (auto-enter)\n");
  else if (!g_orig_getinputtext && !ter_known_build()) {
    static int warned;
    if (!warned++)
      fprintf(stderr, "[VKBD] GetInputText por RVA requer o build de referencia; "
              "entrada de nome usa o padrao TER_VK_DEFAULT neste build\n");
  }
  unsigned long pen = ter_method_off("", "GUIPlayerCreateMenu", "EnterName", 0);
  unsigned long wen = ter_method_off("", "GUIWorldCreateMenu", "EnterName", 0);
  if (pen && !g_orig_player_entername &&
      ter_install_hook4(pen, (void*)ter_player_entername_hook, (void**)&g_orig_player_entername))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.EnterName hookado @0x%lx (fluxo nativo)\n", pen);
  if (wen && !g_orig_world_entername &&
      ter_install_hook4(wen, (void*)ter_world_entername_hook, (void**)&g_orig_world_entername))
    fprintf(stderr, "[VKBD] GUIWorldCreateMenu.EnterName hookado @0x%lx (fluxo nativo)\n", wen);
  if (wnn && !g_orig_world_name_draw &&
      ter_install_hook4(wnn, (void*)ter_world_name_draw_hook, (void**)&g_orig_world_name_draw))
    fprintf(stderr, "[VKBD] GUIWorldNameMenu.Draw hookado @0x%lx\n", wnn);
  unsigned long no = ter_method_off("", "GUIMenuNameEdit", "Enable", 1);
  unsigned long ps = ter_method_off("", "GUIPlayerCreateMenu", "CreateAndSave", 0);
  unsigned long pc = ter_method_off("", "GUIPlayerCreateMenu", "CreatePlayer", 0);
  unsigned long wc = ter_method_off("", "GUIWorldCreateMenu", "CreateWorld", 0);
  if (po && !g_orig_player_create_draw &&
      ter_install_hook4(po, (void*)ter_player_create_draw_hook, (void**)&g_orig_player_create_draw))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.Draw hookado @0x%lx\n", po);
  if (pno && !g_orig_player_name_draw &&
      ter_install_hook4(pno, (void*)ter_player_name_draw_hook, (void**)&g_orig_player_name_draw))
    fprintf(stderr, "[VKBD] GUIPlayerNameMenu.Draw hookado @0x%lx\n", pno);
  if (wo && !g_orig_world_create_draw &&
      ter_install_hook4(wo, (void*)ter_world_create_draw_hook, (void**)&g_orig_world_create_draw))
    fprintf(stderr, "[VKBD] GUIWorldCreateMenu.Draw hookado @0x%lx\n", wo);
  if (no && !g_orig_nameedit_enable &&
      ter_install_hook4(no, (void*)ter_nameedit_enable_hook, (void**)&g_orig_nameedit_enable))
    fprintf(stderr, "[VKBD] GUIMenuNameEdit.Enable hookado @0x%lx\n", no);
  if (ps && !g_orig_player_create_save &&
      ter_install_hook4(ps, (void*)ter_player_create_save_hook, (void**)&g_orig_player_create_save))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.CreateAndSave hookado @0x%lx\n", ps);
  if (pc && !g_orig_player_create_player &&
      ter_install_hook4(pc, (void*)ter_player_create_player_hook, (void**)&g_orig_player_create_player))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.CreatePlayer hookado @0x%lx\n", pc);
  if (wc && !g_orig_world_create &&
      ter_install_hook4(wc, (void*)ter_world_create_hook, (void**)&g_orig_world_create))
    fprintf(stderr, "[VKBD] GUIWorldCreateMenu.CreateWorld hookado @0x%lx\n", wc);
  if (g_orig_player_create_draw && g_orig_player_name_draw && g_orig_world_create_draw && g_orig_nameedit_enable &&
      g_orig_player_create_save && g_orig_player_create_player && g_orig_world_create) {
    done = 1; fsync(2);
  }
}
static void ter_il2cpp_set_string_field(void *obj, size_t off, void *s) {
  if (!obj || !s) return;
  void **slot = (void **)((char *)obj + off);
  void (*wb)(void *, void **, void *) = (void *)(ter_i2sym("il2cpp_gc_wbarrier_set_field", 0x73cb34));
  wb(obj, slot, s);
}
static void ter_player_name_menu_force_text(const char *text) {
  if (!g_il2cpp_base || !text || !text[0]) return;
  int fnm = g_render_frame - g_player_name_menu_frame;
  void *inst = (void *)g_player_name_menu_inst;
  if (!inst || fnm < 0 || fnm >= 900) return;
  void *(*isn)(const char *) = (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
  void *s = isn(text);
  ter_il2cpp_set_string_field(inst, 0x18, s);  /* GUIPlayerNameMenu.editPlayerName */
  if (getenv("TER_VKBDLOG")) {
    fprintf(stderr, "[VKBD] GUIPlayerNameMenu.editPlayerName=\"%s\" inst=%p\n", text, inst);
    fsync(2);
  }
}
void *ter_static_obj(const char *ns, const char *cn, const char *fn) {
  if (!g_il2cpp_base) return NULL;
  void *(*dom_get)(void) = (void*)(ter_i2sym("il2cpp_domain_get", 0x73c860));
  const void **(*dom_asms)(void*, size_t*) = (void*)(ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c));
  void *(*asm_img)(const void*) = (void*)(ter_i2sym("il2cpp_assembly_get_image", 0x73c22c));
  void *(*cls_from_name)(void*, const char*, const char*) = (void*)(ter_i2sym("il2cpp_class_from_name", 0x73c264));
  void *(*getf)(void*, const char*) = (void*)(ter_i2sym("il2cpp_class_get_field_from_name", 0x73c284));
  void (*sget)(void*,void*)=(void*)(ter_i2sym("il2cpp_field_static_get_value", 0x73ca44));
  void *domain = dom_get(); if (!domain) return NULL;
  size_t na=0; const void **as = dom_asms(domain, &na); if (!as) return NULL;
  for (size_t i=0; i<na; i++) {
    void *img = asm_img(as[i]); if (!img) continue;
    void *cls = cls_from_name(img, ns, cn); if (!cls) continue;
    void *fld = getf(cls, fn); if (!fld) return NULL;
    void *obj = NULL; sget(fld, &obj); return obj;
  }
  return NULL;
}
static void ter_force_player_obj_name(void *player, void *s) {
  if (!player || !s) return;
  ter_il2cpp_set_string_field(player, 0xe0, s);  /* Terraria.Player.name */
}
static void ter_force_main_player_name(const char *text) {
  if (!g_il2cpp_base || !text || !text[0]) return;
  void *(*isn)(const char *) = (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
  void *s = isn(text);
  int hits = 0;
  /* GUIPlayerCreateMenu.CreateAndSave serializes Main.PendingPlayer directly.
     It is not necessarily present in clientPlayer/player[] during creation.
     Resolvido POR NOME (multi-build); RVA 0xF77378 so no build de referencia.
     Chamar o RVA cru num build desconhecido caia em opcode arbitrario ->
     SIGSEGV na criacao de personagem (crash confirmado no 301544). */
  unsigned long ppo = ter_method_off("Terraria", "Main", "get_PendingPlayer", 0);
  if (!ppo && ter_known_build()) ppo = 0xF77378;
  if (ppo) {
    void *(*get_pending_player)(void *) =
        (void *(*)(void *))(g_il2cpp_base + ppo);
    void *pending = get_pending_player(NULL);
    if (pending) { ter_force_player_obj_name(pending, s); hits++; }
  }
  void *client = ter_static_obj("Terraria", "Main", "clientPlayer");
  if (client) { ter_force_player_obj_name(client, s); hits++; }
  void *arr = ter_static_obj("Terraria", "Main", "player");
  if (arr) {
    size_t len = *(size_t *)((char *)arr + 0x18);
    if (len > 256) len = 256;
    void **vec = (void **)((char *)arr + 0x20);
    for (size_t i=0; i<len; i++) if (vec[i]) {
      ter_force_player_obj_name(vec[i], s);
      hits++;
    }
  }
  if (getenv("TER_VKBDLOG")) {
    fprintf(stderr, "[VKBD] Terraria.Player.name=\"%s\" aplicado em %d instancia(s)\n", text, hits);
    fsync(2);
  }
}
static void ter_name_force_text(const char *text) {
  if (!g_il2cpp_base || !text || !text[0]) return;
  void *(*isn)(const char *) = (void *(*)(const char *))(ter_i2sym("il2cpp_string_new", 0x73cc98));
  void *s = isn(text);
  int fp = g_render_frame - g_player_create_menu_frame;
  int fw = g_render_frame - g_world_create_menu_frame;
  int fn = g_render_frame - g_menu_nameedit_frame;
  void *pinst = (void *)g_player_create_menu_inst;
  void *winst = (void *)g_world_create_menu_inst;
  void *ninst = (void *)g_menu_nameedit_inst;
  if (ninst && fn >= 0 && fn < 900) ter_il2cpp_set_string_field(ninst, 0x10, s);
  if (pinst && fp >= 0 && fp < 900) {
    ter_il2cpp_set_string_field(pinst, 0x80, s);
    ter_il2cpp_set_string_field(pinst, 0x88, s);
  }
  if (winst && fw >= 0 && fw < 900) ter_il2cpp_set_string_field(winst, 0x70, s);
  ter_player_name_menu_force_text(text);
}
static void ter_invoke0(const char *ns, const char *cn, const char *mn, void *obj) {
  if (!g_il2cpp_base || !obj) return;
  void *(*dom_get)(void) = (void*)(ter_i2sym("il2cpp_domain_get", 0x73c860));
  const void **(*dom_asms)(void*, size_t*) = (void*)(ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c));
  void *(*asm_img)(const void*) = (void*)(ter_i2sym("il2cpp_assembly_get_image", 0x73c22c));
  void *(*cls_from_name)(void*, const char*, const char*) = (void*)(ter_i2sym("il2cpp_class_from_name", 0x73c264));
  void *(*cls_method)(void*, const char*, int) = (void*)(ter_i2sym("il2cpp_class_get_method_from_name", 0x73c28c));
  void *(*rt_invoke)(void*, void*, void**, void**) = (void*)(ter_i2sym("il2cpp_runtime_invoke", 0x73cc7c));
  void *domain = dom_get(); if (!domain) return;
  size_t na=0; const void **as = dom_asms(domain, &na); if (!as) return;
  for (size_t i=0; i<na; i++) {
    void *img = asm_img(as[i]); if (!img) continue;
    void *cls = cls_from_name(img, ns, cn); if (!cls) continue;
    void *m = cls_method(cls, mn, 0); if (!m) return;
    void *exc = NULL; rt_invoke(m, obj, NULL, &exc);
    if (exc) fprintf(stderr, "[VKBD] %s.%s gerou excecao %p\n", cn, mn, exc);
    return;
  }
}
static void ter_name_commit_text(const char *text) {
  if (!g_il2cpp_base) return;
  if (!text || !text[0]) text = getenv("TER_VK_DEFAULT") ? getenv("TER_VK_DEFAULT") : "PLAYER";
  int fn = g_render_frame - g_menu_nameedit_frame;
  void *ninst = (void *)g_menu_nameedit_inst;
  int fpn = g_render_frame - g_player_name_menu_frame;
  int fwn = g_render_frame - g_world_name_menu_frame;

  /* Initial player/world naming stays on Terraria's native menu sequence.
     Publish the committed text and synthesize only the missing keyboard Enter;
     the original Draw method performs validation, CreateAndSave/CreateWorld
     and the menu transition itself. */
  if (g_player_name_menu_inst && fpn >= 0 && fpn < 30) {
    g_name_enter_frames = 12;
    fprintf(stderr, "[VKBD] nome do jogador \"%s\" entregue ao fluxo nativo\n", text);
    fsync(2);
    return;
  }
  if (g_world_name_menu_inst && fwn >= 0 && fwn < 30) {
    g_name_enter_frames = 12;
    fprintf(stderr, "[VKBD] nome do mundo \"%s\" entregue ao fluxo nativo\n", text);
    fsync(2);
    return;
  }
  if (ninst && fn >= 0 && fn < 600) {
    /* Toda escrita gerenciada e o save acontecem no proximo Draw, na thread do
       jogo. O swap/teclado apenas agenda a confirmacao. */
    int create_active =
        (g_player_create_menu_inst && g_render_frame-g_player_create_menu_frame < 5) ||
        (g_world_create_menu_inst && g_render_frame-g_world_create_menu_frame < 5);
    if (!create_active && g_player_select_inst &&
        g_render_frame-g_player_select_frame < 5) {
      g_pending_rename = g_render_frame;
      fprintf(stderr, "[VKBD] rename agendado (save no Draw)\n"); fsync(2);
      return;
    }
  }
  fprintf(stderr, "[VKBD] OK sem tela de nome ativa: \"%s\" fpn=%d fwn=%d fn=%d\n",
          text, fpn, fwn, fn); fsync(2);
}
/* DONE nasce no swap da thread grafica. A publicacao nos campos IL2CPP e o
   callback de teclado fechado acontecem no Draw seguinte, na thread do jogo,
   antes de o metodo original consumir o estado. */
static int ter_vkbd_finish_pending(void) {
  if (!g_vkbd_commit_pending) return VK_CTX_NONE;
  char text[128];
  int context = g_vkbd_context;
  snprintf(text, sizeof text, "%s", g_vkbd_commit_text);
  g_vkbd_commit_pending = 0;
  g_vkbd_context = VK_CTX_NONE;
  ter_name_force_text(text);
  ter_name_commit_text(text);
  jni_softinput_commit(text);
  g_vkbd_swallow = 24;
  fprintf(stderr, "[VKBD] DONE entregue na thread do jogo: \"%s\"\n", text);
  fsync(2);
  return context;
}
/* pump do autoname/vkbd — ANTES vivia dentro de ter_ctrl_feed (só rodava com TER_CTRL=1);
   agora é chamado incondicionalmente do swap-hook (o caminho NATPAD não seta TER_CTRL). */
static void ter_name_pump(void) {
  if (g_vkbd_force_frames > 0) {
    g_vkbd_force_frames--;
  }
  ter_vkbd_maybe_open();
  ter_vkbd_update();
  if (!jni_softinput_active() && g_vkbd_swallow > 0) g_vkbd_swallow--;
  /* WATCHDOG anti-trava: se o softinput esta ativo mas a tela de nome (GUIMenuNameEdit)
     ja fechou (_enabled=0 em +0x18) ha >90 frames — usuario saiu sem OK/cancelar — cancela.
     Sem isso o ter_vkbd_blocking fica preso e o NATPAD bloqueia TODOS os controles. */
  if (jni_softinput_active() && g_menu_nameedit_inst) {
    void *ne = (void *)g_menu_nameedit_inst;
    int enabled = *(unsigned char *)((char *)ne + 0x18);
    static int offct = 0;
    if (!enabled) {
      if (++offct >= 90) {
        offct = 0; g_vkbd_context = VK_CTX_NONE; jni_softinput_cancel();
        fprintf(stderr, "[VKBD] watchdog: tela de nome fechou sem OK -> cancelando teclado (controles liberados)\n"); fsync(2);
      }
    } else offct = 0;
  }
  /* TER_AUTONAME: sem teclado — quando o jogo abre a edicao de nome (GUIMenuNameEdit.Enable),
     espera ~40 frames e preenche o default (TER_VK_DEFAULT, senao PLAYER) fechando nativamente
     (CloseNameEdit via ter_name_commit_text). Re-arma a cada nova abertura. */
  if (getenv("TER_AUTONAME") && g_menu_nameedit_inst) {
    static int seen = -12345, fired = 0;
    int fn = g_menu_nameedit_frame;
    if (fn != seen) { seen = fn; fired = 0; }
    if (jni_softinput_active()) { seen = fn; fired = 1; }   /* teclado aberto: usuario digita, nao interfere */
    /* tela PC de nome ativa: quem completa e o auto-enter do GetInputText; fechar a
       edicao aqui mataria o caminho de input da tela. */
    { int fpn2 = g_render_frame - g_player_name_menu_frame;
      int fwn2 = g_render_frame - g_world_name_menu_frame;
      if ((g_player_name_menu_inst && fpn2 >= 0 && fpn2 < 5) ||
          (g_world_name_menu_inst && fwn2 >= 0 && fwn2 < 5)) fired = 1; }
    if (!fired && g_render_frame - fn >= 40) {
      fired = 1;
      const char *nm = getenv("TER_VK_DEFAULT") ? getenv("TER_VK_DEFAULT") : "Player";
      snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", nm);
      snprintf(g_vkbd_commit_text, sizeof g_vkbd_commit_text, "%s", nm);
      g_vkbd_force_frames = 180;
      g_vkbd_commit_pending = 1;
      fprintf(stderr, "[AUTONAME] nome \"%s\" agendado na thread do jogo\n", nm); fsync(2);
    }
  }
}
/* 🗺️ Mapa fullscreen do build mobile: o zoom nativo é SÓ pinça de toque
 * (GUIZoom.UpdatePinchZoom) e os botões de zoom são UI de toque, escondida em
 * modo controle; GUIControllerFullScreenMap só navega. Sem toque no so-loader,
 * dirigimos o estado do mapa direto: LT/RT = zoom, stick direito = pan.
 * Neste build o estado mora em UserManagement.LocalUsers[0].GameState
 * (LocalUserGameState.mapFullscreen/mapFullscreenScale/mapFullscreenPos),
 * não em Terraria.Main. Toda a cadeia é resolvida POR NOME via API il2cpp
 * (nenhum RVA: funciona em qualquer build). Desativa com TER_NOMAPCTL=1. */
extern float np_axis(int a);
static void ter_map_controls(void) {
  static int failed, ready;
  static void *fld_users;
  static size_t off_items, off_size, off_gamestate;
  static size_t off_full, off_scale, off_pos;
  static void (*f_sget)(void *, void *);
  static void *(*obj_class)(void *);
  static void *(*cls_field)(void *, const char *);
  static size_t (*fld_off)(void *);
  if (failed || getenv("TER_NOMAPCTL") || !g_il2cpp_base) return;
  if (!ready) {
    static int tries;
    int diag = (++tries == 300 || tries == 1800);
    if (tries > 1800) { failed = 1; return; }
    void *(*dom_get)(void) = (void *)ter_i2sym("il2cpp_domain_get", 0x73c860);
    const void **(*dom_asms)(void *, size_t *) =
        (void *)ter_i2sym("il2cpp_domain_get_assemblies", 0x73c86c);
    void *(*asm_img)(const void *) =
        (void *)ter_i2sym("il2cpp_assembly_get_image", 0x73c22c);
    void *(*cls_from_name)(void *, const char *, const char *) =
        (void *)ter_i2sym("il2cpp_class_from_name", 0x73c264);
    cls_field = (void *)ter_i2sym("il2cpp_class_get_field_from_name", 0x73c284);
    fld_off = (void *)ter_i2sym("il2cpp_field_get_offset", 0x73ca28);
    obj_class = (void *)ter_i2sym("il2cpp_object_get_class", 0x73cc28);
    f_sget = (void *)ter_i2sym("il2cpp_field_static_get_value", 0x73ca44);
    void *domain = dom_get(); if (!domain) return;
    size_t na = 0; const void **asms = dom_asms(domain, &na);
    if (!asms || !na) return;
    void *cls_um = NULL, *cls_gs = NULL;
    for (size_t i = 0; i < na && (!cls_um || !cls_gs); i++) {
      void *img = asm_img(asms[i]); if (!img) continue;
      if (!cls_um) cls_um = cls_from_name(img, "", "UserManagement");
      if (!cls_gs) cls_gs = cls_from_name(img, "", "LocalUserGameState");
    }
    if (!cls_um || !cls_gs) {
      if (diag) { fprintf(stderr, "[MAPCTL] try%d: UserManagement=%p LocalUserGameState=%p\n",
                          tries, cls_um, cls_gs); fsync(2); }
      return;
    }
    fld_users = cls_field(cls_um, "LocalUsers");
    void *f_full  = cls_field(cls_gs, "mapFullscreen");
    void *f_scale = cls_field(cls_gs, "mapFullscreenScale");
    void *f_pos   = cls_field(cls_gs, "mapFullscreenPos");
    if (!fld_users || !f_full || !f_scale || !f_pos) {
      if (diag) { fprintf(stderr, "[MAPCTL] try%d: users=%p full=%p scale=%p pos=%p\n",
                          tries, fld_users, f_full, f_scale, f_pos); fsync(2); }
      failed = 1; return;
    }
    off_full = fld_off(f_full); off_scale = fld_off(f_scale); off_pos = fld_off(f_pos);
    /* diagnostico do "mapa pisca e nao abre": snapshot do estado de carga */
    {
      static const char *dn[] = { "mapEnabled", "mapReady", "mapInit", "loadMap",
                                  "updateMap", "refreshMap", "clearMap", "mapDelay",
                                  "mapStyle", "resetMapFull", NULL };
      extern size_t g_mapdiag_off[10];
      for (int k = 0; dn[k]; k++) {
        void *f = cls_field(cls_gs, dn[k]);
        g_mapdiag_off[k] = f ? fld_off(f) : 0;
      }
    }
    ready = 1;
    fprintf(stderr, "[MAPCTL] mapa fullscreen: zoom LT/RT + pan stick direito "
            "(LocalUserGameState off full=0x%zx scale=0x%zx pos=0x%zx)\n",
            off_full, off_scale, off_pos);
    fsync(2);
  }
  /* Recaminha a cada frame: o LocalUser pode ser recriado em troca de save. */
  void *list = NULL;
  f_sget(fld_users, &list);
  if (!list) return;
  static int users_is_array;
  if (!off_items || !off_size) {
    void *lc = obj_class(list); if (!lc) return;
    void *fi = cls_field(lc, "_items"), *fs = cls_field(lc, "_size");
    if (fi && fs) {
      off_items = fld_off(fi); off_size = fld_off(fs);
    } else {
      /* LocalUser[] direto: length em +0x18, elementos a partir de +0x20 */
      users_is_array = 1; off_items = 0x20; off_size = 0x18;
    }
  }
  void *user;
  if (users_is_array) {
    if (*(int64_t *)((char *)list + off_size) < 1) return;
    user = *(void **)((char *)list + off_items);
  } else {
    if (*(int32_t *)((char *)list + off_size) < 1) return;
    void *items = *(void **)((char *)list + off_items);
    if (!items) return;
    user = *(void **)((char *)items + 0x20); /* header de Il2CppArray */
  }
  if (!user) return;
  if (!off_gamestate) {
    void *uc = obj_class(user); if (!uc) return;
    void *fg = cls_field(uc, "GameState");
    if (!fg) { failed = 1; return; }
    off_gamestate = fld_off(fg);
  }
  void *gs = *(void **)((char *)user + off_gamestate);
  if (!gs) return;
  /* [MAPDIAG] borda do mapFullscreen + aperto de SELECT: por que fecha? */
  {
    extern size_t g_mapdiag_off[10];
    extern int np_btn_down(int b);
    static unsigned char last_full = 0xff;
    unsigned char now_full = *(unsigned char *)((char *)gs + off_full);
    int sel = np_btn_down(6);
    if (now_full != last_full || sel) {
      static const char *dn[] = { "mapEnabled", "mapReady", "mapInit", "loadMap",
                                  "updateMap", "refreshMap", "clearMap", "mapDelay",
                                  "mapStyle", "resetMapFull" };
      char line[512]; int len = 0;
      len += snprintf(line + len, sizeof line - len,
                      "[MAPDIAG] f=%d %s full=%d", g_render_frame,
                      sel ? "SELECT" : "edge", now_full);
      for (int k = 0; k < 10; k++)
        if (g_mapdiag_off[k])
          len += snprintf(line + len, sizeof line - len, " %s=%d", dn[k],
                          *(int32_t *)((char *)gs + g_mapdiag_off[k]));
      fprintf(stderr, "%s\n", line); fsync(2);
      last_full = now_full;
    }
  }
  if (!*(unsigned char *)((char *)gs + off_full)) return; /* mapa fechado */
  float *scale = (float *)((char *)gs + off_scale);
  float *pos = (float *)((char *)gs + off_pos);
  float lt = np_axis(4), rt = np_axis(5);
  if (lt > 0.25f || rt > 0.25f) {
    float s = *scale;
    if (rt > 0.25f) s *= 1.0f + 0.028f * rt;
    if (lt > 0.25f) s /= 1.0f + 0.028f * lt;
    if (s < 0.2f) s = 0.2f;
    if (s > 16.0f) s = 16.0f;
    *scale = s;
  }
  float rx = np_axis(2), ry = np_axis(3);
  if (rx > 0.3f || rx < -0.3f || ry > 0.3f || ry < -0.3f) {
    float s = *scale < 0.2f ? 0.2f : *scale;
    pos[0] += rx * 10.0f / s;
    pos[1] += ry * 10.0f / s;
  }
}

void ter_before_present(void) {
  ter_nuke_methods();   /* TER_NUKEKB: neutraliza KeyboardInput.Update (lazy, até achar) */
  ter_fix_singleplayer(); /* TER_FIXSP: neutraliza OldSaveSynchronise.CopyOldSaves (tela preta SP) */
  ter_jobworkers0();    /* TER_JOBWORKERS0: JobWorkerCount=0 -> jobs inline */
  ter_name_hooks_install(); /* TER_OSK: captura telas de nome p/ gravar texto real */
/* 🎮 NATPAD (native_pad.c): o jogo VE um controle Xbox via InControl attach;
     menu/bolsa/gameplay/glifos 100% nativos. */
  ter_name_pump();  /* autoname/vkbd (independe do sistema de controle) */
  { extern void np_frame(void); np_frame(); }  /* 🎮 NATPAD: controle NATIVO via InControl attach */
  ter_map_controls(); /* 🗺️ mapa fullscreen: zoom LT/RT + pan stick direito */
  rs_present();   /* upscale do FBO lo-res p/ a tela real ANTES do swap */
  ter_vkbd_draw();
  ter_screenshot_maybe();
}

static unsigned my_eglSwapBuffers(void *dpy, void *surf) {
  ter_before_present();
  if (!r_eglSwapBuffers) r_eglSwapBuffers = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  return r_eglSwapBuffers ? r_eglSwapBuffers(dpy, surf) : 1;
}
static void *ds_route(const char *nm, void *real) {
  void *w = real;
  if (!nm || !real) return real;
  /* CUP_RENDERSCALE: intercepta o binding da tela + viewport/scissor (independe de DRAWSPY) */
  if (rs_enabled()) {
    if (!strcmp(nm, "glBindFramebuffer")) return (void *)rs_BindFramebuffer;
    if (!strcmp(nm, "glViewport"))        return (void *)rs_Viewport;
    if (!strcmp(nm, "glScissor"))         return (void *)rs_Scissor;
  }
  /* DRAWS: só envolve quando há contagem/diagnóstico (DRAWSPY=ring+glGetIntegerv,
   * DRAWCOUNT=contador leve). Com TEXHALF SOZINHO os draws passam DIRETO (sem wrapper)
   * — era a SANGRIA de performance (ds_enter fazia 4 glGetIntegerv/draw = sync GPU).
   * O fast-path de my_glDrawElements (g_drawdiag=0) só conta; mesmo assim, sem DRAWCOUNT
   * nem envolvemos. */
  if (g_drawdiag || getenv("CUP_DRAWCOUNT")) {
    if (!strcmp(nm, "glDrawElements")) { ds_r_DrawElements = real; return (void *)my_glDrawElements; }
    if (!strcmp(nm, "glDrawArrays"))   { ds_r_DrawArrays = real;   return (void *)my_glDrawArrays; }
    if (!strcmp(nm, "glClear"))        { ds_r_Clear = real;        return (void *)my_glClear; }
    if (!strcmp(nm, "glClearColor"))   { ds_r_ClearColor = real;   return (void *)my_glClearColor; }
  }
  /* SEMPRE-ON: o rebaixamento silencioso de depth/stencil e o FBO incompleto
     são a assinatura do minimapa (RenderTexture) que "às vezes não abre" —
     precisam aparecer no run.log de produção, não só sob CUP_DRAWSPY. Fora do
     spy, esses wrappers só logam em erro. */
  if (!strcmp(nm, "glRenderbufferStorage")) { r_glRenderbufferStorage = real; return (void *)my_glRenderbufferStorage; }
  if (!strcmp(nm, "glCheckFramebufferStatus")) { r_glCheckFBStatus = real; return (void *)my_glCheckFramebufferStatus; }
  if (!g_drawspy) return real;
  /* TEXTURAS (TEXHALF) — só estas precisam do roteamento em produção */
  if (!strcmp(nm, "glTexImage2D"))   { ds_r_TexImage2D = real;   w = (void *)my_glTexImage2D; }
  else if (!strcmp(nm, "glCompressedTexImage2D")) { ds_r_CompTexImage2D = real; w = (void *)my_glCompTexImage2D; }
  else if (!strcmp(nm, "glCompileShader")) { r_glCompileShader = real; w = (void *)my_glCompileShader; }
  else if (!strcmp(nm, "glLinkProgram"))   { r_glLinkProgram = real;   w = (void *)my_glLinkProgram; }
  else if (!strcmp(nm, "glShaderSource"))  { r_glShaderSource = real;  w = (void *)my_glShaderSource; }
  else if (!strcmp(nm, "glRenderbufferStorage")) { r_glRenderbufferStorage = real; w = (void *)my_glRenderbufferStorage; }
  else if (!strcmp(nm, "glCheckFramebufferStatus")) { r_glCheckFBStatus = real; w = (void *)my_glCheckFramebufferStatus; }
  else if (!strcmp(nm, "glFramebufferTexture2D")) { r_glFBTex2D = real; w = (void *)my_glFramebufferTexture2D; }
  else if (!strcmp(nm, "glFramebufferRenderbuffer")) { r_glFBRb = real; w = (void *)my_glFramebufferRenderbuffer; }
  else if (!strcmp(nm, "glUseProgram")) { r_glUseProgram = real; w = (void *)my_glUseProgram; }
  else if (!strcmp(nm, "glUniformMatrix4fv")) { r_glUniformMatrix4fv = real; w = (void *)my_glUniformMatrix4fv; }
  if (w != real) { fprintf(stderr, "[DS] route %s (real=%p)\n", nm, real); fsync(2); }
  return w;
}
static void ds_init(void) {
  rs_init();   /* CUP_RENDERSCALE: parseia env (o FBO lo-res cria-se lazy no 1º bind) */
  if (getenv("CUP_TEXHALF")) { g_texhalf = atoi(getenv("CUP_TEXHALF")); if (g_texhalf < 2) g_texhalf = 1024; }
  if (!getenv("CUP_DRAWSPY") && !g_texhalf && !rs_enabled()) return;
  g_drawspy = 1;  /* liga roteamento de gl* (DRAWSPY e/ou TEXHALF precisam de glTexImage2D) */
  g_drawdiag = getenv("CUP_DRAWSPY") ? 1 : 0;  /* ⚠️ ring + glGetIntegerv/draw — só em diag */
  g_skipfbo = getenv("CUP_SKIPFBO") ? 1 : 0;
  const char *sp = getenv("CUP_SKIPPROG");
  if (sp) {
    char buf[128]; strncpy(buf, sp, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, ","); t && g_nskipprog < 8; t = strtok(NULL, ","))
      g_skipprog[g_nskipprog++] = atoi(t);
  }
  if (g_drawdiag || getenv("CUP_DRAWCOUNT")) { pthread_t th; pthread_create(&th, NULL, ds_watchdog, NULL); }
  fprintf(stderr, "[DS] roteamento ON (texhalf=%d drawdiag=%d skipfbo=%d)\n",
          g_texhalf, g_drawdiag, g_skipfbo);
}

/* my_eglGetProcAddress: o Unity resolve as funções GL/extensões via
 * eglGetProcAddress (PLT→Mali real). Se uma extensão é ANUNCIADA (glGetString
 * EXTENSIONS) mas a função NÃO resolve (NULL), o Unity guarda um ponteiro
 * inválido e CRASHA ao chamá-lo (fault 0x7f10000004). Loga TODAS as resoluções
 * (com NULL destacado) p/ achar a culpada. CUP_NOVAO força NULL p/ as funções de
 * VAO (testa a hipótese de que GL_OES_vertex_array_object é a culpada). */
static void *(*r_eglGetProcAddress)(const char *);
static unsigned g_egp_n = 0;
static void *my_eglGetProcAddress(const char *nm) {
  /* 🔑 egl*: rotear p/ NOSSOS shims (egl_route). Sem isto, o engine pegava o eglChooseConfig
     REAL do Mali via eglGetProcAddress → o Mali Utgard rejeitava os attribs (GLES3/etc.) com
     EGL_BAD_ATTRIBUTE → o GfxDevice do Unity virava NULL-renderer → 0 chamadas GL → TELA PRETA.
     O nosso egl_shim_ChooseConfig ignora os attribs e devolve a config válida da window SDL. */
  if (nm && nm[0] == 'e' && nm[1] == 'g' && nm[2] == 'l') {
    void *e = egl_route(nm);
    if (e) { if (g_egp_n++ < 400) { fprintf(stderr, "[EGP] %s -> SHIM %p\n", nm, e); } return e; }
  }
  if (!r_eglGetProcAddress) r_eglGetProcAddress = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  void *p = r_eglGetProcAddress ? r_eglGetProcAddress(nm) : NULL;
  if (nm && getenv("CUP_NOVAO") && strstr(nm, "VertexArray")) p = NULL;  /* (vira no-op no Unity) */
  if (g_egp_n++ < 400)
    fprintf(stderr, "[EGP] %s -> %p%s\n", nm ? nm : "(null)", p, p ? "" : "  <== NULL!");
  return ds_route(nm, p);
}

static char g_dl_self, g_dl_il2cpp;
static so_module *g_m_unity = NULL, *g_m_il2cpp = NULL;

/* Resolve a export da API C do il2cpp pelo NOME no dynsym do modulo carregado
 * (funciona em qualquer build). O RVA do build de referencia fica apenas como
 * fallback; num build desconhecido sem o simbolo, devolve um stub que retorna
 * NULL para o caller degradar em silencio em vez de saltar para lixo. */
static long ter_i2sym_missing(void) { return 0; }
static uintptr_t ter_i2sym(const char *name, unsigned long ref_rva);
/* export p/ native_pad.c — mesma política nome-primeiro/fallback-gateado */
uintptr_t ter_i2sym_pub(const char *name, unsigned long ref_rva) {
  return ter_i2sym(name, ref_rva);
}
static uintptr_t ter_i2sym(const char *name, unsigned long ref_rva) {
  if (g_m_il2cpp) {
    so_module *current = so_save();
    so_use(g_m_il2cpp);
    uintptr_t address = so_find_addr_safe(name);
    so_use(current);
    free(current);
    if (address) return address;
  }
  if (ter_known_build() && g_il2cpp_base) return g_il2cpp_base + ref_rva;
  static int logged;
  if (logged++ < 20)
    fprintf(stderr, "[I2SYM] %s indisponivel neste build (rva-ref 0x%lx ignorado)\n",
            name, ref_rva);
  return (uintptr_t)ter_i2sym_missing;
}

/* ---- probe MemoryManager do libunity (RE: GetMemoryManager=0x3cbe2c) ----
 * gMemoryManager (bss)  vaddr 0x1292B48; cursor da arena estatica vaddr 0x11EF4D0;
 * data segment vaddr 0x11e6000. Detecta corrupcao do singleton entre fases. */
static void mm_probe(const char *tag) {
  if (!g_unity_data) return;
  /* offsets do data segment sao do build de referencia; num build menor a
   * leitura cairia fora do mapeamento */
  if (!ter_known_build()) return;
  void *mm  = *(void **)(g_unity_data + (0x1292B48 - 0x11e6000));
  void *cur = *(void **)(g_unity_data + (0x11EF4D0 - 0x11e6000));
  fprintf(stderr, "[MM:%s] gMemoryManager=%p cursor-arena=%p\n", tag, mm, cur);
}

/* ---- spy na entrada do operator-new tagueado (vaddr 0x3cbf2c) ----
 * Na entrada: x0=mgr x1=size x2=align(0x10) x3=kind x4=flag x5=tag-string.
 * O canario estoura nesta funcao durante RecreateGfxState -> capturar a chamada
 * culpada (size/kind gigante). Loga so' qdo g_in_gfx setado (evita flood).
 * O hook clobbera 4 insns; o tramp re-executa e segue em entry+16. */
uintptr_t g_gfx_cont = 0;            /* entry+16 (usado pelo asm) */
uintptr_t g_alloc_ub = 0, g_alloc_ib = 0;
volatile int g_in_gfx = 0;
static unsigned g_ospy_n = 0;
void onew_spy_log(uintptr_t mgr, uintptr_t size, uintptr_t kind, uintptr_t tag);
void onew_spy_log(uintptr_t mgr, uintptr_t size, uintptr_t kind, uintptr_t tag) {
  if (!g_in_gfx) return;
  const char *t = "?";
  if (g_alloc_ub && tag >= g_alloc_ub && tag < g_alloc_ub + 0x11e6000)
    t = (const char *)tag;
  fprintf(stderr, "[ONEW] #%u mgr=%lx size=%lu kind=%lu tag=%s\n",
          ++g_ospy_n, mgr, size, kind, t);
  fflush(stderr);
}
__asm__(
  ".text\n"
  ".global onew_spy_tramp\n"
  "onew_spy_tramp:\n"
  "  stp x29, x30, [sp, #-112]!\n"
  "  stp x0, x1, [sp, #16]\n"
  "  stp x2, x3, [sp, #32]\n"
  "  stp x4, x5, [sp, #48]\n"
  "  stp x6, x7, [sp, #64]\n"
  "  str x8, [sp, #80]\n"
  "  mov x0, x0\n"               /* mgr */
  "  mov x2, x3\n"               /* kind */
  "  mov x3, x5\n"               /* tag */
  "  bl onew_spy_log\n"          /* (mgr,size,kind,tag) */
  "  ldr x8, [sp, #80]\n"
  "  ldp x6, x7, [sp, #64]\n"
  "  ldp x4, x5, [sp, #48]\n"
  "  ldp x2, x3, [sp, #32]\n"
  "  ldp x0, x1, [sp, #16]\n"
  "  ldp x29, x30, [sp], #112\n"
  /* prologo original clobberado (0x3cbf2c..0x3cbf38) */
  "  stp x28, x27, [sp, #-96]!\n"
  "  stp x26, x25, [sp, #16]\n"
  "  stp x24, x23, [sp, #32]\n"
  "  stp x22, x21, [sp, #48]\n"
  "  adrp x17, g_gfx_cont\n"
  "  add x17, x17, :lo12:g_gfx_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern void onew_spy_tramp(void);

/* ===== CUP_WAITGATE: FORCEINTEG cirúrgico (só durante WaitForAll) =====
 * O FORCEINTEG global (NOP em 0x872774) integra ops cedo demais até FORA do
 * WaitForAll → corrompe um delegate → crash 0x7f10000004 ~60 frames depois.
 * Aqui só ignoramos o gate de budget QUANDO a main está dentro de
 * WaitForAllAsyncOperationsToComplete (0x873a90, force-complete legítimo).
 *
 * (1) hook 0x873a90 → my_waitall (C): liga g_in_waitall, chama o original
 *     (waitall_orig_tramp re-executa o prólogo clobberado e segue em +16),
 *     desliga o flag. (2) hook do gate 0x871844 → my_gate: se in_waitall=1
 *     retorna 1; senão replica a lógica original (budget 0x871884 AND
 *     (jobmgr==null OR NOT pending 0x6cdad0)). */
volatile int g_in_waitall = 0;
uintptr_t g_waitall_cont = 0;   /* 0x873a90 + 16 (usado pelo asm) */
/* gate replica — usa as bases já capturadas (g_unity_base/g_unity_data) */
int my_gate(void *op);
static int jobs_pending(void) {
  void *mgr = *(void **)(g_unity_data + 0xd3380);  /* job-scheduler 0x12b9380 */
  if (!mgr) return 0;
  return ((int (*)(void *))(g_unity_base + 0x6cdad0))(mgr);
}
int g_gatewait = 0;   /* CUP_GATEWAIT: gate sempre bypassa budget + spin-wait nos jobs */
int my_gate(void *op) {
  if (g_gatewait) {
    /* SEMPRE ignora budget (time-slice quebrado no so-loader), mas ESPERA os jobs
       do worker terminarem — spin com sched_yield (dá CPU aos workers) até jobmgr
       limpar. Mata a race da integração forçada (objeto malformado -> crash $PC=9). */
    for (int i = 0; i < 200000 && jobs_pending(); i++) sched_yield();
    return 1;
  }
  if (g_in_waitall) {
    if (getenv("CUP_GATEJOBS")) return jobs_pending() ? 0 : 1;
    return 1;
  }
  int budget = ((int (*)(void *))(g_unity_base + 0x871884))((char *)op + 0x98);
  if (!budget) return 0;
  return jobs_pending() ? 0 : 1;
}
/* trampolim que re-executa o prólogo clobberado de 0x873a90 e segue em +16 */
__asm__(
  ".text\n"
  ".global waitall_orig_tramp\n"
  "waitall_orig_tramp:\n"
  "  stp x22, x21, [sp, #-48]!\n"   /* 0x873a90 */
  "  stp x20, x19, [sp, #16]\n"     /* 0x873a94 */
  "  stp x29, x30, [sp, #32]\n"     /* 0x873a98 */
  "  add x29, sp, #0x20\n"          /* 0x873a9c */
  "  adrp x17, g_waitall_cont\n"
  "  add x17, x17, :lo12:g_waitall_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern long waitall_orig_tramp(void *thiz, long a1);
long my_waitall(void *thiz, long a1);
long my_waitall(void *thiz, long a1) {
  g_in_waitall++;
  long r = waitall_orig_tramp(thiz, a1);
  g_in_waitall--;
  return r;
}

/* ===== CUP_CLAMPSIG: clampa o count do Semaphore::Signal (0x65850c) =====
 * Signal(x0=sem, w1=count) posta sem(x0+4) `count` vezes (loop do-while w19=w1).
 * O count deriva p/ um valor enorme (storm/livelock ~frame 110). Hookamos a entrada
 * e clampamos w1 a um máximo são (>nº real de threads ~20) → mata o storm.
 * Prólogo clobberado (4 stp em 0x65850c..0x658518); o tramp re-executa e segue +16. */
uintptr_t g_signal_cont = 0;   /* 0x65850c + 16 */
static int g_signal_clamp = 4096;  /* passa counts legítimos (~dezenas/centenas), só pega o storm */
static volatile unsigned g_signal_clamps = 0;
__asm__(
  ".text\n"
  ".global signal_orig_tramp\n"
  "signal_orig_tramp:\n"
  "  stp x26, x25, [sp, #-80]!\n"   /* 0x65850c */
  "  stp x24, x23, [sp, #16]\n"     /* 0x658510 */
  "  stp x22, x21, [sp, #32]\n"     /* 0x658514 */
  "  stp x20, x19, [sp, #48]\n"     /* 0x658518 */
  "  adrp x17, g_signal_cont\n"
  "  add x17, x17, :lo12:g_signal_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern long signal_orig_tramp(void *sem, long count);
long my_signal(void *sem, long count);
long my_signal(void *sem, long count) {
  int c = (int)count;
  if (c > g_signal_clamp) {
    if (g_signal_clamps++ < 12) {
      /* caller = quem chamou Signal (job-scheduler?) */
      uintptr_t ra = (uintptr_t)__builtin_return_address(0);
      const char *lib = "?"; uintptr_t off = ra;
      if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + text_size) { lib = "libunity"; off = ra - g_unity_base; }
      else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000) { lib = "libil2cpp"; off = ra - g_il2cpp_base; }
      fprintf(stderr, "[CLAMPSIG] Signal(sem=%p) count=%d (0x%x) -> %d  caller=%s+0x%lx\n",
              sem, c, (unsigned)c, g_signal_clamp, lib, (unsigned long)off);
      /* vizinhança do sem (objeto Semaphore/fila de jobs): contador interno + campos */
      uintptr_t b = ((uintptr_t)sem - 0x20) & ~0x7UL;
      for (long d = 0; d < 0x40; d += 8)
        fprintf(stderr, "[CLAMPSIG]   sem%+ld: %016lx\n", d - 0x20, *(unsigned long *)(b + d));
      fsync(2);
    }
    count = (long)g_signal_clamp;
  }
  return signal_orig_tramp(sem, count);
}

/* ===== CUP_CRSPY: espião das coroutines de boot do CupheadStartScene =====
 * O boot (disclaimer) é dirigido por start_cr (RVA il2cpp 0x9A58D0, iterator $PC
 * em +0xBC) que encadeia: settings load → fonts → preload atlases/music →
 * WaitForUserInputBeforeContinue (RVA 0x9A619C, $PC em +0x1C) → load do título.
 * Logamos transições de $PC p/ ver exatamente em qual passo o boot estaciona. */
uintptr_t g_cr1_cont = 0, g_cr2_cont = 0;
__asm__(
  ".text\n"
  ".global cr1_tramp\n"
  "cr1_tramp:\n"
  "  stp x24, x23, [sp, #-64]!\n"    /* 0x9A58D0 */
  "  stp x22, x21, [sp, #16]\n"
  "  stp x20, x19, [sp, #32]\n"
  "  stp x29, x30, [sp, #48]\n"
  "  adrp x17, g_cr1_cont\n"
  "  add x17, x17, :lo12:g_cr1_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
  ".global cr2_tramp\n"
  "cr2_tramp:\n"
  "  stp x22, x21, [sp, #-48]!\n"    /* 0x9A619C */
  "  stp x20, x19, [sp, #16]\n"
  "  stp x29, x30, [sp, #32]\n"
  "  add x29, sp, #0x20\n"
  "  adrp x17, g_cr2_cont\n"
  "  add x17, x17, :lo12:g_cr2_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern long cr1_tramp(void *it);
extern long cr2_tramp(void *it);
long my_start_cr(void *it);
static const char *il2cpp_classname(void *obj) {
  /* obj->klass (off 0) -> klass->name (off 0x10 nesta versão il2cpp 2017) */
  if (!obj) return "(null)";
  void *klass = *(void **)obj;
  if (!klass || ((uintptr_t)klass >> 40)) return "(?)";
  const char *nm = *(const char **)((char *)klass + 0x10);
  return (nm && ((uintptr_t)nm >> 40) == 0) ? nm : "(?)";
}
void *volatile g_startcr_it = NULL;  /* iterator do start_cr capturado (CUP_DRIVECR) */
/* CUP_GATERESTORE: FORCEINTEG (NOP no gate budget 0x871854/0x872774) só é necessário
 * p/ o FontLoader ($PC 7->8). Forçar integração GLOBAL integra ops cujo worker job não
 * rodou -> objeto malformado (vtable/offset uninit) que crasha depois (Cuphead.Init $PC=9
 * na desserialização do CupheadCore: fault = fragmento de string = heap uninit). Restaura
 * o gate ORIGINAL assim que o $PC passa de 8, ANTES do Cuphead.Init. */
static void restore_gate_once(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  uintptr_t a1 = g_unity_base + 0x871854, a2 = g_unity_base + 0x872774;
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t p1 = a1 & ~(uintptr_t)(pg - 1), p2 = a2 & ~(uintptr_t)(pg - 1);
  mprotect((void *)p1, pg, PROT_READ | PROT_WRITE | PROT_EXEC);
  if (p2 != p1) mprotect((void *)p2, pg, PROT_READ | PROT_WRITE | PROT_EXEC);
  *(uint32_t *)a1 = 0x360000c0u;  /* tbz w0,#0,0x87186c (original) */
  *(uint32_t *)a2 = 0x360004e0u;  /* tbz w0,#0,0x872810 (original) */
  __builtin___clear_cache((char *)a1, (char *)a1 + 4);
  __builtin___clear_cache((char *)a2, (char *)a2 + 4);
  mprotect((void *)p1, pg, PROT_READ | PROT_EXEC);
  if (p2 != p1) mprotect((void *)p2, pg, PROT_READ | PROT_EXEC);
  fprintf(stderr, "[GATERESTORE] gate budget restaurado (0x871854/0x872774) apos FontLoader\n");
  fsync(2);
}
long my_start_cr(void *it) {
  static int lastpc = -99; static unsigned n, samepc;
  g_startcr_it = it;
  int pc = *(int *)((char *)it + 0xBC);
  if (pc != lastpc)
    { fprintf(stderr, "[CRSPY] start_cr tick#%u $PC=%d f=%d\n", n, pc, g_render_frame); fsync(2); samepc = 0; }
  else if (++samepc <= 4) {
    void *cur = *(void **)((char *)it + 0xB0);
    fprintf(stderr, "[CRSPY] start_cr RE-ENTER $PC=%d (samepc=%u) $cur=%p cls=%s f=%d\n",
            pc, samepc, cur, il2cpp_classname(cur), g_render_frame); fsync(2);
  }
  else if (samepc % 180 == 0) {
    void *cur = *(void **)((char *)it + 0xB0);  /* $current (objeto yieldado) */
    fprintf(stderr, "[CRSPY] start_cr SPIN $PC=%d x%u $current=%p cls=%s f=%d\n",
            pc, samepc, cur, il2cpp_classname(cur), g_render_frame); fsync(2);
  }
  lastpc = pc; n++;
  long r = cr1_tramp(it);
  int pc2 = *(int *)((char *)it + 0xBC);
  void *cur = *(void **)((char *)it + 0xB0);
  if (pc2 != pc) {
    fprintf(stderr, "[CRSPY] start_cr $PC %d -> %d (ret=%ld $cur=%p cls=%s f=%d)\n",
            pc, pc2, r, cur, il2cpp_classname(cur), g_render_frame);
    fsync(2); lastpc = pc2;
    if (pc2 >= 9 && getenv("CUP_GATERESTORE")) restore_gate_once();
  } else if (samepc <= 4) {
    fprintf(stderr, "[CRSPY] start_cr POST $PC=%d ret=%ld $cur=%p cls=%s f=%d\n",
            pc, r, cur, il2cpp_classname(cur), g_render_frame); fsync(2);
  }
  return r;
}
long my_inputwait_cr(void *it);
long my_inputwait_cr(void *it) {
  static int lastpc = -99; static unsigned n;
  int pc = *(int *)((char *)it + 0x1C);
  if (pc != lastpc)
    { fprintf(stderr, "[CRSPY] inputwait tick#%u $PC=%d f=%d\n", n, pc, g_render_frame); fsync(2); }
  lastpc = pc; n++;
  long r = cr2_tramp(it);
  int pc2 = *(int *)((char *)it + 0x1C);
  if (pc2 != pc) {
    fprintf(stderr, "[CRSPY] inputwait $PC %d -> %d (ret=%ld f=%d)\n", pc, pc2, r, g_render_frame);
    fsync(2); lastpc = pc2;
  }
  return r;
}

/* ===== CUP_BOOTSPY: log de entrada nas funções da cadeia de boot (il2cpp) =====
 * Hooks de log genéricos: trampolim runtime copia as 4 insns clobberadas pelo
 * hook_arm64; stp/add/mov copiam direto, adrp é recomputado (ldr-literal com o
 * endereço absoluto da página). Mostra qual elo da cadeia
 * Start→LoadFromCloud→LoadCloudData→OnLoaded→OnSettingsDataLoaded→start_cr morre. */
static uint32_t *bs_page = NULL; static int bs_used = 0;
static void *mk_tramp(uintptr_t target, const char *name) {
  if (!bs_page) {
    bs_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bs_page == MAP_FAILED) { bs_page = NULL; return NULL; }
  }
  uint32_t *st = bs_page + bs_used;
  uint32_t *p = st;
  const uint32_t *src = (const uint32_t *)target;
  for (int i = 0; i < 4; i++) {
    uint32_t in = src[i];
    if ((in & 0x9F000000u) == 0x90000000u) {            /* adrp rd, page */
      int rd = in & 31;
      long immlo = (in >> 29) & 3, immhi = (in >> 5) & 0x7FFFF;
      long imm = (immhi << 2) | immlo;
      if (imm & (1L << 20)) imm -= (1L << 21);          /* sign extend 21 bits */
      uint64_t page = ((target + i * 4) & ~0xFFFUL) + (imm << 12);
      *p++ = 0x58000040u | rd;                          /* ldr rd, +8 */
      *p++ = 0x14000003u;                               /* b +12 */
      *(uint64_t *)p = page; p += 2;
    } else if ((in & 0x7C000000u) == 0x14000000u || (in & 0xFF000000u) == 0x58000000u ||
               (in & 0x7C000000u) == 0x94000000u || (in & 0xFE000000u) == 0x54000000u) {
      fprintf(stderr, "[BOOTSPY] %s: insn %d não-relocável (%08x) — hook ABORTADO\n", name, i, in);
      return NULL;
    } else {
      *p++ = in;                                        /* stp/add/mov etc: PI, copia */
    }
  }
  *p++ = 0x58000051u;                                   /* ldr x17, #8 */
  *p++ = 0xd61f0220u;                                   /* br x17 */
  *(uint64_t *)p = target + 16; p += 2;
  bs_used += (int)(p - st) + (4 - ((p - st) & 3));      /* avança alinhado */
  __builtin___clear_cache((char *)st, (char *)p);
  return st;
}
#define BS_WRAP(idx, label) \
  static long (*bs_orig_##idx)(long, long, long, long, long, long, long, long); \
  static long bs_hook_##idx(long a, long b, long c, long d, long e, long f, long g, long h) { \
    static unsigned n; \
    if (n++ < 24) { fprintf(stderr, "[BOOTSPY] %s (#%u) x0=%lx x1=%lx f=%d\n", label, n, a, b, g_render_frame); fsync(2); } \
    return bs_orig_##idx(a, b, c, d, e, f, g, h); \
  }
BS_WRAP(0, "CupheadStartScene.Start")
BS_WRAP(1, "CupheadStartScene.OnSettingsDataLoaded")
BS_WRAP(2, "SettingsData.LoadFromCloud")
BS_WRAP(3, "OnlineInterfaceSteam.LoadCloudData")
BS_WRAP(4, "OnlineManager.Init")
BS_WRAP(5, "SettingsData.Save")
BS_WRAP(6, "CupheadStartScene.start_cr(factory)")
BS_WRAP(7, "SettingsData.OnLoadedCloudData")
/* ===== CUP_MASKGUARD (s12): 2º crash do load do mapa =====
 * libunity 0x8f9914 monta arrays de índice (SpriteMask/Tilemap mesh): recebe a
 * CONTAGEM em w0 e escreve em [obj+128][w0-1]. Na cena do mapa, w0 vem LIXO
 * (~0x10000000) -> escrita fora dos limites -> SIGSEGV em 0x8f9b1c. Mesma raiz do
 * SCENEGUARD (objeto da cena do mapa mal-inicializado no so-loader). Clampa a
 * contagem insana p/ 0 (mask vazia) em vez de estourar. */
/* ===== CUP_SCENESKIP (s12): RAIZ da cadeia de crashes do mapa =====
 * A função 0x541c9c processa o tilemap/mesh de um GameObject; resolve a scene via
 * helper 0x8f7c48 = ldp x8,x1,[arg0,#56] (scene = *(void**)(arg0+56)). No mapa, vários
 * GameObjects têm scene NULL (não registrados na cena pelo so-loader) -> a função
 * deref nulls em cascata (0x541cdc, 0x8f9b1c via 0x8f9914, 0x8f9b88, 0x541e54...).
 * Em vez de remendar cada deref (whack-a-mole), PULA a função inteira quando a scene
 * é NULL: o GameObject não monta o mesh (não renderiza), mas nada crasha. Epílogo é
 * void (caller 0x541c2c ignora o retorno). Substitui a abordagem fake-scene do island. */
static long (*scene541_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_sceneskip_hits, g_sceneadopt_hits;
static void *volatile g_map_scene;   /* último scene handle VÁLIDO visto (p/ adoção) */
static long scene541_hook(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  void *scene = a0 ? *(void **)((char *)a0 + 56) : NULL;
  if (scene) {
    g_map_scene = scene;   /* captura: objeto bem-registrado da cena do mapa */
    return scene541_orig(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  /* scene==NULL: o objeto (player/rig de câmera) está numa cena que o so-loader NÃO
   * registrou. CUP_SCENEADOPT (opt-IN; default OFF=skip): tentou ADOTAR o objeto na cena
   * válida (escreve scene real em [a0+56]) — mas FALHOU: o objeto está meio-construído
   * (OUTROS campos null tb: idx/tilemap) -> deref selvagem em 0x541cdc (fault wild). Igual
   * à fake-scene antiga. Raiz = integração async da cena aditiva nunca completa, não só o
   * scene-link. Mantido GATED p/ referência; default = SKIP (mapa renderiza sem o player). */
  if (a0 && g_map_scene && getenv("CUP_SCENEADOPT")) {
    *(void **)((char *)a0 + 56) = g_map_scene;
    if (g_sceneadopt_hits < 12)
      fprintf(stderr, "[SCENEADOPT] 0x541c9c scene=NULL -> adotado na cena do mapa (%p, f=%d)\n",
              g_map_scene, g_render_frame);
    g_sceneadopt_hits++;
    return scene541_orig(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  /* ===== CUP_HIERFIX (s14, default ON; CUP_NOHIERFIX desliga) — FIX REAL =====
   * Ground truth s14: a0 é o TRANSFORM do prefab de ORIGEM do Instantiate (0x541c9c roda
   * no source, início do clone worker 0x5424b0) e {P,idx}={NULL,-1} = o prefab carregado
   * dos assets NUNCA foi inserido numa TransformHierarchy (o passo do load que a
   * integração forçada pula; caller normal 0x459230 no caminho de load). Sem P o clone
   * produz NADA -> CloneObject retorna NULL (os DESERGUARD pareados) -> Instantiate=null
   * -> player/câmera/UI do mapa nem EXISTEM.
   * Fix: libunity 0x901164(transform) = rebuild da hierarchy da árvore inteira: sobe à
   * raiz ([T+0x90]), conta a sub-árvore (0x90110c, só anda em filhos [T+0x70/0x80] —
   * null-safe), cria hierarchy (0x8f9914), insere recursivo (0x9012b8, escreve {P,idx}
   * em cada nó), registra no manager global ([0x12c0398]) e destrói a antiga (0x8f9d80,
   * null-safe). Depois disso o clone segue o caminho NORMAL do engine. */
  if (a0 && !getenv("CUP_NOHIERFIX")) {
    static volatile uint32_t hf_n;
    /* raiz da árvore (p/ log; o rebuild já sobe sozinho) */
    void *root = (void *)a0;
    while (*(void **)((char *)root + 0x90)) root = *(void **)((char *)root + 0x90);
    ((void (*)(void *))(g_unity_base + 0x901164))((void *)a0);
    void *P = *(void **)((char *)a0 + 56);
    long idx = *(long *)((char *)a0 + 64);
    if (hf_n < 16)
      fprintf(stderr, "[HIERFIX] 0x901164(rebuild) t=%p root=%p -> P=%p idx=%ld (f=%d)\n",
              (void *)a0, root, P, idx, g_render_frame);
    hf_n++;
    if (P) return scene541_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    /* rebuild não populou — cai no skip de segurança */
  }
  if (g_sceneskip_hits < 8) {
    /* s14: a0 = Transform (RE 0x541b90: Component -> [obj+0x30]=GameObject -> cast).
     * {P=hierarchy, idx} em [a0+0x38/0x40]; +0x30 = GameObject do Component. */
    fprintf(stderr, "[SCENESKIP] 0x541c9c scene=NULL -> skip GO (f=%d) obj=%p go=%p idx=%ld\n",
            g_render_frame, (void *)a0,
            a0 ? *(void **)((char *)a0 + 0x30) : NULL,
            a0 ? *(long *)((char *)a0 + 0x40) : -1);
  }
  g_sceneskip_hits++;
  return 0;
}
/* ===== CUP_NULLGUARD (s12): 3º crash do load do mapa =====
 * libunity 0x8f9b88 (função de tilemap/mesh, chamada de 0x541dcc) faz
 * `ldr x14,[x0,#24]` SEM null-check; no mapa x0 (arg0) vem NULL (deriva da
 * fake-scene do SCENEGUARD) -> SIGSEGV fault=0x18. Skip quando arg0==NULL. */
static long (*nullfn_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_nullguard_hits;
static long nullfn_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  if (a == 0) {
    if (g_nullguard_hits < 8) fprintf(stderr, "[NULLGUARD] 0x8f9b88 arg0=NULL -> skip (f=%d)\n", g_render_frame);
    g_nullguard_hits++;
    return 0;
  }
  return nullfn_orig(a, b, c, d, e, f, g, h);
}
static long (*maskfn_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_maskguard_hits;
static long maskfn_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  uint32_t n = (uint32_t)a;
  /* A função faz `w10 = count-1` e escreve array[count-1] SEM checar count>0:
   *   count==0 -> w10 = 0xffffffff -> store OOB gigante -> SIGSEGV em 0x8f9b1c.
   *   count enorme (lixo) -> idem. No mapa do Cuphead aparece count==0 (mesh/tilemap
   *   vazio no so-loader). Clampa p/ [1, 0x40000]: count=1 -> w10=0 -> array[0]
   *   (slot que a função JÁ escreve incondicionalmente em 0x8f9b14, logo existe). */
  if (n == 0 || n > 0x40000u) {
    if (g_maskguard_hits < 8)
      fprintf(stderr, "[MASKGUARD] count=%u (0x%x) -> 1 (f=%d)\n", n, n, g_render_frame);
    g_maskguard_hits++;
    a = 1;
  }
  return maskfn_orig(a, b, c, d, e, f, g, h);
}
/* ===== CUP_DESERGUARD (s13): crash #5 do load do mapa =====
 * libunity 0x54220c (cluster de desserialização da cena; recebe arg0=ptr p/ um par
 * {objeto, ...} na stack) faz `ldr x8,[arg0]` (objeto) e logo `ldr w9,[x8,#0xc]`
 * (lê a classe/type do objeto) SEM null-check. Na cena do MAPA várias referências de
 * objeto não resolvem (PPtr -> NULL) -> *arg0 == NULL -> x8=0 -> SIGSEGV fault=0xc em
 * 0x542258. Pula a função quando *arg0==NULL (o objeto null não é processado; mesmo
 * espírito do SCENESKIP). Caller (0x542474) usa o retorno como ponteiro/flag -> 0 é seguro
 * (= "sem tipo/sem objeto"). */
static long (*deser542_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_deserguard_hits;
static long deser542_hook(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  if (a0 == 0 || *(void **)a0 == NULL) {
    if (g_deserguard_hits < 8)
      fprintf(stderr, "[DESERGUARD] 0x54220c *arg0=NULL -> skip (f=%d)\n", g_render_frame);
    g_deserguard_hits++;
    return 0;
  }
  return deser542_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}
/* ===== CUP_SCENESPY / CUP_SETACTIVE (s14): SceneManager nativo =====
 * RE (icall table libunity): INTERNAL_CALL_GetActiveScene=0x1bb414, SetActiveScene=
 * 0x1bb44c -> setter real 0x875dc4(mgr, scene); MoveGameObjectToScene=0x1bbc68.
 * Singleton SceneManager = [libunity+0x12bc850]. Cena ativa = [mgr+0x48]; fallback
 * do GetActiveScene = ÚLTIMA da lista (array de ptrs [mgr+0x50], count [mgr+0x60]).
 * UnityScene: nome std::string SSO (+0x38 ptr de dados; ==NULL -> inline em +0x40),
 * estado +0x9c (2 = loaded; SetActiveScene exige ==2).
 * Hipótese do player-fantasma do mapa: Object.Instantiate (Map.Awake/CreateUI) dá a
 * cena ATIVA aos clones; se o mgr não tem cena registrada/ativa no so-loader, o
 * Transform do clone fica com {hierarchy P, idx} = NULL em [+0x38/+0x40] -> SCENESKIP
 * o esconde -> player invisível. SCENESPY mede; SETACTIVE conserta ([mgr+0x48]==NULL
 * com cena loaded na lista -> chama o setter real). */
static void scenespy_dump(const char *tag) {
  if (!g_unity_base) return;
  void *mgr = *(void **)(g_unity_base + 0x12bc850);
  if (!mgr) { fprintf(stderr, "[SCENESPY:%s] mgr=NULL\n", tag); fsync(2); return; }
  void *active = *(void **)((char *)mgr + 0x48);
  void **arr = *(void ***)((char *)mgr + 0x50);
  long cnt = *(long *)((char *)mgr + 0x60);
  fprintf(stderr, "[SCENESPY:%s] mgr=%p active=%p count=%ld f=%d\n", tag, mgr, active, cnt, g_render_frame);
  for (long i = 0; i < cnt && i < 8 && arr; i++) {
    char *sc = (char *)arr[i];
    if (!sc) { fprintf(stderr, "[SCENESPY:%s]  cena[%ld]=NULL\n", tag, i); continue; }
    char *nm = *(char **)(sc + 0x38); if (!nm) nm = sc + 0x40;
    fprintf(stderr, "[SCENESPY:%s]  cena[%ld]=%p state=%d nome=%.48s%s\n", tag, i, sc,
            *(int *)(sc + 0x9c), nm, sc == active ? " (ATIVA)" : "");
  }
  fsync(2);
}
static volatile uint32_t g_setactive_n;
static void setactive_fix(void) {
  if (!g_unity_base) return;
  void *mgr = *(void **)(g_unity_base + 0x12bc850);
  if (!mgr) return;
  void *active = *(void **)((char *)mgr + 0x48);
  void **arr = *(void ***)((char *)mgr + 0x50);
  long cnt = *(long *)((char *)mgr + 0x60);
  if (active || !arr || cnt <= 0) return;
  /* última cena loaded (state==2) — mesma escolha do fallback do GetActiveScene */
  for (long i = cnt - 1; i >= 0; i--) {
    char *sc = (char *)arr[i];
    if (!sc || *(int *)(sc + 0x9c) != 2) continue;
    int ok = ((int (*)(void *, void *))(g_unity_base + 0x875dc4))(mgr, sc);
    char *nm = *(char **)(sc + 0x38); if (!nm) nm = sc + 0x40;
    fprintf(stderr, "[SETACTIVE] cena[%ld]=%p (%.48s) -> SetActiveScene ret=%d (#%u f=%d)\n",
            i, sc, nm, ok, ++g_setactive_n, g_render_frame);
    fsync(2);
    return;
  }
}
static void bootspy_install(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0x9A55CC, (void *)bs_hook_0, (void **)&bs_orig_0, "Start"},
    {0x9A5828, (void *)bs_hook_1, (void **)&bs_orig_1, "OnSettingsDataLoaded"},
    {0xB73C60, (void *)bs_hook_2, (void **)&bs_orig_2, "LoadFromCloud"},
    {0xB2398C, (void *)bs_hook_3, (void **)&bs_orig_3, "LoadCloudData"},
    {0xB23EF0, (void *)bs_hook_4, (void **)&bs_orig_4, "OnlineMgr.Init"},
    {0xB73798, (void *)bs_hook_5, (void **)&bs_orig_5, "Settings.Save"},
    {0x9A5750, (void *)bs_hook_6, (void **)&bs_orig_6, "start_cr fac"},
    {0xB7422C, (void *)bs_hook_7, (void **)&bs_orig_7, "OnLoadedCloud"},
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) continue;
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
  }
  fprintf(stderr, "[BOOTSPY] %u hooks de boot instalados\n", (unsigned)(sizeof T / sizeof T[0]));
}

/* ===== CUP_MENUSPY: espião do SlotSelectScreen (menu principal pós-título) =====
 * O Update (0xAB4FF0) despacha pelo state(+0x50): se state==0 (InitializeStorage)
 * NÃO faz NADA (ret) — o menu renderiza mas ignora input até o save/storage init
 * completar: OnPlayerDataInitialized(success=true) seta dataStatus(+0x1C8)=1
 * (Received) -> Update inicia allDataLoaded_cr -> SetState(1=MainMenu). Logamos
 * cada elo p/ ver onde a cadeia para no so-loader. */
static long (*ms_orig_update)(void *);
static long ms_hook_update(void *self) {
  static int ls = -1, ld = -1;
  int st = *(int *)((char *)self + 0x50), ds = *(int *)((char *)self + 0x1C8);
  if (st != ls || ds != ld) {
    fprintf(stderr, "[MENUSPY] SlotSelect state=%d dataStatus=%d f=%d\n", st, ds, g_render_frame);
    fsync(2); ls = st; ld = ds;
  }
  return ms_orig_update(self);
}
static long (*ms_orig_setstate)(void *, int);
static long ms_hook_setstate(void *self, int v) {
  fprintf(stderr, "[MENUSPY] SetState(%d) f=%d\n", v, g_render_frame); fsync(2);
  return ms_orig_setstate(self, v);
}
static long (*ms_orig_pdata)(void *, int);
static long ms_hook_pdata(void *self, int ok) {
  fprintf(stderr, "[MENUSPY] OnPlayerDataInitialized(success=%d) f=%d\n", ok & 1, g_render_frame); fsync(2);
  return ms_orig_pdata(self, ok);
}
static long (*ms_orig_sdata)(void *, int);
static long ms_hook_sdata(void *self, int ok) {
  fprintf(stderr, "[MENUSPY] OnSettingsDataLoaded(success=%d) f=%d\n", ok & 1, g_render_frame); fsync(2);
  return ms_orig_sdata(self, ok);
}
static long (*ms_orig_awake)(void *);
static long ms_hook_awake(void *self) {
  fprintf(stderr, "[MENUSPY] SlotSelectScreen.Awake f=%d\n", g_render_frame); fsync(2);
  return ms_orig_awake(self);
}
static void menuspy_install(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0xAB4FF0, (void *)ms_hook_update,   (void **)&ms_orig_update,   "SlotSelect.Update"},
    {0xAB670C, (void *)ms_hook_setstate, (void **)&ms_orig_setstate, "SlotSelect.SetState"},
    {0xAB8868, (void *)ms_hook_pdata,    (void **)&ms_orig_pdata,    "OnPlayerDataInitialized"},
    {0xAB89A0, (void *)ms_hook_sdata,    (void **)&ms_orig_sdata,    "OnSettingsDataLoaded"},
    {0xAB4BA4, (void *)ms_hook_awake,    (void **)&ms_orig_awake,    "SlotSelect.Awake"},
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) continue;
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
  }
  fprintf(stderr, "[MENUSPY] hooks SlotSelectScreen instalados\n"); fsync(2);
}

/* ===== CUP_STAGESPY (s14c): por que o conteúdo da fase (boss/cenário) não aparece? =====
 * Fase: player+chão+céu renderizam, boss+cenário FALTAM; só 29 draws/frame; 1 HIERFIX
 * (≠ problema do mapa). atlas_veggieslevel deployado. Pergunta-chave: os sprites do boss
 * estão sendo ATRIBUÍDOS aos renderers (=> problema é render/Mali) ou NÃO (=> load async
 * da fase não completa)? Hook decisivo: SpriteRenderer.set_sprite (il2cpp 0x178EB3C) —
 * conta atribuições e quantas com sprite NULL. + AssetBundle.LoadAssetAsync (0x17C893C) —
 * loga o que a fase pede async (se nunca completa, o sprite nunca é setado). */
static long (*ss_setsprite_orig)(void *, void *);
static volatile uint32_t g_ss_set, g_ss_null;
static long ss_setsprite_hook(void *self, void *sprite) {
  g_ss_set++;
  if (!sprite) g_ss_null++;
  return ss_setsprite_orig(self, sprite);
}
static long (*ss_loadasync_orig)(void *, void *, void *);
static volatile uint32_t g_ss_async;
static long ss_loadasync_hook(void *self, void *name, void *type) {
  g_ss_async++;
  if (g_ss_async < 60 && name) {
    /* il2cpp String: len@+0x10 (int), chars utf16@+0x14 */
    int len = *(int *)((char *)name + 0x10);
    unsigned short *u = (unsigned short *)((char *)name + 0x14);
    char buf[128]; int n = 0;
    for (int i = 0; i < len && n < (int)sizeof buf - 1; i++)
      buf[n++] = (u[i] < 128) ? (char)u[i] : '?';
    buf[n] = 0;
    fprintf(stderr, "[STAGESPY] LoadAssetAsync(\"%s\") #%u f=%d\n", buf, g_ss_async, g_render_frame);
    fsync(2);
  }
  return ss_loadasync_orig(self, name, type);
}
static void stagespy_install(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0x178EB3C, (void *)ss_setsprite_hook, (void **)&ss_setsprite_orig, "SpriteRenderer.set_sprite"},
    {0x17C893C, (void *)ss_loadasync_hook, (void **)&ss_loadasync_orig, "AssetBundle.LoadAssetAsync"},
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) { fprintf(stderr, "[STAGESPY] tramp %s falhou\n", T[i].nm); continue; }
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
  }
  fprintf(stderr, "[STAGESPY] hooks instalados (set_sprite + LoadAssetAsync)\n"); fsync(2);
}

/* ===== CUP_TAPINPUT: pulsa AnyPlayerInput.GetAnyButtonDown (il2cpp 0xCC2854) =====
 * A coroutine WaitForUserInputBeforeContinue do disclaimer espera
 * WaitUntil(() => AnyPlayerInput.GetAnyButtonDown()). Sem plumbing real de input,
 * fica preso. Hookamos o método: retorna TRUE em janelas periódicas (~3 frames a
 * cada CUP_TAPPERIOD frames) — equivale a um "toque" que destrava o disclaimer e
 * confirma menus, mas sem ficar true p/ sempre (evita auto-navegar descontrolado).
 * CUP_TAPSTART=frame inicial (default 200, dá tempo do disclaimer subir). */
static int g_tap_period = 240, g_tap_start = 200, g_tap_width = 3;
uintptr_t g_tapinput_cont = 0;
__asm__(
  ".text\n"
  ".global tapinput_tramp\n"
  "tapinput_tramp:\n"
  "  stp x28, x27, [sp, #-96]!\n"    /* 0xCC2854 */
  "  stp x26, x25, [sp, #16]\n"      /* 0xCC2858 */
  "  stp x24, x23, [sp, #32]\n"      /* 0xCC285C */
  "  stp x22, x21, [sp, #48]\n"      /* 0xCC2860 */
  "  adrp x17, g_tapinput_cont\n"
  "  add x17, x17, :lo12:g_tapinput_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern int tapinput_tramp(void *self);
int my_getanybuttondown(void *self);
int my_getanybuttondown(void *self) {
  int f = g_render_frame;
  if (f >= g_tap_start) {
    int ph = (f - g_tap_start) % g_tap_period;
    if (ph < g_tap_width) {
      static int lastf = -1;
      if (f != lastf) { fprintf(stderr, "[TAPINPUT] pulse f=%d\n", f); fsync(2); lastf = f; }
      return 1;
    }
  }
  return tapinput_tramp(self);
}

/* ===== CUP_SAPATH: override Application.get_streamingAssetsPath (il2cpp 0x17C7C1C) =====
 * No so-loader o getter retorna "jar:file://!" (caminho do APK vazio) -> os
 * AssetBundles do título (AssetBundle.LoadFromFile(streamingAssetsPath+"/AssetBundles/"+n))
 * falham com "Unable to open archive file" -> NullReferenceException mata a coroutine
 * de boot. Apontamos p/ um diretório REAL do filesystem (CUP_SAPATH=/storage/cuphead-sa)
 * onde deployamos os bundles -> LoadFromFile abre o arquivo de verdade.
 * Cria a string il2cpp 1× via il2cpp_string_new (não chama o original). */
static void *g_sa_string = NULL;
void *my_streamingAssetsPath(void);
void *my_streamingAssetsPath(void) {
  if (!g_sa_string && g_il2cpp_base) {
    void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x1b62c38); /* il2cpp_string_new */
    const char *p = getenv("CUP_SAPATH"); if (!p) p = "/storage/cuphead-sa";
    g_sa_string = isn(p);
    fprintf(stderr, "[SAPATH] streamingAssetsPath -> \"%s\" (il2cpp str=%p)\n", p, g_sa_string);
    fsync(2);
  }
  return g_sa_string;
}
/* AssetBundleLoader.getBasePath(location) (il2cpp 0x1031C8C): p/ StreamingAssets usa
 * streamingAssetsPath (já overridado), mas p/ DLC (location=1) usa OUTRA fonte que no
 * so-loader retorna string-lixo ("Шестигранные врата 1") → o load de DLC persistente
 * falha. Override: retorna SEMPRE o nosso path real (ignora location). */
void *my_getbasepath(int location);
void *my_getbasepath(int location) {
  (void)location;
  return my_streamingAssetsPath();  /* mesmo dir; loader anexa "/AssetBundles/"+nome */
}

static char g_dl_sl; /* sentinela do handle de libOpenSLES (FMOD → opensles_shim) */
static void *my_dlopen(const char *nm, int flag) {
  if (g_dllog) fprintf(stderr, "[dlopen] \"%s\"\n", nm ? nm : "(null)");
  /* il2cpp: nosso modulo ja' carregado (F1). Casa "il2cpp" em qualquer forma. */
  if (nm && strstr(nm, "il2cpp")) { fprintf(stderr, "[DLOPEN] %s -> il2cpp module\n", nm); return &g_dl_il2cpp; }
  /* FMOD (audio do Unity) faz dlopen("libOpenSLES.so") em runtime. CUP_NOSL=1
     desliga o shim (volta ao estado imagem-OK: FMOD cai no null output). */
  if (nm && strstr(nm, "OpenSLES") && !getenv("CUP_NOSL")) {
    fprintf(stderr, "[DLOPEN] %s -> opensles_shim\n", nm); return &g_dl_sl; }
  if (!nm || !nm[0] || strstr(nm, "libc") || strstr(nm, "libunity") || strstr(nm, "libmain"))
    return &g_dl_self;
  void *h = dlopen(nm, flag); return h ? h : &g_dl_self;
}
static void *my_dlsym(void *h, const char *nm) {
  if (!nm) return NULL;
  if (g_dllog) fprintf(stderr, "[dlsym] h=%p \"%s\"\n", h, nm);
  if (!strcmp(nm, "glGetString")) return (void *)my_glGetString;
  if (g_drawspy && nm[0] == 'g' && nm[1] == 'l') {   /* cobre resolução de gl* via dlsym tb */
    void *p = dlsym(RTLD_DEFAULT, nm);
    void *w = ds_route(nm, p);
    if (w != p) return w;
  }
  if (getenv("CUP_SHLOG")) {
    if (!strcmp(nm, "glCompileShader")) return (void *)my_glCompileShader;
    if (!strcmp(nm, "glLinkProgram")) return (void *)my_glLinkProgram;
  }
  if (nm[0] == 'e' && nm[1] == 'g' && nm[2] == 'l') { void *p = egl_route(nm); if (p) return p; }
  /* AUDIO: dlsym do handle de libOpenSLES -> opensles_shim (slCreateEngine + SL_IID_*
     com as identidades DO SHIM — ele compara ponteiro, receita re4/Dysmantle) */
  if (h == &g_dl_sl) {
    fprintf(stderr, "[DLSYM:SL] pede \"%s\"\n", nm);
    if (!strcmp(nm, "slCreateEngine")) return (void *)slCreateEngine_shim;
    if (!strcmp(nm, "SL_IID_ENGINE")) return (void *)&sl_IID_ENGINE;
    if (!strcmp(nm, "SL_IID_PLAY")) return (void *)&sl_IID_PLAY;
    if (!strcmp(nm, "SL_IID_VOLUME")) return (void *)&sl_IID_VOLUME;
    if (!strcmp(nm, "SL_IID_BUFFERQUEUE") || !strcmp(nm, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
      return (void *)&sl_IID_BUFFERQUEUE;
    if (!strcmp(nm, "SL_IID_EFFECTSEND")) return (void *)&sl_IID_EFFECTSEND;
    if (!strcmp(nm, "SL_IID_ANDROIDCONFIGURATION")) return (void *)&sl_IID_ANDROIDCONFIGURATION;
    if (!strcmp(nm, "SL_IID_ENGINECAPABILITIES")) return (void *)&sl_IID_ENGINECAPABILITIES;
    if (!strcmp(nm, "SL_IID_ENVIRONMENTALREVERB")) return (void *)&sl_IID_ENVIRONMENTALREVERB;
    /* s14: o FMOD resolve TODOS os SL_IID_* de antemão e ABORTA o init se qualquer
       um vier NULL (RECORD, MIDI...). Identidade genérica única por nome — os
       GetInterface dos objetos do shim devolvem stub-success p/ IID desconhecido. */
    if (!strncmp(nm, "SL_IID_", 7)) {
      static struct { char name[48]; void *id; } gen[24];
      static void *slots[24];
      for (int i = 0; i < 24; i++) {
        if (gen[i].name[0] && !strcmp(gen[i].name, nm)) return (void *)&gen[i].id;
        if (!gen[i].name[0]) {
          snprintf(gen[i].name, sizeof gen[i].name, "%s", nm);
          gen[i].id = &slots[i];
          fprintf(stderr, "[DLSYM:SL] %s -> identidade generica\n", nm);
          return (void *)&gen[i].id;
        }
      }
    }
    fprintf(stderr, "[DLSYM:SL] %s -> NULL\n", nm);
    return NULL;
  }
  /* qualquer simbolo il2cpp_* resolve no modulo il2cpp (qualquer handle) */
  if (!strncmp(nm, "il2cpp", 6) && g_m_il2cpp) {
    so_module *c = so_save(); so_use(g_m_il2cpp);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:il2cpp*] %s -> %p\n", nm, p);
    return p;
  }
  if (h == &g_dl_il2cpp && g_m_il2cpp) {
    so_module *c = so_save(); so_use(g_m_il2cpp);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:il2cpp] %s -> %p\n", nm, p);
    return p;
  }
  if (h == &g_dl_self) {
    void *p = (void *)so_find_addr_safe(nm);
    if (!p && g_m_il2cpp) { so_module *c = so_save(); so_use(g_m_il2cpp); p = (void *)so_find_addr_safe(nm); so_use(c); free(c); }
    if (!p) p = dlsym(RTLD_DEFAULT, nm);
    return p;
  }
  return dlsym(h, nm);
}
static const char *my_dlerror(void) { return NULL; }
static int my_dladdr(const void *a, void *i) { (void)a; (void)i; return 0; }
static int my_dlclose(void *h) { (void)h; return 0; }

/* ---------- TLS bridge (bionic keys -> slots nossos; 1 glibc key) ---------- */
#define NSLOT 1024
static pthread_key_t g_tls_base; static int g_tls_init = 0;
static int g_slot_next = 1; static pthread_mutex_t g_slot_mtx = PTHREAD_MUTEX_INITIALIZER;
static void tls_dtor(void *p) { free(p); }
static void **tls_slots(void) {
  if (!g_tls_init) { pthread_key_create(&g_tls_base, tls_dtor); g_tls_init = 1; }
  void **s = (void **)pthread_getspecific(g_tls_base);
  if (!s) { s = (void **)calloc(NSLOT, sizeof(void *)); pthread_setspecific(g_tls_base, s); }
  return s;
}
static int sh_key_create(unsigned *k, void (*d)(void *)) { (void)d; pthread_mutex_lock(&g_slot_mtx);
  int n = g_slot_next++; pthread_mutex_unlock(&g_slot_mtx); if (n >= NSLOT) return 11; *k = (unsigned)n; return 0; }
static int sh_key_delete(unsigned k) { (void)k; return 0; }
static void *sh_getspecific(unsigned k) { if ((int)k <= 0 || (int)k >= NSLOT) return NULL; return tls_slots()[(int)k]; }
static int sh_setspecific(unsigned k, const void *v) { if ((int)k <= 0 || (int)k >= NSLOT) return 22; tls_slots()[(int)k] = (void *)v; return 0; }

/* ---------- abort/raise/tgkill: loga o CALLER (achar a origem do fatal) ---------- */
static void map_caller(const char *tag, uintptr_t ra) {
  if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + 0x2000000)
    fprintf(stderr, "%s caller=libunity+0x%lx\n", tag, ra - g_unity_base);
  else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000)
    fprintf(stderr, "%s caller=libil2cpp+0x%lx\n", tag, ra - g_il2cpp_base);
  else fprintf(stderr, "%s caller=0x%lx (?)\n", tag, ra);
  fflush(stderr);
}
static int my_raise(int sig) { map_caller("[RAISE]", (uintptr_t)__builtin_return_address(0));
  fprintf(stderr, "[RAISE] sig=%d\n", sig); if (getenv("CUP_NORAISE")) return 0; return raise(sig); }
static void my_abort(void) { map_caller("[ABORT]", (uintptr_t)__builtin_return_address(0));
  if (getenv("CUP_NORAISE")) return; abort(); }
static int my_tgkill(int tgid, int tid, int sig) { map_caller("[TGKILL]", (uintptr_t)__builtin_return_address(0));
  fprintf(stderr, "[TGKILL] sig=%d\n", sig); if (getenv("CUP_NORAISE")) return 0; return syscall(__NR_tgkill, tgid, tid, sig); }

/* __stack_chk_fail: o operator-new tagueado (0x3cbf2c) tem canario; numa chamada
 * do RecreateGfxState ele falha -> abort. Neutraliza p/ diagnosticar (loga caller). */
static int g_scf_n = 0;
static void my_stack_chk_fail(void) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  if (g_scf_n++ == 0) {
    fprintf(stderr, "\n[SCF] __stack_chk_fail caller=%lx", ra);
    if (g_alloc_ub && ra >= g_alloc_ub && ra < g_alloc_ub + 0x11e6000)
      fprintf(stderr, " (libunity+0x%lx)", ra - g_alloc_ub);
    fprintf(stderr, "\n[SCF] stack scan (callers unity FORA do operator-new):\n");
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    for (int k = 0, hits = 0; k < 800 && hits < 30; k++) {
      uintptr_t v = *(uintptr_t *)(sp + k * 8);
      if (g_alloc_ub && v >= g_alloc_ub && v < g_alloc_ub + 0x11e6000) {
        uintptr_t off = v - g_alloc_ub;
        const char *tag = (off >= 0x3cbe90 && off <= 0x3cc1a0) ? " [op-new]" : " <==";
        fprintf(stderr, "  sp+0x%04x libunity+0x%lx%s\n", k * 8, off, tag);
        hits++;
      } else if (g_alloc_ib && v >= g_alloc_ib && v < g_alloc_ib + 0x2325000) {
        fprintf(stderr, "  sp+0x%04x libil2cpp+0x%lx\n", k * 8, v - g_alloc_ib);
        hits++;
      }
    }
    fflush(stderr);
  }
  /* retorna em vez de abort */
}

/* ---------- _ctype_ (tabela BSD/bionic de classes de caractere) ----------
 * libunity (bionic) importa `_ctype_` — uma `const char*` que aponta p/ uma tabela
 * de 257 bytes; isalpha/isdigit/tolower fazem `_ctype_[(int)c+1] & BITS`. O glibc
 * NÃO exporta `_ctype_` → ficava UNRESOLVED (NULL) → o processamento de string do
 * engine (nomes de asset etc.) fazia `ldr [_ctype_]; ldr [x0]` = NULL deref (crash
 * libunity+0xe449d4 no asset loading). Provemos a tabela (preenchida via glibc) e
 * resolvemos o símbolo. Bits bionic: _U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80. */
#include <ctype.h>
static unsigned char g_ctype_table[257];
static const unsigned char *g_ctype_ptr = g_ctype_table;  /* _ctype_ aponta p/ a base; idx [c+1] */
static unsigned char g_tolower_table[257], g_toupper_table[257];
static const unsigned char *g_tolower_ptr = g_tolower_table, *g_toupper_ptr = g_toupper_table;
static void ctype_init(void) {
  g_ctype_table[0] = 0;                 /* slot do EOF (c=-1) */
  g_tolower_table[0] = 0; g_toupper_table[0] = 0;
  for (int c = 0; c < 256; c++) {
    unsigned char b = 0;
    if (isupper(c)) b |= 0x01;          /* _U */
    if (islower(c)) b |= 0x02;          /* _L */
    if (isdigit(c)) b |= 0x04;          /* _N */
    if (isspace(c)) b |= 0x08;          /* _S */
    if (ispunct(c)) b |= 0x10;          /* _P */
    if (iscntrl(c)) b |= 0x20;          /* _C */
    if (isxdigit(c) && !isdigit(c)) b |= 0x40; /* _X (só hex-letra; dígitos já têm _N) */
    if (c == ' ')  b |= 0x80;           /* _B (blank imprimível) */
    g_ctype_table[c + 1] = b;
    g_tolower_table[c + 1] = (unsigned char)tolower(c);
    g_toupper_table[c + 1] = (unsigned char)toupper(c);
  }
}
/* resolve _ctype_/_tolower_tab_/_toupper_tab_ na GOT do módulo ATIVO. A GOT guarda
 * o ENDEREÇO da variável-ponteiro (code: ldr [got]→ptr_var; ldr [ptr_var]→tabela). */
static void ctype_resolve(void) {
  so_patch_got("_ctype_", (uintptr_t)&g_ctype_ptr);
  so_patch_got("_tolower_tab_", (uintptr_t)&g_tolower_ptr);
  so_patch_got("_toupper_tab_", (uintptr_t)&g_toupper_ptr);
}

/* ---------- helper: override import na tabela ---------- */
static void set_import(const char *name, void *fn) {
  for (size_t i = 0; i < dynlib_numfunctions; i++)
    if (!strcmp(dynlib_functions[i].symbol, name)) { dynlib_functions[i].func = (uintptr_t)fn; return; }
}

/* patch_got: sobrescreve o slot da GOT DIRETO (apos so_resolve). Necessario p/
 * simbolos que NAO estao em dynlib_functions (NDK: ANativeWindow_*, __android_log_*,
 * ASensor*, ...) — p/ esses set_import e' no-op e ficam UNRESOLVED com GOT lixo. */
static int patch_got(const char *name, void *fn) {
  int n = so_patch_got(name, (uintptr_t)fn);
  if (!n) fprintf(stderr, "[GOT] %s: 0 slots (nao achado)\n", name);
  return n;
}

static void *volatile g_preload_mgr;  /* PreloadManager capturado pelo spy (CUP_PSPY) */

/* ===== CUP_SCENEGUARD (s12): crash casa→mapa =====
 * Instantiate na cena do mapa chega em libunity 0x541c9c (resolve {scene,idx}
 * do GameObject via helper 0x8f7c48 = ldp x8,x1,[GO+0x38]) e deref [scene+24]
 * SEM null-check. GO destruído/fora-de-cena tem scene NULL → SIGSEGV fault=0x18
 * (dump s11: x0=0 x1=0xffffffffff). Guard = ilha de código substituindo o bl;
 * hits contados aqui e logados no render loop. */
static volatile uint32_t g_sceneguard_hits;
static uint64_t sg_fake_scene[8];   /* [3] (+24) -> sg_fake_arr (handle w27=0) */
static uint32_t sg_fake_arr[16];

/* stubs NDK no-op (sensores/looper/profiler google) — devolvem 0/NULL */
static long ndk_stub0(void) { return 0; }

extern int my_sigaction();  /* bionic_shims.c (ABI sigset bionic/glibc) */

/* thread de áudio do FMOD (output AudioTrack Java): o Cuphead registra
   org.fmod.FMODAudioDevice.fmodProcess(ByteBuffer) e espera uma thread Java
   chamá-la em loop p/ o mixer avançar. Sem JVM, replicamos em C: assim que
   fmodProcess existir, chamamos com nosso ByteBuffer a cada ~10ms. Isso destrava
   o boot (a main fica presa no nativeRender esperando o áudio progredir). */
static void *g_fmod_env;
static volatile int g_fmod_run = 1;
static volatile int g_fmod_stopped = 1;

/* ---- CUP_PSPY: espião do PreloadManager (muro frame 111) ----
 * RE (libunity, offsets confirmados no disasm):
 *   [mgr+208/+224] = fila de PRELOAD  (array/count) — consumida pela thread
 *                    UnityPreload (entry 0x8736cc): roda op->vt[+80] em bg e
 *                    seta op[72]=1; move a op p/ fila de integração (push 0x873790).
 *   [mgr+240/+256] = fila de INTEGRAÇÃO (array/count) — consumida pela main em
 *                    UpdatePreloadingSingleStep (0x8733a8): chama op->vt[+88]
 *                    (timesliced); o POP ([+256]-- em 0x873500) SÓ acontece se
 *                    vt[+88] retornar true E op[72]==1.
 *   HasPendingOperations (0x8739c4) = lock(mgr+0x78); [+224]||[+256]; unlock.
 *   WaitForAllAsyncOperationsToComplete (0x873a90) gira enquanto pendente.
 * O spy substitui 0x8739c4 por uma réplica C (mesma semântica, mesmos locks
 * 0x8f5d0c/0x8f5d14) que captura o ponteiro do mgr; uma thread despeja as filas
 * (op, vtable, flags, alvos de vt[+80/+88/+112]) p/ identificar a op presa. */
#define UN_HASPEND   0x8739c4
#define UN_MTX_LOCK  0x8f5d0c
#define UN_MTX_UNLK  0x8f5d14
static volatile unsigned long g_haspend_calls;   /* quantas vezes a loading-screen consultou */
static volatile int g_haspend_stalled;            /* filas presas (mesmas ops) — gate da bg thread */
static int my_haspending(void *mgr) {
  g_preload_mgr = mgr;
  g_haspend_calls++;
  void (*lk)(void *) = (void (*)(void *))(g_unity_base + UN_MTX_LOCK);
  void (*ul)(void *) = (void (*)(void *))(g_unity_base + UN_MTX_UNLK);
  lk((char *)mgr + 0x78);
  uintptr_t pq = *(uintptr_t *)((char *)mgr + 224);
  uintptr_t iq = *(uintptr_t *)((char *)mgr + 256);
  uintptr_t pq_a = *(uintptr_t *)((char *)mgr + 208);
  uintptr_t iq_a = *(uintptr_t *)((char *)mgr + 240);
  uintptr_t pop = (pq && pq_a) ? ((uintptr_t *)pq_a)[0] : 0;
  uintptr_t iop = (iq && iq_a) ? ((uintptr_t *)iq_a)[0] : 0;
  ul((char *)mgr + 0x78);
  int pending = (pq || iq) ? 1 : 0;
  /* CUP_HASPEND_STALE=N: se as filas ficam IDÊNTICAS (mesmas ops, mesma contagem)
   * por N consultas seguidas, a(s) op(s) estão PRESAS — done72 nunca vira 1 (a thread
   * UnityPreload não processa a op no so-loader; ver 0x873900). O BOOT tolera essa op
   * persistente (não espera HasPendingOperations==0), mas a tela de loading do MAPA
   * espera -> trava eterna. Reportamos "sem pendências" p/ destravar a loading.
   * Só dispara em fila ESTÁVEL (sem progresso): se as ops mudam, é load real -> verdade. */
  static long stale_thr = -1, skip_thr = -1;
  if (stale_thr == -1) {
    /* detecção do stall: gate da bg thread (CUP_PRELOAD_BG). Limiar baixo p/ a bg
       kickar logo que o mapa trava; no boot/gameplay as filas FLUEM (ops entram/saem)
       -> nunca fica idêntico por tanto tempo -> bg fica ociosa, sem corromper o boot. */
    stale_thr = getenv("CUP_BG_STALL") ? atol(getenv("CUP_BG_STALL")) : 24;
    /* return-0 (pula a espera) — só se CUP_HASPEND_STALE setado (fallback bruto;
       deixa objetos meio-construídos -> use PRELOAD_BG preferencialmente) */
    skip_thr = getenv("CUP_HASPEND_STALE") ? atol(getenv("CUP_HASPEND_STALE")) : 0;
  }
  if (pending) {
    static uintptr_t last_pq, last_iq, last_pop, last_iop;
    static long stall;
    static int logged;
    if (pq == last_pq && iq == last_iq && pop == last_pop && iop == last_iop) {
      stall++;
      if (stall >= stale_thr && !g_haspend_stalled) {
        g_haspend_stalled = 1;
        fprintf(stderr, "[HASPEND] filas PRESAS (pq=%lu iq=%lu pop=%lx iop=%lx, %ld "
                "consultas) -> bg thread liberada\n", pq, iq, pop, iop, stall);
        dbg_sync();
      }
      if (skip_thr > 0 && stall >= skip_thr) {
        if (!logged) { logged = 1;
          fprintf(stderr, "[HASPEND] -> reportando SEM pendencias (CUP_HASPEND_STALE)\n");
          dbg_sync(); }
        return 0;
      }
    } else {
      stall = 0; logged = 0; g_haspend_stalled = 0;
      last_pq = pq; last_iq = iq; last_pop = pop; last_iop = iop;
    }
  } else {
    g_haspend_stalled = 0;
  }
  return pending;
}
static void pspy_dump_op(const char *qn, unsigned i, uintptr_t op) {
  uintptr_t vt = *(uintptr_t *)op;
  uintptr_t vt_off = (vt >= g_unity_base) ? vt - g_unity_base : vt;
  uintptr_t f80 = *(uintptr_t *)(vt + 80), f88 = *(uintptr_t *)(vt + 88);
  uintptr_t f112 = *(uintptr_t *)(vt + 112);
  fprintf(stderr,
          "[PSPY]  %s[%u] op=%lx vt=u+0x%lx done72=%d w64=%d w68=%d "
          "bg(+80)=u+0x%lx integ(+88)=u+0x%lx q(+112)=u+0x%lx\n",
          qn, i, op, vt_off, *(int *)(op + 72), *(int *)(op + 64),
          *(int *)(op + 68), f80 - g_unity_base, f88 - g_unity_base,
          f112 - g_unity_base);
  /* primeiros 0xC0 bytes da op (estado interno: progress/flags/ponteiros) */
  for (int k = 0; k < 24; k += 4)
    fprintf(stderr, "[PSPY]   +%02x: %016lx %016lx %016lx %016lx\n", k * 8,
            ((uintptr_t *)op)[k], ((uintptr_t *)op)[k + 1],
            ((uintptr_t *)op)[k + 2], ((uintptr_t *)op)[k + 3]);
}
/* CUP_PRELOAD_BG: a thread UnityPreload (entry 0x8736cc) processa o BACKGROUND das
 * ops de preload — 0x873900 faz: pop op da fila, chama op->vt[10] e op->vt[14], e
 * seta done72=1. No so-loader essa thread NÃO bombeia (ops do load da cena do mapa
 * ficam com done72=0 -> objetos meio-desserializados, campos null -> crashes
 * 0x541cdc/0x8f9b1c/0x542258; E a integração na main BLOQUEIA esperando o bg da PQ op,
 * por isso CUP_DRAINPRELOAD pendurava). Imitamos a thread faltante: chamamos
 * 0x873900(mgr) em loop. A função trava internamente mgr+0x78 (thread-safe, igual à
 * original); roda numa thread SEPARADA p/ não pendurar o render se uma op bloquear. */
static void *preload_bg_thread(void *arg) {
  (void)arg;
  int (*bg)(void *) = (int (*)(void *))(g_unity_base + 0x873900);
  fprintf(stderr, "[PRELOAD_BG] thread ativa (imita UnityPreload 0x873900)\n");
  unsigned long n = 0;
  /* só processa quando a loading-screen ESTÁ presa (g_haspend_stalled) — no boot
   * o engine bombeia as ops normalmente e processar em paralelo CORROMPE/trava
   * (render congelava em 1860). A detecção de stall só dispara no load do mapa
   * (filas idênticas por N consultas), nunca no boot (ops fluem).
   * CUP_BG_AFTER=N: gate ALTERNATIVO por frame — processa tb se g_render_frame>N
   * (p/ testar drenar a cena aditiva do mapa que NÃO gera stall detectável). */
  long bg_after = getenv("CUP_BG_AFTER") ? atol(getenv("CUP_BG_AFTER")) : 0;
  while (g_fmod_run) {
    void *m = g_preload_mgr;
    int active = g_haspend_stalled || (bg_after > 0 && g_render_frame > bg_after);
    if (m && active) {
      int did = bg(m);   /* processa 1 op de background; !=0 = fez trabalho */
      if (did) {
        if ((n++ % 16) == 0)
          fprintf(stderr, "[PRELOAD_BG] processou op presa (#%lu, f=%d)\n", n, g_render_frame);
        continue;        /* mais ops pendentes? drena sem dormir */
      }
    }
    usleep(2000);
  }
  return NULL;
}
static void *preload_spy_thread(void *arg) {
  (void)arg;
  fprintf(stderr, "[PSPY] thread ativa (2s)\n");
  while (g_fmod_run) {
    sleep(2);
    char *m = (char *)g_preload_mgr;
    if (!m) continue;
    /* leitura SEM lock (racy, mas nunca bloqueia/deadlocka o diagnóstico) */
    uintptr_t pq_a = *(uintptr_t *)(m + 208), pq_n = *(uintptr_t *)(m + 224);
    uintptr_t iq_a = *(uintptr_t *)(m + 240), iq_n = *(uintptr_t *)(m + 256);
    /* job-scheduler global (libunity bss 0x12b9380; o gate da integração só passa
       quando estes 3 contadores estão <=0 — 0x6cdad0). +0x70=jobs / +0x168 / +0x16c */
    void *jm = *(void **)(g_unity_data + 0xd3380);
    if (jm) fprintf(stderr, "[PSPY] jobmgr=%p +70=%d +168=%d +16c=%d\n", jm,
                    *(int *)((char *)jm + 0x70), *(int *)((char *)jm + 0x168),
                    *(int *)((char *)jm + 0x16c));
    fprintf(stderr, "[PSPY] mgr=%p preloadQ=%lu integQ=%lu\n", m, pq_n, iq_n);
    for (unsigned i = 0; i < pq_n && i < 2; i++)
      if (((uintptr_t *)pq_a)[i]) pspy_dump_op("PQ", i, ((uintptr_t *)pq_a)[i]);
    for (unsigned i = 0; i < iq_n && i < 2; i++)
      if (((uintptr_t *)iq_a)[i]) pspy_dump_op("IQ", i, ((uintptr_t *)iq_a)[i]);
    dbg_sync();
  }
  return NULL;
}
/* TESTE CUP_PRELOAD_TICK: posta periodicamente os sems em que threads não-main
   bloqueiam (acorda a UnityPreload p/ processar o item pendente da fila). */
static void *preload_tick_thread(void *arg) {
  (void)arg;
  fprintf(stderr, "[TICK] thread de preload-tick ativa (16ms)\n");
  while (g_fmod_run) { sh_tick_preload(); usleep(16000); }
  return NULL;
}
/* CUP_FORCESL: substitui a decisão de backend de áudio do FMOD (0x350298) */
static long forcesl_hook(void) { return 2; }   /* 2 = OpenSL */
/* CUP_FMODSPY (s14): loga o retorno das etapas do init do FMOD p/ achar QUAL
 * subsistema falha (output OpenSL passa: engine+player+8 enqueues+start ok, e o
 * 3->1 é teardown). 0xa6281c=System::init; 0xa6dbe0=output_opensl init;
 * 0xa6e270=output start (SetPlayState PLAYING). */
static long (*fmod_init_orig)(long, long, long, long, long, long, long, long);
static long (*fmod_oinit_orig)(long, long, long, long, long, long, long, long);
static long (*fmod_ostart_orig)(long, long, long, long, long, long, long, long);
static long fmod_init_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  long r = fmod_init_orig(a, b, c, d, e, f, g, h);
  fprintf(stderr, "[FMODSPY] System::init(maxch=%ld flags=0x%lx extra=%lx) -> %ld\n", b, c, d, r);
  fsync(2);
  return r;
}
static long fmod_oinit_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  long r = fmod_oinit_orig(a, b, c, d, e, f, g, h);
  fprintf(stderr, "[FMODSPY] output_opensl.init -> %ld\n", r); fsync(2);
  return r;
}
static long fmod_ostart_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  long r = fmod_ostart_orig(a, b, c, d, e, f, g, h);
  fprintf(stderr, "[FMODSPY] output_opensl.start -> %ld\n", r); fsync(2);
  return r;
}
/* ===== CUP_FMODALLOCGUARD (s14, default ON com FORCESL) — SOM =====
 * O alocador do FMOD (0xa66e6c / 0xa66b74: pool, size w1, file x2, line w3) recebe
 * um pedido INSANO de ~4.29GB do **DSP de flange** (fmod_dsp_flange.cpp:172): o
 * cálculo do buffer de delay (samplerate*40ms / blocksize * canais * 2) estoura no
 * so-loader (um dos campos do mixer vem corrompido) -> o wrapper loga "System out of
 * memory (MemoryLabel: FMOD)" e ABORTA o engine (fatal, SIGTRAP no boot). O próprio
 * flange tem caminho de falha LIMPO: se a alocação retorna NULL (0xa47570 cbz ->
 * 0xa47658), ele retorna FMOD_ERR_MEMORY e o FMOD SEGUE sem o efeito de flange
 * (irrelevante p/ o jogo). Guard: pedidos > 100MB do FMOD -> NULL (sem chamar o
 * wrapper fatal). Os allocs normais (KB/poucos MB) passam direto. */
#define FMOD_ALLOC_SANE 0x6400000UL  /* 100 MB */
static long (*fmod_alloc_orig)(long, long, long, long, long, long, long, long);
static long fmod_alloc_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  if ((unsigned long)b > FMOD_ALLOC_SANE) {
    static int n; if (n++ < 6)
      fprintf(stderr, "[FMODGUARD] alloc insana %lu (file=%s line=%ld) -> NULL (flange skip)\n",
              (unsigned long)b, (c && *(char *)c) ? (char *)c : "?", d);
    fsync(2);
    return 0;  /* NULL: o caller (flange create) trata como ERR_MEMORY e segue */
  }
  return fmod_alloc_orig(a, b, c, d, e, f, g, h);
}
/* SIGUSR1: dump leve do backtrace da thread que recebe (acha endereços libunity/
 * il2cpp na pilha) e RETORNA (não mata). Diagnóstico de hang: manda SIGUSR1 e vê a
 * call chain do wait. Gateado por nada — só dispara quando o sinal chega. */
static void diag_handler(int sig, siginfo_t *si, void *uc_) {
  (void)sig; (void)si;
  ucontext_t *uc = (ucontext_t *)uc_;
  uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30], sp = uc->uc_mcontext.sp;
  uintptr_t ub = g_unity_base, ib = g_il2cpp_base;
  fprintf(stderr, "[DIAG] tid=%d pc=0x%lx lr=0x%lx", (int)syscall(SYS_gettid),
          (unsigned long)pc, (unsigned long)lr);
  if (ub && lr >= ub && lr < ub + text_size) fprintf(stderr, " lr=libunity+0x%lx", lr - ub);
  fprintf(stderr, "\n");
  int hits = 0;
  for (uintptr_t a = sp; a + 8 <= sp + 16384UL * 8 && hits < 60; a += 8) {
    if (!addr_readable(a)) break;
    uintptr_t v = *(uintptr_t *)a;
    if (ub && v >= ub && v < ub + text_size) { fprintf(stderr, "[DIAG]  libunity+0x%lx\n", v - ub); hits++; }
    else if (ib && v >= ib && v < ib + 0x3000000) { fprintf(stderr, "[DIAG]  libil2cpp+0x%lx\n", v - ib); hits++; }
  }
  fprintf(stderr, "[DIAG] --- fim (%d hits) ---\n", hits); fsync(2);
}
static long (*fmod_alloc2_orig)(long, long, long, long, long, long, long, long);
static long fmod_alloc2_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  if ((unsigned long)b > FMOD_ALLOC_SANE) {
    static int n; if (n++ < 6)
      fprintf(stderr, "[FMODGUARD] alloc2 insana %lu (file=%s line=%ld) -> NULL (flange skip)\n",
              (unsigned long)b, (c && *(char *)c) ? (char *)c : "?", d);
    fsync(2);
    return 0;
  }
  return fmod_alloc2_orig(a, b, c, d, e, f, g, h);
}
/* ---- TER_AUDIOSPY: espião do createSound do Unity-FMOD (wrapper @0x806cb4) ----
 * RE: AudioClip::CreateFMODSound (0x3cf...) chama System::createSound(sys, data, mode, exinfo,
 * &sound) = 0x806cb4 -> impl real 0x7bcf98. O log "Cannot create FMOD::Sound ... INTERNAL"
 * (code 33) vem daqui. Spy loga mode/exinfo/result p/ ver QUAIS sons falham (SFX sample vs
 * Music stream) e o código de erro exato. Gated; default OFF. */
static long (*cs_orig)(void *, void *, int, void *, void *);
/* TER_AUDIOSPY: hook do MIXER (0x805a94) — ground-truth de count(x2) + formato real do
   system ([output+0x60]). Loga 3× e segue. */
static long (*mix_orig)(void *, void *, int);
static long mix_hook(void *output, void *buf, int count) {
  static int n;
  if (n++ < 3) {
    void *sys = (output && addr_readable((uintptr_t)output + 0x60)) ? *(void **)((char *)output + 0x60) : NULL;
    if (sys && addr_readable((uintptr_t)sys + 0x800))
      fprintf(stderr, "[MIXSPY] count(x2)=%d output=%p sys=%p | 7c4=%u 7c8=%u 7d4=%u 7f4=%u 7f8=%u 97e8=%u\n",
              count, output, sys,
              *(uint32_t *)((char *)sys + 0x7c4), *(uint32_t *)((char *)sys + 0x7c8),
              *(uint32_t *)((char *)sys + 0x7d4), *(uint32_t *)((char *)sys + 0x7f4),
              *(uint32_t *)((char *)sys + 0x7f8), *(uint32_t *)((char *)sys + 0x97e8));
    dbg_sync();
  }
  return mix_orig(output, buf, count);
}
static int g_stream_fallback;
static long cs_hook(void *sys, void *data, int mode, void *exinfo, void *out) {
  long r = cs_orig(sys, data, mode, exinfo, out);
  static int nfail, nok;
  int openmem = (mode & 0x800) || (mode & 0x10000000);
  /* TER_STREAMFALLBACK: o open de STREAM (0x864f78) falha INTERNAL(33) no so-loader
     (maquinaria de stream/stream-thread). Refaz como SAMPLE não-streamado (mesma fonte
     via file-callback do Unity) -> a música carrega inteira na memória e toca. SFX (sem
     bit 0x80) não passam por aqui. */
  if (r != 0 && (mode & 0x80) && g_stream_fallback) {
    int m2 = mode & ~0x80;               /* tira CREATESTREAM */
    long r2 = cs_orig(sys, data, m2, exinfo, out);
    static int nf; if (nf++ < 8)
      fprintf(stderr, "[CSSPY] STREAM falhou(%ld) -> retry sample mode=0x%x -> %ld\n", r, (unsigned)m2, r2);
    dbg_sync();
    if (r2 == 0) return 0;
    r = r2;
  }
  if (r != 0) {  /* FALHA: dump detalhado do data source */
    if (nfail++ < 40) {
      char nm[80]; nm[0] = 0;
      if (data && !openmem && addr_readable((uintptr_t)data)) {
        const char *s = (const char *)data; int ok = 1;
        for (int i = 0; i < 70; i++) { char c = s[i]; if (!c) break; if (c < 9 || (unsigned char)c > 126) { ok = 0; break; } }
        if (ok) snprintf(nm, sizeof nm, "\"%.70s\"", s);
      }
      unsigned long d0 = (data && addr_readable((uintptr_t)data)) ? *(unsigned long *)data : 0;
      /* exinfo dump: cbsize@0 length@4 numchannels@? — campos crus p/ diagnóstico */
      fprintf(stderr, "[CSSPY] FAIL createSound mode=0x%x stream=%d openmem=%d data=%p name=%s d[0]=0x%lx exinfo=%p -> %ld\n",
              (unsigned)mode, (mode >> 7) & 1, openmem, data, nm, d0, exinfo, r);
      if (exinfo && addr_readable((uintptr_t)exinfo + 0x40))
        fprintf(stderr, "[CSSPY]   exinfo: cb=%u len=%u +8=%u +c=%u +10=%u +14=%u +18=%u +1c=0x%x +20=0x%lx +28=0x%lx\n",
                *(uint32_t *)((char *)exinfo + 0), *(uint32_t *)((char *)exinfo + 4),
                *(uint32_t *)((char *)exinfo + 8), *(uint32_t *)((char *)exinfo + 0xc),
                *(uint32_t *)((char *)exinfo + 0x10), *(uint32_t *)((char *)exinfo + 0x14),
                *(uint32_t *)((char *)exinfo + 0x18), *(uint32_t *)((char *)exinfo + 0x1c),
                *(unsigned long *)((char *)exinfo + 0x20), *(unsigned long *)((char *)exinfo + 0x28));
      dbg_sync();
    }
  } else if (nok++ < 4) {
    fprintf(stderr, "[CSSPY] ok createSound mode=0x%x stream=%d -> 0\n", (unsigned)mode, (mode >> 7) & 1);
  }
  return r;
}
/* ---- output do FMOD (AudioTrack Java) bombeado em C -> SDL -> Pulse/PipeWire/ALSA ----
 * RAIZ (RE libunity, fmodProcess @0x811378): a thread Java chamaria fmodProcess(ByteBuffer)
 * e escreveria no AudioTrack. fmodProcess: GetDirectBufferAddress -> mixa `blockSize` frames
 * (via 0x805a94) no buffer -> RETORNA 0 em SUCESSO (-1 se o output *0xc7c2f0 ainda é NULL).
 * BUG anterior: o pump só enfileirava se r>0; como fmodProcess SEMPRE retorna 0 no sucesso,
 * NUNCA mandávamos PCM (silêncio total mesmo com o mixer rodando). Fix: enfileirar quando
 * r==0; o nº de bytes preenchidos = blockSize*canais*2 (PCM interleaved s16), lido do struct
 * do FMOD System em runtime ([*(unity+0xc7c2f0)]+0x60 = system; +0x7f4=block, +0x7c4/+0x7c8=
 * rate/canais). Back-pressure (mantém ~6 blocos na fila do SDL) -> ritmo = consumo real-time;
 * fmodProcess É o clock do mixer (mixa só quando chamado). Backend SDL = auto (pulse/pipewire/
 * alsa) via device NULL — portável a qualquer device. */
static unsigned g_fmod_blk, g_fmod_rate, g_fmod_ch;
static int fmod_read_format(unsigned *blk, unsigned *rate, unsigned *ch, void **sysout) {
  uintptr_t outp = g_unity_base + 0xc7c2f0;
  if (!addr_readable(outp)) return 0;
  void *out = *(void **)outp;
  if (!out || !addr_readable((uintptr_t)out + 0x60)) return 0;
  void *sys = *(void **)((char *)out + 0x60);
  if (!sys || !addr_readable((uintptr_t)sys + 0x7f8)) return 0;
  if (blk)  *blk  = *(uint32_t *)((char *)sys + 0x7f4);
  /* RAIZ do "acelerado": a taxa do PCM entregue ao AudioTrack é [system+0x7d4]
     (= o que fmodGetInfo @0x8112b0 reporta como samplerate), NÃO [+0x7c4]. O +0x7c4
     é a taxa do device de saída; o mixer entrega no rate +0x7d4 (mobile costuma usar
     24000). Tocar 24000 como 44100 = ~1.8x rápido/agudo. */
  if (rate) *rate = *(uint32_t *)((char *)sys + 0x7d4);
  if (ch)   *ch   = *(uint32_t *)((char *)sys + 0x7c8);
  if (sysout) *sysout = sys;
  return 1;
}
static void *fmod_audio_thread(void *arg) {
  (void)arg;
  void *fp = NULL;
  while (g_fmod_run && !(fp = jni_find_native("fmodProcess"))) usleep(20000);
  if (!fp) { g_fmod_stopped = 1; return NULL; }
  static long fdev = 0xFAD;            /* this (FMODAudioDevice) fake */
  void *bb = jni_fmod_bytebuffer();
  void *pcm = jni_fmod_pcm();
  int pcmcap = jni_fmod_pcm_size();
  /* 🔑 MEDIR EMPIRICAMENTE quantos bytes o fmodProcess ESCREVE por chamada. O offset da struct do
     System (0xc7c2f0) é não-confiável (fmod_read_format sempre falhava). Mas fmodProcess MIXA um
     nº FIXO de bytes (blockSize*ch*2) no buffer, independente da capacidade. Medimos: pré-enchemos
     o buffer com sentinela 0xAB, chamamos fmodProcess (silêncio = zeros ≠ 0xAB → detectável) e
     achamos o último byte escrito. Enfileirar EXATAMENTE esse nº = clock do mixer casa com o
     playback (senão acelera/atrasa). Tomamos o MÁX sobre várias chamadas (robusto a 0xAB no tail). */
  unsigned rate = 44100, ch = 2, blk = 0, measured = 0;
  unsigned char *pcmb = (unsigned char *)pcm;
  int got = 0;
  for (int t = 0; g_fmod_run && t < 4000; t++) {
    memset(pcmb, 0xAB, pcmcap);
    int r = ((int (*)(void *, void *, void *))fp)(g_fmod_env, &fdev, bb);
    if (r == 0) {
      int last = -1;
      for (int i = pcmcap - 1; i >= 0; i--) if (pcmb[i] != 0xAB) { last = i; break; }
      if (last >= 0) { unsigned wb = (unsigned)(last + 1); if (wb > measured) measured = wb; got++; }
      if (got >= 60) break;   /* ~60 amostras p/ um máximo estável */
    }
    usleep(2000);
  }
  fprintf(stderr, "[AUDIO] MEDIDO: fmodProcess escreve ~%u bytes/chamada (amostras=%d, cap=%d)\n", measured, got, pcmcap);
  /* 🔑🔑 GROUND TRUTH do formato: fmodGetInfo(env,thiz,infoType) @libunity+0x8112b0 é a fn que o FMOD
     expõe p/ o Java montar o AudioTrack. Tipos: 0=SAMPLERATE, 1=blockSize(frames), 4=CHANNELS.
     RAIZ DO ÁUDIO RÁPIDO: o FMOD mixa a **24000 Hz** (mobile), mas o SDL estava a 44100 →
     44100/24000 = 1.84× acelerado. FIX = abrir o SDL na taxa/canais REAIS do fmodGetInfo. */
  { /* resolve por NOME (RegisterNatives), como o fmodProcess — vale p/ qualquer
       build; o RVA 0x8112b0 só é confiável no build de referência (no 301544
       esse endereço cai no meio de outra função -> SIGSEGV, fault=bytes de string). */
    int (*fgi)(void*, void*, int) = (int (*)(void*, void*, int))jni_find_native("fmodGetInfo");
    if (!fgi && ter_known_build())
      fgi = (int (*)(void*, void*, int))(g_unity_base + 0x8112b0);
    if (fgi) {
      for (int it = 0; it < 5; it++)
        fprintf(stderr, "[AUDIO] fmodGetInfo(%d) = %d\n", it, fgi(g_fmod_env, &fdev, it));
      int r0 = fgi(g_fmod_env, &fdev, 0), c4 = fgi(g_fmod_env, &fdev, 4);
      if (r0 >= 8000 && r0 <= 192000) rate = (unsigned)r0;   /* taxa real do mixer FMOD */
      if (c4 == 1 || c4 == 2) ch = (unsigned)c4;             /* canais reais */
    } else {
      fprintf(stderr, "[AUDIO] fmodGetInfo indisponivel neste build; "
                      "mantendo rate=%u ch=%u (override: TER_AUDIO_RATE/TER_AUDIO_CH)\n", rate, ch);
    }
  }
  if (getenv("TER_AUDIO_RATE")) rate = atoi(getenv("TER_AUDIO_RATE"));
  if (getenv("TER_AUDIO_CH"))   ch   = atoi(getenv("TER_AUDIO_CH"));
  if (rate < 8000 || rate > 192000) rate = 44100;
  if (ch != 1 && ch != 2) ch = 2;
  unsigned bytes;
  if (getenv("TER_AUDIO_FRAMES")) bytes = (unsigned)atoi(getenv("TER_AUDIO_FRAMES")) * ch * 2;
  else if (measured >= 256) bytes = (measured / (ch * 2)) * (ch * 2);   /* alinha ao frame */
  else bytes = 4096;   /* fallback se a medição falhar */
  if ((int)bytes > pcmcap) bytes = pcmcap;
  blk = bytes / (ch * 2);
  g_fmod_rate = rate; g_fmod_ch = ch; g_fmod_blk = blk;
  SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
  want.freq = rate; want.format = AUDIO_S16SYS; want.channels = ch; want.samples = 1024;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  /* back-pressure: colchão p/ absorver jitter de scheduling (o jogo ocupa os 4 núcleos do Mali). 6
     blocos default = continuidade SEM engasgo, latência ~256ms (validado ~perfeito). Tunável
     TER_AUDIO_BP. PRÉ-ENCHE a fila antes de despausar (sem corte no arranque). */
  unsigned bpblocks = getenv("TER_AUDIO_BP") ? (unsigned)atoi(getenv("TER_AUDIO_BP")) : 6;
  if (bpblocks < 2) bpblocks = 2; if (bpblocks > 64) bpblocks = 64;
  Uint32 target = bytes * bpblocks;
  unsigned long n = 0, fed = 0;
  for (unsigned i = 0; dev && i < bpblocks && g_fmod_run; i++) {   /* pré-enche */
    int r = ((int (*)(void *, void *, void *))fp)(g_fmod_env, &fdev, bb);
    if (r == 0) { SDL_QueueAudio(dev, pcm, bytes); fed += bytes; } else { usleep(3000); }
  }
  if (dev) SDL_PauseAudioDevice(dev, 0);
  fprintf(stderr, "[AUDIO] SDL dev=%d rate=%u ch=%u blk=%u bytes=%u bp=%u (have f=%d c=%d) err=%s\n",
          dev, rate, ch, blk, bytes, bpblocks, have.freq, have.channels, dev ? "" : SDL_GetError());
  while (g_fmod_run) {
    if (dev && SDL_GetQueuedAudioSize(dev) > target) { usleep(1000); continue; }
    int r = ((int (*)(void *, void *, void *))fp)(g_fmod_env, &fdev, bb);
    if (r == 0 && dev) { SDL_QueueAudio(dev, pcm, bytes); fed += bytes; }
    else if (r < 0) usleep(5000);      /* output ainda não pronto */
    if (n < 5 || n % 2000 == 0) {
      fprintf(stderr, "[AUDIO] fmodProcess #%lu -> %d (fed=%lu q=%u)\n",
              n, r, fed, dev ? SDL_GetQueuedAudioSize(dev) : 0); dbg_sync(); }
    n++;
  }
  if (dev) {
    SDL_PauseAudioDevice(dev, 1);
    SDL_ClearQueuedAudio(dev);
    SDL_CloseAudioDevice(dev);
  }
  g_fmod_stopped = 1;
  return NULL;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
  g_main_tid = (int)syscall(SYS_gettid);  /* p/ o sem_shim distinguir main de workers */
  if (getenv("CUP_SEMPOLL")) sh_sem_set_poll(atoi(getenv("CUP_SEMPOLL")));  /* polling do sem_wait */
  { extern void cond_set_poll(int);  /* polling do pthread_cond_wait (lost-wakeup futex) */
    if (getenv("CUP_CONDPOLL")) cond_set_poll(atoi(getenv("CUP_CONDPOLL"))); }

  /* log persistente: stderr -> debug.log (unbuffered + fsync nos marcos =
     sobrevive a hang/power-cycle do device). CUP_NOLOGFILE=1 desativa. */
  if (!getenv("CUP_NOLOGFILE")) {
    char debug_path[4096];
    int lf = ter_game_path(debug_path, sizeof(debug_path), "debug.log") == 0
               ? open(debug_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    if (lf >= 0) {
      printf("stderr -> %s\n", debug_path);
      dup2(lf, 2); if (lf != 2) close(lf);
    }
  }

  /* valida que tpidr_el0+0x28 (canary bionic) caiu DENTRO do nosso pad TLS */
  { uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t slot = tp + 0x28, lo = (uintptr_t)g_bionic_guard_pad;
    fprintf(stderr, "TLS guard: tpidr=0x%lx slot+0x28=0x%lx pad=[0x%lx..0x%lx] %s (val=0x%lx)\n",
            (unsigned long)tp, (unsigned long)slot, (unsigned long)lo,
            (unsigned long)(lo + sizeof(g_bionic_guard_pad)),
            (slot >= lo && slot + 8 <= lo + sizeof(g_bionic_guard_pad))
                ? "DENTRO ok" : "FORA (canary instavel!)",
            *(unsigned long *)slot);
  }
  /* sigaltstack: p/ o handler reportar STACK OVERFLOW (SIGSEGV na guard page →
     sem espaço na pilha normal p/ rodar o handler → morte silenciosa). */
  g_skipbad = getenv("CUP_SKIPBAD") ? 1 : 0;
  /* CUP_GCSIG: Boehm GC suspende threads via SIGPWR(30)/restart SIGXCPU(24) p/
     stop-the-world. Nossas threads (criadas via pthread_create_fake, fora do
     registro do GC) recebem SIGPWR com ação DEFAULT = mata o processo (exit 158).
     Instalamos handlers que implementam o protocolo (suspende+espera restart) p/
     a thread não morrer. (my_sigaction bloqueia o engine de sobrescrever.) */
  if (getenv("CUP_GCSIG")) {
    extern void gc_suspend_handler(int), gc_restart_handler(int);
    struct sigaction sp; memset(&sp, 0, sizeof sp);
    sp.sa_handler = gc_suspend_handler; sigfillset(&sp.sa_mask);
    sigdelset(&sp.sa_mask, SIGXCPU); sigdelset(&sp.sa_mask, SIGSEGV);
    sigaction(SIGPWR, &sp, 0);
    struct sigaction sr; memset(&sr, 0, sizeof sr);
    sr.sa_handler = gc_restart_handler; sigemptyset(&sr.sa_mask);
    sigaction(SIGXCPU, &sr, 0);
    fprintf(stderr, "[GCSIG] handlers SIGPWR(suspend)+SIGXCPU(restart) instalados\n");
  }
  { static char altstk[256 * 1024]; stack_t ss = {0};
    ss.ss_sp = altstk; ss.ss_size = sizeof altstk; ss.ss_flags = 0;
    sigaltstack(&ss, NULL); }
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGABRT, &sa, 0);
  sigaction(SIGILL, &sa, 0); sigaction(SIGFPE, &sa, 0);
  sigaction(SIGTRAP, &sa, 0); sigaction(SIGSYS, &sa, 0);  /* BRK/seccomp matam calado */
  { struct sigaction sd; memset(&sd, 0, sizeof sd); sd.sa_sigaction = diag_handler;
    sd.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART; sigaction(SIGUSR1, &sd, 0); }
  ter_install_term_handlers();

  fprintf(stderr, "=== Terraria Android Unity 2021.3 IL2CPP (arm64 GLES2) loader ===\n");

  /* GL/EGL/z visíveis p/ dlsym(RTLD_DEFAULT) do Unity */
  dlopen("libz.so.1", RTLD_NOW | RTLD_GLOBAL);
  void *g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL); if (!g) dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
  void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL); if (!e) dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
  fprintf(stderr, "[libs] z/GLESv2/EGL dlopen (glClear=%p)\n", dlsym(RTLD_DEFAULT, "glClear"));

  /* ---- F0: carrega libunity.so ---- */
  size_t hs = (size_t)HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { perror("mmap"); return 1; }
  fprintf(stderr, "[F0] heap %dMB @ %p, carregando libunity.so...\n", HEAP_MB, heap);
  if (so_load("libunity.so", heap, hs) < 0) { fprintf(stderr, "so_load libunity FALHOU\n"); return 1; }
  fprintf(stderr, "[F0] libunity: text=%p+%zu data=%p+%zu\n", text_base, text_size, data_base, data_size);
  g_unity_data = (uintptr_t)data_base;
  if (so_relocate() < 0) { fprintf(stderr, "relocate FALHOU\n"); return 1; }

  /* overrides */
  g_unity_base = (uintptr_t)text_base;
  set_import("abort", (void *)my_abort);
  set_import("raise", (void *)my_raise);
  set_import("tgkill", (void *)my_tgkill);
  set_import("exit", (void *)my_exit);
  set_import("_exit", (void *)my_exit);
  /* __stack_chk_fail nao esta na tabela de imports -> patch direto na GOT (apos resolve) */
  set_import("glGetString", (void *)my_glGetString);
  ds_init();  /* CUP_DRAWSPY: ring de draws + watchdog (intercepta via eglGetProcAddress) */
  if (getenv("CUP_EGPLOG") || getenv("CUP_NOVAO") || g_drawspy)
    set_import("eglGetProcAddress", (void *)my_eglGetProcAddress);
  set_import("sysconf", (void *)my_sysconf);
  /* 🔑 dl_iterate_phdr: libil2cpp importa e o resolvia p/ o STUB (retorna 0) → o unwinder C++ da
     libgcc não acha o .eh_frame de libunity/libil2cpp → exceção C++ real (ex.: shader/currentActivity)
     vira `std::terminate`→abort em vez de ser capturada. Wira o REAL (itera g_so_mods). */
  { extern int dl_iterate_phdr(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    set_import("dl_iterate_phdr", (void *)dl_iterate_phdr); }
  if (getenv("TER_JOBINLINE")) {
    set_import("sched_getaffinity", (void *)my_sched_getaffinity);
    fprintf(stderr, "[JOBINLINE] sched_getaffinity -> 1 CPU (job system roda inline)\n");
  }
  g_mmaplog = getenv("CUP_MMAPLOG") ? 1 : 0;
  g_guidlog = getenv("TER_GUIDLOG") ? 1 : 0;
  set_import("mmap", (void *)my_mmap);
  set_import("mmap64", (void *)my_mmap);
  /* getaddrinfo: o stub gerado retorna 0 (=SUCESSO) sem preencher *res -> o
     perf-reporter do build Play 301544 lia um addrinfo de lixo de stack e
     morria em strlen (lr=unity+0x7b8814). Passthrough pro glibc também não
     serve: o layout bionic troca ai_addr/ai_canonname (offsets 24/32). Falha
     limpa e o engine segue offline, igual ao comportamento do build de
     referência. 8 = EAI_NONAME do bionic. */
  set_import("getaddrinfo", (void *)my_getaddrinfo);
  if (g_guidlog) {
    set_import("read", (void *)my_read);
    set_import("lseek64", (void *)my_lseek64);
    set_import("fstat64", (void *)my_fstat64);
    set_import("mmap64", (void *)my_mmap64);
    set_import("fdopen", (void *)my_fdopen);
  }
  set_import("fopen", (void *)my_fopen);
  set_import("open", (void *)my_open);
  set_import("stat", (void *)my_stat);
  set_import("lstat", (void *)my_lstat);
  set_import("stat64", (void *)my_stat64);
  set_import("lstat64", (void *)my_lstat64);
  /* 🔑 fstat64: glibc≥2.30 NÃO exporta `fstat64` p/ dlsym → passthrough falha → slot
     GOT NULL. libunity chama fstat64 no RecreateGfxState (kmsdrm/G31) → salta p/ NULL
     (pc=0). my_fstat64 chama o fstat64 REAL (linka na buster glibc 2.28). Incondicional
     (antes só com TER_GUIDLOG). Mesma classe do stat/lstat da lição DYSMANTLE. */
  set_import("fstat64", (void *)my_fstat64);
  set_import("access", (void *)my_access);
  set_import("statfs64", (void *)my_statfs64);
  set_import("statfs", (void *)my_statfs64);
  set_import("strlcpy", (void *)my_strlcpy);
  set_import("strlcat", (void *)my_strlcat);
  set_import("memalign", (void *)my_memalign);
  /* 🔑 sincos/sincosf: caiam no stub genérico que NÃO escreve *sin/*cos ->
     lixo de stack em toda rotação nativa da Unity (trilha/partícula/sprite):
     "IsNan NaN,NaN" + tela glitchada com Zenith/armas de efeito pesado.
     glibc tem os dois (GNU ext) — entrega os reais. */
  set_import("sincos", (void *)my_sincos);
  set_import("sincosf", (void *)my_sincosf);
  set_import("syscall", (void *)my_syscall);
  set_import("pthread_kill", (void *)my_pthread_kill);
  set_import("__memmove_chk", (void *)my_memmove_chk);
  set_import("__memcpy_chk", (void *)my_memcpy_chk);
  set_import("__memset_chk", (void *)my_memset_chk);
  set_import("__strlen_chk", (void *)my_strlen_chk);
  set_import("__strcpy_chk", (void *)my_strcpy_chk);
  set_import("__strcat_chk", (void *)my_strcat_chk);
  set_import("__vsnprintf_chk", (void *)my_vsnprintf_chk);
  set_import("__snprintf_chk", (void *)my_snprintf_chk);
  set_import("__FD_SET_chk", (void *)my_FD_SET_chk);
  set_import("__system_property_get", (void *)my_sysprop);
  set_import("__android_log_print", (void *)my_alog_print);
  set_import("__android_log_write", (void *)my_alog_write);
  set_import("sigaction", (void *)my_sigaction);
  install_bionic_sigset_shim();
  set_import("dlopen", (void *)my_dlopen);
  set_import("dlsym", (void *)my_dlsym);
  set_import("dlerror", (void *)my_dlerror);
  set_import("dladdr", (void *)my_dladdr);
  set_import("dlclose", (void *)my_dlclose);
  set_import("pthread_key_create", (void *)sh_key_create);
  set_import("pthread_key_delete", (void *)sh_key_delete);
  set_import("pthread_getspecific", (void *)sh_getspecific);
  set_import("pthread_setspecific", (void *)sh_setspecific);
  set_import("ANativeWindow_fromSurface", (void *)my_aw_fromSurface);
  set_import("ANativeWindow_setBuffersGeometry", (void *)my_aw_setgeom);
  set_import("ANativeWindow_getWidth", (void *)my_aw_getWidth);
  set_import("ANativeWindow_getHeight", (void *)my_aw_getHeight);
  set_import("ANativeWindow_getFormat", (void *)my_aw_getFormat);
  set_import("ANativeWindow_acquire", (void *)my_aw_noop);
  set_import("ANativeWindow_release", (void *)my_aw_noop);

  /* alias bionic->glibc: __errno (bionic) = __errno_location (glibc) */
  { void *el = dlsym(RTLD_DEFAULT, "__errno_location");
    if (el) set_import("__errno", el); }

  install_pthread_shim();  /* complete bionic pthread/sem/attr ABI bridge */

  fprintf(stderr, "[F0] resolvendo %zu imports...\n", dynlib_numfunctions);
  { extern void recon_fill_passthrough(void); recon_fill_passthrough(); }  /* preenche passthrough via dlsym (tabela gerada) */
  if (so_resolve(dynlib_functions, dynlib_numfunctions, 0) < 0) { fprintf(stderr, "resolve FALHOU\n"); return 1; }
  ctype_init(); ctype_resolve();   /* _ctype_/_tolower_tab_/_toupper_tab_ (bionic) p/ libunity */
  so_record_phdr("libunity.so");   /* p/ o dl_iterate_phdr custom (unwind de exceções C++) */
  if (so_register_eh_frame() == 0) fprintf(stderr, "[EH] .eh_frame libunity registrado (exceções C++)\n");
  /* PATCH-GOT: os imports NDK nao estao em dynlib_functions -> set_import foi
   * no-op e ficaram UNRESOLVED (GOT lixo). Sobrescreve os slots DIRETO. */
  patch_got("ANativeWindow_fromSurface", (void *)my_aw_fromSurface);
  patch_got("ANativeWindow_setBuffersGeometry", (void *)my_aw_setgeom);
  patch_got("ANativeWindow_getWidth", (void *)my_aw_getWidth);
  patch_got("ANativeWindow_getHeight", (void *)my_aw_getHeight);
  patch_got("ANativeWindow_getFormat", (void *)my_aw_getFormat);
  patch_got("ANativeWindow_acquire", (void *)my_aw_noop);
  patch_got("ANativeWindow_release", (void *)my_aw_noop);
  patch_got("__android_log_print", (void *)my_alog_print);
  patch_got("__android_log_write", (void *)my_alog_write);
  patch_got("__android_log_vprint", (void *)my_alog_vprint);
  patch_bionic_sigset_shim();
  if (getenv("CUP_EGPLOG") || getenv("CUP_NOVAO") || g_drawspy)
    patch_got("eglGetProcAddress", (void *)my_eglGetProcAddress);
  /* Always intercept present: native controller state and lifecycle fixes must
     run even when optional render diagnostics are disabled. */
  patch_got("eglSwapBuffers", (void *)my_eglSwapBuffers);
  /* dl* estavam COMENTADOS em imports.gen.c -> set_import foi no-op e o dlopen@plt
     caiu no glibc REAL (falha ao carregar .so Android). Sem isso o il2cpp nao carrega. */
  patch_got("dlopen", (void *)my_dlopen);
  patch_got("dlsym", (void *)my_dlsym);
  patch_got("dlerror", (void *)my_dlerror);
  patch_got("dlclose", (void *)my_dlclose);
  patch_got("dladdr", (void *)my_dladdr);
  /* engine checa existência dos arquivos de dados antes de abrir */
  patch_got("open", (void *)my_open);
  patch_got("close", (void *)my_close);
  patch_got("fopen", (void *)my_fopen);
  patch_got("stat", (void *)my_stat);
  patch_got("lstat", (void *)my_lstat);
  patch_got("stat64", (void *)my_stat64);
  patch_got("lstat64", (void *)my_lstat64);
  patch_got("fstat64", (void *)my_fstat64);  /* incondicional: dlsym falha na glibc≥2.30 */
  patch_got("fstat", (void *)my_fstat);      /* NDK novo importa fstat direto (glibc<2.33 nao exporta) */
  patch_got("access", (void *)my_access);
  patch_got("statfs64", (void *)my_statfs64);
  patch_got("statfs", (void *)my_statfs64);
  patch_got("strlcpy", (void *)my_strlcpy);
  patch_got("strlcat", (void *)my_strlcat);
  patch_got("memalign", (void *)my_memalign);
  patch_got("syscall", (void *)my_syscall);
  patch_got("pthread_kill", (void *)my_pthread_kill);
  patch_got("memalign", (void *)my_memalign);
  patch_got("syscall", (void *)my_syscall);
  patch_got("pthread_kill", (void *)my_pthread_kill);
  patch_got("__memmove_chk", (void *)my_memmove_chk);
  patch_got("__memcpy_chk", (void *)my_memcpy_chk);
  patch_got("__memset_chk", (void *)my_memset_chk);
  patch_got("__strlen_chk", (void *)my_strlen_chk);
  patch_got("__strcpy_chk", (void *)my_strcpy_chk);
  patch_got("__strcat_chk", (void *)my_strcat_chk);
  patch_got("__vsnprintf_chk", (void *)my_vsnprintf_chk);
  patch_got("__snprintf_chk", (void *)my_snprintf_chk);
  patch_got("__FD_SET_chk", (void *)my_FD_SET_chk);
  patch_got("exit", (void *)my_exit);
  patch_got("_exit", (void *)my_exit);
  if (g_guidlog) {
    patch_got("read", (void *)my_read);
    patch_got("lseek64", (void *)my_lseek64);
    patch_got("fstat64", (void *)my_fstat64);
    patch_got("mmap64", (void *)my_mmap64);
    patch_got("fdopen", (void *)my_fdopen);
  }
  patch_pthread_shim();
  /* sensores/looper/profiler google: stub no-op (nao usados no path do gfx) */
  const char *ndk_noop[] = {
    "ALooper_forThread","ALooper_prepare","ASensorManager_getInstance",
    "ASensorManager_createEventQueue","ASensorManager_getSensorList",
    "ASensorManager_getDefaultSensor","ASensorManager_destroyEventQueue",
    "ASensorEventQueue_hasEvents","ASensorEventQueue_getEvents",
    "ASensorEventQueue_enableSensor","ASensorEventQueue_disableSensor",
    "ASensorEventQueue_setEventRate","ASensor_getType","ASensor_getResolution",
    "ASensor_getMinDelay","ASensor_getName","ASensor_getVendor",
    "__google_potentially_blocking_region_begin",
    "__google_potentially_blocking_region_end", NULL };
  for (int i = 0; ndk_noop[i]; i++) patch_got(ndk_noop[i], (void *)ndk_stub0);

  /* TER: bypass do "Not enough storage space to install required resources".
   * RE (libunity): em 0x2d8fac `tbz w0,#0, 0x2d9068` — se a checagem de espaço/resources
   * (0x22b7e0) retorna falso, pula pro bloco que monta o AlertDialog (string 0x9288ef).
   * Esse bloco SÓ é alcançável por esse branch. NOP -> sempre segue o caminho de sucesso
   * (dados já estão em bin/Data, lidos via AssetManager). */
  if (!getenv("TER_NOSTORAGEPATCH")) {
    /* Assinatura obrigatoria: o alvo tem de ser o proprio `tbz w0,...` do RE.
     * Num build diferente o RVA aponta para outra instrucao — NOPar as cegas
     * seria corromper codigo arbitrario; nesse caso o pior efeito de pular o
     * patch e o dialogo de storage reaparecer. */
    uint32_t insn = *(const uint32_t *)((uintptr_t)text_base + 0x2d8fac);
    if (((insn >> 24) & 0x7Fu) == 0x36u) {
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      *(uint32_t *)((uintptr_t)text_base + 0x2d8fac) = 0xd503201fu; /* NOP */
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[TER] storage-check 0x2d8fac (tbz->dialog) NOPado\n");
    } else {
      fprintf(stderr,
              "[TER] storage-check pulado: 0x2d8fac nao e tbz neste build "
              "(opcode=%08x)\n", insn);
    }
  }

  /* O FIX REAL do null-deref do Enlighten é o `memalign` (acima, deixou de ser stub).
     Este patch do wrapper 0x861928 -> my_enl_alloc é uma REDE DE SEGURANÇA opcional
     (fallback malloc se o allocator real devolver NULL por qualquer motivo). OPT-IN via
     TER_ENLFIX (default OFF — memalign sozinho já resolve). TER_ENLLOG liga log por-alloc. */
  g_enllog = getenv("TER_ENLLOG") ? 1 : 0;
  if (getenv("TER_ENLFIX")) {
    patch_tramp(0x861928, (void *)my_enl_alloc);
    fprintf(stderr, "[ENL] alloc-wrapper 0x861928 -> my_enl_alloc (rede de segurança)\n");
  }

  /* CUP_FORCEIL2: o helper "load library by name" do Unity (0x357938) faz o
     System.load do il2cpp via JNI -> falha no nosso ambiente ("Failed to load
     Il2CPP"). Mas NOS ja' carregamos libil2cpp.so no F1. Forca retorno 1 (sucesso):
       mov w0,#1 ; ret  */
  if (getenv("CUP_FORCEIL2")) {
    *(uint32_t *)((uintptr_t)text_base + 0x357938) = 0x52800020u; /* mov w0,#1 */
    *(uint32_t *)((uintptr_t)text_base + 0x35793c) = 0xd65f03c0u; /* ret */
    fprintf(stderr, "[FORCEIL2] 0x357938 -> mov w0,#1; ret\n");
  }
  /* CUP_NOEXTRACT: a extracao de recursos do APK (0x94184c) copia de um VFS source
     (o APK) que nao temos -> falha ("Failed to extract resources"). Mas os assets
     JA estao deployados em bin/Data/. Forca a extracao reportar sucesso. */
  if (getenv("CUP_NOEXTRACT")) {
    *(uint32_t *)((uintptr_t)text_base + 0x94184c) = 0x52800020u; /* mov w0,#1 */
    *(uint32_t *)((uintptr_t)text_base + 0x941850) = 0xd65f03c0u; /* ret */
    fprintf(stderr, "[NOEXTRACT] 0x94184c -> mov w0,#1; ret\n");
  }

  so_finalize(); so_flush_caches();
  g_alloc_ub = (uintptr_t)text_base;
  if (getenv("CUP_DLLOG")) g_dllog = 1;

  /* __stack_chk_fail nao esta na tabela -> patch direto no slot da GOT */
  if (getenv("CUP_NOSCF")) {
    extern uintptr_t so_find_rel_addr_safe(const char *);
    uintptr_t got = so_find_rel_addr_safe("__stack_chk_fail");
    if (got) { *(uintptr_t *)got = (uintptr_t)my_stack_chk_fail;
      fprintf(stderr, "[SCF] GOT __stack_chk_fail @ 0x%lx patcheado\n", got); }
    else fprintf(stderr, "[SCF] __stack_chk_fail nao achado na GOT\n");
  }

  /* DIAGNÓSTICO: a main fica presa num loop (libunity 0x873a90) esperando a
     função 0x8739c4 (fila do GfxDevice [+224]/[+256]) zerar — deadlock do
     threaded rendering no Mali. CUP_NOGFXWAIT patcha 0x8739c4 p/ retornar 0
     (não espera) e ver se o jogo avança. */
  if (getenv("CUP_NOGFXWAIT")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    *(uint32_t *)((uintptr_t)text_base + 0x8739c4) = 0x52800000u; /* mov w0,#0 */
    *(uint32_t *)((uintptr_t)text_base + 0x8739c8) = 0xd65f03c0u; /* ret */
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[NOGFXWAIT] 0x8739c4 -> mov w0,#0; ret\n");
  }

  /* CUP_PSPY: substitui HasPendingOperations (0x8739c4) pela réplica C que
     captura o ponteiro do PreloadManager (diagnóstico do muro frame 111).
     CUP_GCEVERY também precisa do mgr (gate de ociosidade da limpeza). */
  if (getenv("CUP_PSPY") || getenv("CUP_GCEVERY")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + UN_HASPEND, (uintptr_t)my_haspending);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[PSPY] hook HasPendingOperations (0x%x) instalado\n", UN_HASPEND);
  }
  /* CUP_FORCEINTEG: dentro de IntegrateOp (0x872758), o gate (0x871844, budget
     time-slice) é checado em 0x872774 `tbz w0,#0, 872810` → se budget recusa,
     integração aborta retornando 0. Em WaitForAll (force-complete) o budget
     DEVERIA ser ignorado. NOP nesse branch: o gate ainda RODA (efeitos colaterais
     do predictor preservados), só o VEREDITO é ignorado → integração prossegue.
     Cirúrgico (só o path de integração; não mexe no gate compartilhado). */
  if (getenv("CUP_FORCEINTEG")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    /* ⚠️ 0x872774 (IntegrateOp) bypassa o VEREDITO INTEIRO do gate, incl. jobs-pending
       -> integra com job do worker pendente = RACE = objeto malformado (crash $PC=9 na
       desserializacao do CupheadCore). CUP_NO872774 pula este (mantem so o 0x871854 que
       bypassa SO o budget e PRESERVA a espera do job). */
    if (!getenv("CUP_NO872774"))
      *(uint32_t *)((uintptr_t)text_base + 0x872774) = 0xd503201fu; /* NOP (IntegrateOp 0x872758) */
    /* + ops cujo integrate É o gate 0x871844 DIRETO (materiais/shaders): NOP no branch
       0x871854 `tbz w0,#0, 87186c` que aborta no veredito do budget -> cai no check de
       jobs (passa qdo jobmgr=0). Cobre as 52 ops de shader/material presas (sessão 8). */
    if (!getenv("CUP_NOGATE854"))
      *(uint32_t *)((uintptr_t)text_base + 0x871854) = 0xd503201fu; /* NOP */
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[FORCEINTEG] 0x872774 + 0x871854 (gate budget-fail branches) -> NOP\n");
  }
  /* ===== CUP_SCENESKIP (default ON; CUP_NOSCENESKIP desliga) — RAIZ =====
   * Hook na ENTRADA de 0x541c9c: se a scene do GameObject ([arg0+56]) for NULL,
   * pula a função INTEIRA (não monta o mesh) em vez de cascatear nulls. Substitui
   * o antigo island fake-scene (que vazava e crashava downstream em 0x8f9b1c/b88/541e54). */
  if (0 /* RE Cuphead 2017.4 — offset inexistente no Terraria 2021.3 */) {
    void *trs = mk_tramp((uintptr_t)text_base + 0x541c9c, "scene541");
    if (trs) {
      scene541_orig = (long (*)(long, long, long, long, long, long, long, long))trs;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x541c9c, (uintptr_t)scene541_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[SCENESKIP] hook 0x541c9c (skip GO se scene[arg0+56]==NULL)\n");
    } else {
      fprintf(stderr, "[SCENESKIP] mk_tramp falhou — guard OFF\n");
    }
  }
  /* CUP_MASKGUARD (default ON): clampa contagem insana em 0x8f9914 (mesh de
     SpriteMask/Tilemap do mapa) — 2º crash do load do mapa (0x8f9b1c). */
  if (0 /* RE Cuphead 2017.4 — offsets inexistentes no Terraria 2021.3 */) {
    void *tr = mk_tramp((uintptr_t)text_base + 0x8f9914, "maskfn");
    if (tr) {
      maskfn_orig = (long (*)(long, long, long, long, long, long, long, long))tr;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x8f9914, (uintptr_t)maskfn_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[MASKGUARD] hook 0x8f9914 (clamp count [1,0x40000])\n");
    } else {
      fprintf(stderr, "[MASKGUARD] mk_tramp falhou — guard OFF\n");
    }
    /* NULLGUARD: 0x8f9b88 (tilemap, arg0 NULL no mapa) */
    void *trn = mk_tramp((uintptr_t)text_base + 0x8f9b88, "nullfn");
    if (trn) {
      nullfn_orig = (long (*)(long, long, long, long, long, long, long, long))trn;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x8f9b88, (uintptr_t)nullfn_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[NULLGUARD] hook 0x8f9b88 (skip se arg0==NULL)\n");
    }
    /* DESERGUARD: 0x54220c (desserialização, *arg0 NULL no mapa) — crash #5 */
    void *trd = mk_tramp((uintptr_t)text_base + 0x54220c, "deser542");
    if (trd) {
      deser542_orig = (long (*)(long, long, long, long, long, long, long, long))trd;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x54220c, (uintptr_t)deser542_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[DESERGUARD] hook 0x54220c (skip se *arg0==NULL)\n");
    }
  }
  /* TER_AUDIOSPY/TER_STREAMFALLBACK: hook do createSound (libunity 0x806cb4).
     SPY loga result de cada som; STREAMFALLBACK refaz streams falhos como sample.
     Instalado aqui (contexto libunity, text_base=libunity, ANTES do F1/il2cpp). */
  if ((getenv("TER_AUDIOSPY") || getenv("TER_STREAMFALLBACK")) &&
      !ter_known_build()) {
    fprintf(stderr,
            "[CSSPY] hook createSound requer o build de referencia; "
            "stream-fallback de audio desativado neste build\n");
  } else if (getenv("TER_AUDIOSPY") || getenv("TER_STREAMFALLBACK")) {
    g_stream_fallback = getenv("TER_STREAMFALLBACK") ? 1 : 0;
    void *tr = mk_tramp((uintptr_t)text_base + 0x806cb4, "createSound");
    if (tr) {
      cs_orig = (long (*)(void *, void *, int, void *, void *))tr;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x806cb4, (uintptr_t)cs_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[CSSPY] hook createSound(0x806cb4) instalado (fallback=%d)\n", g_stream_fallback);
    } else fprintf(stderr, "[CSSPY] mk_tramp falhou\n");
    /* MIXSPY: ground-truth do mixer (count real + formato) p/ achar a causa do áudio rápido */
    if (getenv("TER_AUDIOSPY")) {
      void *trm = mk_tramp((uintptr_t)text_base + 0x805a94, "mixer");
      if (trm) {
        mix_orig = (long (*)(void *, void *, int))trm;
        extern void so_make_text_writable(void), so_make_text_executable(void);
        so_make_text_writable();
        hook_arm64((uintptr_t)text_base + 0x805a94, (uintptr_t)mix_hook);
        so_make_text_executable(); so_flush_caches();
        fprintf(stderr, "[MIXSPY] hook mixer(0x805a94) instalado\n");
      } else fprintf(stderr, "[MIXSPY] mk_tramp falhou\n");
    }
  }
  /* CUP_WAITGATE: FORCEINTEG cirúrgico — ignora o gate de budget SÓ dentro do
     WaitForAll (0x873a90). Hook do WaitForAll (flag in_waitall) + hook do gate
     (0x871844 → my_gate). NÃO combinar com CUP_FORCEINTEG. */
  if (getenv("CUP_WAITGATE")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    g_waitall_cont = (uintptr_t)text_base + 0x873a90 + 16;
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x873a90, (uintptr_t)my_waitall);
    hook_arm64((uintptr_t)text_base + 0x871844, (uintptr_t)my_gate);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[WAITGATE] hook WaitForAll(0x873a90)+gate(0x871844); cont=0x%lx\n",
            (unsigned long)g_waitall_cont);
  }
  /* CUP_GATEWAIT: hook do gate (0x871844) SEMPRE-ativo — bypassa o budget (quebrado no
     so-loader) MAS faz spin-wait nos jobs do worker (sched_yield) antes de liberar a
     integração. Mata a race da integração forçada SEM os NOPs (0x872774/0x871854).
     NÃO combinar com CUP_FORCEINTEG. */
  if (getenv("CUP_GATEWAIT")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    g_gatewait = 1;
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x871844, (uintptr_t)my_gate);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[GATEWAIT] hook gate(0x871844) sempre: bypass budget + spin-wait jobs\n");
  }
  /* ===== CUP_FORCESL (s14, default ON; CUP_NOFORCESL desliga) — SOM =====
   * O FMOD escolhe o output Android em 0x350298: retorna 2=OpenSL (setOutput 22) ou
   * 1=AudioTrack-Java (setOutput 21). Exige SDK>16 (0x3506b8 lê Build.VERSION.SDK_INT
   * via JNI, cache [0x128dcc0] — nosso shim devolve 0) + checks de low-latency → cai
   * SEMPRE no AudioTrack; sem JVM real o init falha → "null output" → SEM SOM (o
   * dlopen(libOpenSLES) era só probe; slCreateEngine nunca rodava). Forçamos retorno
   * 2 → init entra no slCreateEngine → opensles_shim → SDL2 (receita DYSMANTLE sdk=25). */
  if (0 /* RE Cuphead 2017.4 FMOD — offsets inexistentes no Terraria 2021.3 */) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x350298, (uintptr_t)forcesl_hook);
    /* guard do alocador do FMOD (default ON; CUP_NOFMODGUARD desliga) — mata o OOM
       fatal do flange. Dentro da janela WRITABLE (hook escreve no .text). */
    if (!getenv("CUP_NOFMODGUARD")) {
      struct { uintptr_t rva; void *hook; void **orig; const char *nm; } G[] = {
        {0xa66e6c, (void *)fmod_alloc_hook,  (void **)&fmod_alloc_orig,  "fmod.alloc"},
        {0xa66b74, (void *)fmod_alloc2_hook, (void **)&fmod_alloc2_orig, "fmod.alloc2"},
      };
      for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        void *tr = mk_tramp((uintptr_t)text_base + G[i].rva, G[i].nm);
        if (!tr) { fprintf(stderr, "[FMODGUARD] tramp %s falhou\n", G[i].nm); continue; }
        *G[i].orig = tr;
        hook_arm64((uintptr_t)text_base + G[i].rva, (uintptr_t)G[i].hook);
      }
      fprintf(stderr, "[FMODGUARD] guard do alocador instalado\n");
    }
    if (getenv("CUP_FMODSPY")) {   /* dentro da janela WRITABLE (hook escreve no .text) */
      struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
        {0xa6281c, (void *)fmod_init_hook,   (void **)&fmod_init_orig,   "Sys::init"},
        {0xa6dbe0, (void *)fmod_oinit_hook,  (void **)&fmod_oinit_orig,  "osl.init"},
        {0xa6e270, (void *)fmod_ostart_hook, (void **)&fmod_ostart_orig, "osl.start"},
      };
      for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        void *tr = mk_tramp((uintptr_t)text_base + T[i].rva, T[i].nm);
        if (!tr) { fprintf(stderr, "[FMODSPY] tramp %s falhou\n", T[i].nm); continue; }
        *T[i].orig = tr;
        hook_arm64((uintptr_t)text_base + T[i].rva, (uintptr_t)T[i].hook);
      }
      fprintf(stderr, "[FMODSPY] hooks init/oinit/ostart instalados\n");
    }
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[FORCESL] hook 0x350298 -> 2 (FMOD output = OpenSL/shim)\n");
  }
  /* CUP_CLAMPSIG: clampa o count de Semaphore::Signal (0x65850c) p/ matar o storm
     (count deriva p/ enorme ~frame 110 → posta bilhões de vezes = livelock). */
  if (getenv("CUP_CLAMPSIG")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    if (getenv("CUP_SIGCLAMP")) g_signal_clamp = atoi(getenv("CUP_SIGCLAMP"));
    g_signal_cont = (uintptr_t)text_base + 0x65850c + 16;
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x65850c, (uintptr_t)my_signal);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[CLAMPSIG] hook Signal(0x65850c) clamp=%d; cont=0x%lx\n",
            g_signal_clamp, (unsigned long)g_signal_cont);
  }

  fprintf(stderr, "[F0] init_array...\n");
  so_execute_init_array();
  fprintf(stderr, "[F0] libunity init OK\n");
  mm_probe("pos-init_array-unity");

  /* ---- JNI_OnLoad da libunity ---- */
  jni_shim_set_package(ter_selected_android_package(), 0);
  void *vm = NULL, *env = NULL; jni_shim_init(&vm, &env);
  uintptr_t onload = so_find_addr_safe("JNI_OnLoad");
  if (onload) {
    int ver = ((int (*)(void *, void *))onload)(vm, NULL);
    fprintf(stderr, "[F0] JNI_OnLoad = 0x%x\n", ver);
  } else {
    fprintf(stderr, "[F0] JNI_OnLoad não encontrado em libunity\n");
  }
  fprintf(stderr, "[F0] === libunity OK ===\n");
  mm_probe("pos-JNI_OnLoad");
  dbg_sync();

  /* ---- F1: carrega libil2cpp.so (2º módulo, lógica C# do jogo) ---- */
  g_m_unity = so_save();
  size_t i2s = 96UL * 1024 * 1024;
  void *i2heap = mmap(NULL, i2s, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  g_i2heap_base = (uintptr_t)i2heap; g_i2heap_size = i2s;
  if (i2heap != MAP_FAILED && so_load("libil2cpp.so", i2heap, i2s) >= 0) {
    g_il2cpp_base = (uintptr_t)text_base;
    g_alloc_ib = g_il2cpp_base;
    fprintf(stderr, "[F1] libil2cpp: text=%p+%zu\n", text_base, text_size);
    so_relocate();
    { extern void recon_fill_passthrough(void); recon_fill_passthrough(); }
    so_resolve(dynlib_functions, dynlib_numfunctions, 0);
    ctype_resolve();   /* _ctype_/_tolower_tab_/_toupper_tab_ p/ libil2cpp tb */
    so_record_phdr("libil2cpp.so");   /* p/ o dl_iterate_phdr custom (unwind) */
    if (so_register_eh_frame() == 0) fprintf(stderr, "[EH] .eh_frame libil2cpp registrado (exceções C++)\n");
    /* il2cpp abre o global-metadata.dat via open() -> intercepta p/ redirecionar.
       patch_got opera no modulo ATIVO (=il2cpp agora). Tb dlopen/dlsym/log. */
    patch_got("open", (void *)my_open);
    patch_got("close", (void *)my_close);
    patch_got("mmap", (void *)my_mmap);
    patch_got("mmap64", (void *)my_mmap);
    /* sigaction do libil2cpp (o GC instala SIGPWR/SIGXCPU por aqui). Sem patch, o GC
       instalava um handler CORROMPIDO (0x7f10000004) p/ SIGPWR -> stop-the-world
       crashava. Com my_sigaction + CUP_GCSIG, bloqueamos -> nosso handler válido fica. */
    { extern int my_sigaction(); patch_got("sigaction", (void *)my_sigaction); }
    patch_bionic_sigset_shim();
    patch_got("fopen", (void *)my_fopen);
    patch_got("stat", (void *)my_stat);
    patch_got("lstat", (void *)my_lstat);
    patch_got("stat64", (void *)my_stat64);
    patch_got("lstat64", (void *)my_lstat64);
    patch_got("fstat64", (void *)my_fstat64);
    patch_got("fstat", (void *)my_fstat);  /* NDK novo importa fstat direto */
    patch_got("access", (void *)my_access);
  patch_got("statfs64", (void *)my_statfs64);
  patch_got("statfs", (void *)my_statfs64);
  patch_got("strlcpy", (void *)my_strlcpy);
  patch_got("strlcat", (void *)my_strlcat);
  patch_got("__memmove_chk", (void *)my_memmove_chk);
  patch_got("__memcpy_chk", (void *)my_memcpy_chk);
  patch_got("__memset_chk", (void *)my_memset_chk);
  patch_got("__strlen_chk", (void *)my_strlen_chk);
  patch_got("__strcpy_chk", (void *)my_strcpy_chk);
  patch_got("__strcat_chk", (void *)my_strcat_chk);
  patch_got("__vsnprintf_chk", (void *)my_vsnprintf_chk);
  patch_got("__snprintf_chk", (void *)my_snprintf_chk);
  patch_got("__FD_SET_chk", (void *)my_FD_SET_chk);
    patch_got("dlopen", (void *)my_dlopen);
    patch_got("dlsym", (void *)my_dlsym);
    patch_got("exit", (void *)my_exit);
    patch_got("_exit", (void *)my_exit);
    patch_got("__android_log_print", (void *)my_alog_print);
    patch_got("__android_log_write", (void *)my_alog_write);
    patch_got("__android_log_vprint", (void *)my_alog_vprint);
    patch_pthread_shim();
    /* CUP_CRSPY: hooks nos MoveNext das coroutines de boot (antes do flush de caches) */
    if (getenv("CUP_CRSPY")) {
      g_cr1_cont = g_il2cpp_base + 0x9A58D0 + 16;
      g_cr2_cont = g_il2cpp_base + 0x9A619C + 16;
      hook_arm64(g_il2cpp_base + 0x9A58D0, (uintptr_t)my_start_cr);
      hook_arm64(g_il2cpp_base + 0x9A619C, (uintptr_t)my_inputwait_cr);
      fprintf(stderr, "[CRSPY] hooks start_cr(0x9A58D0 $PC+0xBC) + inputwait(0x9A619C $PC+0x1C)\n");
    }
    if (getenv("CUP_BOOTSPY")) bootspy_install(g_il2cpp_base);
    if (getenv("CUP_MENUSPY")) menuspy_install(g_il2cpp_base);
    /* CUP_FORCESTARTCR: CupheadStartScene.Start (0x9A55CC) faz 3 checks
     * op_Inequality em Application.version/productName/identifier e DÁ EARLY-RETURN
     * (0x9A56F8) antes de StartCoroutine(start_cr) se algum não bate. No so-loader
     * esses getters não retornam o esperado -> start_cr NUNCA roda -> disclaimer
     * congela. Forçamos o caminho de prosseguir: NOP nos 2 tbnz-return + B p/ o
     * bloco-proceed no 3º branch. (start_cr dirige disclaimer->preload->título.) */
    if (getenv("CUP_FORCESTARTCR")) {
      uint32_t *t = (uint32_t *)(g_il2cpp_base + 0x9A567C);
      t[0] = 0xd503201fu;                              /* 0x9A567C tbnz -> NOP (cai = prossegue) */
      *(uint32_t *)(g_il2cpp_base + 0x9A56B8) = 0xd503201fu; /* 0x9A56B8 tbnz -> NOP */
      *(uint32_t *)(g_il2cpp_base + 0x9A56F4) = 0x14000006u; /* 0x9A56F4 tbz -> B 0x9A570C (proceed) */
      __builtin___clear_cache((char *)(g_il2cpp_base + 0x9A567C), (char *)(g_il2cpp_base + 0x9A56F8));
      fprintf(stderr, "[FORCESTARTCR] Start() early-returns NOPados -> StartCoroutine(start_cr) forçado\n");
    }
    /* CUP_NOREFRESHDLC: case 9 do start_cr chama DLCManager.RefreshDLC (0xC91C44) que
       no so-loader crasha (blr p/ delegate de plataforma NULL = método il2cpp não-init)
       -> coroutine de boot quebra no $PC=9. NOP a função inteira (ret) p/ pular. */
    if (getenv("CUP_NOREFRESHDLC")) {
      *(uint32_t *)(g_il2cpp_base + 0xC91C44) = 0xd65f03c0u; /* ret */
      __builtin___clear_cache((char *)(g_il2cpp_base + 0xC91C44), (char *)(g_il2cpp_base + 0xC91C48));
      fprintf(stderr, "[NOREFRESHDLC] DLCManager.RefreshDLC(0xC91C44) -> ret\n");
    }
    if (getenv("CUP_SAPATH") || getenv("CUP_SAPATH_ON")) {
      hook_arm64(g_il2cpp_base + 0x17C7C1C, (uintptr_t)my_streamingAssetsPath);
      /* NÃO hookar getBasePath 0x1031C8C: é stub de 3 insns que JÁ tail-calleia
         get_streamingAssetsPath (hookado); o hook de 16B estoura na função seguinte. */
      fprintf(stderr, "[SAPATH] hook get_streamingAssetsPath(0x17C7C1C)\n");
    }
    if (getenv("CUP_TAPINPUT")) {
      if (getenv("CUP_TAPSTART")) g_tap_start = atoi(getenv("CUP_TAPSTART"));
      if (getenv("CUP_TAPPERIOD")) g_tap_period = atoi(getenv("CUP_TAPPERIOD"));
      g_tapinput_cont = g_il2cpp_base + 0xCC2854 + 16;
      hook_arm64(g_il2cpp_base + 0xCC2854, (uintptr_t)my_getanybuttondown);
      fprintf(stderr, "[TAPINPUT] hook GetAnyButtonDown(0xCC2854) start=%d period=%d\n", g_tap_start, g_tap_period);
    }
    /* CUP_GAMEPAD: controle REAL via USB Gamepad (js0). Substitui Rewired.Player
       .GetButton/Down/Up/GetAxis(string) lendo o estado do js0. (gamepad.c) */
    if (getenv("CUP_GAMEPAD")) {
      extern void gp_init(uintptr_t);
      gp_init(g_il2cpp_base);
    }
    if (getenv("CUP_STAGESPY")) stagespy_install(g_il2cpp_base);
    if (!ter_install_game_exit_hook()) {
      /* Build desconhecido (ou assinatura divergente): sem a ponte, o botao
       * Quit Game do menu volta a ser um no-op do stub Android, mas
       * SELECT+START e SDL_QUIT continuam saindo pelo teardown guardado.
       * Melhor rodar degradado do que recusar o jogo inteiro. */
      fprintf(stderr,
              "[QUIT] ponte Game.Exit indisponivel neste build; "
              "saida garantida apenas por SELECT+START\n");
    }
    so_finalize(); so_flush_caches();
    fprintf(stderr, "[F1] libil2cpp init_array...\n");
    so_execute_init_array();
    g_m_il2cpp = so_save();
    fprintf(stderr, "[F1] libil2cpp carregado OK\n");
    mm_probe("pos-init_array-il2cpp");
    dbg_sync();
  } else {
    fprintf(stderr, "[F1] FALHOU carregar libil2cpp (heap=%p)\n", i2heap);
  }
  /* informa o sem_shim das bases p/ o detector de livelock mapear callers */
  { extern void sh_set_bases(unsigned long, unsigned long, unsigned long, unsigned long);
    sh_set_bases(g_unity_base, 0x2000000, g_il2cpp_base, 0x3000000); }
  { extern void pt_set_bases(unsigned long, unsigned long); pt_set_bases(g_unity_base, g_il2cpp_base); }

  so_use(g_m_unity);  /* volta o contexto p/ libunity */

  /* lista os métodos nativos registrados (achar initJni/nativeRender) */
  extern void jni_dump_natives(void);
  extern void *jni_find_native(const char *);
  jni_dump_natives();

  /* ---- F2: SDL probes the real backend, then ownership is selected ---- */
  int sdl_ready = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == 0;
  if (!sdl_ready) {
    fprintf(stderr, "[F2] SDL herdado falhou: %s; uma tentativa sem overrides\n", SDL_GetError());
    SDL_Quit();
    unsetenv("SDL_VIDEODRIVER");
    unsetenv("SDL_AUDIODRIVER");
    sdl_ready = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == 0;
  }
  if (!sdl_ready) {
    fprintf(stderr, "[F2] SDL VIDEO indisponível após fallback: %s\n", SDL_GetError());
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
      fprintf(stderr, "[F2] SDL áudio também indisponível: %s\n", SDL_GetError());
  }
  fprintf(stderr, "[F2] SDL video driver efetivo = %s\n",
          SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(nenhum)");

  if (cup_use_kmsdrm()) {
    extern int egl_shim_ensure_current(void);
    egl_shim_create_window();
    /* ELO: re-rota os egl* da libunity (so_resolve bindou no libEGL real) -> egl_shim.
       Contexto libunity ja' ativo aqui (so_use(g_m_unity) acima). */
    int np = egl_patch_unity_got();
    fprintf(stderr, "[F2] SDL-owned: %d slots egl* da libunity -> egl_shim\n", np);
    egl_shim_ensure_current();   /* deixa o contexto GL current na thread do jogo */
    fprintf(stderr, "[F2] janela GLES criada pelo backend SDL efetivo\n");
  } else {
    fprintf(stderr, "[F2] EGL vendor/fbdev (fbdev_window %ux%u)\n",
            g_fbdev_win.w, g_fbdev_win.h);
  }

  static long thiz = 0xA1, ctx = 0xC0, surf = 0x5F;
  void *fn;
  if ((fn = jni_find_native("initJni"))) {
    fprintf(stderr, "[F2] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, &thiz, &ctx);
    fprintf(stderr, "[F2] initJni OK\n");
    mm_probe("pos-initJni");
    dbg_sync();
  }
  if ((fn = jni_find_native("nativeRecreateGfxState"))) {
    mm_probe("pre-RecreateGfxState");
    /* 🔎 RE-ARMA on_crash: SDL_Init(VIDEO) do kmsdrm e/ou o blob Mali reinstalam
       o SIGSEGV default -> nosso dump nunca roda. Reinstala aqui, colado no ponto
       de crash, p/ o [CR!] async-safe sair. */
    {
      static char rearm_stk[256 * 1024]; stack_t rs = {0};
      rs.ss_sp = rearm_stk; rs.ss_size = sizeof rearm_stk; rs.ss_flags = 0;
      sigaltstack(&rs, NULL);
      struct sigaction sc; memset(&sc, 0, sizeof sc);
      sc.sa_sigaction = on_crash; sc.sa_flags = SA_SIGINFO | SA_ONSTACK;
      sigaction(SIGSEGV, &sc, 0); sigaction(SIGBUS, &sc, 0); sigaction(SIGILL, &sc, 0);
      fprintf(stderr, "[F2] on_crash re-armado pre-RecreateGfxState\n");
    }
    /* 🔎 dump de maps p/ correlacionar o pc do dmesg com a lib (blob Mali/libc/unity). */
    if (getenv("TER_DUMPMAPS")) {
      int mfd = open("/proc/self/maps", O_RDONLY);
      if (mfd >= 0) { char mb[4096]; ssize_t r;
        fprintf(stderr, "[MAPS] ==== inicio ====\n"); dbg_sync();
        while ((r = read(mfd, mb, sizeof mb)) > 0) { ssize_t w=write(2, mb, r); (void)w; }
        close(mfd); fprintf(stderr, "\n[MAPS] ==== fim ====\n"); dbg_sync();
      }
    }
    /* TESTE: anula o instalador de signal-handlers do Unity (0x360af8) com RET.
       Esse caminho (sigaction QUERY -> map RB-tree de old-handlers via operator-new)
       e' onde o canario estoura. Nao precisamos dos handlers do Unity (temos on_crash). */
    if (getenv("CUP_NOSIGINST")) {
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      *(uint32_t *)(g_alloc_ub + 0x360af8) = 0xd65f03c0u;  /* RET */
      so_make_text_executable();
      so_flush_caches();
      fprintf(stderr, "[NOSIGINST] 0x360af8 (install handlers) -> RET\n");
    }
    /* spy: hook na entrada do operator-new (0x3cbf2c) p/ capturar args da
       chamada que estoura o canario. Instala AQUI p/ pegar so' o gfx path. */
    if (getenv("CUP_ASPY")) {
      extern void so_make_text_writable(void), so_make_text_executable(void);
      g_gfx_cont = g_alloc_ub + 0x3cbf2c + 16;
      so_make_text_writable();
      hook_arm64(g_alloc_ub + 0x3cbf2c, (uintptr_t)onew_spy_tramp);
      so_make_text_executable();
      so_flush_caches();
      g_in_gfx = 1;
      fprintf(stderr, "[ONEW] hook em operator-new (0x3cbf2c) instalado\n");
    }
    fprintf(stderr, "[F2] nativeRecreateGfxState...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, &thiz, 0, &surf);
    fprintf(stderr, "[F2] nativeRecreateGfxState OK\n");
    mm_probe("pos-RecreateGfxState");
    dbg_sync();
  }
  /* Match the SurfaceHolder sequence used by this APK and by the proven
   * Horizon Chase / Prizefighters 2 loaders.  surfaceCreated installs the
   * native surface above; surfaceChanged then installs the current surface
   * again and explicitly releases Unity's surface-change wait before resume.
   * The old Terraria bootstrap omitted this event, leaving nativeRender
   * asleep in the engine's condition variable after the first boot frames. */
  if ((fn = jni_find_native("nativeRecreateGfxState"))) {
    fprintf(stderr, "[F2] surfaceChanged: nativeRecreateGfxState...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, &thiz, 0, &surf);
  }
  if ((fn = jni_find_native("nativeSendSurfaceChangedEvent"))) {
    fprintf(stderr, "[F2] nativeSendSurfaceChangedEvent...\n");
    ((void (*)(void *, void *))fn)(env, &thiz);
    fprintf(stderr, "[F2] nativeSendSurfaceChangedEvent OK\n");
  }
  if ((fn = jni_find_native("nativeResume"))) ((void (*)(void *, void *))fn)(env, &thiz);
  if ((fn = jni_find_native("nativeFocusChanged"))) ((void (*)(void *, void *, int))fn)(env, &thiz, 1);

  /* dispara a thread de áudio do FMOD (alimenta fmodProcess em paralelo ao
     render — destrava o boot que espera o mixer). CUP_NOAUDIOTHREAD=1 desliga.
     s14: a própria thread checa opensles_shim_engine_active() e se desliga
     quando o OpenSL (FORCESL) assumiu o mixer. */
  if (getenv("TER_AUDIO")) {  /* experimento de som — gated, OFF no run.sh (não quebra o build OK) */
    g_fmod_env = env;
    g_fmod_stopped = 0;
    pthread_t at;
    if (pthread_create(&at, NULL, fmod_audio_thread, NULL) == 0) {
      pthread_detach(at);
      fprintf(stderr, "[AUDIO] thread de áudio FMOD criada\n");
    } else {
      g_fmod_stopped = 1;
      fprintf(stderr, "[AUDIO] falha ao criar thread FMOD\n");
    }
  }
  if (getenv("CUP_PRELOAD_TICK")) {
    pthread_t tt; pthread_create(&tt, NULL, preload_tick_thread, NULL);
    pthread_detach(tt);
  }
  if (getenv("CUP_PSPY")) {
    pthread_t st; pthread_create(&st, NULL, preload_spy_thread, NULL);
    pthread_detach(st);
  }
  if (getenv("CUP_PRELOAD_BG")) {
    pthread_t bt; pthread_create(&bt, NULL, preload_bg_thread, NULL);
    pthread_detach(bt);
  }
  void *render = jni_find_native("nativeRender");
  fprintf(stderr, "[F2] nativeRender=%p -> loop\n", render);
  int max_f = getenv("CUP_FRAMES") ? atoi(getenv("CUP_FRAMES")) : 0;
  int ter_verbose = getenv("TER_VERBOSE") ? 1 : 0;
  ter_install_term_handlers(); /* Unity may have installed handlers during boot. */
  g_render_tid = (int)syscall(SYS_gettid);   /* p/ recovery longjmp (CUP_SKIPBAD) */
  /* CUP_AUTOTAP: o disclaimer/menu espera "toque/botão pra continuar". Injeta
     periodicamente um botão de confirmação via nativeInjectEvent (KeyEvent) p/
     avançar. CUP_AUTOTAP=keycode (default 66=ENTER; 96=BUTTON_A, 23=DPAD_CENTER). */
  extern struct hk_inject_s { int action, keycode, source, deviceId, metaState, repeat,
                              scancode, flags, unicode; long eventTime, downTime; } g_hk_inject;
  extern void *hk_keyevent_object(void);
  void *inject = jni_find_native("nativeInjectEvent");
  int tapkey = getenv("CUP_AUTOTAP") ? atoi(getenv("CUP_AUTOTAP")) : 0;
  if (tapkey && inject) fprintf(stderr, "[AUTOTAP] keycode=%d via nativeInjectEvent=%p\n", tapkey, inject);
  /* CUP_DRAINPRELOAD=N: os ops de preload do título completam o background (jobmgr=0)
   * mas ficam presos na fila de INTEGRAÇÃO (integQ) pq a integração per-frame não roda
   * fora do WaitForAll. Dirigimos nós: N× UpdatePreloadingSingleStep(mgr,2,0x10) por frame
   * (limitado=não pendura; +FORCEINTEG p/ passar o gate de budget). mgr vem do PSPY. */
  int drainN = getenv("CUP_DRAINPRELOAD") ? atoi(getenv("CUP_DRAINPRELOAD")) : 0;
  void (*preload_step)(void *, int, int) = drainN ? (void (*)(void *, int, int))(g_unity_base + 0x8733a8) : NULL;
  if (drainN) fprintf(stderr, "[DRAINPRELOAD] %d steps/frame (UpdatePreloadingSingleStep=0x8733a8)\n", drainN);
  /* CUP_DRAINWAIT: chama WaitForAllAsyncOperationsToComplete(mgr) (0x873a90) 1×/frame.
   * Diferente do step cru, o WaitForAll roda o loop completo + a fase de "process"
   * (0x738a98) que DISPARA OS CALLBACKS de conclusão das async ops -> o FontLoader
   * vê as fontes como done e avança. ⚠️ pode pendurar se HasPendingOps nunca zerar. */
  int drainWait = getenv("CUP_DRAINWAIT") ? 1 : 0;
  void (*wait_all)(void *) = drainWait ? (void (*)(void *))(g_unity_base + 0x873a90) : NULL;
  if (drainWait) fprintf(stderr, "[DRAINWAIT] WaitForAll(mgr)=0x873a90 1x/frame\n");
  /* CUP_DRIVECR: o pump de coroutine do engine PARA de resumir o start_cr no $PC=9
   * (Cuphead.Init/RefreshDLC) — render voa mas $PC fica preso. Dirigimos o MoveNext
   * nós mesmos a cada N frames (cr1_tramp = MoveNext real). Só age a partir do
   * CUP_DRIVECR_FROM (default 200, dá tempo do boot normal rodar) p/ não atropelar
   * o pump do engine nas fases iniciais. */
  extern long cr1_tramp(void *it); extern void *volatile g_startcr_it;
  int drivecr = getenv("CUP_DRIVECR") ? 1 : 0;
  int drivecr_from = getenv("CUP_DRIVECR_FROM") ? atoi(getenv("CUP_DRIVECR_FROM")) : 200;
  if (drivecr) fprintf(stderr, "[DRIVECR] dirige start_cr MoveNext a partir do frame %d\n", drivecr_from);
  /* CUP_GCOFF: desabilita o GC do il2cpp durante o boot. Hipótese: o crash flaky do
     $PC=9 é use-after-free — o Boehm GC coleta um objeto que a desserialização do
     CupheadCore ainda referencia (a integração forçada cria objeto que o GC não
     rastreia). Sem GC no boot, nada é coletado -> sem UAF. (heap cresce; re-habilitar
     depois do título se preciso.) */
  int gcoff = (getenv("CUP_GCOFF") && g_il2cpp_base) ? 1 : 0;
  /* religa o GC no frame CUP_GCON_F (default 350, bem depois do boot ~frame 200) p/ o
     heap NÃO crescer indefinido (parado no disclaimer = OOM/thrash). 0 = nunca religa. */
  int gcon_f = getenv("CUP_GCON_F") ? atoi(getenv("CUP_GCON_F")) : 350;
  if (gcoff) {
    ((void (*)(void))(ter_i2sym("il2cpp_gc_disable", 0x73ca6c)))();  /* il2cpp_gc_disable */
    fprintf(stderr, "[GCOFF] il2cpp_gc_disable() no boot; religa GC no frame %d\n", gcon_f);
  }
  /* TER_NOGCWAIT: o muro do frame 2 = il2cpp GC stop-the-world (WaitForThreadsToSuspend
     @libil2cpp+0x74f260) que ESPERA p/ sempre uma thread cooperativa (que bloqueia SIGPWR)
     dar ACK do suspend e ela nunca chega num safepoint. EXPERIMENTO: patcha a fn p/ retornar
     0 (=todas suspensas) imediatamente, deixando o GC seguir. ⚠️ pode scan stack de thread
     viva (com GC desligado o scan é mínimo). */
  if (getenv("TER_NOGCWAIT") && g_il2cpp_base) {
    uintptr_t a = g_il2cpp_base + 0x74f260;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)(a & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)a = 0x52800000u;        /* mov w0, #0 */
    *(uint32_t *)(a + 4) = 0xd65f03c0u;  /* ret */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + pgsz * 2);
    fprintf(stderr, "[NOGCWAIT] WaitForThreadsToSuspend 0x74f260 -> return 0\n");
  }
  /* TER_SKIPTASKWAIT: PLANO B — pula a wait do job-queue em libunity+0x2f37b0 (a main constrói
     uma future-task no init de serialização e BLOQUEIA p/ sempre pq o worker nunca produz). A
     saída (0x2f37c4) só faz mutex_unlock+ret (NÃO deref o item), então pular é razoável p/ ver o
     PRÓXIMO muro. Patcha `cbnz x8, 0x2f37c4` -> `b 0x2f37c4` (sai sem esperar). */
  if (getenv("TER_SKIPTASKWAIT") && g_unity_base) {
    uintptr_t a = g_unity_base + 0x2f37b0;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)(a & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)a = 0x14000005u;   /* b 0x2f37c4 (+0x14) — sempre sai do loop de espera */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + pgsz * 2);
    fprintf(stderr, "[SKIPTASKWAIT] libunity+0x2f37b0 cbnz->b (pula a wait do job-queue)\n");
  }
  /* TER_INLINETASK: instala um trampolim no TOPO do loop de espera do per-object task (0x2f37a4)
     que chama ter_inline_task(obj) (finge a conclusão: seta o nó + incrementa c10360) e então
     executa a instrução original + volta. Destrava per-object task (frame 2) E WaitForJobGroup
     (frame 3) sem depender do dispatch p/ workers. (Substitui o SKIPTASKWAIT — NÃO usar ambos.) */
  if (getenv("TER_INLINETASK") && g_unity_base) {
    extern void ter_inline_task(void *);
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t patch = g_unity_base + 0x2f37a4;
    /* trampolim numa página RWX PERTO da libunity (b tem alcance ±128MB) */
    uintptr_t hint = (g_unity_base + 0x2000000) & ~((uintptr_t)pgsz - 1);
    void *tp = mmap((void *)hint, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tp == MAP_FAILED) tp = mmap(NULL, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long d = (long)((uintptr_t)tp) - (long)patch;
    if (tp != MAP_FAILED && d > -0x7000000L && d < 0x7000000L) {
      uint32_t *t = (uint32_t *)tp;
      t[0] = 0xF81F0FFEu;          /* str x30,[sp,#-16]!  */
      t[1] = 0xAA1303E0u;          /* mov x0, x19 (obj)   */
      t[2] = 0x580000D0u;          /* ldr x16,[pc+0x18] -> fn@T+0x20 */
      t[3] = 0xD63F0200u;          /* blr x16             */
      t[4] = 0xF84107FEu;          /* ldr x30,[sp],#16    */
      t[5] = 0xF9402E68u;          /* ldr x8,[x19,#88] (instr original) */
      t[6] = 0x58000090u;          /* ldr x16,[pc+0x10] -> dst@T+0x28 */
      t[7] = 0xD61F0200u;          /* br x16              */
      *(uint64_t *)((char *)tp + 0x20) = (uint64_t)(uintptr_t)ter_inline_task;
      *(uint64_t *)((char *)tp + 0x28) = (uint64_t)(g_unity_base + 0x2f37a8);
      __builtin___clear_cache((char *)tp, (char *)tp + pgsz);
      /* patch 0x2f37a4 -> b trampolim */
      void *pp = (void *)(patch & ~((uintptr_t)pgsz - 1));
      mprotect(pp, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
      *(uint32_t *)patch = 0x14000000u | (uint32_t)(((d) >> 2) & 0x03FFFFFF);
      mprotect(pp, pgsz * 2, PROT_READ | PROT_EXEC);
      __builtin___clear_cache((char *)pp, (char *)pp + pgsz * 2);
      fprintf(stderr, "[INLINETASK] trampolim @%p (d=0x%lx) patch 0x2f37a4->b\n", tp, d);
    } else {
      fprintf(stderr, "[INLINETASK] FALHOU mmap/alcance (tp=%p d=0x%lx)\n", tp, d);
    }
  }
  /* TER_SKIPJOBWAIT: pula TAMBÉM o WaitForJobGroup (0x2f1d1c): `while([0xc10360]<target) cond_wait`.
     ⚠️ causa abort (job results incompletos são necessários) — só p/ diagnóstico. */
  if (getenv("TER_SKIPJOBWAIT") && g_unity_base) {
    uintptr_t b = g_unity_base + 0x2f1d48;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pb = (void *)(b & ~((uintptr_t)pgsz - 1));
    mprotect(pb, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)b = 0x14000005u;   /* b 0x2f1d5c (+0x14) */
    mprotect(pb, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pb, (char *)pb + pgsz * 2);
    fprintf(stderr, "[SKIPJOBWAIT] libunity+0x2f1d48 WaitForJobGroup -> sai imediato\n");
  }
  /* TER_FORCETHREADED: o flag "threaded" do job-system (libunity+0xc0da20) fica 0 no nosso env
     (a capability/boot.config retorna 0) → o scheduler NUNCA despacha p/ os worker threads (que
     EXISTEM e estão parked) → roda "inline" mas o inline não incrementa o contador (0xc10360) →
     WaitForJobGroup trava p/ sempre (gdb: flag=0, counter=0, main em 0x2f1d58 cond_wait).
     FIX: (1) patcha o `cset w20,ne` (0x2eaacc) que computa o flag → `mov w20,#1` (sempre threaded);
     (2) escreve 1 direto no byte (caso o init já tenha rodado antes deste patch). */
  if (getenv("TER_FORCETHREADED") && g_unity_base) {
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t c = g_unity_base + 0x2eaacc;            /* cset w20, ne */
    void *pc = (void *)(c & ~((uintptr_t)pgsz - 1));
    mprotect(pc, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)c = 0x52800034u;                     /* mov w20, #1 */
    mprotect(pc, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pc, (char *)pc + pgsz * 2);
    volatile uint8_t *flag = (uint8_t *)(g_unity_base + 0xc0da20);
    *flag = 1;
    fprintf(stderr, "[FORCETHREADED] flag c0da20 -> 1 (cset->mov#1 @0x2eaacc + write byte)\n");
  }
  /* TER_JOBSPY: lê os contadores globais do job system (0xc10350..0xc10370) periódico p/
     diagnosticar agendado-vs-completado. */
  int jobspy = getenv("TER_JOBSPY") ? 1 : 0;
  int gamepad_on = getenv("CUP_GAMEPAD") ? 1 : 0;
  extern void gp_poll(void); extern void gp_frame_end(void);
  /* CUP_LOADYIELD=us: durante o boot/load, cede CPU aos WORKER threads (sched_yield+usleep)
     ANTES de cada frame integrar, p/ os jobs async COMPLETAREM antes da integração forçada
     (o check jobs-pending 0x6cdad0 é não-confiável aqui -> race -> objeto malformado ->
     crash $PC=9). Só nos primeiros LOADYIELD_F frames (fase de load). */
  int loadyield = getenv("CUP_LOADYIELD") ? atoi(getenv("CUP_LOADYIELD")) : 0;
  int loadyield_f = getenv("CUP_LOADYIELD_F") ? atoi(getenv("CUP_LOADYIELD_F")) : 350;
  if (loadyield) fprintf(stderr, "[LOADYIELD] %dus/frame nos 1ºs %d frames (CPU p/ workers)\n", loadyield, loadyield_f);
  /* CUP_MEMLOG: telemetria de memória a cada ~10s (600 frames) — p/ achar a curva
     do vazamento que mata o device ~6-7min de render (thrash->sshd starved->freeze) */
  int memlog = getenv("CUP_MEMLOG") ? 1 : 0;
  /* CUP_SCENESPY: dump periódico do SceneManager nativo; CUP_SETACTIVE: conserta
     cena ativa NULL (raiz provável do player-fantasma do mapa, s14) */
  int scenespy = getenv("CUP_SCENESPY") ? 1 : 0;
  int setactive = getenv("CUP_SETACTIVE") ? 1 : 0;
  /* CUP_GCEVERY=N: força il2cpp_gc_collect a cada N frames (contém heap Boehm) */
  int gcevery = getenv("CUP_GCEVERY") ? atoi(getenv("CUP_GCEVERY")) : 0;
  int gc_pending = 0, gc_idle = 0;
  for (int f = 0; render && !g_ter_quit_requested &&
                      (max_f <= 0 || f < max_f); f++) {
    g_render_frame = f;  /* CUP_DRAWSPY: amarra os draws ao frame */
    if (memlog && f % 150 == 0) {
      static long last_swfree = -1; static int verbose_until = 0;
      long avail = -1, swfree = -1, rss = -1; char ln[128];
      FILE *mi = fopen("/proc/meminfo", "r");
      if (mi) { while (fgets(ln, sizeof ln, mi)) {
          sscanf(ln, "MemAvailable: %ld", &avail); sscanf(ln, "SwapFree: %ld", &swfree); }
        fclose(mi); }
      /* burst de swap (>8MB desde a última amostra) -> amostragem densa por 1200f */
      if (last_swfree >= 0 && last_swfree - swfree > 8192) verbose_until = f + 1200;
      last_swfree = swfree;
      if (f % 600 == 0 || f < verbose_until) {
        FILE *st = fopen("/proc/self/status", "r");
        if (st) { while (fgets(ln, sizeof ln, st)) sscanf(ln, "VmRSS: %ld", &rss); fclose(st); }
        long gch = ((long (*)(void))(g_il2cpp_base + 0x1b62ad4))();  /* il2cpp_gc_get_heap_size */
        long gcu = ((long (*)(void))(g_il2cpp_base + 0x1b62ad0))();  /* il2cpp_gc_get_used_size */
        fprintf(stderr, "[MEM] f=%d avail=%ldMB swfree=%ldMB rss=%ldMB gcheap=%ldMB gcused=%ldMB\n",
                f, avail / 1024, swfree / 1024, rss / 1024, gch >> 20, gcu >> 20);
        fsync(2);
      }
    }
    if (getenv("CUP_STAGESPY") && f % 300 == 0) {
      fprintf(stderr, "[STAGESPY] f=%d set_sprite=%u (null=%u) loadAsync=%u\n",
              f, g_ss_set, g_ss_null, g_ss_async); fsync(2);
    }
    if (scenespy && f % 600 == 0) scenespy_dump("tick");
    if (setactive && f % 30 == 0) setactive_fix();
    if (gcevery && f > gcon_f && f % gcevery == 0) gc_pending = 1;
    if (gc_pending) {
      /* limpeza de transição (ideia do usuário): solta assets da cena anterior +
         coleta o heap — sem isso o load da cena nova SOMA com a velha -> burst
         de ~150MB -> swap storm -> device asfixia.
         ⚠️ s12: SÓ com o PreloadManager OCIOSO (preloadQ[+224]==0 e integQ[+256]==0
         por 90 frames seguidos). O tick cego da s11 caiu NO MEIO do load assíncrono
         da cena do mapa (f=10800, rss subindo) e varreu objetos ainda não
         enraizados -> GameObject com scene NULL -> crash 0x541cdc. */
      char *m = (char *)g_preload_mgr;
      int idle = !m || (*(volatile uintptr_t *)(m + 224) == 0 &&
                        *(volatile uintptr_t *)(m + 256) == 0);
      gc_idle = idle ? gc_idle + 1 : 0;
      if (gc_idle >= (m ? 90 : 1200)) {   /* sem mgr capturado: espera ~20s */
        gc_pending = 0; gc_idle = 0;
        fprintf(stderr, "[GCEVERY] limpeza f=%d (mgr %s)\n", f, m ? "ocioso" : "n/d");
        ((void *(*)(void))(g_il2cpp_base + 0x178BAAC))(); /* Resources.UnloadUnusedAssets */
        ((void (*)(void))(ter_i2sym("il2cpp_gc_collect", 0x73ca5c)))();  /* il2cpp_gc_collect */
      }
    }
    { /* log de hits do SCENESKIP + MASKGUARD + NULLGUARD */
      static uint32_t sg_last, mg_last;
      if (g_sceneskip_hits != sg_last) {
        fprintf(stderr, "[SCENESKIP] GO sem scene pulado (%u hits, f=%d) — sobrevivido\n",
                g_sceneskip_hits, f); fsync(2);
        sg_last = g_sceneskip_hits;
        if (scenespy) scenespy_dump("skip");   /* estado do mgr NO momento do problema */
      }
      if (g_maskguard_hits != mg_last) {
        fprintf(stderr, "[MASKGUARD] count insana clampada (%u hits, f=%d) — sobrevivido\n",
                g_maskguard_hits, f); fsync(2);
        mg_last = g_maskguard_hits;
      }
      static uint32_t ng_last;
      if (g_nullguard_hits != ng_last) {
        fprintf(stderr, "[NULLGUARD] arg0 NULL skipado (%u hits, f=%d) — sobrevivido\n",
                g_nullguard_hits, f); fsync(2);
        ng_last = g_nullguard_hits;
      }
    }
    if (gcoff && gcon_f > 0 && f == gcon_f) {
      ((void (*)(void))(ter_i2sym("il2cpp_gc_enable", 0x73ca68)))();  /* il2cpp_gc_enable */
      ((void (*)(void))(ter_i2sym("il2cpp_gc_collect", 0x73ca5c)))();  /* il2cpp_gc_collect */
      fprintf(stderr, "[GCOFF] GC RELIGADO + collect no frame %d (boot ja passou)\n", f);
      fflush(stderr);
    }
    if (loadyield && f < loadyield_f) { for (int y = 0; y < 4; y++) sched_yield(); usleep(loadyield); }
    if (gamepad_on) gp_poll();   /* drena eventos do js0 ANTES do frame ler input */
    if (drivecr && f >= drivecr_from && g_startcr_it) cr1_tramp(g_startcr_it);
    if (drainN && g_preload_mgr) {
      for (int k = 0; k < drainN; k++) preload_step(g_preload_mgr, 2, 0x10);
    }
    if (drainWait && g_preload_mgr) {
      /* zera [mgr+0xE0] (flag GfxDevice +224) p/ WaitForAll só checar o integQ [+256]
       * e NÃO pendurar no loop (com MT-off o flag +224 fica setado p/ sempre). */
      if (getenv("CUP_DRAINWAIT_GFX")) *(volatile int *)((char *)g_preload_mgr + 0xE0) = 0;
      wait_all(g_preload_mgr);
    }
    if (ter_verbose && f < 200) { fprintf(stderr, "[r%d>\n", f); dbg_sync(); }
    unsigned char keep_rendering = 1;
    if (g_skipbad) {
      /* arma o recovery: se nativeRender crashar nesta thread, volta aqui e pula o frame */
      if (sigsetjmp(g_render_jmp, 1) == 0) {
        g_render_jmp_armed = 1;
        keep_rendering =
            ((unsigned char (*)(void *, void *))render)(env, &thiz);
      } else {
        if (g_recover_n < 80 || (g_recover_n % 60) == 0)
          fprintf(stderr, "[RECOVER] frame %d pulado (crash #%lu na render)\n", f, g_recover_n);
      }
      g_render_jmp_armed = 0;
    } else {
      keep_rendering =
          ((unsigned char (*)(void *, void *))render)(env, &thiz);
    }
    /* UnityPlayer's Java render thread treats false as an engine-owned quit
     * signal (for example Application.Quit). Calling nativeRender
     * again after that signal waits on teardown work that only the missing
     * Android Activity would drive.  The proven Prizefighters 2 loader stops
     * at this boundary; converge on our existing focus-loss/pause watchdog. */
    if (!keep_rendering) {
      fprintf(stderr,
              "[F2] nativeRender solicitou encerramento no frame %d\n", f);
      ter_request_quit();
      break;
    }
    if (ter_verbose && f < 200) { fprintf(stderr, "<r%d]\n", f); dbg_sync(); }
    opensles_shim_pump_callbacks();
    /* bombeia eventos SDL (foco/janela) p/ o input do Unity não esfomear */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT ||
          (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
        ter_request_quit();
    }
    /* AUTOTAP: a cada ~90 frames, manda DOWN; ~3 frames depois, UP (1 "toque") */
    if (tapkey && inject && f > 120) {
      int phase = f % 90;
      if (phase == 0 || phase == 3) {
        g_hk_inject.action = (phase == 0) ? 0 : 1;   /* 0=DOWN 1=UP */
        g_hk_inject.keycode = tapkey;
        g_hk_inject.source = 0x501;                  /* gamepad|keyboard */
        g_hk_inject.deviceId = 0; g_hk_inject.repeat = 0; g_hk_inject.flags = 0;
        g_hk_inject.metaState = 0; g_hk_inject.scancode = 0; g_hk_inject.unicode = 0;
        int ir = ((int (*)(void *, void *, void *))inject)(env, &thiz, hk_keyevent_object());
        if (f < 600) fprintf(stderr, "[AUTOTAP] %s key=%d (f=%d) ret=%d\n", phase ? "UP" : "DOWN", tapkey, f, ir);
      }
    }
    if (gamepad_on) gp_frame_end();  /* snapshot p/ edge-detect do GetButtonDown/Up */
    if (ter_verbose && f % 60 == 0) { fprintf(stderr, "[render %d]\n", f); dbg_sync(); }
    { /* FPS médio por janela de 600 frames (mede lag do mapa/fases p/ tuning) */
      static struct timespec t0; static int f0 = -1;
      if (ter_verbose && f % 600 == 0) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        if (f0 >= 0) {
          double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
          if (dt > 0.5)
            fprintf(stderr, "[FPS] f=%d media=%.1f draws/f=%lu kverts/f=%lu lo/f=%lu (janela %d frames / %.1fs)\n",
                    f, (f - f0) / dt,
                    (f > f0) ? g_frame_draws / (unsigned)(f - f0) : 0,
                    (f > f0) ? (g_frame_verts / (unsigned)(f - f0)) / 1000 : 0,
                    (f > f0) ? g_draws_lo / (unsigned)(f - f0) : 0,
                    f - f0, dt);
        }
        t0 = t1; f0 = f; g_frame_draws = 0; g_frame_verts = 0; g_draws_lo = 0;
      }
    }
  }
  fprintf(stderr, "[F2] === render loop terminou ===\n");
  fflush(stderr); dbg_sync();

  /* Mirror Android's foreground-loss/pause sequence. The manually mapped
     libraries still cannot safely run ELF destructors, so _exit remains only
     the final process boundary after lifecycle callbacks and audio quiesce. */
  struct sigaction sx; memset(&sx, 0, sizeof(sx));
  sx.sa_handler = ter_shutdown_timeout; sigemptyset(&sx.sa_mask);
  sigaction(SIGALRM, &sx, NULL);
  /* 15s (era 5): num SD lento, nativePause carrega o save final; 5s matava o
   * jogo NO MEIO da escrita do .wld/.map. O handler agora faz sync(), mas o
   * caminho certo e deixar o save terminar e sair pelo sync() daqui de baixo. */
  alarm(15);
  if ((fn = jni_find_native("nativeFocusChanged")))
    ((void (*)(void *, void *, int))fn)(env, &thiz, 0);
  if ((fn = jni_find_native("nativePause")))
    ((void (*)(void *, void *))fn)(env, &thiz);
  g_fmod_run = 0;
  for (int i = 0; !g_fmod_stopped && i < 200; i++) usleep(5000);
  sync();
  dbg_sync();
  alarm(0);
  _exit(0);
}
