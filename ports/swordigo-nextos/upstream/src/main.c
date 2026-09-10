/*
 * main.c — Swordigo 1.4.12 so-loader for NextOS (Mali-450, aarch64)
 *
 * Thin Android shell replica: GLES1.1 + OpenAL + AAsset + MusicPlayer JNI,
 * pad→touch / FWKeyboard. Blueprint: NaGaa95 swordigo_nx (same APK build).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <SDL2/SDL.h>

#include "crash_diag.h"
#include "contract.h"
#include "error.h"
#include "imports.h"
#include "jni_fake.h"
#include "gl_latebind.h"
#include "music_player.h"
#include "so_util.h"
#include "util.h"
#include "glfix.h"
#include "nxgl_frame_proof_adapter.h"

#define MEMORY_MB 384
#define SO_NAME "libswordigo.so"
#define REFERENCE_VIEW_W 1280.0f
#define REFERENCE_VIEW_H 720.0f

/* Bionic reads the stack canary from TLS_SLOT_STACK_GUARD (tpidr_el0 + 0x28),
 * a slot glibc does not reserve, so on some firmwares a freshly created thread
 * finds a different value there than the one the function prologue stored and
 * the game aborts with "stack smashing detected".  A used, 16-byte aligned,
 * never-written thread-local pad anchors the first TLS block right after the
 * 16-byte TCB, which keeps the whole Bionic guard slot stable on every thread.
 * Same audited fix as the proven Bully and Prizefighters 2 runtimes. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

int screen_width = 1280;
int screen_height = 720;

static char data_path[512];
static SDL_Window *g_win;
static SDL_GLContext g_ctx;

/* Release the display before glfix re-execs us (KMSDRM master, VT state). */
static void glfix_video_teardown(void) {
  if (g_ctx) {
    SDL_GL_DeleteContext(g_ctx);
    g_ctx = NULL;
  }
  if (g_win) {
    SDL_DestroyWindow(g_win);
    g_win = NULL;
  }
  SDL_Quit();
}
static volatile int g_running = 1;
static double g_time;
static int debug_control_enabled;
static int g_finish_before_swap;
static int g_fullscreen_desktop;
static int g_opaque_backbuffer;

typedef void (*GlBindFramebufferOesFn)(GLenum target, GLuint framebuffer);
static GlBindFramebufferOesFn g_bind_framebuffer_oes;
static const char *g_bind_framebuffer_source = "unavailable";

