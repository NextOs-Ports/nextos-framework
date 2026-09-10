/*
 * android_shim.c -- camada Android do loader de Titan Souls (ARM32 softfp).
 *
 * A engine e' NativeActivity de verdade e traz o native_app_glue estatico.
 * O loader monta o `struct android_app` no layout do NDK ARM32 (148 B,
 * confirmado pelo malloc(148) do ANativeActivity_onCreate do proprio
 * binario), levanta o pipe de comandos, e chama android_main(app). Dali em
 * diante quem manda e' a maquina de estados da ENGINE: ela pede
 * ALooper_pollAll, processa o source que devolvemos, le AInputQueue e desenha.
 * Nada aqui pula estado; o que fazemos e' entregar, na ordem nativa do
 * Android, os comandos que a Activity real entregaria
 * (START -> RESUME -> INIT_WINDOW -> GAINED_FOCUS).
 *
 * Input: um unico SDL_PollEvent alimenta nxinput sem consumir a fila por fora;
 * o snapshot Xbox-posicional vira AInputQueue com KEYCODE Android padrao +
 * eixos AMOTION_EVENT_AXIS_*. O build ja e' de gamepad de fabrica
 * (AgSDL_IsGameController), entao o cursor nxinput fica explicitamente OFF e
 * nao ha toque sintetico: o caminho da engine continua sendo o nativo.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "asset_shim.h"
#include "egl_shim.h"
#include "framework_bridge.h"
#include "jni_shim.h"
#include "nxinput.h"
#include "opensles_shim.h"
#include "util.h"

extern int ts_screen_w, ts_screen_h; /* egl_shim: drawable REAL */

/* ================= fila de eventos de input ================= */

#define MAX_INPUT_EVENTS 128
static FakeInputEvent g_input_queue[MAX_INPUT_EVENTS];
static int g_input_head = 0, g_input_tail = 0;
static FakeInputEvent g_current_event;
static int g_has_current = 0;

static struct android_app g_app;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;

#if defined(__arm__)
_Static_assert(sizeof(struct android_app) == 148,
               "android_app tem que casar com o malloc(148) da engine");
#endif

static int input_queue_count(void) {
  return (g_input_head - g_input_tail + MAX_INPUT_EVENTS) % MAX_INPUT_EVENTS;
}

static void input_queue_push(const FakeInputEvent *ev) {
  int next = (g_input_head + 1) % MAX_INPUT_EVENTS;
  if (next == g_input_tail)
    return; /* cheia: descarta o mais novo (nunca bloqueia o frame) */
  g_input_queue[g_input_head] = *ev;
  g_input_head = next;
}

static FakeInputEvent *input_queue_pop(void) {
  if (g_input_tail == g_input_head)
    return NULL;
  FakeInputEvent *ev = &g_input_queue[g_input_tail];
  g_input_tail = (g_input_tail + 1) % MAX_INPUT_EVENTS;
  return ev;
}

static void push_key_event(int action, int keycode) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  /* Android manda dpad com source DPAD|GAMEPAD e botao com GAMEPAD|JOYSTICK.
   * A engine (camada AgSDL) filtra por source, entao vale respeitar. */
  if (keycode >= AKEYCODE_DPAD_UP && keycode <= AKEYCODE_DPAD_CENTER)
    ev.source = AINPUT_SOURCE_DPAD | AINPUT_SOURCE_GAMEPAD;
  else
    ev.source = AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK;
  static int n;
  if (getenv("TS_INPUT_DEBUG") && n++ < 200)
    logPrintf("[in] -> AKEYCODE %d action=%d\n", keycode, action);
  input_queue_push(&ev);
}

/* estado analogico corrente (o Android manda o CONJUNTO de eixos por evento) */
static float g_ax_lx, g_ax_ly, g_ax_rx, g_ax_ry, g_ax_lt, g_ax_rt;
static float g_hat_x, g_hat_y;
/* O fallback gptokeyb compartilha a fila Android, nao o snapshot nxinput. */
static float g_kb_hat_x, g_kb_hat_y;

static float clamp1(float v) {
  if (v > 1.0f) return 1.0f;
  if (v < -1.0f) return -1.0f;
  return v;
}

/* A engine (AgAndroidInputManager::handleInput, lido do binario) le a direcao
 * em AMOTION_EVENT_AXIS_X/Y e so' trata os KEYCODE de d-pad quando o modelo e'
 * "Ouya". Num handheld o d-pad E' o controle principal, entao ele entra pelo
 * MESMO eixo que a engine ja' le — nao e' desvio de fluxo, e' o d-pad falando
 * a lingua que este build entende. O stick continua valendo quando o d-pad
 * esta' solto.
 *
 * Os valores TEM que ficar em [-1,1]: a engine descarta o evento INTEIRO se
 * |eixo| > 1.0, e -32768/32767 da' -1.00003 — o stick no fim do curso mataria
 * o quadro de input em silencio. */
