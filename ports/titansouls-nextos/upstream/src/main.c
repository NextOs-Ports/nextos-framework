/*
 * main.c -- TITAN SOULS (Devolver / Abstraction Games, engine C++ propria
 * `libTestSuite.so`, NativeActivity + android_main, GLES2 puro, FMOD Ex)
 * nxloader ARMv7 SOFTFP para NextOS / Mali-450.
 *
 * Forma do loader = ports/castleofillusion (gemeo estrutural aprovado):
 * FMOD Ex entra como MODULO SEPARADO e a engine cross-resolve nele; depois
 * montamos o android_app e chamamos android_main.
 * ABI ARM32/ARMHF = ports/asm2_127 / TASM2 v1.1.7 (aprovado no Mali-450):
 * nxloader, bionic_compat, pthread_bridge e a ponte SOFTFP.
 *
 * FLUXO NATIVO: nao pulamos estado nenhum. Entregamos, na ordem que a
 * Activity real do Android entrega, INPUT_CHANGED -> START -> RESUME ->
 * INIT_WINDOW -> GAINED_FOCUS, e deixamos a maquina de estados da engine
 * andar sozinha (splash -> menu -> carregar -> gameplay).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <libgen.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "android_shim.h"
#include "audio_recovery_policy.h"
#include "asset_shim.h"
#include "bionic_compat.h"
#include "egl_shim.h"
#include "framework_bridge.h"
#include "imports.h"
#include "jni_shim.h"
#include "language_menu.h"
#include "lifecycle.h"
#include "loader_compat.h"
#include "platform_shims.h"
#include "runtime_hooks.h"
#include "ts_loader.h"
#include "util.h"

#define FMOD_SO "lib/libfmodex.so"
#define GAME_SO "lib/libTestSuite.so"

static uintptr_t g_load_base;
static size_t g_load_size;

extern int ts_screen_w, ts_screen_h;

/* ---------------- simbolizacao de crash (ARM32) ---------------- */

static void resolve_addr(uintptr_t a, char *out, int outsz) {
  out[0] = 0;
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    unsigned long s, e;
    char perm[8], path[256];
    path[0] = 0;
    if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >= 3 &&
        a >= s && a < e) {
      const char *base = strrchr(path, '/');
      base = base ? base + 1 : (path[0] ? path : "?");
      snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
      break;
    }
  }
  fclose(f);
}

static void log_frame(const char *tag, uintptr_t pc) {
  char r[300];
  resolve_addr(pc, r, sizeof(r));
  fprintf(stderr, "  %s=%p %s", tag, (void *)pc, r);
  if (g_load_base && pc >= g_load_base && pc - g_load_base < g_load_size)
    fprintf(stderr, "  {libTestSuite.so+0x%lx}", (unsigned long)(pc - g_load_base));
  fprintf(stderr, "\n");
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  { char b[96];
    int n = snprintf(b, sizeof(b), "\n=== CRASH sig=%d addr=%p tid=%d pc=%08lx lr=%08lx ===\n",
                     sig, info->si_addr, (int)syscall(__NR_gettid),
                     (unsigned long)m->arm_pc, (unsigned long)m->arm_lr);
    ssize_t w = write(2, b, n); (void)w; }
  log_frame("PC", m->arm_pc);
  log_frame("LR", m->arm_lr);
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1,
          (unsigned long)m->arm_r2, (unsigned long)m->arm_r3);
  fprintf(stderr, "  sp=%08lx fp=%08lx ip=%08lx\n", (unsigned long)m->arm_sp,
          (unsigned long)m->arm_fp, (unsigned long)m->arm_ip);
  /* pilha crua: a engine e' Thumb com frame pointer irregular, entao varremos
   * a pilha por enderecos que caem dentro do modulo do jogo. */
  uintptr_t sp = m->arm_sp;
  int shown = 0;
  for (uintptr_t p = sp; p < sp + 0x800 && shown < 20; p += 4) {
    uintptr_t v = *(uintptr_t *)p;
    if (g_load_base && v > g_load_base && v - g_load_base < g_load_size) {
      fprintf(stderr, "  stack[%02d] %p {libTestSuite.so+0x%lx}\n", shown,
              (void *)v, (unsigned long)(v - g_load_base));
      shown++;
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}

static void bt_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  ucontext_t *u = (ucontext_t *)uc;
  fprintf(stderr, "\n[BT sig=%d tid=%d]\n", sig, (int)syscall(__NR_gettid));
  log_frame("PC", u->uc_mcontext.arm_pc);
  log_frame("LR", u->uc_mcontext.arm_lr);
  { uintptr_t sp = u->uc_mcontext.arm_sp; int shown = 0;
    for (uintptr_t p = sp; p < sp + 0x1000 && shown < 24; p += 4) {
      uintptr_t v = *(uintptr_t *)p;
      if (g_load_base && v > g_load_base && v - g_load_base < g_load_size) {
        fprintf(stderr, "  bt[%02d] {libTestSuite.so+0x%lx}\n", shown,
                (unsigned long)(v - g_load_base));
        shown++;
      }
    } }
  fflush(stderr);
}

