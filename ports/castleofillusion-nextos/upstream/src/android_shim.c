/*
 * android_shim.c -- fake Android NDK for Linux ARM64
 *
 * Implements enough of the android_native_app_glue + Android NDK
 * to let the game library's android_main() run on Linux.
 *
 * Input handling:
 *   SDL gamepad events are converted to fake AInputEvent structs
 *   (key events for buttons, motion events for analog stick cursor).
 *   The game's onInputEvent callback receives them through the
 *   standard AInputQueue_getEvent flow.
 */

#include <SDL2/SDL.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "error.h"
#include "so_util.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "util.h"
#include "pad_positional_fix.h"

/* ---- Screen resolution (Trimui Smart Pro) ---- */
extern int coi_screen_w, coi_screen_h; /* resolucao real (egl_shim) */
#define SCREEN_WIDTH coi_screen_w
#define SCREEN_HEIGHT coi_screen_h

/* ---- Input event queue ---- */
#define MAX_INPUT_EVENTS 64

static FakeInputEvent g_input_queue[MAX_INPUT_EVENTS];
static int g_input_head = 0; // next write position
static int g_input_tail = 0; // next read position
static FakeInputEvent *g_current_event = NULL; // event being processed

// Virtual cursor for analog stick → touch mapping
static float g_cursor_x = 640.0f; /* recentrado dinamicamente no 1o uso */
static float g_cursor_y = 360.0f;
static int g_cursor_down = 0; // whether virtual "finger" is down

// Last sent joystick axis values (to avoid flooding)
static float g_last_lx = 0, g_last_ly = 0, g_last_rx = 0, g_last_ry = 0;

// SDL gamepad
static SDL_GameController *g_gamecontroller = NULL;

/* ---- Globals ---- */
static struct android_app g_app;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static SDL_Window *g_sdl_window = NULL;

// Fake window handle - we just use a pointer to distinguish it from NULL
static int g_fake_native_window = 1;

// Fake input queue handle
static int g_fake_input_queue = 1;

/* ---- Input event queue helpers ---- */

static int input_queue_count(void) {
  return (g_input_head - g_input_tail + MAX_INPUT_EVENTS) % MAX_INPUT_EVENTS;
}

static int input_queue_push(const FakeInputEvent *ev) {
  int next = (g_input_head + 1) % MAX_INPUT_EVENTS;
  if (next == g_input_tail)
    return 0; // full
  g_input_queue[g_input_head] = *ev;
  g_input_head = next;
  return 1;
}

static FakeInputEvent *input_queue_pop(void) {
  if (g_input_tail == g_input_head)
    return NULL; // empty
  FakeInputEvent *ev = &g_input_queue[g_input_tail];
  g_input_tail = (g_input_tail + 1) % MAX_INPUT_EVENTS;
  return ev;
}

/* ---- Push key event ---- */

static void push_key_event(int action, int keycode) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  input_queue_push(&ev);
}

/* ---- Push motion (touch) event ---- */

static void push_motion_event(int action, float x, float y) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = action;
  ev.source = AINPUT_SOURCE_TOUCHSCREEN;
  ev.x = x;
  ev.y = y;
  ev.pointer_count = 1;
  ev.pointer_id = 0;
  input_queue_push(&ev);
}

/* ---- Push joystick motion event (axis values) ---- */

static void push_joystick_event(float lx, float ly, float rx, float ry) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.pointer_count = 1;
  /* engine oz lê o stick esquerdo via AMotionEvent_getX/getY (== x/y do
   * pointer 0), não getAxisValue — coi_nx confirma. Preencher os dois. */
  ev.x = lx;
  ev.y = ly;
  ev.axes[AMOTION_EVENT_AXIS_X] = lx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = ly;
  ev.axes[AMOTION_EVENT_AXIS_Z] = rx;
  ev.axes[AMOTION_EVENT_AXIS_RZ] = ry;
  input_queue_push(&ev);
}

/* ---- COI direções (modo FireTV/gamepad do engine oz) ----
 * Os keycodes DPAD 19-22 do engine são ROTACIONADOS (coi_nx) — nunca mandar.
 * Direção = evento JOYSTICK com vetor 8-way CANÔNICO em getX/getY: cardinais
 * ±1.0 (só assim o engine limpa o eixo perpendicular — stick cru trava em
 * diagonal), diagonais ±0.7. Dpad tem prioridade sobre o stick. */
static int g_coi_dup, g_coi_ddown, g_coi_dleft, g_coi_dright; /* dpad digital */
static float g_coi_slx, g_coi_sly;                            /* stick esq cru */

/* LATCH DE TAP CURTO (licao Oceanhorn v1.0.4). A direcao so' e' enviada uma vez
 * por frame, no fim do processamento de eventos. Um toque de d-pad mais curto
 * que um frame de poll chega como DOWN e UP na MESMA volta do laco: o estado
 * volta a zero antes de qualquer envio e o toque simplesmente nao acontece —
 * exatamente o "as vezes nao registra" que aparece em pad gasto e em frontend
 * que atrasa o poll. O latch guarda a direcao pressionada no frame e a mantem
 * viva por um frame mesmo que o UP ja tenha chegado. */
static int g_coi_lup, g_coi_ldown, g_coi_lleft, g_coi_lright;
static void coi_latch_dir(int up, int down, int left, int right) {
  if (up) g_coi_lup = 1;
  if (down) g_coi_ldown = 1;
  if (left) g_coi_lleft = 1;
  if (right) g_coi_lright = 1;
}

static void coi_send_dir(void) {
  float cx = 0.0f, cy = 0.0f;
  int dup = g_coi_dup || g_coi_lup, ddown = g_coi_ddown || g_coi_ldown;
  int dleft = g_coi_dleft || g_coi_lleft, dright = g_coi_dright || g_coi_lright;
  float dpx = (float)(dright - dleft);
  float dpy = (float)(ddown - dup);
  if (dpx != 0.0f || dpy != 0.0f) {
    float k = (dpx != 0.0f && dpy != 0.0f) ? 0.7f : 1.0f;
    cx = dpx * k;
    cy = dpy * k;
  } else {
    /* HISTERESE (licao Oceanhorn v1.0.4): um unico limiar trava a direcao em
     * stick gasto — o eixo em repouso fica pousado EM CIMA do limiar e o
     * personagem anda sozinho / pisca entre andar e parar. Entrar exige mais
     * do que sair: >=0.40 para comecar, <0.28 para soltar. */
    static int engaged = 0;
    const float in2 = 0.40f * 0.40f, out2 = 0.28f * 0.28f;
    float lx = g_coi_slx, ly = g_coi_sly;
    float r2 = lx * lx + ly * ly;
    if (engaged ? (r2 > out2) : (r2 > in2)) {
      engaged = 1;
      float ax = lx < 0.0f ? -lx : lx, ay = ly < 0.0f ? -ly : ly;
      float sx = lx < 0.0f ? -1.0f : 1.0f, sy = ly < 0.0f ? -1.0f : 1.0f;
      if      (ay < ax * 0.5f) { cx = sx;        cy = 0.0f; }
      else if (ax < ay * 0.5f) { cx = 0.0f;      cy = sy; }
      else                     { cx = sx * 0.7f; cy = sy * 0.7f; }
    } else {
      engaged = 0;
    }
  }
  static float pcx, pcy; /* re-envia só na mudança (todo frame gira menu) */
  if (cx != pcx || cy != pcy) {
    pcx = cx;
    pcy = cy;
    push_joystick_event(cx, cy, 0.0f, 0.0f);
    debugPrintf("[coi-dir] %.1f,%.1f\n", cx, cy);
  }
  /* O latch vale por UM envio: limpo aqui, depois de ter sido considerado. */
  g_coi_lup = g_coi_ldown = g_coi_lleft = g_coi_lright = 0;
}

/* ---- SDL button → Android keycode mapping ---- */

/* Mapa COI (engine oz em modo FireTV, keycodes que RegisterCommonFilters
 * registra — extraído do coi_nx, mesmo binário):
 *   96 BUTTON_A -> eng 1  (pulo/confirma)   97 BUTTON_B -> eng 33 (ação)
 *  100 BUTTON_Y -> eng 34 (ação)            85          -> eng 32 (ação)
 *    4 BACK     -> pausa/menu de pausa      82 MENU     -> MENU
 * DPAD 19-22 NÃO (rotacionados no engine — direção vai por eixo). */
#define AKEYCODE_COI_ACT32 85
static int sdl_button_to_keycode(int sdl_button) {
  switch (sdl_button) {
  case SDL_CONTROLLER_BUTTON_A:
    return AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_B:
    return AKEYCODE_BUTTON_B;
  case SDL_CONTROLLER_BUTTON_X:
    return AKEYCODE_COI_ACT32;
  case SDL_CONTROLLER_BUTTON_Y:
    return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_BACK:
    return AKEYCODE_MENU;
  case SDL_CONTROLLER_BUTTON_START:
    return AKEYCODE_BACK; /* Start -> pausa (engine trata BACK=pause) */
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return AKEYCODE_BUTTON_R1;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return AKEYCODE_BUTTON_THUMBL;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return AKEYCODE_BUTTON_THUMBR;
  /* dpad: tratado como direção analógica em coi_send_dir(), sem keycode */
  default:
    return -1;
  }
}