static void push_motion_axes(void) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.pointer_count = 1;
  float hat_x = (g_kb_hat_x != 0.0f) ? g_kb_hat_x : g_hat_x;
  float hat_y = (g_kb_hat_y != 0.0f) ? g_kb_hat_y : g_hat_y;
  float dx = (hat_x != 0.0f) ? hat_x : clamp1(g_ax_lx);
  float dy = (hat_y != 0.0f) ? hat_y : clamp1(g_ax_ly);
  ev.x = dx;
  ev.y = dy;
  ev.axes[AMOTION_EVENT_AXIS_X] = dx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = dy;
  ev.axes[AMOTION_EVENT_AXIS_Z] = clamp1(g_ax_rx);
  ev.axes[AMOTION_EVENT_AXIS_RZ] = clamp1(g_ax_ry);
  /* HAT fica em zero de proposito: com HAT != 0 a engine dispara os botoes
   * digitais 0..3 e o menu — que le a DIRECAO pelo eixo analogico — deixa de
   * andar (medido no device). O d-pad ja' esta' representado em X/Y acima. */
  ev.axes[AMOTION_EVENT_AXIS_HAT_X] = 0.0f;
  ev.axes[AMOTION_EVENT_AXIS_HAT_Y] = 0.0f;
  ev.axes[AMOTION_EVENT_AXIS_LTRIGGER] = g_ax_lt;
  ev.axes[AMOTION_EVENT_AXIS_RTRIGGER] = g_ax_rt;
  ev.axes[AMOTION_EVENT_AXIS_BRAKE] = g_ax_lt;
  ev.axes[AMOTION_EVENT_AXIS_GAS] = g_ax_rt;
  input_queue_push(&ev);
}

/* ================= nxinput -> AInputQueue ================= */

static nxinput_context *g_nxinput;

static int nx_button_to_keycode(nxinput_button button) {
  switch (button) {
  case NXINPUT_BUTTON_A:              return AKEYCODE_BUTTON_A;
  case NXINPUT_BUTTON_B:              return AKEYCODE_BUTTON_B;
  case NXINPUT_BUTTON_X:              return AKEYCODE_BUTTON_X;
  case NXINPUT_BUTTON_Y:              return AKEYCODE_BUTTON_Y;
  /* SELECT nao vira tecla: a engine nao trata 109, e o botao e' metade do
   * combo de saida. START vira MENU(82), que e' o que este build mapeia para
   * o botao de pausa (AgGamepadButton 12). */
  case NXINPUT_BUTTON_BACK:           return -1;
  case NXINPUT_BUTTON_START:          return AKEYCODE_MENU;
  case NXINPUT_BUTTON_LEFT_SHOULDER:  return AKEYCODE_BUTTON_L1;
  case NXINPUT_BUTTON_RIGHT_SHOULDER: return AKEYCODE_BUTTON_R1;
  case NXINPUT_BUTTON_LEFT_STICK:     return AKEYCODE_BUTTON_THUMBL;
  case NXINPUT_BUTTON_RIGHT_STICK:    return AKEYCODE_BUTTON_THUMBR;
  /* D-pad NAO vira KEYCODE: este build so' trata AKEYCODE_DPAD_* quando o
   * modelo e' "Ouya" (lido do binario). O d-pad entra pelo eixo analogico,
   * que e' de onde a engine tira a direcao em qualquer aparelho. */
  case NXINPUT_BUTTON_DPAD_UP:
  case NXINPUT_BUTTON_DPAD_DOWN:
  case NXINPUT_BUTTON_DPAD_LEFT:
  case NXINPUT_BUTTON_DPAD_RIGHT:
  case NXINPUT_BUTTON_GUIDE:
  case NXINPUT_BUTTON_COUNT:
  default:
    return -1;
  }
}

static int init_nxinput(void) {
  nxinput_config config;

  if (g_nxinput)
    return -1;
  nxinput_config_init(&config);
  config.initialize_sdl = 1;
  g_nxinput = nxinput_create(&config);
  if (!g_nxinput) {
    logPrintf("android_shim: nxinput_create falhou: %s\n", SDL_GetError());
    return -1;
  }
  nxinput_set_cursor_context(g_nxinput, NXINPUT_CURSOR_OFF);
  if (ts_framework_publish_input(g_nxinput) != 0) {
    logPrintf("android_shim: receipt nxinput recusado\n");
    nxinput_destroy(g_nxinput);
    g_nxinput = NULL;
    return -1;
  }
  logPrintf("android_shim: nxinput ativo, cursor OFF, pads=%u\n",
            nxinput_connected_count(g_nxinput));
  return 0;
}

static void emit_nxinput_buttons(unsigned int slot) {
  uint32_t pressed;
  uint32_t released;
  unsigned int button;

  pressed = nxinput_consume_pressed(g_nxinput, slot,
                                    NXINPUT_BUTTON_MASK_ALL);
  released = nxinput_consume_released(g_nxinput, slot,
                                      NXINPUT_BUTTON_MASK_ALL);
  for (button = 0u; button < (unsigned int)NXINPUT_BUTTON_COUNT; ++button) {
    uint32_t bit = NXINPUT_BUTTON_BIT(button);
    int keycode = nx_button_to_keycode((nxinput_button)button);
    if (keycode < 0)
      continue;
    if ((pressed & bit) != 0u)
      push_key_event(AKEY_EVENT_ACTION_DOWN, keycode);
    if ((released & bit) != 0u)
      push_key_event(AKEY_EVENT_ACTION_UP, keycode);
  }
}

static float dpad_axis(uint32_t buttons, nxinput_button negative,
                       nxinput_button positive) {
  int neg = (buttons & NXINPUT_BUTTON_BIT(negative)) != 0u;
  int pos = (buttons & NXINPUT_BUTTON_BIT(positive)) != 0u;
  return (float)(pos - neg);
}

