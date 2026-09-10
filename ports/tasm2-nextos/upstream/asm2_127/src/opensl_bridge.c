#include "opensl_bridge.h"

#include <SDL2/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_compat.h"
#include "pthread_bridge.h"
#include "util.h"

/* Minimal OpenSL ES 1.0 compatibility layer for the interfaces imported by
 * TASM2 1.2.7d.  OpenSL C handles are deliberately unusual: an interface is
 * a pointer to a pointer to its vtable.  asm2_sl_interface keeps the vtable as
 * its first word, so the address of that field is the exact guest handle. */

enum {
  ASM2_SL_RESULT_SUCCESS = 0,
  ASM2_SL_RESULT_PRECONDITIONS_VIOLATED = 1,
  ASM2_SL_RESULT_PARAMETER_INVALID = 2,
  ASM2_SL_RESULT_MEMORY_FAILURE = 3,
  ASM2_SL_RESULT_RESOURCE_ERROR = 4,
  ASM2_SL_RESULT_BUFFER_INSUFFICIENT = 7,
  ASM2_SL_RESULT_FEATURE_UNSUPPORTED = 12,
};

enum {
  ASM2_SL_OBJECT_STATE_UNREALIZED = 1,
  ASM2_SL_OBJECT_STATE_REALIZED = 2,
  ASM2_SL_OBJECT_STATE_SUSPENDED = 3,
};

enum {
  ASM2_SL_PLAYSTATE_STOPPED = 1,
  ASM2_SL_PLAYSTATE_PAUSED = 2,
  ASM2_SL_PLAYSTATE_PLAYING = 3,
};

enum {
  ASM2_SL_DATALOCATOR_BUFFERQUEUE = 6,
  ASM2_SL_DATAFORMAT_PCM = 2,
  ASM2_SL_BYTEORDER_LITTLEENDIAN = 2,
};

enum asm2_sl_object_kind {
  ASM2_SL_OBJECT_ENGINE,
  ASM2_SL_OBJECT_OUTPUT_MIX,
  ASM2_SL_OBJECT_PLAYER,
};

struct asm2_sl_interface_id {
  uint32_t words[4];
};

static const struct asm2_sl_interface_id asm2_iid_engine_token = {
    {0x454e474eu, 0x494e452du, 0x41534d32u, 0x31323744u}};
static const struct asm2_sl_interface_id asm2_iid_bufferqueue_token = {
    {0x42554646u, 0x4552512du, 0x41534d32u, 0x31323744u}};
static const struct asm2_sl_interface_id asm2_iid_play_token = {
    {0x504c4159u, 0x2d494944u, 0x41534d32u, 0x31323744u}};

const void *asm2_sl_iid_engine = &asm2_iid_engine_token;
const void *asm2_sl_iid_bufferqueue = &asm2_iid_bufferqueue_token;
const void *asm2_sl_iid_play = &asm2_iid_play_token;

struct asm2_sl_interface {
  const uintptr_t *vtable;
  void *owner;
};

struct asm2_sl_object {
  struct asm2_sl_interface interface;
  enum asm2_sl_object_kind kind;
  uint32_t state;
};

struct asm2_sl_engine {
  struct asm2_sl_object object;
  struct asm2_sl_interface engine;
};

struct asm2_sl_output_mix {
  struct asm2_sl_object object;
};

struct asm2_sl_audio_buffer {
  struct asm2_sl_audio_buffer *next;
  uint8_t *data;
  uint32_t size;
  uint32_t offset;
  uint32_t capacity;
};

typedef void(ASM2_GUEST_PCS *asm2_sl_buffer_callback)(void *queue,
                                                       void *context);

struct asm2_sl_player {
  struct asm2_sl_object object;
  struct asm2_sl_interface play;
  struct asm2_sl_interface buffer_queue;

  SDL_AudioDeviceID device;
  SDL_mutex *mutex;
  SDL_cond *callback_condition;
  SDL_Thread *callback_thread;
  SDL_threadID callback_thread_id;

  struct asm2_sl_audio_buffer *head;
  struct asm2_sl_audio_buffer *tail;
  struct asm2_sl_audio_buffer *free_buffers;
  uint32_t queued_count;
  uint32_t queue_capacity;
  uint32_t play_index;
  uint64_t played_frames;

  uint32_t frequency;
  uint32_t channels;
  uint32_t bytes_per_frame;
  uint32_t play_state;
  uint32_t pending_callbacks;
  uint32_t pending_recovery_callbacks;
  uint32_t callback_in_flight;
  uint32_t callback_generation;
  uint32_t in_flight_generation;
  int shutdown;
  int destroy_on_worker_exit;
  int destroy_resources_closed;
  int audio_subsystem_acquired;

  asm2_sl_buffer_callback callback;
  void *callback_context;
  asm2_sl_buffer_callback in_flight_callback;
  void *in_flight_context;

  uint64_t enqueue_serial;
  uint64_t enqueues;
  uint64_t completed_buffers;
  uint64_t callbacks;
  uint64_t callbacks_without_enqueue;
  uint64_t recovery_callbacks;
  uint64_t stale_enqueues;
  uint64_t enqueue_failures;
  uint64_t underruns;
  uint64_t underrun_bytes;
  uint32_t peak_pcm;
  uint32_t last_report_ticks;
};

struct asm2_sl_data_source {
  const void *locator;
  const void *format;
};

