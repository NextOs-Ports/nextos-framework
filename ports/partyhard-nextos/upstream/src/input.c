/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Party Hard GO 0.100038 (universal 1.0.1) — controle NextOS -> entrada
 * Android/Unity.
 *
 * Arquitetura (V5, nxinput 0.11.8):
 *
 *   SDL2 DO FIRMWARE (mapping soberano do CFW/PortMaster, admitido in-process
 *   pela costura C6 com o bundle pinado da autoridade 3)
 *        |
 *        v
 *   controles simbólicos (A, B, ..., LEFT_STICK, RIGHT_STICK), união de todos
 *   os pads admitidos (nxinput_padset)
 *        |
 *        +--> SELECT+START: chord soberano do framework (nxinput_exit_chord),
 *        |    só no MESMO instance, consultado ANTES do GPTK
 *        |
 *        v
 *   NEXTOSCONTROLLERS.gptk vivo (owner/default, lido no pré-init) decide
 *   action / null / native por controle e por contexto PROVADO pela engine
 *   (App.View.Gui._screenType ESTATICO via IL2CPP: GameScreen = gameplay,
 *   GamePauseScreen/janelas = menu; MainMenu tambem contem a partida e nao
 *   prova o contexto. GUI ainda nao inicializada = passthrough)
 *        |
 *        +--> ACTION  -> runtime vivo -> sink real deste adapter: o MESMO
 *        |               KeyEvent/MotionEvent Android que o InControl do
 *        |               jogo lê, ou o ponteiro do port (seta + toque)
 *        +--> null    -> nada, em nenhum caminho
 *        +--> native  -> passthrough: exatamente o comportamento APROVADO
 *                        deste port (KeyEvent/MotionEvent de gamepad; o
 *                        analógico DIREITO move a seta e o R3 clica)
 *
 * O que foi PRESERVADO do port aprovado no Mali-450 (27/08/2026): o ponteiro
 * nasce visível e some quando parado depois do 1º clique; analógico esquerdo
 * anda, direito é a seta, R3 clica (o A é ação, nunca clique); START pausa
 * pelo KEYCODE_BUTTON_START e, com o jogo pausado, fecha o pause pelo BACK do
 * Android; a caixa "SELECT CONTROLS TYPE" é respondida pelo ponteiro; a
 * identidade apresentada à Unity segue "Microsoft X-Box 360 pad" (é ela que
 * escolhe o perfil do InControl — trocar o nome REMAPEIA botões, medido).
 *
 * O que SAIU (autoridades falsas, C3): joystick cru posicional, varredura
 * própria de /dev/input por TRIGGER_HAPPY, heurística de VID/PID para trocar
 * A/B, chord por GUIDE. O caminho nativo é dirigido por ESTADO: todo KeyEvent
 * DOWN recebe o seu UP mesmo quando o controle muda de dono no meio.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gb.h"
#include "il2.h"
#include "nx_elf.h"
#include "nxinput_sdl_seam.h"
#include "nxc6_glue.h"
#include "nxinput_gptk.h"
#include "nxinput_exit_chord.h"
#include "input_gptk.h"
#include "input_guard.h"
#include "tutorial_return.h"
#include "nxinput_padset.h"

/* ===== Constantes Android consumidas pela Unity/InControl =============== */
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
#define AKEY_BUTTON_L2 104
#define AKEY_BUTTON_R2 105
#define AKEY_BUTTON_THUMBL 106
#define AKEY_BUTTON_THUMBR 107
#define AKEY_BUTTON_START 108
#define AKEY_BUTTON_SELECT 109
#define AKEY_BUTTON_MODE 110

#define ST_STICK_DEADZONE 0.15f /* radial, com reescala (vetor do GPTK) */
#define ST_TRIGGER_ENTER NXINPUT_GPTK_TRIGGER_ENTER
#define ST_TRIGGER_EXIT NXINPUT_GPTK_TRIGGER_EXIT
#define ST_SCENE_SAMPLE_FRAMES 30

/* Vários pads admitidos ao mesmo tempo: o estado simbólico é a união deles;
 * o chord SELECT+START só vale no MESMO pad (instance). `controller` segue
 * apontando para o primeiro pad admitido (identidade JNI). */
static nxinput_padset padset;
static nxinput_padset_sdl padset_sdl;
static SDL_GameController *controller;
static uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX];
static volatile sig_atomic_t exit_requested;
static volatile sig_atomic_t signal_exit;
static int input_fatal;
static int input_diag; /* só na bancada (-DST_BENCH_PROBES + env) */
#ifdef ST_BENCH_PROBES
static int input_engine_probe;
static float probe_sdl_x, probe_sdl_y;
static float probe_android_x, probe_android_y;
static float probe_hat_x, probe_hat_y;
static int probe_dpad_up, probe_dpad_down, probe_dpad_left, probe_dpad_right;
/* Faults injected underneath the guard, emulating a cached SDL state whose
 * physical release/centering event was lost. Private bench binary only. */
static int bench_stale_button = -1;
static int bench_stale_axis = -1;
#endif
static void *input_last_env;
static void *input_last_player;
static int touch_origin_x;
static int touch_origin_y;
static int touch_width = 1280;
static int touch_height = 720;

/* SIGTERM/SIGINT convergem no mesmo shutdown do SELECT+START. */
void st_input_request_exit(void)
{
    signal_exit = 1;
    exit_requested = 1;
}

/* ===== Símbolos SDL acima do piso público 2.0.4: dlsym com fallback ===== */
static const char *(*st_sdl_path_for_index)(int);
static SDL_JoystickID (*st_sdl_instance_for_index)(int);
static Uint16 (*st_sdl_joy_vendor)(SDL_Joystick *);
static Uint16 (*st_sdl_joy_product)(SDL_Joystick *);

static void *optional_sdl(const char *name)
{
    (void)dlerror();
    return dlsym(RTLD_DEFAULT, name);
}

/* ===== Snapshot neutro do no exato admitido ============================
 *
 * Alguns CFWs RK3326 perdem raramente uma transicao de release na fila que
 * alimenta a SDL. A fila passa a dizer "pressionado" para sempre, embora o
 * snapshot atual do kernel ja esteja neutro. A protecao abaixo NAO mapeia
 * controles, nao varre /dev/input e nunca fabrica um estado pressionado:
 * associa somente o no de evento fornecido pela propria SDL ao instance
 * admitido e usa o kernel apenas para vereditos seguros de neutralidade:
 *
 *   - nenhum EV_KEY fisico esta pressionado -> um botao SDL, cuja propria
 *     bind e BUTTON, nao pode continuar pressionado;
 *   - o HAT indicado pela bind da SDL esta no centro -> o D-pad nao pode
 *     continuar pressionado;
 *   - o eixo fisico indicado pela bind AXIS da SDL esta no centro -> o stick
 *     esquerdo/direito nao pode continuar fora do centro.
 *
 * Qualquer caminho ausente, bind de outro tipo ou ioctl incerto falha aberto
 * para o comportamento SDL original. GPTK e a SDL continuam soberanos. */
#define ST_INPUT_GUARD_PATH_MAX 96
typedef struct st_input_guard {
    SDL_GameController *controller;
    SDL_JoystickID instance;
    int fd;
    int key_available;
    int key_snapshot_valid;
    int key_idle;
    uint8_t key_backed[SDL_CONTROLLER_BUTTON_MAX];
    int8_t button_hat[SDL_CONTROLLER_BUTTON_MAX];
    unsigned long key_capabilities[PH_INPUT_KEY_WORDS];
    unsigned long abs_capabilities[PH_INPUT_ABS_WORDS];
    uint8_t hat_supported[4];
    uint8_t hat_snapshot_valid[4];
    uint8_t hat_centered[4];
    int stick_abs_code[4];
    uint8_t stick_snapshot_valid[4];
    uint8_t stick_centered[4];
    int button_heal_logged;
    uint8_t axis_heal_logged[4];
    char path[ST_INPUT_GUARD_PATH_MAX];
} st_input_guard;

static st_input_guard input_guards[NXINPUT_PADSET_MAX];
static unsigned input_guard_count;

static int st_input_event_path(const char *path)
{
    /* The path is identity supplied by SDL, never a discovery prefix. Keep
     * it inside the kernel input subtree, reject traversal/control bytes,
     * then let O_NOFOLLOW + fstat(S_ISCHR) establish the object type. */
    static const char device_root[] = "/dev/input/";
    if (!path || strncmp(path, device_root, sizeof device_root - 1) != 0 ||
        strlen(path) >= ST_INPUT_GUARD_PATH_MAX)
        return 0;
    const char *suffix = path + sizeof device_root - 1;
    if (!*suffix || !strcmp(suffix, ".") || !strcmp(suffix, ".."))
        return 0;
    for (const char *p = suffix; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            return 0;
        if ((p == suffix || p[-1] == '/') && p[0] == '.' &&
            (p[1] == '/' || p[1] == '\0' ||
             (p[1] == '.' && (p[2] == '/' || p[2] == '\0'))))
            return 0;
    }
    return 1;
}

static st_input_guard *st_input_guard_for_controller(void *controller_ptr)
{
    for (unsigned i = 0; i < input_guard_count; i++)
        if (input_guards[i].controller == controller_ptr)
            return &input_guards[i];
    return NULL;
}

static void st_input_guard_drop(unsigned index)
{
    if (index >= input_guard_count)
        return;
    if (input_guards[index].fd >= 0)
        close(input_guards[index].fd);
    for (unsigned i = index; i + 1 < input_guard_count; i++)
        input_guards[i] = input_guards[i + 1];
    input_guard_count--;
    memset(&input_guards[input_guard_count], 0,
           sizeof input_guards[input_guard_count]);
    input_guards[input_guard_count].fd = -1;
}

static void st_input_guard_remove(SDL_JoystickID instance)
{
    for (unsigned i = 0; i < input_guard_count; i++) {
        if (input_guards[i].instance == instance) {
            st_input_guard_drop(i);
            return;
        }
    }
}

static void st_input_guard_close_all(void)
{
    while (input_guard_count)
        st_input_guard_drop(input_guard_count - 1);
}

static void st_input_guard_attach(int sdl_index,
                                  SDL_GameController *opened,
                                  SDL_JoystickID instance)
{
    const char *path = st_sdl_path_for_index
                     ? st_sdl_path_for_index(sdl_index) : NULL;
    if (!opened || input_guard_count >= NXINPUT_PADSET_MAX ||
        !st_input_event_path(path) ||
        !st_sdl_instance_for_index ||
        st_sdl_instance_for_index(sdl_index) != instance) {
        fprintf(stderr, "[st/input] release guard: instance=%d unavailable; "
                        "SDL passthrough preserved\n", (int)instance);
        return;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISCHR(info.st_mode)) {
        if (fd >= 0)
            close(fd);
        fprintf(stderr, "[st/input] release guard: exact node rejected for "
                        "instance=%d; SDL passthrough preserved\n",
                (int)instance);
        return;
    }

    st_input_guard *guard = &input_guards[input_guard_count];
    memset(guard, 0, sizeof *guard);
    guard->fd = fd;
    guard->controller = opened;
    guard->instance = instance;
    snprintf(guard->path, sizeof guard->path, "%s", path);
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        guard->button_hat[b] = -1;
    for (int a = 0; a < 4; a++)
        guard->stick_abs_code[a] = -1;

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof guard->key_capabilities),
              guard->key_capabilities) >= 0) {
        for (size_t word = 0; word < PH_INPUT_KEY_WORDS; word++) {
            if (guard->key_capabilities[word]) {
                guard->key_available = 1;
                break;
            }
        }
    }
    int guarded_hats = 0;
    int guarded_axes = 0;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof guard->abs_capabilities),
              guard->abs_capabilities) >= 0) {
        for (int h = 0; h < 4; h++) {
            int xcode = ABS_HAT0X + h * 2;
            int ycode = xcode + 1;
            if (ph_input_test_bit(guard->abs_capabilities,
                                  PH_INPUT_ABS_WORDS, xcode) &&
                ph_input_test_bit(guard->abs_capabilities,
                                  PH_INPUT_ABS_WORDS, ycode)) {
                guard->hat_supported[h] = 1;
                guarded_hats++;
            }
        }
        for (int a = 0; a < 4; a++) {
            SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForAxis(
                opened, (SDL_GameControllerAxis)a);
            if (bind.bindType != SDL_CONTROLLER_BINDTYPE_AXIS)
                continue;
            int code = ph_input_abs_code_for_sdl_axis(
                guard->abs_capabilities, PH_INPUT_ABS_WORDS,
                bind.value.axis);
            if (code >= 0) {
                guard->stick_abs_code[a] = code;
                guarded_axes++;
            }
        }
    }

    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(
            opened, (SDL_GameControllerButton)b);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON &&
            guard->key_available) {
            guard->key_backed[b] = 1;
        } else if (bind.bindType == SDL_CONTROLLER_BINDTYPE_HAT &&
                   bind.value.hat.hat >= 0 && bind.value.hat.hat < 4 &&
                   guard->hat_supported[bind.value.hat.hat]) {
            guard->button_hat[b] = (int8_t)bind.value.hat.hat;
        }
    }

    if (!guard->key_available && !guarded_hats && !guarded_axes) {
        close(fd);
        memset(guard, 0, sizeof *guard);
        guard->fd = -1;
        fprintf(stderr, "[st/input] release guard: no safe snapshot for "
                        "instance=%d; SDL passthrough preserved\n",
                (int)instance);
        return;
    }
    input_guard_count++;
    fprintf(stderr,
            "[st/input] release guard: instance=%d exact=%s keys=%s "
            "hats=%d centered_axes=%d policy=neutral-only\n",
            (int)instance, guard->path,
            guard->key_available ? "ready" : "unavailable",
            guarded_hats, guarded_axes);
}