/* ---- Initialize gamepad ---- */

static void init_gamecontroller(void) {
  if (g_gamecontroller)
    return;
  int num = SDL_NumJoysticks();
  debugPrintf("android_shim: %d joysticks found\n", num);
  for (int i = 0; i < num; i++) {
    /* ANTES do SDL_IsGameController: o gamecontrollerdb do CFW e' autorado com
     * o ROTULO impresso no aparelho (estilo Nintendo), nao com a POSICAO que o
     * SDL usa. Medido neste device: o db entrega `a:b1,b:b0` — ou seja, o botao
     * de BAIXO viraria "B" e o de pular deixaria de ser o de baixo, num jogo de
     * plataforma. O kernel nomeia por posicao real (BTN_SOUTH/EAST/...), e e'
     * dele que tiramos a verdade. Desliga com COI_POSITIONAL_FIX=0. */
    pad_positional_fix_apply(i, "COI");
    if (SDL_IsGameController(i)) {
      g_gamecontroller = SDL_GameControllerOpen(i);
      if (g_gamecontroller) {
        logPrintf("android_shim: Opened gamepad: %s\n",
                    SDL_GameControllerName(g_gamecontroller));
        return;
      }
    }
  }
}

/* ---- Ponte Paddleboat (input nativo herdado do scaffold) ----
 * O Paddleboat está ESTÁTICO no libNativeGame com os entry-points exportados.
 * Alimentamos ele direto (sem Java): registra o controle via
 * Java_..._onControllerConnected e injeta eventos via
 * Paddleboat_processGameActivity{Key,Motion}InputEvent.
 * Layouts extraídos do binário:
 *  key:    {devId@0,src@4,action@8,keyCode@48} size 56
 *  motion: {devId@0,src@4,action@8,ptrCount@56,ptrs@64
 *           (8×{id;float axes[48];rawX;rawY}=204), precision@1696} size 1704
 *  onControllerConnected(env,thiz,jintArray[7],jfloatArray mins/maxs/flats/
 *  fuzzes[48]); deviceInfo={devId,vendor,product,axisBitsLow,axisBitsHigh,
 *  controllerNumber,flags}. Eventos têm que casar o deviceId. */
#define PB_DEVICE_ID 7777
#define PB_SRC_JOYSTICK 0x01000010
#define PB_SRC_GAMEPAD 0x00000401

typedef struct {
  int32_t deviceId, source, action, pad_;
  int64_t eventTime, downTime;
  int32_t flags, metaState, modifiers, repeatCount, keyCode, unicodeChar;
} PbKeyEvent; /* 56 bytes */

typedef struct {
  int32_t id;
  float axisValues[48];
  float rawX, rawY;
} PbPointer; /* 204 bytes */

typedef struct {
  int32_t deviceId, source, action, pad_;
  int64_t eventTime, downTime;
  int32_t flags, metaState, actionButton, buttonState, classification,
      edgeFlags;
  uint32_t pointerCount;
  int32_t pad2_;
  PbPointer pointers[8];
  float precisionX, precisionY;
} PbMotionEvent; /* 1704 bytes */

_Static_assert(sizeof(PbKeyEvent) == 56, "PbKeyEvent layout");
_Static_assert(sizeof(PbMotionEvent) == 1704, "PbMotionEvent layout");

static int g_pb_connected = 0;
static int (*pb_isInitialized)(void);
/* wrappers DA ENGINE (Paddleboat::ProcessInputEvent): além de processar o
 * evento, setam o flag "teve input" [impl+64] que o FrameStart exige p/
 * ler getControllerData. Chamar a API C crua deixa a engine cega! */
static int32_t (*pb_processKey)(const void *);
static int32_t (*pb_processMotion)(const void *);
static void (*pb_onConnected)(void *, void *, void *, void *, void *, void *,
                              void *);

static void pb_try_connect(void) {
  if (g_pb_connected) return;
  /* 🏎️ CACHE NEGATIVO — o gargalo nº 1 de fps deste port, medido no R36T em
   * 06/08/2026 com o profiler de amostragem (COI_PROFILE=1):
   *   libc 47% da CPU do frame, e o chamador de quase toda ela era
   *   so_find_addr_safe, daqui.
   * Esta engine (Sega "oz", 2013) NÃO tem Paddleboat — a string não aparece uma
   * vez sequer no libViewer_GP.so. As quatro buscas falhavam, o código zerava
   * pb_isInitialized e, como isto roda a cada frame pelo process_sdl_events,
   * refazia QUATRO varreduras lineares com strcmp sobre os 21.324 símbolos
   * dinâmicos da engine, todo frame, para sempre (~85 mil strcmp por frame).
   * Um símbolo ausente do .dynsym nunca passa a existir depois do load, então
   * uma tentativa é suficiente e desistir é definitivo. */
  static int pb_absent = 0;
  if (pb_absent) return;
  if (!pb_isInitialized) {
    pb_isInitialized =
        (int (*)(void))so_find_addr_safe("Paddleboat_isInitialized");
    pb_processKey = (int32_t(*)(const void *))so_find_addr_safe(
        "_ZN10Paddleboat17ProcessInputEventERK20GameActivityKeyEvent");
    pb_processMotion = (int32_t(*)(const void *))so_find_addr_safe(
        "_ZN10Paddleboat17ProcessInputEventERK23GameActivityMotionEvent");
    pb_onConnected =
        (void (*)(void *, void *, void *, void *, void *, void *, void *))
            so_find_addr_safe("Java_com_google_android_games_paddleboat_"
                              "GameControllerManager_onControllerConnected");
    if (!pb_isInitialized || !pb_processKey || !pb_processMotion ||
        !pb_onConnected) {
      logPrintf("android_shim: Paddleboat exports not found in the engine -- "
                "controller path stays on SDL (no further symbol scans)\n");
      pb_isInitialized = NULL;
      pb_absent = 1;
      return;
    }
  }
  if (!pb_isInitialized()) return; /* engine ainda não rodou Paddleboat_init */

  /* axisBits: X,Y(sticks L) Z,RZ(stick R) HAT_X/Y(dpad) L/RTRIGGER */
  static const int32_t info[7] = {
      PB_DEVICE_ID, 0x0810, 0x0001,
      (1 << 0) | (1 << 1) | (1 << 11) | (1 << 14) | (1 << 15) | (1 << 16) |
          (1 << 17) | (1 << 18),
      0, 1, 0};
  static float mins[48], maxs[48], flats[48], fuzzes[48];
  for (int i = 0; i < 48; i++) {
    mins[i] = -1.0f; maxs[i] = 1.0f; flats[i] = 0.05f; fuzzes[i] = 0.01f;
  }
  pb_onConnected(g_activity.env, NULL, jni_shim_make_array(info, 7),
                 jni_shim_make_array(mins, 48), jni_shim_make_array(maxs, 48),
                 jni_shim_make_array(flats, 48),
                 jni_shim_make_array(fuzzes, 48));
  g_pb_connected = 1;
  debugPrintf("android_shim: Paddleboat controle conectado (devId=%d)\n",
              PB_DEVICE_ID);
}

static void pb_send_key(int action, int keycode) {
  if (!g_pb_connected) return;
  PbKeyEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.deviceId = PB_DEVICE_ID;
  ev.source = PB_SRC_GAMEPAD;
  ev.action = action; /* 0=down 1=up */
  ev.keyCode = keycode;
  int32_t r = pb_processKey(&ev);
  debugPrintf("android_shim: pb_key action=%d kc=%d -> %d\n", action, keycode,
              (int)r);
}

static void pb_send_motion(float lx, float ly, float rx, float ry, float hx,
                           float hy, float lt, float rt) {
  if (!g_pb_connected) return;
  static PbMotionEvent ev; /* 1.7KB, fora da stack */
  memset(&ev, 0, sizeof(ev));
  ev.deviceId = PB_DEVICE_ID;
  ev.source = PB_SRC_JOYSTICK;
  ev.action = 2; /* AMOTION_EVENT_ACTION_MOVE */
  ev.pointerCount = 1;
  ev.pointers[0].id = 0;
  ev.pointers[0].axisValues[0] = lx;   /* AXIS_X */
  ev.pointers[0].axisValues[1] = ly;   /* AXIS_Y */
  ev.pointers[0].axisValues[11] = rx;  /* AXIS_Z */
  ev.pointers[0].axisValues[14] = ry;  /* AXIS_RZ */
  ev.pointers[0].axisValues[15] = hx;  /* AXIS_HAT_X */
  ev.pointers[0].axisValues[16] = hy;  /* AXIS_HAT_Y */
  ev.pointers[0].axisValues[17] = lt;  /* AXIS_LTRIGGER */
  ev.pointers[0].axisValues[18] = rt;  /* AXIS_RTRIGGER */
  pb_processMotion(&ev);
}

