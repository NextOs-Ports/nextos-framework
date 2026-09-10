/* Native Android Activity/Unity lifecycle host for Dead Trigger 2.1.0. */
#define _GNU_SOURCE

#include "dt.h"
#include "egl_sdl.h"
#include "falsojni.h"
#include "framework_bridge.h"
#include "gamepad_dt.h"
#include "media_dt.h"

#include <SDL2/SDL.h>
#include "pad_ordinal_fix.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define DT_PACKAGE "com.madfingergames.deadtrigger"

typedef int32_t jint;
typedef uint8_t jboolean;
typedef void *jobject;

typedef void (*jni_init_fn)(void *, jobject, jobject);
typedef void (*jni_recreate_fn)(void *, jobject, jint, jobject);
typedef void (*jni_void_fn)(void *, jobject);
typedef void (*jni_bool_arg_fn)(void *, jobject, jboolean);
typedef jboolean (*jni_bool_fn)(void *, jobject);
typedef jboolean (*jni_inject_fn)(void *, jobject, jobject);

struct unity_api {
    jni_init_fn init_jni;
    jni_recreate_fn recreate_gfx;
    jni_void_fn surface_changed;
    jni_bool_arg_fn focus_changed;
    jni_void_fn resume;
    jni_bool_fn render;
    jni_bool_fn pause;
    jni_bool_fn done;
    jni_inject_fn inject;
};

static struct unity_api g_unity;
static atomic_int g_quit;
static atomic_int g_unity_ready;
static atomic_int g_unity_exited;
static atomic_ulong g_frames;
static int g_surface_token;
extern int g_main_tid;

static uint64_t monotonic_milliseconds(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000u +
           (uint64_t)time.tv_nsec / 1000000u;
}

static void signal_quit(int signal_number) {
    (void)signal_number;
    atomic_store(&g_quit, 1);
}

static void crash_handler(int signal_number, siginfo_t *info, void *context) {
    ucontext_t *machine = (ucontext_t *)context;
    uintptr_t pc = 0, sp = 0, lr = 0;
#if defined(__aarch64__)
    pc = machine->uc_mcontext.pc;
    sp = machine->uc_mcontext.sp;
    lr = machine->uc_mcontext.regs[30];
#endif
    uintptr_t unity = dt_unity_base();
    uintptr_t il2cpp = dt_il2cpp_base();
    fprintf(stderr,
            "\n[crash] sinal=%d endereco=%p pc=%p lr=%p sp=%p tid=%ld",
            signal_number, info ? info->si_addr : NULL, (void *)pc,
            (void *)lr, (void *)sp, syscall(SYS_gettid));
    if (unity && pc >= unity && pc < unity + 0x2000000)
        fprintf(stderr, " libunity+0x%lx", (unsigned long)(pc - unity));
    else if (il2cpp && pc >= il2cpp && pc < il2cpp + 0x3000000)
        fprintf(stderr, " libil2cpp+0x%lx", (unsigned long)(pc - il2cpp));
    fputc('\n', stderr);
    fsync(STDERR_FILENO);
    _exit(128 + signal_number);
}

static void install_handlers(void) {
    struct sigaction quit_action;
    memset(&quit_action, 0, sizeof quit_action);
    quit_action.sa_handler = signal_quit;
    sigemptyset(&quit_action.sa_mask);
    sigaction(SIGINT, &quit_action, NULL);
    sigaction(SIGTERM, &quit_action, NULL);

    struct sigaction crash_action;
    memset(&crash_action, 0, sizeof crash_action);
    crash_action.sa_sigaction = crash_handler;
    crash_action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&crash_action.sa_mask);
    sigaction(SIGSEGV, &crash_action, NULL);
    sigaction(SIGBUS, &crash_action, NULL);
    sigaction(SIGILL, &crash_action, NULL);
    sigaction(SIGFPE, &crash_action, NULL);
    sigaction(SIGABRT, &crash_action, NULL);
}

static int make_directory(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    fprintf(stderr, "[host] mkdir %s: %s\n", path, strerror(errno));
    return -1;
}

static int find_unity_api(void) {
#define FIND(field, name)                                                      \
    do {                                                                       \
        g_unity.field = (void *)jni_find_native(name);                         \
        if (!g_unity.field) {                                                  \
            fprintf(stderr, "[jni] nativo obrigatorio ausente: %s\n", name);  \
            return -1;                                                         \
        }                                                                      \
    } while (0)
    FIND(init_jni, "initJni");
    FIND(recreate_gfx, "nativeRecreateGfxState");
    FIND(surface_changed, "nativeSendSurfaceChangedEvent");
    FIND(focus_changed, "nativeFocusChanged");
    FIND(resume, "nativeResume");
    FIND(render, "nativeRender");
    FIND(pause, "nativePause");
    FIND(done, "nativeDone");
    FIND(inject, "nativeInjectEvent");
#undef FIND
    return 0;
}

