/*
 * Native NextOS controller -> Android input bridge for Hitman GO.
 *
 * SDL normalises the physical controller, then Unity receives the same
 * KeyEvent/MotionEvent stream its Android Activity would forward.  InControl
 * remains available to the game.  An opt-in test path selects Hitman GO's own
 * tvOS IInputManager implementation, whose node selector provides true
 * controller board movement.  The right-stick pointer reproduces touch-only
 * UI: R3 starts a touch, stick movement drags it, and releasing R3 ends it.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
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

#include "bc.h"
#include "nx_elf.h"
#include "playerprefs_fix.h"

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

/* SIGTERM/SIGINT convergem no mesmo shutdown do SELECT+START: o loop de
 * render vê exit_requested e percorre pause/save/saída na ordem original. */
void bc_input_request_exit(void)
{
    exit_requested = 1;
}
static int virtual_enabled;
static float virtual_tap_x, virtual_tap_y;
static int virtual_tap_frames;
static int virtual_key_code;
static int virtual_key_frames;
static unsigned virtual_button_frames[SDL_CONTROLLER_BUTTON_MAX];
static unsigned virtual_axis_frames[SDL_CONTROLLER_AXIS_MAX];
static float virtual_axis_values[SDL_CONTROLLER_AXIS_MAX];
static int input_diag;
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
/* Auto-esconder (pedido do NextOS, 07/08/2026): a seta some depois de
 * BC_CURSOR_HIDE segundos parada e volta assim que o analógico direito se
 * mexe (ou num clique).  Visual apenas — o clique continua valendo. */
static uint64_t cursor_seen_tick;
static float cursor_hide_after = 4.0f;
static float cursor_touch_x;
static float cursor_touch_y;
static int ui_tap_release_pending;
static float ui_tap_x;
static float ui_tap_y;
/* 1: START requested the objectives overlay; 2: overlay is visibly active. */
static int menu_overlay_state;
static int shot_hotkey;
/* ===== Pause =====
 * O menu de pause do jogo (Assets/Scripts/Game/PauseMenu.cs, Show/HidePauseMenu)
 * e' TOUCH-ONLY: nao ha modulo de input de gamepad nele, entao nenhum botao o
 * fecha — so o toque no botao de resume.  Isso e' do jogo, nao do port.
 * O START abre pelo caminho nativo (KEYCODE_BUTTON_START).  Para FECHAR,
 * mandamos KEYCODE_BACK, que e' o "voltar" do Android e foi o que o NextOS viu
 * pausar/despausar quando o B ainda era BACK.  Se o jogo ignorar o BACK, o
 * segundo START tambem toca no botao de resume (BC_RESUME_XY, em coordenadas
 * de design 1280x720) — mas o BACK vem primeiro porque nao depende de posicao.
 */
static int pause_open;
static float resume_x = -1.0f, resume_y = -1.0f;

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
#define BC_GET_JOYSTICK_NAMES   0x1cec194u
#define BC_UINPUT_RAW_BUTTON    0x00edb204u
#define BC_UINPUT_RAW_ANALOG    0x00edb2b4u
#define BC_UINPUT_IS_SUPPORTED  0x00edb364u
#define BC_INPUT_IMPLEMENTATION 0x00db7e5cu
#define BC_TVOS_ON_SWIPE        0x00dbb90cu
#define BC_TVOS_CHANGE_SELECTION 0x00dbc09cu
#define BC_TVOS_SELECTION_ACTIVE 0x00dbc960u
#define BC_TVOS_CLICK_UP        0x00dbc968u
#define BC_TVOS_MENU_UP         0x00dbcd58u
#define BC_TVOS_CTOR            0x00dbcdd4u
#define BC_DEFAULT_CTOR         0x00db871cu

typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef void *(*il2cpp_string_new_fn)(const char *);
typedef void *(*il2cpp_array_new_fn)(void *, size_t);
typedef uint32_t (*il2cpp_gchandle_new_fn)(void *, int);
typedef void *(*il2cpp_object_new_fn)(void *);
typedef void *(*il2cpp_class_get_method_from_name_fn)(void *, const char *, int);
typedef void *(*il2cpp_runtime_invoke_fn)(void *, void *, void **, void **);
typedef void *(*il2cpp_class_get_type_fn)(void *);
typedef void *(*il2cpp_type_get_object_fn)(void *);
typedef void *(*il2cpp_object_unbox_fn)(void *);
typedef void *(*il2cpp_object_get_class_fn)(void *);
typedef const char *(*il2cpp_class_get_name_fn)(void *);

static il2cpp_domain_get_fn il2cpp_domain_get_p;
static il2cpp_domain_get_assemblies_fn il2cpp_domain_get_assemblies_p;
static il2cpp_assembly_get_image_fn il2cpp_assembly_get_image_p;
static il2cpp_class_from_name_fn il2cpp_class_from_name_p;
static il2cpp_string_new_fn il2cpp_string_new_p;
static il2cpp_array_new_fn il2cpp_array_new_p;
static il2cpp_gchandle_new_fn il2cpp_gchandle_new_p;
static il2cpp_object_new_fn il2cpp_object_new_p;
static il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name_p;
static il2cpp_runtime_invoke_fn il2cpp_runtime_invoke_p;
static il2cpp_class_get_type_fn il2cpp_class_get_type_p;
static il2cpp_type_get_object_fn il2cpp_type_get_object_p;
static il2cpp_object_unbox_fn il2cpp_object_unbox_p;
static il2cpp_object_get_class_fn il2cpp_object_get_class_p;
static il2cpp_class_get_name_fn il2cpp_class_get_name_p;
static void *joystick_names;

static int cursor_is_active(void)
{
    return cursor_enabled;
}

/*
 * ===== Botão de seleção =====
 * Até aqui só o R3 clicava/arrastava o cursor — pedido de campo recorrente
 * ("R3 é o que eu uso para selecionar"): um clique de menu não pode depender
 * de apertar o analógico. Agora A TAMBÉM clica, e a confirmação que o A
 * entregava ao InControl passa para o L1, de modo que nenhuma função se perde.
 * ⚠️ HERANÇA DO HITMAN GO: lá o A também clicava, porque não existe pulo.
 * No Bomb Chicken o A é AÇÃO/PULO — se ele clicar o cursor, o jogo fica
 * injogável.  Aqui o clique é SÓ o R3, como o NextOS pediu em 07/08/2026.
 * BC_CLICK_A=1 devolve o comportamento do Hitman GO.
 */
static int click_uses_a = 0;

/*
 * ===== Layout dos analógicos =====
 * ESQUERDO anda, DIREITO é o cursor (o D-pad continua movendo sempre).
 *
 * ⚠️ HERANÇA DO HITMAN GO: a base estrutural deste port veio do ports/hitmango,
 * onde o layout é o INVERSO (cursor no esquerdo, tabuleiro no direito) porque
 * lá o jogo é de tabuleiro.  Copiado às cegas, isso fazia o personagem do Bomb
 * Chicken ANDAR COM O ANALÓGICO DIREITO — reportado pelo NextOS em 07/08/2026.
 * Bomb Chicken é plataforma: movimento no esquerdo, sempre.
 * BC_SWAP_STICKS=1 devolve o layout do Hitman GO.
 */
static int swap_sticks = 0;

static SDL_GameControllerAxis move_axis(int vertical)
{
    if (swap_sticks)
        return vertical ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX;
    return vertical ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX;
}