static int socket_path_exists(const char *path) {
  struct stat st;
  return path && path[0] == '/' && lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static void set_default_environment(const char *name, const char *value) {
  const char *current = getenv(name);
  if (value && *value) {
    if (!current)
      (void)setenv(name, value, 0);
    else if (!*current)
      (void)setenv(name, value, 1);
  }
}

/* Everything the old second-stage run.sh contributed to the game process now
 * belongs to the adapter.  nxbootstrap remains generic and never selects an
 * SDL/OpenAL backend; this code only points OpenAL Soft at the shipped policy
 * and exposes a Pulse socket that the current firmware actually owns. */
static void configure_runtime_environment(void) {
  char path[sizeof(data_path) + 64];
  char pulse_server[sizeof(data_path) + 64];
  const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
  const char *pulse_candidates[4] = {
      "/run/pulse/native", "/var/run/pulse/native", NULL, NULL};

  if (snprintf(path, sizeof(path), "%s/alsoft.conf", data_path) > 0) {
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISREG(st.st_mode))
      set_default_environment("ALSOFT_CONF", path);
  }

  if (xdg_runtime && *xdg_runtime &&
      snprintf(path, sizeof(path), "%s/pulse/native", xdg_runtime) > 0)
    pulse_candidates[2] = path;
  if (!getenv("PULSE_SERVER") || !*getenv("PULSE_SERVER")) {
    for (size_t i = 0; pulse_candidates[i]; ++i) {
      if (!socket_path_exists(pulse_candidates[i]))
        continue;
      if (snprintf(pulse_server, sizeof(pulse_server), "unix:%s",
                   pulse_candidates[i]) > 0)
        set_default_environment("PULSE_SERVER", pulse_server);
      break;
    }
  }

  set_default_environment("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
  set_default_environment("SDL_VIDEO_FULLSCREEN_DESKTOP", "1");

  FILE *meminfo = fopen("/proc/meminfo", "r");
  unsigned long memory_kib = 0;
  if (meminfo) {
    char line[160];
    while (fgets(line, sizeof(line), meminfo)) {
      if (sscanf(line, "MemTotal: %lu kB", &memory_kib) == 1)
        break;
    }
    fclose(meminfo);
  }
  if (memory_kib > 0 && memory_kib < 1250000UL) {
    set_default_environment("MALLOC_ARENA_MAX", "2");
    set_default_environment("MALLOC_TRIM_THRESHOLD_", "131072");
    set_default_environment("MALLOC_MMAP_THRESHOLD_", "65536");
  }
}

/* ---- overlay hide (pad used) — same symbols/offsets as swordigo_nx 1.4.12 ---- */
#define GAME_OVERLAY_INIT_SYMBOL \
  "_ZN5Caver15GameOverlayView17InitWithGameStateERKN5boost10shared_ptrINS_9GameStateEEE"
#define GAME_OVERLAY_DESTROY_SYMBOL "_ZN5Caver15GameOverlayViewD1Ev"
#define GAME_OVERLAY_VISIBILITY_SYMBOL \
  "_ZN5Caver15GameOverlayView26SetControlButtonsInvisibleEb"
#define OVERLAY_INIT_TRAMPOLINE_OFFSET 0x6af8d8
#define OVERLAY_DESTROY_TRAMPOLINE_OFFSET 0x6af918
#define MAX_GAME_OVERLAYS 8

typedef void (*GameOverlayInitFn)(void *view, const void *state);
typedef void (*GameOverlayDestroyFn)(void *view);
typedef void (*GameOverlayVisibilityFn)(void *view, int hidden);

static GameOverlayInitFn game_overlay_init_original;
static GameOverlayDestroyFn game_overlay_destroy_original;
static GameOverlayVisibilityFn set_game_overlay_invisible;
static void *game_overlays[MAX_GAME_OVERLAYS];
static int touch_controls_hidden;

static uintptr_t install_game_hook(const char *symbol, uintptr_t replacement,
                                   uintptr_t trampoline_offset) {
  uintptr_t target = so_find_addr(symbol);
  if (!target) {
    debugPrintf("overlay hook: missing %s\n", symbol);
    return 0;
  }
  uint32_t *trampoline = (uint32_t *)((uintptr_t)text_base + trampoline_offset);
  memcpy(trampoline, (const void *)target, 16);
  trampoline[4] = 0x58000051u; /* ldr x17, #8 */
  trampoline[5] = 0xd61f0220u; /* br x17 */
  *(uint64_t *)(trampoline + 6) = target + 16;
  hook_arm64(target, replacement);
  return (uintptr_t)trampoline;
}

static void set_touch_controls_hidden(int hidden) {
  touch_controls_hidden = hidden;
  if (!set_game_overlay_invisible)
    return;
  for (int i = 0; i < MAX_GAME_OVERLAYS; i++)
    if (game_overlays[i])
      set_game_overlay_invisible(game_overlays[i], hidden);
}

static void game_overlay_init_hook(void *view, const void *state) {
  game_overlay_init_original(view, state);
  for (int i = 0; i < MAX_GAME_OVERLAYS; i++) {
    if (game_overlays[i] == view) {
      set_game_overlay_invisible(view, touch_controls_hidden);
      return;
    }
    if (!game_overlays[i]) {
      game_overlays[i] = view;
      set_game_overlay_invisible(view, touch_controls_hidden);
      return;
    }
  }
}

static void game_overlay_destroy_hook(void *view) {
  for (int i = 0; i < MAX_GAME_OVERLAYS; i++)
    if (game_overlays[i] == view)
      game_overlays[i] = NULL;
  game_overlay_destroy_original(view);
}

static void install_game_overlay_hooks(void) {
  set_game_overlay_invisible =
      (GameOverlayVisibilityFn)so_find_addr_rx(GAME_OVERLAY_VISIBILITY_SYMBOL);
  so_make_text_writable();
  game_overlay_init_original = (GameOverlayInitFn)install_game_hook(
      GAME_OVERLAY_INIT_SYMBOL, (uintptr_t)game_overlay_init_hook,
      OVERLAY_INIT_TRAMPOLINE_OFFSET);
  game_overlay_destroy_original = (GameOverlayDestroyFn)install_game_hook(
      GAME_OVERLAY_DESTROY_SYMBOL, (uintptr_t)game_overlay_destroy_hook,
      OVERLAY_DESTROY_TRAMPOLINE_OFFSET);
  so_make_text_executable();
  so_flush_caches();
}

/* ---- Native entry points ---- */
static void (*setFilesDir)(void *, void *, void *);
static void (*setCacheDir)(void *, void *, void *);
static void (*setAssetManager)(void *, void *, void *);
static void (*setupNativeInterface)(void *, void *);
static void (*setupApplication)(void *, void *);
static void (*setApplicationViewSize)(void *, void *, int, int, int, int, int);
static void (*handleApplicationLaunch)(void *, void *);
static void (*applicationDidBecomeActive)(void *, void *);
static void (*updateApplication)(void *, void *, float);
static void (*drawApplication)(void *, void *);
static void (*handleTouchEvent)(void *, void *, int, int, double, float, float,
                                float, float, int);
static void (*initMusicPlayer)(void *, void *);
static void (*googleSignInCompleted)(void *, void *, uint8_t);
static void (*handleMenuButtonPress)(void *, void *);
static void (*handleBackButtonPress)(void *, void *);
static void (*textInputTextDidChange)(void *, void *, void *);
static void (*textInputDidFinish)(void *, void *);
static void (*handleApplicationQuit)(void *, void *);

static void *(*sharedKeyboard)(void);
static void (*sendKeyDownEvent)(void *, unsigned, unsigned, double);
static void (*sendKeyUpEvent)(void *, unsigned, unsigned, double);
static void *(*sharedAudioSystem)(void);
static void (*shutdownAudioSystem)(void *);

static void resolve_entry_points(void) {
#define E(var, sym)                                                            \
  var = (void *)so_find_addr_rx("Java_com_touchfoo_swordigo_" sym);            \
  if (!var)                                                                    \
    fatal_error("missing JNI %s", sym)
  E(setFilesDir, "Native_setFilesDir");
  E(setCacheDir, "Native_setCacheDir");
  E(setAssetManager, "Native_setAssetManager");
  E(setupNativeInterface, "Native_setupNativeInterface");
  E(setupApplication, "Native_setupApplication");
  E(setApplicationViewSize, "Native_setApplicationViewSize");
  E(handleApplicationLaunch, "Native_handleApplicationLaunch");
  E(applicationDidBecomeActive, "Native_applicationDidBecomeActive");
  E(updateApplication, "Native_updateApplication");
  E(drawApplication, "Native_drawApplication");
  E(handleTouchEvent, "Native_handleTouchEvent");
  E(initMusicPlayer, "MusicPlayer_initMusicPlayer");
  E(googleSignInCompleted, "Native_googleSignInCompleted");
  E(textInputTextDidChange, "Native_textInputTextDidChange");
  E(textInputDidFinish, "Native_textInputDidFinish");
#undef E
  handleMenuButtonPress =
      (void *)so_find_addr_safe("Java_com_touchfoo_swordigo_Native_handleMenuButtonPress");
  handleBackButtonPress =
      (void *)so_find_addr_safe("Java_com_touchfoo_swordigo_Native_handleBackButtonPress");
  handleApplicationQuit =
      (void *)so_find_addr_safe("Java_com_touchfoo_swordigo_Native_handleApplicationQuit");

  sharedKeyboard =
      (void *)so_find_addr_rx("_ZN5Caver10FWKeyboard14sharedKeyboardEv");
  sendKeyDownEvent =
      (void *)so_find_addr_rx("_ZN5Caver10FWKeyboard16SendKeyDownEventEjjd");
  sendKeyUpEvent =
      (void *)so_find_addr_rx("_ZN5Caver10FWKeyboard14SendKeyUpEventEjjd");
  sharedAudioSystem =
      (void *)so_find_addr_safe("_ZN5Caver11AudioSystem12sharedSystemEv");
  shutdownAudioSystem =
      (void *)so_find_addr_safe("_ZN5Caver11AudioSystem8ShutdownEv");
}

/* ---- input ---- */
enum { KEY_LEFT = 0x25, KEY_UP = 0x26, KEY_RIGHT = 0x27 };
#define A_R 1
#define A_T 2

typedef struct {
  int sdl_btn;
  int id;
  int flags;
  float ax, ay;
} Ctrl;

/* Hitboxes on 1280x720 overlay (swordigo_nx). SDL = Xbox layout:
 * X=attack, B=magic, Y=item, A=jump (keyboard), LB=magic-equip, Start=menu, RB=back */
static const Ctrl button_map[] = {
    {SDL_CONTROLLER_BUTTON_X, 6, A_R, 335.0f, 105.0f},              /* attack */
    {SDL_CONTROLLER_BUTTON_B, 7, A_R, 190.0f, 240.0f},              /* magic */
    {SDL_CONTROLLER_BUTTON_Y, 8, 0, 640.0f, 75.0f},                 /* item */
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 9, A_R | A_T, 150.0f, 65.0f}, /* magic equip */
};

/* Field instrument, off unless SWORDIGO_DEBUG_TAP="ax,ay[,flags]" is set:
 * presses one overlay hitbox every few seconds so an unmapped on-screen button
 * can be identified on the device.  Never used by the shipped launcher. */
static float g_debug_tap_ax = -1.0f, g_debug_tap_ay = -1.0f;
static int g_debug_tap_flags = A_R;

static SDL_GameController *g_pad;
static uint32_t pad_prev;
static int left_prev, right_prev, jump_prev;

enum { TOUCH_BEGAN = 1, TOUCH_ENDED = 2, TOUCH_CANCELLED = 3, TOUCH_MOVED = 4 };
static void emit_touch(int phase, int id, float x, float y);

/* Right-stick cursor (LEGO pattern): view coords, Y bottom-origin like touches. */
static float g_cursor_x = -1.0f, g_cursor_y = -1.0f;
static float g_cursor_vx, g_cursor_vy;
static float g_cursor_show;
static int g_cursor_click_prev;
static int g_cursor_r2_prev;
static int g_cursor_touch_hold;
static float g_cursor_tx, g_cursor_ty;
static int g_raw_click_layout_logged;
static int g_pad_left_stick_reachable;
static int g_pad_right_stick_reachable;
static int g_pad_left_trigger_reachable;
static int g_pad_right_trigger_reachable;
static int g_pad_r3_reachable;
static int g_cursor_left_modifier_enabled;
static int g_cursor_modifier_held;
static int g_cursor_modifier_prev;

