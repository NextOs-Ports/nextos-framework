/* ab_input.c — adapter de controle do Angry Birds Classic 8.0.3.
 *
 * nxinput owns SDL controller discovery, mappings, hotplug, deadzones and the
 * global cursor. Events still enter through the APK's original MyInputHandler
 * queue immediately before nativeUpdate. The gameplay slingshot remains the
 * original SlingshotSystem Lua flow. The approved Mali-450 cursor contract is
 * global: right stick moves it in menus and gameplay, A or R3 clicks, and the
 * arrow hides after two idle seconds until movement resumes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <GLES2/gl2.h>
#include "nxgl_gles2.h" /* gl* -> ponteiros provados pelo nxgl_gles2 */
#include <SDL.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ab_cursor_buttons.h"
#include "ab_evdev_exit.h"
#include "ab_exit_chord.h"
#include "ab_port.h"
#include "nxinput.h"

#define SF __attribute__((pcs("aapcs")))

typedef void(SF *KeyInputFn)(void *env, void *obj, int32_t key, int32_t event,
                             int32_t unicode, int32_t device);
typedef void(SF *AxisInputFn)(void *env, void *obj, int32_t axis, float value,
                              int32_t device);
typedef void(SF *TouchInputFn)(void *env, void *obj, int32_t event, float x,
                               float y, int32_t index);

static KeyInputFn g_key;
static AxisInputFn g_axis;
static TouchInputFn g_touch;
static void *g_this;

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
#define AKEY_BUTTON_THUMBR 107
#define AKEY_BUTTON_START 108
#define AKEY_BUTTON_SELECT 109

enum { EV_KEY = 0, EV_TOUCH = 1, EV_AXIS = 2 };

typedef struct {
  int kind;
  int a;
  int b;
  int device;
  float x;
  float y;
} InputEvent;

#define QUEUE_MAX 512
static InputEvent g_queue[QUEUE_MAX];
static int g_queue_count;
static SDL_mutex *g_queue_lock;
static nxinput_context *g_input;

static float g_pointer_x, g_pointer_y;
static int g_screen_w = 640, g_screen_h = 480;
static ab_cursor_button_state g_cursor_buttons;
static int g_quit_requested;
static Uint32 g_last_move_ticks;
static Uint32 g_cursor_seen;
static uint32_t g_pad_generation;
static int g_active_slot = -1;
static int g_cursor_warped;
static int g_logged_gameplay = -1;
static int g_cursor_draw_visible;
static ab_exit_chord_state g_guide_exit_chord;

static float g_stick_x, g_stick_y;

/* v1.1.5 (pedido de tester): sensibilidade do analogico ESQUERDO configuravel.
 * Fontes, em ordem: env NXPORT_STICK_SENSITIVITY > arquivo sensitivity.txt no
 * diretorio do port. Valores: low (0.60) | normal (1.00) | high (1.30) ou um
 * numero 0.30..1.50. SEMANTICA: sensibilidade e RESPOSTA, nunca alcance —
 * no estilingue (controle de POSICAO) aplicamos curva expo que PRESERVA as
 * pontas: gamma = 1/valor, entao low = precisao perto do centro com estique
 * TOTAL na borda; no cursor (controle de VELOCIDADE) o valor multiplica a
 * velocidade. Limiares de posse do stick continuam no valor CRU. */
static float g_stick_sens = 1.0f;

static float ab_sens_parse(const char *text) {
  char buffer[32];
  size_t n = 0;
  float value;
  while (*text == ' ' || *text == '\t') text++;
  while (text[n] && text[n] != '\n' && text[n] != '\r' &&
         text[n] != ' ' && n < sizeof(buffer) - 1) {
    buffer[n] = (char)tolower((unsigned char)text[n]);
    n++;
  }
  buffer[n] = 0;
  if (!buffer[0]) return -1.0f;
  if (!strcmp(buffer, "low") || !strcmp(buffer, "baixa")) return 0.60f;
  if (!strcmp(buffer, "normal") || !strcmp(buffer, "auto")) return 1.00f;
  if (!strcmp(buffer, "high") || !strcmp(buffer, "alta")) return 1.30f;
  value = (float)atof(buffer);
  if (value >= 0.30f && value <= 1.50f) return value;
  return -1.0f;
}

