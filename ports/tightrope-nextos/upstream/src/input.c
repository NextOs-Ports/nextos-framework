/*
 * input.c -- native controllers, delivered through SDL's own Android path.
 *
 * The SDL inside liblime is the Android build: its joystick backend is fed by
 * Java calling nativeAddJoystick and then onNativePadDown/Up, onNativeJoy and
 * onNativeHat from the UI thread.  This file is that UI thread.  It reads the
 * real pads with the system SDL2's GameController layer -- so NextOS' own
 * controller database, hotplug and per-device quirks all apply -- and forwards
 * each change through the exact entry points the Java code would have used.
 *
 * Nothing captures a button and nothing writes a mapping file: the pad arrives
 * already normalised by the system SDL, and the masks handed to
 * nativeAddJoystick make the game's SDL build the matching controller mapping
 * itself, which is what it does for a real Android gamepad.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include <SDL2/SDL.h>

#include "nx_elf.h"
#include "tr.h"
#include "probe_ring.h"
#include "input_adapter.h"

/* Android keycodes, as the game's SDL expects to receive them. */
#define AKEYCODE_BACK           4
#define AKEYCODE_DPAD_UP        19
#define AKEYCODE_DPAD_DOWN      20
#define AKEYCODE_DPAD_LEFT      21
#define AKEYCODE_DPAD_RIGHT     22
#define AKEYCODE_BUTTON_A       96
#define AKEYCODE_BUTTON_B       97
#define AKEYCODE_BUTTON_X       99
#define AKEYCODE_BUTTON_Y       100
#define AKEYCODE_BUTTON_L1      102
#define AKEYCODE_BUTTON_R1      103
#define AKEYCODE_BUTTON_L2      104
#define AKEYCODE_BUTTON_R2      105
#define AKEYCODE_BUTTON_THUMBL  106
#define AKEYCODE_BUTTON_THUMBR  107
#define AKEYCODE_BUTTON_START   108
#define AKEYCODE_BUTTON_SELECT  109
#define AKEYCODE_BUTTON_MODE    110

/* The game's SDL rebuilds a controller mapping from these two masks, indexed
 * by SDL_CONTROLLER_BUTTON_* and SDL_CONTROLLER_AXIS_*.  Declaring the full
 * standard layout is what an Android gamepad reports. */
#define BUTTON_MASK ( \
    (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_B) | \
    (1u << SDL_CONTROLLER_BUTTON_X) | (1u << SDL_CONTROLLER_BUTTON_Y) | \
    (1u << SDL_CONTROLLER_BUTTON_BACK) | \
    (1u << SDL_CONTROLLER_BUTTON_START) | \
    (1u << SDL_CONTROLLER_BUTTON_LEFTSTICK) | \
    (1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK) | \
    (1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER) | \
    (1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) | \
    (1u << SDL_CONTROLLER_BUTTON_DPAD_UP) | \
    (1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN) | \
    (1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT) | \
    (1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT))

#define AXIS_MASK ( \
    (1u << SDL_CONTROLLER_AXIS_LEFTX) | (1u << SDL_CONTROLLER_AXIS_LEFTY) | \
    (1u << SDL_CONTROLLER_AXIS_RIGHTX) | (1u << SDL_CONTROLLER_AXIS_RIGHTY) | \
    (1u << SDL_CONTROLLER_AXIS_TRIGGERLEFT) | \
    (1u << SDL_CONTROLLER_AXIS_TRIGGERRIGHT))

#define TR_DEVICE_ID 1
#define MAX_PADS 8

static const struct {
    SDL_GameControllerButton button;
    int keycode;
} button_map[] = {
    { SDL_CONTROLLER_BUTTON_A,             AKEYCODE_BUTTON_A },
    { SDL_CONTROLLER_BUTTON_B,             AKEYCODE_BUTTON_B },
    { SDL_CONTROLLER_BUTTON_X,             AKEYCODE_BUTTON_X },
    { SDL_CONTROLLER_BUTTON_Y,             AKEYCODE_BUTTON_Y },
    { SDL_CONTROLLER_BUTTON_BACK,          AKEYCODE_BUTTON_SELECT },
    { SDL_CONTROLLER_BUTTON_GUIDE,         AKEYCODE_BUTTON_MODE },
    { SDL_CONTROLLER_BUTTON_START,         AKEYCODE_BUTTON_START },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK,     AKEYCODE_BUTTON_THUMBL },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    AKEYCODE_BUTTON_THUMBR },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  AKEYCODE_BUTTON_L1 },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, AKEYCODE_BUTTON_R1 },
    { SDL_CONTROLLER_BUTTON_DPAD_UP,       AKEYCODE_DPAD_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     AKEYCODE_DPAD_DOWN },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     AKEYCODE_DPAD_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    AKEYCODE_DPAD_RIGHT },
};
#define BUTTON_MAP_N (sizeof button_map / sizeof *button_map)

static SDL_GameController *pads[MAX_PADS];
static SDL_JoystickID pad_ids[MAX_PADS];
static int pad_n;

/* Merged state across every connected pad, so a second controller does not
 * fight the first for the single Android device the game sees. */
static int held[BUTTON_MAP_N];
static int axis_value[SDL_CONTROLLER_AXIS_MAX];
static int registered;
static int quit_requested;
static int portable_mapping_used;
static struct tr_input_motion_state motion_state;
static _Atomic int gameplay_scene_active;

#define MAX_EXIT_PROBES 8
struct exit_probe {
    int fd;
    int select_code;
    int start_code;
};
static struct exit_probe exit_probes[MAX_EXIT_PROBES];
static int exit_probe_count;
static int exit_probe_initialized;

static void tr_input_autokey(void);
static void release_game_controls(void);

void tr_input_scene_loaded(const char *scene_name)
{
    int active = tr_input_scene_is_gameplay(scene_name);
    atomic_store_explicit(&gameplay_scene_active, active,
                          memory_order_release);
    fprintf(stderr, "[tr] input scene: %s -> %s controls\n",
            scene_name ? scene_name : "?", active ? "gameplay" : "menu");
}

static int input_bit_is_set(const unsigned long *bits, int code)
{
    return !!(bits[code / (8 * (int)sizeof(unsigned long))] &
              (1UL << (code % (8 * (int)sizeof(unsigned long)))));
}

/* Emergency SELECT+START path from the finite framework quirk registry.
 * It is armed only when SDL did not prove an authoritative BACK+START
 * mapping.  The event devices are read-only and selected by capabilities,
 * never by CFW, device name or path identity. */
static void exit_fallback_init(int mapping_has_exit)
{
    if (exit_probe_initialized)
        return;
    exit_probe_initialized = 1;
    if (mapping_has_exit)
        return;

    for (int i = 0; i < 64 && exit_probe_count < MAX_EXIT_PROBES; i++) {
        char path[64];
        unsigned long keybits[(KEY_MAX + 1 + 8 * sizeof(unsigned long) - 1) /
                              (8 * sizeof(unsigned long))];
        int select_code, start_code;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(keybits, 0, sizeof keybits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0 ||
            !tr_input_pick_exit_codes(
                keybits, sizeof keybits / sizeof keybits[0],
                &select_code, &start_code)) {
            close(fd);
            continue;
        }
        exit_probes[exit_probe_count].fd = fd;
        exit_probes[exit_probe_count].select_code = select_code;
        exit_probes[exit_probe_count].start_code = start_code;
        exit_probe_count++;
    }
    if (exit_probe_count)
        fprintf(stderr, "[tr] evdev SELECT+START fallback armed on %d "
                        "gamepad node(s)\n", exit_probe_count);
    else
        fprintf(stderr, "[tr] SDL mapping lacks SELECT+START and no safe "
                        "evdev fallback was readable\n");
}

static int exit_fallback_pressed(void)
{
    unsigned long state[(KEY_MAX + 1 + 8 * sizeof(unsigned long) - 1) /
                        (8 * sizeof(unsigned long))];
    for (int i = 0; i < exit_probe_count; i++) {
        memset(state, 0, sizeof state);
        if (ioctl(exit_probes[i].fd, EVIOCGKEY(sizeof state), state) < 0)
            continue;
        if (input_bit_is_set(state, exit_probes[i].select_code) &&
            input_bit_is_set(state, exit_probes[i].start_code))
            return 1;
    }
    return 0;
}