static void install_crash_handler(void) {
  /* Pilha alternativa: se o SIGSEGV vier de estouro de pilha (ou de uma
   * thread com pilha curta), o handler nao teria onde rodar e o processo
   * morreria MUDO — que foi exatamente o que aconteceu na primeira
   * aparicao deste crash. */
  static char altstack[131072];
  stack_t ss;
  ss.ss_sp = altstack;
  ss.ss_size = sizeof(altstack);
  ss.ss_flags = 0;
  sigaltstack(&ss, NULL);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = bt_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGUSR1, &sa, NULL);
}

/* ---------------- carga de modulo ---------------- */

static void preload_device_libs(void) {
  static const char *libs[] = {"libGLESv2.so", "libEGL.so", "libm.so.6",
                               "libdl.so.2", "libz.so.1", NULL};
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    if (!h) debugPrintf("preload: %s indisponivel (%s)\n", libs[i], dlerror());
  }
}

/* ------- FMOD: manter o System::update vivo durante a espera da engine -----
 *
 * A engine abre a musica com FMOD_NONBLOCKING e espera por ela num laco
 * PROPRIO: AgAudioChannelFMOD::_play chama playSound e, enquanto o retorno for
 * FMOD_ERR_NOTREADY(54), faz AgThread::sleep(10) e tenta de novo — sem voltar
 * ao laco principal. Acontece que quem termina de abrir um som NONBLOCKING no
 * FMOD Ex e' o proprio System::update(), que a engine so' chamaria DEPOIS
 * desse laco (AgAudioManager::update roda os canais antes do update do
 * sistema). No Android o som normalmente ja' chega pronto e a espera nunca
 * acontece; aqui o quadro que carrega o mundo leva ~11 s (upload de textura no
 * Mali-450, medido) e a musica pede para tocar exatamente dentro dessa janela:
 * a espera comeca, o update nunca roda, e o jogo fica na tela branca para
 * sempre. Medido no device com backtrace de todas as threads: principal em
 * _play, workers ociosos.
 *
 * A correcao nao inventa estado nem pula etapa: enquanto a ENGINE dorme
 * esperando o som, nos chamamos o MESMO System::update() que ela chamaria — a
 * transicao que faltava passa a ser postada por quem devia. Fora desse laco o
 * comportamento e' identico ao original.
 *
 * Tudo por simbolo EXPORTADO (FMOD_System_Create/Update no libfmodex,
 * _ZN8AgThread5sleepEj na engine) — nada de RVA chutada. */
static void *g_fmod_system;
static int (*g_fmod_system_create_real)(void **system);
static int (*g_fmod_system_update)(void *system);
static unsigned int g_fmod_opensl_retry_count;
static pthread_t g_main_thread;

int ts_FMOD_System_Create(void **system) {
  if (!g_fmod_system_create_real) return 33 /* FMOD_ERR_INTERNAL */;
  int r = g_fmod_system_create_real(system);
  if (r == 0 && system && *system) {
    g_fmod_system = *system;
    g_fmod_opensl_retry_count = 0u;
    logPrintf("fmod: System criado (%p) — update sera' bombeado na espera\n",
              g_fmod_system);
  }
  return r;
}

/* platformUpdate() e' quem, do lado da engine, faz o AgPriorityThreadPool do
 * audio andar (e' ele que carrega o AudioStreamJob empilhado pelo callback
 * assincrono do FMOD). Enquanto a engine dorme esperando o som, ninguem o
 * chama — entao chamamos nos, com o MESMO singleton que ela usa. */
static void (*g_ag_platform_update)(void *self);
static void **g_ag_audio_singleton;

static void ts_AgThread_sleep(unsigned int ms) {
  /* So' a thread principal bombeia: e' ela quem ficaria presa no laco de
   * espera e e' dela que o FMOD espera o update. */
  if (pthread_equal(pthread_self(), g_main_thread)) {
    if (g_ag_platform_update && g_ag_audio_singleton && *g_ag_audio_singleton)
      g_ag_platform_update(*g_ag_audio_singleton);
    if (g_fmod_system && g_fmod_system_update)
      g_fmod_system_update(g_fmod_system);
  }
  if (ms > 1000u) ms = 1000u;
  usleep(ms ? ms * 1000u : 1000u);
}