static int pad_binding_reachable(SDL_GameControllerButtonBind binding) {
  SDL_Joystick *joy;
  if (!g_pad || !(joy = SDL_GameControllerGetJoystick(g_pad)))
    return 0;
  switch (binding.bindType) {
  case SDL_CONTROLLER_BINDTYPE_BUTTON:
    return binding.value.button >= 0 &&
           binding.value.button < SDL_JoystickNumButtons(joy);
  case SDL_CONTROLLER_BINDTYPE_AXIS:
    return binding.value.axis >= 0 &&
           binding.value.axis < SDL_JoystickNumAxes(joy);
  case SDL_CONTROLLER_BINDTYPE_HAT:
    return binding.value.hat.hat < SDL_JoystickNumHats(joy) &&
           binding.value.hat.hat_mask != 0;
  case SDL_CONTROLLER_BINDTYPE_NONE:
  default:
    return 0;
  }
}

static int pad_axis_reachable(SDL_GameControllerAxis axis) {
  return pad_binding_reachable(SDL_GameControllerGetBindForAxis(g_pad, axis));
}

static int pad_button_reachable(SDL_GameControllerButton button) {
  return pad_binding_reachable(
      SDL_GameControllerGetBindForButton(g_pad, button));
}

static void pad_refresh_topology(void) {
  SDL_Joystick *joy = g_pad ? SDL_GameControllerGetJoystick(g_pad) : NULL;
  if (!joy) {
    g_pad_left_stick_reachable = 0;
    g_pad_right_stick_reachable = 0;
    g_pad_left_trigger_reachable = 0;
    g_pad_right_trigger_reachable = 0;
    g_pad_r3_reachable = 0;
    g_cursor_left_modifier_enabled = 0;
    g_cursor_modifier_held = 0;
    g_cursor_modifier_prev = 0;
    return;
  }

  g_pad_left_stick_reachable =
      pad_axis_reachable(SDL_CONTROLLER_AXIS_LEFTX) &&
      pad_axis_reachable(SDL_CONTROLLER_AXIS_LEFTY);
  g_pad_right_stick_reachable =
      pad_axis_reachable(SDL_CONTROLLER_AXIS_RIGHTX) &&
      pad_axis_reachable(SDL_CONTROLLER_AXIS_RIGHTY);
  g_pad_left_trigger_reachable =
      pad_axis_reachable(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  g_pad_right_trigger_reachable =
      pad_axis_reachable(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  g_pad_r3_reachable =
      pad_button_reachable(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  g_cursor_left_modifier_enabled = !g_pad_right_stick_reachable &&
                                   g_pad_left_stick_reachable &&
                                   g_pad_left_trigger_reachable;
  g_cursor_modifier_held = 0;
  g_cursor_modifier_prev = 0;

  const char *pad_name = SDL_GameControllerName(g_pad);
  debugPrintf("pad: opened %s raw=%d axes/%d buttons/%d hats "
              "left=%s right=%s L2=%s R2=%s R3=%s\n",
              pad_name ? pad_name : "?", SDL_JoystickNumAxes(joy),
              SDL_JoystickNumButtons(joy), SDL_JoystickNumHats(joy),
              g_pad_left_stick_reachable ? "yes" : "no",
              g_pad_right_stick_reachable ? "yes" : "no",
              g_pad_left_trigger_reachable ? "yes" : "no",
              g_pad_right_trigger_reachable ? "yes" : "no",
              g_pad_r3_reachable ? "yes" : "no");
  if (g_cursor_left_modifier_enabled)
    debugPrintf("cursor: no real right stick; hold L2 + left stick to move "
                "the cursor, R2/R3 clicks\n");
}

static int pad_open_device(int device_index) {
  SDL_GameController *candidate;
  if (g_pad || device_index < 0 || !SDL_IsGameController(device_index))
    return g_pad != NULL;
  candidate = SDL_GameControllerOpen(device_index);
  if (!candidate) {
    debugPrintf("pad: open failed index=%d: %s\n", device_index,
                SDL_GetError());
    return 0;
  }
  g_pad = candidate;
  pad_refresh_topology();
  return 1;
}

static int cursor_modifier_down(void) {
  int down;
  int threshold;
  if (!g_pad || !g_cursor_left_modifier_enabled)
    return 0;
  threshold = g_cursor_modifier_prev ? 10000 : 16000;
  down = SDL_GameControllerGetAxis(
             g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > threshold;
  g_cursor_modifier_prev = down;
  return down;
}

/* A common 12-button USB pad reports both triggers and stick clicks as plain
 * joystick buttons.  Older SDL controller databases expose its sticks but
 * omit those four digital bindings.  Recognize the capability/layout instead
 * of a firmware or screen size: b7=R2 and b11=R3 on this standard mapping. */
static int generic_usb_digital_clicks(int *r2, int *r3) {
  if (!g_pad)
    return 0;
  SDL_Joystick *joy = SDL_GameControllerGetJoystick(g_pad);
  if (!joy || SDL_JoystickNumButtons(joy) != 12 ||
      SDL_JoystickNumAxes(joy) != 4)
    return 0;
  const char *name = SDL_JoystickName(joy);
  if (!name || (!strstr(name, "USB Gamepad") &&
                !strstr(name, "USB gamepad") &&
                !strstr(name, "Usb Gamepad")))
    return 0;
  *r2 = SDL_JoystickGetButton(joy, 7) != 0;
  *r3 = SDL_JoystickGetButton(joy, 11) != 0;
  if (!g_raw_click_layout_logged) {
    debugPrintf("pad: digital R2/R3 fallback active for %s\n", name);
    g_raw_click_layout_logged = 1;
  }
  return 1;
}

static void cursor_ensure(void) {
  if (g_cursor_x < 0.0f) {
    g_cursor_x = (float)screen_width * 0.5f;
    g_cursor_y = (float)screen_height * 0.5f;
  }
}

static void cursor_draw_overlay(void) {
  if (g_cursor_show <= 0 || g_cursor_x < 0.0f)
    return;
  cursor_ensure();
  const float cx = g_cursor_x, cy = g_cursor_y;
  const float W = (float)screen_width, H = (float)screen_height;

  GLboolean was_tex = glIsEnabled(GL_TEXTURE_2D);
  GLboolean was_blend = glIsEnabled(GL_BLEND);
  GLboolean was_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean was_cull = glIsEnabled(GL_CULL_FACE);
  GLboolean was_scis = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean was_light = glIsEnabled(GL_LIGHTING);
  GLboolean was_fog = glIsEnabled(GL_FOG);
  GLboolean was_alpha = glIsEnabled(GL_ALPHA_TEST);
  GLboolean was_colmat = glIsEnabled(GL_COLOR_MATERIAL);
  GLint prev_vp[4];
  GLint prev_tex = 0;
  GLboolean depth_mask = GL_TRUE;
  glGetIntegerv(GL_VIEWPORT, prev_vp);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);

  GLboolean was_va = glIsEnabled(GL_VERTEX_ARRAY);
  GLboolean was_ca = glIsEnabled(GL_COLOR_ARRAY);
  GLboolean was_na = glIsEnabled(GL_NORMAL_ARRAY);
  GLboolean was_ta = glIsEnabled(GL_TEXTURE_COORD_ARRAY);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, W, 0.0f, H, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glViewport(0, 0, screen_width, screen_height);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_LIGHTING);
  glDisable(GL_FOG);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_COLOR_MATERIAL);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  /* Tip at cursor; shaft points down-right (ortho Y-up). Outline then white. */
  float base[6][2] = {
      {cx, cy},
      {cx, cy - 26.0f},
      {cx + 18.0f, cy - 18.0f},
      {cx + 6.0f, cy - 17.0f},
      {cx + 11.0f, cy - 28.0f},
      {cx + 15.0f, cy - 26.0f},
  };
  glEnableClientState(GL_VERTEX_ARRAY);
  for (int pass = 0; pass < 2; pass++) {
    float verts[6][2];
    for (int i = 0; i < 6; i++) {
      verts[i][0] = base[i][0];
      verts[i][1] = base[i][1];
    }
    if (pass == 0) {
      for (int i = 1; i < 6; i++) {
        verts[i][0] += (base[i][0] - base[0][0]) * 0.22f;
        verts[i][1] += (base[i][1] - base[0][1]) * 0.22f;
      }
      glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
  }
  if (!was_va)
    glDisableClientState(GL_VERTEX_ARRAY);
  if (was_ca)
    glEnableClientState(GL_COLOR_ARRAY);
  if (was_na)
    glEnableClientState(GL_NORMAL_ARRAY);
  if (was_ta)
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glDepthMask(depth_mask);
  if (was_tex)
    glEnable(GL_TEXTURE_2D);
  else
    glDisable(GL_TEXTURE_2D);
  if (was_blend)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  if (was_depth)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  if (was_cull)
    glEnable(GL_CULL_FACE);
  else
    glDisable(GL_CULL_FACE);
  if (was_scis)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  if (was_light)
    glEnable(GL_LIGHTING);
  else
    glDisable(GL_LIGHTING);
  if (was_fog)
    glEnable(GL_FOG);
  else
    glDisable(GL_FOG);
  if (was_alpha)
    glEnable(GL_ALPHA_TEST);
  else
    glDisable(GL_ALPHA_TEST);
  if (was_colmat)
    glEnable(GL_COLOR_MATERIAL);
  else
    glDisable(GL_COLOR_MATERIAL);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static void update_cursor(float dt) {
  cursor_ensure();
  float target_vx = 0.0f;
  float target_vy = 0.0f;
  int stick_active = 0;

  if (g_pad) {
    const float scale = 1.0f / 32767.0f;
    const float deadzone = 0.27f;
    float rx = 0.0f;
    float ry = 0.0f;
    if (g_pad_right_stick_reachable) {
      rx = (float)SDL_GameControllerGetAxis(
               g_pad, SDL_CONTROLLER_AXIS_RIGHTX) * scale;
      ry = (float)SDL_GameControllerGetAxis(
               g_pad, SDL_CONTROLLER_AXIS_RIGHTY) * scale;
    } else if (g_cursor_left_modifier_enabled && g_cursor_modifier_held) {
      rx = (float)SDL_GameControllerGetAxis(
               g_pad, SDL_CONTROLLER_AXIS_LEFTX) * scale;
      ry = (float)SDL_GameControllerGetAxis(
               g_pad, SDL_CONTROLLER_AXIS_LEFTY) * scale;
      g_cursor_show = 2.0f;
    }
    float magnitude = sqrtf(rx * rx + ry * ry);
    if (magnitude > deadzone) {
      float strength = (magnitude - deadzone) / (1.0f - deadzone);
      if (strength > 1.0f)
        strength = 1.0f;
      /* Progressive response: precise near the deadzone, fast at the edge. */
      float response = strength * (0.35f + 0.65f * strength);
      float shortest_side = screen_width < screen_height
                                ? (float)screen_width
                                : (float)screen_height;
      float speed = shortest_side * 1.2f * response;
      target_vx = rx / magnitude * speed;
      /* SDL Y+: down; view Y+: up — invert. */
      target_vy = -ry / magnitude * speed;
      stick_active = 1;
      g_cursor_show = 2.0f;
    }
  }

  /* First-order smoothing is stable across 30/60/120 Hz and naturally eases
   * to rest when the stick returns to center. */
  float smoothing = dt / (0.045f + dt);
  g_cursor_vx += (target_vx - g_cursor_vx) * smoothing;
  g_cursor_vy += (target_vy - g_cursor_vy) * smoothing;
  g_cursor_x += g_cursor_vx * dt;
  g_cursor_y += g_cursor_vy * dt;
  if (g_cursor_x < 0.0f) {
    g_cursor_x = 0.0f;
    if (g_cursor_vx < 0.0f)
      g_cursor_vx = 0.0f;
  } else if (g_cursor_x > (float)screen_width) {
    g_cursor_x = (float)screen_width;
    if (g_cursor_vx > 0.0f)
      g_cursor_vx = 0.0f;
  }
  if (g_cursor_y < 0.0f) {
    g_cursor_y = 0.0f;
    if (g_cursor_vy < 0.0f)
      g_cursor_vy = 0.0f;
  } else if (g_cursor_y > (float)screen_height) {
    g_cursor_y = (float)screen_height;
    if (g_cursor_vy > 0.0f)
      g_cursor_vy = 0.0f;
  }
  if (!stick_active && g_cursor_show > 0.0f) {
    g_cursor_show -= dt;
    if (g_cursor_show < 0.0f)
      g_cursor_show = 0.0f;
  }

  /* R3 or R2 is the finger, not a click pulse: press = touch down at the cursor,
   * keep holding while moving the right stick = drag (this is what enchanting
   * the sword with a talisman needs), release = touch up.  A quick press and
   * release still reads as a tap. */
  int r3 = g_pad &&
           SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  int r2 = g_pad && SDL_GameControllerGetAxis(
                           g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >
                       (g_cursor_r2_prev ? 10000 : 16000);
  int raw_r2 = 0, raw_r3 = 0;
  generic_usb_digital_clicks(&raw_r2, &raw_r3);
  r2 = r2 || raw_r2;
  r3 = r3 || raw_r3;
  int click = r3 || r2;
  if (click && !g_cursor_click_prev) {
    g_cursor_tx = g_cursor_x;
    g_cursor_ty = g_cursor_y;
    emit_touch(TOUCH_BEGAN, 9, g_cursor_tx, g_cursor_ty);
    g_cursor_touch_hold = 1;
    g_cursor_show = 2.0f;
    debugPrintf("cursor: %s down (%.0f,%.0f)\n", r3 ? "R3" : "R2",
                g_cursor_tx, g_cursor_ty);
  } else if (click && g_cursor_touch_hold) {
    g_cursor_tx = g_cursor_x;
    g_cursor_ty = g_cursor_y;
    emit_touch(TOUCH_MOVED, 9, g_cursor_tx, g_cursor_ty);
    g_cursor_show = 2.0f;
  } else if (!click && g_cursor_click_prev && g_cursor_touch_hold) {
    g_cursor_tx = g_cursor_x;
    g_cursor_ty = g_cursor_y;
    emit_touch(TOUCH_ENDED, 9, g_cursor_tx, g_cursor_ty);
    g_cursor_touch_hold = 0;
    debugPrintf("cursor: click up (%.0f,%.0f)\n", g_cursor_tx,
                g_cursor_ty);
  }
  g_cursor_r2_prev = r2;
  g_cursor_click_prev = click;
}

static void ctrl_point(int flags, float ax, float ay, float *x, float *y) {
  float sx = (float)screen_width / REFERENCE_VIEW_W;
  float sy = (float)screen_height / REFERENCE_VIEW_H;
  ax *= sx;
  ay *= sy;
  *x = (flags & A_R) ? (float)screen_width - ax : ax;
  *y = (flags & A_T) ? (float)screen_height - ay : ay;
}

static int touch_tap_count(int phase) {
  return phase == TOUCH_BEGAN || phase == TOUCH_ENDED ? 1 : 0;
}

static void emit_touch(int phase, int id, float x, float y) {
  handleTouchEvent(fake_env, NULL, phase, id, g_time, x, y, x, y,
                   touch_tap_count(phase));
}

static void emit_touch_drag(int phase, int id, float x, float y, float ox,
                            float oy) {
  handleTouchEvent(fake_env, NULL, phase, id, g_time, x, y, ox, oy,
                   touch_tap_count(phase));
}

static void drive_button(int held, int was_held, int id, float x, float y) {
  if (held)
    emit_touch(was_held ? TOUCH_MOVED : TOUCH_BEGAN, id, x, y);
  else if (was_held)
    emit_touch(TOUCH_ENDED, id, x, y);
}

static void drive_key(int held, int was_held, unsigned key) {
  if (held == was_held || !sharedKeyboard || !sendKeyDownEvent || !sendKeyUpEvent)
    return;
  void *kb = sharedKeyboard();
  if (!kb)
    return;
  if (held)
    sendKeyDownEvent(kb, key, 0, g_time);
  else
    sendKeyUpEvent(kb, key, 0, g_time);
}

static uint32_t pad_buttons(void) {
  uint32_t m = 0;
  if (!g_pad)
    return 0;
  for (int i = 0; i < (int)(sizeof(button_map) / sizeof(button_map[0])); i++)
    if (SDL_GameControllerGetButton(g_pad, button_map[i].sdl_btn))
      m |= (1u << i);
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START))
    m |= (1u << 16);
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
    m |= (1u << 18);
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
    m |= (1u << 19);
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A))
    m |= (1u << 20); /* jump */
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
    m |= (1u << 21); /* back */
  return m;
}