static void *sdl_native(const char *cls, const char *name)
{
    return tr_jni_native(cls, name);
}

static void pad_down(int keycode)
{
    void *fn = sdl_native("org/libsdl/app/SDLControllerManager",
                          "onNativePadDown");
    if (fn)
        ((int (*)(void *, void *, int, int))fn)(
            tr_jni_env(), tr_jret_class("org/libsdl/app/SDLControllerManager"),
            TR_DEVICE_ID, keycode);
}

static void pad_up(int keycode)
{
    void *fn = sdl_native("org/libsdl/app/SDLControllerManager",
                          "onNativePadUp");
    if (fn)
        ((int (*)(void *, void *, int, int))fn)(
            tr_jni_env(), tr_jret_class("org/libsdl/app/SDLControllerManager"),
            TR_DEVICE_ID, keycode);
}

static void joy_axis(int axis, float value)
{
    void *fn = sdl_native("org/libsdl/app/SDLControllerManager",
                          "onNativeJoy");
    if (fn)
        ((void (*)(void *, void *, int, int, float))fn)(
            tr_jni_env(), tr_jret_class("org/libsdl/app/SDLControllerManager"),
            TR_DEVICE_ID, axis, value);
}

/* --- the pad, on the game's own controls ---------------------------------
 *
 * Tightrope Theatre is a touch-first game: its settings screen offers no
 * controller options, and its in-level controls are three buttons drawn on the
 * screen -- left, right and jump -- plus pause.  It does carry Stencyl's
 * default gamepad table, but nothing above SDL consumes it: the game's own SDL
 * receives our axes and buttons correctly (measured) and the engine ignores
 * them.
 *
 * So the pad drives the game's controls instead of a second input path: a
 * direction holds a finger on that arrow for exactly as long as the direction
 * is held, and the buttons tap theirs.  Nothing is forced and no scene is
 * skipped -- from the game's side this is a player with very steady fingers.
 *
 * Positions are fractions of the window, measured on the real 1280x720 output,
 * so they follow whatever resolution the panel reports.
 */
#define BUTTON_LEFT_X   0.084f
#define BUTTON_LEFT_Y   0.853f
#define BUTTON_RIGHT_X  0.241f
#define BUTTON_RIGHT_Y  0.853f
#define BUTTON_JUMP_X   0.931f
#define BUTTON_JUMP_Y   0.853f
#define BUTTON_PAUSE_X  0.972f
#define BUTTON_PAUSE_Y  0.047f

/* One finger per control, so left/right and jump can be held together. */
enum { FINGER_MOVE = 1, FINGER_JUMP = 2, FINGER_PAUSE = 3,
       FINGER_MENU = 4 };

static void touch_raw(int finger, int action, float x, float y)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativeTouch");
    static int touch_added;
    void *env = tr_jni_env();
    void *cls = tr_jret_class("org/libsdl/app/SDLActivity");
    if (!fn)
        return;
    if (!touch_added) {
        void *add = sdl_native("org/libsdl/app/SDLActivity", "nativeAddTouch");
        if (add)
            ((void (*)(void *, void *, int, void *))add)(
                env, cls, 1, tr_jret_str("nextos-touch"));
        touch_added = 1;
    }
    nx_log("touch: finger=%d action=%d at %.3f,%.3f", finger, action, x, y);
    ((void (*)(void *, void *, int, int, int, float, float, float))fn)(
        env, cls, 1, finger, action, x, y, 1.0f);
}

/* Hold or release one finger at a fixed spot.  Re-sending the position while
 * held is what a real finger produces and what Stencyl's buttons expect. */
/* How long a renewing press stays down before it is pressed again; zero
 * means a plain hold.  Only the movement finger renews. */
static void finger_hold_pulsed(int finger, int down, float x, float y,
                               long pulse_ms);

static void finger_hold(int finger, int down, float x, float y)
{
    finger_hold_pulsed(finger, down, x, y, 0);
}

static void finger_hold_pulsed(int finger, int down, float x, float y,
                               long pulse_ms)
{
    static struct { int down; float x, y; struct timespec last; } state[5];
    if (finger < 0 || finger >= 5)
        return;
    if (down && !state[finger].down) {
        touch_raw(finger, 0, x, y);            /* ACTION_DOWN */
        state[finger].down = 1;
        clock_gettime(CLOCK_MONOTONIC, &state[finger].last);
    } else if (down && (state[finger].x != x || state[finger].y != y)) {
        touch_raw(finger, 1, state[finger].x, state[finger].y);  /* UP */
        touch_raw(finger, 0, x, y);                              /* DOWN */
        clock_gettime(CLOCK_MONOTONIC, &state[finger].last);
    } else if (down && pulse_ms > 0) {
        /* The game's arrow buttons act on the press, not on the press being
         * held: a finger that goes down and stays down moves the character
         * once and then nothing, while the same finger tapping repeatedly
         * moves it the whole time.  Measured on the device, both ways, with
         * the identical event stream -- so a held direction is delivered as a
         * press that renews itself.  The gap is short enough to read as one
         * continuous move and long enough for the button to see two presses. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - state[finger].last.tv_sec) * 1000 +
                  (now.tv_nsec - state[finger].last.tv_nsec) / 1000000;
        if (ms >= pulse_ms) {
            touch_raw(finger, 1, x, y);        /* UP  */
            touch_raw(finger, 0, x, y);        /* DOWN, again */
            state[finger].last = now;
        }
    } else if (!down && state[finger].down) {
        touch_raw(finger, 1, state[finger].x, state[finger].y);  /* UP */
        state[finger].down = 0;
    }
    if (down) {
        state[finger].x = x;
        state[finger].y = y;
    }
}

/* What the game's own SDL made of the device we registered.  liblime exports
 * its whole SDL, so this asks it directly instead of guessing: a device that is
 * a joystick but not a game controller means the mapping was never built from
 * the button and axis masks, and Lime's Gamepad layer will ignore it. */
static void report_game_view_of_pad(void)
{
    nx_mod *lime = nx_find_mod("liblime.so");
    if (!lime)
        return;
    int (*num)(void) = nx_lookup_in(lime, "SDL_NumJoysticks");
    const char *(*name_of)(int) = nx_lookup_in(lime, "SDL_JoystickNameForIndex");
    uint8_t (*is_gc)(int) = nx_lookup_in(lime, "SDL_IsGameController");
    char *(*mapping_of)(int) =
        nx_lookup_in(lime, "SDL_GameControllerMappingForDeviceIndex");
    if (!num) {
        nx_log("input: the game's SDL does not export SDL_NumJoysticks");
        return;
    }
    int n = num();
    fprintf(stderr, "[tr] the game's SDL sees %d joystick(s)\n", n);
    for (int i = 0; i < n; i++) {
        const char *nm = name_of ? name_of(i) : NULL;
        int gc = is_gc ? is_gc(i) : 0;
        char *map = (gc && mapping_of) ? mapping_of(i) : NULL;
        fprintf(stderr, "[tr]   joystick %d: \"%s\" gamecontroller=%s\n",
                i, nm ? nm : "?", gc ? "yes" : "NO");
        if (map)
            fprintf(stderr, "[tr]   mapping: %s\n", map);
    }
}

/* Android registers the device from SDLControllerManager.pollInputDevices(),
 * which SDL calls once its joystick subsystem exists.  Doing it any earlier
 * means calling into a subsystem whose locks have not been created yet. */
