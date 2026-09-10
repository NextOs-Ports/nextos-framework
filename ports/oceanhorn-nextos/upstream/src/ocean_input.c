/*
 * ocean_input.c — pad físico do NextOS entregue ao Unity como UM gamepad Android.
 *
 * Oceanhorn usa Rewired, que NÃO lê evdev nesta build (é Rewired_Android): ele
 * enumera o controle por InputDevice.getDeviceIds()/getMotionRanges() e lê o
 * estado pela fila de input Android do Unity. Então o caminho nativo é o mesmo
 * do Prizefighters 2: normalizar o pad pelo SDL_GameController e injetar
 * KeyEvent/MotionEvent REAIS por UnityPlayer.nativeInjectEvent, com o MESMO
 * deviceId que o InputDevice virtual publica (=1). Evento com deviceId órfão é
 * descartado pelo Rewired sem aviso.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ocean_input.h"

/* KeyEvent do shim (mesma struct usada pelo AUTOTAP). */
extern struct hk_inject_s {
  int action, keycode, source, deviceId, metaState, repeat, scancode, flags, unicode;
  long eventTime, downTime;
} g_hk_inject;
extern void *hk_keyevent_object(void);

/* MotionEvent do shim. */
extern struct ocean_motion_s {
  int action, source, deviceId, metaState, buttonState, flags;
  long eventTime, downTime;
  float axis[32];
} g_ocean_motion;
extern void *ocean_motionevent_object(void);

/* Android: SOURCE_GAMEPAD|SOURCE_DPAD|SOURCE_JOYSTICK e SOURCE_JOYSTICK. */
#define SRC_BUTTONS 0x01000611
#define SRC_MOTION  0x01000010
#define PAD_DEVICE_ID 1

enum { B_A, B_B, B_X, B_Y, B_L1, B_R1, B_BACK, B_START, B_L3, B_R3,
       B_UP, B_DOWN, B_LEFT, B_RIGHT, B_L2, B_R2, B_COUNT };

/* KeyEvent codes na mesma ordem de `enum`. */
static const int g_keycode[B_COUNT] = {
  96, 97, 99, 100,   /* A B X Y */
  102, 103,          /* L1 R1 */
  109, 108,          /* SELECT START */
  106, 107,          /* THUMBL THUMBR */
  19, 20, 21, 22,    /* DPAD UP DOWN LEFT RIGHT */
  104, 105           /* L2 R2 */
};

static const SDL_GameControllerButton g_sdl_button[B_COUNT] = {
  SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
  SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
  SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
  SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
  SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
  SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
  SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
  SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID  /* gatilhos = eixo */
};

static SDL_GameController *g_pad;
static SDL_Joystick *g_raw;            /* fallback: pad fora da base do SDL */
/* Ordinais SDL de BTN_TRIGGER_HAPPY1/2 quando o pad não tem SELECT/START
 * físicos (RG351/R36S/GO-Super). -1 = pad normal, caminho intocado. */
static int g_th_sel = -1, g_th_start = -1;
static unsigned char g_down[B_COUNT];
static unsigned char g_latched[B_COUNT];   /* toque curto confirmado entre polls */
static unsigned char g_press_open[B_COUNT];/* DOWN visto, aguardando o UP */
static Uint32 g_press_ts[B_COUNT];         /* instante do DOWN (ms do SDL) */
static unsigned char g_release_echo[B_COUNT]; /* repetir o UP no frame seguinte */
static long g_down_time[B_COUNT];          /* downTime POR BOTÃO */
static unsigned char g_stick_direction[4]; /* up, down, left, right */
static int g_dir_band[4];                  /* frames parado na faixa morta */
static int g_verbose;
static int g_open_retry;
static int g_ready;
static int g_exit_requested;

static int env_int(const char *name, int def) {
  const char *v = getenv(name);
  return v && *v ? atoi(v) : def;
}

static long ocean_now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static float axis_norm(Sint16 v) {
  float f = v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
  if (f > 1.0f) f = 1.0f;
  if (f < -1.0f) f = -1.0f;
  return (f > -0.12f && f < 0.12f) ? 0.0f : f;   /* zona morta */
}

/*
 * Rewired_Android enumera corretamente os eixos e nativeInjectEvent aceita o
 * MotionEvent, mas esta build só liga as ações de movimento aos KeyEvents do
 * d-pad. Espelhar o stick esquerdo nessas mesmas quatro teclas preserva o fluxo
 * Android nativo que já funciona, inclusive diagonais. A histerese evita
 * DOWN/UP repetidos quando um stick gasto oscila perto da zona morta.
 */
