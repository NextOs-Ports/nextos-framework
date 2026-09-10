/* SPDX-License-Identifier: GPL-3.0-only */
/* BLOSSOMTALES-CONTROLS-LIVE (1.4.0) — ver bt_input.h.
 *
 * Cadeia: pad físico (SDL do firmware, admitido pela costura C6) -> união dos
 * pads (nxinput_padset) -> vocabulário simbólico -> chord soberano
 * SELECT+START (mesmo instance) -> GPTK vivo decide por contexto PROVADO
 * (Game1.currentState / PauseScreen.IsPaused via embedding do Mono) -> sink
 * real = KeyEvent Android para o MonoGameAndroidGameView, o MESMO caminho que
 * o passthrough nativo usa. Um keycode nunca tem dois donos ao mesmo tempo.
 *
 * Nada aqui é posicional: nenhum código evdev vira mapping, nenhum nome de
 * device é condição. A SDL é a do sistema, resolvida por dlsym(RTLD_DEFAULT)
 * exatamente como o resto do port. */
#define _GNU_SOURCE
#include "bt_input.h"

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "input_gptk.h"
#include "jni_shim.h"
#include "nxc6_glue.h"
#include "nxinput_exit_chord.h"
#include "nxinput_gptk.h"
#include "nxinput_padset.h"
#include "nxinput_sdl_seam.h"
#include "sdv_egl_bridge.h"
#include "present_policy.h"

/* ===== Constantes Android consumidas pelo MonoGame ====================== */
#define AKEY_BACK 4
#define AKEY_DPAD_UP 19
#define AKEY_DPAD_DOWN 20
#define AKEY_DPAD_LEFT 21
#define AKEY_DPAD_RIGHT 22
#define AKEY_BUTTON_A 96
#define AKEY_BUTTON_B 97
#define AKEY_BUTTON_X 99
#define AKEY_BUTTON_Y 100
#define AKEY_BUTTON_L1 102
#define AKEY_BUTTON_R1 103
#define AKEY_BUTTON_THUMBL 106
#define AKEY_BUTTON_THUMBR 107
#define AKEY_BUTTON_START 108

/* SDL2 (ABI estável): botões/eixos do GameController e eventos. */
#define BT_SDL_INIT_GAMECONTROLLER 0x2000u
#define BT_SDL_BUTTON_A 0
#define BT_SDL_BUTTON_B 1
#define BT_SDL_BUTTON_X 2
#define BT_SDL_BUTTON_Y 3
#define BT_SDL_BUTTON_BACK 4
#define BT_SDL_BUTTON_START 6
#define BT_SDL_BUTTON_LEFTSTICK 7
#define BT_SDL_BUTTON_RIGHTSTICK 8
#define BT_SDL_BUTTON_LEFTSHOULDER 9
#define BT_SDL_BUTTON_RIGHTSHOULDER 10
#define BT_SDL_BUTTON_DPAD_UP 11
#define BT_SDL_BUTTON_DPAD_DOWN 12
#define BT_SDL_BUTTON_DPAD_LEFT 13
#define BT_SDL_BUTTON_DPAD_RIGHT 14
#define BT_SDL_BUTTON_MAX 15
#define BT_SDL_AXIS_LEFTX 0
#define BT_SDL_AXIS_LEFTY 1
#define BT_SDL_AXIS_RIGHTX 2
#define BT_SDL_AXIS_RIGHTY 3
#define BT_SDL_AXIS_TRIGGERLEFT 4
#define BT_SDL_AXIS_TRIGGERRIGHT 5
#define BT_SDL_QUIT 0x100u
#define BT_SDL_WINDOWEVENT 0x200u
#define BT_SDL_WINDOWEVENT_FOCUS_LOST 13
#define BT_SDL_JOYDEVICEREMOVED 0x606u
#define BT_SDL_CONTROLLERDEVICEADDED 0x653u
#define BT_SDL_CONTROLLERDEVICEREMOVED 0x654u

#define BT_STICK_DEADZONE 0.15f
#define BT_TRIGGER_ENTER NXINPUT_GPTK_TRIGGER_ENTER
#define BT_TRIGGER_EXIT NXINPUT_GPTK_TRIGGER_EXIT
#define BT_ENGINE_SAMPLE_FRAMES 15 /* ~120 ms a 8 ms por quadro */

typedef struct { uint8_t data[16]; } bt_sdl_guid;