void tr_input_register(void)
{
    if (registered || !pad_n)
        return;
    if (getenv("TR_NO_PAD")) {
        fprintf(stderr, "[tr] TR_NO_PAD: not telling the game about any pad\n");
        return;
    }
    /* On a phone the pad is usually already attached when the game starts, but
     * a pad announced during SDL's own joystick init lands before the game has
     * registered its device-added handler, and the game never learns about it.
     * TR_PAD_DELAY defers the announcement so it arrives as a plain hotplug,
     * which every engine handles. */
    {
        static int delay_ms = -1;
        static struct timespec first;
        struct timespec now;
        if (delay_ms < 0) {
            const char *v = getenv("TR_PAD_DELAY");
            delay_ms = v && *v ? atoi(v) : 0;
        }
        if (delay_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!first.tv_sec) {
                first = now;
                return;
            }
            long ms = (now.tv_sec - first.tv_sec) * 1000 +
                      (now.tv_nsec - first.tv_nsec) / 1000000;
            if (ms < delay_ms)
                return;
        }
    }
    void *fn = sdl_native("org/libsdl/app/SDLControllerManager",
                          "nativeAddJoystick");
    if (!fn) {
        nx_log("input: the game's SDL has no nativeAddJoystick");
        return;
    }
    void *name = tr_jret_str("NextOS Controller");
    void *desc = tr_jret_str("nextos-native-controller");
    ((int (*)(void *, void *, int, void *, void *, int, int, uint8_t,
              int, int, int, int, int))fn)(
        tr_jni_env(), tr_jret_class("org/libsdl/app/SDLControllerManager"),
        TR_DEVICE_ID, name, desc,
        0x0000, 0x0000,      /* no vendor/product: a generic Android pad */
        0,                   /* not an accelerometer */
        (int)BUTTON_MASK,
        SDL_CONTROLLER_AXIS_MAX, (int)AXIS_MASK,
        0,                   /* hats: the d-pad is reported as buttons */
        0);                  /* balls */
    registered = 1;
    fprintf(stderr, "[tr] controller registered with the game's SDL\n");
    report_game_view_of_pad();
    void tr_install_event_spy(void);
    tr_install_event_spy();
}

/* [EVSPY] o laco principal do Lime dorme em SDL_WaitEvent e so acorda quando
 * um evento entra na fila.  Na segunda tela isso acontece a cada ~2,4s -- este
 * espiao, pendurado no SDL_AddEventWatch do PROPRIO jogo, nomeia o evento que
 * o acorda (e, por omissao, o que deixou de chegar).  Diagnostico. */
static int tr_event_spy(void *ud, void *ev)
{
    (void)ud;
    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long gap = last.tv_sec ? (now.tv_sec - last.tv_sec) * 1000 +
                             (now.tv_nsec - last.tv_nsec) / 1000000 : 0;
    uint32_t type = *(uint32_t *)ev;
    if (gap > 500)
        nx_log("[EVSPY] tipo=0x%x quebrou %ldms de silencio", type, gap);
    last = now;
    return 1;
}

void tr_install_event_spy(void)
{
    nx_mod *lime = nx_find_mod("liblime.so");
    if (!lime)
        return;
    void (*add_watch)(int (*)(void *, void *), void *) =
        nx_lookup_in(lime, "SDL_AddEventWatch");
    if (!add_watch) {
        nx_log("[EVSPY] SDL_AddEventWatch nao exportado");
        return;
    }
    add_watch(tr_event_spy, NULL);
    nx_log("[EVSPY] instalado");
}

/* [METRONOMO] o laco principal do Lime dorme em WaitEvent e conta com o timer
 * do SDL para poste-lo um evento por frame.  Na troca para a segunda tela essa
 * re-programacao se perde: a thread de timer fica com deadline longe demais,
 * evento nenhum chega (medido com o [EVSPY]: zero eventos) e o jogo so desperta
 * no fallback de ~2,4s -- um frame a cada 2,4s.
 *
 * Este metronomo assume a batida SO quando ela falta: se o jogo passar mais de
 * 20ms sem apresentar frame, postamos o mesmo SDL_USEREVENT que o timer do
 * Lime postaria.  Quando o timer nativo esta vivo (primeira tela, gameplay),
 * os frames avancam e o metronomo fica mudo.  O Lime segue decidindo se
 * renderiza (a checagem de tempo e interna), entao isto nao acelera nada.
 * TR_NO_METRONOME=1 desliga, para medicao. */
void tr_metronome_tick(void)
{
    static int (*push)(void *);
    static int off = -1;
    static unsigned long last_swaps;
    static struct timespec last_change;
    static unsigned long beats;

    /* LIGADO por padrao, com os gates de armamento tardio logo abaixo.  O
     * timer de frame do Lime e one-shot (AddTimer(nextUpdate-currentUpdate) +
     * trava timerActive): UM calculo podre envenena a cadeia para sempre e o
     * jogo fica refem do timeout de ~2,4s do WaitEvent.  O clamp do
     * [FRAMERATE] conserta o periodo, mas nao desenvenena a cadeia -- a
     * batida daqui e quem vira o relogio.  Milhares de batidas validadas nos
     * testes; o unico crash de batida foi CEDO demais (antes do Application
     * do Lime existir), que os gates impedem.  O que NAO pode e chamar
     * set_frame_rate/haxe direto desta thread (SIGSEGV addr=0x8, watchdog
     * morto por isso).  TR_NO_METRONOME=1 desliga para medicao. */
    if (off < 0)
        off = getenv("TR_NO_METRONOME") ? 1 : 0;
    if (off)
        return;

    unsigned long swaps = tr_egl_swap_count();
    /* So arma quando o Application do Lime ja existe (ele proprio chamou o
     * set_frame_rate) e com 2s de frames rodados.  Batida cedo demais mata o
     * handler de USEREVENT em ponteiro nulo -- medido: SIGSEGV addr=0x8 na
     * batida 1 a 20ms do primeiro frame. */
    extern void *tr_lime_app;
    if (!tr_lime_app || swaps == 0)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* "Ja rodou um pouco" medido em TEMPO, nao em contagem de frames.  A regra
     * antiga era "120 swaps", calibrada no Mali-450, onde o jogo passa desse
     * numero antes de envenenar a cadeia do timer.  No R36S o Lime para no
     * frame ~30: com o gate de contagem o metronomo nunca armava e o jogo
     * ficava refem do timeout do WaitEvent.  O que o gate precisa garantir e
     * so' que o Application do Lime ja exista e esteja rodando -- 1 segundo
     * de frames apresentados diz isso em qualquer aparelho. */
    static struct timespec first_swap;
    if (!first_swap.tv_sec) {
        first_swap = now;
        return;
    }
    long since_first = (now.tv_sec - first_swap.tv_sec) * 1000 +
                       (now.tv_nsec - first_swap.tv_nsec) / 1000000;
    if (since_first < 1000 || swaps < 8)
        return;
    if (swaps != last_swaps || !last_change.tv_sec) {
        last_swaps = swaps;
        last_change = now;
        return;
    }
    long ms = (now.tv_sec - last_change.tv_sec) * 1000 +
              (now.tv_nsec - last_change.tv_nsec) / 1000000;
    if (ms < 20)
        return;

    if (!push) {
        nx_mod *lime = nx_find_mod("liblime.so");
        if (lime)
            push = nx_lookup_in(lime, "SDL_PushEvent");
        if (!push)
            return;
    }
    /* (Houve um watchdog aqui que chamava set_frame_rate desta thread.
     * Morreu por SIGSEGV addr=0x8: cffi do hxcpp exige thread com contexto de
     * GC do Haxe.  Nunca ressuscitar por este caminho. */

    unsigned char ev[56];
    memset(ev, 0, sizeof ev);
    *(uint32_t *)ev = 0x8000;         /* SDL_USEREVENT: a batida do OnTimer */
    int rc = push(ev);
    tr_probe_note(TR_PROBE_EVENT, 0x8000, (uint32_t)rc);
    if (++beats == 1 || beats % 512 == 0) {
        /* auditoria do proprio metronomo: o push entrou?  a fila que o jogo
         * le tem os eventos?  (rc 1=entrou, 0=filtrado, <0=erro) */
        int queued = -1;
        uint32_t t[4] = { 0, 0, 0, 0 };
        const char *err = "";
        nx_mod *lime = nx_find_mod("liblime.so");
        if (lime) {
            int (*peep)(void *, int, int, uint32_t, uint32_t) =
                nx_lookup_in(lime, "SDL_PeepEvents");
            const char *(*geterr)(void) = nx_lookup_in(lime, "SDL_GetError");
            unsigned char buf[4][56];
            /* action 1 = SDL_PEEKEVENT (0 seria ADDEVENT: a 1a versao desta
             * auditoria poluiu a fila com lixo por causa disso) */
            if (peep) {
                queued = peep(buf, 4, 1, 0, 0xffff);
                for (int i = 0; i < 4 && i < queued; i++)
                    t[i] = *(uint32_t *)buf[i];
            }
            if (rc <= 0 && geterr)
                err = geterr();
        }
        nx_log("[METRONOMO] batida %lu (%ldms sem frame) push=%d fila=%d "
               "tipos=%x,%x,%x,%x %s",
               beats, ms, rc, queued, t[0], t[1], t[2], t[3], err);
    }
    /* nao rearma last_change: enquanto o frame nao vier, bate a cada tick do
     * laco de input (~4ms), que e proximo do periodo de frame do jogo. */
}