static void st_input_guard_snapshot_all(void)
{
    for (unsigned i = 0; i < input_guard_count; i++) {
        st_input_guard *guard = &input_guards[i];
        if (guard->key_available) {
            unsigned long state[PH_INPUT_KEY_WORDS];
            memset(state, 0, sizeof state);
            if (ioctl(guard->fd, EVIOCGKEY(sizeof state), state) >= 0) {
                guard->key_snapshot_valid = 1;
                guard->key_idle = !ph_input_any_key_pressed(
                    state, guard->key_capabilities, PH_INPUT_KEY_WORDS);
                if (!guard->key_idle)
                    guard->button_heal_logged = 0;
            } else {
                guard->key_snapshot_valid = 0;
                guard->key_available = 0;
                fprintf(stderr, "[st/input] release guard: key snapshot failed "
                                "for instance=%d; SDL passthrough restored\n",
                        (int)guard->instance);
            }
        }
        for (int h = 0; h < 4; h++) {
            if (!guard->hat_supported[h])
                continue;
            struct input_absinfo xinfo, yinfo;
            int xcode = ABS_HAT0X + h * 2;
            int ycode = xcode + 1;
            if (ioctl(guard->fd, EVIOCGABS(xcode), &xinfo) >= 0 &&
                ioctl(guard->fd, EVIOCGABS(ycode), &yinfo) >= 0) {
                guard->hat_snapshot_valid[h] = 1;
                guard->hat_centered[h] =
                    ph_input_hat_axis_is_centered(
                        xinfo.value, xinfo.minimum, xinfo.maximum) &&
                    ph_input_hat_axis_is_centered(
                        yinfo.value, yinfo.minimum, yinfo.maximum);
                if (!guard->hat_centered[h])
                    guard->button_heal_logged = 0;
            } else {
                guard->hat_snapshot_valid[h] = 0;
                guard->hat_supported[h] = 0;
                fprintf(stderr, "[st/input] release guard: hat snapshot failed "
                                "for instance=%d hat=%d; SDL passthrough restored\n",
                        (int)guard->instance, h);
            }
        }
        for (int a = 0; a < 4; a++) {
            int code = guard->stick_abs_code[a];
            if (code < 0)
                continue;
            struct input_absinfo absinfo;
            if (ioctl(guard->fd, EVIOCGABS(code), &absinfo) >= 0) {
                guard->stick_snapshot_valid[a] = 1;
                guard->stick_centered[a] = ph_input_axis_is_centered(
                    absinfo.value, absinfo.minimum, absinfo.maximum,
                    absinfo.flat, absinfo.fuzz);
                if (!guard->stick_centered[a])
                    guard->axis_heal_logged[a] = 0;
            } else {
                guard->stick_snapshot_valid[a] = 0;
                guard->stick_abs_code[a] = -1;
                fprintf(stderr, "[st/input] release guard: axis snapshot failed "
                                "for instance=%d axis=%d; SDL passthrough restored\n",
                        (int)guard->instance, a);
            }
        }
    }
}

/* ===== Admissão canônica do controle (nxinput C6) ======================= */
static char st_staged_mapping[NXINPUT_AUTHORITY_SOURCE_MAX];
static int st_seam_adopted;

static int st_stage_seam_before_init(void)
{
    size_t staged_len = 0;
    int rc;

    if (!getenv("NXC6_SEAM")) {
        fprintf(stderr, "[st/input] NXC6 seam: not adopted for this run "
                        "(NXC6_SEAM absent); stock SDL behaviour\n");
        return 0;
    }
    if (!st_sdl_path_for_index || !st_sdl_instance_for_index) {
        fprintf(stderr, "[st/input] NXC6 seam: this SDL cannot name the "
                        "device node (pre-2.24); staying native\n");
        return 0;
    }
    /* V5 mede a SDL realmente carregada.  Provider conhecido recebe a
     * tradução tipada; provider desconhecido deixa o mapping no ambiente
     * para a importação stock do CFW, sem silenciar o controle (muOS). */
    rc = nxc6_stage_before_init(st_staged_mapping, sizeof st_staged_mapping,
                                &staged_len);
    if (rc < 0) {
        fprintf(stderr, "[st/input] NXC6 seam: staging failed before the "
                        "joystick init (rc=%d); refusing to guess\n", rc);
        return -1;
    }
    st_seam_adopted = 1;
    {
        int declared = nxc6_declare_port_bundle_for_layout(
            st_gamedir, st_gptk_face_layout());
        fprintf(stderr, "[st/input] NXC6 seam: port bundle %s (layout=%s)\n",
                declared > 0 ? getenv("NXCONTROLLER_PROFILES")
                : declared == 0 ? "(none shipped)"
                : "(declaration failed)",
                nxinput_gptk_face_layout_name(st_gptk_face_layout()));
    }
    fprintf(stderr,
            "[st/input] NXC6 seam: result=%s staged=%zu bytes env_still_set=%d "
            "receipt=%s\n",
            rc == 1 ? "left-for-stock" : "staged", staged_len,
            getenv("SDL_GAMECONTROLLERCONFIG") != NULL,
            getenv("NXC6_RECEIPT") ? getenv("NXC6_RECEIPT") : "(none)");
    return 0;
}

/* ===== Contexto provado pela engine: cena ativa por NOME (IL2CPP) =======
 * Lido no máximo a cada ST_SCENE_SAMPLE_FRAMES quadros, só depois do frame 0
 * (entrar no domínio antes do primeiro nativeRender mata a Unity — medido no
 * bring-up deste port). Classes/métodos resolvidos uma vez e cacheados. */
static int scene_api_state;                  /* 0 = não tentado, 1 ok, -1 falhou */
static const MethodInfo *scene_get_active;
static const MethodInfo *scene_get_name;
static const MethodInfo *scene_count_get;
static const MethodInfo *scene_get_at;
static const MethodInfo *scene_is_loaded;
static const MethodInfo *time_get_timescale;
static char scene_name[96];
static int scene_is_loading;
static int scene_is_menu;
static int scene_paused;

/* As cenas do BuildSettings DESTE pacote (lidas do globalgamemanagers do
 * data.unity3d, não presumidas): tudo o que é menu/moldura. As festas vivem
 * no datapack (level1, level2) e chegam com outro nome. */
static const char *const MENU_SCENES[] = {
    "AndroidLicensePermissionResolver", "MainMenu", "cutscene",
};

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
    scene_count_get = manager ? il2_method(manager, "get_sceneCount", 0) : NULL;
    scene_get_at = manager ? il2_method(manager, "GetSceneAt", 1) : NULL;
    scene_get_name = scene ? il2_method(scene, "get_name", 0) : NULL;
    scene_is_loaded = scene ? il2_method(scene, "get_isLoaded", 0) : NULL;
    time_get_timescale = time ? il2_method(time, "get_timeScale", 0) : NULL;
    scene_api_state = scene_get_active && scene_get_name ? 1 : -1;
    fprintf(stderr, "[st/input] scene contract: %s (timescale=%s additive=%s)\n",
            scene_api_state > 0 ? "ready" : "unavailable",
            time_get_timescale ? "ready" : "unavailable",
            scene_count_get && scene_get_at ? "ready" : "unavailable");
    return scene_api_state > 0;
}

static int scene_timescale_zero(void)
{
    if (!time_get_timescale)
        return 0;
    Il2CppObject *r = il2_call(time_get_timescale, NULL, NULL,
                               "Time.get_timeScale");
    void *v = il2_unbox(r);
    return v && *(float *)v < 0.001f;
}

static int scene_name_is_menu(const char *name)
{
    for (size_t i = 0; i < sizeof MENU_SCENES / sizeof *MENU_SCENES; i++)
        if (strcmp(name, MENU_SCENES[i]) == 0)
            return 1;
    return 0;
}

/* Nome de uma Scene (struct) ja' desembrulhada; vazio quando nao ha nome. */
static void scene_struct_name(void *raw, char *out, size_t size)
{
    out[0] = 0;
    if (!raw)
        return;
    Il2CppObject *name = il2_call(scene_get_name, raw, NULL, "Scene.get_name");
    if (!name)
        return;
    il2_str_utf8(name, out, size);
    if (strcmp(out, "(null)") == 0)
        out[0] = 0;
}

/* MEDIDO (03/09/2026): este jogo carrega a festa ADITIVAMENTE — a cena ativa
 * continua "MainMenu" com o nivel tocando.  A prova de gameplay olha entao
 * TODAS as cenas carregadas: qualquer cena que nao seja uma das do
 * BuildSettings (as de moldura) e' uma festa vinda do datapack. */
static void sample_scene(void)
{
    if (!scene_api_resolve())
        return;
    Il2CppObject *boxed = il2_call(scene_get_active, NULL, NULL,
                                   "GetActiveScene");
    void *raw = il2_unbox(boxed);
    char text[96] = "";
    scene_struct_name(raw, text, sizeof text);
    if (strcmp(text, scene_name) != 0) {
        snprintf(scene_name, sizeof scene_name, "%s", text);
        fprintf(stderr, "[st/input] active scene=\"%s\"\n", scene_name);
    }
    scene_is_loading = scene_name[0] == 0;
    scene_is_menu = 0;
    scene_paused = 0;
    if (scene_is_loading)
        return;
    int level_loaded = !scene_name_is_menu(scene_name);
    if (!level_loaded && scene_count_get && scene_get_at) {
        Il2CppObject *cnt = il2_call(scene_count_get, NULL, NULL, "sceneCount");
        void *cv = il2_unbox(cnt);
        int32_t count = cv ? *(int32_t *)cv : 0;
        if (count > 16) count = 16;
        for (int32_t i = 0; i < count && !level_loaded; i++) {
            int32_t index = i;
            void *args[1] = { &index };
            Il2CppObject *sb = il2_call(scene_get_at, NULL, args, "GetSceneAt");
            void *sraw = il2_unbox(sb);
            char other[96] = "";
            scene_struct_name(sraw, other, sizeof other);
            if (!other[0] || scene_name_is_menu(other))
                continue;
            if (scene_is_loaded) {
                Il2CppObject *lb = il2_call(scene_is_loaded, sraw, NULL, "isLoaded");
                void *lv = il2_unbox(lb);
                if (!lv || !*(uint8_t *)lv)
                    continue;
            }
            static char last_level[96];
            if (strcmp(other, last_level) != 0) {
                snprintf(last_level, sizeof last_level, "%s", other);
                fprintf(stderr, "[st/input] loaded level scene=\"%s\"\n", other);
            }
            level_loaded = 1;
        }
    }
    scene_is_menu = !level_loaded;
    if (!scene_is_menu)
        scene_paused = scene_timescale_zero();
}

