#include <errno.h>
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_compat.h"
#include "opensl_bridge.h"
#include "pthread_bridge.h"

enum {
  SL_SUCCESS = 0,
  SL_PLAYSTATE_STOPPED = 1,
  SL_PLAYSTATE_PLAYING = 3,
  SL_DATALOCATOR_BUFFERQUEUE = 6,
  SL_DATAFORMAT_PCM = 2,
  SL_BYTEORDER_LITTLEENDIAN = 2,
};

#if defined(ASM2_TEST_WRAP_SDL_AUDIO_INIT)
static int test_audio_init_calls;
static int test_alsa_fallback_seen;

int __real_SDL_InitSubSystem(Uint32 flags);

int __wrap_SDL_InitSubSystem(Uint32 flags) {
  if ((flags & SDL_INIT_AUDIO) == 0)
    return __real_SDL_InitSubSystem(flags);

  ++test_audio_init_calls;
  if (test_audio_init_calls == 1) {
    SDL_SetError("simulated unavailable automatic audio service");
    return -1;
  }

  const char *driver = getenv("SDL_AUDIODRIVER");
  if (driver && strcmp(driver, "alsa") == 0) {
    test_alsa_fallback_seen = 1;
    if (setenv("SDL_AUDIODRIVER", "dummy", 1) != 0)
      return -1;
    const int result = __real_SDL_InitSubSystem(flags);
    if (setenv("SDL_AUDIODRIVER", "alsa", 1) != 0)
      return -1;
    return result;
  }

  return __real_SDL_InitSubSystem(flags);
}
#endif

struct sl_source {
  const void *locator;
  const void *format;
};

struct sl_locator {
  uint32_t type;
  uint32_t buffers;
};

struct sl_pcm {
  uint32_t type;
  uint32_t channels;
  uint32_t rate_millihz;
  uint32_t bits;
  uint32_t container_bits;
  uint32_t channel_mask;
  uint32_t byte_order;
};

struct sl_queue_state {
  uint32_t count;
  uint32_t index;
};

typedef uint32_t (*sl_realize_fn)(void *, uint32_t);
typedef uint32_t (*sl_get_interface_fn)(void *, const void *, void **);
typedef void (*sl_destroy_fn)(void *);
typedef uint32_t (*sl_create_player_fn)(void *, void **,
                                        const struct sl_source *, const void *,
                                        uint32_t, const void *,
                                        const uint32_t *);
typedef uint32_t (*sl_set_state_fn)(void *, uint32_t);
typedef uint32_t (*sl_enqueue_fn)(void *, const void *, uint32_t);
typedef uint32_t (*sl_clear_fn)(void *);
typedef uint32_t (*sl_get_queue_state_fn)(void *, struct sl_queue_state *);
typedef void (*sl_callback_fn)(void *, void *);
typedef uint32_t (*sl_register_callback_fn)(void *, sl_callback_fn, void *);

static const uintptr_t *vtable(void *handle) {
  return handle ? *(const uintptr_t **)handle : NULL;
}

void debugPrintf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

/* The exact 1.2.7d callback-layout resolver is covered by the loader smoke
 * test, and the real guest mutex bridge by pthread_registry_test.  These
 * stubs connect the same Destroy -> handoff -> join -> reacquire sequence to
 * an SDL mutex so this OpenSL test also covers the end-to-end ownership flow. */
static void *test_handoff_context;
static SDL_mutex *test_handoff_mutex;
static SDL_atomic_t test_handoff_begins;
static SDL_atomic_t test_handoff_ends;

void *asm2_audio_compat_callback_mutex(void *callback, void *context) {
  (void)callback;
  return context == test_handoff_context ? test_handoff_mutex : NULL;
}

int asm2_pthread_mutex_handoff_begin(
    void *guest_mutex, struct asm2_pthread_mutex_handoff *handoff) {
  if (!guest_mutex || guest_mutex != test_handoff_mutex || !handoff)
    return 0;
  if (SDL_UnlockMutex(guest_mutex) != 0)
    return -EIO;
  handoff->bridge_object = guest_mutex;
  handoff->active = 1;
  SDL_AtomicIncRef(&test_handoff_begins);
  return 1;
}

int asm2_pthread_mutex_handoff_end(
    struct asm2_pthread_mutex_handoff *handoff) {
  if (!handoff || !handoff->active ||
      handoff->bridge_object != test_handoff_mutex)
    return EINVAL;
  if (SDL_LockMutex(handoff->bridge_object) != 0)
    return EIO;
  handoff->bridge_object = NULL;
  handoff->active = 0;
  SDL_AtomicIncRef(&test_handoff_ends);
  return 0;
}

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s (SDL=%s)\n", message, SDL_GetError());
    exit(1);
  }
}