/* --- completar o mapping a partir do que o KERNEL expoe -------------------
 *
 * Medido no R36S/ArkOS: a SDL calcula para o "GO-Super Gamepad" o GUID
 * 1900bb3e4b48..., enquanto a linha do gamecontrollerdb do PortMaster tem
 * 190000004b48... -- diferem no campo de CRC.  Sem casar, a SDL monta um
 * mapping automatico que declara SO a0-a3 e b0-b11: fica SEM SELECT, START,
 * L3 e R3.  Isso nao quebra so o clique do R3; quebra o SELECT+START, que e' a
 * saida do jogo em todo port NextOS.
 *
 * A saida honesta nao e' escrever um mapping por nome de aparelho -- e' ler o
 * que o kernel realmente tem.  O indice de botao que a SDL usa e' a POSICAO do
 * codigo BTN_* na lista ordenada de teclas que o evdev declara; entao, sabendo
 * essa lista, sabemos exatamente qual indice e' cada botao, em qualquer pad.
 *
 * Nestes handhelds SELECT/START/L3/R3 costumam vir como BTN_TRIGGER_HAPPY*
 * em vez de BTN_SELECT/BTN_START/BTN_THUMB*, que e' justamente o motivo de o
 * mapping automatico ignorar.  Procuramos os dois jeitos. */
#define TR_BTN_SELECT   0x13a
#define TR_BTN_START    0x13b
#define TR_BTN_THUMBL   0x13d
#define TR_BTN_THUMBR   0x13e
#define TR_BTN_HAPPY1   0x2c0

/* Devolve o indice SDL do codigo evdev, ou -1. */
static int evdev_button_index(const unsigned long *keybits, int nbits, int code)
{
    int idx = 0;
    for (int c = 0; c < nbits; c++) {
        if (!(keybits[c / (8 * (int)sizeof(long))] &
              (1UL << (c % (8 * (int)sizeof(long))))))
            continue;
        if (c == code)
            return idx;
        idx++;
    }
    return -1;
}

/* Acha o /dev/input/eventN cujo nome bate com o do joystick. */
static int evdev_open_by_name(const char *name)
{
    if (!name || !*name)
        return -1;
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char nm[256] = "";
        if (ioctl(fd, EVIOCGNAME(sizeof nm), nm) >= 0 && strcmp(nm, name) == 0)
            return fd;
        close(fd);
    }
    return -1;
}

/* SDL_GameControllerHasButton arrived in SDL 2.0.14, while supported CFWs may
 * still ship the public 2.0.4 floor.  The applied mapping string is available
 * in that floor and is also the authoritative source for these bindings. */
static int controller_has_binding(SDL_GameController *gc, const char *key)
{
    char needle[48];
    char *mapping;
    char *hit;
    int present = 0;

    if (!gc || !key || !*key)
        return 0;
    snprintf(needle, sizeof needle, ",%s:", key);
    mapping = SDL_GameControllerMapping(gc);
    if (!mapping)
        return 0;
    hit = strstr(mapping, needle);
    if (hit) {
        hit += strlen(needle);
        present = *hit && *hit != ',';
    }
    SDL_free(mapping);
    return present;
}

static int complete_mapping(SDL_GameController *gc, SDL_JoystickGUID guid,
                            const char *name)
{
    /* So age quando falta algo essencial: um pad ja correto nao e' tocado. */
    int miss_sel = !controller_has_binding(gc, "back");
    int miss_sta = !controller_has_binding(gc, "start");
    int miss_r3  = !controller_has_binding(gc, "rightstick");
    int miss_l3  = !controller_has_binding(gc, "leftstick");
    if (!miss_sel && !miss_sta && !miss_r3 && !miss_l3)
        return 0;

    int fd = evdev_open_by_name(name);
    if (fd < 0) {
        nx_log("[pad] mapping incompleto e nao achei o evdev de '%s'",
               name ? name : "?");
        return 0;
    }
    unsigned long keybits[(0x300 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
    memset(keybits, 0, sizeof keybits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0) {
        close(fd);
        return 0;
    }
    close(fd);
    const int nbits = 0x300;

    /* Preferir o codigo "certo"; cair no trigger_happy quando o pad usa ele. */
    struct { int miss; const char *key; int code, alt; } add[] = {
        { miss_sel, "back",       TR_BTN_SELECT, TR_BTN_HAPPY1 + 0 },
        { miss_sta, "start",      TR_BTN_START,  TR_BTN_HAPPY1 + 1 },
        { miss_l3,  "leftstick",  TR_BTN_THUMBL, TR_BTN_HAPPY1 + 2 },
        { miss_r3,  "rightstick", TR_BTN_THUMBR, TR_BTN_HAPPY1 + 3 },
    };

    char *base = SDL_GameControllerMapping(gc);
    char out[1024];
    int n = snprintf(out, sizeof out, "%s", base ? base : "");
    if (base) SDL_free(base);
    if (n <= 0 || n >= (int)sizeof out)
        return 0;

    int added = 0;
    for (size_t i = 0; i < sizeof add / sizeof *add; i++) {
        if (!add[i].miss)
            continue;
        int b = evdev_button_index(keybits, nbits, add[i].code);
        if (b < 0)
            b = evdev_button_index(keybits, nbits, add[i].alt);
        if (b < 0)
            continue;                  /* o aparelho realmente nao tem */
        int w = snprintf(out + n, sizeof out - n, ",%s:b%d", add[i].key, b);
        if (w <= 0 || n + w >= (int)sizeof out)
            break;
        n += w;
        nx_log("[pad] completando %s -> b%d (do evdev)", add[i].key, b);
        added++;
    }
    if (!added)
        return 0;

    char gs[64] = "";
    SDL_JoystickGetGUIDString(guid, gs, sizeof gs);
    if (SDL_GameControllerAddMapping(out) < 0) {
        nx_log("[pad] AddMapping falhou: %s", SDL_GetError());
        return 0;
    }
    nx_log("[pad] mapping completado para %s", gs);
    return 1;
}

static void open_pad(int index)
{
    if (pad_n == MAX_PADS)
        return;

    /* Keep enough evidence in the ordinary game log to diagnose a firmware
     * mapping without asking the owner to install evtest or expose a device
     * path.  Names/GUIDs describe SDL's view only; they never select a quirk. */
    {
        SDL_Joystick *probe = SDL_JoystickOpen(index);
        SDL_JoystickGUID probe_guid = SDL_JoystickGetDeviceGUID(index);
        char guid_text[64] = "";
        SDL_JoystickGetGUIDString(probe_guid, guid_text, sizeof guid_text);
        fprintf(stderr,
                "[tr] joystick %d inventory: name=\"%s\" guid=%s "
                "controller=%s axes=%d buttons=%d hats=%d\n",
                index,
                SDL_JoystickNameForIndex(index) ?
                    SDL_JoystickNameForIndex(index) : "?",
                guid_text,
                SDL_IsGameController(index) ? "yes" : "NO",
                probe ? SDL_JoystickNumAxes(probe) : -1,
                probe ? SDL_JoystickNumButtons(probe) : -1,
                probe ? SDL_JoystickNumHats(probe) : -1);
        if (probe)
            SDL_JoystickClose(probe);
    }

    if (!SDL_IsGameController(index)) {
        SDL_Joystick *raw = SDL_JoystickOpen(index);
        if (raw) {
            SDL_JoystickGUID guid = SDL_JoystickGetGUID(raw);
            char guid_text[64] = "";
            char mapping[1024];
            int axes = SDL_JoystickNumAxes(raw);
            int buttons = SDL_JoystickNumButtons(raw);
            int hats = SDL_JoystickNumHats(raw);
            SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
            SDL_JoystickClose(raw);
            if (tr_input_build_raw_mapping(mapping, sizeof mapping, guid_text,
                                           axes, buttons, hats) &&
                SDL_GameControllerAddMapping(mapping) >= 0) {
                portable_mapping_used = 1;
                fprintf(stderr, "[tr] raw pad %d recovered by proven "
                                "topology (%d axes, %d buttons, %d hats)\n",
                        index, axes, buttons, hats);
            }
        }
    }
    if (!SDL_IsGameController(index)) {
        fprintf(stderr, "[tr] joystick %d has no safe controller mapping; "
                        "leaving it untouched\n", index);
        return;
    }

    /* 🚨 O MESMO aparelho pode ser enumerado DUAS vezes (js0 e eventN).  Medido
     * no R36S/ArkOS: o "GO-Super Gamepad" abria como pad 0 e pad 1, e como o
     * merge abaixo fica com o MAIOR valor absoluto entre os pads, o lixo da
     * segunda instancia vencia o eixo parado -- axisX=30437 sem ninguem tocar
     * no controle.  Isso virava "andar para a direita" e martelava um toque na
     * seta de gameplay por cima da tela de idiomas, impedindo o jogo de sair
     * dela.  Recusar duplicata pelo GUID resolve na origem. */
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    for (int i = 0; i < pad_n; i++) {
        SDL_JoystickGUID have =
            SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pads[i]));
        if (memcmp(&have, &guid, sizeof guid) == 0) {
            fprintf(stderr, "[tr] pad %d ignorado: mesmo GUID do pad %d "
                            "(aparelho enumerado duas vezes)\n", index, i);
            return;
        }
    }

    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc) {
        fprintf(stderr, "[tr] SDL_GameControllerOpen(%d): %s\n",
                index, SDL_GetError());
        return;
    }

    /* Um mapping so passa a valer na PROXIMA abertura, entao completar e
     * reabrir -- e' isso que faz o SELECT+START e o R3 existirem de verdade. */
    if (complete_mapping(gc, guid, SDL_GameControllerName(gc))) {
        SDL_GameControllerClose(gc);
        gc = SDL_GameControllerOpen(index);
        if (!gc)
            return;
    }

    SDL_Joystick *js = SDL_GameControllerGetJoystick(gc);
    pads[pad_n] = gc;
    pad_ids[pad_n] = SDL_JoystickInstanceID(js);
    pad_n++;
    fprintf(stderr, "[tr] pad %d: %s\n", pad_n - 1,
            SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?");

    /* O mapping que a SDL DO SISTEMA aplicou -- e nao o que esta no arquivo.
     * Sem isto nao da para saber se um botao "nao funciona" porque o aparelho
     * nao o tem, porque o GUID nao casou com nenhuma linha do banco, ou porque
     * a linha existe mas nao declara aquele botao.  Cada caso pede um remedio
     * diferente, e adivinhar custou uma sessao inteira. */
    {
        char gs[64] = "";
        SDL_JoystickGetGUIDString(guid, gs, sizeof gs);
        char *m = SDL_GameControllerMapping(gc);
        fprintf(stderr, "[tr]   guid=%s\n[tr]   mapping=%s\n", gs,
                m ? m : "(NENHUM: a SDL montou um padrao)");
        /* Quais botoes deste pad a SDL diz TER -- e' o que decide se R3
         * existe aqui. */
        static const struct { const char *key, *n; } want[] = {
            { "rightshoulder", "R1" },
            { "rightstick",    "R3" },
            { "leftstick",     "L3" },
            { "back",          "SELECT" },
            { "start",         "START" },
        };
        for (size_t k = 0; k < sizeof want / sizeof *want; k++)
            fprintf(stderr, "[tr]   %-6s presente=%s\n", want[k].n,
                    controller_has_binding(gc, want[k].key) ? "sim" : "NAO");
        if (m) SDL_free(m);
    }
}

