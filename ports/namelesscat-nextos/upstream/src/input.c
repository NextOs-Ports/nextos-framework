/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Nameless Cat 1.2.6 — controle NextOS -> entrada Android/Unity.
 *
 * Arquitetura (V4, nxinput 0.10.1):
 *
 *   SDL2 DO FIRMWARE (mapping soberano do CFW/PortMaster, admitido in-process
 *   pela costura C6 com o bundle pinado da autoridade 3)
 *        |
 *        v
 *   controles simbólicos (A, B, ..., LEFT_STICK), normalizados UMA vez
 *        |
 *        +--> SELECT+START: chord soberano do framework (nxinput_exit_chord),
 *        |    consultado ANTES do GPTK e fora do arquivo do dono
 *        |
 *        v
 *   NEXTOSCONTROLLERS.gptk vivo (owner/default, lido no pré-init) decide
 *   action / null / native por controle e por contexto PROVADO pela engine
 *        |
 *        +--> ACTION  -> runtime vivo -> sink real deste adapter (KeyEvent /
 *        |               MotionEvent Android que o Input legado da Unity lê,
 *        |               UIManager do próprio jogo para o pause, cursor)
 *        +--> null    -> nada, em nenhum caminho
 *        +--> native  -> passthrough único: o MESMO KeyEvent/MotionEvent que
 *                        a Activity Android entregaria
 *
 * Contexto desconhecido (frame 0, contrato IL2CPP ausente, controle travado
 * por cutscene) = passthrough nativo: o jogo continua jogável com o seu
 * próprio suporte a gamepad; só o remap/null fica indisponível.
 *
 * O caminho nativo é dirigido por ESTADO (desejado x entregue), nunca por
 * borda: um KeyEvent DOWN entregue sempre recebe o seu UP, mesmo quando o
 * controle muda de dono no meio da pressão (a causa da direção presa da
 * 1.2.5).  Não existe joystick cru, ordinal posicional, troca A/B, nome de
 * aparelho, VID/PID, teclado sintético nem SDL privada.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "gb.h"
#include "nx_elf.h"
#include "nxinput_sdl_seam.h"
#include "nxc6_glue.h"
#include "nxinput_gptk.h"
#include "nxinput_gptk_motion.h"
#include "nxinput_exit_chord.h"
#include "input_gptk.h"
#include "nxinput_padset.h"
#include "viewport_policy.h"

/* ===== Constantes Android consumidas pela Unity (Input legado) ========= */
#define AKEY_DPAD_UP 19
#define AKEY_DPAD_DOWN 20
#define AKEY_DPAD_LEFT 21
#define AKEY_DPAD_RIGHT 22
#define AKEY_BUTTON_A 96      /* JoystickButton0  */
#define AKEY_BUTTON_B 97      /* JoystickButton1  */
#define AKEY_BUTTON_X 99      /* JoystickButton2  */
#define AKEY_BUTTON_Y 100     /* JoystickButton3  */
#define AKEY_BUTTON_L1 102    /* JoystickButton4  */
#define AKEY_BUTTON_R1 103    /* JoystickButton5  */
#define AKEY_BUTTON_L2 104
#define AKEY_BUTTON_R2 105
#define AKEY_BUTTON_THUMBL 106 /* JoystickButton8 */
#define AKEY_BUTTON_THUMBR 107 /* JoystickButton9 */
#define AKEY_BUTTON_START 108  /* JoystickButton10 — o "10" do prompt */
#define AKEY_BUTTON_SELECT 109 /* JoystickButton11 */

#define NC_STICK_DEADZONE 0.15f  /* radial, com reescala, para passthrough e
                                    player.move; o cursor tem a sua própria no
                                    [cursor] do arquivo (nxinput_gptk_motion) */
#define NC_TRIGGER_ENTER NXINPUT_GPTK_TRIGGER_ENTER
#define NC_TRIGGER_EXIT NXINPUT_GPTK_TRIGGER_EXIT

/* Vários pads admitidos ao mesmo tempo (até NC_MAX_PADS): o estado simbólico
 * é a união deles; o chord SELECT+START só vale no MESMO pad (instance) —
 * SELECT num pad e START noutro nunca encerram. `controller` segue apontando
 * para o primeiro pad admitido (compatibilidade). */
static nxinput_padset padset;      /* framework: união dos pads, chord por instance */
static nxinput_padset_sdl padset_sdl; /* preenchida com a SDL do firmware já resolvida */
static SDL_GameController *controller; /* primeiro pad admitido (compatibilidade) */
static uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX];
static volatile sig_atomic_t exit_requested;
static volatile sig_atomic_t signal_exit;
static int input_fatal;
static int input_diag;
static int screen_width = 1280;
static int screen_height = 720;
static void *input_last_env;
static void *input_last_player;
static unsigned long input_frame;

void st_input_request_exit(void)
{
    signal_exit = 1;
    exit_requested = 1;
}

/* ===== Admissão canônica do controle (nxinput C6) =======================
 * O pad é admitido neste processo, usando a SDL2 do firmware.  A ordem de
 * autoridades da C3 resolve o mapping soberano do PortMaster (inclusive a
 * projeção de ordinais joydev provada por capabilities e a descoberta por
 * capacidade do 0.10.1) antes de este consumidor chamar
 * SDL_IsGameController/SDL_GameControllerOpen.  Não existe pad ordinal,
 * joystick cru nem leitura paralela do nó de evento. */
static char nc_staged_mapping[NXINPUT_AUTHORITY_SOURCE_MAX];
static const char *(*nc_sdl_path_for_index)(int);
static SDL_JoystickID (*nc_sdl_instance_for_index)(int);
static Uint16 (*nc_sdl_joy_vendor)(SDL_Joystick *);
static Uint16 (*nc_sdl_joy_product)(SDL_Joystick *);
static int nc_seam_adopted;

static const char *nc_env_get(void *userdata, const char *name)
{
    (void)userdata;
    return getenv(name);
}

static int nc_env_unset(void *userdata, const char *name)
{
    (void)userdata;
    return unsetenv(name);
}

static int nc_sdl_was_init(void *userdata)
{
    (void)userdata;
    return SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0;
}

static int nc_stage_seam_before_init(void)
{
    nxinput_sdl_seam_env_ops env;
    size_t staged_len = 0;
    int rc;

    if (!getenv("NXC6_SEAM")) {
        fprintf(stderr, "[nc/input] NXC6 seam: not adopted for this run "
                        "(NXC6_SEAM absent); stock SDL behaviour\n");
        return 0;
    }
    nc_sdl_path_for_index = (const char *(*)(int))
        dlsym(RTLD_DEFAULT, "SDL_JoystickPathForIndex");
    nc_sdl_instance_for_index = (SDL_JoystickID (*)(int))
        dlsym(RTLD_DEFAULT, "SDL_JoystickGetDeviceInstanceID");
    if (!nc_sdl_path_for_index || !nc_sdl_instance_for_index) {
        fprintf(stderr, "[nc/input] NXC6 seam: this SDL cannot name the "
                        "device node (pre-2.24); staying native\n");
        return 0;
    }
    memset(&env, 0, sizeof env);
    env.api_version = NXINPUT_SDL_SEAM_API_VERSION;
    env.struct_size = sizeof env;
    env.getenv_fn = nc_env_get;
    env.unsetenv_fn = nc_env_unset;
    env.sdl_was_init_fn = nc_sdl_was_init;
    rc = nxinput_sdl_seam_stage_before_init(&env, nc_staged_mapping,
                                            sizeof nc_staged_mapping,
                                            &staged_len);
    if (rc != 0) {
        fprintf(stderr, "[nc/input] NXC6 seam: staging failed before the "
                        "joystick init (rc=%d); refusing to guess\n", rc);
        return -1;
    }
    if (staged_len > 0 &&
        setenv("NXC6_STAGED_MAPPING", nc_staged_mapping, 1) != 0) {
        fprintf(stderr, "[nc/input] NXC6 seam: could not hand the staged "
                        "mapping to the seam\n");
        return -1;
    }
    nc_seam_adopted = 1;
    /* Autoridade 3: o bundle pinado no ZIP, selecionado pelo FACE_LAYOUT do
     * preâmbulo V3 lido no pré-init (auto = controllers.nxb). */
    {
        int declared = nxc6_declare_port_bundle_for_layout(
            st_gamedir, nc_gptk_face_layout());
        fprintf(stderr, "[nc/input] NXC6 seam: port bundle %s (layout=%s)\n",
                declared > 0 ? getenv("NXCONTROLLER_PROFILES")
                : declared == 0 ? "(none shipped)"
                : "(declaration failed)",
                nxinput_gptk_face_layout_name(nc_gptk_face_layout()));
    }
    fprintf(stderr,
            "[nc/input] NXC6 seam: staged=%zu bytes env_still_set=%d "
            "receipt=%s\n",
            staged_len, getenv("SDL_GAMECONTROLLERCONFIG") != NULL,
            getenv("NXC6_RECEIPT") ? getenv("NXC6_RECEIPT") : "(none)");
    return 0;
}