/* ===== Current screen: the game's STATIC App.View.Gui contract =========
 * _screenType is static in the authenticated Party Hard GO metadata.
 * Gui has no instance. MainMenu contains gameplay and is not a substitute
 * for this state. Read fields by metadata, never by a hard-coded RVA.
 */
#define FIELD_ATTRIBUTE_STATIC 0x0010
static int gui_api_state;
static FieldInfo *gui_screen_field, *gui_container_field;
static FieldInfo *gui_controls_popup_field, *gui_popup_visible_field;
static int gui_controls_popup_visible;
static const MethodInfo *object_alive_method;
static int32_t gui_game_screen = -1, gui_pause_screen = -1;
static int gui_screen_value = -1;

static int gui_api_resolve(void)
{
    if (gui_api_state)
        return gui_api_state > 0;
    if (!il2_load())
        return 0;
    gui_api_state = -1;
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    if (!mod)
        return 0;
    void *(*class_get_fields)(void *, void **) = NULL;
    const char *(*field_get_name)(void *) = NULL;
    int (*field_get_flags)(void *) = NULL;
    const void *(*field_get_type)(void *) = NULL;
    void *(*class_from_type)(const void *) = NULL;
    *(void **)&class_get_fields = nx_lookup_in(mod, "il2cpp_class_get_fields");
    *(void **)&field_get_name = nx_lookup_in(mod, "il2cpp_field_get_name");
    *(void **)&field_get_flags = nx_lookup_in(mod, "il2cpp_field_get_flags");
    *(void **)&field_get_type = nx_lookup_in(mod, "il2cpp_field_get_type");
    *(void **)&class_from_type = nx_lookup_in(mod, "il2cpp_class_from_il2cpp_type");
    if (!class_get_fields || !field_get_name || !field_get_flags ||
        !field_get_type || !class_from_type)
        return 0;

    Il2CppClass *gui = il2_class("App.View", "Gui");
    Il2CppClass *object = il2_class("UnityEngine", "Object");
    Il2CppClass *visual = il2_class("App.View", "VisualComponent");
    object_alive_method = object ? il2_method(object, "op_Implicit", 1) : NULL;
    gui_screen_field = gui ? il2_field(gui, "_screenType") : NULL;
    gui_container_field = gui ? il2_field(gui, "_screenContainer") : NULL;
    gui_controls_popup_field = gui ? il2_field(gui, "ControlsSelectorPopUp") : NULL;
    gui_popup_visible_field = visual ? il2_field(visual, "_isShow") : NULL;
    int storage_ok = gui_screen_field && gui_container_field &&
        gui_controls_popup_field && gui_popup_visible_field &&
        (field_get_flags(gui_screen_field) & FIELD_ATTRIBUTE_STATIC) &&
        (field_get_flags(gui_container_field) & FIELD_ATTRIBUTE_STATIC) &&
        (field_get_flags(gui_controls_popup_field) & FIELD_ATTRIBUTE_STATIC) &&
        !(field_get_flags(gui_popup_visible_field) & FIELD_ATTRIBUTE_STATIC);
    if (storage_ok) {
        void *enum_class = class_from_type(field_get_type(gui_screen_field));
        void *iter = NULL, *field;
        while (enum_class && (field = class_get_fields(enum_class, &iter))) {
            const char *name = field_get_name(field);
            if (!name || !(field_get_flags(field) & FIELD_ATTRIBUTE_STATIC))
                continue;
            int32_t value = -1;
            if (strcmp(name, "GameScreen") == 0) {
                il2_static_get(field, &value);
                gui_game_screen = value;
            } else if (strcmp(name, "GamePauseScreen") == 0) {
                il2_static_get(field, &value);
                gui_pause_screen = value;
            }
        }
    }
    int ok = storage_ok && object_alive_method &&
             gui_game_screen >= 0 && gui_pause_screen >= 0 &&
             gui_game_screen != gui_pause_screen;
    gui_api_state = ok ? 1 : -1;
    fprintf(stderr, "[st/input] gui contract: %s (storage=%s GameScreen=%d GamePauseScreen=%d)\n",
            ok ? "ready" : "unavailable", storage_ok ? "static" : "invalid",
            gui_game_screen, gui_pause_screen);
    return ok;
}

/* No stale screen survives a missing/destroyed UI container. */
static int sample_gui_screen(void)
{
    gui_screen_value = -1;
    gui_controls_popup_visible = 0;
    if (!gui_api_resolve())
        return -1;
    void *container = NULL;
    il2_static_get(gui_container_field, &container);
    if (!container)
        return -1;
    void *args[1] = { container };
    Il2CppObject *alive = il2_call(object_alive_method, NULL, args,
                                  "Object.op_Implicit(Gui._screenContainer)");
    void *value = il2_unbox(alive);
    if (!value || !*(uint8_t *)value)
        return -1;
    int32_t screen = -1;
    il2_static_get(gui_screen_field, &screen);
    /* The first-run selector overlays GameScreen without changing
     * _screenType. Its actual visibility, not the underlying scene, owns
     * the cursor until the user chooses TOUCH or VIRTUAL. */
    void *popup = NULL;
    il2_static_get(gui_controls_popup_field, &popup);
    if (popup) {
        uint8_t shown = 0;
        il2_field_get(popup, gui_popup_visible_field, &shown);
        gui_controls_popup_visible = shown != 0;
    }
    gui_screen_value = screen;
    static int previous_screen = -1;
    if (screen != previous_screen) {
        previous_screen = screen;
        fprintf(stderr, "[st/input] gui screen=%d\n", screen);
    }
    return screen;
}

static void update_engine_context(unsigned long frame)
{
    if (frame == 0) {
        st_gptk_clear_context("frame0");
        return;
    }
    if (frame % ST_SCENE_SAMPLE_FRAMES == 1)
        sample_scene();
    if (scene_api_state <= 0) {
        st_gptk_clear_context("scene-contract-unavailable");
        return;
    }
    if (scene_is_loading) {
        st_gptk_clear_context("scene:loading");
        return;
    }
    /* Static fields are cheap to sample every frame. A menu closing must
     * release its touch before the next gameplay input, not 30 frames later. */
    int screen = sample_gui_screen();
    if (gui_api_state > 0 && screen < 0) {
        /* The intro/license scenes have their own Unity UI (including
         * Skip), outside Gui. They are proven standalone menus. MainMenu
         * is different: it contains gameplay and requires the Gui state. */
        if (!strcmp(scene_name, "cutscene") ||
            !strcmp(scene_name, "AndroidLicensePermissionResolver"))
            st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "scene:standalone-menu");
        else
            st_gptk_clear_context("gui:not-ready");
        return;
    }
    if (gui_api_state > 0 && screen >= 0) {
        if (gui_controls_popup_visible) {
            st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:controls-selector");
            return;
        }
        if (screen == gui_game_screen) {
            if (scene_timescale_zero())
                st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:paused");
            else
                st_gptk_set_context(ST_GPTK_CONTEXT_GAMEPLAY, "gui:game-screen");
        } else if (screen == gui_pause_screen) {
            st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:pause-screen");
        } else {
            st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:screen");
        }
        return;
    }
    /* Sem a GUI, a cena so' sabe dizer "moldura": as festas nao sao cenas,
     * entao nenhuma cena prova gameplay — cena desconhecida fica passthrough. */
    if (scene_is_menu)
        st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "scene:menu");
    else
        st_gptk_clear_context("scene:unknown");
}