static void close_pad(SDL_JoystickID id)
{
    for (int i = 0; i < pad_n; i++) {
        if (pad_ids[i] != id)
            continue;
        SDL_GameControllerClose(pads[i]);
        for (int k = i; k + 1 < pad_n; k++) {
            pads[k] = pads[k + 1];
            pad_ids[k] = pad_ids[k + 1];
        }
        pad_n--;
        if (!pad_n)
            release_game_controls();
        return;
    }
}

static void apply_mapping_lines(const char *value)
{
    char *copy, *line, *save = NULL;
    if (!value || !*value)
        return;
    copy = SDL_strdup(value);
    if (!copy)
        return;
    for (line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        if (*line && *line != '#')
            SDL_GameControllerAddMapping(line);
    }
    SDL_free(copy);
}

int tr_input_init(void)
{
    /* The audio bridge may have brought SDL up already; either way the driver
     * choice stays with the system SDL. */
    const Uint32 input_flags = SDL_INIT_JOYSTICK |
                               SDL_INIT_GAMECONTROLLER |
                               SDL_INIT_EVENTS;
    if (SDL_InitSubSystem(input_flags) != 0) {
        fprintf(stderr, "[tr] SDL input init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    /* Reapply the firmware mapping explicitly.  SDL versions on supported
     * CFWs differ in whether they consume *_CONFIG_FILE themselves, and the
     * 1.0.1 launcher used to copy this same database beside the loader. */
    char db[1200];
    const char *mapping_file = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
    const char *env = getenv("SDL_GAMECONTROLLERCONFIG");
    if (mapping_file && *mapping_file) {
        int loaded = SDL_GameControllerAddMappingsFromFile(mapping_file);
        fprintf(stderr, "[tr] controller database %s: %d mapping(s)\n",
                mapping_file, loaded);
    }
    apply_mapping_lines(env && *env ? env : getenv("sdl_controllerconfig"));
    snprintf(db, sizeof db, "%s/gamecontrollerdb.txt", tr_gamedir);
    if (SDL_GameControllerAddMappingsFromFile(db) > 0)
        nx_log("input: loaded %s", db);

    int joystick_count = SDL_NumJoysticks();
    fprintf(stderr, "[tr] system SDL sees %d joystick(s) before game start\n",
            joystick_count);
    for (int i = 0; i < joystick_count; i++)
        open_pad(i);

    int authoritative_exit = pad_n > 0 && !portable_mapping_used;
    for (int i = 0; i < pad_n && authoritative_exit; i++)
        authoritative_exit = controller_has_binding(pads[i], "back") &&
                             controller_has_binding(pads[i], "start");
    exit_fallback_init(authoritative_exit);
    return 0;
}

static void publish_button(size_t i, int down)
{
    if (held[i] == down)
        return;
    held[i] = down;
    if (down)
        pad_down(button_map[i].keycode);
    else
        pad_up(button_map[i].keycode);
}

static void publish_axis(int axis, int raw)
{
    if (axis_value[axis] == raw)
        return;
    axis_value[axis] = raw;
    /* SDL's Android backend multiplies by 32767 on the way back in, so the
     * value crossing this boundary is normalised. */
    joy_axis(axis, (float)raw / 32767.0f);
}

/* --- the finger, for the touch-only screens ------------------------------
 *
 * The menus have no controller path at all: they are actors that respond to a
 * touch, and their positions change from screen to screen, so there is nothing
 * fixed to map a button onto.  The right stick therefore moves a finger and R1
 * presses it -- an aid for the screens that need it, not the way the game is
 * played: in a level the pad drives the game's own controls directly and the
 * finger stays out of the way.
 */
static float cursor_x, cursor_y;
static struct timespec cursor_seen;
static int cursor_pressed;

/* A seta e FIXA: fica na tela do inicio ao fim.  Ela sumir depois de alguns
 * segundos era o que fazia perder a referencia entre uma tela e outra --
 * e, pior, devolvia o dpad ao jogo sem aviso. */

static void update_cursor(const int *merged, const int *axis)
{
    static float last_sent_x = -1, last_sent_y = -1;
    static struct timespec last_move;
    const float SPEED = 900.0f;          /* pixels per second at full tilt */
    const float DEADZONE = 0.275f;
    const float SMOOTHING = 14.0f;
    static float smooth_x, smooth_y;
    static struct timespec last;
    static int tapping;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    float dt = last.tv_sec ? (float)(now.tv_sec - last.tv_sec) +
                             (float)(now.tv_nsec - last.tv_nsec) / 1e9f : 0.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;

    int w = tr_screen_width(), h = tr_screen_height();
    if (!cursor_seen.tv_sec) {
        cursor_x = w / 2.0f;
        cursor_y = h / 2.0f;
    }

    float raw_x = axis[SDL_CONTROLLER_AXIS_RIGHTX] / 32767.0f;
    float raw_y = axis[SDL_CONTROLLER_AXIS_RIGHTY] / 32767.0f;
    float magnitude = hypotf(raw_x, raw_y);
    float target_x = 0.0f, target_y = 0.0f;
    if (magnitude > DEADZONE) {
        float clamped = magnitude > 1.0f ? 1.0f : magnitude;
        float response = (clamped - DEADZONE) / (1.0f - DEADZONE);
        /* Smoothstep keeps fine aiming gentle while still reaching full
         * speed. Direction remains radial, including diagonals. */
        response = response * response * (3.0f - 2.0f * response);
        target_x = raw_x / magnitude * response;
        target_y = raw_y / magnitude * response;
    }
    float blend = dt > 0.0f ? 1.0f - expf(-SMOOTHING * dt) : 0.0f;
    smooth_x += (target_x - smooth_x) * blend;
    smooth_y += (target_y - smooth_y) * blend;
    if (fabsf(smooth_x) < 0.001f) smooth_x = 0.0f;
    if (fabsf(smooth_y) < 0.001f) smooth_y = 0.0f;
    if (smooth_x || smooth_y) {
        cursor_x += smooth_x * SPEED * dt;
        cursor_y += smooth_y * SPEED * dt;
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_x > w - 1) cursor_x = w - 1.0f;
        if (cursor_y > h - 1) cursor_y = h - 1.0f;
        cursor_seen = now;
    }

    /* R1 OU R3.  O R3 e o botao que a mao procura quando o polegar ja esta no
     * analogico direito movendo o dedo -- ele so era repassado ao jogo como
     * keycode e nunca chegava aqui.  O R1 continua valendo. */
    /* 🚨 A seta so e' desenhada dentro do eglSwapBuffers, e o OpenFL so
     * apresenta frame quando ALGO MUDA.  Numa tela estatica (a de idiomas, a
     * primeira de uma instalacao nova) o jogo desenha ~40 frames e para: a
     * posicao da seta continua andando por dentro, mas ninguem a redesenha, e
     * ela parece travada -- impossivel de mirar.
     *
     * Ficou escondido a sessao inteira porque no Mali-450 o save ja existia e
     * essa tela nunca mais aparecia; todo teste de cursor foi na tela de
     * titulo, que anima sozinha.
     *
     * A saida e' a que um mouse de verdade produz: avisar o MOVIMENTO do
     * ponteiro.  O OpenFL atualiza o estado de rollover, marca a cena como
     * suja e apresenta o frame -- entao a seta reaparece.  So quando a posicao
     * muda, e no maximo a cada 32ms, para nao virar enxurrada de eventos. */
    if (cursor_x != last_sent_x || cursor_y != last_sent_y) {
        long since = (now.tv_sec - last_move.tv_sec) * 1000 +
                     (now.tv_nsec - last_move.tv_nsec) / 1000000;
        if (!last_move.tv_sec || since >= 32) {
            void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativeMouse");
            if (fn)
                ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
                    tr_jni_env(),
                    tr_jret_class("org/libsdl/app/SDLActivity"),
                    0, 2 /* ACTION_MOVE */, cursor_x, cursor_y, 0);

            /* O movimento do ponteiro sozinho NAO suja toda tela: medido, a de
             * idiomas redesenha (213->236 frames) e a de opcoes nao (33->33).
             * O sinal que existe justamente para pedir redesenho e' o evento
             * de janela EXPOSED -- o Lime o trata marcando o frame como sujo.
             * Sem isto a seta fica parada em qualquer tela estatica, porque
             * ela so e' desenhada dentro do swap. */
            static int (*push)(void *);
            if (!push) {
                nx_mod *lime = nx_find_mod("liblime.so");
                if (lime)
                    push = nx_lookup_in(lime, "SDL_PushEvent");
            }
            if (push) {
                unsigned char ev[56];
                memset(ev, 0, sizeof ev);
                *(uint32_t *)ev = 0x200;          /* SDL_WINDOWEVENT       */
                *(uint32_t *)(ev + 8) = 1;        /* windowID              */
                ev[12] = 3;                       /* SDL_WINDOWEVENT_EXPOSED */
                push(ev);
            }

            last_sent_x = cursor_x;
            last_sent_y = cursor_y;
            last_move = now;
        }
    }

    int press = merged[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] ||
                merged[SDL_CONTROLLER_BUTTON_RIGHTSTICK];
    if (press && !tapping) {
        tapping = 1;
        cursor_pressed = 1;
        cursor_seen = now;
        nx_log("[dedo] press por %s em %.0f,%.0f",
               merged[SDL_CONTROLLER_BUTTON_RIGHTSTICK] ? "R3" : "R1",
               cursor_x, cursor_y);
        /* press_screen, nao finger_hold: as telas deste jogo IGNORAM um dedo
         * que so encosta e fica.  O que elas aceitam e o que um dedo no
         * Android produz -- ponteiro parado no widget por alguns frames e
         * depois o toque -- e press_screen e exatamente esse caminho.  Custa
         * ~180ms, mas roda na thread de input do port, nao na de render. */
        void press_screen(float x, float y);
        press_screen(cursor_x, cursor_y);
    } else if (!press && tapping) {
        tapping = 0;
        cursor_pressed = 0;
    }
}

