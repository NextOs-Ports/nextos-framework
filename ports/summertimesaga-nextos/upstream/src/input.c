// Controller input for Summertime/Ren'Py: register a virtual SDL joystick and
// feed SDL controller events from evdev.
// for one - "process_joysticks: no joystick selected") and feed it physical
// gamepad events read from evdev, translated to SDL 2.0.8's android callbacks
// (onNativePadDown/Up, onNativeHat, onNativeJoy).
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIO_CURSOR
#define FBIO_CURSOR _IOWR('F', 0x08, struct fb_cursor)
#endif

#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

// Android keycodes
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BACK 4
#define AKEYCODE_MENU 82
#define AKEYCODE_SPACE 62
#define AKEYCODE_ENTER 66
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_L2 104
#define AKEYCODE_BUTTON_R2 105
#define AKEYCODE_BUTTON_THUMBL 106
#define AKEYCODE_BUTTON_THUMBR 107
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_BUTTON_MODE 110

#define DEV_ID 0x10001

static void *g_env, *g_cls;
static void (*p_onPadDown)(void *, void *, int, int);
static void (*p_onPadUp)(void *, void *, int, int);
static void (*p_onJoy)(void *, void *, int, int, float);
static void (*p_onHat)(void *, void *, int, int, int, int);
static void (*p_onKeyDown)(void *, void *, int);
static void (*p_onKeyUp)(void *, void *, int);
static void (*p_onTouch)(void *, void *, int, int, int, float, float, float);
static void (*p_onMouse)(void *, void *, int, int, float, float, unsigned char);

extern int summertime_screen_w;
extern int summertime_screen_h;

static float g_mouse_x = 0.84f;
static float g_mouse_y = 0.16f;
volatile float summertime_cursor_x = 0.84f;
volatile float summertime_cursor_y = 0.16f;
static volatile int g_cursor_hat_x = 0;
static volatile int g_cursor_hat_y = 0;
static volatile float g_cursor_axis_x = 0.0f;
static volatile float g_cursor_axis_y = 0.0f;
static volatile int g_cursor_thread_started = 0;
static unsigned g_cursor_seq = 1;
static unsigned g_click_seq = 1;
static int g_last_cursor_file_x = -10000;
static int g_last_cursor_file_y = -10000;
static int64_t g_next_cursor_file_ns = 0;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000000000LL) + (int64_t)ts.tv_nsec;
}

/* ======================================================================
 * Cursor de HARDWARE (Amlogic OSD2 = /dev/fb1 + osd_cursor_hw no kernel).
 * A seta e uma camada de 32x32 que o chip compoe por cima do jogo; mover
 * custa 1 ioctl(FBIO_CURSOR) -> fluidez total, INDEPENDENTE do fps da
 * engine (que nas cenas pesadas roda a ~10fps). Fallback automatico: se
 * /dev/fb1 nao existir (R36S etc.), o cursor da engine continua valendo.
 * Hover: o Ren'Py escreve /dev/shm/summertime_hover; a seta fica VERMELHA.
 * ====================================================================== */
static int g_fb1_fd = -1;
static uint16_t *g_fb1_px; /* RGB565: unico formato com COLOR KEY no driver */
static int g_fb1_hover_state = -1;

/* Seta classica pre-rasterizada (tools/gen_cursor_arrow.py) em RGB565.
 * Transparencia: o driver OSD so aplica COLOR KEY em formatos 16/24-bit
 * (store_color_key: 32-bit ARGB cai no default e ignora — confirmado no
 * fonte do kernel). Entao o fb1 roda em RGB565 e o fundo magenta 0xF81F
 * vira invisivel via color_key. */
#include "cursor_arrow.h"

static void fb1_paint(int hover) {
  if (!g_fb1_px)
    return;
  const unsigned short *sprite =
      hover ? summertime_fb1_arrow_hover : summertime_fb1_arrow;
  memcpy(g_fb1_px, sprite, 32 * 32 * 2);
  g_fb1_hover_state = hover;
}

static void fb1_write_sysfs(const char *file, const char *val) {
  char p[128];
  snprintf(p, sizeof(p), "/sys/class/graphics/fb1/%s", file);
  int fd = open(p, O_WRONLY);
  if (fd >= 0) {
    (void)!write(fd, val, strlen(val));
    close(fd);
  }
}