/* ===== Cursor (menus touch-only) ========================================
 * Seta polida do adapter, movida SOMENTE pelo analógico direito quando o
 * arquivo entrega RIGHT_STICK a cursor.move no contexto vivo; R3 clica.  A
 * cinemática é a canônica do nxinput (deadzone radial, curva, aceleração,
 * suavização por tempo) com o tuning do [cursor] do próprio arquivo.  A seta
 * some depois de ociosa; um clique com a seta escondida apenas a revela — um
 * dedo invisível nunca mais toca a tela (causa do "A vai para QUIT"). */
static nxinput_gptk_cursor_state cursor_state;
static nxinput_gptk_cursor_tuning cursor_tuning;
static int cursor_tuning_ready;
static uint64_t cursor_tick;
static uint64_t cursor_seen_tick;
static float cursor_hide_after = 4.0f;
static int cursor_visible_context;
static int cursor_drag_active;
static float cursor_touch_x, cursor_touch_y;
static int cursor_click_held;
static int cursor_click_prev;
static int cursor_vector_this_frame;

/* ===== Estado da engine (contexto provado) ============================== */
static int platform_modal_active;
static int platform_modal_reported = -1;
static int platform_player_alive;
static int platform_gameplay_active;
static void *platform_player_class;
static void *platform_player_field;
static void *platform_allow_field;
static void *platform_control_flags_field;
static void *platform_player;
static void *platform_ui_class;
static void *platform_ui_instance_field;
static void *platform_ui_menu_field;
static void *platform_ui_button_mode_field;
static void *platform_ui_back_button_method;
static void *platform_ui_set_button_mode_method;
static void *platform_ui_hidden_instance;
static void *platform_ui_hide_failure_instance;
static int platform_hide_touch_buttons = 1;
static void *platform_check_ad_field;
static void *platform_check_ad_class;
static void *platform_check_ad_showing_method;
static void *platform_popup_class;
static void *platform_popup_type;
static void *platform_find_object_method;
static void *platform_object_alive_method;
static int platform_popup_active;
static unsigned long platform_popup_scan_frame;
static int platform_no_ads_enabled = 1;
static int platform_no_ads_applied;
static int platform_no_ads_reported_failure;
static unsigned long platform_no_ads_retry_frame;
static void *platform_ad_manager_class;
static void *platform_ad_get_skip_method;
static void *platform_ad_set_skip_method;

static void *viewport_level_class;
static void *viewport_level_instance_field;
static void *viewport_canvas_class;
static void *viewport_canvas_field;
static void *viewport_get_reference_method;
static void *viewport_get_match_method;
static void *viewport_set_match_method;
static void *viewport_applied_instance;
static int viewport_contract_state;
static int viewport_failed;
static unsigned viewport_contract_attempts;
static unsigned viewport_scene_attempts;

typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef void *(*il2cpp_class_get_method_from_name_fn)(void *, const char *, int);
typedef void *(*il2cpp_runtime_invoke_fn)(void *, void *, void **, void **);
typedef void *(*il2cpp_class_get_type_fn)(void *);
typedef void *(*il2cpp_type_get_object_fn)(void *);
typedef void *(*il2cpp_object_unbox_fn)(void *);
typedef void *(*il2cpp_class_get_field_from_name_fn)(void *, const char *);
typedef void (*il2cpp_field_static_get_value_fn)(void *, void *);
typedef void (*il2cpp_field_get_value_fn)(void *, void *, void *);

static il2cpp_domain_get_fn il2cpp_domain_get_p;
static il2cpp_domain_get_assemblies_fn il2cpp_domain_get_assemblies_p;
static il2cpp_assembly_get_image_fn il2cpp_assembly_get_image_p;
static il2cpp_class_from_name_fn il2cpp_class_from_name_p;
static il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name_p;
static il2cpp_runtime_invoke_fn il2cpp_runtime_invoke_p;
static il2cpp_class_get_type_fn il2cpp_class_get_type_p;
static il2cpp_type_get_object_fn il2cpp_type_get_object_p;
static il2cpp_object_unbox_fn il2cpp_object_unbox_p;
static il2cpp_class_get_field_from_name_fn il2cpp_class_get_field_from_name_p;
static il2cpp_field_static_get_value_fn il2cpp_field_static_get_value_p;
static il2cpp_field_get_value_fn il2cpp_field_get_value_p;

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

/* Exports do il2cpp resolvidos por NOME (nunca RVA).  Chamado somente depois
 * do frame 0: entrar em il2cpp_domain_get antes disso mata a Unity. */