/* 1 enquanto o dedo esta pressionando, para o desenho mudar de cor e dar
 * retorno visual de que o toque saiu. */
int tr_input_cursor_pressed(void)
{
    return cursor_pressed;
}

/* Window coordinates of the finger, or 0 when it should not be drawn.  It
 * fades out a few seconds after the last use so a level never pays for it. */
/* Test-only: nudge and press the finger from a script, so an unattended run
 * can prove the menu path without a hand on the pad. */
void tr_input_cursor_test(int dx, int dy, int press)
{
    int w = tr_screen_width(), h = tr_screen_height();
    static int down;
    if (!cursor_seen.tv_sec) {
        cursor_x = w / 2.0f;
        cursor_y = h / 2.0f;
    }
    cursor_x += dx;
    cursor_y += dy;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x > w - 1) cursor_x = w - 1.0f;
    if (cursor_y > h - 1) cursor_y = h - 1.0f;
    clock_gettime(CLOCK_MONOTONIC, &cursor_seen);
    if (press) {
        finger_hold(FINGER_MENU, 1, cursor_x / w, cursor_y / h);
        down = 1;
        usleep(150000);
    }
    if (down && !press) {
        finger_hold(FINGER_MENU, 0, 0, 0);
        down = 0;
    }
}

int tr_input_cursor(float *x, float *y)
{
    /* Sempre desenhada.  Se ainda nao foi movida, nasce no meio da tela --
     * antes ela so aparecia depois do primeiro toque no analogico, e quem
     * nao soubesse disso concluia que a seta tinha sumido. */
    if (!cursor_seen.tv_sec) {
        cursor_x = tr_screen_width() / 2.0f;
        cursor_y = tr_screen_height() / 2.0f;
        clock_gettime(CLOCK_MONOTONIC, &cursor_seen);
    }
    *x = cursor_x;
    *y = cursor_y;
    return 1;
}

/* Translate the merged pad state into the game's own on-screen controls. */
static void drive_game_controls(const int *merged, const int *axis)
{
    struct tr_input_motion_result motion;

    /* These fixed touch points are real controls only in Level<digit>
     * scenes.  On Title/Settings/selection screens the same coordinates are
     * Options, Credits and More Games, so a direction must be neutral there. */
    if (!atomic_load_explicit(&gameplay_scene_active,
                              memory_order_acquire)) {
        release_game_controls();
        return;
    }

    tr_input_resolve_motion(
        &motion_state,
        axis[SDL_CONTROLLER_AXIS_LEFTX],
        axis[SDL_CONTROLLER_AXIS_LEFTY],
        merged[SDL_CONTROLLER_BUTTON_DPAD_LEFT],
        merged[SDL_CONTROLLER_BUTTON_DPAD_RIGHT],
        merged[SDL_CONTROLLER_BUTTON_DPAD_UP],
        merged[SDL_CONTROLLER_BUTTON_A],
        &motion);
    int jump = motion.jump;
    int pause = merged[6];

    static int last_dir, last_jump;
    int dir = motion.direction;
    if (dir != last_dir || jump != last_jump) {
        nx_log("pad: dir=%d jump=%d axisX=%d dpad=%d/%d/%d",
               dir, jump, axis[SDL_CONTROLLER_AXIS_LEFTX],
               merged[SDL_CONTROLLER_BUTTON_DPAD_LEFT],
               merged[SDL_CONTROLLER_BUTTON_DPAD_RIGHT],
               merged[SDL_CONTROLLER_BUTTON_DPAD_UP]);
        last_dir = dir;
        last_jump = jump;
    }

    const long MOVE_PULSE_MS = 250;
    if (dir < 0)
        finger_hold_pulsed(FINGER_MOVE, 1, BUTTON_LEFT_X, BUTTON_LEFT_Y,
                           MOVE_PULSE_MS);
    else if (dir > 0)
        finger_hold_pulsed(FINGER_MOVE, 1, BUTTON_RIGHT_X, BUTTON_RIGHT_Y,
                           MOVE_PULSE_MS);
    else
        finger_hold(FINGER_MOVE, 0, 0, 0);

    finger_hold(FINGER_JUMP, jump, BUTTON_JUMP_X, BUTTON_JUMP_Y);
    finger_hold(FINGER_PAUSE, pause, BUTTON_PAUSE_X, BUTTON_PAUSE_Y);
}