static int fb1_init(void) {
  if (getenv("SUMMERTIME_NO_FB1"))
    return -1;
  g_fb1_fd = open("/dev/fb1", O_RDWR | O_CLOEXEC);
  if (g_fb1_fd < 0)
    return -1;
  struct fb_var_screeninfo vi;
  if (ioctl(g_fb1_fd, FBIOGET_VSCREENINFO, &vi) < 0) {
    close(g_fb1_fd);
    g_fb1_fd = -1;
    return -1;
  }
  /* RGB565 explicito (offsets/lens fazem o driver escolher
   * COLOR_INDEX_16_565, o formato onde o color key funciona) */
  vi.xres = vi.xres_virtual = 32;
  vi.yres = vi.yres_virtual = 32;
  vi.bits_per_pixel = 16;
  vi.red.offset = 11; vi.red.length = 5;
  vi.green.offset = 5; vi.green.length = 6;
  vi.blue.offset = 0; vi.blue.length = 5;
  vi.transp.offset = 0; vi.transp.length = 0;
  if (ioctl(g_fb1_fd, FBIOPUT_VSCREENINFO, &vi) < 0)
    debugPrintf("fb1: FBIOPUT 565 falhou (segue mesmo assim)\n");
  g_fb1_px = mmap(NULL, 32 * 32 * 2, PROT_READ | PROT_WRITE, MAP_SHARED,
                  g_fb1_fd, 0);
  if (g_fb1_px == MAP_FAILED) {
    g_fb1_px = NULL;
    close(g_fb1_fd);
    g_fb1_fd = -1;
    return -1;
  }
  fb1_paint(0);
  /* color key: em 565 o valor e o proprio pixel 565 (kernel expande
   * r=(k>>11)<<3 g=(k>>5)<<2 b=k<<3). Magenta = 0xf81f. */
  fb1_write_sysfs("color_key", "f81f");
  fb1_write_sysfs("enable_key", "1");
  ioctl(g_fb1_fd, FBIOBLANK, FB_BLANK_UNBLANK);
  /* avisa o core.py (Ren'Py) que a seta agora e por hardware: ele para de
   * desenhar o crosshair e de forcar frames por movimento */
  setenv("SUMMERTIME_FB1_CURSOR", "1", 1);
  debugPrintf("fb1: cursor de HARDWARE (OSD2) ativo\n");
  return 0;
}

static void fb1_move(int x, int y) {
  if (g_fb1_fd < 0)
    return;
  struct fb_cursor cur;
  memset(&cur, 0, sizeof(cur));
  cur.hot.x = (uint16_t)x;
  cur.hot.y = (uint16_t)y;
  ioctl(g_fb1_fd, FBIO_CURSOR, &cur);
}

static void fb1_check_hover(void) {
  if (g_fb1_fd < 0)
    return;
  int hov = 0;
  FILE *f = fopen("/dev/shm/summertime_hover", "r");
  if (f) {
    hov = (fgetc(f) == '1');
    fclose(f);
  }
  if (hov != g_fb1_hover_state)
    fb1_paint(hov);
}

/* saida limpa: esconde a camada de hardware (senao a seta fica por cima da
 * EmulationStation depois do _exit) */
void summertime_fb1_blank(void) {
  if (g_fb1_fd >= 0)
    ioctl(g_fb1_fd, FBIOBLANK, FB_BLANK_NORMAL);
}

static void emit_pointer_file(const char *path, unsigned seq, float nx, float ny) {
  char tmp[96];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  int x = (int)(nx * (float)summertime_screen_w);
  int y = (int)(ny * (float)summertime_screen_h);
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= summertime_screen_w) x = summertime_screen_w - 1;
  if (y >= summertime_screen_h) y = summertime_screen_h - 1;
  fprintf(f, "%u %d %d\n", seq, x, y);
  fclose(f);
  rename(tmp, path);
}

static void mouse_move_to(float nx, float ny) {
  if (nx < 0.0f) nx = 0.0f;
  if (ny < 0.0f) ny = 0.0f;
  if (nx > 1.0f) nx = 1.0f;
  if (ny > 1.0f) ny = 1.0f;
  g_mouse_x = nx;
  g_mouse_y = ny;
  summertime_cursor_x = nx;
  summertime_cursor_y = ny;

  if (getenv("SUMMERTIME_HOVER") || getenv("SUMMERTIME_RENPY_CURSOR")) {
    int x = (int)(nx * (float)summertime_screen_w);
    int y = (int)(ny * (float)summertime_screen_h);
    int64_t now = now_ns();
    if (now >= g_next_cursor_file_ns ||
        abs(x - g_last_cursor_file_x) >= 8 ||
        abs(y - g_last_cursor_file_y) >= 8) {
      g_last_cursor_file_x = x;
      g_last_cursor_file_y = y;
      g_next_cursor_file_ns = now + 16000000LL;
      emit_pointer_file("/dev/shm/summertime_vcursor", g_cursor_seq++, nx, ny);
    }
  }
}

