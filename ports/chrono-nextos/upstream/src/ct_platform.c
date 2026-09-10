/*
 * ct_platform.c — integracao de firmware do Chrono Trigger (multi-device).
 *
 * Tudo aqui e' DETECCAO DE CAPACIDADE, nunca perfil por nome de aparelho:
 *   - instancia unica travada no PROPRIO BINARIO (flock);
 *   - saida unica: SIGTERM, SELECT+START do SDL e SELECT+START cru do evdev
 *     (TRIGGER_HAPPY1/2) convergem no MESMO pedido de shutdown;
 *   - log de uma linha do backend de video/audio, do EGLConfig realmente
 *     entregue e do drawable real;
 *   - relatorio de pico de memoria (VmHWM) para aparelhos de RAM curta.
 *
 * Fonte estrutural do combo evdev: ports/huntdown/src/huntdown_input.c
 * (port aprovado). Fonte do lock: contrato de launcher-portmaster.md §4.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <SDL2/SDL.h>

#include "ct_platform.h"
#include "util.h"

/* Chord de saida canonico do framework: SDL BACK+START por estado (+ botao cru
 * do joystick nos indices do mapping); evdev cru so' como fallback sem pad SDL.
 * Nunca vigia BTN_SELECT/START literais com pad aberto (na familia H700 esses
 * codigos sao L2/R2). Uma unica unidade de traducao define a implementacao. */
#define NXINPUT_EVDEV_CHORD_LOG(...) do { debugPrintf(__VA_ARGS__); } while (0)
#define NXINPUT_EVDEV_CHORD_IMPLEMENTATION
#include "nxinput_evdev_chord.h"

/* ------------------------------------------------------ diretorio do port --- */

/* Nunca cravar a raiz de ROM. A ordem e': override explicito, HOME montado
 * pelo launcher e, por ultimo, o diretorio REAL do proprio executavel — que
 * funciona ate' quando o port e' aberto sem launcher nenhum. */
const char *ct_gamedir(void) {
  static char dir[1024];
  static int resolved;
  if (resolved) return dir;
  resolved = 1;

  const char *env = getenv("NXCOMPAT_GAME_DIR");
  if (!env || !*env) env = getenv("CHRONO_GAMEDIR");
  if (!env || !*env) env = getenv("HOME");
  if (env && *env && env[0] == '/') {
    snprintf(dir, sizeof dir, "%s", env);
  } else {
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n > 0) {
      dir[n] = 0;
      char *slash = strrchr(dir, '/');
      if (slash && slash != dir) *slash = 0; else snprintf(dir, sizeof dir, ".");
    } else {
      snprintf(dir, sizeof dir, ".");
    }
  }
  size_t len = strlen(dir);
  while (len > 1 && dir[len - 1] == '/') dir[--len] = 0;
  return dir;
}

/* ---------------------------------------------------------------- saida --- */

static volatile sig_atomic_t g_exit_requested;
static const char *g_exit_reason = "";

void ct_request_exit(const char *reason) {
  if (!g_exit_requested) {
    g_exit_requested = 1;
    g_exit_reason = reason ? reason : "desconhecido";
  }
}

int ct_exit_requested(void) { return g_exit_requested; }

const char *ct_exit_reason(void) { return g_exit_reason; }

static void ct_signal_handler(int sig) {
  /* async-signal-safe: so' marca a flag; o teardown roda no loop principal,
     no MESMO caminho do SELECT+START. */
  g_exit_requested = 1;
  g_exit_reason = (sig == SIGTERM) ? "SIGTERM" : "SIGINT";
}

void ct_install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = ct_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------- prazo do desligamento --- */

/* O teardown de engine Android/Cocos nao tem garantia de terminar: threads de
 * som, de recurso e do driver podem ficar penduradas. A parte SEGURA (pausa e
 * save pela propria engine) roda primeiro; se ela nao voltar no prazo, o
 * processo morre de vez para nao ficar dono do display e do audio.
 * `_exit` aqui e' terminal DEPOIS da parte segura, nunca atalho de boot. */