static void release_game_controls(void)
{
    tr_input_reset_motion(&motion_state);
    finger_hold(FINGER_MOVE, 0, 0, 0);
    finger_hold(FINGER_JUMP, 0, 0, 0);
    finger_hold(FINGER_PAUSE, 0, 0, 0);
}

void tr_input_poll(void)
{
    tr_input_autokey();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERDEVICEADDED)
            open_pad(e.cdevice.which);
        else if (e.type == SDL_JOYDEVICEADDED &&
                 !SDL_IsGameController(e.jdevice.which))
            open_pad(e.jdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
            close_pad(e.cdevice.which);
        else if (e.type == SDL_QUIT)
            quit_requested = 1;
    }

    /* This must precede the SDL-pad early return: the registered framework
     * quirk exists precisely for a mapping that could not expose the chord. */
    if (exit_fallback_pressed()) {
        if (!quit_requested) {
            fprintf(stderr, "[tr] SELECT+START (evdev fallback): asking the "
                            "game to quit\n");
            tr_input_request_quit();
        }
        return;
    }
    /* If a raw pad appeared after the Android SDL asked for its initial
     * device list, announce it now through the same native hotplug entrypoint.
     * tr_input_register() remains lifecycle-gated and idempotent. */
    if (pad_n && !registered)
        tr_input_register();
    if (!pad_n || !registered)
        return;

    /* Merge: a button is down if any pad holds it, an axis takes the largest
     * deflection.  One physical player, several possible controllers. */
    int merged[BUTTON_MAP_N];
    int merged_axis[SDL_CONTROLLER_AXIS_MAX];
    memset(merged, 0, sizeof merged);
    memset(merged_axis, 0, sizeof merged_axis);
    for (int p = 0; p < pad_n; p++) {
        for (size_t i = 0; i < BUTTON_MAP_N; i++)
            if (SDL_GameControllerGetButton(pads[p], button_map[i].button))
                merged[i] = 1;
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
            int v = SDL_GameControllerGetAxis(pads[p], a);
            if (abs(v) > abs(merged_axis[a]))
                merged_axis[a] = v;
        }
    }

    /* SELECT+START is the frontend's exit gesture on every NextOS port.  It
     * asks the game to quit through SDL's own quit event rather than killing
     * the process, so Lime runs its shutdown and the game writes its save. */
    int select_down = merged[SDL_CONTROLLER_BUTTON_BACK] ||
                      merged[SDL_CONTROLLER_BUTTON_GUIDE];
    int start_down = merged[SDL_CONTROLLER_BUTTON_START];
    if (select_down && start_down) {
        if (!quit_requested) {
            fprintf(stderr, "[tr] SELECT+START: asking the game to quit\n");
            quit_requested = 1;
            tr_input_request_quit();
        }
        return;
    }

    /* The D-pad belongs to the touch gameplay controls above.  Menus retain
     * the polished right-stick pointer plus R1/R3; publishing Android D-pad
     * keys as well would create a second, conflicting input path. */
    for (size_t i = 0; i < BUTTON_MAP_N; i++) {
        int v = merged[i];
        /* D-pad is consumed by drive_game_controls/finger_hold. */
        if (i == SDL_CONTROLLER_BUTTON_DPAD_UP ||
            i == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
            i == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
            i == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
            v = 0;
        /* [BOTAO] diz QUAL botao do SDL cada tecla fisica produz.  Sem isto a
         * conversa vira adivinhacao: nestes handhelds SELECT/START/L3/R3 nao
         * sao BTN_SELECT/BTN_START, e sim TRIGGER_HAPPY1..5 remapeados pelo
         * gamecontrollerdb -- entao o unico jeito honesto de saber e' ver o
         * que chega quando a mao aperta. */
        {
            static int was[BUTTON_MAP_N];
            if (merged[i] != was[i]) {
                was[i] = merged[i];
                if (merged[i])
                    nx_log("[BOTAO] SDL button %zu (keycode %d) pressionado",
                           i, button_map[i].keycode);
            }
        }
        publish_button(i, v);
    }
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        publish_axis(a, merged_axis[a]);

    drive_game_controls(merged, merged_axis);
    update_cursor(merged, merged_axis);
}

/* The game is a touch-first mobile title, so a test that only presses pad
 * buttons cannot tell "the game ignored the pad" from "the game is waiting for
 * a tap".  This sends a tap through SDL's own touch entry points, the same way
 * the Java view would. */
static void tap_screen(float x, float y)
{
    void *env = tr_jni_env();
    void *cls = tr_jret_class("org/libsdl/app/SDLActivity");
    static int touch_added;
    void *add = sdl_native("org/libsdl/app/SDLActivity", "nativeAddTouch");
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativeTouch");
    if (!fn)
        return;
    if (!touch_added && add) {
        ((void (*)(void *, void *, int, void *))add)(
            env, cls, 1, tr_jret_str("nextos-touch"));
        touch_added = 1;
    }
    /* action 0 = ACTION_DOWN, 1 = ACTION_UP; coordinates are normalised. */
    ((void (*)(void *, void *, int, int, int, float, float, float))fn)(
        env, cls, 1, 0, 0, x, y, 1.0f);
    usleep(60000);
    ((void (*)(void *, void *, int, int, int, float, float, float))fn)(
        env, cls, 1, 0, 1, x, y, 1.0f);
}

/* SDL also accepts the pointer as a mouse, which is the path OpenFL's UI
 * listens on when touch-to-mouse translation is not in play.  Coordinates are
 * window pixels here, not normalised. */
static void click_screen(float x, float y)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativeMouse");
    if (!fn)
        return;
    void *env = tr_jni_env();
    void *cls = tr_jret_class("org/libsdl/app/SDLActivity");
    /* action 0 = ACTION_DOWN, 1 = ACTION_UP, 2 = ACTION_MOVE. */
    /* Move first and let the game see the pointer there for a few frames: a
     * button that highlights on roll-over only accepts the press once it has
     * processed the motion. */
    for (int i = 0; i < 3; i++) {
        ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
            env, cls, 0, 2, x, y, 0);
        usleep(60000);
    }
    ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
        env, cls, 1, 0, x, y, 0);
    usleep(120000);
    ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
        env, cls, 1, 1, x, y, 0);
}

/* The game's screens do not all listen on the same pointer: some react to the
 * touch stream, others to the mouse.  A press that arrives on both, with the
 * pointer already parked on the widget, is what a finger on Android produces
 * (SDL raises a touch and synthesises the mouse from it), so that is what this
 * sends. */
void press_screen(float x, float y)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativeMouse");
    void *env = tr_jni_env();
    void *cls = tr_jret_class("org/libsdl/app/SDLActivity");
    float nx = x / (float)tr_screen_width();
    float ny = y / (float)tr_screen_height();

    if (fn)
        for (int i = 0; i < 3; i++) {
            ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
                env, cls, 0, 2, x, y, 0);
            usleep(40000);
        }
    if (fn)
        ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
            env, cls, 1, 0, x, y, 0);
    tap_screen(nx, ny);
    if (fn)
        ((void (*)(void *, void *, int, int, float, float, uint8_t))fn)(
            env, cls, 1, 1, x, y, 0);
}

/* Read the pad back out of the game's own SDL.  If the axes and buttons here
 * carry what we injected, everything below the engine is correct and anything
 * still wrong is above it -- that is the one measurement that splits the two. */