/* ---- Process SDL events into input queue ---- */

#define STICK_DEADZONE 8000
#define CURSOR_SPEED 12.0f

static float g_hat_x = 0, g_hat_y = 0;
static float g_last_lt = 0, g_last_rt = 0;
static int g_motion_dirty = 0;

static void update_hat_from_dpad(int button, int down) {
  float v = down ? 1.0f : 0.0f;
  if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  g_hat_x = down ? -v : 0;
  if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) g_hat_x = v;
  if (button == SDL_CONTROLLER_BUTTON_DPAD_UP)    g_hat_y = down ? -v : 0;
  if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  g_hat_y = v;
  g_motion_dirty = 1;
}

/* modo gptokeyb (launcher seta COI_INPUT=gptk): botões vêm do TECLADO
 * (uinput do gptokeyb via um .gptk); botões nativos do pad são ignorados
 * (duplicariam). Eixos analógicos continuam nativos quando o pad é visível. */
static int gptk_on(void) {
  static int g = -1;
  if (g < 0) {
    const char *ie = getenv("COI_INPUT");
    g = (ie && strcmp(ie, "gptk") == 0) ? 1 : 0;
    if (g) debugPrintf("android_shim: modo GPTOKEYB (teclado via gptokeyb)\n");
  }
  return g;
}

/* Hotkey universal de SAIR (SELECT+START) — igual ao Bully, NO BINARIO. Garantia
 * independente do gptokeyb: o launcher chamaria o gptokeyb mas o processo
 * tem comm="Main" (a engine renomeia a thread), entao o gptokeyb NAO acha o
 * processo p/ matar -> o .sh sozinho nao fecha. Aqui lemos o pad direto (SDL ve
 * o pad mesmo com gptokeyb, que nao faz grab exclusivo) e `_exit` na hora
 * (evita deadlock do blob Mali ao liberar o contexto GL no teardown). */
/* estado SELECT(esc)+START(enter) vindos do gptokeyb -- rastreado SEMPRE (mesmo
 * sem gptk_on), igual o g_kb[] do Bully, p/ a saida funcionar em qualquer device */
static int g_kb_esc = 0, g_kb_ent = 0;

/* ---- Saida: UM caminho so, para TODAS as origens ----
 * SELECT+START (GameController), SELECT+START (pad CRU fora da base SDL),
 * SELECT+START (esc+enter do gptokeyb) e SIGTERM do frontend convergem aqui.
 *
 * Nao da para bloquear esperando a engine: check_exit_hotkey() roda DENTRO do
 * loop principal do jogo (ALooper_pollAll). Entao a saida e' em duas fases:
 *   fase 1 -> empurra PAUSE/SAVE_STATE/LOST_FOCUS no pipe de comandos e VOLTA
 *             para o loop, para a engine drenar o pipe e gravar o save;
 *   fase 2 -> depois da carencia, _exit(0).
 * _exit (e nao exit) continua proposital: liberar o contexto GL no teardown
 * trava o blob Mali. O alarm() e' a rede de seguranca se o loop enroscar. */
static volatile sig_atomic_t g_sigterm = 0;
static int g_shutdown = 0;      /* 0=rodando 1=pause/save enviado */
static Uint32 g_shutdown_at = 0; /* SDL_GetTicks do inicio da fase 1 */
#define COI_SHUTDOWN_GRACE_MS 700

static void coi_sigterm_handler(int sig) {
  (void)sig;
  g_sigterm = 1;
  alarm(5); /* backstop: loop enroscado nao pode segurar o frontend refem */
}
static void coi_sigalrm_handler(int sig) {
  (void)sig;
  _exit(0);
}

void android_shim_install_exit_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = coi_sigterm_handler;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = coi_sigalrm_handler;
  sigaction(SIGALRM, &sa, NULL);
}

/* ---- SELECT+START pelo EVDEV, que e' a verdade do teclado do pad ----------
 *
 * O caminho por ORDINAL de joystick (abaixo) e' um chute educado, e num pad
 * real ele erra: o GO-Super Gamepad do R36S/RG351 nao tem BTN_SELECT/BTN_START
 * nenhum — SELECT e START chegam como BTN_TRIGGER_HAPPY1/2 — e ainda expoe
 * TRIGGER_HAPPY ate o 5. "Os dois ultimos botoes" ali sao TH4 e TH5, botoes que
 * o aparelho nem tem fisicamente, e o combo de saida simplesmente nunca
 * fecharia. Ordinal tambem depende de como CADA versao do SDL enumera as teclas
 * e de quais viram hat.
 *
 * Aqui perguntamos ao proprio dispositivo: `EVIOCGKEY` devolve o estado atual
 * de TODAS as teclas de um `/dev/input/eventN`, por KEYCODE. Nada de adivinhar
 * ordinal, nada de depender de o pad estar na base do SDL. E' leitura pura —
 * sem grab, sem consumir evento — entao o jogo continua recebendo o input
 * normalmente pelo SDL. Cada pad candidato guarda o par de teclas que ELE tem:
 * BTN_SELECT/BTN_START quando existem, senao TRIGGER_HAPPY1/2.
 *
 * Se /dev/input nao for legivel (firmware que roda o port sem grupo `input`),
 * nada disso funciona e os caminhos SDL abaixo continuam valendo — por isso os
 * tres coexistem. */
#define COI_BTN_SELECT 0x13a
#define COI_BTN_START 0x13b
#define COI_BTN_JOYSTICK 0x120
#define COI_BTN_BASE3 0x128 /* SELECT nos pads USB "genericos" de 12 botoes */
#define COI_BTN_BASE4 0x129 /* START idem */
#define COI_BTN_GAMEPAD_LAST 0x13f
#define COI_BTN_TRIGGER_HAPPY1 0x2c0
#define COI_BTN_TRIGGER_HAPPY2 0x2c1
#define COI_EV_KEY 0x01
#define COI_EV_ABS 0x03
#define COI_EVDEV_MAX 8

struct coi_evdev_pad {
  int fd;
  int node;
  int k_sel, k_start;
};
static struct coi_evdev_pad g_evpads[COI_EVDEV_MAX];
static int g_evpad_n = 0;

static int coi_key_bit(const unsigned long *bits, int code) {
  return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1UL;
}

/* Varre /dev/input em busca de pads. Roda de novo periodicamente porque pad
 * USB/BT conectado DEPOIS do jogo aberto e' caso normal em handheld com dock —
 * uma varredura unica no arranque deixaria esse pad sem combo de saida. Nodes
 * ja abertos sao pulados; so' o que e' novo entra. */
static void coi_evdev_scan(void) {
  static int announced_empty = 0;
  for (int i = 0; i < 32 && g_evpad_n < COI_EVDEV_MAX; i++) {
    int known = 0;
    for (int k = 0; k < g_evpad_n; k++)
      if (g_evpads[k].node == i) { known = 1; break; }
    if (known) continue;
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    /* KEY_MAX = 0x2ff; o bitmap tem que ser dimensionado no LONG DE QUEM LE */
    unsigned long keybits[(0x300 / (8 * sizeof(long))) + 1];
    unsigned long evbits[2];
    memset(keybits, 0, sizeof(keybits));
    memset(evbits, 0, sizeof(evbits));
    /* EIXO obrigatorio: receptor de IR e teclado de controle remoto declaram
     * teclas na faixa BTN_* e passariam pelo teste de botao — mas nao tem eixo
     * nenhum. Exigir EV_ABS separa pad de verdade de fantasma. */
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
        !coi_key_bit(evbits, COI_EV_ABS) ||
        ioctl(fd, EVIOCGBIT(COI_EV_KEY, sizeof(keybits)), keybits) < 0) {
      close(fd);
      continue;
    }
    int has_btn = 0;
    for (int k = COI_BTN_JOYSTICK; k <= COI_BTN_GAMEPAD_LAST && !has_btn; k++)
      has_btn = coi_key_bit(keybits, k);
    if (!has_btn) has_btn = coi_key_bit(keybits, COI_BTN_TRIGGER_HAPPY1);
    if (!has_btn) { close(fd); continue; }
    /* O PAR e' o que o aparelho REALMENTE tem, nesta ordem de confianca:
     * nomes canonicos > TRIGGER_HAPPY (R36S/RG351 e boa parte dos handhelds
     * chineses) > BASE3/BASE4 (pad USB generico de 12 botoes estilo joystick,
     * onde SELECT/START sao o 9o e o 10o). */
    int sel = -1, start = -1;
    if (coi_key_bit(keybits, COI_BTN_SELECT) &&
        coi_key_bit(keybits, COI_BTN_START)) {
      sel = COI_BTN_SELECT;
      start = COI_BTN_START;
    } else if (coi_key_bit(keybits, COI_BTN_TRIGGER_HAPPY1) &&
               coi_key_bit(keybits, COI_BTN_TRIGGER_HAPPY2)) {
      sel = COI_BTN_TRIGGER_HAPPY1;
      start = COI_BTN_TRIGGER_HAPPY2;
    } else if (coi_key_bit(keybits, COI_BTN_BASE3) &&
               coi_key_bit(keybits, COI_BTN_BASE4)) {
      sel = COI_BTN_BASE3;
      start = COI_BTN_BASE4;
    }
    if (sel < 0) { close(fd); continue; }
    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    g_evpads[g_evpad_n].fd = fd;
    g_evpads[g_evpad_n].node = i;
    g_evpads[g_evpad_n].k_sel = sel;
    g_evpads[g_evpad_n].k_start = start;
    g_evpad_n++;
    logPrintf("android_shim: evdev exit pair on %s ('%s'): SELECT=0x%x START=0x%x\n",
              path, name, sel, start);
  }
  if (!g_evpad_n && !announced_empty) {
    announced_empty = 1;
    logPrintf("android_shim: no readable pad in /dev/input; exit relies on "
              "SDL (GameController/raw pad/keyboard)\n");
  }
}