static void stick_to_dpad(float x, float y, unsigned char *buttons) {
  /* Stick gasto descansa longe do zero: com release baixo a direção ENGAJADA
   * nunca soltava e o personagem "continuava indo pra cima". */
  const float engage = 0.40f;
  const float release = 0.30f;
  /*
   * Elevar o limiar não resolve o caso de vez: basta o repouso do stick derivar
   * para DENTRO da faixa [release, engage) — com o uso, a temperatura ou o
   * desgaste — e a direção fica engatada para sempre, que é exatamente o
   * "anda sozinho" relatado. O detalhe que fecha o problema: nessa faixa o
   * stick não consegue INICIAR um movimento (precisa passar de engage), então
   * ficar nela por muito tempo não pode significar "andando". Depois de
   * ~0,75 s parado na faixa, tratamos como centro e soltamos. Quem está
   * realmente empurrando fica perto de 1,0 e nunca cai nesta regra.
   */
  const int band_limit = env_int("OCEAN_STICK_BAND_FRAMES", 22);
  const float mag[4] = { -y, y, -x, x };   /* up, down, left, right */

  for (int d = 0; d < 4; d++) {
    if (g_stick_direction[d]) {
      if (mag[d] < release) {
        g_stick_direction[d] = 0;
        g_dir_band[d] = 0;
      } else if (mag[d] < engage) {
        if (band_limit > 0 && ++g_dir_band[d] > band_limit) {
          g_stick_direction[d] = 0;
          g_dir_band[d] = 0;
          if (g_verbose)
            fprintf(stderr,
                    "[OCEANPAD] direção %d presa na faixa morta (%.2f) — "
                    "tratada como centro\n", d, (double)mag[d]);
        }
      } else {
        g_dir_band[d] = 0;
      }
    } else {
      g_dir_band[d] = 0;
      g_stick_direction[d] = mag[d] > engage;
    }
  }

  buttons[B_UP]    |= g_stick_direction[0];
  buttons[B_DOWN]  |= g_stick_direction[1];
  buttons[B_LEFT]  |= g_stick_direction[2];
  buttons[B_RIGHT] |= g_stick_direction[3];
}

void ocean_input_init(void) {
  g_exit_requested = 0;
  g_verbose = getenv("OCEAN_INPUTLOG") ? 1 : 0;
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    fprintf(stderr, "[OCEANPAD] SDL_InitSubSystem falhou: %s\n", SDL_GetError());
  const char *db = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
  if (db) SDL_GameControllerAddMappingsFromFile(db);
  SDL_GameControllerEventState(SDL_ENABLE);
  SDL_JoystickEventState(SDL_ENABLE);
  g_ready = 1;
  fprintf(stderr, "[OCEANPAD] bridge pronta (%d joystick(s) visíveis ao SDL)\n",
          SDL_NumJoysticks());
}

/* ---- SELECT/START em pads sem BTN_SELECT/BTN_START físicos ----
 *
 * O GO-Super Gamepad (RG351/R36S e família RK3326) não expõe BTN_SELECT nem
 * BTN_START: os dois botões chegam como BTN_TRIGGER_HAPPY1/2. A base do SDL
 * não mapeia esses códigos para BACK/START, então o combo de saída nunca era
 * visto e o jogo só terminava por sinal.
 *
 * O ordinal SDL de um botão é a contagem de bits setados em [BTN_JOYSTICK,
 * code) no bitmap EV_KEY do próprio nó de evento — nunca um índice fixo, que
 * varia por pad. O bitmap é lido com o tamanho de long DESTE processo.
 *
 * Se o pad tiver SELECT/START de verdade (Amlogic/Mali-450 e a maioria), a
 * sonda devolve -1 e nada muda: é adição por capacidade, não substituição.
 */
static int ord_bit(const unsigned long *b, int i) {
  return (b[i / (8 * sizeof(long))] >> (i % (8 * sizeof(long)))) & 1UL;
}

static int ord_key_rank(const unsigned long *keyb, int code) {
  if (!ord_bit(keyb, code)) return -1;
  int rank = 0;
  for (int i = BTN_JOYSTICK; i < code; i++)
    if (ord_bit(keyb, i)) rank++;
  return rank;
}