static SDL_GameControllerAxis cursor_axis(int vertical)
{
    if (swap_sticks)
        return vertical ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX;
    return vertical ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX;
}

/* Clique do cursor: A e R3, SEMPRE (pedido do NextOS).  Com a mira de pedra
 * aberta (seleção nativa tvOS) o A deixa de ser clique e volta a ser o botão
 * de arremesso — era assim na v1.1.0 aprovada; a v1.1.1 mandou o arremesso
 * para o L1 e a pedra "parou de sair". */
static int cursor_click_held(void)
{
    return buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK] ||
           (click_uses_a && !native_selection_active &&
            buttons[SDL_CONTROLLER_BUTTON_A]);
}

static int cursor_click_prev(void)
{
    return previous[SDL_CONTROLLER_BUTTON_RIGHTSTICK] ||
           (click_uses_a && !native_selection_active &&
            previous[SDL_CONTROLLER_BUTTON_A]);
}

/* O A vira botão de clique quando o cursor está ativo; nesse modo a antiga
   confirmação do A é servida pelo L1 (livre neste jogo).  Na mira de pedra o
   A é devolvido ao jogo como arremesso. */
static int a_is_click_button(void)
{
    return click_uses_a && cursor_is_active() && !native_selection_active;
}

static int bc_incontrol_button(void *self, int index, void *method)
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

static float bc_incontrol_analog(void *self, int index, void *method)
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

static int bc_incontrol_supported(void *self, void *method)
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

/* Resolver os EXPORTS do il2cpp e' so' lookup de simbolo — inofensivo.  O que
 * fica atras do gate BC_IL2CPP_HOOKS e' o patch de RVA (codigo de outro jogo),
 * nunca isto.  Chamado tarde (gameplay ja' rodando), nunca antes do frame 0 —
 * entrar em il2cpp_domain_get cedo demais mata a Unity (ver
 * bc_get_native_input_implementation). */
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
    return il2cpp_runtime_invoke_p && il2cpp_class_get_method_from_name_p &&
           il2cpp_class_get_type_p && il2cpp_type_get_object_p &&
           il2cpp_domain_get_p && il2cpp_domain_get_assemblies_p &&
           il2cpp_assembly_get_image_p && il2cpp_class_from_name_p;
}

/* Oraculo de pause: Time.timeScale == 0 quando o jogo esta' pausado.  E' o
 * estado do PROPRIO jogo — nao depende de paridade de START (que dessincroniza
 * ja' no titulo, onde START comeca o jogo em vez de pausar).  Devolve 1 pausado,
 * 0 rodando, -1 quando a API nao esta' disponivel (dai vale a paridade). */
static int bc_pause_is_open(void)
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

/* Achar a instancia viva de uma classe do jogo via
 * UnityEngine.Object.FindObjectOfType(typeof(K)). */
static void *bc_find_instance(void *klass)
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

/* Como bc_find_instance, mas enxerga objetos INATIVOS — o painel do
 * PauseMenu vive desligado (SetActive(false)) ate' alguem mostra'-lo, e o
 * FindObjectOfType de um argumento nao encontra inativos.  Unity 2020+
 * tem a sobrecarga FindObjectOfType(Type, bool includeInactive). */
static void *bc_find_instance_inactive(void *klass)
{
    void *type_obj = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!type_obj)
        return NULL;
    /* Unity 2020+: FindObjectOfType(Type, bool includeInactive). */
    void *object_class = find_managed_class("UnityEngine", "Object");
    void *find_by_type = object_class
        ? il2cpp_class_get_method_from_name_p(object_class,
                                              "FindObjectOfType", 2)
        : NULL;
    if (find_by_type) {
        uint8_t include_inactive = 1;
        void *exc = NULL;
        void *args[2] = { type_obj, &include_inactive };
        void *instance = il2cpp_runtime_invoke_p(find_by_type, NULL, args,
                                                 &exc);
        if (!exc && instance)
            return instance;
    }
    /* Fallback: Resources.FindObjectsOfTypeAll(Type) enxerga inativos em
     * qualquer versao; o array il2cpp guarda o tamanho em +0x18 e os
     * elementos a partir de +0x20. */
    void *resources_class = find_managed_class("UnityEngine", "Resources");
    void *find_all = resources_class
        ? il2cpp_class_get_method_from_name_p(resources_class,
                                              "FindObjectsOfTypeAll", 1)
        : NULL;
    if (!find_all)
        return NULL;
    void *exc = NULL;
    void *args[1] = { type_obj };
    void *array = il2cpp_runtime_invoke_p(find_all, NULL, args, &exc);
    if (exc || !array)
        return NULL;
    int64_t length = *(int64_t *)((uint8_t *)array + 0x18);
    if (length <= 0)
        return NULL;
    return ((void **)((uint8_t *)array + 0x20))[0];
}

/* Ha' um Player vivo? (classe global "Player" do jogo, nao a do Rewired —
 * namespace vazio nao casa com "Rewired").  Player so' existe com nivel
 * carregado, entao isto e' o gate "estamos jogando" para dar ao A o papel de
 * botar bomba sem contaminar os menus. */
static int bc_player_alive(void)
{
    if (!resolve_il2cpp_invoke_api())
        return 0;
    void *player_class = find_managed_class("", "Player");
    return player_class && bc_find_instance(player_class) != NULL;
}

/* O pause DO JOGO: LevelStart.PausePressed() e' o toggle oficial (abre E
 * fecha), com toda a logica de estado do proprio jogo — e' o que um botao de
 * pause chamaria.  O menu que ele abre (PauseMenu) e' touch-only, entao sem
 * isto nenhum botao fecha o pause.  Devolve 1 quando conseguiu chamar;
 * 0 manda o chamador cair no fallback (BACK/toque).  Fora de nivel (titulo)
 * nao ha LevelStart vivo e devolvemos 0 — o START segue como tecla normal. */
static int bc_pause_hide_native(void);
static int bc_pause_show_native(void);

static int bc_pause_toggle_native(void)
{
    const char *flag = getenv("BC_PAUSE_NATIVE");
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
        fprintf(stderr, "[bc/pause] LevelStart.PausePressed() ausente\n");
        return 0;
    }
    void *instance = bc_find_instance(level_class);
    if (!instance) {
        if (input_diag)
            fprintf(stderr, "[bc/pause] sem LevelStart vivo (fora de nivel)\n");
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
        /* Excecao tardia (analytics/ads) NAO desfaz o toggle — logar e seguir
           decidindo pelo estado, como no caminho sem excecao. */
        const char *what = (il2cpp_object_get_class_p && il2cpp_class_get_name_p)
            ? il2cpp_class_get_name_p(il2cpp_object_get_class_p(exc))
            : "?";
        fprintf(stderr, "[bc/pause] PausePressed lancou %s (seguindo)\n",
                what ? what : "?");
    }
    if (get_paused && il2cpp_object_unbox_p) {
        exc = NULL;
        void *boxed = il2cpp_runtime_invoke_p(get_paused, instance, NULL, &exc);
        uint8_t *state = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
        now_paused = state && *state;
    }
    if (input_diag)
        fprintf(stderr, "[bc/pause] PausePressed() IsPaused %d -> %d\n",
                was_paused, now_paused);
    /* No celular o botao ‖ da UI liga DOIS listeners: PausePressed (trava o
       tempo) e ShowPauseMenu (SetActive do painel — puro UI, medido no dump:
       nao mexe em timeScale).  So' invocar PausePressed pausava o jogo com a
       tela crua, sem menu — relato do NextOS e da comunidade.  Espelhar o
       segundo listener nos dois sentidos. */
    if (!was_paused && now_paused) {
        if (!bc_pause_show_native())
            fprintf(stderr, "[bc/pause] menu de pause nao exibido\n");
    } else if (was_paused && !now_paused) {
        bc_pause_hide_native();
    }
    /* Fora de nivel (titulo/intro) o PausePressed e' no-op e o estado nao
       muda: devolver 0 deixa o START seguir como tecla normal (comecar o
       jogo no titulo). */
    return was_paused || now_paused;
}

