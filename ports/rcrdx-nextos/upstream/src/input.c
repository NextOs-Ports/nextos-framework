/*
 * input.c -- native controllers, delivered through the game's own SDL path.
 *
 * Retro City Rampage DX is a gamepad game from the factory: the binary carries
 * the whole SDL_GameController layer and the manifest declares
 * android.hardware.gamepad.  So there is no touch cursor here and nothing
 * captures a button.  This file is the Android UI thread: it reads the real
 * pads with the SYSTEM SDL2 (so NextOS' controller database, hotplug and
 * per-device quirks all apply) and forwards each change through the exact
 * native entry points the Java view would have used --
 * SDLActivity.nativeAddJoystick, onNativePadDown/Up and onNativeJoy.
 *
 * The SDL inside libRCRDX.so is an old (2.0.4/2.0.5-era) Android build: those
 * entry points live on SDLActivity itself, not on SDLControllerManager, and
 * nativeAddJoystick takes (id, name, is_accelerometer, nbuttons, naxes, nhats,
 * nballs) with no vendor/product and no masks.  Its joystick GUID is therefore
 * the first sixteen bytes of the device NAME, and its keycode->button table
 * (measured in the binary at Android_OnPadDown) is the standard
 * SDL_CONTROLLER_BUTTON_* order.  The mapping we hand it below restates that
 * same layout for our device name -- it is the game's own arrangement, not an
 * invented map.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include <SDL2/SDL.h>

#include "nx_elf.h"
#include "rcr.h"

/* Android keycodes, as the game's SDL expects to receive them. */
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

#define RCR_DEVICE_ID 1
#define MAX_PADS 8

/* The device the game sees.  Sixteen bytes exactly: the game's SDL builds the
 * joystick GUID from the first sixteen bytes of this string, and the mapping
 * below is registered under that same GUID. */
#define PAD_NAME "NextOS Gamepad"

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

/* The triggers are analogue axes on the pad and shoulder BUTTONS on Android's
 * gamepad profile; the game reads L2/R2 through the controller layer either
 * way, so a crossed threshold is published as the matching keycode too. */
#define TRIGGER_ON  16384

static SDL_GameController *pads[MAX_PADS];
static SDL_JoystickID pad_ids[MAX_PADS];
static int pad_n;

static int held[BUTTON_MAP_N];
static int trigger_held[2];
static int axis_value[SDL_CONTROLLER_AXIS_MAX];
static int registered;
static int quit_requested;

static void *sdl_native(const char *cls, const char *name)
{
    return rcr_jni_native(cls, name);
}

static void pad_down(int keycode)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativePadDown");
    if (fn)
        ((int (*)(void *, void *, int, int))fn)(
            rcr_jni_env(), rcr_jret_class("org/libsdl/app/SDLActivity"),
            RCR_DEVICE_ID, keycode);
}

static void pad_up(int keycode)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativePadUp");
    if (fn)
        ((int (*)(void *, void *, int, int))fn)(
            rcr_jni_env(), rcr_jret_class("org/libsdl/app/SDLActivity"),
            RCR_DEVICE_ID, keycode);
}

static void joy_axis(int axis, float value)
{
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "onNativeJoy");
    if (fn)
        ((void (*)(void *, void *, int, int, float))fn)(
            rcr_jni_env(), rcr_jret_class("org/libsdl/app/SDLActivity"),
            RCR_DEVICE_ID, axis, value);
}

/* --- what the game's own SDL ended up with ------------------------------- */

static void report_game_view_of_pad(void)
{
    nx_mod *g = nx_find_mod("libRCRDX.so");
    if (!g)
        return;
    int (*num)(void) = nx_lookup_in(g, "SDL_NumJoysticks");
    const char *(*name_of)(int) = nx_lookup_in(g, "SDL_JoystickNameForIndex");
    uint8_t (*is_gc)(int) = nx_lookup_in(g, "SDL_IsGameController");
    if (!num) {
        nx_log("input: the game's SDL does not export SDL_NumJoysticks");
        return;
    }
    int n = num();
    fprintf(stderr, "[rcr] the game's SDL sees %d joystick(s)\n", n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "[rcr]   joystick %d: \"%s\" gamecontroller=%s\n", i,
                name_of && name_of(i) ? name_of(i) : "?",
                (is_gc && is_gc(i)) ? "yes" : "NO");
}

/* The GUID this SDL builds for a Java-registered pad is the first sixteen
 * bytes of the name, zero padded -- measured in Android_AddJoystick.  Restate
 * the layout its own keycode table produces, so the controller layer inside
 * the game is complete from the first frame. */
static void announce_mapping(void)
{
    nx_mod *g = nx_find_mod("libRCRDX.so");
    if (!g)
        return;
    int (*add)(const char *) = nx_lookup_in(g, "SDL_GameControllerAddMapping");
    if (!add) {
        nx_log("input: the game's SDL has no SDL_GameControllerAddMapping");
        return;
    }
    unsigned char guid[16];
    memset(guid, 0, sizeof guid);
    memcpy(guid, PAD_NAME, strnlen(PAD_NAME, sizeof guid));
    char line[512];
    int n = 0;
    for (size_t i = 0; i < sizeof guid; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, "%02x", guid[i]);
    snprintf(line + n, sizeof line - (size_t)n,
             ",%s,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
             "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
             "lefttrigger:a4,righttrigger:a5,platform:Android,", PAD_NAME);
    int r = add(line);
    nx_log("input: mapping announced to the game's SDL -> %d", r);
}

