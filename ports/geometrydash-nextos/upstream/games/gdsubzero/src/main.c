/*
 * main.c -- Geometry Dash SubZero (Cocos2d-x 2.x, arm64) so-loader for NextOS
 * on Mali-450.
 *
 * Boot follows the Android sequence exactly:
 *   load libfmod.so -> load libcocos2dcpp.so -> init_array -> JNI_OnLoad(vm)
 *   -> Cocos2dxHelper.nativeSetApkPath(game.apk) -> Cocos2dxRenderer.nativeInit(w,h)
 *   -> loop { input; nativeRender(); swap }
 *
 * The engine reads its assets straight out of game.apk with its own minizip, so
 * nothing is extracted.  Everything the Java side would have answered lives in
 * jni_shim.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "egl_shim.h"
#include "error.h"
#include "jni_shim.h"
#include "nx_frameprobe.h"
#include "gd_platform.h"
#include "gd_move.h"
#include "pad_evdev.h"
#include "so_util.h"
#include "util.h"

extern void gd_clock_absorb_us(int64_t us);
extern void gd_install_step_guard(void);
extern DynLibFunction *gd_fmod_probe_table(DynLibFunction *fmod, int fmod_n,
                                           int *out_n);
extern int gd_apk_index(const char *apk_path, const char *cache_dir);

#define FMOD_SO "libfmod.so"
#define GAME_SO "libcocos2dcpp.so"
#define FMOD_HEAP_MB 24
#define GAME_HEAP_MB 320

extern DynLibFunction gd_overrides[];
extern const int gd_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

/* Bionic keeps the stack guard at tpidr_el0+0x28; under glibc that slot lands
 * in some other library's TLS and moves.  A TLS pad in the executable's own
 * block pins it to a stable zero. */
__attribute__((aligned(16))) static _Thread_local char g_tls_pad[256];

static volatile uintptr_t g_game_base;
static volatile int g_running = 1;

/* ---------------------------------------------------------- entry points -- */
typedef void *JNIEnvPtr;
static void *g_env, *g_vm;

static int (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_setApkPath)(void *env, void *thiz, void *jstr);
static void (*e_nativeInit)(void *env, void *thiz, int w, int h);
static void (*e_nativeRender)(void *env, void *thiz);
static void (*e_onPause)(void *env, void *thiz);
static void (*e_onResume)(void *env, void *thiz);
static void (*e_touchBegin)(void *env, void *thiz, int id, float x, float y);
static void (*e_touchEnd)(void *env, void *thiz, int id, float x, float y);
static void (*e_touchMove)(void *env, void *thiz, void *ids, void *xs, void *ys);
static void (*e_keyDown)(void *env, void *thiz, int keycode);
static void (*e_bitmapDC)(void *env, void *thiz, int w, int h, void *pixels);

/* --------------------------------------------------------- crash reports -- */
static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0)
    return;
  char buf[8192];
  char line[400];
  int li = 0, n;
  while ((n = (int)read(fd, buf, sizeof(buf))) > 0)
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e;
        char perm[8], path[256];
        path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >= 3 &&
            a >= s && a < e) {
          const char *base = strrchr(path, '/');
          base = base ? base + 1 : (path[0] ? path : "?");
          snprintf(out, (size_t)outsz, "%s+0x%lx", base, (unsigned long)(a - s));
          close(fd);
          return;
        }
        li = 0;
      } else
        line[li++] = c;
    }
  close(fd);
}