/* Drives the debug hitbox: press for ~0.4 s once every 3 s. */
static void debug_tap_step(void) {
  static int held;
  static unsigned frame;
  float x, y;

  if (g_debug_tap_ax < 0.0f)
    return;
  frame++;
  int want = (frame % 180u) < 24u;
  if (want == held)
    return;
  held = want;
  ctrl_point(g_debug_tap_flags, g_debug_tap_ax, g_debug_tap_ay, &x, &y);
  emit_touch(want ? TOUCH_BEGAN : TOUCH_ENDED, 15, x, y);
  debugPrintf("debug-tap: %s overlay(%.0f,%.0f) screen(%.0f,%.0f)\n",
              want ? "down" : "up", g_debug_tap_ax, g_debug_tap_ay, x, y);
}

static void update_pad(void) {
  debug_tap_step();
  if (!g_pad) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
      if (pad_open_device(i))
        break;
    }
  }
  if (!g_pad) {
    g_cursor_modifier_held = 0;
    return;
  }

  Sint16 lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
  Sint16 ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
  g_cursor_modifier_held = cursor_modifier_down();
  int cursor_owns_left =
      g_cursor_left_modifier_enabled && g_cursor_modifier_held;
  int stick_active = !cursor_owns_left &&
                     (lx < -10000 || lx > 10000 ||
                      ly < -10000 || ly > 10000);
  uint32_t down = pad_buttons();
  int controller_used = down || pad_prev || stick_active;

  for (int i = 0; i < (int)(sizeof(button_map) / sizeof(button_map[0])); i++) {
    float x, y;
    ctrl_point(button_map[i].flags, button_map[i].ax, button_map[i].ay, &x, &y);
    int held = (down >> i) & 1;
    int was = (pad_prev >> i) & 1;
    drive_button(held, was, button_map[i].id, x, y);
  }

  if ((down & (1u << 16)) && !(pad_prev & (1u << 16)) && handleMenuButtonPress)
    handleMenuButtonPress(fake_env, NULL);
  if ((down & (1u << 21)) && !(pad_prev & (1u << 21)) && handleBackButtonPress)
    handleBackButtonPress(fake_env, NULL);

  int left_now = ((down >> 18) & 1) ||
                 (!cursor_owns_left && lx < -10000);
  int right_now = ((down >> 19) & 1) ||
                  (!cursor_owns_left && lx > 10000);
  int jump_now = (down >> 20) & 1;
  drive_key(left_now, left_prev, KEY_LEFT);
  drive_key(right_now, right_prev, KEY_RIGHT);
  drive_key(jump_now, jump_prev, KEY_UP);
  left_prev = left_now;
  right_prev = right_now;
  jump_prev = jump_now;

  /* SELECT+START exit */
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START))
    g_running = 0;

  if (controller_used)
    set_touch_controls_hidden(1);
  pad_prev = down;
}