static void ab_sens_init(const char *root) {
  const char *env = getenv("NXPORT_STICK_SENSITIVITY");
  const char *source = "padrao";
  float value = -1.0f;
  if (env && *env) {
    value = ab_sens_parse(env);
    source = "env NXPORT_STICK_SENSITIVITY";
  }
  if (value < 0.0f && root && *root) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/sensitivity.txt", root);
    f = fopen(path, "r");
    if (f) {
      /* primeira linha que NAO e comentario/vazia = o valor (o arquivo pode
       * ter um cabecalho explicativo em cima). */
      char line[96];
      while (fgets(line, sizeof(line), f)) {
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '#' || *t == '\n' || *t == '\r' || !*t)
          continue;
        value = ab_sens_parse(t);
        source = "sensitivity.txt";
        break;
      }
      fclose(f);
    }
  }
  if (value < 0.0f) {
    value = 1.0f;
    source = "padrao";
  }
  g_stick_sens = value;
  ab_log("[input] sensibilidade do analogico esquerdo = %.2f (%s)",
         (double)g_stick_sens, source);
}
static int g_pinch_close, g_pinch_open;
static int g_dpad_l, g_dpad_r;
static int g_sling_input_owner;
static int g_camera_input_owner;
static int g_sling_release_guard;
static int g_disconnect_block_guard;

static int g_peek_active;
static float g_peek_gap, g_peek_px;

static void request_quit(const char *route) {
  if (!g_quit_requested)
    ab_log("[input] saida solicitada: %s", route);
  g_quit_requested = 1;
}

static void enqueue(InputEvent event) {
  if (!g_queue_lock)
    return;
  SDL_LockMutex(g_queue_lock);
  if (g_queue_count < QUEUE_MAX)
    g_queue[g_queue_count++] = event;
  SDL_UnlockMutex(g_queue_lock);
}

static void queue_key(int keycode, int down, int device) {
  InputEvent e;
  memset(&e, 0, sizeof(e));
  e.kind = EV_KEY;
  e.a = keycode;
  e.b = down ? 1 : 0;
  e.device = device;
  enqueue(e);
}

static void queue_touch_id(int action, float x, float y, int pointer_id) {
  InputEvent e;
  memset(&e, 0, sizeof(e));
  e.kind = EV_TOUCH;
  e.a = action;
  e.b = pointer_id;
  e.x = x;
  e.y = y;
  enqueue(e);
}

static void queue_touch(int action, float x, float y) {
  queue_touch_id(action, x, y, 0);
}

void ab_input_set_screen(int width, int height) {
  g_screen_w = width > 0 ? width : 640;
  g_screen_h = height > 0 ? height : 480;
  g_pointer_x = (float)g_screen_w * 0.5f;
  g_pointer_y = (float)g_screen_h * 0.5f;
}

static float left_magnitude(void) {
  return sqrtf(g_stick_x * g_stick_x + g_stick_y * g_stick_y);
}

static void release_synthetic_touches(void) {
  if (g_cursor_buttons.touch_down)
    queue_touch(1, g_pointer_x, g_pointer_y);
  if (g_peek_active) {
    float cx = (float)g_screen_w * 0.5f;
    float cy = (float)g_screen_h * 0.5f;
    queue_touch_id(1, cx - g_peek_gap * 0.5f + g_peek_px, cy, 1);
    queue_touch_id(1, cx + g_peek_gap * 0.5f + g_peek_px, cy, 2);
  }
  memset(&g_cursor_buttons, 0, sizeof(g_cursor_buttons));
  g_peek_active = 0;
  g_stick_x = g_stick_y = 0.0f;
  g_pinch_close = g_pinch_open = 0;
  g_dpad_l = g_dpad_r = 0;
  g_sling_input_owner = g_camera_input_owner = 0;
  g_disconnect_block_guard = 2;
  ab_lua_control_set_stick(0.0f, 0.0f, 1);
  ab_lua_control_clear_buttons();
}