static void dump_context(const char *what, mcontext_t *m) {
  char r[320];
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "%s PC=%p %s", what, (void *)m->pc, r);
  if (g_game_base && m->pc >= g_game_base)
    fprintf(stderr, " {game+0x%lx}", (unsigned long)(m->pc - g_game_base));
  fprintf(stderr, "\n");
  resolve_addr(m->regs[30], r, sizeof(r));
  fprintf(stderr, "  LR=%p %s", (void *)m->regs[30], r);
  if (g_game_base && m->regs[30] >= g_game_base)
    fprintf(stderr, " {game+0x%lx}", (unsigned long)(m->regs[30] - g_game_base));
  fprintf(stderr, "\n");
  /* Registers as well: a stack alone does not say which loop bound went wrong,
   * and on a hang there is no core to inspect afterwards. */
  for (int i = 0; i < 31; i += 4) {
    fprintf(stderr, "  ");
    for (int k = i; k < i + 4 && k < 31; k++)
      fprintf(stderr, "x%-2d=%016lx ", k, (unsigned long)m->regs[k]);
    fprintf(stderr, "\n");
  }
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < 24 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp, next = p[0], lr = p[1];
    if (!lr)
      break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_game_base && lr >= g_game_base)
      fprintf(stderr, " {game+0x%lx}", (unsigned long)(lr - g_game_base));
    fprintf(stderr, "\n");
    if (next <= fp)
      break;
    fp = next;
  }
  fflush(stderr);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  mcontext_t *m = &((ucontext_t *)uc)->uc_mcontext;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(SYS_gettid));
  dump_context(" ", m);
  _exit(128 + sig);
}

static void bt_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  mcontext_t *m = &((ucontext_t *)uc)->uc_mcontext;
  fprintf(stderr, "\n[BT sig=%d tid=%d]", sig, (int)syscall(SYS_gettid));
  dump_context("", m);
}

static void term_handler(int sig) {
  (void)sig;
  g_running = 0;
}

static void install_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);

  struct sigaction sb;
  memset(&sb, 0, sizeof(sb));
  sb.sa_sigaction = bt_handler;
  sb.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &sb, NULL);

  struct sigaction sc;
  memset(&sc, 0, sizeof(sc));
  sc.sa_handler = term_handler;
  sigaction(SIGTERM, &sc, NULL);
  sigaction(SIGINT, &sc, NULL);
}

/* ------------------------------------------------------------- loading ---- */
static void preload_device_libs(void) {
  static const char *libs[] = {"libGLESv2.so", "libEGL.so",
                               "libSDL2-2.0.so.0", "libz.so.1",
                               "libm.so.6",       "libdl.so.2",
                               "libstdc++.so.6",  NULL};
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    if (!h)
      debugPrintf("[preload] %s: %s\n", libs[i], dlerror());
  }
}

static DynLibFunction *g_base;
static int g_base_n;

static void build_base_table(void) {
  g_base_n = gd_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * (size_t)g_base_n);
  memcpy(g_base, gd_overrides,
         sizeof(DynLibFunction) * (size_t)gd_overrides_count);
  memcpy(g_base + gd_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * (size_t)revc_pthread_count);
}