static void ts_zoom_update(float lt, float rt);

static void emit_nxinput_motion(void) {
  nxinput_pad_state pad;
  int slot = nxinput_first_connected(g_nxinput);
  float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
  float lt = 0.0f, rt = 0.0f, hat_x = 0.0f, hat_y = 0.0f;

  if (slot >= 0 &&
      nxinput_get_pad(g_nxinput, (unsigned int)slot, &pad) &&
      pad.connected && pad.focused) {
    lx = clamp1(pad.left_x);
    ly = clamp1(pad.left_y);
    rx = clamp1(pad.right_x);
    ry = clamp1(pad.right_y);
    lt = clamp1(pad.left_trigger);
    rt = clamp1(pad.right_trigger);
    hat_x = dpad_axis(pad.buttons, NXINPUT_BUTTON_DPAD_LEFT,
                      NXINPUT_BUTTON_DPAD_RIGHT);
    hat_y = dpad_axis(pad.buttons, NXINPUT_BUTTON_DPAD_UP,
                      NXINPUT_BUTTON_DPAD_DOWN);
  }
  ts_zoom_update(lt, rt);
  if (lx == g_ax_lx && ly == g_ax_ly && rx == g_ax_rx && ry == g_ax_ry &&
      lt == g_ax_lt && rt == g_ax_rt && hat_x == g_hat_x &&
      hat_y == g_hat_y)
    return;
  g_ax_lx = lx;
  g_ax_ly = ly;
  g_ax_rx = rx;
  g_ax_ry = ry;
  g_ax_lt = lt;
  g_ax_rt = rt;
  g_hat_x = hat_x;
  g_hat_y = hat_y;
  push_motion_axes();
}

static void emit_nxinput_state(void) {
  unsigned int slot;

  for (slot = 0u; slot < NXINPUT_MAX_PADS; ++slot)
    emit_nxinput_buttons(slot);
  emit_nxinput_motion();
}

/* ================= saida: SELECT+START ================= */

static volatile sig_atomic_t g_sigterm = 0;
static int g_shutdown = 0;
static Uint32 g_shutdown_at = 0;
#define TS_SHUTDOWN_GRACE_MS 700

static void ts_sigterm_handler(int sig) {
  (void)sig;
  g_sigterm = 1;
  alarm(5);
}
static void ts_sigalrm_handler(int sig) {
  (void)sig;
  _exit(0);
}

void android_shim_install_exit_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ts_sigterm_handler;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ts_sigalrm_handler;
  sigaction(SIGALRM, &sa, NULL);
}

static int g_kb_esc = 0, g_kb_ent = 0;

/* ================= zoom: L2/R2 -> FP::SetZoom ================= */
/* O engine (FlashPunk) exporta FP::GetZoom/SetZoom estaticos. No mobile o
 * zoom era gesto de pinca; aqui L2 afasta e R2 aproxima chamando o METODO do
 * proprio jogo (nunca emulamos dedo quando da pra chamar o motor). Guest e
 * ARMv7 SOFTFP: float viaja em r0 como bits — assinaturas em uint32_t. */
typedef uint32_t (*ts_fp_get_zoom_fn)(void);
typedef void (*ts_fp_set_zoom_fn)(uint32_t float_bits);
static ts_fp_get_zoom_fn g_fp_get_zoom;
static ts_fp_set_zoom_fn g_fp_set_zoom;
static int g_zoom_probe_done;
static float g_zoom_baseline = 1.0f;
static Uint32 g_zoom_last_ms;

static float ts_bits_to_float(uint32_t bits) {
  float value; memcpy(&value, &bits, sizeof value); return value;
}
static uint32_t ts_float_to_bits(float value) {
  uint32_t bits; memcpy(&bits, &value, sizeof bits); return bits;
}

void android_shim_set_zoom_symbols(unsigned long get_address,
                                   unsigned long set_address) {
  g_fp_get_zoom = (ts_fp_get_zoom_fn)get_address;
  g_fp_set_zoom = (ts_fp_set_zoom_fn)set_address;
  if (!get_address || !set_address)
    logPrintf("android_shim: zoom nativo INDISPONIVEL "
              "(FP::GetZoom/SetZoom nao exportados)\n");
}

static void ts_zoom_update(float lt, float rt) {
  Uint32 now;
  float dt, current, factor, minimum, maximum;
  if (!g_fp_get_zoom || !g_fp_set_zoom)
    return;
  now = SDL_GetTicks();
  dt = g_zoom_last_ms ? (float)(now - g_zoom_last_ms) / 1000.0f : 0.0f;
  g_zoom_last_ms = now;
  if (dt <= 0.0f || dt > 0.25f)
    return;
  if (lt < 0.45f && rt < 0.45f)
    return;
  /* So tocamos no engine com o gatilho APERTADO — a essa altura o jogo ja
   * esta renderizando. Ler FP::GetZoom durante o boot deref-ava estado ainda
   * nao inicializado do engine (SIGSEGV provado no device). */
  if (!g_zoom_probe_done) {
    g_zoom_probe_done = 1;
    g_zoom_baseline = ts_bits_to_float(g_fp_get_zoom());
    if (!(g_zoom_baseline > 0.05f && g_zoom_baseline < 20.0f))
      g_zoom_baseline = 1.0f;
    logPrintf("android_shim: zoom nativo ligado (L2 afasta / R2 aproxima); "
              "base=%.3f\n", (double)g_zoom_baseline);
  }
  current = ts_bits_to_float(g_fp_get_zoom());
  if (!(current > 0.01f && current < 50.0f))
    return;
  factor = 1.0f;
  if (lt >= 0.45f)
    factor /= 1.0f + 1.4f * dt; /* afastar */
  if (rt >= 0.45f)
    factor *= 1.0f + 1.4f * dt; /* aproximar */
  minimum = g_zoom_baseline * 0.45f;
  maximum = g_zoom_baseline * 2.2f;
  current *= factor;
  if (current < minimum) current = minimum;
  if (current > maximum) current = maximum;
  g_fp_set_zoom(ts_float_to_bits(current));
}