/* ===== Injeção Android -> Unity ========================================= */
static void inject(void *env, void *player, void *event)
{
    static void *native_inject;
    if (!native_inject)
        native_inject = st_jni_native("com/unity3d/player/UnityPlayer",
                                      "nativeInjectEvent");
    if (native_inject && event) {
        /* Unity 2022+ registra nativeInjectEvent(InputEvent, displayId).  A
         * Activity passa o display padrão (0); sem o 4º argumento o escalador
         * de toque trata lixo de registrador como índice de array. */
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (input_diag)
            fprintf(stderr, "[st/input] inject event=%p consumed=%d\n",
                    event, consumed);
    } else if (input_diag) {
        fprintf(stderr, "[st/input] inject SKIPPED inject=%p event=%p\n",
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
        fprintf(stderr, "[st/key] keycode=%d %s\n", keycode,
                down ? "down" : "up");
    inject(input_last_env, input_last_player,
           st_jni_key_event(down ? 0 : 1, keycode, keycode));
}

static void release_all_keys(void)
{
    for (int k = 1; k < 256; k++) {
        sink_key_pressed[k] = 0;
        if (key_down_state[k])
            deliver_key(k, 0);
    }
}

/* MotionEvent is level state. Publish an explicit all-zero level at every
 * lifecycle/input boundary so Unity cannot retain the last stick/HAT vector
 * while the controller is unavailable or the frame loop is leaving. */
static void deliver_neutral_motion(const char *reason)
{
    if (!input_last_env || !input_last_player)
        return;
    inject(input_last_env, input_last_player,
           st_jni_motion_event(0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 0.0f));
    if (input_diag)
        fprintf(stderr, "[st/input] neutral motion reason=%s\n",
                reason ? reason : "boundary");
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

/* ===== IL2CPP direto (popup e pause) — código APROVADO, intocado ========
 * Resolver os EXPORTS do il2cpp e' so' lookup de simbolo.  Chamado tarde
 * (gameplay ja' rodando), nunca antes do frame 0. */
typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef void *(*il2cpp_class_get_method_from_name_fn)(void *, const char *, int);
typedef void *(*il2cpp_runtime_invoke_fn)(void *, void *, void **, void **);
typedef void *(*il2cpp_class_get_type_fn)(void *);
typedef void *(*il2cpp_type_get_object_fn)(void *);
typedef void *(*il2cpp_object_unbox_fn)(void *);
typedef void *(*il2cpp_object_get_class_fn)(void *);
typedef const char *(*il2cpp_class_get_name_fn)(void *);
typedef void (*il2cpp_runtime_class_init_fn)(void *);
typedef void (*il2cpp_gc_collect_fn)(int);
typedef int64_t (*il2cpp_gc_size_fn)(void);
typedef int (*il2cpp_gc_flag_fn)(void);

static il2cpp_domain_get_fn il2cpp_domain_get_p;
static il2cpp_domain_get_assemblies_fn il2cpp_domain_get_assemblies_p;
static il2cpp_assembly_get_image_fn il2cpp_assembly_get_image_p;
static il2cpp_class_from_name_fn il2cpp_class_from_name_p;
static il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name_p;
static il2cpp_runtime_invoke_fn il2cpp_runtime_invoke_p;
static il2cpp_class_get_type_fn il2cpp_class_get_type_p;
static il2cpp_type_get_object_fn il2cpp_type_get_object_p;
static il2cpp_object_unbox_fn il2cpp_object_unbox_p;
static il2cpp_object_get_class_fn il2cpp_object_get_class_p;
static il2cpp_class_get_name_fn il2cpp_class_get_name_p;
static il2cpp_runtime_class_init_fn il2cpp_runtime_class_init_p;
static il2cpp_gc_collect_fn il2cpp_gc_collect_p;
static il2cpp_gc_size_fn il2cpp_gc_get_used_size_p;
static il2cpp_gc_size_fn il2cpp_gc_get_heap_size_p;
static il2cpp_gc_flag_fn il2cpp_gc_is_disabled_p;
static il2cpp_gc_flag_fn il2cpp_gc_is_incremental_p;

/* As sondas da busca do popup sao de bring-up: ligam com ST_POPUP_DIAG=1 e
 * ficam caladas num lancamento normal. */
static int popup_diag(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ST_POPUP_DIAG");
        on = v && *v && *v != '0';
    }
    return on;
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

static int resolve_il2cpp_invoke_api(void)
{
    static int tried;
    if (il2cpp_runtime_invoke_p && il2cpp_class_get_method_from_name_p &&
        il2cpp_class_get_type_p && il2cpp_type_get_object_p &&
        il2cpp_domain_get_p)
        return 1;
    if (tried)
        return 0;
    tried = 1;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;
    if (!il2cpp_domain_get_p)
        il2cpp_domain_get_p = (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get");
    if (!il2cpp_domain_get_assemblies_p)
        il2cpp_domain_get_assemblies_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    if (!il2cpp_assembly_get_image_p)
        il2cpp_assembly_get_image_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    if (!il2cpp_class_from_name_p)
        il2cpp_class_from_name_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    il2cpp_class_get_method_from_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    il2cpp_runtime_invoke_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    il2cpp_class_get_type_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_type");
    il2cpp_type_get_object_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_type_get_object");
    il2cpp_object_unbox_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_object_unbox");
    il2cpp_object_get_class_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_object_get_class");
    il2cpp_class_get_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    il2cpp_runtime_class_init_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_runtime_class_init");
    il2cpp_gc_collect_p = (void *)nx_lookup_in(il2cpp, "il2cpp_gc_collect");
    il2cpp_gc_get_used_size_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_get_used_size");
    il2cpp_gc_get_heap_size_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_get_heap_size");
    il2cpp_gc_is_disabled_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_is_disabled");
    il2cpp_gc_is_incremental_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_is_incremental");
    return il2cpp_runtime_invoke_p && il2cpp_class_get_method_from_name_p &&
           il2cpp_class_get_type_p && il2cpp_type_get_object_p &&
           il2cpp_domain_get_p && il2cpp_domain_get_assemblies_p &&
           il2cpp_assembly_get_image_p && il2cpp_class_from_name_p;
}

/* Oraculo de pause: Time.timeScale == 0 quando o jogo esta' pausado.  Devolve
 * 1 pausado, 0 rodando, -1 quando a API nao esta' disponivel (dai vale a
 * paridade de START). */
static int st_pause_is_open(void)
{
    if (!resolve_il2cpp_invoke_api() || !il2cpp_object_unbox_p)
        return -1;
    void *time_class = find_managed_class("UnityEngine", "Time");
    if (!time_class)
        return -1;
    void *get_scale =
        il2cpp_class_get_method_from_name_p(time_class, "get_timeScale", 0);
    if (!get_scale)
        return -1;
    void *exc = NULL;
    void *boxed = il2cpp_runtime_invoke_p(get_scale, NULL, NULL, &exc);
    if (exc || !boxed)
        return -1;
    float *scale = il2cpp_object_unbox_p(boxed);
    if (!scale)
        return -1;
    return *scale < 0.05f ? 1 : 0;
}

static void *st_find_instance(void *klass)
{
    void *object_class = find_managed_class("UnityEngine", "Object");
    void *find_by_type = object_class
        ? il2cpp_class_get_method_from_name_p(object_class,
                                              "FindObjectOfType", 1)
        : NULL;
    if (!find_by_type)
        return NULL;
    void *type_obj = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!type_obj)
        return NULL;
    void *exc = NULL;
    void *args[1] = { type_obj };
    void *instance = il2cpp_runtime_invoke_p(find_by_type, NULL, args, &exc);
    return exc ? NULL : instance;
}

/* A Unity 6 DEPRECIOU `Object.FindObjectOfType`; este finder tenta a API
 * nova e depois a antiga, inclusive com includeInactive. */
static void *st_find_instance_u6(void *klass)
{
    static const struct { const char *name; int argc; } FINDERS[] = {
        { "FindAnyObjectByType",   2 },
        { "FindFirstObjectByType", 2 },
        { "FindAnyObjectByType",   1 },
        { "FindFirstObjectByType", 1 },
        { "FindObjectOfType",      2 },
        { "FindObjectOfType",      1 },
    };
    void *object_class = find_managed_class("UnityEngine", "Object");
    if (!object_class)
        return NULL;
    {
        static const struct { const char *ns; const char *cls;
                              const char *name; int argc; } PLURAL[] = {
            { "UnityEngine", "Object",    "FindObjectsOfType",      1 },
            { "UnityEngine", "Object",    "FindObjectsOfType",      2 },
            { "UnityEngine", "Resources", "FindObjectsOfTypeAll",   1 },
        };
        for (size_t k = 0; k < sizeof PLURAL / sizeof *PLURAL; k++) {
            void *owner = find_managed_class(PLURAL[k].ns, PLURAL[k].cls);
            void *method = owner
                ? il2cpp_class_get_method_from_name_p(owner, PLURAL[k].name,
                                                      PLURAL[k].argc)
                : NULL;
            if (!method)
                continue;
            void *type_probe =
                il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
            if (!type_probe)
                break;
            int32_t inactive = 1;
            void *args[2] = { type_probe, &inactive };
            void *exc = NULL;
            void *array = il2cpp_runtime_invoke_p(method, NULL, args, &exc);
            static int said[sizeof PLURAL / sizeof *PLURAL];
            if (popup_diag() && !said[k]++)
                fprintf(stderr, "[st/popup] %s.%s/%d -> %s\n", PLURAL[k].cls,
                        PLURAL[k].name, PLURAL[k].argc,
                        exc ? "excecao" : array ? "array" : "nulo");
            if (!exc && array) {
                uintptr_t length = ((uintptr_t *)array)[3];
                void **items = (void **)((uint8_t *)array + 4 * sizeof(void *));
                if (length > 0 && items[0])
                    return items[0];
            }
        }
    }
    if (il2cpp_runtime_class_init_p) {
        il2cpp_runtime_class_init_p(object_class);
        il2cpp_runtime_class_init_p(klass);
    }
    void *type_obj = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!type_obj)
        return NULL;

    for (size_t i = 0; i < sizeof FINDERS / sizeof *FINDERS; i++) {
        void *method = il2cpp_class_get_method_from_name_p(
            object_class, FINDERS[i].name, FINDERS[i].argc);
        if (!method)
            continue;
        int32_t include_inactive = 1;
        void *args[2] = { type_obj, &include_inactive };
        void *exc = NULL;
        void *instance = il2cpp_runtime_invoke_p(method, NULL, args, &exc);
        static int reported[sizeof FINDERS / sizeof *FINDERS];
        if (popup_diag() && !reported[i]++) {
            const char *why = exc ? "excecao" : instance ? "instancia" : "nulo";
            const char *exc_name = NULL;
            if (exc && il2cpp_object_get_class_p && il2cpp_class_get_name_p)
                exc_name = il2cpp_class_get_name_p(
                    il2cpp_object_get_class_p(exc));
            fprintf(stderr, "[st/popup] %s/%d -> %s%s%s\n", FINDERS[i].name,
                    FINDERS[i].argc, why, exc_name ? " " : "",
                    exc_name ? exc_name : "");
        }
        if (!exc && instance)
            return instance;
    }
    return NULL;
}

static void *st_find_component_by_walk(void *klass)
{
    void *object_class = find_managed_class("UnityEngine", "Object");
    void *go_class = find_managed_class("UnityEngine", "GameObject");
    if (!object_class || !go_class)
        return NULL;
    void *find_all = il2cpp_class_get_method_from_name_p(
        object_class, "FindObjectsOfType", 1);
    void *get_component = il2cpp_class_get_method_from_name_p(
        go_class, "GetComponent", 1);
    if (!find_all || !get_component)
        return NULL;

    void *go_type = il2cpp_type_get_object_p(il2cpp_class_get_type_p(go_class));
    void *want_type = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!go_type || !want_type)
        return NULL;

    void *args[1] = { go_type };
    void *exc = NULL;
    void *array = il2cpp_runtime_invoke_p(find_all, NULL, args, &exc);
    if (exc || !array)
        return NULL;

    uintptr_t length = ((uintptr_t *)array)[3];
    void **items = (void **)((uint8_t *)array + 4 * sizeof(void *));
    if (length > 100000)
        return NULL;
    for (uintptr_t i = 0; i < length; i++) {
        if (!items[i])
            continue;
        void *cargs[1] = { want_type };
        void *cexc = NULL;
        void *component =
            il2cpp_runtime_invoke_p(get_component, items[i], cargs, &cexc);
        if (!cexc && component) {
            static int announced;
            if (popup_diag() && !announced++)
                fprintf(stderr, "[st/popup] achado por varredura de %lu "
                                "GameObject ativos\n", (unsigned long)length);
            return component;
        }
    }
    static int said;
    if (popup_diag() && !said++)
        fprintf(stderr, "[st/popup] varredura de %lu GameObject ativos sem o "
                        "componente\n", (unsigned long)length);
    return NULL;
}

/* A caixa "SELECT CONTROLS TYPE": tentar responder pelo MESMO metodo que o
 * botao chamaria (ControlsSelectorPopUp.SelectTouchControls).  Neste alvo a
 * instancia nunca foi achada (medido, ver HANDOFF); o codigo fica inerte e a
 * caixa e' respondida pelo ponteiro.  ST_NO_CONTROLS_POPUP_FIX=1 desliga. */
static int controls_popup_answered;
static int controls_popup_attempts;

static void st_answer_controls_popup(void)
{
    const char *off = getenv("ST_NO_CONTROLS_POPUP_FIX");
    if (controls_popup_answered || (off && *off && *off != '0'))
        return;
    if (++controls_popup_attempts > 600) {
        controls_popup_answered = 1;
        return;
    }
    static int diag_api, diag_class, diag_inst;
    if (!resolve_il2cpp_invoke_api()) {
        if (popup_diag() && !diag_api++)
            fprintf(stderr, "[st/popup] il2cpp indisponivel\n");
        return;
    }
    {
        static int probed;
        if (popup_diag() && !probed++) {
            fprintf(stderr, "[st/popup] sonda invoke: Time.timeScale -> %d\n",
                    st_pause_is_open());
            void *cam = find_managed_class("UnityEngine", "Camera");
            fprintf(stderr, "[st/popup] sonda Camera -> %p\n",
                    cam ? st_find_instance_u6(cam) : NULL);
        }
    }
    void *klass = find_managed_class("Assets.Scripts.View.Windows",
                                     "ControlsSelectorPopUp");
    if (!klass) {
        if (popup_diag() && !diag_class++)
            fprintf(stderr, "[st/popup] classe ControlsSelectorPopUp nao "
                            "encontrada\n");
        return;
    }
    void *popup = st_find_instance_u6(klass);
    if (!popup)
        popup = st_find_component_by_walk(klass);
    if (!popup) {
        if (popup_diag() && !diag_inst++)
            fprintf(stderr, "[st/popup] classe achada, instancia ainda "
                            "nao\n");
        return;
    }
    void *select = il2cpp_class_get_method_from_name_p(
        klass, "SelectTouchControls", 0);
    if (!select) {
        fprintf(stderr, "[st/input] ControlsSelectorPopUp sem "
                        "SelectTouchControls; caixa mantida\n");
        controls_popup_answered = 1;
        return;
    }
    void *exc = NULL;
    il2cpp_runtime_invoke_p(select, popup, NULL, &exc);
    controls_popup_answered = 1;
    fprintf(stderr,
            "[st/input] SELECT CONTROLS TYPE respondido pelo proprio jogo "
            "(SelectTouchControls) -- %s\n", exc ? "EXCECAO" : "ok");
}

/* O pause DO JOGO pelo toggle oficial (LevelStart.PausePressed), opt-in
 * ST_PAUSE_NATIVE=1 (desligado por padrao, como no port aprovado). */
static int st_pause_toggle_native(void)
{
    const char *flag = getenv("ST_PAUSE_NATIVE");
    if (!flag || !*flag || strcmp(flag, "0") == 0)
        return 0;
    if (!resolve_il2cpp_invoke_api())
        return 0;
    void *level_class = find_managed_class("", "LevelStart");
    if (!level_class)
        return 0;
    void *toggle = il2cpp_class_get_method_from_name_p(level_class,
                                                       "PausePressed", 0);
    if (!toggle) {
        fprintf(stderr, "[st/pause] LevelStart.PausePressed() ausente\n");
        return 0;
    }
    void *instance = st_find_instance(level_class);
    if (!instance) {
        if (input_diag)
            fprintf(stderr, "[st/pause] sem LevelStart vivo (fora de nivel)\n");
        return 0;
    }
    void *get_paused = il2cpp_class_get_method_from_name_p(
        level_class, "get_IsPaused", 0);
    int was_paused = 0, now_paused = 0;
    void *exc = NULL;
    if (get_paused && il2cpp_object_unbox_p) {
        void *boxed = il2cpp_runtime_invoke_p(get_paused, instance, NULL, &exc);
        uint8_t *state = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
        was_paused = state && *state;
    }
    exc = NULL;
    il2cpp_runtime_invoke_p(toggle, instance, NULL, &exc);
    if (exc) {
        const char *what = (il2cpp_object_get_class_p && il2cpp_class_get_name_p)
            ? il2cpp_class_get_name_p(il2cpp_object_get_class_p(exc))
            : "?";
        fprintf(stderr, "[st/pause] PausePressed lancou %s (seguindo)\n",
                what ? what : "?");
    }
    if (get_paused && il2cpp_object_unbox_p) {
        exc = NULL;
        void *boxed = il2cpp_runtime_invoke_p(get_paused, instance, NULL, &exc);
        uint8_t *state = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
        now_paused = state && *state;
    }
    if (input_diag)
        fprintf(stderr, "[st/pause] PausePressed() IsPaused %d -> %d\n",
                was_paused, now_paused);
    return was_paused || now_paused;
}

static int st_pause_hide_native(void)
{
    const char *flag = getenv("ST_PAUSE_NATIVE");
    if (!flag || !*flag || strcmp(flag, "0") == 0)
        return 0;
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[st/pause] il2cpp invoke API indisponivel\n");
        return 0;
    }
    void *pause_class = find_managed_class("", "PauseMenu");
    if (!pause_class) {
        fprintf(stderr, "[st/pause] classe PauseMenu nao encontrada\n");
        return 0;
    }
    void *hide = il2cpp_class_get_method_from_name_p(pause_class,
                                                     "HidePauseMenu", 0);
    if (!hide) {
        fprintf(stderr, "[st/pause] HidePauseMenu ausente\n");
        return 0;
    }
    void *instance = st_find_instance(pause_class);
    if (!instance) {
        fprintf(stderr, "[st/pause] instancia de PauseMenu nao achada\n");
        return 0;
    }
    void *exc = NULL;
    il2cpp_runtime_invoke_p(hide, instance, NULL, &exc);
    if (exc) {
        fprintf(stderr, "[st/pause] HidePauseMenu lancou excecao\n");
        return 0;
    }
    fprintf(stderr, "[st/pause] HidePauseMenu() invocado no jogo\n");
    return 1;
}