/* Fechar o pause pelo METODO DO PROPRIO JOGO.  O PauseMenu e' touch-only (sem
 * modulo de gamepad), entao tecla nenhuma o fecha; em vez de chutar coordenada
 * de toque, invocamos Assets/Scripts/Game/PauseMenu.HidePauseMenu() na
 * instancia viva — exatamente o que o botao de resume faria.  Roda na mesma
 * thread do nativeRender (a que executa o managed), entao runtime_invoke e'
 * seguro aqui.  BC_PAUSE_NATIVE=0 desliga para diagnostico. */
/* Exibir o menu de pause pelo metodo do proprio jogo: e' o segundo listener
 * do botao ‖ da UI (ShowPauseMenu = SetActive do painel, sem tocar no
 * timeScale — conferido no dump).  Mesmo gate do resto do pause nativo. */
static int bc_pause_show_native(void)
{
    const char *flag = getenv("BC_PAUSE_NATIVE");
    if (!flag || !*flag || strcmp(flag, "0") == 0)
        return 0;
    if (!resolve_il2cpp_invoke_api())
        return 0;
    void *pause_class = find_managed_class("", "PauseMenu");
    void *show = pause_class
        ? il2cpp_class_get_method_from_name_p(pause_class, "ShowPauseMenu", 0)
        : NULL;
    void *instance = pause_class ? bc_find_instance(pause_class) : NULL;
    if (!instance && pause_class)
        instance = bc_find_instance_inactive(pause_class);
    if (!show || !instance) {
        fprintf(stderr, "[bc/pause] ShowPauseMenu/instancia ausente\n");
        return 0;
    }
    void *exc = NULL;
    il2cpp_runtime_invoke_p(show, instance, NULL, &exc);
    if (exc) {
        fprintf(stderr, "[bc/pause] ShowPauseMenu lancou excecao\n");
        return 0;
    }
    fprintf(stderr, "[bc/pause] ShowPauseMenu() invocado no jogo\n");
    return 1;
}

static int bc_pause_hide_native(void)
{
    const char *flag = getenv("BC_PAUSE_NATIVE");
    if (!flag || !*flag || strcmp(flag, "0") == 0)
        return 0;
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[bc/pause] il2cpp invoke API indisponivel\n");
        return 0;
    }
    void *pause_class = find_managed_class("", "PauseMenu");
    if (!pause_class) {
        fprintf(stderr, "[bc/pause] classe PauseMenu nao encontrada\n");
        return 0;
    }
    void *hide = il2cpp_class_get_method_from_name_p(pause_class,
                                                     "HidePauseMenu", 0);
    if (!hide) {
        fprintf(stderr, "[bc/pause] HidePauseMenu ausente\n");
        return 0;
    }
    void *instance = bc_find_instance(pause_class);
    if (!instance) {
        fprintf(stderr, "[bc/pause] instancia de PauseMenu nao achada\n");
        return 0;
    }
    void *exc = NULL;
    il2cpp_runtime_invoke_p(hide, instance, NULL, &exc);
    if (exc) {
        fprintf(stderr, "[bc/pause] HidePauseMenu lancou excecao\n");
        return 0;
    }
    fprintf(stderr, "[bc/pause] HidePauseMenu() invocado no jogo\n");
    return 1;
}

static void *bc_get_native_input_implementation(void *self, void *method)
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
             (il2cpp_base + BC_DEFAULT_CTOR))(fallback, NULL);
            native_controls_enabled = 0;
            fprintf(stderr,
                    "[bc/input] tvOS class missing; Android input preserved\n");
        }
        return fallback;
    }

    void *manager = il2cpp_object_new_p(native_input_class);
    if (!manager)
        return NULL;
    ((void (*)(void *, void *))(il2cpp_base + BC_TVOS_CTOR))(manager, NULL);
    native_input_manager = manager;
    native_activity_reported = -1;
    fprintf(stderr,
            "[bc/input] native node controls: InputManager_tvOS selected\n");
    return manager;
}

static void *bc_get_joystick_names(void *method)
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
    fprintf(stderr, "[bc/input] InControl sees Microsoft X-Box 360 pad\n");
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

/* Dispara o fim de fase pelo caminho do proprio jogo, para provar o conserto
 * sem ter de jogar ate' o fim da fase (token `chk`, so' com BC_GPVIRT). */
static void bc_debug_goto_checkpoint(int load_next)
{
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[bc/dbg] il2cpp indisponivel\n");
        return;
    }
    void *level_class = find_managed_class("", "LevelStart");
    void *go = level_class
        ? il2cpp_class_get_method_from_name_p(level_class, "GoToCheckpoint", 1)
        : NULL;
    void *instance = level_class ? bc_find_instance(level_class) : NULL;
    if (!go || !instance) {
        fprintf(stderr, "[bc/dbg] GoToCheckpoint indisponivel (fora de nivel?)\n");
        return;
    }
    uint8_t flag = load_next ? 1 : 0;
    void *args[1] = { &flag };
    void *exc = NULL;
    il2cpp_runtime_invoke_p(go, instance, args, &exc);
    fprintf(stderr, "[bc/dbg] GoToCheckpoint(%d) -> %s\n", load_next,
            exc ? "EXCECAO" : "ok");
}

static void il2cpp_string_ascii(void *string, char *out, size_t size);

/* Sonda do resume (token `sav`, so' com BC_GPVIRT): imprime o que o jogo
 * responde por GetStartingWorld()/GetStartingGroup() e o que esta' persistido
 * nas nossas prefs — o delta aponta onde o "voltou pro tutorial" se perde. */
static void bc_debug_save_probe(void)
{
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[bc/dbg] il2cpp indisponivel\n");
        return;
    }
    char world[128] = "?", group[128] = "?";
    bc_prefs_get_string("START_WORLD", world, sizeof world);
    bc_prefs_get_string("START_GROUP", group, sizeof group);
    fprintf(stderr, "[bc/sav] prefs: START_WORLD='%s' START_GROUP='%s'\n",
            world, group);

    void *level_class = find_managed_class("", "LevelStart");
    void *instance = level_class ? bc_find_instance(level_class) : NULL;
    if (!instance) {
        fprintf(stderr, "[bc/sav] sem LevelStart vivo\n");
        return;
    }
    static const char *const getters[] = {
        "GetStartingWorld", "GetStartingGroup", "GetCurrentWorld",
        "get_CurrentGroupID",
    };
    for (size_t i = 0; i < sizeof getters / sizeof *getters; i++) {
        void *method = il2cpp_class_get_method_from_name_p(level_class,
                                                           getters[i], 0);
        if (!method) {
            fprintf(stderr, "[bc/sav] %s ausente\n", getters[i]);
            continue;
        }
        void *exc = NULL;
        void *result = il2cpp_runtime_invoke_p(method, instance, NULL, &exc);
        char text[128] = "";
        if (!exc && result)
            il2cpp_string_ascii(result, text, sizeof text);
        fprintf(stderr, "[bc/sav] %s() -> '%s'%s\n", getters[i], text,
                exc ? " EXCECAO" : "");
    }
    /* Os campos da cena que vencem as prefs quando nao-vazios. */
    void *starting_world = *(void **)((uint8_t *)instance + 0x50);
    void *starting_group = *(void **)((uint8_t *)instance + 0x58);
    char sw[128] = "", sg[128] = "";
    il2cpp_string_ascii(starting_world, sw, sizeof sw);
    il2cpp_string_ascii(starting_group, sg, sizeof sg);
    fprintf(stderr, "[bc/sav] cena: m_StartingWorld='%s' m_StartingGroup='%s'\n",
            sw, sg);
}