/* ===== SDL do firmware por dlsym (RTLD_DEFAULT): nunca privada ========== */
static struct {
    int (*num_joysticks)(void);
    int (*is_game_controller)(int);
    void *(*gc_open)(int);
    void (*gc_close)(void *);
    void *(*gc_get_joystick)(void *);
    void (*gc_update)(void);
    uint8_t (*gc_get_button)(void *, int);
    int16_t (*gc_get_axis)(void *, int);
    const char *(*gc_name)(void *);
    char *(*gc_mapping)(void *);
    int32_t (*joy_instance_id)(void *);
    int32_t (*joy_device_instance_id)(int);         /* SDL >= 2.0.6 */
    const char *(*joy_path_for_index)(int);         /* SDL >= 2.24 */
    bt_sdl_guid (*joy_device_guid)(int);
    void (*joy_guid_string)(bt_sdl_guid, char *, int);
    uint16_t (*joy_vendor)(void *);
    uint16_t (*joy_product)(void *);
    int (*poll_event)(void *);
    void (*pump_events)(void);
    uint32_t (*was_init)(uint32_t);
    const char *(*get_error)(void);
    void (*sdl_free)(void *);
} sdl;

static void *bt_sym(const char *name)
{
    (void)dlerror();
    return dlsym(RTLD_DEFAULT, name);
}

static int bt_resolve_sdl(void)
{
    static int done, ok;
    if (done) return ok;
    done = 1;
#define REQ(f, n) do { *(void **)&sdl.f = bt_sym(n); if (!sdl.f) { fprintf(stderr, "[bt/input] SDL symbol missing: %s\n", n); return 0; } } while (0)
#define OPT(f, n) do { *(void **)&sdl.f = bt_sym(n); } while (0)
    REQ(num_joysticks, "SDL_NumJoysticks");
    REQ(is_game_controller, "SDL_IsGameController");
    REQ(gc_open, "SDL_GameControllerOpen");
    REQ(gc_close, "SDL_GameControllerClose");
    REQ(gc_get_joystick, "SDL_GameControllerGetJoystick");
    REQ(gc_update, "SDL_GameControllerUpdate");
    REQ(gc_get_button, "SDL_GameControllerGetButton");
    REQ(gc_get_axis, "SDL_GameControllerGetAxis");
    REQ(joy_instance_id, "SDL_JoystickInstanceID");
    REQ(joy_device_guid, "SDL_JoystickGetDeviceGUID");
    REQ(joy_guid_string, "SDL_JoystickGetGUIDString");
    REQ(poll_event, "SDL_PollEvent");
    REQ(pump_events, "SDL_PumpEvents");
    REQ(was_init, "SDL_WasInit");
    REQ(get_error, "SDL_GetError");
    REQ(sdl_free, "SDL_free");
    OPT(gc_name, "SDL_GameControllerName");
    OPT(gc_mapping, "SDL_GameControllerMapping");
    OPT(joy_device_instance_id, "SDL_JoystickGetDeviceInstanceID");
    OPT(joy_path_for_index, "SDL_JoystickPathForIndex");
    OPT(joy_vendor, "SDL_JoystickGetVendor");
    OPT(joy_product, "SDL_JoystickGetProduct");
#undef REQ
#undef OPT
    ok = 1;
    return 1;
}

/* ===== Estado ============================================================= */
static char bt_gamedir[1024];
static nxinput_padset padset;
static nxinput_padset_sdl padset_sdl;
static nxinput_exit_chord exit_chord;
static int seam_adopted;
static char staged_mapping[NXINPUT_AUTHORITY_SOURCE_MAX];
static int control_down[NXINPUT_GPTK_CONTROL_COUNT];
static float trigger_value[2];
static int trigger_digital[2];
static int input_fatal;
static volatile sig_atomic_t *exit_flag;
static volatile sig_atomic_t exit_requested;

/* Entrega ao MonoGame: os handlers marshalled e um KeyEvent/MotionEvent falso
 * por tipo; o mesmo objeto é reaproveitado porque o managed consome o evento
 * sincronamente dentro do callback (contrato do 1.3.0). */
typedef unsigned char (*key_cb)(void *, void *, int, void *);
typedef unsigned char (*motion_cb)(void *, void *, void *);
typedef unsigned char (*touch_cb)(void *, void *, void *, void *);
static void *jni_env, *jni_view;
static key_cb cb_key_down, cb_key_up;
static motion_cb cb_motion;
static touch_cb cb_touch;
static void *key_event, *gamepad_event, *touch_event;
#define BT_DEVICE_ID 1 /* P1: o único jogador lógico (união dos pads) */

/* ===== Bancada (nunca na release) ======================================= */
#ifdef BT_BENCH_PROBES
static int input_diag;
#else
enum { input_diag = 0 };
#endif