static int resolve_il2cpp_api(void)
{
    static int tried;
    if (il2cpp_runtime_invoke_p)
        return 1;
    if (tried)
        return 0;
    tried = 1;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;
    il2cpp_domain_get_p = (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get");
    il2cpp_domain_get_assemblies_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    il2cpp_assembly_get_image_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    il2cpp_class_from_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    il2cpp_class_get_method_from_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    il2cpp_class_get_type_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_type");
    il2cpp_type_get_object_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_type_get_object");
    il2cpp_object_unbox_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_object_unbox");
    il2cpp_class_get_field_from_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_field_from_name");
    il2cpp_field_static_get_value_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_field_static_get_value");
    il2cpp_field_get_value_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_field_get_value");
    void *invoke = nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    if (!(il2cpp_domain_get_p && il2cpp_domain_get_assemblies_p &&
          il2cpp_assembly_get_image_p && il2cpp_class_from_name_p &&
          il2cpp_class_get_method_from_name_p && il2cpp_class_get_type_p &&
          il2cpp_type_get_object_p && il2cpp_object_unbox_p &&
          il2cpp_class_get_field_from_name_p &&
          il2cpp_field_static_get_value_p && il2cpp_field_get_value_p &&
          invoke))
        return 0;
    il2cpp_runtime_invoke_p = (il2cpp_runtime_invoke_fn)invoke;
    return 1;
}

static int platform_invoke_bool_getter(void *method, void *instance,
                                       int *value)
{
    if (!method || !il2cpp_runtime_invoke_p)
        return 0;
    void *exc = NULL;
    void *boxed = il2cpp_runtime_invoke_p(method, instance, NULL, &exc);
    uint8_t *unboxed = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
    if (!unboxed)
        return 0;
    *value = *unboxed != 0;
    return 1;
}

/* UnityEngine.Object.op_Implicit(obj): falso para objeto destruído (o static
 * _instance sobrevive à cena; sem esta prova o level-select pareceria
 * gameplay). */
static int platform_object_alive(void *obj)
{
    if (!obj)
        return 0;
    if (!platform_object_alive_method) {
        void *object_class = find_managed_class("UnityEngine", "Object");
        platform_object_alive_method = object_class
            ? il2cpp_class_get_method_from_name_p(object_class,
                                                  "op_Implicit", 1)
            : NULL;
        if (!platform_object_alive_method)
            return 1; /* sem a prova, confia no ponteiro (comportamento 1.2.5) */
    }
    void *args[1] = { obj };
    void *exc = NULL;
    void *boxed = il2cpp_runtime_invoke_p(platform_object_alive_method, NULL,
                                          args, &exc);
    uint8_t *alive = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
    return alive ? *alive != 0 : 1;
}

static int platform_resolve_player_contract(void)
{
    if (platform_player_class)
        return platform_player_field && platform_allow_field &&
               platform_control_flags_field;
    if (!resolve_il2cpp_api())
        return 0;
    platform_player_class =
        find_managed_class("", "PlayerPlatformerController");
    if (!platform_player_class)
        return 0;
    platform_player_field = il2cpp_class_get_field_from_name_p(
        platform_player_class, "_instance");
    platform_allow_field = il2cpp_class_get_field_from_name_p(
        platform_player_class, "allowControl");
    platform_control_flags_field = il2cpp_class_get_field_from_name_p(
        platform_player_class, "AllowControlFlag");
    int complete = platform_player_field && platform_allow_field &&
                   platform_control_flags_field;
    fprintf(stderr, "[nc/input] Nameless Cat player contract: %s\n",
            complete ? "ready" : "incomplete");
    return complete;
}

static int platform_resolve_ui_contract(void)
{
    if (platform_ui_class)
        return platform_ui_instance_field && platform_check_ad_field &&
               platform_ui_menu_field && platform_ui_button_mode_field &&
               platform_ui_set_button_mode_method &&
               platform_check_ad_class && platform_check_ad_showing_method;
    if (!resolve_il2cpp_api())
        return 0;
    platform_ui_class = find_managed_class("", "UIManager");
    platform_check_ad_class = find_managed_class("", "CheckAdPanel");
    platform_popup_class = find_managed_class("", "PopupMessage");
    void *unity_object_class = find_managed_class("UnityEngine", "Object");
    if (!platform_ui_class || !platform_check_ad_class ||
        !platform_popup_class || !unity_object_class)
        return 0;
    platform_ui_instance_field = il2cpp_class_get_field_from_name_p(
        platform_ui_class, "_instance");
    platform_ui_menu_field = il2cpp_class_get_field_from_name_p(
        platform_ui_class, "meun");
    platform_ui_button_mode_field = il2cpp_class_get_field_from_name_p(
        platform_ui_class, "buttonMode");
    platform_ui_back_button_method = il2cpp_class_get_method_from_name_p(
        platform_ui_class, "backButtonInGame", 0);
    platform_ui_set_button_mode_method =
        il2cpp_class_get_method_from_name_p(platform_ui_class,
                                            "setButtonMode", 1);
    platform_check_ad_field = il2cpp_class_get_field_from_name_p(
        platform_ui_class, "checkAdPanel");
    platform_check_ad_showing_method = il2cpp_class_get_method_from_name_p(
        platform_check_ad_class, "get_Showing", 0);
    platform_find_object_method = il2cpp_class_get_method_from_name_p(
        unity_object_class, "FindObjectOfType", 1);
    if (platform_popup_class) {
        void *popup_type = il2cpp_class_get_type_p(platform_popup_class);
        platform_popup_type = popup_type
                            ? il2cpp_type_get_object_p(popup_type) : NULL;
    }
    int complete = platform_ui_instance_field && platform_ui_menu_field &&
                   platform_ui_button_mode_field &&
                   platform_ui_set_button_mode_method &&
                   platform_check_ad_field &&
                   platform_check_ad_showing_method;
    fprintf(stderr,
            "[nc/input] Nameless Cat modal contract: %s; popups=%s; pause=%s\n",
            complete ? "ready" : "incomplete",
            platform_find_object_method && platform_popup_type
                ? "ready" : "unavailable",
            platform_ui_back_button_method ? "ready" : "unavailable");
    return complete;
}

static void platform_hide_native_touch_buttons(void *ui)
{
    if (!platform_hide_touch_buttons || !ui ||
        !platform_ui_button_mode_field ||
        !platform_ui_set_button_mode_method)
        return;
    int32_t current_mode = -1;
    il2cpp_field_get_value_p(ui, platform_ui_button_mode_field,
                             &current_mode);
    if (ui == platform_ui_hidden_instance && current_mode == 1)
        return;
    int32_t hide = 1; /* UIManager.ButtonMode.hide */
    void *args[1] = { &hide };
    void *exc = NULL;
    il2cpp_runtime_invoke_p(platform_ui_set_button_mode_method, ui,
                            args, &exc);
    if (exc) {
        if (platform_ui_hide_failure_instance != ui)
            fprintf(stderr,
                    "[nc/input] UIManager.setButtonMode(1) raised an "
                    "exception; will retry\n");
        platform_ui_hide_failure_instance = ui;
        return;
    }
    current_mode = -1;
    il2cpp_field_get_value_p(ui, platform_ui_button_mode_field,
                             &current_mode);
    if (current_mode != 1) {
        if (platform_ui_hide_failure_instance != ui)
            fprintf(stderr,
                    "[nc/input] UIManager.setButtonMode(1) readback failed; "
                    "will retry\n");
        platform_ui_hide_failure_instance = ui;
        return;
    }
    platform_ui_hidden_instance = ui;
    platform_ui_hide_failure_instance = NULL;
    fprintf(stderr,
            "[nc/input] touch buttons hidden via UIManager.setButtonMode(1)\n");
}

static void platform_update_modal_state(unsigned long frame)
{
    int check_ad_active = 0;
    uint8_t pause_menu_active = 0;
    if (platform_resolve_ui_contract()) {
        void *ui = NULL;
        void *check_ad = NULL;
        il2cpp_field_static_get_value_p(platform_ui_instance_field, &ui);
        if (ui) {
            platform_hide_native_touch_buttons(ui);
            il2cpp_field_get_value_p(ui, platform_ui_menu_field,
                                     &pause_menu_active);
            il2cpp_field_get_value_p(ui, platform_check_ad_field, &check_ad);
        }
        if (check_ad)
            platform_invoke_bool_getter(platform_check_ad_showing_method,
                                        check_ad, &check_ad_active);
        if (platform_find_object_method && platform_popup_type &&
            (!platform_popup_scan_frame ||
             frame - platform_popup_scan_frame >= 6)) {
            void *args[1] = { platform_popup_type };
            void *exc = NULL;
            void *popup = il2cpp_runtime_invoke_p(
                platform_find_object_method, NULL, args, &exc);
            platform_popup_active = !exc && popup;
            platform_popup_scan_frame = frame;
        } else if (!platform_find_object_method || !platform_popup_type) {
            platform_popup_active = 0;
        }
    } else {
        platform_popup_active = 0;
    }
    int active = pause_menu_active || check_ad_active || platform_popup_active;
    platform_modal_active = active;
    if (platform_modal_active != platform_modal_reported) {
        platform_modal_reported = platform_modal_active;
        fprintf(stderr, "[nc/input] UI modal: %s\n",
                platform_modal_active ? "active (menu)" : "inactive");
    }
}

static int platform_resolve_no_ads_contract(void)
{
    if (platform_ad_manager_class)
        return platform_ad_get_skip_method && platform_ad_set_skip_method;
    if (!resolve_il2cpp_api())
        return 0;
    platform_ad_manager_class = find_managed_class("", "AdManager");
    if (!platform_ad_manager_class)
        return 0;
    platform_ad_get_skip_method = il2cpp_class_get_method_from_name_p(
        platform_ad_manager_class, "get_IsSkipAds", 0);
    platform_ad_set_skip_method = il2cpp_class_get_method_from_name_p(
        platform_ad_manager_class, "set_IsSkipAds", 1);
    return platform_ad_get_skip_method && platform_ad_set_skip_method;
}

static void platform_update_no_ads(unsigned long frame)
{
    if (!platform_no_ads_enabled || platform_no_ads_applied || frame == 0 ||
        frame < platform_no_ads_retry_frame)
        return;
    platform_no_ads_retry_frame = frame + 300;
    if (!platform_resolve_no_ads_contract())
        return;
    int active = 0;
    if (!platform_invoke_bool_getter(platform_ad_get_skip_method, NULL,
                                     &active))
        return;
    if (!active) {
        uint8_t enabled = 1;
        void *args[1] = { &enabled };
        void *exc = NULL;
        il2cpp_runtime_invoke_p(platform_ad_set_skip_method, NULL, args, &exc);
        if (exc) {
            if (!platform_no_ads_reported_failure) {
                platform_no_ads_reported_failure = 1;
                fprintf(stderr,
                        "[nc/ads] native no-ads setter raised an exception; "
                        "will retry\n");
            }
            return;
        }
        if (!platform_invoke_bool_getter(platform_ad_get_skip_method, NULL,
                                         &active))
            return;
    }
    if (active) {
        platform_no_ads_applied = 1;
        fprintf(stderr,
                "[nc/ads] native no-ads state: active (persistent)\n");
    }
}

/* Contexto provado pela engine, a cada quadro:
 *   modal (pause/checkpoint/popup)      -> menu     ("ui:modal")
 *   player vivo + allowControl + flags 0 -> gameplay ("player:allow-control")
 *   player vivo com controle travado     -> menu     ("player:control-locked")
 *   sem player vivo (título/seleção)     -> menu     ("scene:touch-ui")
 *   contrato IL2CPP indisponível         -> desconhecido (passthrough) */
static void update_engine_context(unsigned long frame)
{
    if (frame == 0) {
        platform_gameplay_active = 0;
        platform_player_alive = 0;
        nc_gptk_clear_context("frame0");
        return;
    }
    platform_update_modal_state(frame);
    if (!platform_resolve_player_contract()) {
        platform_gameplay_active = 0;
        platform_player_alive = 0;
        nc_gptk_clear_context("player-contract-unavailable");
        return;
    }
    void *instance = NULL;
    il2cpp_field_static_get_value_p(platform_player_field, &instance);
    int alive = platform_object_alive(instance);
    if (instance != platform_player || alive != platform_player_alive) {
        platform_player = instance;
        platform_player_alive = alive;
        fprintf(stderr, "[nc/input] player context: %s\n",
                alive ? "present" : "menu");
    }
    uint8_t allow_control = 0;
    int32_t control_flags = 1;
    if (alive) {
        il2cpp_field_get_value_p(platform_player, platform_allow_field,
                                 &allow_control);
        il2cpp_field_get_value_p(platform_player,
                                 platform_control_flags_field,
                                 &control_flags);
    }
    int gameplay = alive && allow_control && control_flags == 0 &&
                   !platform_modal_active;
    if (gameplay != platform_gameplay_active) {
        platform_gameplay_active = gameplay;
        fprintf(stderr, "[nc/input] gameplay controls: %s\n",
                gameplay ? "active" : "not active");
    }
    if (platform_modal_active)
        nc_gptk_set_context(NC_GPTK_CONTEXT_MENU, "ui:modal");
    else if (gameplay)
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
    else if (alive)
        /* Player vivo com o controle travado pela engine = cutscene/diálogo
         * touch-only (SKIP no canto, balões por toque): estado PROVADO pelo
         * campo allowControl, não presumido — o cursor e o accept ficam
         * disponíveis; o pulo não. */
        nc_gptk_set_context(NC_GPTK_CONTEXT_MENU, "player:control-locked");
    else
        nc_gptk_set_context(NC_GPTK_CONTEXT_MENU, "scene:touch-ui");
}

/* ===== Viewport da seleção de fases (inalterado desde a 1.2.3) ========== */
static int viewport_resolve_contract(void)
{
    if (viewport_contract_state > 0)
        return 1;
    if (!resolve_il2cpp_api())
        return 0;
    viewport_level_class = find_managed_class("", "LevelManager");
    viewport_canvas_class =
        find_managed_class("UnityEngine.UI", "CanvasScaler");
    if (viewport_level_class) {
        viewport_level_instance_field = il2cpp_class_get_field_from_name_p(
            viewport_level_class, "_instance");
        viewport_canvas_field = il2cpp_class_get_field_from_name_p(
            viewport_level_class, "canvasScaler");
    }
    if (viewport_canvas_class) {
        viewport_get_reference_method = il2cpp_class_get_method_from_name_p(
            viewport_canvas_class, "get_referenceResolution", 0);
        viewport_get_match_method = il2cpp_class_get_method_from_name_p(
            viewport_canvas_class, "get_matchWidthOrHeight", 0);
        viewport_set_match_method = il2cpp_class_get_method_from_name_p(
            viewport_canvas_class, "set_matchWidthOrHeight", 1);
    }
    viewport_contract_state =
        viewport_level_class && viewport_level_instance_field &&
        viewport_canvas_class &&
        viewport_canvas_field && viewport_get_reference_method &&
        viewport_get_match_method && viewport_set_match_method ? 1 : 0;
    return viewport_contract_state == 1;
}

static void viewport_fail(const char *reason)
{
    if (!viewport_failed)
        fprintf(stderr,
                "[nc/viewport] ADAPTIVE-VIEWPORT/1 FAIL reason=%s "
                "drawable=%dx%d\n",
                reason, screen_width, screen_height);
    viewport_failed = 1;
    exit_requested = 1;
}

static int viewport_retry_or_fail(unsigned *attempts, const char *reason)
{
    (*attempts)++;
    if (*attempts >= 4) {
        viewport_fail(reason);
        return 0;
    }
    fprintf(stderr,
            "[nc/viewport] waiting for %s (%u/4)\n", reason, *attempts);
    return 1;
}

static void viewport_adapt_level_select(unsigned long frame)
{
    if (viewport_failed || !nc_viewport_should_probe(frame, 0))
        return;
    if (!viewport_resolve_contract()) {
        viewport_retry_or_fail(&viewport_contract_attempts,
                               "contract-unavailable");
        return;
    }
    viewport_contract_attempts = 0;
    void *instance = NULL;
    il2cpp_field_static_get_value_p(viewport_level_instance_field, &instance);
    if (!instance) {
        viewport_applied_instance = NULL;
        viewport_scene_attempts = 0;
        return;
    }
    if (instance == viewport_applied_instance)
        return;
    void *canvas = NULL;
    il2cpp_field_get_value_p(instance, viewport_canvas_field, &canvas);
    if (!canvas) {
        viewport_retry_or_fail(&viewport_scene_attempts, "canvas-missing");
        return;
    }
    void *exc = NULL;
    void *boxed_reference = il2cpp_runtime_invoke_p(
        viewport_get_reference_method, canvas, NULL, &exc);
    if (exc || !boxed_reference) {
        viewport_retry_or_fail(&viewport_scene_attempts, "reference-read");
        return;
    }
    float *reference = il2cpp_object_unbox_p(boxed_reference);
    if (!reference) {
        viewport_retry_or_fail(&viewport_scene_attempts, "reference-unbox");
        return;
    }
    exc = NULL;
    void *boxed_match = il2cpp_runtime_invoke_p(
        viewport_get_match_method, canvas, NULL, &exc);
    float *old_match_p = boxed_match ? il2cpp_object_unbox_p(boxed_match) : NULL;
    if (exc || !old_match_p) {
        viewport_retry_or_fail(&viewport_scene_attempts, "match-read");
        return;
    }
    float match = 0.0f;
    if (nc_viewport_containment_match(screen_width, screen_height,
                                      reference[0], reference[1],
                                      &match) != 0) {
        viewport_fail("invalid-dimensions");
        return;
    }
    float old_match = *old_match_p;
    void *args[1] = { &match };
    exc = NULL;
    il2cpp_runtime_invoke_p(viewport_set_match_method, canvas, args, &exc);
    if (exc) {
        viewport_retry_or_fail(&viewport_scene_attempts, "match-write");
        return;
    }
    viewport_scene_attempts = 0;
    viewport_applied_instance = instance;
    fprintf(stderr,
            "[nc/viewport] ADAPTIVE-VIEWPORT/1 OK drawable=%dx%d "
            "reference=%.3fx%.3f match=%.6f->%.1f instance=%p\n",
            screen_width, screen_height, reference[0], reference[1],
            old_match, match, instance);
}

/* ===== Injeção Android -> Unity ========================================= */
static void inject(void *env, void *player, void *event)
{
    static void *native_inject;
    if (!native_inject)
        native_inject = st_jni_native("com/unity3d/player/UnityPlayer",
                                       "nativeInjectEvent");
    if (native_inject && event) {
        /* Unity 2022+ registra nativeInjectEvent(InputEvent, displayId). */
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (input_diag)
            fprintf(stderr, "[nc/input] inject event=%p consumed=%d\n",
                    event, consumed);
    } else if (input_diag) {
        fprintf(stderr, "[nc/input] inject SKIPPED inject=%p event=%p\n",
                native_inject, event);
    }
}

/* Estado entregue de teclas Android (por keycode): a MESMA tabela serve o
 * passthrough nativo e os sinks, de modo que um DOWN sempre recebe o seu UP e
 * dois donos nunca produzem dois DOWNs para o mesmo keycode. */
static uint8_t key_down_state[256];

static void deliver_key(int keycode, int down)
{
    if (keycode <= 0 || keycode >= 256)
        return;
    if ((key_down_state[keycode] != 0) == (down != 0))
        return;
    key_down_state[keycode] = down ? 1 : 0;
    if (input_diag)
        fprintf(stderr, "[nc/key] keycode=%d %s\n", keycode,
                down ? "down" : "up");
    inject(input_last_env, input_last_player,
           st_jni_key_event(down ? 0 : 1, keycode, keycode));
}

static int sink_key_pressed[256];
static void release_all_keys(void)
{
    for (int k = 1; k < 256; k++) {
        sink_key_pressed[k] = 0;
        if (key_down_state[k])
            deliver_key(k, 0);
    }
}

/* ===== Sinks reais do adapter (ACK = 0) =================================
 * Símbolos exportados de propósito: o nxrelease liga cada sink-id do
 * adapter-contract a um símbolo definido neste ELF. */
/* Quantos sinks seguram cada keycode agora (declarado acima): o passthrough
 * nativo nunca solta um DOWN que pertence a um sink (A=96 por
 * player.jump/menu.accept). */

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

int nc_sink_engine_input_jump(void *user, const char *action, int pressed,
                              float value)
{
    (void)user; (void)action; (void)value;
    sink_key(AKEY_BUTTON_A, pressed);
    return 0;
}

int nc_sink_engine_ui_accept(void *user, const char *action, int pressed,
                             float value)
{
    (void)user; (void)action; (void)value;
    sink_key(AKEY_BUTTON_A, pressed);
    return 0;
}

int nc_sink_engine_input_interact(void *user, const char *action,
                                  int pressed, float value)
{
    (void)user; (void)action; (void)value;
    sink_key(AKEY_BUTTON_B, pressed);
    return 0;
}

/* player.down: o jogo lê "axis +2" (AXIS_Y positivo) para agachar/descer;
 * o sink marca o estado e o MotionEvent do quadro carrega o eixo. */
static int down_action_held;
int nc_sink_engine_input_down(void *user, const char *action, int pressed,
                              float value)
{
    (void)user; (void)action; (void)value;
    down_action_held = pressed ? 1 : 0;
    return 0;
}

/* player.move: vetor do stick esquerdo -> AXIS_X/AXIS_Y do MotionEvent. */
static float move_axis_x, move_axis_y;
static int move_vector_this_frame;
int nc_sink_engine_input_move(void *user, const char *action, float x,
                              float y)
{
    (void)user; (void)action;
    move_axis_x = x;
    move_axis_y = y;
    move_vector_this_frame = 1;
    return 0;
}

/* system.pause: no jogo (UIManager vivo) usa o MESMO fluxo do botão de pause
 * do próprio Nameless Cat, backButtonInGame(), que abre com todos os gates e
 * fecha restaurando timeScale; fora dele (título/intros) START segue como o
 * KeyEvent KEYCODE_BUTTON_START = JoystickButton10 — o "10" que o jogo pede.
 * Nunca os dois para a mesma pressão. */
static int pause_key_sent;
static int nc_pause_toggle_native(void)
{
    if (!platform_resolve_ui_contract() || !platform_ui_back_button_method)
        return 0;
    void *ui = NULL;
    il2cpp_field_static_get_value_p(platform_ui_instance_field, &ui);
    if (!ui || !platform_player_alive)
        return 0;
    uint8_t was_open = 0, now_open = 0;
    il2cpp_field_get_value_p(ui, platform_ui_menu_field, &was_open);
    void *exc = NULL;
    il2cpp_runtime_invoke_p(platform_ui_back_button_method, ui, NULL, &exc);
    il2cpp_field_get_value_p(ui, platform_ui_menu_field, &now_open);
    if (exc)
        fprintf(stderr,
                "[nc/pause] backButtonInGame() late exception; state %d -> %d\n",
                was_open != 0, now_open != 0);
    if (was_open != now_open)
        fprintf(stderr, "[nc/pause] native menu %d -> %d\n",
                was_open != 0, now_open != 0);
    return was_open != now_open;
}

int nc_sink_engine_input_pause(void *user, const char *action, int pressed,
                               float value)
{
    (void)user; (void)action; (void)value;
    if (pressed) {
        if (nc_pause_toggle_native()) {
            pause_key_sent = 0;
            return 0;
        }
        pause_key_sent = 1;
        sink_key(AKEY_BUTTON_START, 1);
    } else if (pause_key_sent) {
        pause_key_sent = 0;
        sink_key(AKEY_BUTTON_START, 0);
    }
    return 0;
}

int nc_sink_adapter_system_quit(void *user, const char *action, int pressed,
                                float value)
{
    (void)user; (void)action; (void)value;
    if (pressed) {
        fprintf(stderr, "[nc/input] system.quit: shutdown normal\n");
        exit_requested = 1;
    }
    return 0;
}

int nc_sink_adapter_pointer_click(void *user, const char *action,
                                  int pressed, float value)
{
    (void)user; (void)action; (void)value;
    cursor_click_held = pressed ? 1 : 0;
    return 0;
}

int nc_sink_adapter_pointer_move(void *user, const char *action, float x,
                                 float y)
{
    (void)user; (void)action;
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = cursor_tick && frequency
             ? (float)((double)(now - cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;
    if (!cursor_tuning_ready)
        return 0;
    float before_x = cursor_state.x, before_y = cursor_state.y;
    int rc = nxinput_gptk_cursor_step(&cursor_tuning, x, y, dt, screen_width,
                                      screen_height, &cursor_state);
    if (input_diag && (fabsf(x) > 0.01f || fabsf(y) > 0.01f) &&
        input_frame % 30 == 0)
        fprintf(stderr, "[nc/cursor] frame=%lu axis=%.2f,%.2f dt=%.4f rc=%d "
                        "pos=%.1f,%.1f drawable=%dx%d speed=%.2f\n",
                input_frame, x, y, dt, rc, cursor_state.x, cursor_state.y,
                screen_width, screen_height, cursor_tuning.speed);
    if (fabsf(cursor_state.x - before_x) > 0.01f ||
        fabsf(cursor_state.y - before_y) > 0.01f)
        cursor_seen_tick = now;
    cursor_vector_this_frame = 1;
    return 0;
}

/* ===== Registro dos sinks + selo ======================================== */
static int nc_register_sinks(void)
{
    struct { const char *action, *sink; nc_gptk_button_sink_fn fn; } b[] = {
        { "player.jump",     "engine.input.jump",     nc_sink_engine_input_jump },
        { "player.interact", "engine.input.interact", nc_sink_engine_input_interact },
        { "player.down",     "engine.input.down",     nc_sink_engine_input_down },
        { "menu.accept",     "engine.ui_accept",      nc_sink_engine_ui_accept },
        { "system.pause",    "engine.input.pause",    nc_sink_engine_input_pause },
        { "system.quit",     "adapter.system.quit",   nc_sink_adapter_system_quit },
        { "cursor.click",    "adapter.pointer.click", nc_sink_adapter_pointer_click },
    };
    struct { const char *action, *sink; nc_gptk_vector_sink_fn fn; } v[] = {
        { "player.move",  "engine.input.move",    nc_sink_engine_input_move },
        { "cursor.move",  "adapter.pointer.move", nc_sink_adapter_pointer_move },
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (nc_gptk_register_button(b[i].action, b[i].sink, b[i].fn, NULL) != 0)
            return -1;
    for (size_t i = 0; i < sizeof v / sizeof *v; i++)
        if (nc_gptk_register_vector(v[i].action, v[i].sink, v[i].fn, NULL) != 0)
            return -1;
    return nc_gptk_seal();
}

/* ===== Ponte SDL_GameController -> vocabulário simbólico ================ */
static int nc_control_of(int sdl_button)
{
    switch (sdl_button) {
    case SDL_CONTROLLER_BUTTON_A:             return NXINPUT_GPTK_A;
    case SDL_CONTROLLER_BUTTON_B:             return NXINPUT_GPTK_B;
    case SDL_CONTROLLER_BUTTON_X:             return NXINPUT_GPTK_X;
    case SDL_CONTROLLER_BUTTON_Y:             return NXINPUT_GPTK_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return NXINPUT_GPTK_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return NXINPUT_GPTK_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return NXINPUT_GPTK_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return NXINPUT_GPTK_R3;
    case SDL_CONTROLLER_BUTTON_START:         return NXINPUT_GPTK_START;
    case SDL_CONTROLLER_BUTTON_BACK:          return NXINPUT_GPTK_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return NXINPUT_GPTK_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return NXINPUT_GPTK_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return NXINPUT_GPTK_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return NXINPUT_GPTK_RIGHT;
    default:                                  return -1; /* GUIDE: fora */
    }
}

/* Keycode Android do passthrough nativo de cada controle simbólico. */
static int nc_native_keycode(int control)
{
    switch (control) {
    case NXINPUT_GPTK_A:      return AKEY_BUTTON_A;
    case NXINPUT_GPTK_B:      return AKEY_BUTTON_B;
    case NXINPUT_GPTK_X:      return AKEY_BUTTON_X;
    case NXINPUT_GPTK_Y:      return AKEY_BUTTON_Y;
    case NXINPUT_GPTK_L1:     return AKEY_BUTTON_L1;
    case NXINPUT_GPTK_R1:     return AKEY_BUTTON_R1;
    case NXINPUT_GPTK_L2:     return AKEY_BUTTON_L2;
    case NXINPUT_GPTK_R2:     return AKEY_BUTTON_R2;
    case NXINPUT_GPTK_L3:     return AKEY_BUTTON_THUMBL;
    case NXINPUT_GPTK_R3:     return AKEY_BUTTON_THUMBR;
    case NXINPUT_GPTK_START:  return AKEY_BUTTON_START;
    case NXINPUT_GPTK_SELECT: return AKEY_BUTTON_SELECT;
    case NXINPUT_GPTK_UP:     return AKEY_DPAD_UP;
    case NXINPUT_GPTK_DOWN:   return AKEY_DPAD_DOWN;
    case NXINPUT_GPTK_LEFT:   return AKEY_DPAD_LEFT;
    case NXINPUT_GPTK_RIGHT:  return AKEY_DPAD_RIGHT;
    default:                  return 0;
    }
}

/* Estado físico normalizado do quadro (um valor por controle). */
static int control_down[NXINPUT_GPTK_CONTROL_COUNT];
static float trigger_value[2];
static int trigger_digital[2];

static float axis_value(SDL_GameControllerAxis axis)
{
    Sint16 value = nxinput_padset_axis(&padset, (int)axis); /* maior deflexão entre os pads */
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
        axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return value > 0 ? value / 32767.0f : 0.0f;
    return value < 0 ? value / 32768.0f : value / 32767.0f;
}

/* Deadzone radial com reescala: o repouso é ZERO exato (nunca ±0.05 todo
 * quadro) e a deflexão total continua alcançando 1. */
static void radial_deadzone(float *x, float *y)
{
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= NC_STICK_DEADZONE) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    float scale = (m - NC_STICK_DEADZONE) / (1.0f - NC_STICK_DEADZONE);
    if (scale > 1.0f)
        scale = 1.0f;
    *x = *x / m * scale;
    *y = *y / m * scale;
}

static float clampf(float v)
{
    return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

#ifdef NC_BENCH
/* ===== Pad virtual de bancada (SÓ na variante de bancada -DNC_BENCH; a
 * release pública não compila este caminho e não conhece a env) ====================
 * Um token por linha em NC_VPAD_FILE (default /tmp/namelesscat-vpad) vira um
 * pulso de N quadros no estado SIMBÓLICO — depois do mapping soberano da SDL,
 * antes do GPTK.  Estímulo determinístico para a prova de remap/null; nunca
 * prova o mapping físico sozinho (o recibo C6 do pad real acompanha). */
static int vpad_enabled;
static const char *vpad_file = "/tmp/namelesscat-vpad";
static unsigned vpad_frames[NXINPUT_GPTK_CONTROL_COUNT];
static unsigned vpad_axis_frames[4];
static float vpad_axis_values[4];

static void vpad_poll(void)
{
    if (!vpad_enabled)
        return;
    for (int i = 0; i < NXINPUT_GPTK_CONTROL_COUNT; i++)
        if (vpad_frames[i] > 0) vpad_frames[i]--;
    for (int i = 0; i < 4; i++)
        if (vpad_axis_frames[i] > 0) vpad_axis_frames[i]--;
    FILE *in = fopen(vpad_file, "r");
    if (!in)
        return;
    char token[32] = { 0 };
    int have = fscanf(in, "%31s", token) == 1 && token[0];
    fclose(in);
    unlink(vpad_file);
    if (!have)
        return;
    unsigned duration = 6;
    if (!strncasecmp(token, "cursor:", 7)) {
        /* Posição absoluta da seta em pixels do drawable (bancada): torna o
         * clique de teste independente da taxa de quadros. */
        float cx = 0, cy = 0;
        if (sscanf(token + 7, "%f,%f", &cx, &cy) == 2) {
            nxinput_gptk_cursor_state_reset(&cursor_state, cx, cy);
            cursor_seen_tick = SDL_GetPerformanceCounter();
            fprintf(stderr, "[nc/vpad] cursor placed at %.0f,%.0f\n", cx, cy);
        } else {
            fprintf(stderr, "[nc/vpad] cursor token malformed\n");
        }
        return;
    }
    char *sep = strrchr(token, ':');
    if (sep && sep[1]) {
        long parsed = strtol(sep + 1, NULL, 10);
        if (parsed > 0 && parsed <= 600)
            duration = (unsigned)parsed;
        *sep = '\0';
    }
    int matched = 1;
    static const struct { const char *name; int control; } names[] = {
        { "a", NXINPUT_GPTK_A }, { "b", NXINPUT_GPTK_B },
        { "x", NXINPUT_GPTK_X }, { "y", NXINPUT_GPTK_Y },
        { "l1", NXINPUT_GPTK_L1 }, { "r1", NXINPUT_GPTK_R1 },
        { "l2", NXINPUT_GPTK_L2 }, { "r2", NXINPUT_GPTK_R2 },
        { "l3", NXINPUT_GPTK_L3 }, { "r3", NXINPUT_GPTK_R3 },
        { "start", NXINPUT_GPTK_START }, { "select", NXINPUT_GPTK_SELECT },
        { "up", NXINPUT_GPTK_UP }, { "down", NXINPUT_GPTK_DOWN },
        { "left", NXINPUT_GPTK_LEFT }, { "right", NXINPUT_GPTK_RIGHT },
    };
    int found = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (!strcasecmp(token, names[i].name)) {
            vpad_frames[names[i].control] = duration;
            found = 1;
            break;
        }
    }
    if (!found) {
        static const struct { const char *name; int axis; float v; } axes[] = {
            { "lx+", 0, 1.0f }, { "lx-", 0, -1.0f },
            { "ly+", 1, 1.0f }, { "ly-", 1, -1.0f },
            { "rx+", 2, 1.0f }, { "rx-", 2, -1.0f },
            { "ry+", 3, 1.0f }, { "ry-", 3, -1.0f },
        };
        for (size_t i = 0; i < sizeof axes / sizeof *axes; i++) {
            if (!strcasecmp(token, axes[i].name)) {
                vpad_axis_frames[axes[i].axis] = duration;
                vpad_axis_values[axes[i].axis] = axes[i].v;
                found = 1;
                break;
            }
        }
    }
    if (!found && !strcasecmp(token, "exit")) {
        vpad_frames[NXINPUT_GPTK_SELECT] = duration;
        vpad_frames[NXINPUT_GPTK_START] = duration;
        found = 1;
    }
    if (!found && !strcasecmp(token, "shot")) {
        /* Captura do próximo quadro apresentado (glReadPixels de dentro: a
         * única testemunha honesta de imagem neste aparelho). */
        extern int st_shot_request;
        st_shot_request = 1;
        found = 1;
    }
    if (!found)
        matched = 0;
    fprintf(stderr, "[nc/vpad] pulse %s x%u (%s)\n", token, duration,
            matched ? "accepted" : "unknown");
}

#else /* release pública: nenhum caminho de injeção compilado */
enum { vpad_enabled = 0 };
static const unsigned vpad_frames[NXINPUT_GPTK_CONTROL_COUNT] = { 0 };
static const unsigned vpad_axis_frames[4] = { 0 };
static const float vpad_axis_values[4] = { 0 };
static void vpad_poll(void) { }
#endif /* NC_BENCH */

/* ===== Controle: abertura e identidade =================================== */
/* Admissão pela autoridade do port (costura C6): o padset nunca decide. */
static int padset_admit(int i, void *user)
{
    (void)user;
    if (nc_seam_adopted) {
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_text[64];
        const char *devpath = nc_sdl_path_for_index
                            ? nc_sdl_path_for_index(i) : NULL;
        SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
        if (!nxc6_admit_before_announce(
                (int)nc_sdl_instance_for_index(i), guid_text,
                devpath ? devpath : "")) {
            fprintf(stderr, "[nc/input] NXC6 seam: device %d (%s) "
                            "refused by the authority order\n",
                    i, guid_text);
            return 0;
        }
    }
    return 1;
}

/* Um pad recém-aberto: identidade JNI só para o primeiro; log para todos. */
static void padset_opened(int i, unsigned slot, void *opened_ptr, void *user)
{
    (void)user;
    SDL_GameController *opened = opened_ptr;
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(opened);
    if (slot == 0)
        controller = opened;
    const char *physical = SDL_GameControllerName(opened);
    int vendor = (joy && nc_sdl_joy_vendor) ? nc_sdl_joy_vendor(joy) : 0;
    int product = (joy && nc_sdl_joy_product) ? nc_sdl_joy_product(joy) : 0;
    char *mapping = SDL_GameControllerMapping(opened);
    /* Identidade apresentada à Unity: o nome que o Input legado consulta
     * em Input.GetJoystickNames().  O jogo não escolhe glyphs por nome
     * (medido: os prompts do jogo saem crus tanto com o nome espúrio anterior quanto com o nome real); o nome é
     * informativo e o mapping soberano da SDL é a autoridade física. */
    if (slot == 0)
        st_jni_input_device_info(physical ? physical : "NextOS Gamepad",
                                 vendor, product,
                                 physical ? physical : "gamepad");
    fprintf(stderr, "[nc/input] controller: %s (%04x:%04x) mapping=%s\n",
            physical ? physical : "unknown", vendor & 0xffff,
            product & 0xffff, mapping ? mapping : "unavailable");
    fprintf(stderr, "[nc/input] pad slot=%d instance=%d sdl_index=%d\n",
            slot, (int)padset.instances[slot], i);
    SDL_free(mapping);
}

static void padset_log(const char *line, void *user)
{
    (void)user;
    fprintf(stderr, "[nc/input] %s\n", line);
}


/* ===== nxinput_padset: vtable sobre a SDL do firmware (nunca privada) ===== */
static int ps_num_joysticks(void) { return SDL_NumJoysticks(); }
static int32_t ps_instance_for_index(int i) { return (int32_t)nc_sdl_instance_for_index(i); }
static int ps_is_game_controller(int i) { return SDL_IsGameController(i) ? 1 : 0; }
static void *ps_open(int i) { return SDL_GameControllerOpen(i); }
static void ps_close(void *c) { SDL_GameControllerClose(c); }
static void *ps_get_joystick(void *c) { return SDL_GameControllerGetJoystick(c); }
static int32_t ps_joystick_instance(void *j) { return (int32_t)SDL_JoystickInstanceID(j); }
static void ps_update(void) { SDL_GameControllerUpdate(); }
static uint8_t ps_get_button(void *c, int b) { return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b); }
static int16_t ps_get_axis(void *c, int a) { return SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a); }

static int padset_setup(void)
{
    padset_sdl.num_joysticks = ps_num_joysticks;
    padset_sdl.instance_for_index = ps_instance_for_index;
    padset_sdl.is_game_controller = ps_is_game_controller;
    padset_sdl.open = ps_open;
    padset_sdl.close = ps_close;
    padset_sdl.get_joystick = ps_get_joystick;
    padset_sdl.joystick_instance = ps_joystick_instance;
    padset_sdl.update = ps_update;
    padset_sdl.get_button = ps_get_button;
    padset_sdl.get_axis = ps_get_axis;
    if (nxinput_padset_init(&padset, &padset_sdl, padset_log, NULL) != 0) {
        fprintf(stderr, "[nc/input] nxinput_padset: vtable incompleta (fail-closed)\n");
        return -1;
    }
    fprintf(stderr, "[nc/input] pads: %s (união dos admitidos, chord por instance)\n",
            nxinput_padset_marker());
    return 0;
}

static void open_controller(void)
{
    nxinput_padset_open_all(&padset, padset_admit, padset_opened, NULL);
    controller = nxinput_padset_first(&padset);
    if (padset.count == 0 && SDL_NumJoysticks() > 0)
        fprintf(stderr, "[nc/input] %d joystick(s) visible but none admitted "
                        "as GameController; no fallback by design\n",
                SDL_NumJoysticks());
}

static void close_controller(void)
{
    nxinput_padset_close_all(&padset);
    controller = NULL;
    memset(buttons, 0, sizeof buttons);
}

/* ===== Toque sintético do cursor ======================================== */
static void cancel_synthetic_touch(const char *reason)
{
    if (cursor_drag_active && input_last_env && input_last_player) {
        inject(input_last_env, input_last_player,
               st_jni_touch_event(3, cursor_touch_x, cursor_touch_y));
        fprintf(stderr, "[nc/touch] CANCEL reason=%s\n",
                reason ? reason : "lifecycle");
    }
    cursor_drag_active = 0;
    cursor_click_held = 0;
    cursor_click_prev = 0;
}

static int cursor_idle_hidden(void)
{
    if (cursor_hide_after <= 0.0f)
        return 0;
    uint64_t freq = SDL_GetPerformanceFrequency();
    if (!cursor_seen_tick || !freq)
        return 1;
    double idle = (double)(SDL_GetPerformanceCounter() - cursor_seen_tick)
                / (double)freq;
    return idle > (double)cursor_hide_after;
}

static void update_cursor(void)
{
    /* O cursor só existe onde o arquivo entrega o RIGHT_STICK a cursor.move
     * no contexto vivo (menu por default); em gameplay não há seta. */
    const char *stick_action = NULL;
    int stick_is_cursor =
        nc_gptk_decision(NXINPUT_GPTK_RIGHT_STICK, &stick_action) ==
            NC_GPTK_DECIDE_ACTION &&
        stick_action && strncmp(stick_action, "cursor.", 7) == 0;
    cursor_visible_context = stick_is_cursor;
    if (!stick_is_cursor) {
        if (cursor_drag_active)
            cancel_synthetic_touch("cursor-context-left");
        cursor_click_prev = cursor_click_held;
        return;
    }
    static int reveal_only;
    int held = cursor_click_held;
    int down = held && !cursor_click_prev;
    int up = !held && cursor_click_prev;
    cursor_click_prev = held;
    if (down && cursor_idle_hidden() && !cursor_drag_active) {
        /* Seta escondida: a pressão inteira só a revela; nada toca a tela. */
        cursor_seen_tick = SDL_GetPerformanceCounter();
        reveal_only = 1;
        return;
    }
    if (reveal_only) {
        if (up)
            reveal_only = 0;
        return;
    }
    float touch_x = cursor_state.x;
    float touch_y = cursor_state.y;
    if (down) {
        cursor_seen_tick = SDL_GetPerformanceCounter();
        inject(input_last_env, input_last_player,
               st_jni_touch_event(0, touch_x, touch_y));
        cursor_drag_active = 1;
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
        if (input_diag)
            fprintf(stderr, "[nc/touch] DOWN %.0f,%.0f\n", touch_x, touch_y);
    } else if (held && cursor_drag_active &&
               (fabsf(touch_x - cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - cursor_touch_y) >= 0.25f)) {
        inject(input_last_env, input_last_player,
               st_jni_touch_event(2, touch_x, touch_y));
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
    }
    if (up && cursor_drag_active) {
        inject(input_last_env, input_last_player,
               st_jni_touch_event(1, touch_x, touch_y));
        cursor_drag_active = 0;
        if (input_diag)
            fprintf(stderr, "[nc/touch] UP   %.0f,%.0f\n", touch_x, touch_y);
    }
}

/* ===== Chord soberano ==================================================== */
static nxinput_exit_chord exit_chord;

/* ===== Init ============================================================== */
int st_input_preinit(void)
{
    /* Fronteira pré-init do nxinput 0.10.0: owner/default + FACE_LAYOUT lidos
     * UMA vez, antes de bundle, staging e de qualquer SDL_Init. */
    int rc = nc_gptk_preinit(st_gamedir);
    if (rc != 0)
        return -1;
    if (nc_gptk_loaded()) {
        /* O tuning do [cursor] do arquivo (defaults quando ausente) é a única
         * autoridade de movimento da seta. */
        nc_gptk_cursor_tuning_copy(&cursor_tuning);
        cursor_tuning_ready = 1;
    }
    return 0;
}

int st_input_init(void)
{
#ifdef NC_BENCH
    input_diag = getenv("NC_INPUT_DIAG") != NULL;
#endif
    if (getenv("NC_CURSOR_HIDE")) {
        float v = strtof(getenv("NC_CURSOR_HIDE"), NULL);
        cursor_hide_after = (v >= 0.0f) ? v : 4.0f;
    }
    if (getenv("ST_NO_ADS"))
        platform_no_ads_enabled = strcmp(getenv("ST_NO_ADS"), "0") != 0;
    if (getenv("ST_HIDE_TOUCH_BUTTONS"))
        platform_hide_touch_buttons =
            strcmp(getenv("ST_HIDE_TOUCH_BUTTONS"), "0") != 0;
#ifdef NC_BENCH
    vpad_enabled = getenv("NC_VPAD") && strcmp(getenv("NC_VPAD"), "0") != 0;
    if (getenv("NC_VPAD_FILE") && *getenv("NC_VPAD_FILE"))
        vpad_file = getenv("NC_VPAD_FILE");
#endif

    nxinput_gptk_cursor_state_reset(&cursor_state, screen_width * 0.5f,
                                    screen_height * 0.5f);
    /* Disparo no PRIMEIRO quadro em que SELECT e START estão ambos lógicos
     * (regra #40: chord sem hold/atraso); nada do chord vaza ao jogo. */
    nxinput_exit_chord_init(&exit_chord, 1);

    nc_sdl_joy_vendor = (Uint16 (*)(SDL_Joystick *))
        dlsym(RTLD_DEFAULT, "SDL_JoystickGetVendor");
    nc_sdl_joy_product = (Uint16 (*)(SDL_Joystick *))
        dlsym(RTLD_DEFAULT, "SDL_JoystickGetProduct");
    if (!nc_gptk_loaded()) {
        /* Sem mapa válido não existe caminho de palpite: fail-closed. */
        fprintf(stderr, "[nc/input] NEXTOSCONTROLLERS ausente/inválido; "
                        "abortando (fail-closed)\n");
        return -1;
    }
    if (nc_stage_seam_before_init() != 0)
        return -1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[nc/input] SDL controller init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    if (padset_setup() != 0)
        return -1;
    open_controller();
    if (nc_register_sinks() != 0) {
        fprintf(stderr, "[nc/input] runtime vivo não selado; abortando "
                        "(fail-closed)\n");
        return -1;
    }
    fprintf(stderr,
            "[nc/input] layout: cursor=menu-only(right-stick+R3) "
            "chord=SELECT+START(sovereign) touch-buttons=%s no-ads=%s "
            "vpad=%s\n",
            platform_hide_touch_buttons ? "hidden" : "owner-default",
            platform_no_ads_enabled ? "on" : "off",
            vpad_enabled ? "on" : "off");
    return (controller || vpad_enabled) ? 0 : -1;
}

/* ===== Poll por quadro =================================================== */
static void sample_controls(void)
{
    memset(control_down, 0, sizeof control_down);
    /* Framework: união dos pads admitidos, chord só no mesmo instance,
     * cross-pad negado e registrado pelo padset. */
    nxinput_padset_sample(&padset);
    memcpy(buttons, padset.buttons, sizeof buttons < sizeof padset.buttons ? sizeof buttons : sizeof padset.buttons);
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        int control = nc_control_of(i);
        if (control >= 0 && buttons[i])
            control_down[control] = 1;
    }
    trigger_value[0] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    trigger_value[1] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    for (int t = 0; t < 2; t++) {
        int was = trigger_digital[t];
        trigger_digital[t] = was ? trigger_value[t] > NC_TRIGGER_EXIT
                                 : trigger_value[t] > NC_TRIGGER_ENTER;
    }
    control_down[NXINPUT_GPTK_L2] = trigger_digital[0];
    control_down[NXINPUT_GPTK_R2] = trigger_digital[1];
    if (vpad_enabled)
        for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++)
            if (vpad_frames[c] > 0)
                control_down[c] = 1;
}