struct callback_context {
  void *queue;
  SDL_atomic_t calls;
  SDL_atomic_t dry_callbacks_remaining;
  SDL_atomic_t enqueues;
  uint8_t pcm[4096];
};

struct blocking_context {
  SDL_mutex *guest_mutex;
  SDL_atomic_t entered;
  SDL_atomic_t exited;
};

struct self_destroy_context {
  void *player_object;
  SDL_atomic_t entered;
  SDL_atomic_t returned;
};

static void refill_callback(void *queue, void *opaque) {
  struct callback_context *context = opaque;
  SDL_AtomicIncRef(&context->calls);
  if (SDL_AtomicGet(&context->dry_callbacks_remaining) > 0) {
    SDL_AtomicAdd(&context->dry_callbacks_remaining, -1);
    return;
  }

  struct sl_queue_state state = {0, 0};
  const uintptr_t *queue_vtable = vtable(queue);
  if (((sl_get_queue_state_fn)queue_vtable[2])(queue, &state) != SL_SUCCESS)
    return;
  while (state.count < 2) {
    if (((sl_enqueue_fn)queue_vtable[0])(queue, context->pcm,
                                         sizeof(context->pcm)) != SL_SUCCESS)
      return;
    SDL_AtomicIncRef(&context->enqueues);
    ++state.count;
  }
}

static void blocking_callback(void *queue, void *opaque) {
  (void)queue;
  struct blocking_context *context = opaque;
  SDL_AtomicSet(&context->entered, 1);
  SDL_LockMutex(context->guest_mutex);
  SDL_Delay(50);
  SDL_UnlockMutex(context->guest_mutex);
  SDL_AtomicSet(&context->exited, 1);
}

static void self_destroy_callback(void *queue, void *opaque) {
  (void)queue;
  struct self_destroy_context *context = opaque;
  SDL_AtomicSet(&context->entered, 1);
  const uintptr_t *object_vtable = vtable(context->player_object);
  ((sl_destroy_fn)object_vtable[6])(context->player_object);
  SDL_AtomicSet(&context->returned, 1);
}