static DynLibFunction *load_module(const char *name, int heap_mb,
                                   DynLibFunction *tbl, int n, int *out_n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %d MB falhou para %s", heap_mb, name);
  debugPrintf("== carregando %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0)
    fatal_error("so_load(%s) falhou", name);
  if (so_relocate() < 0)
    fatal_error("so_relocate(%s) falhou", name);
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  debugPrintf("== %s pronto: text=%p+%zu data=%p+%zu ==\n", name, text_base,
              text_size, data_base, data_size);
  if (out_n)
    return so_snapshot_symbols(out_n);
  return NULL;
}

static uintptr_t table_find(DynLibFunction *t, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (t[i].symbol && !strcmp(t[i].symbol, name))
      return t[i].func;
  return 0;
}

static DynLibFunction *tbl_concat(DynLibFunction *a, int an, DynLibFunction *b,
                                  int bn, int *out_n) {
  DynLibFunction *c = malloc(sizeof(DynLibFunction) * (size_t)(an + bn));
  memcpy(c, a, sizeof(DynLibFunction) * (size_t)an);
  memcpy(c + an, b, sizeof(DynLibFunction) * (size_t)bn);
  *out_n = an + bn;
  return c;
}

/* ---------------------------------------------------------------- input --- */
/* Geometry Dash is a one-button game almost everywhere: every face button is
 * "touch the screen", held for as long as the button is held (ship and wave
 * modes need the sustained touch -- never Begin+End in the same frame).  The
 * exception is 2.2's platformer levels, the Tower's challenges among them,
 * where the game also has to be walked left and right -- that goes through the
 * engine's own key handler in gd_move.c, on the D-pad and the LEFT stick.
 *
 * The menus are touch-first, so a small arrow aims the taps.  It is driven by
 * the RIGHT stick (the NextOS asked for the right stick) plus the D-pad, and R3
 * presses wherever it sits.  The arrow is FIXED: born in the middle of the
 * screen and always on.  Nothing else reaches the game by any other path. */
#define AKEYCODE_BACK 4

#define MAX_PADS 4
static SDL_GameController *g_pad[MAX_PADS];
static SDL_Joystick *g_joy[MAX_PADS];
static int g_npads;
static int g_touching;
static float g_cursor_x, g_cursor_y;

/* The arrow is only shown while it is being used: it appears the moment the
 * right stick (or the D-pad) moves and fades out again after this long with no
 * movement.  Its POSITION is kept either way, so a press always lands where
 * the arrow was left -- hiding it never loses the aim. */
#define CURSOR_VISIBLE_MS 4000
static Uint32 g_cursor_shown_until;

/* Well past the resting noise of the sticks seen here, but still a light
 * touch: the arrow has to answer a small nudge. */
#define CURSOR_DEADZONE 8000

/* Walking is a real direction, not an aim: it only answers a deliberate push,
 * so a stick resting a little off centre never walks on its own. */
#define MOVE_DEADZONE 12000

/* Every connected pad is read and OR-ed together, so the port does not care
 * which device index the frontend handed us. */
static int pad_button(int b) {
  for (int i = 0; i < g_npads; i++)
    if (g_pad[i] &&
        SDL_GameControllerGetButton(g_pad[i], (SDL_GameControllerButton)b))
      return 1;
  return 0;
}
/* Some USB adapters rest with a permanent offset on the sticks (the Twin USB
 * PS2 adapter here sits a few thousand units off zero).  The value seen shortly
 * after the pad shows up becomes the zero point, so a drifting stick does not
 * walk the cursor across the screen on its own. */
static int g_axis_zero[MAX_PADS][SDL_CONTROLLER_AXIS_MAX];
static Uint32 g_axis_zero_at[MAX_PADS];

static void calibrate_axes(void) {
  Uint32 now = SDL_GetTicks();
  for (int i = 0; i < g_npads; i++) {
    if (!g_pad[i] || !g_axis_zero_at[i] || now < g_axis_zero_at[i])
      continue;
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
      if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        continue;
      g_axis_zero[i][a] =
          SDL_GameControllerGetAxis(g_pad[i], (SDL_GameControllerAxis)a);
    }
    debugPrintf("[pad] zero do controle %d: RX=%d RY=%d (LX=%d LY=%d)\n", i,
                g_axis_zero[i][SDL_CONTROLLER_AXIS_RIGHTX],
                g_axis_zero[i][SDL_CONTROLLER_AXIS_RIGHTY],
                g_axis_zero[i][SDL_CONTROLLER_AXIS_LEFTX],
                g_axis_zero[i][SDL_CONTROLLER_AXIS_LEFTY]);
    g_axis_zero_at[i] = 0;
  }
}