/* Android registers the device from pollInputDevices(), which SDL calls once
 * its joystick subsystem exists.  Doing it any earlier means calling into a
 * subsystem whose locks have not been created yet. */
void rcr_input_register(void)
{
    if (registered || !pad_n)
        return;
    if (getenv("RCR_NO_PAD")) {
        fprintf(stderr, "[rcr] RCR_NO_PAD: not telling the game about a pad\n");
        return;
    }
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "nativeAddJoystick");
    if (!fn) {
        nx_log("input: the game's SDL has no nativeAddJoystick");
        return;
    }
    announce_mapping();
    void *name = rcr_jret_str(PAD_NAME);
    /* (device_id, name, is_accelerometer, nbuttons, naxes, nhats, nballs) --
     * the argument list this build's Android_AddJoystick consumes. */
    ((int (*)(void *, void *, int, void *, int, int, int, int, int))fn)(
        rcr_jni_env(), rcr_jret_class("org/libsdl/app/SDLActivity"),
        RCR_DEVICE_ID, name,
        0,                              /* not an accelerometer */
        15,                             /* buttons: A..dpright */
        6,                              /* axes: 2 sticks + 2 triggers */
        0,                              /* hats: the d-pad arrives as buttons */
        0);                             /* balls */
    registered = 1;
    fprintf(stderr, "[rcr] controller registered with the game's SDL\n");
    report_game_view_of_pad();
}

/* --- the pads on our side ------------------------------------------------ */

#define RCR_BTN_SELECT   0x13a
#define RCR_BTN_START    0x13b
#define RCR_BTN_THUMBL   0x13d
#define RCR_BTN_THUMBR   0x13e
#define RCR_BTN_HAPPY1   0x2c0

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

/* SDL_GameControllerHasButton was added in SDL 2.0.14, while the public
 * PortMaster floor is SDL 2.0.4.  GetBindForButton has existed since the
 * controller API was introduced and answers the same capability question. */
static int controller_has_button(SDL_GameController *gc,
                                 SDL_GameControllerButton button)
{
    SDL_GameControllerButtonBind bind =
        SDL_GameControllerGetBindForButton(gc, button);
    return bind.bindType != SDL_CONTROLLER_BINDTYPE_NONE;
}

/* On these handhelds SELECT/START/L3/R3 usually arrive as BTN_TRIGGER_HAPPY*
 * instead of BTN_SELECT/BTN_START/BTN_THUMB*, which is exactly why the
 * automatic mapping leaves them out.  Look for both spellings. */
static int complete_mapping(SDL_GameController *gc, SDL_JoystickGUID guid,
                            const char *name)
{
    int miss_sel = !controller_has_button(gc, SDL_CONTROLLER_BUTTON_BACK);
    int miss_sta = !controller_has_button(gc, SDL_CONTROLLER_BUTTON_START);
    int miss_r3  = !controller_has_button(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    int miss_l3  = !controller_has_button(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK);
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

    struct { int miss; const char *key; int code, alt; } add[] = {
        { miss_sel, "back",       RCR_BTN_SELECT, RCR_BTN_HAPPY1 + 0 },
        { miss_sta, "start",      RCR_BTN_START,  RCR_BTN_HAPPY1 + 1 },
        { miss_l3,  "leftstick",  RCR_BTN_THUMBL, RCR_BTN_HAPPY1 + 2 },
        { miss_r3,  "rightstick", RCR_BTN_THUMBR, RCR_BTN_HAPPY1 + 3 },
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
            continue;
        int w = snprintf(out + n, sizeof out - (size_t)n, ",%s:b%d",
                         add[i].key, b);
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
    if (pad_n == MAX_PADS || !SDL_IsGameController(index))
        return;

    /* The same device can be enumerated twice (js0 and eventN); the merge
     * below keeps the largest deflection, so the duplicate's idle noise would
     * win over the real stick.  Refuse it by GUID at the source. */
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    for (int i = 0; i < pad_n; i++) {
        SDL_JoystickGUID have =
            SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pads[i]));
        if (memcmp(&have, &guid, sizeof guid) == 0) {
            fprintf(stderr, "[rcr] pad %d ignorado: mesmo GUID do pad %d\n",
                    index, i);
            return;
        }
    }

    /* Diagnóstico: o adaptador USB deste banco de teste dispara o botão A
     * sozinho (medido no evdev cru, com o jogo fechado), o que torna qualquer
     * teste de menu ilegível.  RCR_PAD_IGNORE=<trecho do nome> deixa de fora um
     * aparelho pelo nome.  Vazio por padrão: nenhum pad é recusado no uso
     * normal. */
    {
        const char *skip = getenv("RCR_PAD_IGNORE");
        const char *nm = SDL_GameControllerNameForIndex(index);
        if (skip && *skip && nm && strstr(nm, skip)) {
            fprintf(stderr, "[rcr] pad %d (%s) ignorado por RCR_PAD_IGNORE\n",
                    index, nm);
            return;
        }
    }

    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc)
        return;

    /* A mapping only takes effect on the NEXT open, so complete and reopen. */
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
    fprintf(stderr, "[rcr] pad %d: %s\n", pad_n - 1,
            SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?");
    {
        char gs[64] = "";
        SDL_JoystickGetGUIDString(guid, gs, sizeof gs);
        char *m = SDL_GameControllerMapping(gc);
        fprintf(stderr, "[rcr]   guid=%s\n[rcr]   mapping=%s\n", gs,
                m ? m : "(NENHUM: a SDL montou um padrao)");
        static const struct { int b; const char *n; } want[] = {
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1" },
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    "R3" },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK,     "L3" },
            { SDL_CONTROLLER_BUTTON_BACK,          "SELECT" },
            { SDL_CONTROLLER_BUTTON_START,         "START" },
        };
        for (size_t k = 0; k < sizeof want / sizeof *want; k++)
            fprintf(stderr, "[rcr]   %-6s presente=%s\n", want[k].n,
                    controller_has_button(gc, want[k].b) ? "sim" : "NAO");
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
        return;
    }
}

