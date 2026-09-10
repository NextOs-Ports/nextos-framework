#define _GNU_SOURCE
/*
 * android_shim.c -- fake Android NDK for Linux ARM64
 *
 * Implements enough of the android_native_app_glue + Android NDK
 * to let the game library's android_main() run on Linux.
 *
 * Input handling:
 *   SDL gamepad events are converted to fake AInputEvent structs
 *   (key events for buttons and native joystick-axis motion events).
 *   The game's onInputEvent callback receives them through the
 *   standard AInputQueue_getEvent flow.
 */

#include <dlfcn.h>
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
#include "pad_ordinal_fix.h"
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

// Last sent joystick axis values (to avoid flooding)
static float g_last_lx = 0, g_last_ly = 0, g_last_rx = 0, g_last_ry = 0;

// SDL gamepad
static SDL_GameController *g_gamecontroller = NULL;

/* ---- Globals ---- */
static struct android_app g_app;
static struct android_app *g_runtime_app;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static SDL_Window *g_sdl_window = NULL;

// Fake window handle - we just use a pointer to distinguish it from NULL
static int g_fake_native_window = 1;

// Fake input queue handle
static int g_fake_input_queue = 1;

static struct android_app *active_app(void) {
  return g_runtime_app ? g_runtime_app : &g_app;
}

static int as_input_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *v = getenv("AS_INPUT_TRACE");
    enabled = (v && strcmp(v, "0") != 0) ? 1 : 0;
  }
  return enabled;
}

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
  if (as_input_trace_enabled())
    logPrintf("[AS-INPUT] key action=%d keycode=%d\n", action, keycode);
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

static void push_joystick_event(float lx, float ly, float rx, float ry,
                                float hx, float hy, float lt, float rt) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.pointer_count = 1;
  /* Mapa lido do EngineHandleJoystickInput do Action Squad:
   *   X/Y -> eixos internos 0/1 (movimento)
   *   Z/RZ -> 2/3 (stick direito)
   *   BRAKE/GAS -> 4/5 (gatilhos esquerdo/direito)
   * HAT_X/Y e RX/RY são os fallbacks nativos da mesma rotina. x/y também
   * ficam preenchidos porque AMotionEvent_getX/Y devem refletir AXIS_X/Y. */
  ev.x = lx;
  ev.y = ly;
  ev.axes[AMOTION_EVENT_AXIS_X] = lx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = ly;
  ev.axes[AMOTION_EVENT_AXIS_Z] = rx;
  ev.axes[AMOTION_EVENT_AXIS_RZ] = ry;
  ev.axes[AMOTION_EVENT_AXIS_HAT_X] = hx;
  ev.axes[AMOTION_EVENT_AXIS_HAT_Y] = hy;
  /* 17/18 são mantidos para a superfície Android genérica; este jogo lê
   * explicitamente 23/22, exatamente como o binário Android original. */
  ev.axes[AMOTION_EVENT_AXIS_LTRIGGER] = lt;
  ev.axes[AMOTION_EVENT_AXIS_RTRIGGER] = rt;
  ev.axes[AMOTION_EVENT_AXIS_BRAKE] = lt;
  ev.axes[AMOTION_EVENT_AXIS_GAS] = rt;
  input_queue_push(&ev);
  if (as_input_trace_enabled())
    logPrintf("[AS-INPUT] motion L=(%.3f,%.3f) R=(%.3f,%.3f) "
              "HAT=(%.0f,%.0f) BRAKE=%.3f GAS=%.3f\n",
              lx, ly, rx, ry, hx, hy, lt, rt);
}

/* ---- SDL button → Android keycode mapping ---- */

/* Mapa extraído do jump-table de EngineHandleJoystickInput do próprio
 * libAndroidEntryPoint.so. O d-pad continua por HAT_X/HAT_Y: os keycodes
 * Android 19..22 estão rotacionados nesta versão da engine. */