static int coi_evdev_exit_combo(void) {
  static int tick = 0;
  if ((tick++ % 180) == 0) coi_evdev_scan(); /* ~a cada 3s: cobre hotplug */
  for (int i = 0; i < g_evpad_n; i++) {
    unsigned long keys[(0x300 / (8 * sizeof(long))) + 1];
    memset(keys, 0, sizeof(keys));
    if (ioctl(g_evpads[i].fd, EVIOCGKEY(sizeof(keys)), keys) < 0) continue;
    if (coi_key_bit(keys, g_evpads[i].k_sel) &&
        coi_key_bit(keys, g_evpads[i].k_start))
      return 1;
  }
  return 0;
}

/* Pad CRU: quando o firmware entrega um pad que NAO esta na base do SDL, o
 * GameController nunca abre e o combo de saida sumiria (relato muOS/RG40XX-H).
 * Ai lemos os botoes por ORDINAL de joystick. SELECT/START chegam como
 * BTN_TRIGGER_HAPPY1/2 em varios handhelds, o que empurra os ordinais para o
 * fim da lista; por isso checamos os ordinais classicos (6/7) E os dois
 * ultimos botoes do dispositivo. Fica como REDE — a autoridade e' o evdev. */
static SDL_Joystick *g_raw_joy = NULL;
static int raw_pad_exit_combo(void) {
  if (g_gamecontroller)
    return 0; /* pad ja normalizado: o caminho GameController vale */
  if (!g_raw_joy) {
    if (SDL_NumJoysticks() <= 0)
      return 0;
    g_raw_joy = SDL_JoystickOpen(0);
    if (!g_raw_joy)
      return 0;
    logPrintf("android_shim: raw pad opened for the exit hotkey: %s (%d buttons)\n",
                SDL_JoystickName(g_raw_joy), SDL_JoystickNumButtons(g_raw_joy));
  }
  SDL_JoystickUpdate();
  int nb = SDL_JoystickNumButtons(g_raw_joy);
  if (nb < 2)
    return 0;
  int classic = (nb > 7) && SDL_JoystickGetButton(g_raw_joy, 6) &&
                SDL_JoystickGetButton(g_raw_joy, 7);
  int happy = SDL_JoystickGetButton(g_raw_joy, nb - 2) &&
              SDL_JoystickGetButton(g_raw_joy, nb - 1);
  return classic || happy;
}

static void coi_begin_shutdown(const char *why) {
  if (g_shutdown)
    return;
  g_shutdown = 1;
  g_shutdown_at = SDL_GetTicks();
  logPrintf("android_shim: exit via %s -> pause/save, _exit in %dms\n",
              why, COI_SHUTDOWN_GRACE_MS);
  android_shim_send_cmd(&g_app, APP_CMD_LOST_FOCUS);
  android_shim_send_cmd(&g_app, APP_CMD_SAVE_STATE);
  android_shim_send_cmd(&g_app, APP_CMD_PAUSE);
  alarm(5); /* mesma rede de seguranca do SIGTERM */
}