static int pad_axis(int a) {
  int best = 0;
  for (int i = 0; i < g_npads; i++) {
    if (!g_pad[i])
      continue;
    int v = SDL_GameControllerGetAxis(g_pad[i], (SDL_GameControllerAxis)a);
    if (a != SDL_CONTROLLER_AXIS_TRIGGERLEFT &&
        a != SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
      v -= g_axis_zero[i][a];
    if (abs(v) > abs(best))
      best = v;
  }
  return best;
}

/* SELECT/START also arrive as TRIGGER_HAPPY on several handhelds, and some
 * firmware GUIDs carry a CRC that does not match gamecontrollerdb at all, so
 * the raw joystick buttons are scanned as well -- but ONLY the ones the
 * mapping does not already own.  Guessing bare indices is what made R3 (raw
 * button 10 on this pad) fire "back" on every press: one click both tapped the
 * screen and paused the game. */
static unsigned g_raw_bound[MAX_PADS]; /* raw indices claimed by the mapping */

static void scan_mapping_bound(int slot, SDL_GameController *c) {
  g_raw_bound[slot] = 0;
  char *m = SDL_GameControllerMapping(c);
  if (!m)
    return;
  for (char *p = m; *p; p++)
    if (*p == 'b' && p != m && p[-1] == ':') {
      int n = atoi(p + 1);
      if (n >= 0 && n < 32)
        g_raw_bound[slot] |= 1u << n;
    }
  debugPrintf("[pad] mapping do controle %d ocupa os botoes crus 0x%x\n", slot,
              g_raw_bound[slot]);
  SDL_free(m);
}

static int raw_button(int idx) {
  for (int i = 0; i < g_npads; i++) {
    SDL_Joystick *j = g_joy[i];
    if (!j || idx < 0 || idx >= SDL_JoystickNumButtons(j))
      continue;
    if (g_pad[i] && (g_raw_bound[i] & (1u << (unsigned)idx)))
      continue; /* the mapping already gave this index a job */
    if (SDL_JoystickGetButton(j, idx))
      return 1;
  }
  return 0;
}

static void add_pad(int index) {
  if (g_npads >= MAX_PADS)
    return;
  if (SDL_IsGameController(index)) {
    SDL_GameController *c = SDL_GameControllerOpen(index);
    if (!c)
      return;
    g_pad[g_npads] = c;
    g_joy[g_npads] = SDL_GameControllerGetJoystick(c);
    g_axis_zero_at[g_npads] = SDL_GetTicks() + 800;
    memset(g_axis_zero[g_npads], 0, sizeof(g_axis_zero[0]));
    scan_mapping_bound(g_npads, c);
    debugPrintf("[pad] %s (controller %d)\n", SDL_GameControllerName(c), index);
  } else {
    SDL_Joystick *j = SDL_JoystickOpen(index);
    if (!j)
      return;
    g_pad[g_npads] = NULL;
    g_joy[g_npads] = j;
    g_raw_bound[g_npads] = 0;
    debugPrintf("[pad] %s (joystick cru, %d botoes)\n", SDL_JoystickName(j),
                SDL_JoystickNumButtons(j));
  }
  g_npads++;
}

static void open_pads(void) {
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    debugPrintf("[pad] InitSubSystem: %s\n", SDL_GetError());
  const char *db = getenv("sdl_controllerconfig");
  if (db && *db) {
    if (SDL_GameControllerAddMapping(db) < 0)
      debugPrintf("[pad] AddMapping: %s\n", SDL_GetError());
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    add_pad(i);
  if (!g_npads)
    debugPrintf("[pad] nenhum controle encontrado ainda (hotplug ligado)\n");
  /* O mapping da SDL costuma vir sem back/start/leftstick/rightstick nestes
   * portateis; o evdev completa sem cravar indice de botao. */
  pad_evdev_open();
}

static void touch_set(int down) {
  if (down == g_touching)
    return;
  g_touching = down;
  if (down) {
    if (e_touchBegin)
      e_touchBegin(g_env, NULL, 0, g_cursor_x, g_cursor_y);
  } else {
    if (e_touchEnd)
      e_touchEnd(g_env, NULL, 0, g_cursor_x, g_cursor_y);
  }
}

static void pump_input(void) {
  pad_evdev_poll();
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_QUIT)
      g_running = 0;
    if (ev.type == SDL_JOYDEVICEADDED) {
      add_pad(ev.jdevice.which);
      /* O evdev do pad novo tambem precisa ser aberto: e' de la' que vem
       * SELECT/START/L3/R3 nos aparelhos que os entregam como TRIGGER_HAPPY. */
      pad_evdev_rescan();
    }
    if (ev.type == SDL_CONTROLLERDEVICEREMOVED ||
        ev.type == SDL_JOYDEVICEREMOVED) {
      for (int i = 0; i < g_npads; i++) {
        if (g_joy[i] && !SDL_JoystickGetAttached(g_joy[i])) {
          if (g_pad[i])
            SDL_GameControllerClose(g_pad[i]);
          for (int k = i; k + 1 < g_npads; k++) {
            g_pad[k] = g_pad[k + 1];
            g_joy[k] = g_joy[k + 1];
          }
          g_npads--;
          i--;
        }
      }
    }
    /* Every button the pad sends, by NAME, once per press.  A pad whose
     * firmware GUID does not match gamecontrollerdb gets an automatic mapping
     * that can leave a face button with a different name than the one printed
     * on it -- guessing which is why "A does not jump" is unanswerable from
     * here.  This says it out loud. */
    if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
      static unsigned seen_named;
      unsigned bit = 1u << (ev.cbutton.button & 31);
      if (!(seen_named & bit)) {
        seen_named |= bit;
        const char *name = SDL_GameControllerGetStringForButton(
            (SDL_GameControllerButton)ev.cbutton.button);
        debugPrintf("[pad] botao \"%s\" chegou (indice %d do mapping)\n",
                    name ? name : "?", (int)ev.cbutton.button);
      }
    }
    if (ev.type == SDL_JOYBUTTONDOWN) {
      static unsigned seen_raw;
      unsigned bit = 1u << (ev.jbutton.button & 31);
      if (!(seen_raw & bit)) {
        seen_raw |= bit;
        debugPrintf("[pad] botao CRU %d chegou\n", (int)ev.jbutton.button);
      }
    }
    if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
      g_running = 0;
  }

  calibrate_axes();

  const float ew = (float)gd_engine_w(), eh = (float)gd_engine_h();

  /* Walking left and right in the 2.2 platformer levels (the Tower's
   * challenges): the D-pad and the LEFT stick press the game's own arrow keys
   * through UILayer -- see gd_move.c.  The left stick does nothing else, and
   * outside a level (menus) this is inert. */
  int lx = pad_axis(SDL_CONTROLLER_AXIS_LEFTX);
  int walk_left = pad_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -MOVE_DEADZONE;
  int walk_right = pad_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > MOVE_DEADZONE;
  int walking = gd_move_live();
  gd_move_set(walk_left, walk_right);

  /* cursor.  While a level is on screen the D-pad's left/right are walking, so
   * they stop dragging the arrow -- it stays where it was aimed.  Up/down and
   * the right stick keep moving it, which is what the pause menu needs. */
  float dx = 0.0f, dy = 0.0f;
  if (!walking && pad_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT))
    dx -= 1.0f;
  if (!walking && pad_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
    dx += 1.0f;
  if (pad_button(SDL_CONTROLLER_BUTTON_DPAD_UP))
    dy -= 1.0f;
  if (pad_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN))
    dy += 1.0f;
  /* RIGHT stick only.  The left stick is deliberately inert: the NextOS wants
   * the arrow on the right one, and a cursor on both sticks is how it ends up
   * being moved by accident. */
  int ax = pad_axis(SDL_CONTROLLER_AXIS_RIGHTX),
      ay = pad_axis(SDL_CONTROLLER_AXIS_RIGHTY);
  if (ax > CURSOR_DEADZONE || ax < -CURSOR_DEADZONE)
    dx += (float)ax / 32767.0f;
  if (ay > CURSOR_DEADZONE || ay < -CURSOR_DEADZONE)
    dy += (float)ay / 32767.0f;

  /* Time-based, so the arrow travels the same distance regardless of fps. */
  static Uint32 last_tick;
  Uint32 now_ms = SDL_GetTicks();
  float dt = last_tick ? (float)(now_ms - last_tick) / 1000.0f : 1.0f / 60.0f;
  last_tick = now_ms;
  if (dt > 0.1f)
    dt = 0.1f;

  if (dx != 0.0f || dy != 0.0f) {
    float speed = ew * 0.45f * dt; /* ~2.2 s to cross the screen */
    g_cursor_x += dx * speed;
    g_cursor_y += dy * speed;
    if (g_cursor_x < 0.0f) g_cursor_x = 0.0f;
    if (g_cursor_y < 0.0f) g_cursor_y = 0.0f;
    if (g_cursor_x > ew - 1.0f) g_cursor_x = ew - 1.0f;
    if (g_cursor_y > eh - 1.0f) g_cursor_y = eh - 1.0f;
    g_cursor_shown_until = now_ms + CURSOR_VISIBLE_MS;
    if (g_touching && e_touchMove) {
      int ids[1] = {0};
      float xs[1] = {g_cursor_x}, ys[1] = {g_cursor_y};
      e_touchMove(g_env, NULL, jni_shim_new_int_array(ids, 1),
                  jni_shim_new_float_array(xs, 1),
                  jni_shim_new_float_array(ys, 1));
    }
  }
  gd_cursor_set(g_cursor_x, g_cursor_y, now_ms < g_cursor_shown_until);

  /* jump / tap.  Two jobs on purpose, and the button decides which -- never a
   * clock: a rule that expires mid-press makes R3 click and let go on its own.
   *
   *   R3 (the stick that aims the arrow) ALWAYS taps, anywhere, always.
   *   Every other jump button taps in the menus and is a KEY inside a level.
   *
   * The key is what makes the platformer levels playable: there the game draws
   * its own left/right arrows in the corner, and a tap parked on one of them
   * walks instead of jumping.  A key lands on the player wherever the arrow is.
   * The pause menu of a level is still pressed -- with R3, which is the button
   * that goes with aiming anyway. */
  int tap = pad_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK) || pad_evdev_r3();
  int jump = pad_button(SDL_CONTROLLER_BUTTON_A) ||
             pad_button(SDL_CONTROLLER_BUTTON_B) ||
             pad_button(SDL_CONTROLLER_BUTTON_X) ||
             pad_button(SDL_CONTROLLER_BUTTON_Y) ||
             pad_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
             pad_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
             pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 ||
             pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
  {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks && (ks[SDL_SCANCODE_SPACE] || ks[SDL_SCANCODE_RETURN]))
      jump = 1;
  }
  /* Never both at once: two presses of the same button are two jumps. */
  gd_move_jump(walking ? jump : 0);
  int press = walking ? tap : (tap || jump);
  if (press != g_touching)
    debugPrintf("[input] toque %s em (%.0f, %.0f)\n", press ? "DOWN" : "UP",
                g_cursor_x, g_cursor_y);
  touch_set(press ? 1 : 0);

  /* START = back/pause; SELECT+START = quit.  Os tres caminhos se somam: o
   * mapping da SDL quando ele existe, os indices crus que o mapping NAO
   * reivindicou, e o evdev -- que e' o unico que sempre acha SELECT/START
   * nestes portateis, onde eles chegam como TRIGGER_HAPPY. */
  static int prev_start;
  int start = pad_button(SDL_CONTROLLER_BUTTON_START) || raw_button(9) ||
              raw_button(11) || pad_evdev_start();
  int select = pad_button(SDL_CONTROLLER_BUTTON_BACK) || raw_button(8) ||
               raw_button(10) || pad_evdev_select();
  if (start && select) {
    debugPrintf("[input] SELECT+START -> sair\n");
    g_running = 0;
  } else if (start && !prev_start && e_keyDown) {
    e_keyDown(g_env, NULL, AKEYCODE_BACK);
  }
  prev_start = start;
}