static void report_game_pad_state(void)
{
    static void *js;
    nx_mod *lime = nx_find_mod("liblime.so");
    if (!lime)
        return;
    if (!js) {
        void *(*open_js)(int) = nx_lookup_in(lime, "SDL_JoystickOpen");
        if (!open_js)
            return;
        js = open_js(0);
        if (!js) {
            const char *(*err)(void) = nx_lookup_in(lime, "SDL_GetError");
            fprintf(stderr, "[tr] the game's SDL_JoystickOpen(0) failed: %s\n",
                    err ? err() : "?");
            return;
        }
    }
    int16_t (*axis)(void *, int) = nx_lookup_in(lime, "SDL_JoystickGetAxis");
    uint8_t (*button)(void *, int) = nx_lookup_in(lime, "SDL_JoystickGetButton");
    int (*nhats)(void *) = nx_lookup_in(lime, "SDL_JoystickNumHats");
    uint8_t (*hat)(void *, int) = nx_lookup_in(lime, "SDL_JoystickGetHat");
    if (!axis || !button)
        return;
    fprintf(stderr, "[tr]   game's SDL pad: axis0=%d axis1=%d b0=%d b14=%d "
                    "hats=%d hat0=%d\n",
            axis(js, 0), axis(js, 1), button(js, 0), button(js, 14),
            nhats ? nhats(js) : -1,
            (nhats && nhats(js) > 0 && hat) ? hat(js, 0) : -1);
}

static void report_reaction(unsigned long before)
{
    usleep(700000);
    unsigned long after = tr_egl_swap_count();
    fprintf(stderr, "[tr] auto-input: the game drew %lu frames after that\n",
            after - before);
    if (getenv("TR_PADPROBE"))
        report_game_pad_state();
}

/* Test-only: walk the game's own UI on a timer so an unattended run can reach
 * gameplay and be photographed.  Everything goes through the same entry points
 * a real pad or a real finger uses -- no state is forced and no scene is
 * skipped.  Off unless TR_AUTOKEY names an interval in milliseconds.
 *
 * TR_AUTOSCRIPT is a ';'-separated list of steps, one per interval:
 *   "x,y"  tap at those normalised screen coordinates
 *   "kN"   press Android keycode N
 * Without it, the steps are a plain button walk. */
static void tr_input_autokey(void)
{
    static int interval = -1;
    static const char *script;
    static const int keys[] = {
        AKEYCODE_BUTTON_START, AKEYCODE_BUTTON_A, AKEYCODE_DPAD_DOWN,
        AKEYCODE_BUTTON_A,
    };
    static unsigned step;
    static struct timespec last;

    static int delay_ms;
    static struct timespec armed;

    if (interval < 0) {
        const char *v = getenv("TR_AUTOKEY");
        interval = v && *v ? atoi(v) : 0;
        v = getenv("TR_AUTODELAY");
        delay_ms = v && *v ? atoi(v) : 10000;
        script = getenv("TR_AUTOSCRIPT");
        if (interval > 0)
            fprintf(stderr, "[tr] auto-input every %d ms after %d ms "
                            "(test only)\n", interval, delay_ms);
    }
    if (interval <= 0 || !registered)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    /* Hold off until the game has settled: an event delivered before SDL has
     * a focused window is discarded, and that is indistinguishable from the
     * game ignoring it. */
    if (!armed.tv_sec) {
        armed = now;
        return;
    }
    long since_armed = (now.tv_sec - armed.tv_sec) * 1000 +
                       (now.tv_nsec - armed.tv_nsec) / 1000000;
    if (since_armed < delay_ms)
        return;
    long ms = (now.tv_sec - last.tv_sec) * 1000 +
              (now.tv_nsec - last.tv_nsec) / 1000000;
    if (last.tv_sec && ms < interval)
        return;
    last = now;

    /* A screen that redraws is a screen that noticed: OpenFL only presents a
     * frame when something changed, so counting swaps around the action says
     * whether it landed, without a screenshot. */
    unsigned long before_swaps = tr_egl_swap_count();

    unsigned n = step++;
    if (script) {
        const char *p = script;
        for (unsigned i = 0; i < n && p; i++) {
            p = strchr(p, ';');
            if (p)
                p++;
        }
        if (!p || !*p)
            return;                       /* script finished: stay put */
        float tx, ty;
        int key;
        if (*p == 'p' && sscanf(p + 1, "%f,%f", &tx, &ty) == 2) {
            fprintf(stderr, "[tr] auto-input: press at %.0f,%.0f\n", tx, ty);
            press_screen(tx, ty);
        } else if (*p == 'm' && sscanf(p + 1, "%f,%f", &tx, &ty) == 2) {
            fprintf(stderr, "[tr] auto-input: click at %.0f,%.0f\n", tx, ty);
            click_screen(tx, ty);
        } else if (*p == 'c') {
            fprintf(stderr, "[tr] auto-input: finger '%c'\n", p[1]);
            switch (p[1]) {
            case 'l': tr_input_cursor_test(-90, 0, 0); break;
            case 'r': tr_input_cursor_test(90, 0, 0); break;
            case 'u': tr_input_cursor_test(0, -90, 0); break;
            case 'd': tr_input_cursor_test(0, 90, 0); break;
            case 'p': tr_input_cursor_test(0, 0, 1);
                      tr_input_cursor_test(0, 0, 0); break;
            default: break;
            }
        } else if (*p == 'g') {
            /* Exercise the pad-to-controls mapping without a physical pad:
             * gl/gr hold a direction, gn releases it, gj jumps. */
            fprintf(stderr, "[tr] auto-input: game control '%c'\n", p[1]);
            switch (p[1]) {
            case 'l': finger_hold(FINGER_MOVE, 1, BUTTON_LEFT_X,
                                  BUTTON_LEFT_Y); break;
            case 'r': finger_hold(FINGER_MOVE, 1, BUTTON_RIGHT_X,
                                  BUTTON_RIGHT_Y); break;
            case 'n': finger_hold(FINGER_MOVE, 0, 0, 0); break;
            case 'j':
                finger_hold(FINGER_JUMP, 1, BUTTON_JUMP_X, BUTTON_JUMP_Y);
                usleep(120000);
                finger_hold(FINGER_JUMP, 0, 0, 0);
                break;
            default: break;
            }
        } else if (*p == 'a' && sscanf(p + 1, "%d,%f", &key, &tx) == 2) {
            fprintf(stderr, "[tr] auto-input: axis %d -> %.2f\n", key, tx);
            joy_axis(key, tx);
        } else if (*p == 'd' && sscanf(p + 1, "%d", &key) == 1) {
            fprintf(stderr, "[tr] auto-input: holding keycode %d\n", key);
            pad_down(key);
        } else if (*p == 'u' && sscanf(p + 1, "%d", &key) == 1) {
            fprintf(stderr, "[tr] auto-input: releasing keycode %d\n", key);
            pad_up(key);
        } else if (*p == 'k' && sscanf(p + 1, "%d", &key) == 1) {
            fprintf(stderr, "[tr] auto-input: keycode %d\n", key);
            pad_down(key);
            usleep(60000);
            pad_up(key);
        } else if (*p == 'q') {
            fprintf(stderr, "[tr] auto-input: asking the game to quit\n");
            tr_input_request_quit();
        } else if (sscanf(p, "%f,%f", &tx, &ty) == 2) {
            fprintf(stderr, "[tr] auto-input: tap at %.3f,%.3f\n", tx, ty);
            tap_screen(tx, ty);
        }
        report_reaction(before_swaps);
        return;
    }

    int key = keys[n % (sizeof keys / sizeof *keys)];
    fprintf(stderr, "[tr] auto-input: keycode %d\n", key);
    pad_down(key);
    usleep(60000);
    pad_up(key);
    report_reaction(before_swaps);
}

void tr_input_request_quit(void)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "nativeSendQuit");
    quit_requested = 1;
    if (fn)
        ((void (*)(void *, void *))fn)(
            tr_jni_env(), tr_jret_class("org/libsdl/app/SDLActivity"));
}

int tr_input_should_quit(void)
{
    return quit_requested;
}

void tr_input_close(void)
{
    release_game_controls();
    for (int i = 0; i < pad_n; i++)
        SDL_GameControllerClose(pads[i]);
    pad_n = 0;
    for (int i = 0; i < exit_probe_count; i++) {
        close(exit_probes[i].fd);
        exit_probes[i].fd = -1;
    }
    exit_probe_count = 0;
}
