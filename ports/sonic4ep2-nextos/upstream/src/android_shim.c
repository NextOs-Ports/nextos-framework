/*
 * android_shim.c -- fake Android NDK for Linux ARM64
 *
 * Implements enough of the android_native_app_glue + Android NDK
 * to let libsyberia1.so's android_main() run on Linux.
 *
 * Input handling:
 *   SDL gamepad events are converted to fake AInputEvent structs
 *   (key events for buttons, motion events for analog stick cursor).
 *   The game's onInputEvent callback receives them through the
 *   standard AInputQueue_getEvent flow.
 */

#include <SDL2/SDL.h>
#include <poll.h>
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

/* ---- Screen resolution ---- */
extern int sonic_screen_w, sonic_screen_h; /* resolucao real (egl_shim) */
#define SCREEN_WIDTH sonic_screen_w
#define SCREEN_HEIGHT sonic_screen_h

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

#ifndef AINPUT_SOURCE_GAMEPAD
#define AINPUT_SOURCE_GAMEPAD 0x00000401
#endif
#ifndef AINPUT_SOURCE_KEYBOARD
#define AINPUT_SOURCE_KEYBOARD 0x00000101
#endif
#ifndef AINPUT_SOURCE_DPAD
#define AINPUT_SOURCE_DPAD 0x00000201
#endif
static void push_key_event(int action, int keycode) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  /* O Black::Input::Controller_impl::receiveMsg exige msg.source == 0x501
   * (=AINPUT_SOURCE_GAMEPAD 0x401 | AINPUT_SOURCE_KEYBOARD 0x101 = source REAL de
   * botão de gamepad Android) + type==1 + getRepeatCount==0, e então decodifica o
   * keycode. (Keyboard_impl quer 0x301.) Sem o source EXATO, nenhum handler lê a
   * tecla. Mandamos 0x501 p/ todos os botões/dpad (Controller_impl mapeia). */
  ev.source = AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_KEYBOARD; /* 0x501 */
  input_queue_push(&ev);
}

/* Tecla de TECLADO: o Black::Input::Keyboard_impl::receiveMsg exige source==0x301
 * (AINPUT_SOURCE_KEYBOARD 0x101 | AINPUT_SOURCE_DPAD 0x201) e grava keystate por
 * keycode. O MOVIMENTO/navegação do jogo lê o keystate do teclado (não o eixo do
 * gamepad, que neste build mobile não move). Mandamos dpad/analógico como
 * AKEYCODE_DPAD_* (19-22) por aqui. */
#define SONIC_KBD_SOURCE 0x301
static void push_kbd_event(int action, int keycode) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  ev.source = SONIC_KBD_SOURCE;
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

/* eixos extra que o Controller_impl do jogo lê (além de X/Y/Z/RZ/HAT já no .h) */
#ifndef AMOTION_EVENT_AXIS_GAS
#define AMOTION_EVENT_AXIS_GAS 22
#endif
#ifndef AMOTION_EVENT_AXIS_BRAKE
#define AMOTION_EVENT_AXIS_BRAKE 23
#endif
/* source que o Controller_impl::receiveMsg EXIGE pra eventos de MOTION (eixo):
 * o branch MOTION (0x24ee84) compara `msg.source == 0x01000010`
 * (AINPUT_SOURCE_JOYSTICK), NÃO 0x10. Mandar 0x10 = a engine faz `bne` e nem lê
 * os eixos -> stick/dpad/triggers IGNORADOS (era o bug do "menu não navega").
 * O HAT (axes 15/16) vira os 4 bits baixos de [ctrl+48] = MESMA palavra de estado
 * dos botões que JÁ funcionam -> com o source certo, dpad/analógico navegam. */
#define SONIC_MOTION_SOURCE 0x01000010

/* Envia UM evento de motion com TODOS os eixos que o jogo lê:
 * left stick (0,1), right stick (11,14), dpad HAT (15,16), triggers (GAS22/BRAKE23). */