/* mouse/touch → view space (bottom-origin Y like the engine) */
static void handle_pointer_event(const SDL_Event *e) {
  float x = 0, y = 0;
  int phase = 0;
  int id = 0;
  if (e->type == SDL_FINGERDOWN || e->type == SDL_FINGERMOTION ||
      e->type == SDL_FINGERUP) {
    x = e->tfinger.x * (float)screen_width;
    y = (1.0f - e->tfinger.y) * (float)screen_height;
    id = (int)(e->tfinger.fingerId & 0x7fffffff);
    if (e->type == SDL_FINGERDOWN)
      phase = TOUCH_BEGAN;
    else if (e->type == SDL_FINGERUP)
      phase = TOUCH_ENDED;
    else
      phase = TOUCH_MOVED;
    set_touch_controls_hidden(0);
  } else if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP ||
             e->type == SDL_MOUSEMOTION) {
    if (e->type == SDL_MOUSEMOTION && !(e->motion.state & SDL_BUTTON_LMASK))
      return;
    x = (float)(e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x);
    float sy = (float)(e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y);
    y = (float)screen_height - sy;
    id = 0;
    if (e->type == SDL_MOUSEBUTTONDOWN)
      phase = TOUCH_BEGAN;
    else if (e->type == SDL_MOUSEBUTTONUP)
      phase = TOUCH_ENDED;
    else
      phase = TOUCH_MOVED;
    set_touch_controls_hidden(0);
  } else
    return;
  emit_touch(phase, id, x, y);
}

static void *exit_deadline(void *arg) {
  (void)arg;
  sleep(2);
  _exit(0);
  return NULL;
}

static void request_exit(void) {
  g_running = 0;
}

static GlBindFramebufferOesFn resolve_bind_framebuffer_name(
    const char *name, const char **source) {
  void *sdl_symbol = SDL_GL_GetProcAddress(name);
  if (sdl_symbol) {
    *source = "SDL_GL_GetProcAddress";
    return (GlBindFramebufferOesFn)sdl_symbol;
  }

  __eglMustCastToProperFunctionPointerType egl_symbol = eglGetProcAddress(name);
  if (egl_symbol) {
    *source = "eglGetProcAddress";
    return (GlBindFramebufferOesFn)egl_symbol;
  }

  void *global_symbol = dlsym(RTLD_DEFAULT, name);
  if (global_symbol) {
    *source = "dlsym";
    return (GlBindFramebufferOesFn)global_symbol;
  }
  return NULL;
}

static void resolve_present_functions(void) {
  g_bind_framebuffer_oes = resolve_bind_framebuffer_name(
      "glBindFramebufferOES", &g_bind_framebuffer_source);
  if (!g_bind_framebuffer_oes)
    g_bind_framebuffer_oes = resolve_bind_framebuffer_name(
        "glBindFramebuffer", &g_bind_framebuffer_source);
}

