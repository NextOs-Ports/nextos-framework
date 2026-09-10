/*
 * gd_move.c -- walking left and right in the 2.2 platformer levels.
 *
 * Geometry Dash stopped being a one-button game in 2.2: the Tower's platformer
 * challenges ask the player to walk back and forth, and a touch anywhere on the
 * screen only ever jumps.  On a phone those two directions come from an
 * on-screen D-pad that the port's aiming arrow would have to be parked on --
 * unplayable with a stick.
 *
 * The engine already knows how to walk from a key, because the desktop build
 * shares this code: UILayer::handleKeypress(key, down, time) turns the left and
 * right arrows into GJBaseGameLayer::queueButton(2 or 3), which is the same
 * queue the on-screen buttons feed.  So the port does not simulate anything --
 * it presses the game's own key handler.
 *
 * The one thing missing is WHICH UILayer: handleKeypress is a method and the
 * instance is only reachable through fields whose offsets move between builds
 * (reading offsets out of one binary is exactly what froze the level once
 * already -- see step_guard.c).  So nothing is read by offset here.  The
 * UILayer hands itself over instead: its vtable slot for draw() -- a method the
 * scene graph calls on it every single frame -- is pointed at a stub that notes
 * `this` and jumps to the original.  The slot is FOUND by comparing against the
 * address of UILayer::draw, never by a hardcoded index, so a different build
 * that reorders its virtuals still lands on the right one.
 *
 * "There is a level on screen" is then simply "a UILayer drew recently".  When
 * it stops drawing (level left, menu open again) the keys are considered
 * released and nothing is sent -- the pointer is never used after the object it
 * came from could have died.
 *
 * SubZero is the same 2.2 engine, checked symbol by symbol: same
 * handleKeypress, same queueButton, same key constants and the same button
 * numbers.  So this works there too -- its levels are all classic, so nothing
 * walks, but the jump inside a level goes through the key exactly as here.  A
 * build that does NOT have these symbols simply gets the feature switched off,
 * with the reason in the log, and behaves as it did before.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "gd_move.h"
#include "so_util.h"
#include "util.h"

/* cocos2d enumKeyCodes, the Windows virtual-key values the engine kept.
 *
 * A and D, not the arrows: handleKeypress sends the arrows to queueButton with
 * the second-player flag set (the desktop build gives player 2 the arrows in a
 * dual level) and A/D with it clear.  Both walk in a single-player level, only
 * A/D walk the right character in a dual one.  Space is the jump the desktop
 * build uses, and it is the SAME queue the on-screen buttons feed. */
#define KEY_LEFT 0x41  /* 'A' -- move player 1 left  */
#define KEY_RIGHT 0x44 /* 'D' -- move player 1 right */
#define KEY_JUMP 0x20  /* space */

/* How long after its last draw() a UILayer still counts as on screen.  Two or
 * three frames of slack: enough that a hitch does not drop the key, short
 * enough that the pointer is dropped as soon as the level goes away. */
#define UILAYER_FRESH_MS 250

/* Slots scanned when looking for draw() in the vtable.  UILayer's has 218. */
#define VTABLE_SLOTS 512

static void (*e_ui_draw)(void *self);
static void (*e_handle_keypress)(void *self, int key, int down, double time);
static void *g_ui;
static unsigned g_ui_ms;
static int g_left, g_right, g_jump;

static unsigned now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned)(ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000));
}

static int make_writable(void *addr, size_t len) {
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0)
    page = 4096;
  uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(page - 1);
  uintptr_t end = ((uintptr_t)addr + len + (uintptr_t)page - 1) &
                  ~(uintptr_t)(page - 1);
  return mprotect((void *)start, (size_t)(end - start),
                  PROT_READ | PROT_WRITE | PROT_EXEC);
}

static void gd_ui_draw(void *self) {
  g_ui = self;
  g_ui_ms = now_ms();
  if (e_ui_draw)
    e_ui_draw(self);
}