static void begin_shutdown(const char *why) {
  if (g_shutdown) return;
  g_shutdown = 1;
  g_shutdown_at = SDL_GetTicks();
  logPrintf("android_shim: saida por %s -> pause/save, _exit em %dms\n",
            why, TS_SHUTDOWN_GRACE_MS);
  /* Fluxo NATIVO de saida do Android: perde foco, salva estado, pausa. A
   * engine grava o save dela nesse caminho — nao matamos o processo antes. */
  android_shim_send_cmd(&g_app, APP_CMD_LOST_FOCUS);
  android_shim_send_cmd(&g_app, APP_CMD_SAVE_STATE);
  android_shim_send_cmd(&g_app, APP_CMD_PAUSE);
  alarm(5);
}

static void check_exit_hotkey(void) {
  if (g_shutdown) {
    if (SDL_GetTicks() - g_shutdown_at >= TS_SHUTDOWN_GRACE_MS) {
      logPrintf("android_shim: pause/save concluido -> saindo\n");
      _exit(0);
    }
    return;
  }
  if (g_sigterm)                 begin_shutdown("SIGTERM");
  else if (nxinput_consume_quit_request(g_nxinput))
                                 begin_shutdown("SELECT+START (nxinput)");
  else if (g_kb_esc && g_kb_ent) begin_shutdown("SELECT+START (teclado/gptokeyb)");
}

/* ================= memoria ================= */

/* Handheld de 1 GB sem swap: o OOM killer leva o jogo sem aviso. A resposta e'
 * ECONOMIA LOCAL — APP_CMD_LOW_MEMORY e' o sinal que a onAppCmd da engine ja
 * sabe tratar. NUNCA criamos swap nem mexemos no sistema do usuario. */
static void check_low_memory(void) {
  static int armed = 1, tick = 0;
  if ((tick++ % 240) != 0) return;
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return;
  char line[128];
  long avail_kb = -1;
  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1) break;
  fclose(f);
  if (avail_kb < 0) return;
  if (armed && avail_kb < 60L * 1024) {
    armed = 0;
    logPrintf("android_shim: MemAvailable=%ld kB -> APP_CMD_LOW_MEMORY\n", avail_kb);
    android_shim_send_cmd(&g_app, APP_CMD_LOW_MEMORY);
  } else if (!armed && avail_kb > 110L * 1024) {
    armed = 1;
  }
}

/* ================= bomba de eventos do host ================= */

static int gptk_on(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("TS_INPUT");
    v = (e && strcmp(e, "gptk") == 0) ? 1 : 0;
  }
  return v;
}

static void process_sdl_events(void) {
  static int in_dbg = -1;
  if (in_dbg < 0) in_dbg = getenv("TS_INPUT_DEBUG") ? 1 : 0;

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    /* Ownership unico da fila SDL: nxinput somente observa o evento que este
     * adapter ja retirou. Nenhuma outra camada chama SDL_PollEvent. */
    nxinput_observe_event(g_nxinput, &e);
    if (in_dbg) {
      switch (e.type) {
      case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
        logPrintf("[in] JOYBUTTON %d %s\n", e.jbutton.button,
                  e.type == SDL_JOYBUTTONDOWN ? "down" : "up"); break;
      case SDL_JOYHATMOTION:
        logPrintf("[in] JOYHAT %d = %d\n", e.jhat.hat, e.jhat.value); break;
      case SDL_JOYAXISMOTION:
        logPrintf("[in] JOYAXIS %d = %d\n", e.jaxis.axis, e.jaxis.value); break;
      case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
        logPrintf("[in] CBUTTON %d %s\n", e.cbutton.button,
                  e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up"); break;
      case SDL_CONTROLLERAXISMOTION:
        logPrintf("[in] CAXIS %d = %d\n", e.caxis.axis, e.caxis.value); break;
      default: break;
      }
    }
    switch (e.type) {
    case SDL_QUIT:
      g_app.destroyRequested = 1;
      begin_shutdown("SDL_QUIT");
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      /* GARANTIA DE SAIDA: rastreia esc+enter sempre, mesmo fora do gptk. */
      if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        g_kb_esc = (e.type == SDL_KEYDOWN);
      if (e.key.keysym.scancode == SDL_SCANCODE_RETURN)
        g_kb_ent = (e.type == SDL_KEYDOWN);
      if (!gptk_on() || e.key.repeat) break;
      int dn = (e.type == SDL_KEYDOWN);
      int act = dn ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
      int kc = -1, hat = 0;
      switch (e.key.keysym.scancode) {
      case SDL_SCANCODE_X:      kc = AKEYCODE_BUTTON_A; break;
      case SDL_SCANCODE_C:      kc = AKEYCODE_BUTTON_B; break;
      case SDL_SCANCODE_Q:      kc = AKEYCODE_BUTTON_X; break;
      case SDL_SCANCODE_T:      kc = AKEYCODE_BUTTON_Y; break;
      case SDL_SCANCODE_RETURN: kc = AKEYCODE_MENU; break;
      case SDL_SCANCODE_ESCAPE: break;
      case SDL_SCANCODE_H:      kc = AKEYCODE_BUTTON_L1; break;
      case SDL_SCANCODE_J:      kc = AKEYCODE_BUTTON_R1; break;
      case SDL_SCANCODE_K:      kc = AKEYCODE_BUTTON_L2; break;
      case SDL_SCANCODE_L:      kc = AKEYCODE_BUTTON_R2; break;
      case SDL_SCANCODE_UP:     kc = AKEYCODE_DPAD_UP;    g_kb_hat_y = dn ? -1.f : 0.f; hat = 1; break;
      case SDL_SCANCODE_DOWN:   kc = AKEYCODE_DPAD_DOWN;  g_kb_hat_y = dn ?  1.f : 0.f; hat = 1; break;
      case SDL_SCANCODE_LEFT:   kc = AKEYCODE_DPAD_LEFT;  g_kb_hat_x = dn ? -1.f : 0.f; hat = 1; break;
      case SDL_SCANCODE_RIGHT:  kc = AKEYCODE_DPAD_RIGHT; g_kb_hat_x = dn ?  1.f : 0.f; hat = 1; break;
      default: break;
      }
      if (kc >= 0) push_key_event(act, kc);
      if (hat) push_motion_axes();
      break;
    }

    default:
      break;
    }
  }
  nxinput_poll(g_nxinput);
  emit_nxinput_state();
  check_exit_hotkey();
  check_low_memory();
}