static int sdl_button_to_keycode(int sdl_button) {
  switch (sdl_button) {
  case SDL_CONTROLLER_BUTTON_A:
    return AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_B:
    return AKEYCODE_BUTTON_B;
  case SDL_CONTROLLER_BUTTON_X:
    return AKEYCODE_BUTTON_X;
  case SDL_CONTROLLER_BUTTON_Y:
    return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_BACK:
    return AKEYCODE_BUTTON_SELECT;
  case SDL_CONTROLLER_BUTTON_START:
    return AKEYCODE_BUTTON_START;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return AKEYCODE_BUTTON_R1;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return AKEYCODE_BUTTON_THUMBL;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return AKEYCODE_BUTTON_THUMBR;
  /* D-pad: HAT_X/HAT_Y, sem os keycodes 19..22 rotacionados do upstream. */
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
    /* ANTES do SDL_IsGameController: primeiro corrige kernels antigos que
     * expõem botões apenas pela ordem HID (BTN_C/BTN_Z); depois corrige rótulos
     * Nintendo em kernels modernos com códigos posicionais semânticos. Os
     * detectores são mutuamente exclusivos. */
    pad_ordinal_fix_apply(i, "AS");
    pad_positional_fix_apply(i, "AS");
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
    char guid_string[64];
    SDL_JoystickGetGUIDString(guid, guid_string, sizeof(guid_string));
    logPrintf("android_shim: gamepad candidate %d name=\"%s\" guid=%s "
              "mapped=%d\n", i,
              SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "",
              guid_string, SDL_IsGameController(i));
    /* SDL_GameControllerMappingForDeviceIndex is SDL 2.0.9; the public ELF
     * keeps its floor at 2.0.4, so the diagnostic mapping dump is resolved at
     * runtime and simply absent on older firmware. */
    {
      typedef char *(*mapping_for_index_fn)(int);
      static mapping_for_index_fn mapping_for_index;
      static int resolved;
      if (!resolved) {
        resolved = 1;
        mapping_for_index = (mapping_for_index_fn)dlsym(
            RTLD_DEFAULT, "SDL_GameControllerMappingForDeviceIndex");
      }
      if (mapping_for_index) {
        char *mapping = mapping_for_index(i);
        if (mapping) {
          logPrintf("android_shim: candidate mapping: %s\n", mapping);
          SDL_free(mapping);
        }
      }
    }
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

/* ---- Action Squad: identidade do pad p/ SetHaveController ----
 * A engine so' liga o joystick depois que recebe count/name/deviceId; o nome
 * decide o layout de icones (GetControllerType).  Ver MEDIDAS.md. */
const char *android_shim_get_gamepad_name(void) {
  init_gamecontroller();
  if (g_gamecontroller)
    return SDL_GameControllerName(g_gamecontroller);
  int num = SDL_NumJoysticks();
  return num > 0 ? SDL_JoystickNameForIndex(0) : NULL;
}

int android_shim_get_gamepad_id(void) {
  if (g_gamecontroller) {
    SDL_Joystick *js = SDL_GameControllerGetJoystick(g_gamecontroller);
    if (js)
      return (int)SDL_JoystickInstanceID(js);
  }
  return 0;
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
      debugPrintf("android_shim: Paddleboat exports not found\n");
      pb_isInitialized = NULL;
      return;
    }
  }
  if (!pb_isInitialized()) return; /* engine ainda não rodou Paddleboat_init */

  /* axisBits: sticks, HAT e as duas nomenclaturas Android dos gatilhos. */
  static const int32_t info[7] = {
      PB_DEVICE_ID, 0x0810, 0x0001,
      (1 << 0) | (1 << 1) | (1 << 11) | (1 << 14) | (1 << 15) | (1 << 16) |
          (1 << 17) | (1 << 18) | (1 << 22) | (1 << 23),
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
  ev.pointers[0].axisValues[23] = lt;  /* AXIS_BRAKE (Action Squad L2) */
  ev.pointers[0].axisValues[22] = rt;  /* AXIS_GAS (Action Squad R2/FIRE) */
  pb_processMotion(&ev);
}

/* ---- Canal do MENU (CControllersManager) --------------------------------
 * Causa-raiz do "menu morto" no Knulli/CubeXX (relato bbilford83): a UI de
 * foco (layer SWITCHCONTROL etc.) NAO escuta Paddleboat — ela consome
 * SendControllerButton/SendControllerAxis -> CControllersManager::Receive*.
 * Nossa ponte so alimentava o Paddleboat (gameplay), entao o menu nunca via
 * botao nenhum.  Mapa extraido do proprio EngineHandleJoystickInput do jogo
 * (jump table keycode->ID interno, .rodata 0x3e9814):
 *   A=0  B=1  X=2  Y=3  SELECT=4  START=6  L1|THUMBL=9  R1|THUMBR=10
 *   eixo 0 = dpad vertical (UP=-1)   eixo 1 = dpad horizontal (LEFT=-1)
 *   eixo 4 = L2                      eixo 5 = R2 */
static void (*ui_send_button)(int, int);
static void (*ui_send_axis)(float, unsigned char);

static void ui_try_resolve(void) {
  static int tried = 0;
  if (tried) return;
  tried = 1;
  ui_send_button =
      (void (*)(int, int))so_find_addr_safe("_Z20SendControllerButtonbi");
  ui_send_axis =
      (void (*)(float, unsigned char))so_find_addr_safe(
          "_Z18SendControllerAxisfh");
  debugPrintf("android_shim: canal do menu (SendControllerButton) %s\n",
              (ui_send_button && ui_send_axis) ? "resolvido" : "AUSENTE");
}

static int keycode_to_ui_id(int kc) {
  switch (kc) {
  case AKEYCODE_BUTTON_A: return 0;
  case AKEYCODE_BUTTON_B: return 1;
  case AKEYCODE_BUTTON_X: return 2;
  case AKEYCODE_BUTTON_Y: return 3;
  case AKEYCODE_BUTTON_SELECT: return 4;
  case AKEYCODE_BUTTON_START: return 6;
  case AKEYCODE_BUTTON_L1: case AKEYCODE_BUTTON_THUMBL: return 9;
  case AKEYCODE_BUTTON_R1: case AKEYCODE_BUTTON_THUMBR: return 10;
  default: return -1;
  }
}

static void ui_send_key(int action, int kc) {
  ui_try_resolve();
  if (!ui_send_button) return;
  int id = keycode_to_ui_id(kc);
  if (id >= 0) ui_send_button(action == AKEY_EVENT_ACTION_DOWN ? 1 : 0, id);
}

static void ui_send_dpad(float hx, float hy) {
  ui_try_resolve();
  if (!ui_send_axis) return;
  ui_send_axis(hy, 0); /* vertical: UP=-1 */
  ui_send_axis(hx, 1); /* horizontal: LEFT=-1 */
}

/* ---- Process SDL events into input queue ---- */

#define STICK_DEADZONE 8000

static float normalize_stick_axis(int raw) {
  if (raw > -STICK_DEADZONE && raw < STICK_DEADZONE)
    return 0.0f;
  if (raw <= -32768)
    return -1.0f;
  return (float)raw / 32767.0f;
}

static float normalize_trigger_axis(int raw) {
  /* SDL_GameController normaliza gatilhos para 0..32767. Alguns drivers
   * antigos ainda entregam repouso negativo; nunca deixe isso virar comando. */
  if (raw <= 0)
    return 0.0f;
  if (raw >= 32767)
    return 1.0f;
  return (float)raw / 32767.0f;
}

static float g_hat_x = 0, g_hat_y = 0;
static float g_last_lt = 0, g_last_rt = 0;

static void update_hat_from_dpad(int button, int down) {
  static int left = 0, right = 0, up = 0, down_state = 0;
  switch (button) {
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  left = down; break;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: right = down; break;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:    up = down; break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  down_state = down; break;
  default: return;
  }
  g_hat_x = (float)(right - left);
  g_hat_y = (float)(down_state - up);
}

/* modo gptokeyb opcional (AS_INPUT=gptk): botões vêm do TECLADO
 * (uinput do gptokeyb via um .gptk); botões nativos do pad são ignorados
 * (duplicariam). Eixos analógicos continuam nativos quando o pad é visível. */
static int gptk_on(void) {
  static int g = -1;
  if (g < 0) {
    const char *ie = getenv("AS_INPUT");
    if (!ie)
      ie = getenv("COI_INPUT"); /* compatibilidade de bancada antiga */
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
  android_shim_send_cmd(active_app(), APP_CMD_LOST_FOCUS);
  android_shim_send_cmd(active_app(), APP_CMD_SAVE_STATE);
  android_shim_send_cmd(active_app(), APP_CMD_PAUSE);
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

/* ---- Ponte estrita do splash touch-only ----
 * g_gameState=4 e' GAME_STATE_SPLASH no enum do próprio jogo. Nesse estado o
 * SetHaveController ainda e' recusado, embora a tela espere "tap". O primeiro
 * A/confirmar envia também UM toque central; fora desse estado nenhum botão é
 * convertido em touch. Assim L1 e todos os demais comandos ficam 100% nativos. */
static int g_synth_tap_hold = 0;
static float g_synth_tap_x = 0, g_synth_tap_y = 0;
static int g_controller_confirm_pending = 0;
static int g_controller_confirm_retry_delay = 0;
static int g_controller_confirm_auto_delay = 0; /* v1.0.3: confirmar SOZINHO */
static int g_suppress_confirm_a_up = 0;

void android_shim_expect_controller_confirmation(void) {
  /* SetHaveController creates this touch-first Android dialog synchronously.
   * Arm only after that native call, so ordinary menu A presses never become
   * coordinates. */
  g_controller_confirm_pending = 1;
  g_controller_confirm_retry_delay = 0;
  /* v1.0.3 (pedido de campo): o dialogo "Controller detected" e touch-first e
   * confundia todo mundo. O proprio port confirma ~0,5s depois de o jogo
   * criar o dialogo; apertar A antes disso continua funcionando igual. */
  g_controller_confirm_auto_delay = 30;
}

static void as_touch_begin(float x, float y, const char *why) {
  if (g_synth_tap_hold > 0) return;
  g_synth_tap_x = x;
  g_synth_tap_y = y;
  push_motion_event(5, x, y); /* POINTER_DOWN */
  g_synth_tap_hold = 3;
  logPrintf("android_shim: %s -> one native touch at %.0f,%.0f\n", why, x,
            y);
}

/* Alvo vertical do botao Confirm do dialogo "Controller detected".  A UI e'
 * desenhada para 1280x720 (16:9): ali o Confirm fica em y=250/720.  Numa tela
 * NAO-16:9 (RG CubeXX 1:1 720x720, RG34XX-SP), a engine recentraliza o dialogo
 * e o Confirm passa a ficar logo ACIMA do centro da tela -- o alvo 250/720
 * (34,7%) erra o botao (era esse o bug de campo).  Para telas largas mantemos
 * exatamente o alvo calibrado (zero regressao nos devices ja provados); para
 * telas quadradas/altas miramos 46% da altura (acima do centro, longe do
 * Cancel que fica abaixo).  coi_screen_w/h sao a resolucao REAL do device. */
static float as_confirm_y_frac(void) {
  float aspect =
      (coi_screen_h > 0) ? (float)coi_screen_w / (float)coi_screen_h : 1.777f;
  if (aspect >= 1.2f)
    return 250.0f / 720.0f; /* 16:9 / 4:3 provados: inalterado */
  return 0.46f;             /* quadrada/tall: Confirm acima do centro */
}

static void as_splash_tap_begin(void) {
  as_touch_begin((float)coi_screen_w * 0.5f,
                 (float)coi_screen_h * 0.5f, "splash confirm");
}
static void as_splash_tap_pump(void) {
  if (g_synth_tap_hold > 0 && --g_synth_tap_hold == 0)
    push_motion_event(6, g_synth_tap_x, g_synth_tap_y); /* POINTER_UP */
  if (g_controller_confirm_pending && g_controller_confirm_auto_delay > 0 &&
      --g_controller_confirm_auto_delay == 0) {
    g_controller_confirm_pending = 0;
    g_controller_confirm_retry_delay = 10;
    as_touch_begin((float)coi_screen_w * 0.5f,
                   (float)coi_screen_h * as_confirm_y_frac(),
                   "controller dialog AUTO confirm");
  }
  /* O mesmo A tambem chega ao caminho de tecla do popup. Algumas vezes o
   * primeiro POINTER_DOWN coincide com esse edge e e' descartado. Repetir o
   * toque uma vez, depois do key-up, preserva uma unica acao do usuario e cai
   * numa area vazia do menu caso o primeiro ja tenha confirmado. */
  if (g_synth_tap_hold == 0 && g_controller_confirm_retry_delay > 0 &&
      --g_controller_confirm_retry_delay == 0)
    as_touch_begin((float)coi_screen_w * 0.5f,
                   (float)coi_screen_h * as_confirm_y_frac(),
                   "controller dialog confirm retry");
}

static int as_maybe_confirm_splash(int action, int keycode) {
  /* Não trave para sempre depois da primeira tentativa: g_gameState já vale
   * SPLASH durante a animação anterior ao texto "Tap to play". Se o jogador
   * apertava A cedo, o toque era legitimamente ignorado pela UI e o latch
   * antigo impedia qualquer nova tentativa. Cada novo DOWN de A pode emitir
   * um único tap enquanto continuarmos no splash; a transição de estado é a
   * autoridade que desliga esta ponte. */
  if (keycode != AKEYCODE_BUTTON_A)
    return 0;
  if (action == AKEY_EVENT_ACTION_UP && g_suppress_confirm_a_up) {
    g_suppress_confirm_a_up = 0;
    return 1;
  }
  if (action != AKEY_EVENT_ACTION_DOWN)
    return 0;
  if (g_controller_confirm_pending) {
    g_controller_confirm_pending = 0;
    g_controller_confirm_retry_delay = 10;
    g_suppress_confirm_a_up = 1;
    /* Confirm is centered at y=250 on the game's 1280x720 reference UI. */
    as_touch_begin((float)coi_screen_w * 0.5f,
                   (float)coi_screen_h * as_confirm_y_frac(),
                   "controller dialog confirm");
    return 1;
  }
  static int *game_state = NULL;
  static int looked_up = 0;
  if (!looked_up) {
    looked_up = 1;
    game_state = (int *)so_find_addr_safe("g_gameState");
  }
  if (game_state && *game_state == 4) { /* GAME_STATE_SPLASH */
    g_suppress_confirm_a_up = 1;
    as_splash_tap_begin();
    return 1;
  }
  return 0;
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
    if (!as_maybe_confirm_splash(AKEY_EVENT_ACTION_UP, kkc))
      push_key_event(AKEY_EVENT_ACTION_UP, kkc);
    debugPrintf("[key] %d up\n", kkc);
    kkc = 0;
  }
  if (++chk % 6) return;
  /* tecla via /dev/shm/coi_key: A=96 B=97 X=99 Y=100 L2=104 R2=105
   * START=108 SELECT=109. */
  FILE *k = fopen("/dev/shm/coi_key", "r");
  if (k) {
    int kc = 0;
    if (fscanf(k, "%d", &kc) == 1 && kc) {
      if (!as_maybe_confirm_splash(AKEY_EVENT_ACTION_DOWN, kc))
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
      push_joystick_event(dx, dy, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                          0.0f);
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
    android_shim_send_cmd(active_app(), APP_CMD_LOW_MEMORY);
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
  as_splash_tap_pump(); /* fecha o único toque do confirmar no splash */
  coi_tap_inject();     /* automação de bancada, gated e OFF por padrão */

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
  }

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_QUIT:
      active_app()->destroyRequested = 1;
      break;

    /* 🎮 modo GPTOKEYB (AS_INPUT=gptk): o gptokeyb
     * do CFW lê o controle físico e emite TECLADO via uinput conforme o
     * .gptk. Traduzimos as teclas para os mesmos eventos Android nativos:
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
      int kc = -1, stick = 0, dpadbtn = -1;
      switch (e.key.keysym.scancode) {
      case SDL_SCANCODE_X:      kc = AKEYCODE_BUTTON_A; break;
      case SDL_SCANCODE_C:      kc = AKEYCODE_BUTTON_B; break;
      case SDL_SCANCODE_Q:      kc = AKEYCODE_BUTTON_X; break;
      case SDL_SCANCODE_T:      kc = AKEYCODE_BUTTON_Y; break;
      case SDL_SCANCODE_RETURN: kc = AKEYCODE_BUTTON_START; kb_ent = dn; break;
      case SDL_SCANCODE_ESCAPE: kc = AKEYCODE_BUTTON_SELECT; kb_esc = dn; break;
      case SDL_SCANCODE_H:      kc = AKEYCODE_BUTTON_L1; break;
      case SDL_SCANCODE_J:      kc = AKEYCODE_BUTTON_R1; break;
      case SDL_SCANCODE_K:      kc = AKEYCODE_BUTTON_L2; break;
      case SDL_SCANCODE_L:      kc = AKEYCODE_BUTTON_R2; break;
      case SDL_SCANCODE_N:      kc = AKEYCODE_BUTTON_THUMBL; break;
      case SDL_SCANCODE_M:      kc = AKEYCODE_BUTTON_THUMBR; break;
      /* Setas seguem HAT_X/Y; os keycodes 19..22 do upstream são rotacionados. */
      case SDL_SCANCODE_UP:     dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_UP; stick = 1; break;
      case SDL_SCANCODE_DOWN:   dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_DOWN; stick = 1; break;
      case SDL_SCANCODE_LEFT:   dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_LEFT; stick = 1; break;
      case SDL_SCANCODE_RIGHT:  dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; stick = 1; break;
      case SDL_SCANCODE_W:      kb_w = dn; stick = 1; break;
      case SDL_SCANCODE_A:      kb_a = dn; stick = 1; break;
      case SDL_SCANCODE_S:      kb_s = dn; stick = 1; break;
      case SDL_SCANCODE_D:      kb_d = dn; stick = 1; break;
      default: break;
      }
      if (kb_esc && kb_ent) coi_begin_shutdown("SELECT+START (gptk)");
      if (kc >= 0) {
        if (!as_maybe_confirm_splash(act, kc)) {
          push_key_event(act, kc);
          pb_send_key(act, kc);
          ui_send_key(act, kc);
        }
      }
      if (dpadbtn >= 0) {
        update_hat_from_dpad(dpadbtn, dn);
        ui_send_dpad(g_hat_x, g_hat_y);
      }
      if (stick) {
        float lx = (kb_d ? 1.0f : 0.0f) - (kb_a ? 1.0f : 0.0f);
        float ly = (kb_s ? 1.0f : 0.0f) - (kb_w ? 1.0f : 0.0f);
        if (lx != 0.0f && ly != 0.0f) { lx *= 0.7071f; ly *= 0.7071f; }
        push_joystick_event(lx, ly, 0.0f, 0.0f, g_hat_x, g_hat_y,
                            0.0f, 0.0f);
        pb_send_motion(lx, ly, 0.0f, 0.0f, g_hat_x, g_hat_y, 0.0f,
                       0.0f);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONDOWN: {
      if (gptk_on()) break; /* botões vêm do teclado (gptokeyb) */
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        if (!as_maybe_confirm_splash(AKEY_EVENT_ACTION_DOWN, kc)) {
          push_key_event(AKEY_EVENT_ACTION_DOWN, kc);
          pb_send_key(AKEY_EVENT_ACTION_DOWN, kc);
          ui_send_key(AKEY_EVENT_ACTION_DOWN, kc);
        }
        debugPrintf("android_shim: button DOWN keycode=%d\n", kc);
      }
      /* D-pad via HAT e envio imediato: DOWN+UP no mesmo poll ainda produz
       * os dois estados e nunca desaparece entre dois frames lentos. */
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        update_hat_from_dpad(e.cbutton.button, 1);
        ui_send_dpad(g_hat_x, g_hat_y);
        push_joystick_event(g_last_lx, g_last_ly, g_last_rx, g_last_ry,
                            g_hat_x, g_hat_y, g_last_lt, g_last_rt);
        pb_send_motion(g_last_lx, g_last_ly, g_last_rx, g_last_ry, g_hat_x,
                       g_hat_y, g_last_lt, g_last_rt);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONUP: {
      if (gptk_on()) break; /* botões vêm do teclado (gptokeyb) */
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        if (!as_maybe_confirm_splash(AKEY_EVENT_ACTION_UP, kc)) {
          push_key_event(AKEY_EVENT_ACTION_UP, kc);
          pb_send_key(AKEY_EVENT_ACTION_UP, kc);
          ui_send_key(AKEY_EVENT_ACTION_UP, kc);
        }
      }
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        update_hat_from_dpad(e.cbutton.button, 0);
        ui_send_dpad(g_hat_x, g_hat_y);
        push_joystick_event(g_last_lx, g_last_ly, g_last_rx, g_last_ry,
                            g_hat_x, g_hat_y, g_last_lt, g_last_rt);
        pb_send_motion(g_last_lx, g_last_ly, g_last_rx, g_last_ry, g_hat_x,
                       g_hat_y, g_last_lt, g_last_rt);
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
      g_last_lx = g_last_ly = g_last_rx = g_last_ry = 0.0f;
      g_last_lt = g_last_rt = g_hat_x = g_hat_y = 0.0f;
      push_joystick_event(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                          0.0f);
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

    float lx = normalize_stick_axis(raw_lx);
    float ly = normalize_stick_axis(raw_ly);
    float rx = normalize_stick_axis(raw_rx);
    float ry = normalize_stick_axis(raw_ry);

    // Triggers analógicos
    float lt = normalize_trigger_axis(SDL_GameControllerGetAxis(
        g_gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    float rt = normalize_trigger_axis(SDL_GameControllerGetAxis(
        g_gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));

    // Send joystick event only when values change
    if (lx != g_last_lx || ly != g_last_ly ||
        rx != g_last_rx || ry != g_last_ry ||
        lt != g_last_lt || rt != g_last_rt) {
      /* Autoridade = AInputQueue nativa usada por EngineHandleInput. O caminho
       * Paddleboat permanece como espelho somente se a biblioteca o expuser. */
      push_joystick_event(lx, ly, rx, ry, g_hat_x, g_hat_y, lt, rt);
      pb_send_motion(lx, ly, rx, ry, g_hat_x, g_hat_y, lt, rt);
      g_last_lx = lx;
      g_last_ly = ly;
      g_last_rx = rx;
      g_last_ry = ry;
      g_last_lt = lt;
      g_last_rt = rt;
    }
  }
}

/* ---- ALooper ----
 *
 * O Action Squad traz o android_native_app_glue dentro da .so. A thread criada
 * por ANativeActivity_onCreate registra o pipe DELA em ALooper_addFd e espera
 * receber de volta exatamente o ident/data cadastrados. Portanto o looper não
 * pode sondar o pipe do scaffold do loader (erro herdado do caminho Castle):
 * ele precisa honrar os registros feitos pela própria glue do jogo.
 */
#define MAX_LOOPER_FDS 8
typedef int (*LooperCallback)(int fd, int events, void *data);
typedef struct {
  pthread_t owner;
  int refs;
  struct {
    int fd;
    int ident;
    int events;
    LooperCallback callback;
    void *data;
  } reg[MAX_LOOPER_FDS];
  int reg_count;
  AInputQueue *input_queue;
  int input_ident;
  LooperCallback input_callback;
  void *input_data;
} FakeLooper;

static _Thread_local FakeLooper *g_thread_looper;

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  if (!g_thread_looper) {
    g_thread_looper = calloc(1, sizeof(*g_thread_looper));
    if (!g_thread_looper)
      return NULL;
    g_thread_looper->owner = pthread_self();
    g_thread_looper->refs = 1;
    debugPrintf("android_shim: ALooper_prepare -> %p tid=%lx\n",
                (void *)g_thread_looper, (unsigned long)pthread_self());
  }
  return (ALooper *)g_thread_looper;
}

ALooper *ALooper_forThread(void) { return (ALooper *)g_thread_looper; }

void ALooper_acquire(ALooper *looper) {
  FakeLooper *l = (FakeLooper *)looper;
  if (l)
    l->refs++;
}

void ALooper_release(ALooper *looper) {
  FakeLooper *l = (FakeLooper *)looper;
  if (l && l->refs > 0)
    l->refs--;
}

int ALooper_addFd(void *looper, int fd, int ident, int events,
                  void *callback, void *data) {
  FakeLooper *l = (FakeLooper *)looper;
  if (!l || fd < 0)
    return -1;
  int slot = -1;
  for (int i = 0; i < l->reg_count; i++)
    if (l->reg[i].fd == fd)
      slot = i;
  if (slot < 0) {
    if (l->reg_count >= MAX_LOOPER_FDS)
      return -1;
    slot = l->reg_count++;
  }
  l->reg[slot].fd = fd;
  l->reg[slot].ident = ident;
  l->reg[slot].events = events;
  l->reg[slot].callback = (LooperCallback)callback;
  l->reg[slot].data = data;
  debugPrintf("android_shim: ALooper_addFd looper=%p fd=%d ident=%d "
              "events=0x%x data=%p\n", (void *)l, fd, ident, events, data);
  return 1;
}

int ALooper_removeFd(ALooper *looper, int fd) {
  FakeLooper *l = (FakeLooper *)looper;
  if (!l)
    return 0;
  for (int i = 0; i < l->reg_count; i++) {
    if (l->reg[i].fd == fd) {
      memmove(&l->reg[i], &l->reg[i + 1],
              (size_t)(l->reg_count - i - 1) * sizeof(l->reg[0]));
      l->reg_count--;
      return 1;
    }
  }
  return 0;
}

void ALooper_wake(ALooper *looper) { (void)looper; }

static int looper_events_from_poll(short revents) {
  int events = 0;
  if (revents & POLLIN) events |= 1;  /* ALOOPER_EVENT_INPUT */
  if (revents & POLLOUT) events |= 2; /* ALOOPER_EVENT_OUTPUT */
  if (revents & POLLERR) events |= 4;
  if (revents & POLLHUP) events |= 8;
  if (revents & POLLNVAL) events |= 16;
  return events;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
  FakeLooper *l = g_thread_looper;
  if (outFd) *outFd = -1;
  if (outEvents) *outEvents = 0;
  if (outData) *outData = NULL;
  if (!l)
    return -4; /* ALOOPER_POLL_ERROR */

  if (input_queue_count() == 0)
    process_sdl_events();

  struct pollfd pfds[MAX_LOOPER_FDS];
  for (int i = 0; i < l->reg_count; i++) {
    pfds[i].fd = l->reg[i].fd;
    pfds[i].events = 0;
    if (l->reg[i].events & 1) pfds[i].events |= POLLIN;
    if (l->reg[i].events & 2) pfds[i].events |= POLLOUT;
    pfds[i].revents = 0;
  }

  int timeout = timeoutMillis;
  if (input_queue_count() > 0)
    timeout = 0;
  else if (timeout < 0 || timeout > 5)
    timeout = 5; /* mantém SDL e a fila OpenSL responsivos */

  int ready = poll(pfds, (nfds_t)l->reg_count, timeout);
  if (ready < 0)
    return -4;
  for (int i = 0; i < l->reg_count; i++) {
    if (!pfds[i].revents)
      continue;
    int events = looper_events_from_poll(pfds[i].revents);
    if (l->reg[i].callback) {
      if (!l->reg[i].callback(l->reg[i].fd, events, l->reg[i].data))
        ALooper_removeFd((ALooper *)l, l->reg[i].fd);
      return -2; /* ALOOPER_POLL_CALLBACK */
    }
    if (outFd) *outFd = l->reg[i].fd;
    if (outEvents) *outEvents = events;
    if (outData) *outData = l->reg[i].data;
    return l->reg[i].ident;
  }

  if (input_queue_count() > 0 && l->input_queue) {
    if (l->input_callback) {
      if (!l->input_callback(-1, 1, l->input_data)) {
        l->input_queue = NULL;
        l->input_data = NULL;
      }
      return -2;
    }
    if (outEvents) *outEvents = 1;
    if (outData) *outData = l->input_data;
    return l->input_ident;
  }

  return -3; /* ALOOPER_POLL_TIMEOUT */
}

/* ---- AInputQueue ---- */

void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data) {
  FakeLooper *l = (FakeLooper *)looper;
  if (!l)
    return;
  l->input_queue = (AInputQueue *)queue;
  l->input_ident = ident;
  l->input_callback = (LooperCallback)callback;
  l->input_data = data;
  debugPrintf("android_shim: AInputQueue_attachLooper queue=%p looper=%p "
              "ident=%d data=%p\n", queue, looper, ident, data);
}

void AInputQueue_detachLooper(void *queue) {
  FakeLooper *l = g_thread_looper;
  if (l && l->input_queue == (AInputQueue *)queue) {
    l->input_queue = NULL;
    l->input_data = NULL;
    l->input_callback = NULL;
  }
}

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

int AInputQueue_hasEvents(void *queue) {
  (void)queue;
  return input_queue_count() > 0 ? 1 : 0;
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

int AMotionEvent_getFlags(void *event) {
  (void)event;
  return 0;
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

int AConfiguration_getLanguage(void *config, char *outLanguage) {
  (void)config;
  if (outLanguage) {
    outLanguage[0] = 'e';
    outLanguage[1] = 'n';
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
  active_app()->destroyRequested = 1;
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
  g_runtime_app = NULL;
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

  /* O Action Squad contém a glue dentro da própria .so e mantém pre/post
   * exec locais. A fonte oficial registra &cmdPollSource em ALooper_addFd;
   * o nosso looper devolve esse ponteiro e é a própria glue que executa os
   * helpers. Não procurar nem substituir símbolos internos. */
  g_app_pre_exec_cmd = NULL;
  g_app_post_exec_cmd = NULL;

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
  pad_positional_fix_env("AS");

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

void android_shim_bind_app(struct android_app *app) {
  g_runtime_app = app;
  debugPrintf("android_shim: runtime android_app vinculado em %p\n", (void *)app);
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