void ab_input_init(void) {
  nxinput_config config;

  g_queue_lock = SDL_CreateMutex();
  g_this = NULL;
  g_key = (KeyInputFn)ab_sym(
      "Java_com_rovio_fusion_MyInputHandler_nativeKeyInput");
  g_axis = (AxisInputFn)ab_sym(
      "Java_com_rovio_fusion_MyInputHandler_nativeInputAxis");
  g_touch =
      (TouchInputFn)ab_sym("Java_com_rovio_fusion_MyInputHandler_nativeInput");
  ab_log("[input] exports: key=%p axis=%p touch=%p", (void *)g_key,
         (void *)g_axis, (void *)g_touch);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  ab_sens_init(getenv("NXCOMPAT_GAME_DIR"));
  config.cursor_speed = 0.85f * g_stick_sens;
  config.cursor_smoothing = 0.07f;
  g_input = nxinput_create(&config);
  if (!g_input)
    ab_log("[input] nxinput %s falhou: %s", NXINPUT_VERSION, SDL_GetError());
  else
    ab_log("[input] nxinput %s ativo; pads=%u hint_sticks=%d", NXINPUT_VERSION,
           nxinput_connected_count(g_input),
           nxinput_host_analog_sticks_hint(g_input));
  g_last_move_ticks = SDL_GetTicks();
  memset(&g_guide_exit_chord, 0, sizeof(g_guide_exit_chord));
  ab_evdev_exit_init(g_last_move_ticks);
}

nxinput_context *ab_input_context(void) { return g_input; }

static int key_for_button(nxinput_button button) {
  switch (button) {
  case NXINPUT_BUTTON_A:
    return AKEY_BUTTON_A;
  case NXINPUT_BUTTON_B:
    return AKEY_BACK;
  case NXINPUT_BUTTON_X:
    return AKEY_BUTTON_X;
  case NXINPUT_BUTTON_Y:
    return AKEY_BUTTON_Y;
  case NXINPUT_BUTTON_BACK:
    return AKEY_BUTTON_SELECT;
  case NXINPUT_BUTTON_START:
    return AKEY_BACK;
  case NXINPUT_BUTTON_LEFT_SHOULDER:
    return AKEY_BUTTON_L1;
  case NXINPUT_BUTTON_RIGHT_SHOULDER:
    return AKEY_BUTTON_R1;
  case NXINPUT_BUTTON_RIGHT_STICK:
    return AKEY_BUTTON_THUMBR;
  case NXINPUT_BUTTON_DPAD_UP:
    return AKEY_DPAD_UP;
  case NXINPUT_BUTTON_DPAD_DOWN:
    return AKEY_DPAD_DOWN;
  case NXINPUT_BUTTON_DPAD_LEFT:
    return AKEY_DPAD_LEFT;
  case NXINPUT_BUTTON_DPAD_RIGHT:
    return AKEY_DPAD_RIGHT;
  default:
    return 0;
  }
}

static void forward_button_edges(uint32_t pressed, uint32_t released,
                                 int gameplay) {
  unsigned int button;
  for (button = 0; button < (unsigned int)NXINPUT_BUTTON_COUNT; button++) {
    uint32_t bit = NXINPUT_BUTTON_BIT(button);
    int key;

    if ((pressed & bit) != 0u) {
      if (gameplay && button == NXINPUT_BUTTON_Y)
        ab_lua_control_press_triangle();
      if (gameplay && button == NXINPUT_BUTTON_LEFT_SHOULDER)
        ab_lua_control_press_l1();
      if (gameplay && button == NXINPUT_BUTTON_RIGHT_SHOULDER)
        ab_lua_control_press_r1();
    }

    /* A and R3 globally own the pointer touch, exactly as in the approved
     * Mali-450 build. Never emit a parallel Android key: one edge has one
     * authority in both menus and gameplay. */
    if (button == NXINPUT_BUTTON_A ||
        button == NXINPUT_BUTTON_RIGHT_STICK)
      continue;

    /* Gameplay owns D-pad left/right for camera. A/R3 were already removed
     * above because the global cursor owns them. */
    if (gameplay &&
        (button == NXINPUT_BUTTON_DPAD_LEFT ||
         button == NXINPUT_BUTTON_DPAD_RIGHT))
      continue;

    key = key_for_button((nxinput_button)button);
    if (!key)
      continue;
    if ((pressed & bit) != 0u)
      queue_key(key, 1, 0);
    if ((released & bit) != 0u)
      queue_key(key, 0, 0);
  }
}

