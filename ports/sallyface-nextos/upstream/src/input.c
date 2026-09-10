/*
 * Native NextOS controller -> Android input bridge for Sally Face.
 *
 * SDL normalises the physical controller, then Unity receives the same
 * KeyEvent/MotionEvent stream UnityPlayerActivity forwards on Android.  Keep
 * this layer deliberately boring: no managed-method patches, game-specific
 * coordinates, duplicated actions or shortcuts borrowed from sibling ports.
 * The only touch synthesis retained is the explicit right-stick/R3 pointer.
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

#include "sf.h"
#include "nx_elf.h"
#include "gptk_adapter.h"

#include <dlfcn.h>

/*
 * SDL_JoystickGetVendor/GetProduct/GetDeviceInstanceID so existem a partir do
 * SDL 2.0.6; o piso universal declarado e 2.0.4. Resolver por dlsym mantem o
 * loader carregavel num SDL antigo: sem o simbolo, VID/PID viram 0 e o id de
 * device vira -1 (o caminho por indice/GUID continua respondendo).
 */
static Uint16 sf_joystick_vendor(SDL_Joystick *joy)
{
    static Uint16 (*fn)(SDL_Joystick *);
    static int probed;
    if (!probed) {
        probed = 1;
        fn = (Uint16 (*)(SDL_Joystick *))(uintptr_t)
            dlsym(RTLD_DEFAULT, "SDL_JoystickGetVendor");
    }
    return (fn && joy) ? fn(joy) : 0;
}

static Uint16 sf_joystick_product(SDL_Joystick *joy)
{
    static Uint16 (*fn)(SDL_Joystick *);
    static int probed;
    if (!probed) {
        probed = 1;
        fn = (Uint16 (*)(SDL_Joystick *))(uintptr_t)
            dlsym(RTLD_DEFAULT, "SDL_JoystickGetProduct");
    }
    return (fn && joy) ? fn(joy) : 0;
}

static SDL_JoystickID sf_joystick_device_instance_id(int index)
{
    static SDL_JoystickID (*fn)(int);
    static int probed;
    if (!probed) {
        probed = 1;
        fn = (SDL_JoystickID (*)(int))(uintptr_t)
            dlsym(RTLD_DEFAULT, "SDL_JoystickGetDeviceInstanceID");
    }
    return fn ? fn(index) : -1;
}

/* TODOS os pads conectados valem, nao so' o primeiro.  Num handheld com um
 * controle externo plugado, o jogador espera que qualquer um dos dois ande --
 * e um pad que aparece depois do jogo aberto tambem tem que valer.  Os estados
 * sao combinados por OR: e' o mesmo jogador nos dois. */
#define SF_MAX_PADS 4
static SDL_GameController *pads[SF_MAX_PADS];
static int pad_n;
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
void sf_input_request_exit(void)
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
 * SF_CURSOR_HIDE segundos parada e volta assim que o analógico direito se
 * mexe (ou num clique).  Visual apenas — o clique continua valendo. */
static uint64_t cursor_seen_tick;
static float cursor_hide_after = 4.0f;
static float cursor_touch_x;
static float cursor_touch_y;
static int ui_tap_release_pending;
static int shot_hotkey;

static int native_controls_enabled;
static int native_selection_active;