struct asm2_sl_buffer_queue_locator {
  uint32_t locator_type;
  uint32_t num_buffers;
};

struct asm2_sl_pcm_format {
  uint32_t format_type;
  uint32_t num_channels;
  uint32_t samples_per_sec;
  uint32_t bits_per_sample;
  uint32_t container_size;
  uint32_t channel_mask;
  uint32_t endianness;
};

struct asm2_sl_buffer_queue_state {
  uint32_t count;
  uint32_t index;
};

static const uintptr_t asm2_sl_object_vtable[10];
static const uintptr_t asm2_sl_engine_vtable[15];
static const uintptr_t asm2_sl_play_vtable[12];
static const uintptr_t asm2_sl_buffer_queue_vtable[4];
static uint64_t asm2_sl_players_created;
static uint64_t asm2_sl_players_released;
static uint64_t asm2_sl_workers_started;
static uint64_t asm2_sl_workers_exited;
static uint64_t asm2_sl_self_deferred_destroys;

void asm2_opensl_get_stats(struct asm2_opensl_stats *stats) {
  if (!stats)
    return;
  stats->players_created =
      __atomic_load_n(&asm2_sl_players_created, __ATOMIC_RELAXED);
  stats->players_released =
      __atomic_load_n(&asm2_sl_players_released, __ATOMIC_RELAXED);
  stats->workers_started =
      __atomic_load_n(&asm2_sl_workers_started, __ATOMIC_RELAXED);
  stats->workers_exited =
      __atomic_load_n(&asm2_sl_workers_exited, __ATOMIC_RELAXED);
  stats->self_deferred_destroys =
      __atomic_load_n(&asm2_sl_self_deferred_destroys, __ATOMIC_RELAXED);
}