/* ===== Costura C6 antes de qualquer SDL_Init ============================ */
static int stage_seam_before_init(void)
{
    size_t staged_len = 0;
    if (!getenv("NXC6_SEAM")) {
        fprintf(stderr, "[bt/input] NXC6 seam: not adopted for this run (NXC6_SEAM absent); stock SDL behaviour\n");
        return 0;
    }
    if (!sdl.joy_path_for_index || !sdl.joy_device_instance_id) {
        fprintf(stderr, "[bt/input] NXC6 seam: this SDL cannot name the device node (pre-2.24); staying native\n");
        return 0;
    }
    /* V5: a própria costura identifica a SDL realmente mapeada antes de
     * decidir. Provider desconhecido deixa SDL_GAMECONTROLLERCONFIG no lugar
     * para a importação stock do CFW; provider provado encena os bytes para a
     * tradução tipada. Nunca decidir por nome do firmware. */
    int rc = nxc6_stage_before_init(staged_mapping, sizeof staged_mapping,
                                    &staged_len);
    if (rc < 0) {
        fprintf(stderr, "[bt/input] NXC6 seam: staging failed before the joystick init (rc=%d); refusing to guess\n", rc);
        return -1;
    }
    seam_adopted = 1;
    int declared = nxc6_declare_port_bundle_for_layout(bt_gamedir, bt_gptk_face_layout());
    fprintf(stderr, "[bt/input] NXC6 seam: port bundle %s (layout=%s)\n",
            declared > 0 ? getenv("NXCONTROLLER_PROFILES") : declared == 0 ? "(none shipped)" : "(declaration failed)",
            nxinput_gptk_face_layout_name(bt_gptk_face_layout()));
    fprintf(stderr, "[bt/input] NXC6 seam: result=%s staged=%zu bytes env_still_set=%d receipt=%s\n",
            rc == 1 ? "left-for-stock" : "staged", staged_len,
            getenv("SDL_GAMECONTROLLERCONFIG") != NULL,
            getenv("NXC6_RECEIPT") ? getenv("NXC6_RECEIPT") : "(none)");
    return 0;
}

int bt_input_preinit(const char *gamedir)
{
    snprintf(bt_gamedir, sizeof bt_gamedir, "%s", gamedir && *gamedir ? gamedir : ".");
#ifdef BT_BENCH_PROBES
    input_diag = getenv("BT_INPUT_DIAG") != NULL;
#endif
    if (!bt_resolve_sdl())
        return -1;
    if (bt_gptk_preinit(bt_gamedir) != 0)
        return -1;
    if (!bt_gptk_loaded()) {
        fprintf(stderr, "[bt/input] NEXTOSCONTROLLERS ausente/inválido; abortando (fail-closed)\n");
        return -1;
    }
    return stage_seam_before_init();
}

int bt_input_vendor_id(void)
{
    /* Nintendo (0x057e) faz o jogo mostrar rótulos Nintendo (B embaixo);
     * qualquer outro vendor = glyphs genéricos/Xbox (A embaixo). */
    return bt_gptk_face_layout() == NXC6_FACE_LAYOUT_RETRO ? 0x057e : 0x045e;
}

int bt_input_product_id(void)
{
    return bt_gptk_face_layout() == NXC6_FACE_LAYOUT_RETRO ? 0x2009 : 0x02ea;
}

/* ===== Padset: vtable sobre a SDL resolvida ============================== */
static int ps_num(void) { return sdl.num_joysticks(); }
static int32_t ps_inst(int i) { return sdl.joy_device_instance_id ? sdl.joy_device_instance_id(i) : -1; }
static int ps_isgc(int i) { return sdl.is_game_controller(i) ? 1 : 0; }
static void *ps_open(int i) { return sdl.gc_open(i); }
static void ps_close(void *c) { sdl.gc_close(c); }
static void *ps_joy(void *c) { return sdl.gc_get_joystick(c); }
static int32_t ps_joyinst(void *j) { return sdl.joy_instance_id(j); }
static void ps_update(void) { sdl.gc_update(); }
static uint8_t ps_btn(void *c, int b) { return sdl.gc_get_button(c, b); }
static int16_t ps_axis(void *c, int a) { return sdl.gc_get_axis(c, a); }

static void padset_log(const char *line, void *u) { (void)u; fprintf(stderr, "[bt/input] %s\n", line); }

/* Admissão pela autoridade do port (costura C6): o padset nunca decide. */
static int padset_admit(int i, void *u)
{
    (void)u;
    if (!seam_adopted)
        return 1;
    bt_sdl_guid guid = sdl.joy_device_guid(i);
    char guid_text[64];
    sdl.joy_guid_string(guid, guid_text, sizeof guid_text);
    const char *devpath = sdl.joy_path_for_index ? sdl.joy_path_for_index(i) : NULL;
    if (!nxc6_admit_before_announce((int)ps_inst(i), guid_text, devpath ? devpath : "")) {
        fprintf(stderr, "[bt/input] NXC6 seam: device %d (%s) refused by the authority order\n", i, guid_text);
        return 0;
    }
    return 1;
}