static float axis_value(SDL_GameControllerAxis axis);


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
static il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name_p;
static il2cpp_runtime_invoke_fn il2cpp_runtime_invoke_p;
static il2cpp_class_get_type_fn il2cpp_class_get_type_p;
static il2cpp_type_get_object_fn il2cpp_type_get_object_p;
static il2cpp_object_unbox_fn il2cpp_object_unbox_p;
static il2cpp_object_get_class_fn il2cpp_object_get_class_p;
static il2cpp_class_get_name_fn il2cpp_class_get_name_p;

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
 * No Blasphemous o A é AÇÃO/PULO — se ele clicar o cursor, o jogo fica
 * injogável.  Aqui o clique é SÓ o R3, como o NextOS pediu em 07/08/2026.
 * SF_CLICK_A=1 devolve o comportamento do Hitman GO.
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
 * Blasphemous é plataforma: movimento no esquerdo, sempre.
 * SF_SWAP_STICKS=1 devolve o layout do Hitman GO.
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
    return (!sf_gptk_control_owned(NXINPUT_GPTK_R3) &&
            buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK]) ||
           (click_uses_a && !native_selection_active &&
            !sf_gptk_control_owned(NXINPUT_GPTK_A) &&
            buttons[SDL_CONTROLLER_BUTTON_A]);
}

static int cursor_click_prev(void)
{
    return (!sf_gptk_control_owned(NXINPUT_GPTK_R3) &&
            previous[SDL_CONTROLLER_BUTTON_RIGHTSTICK]) ||
           (click_uses_a && !native_selection_active &&
            !sf_gptk_control_owned(NXINPUT_GPTK_A) &&
            previous[SDL_CONTROLLER_BUTTON_A]);
}

/* O A vira botão de clique quando o cursor está ativo; nesse modo a antiga
   confirmação do A é servida pelo L1 (livre neste jogo).  Na mira de pedra o
   A é devolvido ao jogo como arremesso. */
