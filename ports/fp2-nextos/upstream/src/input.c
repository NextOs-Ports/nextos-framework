/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Freedom Planet 2 1.1.2 — controle NextOS -> entrada Android/Unity.
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
 *   (FPStage.playerInstance_FPPlayer VIVO via IL2CPP -> gameplay; sem
 *   jogadora viva -> menu; cena vazia/Loading ou contrato indisponível ->
 *   passthrough; FPStage.state PAUSED ou timeScale 0 -> menu de pause)
 *        |
 *        +--> ACTION  -> runtime vivo -> sink real deste adapter (o MESMO
 *        |               KeyEvent/MotionEvent Android que o Input legado da
 *        |               Unity lê: o FP2 consome JoystickButtonN e eixos)
 *        +--> null    -> nada, em nenhum caminho
 *        +--> native  -> passthrough único: o mesmo KeyEvent/MotionEvent que
 *                        a Activity Android entregaria
 *
 * O caminho nativo é dirigido por ESTADO (desejado x entregue), nunca por
 * borda: todo KeyEvent DOWN recebe o seu UP mesmo quando o controle muda de
 * dono no meio da pressão.  Não existe joystick cru, ordinal posicional,
 * varredura própria de evdev, nome de aparelho, VID/PID, teclado sintético
 * nem SDL privada.  O jogo é plataforma de console: não há cursor.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "gb.h"
#include "il2.h"
#include "nx_elf.h"
#include "nxinput_sdl_seam.h"
#include "nxc6_glue.h"
#include "nxinput_gptk.h"
#include "nxinput_exit_chord.h"
#include "input_gptk.h"
#include "nxinput_padset.h"
#ifdef FP2_BENCH_PROBES
#include "bench_probes.h"
#endif

/* ===== Constantes Android consumidas pela Unity (Input legado) =========
 * Unity Android: BUTTON_A..Y -> JoystickButton0..3, L1/R1 -> 4/5,
 * THUMBL/THUMBR -> 8/9, START -> 10 (o "PRESS 10 TO BEGIN" do título),
 * SELECT -> 11. */
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
#define AKEY_BUTTON_L2 104
#define AKEY_BUTTON_R2 105
#define AKEY_BUTTON_THUMBL 106
#define AKEY_BUTTON_THUMBR 107
#define AKEY_BUTTON_START 108
#define AKEY_BUTTON_SELECT 109

#define FP2_STICK_DEADZONE 0.15f /* radial, com reescala */
#define FP2_TRIGGER_ENTER NXINPUT_GPTK_TRIGGER_ENTER
#define FP2_TRIGGER_EXIT NXINPUT_GPTK_TRIGGER_EXIT
#define FP2_SCENE_SAMPLE_FRAMES 30

/* Vários pads admitidos ao mesmo tempo (até FP2_MAX_PADS): o estado
 * simbólico é a união deles; o chord SELECT+START só vale no MESMO pad
 * (instance) — SELECT num pad e START noutro nunca encerram. `controller`
 * segue apontando para o primeiro pad admitido (compatibilidade). */
static nxinput_padset padset;      /* framework: união dos pads, chord por instance */
static nxinput_padset_sdl padset_sdl; /* preenchida com a SDL do firmware já resolvida */
static SDL_GameController *controller; /* primeiro pad admitido (compatibilidade) */
static uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX];
static volatile sig_atomic_t exit_requested;
static volatile sig_atomic_t signal_exit;
static int input_fatal;
/* Diagnóstico de bancada (-DFP2_BENCH_PROBES + env); sempre 0 na release. */
static int input_diag;
static void *input_last_env;
static void *input_last_player;

void fp2_input_request_exit(void)
{
    signal_exit = 1;
    exit_requested = 1;
}

/* ===== Símbolos SDL acima do piso público 2.0.4: dlsym com fallback ===== */
static const char *(*fp2_sdl_path_for_index)(int);
static SDL_JoystickID (*fp2_sdl_instance_for_index)(int);
static Uint16 (*fp2_sdl_joy_vendor)(SDL_Joystick *);
static Uint16 (*fp2_sdl_joy_product)(SDL_Joystick *);

static void *optional_sdl(const char *name)
{
    (void)dlerror();
    return dlsym(RTLD_DEFAULT, name);
}

/* ===== Admissão canônica do controle (nxinput C6) ======================= */
static char fp2_staged_mapping[NXINPUT_AUTHORITY_SOURCE_MAX];
static int fp2_seam_adopted;