static void ocean_find_th_ordinals(const char *sdl_name) {
  g_th_sel = g_th_start = -1;
  for (int i = 0; i < 32; i++) {
    char path[64];
    snprintf(path, sizeof path, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    /*
     * Os ordinais só valem para o pad que o SDL abriu. Com dois controles
     * ligados, pegar o primeiro nó que "parece" servir faria SELECT/START
     * saírem de OUTRO aparelho — um toque fantasma que ainda por cima pode
     * disparar o combo de saída. Confere o nome antes de aceitar o nó.
     */
    char evname[128];
    memset(evname, 0, sizeof evname);
    if (sdl_name && *sdl_name &&
        ioctl(fd, EVIOCGNAME(sizeof evname - 1), evname) >= 0 &&
        evname[0] && strcmp(evname, sdl_name) != 0) {
      close(fd);
      continue;
    }
    unsigned long keyb[(KEY_MAX + 1 + 8 * sizeof(long) - 1) /
                       (8 * sizeof(long))];
    memset(keyb, 0, sizeof keyb);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb) >= 0 &&
        ord_bit(keyb, BTN_GAMEPAD) && !ord_bit(keyb, BTN_SELECT) &&
        !ord_bit(keyb, BTN_START) && ord_bit(keyb, BTN_TRIGGER_HAPPY1)) {
      g_th_sel = ord_key_rank(keyb, BTN_TRIGGER_HAPPY1);
      g_th_start = ord_key_rank(keyb, BTN_TRIGGER_HAPPY2);
      fprintf(stderr,
              "[OCEANPAD] %s sem SELECT/START físicos: ordinais "
              "TRIGGER_HAPPY1/2 = %d/%d\n",
              path, g_th_sel, g_th_start);
      fflush(stderr);
      close(fd);
      return;
    }
    close(fd);
  }
}

/* Estado cru dos dois ordinais, lido do joystick por trás do GameController. */
static void ocean_apply_th_buttons(unsigned char *now_down) {
  if (g_th_sel < 0 && g_th_start < 0) return;
  SDL_Joystick *joy = g_raw;
  if (!joy && g_pad) joy = SDL_GameControllerGetJoystick(g_pad);
  if (!joy) return;
  int nb = SDL_JoystickNumButtons(joy);
  if (g_th_sel >= 0 && g_th_sel < nb && SDL_JoystickGetButton(joy, g_th_sel))
    now_down[B_BACK] = 1;
  if (g_th_start >= 0 && g_th_start < nb &&
      SDL_JoystickGetButton(joy, g_th_start))
    now_down[B_START] = 1;
}

static void ocean_open_pad(void) {
  if (g_pad || g_raw) return;
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) {
    if (SDL_IsGameController(i)) {
      g_pad = SDL_GameControllerOpen(i);
      if (g_pad) {
        fprintf(stderr, "[OCEANPAD] controle: \"%s\" (SDL GameController #%d)\n",
                SDL_GameControllerName(g_pad), i);
        ocean_find_th_ordinals(SDL_GameControllerName(g_pad));
        return;
      }
    }
  }
  /* Sem mapeamento na base do SDL: abre como joystick cru e usa a ordem
     posicional. Melhor um pad funcionando do que exigir entrada no db. */
  if (n > 0) {
    g_raw = SDL_JoystickOpen(0);
    if (g_raw)
      fprintf(stderr, "[OCEANPAD] controle CRU: \"%s\" (%d botões, %d eixos, %d hats)\n",
              SDL_JoystickName(g_raw), SDL_JoystickNumButtons(g_raw),
              SDL_JoystickNumAxes(g_raw), SDL_JoystickNumHats(g_raw));
  }
}