static void cursor_button_edges(uint32_t pressed, uint32_t released,
                                Uint32 now, int allow_touch_down) {
  const uint32_t a = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A);
  const uint32_t r3 = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_RIGHT_STICK);
  unsigned int actions = ab_cursor_button_update(
      &g_cursor_buttons, (pressed & a) != 0u, (released & a) != 0u,
      (pressed & r3) != 0u, (released & r3) != 0u, allow_touch_down);

  if ((actions & AB_CURSOR_TOUCH_DOWN) != 0u) {
    g_cursor_seen = now;
    queue_touch(0, g_pointer_x, g_pointer_y);
    ab_log("[input] cursor A/R3 down x=%.0f y=%.0f", g_pointer_x,
           g_pointer_y);
  }
  if ((actions & AB_CURSOR_TOUCH_UP) != 0u) {
    g_cursor_seen = now;
    queue_touch(1, g_pointer_x, g_pointer_y);
    ab_log("[input] cursor A/R3 up x=%.0f y=%.0f", g_pointer_x,
           g_pointer_y);
  }
}

static void peek_step(float dt) {
  float min_dim = (float)(g_screen_w < g_screen_h ? g_screen_w : g_screen_h);
  float gap0 = min_dim * 0.45f;
  float gap_min = min_dim * 0.10f;
  float gap_max = min_dim * 0.90f;
  float cx = (float)g_screen_w * 0.5f;
  float cy = (float)g_screen_h * 0.5f;
  int zoom = (g_pinch_close ? -1 : 0) + (g_pinch_open ? 1 : 0);
  float pan = (g_dpad_l ? 1.0f : 0.0f) -
              (g_dpad_r ? 1.0f : 0.0f);
  int active = zoom != 0 || fabsf(pan) > 0.18f;
  float f1x, f2x;

  if (!active && !g_peek_active)
    return;
  if (active && !g_peek_active) {
    g_peek_gap = gap0;
    g_peek_px = 0.0f;
    queue_touch_id(0, cx - gap0 * 0.5f, cy, 1);
    queue_touch_id(0, cx + gap0 * 0.5f, cy, 2);
    g_peek_active = 1;
    return;
  }
  if (!active) {
    queue_touch_id(1, cx - g_peek_gap * 0.5f + g_peek_px, cy, 1);
    queue_touch_id(1, cx + g_peek_gap * 0.5f + g_peek_px, cy, 2);
    g_peek_active = 0;
    return;
  }

  g_peek_gap += (float)zoom * min_dim * 1.1f * dt;
  if (g_peek_gap < gap_min)
    g_peek_gap = gap_min;
  if (g_peek_gap > gap_max)
    g_peek_gap = gap_max;
  g_peek_px += pan * (float)g_screen_w * 0.6f * dt;
  if (g_peek_px < -(float)g_screen_w * 0.45f)
    g_peek_px = -(float)g_screen_w * 0.45f;
  if (g_peek_px > (float)g_screen_w * 0.45f)
    g_peek_px = (float)g_screen_w * 0.45f;

  f1x = cx - g_peek_gap * 0.5f + g_peek_px;
  f2x = cx + g_peek_gap * 0.5f + g_peek_px;
  queue_touch_id(2, f1x, cy, 1);
  queue_touch_id(2, f2x, cy, 2);
}