void android_shim_pump(void) { process_sdl_events(); }

/* ================= ALooper ================= */

/* Looper de verdade, por thread: a engine registra o fd do pipe de comandos
 * (LOOPER_ID_MAIN) e o do input (LOOPER_ID_INPUT) e espera receber de volta o
 * `data` que ela registrou — que e' o android_poll_source dela. */
#define LOOPER_MAX_FDS 8
typedef struct {
  int n;
  struct { int fd, ident, events; void *data; } e[LOOPER_MAX_FDS];
} LooperImpl;

static __thread LooperImpl *tls_looper;

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  if (!tls_looper)
    tls_looper = (LooperImpl *)calloc(1, sizeof(LooperImpl));
  return (ALooper *)tls_looper;
}

void ALooper_addFd(void *looper, int fd, int ident, int events, void *callback,
                   void *data) {
  (void)callback;
  LooperImpl *l = looper ? (LooperImpl *)looper : tls_looper;
  if (!l) return;
  for (int i = 0; i < l->n; i++)
    if (l->e[i].fd == fd) { l->e[i].ident = ident; l->e[i].data = data; return; }
  if (l->n >= LOOPER_MAX_FDS) return;
  l->e[l->n].fd = fd;
  l->e[l->n].ident = ident;
  l->e[l->n].events = events;
  l->e[l->n].data = data;
  l->n++;
  debugPrintf("ALooper_addFd: fd=%d ident=%d data=%p\n", fd, ident, data);
}

int ALooper_removeFd(void *looper, int fd) {
  LooperImpl *l = looper ? (LooperImpl *)looper : tls_looper;
  if (!l) return 0;
  for (int i = 0; i < l->n; i++)
    if (l->e[i].fd == fd) {
      l->e[i] = l->e[l->n - 1];
      l->n--;
      return 1;
    }
  return 0;
}

/* fd auto-pipe do input: escrito quando entra evento, lido no pollAll */
static int g_input_pipe[2] = {-1, -1};

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
  LooperImpl *l = tls_looper;
  static int heartbeat_enabled = -1;

  /* Bombeia o host ANTES de qualquer bloqueio: e' aqui que o pad vira evento
   * Android e que o combo de saida e' checado. */
  process_sdl_events();

  /* Diagnostico opt-in: TS_HEARTBEAT=1 relata o looper a cada ~2s. */
  if (heartbeat_enabled < 0)
    heartbeat_enabled = ts_env_enabled("TS_HEARTBEAT");
  if (heartbeat_enabled) {
    static unsigned n;
    static Uint32 last;
    n++;
    Uint32 now = SDL_GetTicks();
    if (now - last >= 2000) {
      logPrintf("[hb] pollAll=%u (%.1f/s) window=%p state=%d fila=%d\n", n,
                (double)n * 1000.0 / (double)(now - last ? now - last : 1),
                (void *)g_app.window, g_app.activityState, input_queue_count());
      last = now;
      n = 0;
    }
  }

  if (input_queue_count() > 0 && g_input_pipe[1] >= 0) {
    char c = 1;
    ssize_t w = write(g_input_pipe[1], &c, 1); /* arma o fd do input */
    (void)w;
  }

  if (!l || l->n == 0) {
    if (timeoutMillis > 0) SDL_Delay(timeoutMillis > 4 ? 4 : timeoutMillis);
    return -1;
  }

  struct pollfd pfd[LOOPER_MAX_FDS];
  for (int i = 0; i < l->n; i++) {
    pfd[i].fd = l->e[i].fd;
    pfd[i].events = POLLIN;
    pfd[i].revents = 0;
  }
  /* Nunca bloqueamos de verdade: o laco tem que voltar para bombear o pad, o
   * audio e o hotkey. -1 da engine vira "espera curta". */
  int t = timeoutMillis;
  if (t < 0 || t > 4) t = 4;
  int ret = poll(pfd, l->n, t);
  if (ret > 0) {
    for (int i = 0; i < l->n; i++) {
      if (!(pfd[i].revents & POLLIN)) continue;
      if (outFd) *outFd = l->e[i].fd;
      if (outEvents) *outEvents = 1;
      if (outData) *outData = l->e[i].data;
      { static int n;
        if (getenv("TS_INPUT_DEBUG") && l->e[i].ident == LOOPER_ID_INPUT && n++ < 50)
          logPrintf("[in] pollAll -> LOOPER_ID_INPUT data=%p\n", l->e[i].data); }
      return l->e[i].ident;
    }
  }
  return -1; /* ALOOPER_POLL_TIMEOUT */
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents,
                     void **outData) {
  return ALooper_pollAll(timeoutMillis, outFd, outEvents, outData);
}

