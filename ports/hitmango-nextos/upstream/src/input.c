/*
 * Native NextOS controller -> Android input bridge for Hitman GO.
 *
 * SDL normalises the physical controller, then Unity receives the same
 * KeyEvent/MotionEvent stream its Android Activity would forward.  InControl
 * remains available to the game.  An opt-in test path selects Hitman GO's own
 * tvOS IInputManager implementation, whose node selector provides true
 * controller board movement.  By default the left-stick pointer reproduces
 * touch-only UI: A starts a touch, stick movement drags it, and release ends
 * it.  The right-stick/R3 layout remains an explicit alternative.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "hgo.h"
#include "input_layout.h"
#include "input_lifecycle.h"
#include "nx_elf.h"
#include "touch_arbiter.h"

static SDL_GameController *controller;
/*
 * Pad fora da base de mapeamentos do SDL: sem este caminho o controle não é
 * reconhecido como GameController, `controller` fica NULL e o jogo perde a
 * navegação INTEIRA — foi o relato do RG40XX-H/muOS ("no navigation control,
 * the character won't move"). Abrir como joystick cru e usar a ordem
 * posicional dos pads USB comuns é melhor do que exigir entrada na base.
 */
static SDL_Joystick *raw_joystick;
static uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX];
static uint8_t previous[SDL_CONTROLLER_BUTTON_MAX];
static volatile sig_atomic_t exit_requested;
static int input_rearm_required;
static hgo_input_lifecycle_state input_lifecycle;

/* SIGTERM/SIGINT convergem no mesmo shutdown do SELECT+START: o loop de
 * render vê exit_requested e percorre pause/save/saída na ordem original. */
void hgo_input_request_exit(void)
{
    exit_requested = 1;
}
static int virtual_enabled;
static unsigned virtual_button_frames[SDL_CONTROLLER_BUTTON_MAX];
static unsigned virtual_axis_frames[SDL_CONTROLLER_AXIS_MAX];
static float virtual_axis_values[SDL_CONTROLLER_AXIS_MAX];
static int screen_width = 1280;
static int screen_height = 720;

static unsigned long joystick_name_calls;
static unsigned long raw_button_calls;
static unsigned long raw_analog_calls;
static uint32_t queried_buttons;
static uint32_t queried_analogs;

static int cursor_enabled;
static float cursor_x = 640.0f;
static float cursor_y = 360.0f;
static float cursor_vx;
static float cursor_vy;
static uint64_t cursor_tick;
static int cursor_drag_active;
static float cursor_touch_x;
static float cursor_touch_y;
static int ui_tap_release_pending;
static float ui_tap_x;
static float ui_tap_y;
static hgo_touch_arbiter_state touch_arbiter;
static int swipe_step;          /* 0 = ocioso; conta os quadros do gesto */
static float swipe_from_x, swipe_from_y, swipe_dx, swipe_dy;
static int swipe_latched;
/* 1: START requested the objectives overlay; 2: overlay is visibly active. */
static int menu_overlay_state;

static int native_controls_enabled;
static int native_selection_active;
static int native_gameplay_active;
static int native_activity_reported = -1;
static int native_direction_latched;
static void *native_input_class;
static void *native_input_manager;
static uint8_t *il2cpp_base;

static float axis_value(SDL_GameControllerAxis axis);

/* Offsets derived from this port's own plaintext metadata + libil2cpp.so
 * (Hitman GO 1.18.1, build-id fff24ec90a21f8a922540764744143e99ea49167).
 * They are deliberately not inherited from Terraria or another game. */
#define HGO_GET_JOYSTICK_NAMES   0x1cec194u
#define HGO_UINPUT_RAW_BUTTON    0x00edb204u
#define HGO_UINPUT_RAW_ANALOG    0x00edb2b4u
#define HGO_UINPUT_IS_SUPPORTED  0x00edb364u
#define HGO_INPUT_IMPLEMENTATION 0x00db7e5cu
#define HGO_TVOS_ON_SWIPE        0x00dbb90cu
#define HGO_TVOS_CHANGE_SELECTION 0x00dbc09cu
#define HGO_TVOS_SELECTION_ACTIVE 0x00dbc960u
#define HGO_TVOS_CLICK_UP        0x00dbc968u
#define HGO_TVOS_MENU_UP         0x00dbcd58u
#define HGO_TVOS_CTOR            0x00dbcdd4u
#define HGO_DEFAULT_CTOR         0x00db871cu

typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef void *(*il2cpp_string_new_fn)(const char *);
typedef void *(*il2cpp_array_new_fn)(void *, size_t);
typedef uint32_t (*il2cpp_gchandle_new_fn)(void *, int);
typedef void *(*il2cpp_object_new_fn)(void *);

static il2cpp_domain_get_fn il2cpp_domain_get_p;
static il2cpp_domain_get_assemblies_fn il2cpp_domain_get_assemblies_p;
static il2cpp_assembly_get_image_fn il2cpp_assembly_get_image_p;
static il2cpp_class_from_name_fn il2cpp_class_from_name_p;
static il2cpp_string_new_fn il2cpp_string_new_p;
static il2cpp_array_new_fn il2cpp_array_new_p;
static il2cpp_gchandle_new_fn il2cpp_gchandle_new_p;
static il2cpp_object_new_fn il2cpp_object_new_p;
static void *joystick_names;

static int cursor_is_active(void)
{
    return cursor_enabled;
}

/*
 * ===== Botão de seleção =====
 * O contrato publicado da v1.2.0 usa A para clicar/segurar o cursor. O layout
 * alternativo usa R3 somente quando HGO_CLICK_A=0.
 */
static hgo_input_layout input_layout = HGO_INPUT_LAYOUT_V120_INITIALIZER;

/*
 * ===== Layout dos analógicos (pedido do NextOS, 05/08) =====
 * Cursor no analógico ESQUERDO; movimento do tabuleiro no DIREITO (o D-pad
 * continua movendo sempre). A troca mora nestas duas funções: quem quer o
 * eixo de movimento ou do cursor pergunta aqui, então polling do InControl,
 * ponte tvOS, MotionEvent Android e cursor nunca discordam de qual stick é
 * qual. HGO_SWAP_STICKS=0 habilita o layout alternativo direita/R3.
 */
static SDL_GameControllerAxis move_axis(int vertical)
{
    if (input_layout.cursor_on_left)
        return vertical ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX;
    return vertical ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX;
}

static SDL_GameControllerAxis cursor_axis(int vertical)
{
    if (input_layout.cursor_on_left)
        return vertical ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX;
    return vertical ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX;
}

/* Os dois contratos são mutuamente exclusivos: esquerda/A por padrão;
 * direita/R3 somente no opt-in alternativo. Na seleção nativa, A volta a ser
 * o botão de confirmação/arremesso do próprio jogo. */
static int cursor_click_held(void)
{
    return hgo_input_cursor_button_held(
        &input_layout, native_selection_active,
        buttons[SDL_CONTROLLER_BUTTON_A],
        buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK]);
}