static const char *fp2_env_get(void *userdata, const char *name)
{
    (void)userdata;
    return getenv(name);
}

static int fp2_env_unset(void *userdata, const char *name)
{
    (void)userdata;
    return unsetenv(name);
}

static int fp2_sdl_was_init(void *userdata)
{
    (void)userdata;
    return SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0;
}

static int fp2_stage_seam_before_init(void)
{
    nxinput_sdl_seam_env_ops env;
    size_t staged_len = 0;
    int rc;

    if (!getenv("NXC6_SEAM")) {
        fprintf(stderr, "[fp2/input] NXC6 seam: not adopted for this run "
                        "(NXC6_SEAM absent); stock SDL behaviour\n");
        return 0;
    }
    *(void **)&fp2_sdl_path_for_index = optional_sdl("SDL_JoystickPathForIndex");
    *(void **)&fp2_sdl_instance_for_index =
        optional_sdl("SDL_JoystickGetDeviceInstanceID");
    if (!fp2_sdl_path_for_index || !fp2_sdl_instance_for_index) {
        fprintf(stderr, "[fp2/input] NXC6 seam: this SDL cannot name the "
                        "device node (pre-2.24); staying native\n");
        return 0;
    }
    memset(&env, 0, sizeof env);
    env.api_version = NXINPUT_SDL_SEAM_API_VERSION;
    env.struct_size = sizeof env;
    env.getenv_fn = fp2_env_get;
    env.unsetenv_fn = fp2_env_unset;
    env.sdl_was_init_fn = fp2_sdl_was_init;
    rc = nxinput_sdl_seam_stage_before_init(&env, fp2_staged_mapping,
                                            sizeof fp2_staged_mapping,
                                            &staged_len);
    if (rc != 0) {
        fprintf(stderr, "[fp2/input] NXC6 seam: staging failed before the "
                        "joystick init (rc=%d); refusing to guess\n", rc);
        return -1;
    }
    if (staged_len > 0 &&
        setenv("NXC6_STAGED_MAPPING", fp2_staged_mapping, 1) != 0) {
        fprintf(stderr, "[fp2/input] NXC6 seam: could not hand the staged "
                        "mapping to the seam\n");
        return -1;
    }
    fp2_seam_adopted = 1;
    {
        int declared = nxc6_declare_port_bundle_for_layout(
            fp2_gamedir, fp2_gptk_face_layout());
        fprintf(stderr, "[fp2/input] NXC6 seam: port bundle %s (layout=%s)\n",
                declared > 0 ? getenv("NXCONTROLLER_PROFILES")
                : declared == 0 ? "(none shipped)"
                : "(declaration failed)",
                nxinput_gptk_face_layout_name(fp2_gptk_face_layout()));
    }
    fprintf(stderr,
            "[fp2/input] NXC6 seam: staged=%zu bytes env_still_set=%d "
            "receipt=%s\n",
            staged_len, getenv("SDL_GAMECONTROLLERCONFIG") != NULL,
            getenv("NXC6_RECEIPT") ? getenv("NXC6_RECEIPT") : "(none)");
    return 0;
}

/* ===== Contexto provado pela engine: cena ativa por NOME (IL2CPP) =======
 * Nada de RVA.  Lido no máximo a cada FP2_SCENE_SAMPLE_FRAMES quadros, só
 * depois do frame 0 (entrar no domínio antes do primeiro nativeRender mata a
 * Unity).  Classes/métodos resolvidos uma única vez e cacheados. */
static int scene_api_state;                  /* 0 = não tentado, 1 ok, -1 falhou */
static const MethodInfo *scene_get_active;
static const MethodInfo *scene_get_name;
static const MethodInfo *time_get_timescale;
static char scene_name[96];
static int scene_is_loading;
static int scene_is_stage;
static int scene_paused;

static int scene_api_resolve(void)
{
    if (scene_api_state)
        return scene_api_state > 0;
    if (!il2_load()) {
        scene_api_state = -1;
        return 0;
    }
    il2_attach();
    Il2CppClass *manager = il2_class("UnityEngine.SceneManagement",
                                     "SceneManager");
    Il2CppClass *scene = il2_class("UnityEngine.SceneManagement", "Scene");
    Il2CppClass *time = il2_class("UnityEngine", "Time");
    scene_get_active = manager ? il2_method(manager, "GetActiveScene", 0) : NULL;
    scene_get_name = scene ? il2_method(scene, "get_name", 0) : NULL;
    time_get_timescale = time ? il2_method(time, "get_timeScale", 0) : NULL;
    scene_api_state = scene_get_active && scene_get_name ? 1 : -1;
    fprintf(stderr, "[fp2/input] scene contract: %s (timescale=%s)\n",
            scene_api_state > 0 ? "ready" : "unavailable",
            time_get_timescale ? "ready" : "unavailable");
    return scene_api_state > 0;
}