static float stick_axis(int index)
{
    if (vpad_enabled && index >= 0 && index < 4 && vpad_axis_frames[index] > 0)
        return vpad_axis_values[index];
    static const SDL_GameControllerAxis map[4] = {
        SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
        SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
    };
    return axis_value(map[index]);
}

void st_input_poll(void *env, void *player, unsigned long frame)
{
    input_last_env = env;
    input_last_player = player;
    input_frame = frame;
    move_vector_this_frame = 0;
    cursor_vector_this_frame = 0;
    viewport_adapt_level_select(frame);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_controller();
        if (event.type == SDL_JOYDEVICEREMOVED && nc_seam_adopted)
            nxc6_forget((int)event.jdevice.which);
        if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
            nxinput_padset_remove_instance(&padset, event.cdevice.which)) {
            controller = nxinput_padset_first(&padset);
            cancel_synthetic_touch("controller-removed");
            nc_gptk_release_all("controller-removed");
            release_all_keys();
            down_action_held = 0;
            open_controller();
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            cancel_synthetic_touch("focus-lost");
            nc_gptk_release_all("focus-lost");
            release_all_keys();
            down_action_held = 0;
        }
    }
    vpad_poll();
    sample_controls();
    platform_update_no_ads(frame);
    update_engine_context(frame);

    if (!controller && !vpad_enabled) {
        cancel_synthetic_touch("controller-unavailable");
        nc_gptk_release_all("controller-unavailable");
        release_all_keys();
        return;
    }

    /* Chord soberano SELECT+START: fronteira do framework, lida ANTES do GPTK
     * e fora do arquivo do dono.  Somente SELECT e START lógicos do MESMO pad;
     * GUIDE, L1+R1, L2+R2 ou qualquer outra combinação jamais encerram. */
    /* Chord SELECT+START pelo nxinput: só quando UM instance segura os dois
     * (nxinput_padset); SELECT num pad + START noutro nunca encerra. */
    int chord_select = 0, chord_start = 0;
    nxinput_padset_chord_inputs(&padset, &chord_select, &chord_start);
    if (nxinput_exit_chord_fold_signal(&exit_chord, &signal_exit) ||
        nxinput_exit_chord_update(&exit_chord, chord_select, chord_start)) {
        (void)nxinput_exit_chord_consume(&exit_chord);
        fprintf(stderr, "[nc/input] SELECT+START: lifecycle exit requested\n");
        exit_requested = 1;
        cancel_synthetic_touch("exit-chord");
        nc_gptk_release_all("exit-chord");
        release_all_keys();
        return;
    }

    /* ===== Despacho GPTK (botões e gatilhos: transições físicas) ===== */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (c == NXINPUT_GPTK_LEFT_STICK || c == NXINPUT_GPTK_RIGHT_STICK)
            continue;
        float value = control_down[c] ? 1.0f : 0.0f;
        if (c == NXINPUT_GPTK_L2) value = trigger_value[0];
        if (c == NXINPUT_GPTK_R2) value = trigger_value[1];
        int rc = nc_gptk_feed_button(c, control_down[c], value);
        if (rc == NC_GPTK_LIVE_FATAL)
            input_fatal = 1;
    }

    /* ===== Vetores ===== */
    float lx = stick_axis(0), ly = stick_axis(1);
    float rx = stick_axis(2), ry = stick_axis(3);
    float lx_dz = lx, ly_dz = ly, rx_dz = rx, ry_dz = ry;
    radial_deadzone(&lx_dz, &ly_dz);
    radial_deadzone(&rx_dz, &ry_dz);
    int left_consumed = nc_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK);
    int right_consumed = nc_gptk_should_consume(NXINPUT_GPTK_RIGHT_STICK);
    if (input_diag && frame % 30 == 0 &&
        (fabsf(lx) > 0.01f || fabsf(ly) > 0.01f || fabsf(rx) > 0.01f ||
         fabsf(ry) > 0.01f))
        fprintf(stderr, "[nc/axes] frame=%lu L=%.2f,%.2f R=%.2f,%.2f "
                        "consumed=%d/%d vpad_frames=%u,%u,%u,%u\n",
                frame, lx, ly, rx, ry, left_consumed, right_consumed,
                vpad_axis_frames[0], vpad_axis_frames[1],
                vpad_axis_frames[2], vpad_axis_frames[3]);
    if (left_consumed &&
        nc_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, lx_dz, ly_dz) ==
            NC_GPTK_LIVE_FATAL)
        input_fatal = 1;
    if (right_consumed) {
        /* cursor.move recebe o eixo CRU: a deadzone é a do [cursor] do
         * arquivo, aplicada uma única vez pela cinemática canônica. */
        if (nc_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, rx, ry) ==
                NC_GPTK_LIVE_FATAL)
            input_fatal = 1;
    }

    if (input_fatal) {
        fprintf(stderr, "[nc/input] FATAL no runtime vivo: encerrando sem "
                        "reproduzir nativamente\n");
        exit_requested = 1;
        cancel_synthetic_touch("gptk-fatal");
        release_all_keys();
        return;
    }

    /* ===== Passthrough nativo dirigido por ESTADO =====
     * desejado = pressionado && não consumido pelo GPTK (native/desconhecido);
     * a tabela de teclas entregues garante o UP de todo DOWN. */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        int keycode = nc_native_keycode(c);
        if (!keycode)
            continue;
        int desired = control_down[c] && !nc_gptk_should_consume(c);
        /* Um keycode compartilhado com um sink (A=96 por player.jump/
         * menu.accept) só é liberado pelo caminho nativo quando o sink não
         * está segurando o mesmo keycode. */
        if (!desired && sink_key_pressed[keycode])
            continue;
        deliver_key(keycode, desired);
    }

    /* ===== MotionEvent do quadro: eixos X/Y (esquerdo), Z/RZ (direito),
     * HAT (D-pad nativo).  Fontes por eixo, sem dupla entrega:
     *   X/Y  <- player.move (sink) OU LEFT_STICK native (deadzone radial)
     *           + player.down (sink) + D-pad native (digital)
     *   Z/RZ <- RIGHT_STICK native (deadzone radial); cursor.move nunca vaza
     *   HAT  <- D-pad native
     *   gatilhos: nunca como eixo (o perfil móvel do jogo lê gatilho como
     *   pause — medido no fechamento 1.0.0); L2/R2 só como keycode/sink. */
    float ax = 0.0f, ay = 0.0f, az = 0.0f, arz = 0.0f, hx = 0.0f, hy = 0.0f;
    if (move_vector_this_frame) {
        ax = move_axis_x;
        ay = move_axis_y;
    } else if (!left_consumed) {
        ax = lx_dz;
        ay = ly_dz;
    }
    if (!right_consumed) {
        az = rx_dz;
        arz = ry_dz;
    }
    int up = control_down[NXINPUT_GPTK_UP] &&
             !nc_gptk_should_consume(NXINPUT_GPTK_UP);
    int dn = control_down[NXINPUT_GPTK_DOWN] &&
             !nc_gptk_should_consume(NXINPUT_GPTK_DOWN);
    int lf = control_down[NXINPUT_GPTK_LEFT] &&
             !nc_gptk_should_consume(NXINPUT_GPTK_LEFT);
    int rt = control_down[NXINPUT_GPTK_RIGHT] &&
             !nc_gptk_should_consume(NXINPUT_GPTK_RIGHT);
    hx = (float)(rt - lf);
    hy = (float)(dn - up);
    /* D-pad nativo também alimenta os eixos principais: o jogo lê "axis 1/2"
     * para andar/agachar; o mesmo estado digital em X/Y é uma fusão de
     * estado, não uma segunda entrega. */
    ax = clampf(ax + hx);
    ay = clampf(ay + hy + (down_action_held ? 1.0f : 0.0f));
    inject(env, player, st_jni_motion_event(ax, ay, az, arz, 0.0f, 0.0f,
                                            hx, hy));

    update_cursor();

    if (input_diag && frame > 0 && frame % 300 == 0)
        fprintf(stderr,
                "[nc/input] diag ctx=%d src=%s deliveries=%lu keys_down=%d\n",
                nc_gptk_context(), nc_gptk_context_source(),
                nc_gptk_delivery_count(), key_down_state[AKEY_BUTTON_A]);
}