/* ---- Abrir o stream de forma SINCRONA -------------------------------------
 * Provado no device: com FMOD_NONBLOCKING a engine entra no laco de
 * AgAudioChannelFMOD::_play e o som NUNCA fica pronto — nem bombeando
 * System::update e platformUpdate de fora. O caminho que resta, e que e' um
 * modo legitimo do proprio FMOD Ex, e' abrir sincronamente: para isso o FMOD
 * precisa do read SINCRONO da engine (fmodFileRead, que existe) e NAO pode ver
 * um callback assincrono registrado — se ve, recusa o open bloqueante com
 * FMOD_ERR_INTERNAL. Entao as duas coisas andam juntas:
 *   setFileSystem(..., asyncread=NULL, asynccancel=NULL)
 *   createStream/createSound sem FMOD_NONBLOCKING
 * A engine faz a MESMA chamada; so' tiramos a flag que este ambiente nao
 * consegue honrar. */
#define TS_FMOD_NONBLOCKING 0x00010000u

typedef int (*ts_createsound_fn)(void *system, const char *name,
                                 unsigned int mode, void *exinfo, void **sound);
typedef int (*ts_setfs_fn)(void *system, void *open, void *close, void *read,
                           void *seek, void *aread, void *acancel, int align);
typedef int (*ts_fmod_init_fn)(void *system, int maxchannels,
                               unsigned int flags, void *extradriverdata);
typedef int (*ts_fmod_getoutput_fn)(void *system, int *output);
typedef int (*ts_fmod_setoutput_fn)(void *system, int output);
typedef int (*ts_fmod_getnumdrivers_fn)(void *system, int *drivers);
typedef int (*ts_fmod_getversion_fn)(void *system, unsigned int *version);

static ts_createsound_fn g_create_stream_real, g_create_sound_real;
static ts_setfs_fn g_setfs_real;
static ts_fmod_init_fn g_fmod_init_real;
static ts_fmod_getoutput_fn g_fmod_getoutput_real;
static ts_fmod_setoutput_fn g_fmod_setoutput_real;
static ts_fmod_getnumdrivers_fn g_fmod_getnumdrivers_real;
static ts_fmod_getversion_fn g_fmod_getversion_real;

static const char *ts_fmod_result_name(int result) {
  switch (result) {
    case 0: return "FMOD_OK";
    case 31: return "FMOD_ERR_INITIALIZATION";
    case 32: return "FMOD_ERR_INITIALIZED";
    case 33: return "FMOD_ERR_INTERNAL";
    case 48: return "FMOD_ERR_NEEDSHARDWARE";
    case 55: return "FMOD_ERR_OUTPUT_ALLOCATED";
    case 56: return "FMOD_ERR_OUTPUT_CREATEBUFFER";
    case 57: return "FMOD_ERR_OUTPUT_DRIVERCALL";
    case 58: return "FMOD_ERR_OUTPUT_ENUMERATION";
    case 59: return "FMOD_ERR_OUTPUT_FORMAT";
    case 60: return "FMOD_ERR_OUTPUT_INIT";
    case 61: return "FMOD_ERR_OUTPUT_NOHARDWARE";
    case 62: return "FMOD_ERR_OUTPUT_NOSOFTWARE";
    case 66: return "FMOD_ERR_PLUGIN_MISSING";
    case 79: return "FMOD_ERR_UNINITIALIZED";
    default: return "FMOD_RESULT";
  }
}

int ts_fmod_init(void *system, int maxchannels, unsigned int flags,
                 void *extradriverdata) {
  unsigned int version = 0u;
  int output = -1;
  int drivers = -1;
  int version_result;
  int output_result;
  int drivers_result;
  int selected_opensl = 0;
  int audio_disabled = getenv("TS_NOAUDIO") != NULL;
  int result;

  if (!g_fmod_init_real || !g_fmod_getoutput_real ||
      !g_fmod_setoutput_real || !g_fmod_getnumdrivers_real ||
      !g_fmod_getversion_real)
    return 33;

  version_result = g_fmod_getversion_real(system, &version);
  output_result = g_fmod_getoutput_real(system, &output);
  drivers_result = g_fmod_getnumdrivers_real(system, &drivers);
  logPrintf("[fmod] pre-init version=0x%08x(%d) output=%d(%d) "
            "drivers=%d(%d)\n", version, version_result, output,
            output_result, drivers, drivers_result);
  logPrintf("[fmod] init args maxchannels=%d flags=0x%08x extra=%p\n",
            maxchannels, flags, extradriverdata);

  /* The engine already tried AUTODETECT in _selectBestDevice.  With no
   * Android driver it changes the output to NOSOUND, which would make init
   * succeed while violating the required audio capability.  Recover only
   * that observed condition and only for the exact audited FMOD ABI. */
  if (ts_audio_select_opensl_preinit(audio_disabled, version_result, version,
                                     output_result, output)) {
    int select_result =
        g_fmod_setoutput_real(system, TS_FMOD_OUTPUT_OPENSL);
    logPrintf("[fmod] default sem driver real; setOutput(OPENSL=%d) -> "
              "%s(%d)\n", TS_FMOD_OUTPUT_OPENSL,
              ts_fmod_result_name(select_result), select_result);
    if (select_result != 0)
      return select_result;
    selected_opensl = 1;
  }

  result = g_fmod_init_real(system, maxchannels, flags, extradriverdata);
  logPrintf("[fmod] System::init output=%d -> %s(%d)\n",
            selected_opensl ? TS_FMOD_OUTPUT_OPENSL : output,
            ts_fmod_result_name(result), result);
  if (result == 0 ||
      !ts_audio_retry_opensl(
          audio_disabled, version_result, version, result,
          selected_opensl ||
              (output_result == 0 && output == TS_FMOD_OUTPUT_OPENSL),
          g_fmod_opensl_retry_count))
    return result;

  /* One bounded recovery after a real output/driver failure.  Never guess
   * the enum for an unknown guest and never turn a failed retry into OK. */
  {
    g_fmod_opensl_retry_count++;
    int select_result =
        g_fmod_setoutput_real(system, TS_FMOD_OUTPUT_OPENSL);
    logPrintf("[fmod] init falhou; setOutput(OPENSL=%d) -> %s(%d)\n",
              TS_FMOD_OUTPUT_OPENSL, ts_fmod_result_name(select_result),
              select_result);
    if (select_result != 0)
      return result;
  }
  result = g_fmod_init_real(system, maxchannels, flags, extradriverdata);
  logPrintf("[fmod] System::init retry OPENSL -> %s(%d)\n",
            ts_fmod_result_name(result), result);
  return result;
}