/* ===== Estágio vivo provado pela engine ================================
 * O menu principal do FP2 também é um FPStage (currentStage vivo no título),
 * então a prova de GAMEPLAY é a JOGADORA: FPStage.playerInstance_FPPlayer
 * apontando para um FPPlayer VIVO (UnityEngine.Object.op_Implicit é falso
 * para objeto destruído — os estáticos sobrevivem à troca de cena). Pause =
 * FPStage.currentStage.state == STATE_PAUSED (enum FPStageState do jogo) ou
 * Time.timeScale 0. Sem jogadora viva = menu: título, mapa-múndi, lojas e
 * cutscenes usam as mesmas teclas de confirmar/cancelar. Cena vazia ou
 * "Loading", ou contrato indisponível = desconhecido (passthrough). Nenhuma
 * lista de cenas por nome. Campos lidos como estático ou de instância
 * conforme os FLAGS que o próprio il2cpp declara (nada presumido). */
#define FIELD_ATTRIBUTE_STATIC 0x0010
#define FP_STAGE_STATE_PAUSED 2   /* FPStageState: INIT=0, RUNNING=1, PAUSED=2 */
static int stage_api_state;                  /* 0 = não tentado, 1 ok, -1 falhou */
static FieldInfo *stage_current_field;
static FieldInfo *stage_player_field;
static FieldInfo *stage_state_field;
static int stage_player_static, stage_state_static;
static const MethodInfo *object_alive_method;

static int field_flags(FieldInfo *f, int *is_static)
{
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    int (*get_flags)(void *) = mod ? (void *)nx_lookup_in(mod, "il2cpp_field_get_flags") : NULL;
    if (!f || !get_flags)
        return 0;
    *is_static = (get_flags(f) & FIELD_ATTRIBUTE_STATIC) != 0;
    return 1;
}

static int stage_api_resolve(void)
{
    if (stage_api_state)
        return stage_api_state > 0;
    Il2CppClass *stage = il2_class("", "FPStage");
    Il2CppClass *object = il2_class("UnityEngine", "Object");
    stage_current_field = stage ? il2_field(stage, "currentStage") : NULL;
    stage_player_field = stage ? il2_field(stage, "playerInstance_FPPlayer") : NULL;
    stage_state_field = stage ? il2_field(stage, "state") : NULL;
    object_alive_method = object ? il2_method(object, "op_Implicit", 1) : NULL;
    int current_static = 0;
    int ok = field_flags(stage_current_field, &current_static) && current_static &&
             field_flags(stage_player_field, &stage_player_static) &&
             field_flags(stage_state_field, &stage_state_static) &&
             object_alive_method != NULL;
    stage_api_state = ok ? 1 : -1;
    fprintf(stderr, "[fp2/input] stage contract: %s (currentStage=%s player=%s%s state=%s%s op_Implicit=%s)\n",
            ok ? "ready" : "unavailable",
            stage_current_field ? (current_static ? "static" : "instance!") : "missing",
            stage_player_field ? "ok" : "missing", stage_player_static ? "/static" : "/instance",
            stage_state_field ? "ok" : "missing", stage_state_static ? "/static" : "/instance",
            object_alive_method ? "ok" : "missing");
    return stage_api_state > 0;
}

static int object_alive(void *obj)
{
    if (!obj)
        return 0;
    void *args[1] = { obj };
    Il2CppObject *boxed = il2_call(object_alive_method, NULL, args,
                                   "Object.op_Implicit");
    void *v = il2_unbox(boxed);
    return v && *(uint8_t *)v != 0;
}

/* Lê um campo do FPStage: estático direto, de instância via currentStage vivo. */
static int stage_read(FieldInfo *f, int is_static, void *out, size_t size)
{
    memset(out, 0, size);
    if (is_static) {
        il2_static_get(f, out);
        return 1;
    }
    void *current = NULL;
    il2_static_get(stage_current_field, &current);
    if (!object_alive(current))
        return 0;
    il2_field_get(current, f, out);
    return 1;
}

static int stage_player_alive(void)
{
    void *player = NULL;
    if (!stage_read(stage_player_field, stage_player_static, &player, sizeof player))
        return 0;
    return object_alive(player);
}