static void check_exit_hotkey(void) {
  if (g_shutdown) {
    if (SDL_GetTicks() - g_shutdown_at >= COI_SHUTDOWN_GRACE_MS) {
      logPrintf("android_shim: pause/save done -> exiting\n");
      _exit(0);
    }
    return;
  }
  int pad_combo = 0;
  if (g_gamecontroller) {
    SDL_GameControllerUpdate();
    pad_combo = SDL_GameControllerGetButton(g_gamecontroller, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(g_gamecontroller, SDL_CONTROLLER_BUTTON_START);
  }
  if (g_sigterm)
    coi_begin_shutdown("SIGTERM");
  else if (coi_evdev_exit_combo())
    coi_begin_shutdown("SELECT+START (evdev)");
  else if (pad_combo)
    coi_begin_shutdown("SELECT+START (pad)");
  else if (g_kb_esc && g_kb_ent)
    coi_begin_shutdown("SELECT+START (teclado/gptokeyb)");
  else if (raw_pad_exit_combo())
    coi_begin_shutdown("SELECT+START (pad cru)");
}

/* ---- TOQUE SINTETICO NO L1: a tela touch-only alcancada pelo controle ----
 *
 * As cutscenes do jogo trazem "Tap to Skip", e o pulo so' existe pelo TOQUE:
 * nem A, nem BACK, nem START fazem nada ali. Num handheld sem tela sensivel o
 * jogador nao fica preso — a cena termina sozinha — mas fica REFEM de ~90s de
 * introducao toda vez, e essa e' a mesma familia de problema que ja custou uma
 * release (TASM2 v1.1.9, tela touch-only que o controle nao alcancava).
 *
 * A resposta e' um botao dedicado, nao um sequestro de botao usado: L1 nao tem
 * funcao nenhuma neste jogo no modo gamepad. L1 emite um toque no CENTRO do
 * drawable real — mesmo caminho de input do dedo, portanto vale para qualquer
 * prompt "toque para continuar", em qualquer resolucao (a coordenada sai do
 * drawable, nunca de um tamanho cravado). Fora dessas telas o toque no centro
 * nao cai em nenhum controle virtual (dpad fica na esquerda, acao na direita),
 * entao apertar L1 jogando nao faz nada.
 *
 * Sequencia em frames separados de proposito: o edge-detect da engine em modo
 * FireTV engole down+up no mesmo pump. */
static int g_synth_tap_hold = 0;
static float g_synth_tap_x = 0, g_synth_tap_y = 0;
static void coi_synth_tap_begin(void) {
  if (g_synth_tap_hold > 0) return;
  g_synth_tap_x = (float)coi_screen_w * 0.5f;
  g_synth_tap_y = (float)coi_screen_h * 0.5f;
  push_motion_event(5, g_synth_tap_x, g_synth_tap_y); /* POINTER_DOWN */
  g_synth_tap_hold = 3;
  /* As primeiras vezes vao para o log de release: se um usuario relatar que
   * L1 "nao pula a cutscene", a primeira pergunta e' se o toque saiu, e em que
   * coordenada. Depois disso cala, para nao virar ruido. */
  static int n = 0;
  if (n < 3) { n++; logPrintf("android_shim: L1 -> synthetic tap at %.0f,%.0f\n", g_synth_tap_x, g_synth_tap_y); }
}
static void coi_synth_tap_pump(void) {
  if (g_synth_tap_hold > 0 && --g_synth_tap_hold == 0)
    push_motion_event(6, g_synth_tap_x, g_synth_tap_y); /* POINTER_UP */
}

/* Injetor de TOQUE por coordenada (debug/automação): `echo "x y" > /dev/shm/coi_tap`
 * -> toca (down, move, up ~5 frames) na posição ABSOLUTA x,y. A UI do menu é touch,
 * e o toque é caminho separado do controle 0 -> IMUNE ao attract demo que sobrescreve
 * o pad (por isso a navegação por botão era não-determinística). Permite entrar no
 * jogo de forma confiável (tocar no PLAY). Custo zero sem o trigger. */
void coi_tap_inject(void) {   /* chamado de my_pb_getdata (roda no menu E in-game) */
  /* GATE OFF POR PADRAO (regra: experimento fora do binario de release). Isto
   * e' ferramenta de diagnostico da bancada: sem o gate, o release faria tres
   * fopen() em /dev/shm a cada 6 frames para sempre, e um /dev/shm com permissao
   * frouxa viraria canal de injecao de input no jogo do usuario.
   * Ligar so' para depurar: COI_DEBUG_INJECT=1. */
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("COI_DEBUG_INJECT") ? 1 : 0;
  if (!enabled)
    return;
  static int chk = 0, hold = 0;
  static float tx = 0, ty = 0;
  if (hold > 0) {
    if (--hold == 0) {
      /* engine oz checa POINTER_UP(6); COI_TAPDU=1 usa UP(1) classico */
      push_motion_event(getenv("COI_TAPDU") ? 1 : 6, tx, ty);
      debugPrintf("[tap] up %.0f,%.0f\n", tx, ty);
    }
    return;
  }
  /* tecla segurada: UP só depois de kh frames (down+up no mesmo pump é
   * engolido pelo edge-detect do engine em modo FireTV) */
  static int kh = 0, kkc = 0;
  if (kh > 0 && --kh == 0 && kkc) {
    push_key_event(AKEY_EVENT_ACTION_UP, kkc);
    debugPrintf("[key] %d up\n", kkc);
    kkc = 0;
  }
  if (++chk % 6) return;
  /* tecla via /dev/shm/coi_key "keycode" (A=96 B=97 Y=100 X=85 BACK=4 MENU=82) */
  FILE *k = fopen("/dev/shm/coi_key", "r");
  if (k) {
    int kc = 0;
    if (fscanf(k, "%d", &kc) == 1 && kc) {
      push_key_event(AKEY_EVENT_ACTION_DOWN, kc);
      kkc = kc;
      kh = 4; /* UP em ~4 frames */
      debugPrintf("[key] %d down (up em %d frames)\n", kc, kh);
    }
    fclose(k);
    unlink("/dev/shm/coi_key");
  }
  /* direção via /dev/shm/coi_dir "x y" (floats -1..1; 0 0 = solta) */
  FILE *d = fopen("/dev/shm/coi_dir", "r");
  if (d) {
    float dx, dy;
    if (fscanf(d, "%f %f", &dx, &dy) == 2) {
      push_joystick_event(dx, dy, 0.0f, 0.0f);
      debugPrintf("[dir] %.2f,%.2f\n", dx, dy);
    }
    fclose(d);
    unlink("/dev/shm/coi_dir");
  }
  FILE *f = fopen("/dev/shm/coi_tap", "r");
  if (!f) return;
  float x, y;
  if (fscanf(f, "%f %f", &x, &y) == 2) {
    tx = x; ty = y; hold = 2;   /* DOWN agora, UP em 2 frames (tap limpo) */
    /* engine oz checa POINTER_DOWN(5); COI_TAPDU=1 usa DOWN(0) classico */
    push_motion_event(getenv("COI_TAPDU") ? 0 : 5, x, y);
    debugPrintf("[tap] down %.0f,%.0f\n", x, y);
  }
  fclose(f);
  unlink("/dev/shm/coi_tap");
}

/* PRESSAO DE MEMORIA -> caminho LEGITIMO da plataforma (licao Oceanhorn
 * v1.0.3/v1.0.4). Em handheld de 1 GB sem swap, o OOM killer leva o jogo antes
 * de qualquer aviso. A resposta certa e' ECONOMIA LOCAL avisando a ENGINE para
 * podar os proprios caches — `APP_CMD_LOW_MEMORY` e' exatamente o sinal que a
 * onAppCmd do jogo ja sabe tratar. Duas coisas que NAO fazemos, ambas por
 * licao paga: (a) varredura/GC proprio, que deu SIGSEGV; (b) qualquer toque no
 * sistema do usuario — nunca criamos swap, nunca mexemos em servico ou config.
 * Le MemAvailable, que ja desconta cache recuperavel; abaixo do piso avisa uma
 * vez e so' rearma depois que a memoria folga de novo (sem tempestade de
 * sinais). */
static void coi_check_low_memory(void) {
  static int armed = 1;
  static int tick = 0;
  if ((tick++ % 120) != 0) return; /* ~2x/s no pior caso; custo irrelevante */
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return;
  char line[128];
  long avail_kb = -1;
  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1) break;
  fclose(f);
  if (avail_kb < 0) return;
  const long low_kb = 60L * 1024;   /* < 60 MB: pedir poda */
  const long rearm_kb = 110L * 1024; /* > 110 MB: histerese, rearma o aviso */
  if (armed && avail_kb < low_kb) {
    armed = 0;
    logPrintf("android_shim: MemAvailable=%ld kB -> APP_CMD_LOW_MEMORY "
                "(the engine trims its own caches; nothing is done to the system)\n",
                avail_kb);
    android_shim_send_cmd(&g_app, APP_CMD_LOW_MEMORY);
  } else if (!armed && avail_kb > rearm_kb) {
    armed = 1;
  }
}