/* Escada de modos: o open bloqueante recusa combinacoes que o FMOD Ex desta
 * versao nao serve neste ambiente (HARDWARE nao existe fora do Android; o
 * stream por si so' pode falhar com o file system da engine). Em vez de
 * cravar um modo, tentamos na ordem e ficamos com o primeiro que abre — e o
 * log diz qual foi, para o proximo aparelho nao virar adivinhacao. */
#define TS_FMOD_HARDWARE   0x00000020u
#define TS_FMOD_SOFTWARE   0x00000040u
#define TS_FMOD_CREATESTREAM 0x00000080u
#define TS_FMOD_CREATESAMPLE 0x00000100u
#define TS_FMOD_CREATECOMPRESSEDSAMPLE 0x00000200u

int ts_fmod_createStream(void *system, const char *name, unsigned int mode,
                         void *exinfo, void **sound) {
  if (!g_create_stream_real) return 33;
  unsigned int base = mode & ~TS_FMOD_NONBLOCKING;
  unsigned int ladder[4];
  int n = 0;
  ladder[n++] = base;
  ladder[n++] = (base & ~TS_FMOD_HARDWARE) | TS_FMOD_SOFTWARE;
  ladder[n++] = ((base & ~TS_FMOD_HARDWARE & ~TS_FMOD_CREATESTREAM) |
                 TS_FMOD_SOFTWARE | TS_FMOD_CREATECOMPRESSEDSAMPLE);
  ladder[n++] = ((base & ~TS_FMOD_HARDWARE & ~TS_FMOD_CREATESTREAM) |
                 TS_FMOD_SOFTWARE | TS_FMOD_CREATESAMPLE);
  int r = 33;
  for (int i = 0; i < n; i++) {
    r = g_create_stream_real(system, name, ladder[i], exinfo, sound);
    if (r == 0) {
      static unsigned logged;
      if (logged < 4u) {
        logged++;
        logPrintf("[fmod] createStream('%s') 0x%x -> modo 0x%x OK\n",
                  name ? name : "?", mode, ladder[i]);
      }
      return 0;
    }
  }
  static unsigned failed;
  if (failed++ < 4u)
    logPrintf("[fmod] createStream('%s') 0x%x FALHOU em toda a escada (%d)\n",
              name ? name : "?", mode, r);
  return r;
}

int ts_fmod_createSound(void *system, const char *name, unsigned int mode,
                        void *exinfo, void **sound) {
  if (!g_create_sound_real) return 33;
  return g_create_sound_real(system, name, mode & ~TS_FMOD_NONBLOCKING, exinfo,
                             sound);
}

int ts_fmod_setFileSystem(void *system, void *open, void *close, void *read,
                          void *seek, void *aread, void *acancel, int align) {
  if (!g_setfs_real) return 33;
  logPrintf("[fmod] setFileSystem read=%p async=%p/%p align=%d -> SINCRONO\n",
            read, aread, acancel, align);
  if (!read)
    return g_setfs_real(system, open, close, read, seek, aread, acancel, align);
  /* blockalign negativo so' faz sentido no caminho assincrono (a engine pede
   * "sem bufferizacao, eu entrego os blocos"). Sem o async, o FMOD recusa o
   * open bloqueante com FMOD_ERR_INTERNAL; 0 = bufferizacao padrao dele. */
  return g_setfs_real(system, open, close, read, seek, NULL, NULL,
                      align < 0 ? 0 : align);
}