void gd_move_install(void) {
  uintptr_t vtable = so_find_addr_safe("_ZTV7UILayer");
  uintptr_t draw = so_find_addr_safe("_ZN7UILayer4drawEv");
  e_handle_keypress = (void *)so_find_addr_safe(
      "_ZN7UILayer14handleKeypressEN7cocos2d12enumKeyCodesEbd");
  if (!vtable || !draw || !e_handle_keypress) {
    e_handle_keypress = NULL;
    debugPrintf("[andar] este jogo nao tem UILayer::handleKeypress -- sem "
                "movimento lateral (esperado no SubZero)\n");
    return;
  }

  uintptr_t *slots = (uintptr_t *)vtable;
  int found = -1, extra = 0;
  for (int i = 0; i < VTABLE_SLOTS; i++) {
    if (slots[i] != draw)
      continue;
    if (found < 0)
      found = i;
    else
      extra++;
  }
  if (found < 0) {
    e_handle_keypress = NULL;
    debugPrintf("[andar] draw() nao esta' na vtable de UILayer -- sem "
                "movimento lateral\n");
    return;
  }

  /* The vtable lives in `.data.rel.ro`, inside the module's RW mapping, and the
   * loader never applies RELRO -- so this write is expected to be legal.  It is
   * asked for anyway: a firmware that maps it read-only must turn the feature
   * OFF, never take the game down with a fault on a page that is none of the
   * player's business.  EXEC is kept because the same mapping ends in the
   * trampoline pool. */
  if (make_writable(&slots[found], sizeof(slots[0])) != 0) {
    e_handle_keypress = NULL;
    debugPrintf("[andar] a vtable de UILayer nao aceita escrita neste sistema "
                "-- sem movimento lateral, o resto do jogo segue igual\n");
    return;
  }

  e_ui_draw = (void *)slots[found];
  slots[found] = (uintptr_t)&gd_ui_draw;
  debugPrintf("[andar] movimento lateral ligado (draw na posicao %d da vtable, "
              "%d repetida(s))\n",
              found, extra);
}

int gd_move_live(void) {
  if (!e_handle_keypress || !g_ui)
    return 0;
  return (unsigned)(now_ms() - g_ui_ms) < UILAYER_FRESH_MS;
}

/* Only the edges are sent: handleKeypress feeds a queue that holds the button
 * down until the matching release, exactly like a real key. */
void gd_move_set(int left, int right) {
  if (!gd_move_live()) {
    /* The layer is gone; whatever it had queued went with it. */
    g_left = g_right = 0;
    return;
  }
  left = left ? 1 : 0;
  right = right ? 1 : 0;
  /* Both directions at once cancel out, the way two opposite keys would. */
  if (left && right)
    left = right = 0;
  if (left != g_left) {
    g_left = left;
    e_handle_keypress(g_ui, KEY_LEFT, left, 0.0);
    debugPrintf("[andar] esquerda %s\n", left ? "DOWN" : "UP");
  }
  if (right != g_right) {
    g_right = right;
    e_handle_keypress(g_ui, KEY_RIGHT, right, 0.0);
    debugPrintf("[andar] direita %s\n", right ? "DOWN" : "UP");
  }
}

/* Jumping INSIDE a level goes through the same key handler.
 *
 * It has to, in the platformer levels: the game draws its own left/right arrows
 * over the bottom-left corner there, and the port's aiming arrow taps wherever
 * it was left -- park it over one of those and the jump button presses "walk
 * left" instead.  A key lands on the player, never on a piece of UI.
 *
 * The caller sends the touch OR this, never both: two identical presses reach
 * PlayerObject as two separate pushes, which in a classic level is a second
 * jump nobody asked for. */
void gd_move_jump(int down) {
  if (!gd_move_live()) {
    g_jump = 0;
    return;
  }
  down = down ? 1 : 0;
  if (down == g_jump)
    return;
  g_jump = down;
  e_handle_keypress(g_ui, KEY_JUMP, down, 0.0);
  debugPrintf("[andar] pulo %s (tecla)\n", down ? "DOWN" : "UP");
}