static int a_is_click_button(void)
{
    return click_uses_a && cursor_is_active() && !native_selection_active;
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
 * fica atras do gate SF_IL2CPP_HOOKS e' o patch de RVA (codigo de outro jogo),
 * nunca isto.  Chamado tarde (gameplay ja' rodando), nunca antes do frame 0 —
 * entrar em il2cpp_domain_get cedo demais mata a Unity (ver
 * sf_get_native_input_implementation). */
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


/* Achar a instancia viva de uma classe do jogo via
 * UnityEngine.Object.FindObjectOfType(typeof(K)). */
static void *sf_find_instance(void *klass)
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










/* Dispara o fim de fase pelo caminho do proprio jogo, para provar o conserto
 * sem ter de jogar ate' o fim da fase (token `chk`, so' com SF_GPVIRT). */
static void sf_debug_goto_checkpoint(int load_next)
{
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[bc/dbg] il2cpp indisponivel\n");
        return;
    }
    void *level_class = find_managed_class("", "LevelStart");
    void *go = level_class
        ? il2cpp_class_get_method_from_name_p(level_class, "GoToCheckpoint", 1)
        : NULL;
    void *instance = level_class ? sf_find_instance(level_class) : NULL;
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

/* Sonda a FollowCam ao vivo (token `cam`): quais referencias dela estao nulas
 * quando a tela fica preta depois do checkpoint.  Offsets do dump deste jogo:
 * m_Target 0x20, m_CamComponent 0x38, m_Following 0x44, m_CurrentLevel 0x70. */
static void sf_debug_camera(void)
{
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[bc/dbg] il2cpp indisponivel\n");
        return;
    }
    void *cam_class = find_managed_class("", "FollowCam");
    void *cam = cam_class ? sf_find_instance(cam_class) : NULL;
    if (!cam) {
        fprintf(stderr, "[bc/dbg] FollowCam ausente\n");
        return;
    }
    uint8_t *c = cam;
    void *player_class = find_managed_class("", "Player");
    void *player = player_class ? sf_find_instance(player_class) : NULL;
    void *level_class = find_managed_class("", "Level");
    void *level_any = level_class ? sf_find_instance(level_class) : NULL;
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
 * global-metadata.dat), nunca herdado de outro port.  SF_PROGRESS_FIX=0
 * devolve o comportamento original para diagnostico.
 */



/* Blasphemous uses Rewired, not InControl, and the RVAs above belong to a
 * different game's libil2cpp.  Patching them here would smash unrelated code
 * (armadilha 13/17).  The hooks stay compiled but are only armed when
 * SF_IL2CPP_HOOKS explicitly asks for them during bring-up. */


static const int android_key[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = 96,              /* KEYCODE_BUTTON_A */
    /* ⚠️ Era KEYCODE_BACK (4), o "voltar" do Android — herança do Hitman GO,
       onde B fecha tela.  A Unity trata BACK como pause/menu, então no Bomb
       Chicken o botao X do pad (b2 = 'b' no es_input.cfg) PAUSAVA o jogo em
       vez de agir.  Reportado pelo NextOS em 07/08/2026.  Agora vai como
       botao de jogo de verdade.  SF_B_IS_BACK=1 devolve o comportamento antigo
       se alguma tela precisar do voltar. */
    [SDL_CONTROLLER_BUTTON_B] = 97,              /* KEYCODE_BUTTON_B */
    [SDL_CONTROLLER_BUTTON_X] = 99,              /* KEYCODE_BUTTON_X */
    [SDL_CONTROLLER_BUTTON_Y] = 100,             /* KEYCODE_BUTTON_Y */
    [SDL_CONTROLLER_BUTTON_BACK] = 109,           /* KEYCODE_BUTTON_SELECT */
    [SDL_CONTROLLER_BUTTON_GUIDE] = 110,          /* KEYCODE_BUTTON_MODE */
    /* A build Android de Sally Face ignora KEYCODE_BUTTON_START (108) no
     * InputManager legado. Em campo, o menu respondeu a KEYCODE_BUTTON_R2
     * (105), que o asset associa a "Menu". O gatilho R2 fisico continua sendo
     * somente eixo; portanto este sinal digital pertence exclusivamente ao
     * START fisico e nao faz o R2 pausar. */
    [SDL_CONTROLLER_BUTTON_START] = 105,          /* Menu comprovado no jogo */
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = 106,      /* KEYCODE_BUTTON_THUMBL */
    [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = 107,     /* KEYCODE_BUTTON_THUMBR */
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 102,   /* KEYCODE_BUTTON_L1 */
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 103,  /* KEYCODE_BUTTON_R1 */
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = 19,
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = 20,
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = 21,
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = 22,
};

/* Copia mutavel do mapa para a opcao diagnostica SF_B_IS_BACK. Gatilhos
 * permanecem MotionEvent axes; nao entram neste mapa digital. */
static int android_key_rt[SDL_CONTROLLER_BUTTON_MAX];

static float axis_value(SDL_GameControllerAxis axis)
{
    if (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX &&
        virtual_axis_frames[axis] > 0)
        return virtual_axis_values[axis];
    Sint16 value = 0;
    if (pad_n) {
        /* O eixo vale o MAIOR DESVIO entre os pads: um pad parado no centro
         * nunca cancela o movimento que o outro esta pedindo. */
        for (int i = 0; i < pad_n; i++) {
            Sint16 v = SDL_GameControllerGetAxis(pads[i], axis);
            int mag = v < 0 ? -v : v;
            int best = value < 0 ? -value : value;
            if (mag > best)
                value = v;
        }
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

/* Approved-port bring-up path: one token written to /tmp/sallyface-gp becomes a
 * short native-controller pulse.  This never enters the touch/mouse path and
 * is inactive unless SF_GPVIRT is explicitly enabled for a test launch. */
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

    FILE *input = fopen("/tmp/sallyface-gp", "r");
    if (input) {
        char token[24] = { 0 };
        int have_token = fscanf(input, "%23s", token) == 1 && token[0];
        fclose(input);
        unlink("/tmp/sallyface-gp");
        if (have_token) {
            unsigned duration = 6;
            const char *duration_value = getenv("SF_GPVDUR");
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
                sf_debug_camera();
            } else if (!strcasecmp(token, "chk")) {
                /* Reproduz o fim de fase sem jogar: a mesma chamada que a
                   corrotina do Teleporter faz.  So' com SF_GPVIRT. */
                sf_debug_goto_checkpoint(1);
            } else if (!strncasecmp(token, "key:", 4)) {
                /* key:N — injeta um KEYCODE Android arbitrario (bring-up). */
                virtual_key_code = atoi(token + 4);
                virtual_key_frames = 3;
            } else if (!strncasecmp(token, "tap:", 4)) {
                /* tap:X,Y em coordenadas de design 1280x720 — toque direto,
                   so' para bring-up (SF_GPVIRT). */
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

static void add_known_mappings(void)
{
    SDL_GameControllerAddMapping(
        "0300605b100800000100000010010000,USB Gamepad,platform:Linux,"
        "a:b2,b:b1,x:b3,y:b0,leftshoulder:b4,rightshoulder:b5,"
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
    /* SELECT/START chegam como BTN_TRIGGER_HAPPY1/2 em varios handhelds e a
     * base do SDL nao os mapeia para BACK/START -- sem isto o combo de saida
     * nunca e' visto.  Vale para QUALQUER pad conectado, nao so' o primeiro. */
    for (int i = 0; i <= pad_n; i++) {
        SDL_Joystick *joy = i < pad_n
            ? SDL_GameControllerGetJoystick(pads[i])
            : (pad_n ? NULL : raw_joystick);
        if (!joy)
            continue;
        int count = SDL_JoystickNumButtons(joy);
        if (th_select_ordinal >= 0 && th_select_ordinal < count &&
            SDL_JoystickGetButton(joy, th_select_ordinal))
            buttons[SDL_CONTROLLER_BUTTON_BACK] = 1;
        if (th_start_ordinal >= 0 && th_start_ordinal < count &&
            SDL_JoystickGetButton(joy, th_start_ordinal))
            buttons[SDL_CONTROLLER_BUTTON_START] = 1;
    }
}

static int pad_already_open(SDL_JoystickID id)
{
    for (int i = 0; i < pad_n; i++) {
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(pads[i]);
        if (joy && SDL_JoystickInstanceID(joy) == id)
            return 1;
    }
    return 0;
}

static void open_controller(void)
{
    for (int i = 0; i < SDL_NumJoysticks() && pad_n < SF_MAX_PADS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        SDL_JoystickID id = sf_joystick_device_instance_id(i);
        if (id >= 0 && pad_already_open(id))
            continue;
        SDL_GameController *pad = SDL_GameControllerOpen(i);
        if (!pad)
            continue;
        pads[pad_n++] = pad;
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(pad);
        const char *physical = SDL_GameControllerName(pad);
        int vendor = joy ? sf_joystick_vendor(joy) : 0;
        int product = joy ? sf_joystick_product(joy) : 0;
        sf_jni_input_device_info("Microsoft X-Box 360 pad", vendor, product,
                                  physical ? physical : "nextos-gamepad");
        fprintf(stderr, "[bc/input] controller %d: %s (%04x:%04x)\n",
                pad_n - 1, physical ? physical : "unknown", vendor & 0xffff,
                product & 0xffff);
        find_trigger_happy_ordinals();
    }
    if (pad_n)
        return;
    /* Nenhum pad na base do SDL: abre o primeiro joystick cru. */
    if (!raw_joystick && SDL_NumJoysticks() > 0) {
        raw_joystick = SDL_JoystickOpen(0);
        if (raw_joystick) {
            const char *name = SDL_JoystickName(raw_joystick);
            sf_jni_input_device_info("Microsoft X-Box 360 pad",
                                      sf_joystick_vendor(raw_joystick),
                                      sf_joystick_product(raw_joystick),
                                      name ? name : "nextos-gamepad");
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
        native_inject = sf_jni_native("com/unity3d/player/UnityPlayer",
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

typedef struct {
    void *env;
    void *player;
} sf_gptk_delivery_context;

static sf_gptk_delivery_context gptk_delivery;

static void gptk_deliver_button(void *user, int android_keycode, int pressed,
                                int physical_control)
{
    sf_gptk_delivery_context *context = user;
    if (!context || !context->env || !context->player)
        return;
    inject(context->env, context->player,
           sf_jni_key_event(pressed ? 0 : 1, android_keycode,
                            physical_control));
}

static int sdl_button_to_gptk(int button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return NXINPUT_GPTK_A;
    case SDL_CONTROLLER_BUTTON_B: return NXINPUT_GPTK_B;
    case SDL_CONTROLLER_BUTTON_X: return NXINPUT_GPTK_X;
    case SDL_CONTROLLER_BUTTON_Y: return NXINPUT_GPTK_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return NXINPUT_GPTK_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return NXINPUT_GPTK_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return NXINPUT_GPTK_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return NXINPUT_GPTK_R3;
    case SDL_CONTROLLER_BUTTON_START: return NXINPUT_GPTK_START;
    case SDL_CONTROLLER_BUTTON_BACK: return NXINPUT_GPTK_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return NXINPUT_GPTK_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return NXINPUT_GPTK_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return NXINPUT_GPTK_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return NXINPUT_GPTK_RIGHT;
    default: return -1;
    }
}

static void update_cursor(void *env, void *player)
{
    if (!cursor_is_active() || (!pad_n && !raw_joystick && !virtual_enabled))
        return;

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = cursor_tick && frequency
             ? (float)((double)(now - cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;

    float x = 0.0f;
    float y = 0.0f;
    if (!sf_gptk_control_owned(NXINPUT_GPTK_RIGHT_STICK)) {
        x = axis_value(cursor_axis(0));
        y = axis_value(cursor_axis(1));
    }
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
        inject(env, player, sf_jni_touch_event(0, touch_x, touch_y));
        cursor_drag_active = 1;
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
        if (input_diag)
            fprintf(stderr, "[bc/touch] DOWN %.0f,%.0f\n", touch_x, touch_y);
    } else if (held && cursor_drag_active &&
               (fabsf(touch_x - cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - cursor_touch_y) >= 0.25f)) {
        inject(env, player, sf_jni_touch_event(2, touch_x, touch_y));
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
    }
    if (up && cursor_drag_active) {
        inject(env, player, sf_jni_touch_event(1, touch_x, touch_y));
        cursor_drag_active = 0;
        if (input_diag)
            fprintf(stderr, "[bc/touch] UP   %.0f,%.0f\n", touch_x, touch_y);
    }
}


/*
 * ===== Andar por swipe sintético (achado do NextOS, 05/08) =====
 * O InputManager_tvOS e o de toque são MUTUAMENTE exclusivos no jogo: com o
 * tvOS selecionado o LevelState ignora toques nos nós, e a pedra (mira por
 * toque) nunca sai.  Então o modo padrão volta ao gerenciador de TOQUE — tudo
 * clicável — e o D-pad/analógico de movimento vira um swipe sintético, que é
 * mecânica nativa do jogo (swipe em qualquer lugar move o 47).  O caminho
 * tvOS continua atrás de SF_NATIVE_CONTROLS=1 para comparação.
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
               sf_jni_touch_event(action, swipe_from_x + swipe_dx * t,
                                   swipe_from_y + swipe_dy * t));
        swipe_step++;
        if (swipe_step > 4)
            swipe_step = 0;
        return;
    }

    float x = sf_gptk_control_owned(NXINPUT_GPTK_LEFT_STICK)
                  ? 0.0f : axis_value(move_axis(0));
    float y = sf_gptk_control_owned(NXINPUT_GPTK_LEFT_STICK)
                  ? 0.0f : -axis_value(move_axis(1));
    float dpad_x = (float)(
        (sf_gptk_control_owned(NXINPUT_GPTK_RIGHT) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]) -
        (sf_gptk_control_owned(NXINPUT_GPTK_LEFT) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]));
    float dpad_y = (float)(
        (sf_gptk_control_owned(NXINPUT_GPTK_UP) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]) -
        (sf_gptk_control_owned(NXINPUT_GPTK_DOWN) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]));
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
    inject(env, player, sf_jni_touch_event(0, swipe_from_x, swipe_from_y));
    swipe_step = 1;
    if (input_diag)
        fprintf(stderr, "[bc/input] swipe-move %.2f %.2f\n", x, y);
}


int sf_input_init(void)
{
    input_diag = getenv("SF_INPUT_DIAG") != NULL;
    shot_hotkey = getenv("SF_SHOT") != NULL;
    /* SF_B_IS_BACK=1 devolve o "voltar" no B. */
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
        android_key_rt[i] = android_key[i];
    if (getenv("SF_B_IS_BACK") &&
        strcmp(getenv("SF_B_IS_BACK"), "0") != 0)
        android_key_rt[SDL_CONTROLLER_BUTTON_B] = 4;   /* KEYCODE_BACK */
    virtual_enabled = getenv("SF_GPVIRT") &&
                      strcmp(getenv("SF_GPVIRT"), "0") != 0;
    /* cursor LIGADO por padrão: é o fallback pedido pelo NextOS para as telas
       que não respondem ao pad.  SF_CURSOR=0 desliga. */
    cursor_enabled = !getenv("SF_CURSOR") ||
                     strcmp(getenv("SF_CURSOR"), "0") != 0;
    {
        const char *hide = getenv("SF_CURSOR_HIDE");
        if (hide) {
            float v = strtof(hide, NULL);
            cursor_hide_after = (v >= 0.0f) ? v : 4.0f;   /* 0 = nunca some */
        }
    }
    native_controls_enabled = getenv("SF_NATIVE_CONTROLS") &&
                              strcmp(getenv("SF_NATIVE_CONTROLS"), "0") != 0;
    /* ⚠️ Estas duas NASCEM DESLIGADAS neste port (ver o comentário de cada uma
       lá em cima).  Antes a inicialização era `!getenv(X) || ...`, que com a
       env AUSENTE devolvia 1 e ressuscitava o layout do Hitman GO mesmo com o
       valor inicial em 0 — foi o que fez o personagem andar com o analógico
       DIREITO depois do "conserto".  Agora só liga se a env pedir. */
    click_uses_a = getenv("SF_CLICK_A") &&
                   strcmp(getenv("SF_CLICK_A"), "0") != 0;
    swap_sticks = getenv("SF_SWAP_STICKS") &&
                  strcmp(getenv("SF_SWAP_STICKS"), "0") != 0;
    /* Blasphemous nao e jogo de toque: sem cursor e sem swipe sintetico.
     * O pad vai puro, como KeyEvent/MotionEvent de gamepad Android. */
    swipe_move_enabled = getenv("SF_SWIPE_MOVE") &&
                         strcmp(getenv("SF_SWIPE_MOVE"), "0") != 0;
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
    add_known_mappings();
    open_controller();
    if (sf_gptk_init(sf_gamedir, gptk_deliver_button, &gptk_delivery) != 0) {
        fprintf(stderr,
                "[sf/gptk] adapter initialization failed; refusing raw mapped controls\n");
        return -1;
    }
    return (pad_n || raw_joystick || virtual_enabled) ? 0 : -1;
}

void sf_input_poll(void *env, void *player, unsigned long frame)
{
    gptk_delivery.env = env;
    gptk_delivery.player = player;
    memcpy(previous, buttons, sizeof previous);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_controller();
        if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            for (int i = 0; i < pad_n; i++) {
                SDL_Joystick *joy = SDL_GameControllerGetJoystick(pads[i]);
                if (!joy || SDL_JoystickInstanceID(joy) != event.cdevice.which)
                    continue;
                SDL_GameControllerClose(pads[i]);
                for (int k = i; k + 1 < pad_n; k++)
                    pads[k] = pads[k + 1];
                pads[--pad_n] = NULL;
                memset(buttons, 0, sizeof buttons);
                break;
            }
            open_controller();
        }
    }
    if (pad_n) {
        SDL_GameControllerUpdate();
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
            int down = 0;
            for (int k = 0; k < pad_n && !down; k++)
                down = SDL_GameControllerGetButton(
                    pads[k], (SDL_GameControllerButton)i) ? 1 : 0;
            buttons[i] = down;
        }
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
    apply_trigger_happy_buttons();
    poll_virtual_controller();
    if (virtual_key_frames > 0) {
        if (virtual_key_frames == 3)
            inject(env, player, sf_jni_key_event(0, virtual_key_code,
                                                 SDL_CONTROLLER_BUTTON_MAX + 2));
        else if (virtual_key_frames == 1)
            inject(env, player, sf_jni_key_event(1, virtual_key_code,
                                                 SDL_CONTROLLER_BUTTON_MAX + 2));
        virtual_key_frames--;
    }
    if (virtual_tap_frames > 0) {
        if (virtual_tap_frames == 3)
            inject(env, player, sf_jni_touch_event(0, virtual_tap_x,
                                                   virtual_tap_y));
        else if (virtual_tap_frames == 1)
            inject(env, player, sf_jni_touch_event(1, virtual_tap_x,
                                                   virtual_tap_y));
        virtual_tap_frames--;
    }
    if (!pad_n && !raw_joystick && !virtual_enabled)
        return;

    int select = buttons[SDL_CONTROLLER_BUTTON_BACK] ||
                 buttons[SDL_CONTROLLER_BUTTON_GUIDE];
    if (select && buttons[SDL_CONTROLLER_BUTTON_START]) {
        exit_requested = 1;
        memset(buttons, 0, sizeof buttons);
        return;
    }

    /* Mapped controls have one authority: physical edge -> GPTK dispatcher
     * -> semantic Sally sink. They are explicitly excluded from the legacy
     * physical-key loop below, which makes an A/B swap real without doubling
     * the original Android event. */
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        int control = sdl_button_to_gptk(i);
        if (control < 0 || !sf_gptk_control_owned(control) ||
            buttons[i] == previous[i])
            continue;
        sf_gptk_feed_button(control, buttons[i] ? 1 : 0,
                            buttons[i] ? 1.0f : 0.0f);
    }

    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        int control = sdl_button_to_gptk(i);
        if (control >= 0 && sf_gptk_control_owned(control))
            continue;
        if (cursor_is_active() && i == SDL_CONTROLLER_BUTTON_RIGHTSTICK)
            continue;
        if (a_is_click_button() && i == SDL_CONTROLLER_BUTTON_A)
            continue;
        if (!android_key_rt[i] || buttons[i] == previous[i])
            continue;
        if (input_diag && buttons[i])
            fprintf(stderr, "[bc/btn] sdl=%d keycode=%d\n", i, android_key_rt[i]);
        if (control >= 0)
            sf_gptk_note_native_button_delivery(control);
        inject(env, player,
               sf_jni_key_event(buttons[i] ? 0 : 1, android_key_rt[i], i));
    }

    /* Preserve LT/RT only as Android analog axes below.  Sally Face's original
     * InputManager maps Menu to `joystick button 7` OR `joystick button 9`.
     * Neste runtime, KEYCODE_BUTTON_R2 aciona a entrada comprovada enquanto
     * KEYCODE_BUTTON_START e ignorado. O START fisico envia o primeiro acima;
     * os gatilhos nao geram KeyEvent e, portanto, nenhum deles pausa. */

    /* Print: por botao (L3, com SF_SHOT) ou por ARQUIVO (/tmp/bcshot), que
       permite capturar sem a mao do NextOS.  O glReadPixels de dentro e' a
       unica captura confiavel neste device — ler /dev/fb0 de fora da preto
       enquanto o Mali renderiza. */
    {
        extern int sf_shot_request;
        if (shot_hotkey && buttons[SDL_CONTROLLER_BUTTON_LEFTSTICK] &&
            !previous[SDL_CONTROLLER_BUTTON_LEFTSTICK])
            sf_shot_request = 1;
        if (shot_hotkey && !access("/tmp/bcshot", F_OK)) {
            unlink("/tmp/bcshot");
            sf_shot_request = 1;
        }
    }

    float physical_lx = axis_value(move_axis(0));
    float physical_ly = axis_value(move_axis(1));
    float physical_rx = axis_value(cursor_axis(0));
    float physical_ry = axis_value(cursor_axis(1));
    float lx = 0.0f, ly = 0.0f;
    float rx = 0.0f, ry = 0.0f;
    if (!sf_gptk_route_stick(NXINPUT_GPTK_LEFT_STICK,
                             physical_lx, physical_ly, &lx, &ly)) {
        lx = physical_lx;
        ly = physical_ly;
        sf_gptk_note_native_stick_delivery(NXINPUT_GPTK_LEFT_STICK);
    }
    {
        float routed_x = 0.0f, routed_y = 0.0f;
        if (sf_gptk_route_stick(NXINPUT_GPTK_RIGHT_STICK,
                                physical_rx, physical_ry,
                                &routed_x, &routed_y)) {
            float old_mag = lx * lx + ly * ly;
            float new_mag = routed_x * routed_x + routed_y * routed_y;
            if (new_mag > old_mag) {
                lx = routed_x;
                ly = routed_y;
            }
        } else {
            rx = physical_rx;
            ry = physical_ry;
            sf_gptk_note_native_stick_delivery(NXINPUT_GPTK_RIGHT_STICK);
        }
    }
    float lt = axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    float rt = axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    float hx = (float)(
        (sf_gptk_control_owned(NXINPUT_GPTK_RIGHT) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]) -
        (sf_gptk_control_owned(NXINPUT_GPTK_LEFT) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]));
    float hy = (float)(
        (sf_gptk_control_owned(NXINPUT_GPTK_DOWN) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]) -
        (sf_gptk_control_owned(NXINPUT_GPTK_UP) ? 0 :
             buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]));
    inject(env, player, sf_jni_motion_event(lx, ly, rx, ry, lt, rt, hx, hy));
    update_cursor(env, player);
    update_swipe_move(env, player);
    sf_gptk_periodic_receipt(frame);

    if (input_diag && frame > 0 && frame % 300 == 0) {
        fprintf(stderr,
                "[bc/input] diag names=%lu raw-buttons=%lu mask=%#x "
                "raw-analogs=%lu mask=%#x\n",
                joystick_name_calls, raw_button_calls, queried_buttons,
                raw_analog_calls, queried_analogs);
    }
}

void sf_input_close(void)
{
    sf_gptk_close();
    while (pad_n > 0) {
        SDL_GameControllerClose(pads[--pad_n]);
        pads[pad_n] = NULL;
    }
    if (raw_joystick) {
        SDL_JoystickClose(raw_joystick);
        raw_joystick = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
                      SDL_INIT_EVENTS);
}

int sf_input_exit_requested(void)
{
    return exit_requested;
}

int sf_input_cursor(float *x, float *y)
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

void sf_input_set_screen_size(int width, int height)
{
    if (width > 0) screen_width = width;
    if (height > 0) screen_height = height;
}

void sf_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void sf_input_keyboard_set(const char *text)
{
    (void)text;
}

void sf_input_keyboard_hide(void)
{
}

int sf_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const sf_keyboard_key **keys,
                                size_t *key_count)
{
    if (text && text_size) text[0] = '\0';
    if (uppercase) *uppercase = 0;
    if (selected) *selected = 0;
    if (keys) *keys = NULL;
    if (key_count) *key_count = 0;
    return 0;
}