#define TS_SYM_STREAM \
  "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define TS_SYM_SOUND \
  "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define TS_SYM_SETFS                                                          \
  "_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKciPjPPvS6_EPFS1_S5_S5_EPF" \
  "S1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESA_i"
#define TS_SYM_INIT "_ZN4FMOD6System4initEijPv"
#define TS_SYM_GETOUTPUT "_ZN4FMOD6System9getOutputEP15FMOD_OUTPUTTYPE"
#define TS_SYM_SETOUTPUT "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE"
#define TS_SYM_GETNUMDRIVERS "_ZN4FMOD6System13getNumDriversEPi"
#define TS_SYM_GETVERSION "_ZN4FMOD6System10getVersionEPj"

static int bind_export(ts_loader *loader, ts_loader_module_id module_id,
                       const char *name, void *destination,
                       size_t destination_size) {
  uintptr_t address = 0u;
  nxloader_result result;
  if (!loader || !name || !destination ||
      destination_size != sizeof(address))
    return -1;
  result = ts_loader_find_export(loader, module_id, name, &address);
  if (result != NXLOADER_OK || address == 0u) {
    logPrintf("nxloader: export obrigatorio ausente: %s (%s)\n", name,
              nxloader_result_string(result));
    return -1;
  }
  memcpy(destination, &address, destination_size);
  return 0;
}