static void *ct_watchdog_thread(void *arg) {
  long seconds = (long)(intptr_t)arg;
  struct timespec ts = { .tv_sec = seconds, .tv_nsec = 0 };
  nanosleep(&ts, NULL);
  debugPrintf("SHUTDOWN: prazo de %ld s estourado; encerrando o processo\n",
              seconds);
  _exit(0);
  return NULL;
}

/* Depois que a parte segura terminou, um SIGSEGV/SIGBUS no teardown das
 * threads da engine nao pode virar "o jogo quebrou": o save ja esta em disco e
 * o frontend so' precisa do processo fora do caminho. Convertemos em saida
 * limpa, registrando o motivo. */
static void ct_shutdown_fault(int sig) {
  const char *msg = (sig == SIGSEGV) ? "SHUTDOWN: falha no teardown (SIGSEGV) apos o save; saindo limpo\n"
                                     : "SHUTDOWN: falha no teardown apos o save; saindo limpo\n";
  ssize_t ignored = write(2, msg, strlen(msg));
  (void)ignored;
  _exit(0);
}

void ct_start_shutdown_watchdog(int seconds) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = ct_shutdown_fault;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);

  const char *env = getenv("CHRONO_SHUTDOWN_DEADLINE");
  if (env) seconds = atoi(env);
  if (seconds <= 0) return;
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, ct_watchdog_thread,
                     (void *)(intptr_t)seconds) == 0)
    debugPrintf("SHUTDOWN: prazo de %d s armado\n", seconds);
  pthread_attr_destroy(&attr);
}

/* ------------------------------------------------------- instancia unica --- */

static int g_lock_fd = -1;

int ct_single_instance_lock(const char *self_path) {
  /* Regra do projeto: a trava mora no PROPRIO BINARIO, nunca so' no script —
     script morto libera a trava enquanto o jogo continua dono do display. */
  g_lock_fd = open(self_path, O_RDONLY | O_CLOEXEC);
  if (g_lock_fd < 0) {
    debugPrintf("LOCK: nao consegui abrir %s (%s); seguindo sem trava\n",
                self_path, strerror(errno));
    return 0;
  }
  if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
    debugPrintf("LOCK: outra instancia do Chrono Trigger ja esta rodando (%s)\n",
                strerror(errno));
    close(g_lock_fd);
    g_lock_fd = -1;
    return -1;
  }
  debugPrintf("LOCK: instancia unica adquirida em %s\n", self_path);
  return 0;
}

void ct_single_instance_unlock(void) {
  if (g_lock_fd >= 0) {
    flock(g_lock_fd, LOCK_UN);
    close(g_lock_fd);
    g_lock_fd = -1;
  }
}

/* ------------------------------------------------- combo de saida (evdev) --- */

/* O pad SDL corrente (ligado pelo main a cada frame). NULL = sem pad -> fallback
 * evdev cru ativo; nao-NULL -> SDL manda e o evdev fica mudo. */
static SDL_GameController *g_chord_pad;

void ct_exit_chord_set_controller(struct _SDL_GameController *pad) {
  g_chord_pad = pad;
}

void ct_exit_chord_open(void) {
  nx_evdev_chord_open();
}

void ct_exit_chord_poll(void) {
  SDL_GameController *pads[1];
  pads[0] = g_chord_pad;
  if (nx_exit_chord_update(pads, 1))
    ct_request_exit("SELECT+START");
}

void ct_exit_chord_close(void) {
  nx_evdev_chord_close();
  g_chord_pad = NULL;
}

/* ----------------------------------------------------------------- audio --- */

/* O adapter preserva integralmente a escolha herdada/default do firmware.
 * `dummy`/`disk` nao contam como saida real e uma falha nunca derruba o video.
 * A publicacao forte acontece somente quando o OpenSL shim abre de verdade o
 * device e entrega o formato obtido ao ct_framework. Nenhum hint/backend global
 * e removido ou forcado aqui. */