static void *asm2_sl_owner(void *self) {
  return self ? ((struct asm2_sl_interface *)self)->owner : NULL;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_unsupported(void *self) {
  (void)self;
  return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_false(void *self) {
  (void)self;
  return 0;
}

static void asm2_sl_destroy_buffer_list(struct asm2_sl_audio_buffer *buffer) {
  while (buffer) {
    struct asm2_sl_audio_buffer *next = buffer->next;
    free(buffer->data);
    free(buffer);
    buffer = next;
  }
}

/* Completed buffers stay owned by the player and are reused by Enqueue().
 * The SDL callback must not enter malloc/free while the device is pulling
 * audio, especially on the memory-constrained Mali-450 target. */
static void asm2_sl_recycle_buffer_locked(
    struct asm2_sl_player *player, struct asm2_sl_audio_buffer *buffer) {
  if (!buffer)
    return;
  buffer->size = 0;
  buffer->offset = 0;
  buffer->next = player->free_buffers;
  player->free_buffers = buffer;
}

static void asm2_sl_clear_queued_buffers_locked(
    struct asm2_sl_player *player) {
  struct asm2_sl_audio_buffer *buffer = player->head;
  while (buffer) {
    struct asm2_sl_audio_buffer *next = buffer->next;
    asm2_sl_recycle_buffer_locked(player, buffer);
    buffer = next;
  }
  player->head = NULL;
  player->tail = NULL;
  player->queued_count = 0;
}

static void asm2_sl_release_player(struct asm2_sl_player *player) {
  if (!player)
    return;
  asm2_sl_destroy_buffer_list(player->head);
  asm2_sl_destroy_buffer_list(player->free_buffers);
  if (player->callback_condition)
    SDL_DestroyCond(player->callback_condition);
  if (player->mutex)
    SDL_DestroyMutex(player->mutex);
  if (player->audio_subsystem_acquired)
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  __atomic_add_fetch(&asm2_sl_players_released, 1u, __ATOMIC_RELAXED);
  free(player);
}

static int asm2_sl_recovery_ready_locked(
    const struct asm2_sl_player *player) {
  return !player->shutdown &&
         player->play_state == ASM2_SL_PLAYSTATE_PLAYING &&
         player->callback && !player->head && player->queued_count == 0 &&
         player->pending_callbacks == 0 && !player->callback_in_flight;
}

static void asm2_sl_maybe_report(struct asm2_sl_player *player) {
  const uint32_t now = SDL_GetTicks();
  uint32_t queued;
  uint32_t pending;
  uint32_t in_flight;
  uint32_t state;
  uint32_t peak;
  uint64_t enqueues;
  uint64_t completed;
  uint64_t callbacks;
  uint64_t dry_callbacks;
  uint64_t recoveries;
  uint64_t underruns;
  uint64_t underrun_bytes;
  uint64_t failures;
  uint64_t stale;
  SDL_AudioDeviceID device;

  SDL_LockMutex(player->mutex);
  if ((uint32_t)(now - player->last_report_ticks) < 5000u) {
    SDL_UnlockMutex(player->mutex);
    return;
  }
  player->last_report_ticks = now;
  queued = player->queued_count;
  pending = player->pending_callbacks;
  in_flight = player->callback_in_flight;
  state = player->play_state;
  peak = player->peak_pcm;
  player->peak_pcm = 0;
  enqueues = player->enqueues;
  completed = player->completed_buffers;
  callbacks = player->callbacks;
  dry_callbacks = player->callbacks_without_enqueue;
  recoveries = player->recovery_callbacks;
  underruns = player->underruns;
  underrun_bytes = player->underrun_bytes;
  failures = player->enqueue_failures;
  stale = player->stale_enqueues;
  device = player->device;
  SDL_UnlockMutex(player->mutex);

  debugPrintf("ASM2_OPENSL_STATE device=%u status=%d state=%u queue=%u/%u "
              "pending=%u inflight=%u enqueues=%llu completed=%llu "
              "callbacks=%llu dry=%llu recoveries=%llu underruns=%llu "
              "missing_bytes=%llu failures=%llu stale=%llu peak=%u\n",
              (unsigned)device,
              (int)SDL_GetAudioDeviceStatus(device), state, queued,
              player->queue_capacity, pending, in_flight,
              (unsigned long long)enqueues,
              (unsigned long long)completed,
              (unsigned long long)callbacks,
              (unsigned long long)dry_callbacks,
              (unsigned long long)recoveries,
              (unsigned long long)underruns,
              (unsigned long long)underrun_bytes,
              (unsigned long long)failures, (unsigned long long)stale, peak);
}

static int asm2_sl_callback_worker(void *argument) {
  struct asm2_sl_player *player = argument;
  __atomic_add_fetch(&asm2_sl_workers_started, 1u, __ATOMIC_RELAXED);
  SDL_LockMutex(player->mutex);
  player->callback_thread_id = SDL_ThreadID();
  SDL_CondBroadcast(player->callback_condition);
  SDL_UnlockMutex(player->mutex);
  if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) != 0)
    debugPrintf("ASM2_OPENSL worker priority fallback: %s\n", SDL_GetError());

  for (;;) {
    asm2_sl_buffer_callback callback = NULL;
    void *context = NULL;
    uint32_t generation = 0;
    uint64_t enqueue_before = 0;

    SDL_LockMutex(player->mutex);
    while (!player->shutdown && player->pending_callbacks == 0) {
      /* A completion callback which produces no buffer used to leave this
       * player in an absorbing silent state.  Poll a dry PLAYING queue at a
       * conservative 50 ms cadence, outside the SDL audio thread. */
      SDL_CondWaitTimeout(player->callback_condition, player->mutex, 50);
      if (asm2_sl_recovery_ready_locked(player)) {
        player->pending_callbacks = 1;
        player->pending_recovery_callbacks = 1;
      }
    }
    if (!player->shutdown && player->pending_callbacks != 0) {
      --player->pending_callbacks;
      if (player->pending_recovery_callbacks != 0) {
        --player->pending_recovery_callbacks;
        ++player->recovery_callbacks;
      }
      callback = player->callback;
      context = player->callback_context;
      if (callback) {
        generation = player->callback_generation;
        enqueue_before = player->enqueue_serial;
        player->callback_in_flight = 1;
        player->in_flight_generation = generation;
        player->in_flight_callback = callback;
        player->in_flight_context = context;
      }
    }
    const int shutdown = player->shutdown;
    SDL_UnlockMutex(player->mutex);

    if (shutdown)
      break;

    /* Never hold the queue mutex while entering guest code.  The normal
     * OpenSL callback immediately calls Enqueue(), so invoking it under the
     * mutex would deadlock deterministically. */
    if (callback) {
      callback(&player->buffer_queue, context);

      SDL_LockMutex(player->mutex);
      ++player->callbacks;
      if (player->enqueue_serial == enqueue_before)
        ++player->callbacks_without_enqueue;
      player->callback_in_flight = 0;
      player->in_flight_generation = 0;
      player->in_flight_callback = NULL;
      player->in_flight_context = NULL;
      SDL_CondBroadcast(player->callback_condition);
      SDL_UnlockMutex(player->mutex);
      asm2_sl_maybe_report(player);
    }
  }

  const int release_on_exit = player->destroy_on_worker_exit;
  if (release_on_exit) {
    SDL_LockMutex(player->mutex);
    while (!player->destroy_resources_closed)
      SDL_CondWait(player->callback_condition, player->mutex);
    SDL_UnlockMutex(player->mutex);
  }
  __atomic_add_fetch(&asm2_sl_workers_exited, 1u, __ATOMIC_RELAXED);
  if (release_on_exit)
    asm2_sl_release_player(player);
  return 0;
}

static void asm2_sl_sdl_audio_callback(void *userdata, Uint8 *stream,
                                       int length) {
  struct asm2_sl_player *player = userdata;
  memset(stream, 0, (size_t)length);

  SDL_LockMutex(player->mutex);
  if (player->play_state != ASM2_SL_PLAYSTATE_PLAYING || player->shutdown) {
    SDL_UnlockMutex(player->mutex);
    return;
  }

  uint8_t *output = stream;
  uint32_t remaining = (uint32_t)length;
  while (remaining && player->head) {
    struct asm2_sl_audio_buffer *buffer = player->head;
    uint32_t available = buffer->size - buffer->offset;
    uint32_t count = available < remaining ? available : remaining;
    memcpy(output, buffer->data + buffer->offset, count);
    buffer->offset += count;
    output += count;
    remaining -= count;
    player->played_frames += count / player->bytes_per_frame;

    if (buffer->offset == buffer->size) {
      player->head = buffer->next;
      if (!player->head)
        player->tail = NULL;
      if (player->queued_count)
        --player->queued_count;
      ++player->play_index;
      ++player->pending_callbacks;
      ++player->completed_buffers;
      asm2_sl_recycle_buffer_locked(player, buffer);
    }
  }

  if (remaining != 0) {
    ++player->underruns;
    player->underrun_bytes += remaining;
  }
  if (player->pending_callbacks)
    SDL_CondSignal(player->callback_condition);
  SDL_UnlockMutex(player->mutex);
}