/* ===== START: pausa/despausa como no port aprovado =======================
 * O menu de pause do jogo e' touch-only.  START abre pelo caminho nativo
 * (KEYCODE_BUTTON_START); com o jogo PAUSADO (Time.timeScale == 0) o START
 * manda KEYCODE_BACK, o "voltar" do Android, que fecha o pause.  Sem a API
 * (raro) vale a paridade `pause_open`.  Um clique do ponteiro zera a
 * paridade (o dono pode ter fechado pelo botao de resume). */
static int pause_open;
static int start_keycode_down; /* keycode entregue no DOWN corrente (0 = nenhum) */

static void start_press(int pressed)
{
    if (pressed) {
        if (st_pause_toggle_native()) {
            pause_open = 0;
            start_keycode_down = 0;
            return;
        }
        int game_paused = st_pause_is_open();
        if (input_diag)
            fprintf(stderr, "[st/pause] START: timeScale diz %s\n",
                    game_paused < 0 ? "n/d (usando paridade)"
                                    : game_paused ? "PAUSADO" : "rodando");
        if (game_paused > 0 || (game_paused < 0 && pause_open)) {
            if (!st_pause_hide_native()) {
                start_keycode_down = AKEY_BACK;
                sink_key(AKEY_BACK, 1);
                if (input_diag)
                    fprintf(stderr, "[st/pause] fechando (BACK)\n");
            } else {
                start_keycode_down = 0;
            }
            pause_open = 0;
            return;
        }
        pause_open = 1;
        start_keycode_down = AKEY_BUTTON_START;
        sink_key(AKEY_BUTTON_START, 1);
        if (input_diag)
            fprintf(stderr, "[st/pause] abrindo (BUTTON_START)\n");
    } else if (start_keycode_down) {
        sink_key(start_keycode_down, 0);
        start_keycode_down = 0;
    }
}

/* ===== Ponteiro (seta + toque) — somente menus touch ==================== */
static int cursor_enabled;
static int cursor_menu_active;
static float cursor_x = 640.0f;
static float cursor_y = 360.0f;
static float cursor_vx;
static float cursor_vy;
static uint64_t cursor_tick;
static int cursor_drag_active;
static uint64_t cursor_seen_tick;
static float cursor_hide_after = 4.0f;
static int cursor_first_click_done;
static float cursor_touch_x;
static float cursor_touch_y;
/* Entrada do quadro vem do sink de menu provado pelo GPTK. */
static float cursor_in_x, cursor_in_y;
static int cursor_click_held, cursor_click_prev;

static int cursor_is_active(void)
{
    return cursor_enabled && cursor_menu_active;
}

static void update_cursor(void *env, void *player)
{
    if (!cursor_is_active()) {
        /* Uma troca menu -> gameplay durante o clique nunca pode deixar um
         * toque Android preso. O UP usa a última coordenada efetivamente
         * entregue, antes de devolver direito/R3 ao jogo. */
        if (cursor_drag_active)
            inject(env, player,
                   st_jni_touch_event(1, cursor_touch_x, cursor_touch_y));
        cursor_drag_active = 0;
        cursor_click_held = 0;
        cursor_click_prev = 0;
        cursor_in_x = cursor_in_y = 0.0f;
        cursor_vx = cursor_vy = 0.0f;
        cursor_tick = 0;
        return;
    }

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = cursor_tick && frequency
             ? (float)((double)(now - cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;

    float x = cursor_in_x;
    float y = cursor_in_y;
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
    if (magnitude > deadzone || cursor_click_held)
        cursor_seen_tick = now;   /* mexeu ou clicou: a seta reaparece */

    float blend = 1.0f - expf(-14.0f * dt);
    cursor_vx += (target_x - cursor_vx) * blend;
    cursor_vy += (target_y - cursor_vy) * blend;
    cursor_x += cursor_vx * dt;
    cursor_y += cursor_vy * dt;
    if (cursor_x < 0.0f) cursor_x = 0.0f;
    if (cursor_x > 1279.0f) cursor_x = 1279.0f;
    if (cursor_y < 0.0f) cursor_y = 0.0f;
    if (cursor_y > 719.0f) cursor_y = 719.0f;

    int held = cursor_click_held;
    int down = held && !cursor_click_prev;
    int up = !held && cursor_click_prev;
    cursor_click_prev = held;
    /* MotionEvent usa coordenadas da view/painel, não as dimensões do FBO
     * interno.  A mesma transformação que desenha a seta precisa governar o
     * toque; caso contrário preserve desloca o clique pelas barras e stretch
     * muda sua escala em painéis 4:3/1:1. */
    float touch_x = (float)touch_origin_x +
                    cursor_x * (float)touch_width / 1280.0f;
    float touch_y = (float)touch_origin_y +
                    cursor_y * (float)touch_height / 720.0f;
    if (down) {
        inject(env, player, st_jni_touch_event(0, touch_x, touch_y));
        cursor_first_click_done = 1;
        cursor_drag_active = 1;
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
        pause_open = 0;   /* clicou na tela: o estado do pause deixa de ser nosso */
        if (input_diag)
            fprintf(stderr, "[st/touch] DOWN %.0f,%.0f\n", touch_x, touch_y);
    } else if (held && cursor_drag_active &&
               (fabsf(touch_x - cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - cursor_touch_y) >= 0.25f)) {
        inject(env, player, st_jni_touch_event(2, touch_x, touch_y));
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
    }
    if (up && cursor_drag_active) {
        inject(env, player, st_jni_touch_event(1, touch_x, touch_y));
        cursor_drag_active = 0;
        if (input_diag)
            fprintf(stderr, "[st/touch] UP   %.0f,%.0f\n", touch_x, touch_y);
    }
}

/* ===== Sinks reais do adapter (ACK = 0) =================================
 * Símbolos exportados de propósito: o nxrelease liga o sink-id do
 * adapter-contract a um símbolo definido neste ELF. */
int st_sink_android_keyevent(void *user, const char *action, int pressed,
                             float value)
{
    (void)value;
    int keycode = (int)(intptr_t)user;
    if (input_diag)
        fprintf(stderr, "[st/sink] %s keycode=%d %s\n", action, keycode,
                pressed ? "down" : "up");
    if (keycode == AKEY_BUTTON_START)
        start_press(pressed);
    else
        sink_key(keycode, pressed);
    return 0;
}

static float move_axis_x, move_axis_y;
static int move_vector_this_frame;
int st_sink_android_motion(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    move_axis_x = x;
    move_axis_y = y;
    move_vector_this_frame = 1;
    return 0;
}

static int cursor_vector_this_frame;
int st_sink_cursor(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    cursor_in_x = x;
    cursor_in_y = y;
    cursor_vector_this_frame = 1;
    return 0;
}

static int cursor_click_from_sink;
int st_sink_cursor_click(void *user, const char *action, int pressed,
                         float value)
{
    (void)user; (void)value;
    if (input_diag)
        fprintf(stderr, "[st/sink] %s click %s\n", action,
                pressed ? "down" : "up");
    cursor_click_from_sink = pressed ? 1 : 0;
    return 0;
}

static int st_register_sinks(void)
{
    /* Keycode Android que o InControl do jogo espera para cada botao: o
     * MESMO que o port aprovado injetava (KEYCODE_BUTTON_A..Y, L1/R1,
     * START).  O nome semantico descreve a posicao no pad; o mapa de acoes
     * do jogo (KILL/SPRINT/DANCE/...) e' desenhado pelo proprio jogo. */
    struct { const char *action; int keycode; } b[] = {
        { "partyhard.action1",      AKEY_BUTTON_A },
        { "partyhard.action2",      AKEY_BUTTON_B },
        { "partyhard.action3",      AKEY_BUTTON_X },
        { "partyhard.action4",      AKEY_BUTTON_Y },
        { "partyhard.bumper_left",  AKEY_BUTTON_L1 },
        { "partyhard.bumper_right", AKEY_BUTTON_R1 },
        { "partyhard.pause",        AKEY_BUTTON_START },
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (st_gptk_register_button(b[i].action,
                                    "adapter.input.android-keyevent",
                                    st_sink_android_keyevent,
                                    (void *)(intptr_t)b[i].keycode) != 0)
            return -1;
    if (st_gptk_register_vector("partyhard.move", "adapter.input.android-motion",
                                st_sink_android_motion, NULL) != 0)
        return -1;
    if (st_gptk_register_vector("partyhard.cursor", "adapter.input.cursor",
                                st_sink_cursor, NULL) != 0)
        return -1;
    if (st_gptk_register_button("partyhard.click", "adapter.input.cursor-click",
                                st_sink_cursor_click, NULL) != 0)
        return -1;
    return st_gptk_seal();
}

/* ===== Ponte SDL_GameController -> vocabulário simbólico ================ */
static int st_control_of(int sdl_button)
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

/* Keycode do passthrough nativo. No menu o GPTK consome R3 para o clique;
 * em gameplay ele volta a KEYCODE_BUTTON_THUMBR como no Android real. */
static int st_native_keycode(int control)
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
    Sint16 value = nxinput_padset_axis(&padset, (int)axis);
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
        axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return value > 0 ? value / 32767.0f : 0.0f;
    /* Repouso = ZERO exato (v/32768 no negativo, v/32767 no positivo). */
    return value < 0 ? value / 32768.0f : value / 32767.0f;
}

/* Deadzone radial com reescala: repouso é ZERO exato e a deflexão total
 * continua alcançando 1. */
static void radial_deadzone(float *x, float *y)
{
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= ST_STICK_DEADZONE) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    float scale = (m - ST_STICK_DEADZONE) / (1.0f - ST_STICK_DEADZONE);
    if (scale > 1.0f)
        scale = 1.0f;
    *x = *x / m * scale;
    *y = *y / m * scale;
}

#ifdef ST_BENCH_PROBES
/* ===== Pad virtual de bancada (SÓ com -DST_BENCH_PROBES) ================
 * Um token por arquivo em ST_VPAD_FILE, lido uma vez por quadro e apagado,
 * vira um pulso de N quadros no estado SIMBÓLICO — depois do mapping
 * soberano da SDL, antes do GPTK.  Nunca compila na release pública. */
static int vpad_enabled;
static const char *vpad_file = "/tmp/st-vpad";
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
        extern int st_shot_request;
        st_shot_request = 1;
        found = 1;
    }
    fprintf(stderr, "[st/vpad] pulse %s x%u (%s)\n", token, duration,
            found ? "accepted" : "unknown");
}