static void *unity_main(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "UnityMain");
    void *environment = jni_get_env();
    jobject activity = jni_get_activity();
    jobject surface = &g_surface_token;

    /*
     * SurfaceHolder.Callback flow from this exact APK:
     * surfaceCreated -> updateDisplay; surfaceChanged -> updateDisplay +
     * nativeSendSurfaceChangedEvent. Both updateDisplay calls execute on
     * UnityMain and wait for completion.
     */
    fprintf(stderr, "[flow] UnityMain: surfaceCreated\n");
    g_unity.recreate_gfx(environment, activity, 0, surface);
    fprintf(stderr, "[flow] UnityMain: surfaceChanged %dx%d\n",
            dt_android_width(), dt_android_height());
    g_unity.recreate_gfx(environment, activity, 0, surface);
    g_unity.surface_changed(environment, activity);

    /*
     * windowFocusChanged(true) queues FOCUS_GAINED first; checkResumePlayer
     * then queues nativeResume once activity/surface/focus are all valid.
     */
    fprintf(stderr, "[flow] UnityMain: focus gained -> resume\n");
    g_unity.focus_changed(environment, activity, 1);
    g_unity.resume(environment, activity);
    atomic_store(&g_unity_ready, 1);

    while (!atomic_load(&g_quit)) {
        dt_gamepad_frame_begin();
        dt_gamepad_try_install();
        dt_media_try_install();
        if (!g_unity.render(environment, activity)) {
            fprintf(stderr, "[flow] nativeRender solicitou encerramento\n");
            atomic_store(&g_quit, 1);
            break;
        }
        unsigned long frame = atomic_fetch_add(&g_frames, 1) + 1;
        if (frame == 1 || frame == 60 || frame % 600 == 0)
            fprintf(stderr, "[frame] %lu\n", frame);
    }

    fprintf(stderr, "[flow] UnityMain: focus lost -> pause\n");
    g_unity.focus_changed(environment, activity, 0);
    jboolean requests_shutdown = g_unity.pause(environment, activity);
    fprintf(stderr, "[flow] nativePause=%u\n", requests_shutdown);
    if (requests_shutdown)
        fprintf(stderr, "[flow] nativeDone=%u\n",
                g_unity.done(environment, activity));
    atomic_store(&g_unity_exited, 1);
    return NULL;
}

static float normalized_axis(Sint16 value) {
    if (value >= 0)
        return (float)value / 32767.0f;
    return (float)value / 32768.0f;
}

/*
 * Receita NextOS de controles: pad_ordinal_fix antes de IsGameController,
 * TODOS os joysticks abertos e hotplug tratado. O dongle sem fio cria mais
 * de um nó e trocar de pad com o jogo aberto matava o controle.
 */
#define DT_MAX_PADS 4
static SDL_GameController *g_pads[DT_MAX_PADS];
static int g_l2_raw_fallback[DT_MAX_PADS];
static int g_r2_raw_fallback[DT_MAX_PADS];
static int g_r3_raw_fallback[DT_MAX_PADS];

static int controller_button_is_unmapped(
        SDL_GameController *controller, SDL_GameControllerButton button) {
    SDL_GameControllerButtonBind binding =
        SDL_GameControllerGetBindForButton(controller, button);
    return binding.bindType == SDL_CONTROLLER_BINDTYPE_NONE;
}

static int controller_button_is_raw(
        SDL_GameController *controller, SDL_GameControllerButton button,
        int raw_button) {
    SDL_GameControllerButtonBind binding =
        SDL_GameControllerGetBindForButton(controller, button);
    return binding.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
           binding.value.button == raw_button;
}

static int controller_axis_is_unmapped(
        SDL_GameController *controller, SDL_GameControllerAxis axis) {
    SDL_GameControllerButtonBind binding =
        SDL_GameControllerGetBindForAxis(controller, axis);
    return binding.bindType == SDL_CONTROLLER_BINDTYPE_NONE;
}

/*
 * SDL's built-in mapping on some older firmwares truncates the classic
 * 12-button USB layout after Start (b9), leaving physical L2/R2/L3/R3 visible
 * to SDL_Joystick but absent from SDL_GameController.  Recognize the layout
 * by capabilities and already-mapped semantics, never by device name, GUID,
 * CFW or screen backend.  Existing complete mappings remain untouched.
 */
static int controller_has_classic_12_button_layout(
        SDL_GameController *controller) {
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    return joystick && SDL_JoystickNumButtons(joystick) >= 12 &&
           controller_button_is_raw(
               controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 4) &&
           controller_button_is_raw(
               controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 5) &&
           controller_button_is_raw(
               controller, SDL_CONTROLLER_BUTTON_BACK, 8) &&
           controller_button_is_raw(
               controller, SDL_CONTROLLER_BUTTON_START, 9);
}

static int any_pad(void) {
    for (int slot = 0; slot < DT_MAX_PADS; ++slot)
        if (g_pads[slot])
            return 1;
    return 0;
}