static void mouse_move_by(float dx, float dy) {
  mouse_move_to(g_mouse_x + dx, g_mouse_y + dy);
}

static void mouse_click_current(void) {
  if (!p_onMouse)
    return;
  float x = g_mouse_x * (float)summertime_screen_w;
  float y = g_mouse_y * (float)summertime_screen_h;
  p_onMouse(g_env, g_cls, 0, 2 /*MOVE*/, x, y, 0);
  usleep(25000);
  p_onMouse(g_env, g_cls, 1, 0 /*DOWN*/, x, y, 0);
  usleep(70000);
  p_onMouse(g_env, g_cls, 0, 1 /*UP*/, x, y, 0);
}

static void touch_tap(float x, float y) {
  if (!p_onTouch)
    return;
  p_onTouch(g_env, g_cls, 0, 0, 0 /*DOWN*/, x, y, 1.0f);
  usleep(60000);
  p_onTouch(g_env, g_cls, 0, 0, 1 /*UP*/, x, y, 1.0f);
}

static void key_press(int keycode) {
  if (!p_onKeyDown || !p_onKeyUp)
    return;
  p_onKeyDown(g_env, g_cls, keycode);
  usleep(70000);
  p_onKeyUp(g_env, g_cls, keycode);
}

static void key_state(int keycode, int down) {
  if (down) {
    if (p_onKeyDown)
      p_onKeyDown(g_env, g_cls, keycode);
  } else {
    if (p_onKeyUp)
      p_onKeyUp(g_env, g_cls, keycode);
  }
}

static void *cursor_thread(void *arg) {
  (void)arg;
  float speed_scale = 1.0f;
  const char *speed_value = getenv("SUMMERTIME_CURSOR_SCALE");
  if (speed_value && *speed_value) {
    char *end = NULL;
    float parsed = strtof(speed_value, &end);
    if (end != speed_value && end && *end == '\0' && parsed >= 0.20f &&
        parsed <= 2.00f)
      speed_scale = parsed;
  }
  debugPrintf("input: escala de velocidade do cursor=%.2f\n", speed_scale);

  int64_t last = now_ns();
  for (;;) {
    int64_t now = now_ns();
    float dt = (float)(now - last) / 1000000000.0f;
    last = now;
    if (dt < 0.001f)
      dt = 0.001f;
    if (dt > 0.050f)
      dt = 0.050f;

    /* Velocidade ~10% mais lenta (pedido do usuario): 0.62->0.558,
     * 0.24->0.216, 0.86->0.774. */
    float ax = g_cursor_axis_x;
    float ay = g_cursor_axis_y;
    float dx = (float)g_cursor_hat_x * 0.558f * speed_scale * dt;
    float dy = (float)g_cursor_hat_y * 0.558f * speed_scale * dt;

    if (ax > 0.18f || ax < -0.18f) {
      float sx = ax < 0.0f ? -1.0f : 1.0f;
      float n = (sx * ax - 0.18f) / 0.82f;
      dx += sx * (0.216f + 0.774f * n * n) * speed_scale * dt;
    }
    if (ay > 0.18f || ay < -0.18f) {
      float sy = ay < 0.0f ? -1.0f : 1.0f;
      float n = (sy * ay - 0.18f) / 0.82f;
      dy += sy * (0.216f + 0.774f * n * n) * speed_scale * dt;
    }

    if (dx != 0.0f || dy != 0.0f)
      mouse_move_by(dx, dy);

    /* cursor de hardware: reposiciona a camada OSD2 a cada tick (8ms =
     * 125Hz), custo ~1 ioctl. E o que da a fluidez total da seta. */
    if (g_fb1_fd >= 0) {
      static int last_hx = -1, last_hy = -1;
      static int hover_div = 0;
      int hx = (int)(g_mouse_x * (float)summertime_screen_w);
      int hy = (int)(g_mouse_y * (float)summertime_screen_h);
      if (hx != last_hx || hy != last_hy) {
        fb1_move(hx, hy);
        last_hx = hx;
        last_hy = hy;
      }
      if (++hover_div >= 6) { /* ~48ms: checa cor do hover */
        hover_div = 0;
        fb1_check_hover();
      }
    }

    usleep(8000);
  }
  return NULL;
}

static void start_cursor_thread(void) {
  if (g_cursor_thread_started)
    return;
  g_cursor_thread_started = 1;
  pthread_t t;
  pthread_create(&t, NULL, cursor_thread, NULL);
  pthread_detach(t);
}