static int finish_before_swap_policy(const char *video_driver) {
  int automatic = swordigo_contract_quirk_enabled(
                      "game.swordigo.present-finish") &&
                  video_driver && strcmp(video_driver, "KMSDRM") == 0;
  const char *override = getenv("SWORDIGO_GLFINISH");
  if (!override)
    return automatic;
  if (strcmp(override, "0") == 0)
    return 0;
  if (strcmp(override, "1") == 0)
    return 1;
  debugPrintf("gl: invalid SWORDIGO_GLFINISH='%s'; using backend policy\n",
              override);
  return automatic;
}

static int gl_init(void) {
  const char *debug_tap = getenv("SWORDIGO_DEBUG_TAP");
  if (debug_tap && *debug_tap) {
    float ax = -1.0f, ay = -1.0f;
    int flags = A_R;
    if (sscanf(debug_tap, "%f,%f,%d", &ax, &ay, &flags) >= 2) {
      g_debug_tap_ax = ax;
      g_debug_tap_ay = ay;
      g_debug_tap_flags = flags;
      debugPrintf("debug-tap: armed overlay(%.0f,%.0f) flags=%d\n", ax, ay, flags);
    }
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return -1;
  }
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    screen_width = dm.w;
    screen_height = dm.h;
  }
  const char *ew = getenv("SWORDIGO_W");
  const char *eh = getenv("SWORDIGO_H");
  if (ew)
    screen_width = atoi(ew);
  if (eh)
    screen_height = atoi(eh);

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  /* No alpha in the window config.  A firmware whose scanout honours per-pixel
   * alpha (KMSDRM plane in ARGB8888 -- ArkOS/ROCKNIX/DarkOS R36S) shows a frame
   * the game left at alpha 0 as transparent, which reads as a black screen with
   * the engine perfectly alive: audio, music and input all running.  Amlogic's
   * OSD ignores alpha, which is why the same build draws fine here.  Asking for
   * a config without alpha makes the game's alpha irrelevant to the scanout;
   * present_opaque_alpha() below covers the firmwares that hand out an alpha
   * config anyway (these attributes are minimums, not exact matches). */
  /* The approved KMSDRM ports probe RGBA first.  Some vendor EGL stacks only
   * finish their first window negotiation on that request; if it is refused,
   * retry the same desktop mode with RGBX before considering a modeset. */
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  /* Exclusive fullscreen asks KMSDRM for a modeset to the size we requested; a
   * firmware that reports a desktop mode its panel does not actually drive then
   * hands back a window that never reaches the screen -- black picture with the
   * engine alive.  FULLSCREEN_DESKTOP keeps the mode the firmware already set,
   * which is what every published port on this fleet does (Horizon Chase,
   * Prizefighters 2, Hitman GO, Geometry Dash).  The escape hatch stays for a
   * device that genuinely wants the modeset. */
  const char *exclusive = getenv("SWORDIGO_EXCLUSIVE_FULLSCREEN");
  Uint32 fullscreen = g_fullscreen_desktop
                          ? SDL_WINDOW_FULLSCREEN_DESKTOP
                          : SDL_WINDOW_FULLSCREEN;
  if (exclusive && exclusive[0] == '1')
    fullscreen = SDL_WINDOW_FULLSCREEN;
  g_win = SDL_CreateWindow("Swordigo", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, screen_width, screen_height,
                           SDL_WINDOW_OPENGL | fullscreen);
  if (!g_win && fullscreen == SDL_WINDOW_FULLSCREEN_DESKTOP) {
    debugPrintf("gl: RGBA GLES1 desktop window refused (%s); "
                "probing SDL-owned GLES2 backend\n", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_Window *probe = SDL_CreateWindow(
        "Swordigo GL probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        screen_width, screen_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!probe) {
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
      probe = SDL_CreateWindow(
          "Swordigo GL probe", SDL_WINDOWPOS_UNDEFINED,
          SDL_WINDOWPOS_UNDEFINED, screen_width, screen_height,
          SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    if (probe) {
      SDL_GLContext probe_context = SDL_GL_CreateContext(probe);
      if (probe_context)
        SDL_GL_DeleteContext(probe_context);
      SDL_DestroyWindow(probe);
      debugPrintf("gl: SDL GLES2 capability probe completed\n");
    } else {
      debugPrintf("gl: SDL GLES2 capability probe refused (%s)\n",
                  SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    g_win = SDL_CreateWindow("Swordigo", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, screen_width,
                             screen_height,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
  }
  if (!g_win && fullscreen == SDL_WINDOW_FULLSCREEN_DESKTOP) {
    debugPrintf("gl: fullscreen-desktop refused (%s); retrying exclusive\n",
                SDL_GetError());
    fullscreen = SDL_WINDOW_FULLSCREEN;
    g_win = SDL_CreateWindow("Swordigo", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, screen_width,
                             screen_height, SDL_WINDOW_OPENGL | fullscreen);
  }
  /* Deliberately no "portable provider name" fallback here.  Forcing
   * SDL_VIDEO_GL_DRIVER at this point opens a window that succeeds and draws
   * nothing (the engine is fixed-function GLES 1.1; the unversioned providers
   * on crossed-SONAME firmware hand back an ES 3.x context), and a successful
   * window short-circuits the proven repair below -- glfix never gets to probe
   * a real provider and re-exec with it preloaded.  Failing here is what makes
   * the repair run. */
  if (!g_win) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    /* AeUX-class firmware: the resolved EGL provider refuses the kernel
     * driver (user/kernel API mismatch) and no window ever exists.  Probe
     * for a provider that initializes on this kernel and re-exec once with
     * it preloaded; returns untouched when no candidate proves itself. */
    glfix_maybe_reexec_noctx("window creation failed",
                             SDL_GetCurrentVideoDriver(),
                             glfix_video_teardown);
    return -1;
  }
  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) {
    fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
    glfix_maybe_reexec_noctx("context creation failed",
                             SDL_GetCurrentVideoDriver(),
                             glfix_video_teardown);
    return -1;
  }
  if (SDL_GL_MakeCurrent(g_win, g_ctx) != 0) {
    fprintf(stderr, "SDL_GL_MakeCurrent: %s\n", SDL_GetError());
    return -1;
  }
  int swap_request = SDL_GL_SetSwapInterval(1);
  resolve_present_functions();
  /* The window size a firmware grants is not always the size it draws into
   * (KMSDRM and scaled panels differ).  Touch hitboxes and the viewport must
   * follow the real drawable, otherwise the HUD lands off-target on some
   * devices -- the zoom class of bug seen in the LEGO/LOTR ports. */
  int draw_w = 0, draw_h = 0;
  {
    SDL_GL_GetDrawableSize(g_win, &draw_w, &draw_h);
    if (draw_w > 0 && draw_h > 0 &&
        (draw_w != screen_width || draw_h != screen_height)) {
      debugPrintf("gl: drawable %dx%d overrides window %dx%d\n", draw_w, draw_h,
                  screen_width, screen_height);
      screen_width = draw_w;
      screen_height = draw_h;
    }
  }
  int got_alpha = -1, got_depth = -1;
  SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &got_alpha);
  SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &got_depth);
  /* A black-screen report is only actionable with the video driver, the config
   * the firmware actually granted (alpha above all) and the GL blob in use. */
  const GLubyte *renderer = glGetString(GL_RENDERER);
  const GLubyte *version = glGetString(GL_VERSION);
  const char *video_driver = SDL_GetCurrentVideoDriver();
  gl_latebind_configure(video_driver, (const char *)renderer,
                        (const char *)version);
  /* This game is fixed-function GLES 1.1.  A context that reports ES 2.x/3.x
   * without the ES-CM profile has no fixed-function pipeline, so every draw
   * the engine issues is silently dropped and the panel stays black while
   * audio and input keep working.  Name it in the log: that symptom is
   * otherwise indistinguishable from a dead engine. */
  {
    const char *v = version ? (const char *)version : "";
    int is_es1 = (strstr(v, "ES-CM") != NULL) || (strstr(v, "ES 1.") != NULL);
    if (!is_es1)
      debugPrintf(
          "gl: WARNING context is '%s' but this engine is fixed-function "
          "GLES1.1 -- draws will not reach the panel\n",
          v[0] ? v : "?");
  }
  /* Crossed-provider firmware can yield a real context whose renderer is empty
   * and whose draws never reach the panel. Re-exec once with SDL's EGL/GLES
   * paths bound to the same proven object; healthy stacks (including
   * Panfrost, which reports a renderer) are never touched. */
  glfix_maybe_reexec(
      (const char *)renderer, video_driver, g_win != NULL,
      SDL_GL_GetCurrentContext() == g_ctx, draw_w > 0 && draw_h > 0,
      glfix_video_teardown);
  g_finish_before_swap = finish_before_swap_policy(video_driver);
  debugPrintf("gl: GLES1.1 %dx%d alpha=%d depth=%d driver='%s'\n", screen_width,
              screen_height, got_alpha, got_depth,
              video_driver ? video_driver : "?");
  debugPrintf("gl: renderer='%s' version='%s' fullscreen=%s flags=0x%x\n",
              renderer ? (const char *)renderer : "?",
              version ? (const char *)version : "?",
              fullscreen == SDL_WINDOW_FULLSCREEN ? "exclusive" : "desktop",
              (unsigned)SDL_GetWindowFlags(g_win));
  debugPrintf("gl: swap requested=%s interval=%d finish-before-swap=%s\n",
              swap_request == 0 ? "yes" : "no", SDL_GL_GetSwapInterval(),
              g_finish_before_swap ? "yes" : "no");
  return 0;
}

/* Force the frame opaque immediately before the present.  GLES1.1 has no core
 * framebuffer object, but the game may bind one through GL_OES_framebuffer_object
 * and a clear aimed at a bound FBO would leave the window untouched -- the trap
 * that cost Horizon Chase v1.2.0 -- so framebuffer 0 is rebound explicitly when
 * the extension entry point exists.  Runs once per frame and logs once. */
#ifndef GL_FRAMEBUFFER_OES
#define GL_FRAMEBUFFER_OES 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING_OES
#define GL_FRAMEBUFFER_BINDING_OES 0x8CA6
#endif

typedef struct PresentState {
  GLboolean scissor_enabled;
  GLboolean color_mask[4];
  GLfloat clear_colour[4];
  GLint framebuffer;
  int framebuffer_known;
} PresentState;

static void snapshot_present_state(PresentState *state) {
  memset(state, 0, sizeof(*state));
  state->scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
  glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, state->clear_colour);
  if (g_bind_framebuffer_oes) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &state->framebuffer);
    state->framebuffer_known = 1;
  }
}