static int stage_paused(void)
{
    int32_t state = 0;
    if (stage_read(stage_state_field, stage_state_static, &state, sizeof state) &&
        state == FP_STAGE_STATE_PAUSED)
        return 1;
    if (time_get_timescale) {
        Il2CppObject *r = il2_call(time_get_timescale, NULL, NULL,
                                   "Time.get_timeScale");
        void *v = il2_unbox(r);
        if (v && *(float *)v < 0.001f)
            return 1;
    }
    return 0;
}

static void sample_scene(void)
{
    if (!scene_api_resolve())
        return;
    Il2CppObject *boxed = il2_call(scene_get_active, NULL, NULL,
                                   "GetActiveScene");
    void *raw = il2_unbox(boxed);
    char text[96] = "";
    if (raw) {
        Il2CppObject *name = il2_call(scene_get_name, raw, NULL,
                                      "Scene.get_name");
        il2_str_utf8(name, text, sizeof text);
    }
    if (strcmp(text, scene_name) != 0) {
        snprintf(scene_name, sizeof scene_name, "%s", text);
        fprintf(stderr, "[fp2/input] active scene=\"%s\"\n", scene_name);
    }
    scene_is_loading = scene_name[0] == 0 || strcmp(scene_name, "Loading") == 0;
    scene_is_stage = 0;
    scene_paused = 0;
    if (!stage_api_resolve() || scene_is_loading)
        return;
    scene_is_stage = stage_player_alive();
    if (scene_is_stage)
        scene_paused = stage_paused();
}

static void update_engine_context(unsigned long frame)
{
    if (frame == 0) {
        fp2_gptk_clear_context("frame0");
        return;
    }
    if (frame % FP2_SCENE_SAMPLE_FRAMES == 1)
        sample_scene();
    if (scene_api_state <= 0) {
        fp2_gptk_clear_context("scene-contract-unavailable");
        return;
    }
    if (scene_is_loading)
        fp2_gptk_clear_context("scene:loading");
    else if (stage_api_state <= 0)
        fp2_gptk_clear_context("stage-contract-unavailable");
    else if (scene_is_stage && scene_paused)
        fp2_gptk_set_context(FP2_GPTK_CONTEXT_MENU, "stage:paused");
    else if (scene_is_stage)
        fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    else
        fp2_gptk_set_context(FP2_GPTK_CONTEXT_MENU, "stage:no-player");
}

/* ===== Injeção Android -> Unity ========================================= */
static void inject(void *env, void *player, void *event)
{
    static void *native_inject;
    if (!native_inject)
        native_inject = fp2_jni_native("com/unity3d/player/UnityPlayer",
                                       "nativeInjectEvent");
    if (native_inject && event) {
        /* Unity 2022+ registra nativeInjectEvent(InputEvent, displayId). */
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (input_diag)
            fprintf(stderr, "[fp2/input] inject event=%p consumed=%d\n",
                    event, consumed);
    } else if (input_diag) {
        fprintf(stderr, "[fp2/input] inject SKIPPED inject=%p event=%p\n",
                native_inject, event);
    }
}

/* Estado entregue de teclas Android: a MESMA tabela serve o passthrough
 * nativo e os sinks; todo DOWN recebe o seu UP e dois donos nunca produzem
 * dois DOWNs para o mesmo keycode. */
static uint8_t key_down_state[256];
static int sink_key_pressed[256];