/* Sonda a FollowCam ao vivo (token `cam`): quais referencias dela estao nulas
 * quando a tela fica preta depois do checkpoint.  Offsets do dump deste jogo:
 * m_Target 0x20, m_CamComponent 0x38, m_Following 0x44, m_CurrentLevel 0x70. */
static void bc_debug_camera(void)
{
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[bc/dbg] il2cpp indisponivel\n");
        return;
    }
    void *cam_class = find_managed_class("", "FollowCam");
    void *cam = cam_class ? bc_find_instance(cam_class) : NULL;
    if (!cam) {
        fprintf(stderr, "[bc/dbg] FollowCam ausente\n");
        return;
    }
    uint8_t *c = cam;
    void *player_class = find_managed_class("", "Player");
    void *player = player_class ? bc_find_instance(player_class) : NULL;
    void *level_class = find_managed_class("", "Level");
    void *level_any = level_class ? bc_find_instance(level_class) : NULL;
    fprintf(stderr,
            "[bc/dbg] FollowCam target=%p camera=%p level=%p following=%d "
            "| Player=%p LevelNaCena=%p\n",
            *(void **)(c + 0x20), *(void **)(c + 0x38), *(void **)(c + 0x70),
            c[0x44], player, level_any);
}

/* ===== Fim de fase: LevelStart.UpdateLevelCompletion reescrito =============
 *
 * Terminar uma fase leva o Teleporter a rodar a corrotina TeleportToCheckpoint,
 * que chama LevelStart.GoToCheckpoint -> CheckpointUnlocked ->
 * UpdateLevelCompletion.  Esse ultimo le a chave "Progress" do PlayerPrefs,
 * quebra em linhas por virgula e cada linha em `mundo-grupo-completude` por
 * traco — e indexa a linha SEM conferir o formato.  A string termina em
 * virgula, entao a ultima linha e' VAZIA: quando o grupo procurado nao aparece
 * antes dela, o `linha[1]` estoura em IndexOutOfRangeException.  A corrotina
 * morre no meio do teleporte: a fase nunca carrega, o jogador ja' foi destruido
 * e a FollowCam passa a estourar NullReference todo quadro — a TELA PRETA que o
 * NextOS viu ao terminar a fase (07/08/2026).
 *
 * Nao da' para consertar C# compilado, mas o dado e' NOSSO: o PlayerPrefs vive
 * no shim deste port.  Entao o corpo do metodo passa a ser esta funcao, que faz
 * o mesmo trabalho em C — atualizar a completude do grupo, mantendo o maior
 * valor — e que simplesmente nao tem como estourar.  Linhas malformadas (a
 * vazia do fim, e o "0---0" que aparecia no save) sao descartadas na volta, de
 * modo que o proprio parser do jogo nunca mais as veja.
 *
 * RVA lido do dump DESTE jogo (Il2CppDumper sobre o nosso libil2cpp.so +
 * global-metadata.dat), nunca herdado de outro port.  BC_PROGRESS_FIX=0
 * devolve o comportamento original para diagnostico.
 */
#define BC_UPDATE_LEVEL_COMPLETION 0x0107ce0cu

/* System.String do IL2CPP: comprimento em +0x10, UTF-16 em +0x14. */
static void il2cpp_string_ascii(void *string, char *out, size_t size)
{
    if (!size)
        return;
    out[0] = '\0';
    if (!string)
        return;
    int32_t length = *(int32_t *)((uint8_t *)string + 0x10);
    const uint16_t *chars = (const uint16_t *)((uint8_t *)string + 0x14);
    size_t n = 0;
    for (int32_t i = 0; i < length && n + 1 < size; i++)
        out[n++] = chars[i] < 0x80 ? (char)chars[i] : '?';
    out[n] = '\0';
}

static void bc_update_level_completion(void *self, int32_t completion,
                                       void *group_string, void *method)
{
    (void)self;
    (void)method;

    char group[64];
    il2cpp_string_ascii(group_string, group, sizeof group);
    char progress[8192];
    if (!group[0] || !bc_prefs_get_string("Progress", progress, sizeof progress))
        return;

    /* A Unity urlencoda o valor no Android: a virgula chega como %2C. */
    const char *sep = strstr(progress, "%2C") ? "%2C" : ",";
    size_t sep_length = strlen(sep);

    char rebuilt[8192];
    size_t out = 0;
    int changed = 0;
    for (const char *p = progress;;) {
        const char *end = strstr(p, sep);
        size_t row_length = end ? (size_t)(end - p) : strlen(p);
        char row[256];
        int well_formed = 0;
        if (row_length && row_length < sizeof row) {
            memcpy(row, p, row_length);
            row[row_length] = '\0';
            char *first = strchr(row, '-');
            char *second = first ? strchr(first + 1, '-') : NULL;
            if (first && second && !strchr(second + 1, '-')) {
                *first = *second = '\0';
                const char *world = row;
                const char *id = first + 1;
                int value = atoi(second + 1);
                if (strcmp(id, group) == 0 && completion > value) {
                    value = completion;
                    changed = 1;
                }
                if (out + 128 < sizeof rebuilt) {
                    out += (size_t)snprintf(rebuilt + out, sizeof rebuilt - out,
                                            "%s%s-%s-%d", out ? sep : "",
                                            world, id, value);
                    well_formed = 1;
                }
            }
        }
        if (!well_formed && row_length)
            changed = 1;   /* linha malformada: some, era ela que estourava */
        if (!end)
            break;
        if (!row_length)
            changed = 1;   /* virgula final: a linha vazia tambem some */
        p = end + sep_length;
    }

    if (changed && bc_prefs_set_string("Progress", rebuilt))
        fprintf(stderr, "[bc/save] fim de fase: grupo %s -> %d\n",
                group, completion);
}