void ab_input_pump(void) {
  SDL_Event event;
  nxinput_pad_state pad;
  nxinput_cursor_state cursor;
  Uint32 now = SDL_GetTicks();
  float dt = (float)(now - g_last_move_ticks) / 1000.0f;
  int gameplay = ab_lua_control_is_gameplay();
  int slot;
  uint32_t pressed;
  uint32_t released;
  uint32_t capabilities = 0u;

  g_last_move_ticks = now;
  if (dt > 0.1f)
    dt = 0.1f;
  if (dt < 0.0f)
    dt = 0.0f;

  while (SDL_PollEvent(&event)) {
    if (g_input)
      nxinput_observe_event(g_input, &event);
    if (event.type == SDL_QUIT)
      request_quit("SDL_QUIT");
    else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
      request_quit("ESC");
  }

  if (ab_evdev_exit_poll(now))
    request_quit("SELECT+START evdev");

  if (!g_input)
    return;
  nxinput_poll(g_input);
  if (nxinput_consume_quit_request(g_input))
    request_quit("SELECT+START SDL BACK");

  slot = nxinput_first_connected(g_input);
  if (slot < 0 || !nxinput_get_pad(g_input, (unsigned int)slot, &pad)) {
    (void)ab_exit_chord_update(&g_guide_exit_chord, 0, 0);
    if (g_active_slot >= 0)
      release_synthetic_touches();
    g_active_slot = -1;
    g_cursor_warped = 0;
    return;
  }

  /* A few handheld mappings expose the physical SELECT key as GUIDE. This is
   * an exit-only alias: GUIDE is never forwarded to the Android game. */
  if (ab_exit_chord_update(
          &g_guide_exit_chord,
          (pad.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_GUIDE)) != 0u,
          (pad.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_START)) != 0u))
    request_quit("SELECT+START SDL GUIDE");

  if (g_active_slot != slot || g_pad_generation != pad.generation) {
    if (g_active_slot >= 0)
      release_synthetic_touches();
    g_active_slot = slot;
    g_pad_generation = pad.generation;
    g_cursor_warped = 0;
    ab_log("[input] topologia=%u pads=%u", pad.generation,
           nxinput_connected_count(g_input));
  }

  if (g_logged_gameplay != gameplay) {
    g_logged_gameplay = gameplay;
    ab_log("[input] contexto=%s", gameplay ? "gameplay" : "menu");
  }

  pressed = nxinput_consume_pressed(g_input, (unsigned int)slot,
                                    NXINPUT_BUTTON_MASK_ALL);
  released = nxinput_consume_released(g_input, (unsigned int)slot,
                                      NXINPUT_BUTTON_MASK_ALL);
  forward_button_edges(pressed, released, gameplay);

  g_stick_x = pad.left_x;
  g_stick_y = pad.left_y;
  g_dpad_l =
      (pad.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT)) != 0u;
  g_dpad_r =
      (pad.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT)) != 0u;
  g_pinch_close = pad.left_trigger > 0.45f;
  g_pinch_open = pad.right_trigger > 0.45f;

  cursor_button_edges(
      pressed, released, now,
      !g_sling_input_owner && !g_camera_input_owner &&
          left_magnitude() < 0.12f);

  if (gameplay) {
    float magnitude = left_magnitude();
    int sling_owned_before = g_sling_input_owner;
    int camera_requested = g_pinch_close || g_pinch_open || g_dpad_l ||
                           g_dpad_r;

    if (!g_sling_input_owner && !g_camera_input_owner &&
        !g_sling_release_guard && !g_cursor_buttons.touch_down &&
        magnitude > 0.22f) {
      g_sling_input_owner = 1;
      ab_log("[sling] controle assumiu o estilingue");
    }
    /* A remains the global pointer click everywhere else. While an existing
     * left-stick sling gesture owns input, route only its fresh press to Lua;
     * Lua is the final authority and launches only if a real bird is dragged. */
    if (sling_owned_before &&
        (pressed & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A)) != 0u)
      ab_lua_control_press_launch();
    if (g_sling_input_owner && magnitude < 0.12f) {
      g_sling_input_owner = 0;
      g_sling_release_guard = 2;
      ab_log("[sling] analogico solto");
    }
    if (!g_camera_input_owner && camera_requested &&
        !g_sling_input_owner && !g_sling_release_guard &&
        !g_cursor_buttons.touch_down && magnitude < 0.12f) {
      g_camera_input_owner = 1;
      ab_log("[camera] controle assumiu pan/zoom");
    }
    if (g_camera_input_owner)
      peek_step(dt);
    if (g_camera_input_owner && !camera_requested && !g_peek_active) {
      g_camera_input_owner = 0;
      ab_log("[camera] pan/zoom solto");
    }

    {
      /* curva expo por MAGNITUDE (preserva direcao e as pontas 0 e 1) +
       * SUAVIZACAO DE TAXA quando sens < 1: o vetor entregue PERSEGUE o
       * analogico com velocidade limitada — movimento fino tambem esticado
       * (girar a mira na borda), e continua alcancando qualquer posicao. */
      static float g_dx, g_dy;
      float mag = left_magnitude();
      float shaped = 1.0f;
      float tx, ty;
      if (mag > 0.0001f && g_stick_sens > 0.0f) {
        float m = mag > 1.0f ? 1.0f : mag;
        shaped = powf(m, 1.0f / g_stick_sens) / mag;
      }
      tx = g_stick_x * shaped;
      ty = g_stick_y * shaped;
      if (g_stick_sens < 0.999f) {
        float rate = 6.0f * g_stick_sens * dt; /* low=0.6 -> 3.6/s */
        float ex = tx - g_dx, ey = ty - g_dy;
        float ed = sqrtf(ex * ex + ey * ey);
        if (ed > rate && ed > 0.0001f) {
          g_dx += ex * (rate / ed);
          g_dy += ey * (rate / ed);
        } else {
          g_dx = tx;
          g_dy = ty;
        }
      } else {
        g_dx = tx;
        g_dy = ty;
      }
      ab_lua_control_set_stick(
          g_dx, g_dy,
        g_camera_input_owner || g_cursor_buttons.touch_down ||
            g_disconnect_block_guard > 0);
    }
    if (g_sling_release_guard > 0)
      g_sling_release_guard--;
    if (g_disconnect_block_guard > 0)
      g_disconnect_block_guard--;
  } else {
    if (g_peek_active) {
      float cx = (float)g_screen_w * 0.5f;
      float cy = (float)g_screen_h * 0.5f;
      queue_touch_id(1, cx - g_peek_gap * 0.5f + g_peek_px, cy, 1);
      queue_touch_id(1, cx + g_peek_gap * 0.5f + g_peek_px, cy, 2);
      g_peek_active = 0;
    }
    g_sling_input_owner = 0;
    g_camera_input_owner = 0;
    g_sling_release_guard = 0;
    ab_lua_control_set_stick(0.0f, 0.0f, 1);
  }

  /* The Angry Birds pointer is global, not a menu/gameplay mode switch. The
   * right stick always owns it; only devices without one may use the left
   * stick fallback, and never during gameplay where left owns the slingshot. */
  nxinput_set_cursor_context(g_input, NXINPUT_CURSOR_MENU);
  if (!g_cursor_warped) {
    nxinput_cursor_warp(g_input, (unsigned int)slot, 0.5f, 0.5f);
    g_cursor_warped = 1;
  }
  if (!nxinput_get_pad_capabilities(g_input, (unsigned int)slot,
                                    &capabilities))
    capabilities = 0u;
  if (nxinput_cursor_update_with_options(
          g_input, (unsigned int)slot, dt,
          (capabilities & NXINPUT_PAD_CAP_RIGHT_STICK)
              ? NXINPUT_CURSOR_OPTION_NONE
              : (gameplay ? NXINPUT_CURSOR_OPTION_NONE
                          : NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING),
          &cursor)) {
    g_pointer_x = cursor.x * (float)(g_screen_w - 1);
    g_pointer_y = cursor.y * (float)(g_screen_h - 1);
    if (cursor.moved) {
      g_cursor_seen = now;
      queue_touch(g_cursor_buttons.touch_down ? 2 : 7, g_pointer_x,
                  g_pointer_y);
    }
  }
  /* Drain nxinput's R3-only convenience latch because this adapter owns the
   * proven A/R3 combined press/hold/release contract below. */
  (void)nxinput_cursor_consume_click(g_input, (unsigned int)slot);
}