static void deliver_key(int keycode, int down)
{
    if (keycode <= 0 || keycode >= 256)
        return;
    if ((key_down_state[keycode] != 0) == (down != 0))
        return;
    key_down_state[keycode] = down ? 1 : 0;
    if (input_diag)
        fprintf(stderr, "[fp2/key] keycode=%d %s\n", keycode,
                down ? "down" : "up");
    inject(input_last_env, input_last_player,
           fp2_jni_key_event(down ? 0 : 1, keycode, keycode));
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

/* ===== Sinks reais do adapter (ACK = 0) =================================
 * Símbolos exportados de propósito: o nxrelease liga o sink-id do
 * adapter-contract a um símbolo definido neste ELF.  A tabela ação ->
 * keycode é o binding padrão de gamepad do FP2 (o menu Controls do jogo pode
 * rebindar; o nome semântico descreve o default). */
int fp2_sink_android_keyevent(void *user, const char *action, int pressed,
                              float value)
{
    (void)value;
    int keycode = (int)(intptr_t)user;
    if (input_diag)
        fprintf(stderr, "[fp2/sink] %s keycode=%d %s\n", action, keycode,
                pressed ? "down" : "up");
    sink_key(keycode, pressed);
    return 0;
}

static float move_axis_x, move_axis_y;
static int move_vector_this_frame;
int fp2_sink_android_motion(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    move_axis_x = x;
    move_axis_y = y;
    move_vector_this_frame = 1;
    return 0;
}

static int fp2_register_sinks(void)
{
    /* Keycode Android que o binding NATIVO do FP2 espera para cada ação —
     * lido de dentro da engine (InputControl.mKeysList, JoystickInput.mButton,
     * 0-based): Attack=Button0 (KEYCODE_BUTTON_A), Jump=Button1 (B),
     * Special=Button2 (X), Guard=Button3 (Y), Pause=Button9 (THUMBR — o
     * "Joystick Button 10" que o título pede). Confirmar nos menus = Jump;
     * cancelar = Attack (medido: B ativou Quit, A voltou o destaque). */
    struct { const char *action; int keycode; } b[] = {
        { "fp2.jump",    AKEY_BUTTON_B },
        { "fp2.attack",  AKEY_BUTTON_A },
        { "fp2.special", AKEY_BUTTON_X },
        { "fp2.guard",   AKEY_BUTTON_Y },
        { "fp2.pause",   AKEY_BUTTON_THUMBR },
        { "fp2.confirm", AKEY_BUTTON_B },
        { "fp2.cancel",  AKEY_BUTTON_A },
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (fp2_gptk_register_button(b[i].action,
                                     "adapter.input.android-keyevent",
                                     fp2_sink_android_keyevent,
                                     (void *)(intptr_t)b[i].keycode) != 0)
            return -1;
    if (fp2_gptk_register_vector("fp2.move", "adapter.input.android-motion",
                                 fp2_sink_android_motion, NULL) != 0)
        return -1;
    return fp2_gptk_seal();
}

/* ===== Ponte SDL_GameController -> vocabulário simbólico ================ */
static int fp2_control_of(int sdl_button)
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

static int fp2_native_keycode(int control)
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

/* Deadzone radial com reescala: repouso é ZERO exato e a deflexão total
 * continua alcançando 1. */
static void radial_deadzone(float *x, float *y)
{
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= FP2_STICK_DEADZONE) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    float scale = (m - FP2_STICK_DEADZONE) / (1.0f - FP2_STICK_DEADZONE);
    if (scale > 1.0f)
        scale = 1.0f;
    *x = *x / m * scale;
    *y = *y / m * scale;
}

#ifdef FP2_BENCH_PROBES
/* ===== Pad virtual de bancada (SÓ na variante de bancada
 * -DFP2_BENCH_PROBES; a release pública não compila este caminho) ===================
 * Um token por arquivo em FP2_VPAD_FILE (default /tmp/fp2-vpad), lido uma
 * vez por quadro e apagado, vira um pulso de N quadros no estado SIMBÓLICO —
 * depois do mapping soberano da SDL, antes do GPTK.  Estímulo determinístico
 * para a prova de remap/null; nunca prova o mapping físico sozinho (o recibo
 * C6 do pad real acompanha).  Inerte sem a env. */
static int vpad_enabled;
static const char *vpad_file = "/tmp/fp2-vpad";
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
    char *sep = strrchr(token, ':');
    if (sep && sep[1]) {
        long parsed = strtol(sep + 1, NULL, 10);
        if (parsed > 0 && parsed <= 600)
            duration = (unsigned)parsed;
        *sep = '\0';
    }
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
    static const struct { const char *name; int axis; float v; } axes[] = {
        { "lx+", 0, 1.0f }, { "lx-", 0, -1.0f },
        { "ly+", 1, 1.0f }, { "ly-", 1, -1.0f },
        { "rx+", 2, 1.0f }, { "rx-", 2, -1.0f },
        { "ry+", 3, 1.0f }, { "ry-", 3, -1.0f },
    };
    int found = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names && !found; i++)
        if (!strcasecmp(token, names[i].name)) {
            vpad_frames[names[i].control] = duration;
            found = 1;
        }
    for (size_t i = 0; i < sizeof axes / sizeof *axes && !found; i++)
        if (!strcasecmp(token, axes[i].name)) {
            vpad_axis_frames[axes[i].axis] = duration;
            vpad_axis_values[axes[i].axis] = axes[i].v;
            found = 1;
        }
    if (!found && !strcasecmp(token, "exit")) {
        vpad_frames[NXINPUT_GPTK_SELECT] = duration;
        vpad_frames[NXINPUT_GPTK_START] = duration;
        found = 1;
    }
    if (!found && !strcasecmp(token, "shot")) {
        extern int fp2_shot_request;
        fp2_shot_request = 1;
        found = 1;
    }
    fprintf(stderr, "[fp2/vpad] pulse %s x%u (%s)\n", token, duration,
            found ? "accepted" : "unknown");
}