/* O A vira botão de clique quando o cursor está ativo; nesse modo a antiga
   confirmação do A é servida pelo L1 (livre neste jogo).  Na mira de pedra o
   A é devolvido ao jogo como arremesso. */
static int a_is_click_button(void)
{
    return input_layout.cursor_uses_a && cursor_is_active() &&
           !native_selection_active;
}

static int hgo_incontrol_button(void *self, int index, void *method)
{
    (void)self;
    (void)method;
    raw_button_calls++;
    if (index >= 0 && index < 20)
        queried_buttons |= UINT32_C(1) << index;
    static const int map[] = {
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        -1, -1,
        SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_BACK,
    };
    if (exit_requested || index < 0 || (size_t)index >= sizeof map / sizeof *map)
        return 0;
    int button = map[index];
    if (button < 0 || (cursor_is_active() &&
                       button == SDL_CONTROLLER_BUTTON_RIGHTSTICK))
        return 0;
    /* A vira clique do cursor: quem entrega a confirmação ao InControl é o L1.
       Sem essa troca, a função que o A tinha simplesmente sumiria. */
    if (a_is_click_button()) {
        if (button == SDL_CONTROLLER_BUTTON_A)
            return buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] ? 1 : 0;
        if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            return 0;
    }
    /* B is delivered as Android's real BACK key below.  Keeping it in the
     * InControl button stream as well would dispatch the same menu action
     * twice on views that listen to both input paths. */
    if (button == SDL_CONTROLLER_BUTTON_B)
        return 0;
    if (native_selection_active && button == SDL_CONTROLLER_BUTTON_A)
        return 0;
    if (native_gameplay_active &&
        (button == SDL_CONTROLLER_BUTTON_START ||
         button == SDL_CONTROLLER_BUTTON_Y ||
         button == SDL_CONTROLLER_BUTTON_X))
        return 0;
    if (menu_overlay_state == 2 &&
        (button == SDL_CONTROLLER_BUTTON_B ||
         button == SDL_CONTROLLER_BUTTON_START))
        return 0;
    return buttons[button] ? 1 : 0;
}

static float hgo_incontrol_analog(void *self, int index, void *method)
{
    (void)self;
    (void)method;
    raw_analog_calls++;
    if (index >= 0 && index < 20)
        queried_analogs |= UINT32_C(1) << index;
    if (exit_requested)
        return 0.0f;
    switch (index) {
    case 0: return axis_value(move_axis(0));
    case 1: return axis_value(move_axis(1));
    case 2: return cursor_is_active() ? 0.0f : axis_value(cursor_axis(0));
    case 3: return cursor_is_active() ? 0.0f : axis_value(cursor_axis(1));
    case 4: return (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] -
                           buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]);
    case 5: return (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] -
                           buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]);
    case 6: return axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    case 7: return axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    default: return 0.0f;
    }
}

static int hgo_incontrol_supported(void *self, void *method)
{
    (void)self;
    (void)method;
    return 1;
}

static void *find_managed_class(const char *namespaze, const char *name)
{
    if (!il2cpp_domain_get_p || !il2cpp_domain_get_assemblies_p ||
        !il2cpp_assembly_get_image_p || !il2cpp_class_from_name_p)
        return NULL;

    void *domain = il2cpp_domain_get_p();
    size_t count = 0;
    const void **assemblies = domain
                            ? il2cpp_domain_get_assemblies_p(domain, &count)
                            : NULL;
    for (size_t i = 0; assemblies && i < count; i++) {
        void *image = il2cpp_assembly_get_image_p(assemblies[i]);
        void *klass = image
                    ? il2cpp_class_from_name_p(image, namespaze, name)
                    : NULL;
        if (klass)
            return klass;
    }
    return NULL;
}

static void *hgo_get_native_input_implementation(void *self, void *method)
{
    (void)self;
    (void)method;
    if (!il2cpp_object_new_p || !il2cpp_base)
        return NULL;

    /* This hook runs from InputManager.Initialize(), after IL2CPP has entered
     * managed execution.  Resolving classes here is safe; doing it while the
     * nativeRender loop is only being prepared enters il2cpp_domain_get too
     * early and kills Unity before frame zero. */
    if (!native_input_class)
        native_input_class =
            find_managed_class("HMGO", "InputManager_tvOS");
    if (!native_input_class) {
        void *default_class =
            find_managed_class("HMGO", "InputManager_Default");
        void *fallback = default_class
                       ? il2cpp_object_new_p(default_class)
                       : NULL;
        if (fallback) {
            ((void (*)(void *, void *))
             (il2cpp_base + HGO_DEFAULT_CTOR))(fallback, NULL);
            native_controls_enabled = 0;
            fprintf(stderr,
                    "[hgo/input] tvOS class missing; Android input preserved\n");
        }
        return fallback;
    }

    void *manager = il2cpp_object_new_p(native_input_class);
    if (!manager)
        return NULL;
    ((void (*)(void *, void *))(il2cpp_base + HGO_TVOS_CTOR))(manager, NULL);
    native_input_manager = manager;
    native_activity_reported = -1;
    fprintf(stderr,
            "[hgo/input] native node controls: InputManager_tvOS selected\n");
    return manager;
}

static void *hgo_get_joystick_names(void *method)
{
    (void)method;
    joystick_name_calls++;
    if (joystick_names)
        return joystick_names;
    if (!il2cpp_domain_get_p || !il2cpp_domain_get_assemblies_p ||
        !il2cpp_assembly_get_image_p || !il2cpp_class_from_name_p ||
        !il2cpp_string_new_p || !il2cpp_array_new_p)
        return NULL;

    void *string_class = find_managed_class("System", "String");
    if (!string_class)
        return NULL;

    void *array = il2cpp_array_new_p(string_class, 1);
    void *name = il2cpp_string_new_p("Microsoft X-Box 360 pad");
    if (!array || !name)
        return NULL;
    ((void **)((uint8_t *)array + 0x20))[0] = name;
    if (il2cpp_gchandle_new_p)
        il2cpp_gchandle_new_p(array, 1);
    joystick_names = array;
    fprintf(stderr, "[hgo/input] InControl sees Microsoft X-Box 360 pad\n");
    return joystick_names;
}