static void confirm_current(void) {
  emit_pointer_file("/dev/shm/summertime_vclick", g_click_seq++, g_mouse_x, g_mouse_y);
}

static void back_current(void) {
  /* BACK do Android (keycode 4): o build touch original fecha QUALQUER tela
   * com ele (telefone, mochila, mapa, dialogos) — voltar universal, em vez
   * de clique posicional que so acertava o canto de algumas telas. */
  key_press(AKEYCODE_BACK);
}

static void menu_current(void) {
  key_press(AKEYCODE_MENU);
}

static void pointer_tap(float nx, float ny) {
  if (p_onMouse) {
    mouse_move_to(nx, ny);
    mouse_click_current();
  }

  touch_tap(nx, ny);
  key_press(AKEYCODE_ENTER);
}
// SDL 2.0.6-style (no vendor/product):
// (env, cls, device_id, name, desc, is_accelerometer, button_mask, naxes,
//  nhats, nballs)
static int (*p_addJoystick)(void *, void *, int, void *, void *, int, int, int,
                            int, int);

// Standard Xbox button positions (matches SDL/ES naming). Used as the
// normalized layer so every controller is treated as an Xbox pad.
enum {
  POS_A = 0, // bottom  (PS cross, Xbox A)
  POS_B,     // right   (PS circle, Xbox B)
  POS_X,     // left    (PS square, Xbox X)
  POS_Y,     // top     (PS triangle, Xbox Y)
  POS_L1,
  POS_R1,
  POS_L2,
  POS_R2,
  POS_SELECT,
  POS_START,
  POS_L3,
  POS_R3,
  POS_COUNT
};
static const char *pos_names[POS_COUNT] = {
    "a", "b", "x", "y", "l1", "r1", "l2", "r2", "select", "start", "l3", "r3"};

// Editable controller config (".gptk"): physical Xbox position -> the Android
// button keycode the game receives. Default = Xbox standard (a->A, b->B...).
// Edit summertimesaga.gptk in the game dir to remap any button without rebuilding.
static int g_btnmap[POS_COUNT] = {
    AKEYCODE_BUTTON_A,      AKEYCODE_BUTTON_B,     AKEYCODE_BUTTON_X,
    AKEYCODE_BUTTON_Y,      AKEYCODE_BUTTON_L1,    AKEYCODE_BUTTON_R1,
    AKEYCODE_BUTTON_L2,     AKEYCODE_BUTTON_R2,    AKEYCODE_BUTTON_SELECT,
    AKEYCODE_BUTTON_START,  AKEYCODE_BUTTON_THUMBL, AKEYCODE_BUTTON_THUMBR};

// Map a raw evdev button code to a normalized Xbox position. Handles both the
// BTN_GAMEPAD range (0x130, real Xbox/PS pads) and the BTN_JOYSTICK range
// (0x120, generic " USB Gamepad" — order from the device es_input.cfg).
static int evdev_to_pos(int code) {
  switch (code) {
  case BTN_SOUTH:   return POS_A;
  case BTN_EAST:    return POS_B;
  case BTN_WEST:    return POS_X; // left
  case BTN_NORTH:   return POS_Y; // top
  case BTN_TL:      return POS_L1;
  case BTN_TR:      return POS_R1;
  case BTN_TL2:     return POS_L2;
  case BTN_TR2:     return POS_R2;
  case BTN_SELECT:  return POS_SELECT;
  case BTN_START:   return POS_START;
  case BTN_THUMBL:  return POS_L3;
  case BTN_THUMBR:  return POS_R3;
  // generic " USB Gamepad" (ordem REAL confirmada com captura de botões)
  case BTN_TRIGGER: return POS_Y; // 0x120 = Xbox Y
  case BTN_THUMB:   return POS_B; // 0x121 = Xbox B
  case BTN_THUMB2:  return POS_A; // 0x122 = Xbox A
  case BTN_TOP:     return POS_X; // 0x123 = Xbox X
  case BTN_TOP2:    return POS_L1;
  case BTN_PINKIE:  return POS_R1;
  case BTN_BASE:    return POS_L2;
  case BTN_BASE2:   return POS_R2;
  case BTN_BASE3:   return POS_SELECT;
  case BTN_BASE4:   return POS_START;
  case BTN_BASE5:   return POS_L3;
  case BTN_BASE6:   return POS_R3;
  // Familia RG351/GO-Super (odroidgo-joypad): SELECT/START (e F3/F4) saem
  // como BTN_TRIGGER_HAPPY em vez de BTN_SELECT/BTN_START.
  case BTN_TRIGGER_HAPPY1: return POS_SELECT;
  case BTN_TRIGGER_HAPPY2: return POS_START;
  case BTN_TRIGGER_HAPPY3: return POS_L3;
  case BTN_TRIGGER_HAPPY4: return POS_R3;
  default:          return -1;
  }
}