#else /* release pública: nenhum caminho de injeção compilado */
enum { vpad_enabled = 0 };
static const unsigned vpad_frames[NXINPUT_GPTK_CONTROL_COUNT] = { 0 };
static const unsigned vpad_axis_frames[4] = { 0 };
static const float vpad_axis_values[4] = { 0 };
static void vpad_poll(void) { }
#endif /* FP2_BENCH_PROBES */

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

/* ===== Controle: abertura e identidade =================================== */
/* Admissão pela autoridade do port (costura C6): o padset nunca decide. */
static int padset_admit(int i, void *user)
{
    (void)user;
    if (fp2_seam_adopted) {
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_text[64];
        const char *devpath = fp2_sdl_path_for_index
                            ? fp2_sdl_path_for_index(i) : NULL;
        SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
        if (!nxc6_admit_before_announce(
                (int)fp2_sdl_instance_for_index(i), guid_text,
                devpath ? devpath : "")) {
            fprintf(stderr, "[fp2/input] NXC6 seam: device %d (%s) "
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
    int vendor = (joy && fp2_sdl_joy_vendor) ? fp2_sdl_joy_vendor(joy) : 0;
    int product = (joy && fp2_sdl_joy_product) ? fp2_sdl_joy_product(joy) : 0;
    char *mapping = SDL_GameControllerMapping(opened);
    /* Identidade apresentada à Unity: o nome real do pad.  O FP2 não
     * escolhe glyphs por nome (imprime o KeyCode da Unity); o mapping
     * soberano da SDL é a autoridade física. */
    if (slot == 0)
        fp2_jni_input_device_info(physical ? physical : "NextOS Gamepad",
                                  vendor, product,
                                  physical ? physical : "gamepad");
    fprintf(stderr, "[fp2/input] controller: %s (%04x:%04x) mapping=%s\n",
            physical ? physical : "unknown", vendor & 0xffff,
            product & 0xffff, mapping ? mapping : "unavailable");
    fprintf(stderr, "[fp2/input] pad slot=%d instance=%d sdl_index=%d\n",
            slot, (int)padset.instances[slot], i);
    SDL_free(mapping);
}

static void padset_log(const char *line, void *user)
{
    (void)user;
    fprintf(stderr, "[fp2/input] %s\n", line);
}


/* ===== nxinput_padset: vtable sobre a SDL do firmware (nunca privada) ===== */
static int ps_num_joysticks(void) { return SDL_NumJoysticks(); }
static int32_t ps_instance_for_index(int i) { return (int32_t)fp2_sdl_instance_for_index(i); }
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
        fprintf(stderr, "[fp2/input] nxinput_padset: vtable incompleta (fail-closed)\n");
        return -1;
    }
    fprintf(stderr, "[fp2/input] pads: %s (união dos admitidos, chord por instance)\n",
            nxinput_padset_marker());
    return 0;
}

static void open_controller(void)
{
    nxinput_padset_open_all(&padset, padset_admit, padset_opened, NULL);
    controller = nxinput_padset_first(&padset);
    if (padset.count == 0 && SDL_NumJoysticks() > 0)
        fprintf(stderr, "[fp2/input] %d joystick(s) visible but none admitted "
                        "as GameController; no fallback by design\n",
                SDL_NumJoysticks());
}

static void close_controller(void)
{
    nxinput_padset_close_all(&padset);
    controller = NULL;
    memset(buttons, 0, sizeof buttons);
}

/* ===== Chord soberano ==================================================== */
static nxinput_exit_chord exit_chord;

/* ===== Init ============================================================== */
int fp2_input_preinit(void)
{
    /* Fronteira pré-init do nxinput 0.10.0: owner/default + FACE_LAYOUT lidos
     * UMA vez, antes de bundle, staging e de qualquer SDL_Init. */
    return fp2_gptk_preinit(fp2_gamedir) != 0 ? -1 : 0;
}

int fp2_input_init(void)
{
    /* Diagnóstico de bancada por env, inerte sem ela (também no release). */
#ifdef FP2_BENCH_PROBES
    input_diag = getenv("FP2_INPUT_DIAG") != NULL; /* só na bancada; a release nunca sonda */
#endif
    /* Disparo no PRIMEIRO quadro em que SELECT e START estão ambos lógicos
     * (regra #40: chord sem hold/atraso); nada do chord vaza ao jogo. */
    nxinput_exit_chord_init(&exit_chord, 1);
#ifdef FP2_BENCH_PROBES
    vpad_enabled = getenv("FP2_VPAD") && strcmp(getenv("FP2_VPAD"), "0") != 0;
    if (getenv("FP2_VPAD_FILE") && *getenv("FP2_VPAD_FILE"))
        vpad_file = getenv("FP2_VPAD_FILE");
#endif
    *(void **)&fp2_sdl_joy_vendor = optional_sdl("SDL_JoystickGetVendor");
    *(void **)&fp2_sdl_joy_product = optional_sdl("SDL_JoystickGetProduct");
    if (!fp2_gptk_loaded()) {
        /* Sem mapa válido não existe caminho de palpite: fail-closed. */
        fprintf(stderr, "[fp2/input] NEXTOSCONTROLLERS ausente/inválido; "
                        "abortando (fail-closed)\n");
        return -1;
    }
    if (fp2_stage_seam_before_init() != 0)
        return -1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[fp2/input] SDL controller init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    if (padset_setup() != 0)
        return -1;
    open_controller();
    if (fp2_register_sinks() != 0) {
        fprintf(stderr, "[fp2/input] runtime vivo não selado; abortando "
                        "(fail-closed)\n");
        return -1;
    }
    fprintf(stderr,
            "[fp2/input] layout: gamepad nativo + GPTK vivo; "
            "chord=SELECT+START(sovereign); cursor=none; vpad=%s\n",
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
        int control = fp2_control_of(i);
        if (control >= 0 && buttons[i])
            control_down[control] = 1;
    }
    trigger_value[0] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    trigger_value[1] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    for (int t = 0; t < 2; t++) {
        int was = trigger_digital[t];
        trigger_digital[t] = was ? trigger_value[t] > FP2_TRIGGER_EXIT
                                 : trigger_value[t] > FP2_TRIGGER_ENTER;
    }
    control_down[NXINPUT_GPTK_L2] = trigger_digital[0];
    control_down[NXINPUT_GPTK_R2] = trigger_digital[1];
    if (vpad_enabled)
        for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++)
            if (vpad_frames[c] > 0)
                control_down[c] = 1;
}