ALooper *ALooper_forThread(void) { return (ALooper *)tls_looper; }
void ALooper_acquire(void *l) { (void)l; }
void ALooper_release(void *l) { (void)l; }
int ALooper_wake(void *l) { (void)l; return 0; }

/* ================= AInputQueue ================= */

static int g_fake_input_queue = 1;

void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data) {
  (void)queue;
  if (g_input_pipe[0] < 0) return;
  ALooper_addFd(looper, g_input_pipe[0], ident, 1, callback, data);
  debugPrintf("AInputQueue_attachLooper: ident=%d data=%p\n", ident, data);
}

void AInputQueue_detachLooper(void *queue) {
  (void)queue;
  if (g_input_pipe[0] >= 0) ALooper_removeFd(NULL, g_input_pipe[0]);
}

int AInputQueue_hasEvents(void *queue) {
  (void)queue;
  return input_queue_count() > 0;
}

int AInputQueue_getEvent(void *queue, AInputEvent **outEvent) {
  (void)queue;
  /* drena o byte do auto-pipe (o fd so' sinaliza "tem evento") */
  if (g_input_pipe[0] >= 0) {
    char buf[64];
    while (read(g_input_pipe[0], buf, sizeof(buf)) > 0) {}
  }
  { static int n;
    if (getenv("TS_INPUT_DEBUG") && n++ < 50)
      logPrintf("[in] AInputQueue_getEvent (fila=%d)\n", input_queue_count()); }
  FakeInputEvent *ev = input_queue_pop();
  if (!ev) {
    if (outEvent) *outEvent = NULL;
    return -1;
  }
  g_current_event = *ev;
  g_has_current = 1;
  if (outEvent) *outEvent = (AInputEvent *)&g_current_event;
  return 0;
}

int AInputQueue_preDispatchEvent(void *queue, void *event) {
  (void)queue; (void)event;
  return 0;
}

void AInputQueue_finishEvent(void *queue, void *event, int handled) {
  (void)queue; (void)event; (void)handled;
  g_has_current = 0;
}

/* ================= AInputEvent getters ================= */

static FakeInputEvent *ev_of(void *e) { return (FakeInputEvent *)e; }

int AInputEvent_getType(void *e) { return e ? ev_of(e)->type : 0; }
int AInputEvent_getSource(void *e) { return e ? ev_of(e)->source : 0; }
int AInputEvent_getDeviceId(void *e) { (void)e; return 1; }
int AKeyEvent_getAction(void *e) { return e ? ev_of(e)->action : 0; }
int AKeyEvent_getKeyCode(void *e) {
  { static int n;
    if (getenv("TS_INPUT_DEBUG") && n++ < 300)
      logPrintf("[in] engine le keycode %d (action=%d src=0x%x)\n",
                e ? ev_of(e)->keycode : -1, e ? ev_of(e)->action : -1,
                e ? ev_of(e)->source : 0); }
  return e ? ev_of(e)->keycode : 0;
}
int AKeyEvent_getFlags(void *e) { (void)e; return 0; }
int AKeyEvent_getMetaState(void *e) { (void)e; return 0; }
int AKeyEvent_getRepeatCount(void *e) { (void)e; return 0; }
int AMotionEvent_getAction(void *e) { return e ? ev_of(e)->action : 0; }
int AMotionEvent_getPointerCount(void *e) {
  return e ? (ev_of(e)->pointer_count ? ev_of(e)->pointer_count : 1) : 0;
}
int AMotionEvent_getPointerId(void *e, int i) { (void)i; return e ? ev_of(e)->pointer_id : 0; }
int AMotionEvent_getButtonState(void *e) { (void)e; return 0; }

float TS_GUEST_PCS AMotionEvent_getX(void *e, int i) {
  (void)i;
  return e ? ev_of(e)->x : 0.0f;
}
float TS_GUEST_PCS AMotionEvent_getY(void *e, int i) {
  (void)i;
  return e ? ev_of(e)->y : 0.0f;
}
float TS_GUEST_PCS AMotionEvent_getAxisValue(void *e, int axis, int ptr) {
  (void)ptr;
  if (!e || axis < 0 || axis >= AMOTION_EVENT_AXIS_MAX) return 0.0f;
  { static int n;
    if (getenv("TS_INPUT_DEBUG") && n++ < 120)
      logPrintf("[in] engine le axis %d = %.3f\n", axis,
                (double)ev_of(e)->axes[axis]); }
  return ev_of(e)->axes[axis];
}