static void padset_opened(int i, unsigned slot, void *opened, void *u)
{
    (void)u;
    void *joy = sdl.gc_get_joystick(opened);
    const char *physical = sdl.gc_name ? sdl.gc_name(opened) : NULL;
    int vendor = (joy && sdl.joy_vendor) ? sdl.joy_vendor(joy) : 0;
    int product = (joy && sdl.joy_product) ? sdl.joy_product(joy) : 0;
    char *mapping = sdl.gc_mapping ? sdl.gc_mapping(opened) : NULL;
    fprintf(stderr, "[bt/input] controller: %s (%04x:%04x) mapping=%s\n",
            physical ? physical : "unknown", vendor & 0xffff, product & 0xffff, mapping ? mapping : "unavailable");
    fprintf(stderr, "[bt/input] pad slot=%u instance=%d sdl_index=%d\n", slot, (int)padset.instances[slot], i);
    if (mapping) sdl.sdl_free(mapping);
}

static void open_controllers(void)
{
    nxinput_padset_open_all(&padset, padset_admit, padset_opened, NULL);
    if (padset.count == 0 && sdl.num_joysticks() > 0)
        fprintf(stderr, "[bt/input] %d joystick(s) visible but none admitted as GameController; no fallback by design\n", sdl.num_joysticks());
    sdv_egl_set_gamepad_mask(padset.count ? 1u : 0u);
}

/* ===== Entrega de teclas: uma tabela, dois donos possíveis, um DOWN ====== */
static uint8_t key_down_state[256];
static int sink_key_pressed[256];

static void deliver_key(int keycode, int down)
{
    if (keycode <= 0 || keycode >= 256 || !cb_key_down || !cb_key_up || !key_event)
        return;
    if ((key_down_state[keycode] != 0) == (down != 0))
        return;
    key_down_state[keycode] = down ? 1 : 0;
    jni_set_key_event_keycode(key_event, keycode, BT_DEVICE_ID);
    unsigned char handled = down ? cb_key_down(jni_env, jni_view, keycode, key_event)
                                 : cb_key_up(jni_env, jni_view, keycode, key_event);
    if (input_diag)
        fprintf(stderr, "[bt/key] keycode=%d %s handled=%u\n", keycode, down ? "down" : "up", handled);
}

static void release_all_keys(void)
{
    for (int k = 1; k < 256; k++) {
        sink_key_pressed[k] = 0;
        if (key_down_state[k])
            deliver_key(k, 0);
    }
}

static void sink_key(int keycode, int pressed)
{
    if (keycode <= 0 || keycode >= 256)
        return;
    if (pressed) {
        sink_key_pressed[keycode]++;
        deliver_key(keycode, 1);
    } else {
        if (sink_key_pressed[keycode] > 0)
            sink_key_pressed[keycode]--;
        if (sink_key_pressed[keycode] == 0)
            deliver_key(keycode, 0);
    }
}

int bt_sink_android_keyevent(void *user, const char *action, int pressed, float value)
{
    (void)value;
    int keycode = (int)(intptr_t)user;
    if (input_diag)
        fprintf(stderr, "[bt/sink] %s keycode=%d %s\n", action, keycode, pressed ? "down" : "up");
    sink_key(keycode, pressed);
    return 0;
}