static int asm2_sl_initialize_audio_subsystem(void) {
#if defined(ASM2_ARMHF_AUDIO_ALSA_FALLBACK)
  const char *requested_driver = getenv("SDL_AUDIODRIVER");
  char initial_error[256];
  char primary_driver[32];
#endif

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
    return 0;

#if !defined(ASM2_ARMHF_AUDIO_ALSA_FALLBACK)
  debugPrintf("ASM2_OPENSL SDL audio init failed: %s\n", SDL_GetError());
  return -1;
#else
  const char *error = SDL_GetError();
  if (!error)
    error = "unknown SDL audio initialization error";
  strncpy(initial_error, error, sizeof(initial_error) - 1);
  initial_error[sizeof(initial_error) - 1] = '\0';

  if (requested_driver && requested_driver[0]) {
    strncpy(primary_driver, requested_driver, sizeof(primary_driver) - 1);
    primary_driver[sizeof(primary_driver) - 1] = '\0';
  } else {
    strcpy(primary_driver, "automatic");
  }

  /* Preserve explicit diagnostic choices except for the inherited
   * PulseAudio selection observed on ROCKNIX.  That firmware-selected server
   * can be unavailable to its ARMHF process even though its ALSA route and
   * 32-bit modules are usable. */
  if (requested_driver && requested_driver[0] &&
      strcmp(requested_driver, "pulse") != 0 &&
      strcmp(requested_driver, "pulseaudio") != 0) {
    debugPrintf("ASM2_OPENSL SDL audio init failed driver=%s: %s\n",
                requested_driver, initial_error);
    return -1;
  }

  if (setenv("SDL_AUDIODRIVER", "alsa", 1) != 0) {
    debugPrintf("ASM2_OPENSL SDL audio primary init failed driver=%s: %s; "
                "could not enable ALSA fallback\n",
                primary_driver, initial_error);
    return -1;
  }

  SDL_ClearError();
  debugPrintf("ASM2_OPENSL SDL audio primary init failed driver=%s: %s; "
              "retrying driver=alsa\n",
              primary_driver, initial_error);
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    char fallback_error[256];

    error = SDL_GetError();
    if (!error)
      error = "unknown SDL ALSA initialization error";
    strncpy(fallback_error, error, sizeof(fallback_error) - 1);
    fallback_error[sizeof(fallback_error) - 1] = '\0';
    unsetenv("SDL_AUDIODRIVER");
    debugPrintf("ASM2_OPENSL audio fallback failed driver=alsa: %s "
                "(primary driver=%s: %s)\n",
                fallback_error, primary_driver, initial_error);
    return -1;
  }

  debugPrintf("ASM2_OPENSL audio fallback ready driver=%s\n",
              SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                          : "alsa");
  return 0;
#endif
}