/* ================= AConfiguration ================= */

static int g_config_dummy = 1;

AConfiguration *AConfiguration_new(void) {
  return (AConfiguration *)&g_config_dummy;
}
void AConfiguration_delete(void *c) { (void)c; }
void AConfiguration_fromAssetManager(void *c, void *am) { (void)c; (void)am; }
void AConfiguration_setLocale(void *c, const char *l) { (void)c; (void)l; }

/* A escolha manual pertence ao menu nativo do jogo. A camada Android conserva
 * o fallback historico em ingles; o adapter do menu trata os cinco idiomas. */
int AConfiguration_getLanguage(void *c, char *out) {
  (void)c;
  if (out) { out[0] = 'e'; out[1] = 'n'; }
  return 2;
}
int AConfiguration_getCountry(void *c, char *out) {
  (void)c;
  if (out) { out[0] = 'U'; out[1] = 'S'; }
  return 2;
}
int AConfiguration_getDensity(void *c) { (void)c; return 160; }
int AConfiguration_getOrientation(void *c) { (void)c; return 2; /* LAND */ }
int AConfiguration_getScreenSize(void *c) { (void)c; return 3; /* LARGE */ }

/* ================= ANativeWindow ================= */

struct ANativeWindow { int w, h, format; };
static struct ANativeWindow g_window;

int ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt) {
  (void)w;
  /* REGRA #25: nunca cravar resolucao. O drawable REAL manda; o pedido da
   * engine (config.txt diz 1440x900, que e' default de arquivo) e' so' um
   * pedido. Registramos o que ela pediu e devolvemos o que existe. */
  logPrintf("ANativeWindow_setBuffersGeometry(%d,%d,fmt=%d) -> drawable real %dx%d\n",
            width, height, fmt, ts_screen_w, ts_screen_h);
  g_window.w = ts_screen_w;
  g_window.h = ts_screen_h;
  if (fmt) g_window.format = fmt;
  return 0;
}
int ANativeWindow_getWidth(void *w) { (void)w; return ts_screen_w; }
int ANativeWindow_getHeight(void *w) { (void)w; return ts_screen_h; }
int ANativeWindow_getFormat(void *w) { (void)w; return g_window.format; }
void ANativeWindow_acquire(void *w) { (void)w; }
void ANativeWindow_release(void *w) { (void)w; }

ANativeWindow *android_shim_get_window(void) { return &g_window; }

/* ================= ANativeActivity ================= */

void ANativeActivity_finish(void *activity) {
  (void)activity;
  logPrintf("ANativeActivity_finish -> destroyRequested\n");
  g_app.destroyRequested = 1;
  begin_shutdown("ANativeActivity_finish");
}
void ANativeActivity_setWindowFlags(void *a, unsigned add, unsigned rem) {
  (void)a; (void)add; (void)rem;
}

/* ================= pipe de comandos + poll sources ================= */

/* Replica exatamente o que o android_native_app_glue faz do lado da Activity:
 * o comando vai pelo pipe, e quem o consome (na thread do jogo) aplica o
 * estado pendente e chama app->onAppCmd. A ENGINE continua sendo a dona da
 * transicao — nos so' entregamos o comando na ordem nativa. */
void android_shim_send_cmd(struct android_app *app, int8_t cmd) {
  if (!app || app->msgwrite < 0) return;
  if (write(app->msgwrite, &cmd, sizeof(cmd)) != sizeof(cmd))
    logPrintf("android_shim: falha ao escrever cmd %d: %s\n", cmd, strerror(errno));
}

static const char *cmd_name(int8_t c) {
  switch (c) {
  case APP_CMD_INPUT_CHANGED: return "INPUT_CHANGED";
  case APP_CMD_INIT_WINDOW: return "INIT_WINDOW";
  case APP_CMD_TERM_WINDOW: return "TERM_WINDOW";
  case APP_CMD_WINDOW_RESIZED: return "WINDOW_RESIZED";
  case APP_CMD_WINDOW_REDRAW_NEEDED: return "REDRAW_NEEDED";
  case APP_CMD_CONTENT_RECT_CHANGED: return "CONTENT_RECT";
  case APP_CMD_GAINED_FOCUS: return "GAINED_FOCUS";
  case APP_CMD_LOST_FOCUS: return "LOST_FOCUS";
  case APP_CMD_CONFIG_CHANGED: return "CONFIG_CHANGED";
  case APP_CMD_LOW_MEMORY: return "LOW_MEMORY";
  case APP_CMD_START: return "START";
  case APP_CMD_RESUME: return "RESUME";
  case APP_CMD_SAVE_STATE: return "SAVE_STATE";
  case APP_CMD_PAUSE: return "PAUSE";
  case APP_CMD_STOP: return "STOP";
  case APP_CMD_DESTROY: return "DESTROY";
  default: return "?";
  }
}