static int ct_audio_driver_is_real(void) {
  const char *driver = SDL_GetCurrentAudioDriver();
  if (!driver) return 0;
  return strcmp(driver, "dummy") != 0 && strcmp(driver, "disk") != 0;
}

void ct_init_audio_subsystem(void) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 && ct_audio_driver_is_real()) {
    debugPrintf("AUDIO: subsistema aberto no backend %s\n",
                SDL_GetCurrentAudioDriver());
    return;
  }

  const char *driver = SDL_GetCurrentAudioDriver();
  debugPrintf("AUDIO: backend herdado/default indisponivel ou nao-real "
              "(backend=%s erro=%s); o jogo segue COM VIDEO e SEM SOM\n",
              driver ? driver : "nenhum", SDL_GetError());
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/* ------------------------------------------------------------ diagnostico --- */

extern const unsigned char *glGetString(unsigned name);

void ct_log_video_profile(SDL_Window *window, int drawable_w, int drawable_h) {
  int red = 0, green = 0, blue = 0, alpha = 0, depth = 0, stencil = 0;
  int major = 0, minor = 0, profile = 0, doublebuffer = 0;
  SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &red);
  SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &green);
  SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &blue);
  SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alpha);
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
  SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
  SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &doublebuffer);

  const char *video = SDL_GetCurrentVideoDriver();
  const char *audio = SDL_GetCurrentAudioDriver();
  int window_w = 0, window_h = 0;
  SDL_GetWindowSize(window, &window_w, &window_h);

  /* EGLConfig EXATO entregue: o Mesa oferece RGBX8888 antes do RGBA8888 e a
     engine do Cocos conta com o canal alpha. Registrar (nao adivinhar). */
  debugPrintf("VIDEO backend=%s audio=%s window=%dx%d drawable=%dx%d\n",
              video ? video : "?", audio ? audio : "?",
              window_w, window_h, drawable_w, drawable_h);
  debugPrintf("VIDEO eglconfig R%d G%d B%d A%d D%d S%d gles=%d.%d profile=0x%x "
              "doublebuffer=%d vsync=%d\n",
              red, green, blue, alpha, depth, stencil, major, minor,
              (unsigned)profile, doublebuffer, SDL_GL_GetSwapInterval());
  if (alpha < 8)
    debugPrintf("VIDEO AVISO: config sem alpha de 8 bits (A=%d) — o driver "
                "provavelmente entregou RGBX8888\n", alpha);
  debugPrintf("VIDEO vendor=%s renderer=%s gl=%s glsl=%s\n",
              (const char *)glGetString(0x1F00), (const char *)glGetString(0x1F01),
              (const char *)glGetString(0x1F02), (const char *)glGetString(0x8B8C));
}

/* No KMSDRM o page-flip so' e' agendado quando o pipeline drena; sem o
   glFinish antes do swap o SDL perde meio vsync e o jogo trava em 30fps.
   No fbdev/Mali-450 essa chamada e' custo puro — por isso e' condicionada ao
   backend REALMENTE aberto, nunca ao nome do aparelho. */
int ct_needs_finish_before_swap(void) {
  const char *forced = getenv("CHRONO_GLFINISH");
  if (forced) return atoi(forced) != 0;
  const char *video = SDL_GetCurrentVideoDriver();
  return video && strcmp(video, "KMSDRM") == 0;
}

/* --------------------------------------------------------------- memoria --- */

long ct_peak_rss_kb(void) {
  FILE *f = fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long value = -1;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "VmHWM:", 6) == 0) { value = atol(line + 6); break; }
  }
  fclose(f);
  return value;
}

long ct_mem_total_kb(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return -1;
  char line[256];
  long value = -1;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "MemTotal:", 9) == 0) { value = atol(line + 9); break; }
  }
  fclose(f);
  return value;
}