static void replace_body(uint8_t *base, uintptr_t offset, void *function)
{
    uint8_t *code = base + offset;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)code & ~(page_size - 1);
    if (mprotect((void *)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return;
    uint32_t *words = (uint32_t *)code;
    words[0] = 0x58000050u;  /* ldr x16, [pc, #8] */
    words[1] = 0xd61f0200u;  /* br x16 */
    *(uint64_t *)(words + 2) = (uint64_t)(uintptr_t)function;
    __builtin___clear_cache((char *)code, (char *)code + 16);
    mprotect((void *)page, page_size, PROT_READ | PROT_EXEC);
}

static void install_incontrol_hooks(void)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    il2cpp_domain_get_p = (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get");
    il2cpp_domain_get_assemblies_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    il2cpp_assembly_get_image_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    il2cpp_class_from_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    il2cpp_string_new_p = (void *)nx_lookup_in(il2cpp, "il2cpp_string_new");
    il2cpp_array_new_p = (void *)nx_lookup_in(il2cpp, "il2cpp_array_new");
    il2cpp_gchandle_new_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gchandle_new");
    il2cpp_object_new_p = (void *)nx_lookup_in(il2cpp, "il2cpp_object_new");
    il2cpp_base = il2cpp->base;

    replace_body(il2cpp->base, HGO_GET_JOYSTICK_NAMES,
                 hgo_get_joystick_names);
    replace_body(il2cpp->base, HGO_UINPUT_RAW_BUTTON,
                 hgo_incontrol_button);
    replace_body(il2cpp->base, HGO_UINPUT_RAW_ANALOG,
                 hgo_incontrol_analog);
    replace_body(il2cpp->base, HGO_UINPUT_IS_SUPPORTED,
                 hgo_incontrol_supported);
    if (native_controls_enabled && il2cpp_object_new_p &&
        il2cpp_domain_get_p && il2cpp_domain_get_assemblies_p &&
        il2cpp_assembly_get_image_p && il2cpp_class_from_name_p) {
        replace_body(il2cpp->base, HGO_INPUT_IMPLEMENTATION,
                     hgo_get_native_input_implementation);
        fprintf(stderr,
                "[hgo/input] native node-control selector armed\n");
    }
    fprintf(stderr,
            "[hgo/input] Hitman GO InControl hooks installed from own metadata\n");
}

static const int android_key[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = 96,              /* KEYCODE_BUTTON_A */
    [SDL_CONTROLLER_BUTTON_B] = 4,               /* KEYCODE_BACK */
    [SDL_CONTROLLER_BUTTON_X] = 99,              /* KEYCODE_BUTTON_X */
    [SDL_CONTROLLER_BUTTON_Y] = 100,             /* KEYCODE_BUTTON_Y */
    [SDL_CONTROLLER_BUTTON_BACK] = 109,           /* KEYCODE_BUTTON_SELECT */
    [SDL_CONTROLLER_BUTTON_GUIDE] = 110,          /* KEYCODE_BUTTON_MODE */
    [SDL_CONTROLLER_BUTTON_START] = 108,          /* KEYCODE_BUTTON_START */
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = 106,      /* KEYCODE_BUTTON_THUMBL */
    [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = 107,     /* KEYCODE_BUTTON_THUMBR */
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 102,   /* KEYCODE_BUTTON_L1 */
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 103,  /* KEYCODE_BUTTON_R1 */
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = 19,
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = 20,
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = 21,
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = 22,
};

static float axis_value(SDL_GameControllerAxis axis)
{
    if (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX &&
        virtual_axis_frames[axis] > 0)
        return virtual_axis_values[axis];
    Sint16 value = 0;
    if (controller) {
        value = SDL_GameControllerGetAxis(controller, axis);
    } else if (raw_joystick) {
        /* ordem posicional: LX LY RX RY (gatilhos ficam nos botões 6/7) */
        static const int raw_axis[SDL_CONTROLLER_AXIS_MAX] = {
            [SDL_CONTROLLER_AXIS_LEFTX] = 0, [SDL_CONTROLLER_AXIS_LEFTY] = 1,
            [SDL_CONTROLLER_AXIS_RIGHTX] = 2, [SDL_CONTROLLER_AXIS_RIGHTY] = 3,
            [SDL_CONTROLLER_AXIS_TRIGGERLEFT] = -1,
            [SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = -1,
        };
        int index = (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX)
                  ? raw_axis[axis] : -1;
        if (index >= 0 && index < SDL_JoystickNumAxes(raw_joystick))
            value = SDL_JoystickGetAxis(raw_joystick, index);
    }
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
        axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return value > 0 ? value / 32767.0f : 0.0f;
    return value < 0 ? value / 32768.0f : value / 32767.0f;
}

static int raw_button_index(SDL_GameControllerButton button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return 0;
    case SDL_CONTROLLER_BUTTON_B: return 1;
    case SDL_CONTROLLER_BUTTON_X: return 2;
    case SDL_CONTROLLER_BUTTON_Y: return 3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 4;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
    case SDL_CONTROLLER_BUTTON_BACK: return 8;
    case SDL_CONTROLLER_BUTTON_START: return 9;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 10;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 11;
    default: return -1;
    }
}

static int input_controls_neutral(void)
{
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
        if (buttons[i])
            return 0;
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
        if (fabsf(axis_value((SDL_GameControllerAxis)i)) > 0.25f)
            return 0;
    return 1;
}

static void virtual_press_button(SDL_GameControllerButton button,
                                 unsigned duration)
{
    if (button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX)
        virtual_button_frames[button] = duration;
}

static void virtual_press_axis(SDL_GameControllerAxis axis, float value,
                               unsigned duration)
{
    if (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX) {
        virtual_axis_frames[axis] = duration;
        virtual_axis_values[axis] = value;
    }
}

/* Approved-port bring-up path: one token written to /tmp/hgogp becomes a
 * short native-controller pulse.  This never enters the touch/mouse path and
 * is inactive unless HGO_GPVIRT is explicitly enabled for a test launch. */
static void poll_virtual_controller(void)
{
    if (!virtual_enabled)
        return;

    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        if (virtual_button_frames[i] > 0)
            virtual_button_frames[i]--;
    }
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
        if (virtual_axis_frames[i] > 0)
            virtual_axis_frames[i]--;
    }

    FILE *input = fopen("/tmp/hgogp", "r");
    if (input) {
        char token[24] = { 0 };
        int have_token = fscanf(input, "%23s", token) == 1 && token[0];
        fclose(input);
        unlink("/tmp/hgogp");
        if (have_token) {
            unsigned duration = 6;
            const char *duration_value = getenv("HGO_GPVDUR");
            if (duration_value && *duration_value) {
                long parsed = strtol(duration_value, NULL, 10);
                if (parsed > 0 && parsed <= 600)
                    duration = (unsigned)parsed;
            }
            /* A per-pulse suffix (for example r3:60 or rx+:12) makes the
             * disabled-by-default virtual test controller precise enough to
             * validate hold-and-drag gestures without affecting players. */
            char *duration_separator = strrchr(token, ':');
            if (duration_separator && duration_separator[1]) {
                long parsed = strtol(duration_separator + 1, NULL, 10);
                if (parsed > 0 && parsed <= 600) {
                    duration = (unsigned)parsed;
                    *duration_separator = '\0';
                }
            }
            int matched = 1;
            if (!strcasecmp(token, "a"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_A, duration);
            else if (!strcasecmp(token, "b"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_B, duration);
            else if (!strcasecmp(token, "x"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_X, duration);
            else if (!strcasecmp(token, "y"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_Y, duration);
            else if (!strcasecmp(token, "l1"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                                     duration);
            else if (!strcasecmp(token, "r1"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                                     duration);
            else if (!strcasecmp(token, "select"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_BACK, duration);
            else if (!strcasecmp(token, "start"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_START, duration);
            else if (!strcasecmp(token, "l3"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_LEFTSTICK,
                                     duration);
            else if (!strcasecmp(token, "r3"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK,
                                     duration);
            else if (!strcasecmp(token, "up"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_DPAD_UP, duration);
            else if (!strcasecmp(token, "down"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                     duration);
            else if (!strcasecmp(token, "left"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                                     duration);
            else if (!strcasecmp(token, "right"))
                virtual_press_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                                     duration);
            else if (!strcasecmp(token, "lx+"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_LEFTX, 1.0f, duration);
            else if (!strcasecmp(token, "lx-"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_LEFTX, -1.0f, duration);
            else if (!strcasecmp(token, "ly+"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_LEFTY, 1.0f, duration);
            else if (!strcasecmp(token, "ly-"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_LEFTY, -1.0f, duration);
            else if (!strcasecmp(token, "rx+"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_RIGHTX, 1.0f, duration);
            else if (!strcasecmp(token, "rx-"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_RIGHTX, -1.0f, duration);
            else if (!strcasecmp(token, "ry+"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_RIGHTY, 1.0f, duration);
            else if (!strcasecmp(token, "ry-"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_RIGHTY, -1.0f, duration);
            else if (!strcasecmp(token, "lt"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1.0f,
                                   duration);
            else if (!strcasecmp(token, "rt"))
                virtual_press_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1.0f,
                                   duration);
            else if (!strcasecmp(token, "exit")) {
                virtual_press_button(SDL_CONTROLLER_BUTTON_BACK, duration);
                virtual_press_button(SDL_CONTROLLER_BUTTON_START, duration);
            } else {
                matched = 0;
            }
            fprintf(stderr, "[hgo/input] virtual pulse %s x%u (%s)\n",
                    token, duration, matched ? "accepted" : "unknown");
        }
    }

    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        if (virtual_button_frames[i] > 0)
            buttons[i] = 1;
    }
}

static void add_known_mappings(void)
{
    SDL_GameControllerAddMapping(
        "0300605b100800000100000010010000,USB Gamepad,platform:Linux,"
        "a:b1,b:b2,x:b0,y:b3,leftshoulder:b4,rightshoulder:b5,"
        "lefttrigger:b6,righttrigger:b7,back:b8,start:b9,leftstick:b10,"
        "rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,"
        "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        const char *name = SDL_JoystickNameForIndex(i);
        if (!name || strcmp(name, "GO-Super Gamepad") != 0)
            continue;
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_text[33];
        char mapping[512];
        SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
        int n = snprintf(
            mapping, sizeof mapping,
            "%s,GO-Super Gamepad,platform:Linux,"
            "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
            "lefttrigger:b6,righttrigger:b7,dpup:b8,dpdown:b9,"
            "dpleft:b10,dpright:b11,back:b12,start:b13,leftstick:b14,"
            "rightstick:b15,guide:b16,leftx:a0,lefty:a1,rightx:a2,righty:a3,",
            guid_text);
        if (n > 0 && (size_t)n < sizeof mapping)
            SDL_GameControllerAddMapping(mapping);
    }
}

/* SELECT/START em pads sem BTN_SELECT/BTN_START físicos (GO-Super e família
 * RK3326): os dois botões chegam como BTN_TRIGGER_HAPPY1/2 e a base do SDL não
 * os mapeia para BACK/START, então o combo de saída nunca era visto.  O
 * ordinal SDL de um botão é a contagem de bits setados em [BTN_JOYSTICK, code)
 * no bitmap EV_KEY do nó de evento, lido com o long DESTE processo.  Se o pad
 * tiver SELECT/START reais a sonda devolve -1 e nada muda. */
static int th_select_ordinal = -1;
static int th_start_ordinal = -1;

/* SDL_JoystickGetVendor/Product were added in SDL 2.0.6.  Resolve them as
 * optional diagnostics so the game still starts on firmware whose PortMaster
 * SDL is at the public 2.0.4 floor.  A missing API means unknown VID/PID, not
 * a missing controller. */
typedef Uint16 (*hgo_sdl_joystick_id_fn)(SDL_Joystick *joystick);
static hgo_sdl_joystick_id_fn sdl_joystick_get_vendor;
static hgo_sdl_joystick_id_fn sdl_joystick_get_product;
static int sdl_joystick_id_resolved;

static void resolve_sdl_joystick_id_api(void)
{
    if (sdl_joystick_id_resolved)
        return;
    sdl_joystick_id_resolved = 1;
    sdl_joystick_get_vendor = (hgo_sdl_joystick_id_fn)
        dlsym(RTLD_DEFAULT, "SDL_JoystickGetVendor");
    sdl_joystick_get_product = (hgo_sdl_joystick_id_fn)
        dlsym(RTLD_DEFAULT, "SDL_JoystickGetProduct");
    if (!sdl_joystick_get_vendor || !sdl_joystick_get_product)
        fprintf(stderr,
                "[hgo/input] SDL VID/PID diagnostics unavailable; "
                "using 0000 for missing values\n");
}

static int joystick_vendor(SDL_Joystick *joystick)
{
    if (!joystick)
        return 0;
    resolve_sdl_joystick_id_api();
    return sdl_joystick_get_vendor ? sdl_joystick_get_vendor(joystick) : 0;
}

static int joystick_product(SDL_Joystick *joystick)
{
    if (!joystick)
        return 0;
    resolve_sdl_joystick_id_api();
    return sdl_joystick_get_product ? sdl_joystick_get_product(joystick) : 0;
}

static int evdev_bit(const unsigned long *bits, int i)
{
    return (bits[i / (8 * sizeof(long))] >> (i % (8 * sizeof(long)))) & 1UL;
}

static int evdev_key_rank(const unsigned long *keyb, int code)
{
    if (!evdev_bit(keyb, code))
        return -1;
    int rank = 0;
    for (int i = BTN_JOYSTICK; i < code; i++)
        if (evdev_bit(keyb, i))
            rank++;
    return rank;
}

static void find_trigger_happy_ordinals(void)
{
    th_select_ordinal = th_start_ordinal = -1;
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long keyb[(KEY_MAX + 1 + 8 * sizeof(long) - 1) /
                           (8 * sizeof(long))];
        memset(keyb, 0, sizeof keyb);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb) >= 0 &&
            evdev_bit(keyb, BTN_GAMEPAD) && !evdev_bit(keyb, BTN_SELECT) &&
            !evdev_bit(keyb, BTN_START) &&
            evdev_bit(keyb, BTN_TRIGGER_HAPPY1)) {
            th_select_ordinal = evdev_key_rank(keyb, BTN_TRIGGER_HAPPY1);
            th_start_ordinal = evdev_key_rank(keyb, BTN_TRIGGER_HAPPY2);
            fprintf(stderr,
                    "[hgo/input] %s has no physical SELECT/START; "
                    "TRIGGER_HAPPY1/2 ordinals = %d/%d\n",
                    path, th_select_ordinal, th_start_ordinal);
            close(fd);
            return;
        }
        close(fd);
    }
}

static void apply_trigger_happy_buttons(void)
{
    if (th_select_ordinal < 0 && th_start_ordinal < 0)
        return;
    SDL_Joystick *joy = controller ? SDL_GameControllerGetJoystick(controller)
                                   : raw_joystick;
    if (!joy)
        return;
    int count = SDL_JoystickNumButtons(joy);
    if (th_select_ordinal >= 0 && th_select_ordinal < count &&
        SDL_JoystickGetButton(joy, th_select_ordinal))
        buttons[SDL_CONTROLLER_BUTTON_BACK] = 1;
    if (th_start_ordinal >= 0 && th_start_ordinal < count &&
        SDL_JoystickGetButton(joy, th_start_ordinal))
        buttons[SDL_CONTROLLER_BUTTON_START] = 1;
}

static void open_controller(void)
{
    if (controller)
        return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;
        controller = SDL_GameControllerOpen(i);
        if (!controller)
            continue;
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
        const char *physical = SDL_GameControllerName(controller);
        int vendor = joystick_vendor(joy);
        int product = joystick_product(joy);
        hgo_jni_input_device_info("Microsoft X-Box 360 pad", vendor, product,
                                  physical ? physical : "nextos-gamepad");
        fprintf(stderr, "[hgo/input] controller: %s (%04x:%04x)\n",
                physical ? physical : "unknown", vendor & 0xffff,
                product & 0xffff);
        find_trigger_happy_ordinals();
        return;
    }
    /* Nenhum pad na base do SDL: abre o primeiro joystick cru. */
    if (!raw_joystick && SDL_NumJoysticks() > 0) {
        raw_joystick = SDL_JoystickOpen(0);
        if (raw_joystick) {
            const char *name = SDL_JoystickName(raw_joystick);
            hgo_jni_input_device_info("Microsoft X-Box 360 pad",
                                      joystick_vendor(raw_joystick),
                                      joystick_product(raw_joystick),
                                      name ? name : "nextos-gamepad");
            fprintf(stderr,
                    "[hgo/input] controle CRU: \"%s\" (%d botões, %d eixos, "
                    "%d hats) — sem mapeamento na base do SDL\n",
                    name ? name : "desconhecido",
                    SDL_JoystickNumButtons(raw_joystick),
                    SDL_JoystickNumAxes(raw_joystick),
                    SDL_JoystickNumHats(raw_joystick));
            find_trigger_happy_ordinals();
        }
    }
}

static void inject(void *env, void *player, void *event)
{
    static void *native_inject;
    if (!native_inject)
        native_inject = hgo_jni_native("com/unity3d/player/UnityPlayer",
                                       "nativeInjectEvent");
    if (native_inject && event) {
        /* Unity 2022 registers nativeInjectEvent(InputEvent, displayId).
         * The Android Activity passes its default display (0); omitting this
         * fourth native argument leaves an arbitrary register value that the
         * touch scaler later treats as an array index. */
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (getenv("HGO_INPUT_DIAG"))
            fprintf(stderr, "[hgo/input] inject event=%p consumed=%d\n",
                    event, consumed);
    } else if (getenv("HGO_INPUT_DIAG")) {
        fprintf(stderr, "[hgo/input] inject SKIPPED inject=%p event=%p\n",
                native_inject, event);
    }
}

static int touch_begin(void *env, void *player, enum touch_owner owner,
                       float x, float y)
{
    void *event;
    if (hgo_touch_arbiter_owner(&touch_arbiter) != TOUCH_OWNER_NONE)
        return 0;
    event = hgo_jni_touch_event(0, x, y);
    if (!event)
        return 0;
    if (!hgo_touch_arbiter_begin(&touch_arbiter, owner, x, y))
        return 0;
    inject(env, player, event);
    return 1;
}

static void touch_move(void *env, void *player, enum touch_owner owner,
                       float x, float y)
{
    void *event;
    if (hgo_touch_arbiter_owner(&touch_arbiter) != owner)
        return;
    event = hgo_jni_touch_event(2, x, y);
    if (!event)
        return;
    inject(env, player, event);
    (void)hgo_touch_arbiter_move(&touch_arbiter, owner, x, y);
}

static void touch_end(void *env, void *player, enum touch_owner owner,
                      float x, float y)
{
    void *event;
    if (hgo_touch_arbiter_owner(&touch_arbiter) != owner)
        return;
    event = hgo_jni_touch_event(1, x, y);
    if (event)
        inject(env, player, event);
    (void)hgo_touch_arbiter_end(&touch_arbiter, owner, x, y);
}

static void touch_cancel(void *env, void *player, const char *reason)
{
    enum touch_owner owner = hgo_touch_arbiter_owner(&touch_arbiter);
    void *event;
    float x = 0.0f;
    float y = 0.0f;
    if (owner == TOUCH_OWNER_NONE)
        return;
    hgo_touch_arbiter_position(&touch_arbiter, &x, &y);
    event = hgo_jni_touch_event(3, x, y);
    if (event)
        inject(env, player, event);
    (void)hgo_touch_arbiter_cancel(&touch_arbiter);
    if (owner == TOUCH_OWNER_CURSOR)
        cursor_drag_active = 0;
    if (owner == TOUCH_OWNER_SWIPE) {
        swipe_step = 0;
        swipe_latched = 1;
    }
    if (owner == TOUCH_OWNER_SHORTCUT)
        ui_tap_release_pending = 0;
    if (getenv("HGO_INPUT_DIAG"))
        fprintf(stderr, "[hgo/input] touch owner %d canceled (%s)\n",
                owner, reason ? reason : "state change");
}

static void update_cursor(void *env, void *player)
{
    if (!cursor_is_active())
        return;
    int input_available = controller || raw_joystick || virtual_enabled;
    if (!input_available && !cursor_drag_active)
        return;

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = cursor_tick && frequency
             ? (float)((double)(now - cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;

    float x = input_available ? axis_value(cursor_axis(0)) : 0.0f;
    float y = input_available ? axis_value(cursor_axis(1)) : 0.0f;
    float magnitude = sqrtf(x * x + y * y);
    float target_x = 0.0f;
    float target_y = 0.0f;
    const float deadzone = 0.18f;
    if (magnitude > deadzone) {
        float response = (magnitude - deadzone) / (1.0f - deadzone);
        if (response > 1.0f)
            response = 1.0f;
        response *= response;
        target_x = x / magnitude * response * 1050.0f;
        target_y = y / magnitude * response * 1050.0f;
    }
    float blend = 1.0f - expf(-14.0f * dt);
    cursor_vx += (target_x - cursor_vx) * blend;
    cursor_vy += (target_y - cursor_vy) * blend;
    cursor_x += cursor_vx * dt;
    cursor_y += cursor_vy * dt;
    if (cursor_x < 0.0f) cursor_x = 0.0f;
    if (cursor_x > 1279.0f) cursor_x = 1279.0f;
    if (cursor_y < 0.0f) cursor_y = 0.0f;
    if (cursor_y > 719.0f) cursor_y = 719.0f;

    int held = input_available && cursor_click_held();
    float touch_x = cursor_x * screen_width / 1280.0f;
    float touch_y = cursor_y * screen_height / 720.0f;
    if (held && !cursor_drag_active) {
        /* The cursor has priority over a partially emitted synthetic gesture.
         * Cancel the old Android finger before publishing a new DOWN. */
        if (hgo_touch_arbiter_owner(&touch_arbiter) != TOUCH_OWNER_NONE)
            touch_cancel(env, player, "cursor pressed");
        if (touch_begin(env, player, TOUCH_OWNER_CURSOR, touch_x, touch_y)) {
            cursor_drag_active = 1;
            cursor_touch_x = touch_x;
            cursor_touch_y = touch_y;
        }
    } else if (held && cursor_drag_active &&
               (fabsf(touch_x - cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - cursor_touch_y) >= 0.25f)) {
        touch_move(env, player, TOUCH_OWNER_CURSOR, touch_x, touch_y);
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
    }
    if (!held && cursor_drag_active) {
        touch_end(env, player, TOUCH_OWNER_CURSOR, touch_x, touch_y);
        cursor_drag_active = 0;
    }
}

static void update_native_controls(void)
{
    if (!native_controls_enabled || !native_input_manager || !il2cpp_base) {
        native_selection_active = 0;
        native_gameplay_active = 0;
        native_direction_latched = 0;
        return;
    }

    native_selection_active =
        ((uint8_t (*)(void *, void *))
         (il2cpp_base + HGO_TVOS_SELECTION_ACTIVE))(native_input_manager,
                                                     NULL) != 0;
    native_gameplay_active = native_selection_active ||
        (*((uint8_t *)native_input_manager + 0x70) != 0);
    if (native_gameplay_active != native_activity_reported) {
        fprintf(stderr,
                "[hgo/input] native gameplay=%d selection=%d\n",
                native_gameplay_active, native_selection_active);
        native_activity_reported = native_gameplay_active;
    }
    if (!native_gameplay_active) {
        native_direction_latched = 0;
        return;
    }

    /* Mira de pedra: A arremessa (comportamento da v1.1.0 aprovada); o L1
       segue aceito para quem se acostumou com a v1.1.1. */
    const int activate = SDL_CONTROLLER_BUTTON_A;
    if (native_selection_active &&
        ((buttons[activate] && !previous[activate]) ||
         (buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] &&
          !previous[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]))) {
        ((void (*)(void *, void *))(il2cpp_base + HGO_TVOS_CLICK_UP))(
            native_input_manager, NULL);
        fprintf(stderr, "[hgo/input] native selection activate\n");
    }
    if (native_selection_active &&
        ((buttons[SDL_CONTROLLER_BUTTON_B] &&
         !previous[SDL_CONTROLLER_BUTTON_B]) ||
        (buttons[SDL_CONTROLLER_BUTTON_BACK] &&
         !previous[SDL_CONTROLLER_BUTTON_BACK]))) {
        ((void (*)(void *, void *))(il2cpp_base + HGO_TVOS_MENU_UP))(
            native_input_manager, NULL);
    }

    float x = axis_value(move_axis(0));
    float y = -axis_value(move_axis(1));
    float dpad_x = (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] -
                           buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]);
    float dpad_y = (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] -
                           buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]);
    if (dpad_x != 0.0f || dpad_y != 0.0f) {
        x = dpad_x;
        y = dpad_y;
    }
    if (fabsf(x) < 0.55f && fabsf(y) < 0.55f) {
        native_direction_latched = 0;
        return;
    }
    if (native_direction_latched)
        return;
    native_direction_latched = 1;

    if (native_selection_active) {
        uint8_t face_towards_selection =
            *((uint8_t *)native_input_manager + 0x44);
        ((void (*)(void *, float, float, int, void *))
         (il2cpp_base + HGO_TVOS_CHANGE_SELECTION))(
            native_input_manager, x, y, face_towards_selection, NULL);
        if (getenv("HGO_INPUT_DIAG"))
            fprintf(stderr,
                    "[hgo/input] native selection direction %.2f %.2f\n",
                    x, y);
    } else {
        /* InputManager_tvOS.OnSwipe consumes a screen-space position relative
         * to m_InitialPosition.  A 120 px virtual swipe clears its own 50 px
         * threshold, then the original method resolves the adjacent Node and
         * calls LevelState.OnNodeClicked(Node). */
        float *initial = (float *)((uint8_t *)native_input_manager + 0x88);
        initial[0] = 0.0f;
        initial[1] = 0.0f;
        *((uint8_t *)native_input_manager + 0xa2) = 0;
        ((void (*)(void *, float, float, void *))
         (il2cpp_base + HGO_TVOS_ON_SWIPE))(
            native_input_manager, x * 120.0f, y * 120.0f, NULL);
        if (getenv("HGO_INPUT_DIAG"))
            fprintf(stderr, "[hgo/input] native pawn swipe %.2f %.2f\n",
                    x, y);
    }
}

/*
 * ===== Andar por swipe sintético (achado do NextOS, 05/08) =====
 * O InputManager_tvOS e o de toque são MUTUAMENTE exclusivos no jogo: com o
 * tvOS selecionado o LevelState ignora toques nos nós, e a pedra (mira por
 * toque) nunca sai.  Então o modo padrão volta ao gerenciador de TOQUE — tudo
 * clicável — e o D-pad/analógico de movimento vira um swipe sintético, que é
 * mecânica nativa do jogo (swipe em qualquer lugar move o 47).  O caminho
 * tvOS continua atrás de HGO_NATIVE_CONTROLS=1 para comparação.
 */
static int swipe_move_enabled = 1;

static void update_swipe_move(void *env, void *player)
{
    if (!swipe_move_enabled || native_controls_enabled) {
        if (hgo_touch_arbiter_owner(&touch_arbiter) == TOUCH_OWNER_SWIPE)
            touch_cancel(env, player, "swipe disabled");
        return;
    }
    /* nunca por cima de um clique/arraste do cursor: é o mesmo dedo */
    if (cursor_drag_active || cursor_click_held() || ui_tap_release_pending)
        return;

    if (swipe_step > 0) {
        if (hgo_touch_arbiter_owner(&touch_arbiter) != TOUCH_OWNER_SWIPE) {
            swipe_step = 0;
            return;
        }
        float t = (float)swipe_step / 4.0f;
        int action = swipe_step >= 4 ? 1 : 2;  /* 4 = solta, 1..3 = arrasta */
        float x = swipe_from_x + swipe_dx * t;
        float y = swipe_from_y + swipe_dy * t;
        if (action == 1)
            touch_end(env, player, TOUCH_OWNER_SWIPE, x, y);
        else
            touch_move(env, player, TOUCH_OWNER_SWIPE, x, y);
        swipe_step++;
        if (swipe_step > 4)
            swipe_step = 0;
        return;
    }

    float x = axis_value(move_axis(0));
    float y = -axis_value(move_axis(1));
    float dpad_x = (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] -
                           buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]);
    float dpad_y = (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] -
                           buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]);
    if (dpad_x != 0.0f || dpad_y != 0.0f) {
        x = dpad_x;
        y = dpad_y;
    }
    if (fabsf(x) < 0.55f && fabsf(y) < 0.55f) {
        swipe_latched = 0;
        return;
    }
    if (swipe_latched)
        return;
    if (hgo_touch_arbiter_owner(&touch_arbiter) != TOUCH_OWNER_NONE)
        return;
    swipe_latched = 1;

    float magnitude = sqrtf(x * x + y * y);
    float length = 0.22f * (float)(screen_width < screen_height
                                   ? screen_width : screen_height);
    swipe_from_x = screen_width * 0.5f;
    swipe_from_y = screen_height * 0.55f;
    swipe_dx = x / magnitude * length;
    swipe_dy = -y / magnitude * length;   /* tela cresce para baixo */
    if (touch_begin(env, player, TOUCH_OWNER_SWIPE,
                    swipe_from_x, swipe_from_y))
        swipe_step = 1;
    if (getenv("HGO_INPUT_DIAG"))
        fprintf(stderr, "[hgo/input] swipe-move %.2f %.2f\n", x, y);
}

static void update_gameplay_shortcuts(void *env, void *player)
{
    if (ui_tap_release_pending) {
        touch_end(env, player, TOUCH_OWNER_SHORTCUT, ui_tap_x, ui_tap_y);
        ui_tap_release_pending = 0;
    }

    /* The selected cursor button owns the complete press/drag/release gesture. Do
     * not publish a one-frame menu/restart/hint DOWN that update_cursor()
     * would have to cancel later in this same poll: besides being an invalid
     * user gesture, that could leave menu_overlay_state ahead of the UI. */
    if (cursor_click_held())
        return;

    const char *action = NULL;
    float design_x = 0.0f;
    float design_y = 0.0f;

    if (menu_overlay_state == 1 && !native_gameplay_active)
        menu_overlay_state = 2;
    else if (menu_overlay_state == 2 && native_gameplay_active)
        menu_overlay_state = 0;

    if (!native_gameplay_active) {
        if (menu_overlay_state == 2 &&
            ((buttons[SDL_CONTROLLER_BUTTON_B] &&
              !previous[SDL_CONTROLLER_BUTTON_B]) ||
             (buttons[SDL_CONTROLLER_BUTTON_START] &&
              !previous[SDL_CONTROLLER_BUTTON_START]))) {
            action = "back";
            design_x = 821.0f;
            design_y = 48.0f;
        } else {
            return;
        }
    } else if (buttons[SDL_CONTROLLER_BUTTON_START] &&
        !previous[SDL_CONTROLLER_BUTTON_START]) {
        action = "menu";
        design_x = 1208.0f;
        design_y = 70.0f;
    } else if (buttons[SDL_CONTROLLER_BUTTON_Y] &&
               !previous[SDL_CONTROLLER_BUTTON_Y]) {
        action = "restart";
        design_x = 1122.0f;
        design_y = 70.0f;
    } else if (buttons[SDL_CONTROLLER_BUTTON_X] &&
               !previous[SDL_CONTROLLER_BUTTON_X]) {
        action = "hint";
        design_x = 1209.0f;
        design_y = 649.0f;
    }
    if (!action)
        return;

    ui_tap_x = design_x * screen_width / 1280.0f;
    ui_tap_y = design_y * screen_height / 720.0f;
    if (!touch_begin(env, player, TOUCH_OWNER_SHORTCUT,
                     ui_tap_x, ui_tap_y)) {
        if (getenv("HGO_INPUT_DIAG"))
            fprintf(stderr, "[hgo/input] shortcut %s blocked by touch owner %d\n",
                    action, hgo_touch_arbiter_owner(&touch_arbiter));
        return;
    }
    ui_tap_release_pending = 1;
    if (strcmp(action, "menu") == 0)
        menu_overlay_state = 1;
    if (getenv("HGO_INPUT_DIAG"))
        fprintf(stderr, "[hgo/input] gameplay shortcut %s\n", action);
}

int hgo_input_init(void)
{
    hgo_input_lifecycle_init(&input_lifecycle);
    hgo_touch_arbiter_init(&touch_arbiter);
    hgo_input_layout_from_environment(&input_layout);
    virtual_enabled = getenv("HGO_GPVIRT") &&
                      strcmp(getenv("HGO_GPVIRT"), "0") != 0;
    cursor_enabled = getenv("HGO_CURSOR") &&
                     strcmp(getenv("HGO_CURSOR"), "0") != 0;
    native_controls_enabled = getenv("HGO_NATIVE_CONTROLS") &&
                              strcmp(getenv("HGO_NATIVE_CONTROLS"), "0") != 0;
    swipe_move_enabled = !getenv("HGO_SWIPE_MOVE") ||
                         strcmp(getenv("HGO_SWIPE_MOVE"), "0") != 0;
    fprintf(stderr,
            "[hgo/input] layout: cursor=%s movimento=%s+dpad clique=%s "
            "andar=%s\n",
            input_layout.cursor_on_left ? "esquerdo" : "direito",
            input_layout.cursor_on_left ? "direito" : "esquerdo",
            input_layout.cursor_uses_a ? "A" : "R3",
            native_controls_enabled ? "tvOS" :
            (swipe_move_enabled ? "swipe-sintetico" : "so-toque"));
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[hgo/input] SDL controller init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    add_known_mappings();
    open_controller();
    install_incontrol_hooks();
    return (controller || raw_joystick || virtual_enabled) ? 0 : -1;
}

void hgo_input_poll(void *env, void *player, unsigned long frame)
{
    (void)frame;
    memcpy(previous, buttons, sizeof previous);

    int cancel_touch_requested = 0;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                hgo_input_lifecycle_event(&input_lifecycle,
                                          HGO_INPUT_FOCUS_LOST);
                input_rearm_required = 1;
                cancel_touch_requested = 1;
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                hgo_input_lifecycle_event(&input_lifecycle,
                                          HGO_INPUT_FOCUS_GAINED);
            } else if (event.window.event == SDL_WINDOWEVENT_HIDDEN ||
                       event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                hgo_input_lifecycle_event(&input_lifecycle,
                                          HGO_INPUT_HIDDEN);
                input_rearm_required = 1;
                cancel_touch_requested = 1;
            } else if (event.window.event == SDL_WINDOWEVENT_SHOWN ||
                       event.window.event == SDL_WINDOWEVENT_RESTORED) {
                hgo_input_lifecycle_event(&input_lifecycle,
                                          HGO_INPUT_SHOWN);
            }
        }
        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_JOYDEVICEADDED) {
            open_controller();
            input_rearm_required = 1;
        }
        if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
            SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
            if (joy && SDL_JoystickInstanceID(joy) == event.cdevice.which) {
                SDL_GameControllerClose(controller);
                controller = NULL;
                memset(buttons, 0, sizeof buttons);
                input_rearm_required = 1;
                cancel_touch_requested = 1;
                open_controller();
            }
        }
        if (event.type == SDL_JOYDEVICEREMOVED && raw_joystick &&
            SDL_JoystickInstanceID(raw_joystick) == event.jdevice.which) {
            SDL_JoystickClose(raw_joystick);
            raw_joystick = NULL;
            memset(buttons, 0, sizeof buttons);
            input_rearm_required = 1;
            cancel_touch_requested = 1;
            open_controller();
        }
    }
    if (cancel_touch_requested)
        touch_cancel(env, player, "focus or controller lost");
    if (controller) {
        SDL_GameControllerUpdate();
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
            buttons[i] = SDL_GameControllerGetButton(
                controller, (SDL_GameControllerButton)i) ? 1 : 0;
    } else if (raw_joystick) {
        SDL_JoystickUpdate();
        memset(buttons, 0, sizeof buttons);
        /* ordem posicional dos pads USB/handheld comuns */
        int count = SDL_JoystickNumButtons(raw_joystick);
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
            int index = raw_button_index((SDL_GameControllerButton)i);
            if (index >= 0 && index < count)
                buttons[i] = SDL_JoystickGetButton(raw_joystick, index) ? 1 : 0;
        }
        /* d-pad: hat quando existe, senão botões 12..15 (RK3326 e família) */
        if (SDL_JoystickNumHats(raw_joystick) > 0) {
            Uint8 hat = SDL_JoystickGetHat(raw_joystick, 0);
            buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] = (hat & SDL_HAT_UP) ? 1 : 0;
            buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = (hat & SDL_HAT_DOWN) ? 1 : 0;
            buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = (hat & SDL_HAT_LEFT) ? 1 : 0;
            buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = (hat & SDL_HAT_RIGHT) ? 1 : 0;
        } else if (count > 15) {
            buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] = SDL_JoystickGetButton(raw_joystick, 12) ? 1 : 0;
            buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = SDL_JoystickGetButton(raw_joystick, 13) ? 1 : 0;
            buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = SDL_JoystickGetButton(raw_joystick, 14) ? 1 : 0;
            buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = SDL_JoystickGetButton(raw_joystick, 15) ? 1 : 0;
        }
    } else {
        memset(buttons, 0, sizeof buttons);
    }
    apply_trigger_happy_buttons();
    poll_virtual_controller();

    if (exit_requested) {
        touch_cancel(env, player, "runtime exit");
        memset(buttons, 0, sizeof buttons);
        return;
    }

    if (hgo_input_lifecycle_suspended(&input_lifecycle)) {
        memset(buttons, 0, sizeof buttons);
        return;
    }
    if (input_rearm_required) {
        if (input_controls_neutral()) {
            input_rearm_required = 0;
            if (getenv("HGO_INPUT_DIAG"))
                fprintf(stderr, "[hgo/input] controls rearmed after boundary\n");
        }
        memset(buttons, 0, sizeof buttons);
        return;
    }

    update_native_controls();

    if (!controller && !raw_joystick && !virtual_enabled)
        return;

    int select = buttons[SDL_CONTROLLER_BUTTON_BACK] ||
                 buttons[SDL_CONTROLLER_BUTTON_GUIDE];
    if (select && buttons[SDL_CONTROLLER_BUTTON_START]) {
        exit_requested = 1;
        touch_cancel(env, player, "exit chord");
        memset(buttons, 0, sizeof buttons);
        return;
    }

    update_gameplay_shortcuts(env, player);

    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        if (cursor_is_active() && i == SDL_CONTROLLER_BUTTON_RIGHTSTICK)
            continue;
        if (a_is_click_button() && i == SDL_CONTROLLER_BUTTON_A)
            continue;
        if (native_selection_active && i == SDL_CONTROLLER_BUTTON_A)
            continue;
        if (native_selection_active && i == SDL_CONTROLLER_BUTTON_B)
            continue;
        if (native_gameplay_active &&
            (i == SDL_CONTROLLER_BUTTON_START ||
             i == SDL_CONTROLLER_BUTTON_Y ||
             i == SDL_CONTROLLER_BUTTON_X))
            continue;
        if (menu_overlay_state == 2 &&
            (i == SDL_CONTROLLER_BUTTON_B ||
             i == SDL_CONTROLLER_BUTTON_START))
            continue;
        if (!android_key[i] || buttons[i] == previous[i])
            continue;
        inject(env, player,
               hgo_jni_key_event(buttons[i] ? 0 : 1, android_key[i], i));
    }

    float lx = axis_value(move_axis(0));
    float ly = axis_value(move_axis(1));
    float rx = axis_value(cursor_axis(0));
    float ry = axis_value(cursor_axis(1));
    float lt = axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    float rt = axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    float hx = (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] -
                       buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]);
    float hy = (float)(buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] -
                       buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]);
    inject(env, player, hgo_jni_motion_event(lx, ly, rx, ry, lt, rt, hx, hy));
    update_cursor(env, player);
    update_swipe_move(env, player);

    if (getenv("HGO_INPUT_DIAG") && frame > 0 && frame % 300 == 0) {
        fprintf(stderr,
                "[hgo/input] diag names=%lu raw-buttons=%lu mask=%#x "
                "raw-analogs=%lu mask=%#x\n",
                joystick_name_calls, raw_button_calls, queried_buttons,
                raw_analog_calls, queried_analogs);
    }
}

void hgo_input_close(void)
{
    if (controller) {
        SDL_GameControllerClose(controller);
        controller = NULL;
    }
    if (raw_joystick) {
        SDL_JoystickClose(raw_joystick);
        raw_joystick = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
                      SDL_INIT_EVENTS);
}

void hgo_input_cancel_touch(void *env, void *player)
{
    touch_cancel(env, player, "runtime shutdown");
}

int hgo_input_exit_requested(void)
{
    return exit_requested;
}

int hgo_input_cursor(float *x, float *y)
{
    if (!cursor_is_active())
        return 0;
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
    return 1;
}

void hgo_input_set_screen_size(int width, int height)
{
    if (width > 0) screen_width = width;
    if (height > 0) screen_height = height;
}

void hgo_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void hgo_input_keyboard_set(const char *text)
{
    (void)text;
}

void hgo_input_keyboard_hide(void)
{
}

int hgo_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const hgo_keyboard_key **keys,
                                size_t *key_count)
{
    if (text && text_size) text[0] = '\0';
    if (uppercase) *uppercase = 0;
    if (selected) *selected = 0;
    if (keys) *keys = NULL;
    if (key_count) *key_count = 0;
    return 0;
}