/* UnityEngine.PlayerPrefs::GetString devolve SEMPRE "" no managed neste
 * ambiente: o JNI entrega o valor certo (medido: getString("START_WORLD") ->
 * "Skull_World" no shim) e a libunity descarta o jstring antes de converter.
 * Ints/bools passam; so' o RETORNO de string se perde.  E' o que quebrava o
 * resume ("voltou pro tutorial"), o Continue do menu e os mapas do Rewired.
 *
 * Conserto no nivel do jogo, mesma tecnica do UpdateLevelCompletion: o corpo
 * do wrapper icall (RVA do dump DESTE jogo) passa a ler o NOSSO store — a
 * mesma tabela que o putString/apply do jogo ja alimenta.  Valores ficam
 * urlencodados no store (marca do PlayerPrefs v2); decodificar ao devolver.
 *
 * ATENCAO A ABI DOS OVERLOADS: no IL2CPP, os argumentos C# vêm primeiro e o
 * MethodInfo* oculto vem por ultimo. O RVA 0x21922A0 e' GetString(key), logo
 * sua ABI tem DOIS ponteiros; trata-lo como GetString(key, defaultValue,
 * MethodInfo*) faz uma chave ausente devolver o MethodInfo* como System.String.
 * Isso passou com save populado e deixou New Game preto num save limpo.
 *
 * Os dois corpos e ABIs ficam separados e cobertos por teste host:
 *   0x219225C GetString(string key, string defaultValue)
 *   0x21922A0 GetString(string key)
 * Nunca selecionar patch IL2CPP somente pelo nome do metodo. */
#define BC_PLAYERPREFS_GETSTRING_DEFAULT 0x219225cu
#define BC_PLAYERPREFS_GETSTRING_EMPTY   0x21922a0u

static void install_playerprefs_string_fix(nx_mod *il2cpp)
{
    const char *flag = getenv("BC_PREFS_STRING_FIX");
    if (!flag || !*flag || strcmp(flag, "0") == 0) {
        fprintf(stderr, "[bc/save] conserto do GetString nao habilitado "
                        "pelo manifesto\n");
        return;
    }
    /* O resto do resolve vive em resolve_il2cpp_invoke_api; string_new so'
       era resolvido no bloco de bring-up (BC_IL2CPP_HOOKS) e ficava NULL na
       release — resolver aqui, onde o conserto precisa dele. */
    if (!il2cpp_string_new_p)
        il2cpp_string_new_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_string_new");
    if (!il2cpp_string_new_p) {
        fprintf(stderr, "[bc/save] il2cpp_string_new ausente; conserto do "
                        "GetString desarmado\n");
        return;
    }
    bc_playerprefs_fix_configure(bc_prefs_get_string, il2cpp_string_new_p,
                                 il2cpp_string_ascii);
    replace_body(il2cpp->base, BC_PLAYERPREFS_GETSTRING_DEFAULT,
                 (void *)bc_playerprefs_getstring_default_hook);
    replace_body(il2cpp->base, BC_PLAYERPREFS_GETSTRING_EMPTY,
                 (void *)bc_playerprefs_getstring_empty_hook);
    fprintf(stderr,
            "[bc/save] PlayerPrefs.GetString lendo o store persistido "
            "(2 overloads)\n");
}

/* Bomb Chicken uses Rewired, not InControl, and the RVAs above belong to a
 * different game's libil2cpp.  Patching them here would smash unrelated code
 * (armadilha 13/17).  The hooks stay compiled but are only armed when
 * BC_IL2CPP_HOOKS explicitly asks for them during bring-up. */
/* Instalado assim que o libil2cpp esta' mapeado (frame 1), fora do gate
 * BC_IL2CPP_HOOKS — este RVA e' do proprio jogo, nao herdado de outro port. */
static void install_progress_fix(void)
{
    static int done;
    if (done)
        return;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    done = 1;
    install_playerprefs_string_fix(il2cpp);
    const char *flag = getenv("BC_PROGRESS_FIX");
    if (!flag || !*flag || strcmp(flag, "0") == 0) {
        fprintf(stderr,
                "[bc/save] conserto de fim de fase nao habilitado pelo manifesto\n");
        return;
    }
    replace_body(il2cpp->base, BC_UPDATE_LEVEL_COMPLETION,
                 (void *)bc_update_level_completion);
    fprintf(stderr,
            "[bc/save] UpdateLevelCompletion reescrito (fim de fase nao trava)\n");
}

static void install_incontrol_hooks(void)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    if (!getenv("BC_IL2CPP_HOOKS"))
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

    replace_body(il2cpp->base, BC_GET_JOYSTICK_NAMES,
                 bc_get_joystick_names);
    replace_body(il2cpp->base, BC_UINPUT_RAW_BUTTON,
                 bc_incontrol_button);
    replace_body(il2cpp->base, BC_UINPUT_RAW_ANALOG,
                 bc_incontrol_analog);
    replace_body(il2cpp->base, BC_UINPUT_IS_SUPPORTED,
                 bc_incontrol_supported);
    if (native_controls_enabled && il2cpp_object_new_p &&
        il2cpp_domain_get_p && il2cpp_domain_get_assemblies_p &&
        il2cpp_assembly_get_image_p && il2cpp_class_from_name_p) {
        replace_body(il2cpp->base, BC_INPUT_IMPLEMENTATION,
                     bc_get_native_input_implementation);
        fprintf(stderr,
                "[bc/input] native node-control selector armed\n");
    }
    fprintf(stderr,
            "[bc/input] Hitman GO InControl hooks installed from own metadata\n");
}