/* ------------------------------------------------------------------ main -- */
static void bind_entry_points(void) {
  e_JNI_OnLoad = (void *)so_find_addr("JNI_OnLoad");
  e_setApkPath =
      (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  e_nativeInit =
      (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  e_nativeRender =
      (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  e_onPause = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  e_onResume = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  e_touchBegin = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  e_touchEnd = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  e_touchMove = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  e_keyDown = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
  e_bitmapDC = (void *)so_find_addr_safe(
      "Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
  gd_install_step_guard();
  gd_move_install();
  jni_shim_set_bitmap_dc(e_bitmapDC);
  g_game_base = (uintptr_t)text_base;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_handlers();
  /* A trava definitiva de instancia unica mora aqui, no binario: se o script
   * do launcher morrer, a dele morre junto e o jogo continuaria vivo. */
  if (gd_single_instance_lock() < 0)
    return 74;
  fprintf(stderr,
          "=== Geometry Dash SubZero -- so-loader NextOS aarch64 / Mali-450 ===\n");

  {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t slot = tp + 0x28, lo = (uintptr_t)g_tls_pad;
    fprintf(stderr, "TLS guard slot=0x%lx pad=[0x%lx..0x%lx] %s\n",
            (unsigned long)slot, (unsigned long)lo,
            (unsigned long)(lo + sizeof(g_tls_pad)),
            (slot >= lo && slot + 8 <= lo + sizeof(g_tls_pad)) ? "DENTRO"
                                                               : "FORA(!)");
  }

  /* Everything is relative to the port directory. */
  char base[PATH_MAX];
  if (!getcwd(base, sizeof(base)))
    snprintf(base, sizeof(base), ".");
  char writable[PATH_MAX + 16];
  snprintf(writable, sizeof(writable), "%s/userdata", base);
  mkdir(writable, 0755);
  char apk[PATH_MAX + 16];
  snprintf(apk, sizeof(apk), "%s/game.apk", base);
  if (access(apk, R_OK) != 0)
    fatal_error("game.apk nao encontrado em %s", apk);
  jni_shim_set_paths(writable, "com.robtopx.geometrydashsubzero");
  {
    char audio_cache[PATH_MAX + 24];
    snprintf(audio_cache, sizeof(audio_cache), "%s/audio", writable);
    gd_apk_index(apk, audio_cache);
  }

  if (SDL_Init(0) != 0)
    debugPrintf("[sdl] Init: %s\n", SDL_GetError());
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    debugPrintf("[sdl] InitAudio: %s\n", SDL_GetError());

  preload_device_libs();
  build_base_table();
  jni_shim_init(&g_vm, &g_env);

  /* Module A: libfmod.so.  The engine cross-resolves ~70 FMOD symbols in it. */
  int fmod_n = 0;
  DynLibFunction *fmod = load_module(FMOD_SO, FMOD_HEAP_MB, g_base, g_base_n,
                                     &fmod_n);
  debugPrintf("libfmod: %d simbolos exportados\n", fmod_n);

  /* libfmod.so has a JNI_OnLoad of its own, and that is where it stores the
   * JavaVM it needs to reach org/fmod/FMOD.  Skipping it is why FMOD never got
   * as far as picking an output: no dlopen of libOpenSLES, no sound at all. */
  {
    int (*fmod_onload)(void *, void *) =
        (void *)table_find(fmod, fmod_n, "JNI_OnLoad");
    if (fmod_onload)
      debugPrintf("libfmod JNI_OnLoad -> 0x%x\n", fmod_onload(g_vm, NULL));
    else
      debugPrintf("libfmod: JNI_OnLoad nao exportado (audio pode ficar mudo)\n");
  }
  int t1n = g_base_n;
  DynLibFunction *t1 = g_base;
  if (fmod) {
    /* The probes must sit AHEAD of libfmod's own exports: so_resolve takes the
     * first match, and each probe forwards to the real address it captured. */
    int pn = 0;
    DynLibFunction *probes = gd_fmod_probe_table(fmod, fmod_n, &pn);
    if (probes) {
      int mid = 0;
      DynLibFunction *m = tbl_concat(g_base, g_base_n, probes, pn, &mid);
      t1 = tbl_concat(m, mid, fmod, fmod_n, &t1n);
    } else {
      t1 = tbl_concat(g_base, g_base_n, fmod, fmod_n, &t1n);
    }
  }

  /* Module B: the engine. */
  load_module(GAME_SO, GAME_HEAP_MB, t1, t1n, NULL);
  bind_entry_points();

  int r = e_JNI_OnLoad(g_vm, NULL);
  debugPrintf("JNI_OnLoad(vm=%p) -> 0x%x\n", g_vm, r);

  /* The engine opens game.apk itself (minizip) -- nothing is extracted. */
  debugPrintf("nativeSetApkPath(%s)\n", apk);
  e_setApkPath(g_env, NULL, jni_shim_new_jstring(apk));

  if (!gd_gl_init())
    fatal_error("nao consegui criar o contexto GLES2");

  int ew = gd_engine_w(), eh = gd_engine_h();
  debugPrintf("nativeInit(%d, %d)\n", ew, eh);
  g_cursor_x = (float)ew * 0.5f;
  g_cursor_y = (float)eh * 0.5f;
  g_cursor_shown_until = SDL_GetTicks() + CURSOR_VISIBLE_MS;
  e_nativeInit(g_env, NULL, ew, eh);
  if (e_onResume)
    e_onResume(g_env, NULL);

  open_pads();

  debugPrintf("=== entrando no loop de frames ===\n");
  unsigned long frames = 0;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  while (g_running) {
    pump_input();
    struct timespec r0, r1;
    clock_gettime(CLOCK_MONOTONIC, &r0);
    e_nativeRender(g_env, NULL);
    clock_gettime(CLOCK_MONOTONIC, &r1);
    {
      double rms = (double)(r1.tv_sec - r0.tv_sec) * 1000.0 +
                   (double)(r1.tv_nsec - r0.tv_nsec) / 1e6;
      if (rms > 100.0) {
        /* Hand the stall back to the clock, or the next frame inherits it as
         * its delta and the collision pass runs for as many fixed ticks as
         * that delta is long. */
        gd_clock_absorb_us((int64_t)((rms - 16.0) * 1000.0));
        debugPrintf("[lento] frame %lu: nativeRender levou %.0f ms (relogio "
                    "devolveu %.0f ms)\n",
                    frames, rms, rms - 16.0);
      }
    }
    /* The overlay saves and restores the GL state itself.  Do NOT poke the
     * engine's state cache here: ccGLInvalidateStateCache() begins with
     * kmGLFreeAll(), which throws away the projection matrix and leaves every
     * later frame black or flat purple. */
    gd_cursor_draw();
    nx_frameprobe_before_swap();
    gd_gl_swap();
    frames++;
    if ((frames % 300) == 0) {
      struct timespec t1s;
      clock_gettime(CLOCK_MONOTONIC, &t1s);
      double dt = (double)(t1s.tv_sec - t0.tv_sec) +
                  (double)(t1s.tv_nsec - t0.tv_nsec) / 1e9;
      debugPrintf("[fps] %lu frames, %.1f fps\n", frames, 300.0 / dt);
      t0 = t1s;
    }
    /* Persist saves during play, not only at a clean exit: the SharedPreferences
     * model the game assumes (set -> apply) is durable within seconds, so a
     * kill from the frontend must not lose progress. Flushes only when dirty. */
    if ((frames % 180) == 0)
      jni_shim_prefs_flush();
  }

  debugPrintf("=== saindo ===\n");
  if (e_onPause)
    e_onPause(g_env, NULL);
  jni_shim_prefs_flush();
  pad_evdev_close();
  gd_single_instance_unlock();
  fflush(stderr);
  /* Do not tear the Mali context down politely: the Utgard driver can leave the
   * framebuffer black for the frontend if we do. */
  _exit(0);
}