void coi_tap_inject(void);
static void process_sdl_events(void) {
  // Try to open a gamepad if we don't have one yet
  init_gamecontroller();
  pb_try_connect();
  check_exit_hotkey();  /* SELECT+START -> sai (garantia, qualquer device) */
  coi_check_low_memory();
  coi_synth_tap_pump();  /* fecha o toque sintetico do L1 alguns frames depois */
  coi_tap_inject();     /* COI modo touch (sem Paddleboat): tap via /dev/shm/coi_tap */
  /* coi_tap_inject e' chamado de my_pb_getdata (main.c) -> roda no menu tambem */

  /* diag: loga status Paddleboat do pad 0 periodicamente */
  if (g_pb_connected) {
    static int poll_n = 0;
    static int32_t (*pb_getStatus)(int32_t) = NULL;
    if (!pb_getStatus)
      pb_getStatus = (int32_t(*)(int32_t))so_find_addr_safe(
          "Paddleboat_getControllerStatus");
    if (pb_getStatus && (poll_n++ % 180) == 0)
      debugPrintf("android_shim: PB status(0)=%d\n", (int)pb_getStatus(0));

    /* força polling: FrameStart só lê getControllerData se o flag "teve
     * input" [impl+64] estiver setado, e Update() limpa ele todo frame
     * (a ordem engole o set feito pelos eventos injetados no pollAll).
     * Setamos 1 a cada pump -> engine lê o pad TODO frame (modo console). */
    static uint8_t **pb_impl = NULL;
    if (!pb_impl) {
      pb_impl = (uint8_t **)so_find_addr_safe("_ZN10Paddleboat14implementationE");
      debugPrintf("android_shim: pb_impl @ %p -> %p\n", (void *)pb_impl,
                  pb_impl ? (void *)*pb_impl : NULL);
    }
    if (pb_impl && *pb_impl) {
      uint8_t *impl = *pb_impl;
      if ((poll_n % 180) == 1)
        debugPrintf("android_shim: impl conn0=%d dirty=%d\n", impl[16],
                    impl[64]);
      impl[64] = 1;
    }
    /* self-test autônomo: sequência de botões cronometrada (sem humano).
     * COI_PB_SCRIPT="frame:action:keycode,..." (action 0=down 1=up).
     * Ex default: aperta A no frame 300 (confirma menu inicial), depois
     * DOWN/A pra navegar. Cada frame ~ pump do pollAll. */
    if (getenv("COI_PB_SELFTEST")) {
      /* Sequência de navegação: no menu inicial = 1× pra BAIXO,
       * depois A/X pra entrar no jogo. Frames ~ pumps (poll_n).
       * Cada press = down no frame f, up em f+10. */
      struct { int f, act, kc; } seq[] = {
        {360, 0, AKEYCODE_BUTTON_A},   {370, 1, AKEYCODE_BUTTON_A},   /* title->menu */
        {480, 0, AKEYCODE_DPAD_DOWN},  {490, 1, AKEYCODE_DPAD_DOWN},  /* 1x baixo */
        {560, 0, AKEYCODE_BUTTON_X},   {570, 1, AKEYCODE_BUTTON_X},   /* X entra (sequência do usuário) */
        {660, 0, AKEYCODE_BUTTON_B},   {670, 1, AKEYCODE_BUTTON_B},   /* B dispensa promo DLC */
        {760, 0, AKEYCODE_DPAD_DOWN},  {770, 1, AKEYCODE_DPAD_DOWN},  /* retry: baixo */
        {820, 0, AKEYCODE_BUTTON_X},   {830, 1, AKEYCODE_BUTTON_X},   /* retry: X entra */
        {920, 0, AKEYCODE_BUTTON_B},   {930, 1, AKEYCODE_BUTTON_B},   /* B dispensa popup */
        {1000, 0, AKEYCODE_BUTTON_A},  {1010, 1, AKEYCODE_BUTTON_A},  /* A confirma */
        {1100, 0, AKEYCODE_BUTTON_X},  {1110, 1, AKEYCODE_BUTTON_X},  /* X */
      };
      for (unsigned i = 0; i < sizeof(seq)/sizeof(seq[0]); i++)
        if (poll_n == seq[i].f) {
          debugPrintf("SELFTEST f=%d act=%d kc=%d\n", seq[i].f, seq[i].act,
                      seq[i].kc);
          pb_send_key(seq[i].act, seq[i].kc);
        }
    }
  }

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_QUIT:
      g_app.destroyRequested = 1;
      break;

    /* 🎮 modo GPTOKEYB (COI_INPUT=gptk, padrão PortMaster): o gptokeyb
     * do CFW lê o controle físico e emite TECLADO via uinput conforme o
     * o .gptk. Traduzimos as teclas pros MESMOS eventos Paddleboat:
     *   x=A c=B q=X t=Y enter=START esc=SELECT h=L1 j=R1 k=L2 l=R2
     *   n=L3 m=R3 setas=dpad wasd=stick esq (digital)
     * Sair: SELECT+START (esc+enter). */
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      /* GARANTIA DE SAIDA: rastreia SELECT(esc)+START(enter) SEMPRE, mesmo sem
       * gptk_on/repeat -> check_exit_hotkey (todo frame) fecha o jogo. */
      if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g_kb_esc = (e.type == SDL_KEYDOWN);
      if (e.key.keysym.scancode == SDL_SCANCODE_RETURN) g_kb_ent = (e.type == SDL_KEYDOWN);
      if (g_kb_esc && g_kb_ent) coi_begin_shutdown("SELECT+START (kbd)");
      if (!gptk_on() || e.key.repeat) break;
      int dn = (e.type == SDL_KEYDOWN);
      int act = dn ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
      static int kb_w = 0, kb_a = 0, kb_s = 0, kb_d = 0;
      static int kb_esc = 0, kb_ent = 0;
      static float kb_lt = 0, kb_rt = 0;
      int kc = -1, stick = 0, dpadbtn = -1;
      switch (e.key.keysym.scancode) {
      case SDL_SCANCODE_X:      kc = AKEYCODE_BUTTON_A; break;
      case SDL_SCANCODE_C:      kc = AKEYCODE_BUTTON_B; break;
      case SDL_SCANCODE_Q:      kc = AKEYCODE_COI_ACT32; break;
      case SDL_SCANCODE_T:      kc = AKEYCODE_BUTTON_Y; break;
      case SDL_SCANCODE_RETURN: kc = AKEYCODE_BACK; kb_ent = dn; break; /* Start->pausa */
      case SDL_SCANCODE_ESCAPE: kc = AKEYCODE_MENU; kb_esc = dn; break;
      case SDL_SCANCODE_H:      kc = AKEYCODE_BUTTON_L1; if (dn) coi_synth_tap_begin(); break;
      case SDL_SCANCODE_J:      kc = AKEYCODE_BUTTON_R1; break;
      case SDL_SCANCODE_K:      kc = AKEYCODE_BUTTON_L2; kb_lt = dn ? 1.0f : 0.0f; stick = 1; break;
      case SDL_SCANCODE_L:      kc = AKEYCODE_BUTTON_R2; kb_rt = dn ? 1.0f : 0.0f; stick = 1; break;
      case SDL_SCANCODE_N:      kc = AKEYCODE_BUTTON_THUMBL; break;
      case SDL_SCANCODE_M:      kc = AKEYCODE_BUTTON_THUMBR; break;
      /* setas: direção COI (sem keycode — DPAD 19-22 rotacionado no engine) */
      case SDL_SCANCODE_UP:     g_coi_dup = dn;    coi_latch_dir(dn,0,0,0); dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
      case SDL_SCANCODE_DOWN:   g_coi_ddown = dn;  coi_latch_dir(0,dn,0,0); dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
      case SDL_SCANCODE_LEFT:   g_coi_dleft = dn;  coi_latch_dir(0,0,dn,0); dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
      case SDL_SCANCODE_RIGHT:  g_coi_dright = dn; coi_latch_dir(0,0,0,dn); dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
      case SDL_SCANCODE_W:      kb_w = dn; stick = 1; break;
      case SDL_SCANCODE_A:      kb_a = dn; stick = 1; break;
      case SDL_SCANCODE_S:      kb_s = dn; stick = 1; break;
      case SDL_SCANCODE_D:      kb_d = dn; stick = 1; break;
      default: break;
      }
      if (kb_esc && kb_ent) coi_begin_shutdown("SELECT+START (gptk)");
      if (kc >= 0) {
        push_key_event(act, kc);
        pb_send_key(act, kc);
      }
      if (dpadbtn >= 0) update_hat_from_dpad(dpadbtn, dn);
      if (stick) {
        float lx = (kb_d ? 1.0f : 0.0f) - (kb_a ? 1.0f : 0.0f);
        float ly = (kb_s ? 1.0f : 0.0f) - (kb_w ? 1.0f : 0.0f);
        if (lx != 0.0f && ly != 0.0f) { lx *= 0.7071f; ly *= 0.7071f; }
        g_coi_slx = lx; /* COI: wasd -> direção canônica */
        g_coi_sly = ly;
        pb_send_motion(lx, ly, 0, 0, g_hat_x, g_hat_y, kb_lt, kb_rt);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONDOWN: {
      if (gptk_on()) break; /* botões vêm do teclado (gptokeyb) */
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_DOWN, kc);
        pb_send_key(AKEY_EVENT_ACTION_DOWN, kc);
        debugPrintf("android_shim: button DOWN keycode=%d\n", kc);
      }
      // D-pad also feeds HAT axes (Paddleboat dpad via motion)
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        update_hat_from_dpad(e.cbutton.button, 1);
      switch (e.cbutton.button) { /* COI: dpad -> vetor de direção */
      case SDL_CONTROLLER_BUTTON_DPAD_UP:    g_coi_dup = 1; break;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  g_coi_ddown = 1; break;
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  g_coi_dleft = 1; break;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: g_coi_dright = 1; break;
      }
      if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        coi_synth_tap_begin(); /* L1 = toque no centro (prompts touch-only) */
      /* latch: garante que um DOWN+UP no mesmo frame ainda vire direcao */
      coi_latch_dir(e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP,
                    e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                    e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                    e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
      break;
    }

    case SDL_CONTROLLERBUTTONUP: {
      if (gptk_on()) break; /* botões vêm do teclado (gptokeyb) */
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_UP, kc);
        pb_send_key(AKEY_EVENT_ACTION_UP, kc);
      }
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        update_hat_from_dpad(e.cbutton.button, 0);
      switch (e.cbutton.button) { /* COI: dpad -> vetor de direção */
      case SDL_CONTROLLER_BUTTON_DPAD_UP:    g_coi_dup = 0; break;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  g_coi_ddown = 0; break;
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  g_coi_dleft = 0; break;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: g_coi_dright = 0; break;
      }
      break;
    }

    case SDL_CONTROLLERDEVICEADDED:
      debugPrintf("android_shim: Controller added: %d\n", e.cdevice.which);
      init_gamecontroller();
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      debugPrintf("android_shim: Controller removed\n");
      if (g_gamecontroller) {
        SDL_GameControllerClose(g_gamecontroller);
        g_gamecontroller = NULL;
      }
      break;

    default:
      break;
    }
  }

  // Send analog stick values as joystick motion events
  if (g_gamecontroller) {
    int raw_lx = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_LEFTX);
    int raw_ly = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_LEFTY);
    int raw_rx = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_RIGHTX);
    int raw_ry = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_RIGHTY);

    // Apply deadzone
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE)
      lx = (float)raw_lx / 32767.0f;
    if (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE)
      ly = (float)raw_ly / 32767.0f;
    if (raw_rx > STICK_DEADZONE || raw_rx < -STICK_DEADZONE)
      rx = (float)raw_rx / 32767.0f;
    if (raw_ry > STICK_DEADZONE || raw_ry < -STICK_DEADZONE)
      ry = (float)raw_ry / 32767.0f;

    // Triggers analógicos
    float lt = (float)SDL_GameControllerGetAxis(
                   g_gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) /
               32767.0f;
    float rt = (float)SDL_GameControllerGetAxis(
                   g_gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) /
               32767.0f;

    // Send joystick event only when values change
    if (lx != g_last_lx || ly != g_last_ly ||
        rx != g_last_rx || ry != g_last_ry ||
        lt != g_last_lt || rt != g_last_rt || g_motion_dirty) {
      /* COI: stick esq vira direção canônica em coi_send_dir() (não mandar
       * eixo cru pro engine — trava em diagonal, ver comentário lá) */
      g_coi_slx = lx;
      g_coi_sly = ly;
      pb_send_motion(lx, ly, rx, ry, g_hat_x, g_hat_y, lt, rt);
      g_last_lx = lx;
      g_last_ly = ly;
      g_last_rx = rx;
      g_last_ry = ry;
      g_last_lt = lt;
      g_last_rt = rt;
      g_motion_dirty = 0;
    }

    // Also update virtual cursor for touch simulation
    if (lx != 0 || ly != 0) {
      g_cursor_x += lx * CURSOR_SPEED;
      g_cursor_y += ly * CURSOR_SPEED;
      if (g_cursor_x < 0)
        g_cursor_x = 0;
      if (g_cursor_x >= SCREEN_WIDTH)
        g_cursor_x = SCREEN_WIDTH - 1;
      if (g_cursor_y < 0)
        g_cursor_y = 0;
      if (g_cursor_y >= SCREEN_HEIGHT)
        g_cursor_y = SCREEN_HEIGHT - 1;
    }
  }

  /* COI: direção 8-way (dpad+stick+gptk) -> evento joystick, todo frame */
  coi_send_dir();
}