static int register_sinks(void)
{
    /* Keycode Android que o binding nativo do MonoGame/Blossom Tales espera
     * para cada ação (tabela do 1.3.0: BUTTON_A=ação/confirmar, B=item/voltar,
     * X=ferramenta, Y=menu, L1/R1=item anterior/seguinte, THUMBL=diário,
     * START=pause, BACK=escape). */
    struct { const char *action; int keycode; } b[] = {
        { "player.attack",          AKEY_BUTTON_A },
        { "player.use_item",        AKEY_BUTTON_B },
        { "player.use_tool",        AKEY_BUTTON_X },
        { "player.open_menu",       AKEY_BUTTON_Y },
        { "player.select_previous", AKEY_BUTTON_L1 },
        { "player.select_next",     AKEY_BUTTON_R1 },
        { "player.open_journal",    AKEY_BUTTON_THUMBL },
        { "system.pause",           AKEY_BUTTON_START },
        { "system.back",            AKEY_BACK },
        { "menu.accept",            AKEY_BUTTON_A },
        { "menu.back",              AKEY_BUTTON_B },
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (bt_gptk_register_button(b[i].action, "adapter.input.android-keyevent",
                                    bt_sink_android_keyevent, (void *)(intptr_t)b[i].keycode) != 0)
            return -1;
    return bt_gptk_seal();
}

/* ===== Ponte SDL_GameController -> vocabulário simbólico ================ */
static int control_of(int sdl_button)
{
    switch (sdl_button) {
    case BT_SDL_BUTTON_A: return NXINPUT_GPTK_A;
    case BT_SDL_BUTTON_B: return NXINPUT_GPTK_B;
    case BT_SDL_BUTTON_X: return NXINPUT_GPTK_X;
    case BT_SDL_BUTTON_Y: return NXINPUT_GPTK_Y;
    case BT_SDL_BUTTON_LEFTSHOULDER: return NXINPUT_GPTK_L1;
    case BT_SDL_BUTTON_RIGHTSHOULDER: return NXINPUT_GPTK_R1;
    case BT_SDL_BUTTON_LEFTSTICK: return NXINPUT_GPTK_L3;
    case BT_SDL_BUTTON_RIGHTSTICK: return NXINPUT_GPTK_R3;
    case BT_SDL_BUTTON_START: return NXINPUT_GPTK_START;
    case BT_SDL_BUTTON_BACK: return NXINPUT_GPTK_SELECT;
    case BT_SDL_BUTTON_DPAD_UP: return NXINPUT_GPTK_UP;
    case BT_SDL_BUTTON_DPAD_DOWN: return NXINPUT_GPTK_DOWN;
    case BT_SDL_BUTTON_DPAD_LEFT: return NXINPUT_GPTK_LEFT;
    case BT_SDL_BUTTON_DPAD_RIGHT: return NXINPUT_GPTK_RIGHT;
    default: return -1; /* GUIDE: fora */
    }
}

/* Keycode nativo do 1.3.0 por controle. R3 NÃO entra: ele pertence ao cursor
 * (clique com seta visível, THUMBR sem seta), decidido no laço; L2/R2 chegam
 * só pelos eixos de gatilho do MotionEvent, como antes. */
static int native_keycode(int control)
{
    switch (control) {
    case NXINPUT_GPTK_A: return AKEY_BUTTON_A;
    case NXINPUT_GPTK_B: return AKEY_BUTTON_B;
    case NXINPUT_GPTK_X: return AKEY_BUTTON_X;
    case NXINPUT_GPTK_Y: return AKEY_BUTTON_Y;
    case NXINPUT_GPTK_L1: return AKEY_BUTTON_L1;
    case NXINPUT_GPTK_R1: return AKEY_BUTTON_R1;
    case NXINPUT_GPTK_L3: return AKEY_BUTTON_THUMBL;
    case NXINPUT_GPTK_START: return AKEY_BUTTON_START;
    case NXINPUT_GPTK_SELECT: return AKEY_BACK;
    case NXINPUT_GPTK_UP: return AKEY_DPAD_UP;
    case NXINPUT_GPTK_DOWN: return AKEY_DPAD_DOWN;
    case NXINPUT_GPTK_LEFT: return AKEY_DPAD_LEFT;
    case NXINPUT_GPTK_RIGHT: return AKEY_DPAD_RIGHT;
    default: return 0;
    }
}

static float axis_norm(int axis)
{
    int16_t v = nxinput_padset_axis(&padset, axis);
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void radial_deadzone(float *x, float *y)
{
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= BT_STICK_DEADZONE) { *x = *y = 0.0f; return; }
    float scale = (m - BT_STICK_DEADZONE) / (1.0f - BT_STICK_DEADZONE) / m;
    if (m > 1.0f) scale = 1.0f / m;
    *x *= scale;
    *y *= scale;
}

static void sample_controls(void)
{
    memset(control_down, 0, sizeof control_down);
    nxinput_padset_sample(&padset);
    for (int i = 0; i < BT_SDL_BUTTON_MAX && i < (int)NXINPUT_PADSET_BUTTON_MAX; i++) {
        int c = control_of(i);
        if (c >= 0 && padset.buttons[i])
            control_down[c] = 1;
    }
    trigger_value[0] = axis_norm(BT_SDL_AXIS_TRIGGERLEFT);
    trigger_value[1] = axis_norm(BT_SDL_AXIS_TRIGGERRIGHT);
    if (trigger_value[0] < 0.0f) trigger_value[0] = 0.0f;
    if (trigger_value[1] < 0.0f) trigger_value[1] = 0.0f;
    for (int t = 0; t < 2; t++)
        trigger_digital[t] = trigger_digital[t] ? trigger_value[t] > BT_TRIGGER_EXIT
                                                : trigger_value[t] > BT_TRIGGER_ENTER;
    control_down[NXINPUT_GPTK_L2] = trigger_digital[0];
    control_down[NXINPUT_GPTK_R2] = trigger_digital[1];
}

/* ===== Contexto provado pela engine (embedding do Mono, via main.c) ====== */
static void update_engine_context(unsigned long frame)
{
    static char state[64];
    static int paused, probed;
    if (frame == 0) {
        bt_gptk_clear_context("frame0");
        return;
    }
    if (frame % BT_ENGINE_SAMPLE_FRAMES == 1) {
        char s[64] = "";
        int p = 0;
        probed = sb_engine_state_probe(s, sizeof s, &p);
        if (probed && strcmp(s, state) != 0)
            fprintf(stderr, "[bt/input] engine state=\"%s\"\n", s);
        snprintf(state, sizeof state, "%s", s);
        paused = p;
    }
    if (!probed) {
        bt_gptk_clear_context("engine-contract-unavailable");
        return;
    }
    if (strcmp(state, "GameState_Playing") == 0 && paused)
        bt_gptk_set_context(BT_GPTK_CONTEXT_MENU, "state:paused");
    else if (strcmp(state, "GameState_Playing") == 0)
        bt_gptk_set_context(BT_GPTK_CONTEXT_GAMEPLAY, "state:playing");
    else if (strcmp(state, "GameState_Menu") == 0)
        bt_gptk_set_context(BT_GPTK_CONTEXT_MENU, "state:menu");
    else if (state[0] == 0 || strcmp(state, "GameState_Loading") == 0)
        bt_gptk_clear_context("state:loading");
    else
        bt_gptk_set_context(BT_GPTK_CONTEXT_MENU, "state:other");
}

/* ===== Init ============================================================== */
int bt_input_init(void)
{
    nxinput_exit_chord_init(&exit_chord, 1);
    if (!(sdl.was_init(0) & BT_SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "[bt/input] SDL GAMECONTROLLER subsystem is not up (bridge bootstrap missing)\n");
        return -1;
    }
    padset_sdl.num_joysticks = ps_num;
    padset_sdl.instance_for_index = ps_inst;
    padset_sdl.is_game_controller = ps_isgc;
    padset_sdl.open = ps_open;
    padset_sdl.close = ps_close;
    padset_sdl.get_joystick = ps_joy;
    padset_sdl.joystick_instance = ps_joyinst;
    padset_sdl.update = ps_update;
    padset_sdl.get_button = ps_btn;
    padset_sdl.get_axis = ps_axis;
    if (nxinput_padset_init(&padset, &padset_sdl, padset_log, NULL) != 0) {
        fprintf(stderr, "[bt/input] nxinput_padset: vtable incompleta (fail-closed)\n");
        return -1;
    }
    fprintf(stderr, "[bt/input] pads: %s (união dos admitidos, chord por instance)\n", nxinput_padset_marker());
    open_controllers();
    if (register_sinks() != 0) {
        fprintf(stderr, "[bt/input] runtime vivo não selado; abortando (fail-closed)\n");
        return -1;
    }
    fprintf(stderr, "[bt/input] layout: gamepad nativo + GPTK vivo; chord=SELECT+START(sovereign); cursor=right-stick+R3 (P1, native)\n");
    return 0;
}

/* ===== Cursor do analógico direito (comportamento aprovado do 1.3.0) ==== */
static double mono_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void send_touch(int action, float x, float y)
{
    float sx = x, sy = y;
    if (!cb_touch || !touch_event) return;
    /* V5 / FV5: the cursor lives in drawable pixels; the game receives the
     * point through the INVERSE of the present content rect (letterbox), so
     * a tap on the visible image lands on the same image pixel. */
    (void)sb_present_policy_touch(x, y, &sx, &sy);
    jni_set_motion_event(touch_event, action, sx, sy);
    cb_touch(jni_env, jni_view, jni_view, touch_event);
}

/* ===== Laço ============================================================== */
void bt_input_run_loop(void *env, void *view, void *down_handler, void *up_handler,
                       void *motion_handler, void *touch_handler,
                       volatile sig_atomic_t *flag)
{
    jni_env = env;
    jni_view = view;
    cb_key_down = (key_cb)down_handler;
    cb_key_up = (key_cb)up_handler;
    cb_motion = (motion_cb)motion_handler;
    cb_touch = (touch_cb)touch_handler;
    exit_flag = flag;
    key_event = jni_make_object(jni_make_class("android.view.KeyEvent"));
    gamepad_event = cb_motion ? jni_make_object(jni_make_class("android.view.MotionEvent")) : NULL;
    touch_event = cb_touch ? jni_make_object(jni_make_class("android.view.MotionEvent")) : NULL;

    int cursor_w = sdv_egl_width(), cursor_h = sdv_egl_height();
    if (cursor_w <= 0) cursor_w = 1280;
    if (cursor_h <= 0) cursor_h = 720;
    float cursor_x = (float)cursor_w * 0.5f, cursor_y = (float)cursor_h * 0.5f;
    int cursor_visible = 0, cursor_touch_down = 0, r3_was_down = 0, r3_mode = 0;
    double last_cursor_activity = 0.0, last_tick = mono_seconds();
    const char *cursor_env = getenv("SB_RIGHT_CURSOR");
    int right_cursor_enabled = cb_touch && !(cursor_env && cursor_env[0] == '0');
    float prev_axes[8] = {0};
    int prev_axes_valid = 0;
    unsigned long frame = 0;

    sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
    fprintf(stderr, "[bt/input] loop ativo; cursor direito=%s (R3=clique, timeout=2s)\n",
            right_cursor_enabled ? "ativo" : "inativo");

    while (!(exit_flag && *exit_flag) && !exit_requested) {
        double now = mono_seconds();
        double elapsed = now - last_tick;
        if (elapsed < 0.0 || elapsed > 0.05) elapsed = 0.05;
        last_tick = now;

        /* Eventos SDL: QUIT, hotplug, foco. */
        union { long double align; unsigned char bytes[128]; } ev;
        sdl.pump_events();
        while (sdl.poll_event(ev.bytes)) {
            uint32_t type; memcpy(&type, ev.bytes, sizeof type);
            if (type == BT_SDL_QUIT) {
                fprintf(stderr, "[bt/input] SDL_QUIT\n");
                exit_requested = 1;
            } else if (type == BT_SDL_CONTROLLERDEVICEADDED) {
                open_controllers();
            } else if (type == BT_SDL_JOYDEVICEREMOVED || type == BT_SDL_CONTROLLERDEVICEREMOVED) {
                int32_t which; memcpy(&which, ev.bytes + 8, sizeof which);
                if (type == BT_SDL_JOYDEVICEREMOVED && seam_adopted)
                    nxc6_forget((int)which);
                if (type == BT_SDL_CONTROLLERDEVICEREMOVED &&
                    nxinput_padset_remove_instance(&padset, which)) {
                    fprintf(stderr, "[bt/input] controller-removed instance=%d\n", (int)which);
                    bt_gptk_release_all("controller-removed");
                    release_all_keys();
                    open_controllers();
                }
            } else if (type == BT_SDL_WINDOWEVENT && ev.bytes[12] == BT_SDL_WINDOWEVENT_FOCUS_LOST) {
                bt_gptk_release_all("focus-lost");
                release_all_keys();
            }
        }
        if (exit_requested) break;

        sample_controls();
        update_engine_context(frame);

        if (padset.count == 0) {
            bt_gptk_release_all("controller-unavailable");
            release_all_keys();
            frame++;
            usleep(8000);
            continue;
        }

        /* Chord soberano SELECT+START: só no MESMO instance; lido antes do
         * GPTK e fora do arquivo do dono; nada dele vaza ao jogo. */
        int chord_select = 0, chord_start = 0;
        nxinput_padset_chord_inputs(&padset, &chord_select, &chord_start);
        if (nxinput_exit_chord_update(&exit_chord, chord_select, chord_start)) {
            (void)nxinput_exit_chord_consume(&exit_chord);
            fprintf(stderr, "[bt/input] SELECT+START: lifecycle exit requested\n");
            bt_gptk_release_all("exit-chord");
            release_all_keys();
            exit_requested = 1;
            break;
        }

        /* Despacho GPTK: transições físicas de botões e gatilhos. */
        for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
            if (c == NXINPUT_GPTK_LEFT_STICK || c == NXINPUT_GPTK_RIGHT_STICK)
                continue;
            float value = control_down[c] ? 1.0f : 0.0f;
            if (c == NXINPUT_GPTK_L2) value = trigger_value[0];
            if (c == NXINPUT_GPTK_R2) value = trigger_value[1];
            if (bt_gptk_feed_button(c, control_down[c], value) == BT_GPTK_LIVE_FATAL)
                input_fatal = 1;
        }
        /* Vetores: este port declara os dois sticks nativos; se um dono os
         * ligar a uma ação vetorial no futuro, o feed já está no lugar. */
        float lx = axis_norm(BT_SDL_AXIS_LEFTX), ly = axis_norm(BT_SDL_AXIS_LEFTY);
        float rx = axis_norm(BT_SDL_AXIS_RIGHTX), ry = axis_norm(BT_SDL_AXIS_RIGHTY);
        int left_consumed = bt_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK);
        int right_consumed = bt_gptk_should_consume(NXINPUT_GPTK_RIGHT_STICK);
        if (left_consumed) {
            float dx = lx, dy = ly; radial_deadzone(&dx, &dy);
            if (bt_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, dx, dy) == BT_GPTK_LIVE_FATAL) input_fatal = 1;
        }
        if (right_consumed) {
            float dx = rx, dy = ry; radial_deadzone(&dx, &dy);
            if (bt_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, dx, dy) == BT_GPTK_LIVE_FATAL) input_fatal = 1;
        }
        if (input_fatal) {
            fprintf(stderr, "[bt/input] FATAL no runtime vivo: encerrando sem reproduzir nativamente\n");
            release_all_keys();
            exit_requested = 1;
            break;
        }

        /* Cursor do analógico direito (nativo, P1) e R3 na borda. */
        int r3_down = control_down[NXINPUT_GPTK_R3] && !bt_gptk_should_consume(NXINPUT_GPTK_R3);
        if (right_cursor_enabled && !right_consumed) {
            float mag = sqrtf(rx * rx + ry * ry);
            const float dz = 7500.0f / 32767.0f;
            if (mag > dz) {
                float amount = (mag - dz) / (1.0f - dz);
                if (amount > 1.0f) amount = 1.0f;
                float speed = 120.0f * amount + 1000.0f * amount * amount;
                cursor_x += rx / mag * speed * (float)elapsed;
                cursor_y += ry / mag * speed * (float)elapsed;
                if (cursor_x < 0.0f) cursor_x = 0.0f;
                if (cursor_y < 0.0f) cursor_y = 0.0f;
                if (cursor_x > (float)(cursor_w - 1)) cursor_x = (float)(cursor_w - 1);
                if (cursor_y > (float)(cursor_h - 1)) cursor_y = (float)(cursor_h - 1);
                cursor_visible = 1;
                last_cursor_activity = now;
                sdv_egl_set_right_cursor(cursor_x, cursor_y, 1);
                if (cursor_touch_down) send_touch(2, cursor_x, cursor_y);
            }
        }
        if (right_cursor_enabled && r3_down && !r3_was_down) {
            if (cursor_visible) { r3_mode = 2; cursor_touch_down = 1; last_cursor_activity = now; send_touch(0, cursor_x, cursor_y); }
            else r3_mode = 1;
        }
        if (right_cursor_enabled && !r3_down && r3_was_down) {
            if (r3_mode == 2) { send_touch(1, cursor_x, cursor_y); cursor_touch_down = 0; last_cursor_activity = now; }
            r3_mode = 0;
        }
        r3_was_down = r3_down;
        if (right_cursor_enabled && cursor_visible && !cursor_touch_down && now - last_cursor_activity >= 2.0) {
            cursor_visible = 0;
            sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
        }

        /* Passthrough nativo dirigido por ESTADO (nunca dois donos). */
        for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
            int keycode = native_keycode(c);
            if (!keycode) continue;
            int desired = control_down[c] && !bt_gptk_should_consume(c);
            if (!desired && sink_key_pressed[keycode]) continue;
            deliver_key(keycode, desired);
        }
        /* R3 nativo = THUMBR só quando não é clique do cursor. */
        {
            int desired = (!right_cursor_enabled && r3_down) || r3_mode == 1;
            if (!(!desired && sink_key_pressed[AKEY_BUTTON_THUMBR]))
                deliver_key(AKEY_BUTTON_THUMBR, desired);
        }

        /* MotionEvent do quadro (sticks, gatilhos, HAT): fontes por eixo, só
         * quando algo mudou — o primeiro registra o device no MonoGame. */
        if (cb_motion && gamepad_event) {
            float ax = left_consumed ? 0.0f : lx, ay = left_consumed ? 0.0f : ly;
            float az = right_consumed ? 0.0f : rx, arz = right_consumed ? 0.0f : ry;
            float lt = bt_gptk_should_consume(NXINPUT_GPTK_L2) ? 0.0f : trigger_value[0];
            float rt = bt_gptk_should_consume(NXINPUT_GPTK_R2) ? 0.0f : trigger_value[1];
            int up = control_down[NXINPUT_GPTK_UP] && !bt_gptk_should_consume(NXINPUT_GPTK_UP);
            int dn = control_down[NXINPUT_GPTK_DOWN] && !bt_gptk_should_consume(NXINPUT_GPTK_DOWN);
            int lf = control_down[NXINPUT_GPTK_LEFT] && !bt_gptk_should_consume(NXINPUT_GPTK_LEFT);
            int rg = control_down[NXINPUT_GPTK_RIGHT] && !bt_gptk_should_consume(NXINPUT_GPTK_RIGHT);
            float axes[8] = { ax, ay, az, arz, lt, rt, (float)(rg - lf), (float)(dn - up) };
            if (!prev_axes_valid || memcmp(axes, prev_axes, sizeof axes) != 0) {
                jni_set_gamepad_motion_event(gamepad_event, BT_DEVICE_ID, ax, ay, az, arz, lt, rt, axes[6], axes[7]);
                cb_motion(jni_env, jni_view, gamepad_event);
                memcpy(prev_axes, axes, sizeof axes);
                prev_axes_valid = 1;
            }
        }

        frame++;
        usleep(8000);
    }
    if (cursor_touch_down) send_touch(3, cursor_x, cursor_y);
    sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
    bt_gptk_release_all("shutdown");
    release_all_keys();
    fprintf(stderr, "[bt/input] encerramento solicitado\n");
    nxinput_padset_close_all(&padset);
}