#else /* release pública: nenhum caminho de injeção compilado */
enum { vpad_enabled = 0 };
static const unsigned vpad_frames[NXINPUT_GPTK_CONTROL_COUNT] = { 0 };
static const unsigned vpad_axis_frames[4] = { 0 };
static const float vpad_axis_values[4] = { 0 };
static void vpad_poll(void) { }
#endif /* ST_BENCH_PROBES */

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
static int padset_admit(int i, void *user)
{
    (void)user;
    if (st_seam_adopted) {
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_text[64];
        const char *devpath = st_sdl_path_for_index
                            ? st_sdl_path_for_index(i) : NULL;
        SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
        if (!nxc6_admit_before_announce(
                (int)st_sdl_instance_for_index(i), guid_text,
                devpath ? devpath : "")) {
            fprintf(stderr, "[st/input] NXC6 seam: device %d (%s) "
                            "refused by the authority order\n",
                    i, guid_text);
            return 0;
        }
    }
    return 1;
}

static void padset_opened(int i, unsigned slot, void *opened_ptr, void *user)
{
    (void)user;
    SDL_GameController *opened = opened_ptr;
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(opened);
    if (slot == 0)
        controller = opened;
    const char *physical = SDL_GameControllerName(opened);
    int vendor = (joy && st_sdl_joy_vendor) ? st_sdl_joy_vendor(joy) : 0;
    int product = (joy && st_sdl_joy_product) ? st_sdl_joy_product(joy) : 0;
    char *mapping = SDL_GameControllerMapping(opened);
    /* Identidade apresentada a' Unity: a MESMA do port aprovado.  Nao e'
     * autoridade de mapping (o mapping soberano e' o da SDL/CFW); e' o nome
     * pelo qual o InControl escolhe o PERFIL de botoes -- medido: publicar o
     * nome real faz o InControl trocar de perfil e REMAPEAR os botoes. */
    if (slot == 0)
        st_jni_input_device_info("Microsoft X-Box 360 pad", vendor, product,
                                 physical ? physical : "gamepad");
    fprintf(stderr, "[st/input] controller: %s (%04x:%04x) mapping=%s\n",
            physical ? physical : "unknown", vendor & 0xffff,
            product & 0xffff, mapping ? mapping : "unavailable");
    fprintf(stderr, "[st/input] pad slot=%d instance=%d sdl_index=%d\n",
            slot, (int)padset.instances[slot], i);
    st_input_guard_attach(i, opened, padset.instances[slot]);
    SDL_free(mapping);
}

static void padset_log(const char *line, void *user)
{
    (void)user;
    fprintf(stderr, "[st/input] %s\n", line);
}

/* ===== nxinput_padset: vtable sobre a SDL do firmware (nunca privada) ===== */
static int ps_num_joysticks(void) { return SDL_NumJoysticks(); }
static int32_t ps_instance_for_index(int i) { return (int32_t)st_sdl_instance_for_index(i); }
static int ps_is_game_controller(int i) { return SDL_IsGameController(i) ? 1 : 0; }
static void *ps_open(int i) { return SDL_GameControllerOpen(i); }
static void ps_close(void *c) { SDL_GameControllerClose(c); }
static void *ps_get_joystick(void *c) { return SDL_GameControllerGetJoystick(c); }
static int32_t ps_joystick_instance(void *j) { return (int32_t)SDL_JoystickInstanceID(j); }
static void ps_update(void)
{
    SDL_GameControllerUpdate();
    st_input_guard_snapshot_all();
}

static uint8_t ps_get_button(void *c, int b)
{
    uint8_t raw = SDL_GameControllerGetButton(
        c, (SDL_GameControllerButton)b) ? 1u : 0u;
#ifdef ST_BENCH_PROBES
    if (b == bench_stale_button)
        raw = 1u;
#endif
    st_input_guard *guard = st_input_guard_for_controller(c);
    int valid_button = guard && b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX;
    int hat = valid_button ? guard->button_hat[b] : -1;
    int snapshot_valid = 0;
    int physically_neutral = 0;
    int binding_supported = 0;
    if (valid_button && guard->key_backed[b]) {
        snapshot_valid = guard->key_snapshot_valid;
        physically_neutral = guard->key_idle;
        binding_supported = 1;
    } else if (hat >= 0 && hat < 4 && guard->hat_supported[hat]) {
        snapshot_valid = guard->hat_snapshot_valid[hat];
        physically_neutral = guard->hat_centered[hat];
        binding_supported = 1;
    }
    uint8_t filtered = (uint8_t)ph_input_guard_button_value(
        raw, snapshot_valid, physically_neutral, binding_supported);
    if (guard && raw && !filtered) {
        if (!guard->button_heal_logged) {
            fprintf(stderr,
                    "[st/input] release guard: corrected stale SDL button "
                    "instance=%d button=%d (kernel neutral)\n",
                    (int)guard->instance, b);
            guard->button_heal_logged = 1;
        }
    }
    return filtered;
}

static int16_t ps_get_axis(void *c, int a)
{
    int16_t raw = SDL_GameControllerGetAxis(
        c, (SDL_GameControllerAxis)a);
#ifdef ST_BENCH_PROBES
    if (a == bench_stale_axis)
        raw = 32767;
#endif
    st_input_guard *guard = st_input_guard_for_controller(c);
    int valid_axis = guard && a >= 0 && a < 4;
    int16_t filtered = (int16_t)ph_input_guard_axis_value(
        raw,
        valid_axis ? guard->stick_snapshot_valid[a] : 0,
        valid_axis ? guard->stick_centered[a] : 0);
    if (guard && raw && !filtered) {
        if (!guard->axis_heal_logged[a]) {
            fprintf(stderr,
                    "[st/input] release guard: corrected stale SDL axis "
                    "instance=%d axis=%d (kernel centered)\n",
                    (int)guard->instance, a);
            guard->axis_heal_logged[a] = 1;
        }
    }
    return filtered;
}

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
        fprintf(stderr, "[st/input] nxinput_padset: vtable incompleta (fail-closed)\n");
        return -1;
    }
    fprintf(stderr, "[st/input] pads: %s (união dos admitidos, chord por instance)\n",
            nxinput_padset_marker());
    return 0;
}

static void open_controller(void)
{
    nxinput_padset_open_all(&padset, padset_admit, padset_opened, NULL);
    controller = nxinput_padset_first(&padset);
    if (padset.count == 0 && SDL_NumJoysticks() > 0)
        fprintf(stderr, "[st/input] %d joystick(s) visible but none admitted "
                        "as GameController; no fallback by design\n",
                SDL_NumJoysticks());
}

static void close_controller(void)
{
    st_input_guard_close_all();
    nxinput_padset_close_all(&padset);
    controller = NULL;
    memset(buttons, 0, sizeof buttons);
}

/* ===== Chord soberano ==================================================== */
static nxinput_exit_chord exit_chord;

/* ===== Init ============================================================== */
int st_input_preinit(void)
{
    /* Fronteira pré-init do nxinput 0.10.x: owner/default + FACE_LAYOUT lidos
     * UMA vez, antes de bundle, staging e de qualquer SDL_Init. */
    return st_gptk_preinit(st_gamedir) != 0 ? -1 : 0;
}

int st_input_init(void)
{
#ifdef ST_BENCH_PROBES
    input_diag = getenv("ST_INPUT_DIAG") != NULL;
    input_engine_probe = getenv("ST_INPUT_ENGINE_PROBE") != NULL;
    vpad_enabled = getenv("ST_VPAD") && strcmp(getenv("ST_VPAD"), "0") != 0;
    if (getenv("ST_VPAD_FILE") && *getenv("ST_VPAD_FILE"))
        vpad_file = getenv("ST_VPAD_FILE");
    if (getenv("ST_INPUT_STALE_BUTTON"))
        bench_stale_button = atoi(getenv("ST_INPUT_STALE_BUTTON"));
    if (getenv("ST_INPUT_STALE_AXIS"))
        bench_stale_axis = atoi(getenv("ST_INPUT_STALE_AXIS"));
#endif
    /* Disparo no PRIMEIRO quadro em que SELECT e START estão ambos lógicos
     * (regra #40: chord sem hold/atraso); nada do chord vaza ao jogo. */
    nxinput_exit_chord_init(&exit_chord, 1);
    *(void **)&st_sdl_path_for_index = optional_sdl("SDL_JoystickPathForIndex");
    *(void **)&st_sdl_instance_for_index =
        optional_sdl("SDL_JoystickGetDeviceInstanceID");
    *(void **)&st_sdl_joy_vendor = optional_sdl("SDL_JoystickGetVendor");
    *(void **)&st_sdl_joy_product = optional_sdl("SDL_JoystickGetProduct");

    /* 🚨 NESTE PORT O PONTEIRO NASCE LIGADO, e isso e' necessidade, nao gosto:
     * a caixa "SELECT CONTROLS TYPE" so' aceita ponteiro (medido: nenhuma
     * tecla do pad a fecha) e um portatil nao tem tela de toque.  O analogico
     * DIREITO move a seta e o R3 clica; o A NAO clica, porque neste jogo ele
     * e' acao.  ST_CURSOR=0 desliga. */
    {
        const char *want = getenv("ST_CURSOR");
        cursor_enabled = !want || (*want && strcmp(want, "0") != 0);
    }
    {
        const char *hide = getenv("ST_CURSOR_HIDE");
        if (hide) {
            float v = strtof(hide, NULL);
            cursor_hide_after = (v >= 0.0f) ? v : 4.0f;   /* 0 = nunca some */
        }
    }
    if (!st_gptk_loaded()) {
        /* Sem mapa válido não existe caminho de palpite: fail-closed. */
        fprintf(stderr, "[st/input] NEXTOSCONTROLLERS ausente/inválido; "
                        "abortando (fail-closed)\n");
        return -1;
    }
    if (st_stage_seam_before_init() != 0)
        return -1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[st/input] SDL controller init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    if (padset_setup() != 0)
        return -1;
    open_controller();
    if (st_register_sinks() != 0) {
        fprintf(stderr, "[st/input] runtime vivo não selado; abortando "
                        "(fail-closed)\n");
        return -1;
    }
    fprintf(stderr,
            "[st/input] layout: gamepad nativo + GPTK vivo; "
            "chord=SELECT+START(sovereign); cursor=%s (direito move, R3 clica); "
            "vpad=%s\n",
            cursor_enabled ? "on" : "off", vpad_enabled ? "on" : "off");
    return (controller || vpad_enabled) ? 0 : -1;
}

/* ===== Menu focus: native navigation versus stale touch hover ===========
 * Unity retains mousePosition after a touch UP. MouseOverHandler keeps
 * selecting that old position while MenuInputManager.mouseControl is true,
 * undoing D-pad selection without updating activeButtonIndex. Hand focus to
 * native navigation until the owner actually uses the auxiliary pointer.
 * Keep the game's selection/click handlers and its input mode untouched.
 */