int rcr_input_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[rcr] SDL_INIT_GAMECONTROLLER: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GameControllerEventState(SDL_ENABLE);

    /* NextOS ships its own controller database next to the port. */
    char db[1200];
    const char *env = getenv("sdl_controllerconfig");
    if (env && *env)
        SDL_GameControllerAddMapping(env);
    snprintf(db, sizeof db, "%s/gamecontrollerdb.txt", rcr_gamedir);
    if (SDL_GameControllerAddMappingsFromFile(db) > 0)
        nx_log("input: loaded %s", db);

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        open_pad(i);
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
    /* The game's SDL multiplies by 32767 on the way back in, so what crosses
     * this boundary is the normalised value. */
    joy_axis(axis, (float)raw / 32767.0f);
}

void rcr_input_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERDEVICEADDED)
            open_pad(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
            close_pad(e.cdevice.which);
        else if (e.type == SDL_QUIT && !quit_requested)
            rcr_input_request_quit();
    }
    if (!pad_n || !registered)
        return;

    /* Merge: a button is down if any pad holds it, an axis takes the largest
     * deflection.  One player, several possible controllers. */
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

    /* SELECT+START is the frontend's exit gesture on every NextOS port: it
     * asks the game to quit through its own path so the save is written. */
    if (merged[SDL_CONTROLLER_BUTTON_BACK] &&
        merged[SDL_CONTROLLER_BUTTON_START]) {
        if (!quit_requested) {
            fprintf(stderr, "[rcr] SELECT+START: asking the game to quit\n");
            rcr_input_request_quit();
        }
        return;
    }

    for (size_t i = 0; i < BUTTON_MAP_N; i++) {
        static int was[BUTTON_MAP_N];
        if (merged[i] != was[i]) {
            was[i] = merged[i];
            if (merged[i])
                nx_log("[BOTAO] SDL button %zu (keycode %d) pressionado", i,
                       button_map[i].keycode);
        }
        publish_button(i, merged[i]);
    }
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        publish_axis(a, merged_axis[a]);

    /* Triggers also as shoulder keycodes, the way Android's profile reports
     * them, so a game that reads L2/R2 as buttons still sees them. */
    static const struct { int axis, keycode; } trig[2] = {
        { SDL_CONTROLLER_AXIS_TRIGGERLEFT,  AKEYCODE_BUTTON_L2 },
        { SDL_CONTROLLER_AXIS_TRIGGERRIGHT, AKEYCODE_BUTTON_R2 },
    };
    for (int t = 0; t < 2; t++) {
        int down = merged_axis[trig[t].axis] > TRIGGER_ON;
        if (down == trigger_held[t])
            continue;
        trigger_held[t] = down;
        if (down)
            pad_down(trig[t].keycode);
        else
            pad_up(trig[t].keycode);
    }
}

void rcr_input_request_quit(void)
{
    quit_requested = 1;
    /* This SDL has no nativeSendQuit: the Activity's own quit path is
     * nativePause (so the game writes its state) followed by nativeQuit,
     * which is what SDLActivity.onDestroy does on a phone. */
    void *env = rcr_jni_env();
    void *cls = rcr_jret_class("org/libsdl/app/SDLActivity");
    void *fn = sdl_native("org/libsdl/app/SDLActivity", "nativePause");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);
    fn = sdl_native("org/libsdl/app/SDLActivity", "nativeQuit");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);
}

int rcr_input_should_quit(void)
{
    return quit_requested;
}

void rcr_input_close(void)
{
    for (int i = 0; i < pad_n; i++)
        SDL_GameControllerClose(pads[i]);
    pad_n = 0;
}