void st_input_close(void)
{
    cancel_synthetic_touch("shutdown");
    nc_gptk_release_all("shutdown");
    release_all_keys();
    close_controller();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
                      SDL_INIT_EVENTS);
    input_last_env = NULL;
    input_last_player = NULL;
}

int st_input_exit_requested(void)
{
    return exit_requested;
}

int st_input_fatal(void)
{
    return input_fatal;
}

int st_input_cursor(float *x, float *y)
{
    if (!cursor_visible_context || cursor_idle_hidden())
        return 0;
    if (x) *x = cursor_state.x * 1280.0f / (float)screen_width;
    if (y) *y = cursor_state.y * 720.0f / (float)screen_height;
    return 1;
}

void st_input_set_screen_size(int width, int height)
{
    /* O EGL publica o viewport a cada quadro: só um tamanho NOVO recentra a
     * seta (recentrar sempre congelava o cursor no meio da tela). */
    if (width <= 0 || height <= 0 ||
        (width == screen_width && height == screen_height))
        return;
    screen_width = width;
    screen_height = height;
    nxinput_gptk_cursor_state_reset(&cursor_state, screen_width * 0.5f,
                                    screen_height * 0.5f);
}

void st_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void st_input_keyboard_set(const char *text)
{
    (void)text;
}

void st_input_keyboard_hide(void)
{
}

int st_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const st_keyboard_key **keys,
                                size_t *key_count)
{
    if (text && text_size) text[0] = '\0';
    if (uppercase) *uppercase = 0;
    if (selected) *selected = 0;
    if (keys) *keys = NULL;
    if (key_count) *key_count = 0;
    return 0;
}