/* Consumido logo antes de nativeUpdate, como o handleEvents() do APK. */
void ab_input_flush(void) {
  InputEvent local[QUEUE_MAX];
  int count;
  int i;
  void *env = ab_jni_env();
  if (!g_queue_lock)
    return;
  SDL_LockMutex(g_queue_lock);
  count = g_queue_count;
  if (count)
    memcpy(local, g_queue, sizeof(InputEvent) * (size_t)count);
  g_queue_count = 0;
  SDL_UnlockMutex(g_queue_lock);
  for (i = 0; i < count; i++) {
    InputEvent *e = &local[i];
    if (e->kind == EV_KEY && g_key)
      g_key(env, g_this, e->a, e->b, 0, e->device);
    else if (e->kind == EV_AXIS && g_axis)
      g_axis(env, g_this, e->a, e->x, e->device);
    else if (e->kind == EV_TOUCH && g_touch)
      g_touch(env, g_this, e->a, e->x, e->y, e->b);
  }
}

/* Cursor global: seta clássica com contorno. Some após dois segundos parada e
 * reaparece no primeiro movimento/clique, em menus e gameplay. */
static const GLfloat CUR_VERTS[] = {
    /* seta em px (y pra baixo), ponta na origem; 5 triângulos */
    0.0f, 0.0f, 0.0f, 16.0f, 4.3f,  12.5f, /* haste esquerda */
    0.0f, 0.0f, 4.3f, 12.5f, 6.7f,  11.5f, /* miolo */
    0.0f, 0.0f, 6.7f, 11.5f, 10.5f, 11.0f, /* aba direita */
    4.3f, 12.5f, 7.3f, 19.2f, 9.7f, 18.2f, /* rabinho a */
    4.3f, 12.5f, 9.7f, 18.2f, 6.7f, 11.5f, /* rabinho b */
};
static GLuint g_cur_prog;
static GLint g_cur_a_pos, g_cur_u_origin, g_cur_u_scale, g_cur_u_size,
    g_cur_u_color;