void fp2_input_poll(void *env, void *player, unsigned long frame)
{
    input_last_env = env;
    input_last_player = player;
    move_vector_this_frame = 0;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_controller();
        if (event.type == SDL_JOYDEVICEREMOVED && fp2_seam_adopted)
            nxc6_forget((int)event.jdevice.which);
        if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
            nxinput_padset_remove_instance(&padset, event.cdevice.which)) {
            controller = nxinput_padset_first(&padset);
            fp2_gptk_release_all("controller-removed");
            release_all_keys();
            open_controller();
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            fp2_gptk_release_all("focus-lost");
            release_all_keys();
        }
    }
    vpad_poll();
    sample_controls();
    update_engine_context(frame);

    if (!controller && !vpad_enabled) {
        fp2_gptk_release_all("controller-unavailable");
        release_all_keys();
        return;
    }

    /* Chord soberano SELECT+START: fronteira do framework, lida ANTES do GPTK
     * e fora do arquivo do dono.  Só SELECT e START lógicos; GUIDE, L1+R1,
     * L2+R2 ou qualquer outra combinação jamais encerram. */
    /* Chord SELECT+START pelo nxinput: só quando UM instance segura os dois
     * (nxinput_padset); SELECT num pad + START noutro nunca encerra. */
    int chord_select = 0, chord_start = 0;
    nxinput_padset_chord_inputs(&padset, &chord_select, &chord_start);
    if (nxinput_exit_chord_fold_signal(&exit_chord, &signal_exit) ||
        nxinput_exit_chord_update(&exit_chord, chord_select, chord_start)) {
        (void)nxinput_exit_chord_consume(&exit_chord);
        fprintf(stderr, "[fp2/input] SELECT+START: lifecycle exit requested\n");
        exit_requested = 1;
        fp2_gptk_release_all("exit-chord");
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
        if (fp2_gptk_feed_button(c, control_down[c], value) ==
            FP2_GPTK_LIVE_FATAL)
            input_fatal = 1;
    }

    /* ===== Vetores ===== */
    float lx = stick_axis(0), ly = stick_axis(1);
    float rx = stick_axis(2), ry = stick_axis(3);
    radial_deadzone(&lx, &ly);
    radial_deadzone(&rx, &ry);
    int left_consumed = fp2_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK);
    int right_consumed = fp2_gptk_should_consume(NXINPUT_GPTK_RIGHT_STICK);
    if (left_consumed &&
        fp2_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, lx, ly) ==
            FP2_GPTK_LIVE_FATAL)
        input_fatal = 1;
    if (right_consumed &&
        fp2_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, rx, ry) ==
            FP2_GPTK_LIVE_FATAL)
        input_fatal = 1;

    if (input_fatal) {
        fprintf(stderr, "[fp2/input] FATAL no runtime vivo: encerrando sem "
                        "reproduzir nativamente\n");
        exit_requested = 1;
        release_all_keys();
        return;
    }

    /* ===== Passthrough nativo dirigido por ESTADO ===== */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        int keycode = fp2_native_keycode(c);
        if (!keycode)
            continue;
        int desired = control_down[c] && !fp2_gptk_should_consume(c);
        if (!desired && sink_key_pressed[keycode])
            continue;
        deliver_key(keycode, desired);
    }

    /* ===== MotionEvent do quadro: X/Y (esquerdo), Z/RZ (direito), gatilhos
     * e HAT (D-pad nativo, como antes).  Fontes por eixo, sem dupla entrega:
     *   X/Y   <- fp2.move (sink) OU LEFT_STICK native
     *   Z/RZ  <- RIGHT_STICK native
     *   L/RTRIGGER <- L2/R2 native (junto do KEYCODE_BUTTON_L2/R2 nativo)
     *   HAT   <- D-pad native */
    float ax = 0.0f, ay = 0.0f, az = 0.0f, arz = 0.0f, lt = 0.0f, rt = 0.0f;
    if (move_vector_this_frame) {
        ax = move_axis_x;
        ay = move_axis_y;
    } else if (!left_consumed) {
        ax = lx;
        ay = ly;
    }
    if (!right_consumed) {
        az = rx;
        arz = ry;
    }
    if (!fp2_gptk_should_consume(NXINPUT_GPTK_L2))
        lt = trigger_value[0];
    if (!fp2_gptk_should_consume(NXINPUT_GPTK_R2))
        rt = trigger_value[1];
    int up = control_down[NXINPUT_GPTK_UP] &&
             !fp2_gptk_should_consume(NXINPUT_GPTK_UP);
    int dn = control_down[NXINPUT_GPTK_DOWN] &&
             !fp2_gptk_should_consume(NXINPUT_GPTK_DOWN);
    int lf = control_down[NXINPUT_GPTK_LEFT] &&
             !fp2_gptk_should_consume(NXINPUT_GPTK_LEFT);
    int rg = control_down[NXINPUT_GPTK_RIGHT] &&
             !fp2_gptk_should_consume(NXINPUT_GPTK_RIGHT);
    float hx = (float)(rg - lf);
    float hy = (float)(dn - up);
    inject(env, player, fp2_jni_motion_event(ax, ay, az, arz, lt, rt, hx, hy));