static void update_menu_focus(int native_intent, int pointer_intent)
{
    static int native_focus;
    static int resolved;
    static FieldInfo *instance_field, *mouse_field;
    if (st_gptk_context() != ST_GPTK_CONTEXT_MENU) {
        native_focus = 0;
        return;
    }
    int release_pointer = pointer_intent && native_focus;
    if (pointer_intent)
        native_focus = 0;
    else if (native_intent)
        native_focus = 1;
    if ((!native_focus && !release_pointer) || !il2_load())
        return;
    if (!resolved) {
        resolved = -1;
        Il2CppClass *manager = il2_class("Assets.Scripts.Controllers.Game", "MenuInputManager");
        if (!manager)
            return;
        instance_field = il2_field(manager, "instance");
        mouse_field = il2_field(manager, "mouseControl");
        if (instance_field && mouse_field)
            resolved = 1;
    }
    if (resolved < 0)
        return;
    void *manager = NULL;
    il2_static_get(instance_field, &manager);
    if (!manager)
        return;
    uint8_t before = 0, mouse = native_focus ? 0 : 1;
    il2_field_get(manager, mouse_field, &before);
    if (before != mouse) {
        il2_field_set(manager, mouse_field, &mouse);
        if (input_diag)
            fprintf(stderr, "[st/input] menu focus=%s (native hover gate)\n",
                    native_focus ? "gamepad" : "pointer");
    }
}

/* ===== Tutorial glyph visibility (presentation only) ====================
 * The Android game keeps TouchMobileMode=true even while its input-sign
 * controller selects XboxGamepadButtons. TutorialArrowButtonSign hides its
 * animator GameObject under that flag. Restore only that existing, selected
 * glyph, under a visible parent; never change mobile mode, input profile,
 * animation, tutorial progress, or a hidden tutorial container.
 */
static int glyph_bool(const MethodInfo *method, void *self)
{
    void *value = il2_unbox(il2_call(method, self, NULL, "tutorial glyph state"));
    return value ? *(uint8_t *)value != 0 : 0;
}

static void restore_tutorial_gamepad_glyphs(unsigned long frame)
{
    if (!frame || frame % 30 || !controller ||
        st_gptk_context() != ST_GPTK_CONTEXT_GAMEPLAY || !il2_load())
        return;
    static int resolved;
    static Il2CppClass *tutorial;
    static FieldInfo *mobile_field, *sign_instance_field, *sign_input_field;
    static FieldInfo *animator_field, *finished_field;
    static const MethodInfo *find_all, *active_behaviour, *get_go, *get_transform;
    static const MethodInfo *get_parent, *active_self, *active_hierarchy, *set_active;
    static const MethodInfo *current_clip;
    if (!resolved) {
        resolved = -1;
        Il2CppClass *touch = il2_class("Assets.Scripts.Controllers.Touch", "TouchController");
        Il2CppClass *sign = il2_class("Assets.Scripts.Controllers.InputButton", "InputButtonSignController");
        tutorial = il2_class("Assets.Scripts.SceneObjects", "TutorialArrowButtonSign");
        Il2CppClass *object = il2_class("UnityEngine", "Object");
        Il2CppClass *behaviour = il2_class("UnityEngine", "Behaviour");
        Il2CppClass *component = il2_class("UnityEngine", "Component");
        Il2CppClass *go = il2_class("UnityEngine", "GameObject");
        Il2CppClass *transform = il2_class("UnityEngine", "Transform");
        Il2CppClass *animator = il2_class("", "tk2dSpriteAnimator");
        if (!touch || !sign || !tutorial || !object || !behaviour ||
            !component || !go || !transform || !animator)
            return;
        mobile_field = il2_field(touch, "<TouchMobileMode>k__BackingField");
        sign_instance_field = il2_field(sign, "_instance");
        sign_input_field = il2_field(sign, "currentInput");
        animator_field = il2_field(tutorial, "infoSignSelectAnimation");
        finished_field = il2_field(tutorial, "finishGame");
        find_all = il2_method_p(object, "FindObjectsOfType", 1, "Type");
        active_behaviour = il2_method(behaviour, "get_isActiveAndEnabled", 0);
        get_go = il2_method(component, "get_gameObject", 0);
        get_transform = il2_method(go, "get_transform", 0);
        get_parent = il2_method(transform, "get_parent", 0);
        active_self = il2_method(go, "get_activeSelf", 0);
        active_hierarchy = il2_method(go, "get_activeInHierarchy", 0);
        set_active = il2_method(go, "SetActive", 1);
        current_clip = il2_method(animator, "get_CurrentClip", 0);
        if (mobile_field && sign_instance_field && sign_input_field &&
            animator_field && finished_field && find_all && active_behaviour &&
            get_go && get_transform && get_parent && active_self &&
            active_hierarchy && set_active && current_clip)
            resolved = 1;
    }
    if (resolved < 0)
        return;
    uint8_t mobile = 0;
    void *sign = NULL, *input = NULL;
    il2_static_get(mobile_field, &mobile);
    il2_static_get(sign_instance_field, &sign);
    if (!mobile || !sign)
        return;
    il2_field_get(sign, sign_input_field, &input);
    if (!input || strcmp(il2_class_name(il2_class_of(input)), "XboxGamepadButtons"))
        return;
    void *type = il2_type(tutorial);
    if (!type)
        return;
    void *args[1] = {type};
    void *objects = il2_call(find_all, NULL, args, "active tutorial signs");
    if (!objects)
        return;
    uint32_t count = il2_arr_len(objects);
    if (count > 16)
        return;
    static unsigned glyph_probe_samples;
    int probe = input_diag && glyph_probe_samples++ < 4;
    if (probe)
        fprintf(stderr, "[st/input] tutorial glyph candidates=%u\n", count);
    for (uint32_t i = 0; i < count; ++i) {
        void *object = il2_arr_at(objects, i), *animator = NULL;
        uint8_t finished = 0;
        if (!object)
            continue;
        il2_field_get(object, finished_field, &finished);
        il2_field_get(object, animator_field, &animator);
        int enabled = glyph_bool(active_behaviour, object);
        void *clip = animator ? il2_call(current_clip, animator, NULL, "selected tutorial animation") : NULL;
        void *go = animator ? il2_call(get_go, animator, NULL, "tutorial glyph object") : NULL;
        void *transform = go ? il2_call(get_transform, go, NULL, "tutorial glyph transform") : NULL;
        void *parent = transform ? il2_call(get_parent, transform, NULL, "tutorial glyph parent") : NULL;
        void *parent_go = parent ? il2_call(get_go, parent, NULL, "tutorial container") : NULL;
        if (probe) {
            char clip_name[96] = "none";
            if (clip) {
                FieldInfo *name_field = il2_field(il2_class_of(clip), "name");
                void *name = NULL;
                if (name_field) il2_field_get(clip, name_field, &name);
                il2_str_utf8(name, clip_name, sizeof clip_name);
            }
            fprintf(stderr, "[st/input] tutorial glyph index=%u enabled=%d finished=%d animator=%d clip=%s active=%d parent_visible=%d\n",
                    i, enabled, finished, animator != NULL, clip_name,
                    go ? glyph_bool(active_self, go) : -1,
                    parent_go ? glyph_bool(active_hierarchy, parent_go) : -1);
        }
        if (!enabled || finished || !clip || !go || glyph_bool(active_self, go) ||
            !parent_go || !glyph_bool(active_hierarchy, parent_go))
            continue;
        uint8_t show = 1;
        void *show_args[1] = {&show};
        il2_call(set_active, go, show_args, "restore existing gamepad tutorial glyph");
        if (input_diag)
            fprintf(stderr, "[st/input] tutorial glyph restored=%d index=%u\n",
                    glyph_bool(active_self, go), i);
    }
}

/* ===== Poll por quadro =================================================== */
static void sample_controls(void)
{
    memset(control_down, 0, sizeof control_down);
    nxinput_padset_sample(&padset);
    memcpy(buttons, padset.buttons, sizeof buttons < sizeof padset.buttons ? sizeof buttons : sizeof padset.buttons);
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        int control = st_control_of(i);
        if (control >= 0 && buttons[i])
            control_down[control] = 1;
    }
    trigger_value[0] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    trigger_value[1] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    for (int t = 0; t < 2; t++) {
        int was = trigger_digital[t];
        trigger_digital[t] = was ? trigger_value[t] > ST_TRIGGER_EXIT
                                 : trigger_value[t] > ST_TRIGGER_ENTER;
    }
    control_down[NXINPUT_GPTK_L2] = trigger_digital[0];
    control_down[NXINPUT_GPTK_R2] = trigger_digital[1];
    if (vpad_enabled)
        for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++)
            if (vpad_frames[c] > 0)
                control_down[c] = 1;

    unsigned conflicts = ph_input_resolve_dpad(
        &control_down[NXINPUT_GPTK_UP],
        &control_down[NXINPUT_GPTK_DOWN],
        &control_down[NXINPUT_GPTK_LEFT],
        &control_down[NXINPUT_GPTK_RIGHT]);
    static unsigned previous_conflicts;
    if (conflicts && conflicts != previous_conflicts)
        fprintf(stderr,
                "[st/input] D-pad limiter: contradictory pair cancelled "
                "(vertical=%d horizontal=%d)\n",
                !!(conflicts & PH_INPUT_DPAD_CONFLICT_VERTICAL),
                !!(conflicts & PH_INPUT_DPAD_CONFLICT_HORIZONTAL));
    previous_conflicts = conflicts;
}