static GLuint cursor_compile(GLenum kind, const char *src) {
  GLuint sh = glCreateShader(kind);
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  return sh;
}

static int cursor_prog_init(void) {
  static const char *vs =
      "attribute vec2 a_pos;\n"
      "uniform vec2 u_origin; uniform vec2 u_scale; uniform float u_size;\n"
      "void main(){ vec2 p=(u_origin+a_pos*u_size)*u_scale+vec2(-1.0,1.0);\n"
      "  gl_Position=vec4(p,0.0,1.0); }\n";
  static const char *fs =
      "precision mediump float; uniform vec4 u_color;\n"
      "void main(){ gl_FragColor=u_color; }\n";
  GLint ok = 0;
  GLuint v, f;
  if (g_cur_prog)
    return 1;
  v = cursor_compile(GL_VERTEX_SHADER, vs);
  f = cursor_compile(GL_FRAGMENT_SHADER, fs);
  g_cur_prog = glCreateProgram();
  glAttachShader(g_cur_prog, v);
  glAttachShader(g_cur_prog, f);
  /* Use a deterministic slot so saving/restoring vertex state never assumes
   * the linker's attribute assignment. */
  glBindAttribLocation(g_cur_prog, 0, "a_pos");
  glLinkProgram(g_cur_prog);
  glDeleteShader(v);
  glDeleteShader(f);
  glGetProgramiv(g_cur_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    ab_log("[input] cursor: programa GL nao linkou");
    glDeleteProgram(g_cur_prog);
    g_cur_prog = 0;
    return 0;
  }
  g_cur_a_pos = glGetAttribLocation(g_cur_prog, "a_pos");
  g_cur_u_origin = glGetUniformLocation(g_cur_prog, "u_origin");
  g_cur_u_scale = glGetUniformLocation(g_cur_prog, "u_scale");
  g_cur_u_size = glGetUniformLocation(g_cur_prog, "u_size");
  g_cur_u_color = glGetUniformLocation(g_cur_prog, "u_color");
  return 1;
}