/* ---- ALooper ---- */

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  static int fake_looper;
  return (ALooper *)&fake_looper;
}

void ALooper_addFd(void *looper, int fd, int ident, int events,
                   void *callback, void *data) {
  (void)looper;
  (void)fd;
  (void)ident;
  (void)events;
  (void)callback;
  (void)data;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
  (void)outFd;
  (void)outEvents;

  // Fire pending audio callbacks first (before any blocking)
  opensles_shim_pump_callbacks();

  // Check for pending commands on the pipe (don't block long)
  struct pollfd pfd;
  pfd.fd = g_app.msgread;
  pfd.events = POLLIN;
  pfd.revents = 0;

  // Cap poll timeout to 5ms to keep audio flowing
  int timeout = timeoutMillis;
  if (timeout < 0 || timeout > 5)
    timeout = 5;

  void coi_tap_inject(void);
  coi_tap_inject();  /* COI: tap via /dev/shm/coi_tap todo frame (independe do queue) */
  static unsigned s_pc = 0; int s_dbg = ((s_pc++ % 120) == 0);
  int ret = poll(&pfd, 1, timeout);
  if (ret > 0 && (pfd.revents & POLLIN)) {
    if (outData)
      *outData = &g_app.cmdPollSource;
    if (s_dbg) debugPrintf("pollAll: -> LOOPER_ID_MAIN (pipe)\n");
    return LOOPER_ID_MAIN;
  }

  // Only poll SDL when input queue is empty (avoids flooding)
  if (input_queue_count() == 0) {
    process_sdl_events();
  }

  // If there are input events queued, return LOOPER_ID_INPUT
  if (input_queue_count() > 0) {
    if (outData)
      *outData = &g_app.inputPollSource;
    if (s_dbg) debugPrintf("pollAll: -> LOOPER_ID_INPUT (q=%d)\n", input_queue_count());
    return LOOPER_ID_INPUT;
  }

  // Fire audio callbacks again after poll
  opensles_shim_pump_callbacks();

  if (s_dbg) debugPrintf("pollAll: -> -1  app: state=%d win=%p [65]=%d [67]=%d\n",
                         g_app.activityState, (void*)g_app.window,
                         (int)((unsigned char*)&g_app)[65], (int)((unsigned char*)&g_app)[67]);
  return -1; // no events
}

/* ---- AInputQueue ---- */

void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data) {
  (void)queue;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
}

void AInputQueue_detachLooper(void *queue) { (void)queue; }

int AInputQueue_getEvent(void *queue, AInputEvent **outEvent) {
  (void)queue;
  FakeInputEvent *ev = input_queue_pop();
  if (!ev) {
    if (outEvent)
      *outEvent = NULL;
    return -1; // no events
  }
  g_current_event = ev;
  if (outEvent)
    *outEvent = (AInputEvent *)ev;
  return 0; // success
}

int AInputQueue_preDispatchEvent(void *queue, void *event) {
  (void)queue;
  (void)event;
  return 0; // don't consume
}

void AInputQueue_finishEvent(void *queue, void *event, int handled) {
  (void)queue;
  (void)event;
  (void)handled;
  g_current_event = NULL;
}

/* ---- AInputEvent getters ---- */

int AInputEvent_getType(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->type;
}

int AKeyEvent_getAction(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->action;
}

int AKeyEvent_getKeyCode(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  debugPrintf("[eng-read] AKeyEvent_getKeyCode -> %d\n", ev->keycode);
  return ev->keycode;
}

float AMotionEvent_getX(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->x;
}

float AMotionEvent_getY(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->y;
}

int AMotionEvent_getAction(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->action;
}

int AMotionEvent_getPointerCount(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->pointer_count;
}

int AMotionEvent_getPointerId(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->pointer_id;
}

float AMotionEvent_getAxisValue(void *event, int axis, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  if (axis >= 0 && axis < AMOTION_EVENT_AXIS_MAX)
    return ev->axes[axis];
  return 0.0f;
}

int AInputEvent_getSource(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->source;
}

/* ---- AConfiguration stubs ---- */

static int g_fake_config = 0;

AConfiguration *AConfiguration_new(void) {
  return (AConfiguration *)&g_fake_config;
}

void AConfiguration_delete(void *config) { (void)config; }

void AConfiguration_fromAssetManager(void *config, void *assetManager) {
  (void)config;
  (void)assetManager;
}

void AConfiguration_setLocale(void *config, const char *locale) {
  (void)config;
  (void)locale;
}

/* Idioma real: a engine (oz::Locale) le' o idioma pela AConfiguration nativa,
 * nao pelo getDeviceLanguage do JNI. O launcher exporta NXPORT_LANGUAGE
 * (GAME_LANGUAGE). Aceitos: en/fr/it/de/es (EFIGS do Castle); japones NUNCA;
 * auto/vazio/desconhecido -> en. */
static const char *coi_config_lang(void) {
  static char code[3] = "en";
  static int done = 0;
  if (!done) {
    done = 1;
    const char *l = getenv("NXPORT_LANGUAGE");
    static const char *ok[] = { "en", "fr", "it", "de", "es", NULL };
    if (l && l[0] && l[1]) {
      for (int i = 0; ok[i]; i++)
        if (l[0] == ok[i][0] && l[1] == ok[i][1]) { code[0] = ok[i][0]; code[1] = ok[i][1]; break; }
    }
    debugPrintf("[coi/idioma] AConfiguration lang=%c%c (NXPORT_LANGUAGE=%s)\n",
                code[0], code[1], (l && *l) ? l : "(vazio)");
  }
  return code;
}

int AConfiguration_getLanguage(void *config, char *outLanguage) {
  (void)config;
  const char *c = coi_config_lang();
  if (outLanguage) {
    outLanguage[0] = c[0];
    outLanguage[1] = c[1];
  }
  return 2;
}

int AConfiguration_getCountry(void *config, char *outCountry) {
  (void)config;
  if (outCountry) {
    outCountry[0] = 'U';
    outCountry[1] = 'S';
  }
  return 2;
}

int AConfiguration_getDensity(void *config) {
  (void)config;
  return 240; // ACONFIGURATION_DENSITY_HIGH (hdpi)
}

int AConfiguration_getOrientation(void *config) {
  (void)config;
  return 2; // ACONFIGURATION_ORIENTATION_LAND
}

void AConfiguration_setOrientation(void *config, int orientation) {
  (void)config;
  (void)orientation;
}

int AConfiguration_getScreenSize(void *config) {
  (void)config;
  return 3; // ACONFIGURATION_SCREENSIZE_LARGE
}

/* ---- ASensorManager stubs ---- */

ASensorManager *ASensorManager_getInstance(void) {
  static int fake_sensor_mgr;
  return (ASensorManager *)&fake_sensor_mgr;
}

void *ASensorManager_getDefaultSensor(void *manager, int type) {
  (void)manager;
  (void)type;
  return NULL;
}

ASensorEventQueue *ASensorManager_createEventQueue(void *manager,
                                                    void *looper, int ident,
                                                    void *callback,
                                                    void *data) {
  (void)manager;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
  static int fake_event_queue;
  return (ASensorEventQueue *)&fake_event_queue;
}

int ASensorEventQueue_enableSensor(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}

int ASensorEventQueue_setEventRate(void *queue, void *sensor,
                                   int32_t usec) {
  (void)queue;
  (void)sensor;
  (void)usec;
  return 0;
}

/* ---- ANativeActivity stubs ---- */

/* SAIDA PELA PROPRIA ENGINE (licao Horizon v1.0.2): num host nativo nao existe
 * activity Java, entao `ANativeActivity_finish` — o que a engine chama quando o
 * jogador escolhe "Quit"/"Exit" no menu — nao fecha nada sozinho: so' levanta
 * uma flag que o loop do jogo pode demorar (ou nunca) observar, e o processo
 * fica vivo com a musica tocando. Todo caminho de saida cai no MESMO shutdown
 * do SELECT+START: pause -> save -> sair. */
void ANativeActivity_finish(void *activity) {
  (void)activity;
  logPrintf("ANativeActivity_finish called (engine quit)\n");
  g_app.destroyRequested = 1;
  coi_begin_shutdown("quit da engine (ANativeActivity_finish)");
}