void st_input_poll(void *env, void *player, unsigned long frame)
{
    input_last_env = env;
    input_last_player = player;
    move_vector_this_frame = 0;
    cursor_vector_this_frame = 0;

    /* Tarde de proposito: entrar no il2cpp_domain_get antes da engine
     * terminar de subir mata a Unity.  A caixa so' aparece bem depois. */
    if (frame > 120 && frame % 30 == 0)
        st_answer_controls_popup();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_controller();
        if (event.type == SDL_JOYDEVICEREMOVED && st_seam_adopted)
            nxc6_forget((int)event.jdevice.which);
        if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            st_input_guard_remove(event.cdevice.which);
            if (nxinput_padset_remove_instance(&padset,
                                               event.cdevice.which)) {
                controller = nxinput_padset_first(&padset);
                st_gptk_release_all("controller-removed");
                release_all_keys();
                deliver_neutral_motion("controller-removed");
                cursor_click_from_sink = 0;
                open_controller();
            }
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            st_gptk_release_all("focus-lost");
            release_all_keys();
            deliver_neutral_motion("focus-lost");
            cursor_click_from_sink = 0;
        }
    }
    vpad_poll();
    sample_controls();
    update_engine_context(frame);
    st_tutorial_return_poll(gui_api_state > 0 && gui_screen_value == 4 &&
                            st_gptk_context() == ST_GPTK_CONTEXT_MENU);
    restore_tutorial_gamepad_glyphs(frame);

    if (!controller && !vpad_enabled) {
        st_gptk_release_all("controller-unavailable");
        release_all_keys();
        deliver_neutral_motion("controller-unavailable");
        return;
    }

    /* Chord soberano SELECT+START: só quando UM instance segura os dois;
     * SELECT num pad + START noutro nunca encerra.  GUIDE, L1+R1, L2+R2 ou
     * qualquer outra combinação jamais encerram. */
    int chord_select = 0, chord_start = 0;
    nxinput_padset_chord_inputs(&padset, &chord_select, &chord_start);
    if (nxinput_exit_chord_fold_signal(&exit_chord, &signal_exit) ||
        nxinput_exit_chord_update(&exit_chord, chord_select, chord_start)) {
        (void)nxinput_exit_chord_consume(&exit_chord);
        fprintf(stderr, "[st/input] SELECT+START: lifecycle exit requested\n");
        exit_requested = 1;
        st_gptk_release_all("exit-chord");
        release_all_keys();
        deliver_neutral_motion("exit-chord");
        return;
    }

    /* ===== Despacho GPTK (botões e gatilhos: transições físicas) ===== */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (c == NXINPUT_GPTK_LEFT_STICK || c == NXINPUT_GPTK_RIGHT_STICK)
            continue;
        float value = control_down[c] ? 1.0f : 0.0f;
        if (c == NXINPUT_GPTK_L2) value = trigger_value[0];
        if (c == NXINPUT_GPTK_R2) value = trigger_value[1];
        if (st_gptk_feed_button(c, control_down[c], value) ==
            ST_GPTK_LIVE_FATAL)
            input_fatal = 1;
    }

    /* ===== Vetores ===== */
    float lx = stick_axis(0), ly = stick_axis(1);
    float rx = stick_axis(2), ry = stick_axis(3);
    radial_deadzone(&lx, &ly);
    radial_deadzone(&rx, &ry);
    int left_consumed = st_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK);
    int right_consumed = st_gptk_should_consume(NXINPUT_GPTK_RIGHT_STICK);
    if (left_consumed &&
        st_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, lx, ly) ==
            ST_GPTK_LIVE_FATAL)
        input_fatal = 1;
    if (right_consumed &&
        st_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, rx, ry) ==
            ST_GPTK_LIVE_FATAL)
        input_fatal = 1;

    if (input_fatal) {
        fprintf(stderr, "[st/input] FATAL no runtime vivo: encerrando sem "
                        "reproduzir nativamente\n");
        exit_requested = 1;
        release_all_keys();
        deliver_neutral_motion("input-fatal");
        return;
    }

    /* ===== Passthrough nativo dirigido por ESTADO ===== */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        int keycode = st_native_keycode(c);
        if (!keycode)
            continue;
        /* One native route for every symbolic control. In this game the
         * Android DPAD KeyEvents drive both menu and CharController; the
         * HAT-only differential below measured engine=0 during gameplay. */
        int desired = control_down[c] && !st_gptk_should_consume(c);
        if (!desired && sink_key_pressed[keycode])
            continue;
        deliver_key(keycode, desired);
    }
    /* START nativo (contexto nao provado): a mesma maquina de pause do sink. */
    {
        static int native_start_down;
        int desired = control_down[NXINPUT_GPTK_START] &&
                      !st_gptk_should_consume(NXINPUT_GPTK_START);
        if (desired != native_start_down) {
            native_start_down = desired;
            if (desired || start_keycode_down)
                start_press(desired);
        }
    }
    /* GUIDE nao e' controle do GPTK: segue nativo como no port aprovado. */
    deliver_key(AKEY_BUTTON_MODE, buttons[SDL_CONTROLLER_BUTTON_GUIDE] ? 1 : 0);

    /* ===== Ponteiro auxiliar de menu =====
     * O contrato provado governa a separação: no menu, direito move a seta e
     * R3 clica; A confirma a seleção nativa. No gameplay, direito, R3 e A
     * seguem o jogo nativamente. */
    cursor_menu_active = st_gptk_context() == ST_GPTK_CONTEXT_MENU;
    if (!cursor_menu_active) {
        cursor_in_x = cursor_in_y = 0.0f;
        cursor_click_from_sink = 0;
    }
    cursor_click_held = cursor_menu_active && cursor_click_from_sink;

    /* ===== MotionEvent do quadro: X/Y (esquerdo), Z/RZ (direito), gatilhos
     * e HAT neutro. Fontes por eixo, sem dupla entrega:
     *   X/Y   <- partyhard.move (sink) OU LEFT_STICK native
     *   Z/RZ  <- RIGHT_STICK native (consumido pela seta: 0)
     *   L/RTRIGGER <- L2/R2 native (junto do KEYCODE_BUTTON_L2/R2 nativo)
     *   HAT   <- 0 (o D-pad deste jogo usa somente KeyEvent) */
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
    if (!st_gptk_should_consume(NXINPUT_GPTK_L2))
        lt = trigger_value[0];
    if (!st_gptk_should_consume(NXINPUT_GPTK_R2))
        rt = trigger_value[1];
    /* Duplicar esses KeyEvents como HAT causou saltos no menu e entregou duas
     * autoridades ao gameplay. O diferencial físico de 05/09/2026 provou
     * que Party Hard lê os KeyEvents e ignora HAT sozinho; mantenha zero. */
    float hx = 0.0f;
    float hy = 0.0f;
#ifdef ST_BENCH_PROBES
    probe_sdl_x = lx;
    probe_sdl_y = ly;
    probe_android_x = ax;
    probe_android_y = ay;
    probe_hat_x = hx;
    probe_hat_y = hy;
    probe_dpad_up = control_down[NXINPUT_GPTK_UP] &&
                    !st_gptk_should_consume(NXINPUT_GPTK_UP);
    probe_dpad_down = control_down[NXINPUT_GPTK_DOWN] &&
                      !st_gptk_should_consume(NXINPUT_GPTK_DOWN);
    probe_dpad_left = control_down[NXINPUT_GPTK_LEFT] &&
                      !st_gptk_should_consume(NXINPUT_GPTK_LEFT);
    probe_dpad_right = control_down[NXINPUT_GPTK_RIGHT] &&
                       !st_gptk_should_consume(NXINPUT_GPTK_RIGHT);
#endif
    int native_menu_intent = key_down_state[AKEY_DPAD_UP] ||
        key_down_state[AKEY_DPAD_DOWN] || key_down_state[AKEY_DPAD_LEFT] ||
        key_down_state[AKEY_DPAD_RIGHT] || key_down_state[AKEY_BUTTON_A] ||
        key_down_state[AKEY_BUTTON_B] || key_down_state[AKEY_BUTTON_X] ||
        key_down_state[AKEY_BUTTON_Y] || key_down_state[AKEY_BUTTON_START] ||
        key_down_state[AKEY_BACK] || fabsf(ax) > 0.25f || fabsf(ay) > 0.25f;
    int pointer_intent = cursor_menu_active &&
        (cursor_click_held || cursor_in_x * cursor_in_x +
         cursor_in_y * cursor_in_y > 0.18f * 0.18f);
    update_menu_focus(native_menu_intent, pointer_intent);
    inject(env, player, st_jni_motion_event(ax, ay, az, arz, lt, rt, hx, hy));
    update_cursor(env, player);

    if (input_diag && frame > 0 && frame % 30 == 0)
        fprintf(stderr,
                "[st/input] diag ctx=%d src=%s scene=%s deliveries=%lu "
                "sdl=%.4f,%.4f,%.4f,%.4f android=%.4f,%.4f,%.4f,%.4f "
                "hat=%.0f,%.0f dpdown=%d touch=%d\n",
                st_gptk_context(), st_gptk_context_source(), scene_name,
                st_gptk_delivery_count(), stick_axis(0), stick_axis(1),
                stick_axis(2), stick_axis(3), ax, ay, az, arz, hx, hy,
                control_down[NXINPUT_GPTK_DOWN], cursor_click_held);
}

#ifdef ST_BENCH_PROBES
/* The adapter-side numbers are not enough to prove a neutral release: this
 * game reads Assets.Scripts.Char.CharController.Hor/Vert, which can fall
 * through from InControl to UnityEngine.Input.GetAxis. Read those exact
 * properties after nativeRender, on the already-attached Unity main thread.
 * This is private measurement code and is absent from public builds. */
void st_input_post_render_probe(unsigned long frame)
{
    static int state;
    static const MethodInfo *get_hor, *get_vert;
    static float previous_hor, previous_vert;
    static int previous_active = -1;
    static unsigned neutral_tail;

    if (!input_engine_probe || frame < 1 ||
        st_gptk_context() != ST_GPTK_CONTEXT_GAMEPLAY)
        return;
    if (!state) {
        state = -1;
        if (!il2_load())
            return;
        Il2CppClass *controller_class =
            il2_class("Assets.Scripts.Char", "CharController");
        get_hor = controller_class
                ? il2_method(controller_class, "get_Hor", 0) : NULL;
        get_vert = controller_class
                 ? il2_method(controller_class, "get_Vert", 0) : NULL;
        if (!get_hor || !get_vert) {
            fprintf(stderr, "[st/input-probe] CharController.Hor/Vert unavailable\n");
            return;
        }
        state = 1;
        fprintf(stderr, "[st/input-probe] CharController.Hor/Vert ready\n");
    }
    if (state < 0)
        return;

    void *hor_value = il2_unbox(il2_call(get_hor, NULL, NULL,
                                         "CharController.get_Hor probe"));
    void *vert_value = il2_unbox(il2_call(get_vert, NULL, NULL,
                                          "CharController.get_Vert probe"));
    if (!hor_value || !vert_value) {
        fprintf(stderr, "[st/input-probe] frame=%lu invoke-failed\n", frame);
        state = -1;
        return;
    }
    float hor = *(float *)hor_value;
    float vert = *(float *)vert_value;
    int active = fabsf(probe_sdl_x) > 0.0001f ||
                 fabsf(probe_sdl_y) > 0.0001f ||
                 fabsf(probe_android_x) > 0.0001f ||
                 fabsf(probe_android_y) > 0.0001f ||
                 fabsf(probe_hat_x) > 0.0001f ||
                 fabsf(probe_hat_y) > 0.0001f ||
                 probe_dpad_up || probe_dpad_down ||
                 probe_dpad_left || probe_dpad_right;
    if (active)
        neutral_tail = 30;
    else if (neutral_tail)
        neutral_tail--;
    int changed = previous_active != active ||
                  fabsf(hor - previous_hor) > 0.0001f ||
                  fabsf(vert - previous_vert) > 0.0001f;
    if (changed || neutral_tail || frame % 60 == 0) {
        fprintf(stderr,
                "[st/input-probe] frame=%lu sdl=%.4f,%.4f "
                "android=%.4f,%.4f hat=%.0f,%.0f dpad=%d%d%d%d "
                "engine=%.4f,%.4f active=%d tail=%u\n",
                frame, probe_sdl_x, probe_sdl_y,
                probe_android_x, probe_android_y,
                probe_hat_x, probe_hat_y,
                probe_dpad_up, probe_dpad_down,
                probe_dpad_left, probe_dpad_right,
                hor, vert, active, neutral_tail);
    }
    previous_hor = hor;
    previous_vert = vert;
    previous_active = active;
}
#endif

void st_input_close(void)
{
    st_gptk_release_all("shutdown");
    release_all_keys();
    deliver_neutral_motion("shutdown");
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
    if (!cursor_is_active())
        return 0;
    /* A seta NASCE VISIVEL e fica ate' o PRIMEIRO clique (a caixa "SELECT
     * CONTROLS TYPE" e' impossivel sem ponteiro).  Depois do primeiro clique
     * ela some sozinha quando parada. */
    if (!cursor_first_click_done)
        goto visible;
    if (cursor_hide_after > 0.0f) {
        uint64_t freq = SDL_GetPerformanceFrequency();
        if (!cursor_seen_tick || !freq)
            return 0;
        double idle = (double)(SDL_GetPerformanceCounter() - cursor_seen_tick)
                    / (double)freq;
        if (idle > (double)cursor_hide_after)
            return 0;
    }
visible:
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
    return 1;
}

void st_input_set_touch_rect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    touch_origin_x = x;
    touch_origin_y = y;
    touch_width = width;
    touch_height = height;
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

/* Sonda e, se pedido, empurra o coletor do IL2CPP.  ST_GC_PROBE=1 so' mede;
 * ST_GC_EVERY=<quadros> forca uma coleta de tempos em tempos. */
void st_gc_tick(unsigned long frame)
{
    static int every = -1;
    static int probe = -1;
    if (every < 0) {
        const char *v = getenv("ST_GC_EVERY");
        every = v && *v ? atoi(v) : 0;
        const char *p = getenv("ST_GC_PROBE");
        probe = p && *p && *p != '0';
    }
    if (!every && !probe)
        return;
    if (!resolve_il2cpp_invoke_api())
        return;
    if (probe && frame % 300 == 0 && il2cpp_gc_get_used_size_p &&
        il2cpp_gc_get_heap_size_p)
        fprintf(stderr, "[st/gc] usado=%lld kB heap=%lld kB desligado=%d "
                        "incremental=%d\n",
                (long long)il2cpp_gc_get_used_size_p() / 1024,
                (long long)il2cpp_gc_get_heap_size_p() / 1024,
                il2cpp_gc_is_disabled_p ? il2cpp_gc_is_disabled_p() : -1,
                il2cpp_gc_is_incremental_p ? il2cpp_gc_is_incremental_p() : -1);
    if (every > 0 && frame % (unsigned long)every == 0 && il2cpp_gc_collect_p)
        il2cpp_gc_collect_p(0);
}