static void push_joystick_event(float lx, float ly, float rx, float ry,
                                float hatx, float haty, float lt, float rt) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = SONIC_MOTION_SOURCE;
  ev.pointer_count = 1;
  ev.axes[AMOTION_EVENT_AXIS_X] = lx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = ly;
  ev.axes[AMOTION_EVENT_AXIS_Z] = rx;
  ev.axes[AMOTION_EVENT_AXIS_RZ] = ry;
  ev.axes[AMOTION_EVENT_AXIS_HAT_X] = hatx;
  ev.axes[AMOTION_EVENT_AXIS_HAT_Y] = haty;
  ev.axes[AMOTION_EVENT_AXIS_BRAKE] = lt; /* L2/LT */
  ev.axes[AMOTION_EVENT_AXIS_GAS]   = rt; /* R2/RT */
  ev.axes[AMOTION_EVENT_AXIS_LTRIGGER] = lt;
  ev.axes[AMOTION_EVENT_AXIS_RTRIGGER] = rt;
  if (getenv("SONIC_INDBG"))
    fprintf(stderr,"[SEND-MOTION] L(%.2f,%.2f) R(%.2f,%.2f) hat(%.2f,%.2f) lt=%.2f rt=%.2f src=0x%x\n",
            lx,ly,rx,ry,hatx,haty,lt,rt,SONIC_MOTION_SOURCE);
  input_queue_push(&ev);
}

/* ---- SDL button → Android keycode mapping ---- */

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
  /* O jogo (Controller_impl) mapeia: A=96 B=97 X=99 Y=100 L1=102 R1=103
   * L3=106 R3=107 MENU(82)=pause BACK(4)=cancelar. START físico -> MENU(82)
   * (é o botão de pause/menu do jogo; keycode START=108 NÃO é mapeado). */
  case SDL_CONTROLLER_BUTTON_START:
    return AKEYCODE_MENU;          /* 82 -> pause/menu do jogo */
  case SDL_CONTROLLER_BUTTON_BACK:
    return AKEYCODE_BACK;          /* 4  -> back/cancelar */
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return AKEYCODE_BUTTON_L1;     /* 102 */
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return AKEYCODE_BUTTON_R1;     /* 103 */
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return AKEYCODE_BUTTON_THUMBL; /* 106 = L3 */
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return AKEYCODE_BUTTON_THUMBR; /* 107 = R3 */
  /* DPAD: NÃO é keycode — vai como HAT (eixos 15/16) via update_hat_from_dpad. */
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
  default:
    return -1;
  }
}

/* DPAD SDL -> AKEYCODE_DPAD_* (19-22), p/ o caminho TECLADO (Keyboard_impl). */
static int sdl_dpad_to_akeycode(int sdl_button) {
  switch (sdl_button) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP:    return AKEYCODE_DPAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return AKEYCODE_DPAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return AKEYCODE_DPAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AKEYCODE_DPAD_RIGHT;
  default: return -1;
  }
}

/* ---- Initialize gamepad ---- */

static void init_gamecontroller(void) {
  if (g_gamecontroller)
    return;
  int num = SDL_NumJoysticks();
  debugPrintf("android_shim: %d joysticks found\n", num);
  for (int i = 0; i < num; i++) {
    if (SDL_IsGameController(i)) {
      g_gamecontroller = SDL_GameControllerOpen(i);
      if (g_gamecontroller) {
        debugPrintf("android_shim: Opened gamepad: %s\n",
                    SDL_GameControllerName(g_gamecontroller));
        return;
      }
    }
  }
}

/* ---- Ponte Paddleboat (input nativo do Sonic) ----
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
  /* Shantae (engine Black) NÃO tem Paddleboat (código herdado do Dysmantle).
   * Desiste após a 1ª falha p/ não spammar o log todo frame. */
  static int pb_gaveup = 0;
  if (pb_gaveup) return;
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
      debugPrintf("android_shim: Paddleboat exports não achados (modo SDL nativo)\n");
      pb_isInitialized = NULL;
      pb_gaveup = 1;
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

/* modo gptokeyb (launcher seta SONIC_INPUT=gptk): botões vêm do TECLADO
 * (uinput do gptokeyb via sonic4.gptk); botões nativos do pad são ignorados
 * (duplicariam). Eixos analógicos continuam nativos quando o pad é visível. */