static int bind_fmod_sync(ts_loader *loader) {
  if (bind_export(loader, TS_LOADER_MODULE_FMOD, "FMOD_System_Create",
                  &g_fmod_system_create_real,
                  sizeof(g_fmod_system_create_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, "FMOD_System_Update",
                  &g_fmod_system_update, sizeof(g_fmod_system_update)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_STREAM,
                  &g_create_stream_real, sizeof(g_create_stream_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_SOUND,
                  &g_create_sound_real, sizeof(g_create_sound_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_SETFS,
                  &g_setfs_real, sizeof(g_setfs_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_INIT,
                  &g_fmod_init_real, sizeof(g_fmod_init_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_GETOUTPUT,
                  &g_fmod_getoutput_real,
                  sizeof(g_fmod_getoutput_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_SETOUTPUT,
                  &g_fmod_setoutput_real,
                  sizeof(g_fmod_setoutput_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_GETNUMDRIVERS,
                  &g_fmod_getnumdrivers_real,
                  sizeof(g_fmod_getnumdrivers_real)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_FMOD, TS_SYM_GETVERSION,
                  &g_fmod_getversion_real,
                  sizeof(g_fmod_getversion_real)) != 0)
    return -1;
  logPrintf("fmod: sync bind stream=%p sound=%p setfs=%p init=%p\n",
            (void *)g_create_stream_real, (void *)g_create_sound_real,
            (void *)g_setfs_real, (void *)g_fmod_init_real);
  return 0;
}

static int install_fmod_update_pump(ts_loader *loader) {
  static const unsigned char expected_sleep_entry[8] = {
      0x07, 0xee, 0x90, 0x0a, 0xb8, 0xee, 0x67, 0x7a,
  };
  uintptr_t sleep_address = 0u;
  nxloader_result result;

  if (bind_export(loader, TS_LOADER_MODULE_GAME,
                  "_ZN18AgAudioManagerFMOD14platformUpdateEv",
                  &g_ag_platform_update, sizeof(g_ag_platform_update)) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_GAME,
                  "_ZN11AgSingletonI14AgAudioManagerE11ms_instanceE",
                  &g_ag_audio_singleton, sizeof(g_ag_audio_singleton)) != 0 ||
      ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                            "_ZN8AgThread5sleepEj", &sleep_address) !=
          NXLOADER_OK)
    return -1;
  logPrintf("fmod: platformUpdate=%p singleton=%p\n",
            (void *)g_ag_platform_update, (void *)g_ag_audio_singleton);
  if (!sleep_address || (!g_fmod_system_update && !g_ag_platform_update)) {
    logPrintf("fmod: pump do update indisponivel (sleep=%p update=%p)\n",
              (void *)sleep_address, (void *)g_fmod_system_update);
    return -1;
  }
  if (memcmp((const void *)(sleep_address & ~(uintptr_t)1u),
             expected_sleep_entry, sizeof(expected_sleep_entry)) != 0) {
    logPrintf("fmod: assinatura de AgThread::sleep divergiu; hook recusado\n");
    return -1;
  }
  result = ts_loader_install_hook(loader, TS_LOADER_MODULE_GAME,
                                  sleep_address,
                                  (uintptr_t)&ts_AgThread_sleep, 8u);
  if (result != NXLOADER_OK) {
    logPrintf("fmod: nxloader recusou hook de sleep: %s\n",
              nxloader_result_string(result));
    return -1;
  }
  logPrintf("fmod: AgThread::sleep bombeia System::update (@0x%lx)\n",
            (unsigned long)sleep_address);
  return 0;
}

typedef void (*ts_android_main_fn)(struct android_app *app);

typedef struct ts_runtime_state {
  ts_loader *loader;
  struct android_app *app;
  ts_android_main_fn android_main;
  ts_lifecycle_runtime *lifecycle_runtime;
  int graphics_attempted;
  int graphics_published;
} ts_runtime_state;

static ts_runtime_state *g_active_runtime;

static void nxloader_log(void *userdata, nxloader_log_level level,
                         const char *message) {
  static const char *const names[] = {"error", "warning", "info", "debug"};
  const char *name = "unknown";
  (void)userdata;
  if ((unsigned)level < sizeof(names) / sizeof(names[0]))
    name = names[level];
  logPrintf("[nxloader/%s] %s\n", name,
            message ? message : "(sem diagnostico)");
}

static int lifecycle_verify_module(void *userdata,
                                   ts_lifecycle_module module) {
  ts_runtime_state *state = (ts_runtime_state *)userdata;
  ts_loader_module_id loader_module;
  nxloader_module_info info;
  if (!state || !state->loader)
    return -1;
  loader_module = module == TS_LIFECYCLE_MODULE_FMODEX
                      ? TS_LOADER_MODULE_FMOD
                      : TS_LOADER_MODULE_GAME;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  return ts_loader_get_info(state->loader, loader_module, &info) ==
                 NXLOADER_OK &&
             info.state == NXLOADER_STATE_INITIALIZED
         ? 0
         : -1;
}

static int lifecycle_create_activity(void *userdata) {
  ts_runtime_state *state = (ts_runtime_state *)userdata;
  if (!state || state->app)
    return -1;
  state->app = android_shim_init();
  if (!state->app)
    return -1;
  android_shim_install_exit_signals();
  return 0;
}

static int lifecycle_verify_graphics(void *userdata) {
  const ts_runtime_state *state = (const ts_runtime_state *)userdata;
  return state && state->graphics_published ? 0 : -1;
}

int ts_runtime_graphics_ready(const nxgl_report_v2 *report) {
  ts_runtime_state *state = g_active_runtime;
  int status;
  if (!state || !state->lifecycle_runtime || !report ||
      state->graphics_attempted)
    return -1;
  state->graphics_attempted = 1;
  if (ts_framework_publish_graphics(report) != 0)
    return -1;
  state->graphics_published = 1;
  status = ts_lifecycle_graphics_ready(state->lifecycle_runtime);
  if (status != 0)
    logPrintf("lifecycle: graphics-ready recusado (%d)\n", status);
  return status;
}

static int lifecycle_run_android_main(void *userdata,
                                      ts_lifecycle_runtime *runtime) {
  ts_runtime_state *state = (ts_runtime_state *)userdata;
  if (!state || !runtime || !state->app || !state->android_main ||
      g_active_runtime)
    return -1;

  state->lifecycle_runtime = runtime;
  g_active_runtime = state;
  /* Ordem nativa de NativeActivity. Nenhum comando ou entrada e' pulado. */
  android_shim_send_cmd(state->app, APP_CMD_INPUT_CHANGED);
  android_shim_send_cmd(state->app, APP_CMD_START);
  android_shim_send_cmd(state->app, APP_CMD_RESUME);
  android_shim_send_cmd(state->app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(state->app, APP_CMD_GAINED_FOCUS);

  logPrintf("=== chamando android_main (runtime delegado nxandroid) ===\n");
  logPrintf("lifecycle: aguardando receipt nxgl-v2 de egl_shim\n");
  state->android_main(state->app);
  logPrintf("=== android_main retornou ===\n");
  g_active_runtime = NULL;
  state->lifecycle_runtime = NULL;
  return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[]) {
  ts_loader_config loader_config;
  ts_loader *loader = NULL;
  ts_runtime_state runtime_state;
  ts_lifecycle_ops lifecycle_ops;
  nxloader_module_info game_info;
  nxloader_result loader_result;
  uintptr_t android_main_address = 0u;
  uintptr_t unexpected_jni_onload = 0u;
  int compat_bound = 0;

  (void)argc;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  g_main_thread = pthread_self();
  logPrintf("=== TITAN SOULS nxloader (ARMv7 softfp) / NextOS ===\n");

  /* gamedir, por ordem de confianca: override explicito > NXCOMPAT_GAME_DIR
   * (o launcher SEMPRE exporta o caminho certo) > NXBOOTSTRAP_EXE (caminho
   * real do binario, imune ao ld.so explicito) > /proc/self/exe > cwd.
   * Caso de campo (Miyoo Flip/spruce): rodando atraves do ld-linux alternativo
   * o /proc/self/exe aponta pro LD.SO -> gamedir virava a pasta do loader e o
   * preflight recusava com 'missing capability=host.armhf-libs'. */
  char gamedir[PATH_MAX];
  {
    const char *env = getenv("TS_GAMEDIR");
    const char *compat = getenv("NXCOMPAT_GAME_DIR");
    const char *exe = getenv("NXBOOTSTRAP_EXE");
    if (env && env[0]) {
      snprintf(gamedir, sizeof(gamedir), "%s", env);
    } else if (compat && compat[0]) {
      snprintf(gamedir, sizeof(gamedir), "%s", compat);
    } else if (exe && exe[0]) {
      char self[PATH_MAX];
      snprintf(self, sizeof(self), "%s", exe);
      snprintf(gamedir, sizeof(gamedir), "%s", dirname(self));
    } else {
      char self[PATH_MAX];
      ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
      if (n > 0) { self[n] = 0; snprintf(gamedir, sizeof(gamedir), "%s", dirname(self)); }
      else if (!getcwd(gamedir, sizeof(gamedir))) snprintf(gamedir, sizeof(gamedir), ".");
    }
  }
  if (chdir(gamedir) != 0) {
    logPrintf("chdir(%s) falhou\n", gamedir);
    return 1;
  }
  if (!getcwd(gamedir, sizeof(gamedir))) {
    logPrintf("getcwd depois de chdir falhou\n");
    return 1;
  }
  logPrintf("gamedir: %s\n", gamedir);

  /* O bridge precisa observar/aplicar o plano antes que SDL, EGL, audio ou
   * input adquiram recursos.  Ele tambem rejeita um game_dir nao absoluto. */
  if (ts_framework_preflight(gamedir) != 0) {
    logPrintf("framework: preflight universal recusado\n");
    return 1;
  }

  jni_shim_set_package("com.devolver.titansouls", 31);
  asset_shim_init(gamedir);
  if (ts_language_menu_prepare(gamedir) != 0) {
    logPrintf("language-menu: preflight recusado\n");
    return 1;
  }
  /* Os literais /sdcard/... do binario (OBB, TitanSoulsSave, logs de debug)
   * passam a apontar para <gamedir>/sdcard. */
  asm2_bionic_init(ts_paths_external());

  /* SDL base/audio/input podem subir aqui; nxgl-v2 possui SDL_INIT_VIDEO para
   * que sua transacao mantenha o retry/autodetect e o teardown coerentes. */
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
               SDL_INIT_GAMECONTROLLER) != 0) {
    logPrintf("SDL_Init falhou: %s\n", SDL_GetError());
    return 1;
  }
  /* REGRA #6: nunca forcamos SDL_VIDEODRIVER/SDL_AUDIODRIVER. O CFW escolhe. */
  { const char *db = "gamecontrollerdb.txt";
    if (access(db, R_OK) == 0 && SDL_GameControllerAddMappingsFromFile(db) > 0)
      logPrintf("gamecontrollerdb.txt do port carregado\n"); }

  egl_shim_create_window();
  if (!egl_shim_get_window()) {
    logPrintf("egl_shim: janela real nao foi criada\n");
    goto fail;
  }
  logPrintf("drawable real: %dx%d\n", ts_screen_w, ts_screen_h);

  /* O provider host consulta apenas a allowlist fixa e apenas uma vez.  As
   * bibliotecas do firmware precisam estar no escopo global antes disso. */
  preload_device_libs();

  ts_loader_config_init(&loader_config);
  loader_config.adapter_imports = ts_imports;
  loader_config.adapter_import_count = (size_t)ts_imports_count;
  loader_config.log = nxloader_log;
  loader_result = ts_loader_create(&loader_config, &loader);
  if (loader_result != NXLOADER_OK) {
    logPrintf("nxloader: create falhou: %s\n",
              nxloader_result_string(loader_result));
    goto fail;
  }

  /* Ordem de dependencia auditada: FMOD registra exports primeiro; a engine
   * entao resolve estritamente contra adapter + softfp + FMOD + host fixo. */
  loader_result = ts_loader_prepare(loader, FMOD_SO, GAME_SO);
  if (loader_result != NXLOADER_OK) {
    logPrintf("nxloader: prepare falhou: %s (%s)\n",
              nxloader_result_string(loader_result),
              ts_loader_last_error(loader));
    goto fail;
  }
  if (bind_fmod_sync(loader) != 0 ||
      bind_export(loader, TS_LOADER_MODULE_GAME, "android_main",
                  &android_main_address, sizeof(android_main_address)) != 0)
    goto fail;

  /* Zoom nativo: FP::GetZoom/SetZoom (FlashPunk) sao estaticos no engine.
   * Ausencia NAO e erro — o shim so desliga o recurso. */
  {
    uintptr_t zoom_get = 0u, zoom_set = 0u;
    if (ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                              "_ZN2FP7GetZoomEv", &zoom_get) != NXLOADER_OK)
      zoom_get = 0u;
    if (ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                              "_ZN2FP7SetZoomEf", &zoom_set) != NXLOADER_OK)
      zoom_set = 0u;
    android_shim_set_zoom_symbols((unsigned long)zoom_get,
                                  (unsigned long)zoom_set);
  }

  /* O APK 1.0.3 auditado declara JNI_NONE.  Encontrar JNI_OnLoad indica outro
   * guest e deve falhar, nunca criar uma sequencia Android improvisada. */
  loader_result = ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                                        "JNI_OnLoad",
                                        &unexpected_jni_onload);
  if (loader_result == NXLOADER_OK) {
    logPrintf("nxloader: guest inesperado exporta JNI_OnLoad @0x%lx\n",
              (unsigned long)unexpected_jni_onload);
    goto fail;
  }
  if (loader_result != NXLOADER_EUNRESOLVED) {
    logPrintf("nxloader: nao comprovou ausencia de JNI_OnLoad: %s\n",
              nxloader_result_string(loader_result));
    goto fail;
  }

  memset(&game_info, 0, sizeof(game_info));
  game_info.struct_size = sizeof(game_info);
  loader_result = ts_loader_get_info(loader, TS_LOADER_MODULE_GAME,
                                     &game_info);
  if (loader_result != NXLOADER_OK || !game_info.mapping_base ||
      game_info.mapping_size == 0u) {
    logPrintf("nxloader: mapping autoritativo do jogo indisponivel: %s\n",
              nxloader_result_string(loader_result));
    goto fail;
  }
  g_load_base = (uintptr_t)game_info.mapping_base;
  g_load_size = game_info.mapping_size;
  logPrintf("android_main @0x%lx mapping=%p+0x%lx\n",
            (unsigned long)android_main_address, game_info.mapping_base,
            (unsigned long)game_info.mapping_size);

  /* As shims intocadas ainda consultam EHABI e o intervalo do callback pelo
   * ABI historico.  O compat apenas publica views nxloader ja validadas. */
  if (ts_loader_compat_bind(loader) != 0) {
    logPrintf("nxloader: bind de compatibilidade EHABI falhou\n");
    goto fail;
  }
  compat_bound = 1;

  /* Este hook guest-especifico precisa entrar na janela PREPARED: finalize
   * fecha RX e RELRO.  A variavel serve apenas para diagnostico controlado;
   * a configuracao normal exige a correcao FMOD comprovada. */
  if (!getenv("TS_NO_FMOD_PUMP") && install_fmod_update_pump(loader) != 0)
    goto fail;
  if (ts_language_menu_install(loader, (uintptr_t)game_info.mapping_base,
                               game_info.mapping_size) != 0) {
    logPrintf("language-menu: instalacao recusada\n");
    goto fail;
  }

  loader_result = ts_loader_finalize(loader);
  if (loader_result != NXLOADER_OK) {
    logPrintf("nxloader: finalize falhou: %s (%s)\n",
              nxloader_result_string(loader_result),
              ts_loader_last_error(loader));
    goto fail;
  }
  loader_result = ts_loader_call_initializers(loader);
  if (loader_result != NXLOADER_OK) {
    logPrintf("nxloader: init_array falhou: %s (%s)\n",
              nxloader_result_string(loader_result),
              ts_loader_last_error(loader));
    goto fail;
  }

  memset(&runtime_state, 0, sizeof(runtime_state));
  runtime_state.loader = loader;
  memcpy(&runtime_state.android_main, &android_main_address,
         sizeof(runtime_state.android_main));
  memset(&lifecycle_ops, 0, sizeof(lifecycle_ops));
  lifecycle_ops.api_version = TS_LIFECYCLE_API_VERSION;
  lifecycle_ops.struct_size = sizeof(lifecycle_ops);
  lifecycle_ops.verify_module_initialized = lifecycle_verify_module;
  lifecycle_ops.create_activity = lifecycle_create_activity;
  lifecycle_ops.verify_graphics_ready = lifecycle_verify_graphics;
  lifecycle_ops.run_android_main = lifecycle_run_android_main;
  lifecycle_ops.userdata = &runtime_state;
  if (ts_lifecycle_run(&lifecycle_ops) != 0) {
    logPrintf("lifecycle: perfil nxandroid falhou fechado\n");
    goto fail;
  }

  /* _exit e nao exit: soltar o contexto GL no teardown trava o blob Mali. */
  _exit(0);

fail:
  g_active_runtime = NULL;
  if (compat_bound)
    ts_loader_compat_unbind();
  ts_loader_destroy(loader);
  /* Nao execute destructors guest registrados nem teardown GL do blob Mali. */
  _exit(1);
}