static uint32_t asm2_sl_open_player(struct asm2_sl_player *player) {
  if (asm2_sl_initialize_audio_subsystem() != 0) {
    return ASM2_SL_RESULT_RESOURCE_ERROR;
  }
  player->audio_subsystem_acquired = 1;

  player->mutex = SDL_CreateMutex();
  player->callback_condition = SDL_CreateCond();
  if (!player->mutex || !player->callback_condition) {
    debugPrintf("ASM2_OPENSL synchronization allocation failed: %s\n",
                SDL_GetError());
    if (player->callback_condition)
      SDL_DestroyCond(player->callback_condition);
    if (player->mutex)
      SDL_DestroyMutex(player->mutex);
    player->callback_condition = NULL;
    player->mutex = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    player->audio_subsystem_acquired = 0;
    return ASM2_SL_RESULT_MEMORY_FAILURE;
  }

  player->callback_thread =
      SDL_CreateThread(asm2_sl_callback_worker, "asm2-opensl", player);
  if (!player->callback_thread) {
    debugPrintf("ASM2_OPENSL callback thread failed: %s\n", SDL_GetError());
    SDL_DestroyCond(player->callback_condition);
    SDL_DestroyMutex(player->mutex);
    player->callback_condition = NULL;
    player->mutex = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    player->audio_subsystem_acquired = 0;
    return ASM2_SL_RESULT_RESOURCE_ERROR;
  }

  SDL_AudioSpec desired;
  SDL_AudioSpec obtained;
  SDL_zero(desired);
  SDL_zero(obtained);
  desired.freq = (int)player->frequency;
  desired.format = AUDIO_S16LSB;
  desired.channels = (Uint8)player->channels;
  desired.samples = 1024;
  desired.callback = asm2_sl_sdl_audio_callback;
  desired.userdata = player;
  player->device =
      SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
  if (!player->device) {
    debugPrintf("ASM2_OPENSL open %u Hz/%u channel S16LE failed: %s\n",
                player->frequency, player->channels, SDL_GetError());
    SDL_LockMutex(player->mutex);
    player->shutdown = 1;
    SDL_CondSignal(player->callback_condition);
    SDL_UnlockMutex(player->mutex);
    SDL_WaitThread(player->callback_thread, NULL);
    player->callback_thread = NULL;
    SDL_DestroyCond(player->callback_condition);
    SDL_DestroyMutex(player->mutex);
    player->callback_condition = NULL;
    player->mutex = NULL;
    player->shutdown = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    player->audio_subsystem_acquired = 0;
    return ASM2_SL_RESULT_RESOURCE_ERROR;
  }

  debugPrintf("ASM2_OPENSL ready driver=%s device=%s format=%d/%u/S16LE "
              "buffers=%u\n",
              SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
              SDL_GetAudioDeviceName(0, 0) ? SDL_GetAudioDeviceName(0, 0)
                                           : "default",
              obtained.freq, obtained.channels, player->queue_capacity);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_realize(void *self,
                                                       uint32_t async) {
  struct asm2_sl_object *object = asm2_sl_owner(self);
  if (!object)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (async)
    return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
  if (object->state == ASM2_SL_OBJECT_STATE_REALIZED)
    return ASM2_SL_RESULT_SUCCESS;

  if (object->kind == ASM2_SL_OBJECT_PLAYER) {
    uint32_t result = asm2_sl_open_player((struct asm2_sl_player *)object);
    if (result != ASM2_SL_RESULT_SUCCESS)
      return result;
  }
  object->state = ASM2_SL_OBJECT_STATE_REALIZED;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_resume(void *self,
                                                      uint32_t async) {
  struct asm2_sl_object *object = asm2_sl_owner(self);
  if (!object)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (async)
    return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
  return object->state == ASM2_SL_OBJECT_STATE_SUSPENDED
             ? ASM2_SL_RESULT_FEATURE_UNSUPPORTED
             : ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_get_state(void *self,
                                                         uint32_t *state) {
  struct asm2_sl_object *object = asm2_sl_owner(self);
  if (!object || !state)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *state = object->state;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_get_interface(
    void *self, const void *interface_id, void **output) {
  struct asm2_sl_object *object = asm2_sl_owner(self);
  if (!object || !interface_id || !output)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *output = NULL;

  if (object->kind == ASM2_SL_OBJECT_ENGINE &&
      interface_id == asm2_sl_iid_engine) {
    struct asm2_sl_engine *engine = (struct asm2_sl_engine *)object;
    *output = &engine->engine;
  } else if (object->kind == ASM2_SL_OBJECT_PLAYER &&
             interface_id == asm2_sl_iid_play) {
    struct asm2_sl_player *player = (struct asm2_sl_player *)object;
    *output = &player->play;
  } else if (object->kind == ASM2_SL_OBJECT_PLAYER &&
             interface_id == asm2_sl_iid_bufferqueue) {
    struct asm2_sl_player *player = (struct asm2_sl_player *)object;
    *output = &player->buffer_queue;
  } else {
    return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_register_callback(
    void *self, void *callback, void *context) {
  (void)self;
  (void)callback;
  (void)context;
  return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
}

static void ASM2_GUEST_PCS asm2_sl_object_abort(void *self) {
  (void)self;
}

static void ASM2_GUEST_PCS asm2_sl_object_destroy(void *self) {
  struct asm2_sl_object *object = asm2_sl_owner(self);
  if (!object)
    return;

  if (object->kind != ASM2_SL_OBJECT_PLAYER) {
    free(object);
    return;
  }

  struct asm2_sl_player *player = (struct asm2_sl_player *)object;
  if (!player->mutex) {
    asm2_sl_release_player(player);
    return;
  }
  struct asm2_pthread_mutex_handoff handoff;
  memset(&handoff, 0, sizeof(handoff));
  SDL_LockMutex(player->mutex);
  asm2_sl_buffer_callback teardown_callback = player->in_flight_callback;
  void *teardown_context = player->in_flight_context;
  const uint32_t callback_in_flight = player->callback_in_flight;
  player->shutdown = 1;
  player->play_state = ASM2_SL_PLAYSTATE_STOPPED;
  ++player->callback_generation;
  player->callback = NULL;
  player->callback_context = NULL;
  player->pending_callbacks = 0;
  player->pending_recovery_callbacks = 0;
  const int deferred_destroy =
      player->callback_thread &&
      player->callback_thread_id == SDL_ThreadID();
  SDL_AudioDeviceID device = player->device;
  player->device = 0;
  if (deferred_destroy)
    player->destroy_on_worker_exit = 1;
  SDL_CondBroadcast(player->callback_condition);
  SDL_UnlockMutex(player->mutex);

  int handoff_result = 0;
  if (!deferred_destroy && callback_in_flight) {
    void *guest_mutex = asm2_audio_compat_callback_mutex(
        (void *)teardown_callback, teardown_context);
    if (guest_mutex) {
      handoff_result =
          asm2_pthread_mutex_handoff_begin(guest_mutex, &handoff);
      if (handoff_result < 0)
        debugPrintf("ASM2_OPENSL teardown handoff failed error=%d\n",
                    -handoff_result);
      else if (handoff_result == 0)
        debugPrintf("ASM2_OPENSL teardown handoff not owned by caller\n");
    } else {
      debugPrintf("ASM2_OPENSL teardown callback layout did not match\n");
    }
  }

  if (device) {
    SDL_PauseAudioDevice(device, 1);
    SDL_CloseAudioDevice(device);
  }

  if (deferred_destroy) {
    /* A callback may legally destroy its own player.  It cannot join itself;
     * keep the bridge alive until that callback returns and the worker exits. */
    __atomic_add_fetch(&asm2_sl_self_deferred_destroys, 1u,
                       __ATOMIC_RELAXED);
    if (player->callback_thread) {
      SDL_DetachThread(player->callback_thread);
      player->callback_thread = NULL;
    }
    SDL_LockMutex(player->mutex);
    player->destroy_resources_closed = 1;
    SDL_CondBroadcast(player->callback_condition);
    SDL_UnlockMutex(player->mutex);
    return;
  }
  if (player->callback_thread)
    SDL_WaitThread(player->callback_thread, NULL);
  if (handoff_result == 1) {
    int handoff_error = asm2_pthread_mutex_handoff_end(&handoff);
    if (handoff_error != 0)
      debugPrintf("ASM2_OPENSL teardown reacquire failed error=%d\n",
                  handoff_error);
  }
  asm2_sl_release_player(player);
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_set_priority(
    void *self, int32_t priority, uint32_t preemptable) {
  (void)self;
  (void)priority;
  (void)preemptable;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_get_priority(
    void *self, int32_t *priority, uint32_t *preemptable) {
  if (!self || !priority || !preemptable)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *priority = 0;
  *preemptable = 0;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_object_set_loss_interfaces(
    void *self, uint32_t count, const void *interfaces, uint32_t enabled) {
  (void)self;
  (void)count;
  (void)interfaces;
  (void)enabled;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t asm2_sl_validate_required_interfaces(
    uint32_t count, const void *interface_ids,
    const uint32_t *interface_required, int player_interfaces) {
  if (!count)
    return ASM2_SL_RESULT_SUCCESS;
  if (!interface_ids)
    return ASM2_SL_RESULT_PARAMETER_INVALID;

  const void *const *ids = interface_ids;
  for (uint32_t index = 0; index < count; ++index) {
    if (interface_required && !interface_required[index])
      continue;
    if (player_interfaces &&
        (ids[index] == asm2_sl_iid_play ||
         ids[index] == asm2_sl_iid_bufferqueue))
      continue;
    if (!player_interfaces && ids[index] == asm2_sl_iid_engine)
      continue;
    return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_engine_create_audio_player(
    void *self, void **output, const struct asm2_sl_data_source *source,
    const void *sink, uint32_t interface_count, const void *interface_ids,
    const uint32_t *interface_required) {
  struct asm2_sl_engine *engine = asm2_sl_owner(self);
  (void)sink;
  if (!engine || !output || !source)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *output = NULL;
  if (engine->object.state != ASM2_SL_OBJECT_STATE_REALIZED)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;

  uint32_t result = asm2_sl_validate_required_interfaces(
      interface_count, interface_ids, interface_required, 1);
  if (result != ASM2_SL_RESULT_SUCCESS)
    return result;

  struct asm2_sl_player *player = calloc(1, sizeof(*player));
  if (!player)
    return ASM2_SL_RESULT_MEMORY_FAILURE;
  __atomic_add_fetch(&asm2_sl_players_created, 1u, __ATOMIC_RELAXED);
  player->object.interface.vtable = asm2_sl_object_vtable;
  player->object.interface.owner = &player->object;
  player->object.kind = ASM2_SL_OBJECT_PLAYER;
  player->object.state = ASM2_SL_OBJECT_STATE_UNREALIZED;
  player->play.vtable = asm2_sl_play_vtable;
  player->play.owner = player;
  player->buffer_queue.vtable = asm2_sl_buffer_queue_vtable;
  player->buffer_queue.owner = player;
  player->frequency = 32000;
  player->channels = 2;
  player->bytes_per_frame = 4;
  player->play_state = ASM2_SL_PLAYSTATE_STOPPED;
  player->queue_capacity = 2;

  if (source->locator) {
    const struct asm2_sl_buffer_queue_locator *locator = source->locator;
    if (locator->locator_type != ASM2_SL_DATALOCATOR_BUFFERQUEUE) {
      debugPrintf("ASM2_OPENSL unsupported source locator=%u\n",
                  locator->locator_type);
      asm2_sl_release_player(player);
      return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
    }
    if (locator->num_buffers > 0 && locator->num_buffers <= 64)
      player->queue_capacity = locator->num_buffers;
  }

  if (source->format) {
    const struct asm2_sl_pcm_format *format = source->format;
    if (format->format_type != ASM2_SL_DATAFORMAT_PCM ||
        format->num_channels != 2 || format->samples_per_sec != 32000000u ||
        format->bits_per_sample != 16 || format->container_size != 16 ||
        (format->endianness != 0 &&
         format->endianness != ASM2_SL_BYTEORDER_LITTLEENDIAN)) {
      debugPrintf("ASM2_OPENSL unsupported PCM type=%u channels=%u rate=%u "
                  "bits=%u container=%u endian=%u\n",
                  format->format_type, format->num_channels,
                  format->samples_per_sec, format->bits_per_sample,
                  format->container_size, format->endianness);
      asm2_sl_release_player(player);
      return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
    }
  }

  *output = &player->object.interface;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_engine_create_output_mix(
    void *self, void **output, uint32_t interface_count,
    const void *interface_ids, const uint32_t *interface_required) {
  struct asm2_sl_engine *engine = asm2_sl_owner(self);
  if (!engine || !output)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *output = NULL;
  if (engine->object.state != ASM2_SL_OBJECT_STATE_REALIZED)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;
  if (interface_count && (!interface_ids || !interface_required))
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  for (uint32_t index = 0; index < interface_count; ++index) {
    if (interface_required[index])
      return ASM2_SL_RESULT_FEATURE_UNSUPPORTED;
  }

  struct asm2_sl_output_mix *mix = calloc(1, sizeof(*mix));
  if (!mix)
    return ASM2_SL_RESULT_MEMORY_FAILURE;
  mix->object.interface.vtable = asm2_sl_object_vtable;
  mix->object.interface.owner = &mix->object;
  mix->object.kind = ASM2_SL_OBJECT_OUTPUT_MIX;
  mix->object.state = ASM2_SL_OBJECT_STATE_UNREALIZED;
  *output = &mix->object.interface;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_play_set_state(void *self,
                                                       uint32_t state) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player || state < ASM2_SL_PLAYSTATE_STOPPED ||
      state > ASM2_SL_PLAYSTATE_PLAYING)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (player->object.state != ASM2_SL_OBJECT_STATE_REALIZED)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;

  SDL_LockMutex(player->mutex);
  if (state == ASM2_SL_PLAYSTATE_STOPPED) {
    ++player->callback_generation;
    player->pending_callbacks = 0;
    player->pending_recovery_callbacks = 0;
  }
  player->play_state = state;
  if (state == ASM2_SL_PLAYSTATE_PLAYING)
    SDL_CondSignal(player->callback_condition);
  SDL_UnlockMutex(player->mutex);
  SDL_PauseAudioDevice(player->device,
                       state == ASM2_SL_PLAYSTATE_PLAYING ? 0 : 1);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_play_get_state(void *self,
                                                       uint32_t *state) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player || !state)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (!player->mutex)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;
  SDL_LockMutex(player->mutex);
  *state = player->play_state;
  SDL_UnlockMutex(player->mutex);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_play_get_duration(void *self,
                                                          uint32_t *duration) {
  if (!self || !duration)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *duration = UINT32_MAX;
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_play_get_position(void *self,
                                                          uint32_t *position) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player || !position)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (!player->mutex)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;
  SDL_LockMutex(player->mutex);
  *position = (uint32_t)((player->played_frames * 1000u) / player->frequency);
  SDL_UnlockMutex(player->mutex);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_buffer_enqueue(void *self,
                                                       const void *data,
                                                       uint32_t size) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player || !data || !size)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (player->object.state != ASM2_SL_OBJECT_STATE_REALIZED)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;

  struct asm2_sl_audio_buffer *buffer = NULL;
  SDL_LockMutex(player->mutex);
  const int stale =
      player->callback_in_flight &&
      player->callback_thread_id == SDL_ThreadID() &&
      player->in_flight_generation != player->callback_generation;
  if (player->shutdown || stale ||
      player->queued_count >= player->queue_capacity) {
    ++player->enqueue_failures;
    if (stale)
      ++player->stale_enqueues;
    SDL_UnlockMutex(player->mutex);
    return player->shutdown || stale
               ? ASM2_SL_RESULT_PRECONDITIONS_VIOLATED
               : ASM2_SL_RESULT_BUFFER_INSUFFICIENT;
  }
  if (player->free_buffers) {
    buffer = player->free_buffers;
    player->free_buffers = buffer->next;
    buffer->next = NULL;
  }
  SDL_UnlockMutex(player->mutex);

  if (!buffer)
    buffer = calloc(1, sizeof(*buffer));
  if (!buffer)
    return ASM2_SL_RESULT_MEMORY_FAILURE;
  if (buffer->capacity < size) {
    uint8_t *resized = realloc(buffer->data, size);
    if (!resized) {
      SDL_LockMutex(player->mutex);
      asm2_sl_recycle_buffer_locked(player, buffer);
      ++player->enqueue_failures;
      SDL_UnlockMutex(player->mutex);
      return ASM2_SL_RESULT_MEMORY_FAILURE;
    }
    buffer->data = resized;
    buffer->capacity = size;
  }
  memcpy(buffer->data, data, size);
  buffer->size = size;
  buffer->offset = 0;
  buffer->next = NULL;

  uint32_t peak = 0;
  const uint8_t *pcm = data;
  for (uint32_t offset = 0; offset + 1 < size; offset += 2) {
    int32_t sample =
        (int16_t)((uint16_t)pcm[offset] | ((uint16_t)pcm[offset + 1] << 8));
    uint32_t magnitude =
        sample < 0 ? (uint32_t)(-(int64_t)sample) : (uint32_t)sample;
    if (magnitude > peak)
      peak = magnitude;
  }

  SDL_LockMutex(player->mutex);
  const int became_stale =
      player->callback_in_flight &&
      player->callback_thread_id == SDL_ThreadID() &&
      player->in_flight_generation != player->callback_generation;
  if (player->shutdown || became_stale ||
      player->queued_count >= player->queue_capacity) {
    ++player->enqueue_failures;
    if (became_stale)
      ++player->stale_enqueues;
    asm2_sl_recycle_buffer_locked(player, buffer);
    SDL_UnlockMutex(player->mutex);
    return player->shutdown || became_stale
               ? ASM2_SL_RESULT_PRECONDITIONS_VIOLATED
               : ASM2_SL_RESULT_BUFFER_INSUFFICIENT;
  }
  if (player->tail)
    player->tail->next = buffer;
  else
    player->head = buffer;
  player->tail = buffer;
  ++player->queued_count;
  ++player->enqueue_serial;
  ++player->enqueues;
  if (peak > player->peak_pcm)
    player->peak_pcm = peak;
  SDL_UnlockMutex(player->mutex);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_buffer_clear(void *self) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (!player->mutex)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;
  SDL_LockMutex(player->mutex);
  asm2_sl_clear_queued_buffers_locked(player);
  player->play_index = 0;
  player->played_frames = 0;
  ++player->callback_generation;
  player->pending_callbacks = 0;
  player->pending_recovery_callbacks = 0;
  SDL_UnlockMutex(player->mutex);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_buffer_get_state(
    void *self, struct asm2_sl_buffer_queue_state *state) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player || !state)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (!player->mutex)
    return ASM2_SL_RESULT_PRECONDITIONS_VIOLATED;
  SDL_LockMutex(player->mutex);
  state->count = player->queued_count;
  state->index = player->play_index;
  SDL_UnlockMutex(player->mutex);
  return ASM2_SL_RESULT_SUCCESS;
}

static uint32_t ASM2_GUEST_PCS asm2_sl_buffer_register_callback(
    void *self, asm2_sl_buffer_callback callback, void *context) {
  struct asm2_sl_player *player = asm2_sl_owner(self);
  if (!player)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  if (!player->mutex) {
    ++player->callback_generation;
    player->callback = callback;
    player->callback_context = context;
    return ASM2_SL_RESULT_SUCCESS;
  }
  SDL_LockMutex(player->mutex);
  ++player->callback_generation;
  player->callback = callback;
  player->callback_context = context;
  player->pending_callbacks = 0;
  player->pending_recovery_callbacks = 0;
  SDL_CondSignal(player->callback_condition);
  SDL_UnlockMutex(player->mutex);
  return ASM2_SL_RESULT_SUCCESS;
}

#define ASM2_SL_FN(function) ((uintptr_t)(function))

/* SLObjectItf_ order from OpenSL ES 1.0.1. */
static const uintptr_t asm2_sl_object_vtable[] = {
    ASM2_SL_FN(asm2_sl_object_realize),
    ASM2_SL_FN(asm2_sl_object_resume),
    ASM2_SL_FN(asm2_sl_object_get_state),
    ASM2_SL_FN(asm2_sl_object_get_interface),
    ASM2_SL_FN(asm2_sl_object_register_callback),
    ASM2_SL_FN(asm2_sl_object_abort),
    ASM2_SL_FN(asm2_sl_object_destroy),
    ASM2_SL_FN(asm2_sl_object_set_priority),
    ASM2_SL_FN(asm2_sl_object_get_priority),
    ASM2_SL_FN(asm2_sl_object_set_loss_interfaces),
};

/* SLEngineItf_ order; TASM2 uses CreateAudioPlayer (slot 2) and
 * CreateOutputMix (slot 7). */
static const uintptr_t asm2_sl_engine_vtable[] = {
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_engine_create_audio_player),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_engine_create_output_mix),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_false),
};

/* SLPlayItf_ order.  Only state and position queries are meaningful for a
 * streaming PCM buffer queue; the remaining controls report unsupported. */
static const uintptr_t asm2_sl_play_vtable[] = {
    ASM2_SL_FN(asm2_sl_play_set_state),
    ASM2_SL_FN(asm2_sl_play_get_state),
    ASM2_SL_FN(asm2_sl_play_get_duration),
    ASM2_SL_FN(asm2_sl_play_get_position),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
    ASM2_SL_FN(asm2_sl_unsupported),
};

/* SLBufferQueueItf_ and SLAndroidSimpleBufferQueueItf share this four-entry
 * ABI for PCM enqueueing on the Android OpenSL implementation. */
static const uintptr_t asm2_sl_buffer_queue_vtable[] = {
    ASM2_SL_FN(asm2_sl_buffer_enqueue),
    ASM2_SL_FN(asm2_sl_buffer_clear),
    ASM2_SL_FN(asm2_sl_buffer_get_state),
    ASM2_SL_FN(asm2_sl_buffer_register_callback),
};

uint32_t ASM2_GUEST_PCS asm2_slCreateEngine(
    void **engine_output, uint32_t option_count, const void *options,
    uint32_t interface_count, const void *interface_ids,
    const uint32_t *interface_required) {
  (void)option_count;
  (void)options;
  if (!engine_output)
    return ASM2_SL_RESULT_PARAMETER_INVALID;
  *engine_output = NULL;

  uint32_t result = asm2_sl_validate_required_interfaces(
      interface_count, interface_ids, interface_required, 0);
  if (result != ASM2_SL_RESULT_SUCCESS)
    return result;

  struct asm2_sl_engine *engine = calloc(1, sizeof(*engine));
  if (!engine)
    return ASM2_SL_RESULT_MEMORY_FAILURE;
  engine->object.interface.vtable = asm2_sl_object_vtable;
  engine->object.interface.owner = &engine->object;
  engine->object.kind = ASM2_SL_OBJECT_ENGINE;
  engine->object.state = ASM2_SL_OBJECT_STATE_UNREALIZED;
  engine->engine.vtable = asm2_sl_engine_vtable;
  engine->engine.owner = engine;
  *engine_output = &engine->object.interface;
  debugPrintf("ASM2_OPENSL engine created\n");
  return ASM2_SL_RESULT_SUCCESS;
}