int main(void) {
#if defined(ASM2_TEST_WRAP_SDL_AUDIO_INIT)
#if defined(ASM2_TEST_INHERITED_PULSEAUDIO)
  require(setenv("SDL_AUDIODRIVER", "pulseaudio", 1) == 0,
          "simulate the inherited ROCKNIX PulseAudio selection");
#else
  require(unsetenv("SDL_AUDIODRIVER") == 0,
          "leave SDL automatic audio selection enabled");
#endif
#else
  require(setenv("SDL_AUDIODRIVER", "dummy", 1) == 0,
          "select SDL dummy audio driver");
#endif

  void *engine_object = NULL;
  require(asm2_slCreateEngine(&engine_object, 0, NULL, 0, NULL, NULL) ==
              SL_SUCCESS &&
              engine_object,
          "create engine");
  const uintptr_t *engine_object_vtable = vtable(engine_object);
  require(((sl_realize_fn)engine_object_vtable[0])(engine_object, 0) ==
              SL_SUCCESS,
          "realize engine");
  void *engine = NULL;
  require(((sl_get_interface_fn)engine_object_vtable[3])(
              engine_object, asm2_sl_iid_engine, &engine) == SL_SUCCESS &&
              engine,
          "get engine interface");

  const struct sl_locator locator = {SL_DATALOCATOR_BUFFERQUEUE, 2};
  const struct sl_pcm pcm = {SL_DATAFORMAT_PCM, 2, 32000000u, 16, 16, 0,
                             SL_BYTEORDER_LITTLEENDIAN};
  const struct sl_source source = {&locator, &pcm};
  const void *interfaces[] = {asm2_sl_iid_bufferqueue};
  const uint32_t required[] = {1};
  void *player_object = NULL;
  const uintptr_t *engine_vtable = vtable(engine);
  require(((sl_create_player_fn)engine_vtable[2])(
              engine, &player_object, &source, NULL, 1, interfaces,
              required) == SL_SUCCESS &&
              player_object,
          "create player");
  const uintptr_t *player_object_vtable = vtable(player_object);
  require(((sl_realize_fn)player_object_vtable[0])(player_object, 0) ==
              SL_SUCCESS,
          "realize player");

  void *play = NULL;
  void *queue = NULL;
  require(((sl_get_interface_fn)player_object_vtable[3])(
              player_object, asm2_sl_iid_play, &play) == SL_SUCCESS &&
              play,
          "get play interface");
  require(((sl_get_interface_fn)player_object_vtable[3])(
              player_object, asm2_sl_iid_bufferqueue, &queue) == SL_SUCCESS &&
              queue,
          "get queue interface");

  struct callback_context context;
  memset(&context, 0, sizeof(context));
  context.queue = queue;
  SDL_AtomicSet(&context.dry_callbacks_remaining, 2);
  for (size_t index = 0; index < sizeof(context.pcm); index += 2) {
    context.pcm[index] = 0x00;
    context.pcm[index + 1] = 0x10;
  }

  const uintptr_t *queue_vtable = vtable(queue);
  require(((sl_register_callback_fn)queue_vtable[3])(
              queue, refill_callback, &context) == SL_SUCCESS,
          "register refill callback");
  require(((sl_enqueue_fn)queue_vtable[0])(queue, context.pcm,
                                            sizeof(context.pcm)) == SL_SUCCESS,
          "enqueue initial buffer one");
  require(((sl_enqueue_fn)queue_vtable[0])(queue, context.pcm,
                                            sizeof(context.pcm)) == SL_SUCCESS,
          "enqueue initial buffer two");
  require(((sl_set_state_fn)vtable(play)[0])(play, SL_PLAYSTATE_PLAYING) ==
              SL_SUCCESS,
          "start playback");

  SDL_Delay(700);
  require(SDL_AtomicGet(&context.calls) >= 4,
          "worker polled and recovered after two dry callbacks");
  require(SDL_AtomicGet(&context.enqueues) >= 2,
          "recovery callback refilled the dry queue");

  int calls_before_clear = SDL_AtomicGet(&context.calls);
  require(((sl_clear_fn)queue_vtable[1])(queue) == SL_SUCCESS,
          "clear queue while playing");
  SDL_Delay(250);
  require(SDL_AtomicGet(&context.calls) > calls_before_clear,
          "playing empty queue recovered after Clear");

  require(((sl_set_state_fn)vtable(play)[0])(play, SL_PLAYSTATE_STOPPED) ==
              SL_SUCCESS,
          "stop playback");
  require(((sl_clear_fn)queue_vtable[1])(queue) == SL_SUCCESS,
          "clear stopped queue");
  SDL_Delay(100);
  const int calls_while_stopped = SDL_AtomicGet(&context.calls);
  SDL_Delay(180);
  require(SDL_AtomicGet(&context.calls) == calls_while_stopped,
          "stopped player does not poll callbacks");
  require(((sl_set_state_fn)vtable(play)[0])(play, SL_PLAYSTATE_PLAYING) ==
              SL_SUCCESS,
          "restart empty playback");
  SDL_Delay(250);
  require(SDL_AtomicGet(&context.calls) > calls_while_stopped,
          "SetPlayState PLAYING wakes recovery worker");

  require(((sl_set_state_fn)vtable(play)[0])(play, SL_PLAYSTATE_STOPPED) ==
              SL_SUCCESS,
          "stop before destroy");
  ((sl_destroy_fn)player_object_vtable[6])(player_object);

  /* Reproduce the exact teardown shape: the guest thread owns its mixer mutex
   * while a callback is blocked on it, then calls Destroy.  Destroy must hand
   * off that mutex, join the callback worker and reacquire it before return. */
  void *blocked_player_object = NULL;
  require(((sl_create_player_fn)engine_vtable[2])(
              engine, &blocked_player_object, &source, NULL, 1, interfaces,
              required) == SL_SUCCESS &&
              blocked_player_object,
          "create player for teardown race");
  const uintptr_t *blocked_object_vtable = vtable(blocked_player_object);
  require(((sl_realize_fn)blocked_object_vtable[0])(blocked_player_object, 0) ==
              SL_SUCCESS,
          "realize player for teardown race");
  void *blocked_play = NULL;
  void *blocked_queue = NULL;
  require(((sl_get_interface_fn)blocked_object_vtable[3])(
              blocked_player_object, asm2_sl_iid_play, &blocked_play) ==
              SL_SUCCESS,
          "get blocked play interface");
  require(((sl_get_interface_fn)blocked_object_vtable[3])(
              blocked_player_object, asm2_sl_iid_bufferqueue,
              &blocked_queue) == SL_SUCCESS,
          "get blocked queue interface");
  struct blocking_context blocked;
  memset(&blocked, 0, sizeof(blocked));
  blocked.guest_mutex = SDL_CreateMutex();
  require(blocked.guest_mutex != NULL, "create simulated guest mutex");
  SDL_LockMutex(blocked.guest_mutex);
  const uintptr_t *blocked_queue_vtable = vtable(blocked_queue);
  require(((sl_register_callback_fn)blocked_queue_vtable[3])(
              blocked_queue, blocking_callback, &blocked) == SL_SUCCESS,
          "register blocking callback");
  require(((sl_enqueue_fn)blocked_queue_vtable[0])(
              blocked_queue, context.pcm, sizeof(context.pcm)) == SL_SUCCESS,
          "enqueue teardown-race buffer");
  require(((sl_set_state_fn)vtable(blocked_play)[0])(
              blocked_play, SL_PLAYSTATE_PLAYING) == SL_SUCCESS,
          "start teardown-race player");
  uint32_t wait_start = SDL_GetTicks();
  while (!SDL_AtomicGet(&blocked.entered) &&
         (uint32_t)(SDL_GetTicks() - wait_start) < 1000u)
    SDL_Delay(1);
  require(SDL_AtomicGet(&blocked.entered),
          "callback reached simulated guest mutex");
  require(((sl_set_state_fn)vtable(blocked_play)[0])(
              blocked_play, SL_PLAYSTATE_STOPPED) == SL_SUCCESS,
          "stop teardown-race player");
  test_handoff_context = &blocked;
  test_handoff_mutex = blocked.guest_mutex;
  ((sl_destroy_fn)blocked_object_vtable[6])(blocked_player_object);
  require(SDL_AtomicGet(&blocked.exited),
          "Destroy did not return before callback context became idle");
  require(SDL_AtomicGet(&test_handoff_begins) == 1 &&
              SDL_AtomicGet(&test_handoff_ends) == 1,
          "Destroy completed one mutex handoff and reacquire");
  require(SDL_UnlockMutex(blocked.guest_mutex) == 0,
          "Destroy returned with the guest mutex reacquired");
  test_handoff_context = NULL;
  test_handoff_mutex = NULL;
  SDL_DestroyMutex(blocked.guest_mutex);

  /* A callback which destroys its own player cannot join itself.  This is the
   * only teardown path intentionally deferred to worker exit. */
  void *self_player_object = NULL;
  require(((sl_create_player_fn)engine_vtable[2])(
              engine, &self_player_object, &source, NULL, 1, interfaces,
              required) == SL_SUCCESS &&
              self_player_object,
          "create self-destroy player");
  const uintptr_t *self_object_vtable = vtable(self_player_object);
  require(((sl_realize_fn)self_object_vtable[0])(self_player_object, 0) ==
              SL_SUCCESS,
          "realize self-destroy player");
  void *self_play = NULL;
  void *self_queue = NULL;
  require(((sl_get_interface_fn)self_object_vtable[3])(
              self_player_object, asm2_sl_iid_play, &self_play) == SL_SUCCESS,
          "get self-destroy play interface");
  require(((sl_get_interface_fn)self_object_vtable[3])(
              self_player_object, asm2_sl_iid_bufferqueue,
              &self_queue) == SL_SUCCESS,
          "get self-destroy queue interface");
  struct self_destroy_context self_destroy;
  memset(&self_destroy, 0, sizeof(self_destroy));
  self_destroy.player_object = self_player_object;
  const uintptr_t *self_queue_vtable = vtable(self_queue);
  require(((sl_register_callback_fn)self_queue_vtable[3])(
              self_queue, self_destroy_callback, &self_destroy) == SL_SUCCESS,
          "register self-destroy callback");
  require(((sl_enqueue_fn)self_queue_vtable[0])(
              self_queue, context.pcm, sizeof(context.pcm)) == SL_SUCCESS,
          "enqueue self-destroy buffer");
  require(((sl_set_state_fn)vtable(self_play)[0])(
              self_play, SL_PLAYSTATE_PLAYING) == SL_SUCCESS,
          "start self-destroy player");
  wait_start = SDL_GetTicks();
  while (!SDL_AtomicGet(&self_destroy.returned) &&
         (uint32_t)(SDL_GetTicks() - wait_start) < 1000u)
    SDL_Delay(1);
  require(SDL_AtomicGet(&self_destroy.entered) &&
              SDL_AtomicGet(&self_destroy.returned),
          "self-destroy callback returned without joining itself");

  struct asm2_opensl_stats stats;
  wait_start = SDL_GetTicks();
  do {
    asm2_opensl_get_stats(&stats);
    if (stats.players_released == 3 && stats.workers_exited == 3)
      break;
    SDL_Delay(1);
  } while ((uint32_t)(SDL_GetTicks() - wait_start) < 1000u);
  require(stats.players_created == 3 && stats.players_released == 3 &&
              stats.workers_started == 3 && stats.workers_exited == 3 &&
              stats.self_deferred_destroys == 1,
          "all players and callback workers were released exactly once");

#if defined(ASM2_TEST_WRAP_SDL_AUDIO_INIT)
  require(test_alsa_fallback_seen &&
              strcmp(getenv("SDL_AUDIODRIVER"), "alsa") == 0,
          "automatic audio failure retried through ALSA exactly as scoped");
#endif

  ((sl_destroy_fn)engine_object_vtable[6])(engine_object);
  puts("OpenSL recovery: starvation, lifecycle and teardown ownership OK");
  return 0;
}