static void open_pad_index(int index) {
    pad_ordinal_fix_apply(index, "DT");
    if (!SDL_IsGameController(index))
        return;
    SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
    int free_slot = -1;
    for (int slot = 0; slot < DT_MAX_PADS; ++slot) {
        if (g_pads[slot]) {
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(
                    g_pads[slot])) == instance)
                return;
        } else if (free_slot < 0) {
            free_slot = slot;
        }
    }
    if (free_slot < 0)
        return;
    SDL_GameController *controller = SDL_GameControllerOpen(index);
    if (!controller)
        return;
    g_pads[free_slot] = controller;
    int classic_12 = controller_has_classic_12_button_layout(controller);
    g_l2_raw_fallback[free_slot] = classic_12 &&
        controller_axis_is_unmapped(
            controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    g_r2_raw_fallback[free_slot] = classic_12 &&
        controller_axis_is_unmapped(
            controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    g_r3_raw_fallback[free_slot] = classic_12 &&
        controller_button_is_unmapped(
            controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    fprintf(stderr, "[input] controle %d: %s\n", free_slot,
            SDL_GameControllerName(controller));
    char *mapping = SDL_GameControllerMapping(controller);
    if (mapping) {
        fprintf(stderr, "[input] mapping: %s\n", mapping);
        SDL_free(mapping);
    }
    if (g_l2_raw_fallback[free_slot] ||
        g_r2_raw_fallback[free_slot] ||
        g_r3_raw_fallback[free_slot])
        fprintf(stderr,
                "[input] mapping incompleto recuperado por capacidade: "
                "L2=%s R2=%s R3=%s (layout fisico b6/b7/b11)\n",
                g_l2_raw_fallback[free_slot] ? "b6" : "nativo",
                g_r2_raw_fallback[free_slot] ? "b7" : "nativo",
                g_r3_raw_fallback[free_slot] ? "b11" : "nativo");
}

static void open_all_pads(void) {
    for (int index = 0; index < SDL_NumJoysticks(); ++index)
        open_pad_index(index);
    if (!any_pad())
        fprintf(stderr, "[input] nenhum controle SDL encontrado no inicio\n");
}

static void close_pad_instance(SDL_JoystickID instance) {
    for (int slot = 0; slot < DT_MAX_PADS; ++slot) {
        if (g_pads[slot] &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(
                g_pads[slot])) == instance) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_l2_raw_fallback[slot] = 0;
            g_r2_raw_fallback[slot] = 0;
            g_r3_raw_fallback[slot] = 0;
            fprintf(stderr, "[input] controle %d removido\n", slot);
        }
    }
}

static int merged_button(SDL_GameControllerButton button) {
    for (int slot = 0; slot < DT_MAX_PADS; ++slot) {
        if (!g_pads[slot])
            continue;
        if (SDL_GameControllerGetButton(g_pads[slot], button))
            return 1;
        if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK &&
            g_r3_raw_fallback[slot] &&
            SDL_JoystickGetButton(
                SDL_GameControllerGetJoystick(g_pads[slot]), 11))
            return 1;
    }
    return 0;
}

static Sint16 merged_axis(SDL_GameControllerAxis axis) {
    Sint16 best = 0;
    for (int slot = 0; slot < DT_MAX_PADS; ++slot) {
        if (!g_pads[slot])
            continue;
        Sint16 value = SDL_GameControllerGetAxis(g_pads[slot], axis);
        if (abs(value) > abs(best))
            best = value;
    }
    return best;
}

static int merged_raw_button(const int fallback[DT_MAX_PADS],
                             int raw_button) {
    for (int slot = 0; slot < DT_MAX_PADS; ++slot) {
        if (g_pads[slot] && fallback[slot] &&
            SDL_JoystickGetButton(
                SDL_GameControllerGetJoystick(g_pads[slot]), raw_button))
            return 1;
    }
    return 0;
}

static Sint16 merged_trigger(SDL_GameControllerAxis axis,
                             const int fallback[DT_MAX_PADS],
                             int raw_button) {
    if (merged_raw_button(fallback, raw_button))
        return INT16_MAX;
    return merged_axis(axis);
}

/* Triggers get hysteresis so an analog resting near the threshold does not
 * flicker fire/aim on and off. */
static int g_fire_held;
static int g_aim_held;