static void restore_present_state(const PresentState *state) {
  glClearColor(state->clear_colour[0], state->clear_colour[1],
               state->clear_colour[2], state->clear_colour[3]);
  glColorMask(state->color_mask[0], state->color_mask[1],
              state->color_mask[2], state->color_mask[3]);
  if (state->scissor_enabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  if (state->framebuffer_known)
    g_bind_framebuffer_oes(GL_FRAMEBUFFER_OES, (GLuint)state->framebuffer);
}

/* Read the finished frame back once, a few seconds in, and log what is in it.
 * A black-screen report from a device nobody here owns is otherwise a guessing
 * game; this single line separates the three mechanisms the fleet has already
 * paid for: colour with alpha 0 means the scanout composited the frame away,
 * colour with alpha 255 means the picture is fine and the wrong surface is
 * being presented, and an empty frame means the engine really drew nothing. */
static void present_opaque_alpha(void) {
  static int logged = 0;
  PresentState state;
  if (!g_opaque_backbuffer)
    return;
  snapshot_present_state(&state);
  if (state.scissor_enabled)
    glDisable(GL_SCISSOR_TEST);
  if (state.framebuffer_known)
    g_bind_framebuffer_oes(GL_FRAMEBUFFER_OES, 0);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  restore_present_state(&state);

  if (!logged) {
    logged = 1;
    debugPrintf("gl: opaque-alpha present active (fbo rebind=%s resolver=%s)\n",
                g_bind_framebuffer_oes ? "yes" : "unavailable",
                g_bind_framebuffer_source);
  }
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("missing %s (place libswordigo.so next to the binary)", SO_NAME);
  debugPrintf("game data: %s size=%lld bytes\n", SO_NAME,
              (long long)st.st_size);
  if (stat("assets/resources", &st) < 0)
    fatal_error("missing assets/resources (extract from APK)");
  if (stat("res/7c.mp3", &st) < 0)
    fatal_error("missing res/*.mp3 (extract APK res/ folder)");
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  glfix_set_argv(argv);

  if (argc > 1 && chdir(argv[1]) == 0)
    debugPrintf("chdir %s\n", argv[1]);
  if (!getcwd(data_path, sizeof(data_path)))
    snprintf(data_path, sizeof(data_path), ".");

  configure_runtime_environment();
  debug_control_enabled = getenv("SWORDIGO_DEBUG_CONTROL") != NULL;
  g_fullscreen_desktop = swordigo_contract_quirk_enabled(
      "game.swordigo.fullscreen-desktop");
  g_opaque_backbuffer = swordigo_contract_quirk_enabled(
      "game.swordigo.present-alpha-one");
  debugPrintf("=== swordigo NextOS — data=%s ===\n", data_path);
  debugPrintf("contract: fullscreen-desktop=%s opaque-backbuffer=%s "
              "finish=%s glfix-context=%s glfix-pre-context=%s\n",
              g_fullscreen_desktop ? "yes" : "no",
              g_opaque_backbuffer ? "yes" : "no",
              swordigo_contract_quirk_enabled(
                  "game.swordigo.present-finish") ? "yes" : "no",
              swordigo_contract_quirk_enabled(
                  "adapter.gl-provider-reexec-preload") ? "yes" : "no",
              swordigo_contract_quirk_enabled(
                  "adapter.gl-provider-probe-init-reexec") ? "yes" : "no");
  /* Emit the launch receipt before anything can fail: a run that dies in GL
   * init is exactly the run whose context someone will need in order to know
   * whether the failure means anything about the port. */
  nxgl_frame_proof_launch_receipt();
  /* This engine resolves every gl* through SDL_GL_GetProcAddress, and on the
   * Mali-450 image glReadPixels is not a global symbol, so hand the adapter
   * the same resolver the engine uses. */
  nxgl_frame_proof_set_resolver(
      (void *(*)(const char *))SDL_GL_GetProcAddress);
  check_data();

  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap heap failed");

  if (so_load(SO_NAME, heap, heap_size) < 0)
    fatal_error("so_load(%s) failed", SO_NAME);
  crash_diag_install((uintptr_t)data_base, data_size);
  crash_diag_set_phase(CRASH_PHASE_RELOCATE);
  if (so_relocate() < 0)
    fatal_error("so_relocate failed");
  crash_diag_set_phase(CRASH_PHASE_RESOLVE);
  if (so_resolve(dynlib_functions, dynlib_functions_count, 1) < 0)
    fatal_error("so_resolve failed");

  /* Overlay hide hooks optional — trampoline slots are Switch-port specific;
   * enable with SWORDIGO_OVERLAY_HOOKS=1 once offsets are re-verified. */
  if (getenv("SWORDIGO_OVERLAY_HOOKS"))
    install_game_overlay_hooks();
  resolve_entry_points();

  crash_diag_set_phase(CRASH_PHASE_FINALIZE);
  so_finalize();
  so_flush_caches();
  so_record_phdr("libswordigo.so");

  crash_diag_set_phase(CRASH_PHASE_CONSTRUCTORS);
  so_execute_init_array();
  so_free_temp();
  crash_diag_install((uintptr_t)data_base, data_size);

  crash_diag_set_phase(CRASH_PHASE_GL);
  if (gl_init() < 0)
    fatal_error("GL init failed");
  crash_diag_install((uintptr_t)data_base, data_size);
  crash_diag_set_phase(CRASH_PHASE_AUDIO);
  init_openal();

  crash_diag_set_phase(CRASH_PHASE_JNI);
  jni_init();
  jni_configure_text_input(textInputTextDidChange, textInputDidFinish);

  crash_diag_set_phase(CRASH_PHASE_SET_DIRS);
  setFilesDir(fake_env, NULL, jni_make_string(data_path));
  setCacheDir(fake_env, NULL, jni_make_string(data_path));
  setAssetManager(fake_env, NULL, NULL);
  googleSignInCompleted(fake_env, NULL, 0);
  crash_diag_set_phase(CRASH_PHASE_APP_LAUNCH);
  handleApplicationLaunch(fake_env, NULL);

  crash_diag_set_phase(CRASH_PHASE_MUSIC);
  music_init(data_path);
  initMusicPlayer(fake_env, jni_make_object("MusicPlayer"));

  crash_diag_set_phase(CRASH_PHASE_NATIVE_INTERFACE);
  setupNativeInterface(fake_env, NULL);
  crash_diag_set_phase(CRASH_PHASE_APP_SETUP);
  setupApplication(fake_env, NULL);
  crash_diag_set_phase(CRASH_PHASE_VIEW_SIZE);
  setApplicationViewSize(fake_env, NULL, screen_width, screen_height, 1,
                         screen_width, screen_height);
  crash_diag_set_phase(CRASH_PHASE_APP_ACTIVE);
  applicationDidBecomeActive(fake_env, NULL);
  crash_diag_install((uintptr_t)data_base, data_size);

  debugPrintf("boot complete — entering loop\n");

  /* Remote tap: echo "tap X Y" > /dev/shm/swordigo_ctl  (view bottom-origin Y) */
  static int ctl_up_frame = -1;
  static float ctl_x, ctl_y;
  int frame = 0;

  Uint32 last = SDL_GetTicks();
  while (g_running) {
    crash_diag_set_phase(CRASH_PHASE_EVENTS);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        request_exit();
      else if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) {
        (void)pad_open_device(e.cdevice.which);
      } else if (e.type == SDL_CONTROLLERDEVICEREMAPPED && g_pad &&
                 e.cdevice.which ==
                     SDL_JoystickInstanceID(
                         SDL_GameControllerGetJoystick(g_pad))) {
        pad_refresh_topology();
      } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
                 e.cdevice.which ==
                     SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
        SDL_GameControllerClose(g_pad);
        g_pad = NULL;
        pad_refresh_topology();
      } else
        handle_pointer_event(&e);
    }
    if (ctl_up_frame >= 0 && frame >= ctl_up_frame) {
      emit_touch(TOUCH_ENDED, 8, ctl_x, ctl_y);
      ctl_up_frame = -1;
    }
    if (debug_control_enabled && (frame % 5) == 0) {
      FILE *cf = fopen("/dev/shm/swordigo_ctl", "r");
      if (cf) {
        char cmd[16] = {0};
        float x = 0, y = 0;
        if (fscanf(cf, "%15s %f %f", cmd, &x, &y) >= 1) {
          if (!strcmp(cmd, "tap")) {
            ctl_x = x;
            ctl_y = y;
            emit_touch(TOUCH_BEGAN, 8, x, y);
            ctl_up_frame = frame + 6;
            debugPrintf("[ctl] tap %.0f,%.0f @f%d\n", x, y, frame);
          } else if (!strcmp(cmd, "cursor")) {
            g_cursor_x = x;
            g_cursor_y = y;
            g_cursor_show = 5.0f;
            debugPrintf("[ctl] cursor %.0f,%.0f\n", x, y);
          } else if (!strcmp(cmd, "quit")) {
            request_exit();
          }
        }
        fclose(cf);
        unlink("/dev/shm/swordigo_ctl");
      }
    }
    Uint32 now = SDL_GetTicks();
    float dt = (now - last) / 1000.0f;
    last = now;
    if (dt <= 0.0f || dt > 0.1f)
      dt = 1.0f / 60.0f;

    frame++;
    crash_diag_set_frame((unsigned)frame);
    crash_diag_set_phase(CRASH_PHASE_CONTROLS);
    update_pad();
    update_cursor(dt);
    crash_diag_set_phase(CRASH_PHASE_JNI_UPDATE);
    jni_update();

    g_time += dt;

    crash_diag_set_phase(CRASH_PHASE_UPDATE);
    updateApplication(fake_env, NULL, dt);
    crash_diag_set_phase(CRASH_PHASE_DRAW);
    drawApplication(fake_env, NULL);
    crash_diag_set_phase(CRASH_PHASE_CURSOR);
    cursor_draw_overlay();
    crash_diag_set_phase(CRASH_PHASE_PRESENT_ALPHA);
    present_opaque_alpha();
    /* Several samples, not one: a title card can legitimately be black at the
     * first sample and a single reading turns that into a false verdict. */
    if (frame == 300 || frame == 600 || frame == 900) {
      crash_diag_set_phase(CRASH_PHASE_FRAME_PROBE);
      /* The framework adapter measures, publishes and writes the proof image;
       * the local FBO rebind stays because this engine presents through an
       * OES framebuffer on some stacks and the readback must see FBO 0. */
      GLint previous_framebuffer = 0;
      if (g_bind_framebuffer_oes) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &previous_framebuffer);
        g_bind_framebuffer_oes(GL_FRAMEBUFFER_OES, 0);
      }
      nxgl_frame_proof_sample(screen_width, screen_height);
      nxgl_frame_proof_publish();
      if (g_bind_framebuffer_oes)
        g_bind_framebuffer_oes(GL_FRAMEBUFFER_OES, (GLuint)previous_framebuffer);
    }
    if (g_finish_before_swap) {
      crash_diag_set_phase(CRASH_PHASE_FINISH);
      glFinish();
    }
    crash_diag_set_phase(CRASH_PHASE_SWAP);
    SDL_GL_SwapWindow(g_win);
  }

  nxgl_frame_proof_publish();
  crash_diag_set_phase(CRASH_PHASE_SHUTDOWN);
  pthread_t d;
  if (pthread_create(&d, NULL, exit_deadline, NULL) == 0)
    pthread_detach(d);

  music_deinit();
  if (sharedAudioSystem && shutdownAudioSystem) {
    void *as = sharedAudioSystem();
    if (as)
      shutdownAudioSystem(as);
  }
  if (handleApplicationQuit)
    handleApplicationQuit(fake_env, NULL);
  deinit_openal();
  _exit(0);
}