static void inject_key(void *env, void *thiz, void *inject, int idx, int down,
                       const char *origem) {
  g_hk_inject.action = down ? 0 : 1;          /* 0=ACTION_DOWN 1=ACTION_UP */
  g_hk_inject.keycode = g_keycode[idx];
  g_hk_inject.source = SRC_BUTTONS;
  g_hk_inject.deviceId = PAD_DEVICE_ID;
  g_hk_inject.metaState = 0;
  g_hk_inject.repeat = 0;
  g_hk_inject.scancode = 0;
  g_hk_inject.flags = 0;
  g_hk_inject.unicode = 0;
  long now = ocean_now_ms();
  g_hk_inject.eventTime = now;
  /* downTime é POR BOTÃO: o Android correlaciona DOWN e UP por ele. Com um
     campo só, o UP de um botão saía carimbado com o downTime de outro que
     tivesse descido depois — par malformado, do ponto de vista de quem lê. */
  if (down) g_down_time[idx] = now;
  g_hk_inject.downTime = g_down_time[idx] ? g_down_time[idx] : now;
  extern unsigned g_keyev_reads;
  unsigned reads_before = g_keyev_reads;
  int r = ((int (*)(void *, void *, void *, int))inject)(env, thiz,
                                                         hk_keyevent_object(), 0);
  /* Se o motor lê os campos DURANTE a chamada, o contador sobe aqui. Se não
     subir, a leitura é adiada e todos os eventos do mesmo quadro acabam lendo
     o último estado — um evento por quadro sobrevive e os outros somem. */
  static int probe;
  if (g_verbose && probe < 6) {
    probe++;
    fprintf(stderr, "[INJPROBE] key %d %s: leituras %u -> %u (%s)\n",
            g_keycode[idx], down ? "DOWN" : "UP", reads_before, g_keyev_reads,
            g_keyev_reads > reads_before ? "SINCRONA" : "ADIADA");
  }
  if (g_verbose)
    fprintf(stderr, "[OCEANPAD] key %d %s%s%s -> aceito=%d\n",
            g_keycode[idx], down ? "DOWN" : "UP", origem ? " " : "",
            origem ? origem : "", r);
}

static void inject_motion(void *env, void *thiz, void *inject) {
  g_ocean_motion.action = 2;                  /* ACTION_MOVE */
  g_ocean_motion.source = SRC_MOTION;
  g_ocean_motion.deviceId = PAD_DEVICE_ID;
  g_ocean_motion.metaState = 0;
  g_ocean_motion.buttonState = 0;
  g_ocean_motion.flags = 0;
  long now = ocean_now_ms();
  g_ocean_motion.eventTime = now;
  g_ocean_motion.downTime = now;
  extern unsigned g_motionev_reads;
  unsigned reads_before = g_motionev_reads;
  int r = ((int (*)(void *, void *, void *, int))inject)(env, thiz,
                                                         ocean_motionevent_object(), 0);
  static int probe;
  if (g_verbose && probe < 4) {
    probe++;
    fprintf(stderr, "[INJPROBE] motion: leituras %u -> %u (%s)\n",
            reads_before, g_motionev_reads,
            g_motionev_reads > reads_before ? "SINCRONA" : "ADIADA");
  }
  if (g_verbose)
    fprintf(stderr, "[OCEANPAD] axes L(%.2f,%.2f) R(%.2f,%.2f) T(%.2f,%.2f) H(%.0f,%.0f) -> aceito=%d\n",
            g_ocean_motion.axis[0], g_ocean_motion.axis[1],
            g_ocean_motion.axis[11], g_ocean_motion.axis[14],
            g_ocean_motion.axis[17], g_ocean_motion.axis[18],
            g_ocean_motion.axis[15], g_ocean_motion.axis[16], r);
}