static int name_to_keycode(const char *v) {
  struct {
    const char *n;
    int kc;
  } t[] = {{"a", AKEYCODE_BUTTON_A},        {"b", AKEYCODE_BUTTON_B},
           {"x", AKEYCODE_BUTTON_X},        {"y", AKEYCODE_BUTTON_Y},
           {"l1", AKEYCODE_BUTTON_L1},      {"r1", AKEYCODE_BUTTON_R1},
           {"l2", AKEYCODE_BUTTON_L2},      {"r2", AKEYCODE_BUTTON_R2},
           {"select", AKEYCODE_BUTTON_SELECT}, {"start", AKEYCODE_BUTTON_START},
           {"l3", AKEYCODE_BUTTON_THUMBL}, {"r3", AKEYCODE_BUTTON_THUMBR},
           {"mode", AKEYCODE_BUTTON_MODE}};
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
    if (strcasecmp(v, t[i].n) == 0)
      return t[i].kc;
  return atoi(v); // allow a raw keycode number too
}

/* Atalhos nativos por TAP: "botao = tap:X,Y" no .gptk, com X/Y normalizados
 * na AREA 16:9 do jogo (0..1). O botao teleporta a seta ate o alvo e clica
 * pelo caminho ja validado (vclick), entao funciona em qualquer tela/HUD
 * de toque. -1 = desativado. */
static float g_tapmap[POS_COUNT][2];

static void tap_game_norm(float gx, float gy) {
  float sw = (float)summertime_screen_w;
  float sh = (float)summertime_screen_h;
  if (sw <= 0.0f || sh <= 0.0f)
    return;
  /* mesmo aspect-fit do Ren'Py: 16:9 (1280x720) dentro da tela fisica */
  float scale = sw / 1280.0f;
  if (sh / 720.0f < scale)
    scale = sh / 720.0f;
  float dw = 1280.0f * scale, dh = 720.0f * scale;
  float off_x = (sw - dw) / 2.0f, off_y = (sh - dh) / 2.0f;
  mouse_move_to((off_x + gx * dw) / sw, (off_y + gy * dh) / sh);
  usleep(30000); /* um frame p/ o motion assentar antes do clique */
  confirm_current();
}

// Load summertimesaga.gptk: lines "physpos = gamebutton" (e.g. "a = b" makes the bottom
// button send the game's B), or "physpos = tap:X,Y" for native tap shortcuts.
// '#' comments. Missing file -> defaults kept.
static void load_gptk(void) {
  for (int p = 0; p < POS_COUNT; p++)
    g_tapmap[p][0] = g_tapmap[p][1] = -1.0f;
  FILE *f = fopen("summertimesaga.gptk", "r");
  if (!f) {
    debugPrintf("input: summertimesaga.gptk nao encontrado, usando padrao Xbox\n");
    return;
  }
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char *h = strchr(line, '#');
    if (h)
      *h = '\0';
    char key[32], val[32];
    float tx, ty;
    if (sscanf(line, " %31[a-zA-Z0-9_] = tap : %f , %f", key, &tx, &ty) == 3) {
      for (int p = 0; p < POS_COUNT; p++)
        if (strcasecmp(key, pos_names[p]) == 0) {
          g_tapmap[p][0] = tx;
          g_tapmap[p][1] = ty;
          debugPrintf("input: gptk %s -> tap %.3f,%.3f\n", key, tx, ty);
        }
    } else if (sscanf(line, " %31[a-zA-Z0-9_] = %31[a-zA-Z0-9_]", key, val) == 2) {
      for (int p = 0; p < POS_COUNT; p++)
        if (strcasecmp(key, pos_names[p]) == 0) {
          g_btnmap[p] = name_to_keycode(val);
          if (getenv("SUMMERTIME_VERBOSE"))
            debugPrintf("input: gptk %s -> %s (kc=%d)\n", key, val, g_btnmap[p]);
        }
    }
  }
  fclose(f);
}

static int evkey_to_android(int code) {
  int p = evdev_to_pos(code);
  if (p < 0)
    return -1;
  return g_btnmap[p];
}