static int gptk_on(void) {
  static int g = -1;
  if (g < 0) {
    const char *ie = getenv("SONIC_INPUT");
    g = (ie && strcmp(ie, "gptk") == 0) ? 1 : 0;
    if (g) debugPrintf("android_shim: modo GPTOKEYB (teclado via sonic4.gptk)\n");
  }
  return g;
}

/* Hotkey universal de SAIR (SELECT+START) — igual ao Bully, NO BINARIO. Garantia
 * independente do gptokeyb: o launcher faz `gptokeyb "sonic4"` mas o processo
 * tem comm="Main" (a engine renomeia a thread), entao o gptokeyb NAO acha o
 * processo p/ matar -> o .sh sozinho nao fecha. Aqui lemos o pad direto (SDL ve
 * o pad mesmo com gptokeyb, que nao faz grab exclusivo) e `_exit` na hora
 * (evita deadlock do blob Mali ao liberar o contexto GL no teardown). */
/* estado SELECT(esc)+START(enter) vindos do gptokeyb -- rastreado SEMPRE (mesmo
 * sem gptk_on), igual o g_kb[] do Bully, p/ a saida funcionar em qualquer device */
static int g_kb_esc = 0, g_kb_ent = 0;
static void check_exit_hotkey(void) {
  /* DOIS caminhos checados TODO FRAME (igual Bully): pad nativo (SDL ve o pad)
   * E teclado do gptokeyb (esc+enter). Qualquer um que chegue fecha o jogo.
   * _exit imediato evita o deadlock do blob Mali ao liberar o contexto GL. */
  int pad_combo = 0;
  if (g_gamecontroller) {
    SDL_GameControllerUpdate();
    pad_combo = SDL_GameControllerGetButton(g_gamecontroller, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(g_gamecontroller, SDL_CONTROLLER_BUTTON_START);
  }
  if (pad_combo || (g_kb_esc && g_kb_ent)) {
    debugPrintf("android_shim: SELECT+START -> saindo do jogo\n");
    _exit(0);
  }
}

/* Injetor de TOQUE por coordenada (debug/automação): `echo "x y" > /dev/shm/sonic_tap`
 * -> toca (down, move, up ~5 frames) na posição ABSOLUTA x,y. A UI do menu é touch,
 * e o toque é caminho separado do controle 0 -> IMUNE ao attract demo que sobrescreve
 * o pad (por isso a navegação por botão era não-determinística). Permite entrar no
 * jogo de forma confiável (tocar no PLAY). Custo zero sem o trigger. */
void sonic_tap_inject(void) {   /* chamado de my_pb_getdata (roda no menu E in-game) */
  static int chk = 0, hold = 0;
  static float tx = 0, ty = 0;
  if (hold > 0) {
    if (--hold == 0) {
      push_motion_event(AMOTION_EVENT_ACTION_UP, tx, ty);
      debugPrintf("[tap] up %.0f,%.0f\n", tx, ty);
    } else {
      push_motion_event(AMOTION_EVENT_ACTION_MOVE, tx, ty);
    }
    return;
  }
  if (++chk % 6) return;
  FILE *f = fopen("/dev/shm/sonic_tap", "r");
  if (!f) return;
  float x, y;
  if (fscanf(f, "%f %f", &x, &y) == 2) {
    tx = x; ty = y; hold = 5;
    push_motion_event(AMOTION_EVENT_ACTION_DOWN, x, y);
    debugPrintf("[tap] down %.0f,%.0f\n", x, y);
  }
  fclose(f);
  unlink("/dev/shm/sonic_tap");
}

static void process_sdl_events(void) {
  // Try to open a gamepad if we don't have one yet
  init_gamecontroller();
  pb_try_connect();
  check_exit_hotkey();  /* SELECT+START -> sai (garantia, qualquer device) */

  /* SONIC_AUTOKEY: self-test autônomo (sem humano) — injeta botões via
   * push_key_event (caminho clássico onInputEvent) p/ avançar
   * título -> menu -> jogo e validar controles + gameplay. */
  if (getenv("SONIC_AUTOKEY")) {
    static int af = 0; af++;
    /* a partir do frame 240 (~4s, pós-intro), alterna A/START a cada ~90 frames
     * (down no fundo do ciclo, up 12 frames depois) p/ atravessar
     * intro->título->menu->jogo. */
    if (af >= 180) {
      /* SÓ o botão A repetido (sem B/BACK que volta do menu, sem dpad que move a
       * seleção): título->splash->menu->seleciona slot1 "New Game"->confirma->
       * cutscene->gameplay. A cada ~90 frames (~1.5s). */
      int cyc = (af - 180) % 90;
      if (cyc == 0)  { fprintf(stderr,"[AUTOKEY] A\n"); push_key_event(AKEY_EVENT_ACTION_DOWN, AKEYCODE_BUTTON_A); }
      if (cyc == 14) { push_key_event(AKEY_EVENT_ACTION_UP, AKEYCODE_BUTTON_A); }
    }
    /* TESTE TECLADO: injeta DPAD-baixo via teclado (source 0x301) p/ validar que
     * o Keyboard_impl move o menu/cursor. */
    if (getenv("SONIC_KBDTEST") && af >= 700) {
      int c = (af - 700) % 120;
      if (c == 0)  { fprintf(stderr,"[KBDTEST] DPAD_DOWN(kbd)\n"); push_kbd_event(AKEY_EVENT_ACTION_DOWN, AKEYCODE_DPAD_DOWN); }
      if (c == 16) { push_kbd_event(AKEY_EVENT_ACTION_UP, AKEYCODE_DPAD_DOWN); }
    }
    /* SONIC_AUTODPAD: depois do frame N, segura dpad-DIREITA (HAT) para validar
     * motion direto. */
    if (getenv("SONIC_AUTODPAD") && af >= 2700) {
      if ((af % 6) == 0) push_joystick_event(0,0,0,0, 1.0f /*hatx=dir*/, 0, 0, 0);
    }
  }
  /* sonic_tap_inject e' chamado de my_pb_getdata (main.c) -> roda no menu tambem */

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
     * SONIC_PB_SCRIPT="frame:action:keycode,..." (action 0=down 1=up).
     * Ex default: aperta A no frame 300 (confirma menu inicial), depois
     * DOWN/A pra navegar. Cada frame ~ pump do pollAll. */
    if (getenv("SONIC_PB_SELFTEST")) {
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
    if (getenv("SONIC_INDBG")) {
      switch (e.type) {
        case SDL_CONTROLLERBUTTONDOWN: fprintf(stderr,"[SDL] CBTN_DOWN btn=%d\n", e.cbutton.button); break;
        case SDL_CONTROLLERBUTTONUP:   fprintf(stderr,"[SDL] CBTN_UP   btn=%d\n", e.cbutton.button); break;
        case SDL_CONTROLLERAXISMOTION: if (e.caxis.value>8000||e.caxis.value<-8000) fprintf(stderr,"[SDL] CAXIS axis=%d val=%d\n", e.caxis.axis, e.caxis.value); break;
        case SDL_JOYBUTTONDOWN: fprintf(stderr,"[SDL] JBTN_DOWN btn=%d\n", e.jbutton.button); break;
        case SDL_JOYHATMOTION:  fprintf(stderr,"[SDL] JHAT hat=%d val=%d\n", e.jhat.hat, e.jhat.value); break;
        case SDL_JOYAXISMOTION:  if (e.jaxis.value>8000||e.jaxis.value<-8000) fprintf(stderr,"[SDL] JAXIS axis=%d val=%d\n", e.jaxis.axis, e.jaxis.value); break;
        case SDL_KEYDOWN: fprintf(stderr,"[SDL] KEYDOWN scan=%d\n", e.key.keysym.scancode); break;
        default: break;
      }
    }
    switch (e.type) {
    case SDL_QUIT:
      g_app.destroyRequested = 1;
      break;

    /* modo GPTOKEYB (SONIC_INPUT=gptk, padrão PortMaster): o gptokeyb
     * do CFW lê o controle físico e emite TECLADO via uinput conforme o
     * sonic4.gptk. Traduzimos as teclas pros MESMOS eventos Paddleboat:
     *   x=A c=B q=X t=Y enter=START esc=SELECT h=L1 j=R1 k=L2 l=R2
     *   n=L3 m=R3 setas=dpad wasd=stick esq (digital)
     * Sair: SELECT+START (esc+enter). */
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      /* GARANTIA DE SAIDA: rastreia SELECT(esc)+START(enter) SEMPRE, mesmo sem
       * gptk_on/repeat -> check_exit_hotkey (todo frame) fecha o jogo. */
      if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g_kb_esc = (e.type == SDL_KEYDOWN);
      if (e.key.keysym.scancode == SDL_SCANCODE_RETURN) g_kb_ent = (e.type == SDL_KEYDOWN);
      if (g_kb_esc && g_kb_ent) { debugPrintf("android_shim: SELECT+START (kbd) -> saindo\n"); _exit(0); }
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
      case SDL_SCANCODE_Q:      kc = AKEYCODE_BUTTON_X; break;
      case SDL_SCANCODE_T:      kc = AKEYCODE_BUTTON_Y; break;
      case SDL_SCANCODE_RETURN: kc = AKEYCODE_BUTTON_START; kb_ent = dn; break;
      case SDL_SCANCODE_ESCAPE: kc = AKEYCODE_BUTTON_SELECT; kb_esc = dn; break;
      case SDL_SCANCODE_H:      kc = AKEYCODE_BUTTON_L1; break;
      case SDL_SCANCODE_J:      kc = AKEYCODE_BUTTON_R1; break;
      case SDL_SCANCODE_K:      kc = AKEYCODE_BUTTON_L2; kb_lt = dn ? 1.0f : 0.0f; stick = 1; break;
      case SDL_SCANCODE_L:      kc = AKEYCODE_BUTTON_R2; kb_rt = dn ? 1.0f : 0.0f; stick = 1; break;
      case SDL_SCANCODE_N:      kc = AKEYCODE_BUTTON_THUMBL; break;
      case SDL_SCANCODE_M:      kc = AKEYCODE_BUTTON_THUMBR; break;
      case SDL_SCANCODE_UP:     kc = AKEYCODE_DPAD_UP;    dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
      case SDL_SCANCODE_DOWN:   kc = AKEYCODE_DPAD_DOWN;  dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
      case SDL_SCANCODE_LEFT:   kc = AKEYCODE_DPAD_LEFT;  dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
      case SDL_SCANCODE_RIGHT:  kc = AKEYCODE_DPAD_RIGHT; dpadbtn = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
      case SDL_SCANCODE_W:      kb_w = dn; stick = 1; break;
      case SDL_SCANCODE_A:      kb_a = dn; stick = 1; break;
      case SDL_SCANCODE_S:      kb_s = dn; stick = 1; break;
      case SDL_SCANCODE_D:      kb_d = dn; stick = 1; break;
      default: break;
      }
      if (kb_esc && kb_ent) {
        debugPrintf("android_shim: SELECT+START (gptk) -> saindo\n");
        _exit(0);
      }
      if (kc >= 0) {
        push_key_event(act, kc);
        pb_send_key(act, kc);
      }
      if (dpadbtn >= 0) update_hat_from_dpad(dpadbtn, dn);
      if (stick) {
        float lx = (kb_d ? 1.0f : 0.0f) - (kb_a ? 1.0f : 0.0f);
        float ly = (kb_s ? 1.0f : 0.0f) - (kb_w ? 1.0f : 0.0f);
        if (lx != 0.0f && ly != 0.0f) { lx *= 0.7071f; ly *= 0.7071f; }
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
      // D-pad: HAT (gamepad) + TECLA de TECLADO (Keyboard_impl move o jogo)
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        update_hat_from_dpad(e.cbutton.button, 1);
        int dk = sdl_dpad_to_akeycode(e.cbutton.button);
        if (dk >= 0) push_kbd_event(AKEY_EVENT_ACTION_DOWN, dk);
      }
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
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        update_hat_from_dpad(e.cbutton.button, 0);
        int dk = sdl_dpad_to_akeycode(e.cbutton.button);
        if (dk >= 0) push_kbd_event(AKEY_EVENT_ACTION_UP, dk);
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

    /* Analógico esquerdo -> HAT digital (axes 15/16) = MESMO caminho do dpad
     * (Controller_impl, bits baixos de [ctrl+48]). O caminho TECLADO (Keyboard_impl)
     * não move o menu neste build, então o stick também navega pelo HAT. O dpad
     * tem prioridade; quando solto, o stick assume. Threshold 0.5. */
    float stick_hx = (lx > 0.5f) ? 1.0f : (lx < -0.5f) ? -1.0f : 0.0f;
    float stick_hy = (ly > 0.5f) ? 1.0f : (ly < -0.5f) ? -1.0f : 0.0f;
    float hx = (g_hat_x != 0.0f) ? g_hat_x : stick_hx;
    float hy = (g_hat_y != 0.0f) ? g_hat_y : stick_hy;

    // Send joystick event only when values change (inclui o HAT efetivo)
    static float g_last_hx = 0, g_last_hy = 0;
    if (lx != g_last_lx || ly != g_last_ly ||
        rx != g_last_rx || ry != g_last_ry ||
        lt != g_last_lt || rt != g_last_rt ||
        hx != g_last_hx || hy != g_last_hy || g_motion_dirty) {
      push_joystick_event(lx, ly, rx, ry, hx, hy, lt, rt);
      pb_send_motion(lx, ly, rx, ry, hx, hy, lt, rt);
      g_last_lx = lx;
      g_last_ly = ly;
      g_last_rx = rx;
      g_last_ry = ry;
      g_last_lt = lt;
      g_last_rt = rt;
      g_last_hx = hx;
      g_last_hy = hy;
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

  int ret = poll(&pfd, 1, timeout);
  if (ret > 0 && (pfd.revents & POLLIN)) {
    if (outData)
      *outData = &g_app.cmdPollSource;
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
    return LOOPER_ID_INPUT;
  }

  // Fire audio callbacks again after poll
  opensles_shim_pump_callbacks();

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
  { static int z=0; if (getenv("SONIC_INLOG") && z<40){fprintf(stderr,"[Q] getKeyCode->%d\n",ev->keycode);z++;} }
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

/* PCS("aapcs") = SOFTFP: o jogo é softfp e espera o float de RETORNO em r0; sem
 * isto a função (hardfp) retorna em s0/VFP -> o jogo lê r0 com LIXO -> eixos do
 * analógico/triggers/hat com valor aleatório -> "anda sozinho" + direcionais
 * quebrados. (Mesmo motivo do glClearColor.) */
__attribute__((pcs("aapcs")))
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
  { static int z=0; if (getenv("SONIC_INLOG") && z<40){fprintf(stderr,"[Q] getSource->0x%x\n",ev->source);z++;} }
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

void ANativeActivity_finish(void *activity) {
  (void)activity;
  debugPrintf("ANativeActivity_finish called\n");
  g_app.destroyRequested = 1;
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
    FakeInputEvent *fe = (FakeInputEvent *)event;
    if (app->onInputEvent) {
      handled = app->onInputEvent(app, event);
      static int z = 0;
      if (z < 20) { /* one-shot diag: confirma entrega ao jogo */
        fprintf(stderr, "[INPUT] %s act=%d kc=%d xy=%.0f,%.0f handled=%d\n",
                fe->type==AINPUT_EVENT_TYPE_KEY?"KEY":"MOTION", fe->action,
                fe->keycode, fe->x, fe->y, handled); z++;
      }
    } else {
      static int zn = 0; if (zn++ < 3) fprintf(stderr, "[INPUT] app->onInputEvent NULO!\n");
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
  g_activity.internalDataPath = "./gamedata";
  g_activity.externalDataPath = "./gamedata";
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

  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fatal_error("android_shim: SDL_Init failed: %s\n", SDL_GetError());
  }
  debugPrintf("android_shim: SDL initialized\n");

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