static void process_cmd(struct android_app *app,
                        struct android_poll_source *src) {
  (void)src;
  int8_t cmd = 0;
  if (read(app->msgread, &cmd, sizeof(cmd)) != sizeof(cmd))
    return;
  logPrintf("android_shim: cmd %s\n", cmd_name(cmd));

  /* pre_exec: o glue aplica o estado ANTES de avisar o jogo */
  switch (cmd) {
  case APP_CMD_INPUT_CHANGED:
    app->inputQueue = app->pendingInputQueue;
    break;
  case APP_CMD_INIT_WINDOW:
    app->window = app->pendingWindow;
    break;
  case APP_CMD_TERM_WINDOW:
    break;
  case APP_CMD_RESUME:
  case APP_CMD_START:
  case APP_CMD_PAUSE:
  case APP_CMD_STOP:
    app->activityState = cmd;
    break;
  case APP_CMD_SAVE_STATE:
    app->stateSaved = 0;
    break;
  default:
    break;
  }

  if (app->onAppCmd)
    app->onAppCmd(app, cmd);

  /* post_exec */
  switch (cmd) {
  case APP_CMD_TERM_WINDOW:
    app->window = NULL;
    break;
  case APP_CMD_SAVE_STATE:
    app->stateSaved = 1;
    break;
  case APP_CMD_RESUME:
    if (app->savedState) { free(app->savedState); app->savedState = NULL; }
    app->savedStateSize = 0;
    break;
  default:
    break;
  }
}

static void process_input(struct android_app *app,
                          struct android_poll_source *src) {
  (void)src;
  { static int n;
    if (getenv("TS_INPUT_DEBUG") && n++ < 50)
      logPrintf("[in] process_input (queue=%p onInputEvent=%p)\n",
                (void *)app->inputQueue, (void *)app->onInputEvent); }
  AInputEvent *ev = NULL;
  while (AInputQueue_getEvent(app->inputQueue, &ev) >= 0) {
    if (AInputQueue_preDispatchEvent(app->inputQueue, ev)) continue;
    int handled = 0;
    if (app->onInputEvent) handled = app->onInputEvent(app, ev);
    { static int n;
      if (getenv("TS_INPUT_DEBUG") && n++ < 200)
        logPrintf("[in] engine onInputEvent(type=%d kc=%d) -> %d\n",
                  AInputEvent_getType(ev), AKeyEvent_getKeyCode(ev), handled); }
    AInputQueue_finishEvent(app->inputQueue, ev, handled);
  }
}

/* ================= init ================= */

struct android_app *android_shim_init(void) {
  memset(&g_app, 0, sizeof(g_app));
  memset(&g_activity, 0, sizeof(g_activity));
  memset(&g_callbacks, 0, sizeof(g_callbacks));

  if (init_nxinput() != 0)
    return NULL;

  int cmdpipe[2];
  if (pipe(cmdpipe) != 0) {
    logPrintf("android_shim: pipe() falhou: %s\n", strerror(errno));
    return NULL;
  }
  fcntl(cmdpipe[0], F_SETFL, O_NONBLOCK);
  if (pipe(g_input_pipe) != 0) {
    logPrintf("android_shim: pipe(input) falhou: %s\n", strerror(errno));
    return NULL;
  }
  fcntl(g_input_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(g_input_pipe[1], F_SETFL, O_NONBLOCK);

  void *vm = NULL, *env = NULL;
  jni_shim_init(&vm, &env);

  g_activity.callbacks = &g_callbacks;
  g_activity.vm = vm;
  g_activity.env = env;
  g_activity.clazz = (void *)0xC1A55;
  g_activity.internalDataPath = ts_paths_internal();
  g_activity.externalDataPath = ts_paths_external();
  g_activity.obbPath = ts_paths_obb();
  g_activity.sdkVersion = 19; /* Android 4.4: a era deste build (2015) */
  g_activity.assetManager = asset_shim_manager();

  g_app.activity = &g_activity;
  g_app.config = AConfiguration_new();
  g_app.msgread = cmdpipe[0];
  g_app.msgwrite = cmdpipe[1];
  g_app.running = 1;
  g_app.destroyRequested = 0;
  g_app.window = NULL;
  g_app.inputQueue = NULL;
  g_app.pendingWindow = &g_window;
  g_app.pendingInputQueue = (AInputQueue *)&g_fake_input_queue;
  g_app.contentRect[0] = 0;
  g_app.contentRect[1] = 0;
  g_app.contentRect[2] = ts_screen_w;
  g_app.contentRect[3] = ts_screen_h;

  g_window.w = ts_screen_w;
  g_window.h = ts_screen_h;
  g_window.format = 1; /* WINDOW_FORMAT_RGBA_8888 */

  g_app.cmdPollSource.id = LOOPER_ID_MAIN;
  g_app.cmdPollSource.app = &g_app;
  g_app.cmdPollSource.process = process_cmd;
  g_app.inputPollSource.id = LOOPER_ID_INPUT;
  g_app.inputPollSource.app = &g_app;
  g_app.inputPollSource.process = process_input;

  /* O looper e' preparado na thread que vai rodar o android_main (a mesma que
   * chama isto), como no glue de verdade. */
  g_app.looper = ALooper_prepare(0);
  ALooper_addFd(g_app.looper, g_app.msgread, LOOPER_ID_MAIN, 1, NULL,
                &g_app.cmdPollSource);
  ALooper_addFd(g_app.looper, g_input_pipe[0], LOOPER_ID_INPUT, 1, NULL,
                &g_app.inputPollSource);

  logPrintf("android_shim: android_app pronto (sizeof=%d) internal=%s obb=%s\n",
            (int)sizeof(g_app), g_activity.internalDataPath, g_activity.obbPath);
  return &g_app;
}