void ab_input_draw_cursor(void) {
  GLint old_prog, old_abuf, old_blend, old_depth, old_scissor, old_cull;
  GLint va_enabled, va_size, va_type, va_norm, va_stride, va_buf;
  void *va_ptr;
  float size;
  if (!g_cursor_seen || SDL_GetTicks() - g_cursor_seen > 2000) {
    if (g_cursor_draw_visible) {
      g_cursor_draw_visible = 0;
      ab_log("[input] cursor oculto por inatividade");
    }
    return;
  }
  if (!g_cursor_draw_visible) {
    g_cursor_draw_visible = 1;
    ab_log("[input] cursor visivel");
  }
  if (!cursor_prog_init())
    return;

  glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_abuf);
  old_blend = glIsEnabled(GL_BLEND);
  old_depth = glIsEnabled(GL_DEPTH_TEST);
  old_scissor = glIsEnabled(GL_SCISSOR_TEST);
  old_cull = glIsEnabled(GL_CULL_FACE);
  glGetVertexAttribiv((GLuint)g_cur_a_pos, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                     &va_enabled);
  glGetVertexAttribiv((GLuint)g_cur_a_pos, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                     &va_size);
  glGetVertexAttribiv((GLuint)g_cur_a_pos, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                     &va_type);
  glGetVertexAttribiv((GLuint)g_cur_a_pos, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                     &va_norm);
  glGetVertexAttribiv((GLuint)g_cur_a_pos, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                     &va_stride);
  glGetVertexAttribiv((GLuint)g_cur_a_pos,
                     GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &va_buf);
  glGetVertexAttribPointerv((GLuint)g_cur_a_pos,
                           GL_VERTEX_ATTRIB_ARRAY_POINTER, &va_ptr);

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(g_cur_prog);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray((GLuint)g_cur_a_pos);
  glVertexAttribPointer((GLuint)g_cur_a_pos, 2, GL_FLOAT, GL_FALSE, 0,
                        CUR_VERTS);
  glUniform2f(g_cur_u_scale, 2.0f / (float)g_screen_w,
              -2.0f / (float)g_screen_h);
  glUniform2f(g_cur_u_origin, g_pointer_x, g_pointer_y);
  size = g_cursor_buttons.touch_down ? 1.25f : 1.6f; /* encolhe no clique */
  /* contorno preto por trás, corpo branco na frente */
  glUniform1f(g_cur_u_size, size * 1.35f);
  glUniform4f(g_cur_u_color, 0.0f, 0.0f, 0.0f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 0, 15);
  glUniform1f(g_cur_u_size, size);
  glUniform4f(g_cur_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 0, 15);

  /* restaura o mundo como estava */
  if (!va_enabled)
    glDisableVertexAttribArray((GLuint)g_cur_a_pos);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)va_buf);
  if (va_enabled || va_ptr)
    glVertexAttribPointer((GLuint)g_cur_a_pos, va_size, (GLenum)va_type,
                          (GLboolean)va_norm, va_stride, va_ptr);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_abuf);
  glUseProgram((GLuint)old_prog);
  if (old_blend)
    glEnable(GL_BLEND);
  if (old_depth)
    glEnable(GL_DEPTH_TEST);
  if (old_scissor)
    glEnable(GL_SCISSOR_TEST);
  if (old_cull)
    glEnable(GL_CULL_FACE);
}

void ab_input_pointer(float *x, float *y, int *down) {
  if (x)
    *x = g_pointer_x;
  if (y)
    *y = g_pointer_y;
  if (down)
    *down = g_cursor_buttons.touch_down;
}

int ab_input_quit_requested(void) { return g_quit_requested; }

void ab_input_shutdown(void) {
  release_synthetic_touches();
  ab_evdev_exit_shutdown();
  nxinput_destroy(g_input);
  g_input = NULL;
  if (g_queue_lock) {
    SDL_DestroyMutex(g_queue_lock);
    g_queue_lock = NULL;
  }
}