static int test_bit(const unsigned long *arr, int b) {
  return (arr[b / (8 * sizeof(long))] >> (b % (8 * sizeof(long)))) & 1UL;
}

// Open the first evdev node that looks like a gamepad/joystick: has gamepad
// buttons (0x130 range) OR joystick buttons (0x120 range) AND ABS_X.
static int open_gamepad(void) {
  for (int i = 0; i < 32; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;
    unsigned long keybits[(KEY_MAX + 1) / (8 * sizeof(long))];
    unsigned long absbits[(ABS_MAX + 1) / (8 * sizeof(long))];
    memset(keybits, 0, sizeof(keybits));
    memset(absbits, 0, sizeof(absbits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
    int gp_btn = test_bit(keybits, BTN_SOUTH) || test_bit(keybits, BTN_A);
    int joy_btn = test_bit(keybits, BTN_TRIGGER) || test_bit(keybits, BTN_THUMB);
    int has_abs = test_bit(absbits, ABS_X);
    if (gp_btn || (joy_btn && has_abs)) {
      char nm[256] = {0};
      ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
      debugPrintf("input: gamepad on %s ('%s')\n", path, nm);
      return fd;
    }
    close(fd);
  }
  return -1;
}

static void *input_thread(void *arg) {
  (void)arg;
  // Wait for SDL_main to init the joystick subsystem, then register.
  usleep(2500000);
  if (p_addJoystick) {
    void *jname = jni_shim_make_jstring("Summertime Controller");
    void *jdesc = jni_shim_make_jstring("03000000810000000100000000000000");
    // is_accel=0, button_mask=0xFFFF (->~16 buttons, need >=7),
    // naxes=6 (need >=4), nhats=1, nballs=0
    p_addJoystick(g_env, g_cls, DEV_ID, jname, jdesc, 0, 0xFFFF, 6, 1, 0);
    debugPrintf("input: nativeAddJoystick(dev=%d) done\n", DEV_ID);
  }

  // Diagnostic: query the engine's own SDL to see if our joystick is a
  // recognized GameController.
  if (getenv("SUMMERTIME_VERBOSE")) {
    int (*num_joy)(void) = (void *)so_find_addr_safe("SDL_NumJoysticks");
    const char *(*joy_name)(int) =
        (void *)so_find_addr_safe("SDL_JoystickNameForIndex");
    void *(*joy_open)(int) = (void *)so_find_addr_safe("SDL_JoystickOpen");
    int (*joy_nbtn)(void *) =
        (void *)so_find_addr_safe("SDL_JoystickNumButtons");
    int (*joy_naxes)(void *) = (void *)so_find_addr_safe("SDL_JoystickNumAxes");
    int (*joy_nhats)(void *) = (void *)so_find_addr_safe("SDL_JoystickNumHats");
    void (*joy_close)(void *) = (void *)so_find_addr_safe("SDL_JoystickClose");
    usleep(300000);
    if (num_joy) {
      int n = num_joy();
      debugPrintf("input: SDL_NumJoysticks=%d\n", n);
      for (int i = 0; i < n; i++) {
        void *j = joy_open ? joy_open(i) : NULL;
        debugPrintf("input:  joy[%d] '%s' nbtn=%d naxes=%d nhats=%d\n", i,
                    joy_name ? joy_name(i) : "?", (j && joy_nbtn) ? joy_nbtn(j) : -1,
                    (j && joy_naxes) ? joy_naxes(j) : -1,
                    (j && joy_nhats) ? joy_nhats(j) : -1);
        if (j && joy_close)
          joy_close(j);
      }
    }
  }

  start_cursor_thread();

  // Autotest: drive the UI without a physical press (for headless bring-up).
  const char *autonav = getenv("SUMMERTIME_AUTONAV");
  if (autonav)
    debugPrintf("input: SUMMERTIME_AUTONAV=%s\n", autonav);
  if (autonav) {
    // Drive the menu: confirm EULA, then walk down + A. Sequence is a string
    // of tokens: a=A b=B s=START u/d/l/r=dpad(hat) t=touchOK w=wait.
    for (const char *p = autonav; *p; p++) {
      usleep(1200000);
      debugPrintf("input: NAV '%c'\n", *p);
      switch (*p) {
      case 'a':
        confirm_current();
        break;
      case 'b':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_B);
        usleep(90000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_B);
        break;
      case 's':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_START);
        usleep(90000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_START);
        break;
      case 'u':
      case 'd':
      case 'l':
      case 'r': {
        int x = (*p == 'l') ? -1 : (*p == 'r') ? 1 : 0;
        int y = (*p == 'u') ? -1 : (*p == 'd') ? 1 : 0;
        // CLEAN hat only (combining hat+stick+button confuses the menu).
        if (p_onHat) {
          p_onHat(g_env, g_cls, DEV_ID, 0, x, y);
          usleep(220000);
          p_onHat(g_env, g_cls, DEV_ID, 0, 0, 0);
        }
        mouse_move_by(0.08f * (float)x, 0.11f * (float)y);
        if (x < 0)
          key_press(AKEYCODE_DPAD_LEFT);
        else if (x > 0)
          key_press(AKEYCODE_DPAD_RIGHT);
        if (y < 0)
          key_press(AKEYCODE_DPAD_UP);
        else if (y > 0)
          key_press(AKEYCODE_DPAD_DOWN);
        break;
      }
      case 't':
        touch_tap(0.5f, 0.96f);
        break;
      case 'S': // scroll a text box down (drag up) several times
        if (p_onTouch) {
          for (int s = 0; s < 5; s++) {
            p_onTouch(g_env, g_cls, 0, 0, 0, 0.5f, 0.75f, 1.0f);
            for (float y = 0.75f; y > 0.25f; y -= 0.05f) {
              usleep(15000);
              p_onTouch(g_env, g_cls, 0, 0, 2, 0.5f, y, 1.0f);
            }
            p_onTouch(g_env, g_cls, 0, 0, 1, 0.5f, 0.25f, 1.0f);
            usleep(120000);
          }
        }
        break;
      case 'T': // tap exact center
        pointer_tap(0.5f, 0.5f);
        break;
      // Main-menu rows (touch directly): Start/Load/Settings/Cookie/Credits/Changelog.
      case '1':
        pointer_tap(0.84f, 0.16f);
        break;
      case '2':
        pointer_tap(0.84f, 0.30f);
        break;
      case '3':
        pointer_tap(0.84f, 0.43f);
        break;
      case '4':
        pointer_tap(0.84f, 0.56f);
        break;
      case '5':
        pointer_tap(0.84f, 0.70f);
        break;
      case '6':
        pointer_tap(0.84f, 0.83f);
        break;
      case 'O': // title Start button
        pointer_tap(0.84f, 0.16f);
        break;
      // isolated directional probes (down): hat / stick-axis1 / dpad-button
      case 'h':
        if (p_onHat) {
          p_onHat(g_env, g_cls, DEV_ID, 0, 0, 1);
          usleep(250000);
          p_onHat(g_env, g_cls, DEV_ID, 0, 0, 0);
        }
        break;
      case 'j':
        if (p_onJoy) {
          p_onJoy(g_env, g_cls, DEV_ID, 1, 1.0f);
          usleep(250000);
          p_onJoy(g_env, g_cls, DEV_ID, 1, 0.0f);
        }
        break;
      case 'k':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_DPAD_DOWN);
        usleep(250000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_DPAD_DOWN);
        break;
      case 'w':
      default:
        break;
      }
    }
    debugPrintf("input: NAV sequence done\n");
    return NULL;
  }

  int fd = open_gamepad();
  if (fd < 0) {
    debugPrintf("input: no gamepad found\n");
    return NULL;
  }

  // Query analog axis ranges to normalize to -1..1.
  struct stick_axis {
    int code, axis;
    int min, max;
  } sticks[] = {{ABS_X, 0, 0, 255}, {ABS_Y, 1, 0, 255},
                {ABS_Z, 2, 0, 255}, {ABS_RZ, 3, 0, 255}};
  for (unsigned s = 0; s < sizeof(sticks) / sizeof(sticks[0]); s++) {
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(sticks[s].code), &ai) == 0 && ai.maximum > ai.minimum) {
      sticks[s].min = ai.minimum;
      sticks[s].max = ai.maximum;
    }
  }

  int hatx = 0, haty = 0;
  int sel_down = 0, start_down = 0; // Select+Start = quit (like Bully/SOR4)
  struct input_event ev;
  while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
    if (ev.type == EV_KEY) {
      /* D-pad por TECLAS (BTN_DPAD_*, ex.: GO-Super/odroidgo-joypad): move o
       * cursor exatamente como o HAT dos pads ABS. */
      if (ev.code >= BTN_DPAD_UP && ev.code <= BTN_DPAD_RIGHT) {
        switch (ev.code) {
        case BTN_DPAD_UP:
          g_cursor_hat_y = ev.value ? -1 : (g_cursor_hat_y < 0 ? 0 : g_cursor_hat_y);
          break;
        case BTN_DPAD_DOWN:
          g_cursor_hat_y = ev.value ? 1 : (g_cursor_hat_y > 0 ? 0 : g_cursor_hat_y);
          break;
        case BTN_DPAD_LEFT:
          g_cursor_hat_x = ev.value ? -1 : (g_cursor_hat_x < 0 ? 0 : g_cursor_hat_x);
          break;
        case BTN_DPAD_RIGHT:
          g_cursor_hat_x = ev.value ? 1 : (g_cursor_hat_x > 0 ? 0 : g_cursor_hat_x);
          break;
        }
        continue;
      }
      int pos = evdev_to_pos(ev.code);
      // Debug: SUMMERTIME_VERBOSE=1 logs each press (raw code -> position -> keycode)
      if (ev.value && getenv("SUMMERTIME_VERBOSE"))
        debugPrintf("BTNLOG: evdev=0x%x pos=%s kc=%d\n", ev.code,
                    (pos >= 0 && pos < POS_COUNT) ? pos_names[pos] : "???",
                    evkey_to_android(ev.code));
      if (pos == POS_SELECT)
        sel_down = ev.value ? 1 : 0;
      if (pos == POS_START)
        start_down = ev.value ? 1 : 0;
      if (sel_down && start_down) {
        debugPrintf("input: Select+Start -> saindo do jogo\n");
        summertime_fb1_blank(); /* esconde a seta de HW antes de sair */
        _exit(0);
      }
      int kc = evkey_to_android(ev.code);
      if (kc >= 0) {
        if (ev.value) {
          if (g_tapmap[pos][0] >= 0.0f)
            /* atalho nativo: teleporta a seta ao alvo do HUD e clica */
            tap_game_norm(g_tapmap[pos][0], g_tapmap[pos][1]);
          else if (pos == POS_A)
            confirm_current();
          else if (pos == POS_B)
            back_current();
          else if (pos == POS_START)
            menu_current();
          else if (pos == POS_X)
            /* ENTER de teclado: submete telas de input do Ren'Py (nome do
             * personagem etc.), que ignoram clique de mouse no botao OK. */
            key_press(AKEYCODE_ENTER);
          else
            p_onPadDown(g_env, g_cls, DEV_ID, kc);
        } else {
          if (pos != POS_A && pos != POS_B && pos != POS_START &&
              pos != POS_X && g_tapmap[pos][0] < 0.0f)
            p_onPadUp(g_env, g_cls, DEV_ID, kc);
        }
      }
    } else if (ev.type == EV_ABS) {
      switch (ev.code) {
      case ABS_HAT0X:
        hatx = ev.value > 0 ? 1 : (ev.value < 0 ? -1 : 0);
        g_cursor_hat_x = hatx;
        break;
      case ABS_HAT0Y:
        haty = ev.value > 0 ? 1 : (ev.value < 0 ? -1 : 0);
        g_cursor_hat_y = haty;
        break;
      case ABS_X:
      case ABS_Y:
      case ABS_Z:
      case ABS_RZ:
        for (unsigned s = 0; s < sizeof(sticks) / sizeof(sticks[0]); s++) {
          if (sticks[s].code == ev.code) {
            float norm = 2.0f * (ev.value - sticks[s].min) /
                             (float)(sticks[s].max - sticks[s].min) -
                         1.0f;
            if (norm > -0.12f && norm < 0.12f)
              norm = 0.0f;
            if (ev.code == ABS_X)
              g_cursor_axis_x = norm;
            else if (ev.code == ABS_Y)
              g_cursor_axis_y = norm;
            break;
          }
        }
        break;
      }
    }
  }
  debugPrintf("input: evdev read loop ended\n");
  return NULL;
}

void summertime_input_start(void *env, void *cls) {
  g_env = env;
  g_cls = cls;
  /* fb1 ANTES do SDL_main/Python: o setenv(SUMMERTIME_FB1_CURSOR) precisa
   * existir quando o os.environ do Python for criado. */
  fb1_init();
  load_gptk();
  p_onPadDown = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativePadDown");
  p_onPadUp = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativePadUp");
  p_onJoy = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativeJoy");
  p_onHat = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativeHat");
  p_addJoystick = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_nativeAddJoystick");
  p_onKeyDown =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeKeyDown");
  p_onKeyUp =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeKeyUp");
  p_onTouch =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeTouch");
  p_onMouse =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeMouse");

  if (!p_onPadDown || !p_addJoystick) {
    debugPrintf("input: missing SDL controller entry points\n");
    return;
  }
  pthread_t t;
  pthread_create(&t, NULL, input_thread, NULL);
  pthread_detach(t);
}