static const int android_key[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = 96,              /* KEYCODE_BUTTON_A */
    /* ⚠️ Era KEYCODE_BACK (4), o "voltar" do Android — herança do Hitman GO,
       onde B fecha tela.  A Unity trata BACK como pause/menu, então no Bomb
       Chicken o botao X do pad (b2 = 'b' no es_input.cfg) PAUSAVA o jogo em
       vez de agir.  Reportado pelo NextOS em 07/08/2026.  Agora vai como
       botao de jogo de verdade.  BC_B_IS_BACK=1 devolve o comportamento antigo
       se alguma tela precisar do voltar. */
    [SDL_CONTROLLER_BUTTON_B] = 97,              /* KEYCODE_BUTTON_B */
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

/* L2/R2 nao tinham keycode nenhum: existiam so como eixo. Neste pad eles sao
   BOTOES (b6/b7), entao viram KEYCODE_BUTTON_L2/R2 e o jogo ganha dois botoes.
   Preenchido em bc_input_init para nao brigar com o inicializador estatico. */
static int android_key_rt[SDL_CONTROLLER_BUTTON_MAX];

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

/* Approved-port bring-up path: one token written to /tmp/bcgp becomes a
 * short native-controller pulse.  This never enters the touch/mouse path and
 * is inactive unless BC_GPVIRT is explicitly enabled for a test launch. */
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

    FILE *input = fopen("/tmp/bcgp", "r");
    if (input) {
        char token[24] = { 0 };
        int have_token = fscanf(input, "%23s", token) == 1 && token[0];
        fclose(input);
        unlink("/tmp/bcgp");
        if (have_token) {
            unsigned duration = 6;
            const char *duration_value = getenv("BC_GPVDUR");
            if (duration_value && *duration_value) {
                long parsed = strtol(duration_value, NULL, 10);
                if (parsed > 0 && parsed <= 600)
                    duration = (unsigned)parsed;
            }
            /* A per-pulse suffix (for example r3:60 or rx+:12) makes the
             * disabled-by-default virtual test controller precise enough to
             * validate hold-and-drag gestures without affecting players. */
            char *duration_separator = strncasecmp(token, "tap:", 4)
                                     ? strrchr(token, ':') : NULL;
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
            } else if (!strcasecmp(token, "cam")) {
                bc_debug_camera();
            } else if (!strcasecmp(token, "chk")) {
                /* Reproduz o fim de fase sem jogar: a mesma chamada que a
                   corrotina do Teleporter faz.  So' com BC_GPVIRT. */
                bc_debug_goto_checkpoint(1);
            } else if (!strcasecmp(token, "sav")) {
                /* Sonda do resume: o que o jogo VE^ (GetStartingWorld/Group)
                   contra o que esta' persistido.  So' com BC_GPVIRT. */
                bc_debug_save_probe();
            } else if (!strncasecmp(token, "key:", 4)) {
                /* key:N — injeta um KEYCODE Android arbitrario (bring-up). */
                virtual_key_code = atoi(token + 4);
                virtual_key_frames = 3;
            } else if (!strncasecmp(token, "tap:", 4)) {
                /* tap:X,Y em coordenadas de design 1280x720 — toque direto,
                   so' para bring-up (BC_GPVIRT). */
                float dx = 0, dy = 0;
                if (sscanf(token + 4, "%f,%f", &dx, &dy) == 2) {
                    virtual_tap_x = dx * screen_width / 1280.0f;
                    virtual_tap_y = dy * screen_height / 720.0f;
                    virtual_tap_frames = 3;
                } else {
                    matched = 0;
                }
            } else {
                matched = 0;
            }
            fprintf(stderr, "[bc/input] virtual pulse %s x%u (%s)\n",
                    token, duration, matched ? "accepted" : "unknown");
        }
    }

    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        if (virtual_button_frames[i] > 0)
            buttons[i] = 1;
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
                    "[bc/input] %s has no physical SELECT/START; "
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
        int vendor = joy ? SDL_JoystickGetVendor(joy) : 0;
        int product = joy ? SDL_JoystickGetProduct(joy) : 0;
        bc_jni_input_device_info("Microsoft X-Box 360 pad", vendor, product,
                                  physical ? physical : "nxcompat-gamepad");
        fprintf(stderr, "[bc/input] controller: %s (%04x:%04x)\n",
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
            bc_jni_input_device_info("Microsoft X-Box 360 pad",
                                      SDL_JoystickGetVendor(raw_joystick),
                                      SDL_JoystickGetProduct(raw_joystick),
                                      name ? name : "nxcompat-gamepad");
            fprintf(stderr,
                    "[bc/input] controle CRU: \"%s\" (%d botões, %d eixos, "
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
        native_inject = bc_jni_native("com/unity3d/player/UnityPlayer",
                                       "nativeInjectEvent");
    if (native_inject && event) {
        /* Unity 2022 registers nativeInjectEvent(InputEvent, displayId).
         * The Android Activity passes its default display (0); omitting this
         * fourth native argument leaves an arbitrary register value that the
         * touch scaler later treats as an array index. */
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (input_diag)
            fprintf(stderr, "[bc/input] inject event=%p consumed=%d\n",
                    event, consumed);
    } else if (input_diag) {
        fprintf(stderr, "[bc/input] inject SKIPPED inject=%p event=%p\n",
                native_inject, event);
    }
}

static void update_cursor(void *env, void *player)
{
    if (!cursor_is_active() || (!controller && !raw_joystick && !virtual_enabled))
        return;

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = cursor_tick && frequency
             ? (float)((double)(now - cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;

    float x = axis_value(cursor_axis(0));
    float y = axis_value(cursor_axis(1));
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
    if (magnitude > deadzone || cursor_click_held())
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

    int held = cursor_click_held();
    int down = held && !cursor_click_prev();
    int up = !held && cursor_click_prev();
    float touch_x = cursor_x * screen_width / 1280.0f;
    float touch_y = cursor_y * screen_height / 720.0f;
    if (down) {
        inject(env, player, bc_jni_touch_event(0, touch_x, touch_y));
        cursor_drag_active = 1;
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
        pause_open = 0;   /* clicou na tela: o estado do pause deixa de ser nosso */
        if (input_diag)
            fprintf(stderr, "[bc/touch] DOWN %.0f,%.0f\n", touch_x, touch_y);
    } else if (held && cursor_drag_active &&
               (fabsf(touch_x - cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - cursor_touch_y) >= 0.25f)) {
        inject(env, player, bc_jni_touch_event(2, touch_x, touch_y));
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
    }
    if (up && cursor_drag_active) {
        inject(env, player, bc_jni_touch_event(1, touch_x, touch_y));
        cursor_drag_active = 0;
        if (input_diag)
            fprintf(stderr, "[bc/touch] UP   %.0f,%.0f\n", touch_x, touch_y);
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
         (il2cpp_base + BC_TVOS_SELECTION_ACTIVE))(native_input_manager,
                                                     NULL) != 0;
    native_gameplay_active = native_selection_active ||
        (*((uint8_t *)native_input_manager + 0x70) != 0);
    if (native_gameplay_active != native_activity_reported) {
        fprintf(stderr,
                "[bc/input] native gameplay=%d selection=%d\n",
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
        ((void (*)(void *, void *))(il2cpp_base + BC_TVOS_CLICK_UP))(
            native_input_manager, NULL);
        fprintf(stderr, "[bc/input] native selection activate\n");
    }
    if (native_selection_active &&
        ((buttons[SDL_CONTROLLER_BUTTON_B] &&
         !previous[SDL_CONTROLLER_BUTTON_B]) ||
        (buttons[SDL_CONTROLLER_BUTTON_BACK] &&
         !previous[SDL_CONTROLLER_BUTTON_BACK]))) {
        ((void (*)(void *, void *))(il2cpp_base + BC_TVOS_MENU_UP))(
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
         (il2cpp_base + BC_TVOS_CHANGE_SELECTION))(
            native_input_manager, x, y, face_towards_selection, NULL);
        if (input_diag)
            fprintf(stderr,
                    "[bc/input] native selection direction %.2f %.2f\n",
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
         (il2cpp_base + BC_TVOS_ON_SWIPE))(
            native_input_manager, x * 120.0f, y * 120.0f, NULL);
        if (input_diag)
            fprintf(stderr, "[bc/input] native pawn swipe %.2f %.2f\n",
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
 * tvOS continua atrás de BC_NATIVE_CONTROLS=1 para comparação.
 */
static int swipe_move_enabled = 1;
static int swipe_step;          /* 0 = ocioso; conta os quadros do gesto */
static float swipe_from_x, swipe_from_y, swipe_dx, swipe_dy;
static int swipe_latched;

static void update_swipe_move(void *env, void *player)
{
    if (!swipe_move_enabled || native_controls_enabled)
        return;
    /* nunca por cima de um clique/arraste do cursor: é o mesmo dedo */
    if (cursor_drag_active || cursor_click_held() || ui_tap_release_pending)
        return;

    if (swipe_step > 0) {
        float t = (float)swipe_step / 4.0f;
        int action = swipe_step >= 4 ? 1 : 2;  /* 4 = solta, 1..3 = arrasta */
        inject(env, player,
               bc_jni_touch_event(action, swipe_from_x + swipe_dx * t,
                                   swipe_from_y + swipe_dy * t));
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
    swipe_latched = 1;

    float magnitude = sqrtf(x * x + y * y);
    float length = 0.22f * (float)(screen_width < screen_height
                                   ? screen_width : screen_height);
    swipe_from_x = screen_width * 0.5f;
    swipe_from_y = screen_height * 0.55f;
    swipe_dx = x / magnitude * length;
    swipe_dy = -y / magnitude * length;   /* tela cresce para baixo */
    inject(env, player, bc_jni_touch_event(0, swipe_from_x, swipe_from_y));
    swipe_step = 1;
    if (input_diag)
        fprintf(stderr, "[bc/input] swipe-move %.2f %.2f\n", x, y);
}

static void update_gameplay_shortcuts(void *env, void *player)
{
    if (ui_tap_release_pending) {
        inject(env, player, bc_jni_touch_event(1, ui_tap_x, ui_tap_y));
        ui_tap_release_pending = 0;
    }
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
        menu_overlay_state = 1;
    }
    /* ⚠️ REMOVIDO: os atalhos "restart" (1122,70) e "hint" (1209,649) eram as
       COORDENADAS DA UI DO HITMAN GO.  Em outro jogo isso e' toque em lugar
       aleatorio — a armadilha nº2 do Tightrope.  Bomb Chicken usa os botoes
       nativos; nada de coordenada fixa emprestada. */
    if (!action)
        return;

    ui_tap_x = design_x * screen_width / 1280.0f;
    ui_tap_y = design_y * screen_height / 720.0f;
    inject(env, player, bc_jni_touch_event(0, ui_tap_x, ui_tap_y));
    ui_tap_release_pending = 1;
    if (input_diag)
        fprintf(stderr, "[bc/input] gameplay shortcut %s\n", action);
}

int bc_input_init(void)
{
    input_diag = getenv("BC_INPUT_DIAG") != NULL;
    shot_hotkey = getenv("BC_SHOT") != NULL;
    {   /* BC_RESUME_XY="x,y" em coordenadas de design 1280x720 */
        const char *xy = getenv("BC_RESUME_XY");
        if (xy && *xy) {
            float x = 0.0f, y = 0.0f;
            if (sscanf(xy, "%f,%f", &x, &y) == 2) { resume_x = x; resume_y = y; }
        }
    }
    /* L2/R2 ganham keycode; BC_B_IS_BACK=1 devolve o "voltar" no B. */
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
        android_key_rt[i] = android_key[i];
    if (getenv("BC_B_IS_BACK") &&
        strcmp(getenv("BC_B_IS_BACK"), "0") != 0)
        android_key_rt[SDL_CONTROLLER_BUTTON_B] = 4;   /* KEYCODE_BACK */
    virtual_enabled = getenv("BC_GPVIRT") &&
                      strcmp(getenv("BC_GPVIRT"), "0") != 0;
    /* O cursor é um quirk v44 explícito do manifesto, nunca um default do
     * runtime compartilhado. */
    cursor_enabled = getenv("BC_CURSOR") &&
                     strcmp(getenv("BC_CURSOR"), "0") != 0;
    {
        const char *hide = getenv("BC_CURSOR_HIDE");
        if (hide) {
            float v = strtof(hide, NULL);
            cursor_hide_after = (v >= 0.0f) ? v : 4.0f;   /* 0 = nunca some */
        }
    }
    native_controls_enabled = getenv("BC_NATIVE_CONTROLS") &&
                              strcmp(getenv("BC_NATIVE_CONTROLS"), "0") != 0;
    /* ⚠️ Estas duas NASCEM DESLIGADAS neste port (ver o comentário de cada uma
       lá em cima).  Antes a inicialização era `!getenv(X) || ...`, que com a
       env AUSENTE devolvia 1 e ressuscitava o layout do Hitman GO mesmo com o
       valor inicial em 0 — foi o que fez o personagem andar com o analógico
       DIREITO depois do "conserto".  Agora só liga se a env pedir. */
    click_uses_a = getenv("BC_CLICK_A") &&
                   strcmp(getenv("BC_CLICK_A"), "0") != 0;
    swap_sticks = getenv("BC_SWAP_STICKS") &&
                  strcmp(getenv("BC_SWAP_STICKS"), "0") != 0;
    /* Bomb Chicken nao e jogo de toque: sem cursor e sem swipe sintetico.
     * O pad vai puro, como KeyEvent/MotionEvent de gamepad Android. */
    swipe_move_enabled = getenv("BC_SWIPE_MOVE") &&
                         strcmp(getenv("BC_SWIPE_MOVE"), "0") != 0;
    fprintf(stderr,
            "[bc/input] layout: gamepad nativo (cursor=%s swipe=%s)\n",
            cursor_enabled ? "on" : "off",
            swipe_move_enabled ? "on" : "off");
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[bc/input] SDL controller init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    open_controller();
    install_incontrol_hooks();
    return (controller || raw_joystick || virtual_enabled) ? 0 : -1;
}

void bc_input_poll(void *env, void *player, unsigned long frame)
{
    (void)frame;
    memcpy(previous, buttons, sizeof previous);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_controller();
        if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
            SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
            if (joy && SDL_JoystickInstanceID(joy) == event.cdevice.which) {
                SDL_GameControllerClose(controller);
                controller = NULL;
                memset(buttons, 0, sizeof buttons);
                open_controller();
            }
        }
    }
    if (controller) {
        SDL_GameControllerUpdate();
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
            buttons[i] = SDL_GameControllerGetButton(
                controller, (SDL_GameControllerButton)i) ? 1 : 0;
    } else if (raw_joystick) {
        SDL_JoystickUpdate();
        memset(buttons, 0, sizeof buttons);
        /* ordem posicional dos pads USB/handheld comuns */
        static const int raw_map[SDL_CONTROLLER_BUTTON_MAX] = {
            [SDL_CONTROLLER_BUTTON_A] = 0, [SDL_CONTROLLER_BUTTON_B] = 1,
            [SDL_CONTROLLER_BUTTON_X] = 2, [SDL_CONTROLLER_BUTTON_Y] = 3,
            [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 4,
            [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 5,
            [SDL_CONTROLLER_BUTTON_BACK] = 8,
            [SDL_CONTROLLER_BUTTON_START] = 9,
            [SDL_CONTROLLER_BUTTON_LEFTSTICK] = 10,
            [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = 11,
            [SDL_CONTROLLER_BUTTON_DPAD_UP] = -1,
            [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = -1,
            [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = -1,
            [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = -1,
        };
        int count = SDL_JoystickNumButtons(raw_joystick);
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
            int index = raw_map[i];
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
    install_progress_fix();
    apply_trigger_happy_buttons();
    poll_virtual_controller();
    if (virtual_key_frames > 0) {
        if (virtual_key_frames == 3)
            inject(env, player, bc_jni_key_event(0, virtual_key_code,
                                                 SDL_CONTROLLER_BUTTON_MAX + 2));
        else if (virtual_key_frames == 1)
            inject(env, player, bc_jni_key_event(1, virtual_key_code,
                                                 SDL_CONTROLLER_BUTTON_MAX + 2));
        virtual_key_frames--;
    }
    if (virtual_tap_frames > 0) {
        if (virtual_tap_frames == 3)
            inject(env, player, bc_jni_touch_event(0, virtual_tap_x,
                                                   virtual_tap_y));
        else if (virtual_tap_frames == 1)
            inject(env, player, bc_jni_touch_event(1, virtual_tap_x,
                                                   virtual_tap_y));
        virtual_tap_frames--;
    }
    update_native_controls();

    if (!controller && !raw_joystick && !virtual_enabled)
        return;

    int select = buttons[SDL_CONTROLLER_BUTTON_BACK] ||
                 buttons[SDL_CONTROLLER_BUTTON_GUIDE];
    if (select && buttons[SDL_CONTROLLER_BUTTON_START]) {
        exit_requested = 1;
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
        /* filtro de B removido: era da mira de pedra do Hitman GO (tvOS) e
           aqui so servia para engolir o botao. */
        if (native_gameplay_active &&
            (i == SDL_CONTROLLER_BUTTON_START ||
             i == SDL_CONTROLLER_BUTTON_Y ||
             i == SDL_CONTROLLER_BUTTON_X))
            continue;
        /* filtro do START removido: era do overlay do Hitman GO e impedia o
           segundo START de chegar ao jogo (pause nao despausava). */
        if (i == SDL_CONTROLLER_BUTTON_START && buttons[i] && !previous[i]) {
            /* pause/despause pelo toggle do PROPRIO jogo; fora de nivel (sem
               LevelStart vivo) devolve 0 e o START segue como tecla normal. */
            if (bc_pause_toggle_native()) {
                pause_open = 0;
                continue;
            }
            int game_paused = bc_pause_is_open();
            if (input_diag)
                fprintf(stderr, "[bc/pause] START: timeScale diz %s\n",
                        game_paused < 0 ? "n/d (usando paridade)"
                                        : game_paused ? "PAUSADO" : "rodando");
            if (game_paused > 0 || (game_paused < 0 && pause_open)) {
                /* fechar: chamar HidePauseMenu() do proprio jogo (o menu e'
                   touch-only, tecla nenhuma o fecha).  So se isso falhar,
                   cair no BACK + toque em coordenada configurada. */
                if (!bc_pause_hide_native()) {
                    inject(env, player, bc_jni_key_event(0, 4, i));
                    inject(env, player, bc_jni_key_event(1, 4, i));
                    if (resume_x >= 0.0f) {
                        float tx = resume_x * screen_width / 1280.0f;
                        float ty = resume_y * screen_height / 720.0f;
                        inject(env, player, bc_jni_touch_event(0, tx, ty));
                        inject(env, player, bc_jni_touch_event(1, tx, ty));
                    }
                    if (input_diag)
                        fprintf(stderr, "[bc/pause] fechando (BACK%s)\n",
                                resume_x >= 0.0f ? " + toque" : "");
                }
                pause_open = 0;
                continue;
            }
            pause_open = 1;
            if (input_diag)
                fprintf(stderr, "[bc/pause] abrindo (BUTTON_START)\n");
        }
        if (!android_key_rt[i] || buttons[i] == previous[i])
            continue;
        if (input_diag && buttons[i])
            fprintf(stderr, "[bc/btn] sdl=%d keycode=%d\n", i, android_key_rt[i]);
        inject(env, player,
               bc_jni_key_event(buttons[i] ? 0 : 1, android_key_rt[i], i));
        /* Pedido do NextOS (07/08): a bomba ("pulo") no botao A.  Em nivel
           (Player vivo) o A tambem manda KEYCODE_BUTTON_B, que e' a acao de
           botar bomba no Rewired do jogo; nos menus segue so' o BUTTON_A
           (confirmar).  O B fisico continua botando bomba tambem. */
        if (i == SDL_CONTROLLER_BUTTON_A && buttons[i] != previous[i]) {
            static int a_bomb_down;
            if (buttons[i] && bc_player_alive()) {
                inject(env, player,
                       bc_jni_key_event(0, 97, SDL_CONTROLLER_BUTTON_MAX + 3));
                a_bomb_down = 1;
                if (input_diag)
                    fprintf(stderr, "[bc/btn] A -> bomba (keycode 97)\n");
            } else if (!buttons[i] && a_bomb_down) {
                inject(env, player,
                       bc_jni_key_event(1, 97, SDL_CONTROLLER_BUTTON_MAX + 3));
                a_bomb_down = 0;
            }
        }
    }

    /* L2/R2: no SDL sao EIXOS, entao nao entram no laco de botoes acima e o
       jogo nunca os via. Sintetizamos KEYCODE_BUTTON_L2/R2 a partir do eixo,
       com histerese para nao tremer no limiar. */
    {
        static int trig_down[2];
        const int trig_key[2] = { 104, 105 };   /* KEYCODE_BUTTON_L2 / R2 */
        const SDL_GameControllerAxis trig_axis[2] = {
            SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT
        };
        for (int t = 0; t < 2; t++) {
            float v = axis_value(trig_axis[t]);
            int now_down = trig_down[t] ? (v > 0.30f) : (v > 0.55f);
            if (now_down != trig_down[t]) {
                trig_down[t] = now_down;
                if (input_diag)
                    fprintf(stderr, "[bc/btn] %s keycode=%d %s\n",
                            t ? "R2" : "L2", trig_key[t],
                            now_down ? "down" : "up");
                inject(env, player,
                       bc_jni_key_event(now_down ? 0 : 1, trig_key[t],
                                        SDL_CONTROLLER_BUTTON_MAX + t));
            }
        }
    }

    /* Print: por botao (L3, com BC_SHOT) ou por ARQUIVO (/tmp/bcshot), que
       permite capturar sem a mao do NextOS.  O glReadPixels de dentro e' a
       unica captura confiavel neste device — ler /dev/fb0 de fora da preto
       enquanto o Mali renderiza. */
    {
        extern int bc_shot_request;
        if (shot_hotkey && buttons[SDL_CONTROLLER_BUTTON_LEFTSTICK] &&
            !previous[SDL_CONTROLLER_BUTTON_LEFTSTICK])
            bc_shot_request = 1;
        if (!access("/tmp/bcshot", F_OK)) {
            unlink("/tmp/bcshot");
            bc_shot_request = 1;
        }
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
    inject(env, player, bc_jni_motion_event(lx, ly, rx, ry, lt, rt, hx, hy));
    update_cursor(env, player);
    update_swipe_move(env, player);

    if (input_diag && frame > 0 && frame % 300 == 0) {
        fprintf(stderr,
                "[bc/input] diag names=%lu raw-buttons=%lu mask=%#x "
                "raw-analogs=%lu mask=%#x\n",
                joystick_name_calls, raw_button_calls, queried_buttons,
                raw_analog_calls, queried_analogs);
    }
}

void bc_input_close(void)
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

int bc_input_exit_requested(void)
{
    return exit_requested;
}

int bc_input_cursor(float *x, float *y)
{
    if (!cursor_is_active())
        return 0;
    /* some depois de cursor_hide_after segundos sem mexer/clicar */
    if (cursor_hide_after > 0.0f) {
        uint64_t freq = SDL_GetPerformanceFrequency();
        if (!cursor_seen_tick || !freq)
            return 0;
        double idle = (double)(SDL_GetPerformanceCounter() - cursor_seen_tick)
                    / (double)freq;
        if (idle > (double)cursor_hide_after)
            return 0;
    }
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
    return 1;
}

void bc_input_set_screen_size(int width, int height)
{
    if (width > 0) screen_width = width;
    if (height > 0) screen_height = height;
}

void bc_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void bc_input_keyboard_set(const char *text)
{
    (void)text;
}

void bc_input_keyboard_hide(void)
{
}

int bc_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const bc_keyboard_key **keys,
                                size_t *key_count)
{
    if (text && text_size) text[0] = '\0';
    if (uppercase) *uppercase = 0;
    if (selected) *selected = 0;
    if (keys) *keys = NULL;
    if (key_count) *key_count = 0;
    return 0;
}