#ifdef FP2_BENCH_PROBES
    fp2_bench_probes(frame);
#endif
    if (input_diag && frame > 0 && frame % 300 == 0)
        fprintf(stderr,
                "[fp2/input] diag ctx=%d src=%s scene=%s deliveries=%lu\n",
                fp2_gptk_context(), fp2_gptk_context_source(), scene_name,
                fp2_gptk_delivery_count());
}

void fp2_input_close(void)
{
    fp2_gptk_release_all("shutdown");
    release_all_keys();
    close_controller();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
                      SDL_INIT_EVENTS);
    input_last_env = NULL;
    input_last_player = NULL;
}

int fp2_input_exit_requested(void)
{
    return exit_requested;
}

int fp2_input_fatal(void)
{
    return input_fatal;
}

/* Sem cursor neste port: o jogo é plataforma de console e navega nativamente. */
int fp2_input_cursor(float *x, float *y)
{
    (void)x;
    (void)y;
    return 0;
}

void fp2_input_set_screen_size(int width, int height)
{
    (void)width;
    (void)height;
}

void fp2_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void fp2_input_keyboard_set(const char *text)
{
    (void)text;
}

void fp2_input_keyboard_hide(void)
{
}

int fp2_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const fp2_keyboard_key **keys,
                                size_t *key_count)
{
    if (text && text_size) text[0] = '\0';
    if (uppercase) *uppercase = 0;
    if (selected) *selected = 0;
    if (keys) *keys = NULL;
    if (key_count) *key_count = 0;
    return 0;
}