void ocean_input_poll(void *env, void *thiz, void *inject) {
  if (!g_ready || !inject) return;
  if (!g_pad && !g_raw) {
    if (g_open_retry-- > 0) return;
    g_open_retry = 120;                        /* ~2 s entre tentativas */
    SDL_JoystickUpdate();
    ocean_open_pad();
    if (!g_pad && !g_raw) return;
  }
  SDL_GameControllerUpdate();
  SDL_JoystickUpdate();

  unsigned char now_down[B_COUNT];
  memset(now_down, 0, sizeof now_down);
  float ax[32];
  memset(ax, 0, sizeof ax);

  if (g_pad) {
    for (int i = 0; i < B_COUNT; i++)
      if (g_sdl_button[i] != SDL_CONTROLLER_BUTTON_INVALID)
        now_down[i] = SDL_GameControllerGetButton(g_pad, g_sdl_button[i]) ? 1 : 0;
    ax[0]  = axis_norm(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX));
    ax[1]  = axis_norm(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY));
    ax[11] = axis_norm(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX));
    ax[14] = axis_norm(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY));
    float lt = (float)SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
    float rt = (float)SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    ax[17] = lt < 0.0f ? 0.0f : lt;
    ax[18] = rt < 0.0f ? 0.0f : rt;
    now_down[B_L2] = ax[17] > 0.35f;
    now_down[B_R2] = ax[18] > 0.35f;
  } else if (g_raw) {
    int nb = SDL_JoystickNumButtons(g_raw);
    /* ordem posicional dos pads USB comuns */
    static const int raw_map[B_COUNT] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, -1, -1, -1, -1, 6, 7 };
    for (int i = 0; i < B_COUNT; i++)
      if (raw_map[i] >= 0 && raw_map[i] < nb)
        now_down[i] = SDL_JoystickGetButton(g_raw, raw_map[i]) ? 1 : 0;
    if (SDL_JoystickNumAxes(g_raw) >= 2) {
      ax[0] = axis_norm(SDL_JoystickGetAxis(g_raw, 0));
      ax[1] = axis_norm(SDL_JoystickGetAxis(g_raw, 1));
    }
    if (SDL_JoystickNumAxes(g_raw) >= 4) {
      ax[11] = axis_norm(SDL_JoystickGetAxis(g_raw, 2));
      ax[14] = axis_norm(SDL_JoystickGetAxis(g_raw, 3));
    }
    if (SDL_JoystickNumHats(g_raw) > 0) {
      Uint8 h = SDL_JoystickGetHat(g_raw, 0);
      now_down[B_UP]    = (h & SDL_HAT_UP)    ? 1 : 0;
      now_down[B_DOWN]  = (h & SDL_HAT_DOWN)  ? 1 : 0;
      now_down[B_LEFT]  = (h & SDL_HAT_LEFT)  ? 1 : 0;
      now_down[B_RIGHT] = (h & SDL_HAT_RIGHT) ? 1 : 0;
    }
  }

  /* SELECT/START de pads sem esses botões físicos (RG351/R36S). */
  ocean_apply_th_buttons(now_down);

  /* Movimento do stick pelo mesmo caminho Android comprovado do d-pad.
     OCEAN_NO_DPAD_KEYS=1 corta ESTE caminho e deixa só os eixos: serve para
     provar, em uma partida, por onde o jogo realmente move o personagem. */
  if (!env_int("OCEAN_NO_DPAD_KEYS", 0))
    stick_to_dpad(ax[0], ax[1], now_down);

  /* Quantas vezes o motor leu cada tipo de evento até agora. Se as leituras de
     eixo ficarem em zero durante o jogo, o movimento não vem dos eixos. */
  if (g_verbose) {
    extern unsigned g_keyev_reads, g_motionev_reads;
    static int next;
    static int frame;
    if (frame++ >= next) {
      next = frame + 150;
      fprintf(stderr, "[INJPROBE] leituras acumuladas: tecla=%u eixo=%u\n",
              g_keyev_reads, g_motionev_reads);
    }
  }

  /*
   * Padrão NextOS/PortMaster: SELECT+START volta ao frontend. O pedido é
   * detectado no mesmo estado SDL que alimenta o Rewired e antes de injetar o
   * segundo botão no Unity. O loop principal então percorre focus(false),
   * nativePause e flush dos saves antes do _exit seguro do so-loader.
   */
  if (now_down[B_BACK] && now_down[B_START]) {
    g_exit_requested = 1;
    /* Solta tudo que estiver pressionado antes de sair: o último quadro não
       pode deixar tecla pendurada na visão do jogo. */
    for (int i = 0; i < B_COUNT; i++)
      if (g_down[i]) { g_down[i] = 0; inject_key(env, thiz, inject, i, 0, "(saída)"); }
    fprintf(stderr, "[OCEANPAD] SELECT+START -> saída limpa solicitada\n");
    fflush(stderr);
    return;
  }

  /* toque mais curto que um poll: latch garante 1 frame de DOWN. Fica ANTES do
     HAT para que um toque de direção vindo do latch apareça nos dois caminhos
     (tecla e eixo) — antes o eixo saía sem a tecla correspondente. */
  for (int i = 0; i < B_COUNT; i++) {
    if (g_latched[i] && !now_down[i] && !g_down[i]) now_down[i] = 1;
    g_latched[i] = 0;
  }

  /* d-pad também vai como HAT_X/HAT_Y: pads Android reportam os dois, e o
     Rewired pode estar ouvindo qualquer um dos caminhos. */
  ax[15] = (float)(now_down[B_RIGHT] - now_down[B_LEFT]);
  ax[16] = (float)(now_down[B_DOWN] - now_down[B_UP]);

  /*
   * Reenvio periódico do estado dos eixos. Até aqui o MotionEvent só era
   * injetado QUANDO MUDAVA — e isso torna um único evento perdido permanente:
   * se o "stick voltou ao centro" se perde numa engasgada (troca de cena, GC,
   * pressão de memória), o jogo segue vendo a última deflexão para sempre e o
   * personagem anda sozinho, sem nada de errado do nosso lado. Reafirmar o
   * estado atual algumas vezes por segundo faz o jogo se recuperar sozinho em
   * meio segundo, e custa um evento a cada 15 quadros.
   */
  static int resend_countdown;
  const int resend_every = env_int("OCEAN_AXIS_RESEND", 15);
  int axes_changed = memcmp(ax, g_ocean_motion.axis, sizeof ax) != 0;
  /* OCEAN_NO_MOTION=1 corta o caminho de EIXO por inteiro (só teclas). Com o
     OCEAN_NO_DPAD_KEYS acima, os dois knobs isolam qual dos dois caminhos
     duplicados do d-pad prende a direção no Rewired. */
  if (!env_int("OCEAN_NO_MOTION", 0) &&
      (axes_changed || (resend_every > 0 && --resend_countdown <= 0))) {
    memcpy(g_ocean_motion.axis, ax, sizeof ax);
    resend_countdown = resend_every;
    inject_motion(env, thiz, inject);
  }

  /* RELEASES primeiro: trocar de direção nunca passa por diagonal fantasma */
  for (int i = 0; i < B_COUNT; i++)
    if (!now_down[i] && g_down[i]) {
      g_down[i] = 0;
      inject_key(env, thiz, inject, i, 0, NULL);
      g_release_echo[i] = 1;   /* eco no próximo quadro (ver abaixo) */
    } else if (g_release_echo[i] && !now_down[i]) {
      /* Um UP perdido deixa a AÇÃO presa no jogo (ataque que não para), e o
         nosso lado nem fica sabendo, porque para nós o botão já está solto.
         Repetir o UP uma vez é idempotente para quem lê e recupera o evento
         perdido no quadro seguinte. */
      g_release_echo[i] = 0;
      inject_key(env, thiz, inject, i, 0, "(eco)");
    } else {
      g_release_echo[i] = 0;
    }
  for (int i = 0; i < B_COUNT; i++)
    if (now_down[i] && !g_down[i]) {
      g_down[i] = 1;
      g_release_echo[i] = 0;
      inject_key(env, thiz, inject, i, 1, NULL);
    }
}