static uint32_t semantic_buttons(void) {
    if (!any_pad()) {
        g_fire_held = g_aim_held = 0;
        return 0;
    }
    uint32_t level = 0;
    Sint16 right_trigger = merged_trigger(
        SDL_CONTROLLER_AXIS_TRIGGERRIGHT, g_r2_raw_fallback, 7);
    Sint16 left_trigger = merged_trigger(
        SDL_CONTROLLER_AXIS_TRIGGERLEFT, g_l2_raw_fallback, 6);
    g_fire_held = right_trigger > (g_fire_held ? 8000 : 12000);
    g_aim_held = left_trigger > (g_aim_held ? 8000 : 12000);
    if (g_fire_held)
        level |= 1u << 0;  /* Fire */
    if (merged_button(SDL_CONTROLLER_BUTTON_X))
        level |= 1u << 1;  /* Reload */
    if (merged_button(SDL_CONTROLLER_BUTTON_START))
        level |= 1u << 2;  /* Pause */
    if (merged_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        level |= 1u << 3;  /* Previous weapon */
    if (merged_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
        merged_button(SDL_CONTROLLER_BUTTON_Y))
        level |= 1u << 4;  /* Next weapon */
    if (g_aim_held)
        level |= 1u << 5;  /* Aim */
    if (merged_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        level |= 1u << 6;  /* Item 1 */
    if (merged_button(SDL_CONTROLLER_BUTTON_DPAD_UP))
        level |= 1u << 7;  /* Item 2 */
    if (merged_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        level |= 1u << 8;  /* Item 3 */
    if (merged_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        level |= 1u << 9;  /* Item 4 */
    if (merged_button(SDL_CONTROLLER_BUTTON_A))
        level |= 1u << 10; /* Action */
    return level;
}

static void publish_controller(int cursor_active,
                               int test_axis_active,
                               float test_left_x, float test_left_y,
                               float test_right_x, float test_right_y,
                               int test_button) {
    float left_x = 0.0f, left_y = 0.0f;
    float right_x = 0.0f, right_y = 0.0f;
    uint32_t buttons = semantic_buttons();
    if (any_pad()) {
        left_x = normalized_axis(merged_axis(SDL_CONTROLLER_AXIS_LEFTX));
        left_y = normalized_axis(merged_axis(SDL_CONTROLLER_AXIS_LEFTY));
        right_x = normalized_axis(merged_axis(SDL_CONTROLLER_AXIS_RIGHTX));
        right_y = normalized_axis(merged_axis(SDL_CONTROLLER_AXIS_RIGHTY));
    }
    if (test_axis_active) {
        left_x = test_left_x;
        left_y = test_left_y;
        right_x = test_right_x;
        right_y = test_right_y;
    } else if (cursor_active) {
        /* In touch menus the right stick belongs exclusively to the arrow.
         * Left stick, D-pad and buttons remain on the game's native path. */
        right_x = 0.0f;
        right_y = 0.0f;
    }
    if (test_button >= 0 && test_button <= 10)
        buttons |= 1u << test_button;
    dt_gamepad_publish(left_x, left_y, right_x, right_y, buttons, any_pad());
}

static int inject_android_key(int keycode, int pressed) {
    if (!atomic_load(&g_unity_ready))
        return 0;
    jobject event = jni_pad_key_event(pressed ? 0 : 1, keycode, 0);
    return g_unity.inject(jni_get_env(), jni_get_activity(), event);
}

static int inject_touch(int action, float x, float y) {
    if (!atomic_load(&g_unity_ready))
        return 0;
    jobject event = jni_touch_event(action, x, y);
    return g_unity.inject(jni_get_env(), jni_get_activity(), event);
}

static int run_ui_loop(pthread_t unity_thread) {
    open_all_pads();
    uint64_t exit_chord_since = 0;
    uint64_t started = monotonic_milliseconds();
    uint64_t last_motion = 0;
    uint64_t last_cursor_update = started;
    uint64_t last_cursor_activity = started;
    const int cursor_width = dt_android_width();
    const int cursor_height = dt_android_height();
    float cursor_x = (float)cursor_width * 0.5f;
    float cursor_y = (float)cursor_height * 0.5f;
    float cursor_velocity_x = 0.0f, cursor_velocity_y = 0.0f;
    int cursor_active = 1;
    int previous_cursor_active = 1;
    int cursor_touch_stage = 0;
    uint64_t next_cursor_touch = 0;
    int previous_r3_down = 0;
    int r3_press_pending = 0;
    int previous_a_down = 0;
    int a_press_pending = 0;
    int previous_b_down = 0;
    int b_press_pending = 0;
    int b_release_pending = 0;
    const char *cursor_touch_source = "R3";
    dt_cursor_update(cursor_x, cursor_y, any_pad() && cursor_active);

    float test_left_x = 0.0f, test_left_y = 0.0f;
    float test_right_x = 0.0f, test_right_y = 0.0f;
    const char *test_axis;
    if ((test_axis = getenv("DT_TEST_LEFT_X")))
        test_left_x = strtof(test_axis, NULL);
    if ((test_axis = getenv("DT_TEST_LEFT_Y")))
        test_left_y = strtof(test_axis, NULL);
    if ((test_axis = getenv("DT_TEST_RIGHT_X")))
        test_right_x = strtof(test_axis, NULL);
    if ((test_axis = getenv("DT_TEST_RIGHT_Y")))
        test_right_y = strtof(test_axis, NULL);
    uint64_t test_axis_delay = 0, test_axis_duration = 0;
    const char *test_axis_file = getenv("DT_TEST_AXIS_FILE");
    const char *test_button_file = getenv("DT_TEST_BUTTON_FILE");
    int test_axis_was_active = 0;
    if ((test_axis = getenv("DT_TEST_AXIS_DELAY_MS")))
        test_axis_delay = strtoull(test_axis, NULL, 10);
    if ((test_axis = getenv("DT_TEST_AXIS_DURATION_MS")))
        test_axis_duration = strtoull(test_axis, NULL, 10);
    unsigned maximum_seconds = 0;
    const char *limit = getenv("DT_MAX_SECONDS");
    if (limit)
        maximum_seconds = (unsigned)strtoul(limit, NULL, 10);
    unsigned auto_a_count = 0;
    unsigned auto_a_interval = 6000;
    uint64_t next_auto_a = 0;
    const char *auto_a = getenv("DT_AUTO_A_COUNT");
    if (auto_a) {
        auto_a_count = (unsigned)strtoul(auto_a, NULL, 10);
        const char *delay = getenv("DT_AUTO_A_DELAY_MS");
        const char *interval = getenv("DT_AUTO_A_INTERVAL_MS");
        next_auto_a = started +
            (delay ? strtoull(delay, NULL, 10) : 10000ULL);
        if (interval)
            auto_a_interval = (unsigned)strtoul(interval, NULL, 10);
    }
    unsigned auto_tap_stage = 0;
    uint64_t next_auto_tap = 0;
    float auto_tap_x = 0.0f, auto_tap_y = 0.0f;
    const char *tap_x = getenv("DT_AUTO_TAP_X");
    const char *tap_y = getenv("DT_AUTO_TAP_Y");
    const char *tap_file = getenv("DT_TEST_TAP_FILE");
    int tap_file_triggered = 0;
    const char *tap2_file = getenv("DT_TEST_TAP2_FILE");
    const char *tap2_x_text = getenv("DT_TEST_TAP2_X");
    const char *tap2_y_text = getenv("DT_TEST_TAP2_Y");
    float tap2_x = tap2_x_text ? strtof(tap2_x_text, NULL) : 0.0f;
    float tap2_y = tap2_y_text ? strtof(tap2_y_text, NULL) : 0.0f;
    int tap2_file_triggered = 0;
    if (tap_x && tap_y) {
        const char *delay = getenv("DT_AUTO_TAP_DELAY_MS");
        auto_tap_x = strtof(tap_x, NULL);
        auto_tap_y = strtof(tap_y, NULL);
        if (!tap_file) {
            next_auto_tap = started +
                (delay ? strtoull(delay, NULL, 10) : 10000ULL);
            auto_tap_stage = 1;
        }
    }

    while (!atomic_load(&g_quit)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            dt_framework_observe_input(&event);
            if (event.type == SDL_QUIT) {
                atomic_store(&g_quit, 1);
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                open_pad_index(event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                close_pad_instance(event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN ||
                       event.type == SDL_CONTROLLERBUTTONUP) {
                int pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
                SDL_GameControllerButton button =
                    (SDL_GameControllerButton)event.cbutton.button;
                if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK && pressed)
                    r3_press_pending = 1;
                if (button == SDL_CONTROLLER_BUTTON_A && pressed)
                    a_press_pending = 1;
                if (button == SDL_CONTROLLER_BUTTON_B) {
                    if (pressed)
                        b_press_pending = 1;
                    else
                        b_release_pending = 1;
                }
            } else if (event.type == SDL_JOYBUTTONDOWN) {
                for (int slot = 0; slot < DT_MAX_PADS; ++slot) {
                    if (g_pads[slot] && g_r3_raw_fallback[slot] &&
                        event.jbutton.button == 11 &&
                        SDL_JoystickInstanceID(
                            SDL_GameControllerGetJoystick(g_pads[slot])) ==
                            event.jbutton.which) {
                        r3_press_pending = 1;
                        break;
                    }
                }
            }
        }
        dt_framework_poll_input();

        uint64_t now = monotonic_milliseconds();
        cursor_active = !dt_gamepad_gameplay_active();
        if (cursor_active != previous_cursor_active) {
            fprintf(stderr, "[input] contexto: %s\n",
                    cursor_active ? "menu-touch" : "gameplay-nativo");
            if (cursor_active)
                last_cursor_activity = now;
            else if (cursor_touch_stage) {
                inject_touch(3, cursor_x, cursor_y);
                cursor_touch_stage = 0;
            }
            cursor_velocity_x = 0.0f;
            cursor_velocity_y = 0.0f;
            previous_cursor_active = cursor_active;
        }

        /*
         * Read button LEVELS every frame. Some old SDL/KMSDRM builds expose
         * the correct controller mapping but occasionally coalesce or drop a
         * BUTTONDOWN event. The event latch above only preserves very short
         * taps; it is never the source of truth for a held button.
         */
        int r3_down = merged_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        int r3_pressed = r3_press_pending ||
                         (r3_down && !previous_r3_down);
        int a_down = merged_button(SDL_CONTROLLER_BUTTON_A);
        int a_pressed = a_press_pending ||
                        (a_down && !previous_a_down);
        r3_press_pending = 0;
        a_press_pending = 0;
        previous_r3_down = r3_down;
        previous_a_down = a_down;
        if (cursor_active && (r3_pressed || a_pressed) &&
            !cursor_touch_stage) {
            cursor_touch_source = r3_pressed ? "R3" : "A/Cross";
            int consumed = inject_touch(0, cursor_x, cursor_y);
            cursor_touch_stage = 1;
            next_cursor_touch = now + 40;
            last_cursor_activity = now;
            fprintf(stderr,
                    "[input] %s toque DOWN (%.0f, %.0f) consumido=%d\n",
                    cursor_touch_source, cursor_x, cursor_y, consumed);
        }

        int b_down = merged_button(SDL_CONTROLLER_BUTTON_B);
        if (cursor_active) {
            if (b_press_pending || (b_down && !previous_b_down))
                inject_android_key(4, 1);
            if (b_release_pending || (!b_down && previous_b_down))
                inject_android_key(4, 0);
            if (b_press_pending || b_release_pending ||
                b_down != previous_b_down)
                last_cursor_activity = now;
        }
        b_press_pending = 0;
        b_release_pending = 0;
        previous_b_down = b_down;

        dt_media_set_skip_requested(
            merged_button(SDL_CONTROLLER_BUTTON_START) ||
            merged_button(SDL_CONTROLLER_BUTTON_A) ||
            merged_button(SDL_CONTROLLER_BUTTON_B));
        if (any_pad() && cursor_active) {
            float elapsed =
                (float)(now - last_cursor_update) / 1000.0f;
            if (elapsed > 0.05f)
                elapsed = 0.05f;
            float horizontal = normalized_axis(
                merged_axis(SDL_CONTROLLER_AXIS_RIGHTX));
            float vertical = normalized_axis(
                merged_axis(SDL_CONTROLLER_AXIS_RIGHTY));
            float magnitude = hypotf(horizontal, vertical);
            float desired_x = 0.0f, desired_y = 0.0f;
            const float deadzone = 0.18f;
            if (magnitude > deadzone) {
                if (magnitude > 1.0f)
                    magnitude = 1.0f;
                float response =
                    (magnitude - deadzone) / (1.0f - deadzone);
                /* Progressive response: precise near center, fast at edge. */
                float speed = (float)cursor_width * 0.70f *
                              response * response;
                desired_x = horizontal / magnitude * speed;
                desired_y = vertical / magnitude * speed;
                last_cursor_activity = now;
            }
            /* Time-based low-pass smoothing, independent of frame rate. */
            float blend = 14.0f * elapsed;
            if (blend > 1.0f)
                blend = 1.0f;
            cursor_velocity_x += (desired_x - cursor_velocity_x) * blend;
            cursor_velocity_y += (desired_y - cursor_velocity_y) * blend;
            cursor_x += cursor_velocity_x * elapsed;
            cursor_y += cursor_velocity_y * elapsed;
            if (cursor_x < 0.0f) cursor_x = 0.0f;
            if (cursor_x > (float)(cursor_width - 1))
                cursor_x = (float)(cursor_width - 1);
            if (cursor_y < 0.0f) cursor_y = 0.0f;
            if (cursor_y > (float)(cursor_height - 1))
                cursor_y = (float)(cursor_height - 1);
        }
        /* Receita: cursor some sozinho depois de ~2,5 s parado. */
        dt_cursor_update(cursor_x, cursor_y,
                         any_pad() && cursor_active &&
                         (now - last_cursor_activity < 2500 ||
                          cursor_touch_stage));
        last_cursor_update = now;

        int test_button = -1;
        if (test_button_file &&
            access(test_button_file, F_OK) == 0) {
            FILE *button_file = fopen(test_button_file, "r");
            if (button_file) {
                if (fscanf(button_file, "%d", &test_button) != 1)
                    test_button = -1;
                fclose(button_file);
            }
        }

        int test_active =
            test_axis_duration &&
            now >= started + test_axis_delay &&
            now < started + test_axis_delay + test_axis_duration;
        if (test_axis_file && access(test_axis_file, F_OK) == 0)
            test_active = 1;
        if (any_pad() && now - last_motion >= 16) {
            if (test_active) {
                if (!test_axis_was_active && test_axis_file) {
                    FILE *axis_file = fopen(test_axis_file, "r");
                    float left_x, left_y, right_x, right_y;
                    if (axis_file &&
                        fscanf(axis_file, "%f %f %f %f",
                               &left_x, &left_y,
                               &right_x, &right_y) == 4) {
                        test_left_x = left_x;
                        test_left_y = left_y;
                        test_right_x = right_x;
                        test_right_y = right_y;
                    }
                    if (axis_file)
                        fclose(axis_file);
                }
                if (!test_axis_was_active)
                    fprintf(stderr,
                            "[input-test] eixo semantico "
                            "L=(%.2f,%.2f) R=(%.2f,%.2f)\n",
                            test_left_x, test_left_y,
                            test_right_x, test_right_y);
            }
            test_axis_was_active = test_active;
            last_motion = now;
        }
        publish_controller(cursor_active, test_active,
                           test_left_x, test_left_y,
                           test_right_x, test_right_y, test_button);
        if (cursor_touch_stage && now >= next_cursor_touch) {
            /* Keep DOWN alive for nine dispatches, matching the proven
             * diagnostic tap. Slow first-load frames must still observe a
             * live touch before uGUI receives ACTION_UP. */
            if (cursor_touch_stage <= 9) {
                inject_touch(2, cursor_x, cursor_y);
                ++cursor_touch_stage;
            } else {
                inject_touch(1, cursor_x, cursor_y);
                fprintf(stderr, "[input] %s toque UP (%.0f, %.0f)\n",
                        cursor_touch_source, cursor_x, cursor_y);
                cursor_touch_stage = 0;
            }
            next_cursor_touch = now + 40;
        }
        int start_down = merged_button(SDL_CONTROLLER_BUTTON_START);
        int select_down = merged_button(SDL_CONTROLLER_BUTTON_BACK);
        if (start_down && select_down) {
            if (!exit_chord_since) {
                exit_chord_since = now;
                fprintf(stderr, "[input] Start+Select: encerrando\n");
                atomic_store(&g_quit, 1);
            }
        } else {
            exit_chord_since = 0;
        }
        if (maximum_seconds && now - started >= maximum_seconds * 1000ULL) {
            fprintf(stderr, "[host] limite de teste de %u s atingido\n",
                    maximum_seconds);
            atomic_store(&g_quit, 1);
        }
        if (auto_a_count && now >= next_auto_a &&
            atomic_load(&g_unity_ready)) {
            fprintf(stderr, "[input-test] A (%u restante)\n", auto_a_count);
            int consumed = inject_android_key(96, 1);
            SDL_Delay(50);
            inject_android_key(96, 0);
            fprintf(stderr, "[input-test] A consumido=%d\n", consumed);
            --auto_a_count;
            next_auto_a = now + auto_a_interval;
        }
        if (tap_file && !tap_file_triggered && !auto_tap_stage &&
            access(tap_file, F_OK) == 0 &&
            atomic_load(&g_unity_ready)) {
            tap_file_triggered = 1;
            auto_tap_stage = 1;
            next_auto_tap = now;
        }
        if (tap2_file && tap2_x_text && tap2_y_text &&
            !tap2_file_triggered && !auto_tap_stage &&
            access(tap2_file, F_OK) == 0 &&
            atomic_load(&g_unity_ready)) {
            tap2_file_triggered = 1;
            auto_tap_x = tap2_x;
            auto_tap_y = tap2_y;
            auto_tap_stage = 1;
            next_auto_tap = now;
        }
        /*
         * Diagnostic equivalent of one real Android finger tap. DOWN is kept
         * alive across several rendered frames; sending DOWN+UP in one frame
         * makes uGUI observe only Ended and silently drops the click.
         */
        if (auto_tap_stage && now >= next_auto_tap &&
            atomic_load(&g_unity_ready)) {
            if (auto_tap_stage == 1) {
                fprintf(stderr,
                        "[input-test] toque DOWN (%.0f, %.0f) consumido=%d\n",
                        auto_tap_x, auto_tap_y,
                        inject_touch(0, auto_tap_x, auto_tap_y));
                auto_tap_stage = 2;
            } else if (auto_tap_stage <= 9) {
                inject_touch(2, auto_tap_x, auto_tap_y);
                ++auto_tap_stage;
            } else {
                inject_touch(1, auto_tap_x, auto_tap_y);
                fprintf(stderr, "[input-test] toque UP (%.0f, %.0f)\n",
                        auto_tap_x, auto_tap_y);
                auto_tap_stage = 0;
            }
            next_auto_tap = now + 40;
        }
        jni_pump_ui_tasks();
        dt_audio_pump();
        SDL_Delay(4);
    }

    /*
     * Android's UI Looper keeps dispatching while UnityMain performs
     * pause/done. Continue pumping instead of joining immediately: callbacks
     * needed by the engine must remain deliverable during shutdown.
     */
    uint64_t shutdown_started = monotonic_milliseconds();
    while (!atomic_load(&g_unity_exited) &&
           monotonic_milliseconds() - shutdown_started < 2500) {
        jni_pump_ui_tasks();
        dt_audio_pump();
        SDL_Delay(4);
    }
    if (atomic_load(&g_unity_exited))
        pthread_join(unity_thread, NULL);
    else
        fprintf(stderr, "[host] UnityMain nao concluiu shutdown em 2,5 s\n");
    dt_cursor_update(cursor_x, cursor_y, 0);
    dt_gamepad_publish(0, 0, 0, 0, 0, 0);
    for (int slot = 0; slot < DT_MAX_PADS; ++slot)
        if (g_pads[slot])
            SDL_GameControllerClose(g_pads[slot]);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    install_handlers();
    g_main_tid = (int)syscall(SYS_gettid);

    const char *root = getenv("DT_GAMEDIR");
    if (!root || !*root)
        root = getenv("NXCOMPAT_GAME_DIR");
    if ((!root || !*root) && argc > 1 && argv[1] && argv[1][0] == '/')
        root = argv[1];
    if (!root || !*root)
        root = "/storage/roms/ports/deadtrigger";
    dt_set_game_root(root);
    if (chdir(root) != 0) {
        fprintf(stderr, "[host] chdir %s: %s\n", root, strerror(errno));
        return 1;
    }
    char userdata[PATH_MAX];
    snprintf(userdata, sizeof userdata, "%s/userdata", root);
    if (make_directory(userdata) != 0)
        return 1;
    char shader_cache[PATH_MAX];
    snprintf(shader_cache, sizeof shader_cache,
             "%s/userdata/UnityShaderCache", root);
    if (make_directory(shader_cache) != 0)
        return 1;

    fprintf(stderr,
            "=== Dead Trigger 2.1.0 ARM64 / Unity 2019.4.40f1 ===\n"
            "[host] raiz=%s pid=%d tid=%d\n",
            root, getpid(), g_main_tid);
    dt_android_set_size(1280, 720);

    if (dt_framework_preflight(root) != 0)
        return 1;

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER |
                 SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[SDL] init: %s\n", SDL_GetError());
        return 1;
    }
    (void)dt_sdl_video_init();
    int video_width = 1280, video_height = 720;
    dt_sdl_video_size(&video_width, &video_height);
    dt_android_set_size(video_width, video_height);
    fprintf(stderr, "[deadtrigger/video] Android surface %dx%d\n",
            video_width, video_height);
    if (dt_framework_open_input() != 0)
        return 1;

    dt_imports_init();
    if (dt_loader_init() != 0)
        return 1;
    jni_init(DT_PACKAGE, root);
    dt_jni_install_android_contract();

    dt_module *main_module = dt_module_load("libmain.so");
    if (!main_module)
        return 1;
    jint (*main_on_load)(void *, void *) =
        (void *)dt_module_symbol(main_module, "JNI_OnLoad");
    if (!main_on_load) {
        fprintf(stderr, "[flow] libmain sem JNI_OnLoad\n");
        return 1;
    }
    jint main_jni = main_on_load(jni_get_vm(), NULL);
    fprintf(stderr, "[flow] libmain JNI_OnLoad -> 0x%x\n", main_jni);
    if (main_jni < 0x00010006)
        return 1;

    jboolean (*native_loader_load)(void *, jobject, jobject) =
        (void *)jni_find_native("load");
    if (!native_loader_load) {
        fprintf(stderr, "[flow] NativeLoader.load nao foi registrado\n");
        return 1;
    }
    char native_library_path[PATH_MAX];
    snprintf(native_library_path, sizeof(native_library_path),
             "%s/lib", root);
    char *library_directory = strdup(native_library_path);
    if (!library_directory)
        return 1;
    fprintf(stderr, "[flow] NativeLoader.load(%s)\n", library_directory);
    jboolean loaded = native_loader_load(jni_get_env(), jni_get_activity(),
                                         library_directory);
    free(library_directory);
    fprintf(stderr, "[flow] NativeLoader.load -> %u\n", loaded);
    if (!loaded)
        return 1;

    dt_module *unity_module = dt_module_find("libunity.so");
    if (!unity_module) {
        fprintf(stderr, "[flow] libunity nao apareceu apos NativeLoader.load\n");
        return 1;
    }
    dt_set_unity_module(unity_module);
    if (getenv("DT_JNI_DUMP"))
        jni_dump_natives();
    if (find_unity_api() != 0)
        return 1;

    /*
     * UnityPlayer constructor calls initJni on the Activity/UI thread before
     * SurfaceHolder callbacks and before the activity can resume rendering.
     */
    fprintf(stderr, "[flow] UnityPlayer.initJni(context)\n");
    g_unity.init_jni(jni_get_env(), jni_get_activity(),
                     jni_get_activity());
    fprintf(stderr, "[flow] initJni concluido; il2cpp=%p\n",
            (void *)dt_il2cpp_base());

    if (getenv("DT_BOOTSTRAP_ONLY")) {
        fprintf(stderr, "[host] DT_BOOTSTRAP_ONLY concluido\n");
        SDL_Quit();
        return 0;
    }

    pthread_t unity_thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 32 * 1024 * 1024);
    int thread_result = pthread_create(&unity_thread, &attributes,
                                       unity_main, NULL);
    pthread_attr_destroy(&attributes);
    if (thread_result != 0) {
        fprintf(stderr, "[host] pthread_create UnityMain: %s\n",
                strerror(thread_result));
        return 1;
    }

    int result = run_ui_loop(unity_thread);
    fprintf(stderr, "[host] encerrado apos %lu frames\n",
            atomic_load(&g_frames));
    /*
     * Unity/FMOD retain Android worker threads after Activity pause. Android
     * terminates that process as a unit; running libc finalizers here can
     * wait forever on foreign workers. The native focus-lost/nativePause
     * sequence has completed, so mirror Android process death explicitly.
     */
    fflush(NULL);
    _exit(result);
}