/* ---- android_app command processing (GameActivity glue) ----
 * O glue ESTÁTICO do jogo expõe android_app_pre_exec_cmd/post_exec_cmd:
 * pre_exec(INIT_WINDOW) faz window=pendingWindow, broadcast cond, seta a flag
 * (app+92) que o loop do android_main espera. Resolvidos em android_shim_init. */
void (*g_app_pre_exec_cmd)(struct android_app *, int) = NULL;
void (*g_app_post_exec_cmd)(struct android_app *, int) = NULL;

static void process_cmd(struct android_app *app,
                        struct android_poll_source *source) {
  (void)source;
  int8_t cmd;
  if (read(app->msgread, &cmd, sizeof(cmd)) == sizeof(cmd)) {
    debugPrintf("android_shim: process_cmd cmd=%d\n", (int)cmd);
    if (g_app_pre_exec_cmd) g_app_pre_exec_cmd(app, cmd);
    if (app->onAppCmd) app->onAppCmd(app, cmd);
    if (g_app_post_exec_cmd) g_app_post_exec_cmd(app, cmd);
  }
}

/* ---- Input processing (called by game via inputPollSource.process) ---- */

static void process_input(struct android_app *app,
                          struct android_poll_source *source) {
  (void)source;
  AInputEvent *event = NULL;
  while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
    if (AInputQueue_preDispatchEvent(app->inputQueue, event))
      continue;
    int handled = 0;
    { static int once=0; if(!once){once=1;
        debugPrintf("[inp] onInputEvent=%p (game module?)\n", (void*)app->onInputEvent); } }
    if (app->onInputEvent) {
      handled = app->onInputEvent(app, event);
      FakeInputEvent *fe = (FakeInputEvent *)event;
      if (fe->type == AINPUT_EVENT_TYPE_KEY) {
        debugPrintf("android_shim: KEY type=%d action=%d keycode=%d handled=%d\n",
                    fe->type, fe->action, fe->keycode, handled);
      } else if (fe->type == AINPUT_EVENT_TYPE_MOTION) {
        debugPrintf("android_shim: MOTION action=%d x=%.0f y=%.0f handled=%d\n",
                    fe->action, fe->x, fe->y, handled);
      }
    }
    AInputQueue_finishEvent(app->inputQueue, event, handled);
  }
}

/* ---- Public API ---- */

struct android_app *android_shim_init(void) {
  debugPrintf("android_shim: Initializing fake Android environment\n");

  memset(&g_app, 0, sizeof(g_app));
  memset(&g_activity, 0, sizeof(g_activity));
  memset(&g_callbacks, 0, sizeof(g_callbacks));

  // Create command pipe
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    fatal_error("android_shim: Failed to create pipe");
  }
  g_app.msgread = pipefd[0];
  g_app.msgwrite = pipefd[1];

  // Setup JNI
  void *fake_vm = NULL;
  void *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);

  // Setup activity
  g_activity.callbacks = &g_callbacks;
  g_activity.vm = fake_vm;
  g_activity.env = fake_env;
  g_activity.sdkVersion = 24; // Android 7.0
  /* Save/config do jogo em ./userdata, NUNCA em ./gamedata.
   * `gamedata/` e' a caixa de ENTRADA do usuario (o APK e o OBB que ele
   * fornece) e o instalador varre essa pasta. Misturar as duas coisas faria o
   * save do jogador conviver com arquivos que a documentacao manda apagar
   * depois da instalacao para liberar espaco no cartao — ou seja, um dia
   * alguem apagaria o proprio save junto. Direcoes separadas, riscos
   * separados. O launcher cria ./userdata antes de iniciar. */
  g_activity.internalDataPath = "./userdata";
  g_activity.externalDataPath = "./userdata";
  g_activity.obbPath = ".";

  // Setup app
  g_app.activity = &g_activity;
  g_app.config = AConfiguration_new();
  g_app.looper = ALooper_prepare(0);
  // GameActivity: o glue copia pendingWindow -> window no APP_CMD_INIT_WINDOW.
  g_app.pendingWindow = (ANativeWindow *)&g_fake_native_window;
  g_app.window = (ANativeWindow *)&g_fake_native_window;
  g_app.inputQueue = (AInputQueue *)&g_fake_input_queue;

  // Resolve os helpers do glue GameActivity (estáticos no libNativeGame).
  extern void (*g_app_pre_exec_cmd)(struct android_app *, int);
  extern void (*g_app_post_exec_cmd)(struct android_app *, int);
  g_app_pre_exec_cmd  = (void (*)(struct android_app *, int))so_find_addr("android_app_pre_exec_cmd");
  g_app_post_exec_cmd = (void (*)(struct android_app *, int))so_find_addr("android_app_post_exec_cmd");
  debugPrintf("android_shim: glue pre_exec=%p post_exec=%p\n",
              (void *)g_app_pre_exec_cmd, (void *)g_app_post_exec_cmd);

  // Command poll source
  g_app.cmdPollSource.id = LOOPER_ID_MAIN;
  g_app.cmdPollSource.app = &g_app;
  g_app.cmdPollSource.process = process_cmd;

  // Input poll source
  g_app.inputPollSource.id = LOOPER_ID_INPUT;
  g_app.inputPollSource.app = &g_app;
  g_app.inputPollSource.process = process_input;

  /* ANTES do SDL_Init: o SDL le SDL_GAMECONTROLLERCONFIG aqui dentro, e uma
   * entrada vinda desse env vence qualquer SDL_GameControllerAddMapping feito
   * depois. Como o mapping do CFW e' autorado em ROTULO, corrigir a tabela do
   * SDL mais tarde nao adianta — tem que ser o env, e tem que ser agora. */
  pad_positional_fix_env("COI");

  /* VIDEO e AUDIO inicializam INDEPENDENTES (licao TASM2 v1.1.7, herdada por
   * Horizon e Oceanhorn). Um PulseAudio HERDADO e morto — daemon do frontend
   * que caiu, socket orfao em XDG_RUNTIME_DIR — faz o backend de audio do SDL
   * falhar; num SDL_Init unico isso derruba VIDEO junto e o port vira "abre e
   * volta pro menu". Video primeiro e sozinho: se ele falhar, ai sim nao ha
   * jogo. Audio depois, e a falha dele nunca e' fatal — o jogo roda mudo, o
   * usuario ve o motivo no log e ainda pode jogar. */
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
    fatal_error("android_shim: SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
  }
  logPrintf("android_shim: SDL video/gamecontroller OK (driver=%s)\n",
              SDL_GetCurrentVideoDriver());

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    logPrintf("android_shim: audio failed on the default backend: %s\n",
                SDL_GetError());
    /* Retry EXPLICITO em ALSA depois de uma falha REAL — nunca antes (forcar
     * SDL_AUDIODRIVER de saida e' proibido: quebra firmware que so tem
     * pipewire/pulse vivo). Restaura o env depois para nao contaminar o resto
     * do processo nem as libs do jogo. */
    const char *prev = getenv("SDL_AUDIODRIVER");
    char saved[64];
    saved[0] = 0;
    if (prev) snprintf(saved, sizeof(saved), "%s", prev);
    setenv("SDL_AUDIODRIVER", "alsa", 1);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
      logPrintf("android_shim: audio unavailable on ALSA too (%s) — "
                  "seguindo SEM som; video e controle continuam\n",
                  SDL_GetError());
    } else {
      logPrintf("android_shim: audio recovered on the ALSA retry\n");
    }
    if (saved[0]) setenv("SDL_AUDIODRIVER", saved, 1);
    else unsetenv("SDL_AUDIODRIVER");
  }
  logPrintf("android_shim: SDL initialized (audio=%s)\n",
              SDL_WasInit(SDL_INIT_AUDIO) ? (SDL_GetCurrentAudioDriver()
                                                 ? SDL_GetCurrentAudioDriver()
                                                 : "ativo")
                                          : "ausente");

  // Try to open a gamepad early
  init_gamecontroller();

  debugPrintf("android_shim: Fake android_app ready at %p\n", &g_app);
  return &g_app;
}

void android_shim_send_cmd(struct android_app *app, int8_t cmd) {
  if (write(app->msgwrite, &cmd, sizeof(cmd)) != sizeof(cmd)) {
    debugPrintf("android_shim: Failed to write command %d\n", cmd);
  }
}

ANativeWindow *android_shim_get_window(void) {
  return (ANativeWindow *)&g_fake_native_window;
}

void android_shim_cleanup(void) {
  debugPrintf("android_shim: Cleaning up\n");
  if (g_app.msgread >= 0)
    close(g_app.msgread);
  if (g_app.msgwrite >= 0)
    close(g_app.msgwrite);

  if (g_gamecontroller) {
    SDL_GameControllerClose(g_gamecontroller);
    g_gamecontroller = NULL;
  }

  if (g_sdl_window) {
    SDL_DestroyWindow(g_sdl_window);
    g_sdl_window = NULL;
  }
  SDL_Quit();
}