/* Chamado pelo dreno de eventos do loop principal: um botão que desceu e subiu
 * entre dois polls de 30 Hz deixa o latch marcado e vira um toque completo no
 * próximo frame — sem isso, troca de herói (L/R) às vezes "não pegava". */
void ocean_input_notify_event(const void *sdl_event) {
  const SDL_Event *ev = (const SDL_Event *)sdl_event;
  if (!ev) return;
  if (ev->type != SDL_CONTROLLERBUTTONDOWN &&
      ev->type != SDL_CONTROLLERBUTTONUP) return;
  for (int i = 0; i < B_COUNT; i++) {
    if (g_sdl_button[i] == SDL_CONTROLLER_BUTTON_INVALID ||
        (int)g_sdl_button[i] != (int)ev->cbutton.button)
      continue;
    if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
      g_press_ts[i] = ev->cbutton.timestamp;
      g_press_open[i] = 1;
    } else if (g_press_open[i]) {
      /*
       * ⚠️ O carimbo de tempo do SDL é o do LOTE de processamento, não o do
       * kernel: todo toque contido entre dois polls sai com duração ZERO, seja
       * ele um repique elétrico ou um toque humano legítimo. Medido no
       * aparelho: 32 toques reais de A/L1/R1 apareceram com 0 ms. Portanto
       * NÃO dá para separar repique de toque rápido por aqui, e filtrar por
       * duração só descarta o toque que o latch existe para salvar. O filtro
       * fica desligado por padrão; a variável continua para experimento.
       */
      Uint32 dur = ev->cbutton.timestamp - g_press_ts[i];
      int min_ms = env_int("OCEAN_TAP_MIN_MS", 0);
      if ((int)dur >= min_ms) g_latched[i] = 1;
      else if (g_verbose)
        fprintf(stderr, "[OCEANPAD] repique de %ums no botão %d descartado\n",
                (unsigned)dur, i);
      g_press_open[i] = 0;
    }
    return;
  }
}

int ocean_input_exit_requested(void) {
  return g_exit_requested;
}

void ocean_input_shutdown(void) {
  if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
  if (g_raw) { SDL_JoystickClose(g_raw); g_raw = NULL; }
  memset(g_stick_direction, 0, sizeof g_stick_direction);
  memset(g_dir_band, 0, sizeof g_dir_band);
  memset(g_latched, 0, sizeof g_latched);
  memset(g_press_open, 0, sizeof g_press_open);
  memset(g_release_echo, 0, sizeof g_release_echo);
  g_ready = 0;
}
