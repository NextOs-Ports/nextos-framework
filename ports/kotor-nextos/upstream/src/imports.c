/* imports.c -- KOTOR import shim table (base, non-libm).
 *
 * KOTOR is a standard SDL2 + GLES2 Android game relinked against the device's
 * NATIVE SDL2/GLESv2 (see config.h). So most symbols resolve straight from the
 * host libs via the so_resolve dlsym fallback; this table only holds the
 * boundaries that DIFFER from glibc:
 *   - bionic ABI specials (__sF, __errno, ctype tables, __aeabi_mem*, stack chk)
 *   - the pthread bridge (bionic 4-byte mutex/cond -> real glibc objects)
 *   - path-fixing fs/stdio (fix_path collapses Android-absolute paths onto cwd)
 *   - bionic struct stat (104-byte arm32 layout)
 *   - AAsset emulation (reads APK assets/ next to the binary)
 *   - OpenSL ES -> SDL audio bridge (FMOD/libandroid_port output)
 *   - GLES2 float-by-value entry points (softfp thunks)
 *   - the two Android-only SDL functions native SDL2 lacks
 *   - GLES2 *OES framebuffer aliases (mapped to core)
 *
 * libm float thunks are appended separately in main.c (softfp_fill_table).
 *
 * MIT license. See LICENSE.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <malloc.h>
#include <time.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "libc_shim.h"
#include "pthr.h"
#include "opensles_shim.h"
#include "softfp_shim.h"
#include "kotor_framework.h"

int screen_width = 1280, screen_height = 720;

extern int __cxa_atexit(void (*)(void *), void *, void *);

/* ---- tiny local shims ----------------------------------------------------- */
static int *errno_fake(void) { extern int *__errno_location(void); return __errno_location(); }
uintptr_t __stack_chk_guard_val = 0xdeadc0de;
__attribute__((noreturn)) static void stack_chk_fail_fake(void) {
  debugPrintf("!!! __stack_chk_fail caller=%p\n",
              __builtin_return_address(0));
  abort();
}
static void abort_log(void) { debugPrintf("!!! game abort()\n"); abort(); }
static int aeabi_atexit_fake(void *obj, void (*dtor)(void *), void *dso) { return __cxa_atexit(dtor, obj, dso); }
static int raise_fake(int sig) { debugPrintf("game raise(%d) ignored\n", sig); return 0; }

/*
 * FMOD's Android output does not link OpenSL ES directly.  It probes
 * libOpenSLES.so with dlopen/dlsym and falls back to a no-sound backend when
 * that probe fails.  NextOS has no Android libOpenSLES, so expose our SDL audio
 * bridge through a private handle.  Keep every other Android dlopen isolated
 * from glibc: passing bionic objects into arbitrary host libraries is unsafe.
 */
#define OPENSL_DLOPEN_HANDLE ((void *)(uintptr_t)0x534c4553u)

static void *dlopen_fake(const char *n, int f) {
  (void)f;
  if (n && strstr(n, "OpenSLES")) {
    debugPrintf("dlopen(%s)->OpenSL shim\n", n);
    return OPENSL_DLOPEN_HANDLE;
  }
  debugPrintf("dlopen(%s)->NULL\n", n ? n : "?");
  return NULL;
}

static void *dlsym_fake(void *h, const char *n) {
  void *result = NULL;
  if (h != OPENSL_DLOPEN_HANDLE || !n)
    return NULL;

  if (!strcmp(n, "slCreateEngine"))
    result = (void *)&slCreateEngine_shim;
  else if (!strcmp(n, "SL_IID_ENGINE"))
    result = (void *)&sl_IID_ENGINE;
  else if (!strcmp(n, "SL_IID_PLAY"))
    result = (void *)&sl_IID_PLAY;
  else if (!strcmp(n, "SL_IID_RECORD"))
    result = (void *)&sl_IID_RECORD;
  else if (!strcmp(n, "SL_IID_VOLUME"))
    result = (void *)&sl_IID_VOLUME;
  else if (!strcmp(n, "SL_IID_BUFFERQUEUE") ||
           !strcmp(n, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
    result = (void *)&sl_IID_BUFFERQUEUE;
  else if (!strcmp(n, "SL_IID_ANDROIDCONFIGURATION"))
    result = (void *)&sl_IID_ANDROIDCONFIGURATION;

  debugPrintf("dlsym(OpenSL,%s)->%p\n", n, result);
  return result;
}

static int dlclose_fake(void *h) {
  if (h == OPENSL_DLOPEN_HANDLE) {
    debugPrintf("dlclose(OpenSL shim)->0\n");
    return 0;
  }
  return 0;
}
static int dladdr_fake(const void *addr, void *info) { (void)addr; (void)info; return 0; }
static int gettid_shim(void) { return (int)syscall(SYS_gettid); }

typedef int (*fmod_system_init_fn)(void *system, int max_channels,
                                   unsigned int flags, void *extra_driver_data);
typedef int (*fmod_system_set_output_fn)(void *system, int output);
typedef int (*fmod_system_get_output_fn)(void *system, int *output);

static fmod_system_init_fn g_fmod_system_init;
static fmod_system_set_output_fn g_fmod_system_set_output;
static fmod_system_get_output_fn g_fmod_system_get_output;

void kotor_bind_fmod_api(void *init_fn, void *set_output_fn,
                         void *get_output_fn) {
  g_fmod_system_init = (fmod_system_init_fn)init_fn;
  g_fmod_system_set_output = (fmod_system_set_output_fn)set_output_fn;
  g_fmod_system_get_output = (fmod_system_get_output_fn)get_output_fn;
  debugPrintf("FMOD API: init=%p setOutput=%p getOutput=%p\n",
              init_fn, set_output_fn, get_output_fn);
}

/*
 * KOTOR's Java-era wrapper explicitly selects AudioTrack (output 15).  On a
 * real Android process that path writes through org.fmod.AudioDevice; NextOS
 * has no Java AudioTrack, while the same FMOD binary also carries its native
 * OpenSL backend (output 16).  During diagnosis this opt-in changes only that
 * final backend selection immediately before the original System::init.
 */
static int fmod_system_init_android(void *system, int max_channels,
                                    unsigned int flags,
                                    void *extra_driver_data) {
  if (!g_fmod_system_init) {
    debugPrintf("FMOD System::init: real entry is not bound\n");
    return 28; /* FMOD_ERR_INTERNAL */
  }

  const char *force_opensl = getenv("KOTOR_FMOD_OPENSL");
  if (force_opensl && strcmp(force_opensl, "0") != 0 &&
      g_fmod_system_set_output) {
    int set_result = g_fmod_system_set_output(system, 16);
    int selected = -1;
    int get_result = g_fmod_system_get_output
                       ? g_fmod_system_get_output(system, &selected) : -1;
    debugPrintf("FMOD System::init: force OpenSL set=%d get=%d output=%d\n",
                set_result, get_result, selected);
  }

  int result = g_fmod_system_init(system, max_channels, flags,
                                  extra_driver_data);
  debugPrintf("FMOD System::init(max=%d flags=0x%x) -> %d\n",
              max_channels, flags, result);
  return result;
}

/*
 * libKOTOR talks to FMOD through Aspyr's FModAudioSystem C++ wrapper in
 * libandroid_port.  Keep the original wrapper intact and interpose only the
 * imports of the subsequently-loaded libKOTOR module.  This opt-in trace lets
 * us correlate a resource key with its decoded duration and channel lifetime
 * without changing either the game's timing or FMOD's mixer.
 *
 * All intercepted signatures below contain only integer/pointer arguments.
 * That is intentional: both Android modules use the softfp ABI while this
 * loader is hardfp, so functions containing float parameters require dedicated
 * assembly thunks and must not be wrapped by ordinary C here.
 */
typedef int (*fmod_audio_create_sound_fn)(
    void *system, char *name, int key, void *data, unsigned long data_length,
    int spatial, int loop);
typedef int (*fmod_audio_release_sound_fn)(void *system, int key);
typedef unsigned long (*fmod_audio_play_sound_fn)(void *system, int key);
typedef int (*fmod_audio_get_sound_int_fn)(void *system, int key);
typedef unsigned long (*fmod_audio_create_stream_fn)(
    void *system, char *name, SDL_RWops *source, int a, int b, int c, int d,
    int e);
typedef int (*fmod_audio_close_stream_fn)(void *system, unsigned long stream);
typedef unsigned long (*fmod_audio_play_stream_fn)(
    void *system, unsigned long stream, int paused);
typedef int (*fmod_audio_get_stream_int_fn)(
    void *system, unsigned long stream);
typedef int (*fmod_audio_channel_int_fn)(void *system, unsigned long channel);

static fmod_audio_create_sound_fn g_fmod_audio_create_sound;
static fmod_audio_release_sound_fn g_fmod_audio_release_sound;
static fmod_audio_play_sound_fn g_fmod_audio_play_sound;
static fmod_audio_get_sound_int_fn g_fmod_audio_get_sound_length;
static fmod_audio_get_sound_int_fn g_fmod_audio_get_sound_sample_rate;
static fmod_audio_create_stream_fn g_fmod_audio_create_stream;
static fmod_audio_close_stream_fn g_fmod_audio_close_stream;
static fmod_audio_play_stream_fn g_fmod_audio_play_stream;
static fmod_audio_get_stream_int_fn g_fmod_audio_get_stream_length;
static fmod_audio_channel_int_fn g_fmod_audio_get_is_channel_playing;
static fmod_audio_channel_int_fn g_fmod_audio_get_channel_position;

typedef struct {
  unsigned long uid;
  int used;
  int last_playing;
  int last_position_bucket;
} FModChannelTrace;

static FModChannelTrace g_fmod_channel_trace[64];
static uint64_t g_fmod_trace_epoch_ms;

static int fmod_audio_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("KOTOR_FMOD_TRACE");
    enabled = value && strcmp(value, "0") != 0;
  }
  return enabled;
}

static uint64_t fmod_audio_trace_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t absolute = (uint64_t)now.tv_sec * 1000u +
                      (uint64_t)now.tv_nsec / 1000000u;
  if (!g_fmod_trace_epoch_ms)
    g_fmod_trace_epoch_ms = absolute;
  return absolute - g_fmod_trace_epoch_ms;
}

static void fmod_audio_trace_name(const char *source, char output[65]) {
  if (!source) {
    strcpy(output, "(null)");
    return;
  }
  size_t i;
  for (i = 0; i < 64; i++) {
    unsigned char value = (unsigned char)source[i];
    if (!value)
      break;
    output[i] = (value >= 32 && value < 127) ? (char)value : '?';
  }
  output[i] = '\0';
}

static FModChannelTrace *fmod_audio_trace_channel(unsigned long uid) {
  FModChannelTrace *free_entry = NULL;
  for (size_t i = 0; i < sizeof(g_fmod_channel_trace) /
                            sizeof(g_fmod_channel_trace[0]); i++) {
    FModChannelTrace *entry = &g_fmod_channel_trace[i];
    if (entry->used && entry->uid == uid)
      return entry;
    if (!entry->used && !free_entry)
      free_entry = entry;
  }
  if (!free_entry)
    free_entry = &g_fmod_channel_trace[uid %
        (sizeof(g_fmod_channel_trace) / sizeof(g_fmod_channel_trace[0]))];
  free_entry->uid = uid;
  free_entry->used = 1;
  free_entry->last_playing = -1;
  free_entry->last_position_bucket = -1;
  return free_entry;
}

static int fmod_audio_create_sound_trace(
    void *system, char *name, int key, void *data, unsigned long data_length,
    int spatial, int loop) {
  if (!g_fmod_audio_create_sound)
    return 0;
  int result = g_fmod_audio_create_sound(system, name, key, data, data_length,
                                         spatial, loop);
  if (fmod_audio_trace_enabled()) {
    char printable_name[65];
    fmod_audio_trace_name(name, printable_name);
    debugPrintf("[FMODWRAP +%llums] CreateSound key=%d name=\"%s\" "
                "data=%p bytes=%lu spatial=%d loop=%d -> %d\n",
                (unsigned long long)fmod_audio_trace_ms(), key, printable_name,
                data, data_length, spatial, loop, result);
  }
  return result;
}

static int fmod_audio_release_sound_trace(void *system, int key) {
  int result = g_fmod_audio_release_sound
                 ? g_fmod_audio_release_sound(system, key) : 0;
  if (fmod_audio_trace_enabled())
    debugPrintf("[FMODWRAP +%llums] ReleaseSound key=%d -> %d\n",
                (unsigned long long)fmod_audio_trace_ms(), key, result);
  return result;
}

static unsigned long fmod_audio_play_sound_trace(void *system, int key) {
  unsigned long uid = g_fmod_audio_play_sound
                        ? g_fmod_audio_play_sound(system, key) : ~0ul;
  if (fmod_audio_trace_enabled()) {
    FModChannelTrace *entry = fmod_audio_trace_channel(uid);
    entry->last_playing = -1;
    entry->last_position_bucket = -1;
    debugPrintf("[FMODWRAP +%llums] PlaySound key=%d -> channel=%lu\n",
                (unsigned long long)fmod_audio_trace_ms(), key, uid);
  }
  return uid;
}

static int fmod_audio_get_sound_length_trace(void *system, int key) {
  int result = g_fmod_audio_get_sound_length
                 ? g_fmod_audio_get_sound_length(system, key) : 0;
  if (fmod_audio_trace_enabled())
    debugPrintf("[FMODWRAP +%llums] GetSoundLength key=%d -> %dms\n",
                (unsigned long long)fmod_audio_trace_ms(), key, result);
  return result;
}

static int fmod_audio_get_sound_sample_rate_trace(void *system, int key) {
  int result = g_fmod_audio_get_sound_sample_rate
                 ? g_fmod_audio_get_sound_sample_rate(system, key) : 0;
  if (fmod_audio_trace_enabled())
    debugPrintf("[FMODWRAP +%llums] GetSoundSampleRate key=%d -> %dHz\n",
                (unsigned long long)fmod_audio_trace_ms(), key, result);
  return result;
}

static unsigned long fmod_audio_create_stream_trace(
    void *system, char *name, SDL_RWops *source, int a, int b, int c, int d,
    int e) {
  unsigned long stream = g_fmod_audio_create_stream
                           ? g_fmod_audio_create_stream(system, name, source,
                                                        a, b, c, d, e) : 0;
  if (fmod_audio_trace_enabled()) {
    char printable_name[65];
    fmod_audio_trace_name(name, printable_name);
    debugPrintf("[FMODWRAP +%llums] CreateStream name=\"%s\" rw=%p "
                "args=%d,%d,%d,%d,%d -> stream=%lu\n",
                (unsigned long long)fmod_audio_trace_ms(), printable_name,
                source, a, b, c, d, e, stream);
  }
  return stream;
}

static int fmod_audio_close_stream_trace(void *system, unsigned long stream) {
  int result = g_fmod_audio_close_stream
                 ? g_fmod_audio_close_stream(system, stream) : 0;
  if (fmod_audio_trace_enabled())
    debugPrintf("[FMODWRAP +%llums] CloseStream stream=%lu -> %d\n",
                (unsigned long long)fmod_audio_trace_ms(), stream, result);
  return result;
}

static unsigned long fmod_audio_play_stream_trace(
    void *system, unsigned long stream, int paused) {
  unsigned long uid = g_fmod_audio_play_stream
                        ? g_fmod_audio_play_stream(system, stream, paused)
                        : ~0ul;
  if (fmod_audio_trace_enabled()) {
    FModChannelTrace *entry = fmod_audio_trace_channel(uid);
    entry->last_playing = -1;
    entry->last_position_bucket = -1;
    debugPrintf("[FMODWRAP +%llums] PlayStream stream=%lu paused=%d "
                "-> channel=%lu\n",
                (unsigned long long)fmod_audio_trace_ms(), stream, paused, uid);
  }
  return uid;
}

static int fmod_audio_get_stream_length_trace(
    void *system, unsigned long stream) {
  int result = g_fmod_audio_get_stream_length
                 ? g_fmod_audio_get_stream_length(system, stream) : 0;
  if (fmod_audio_trace_enabled())
    debugPrintf("[FMODWRAP +%llums] GetStreamLength stream=%lu -> %dms\n",
                (unsigned long long)fmod_audio_trace_ms(), stream, result);
  return result;
}

static int fmod_audio_get_is_channel_playing_trace(
    void *system, unsigned long uid) {
  int result = g_fmod_audio_get_is_channel_playing
                 ? g_fmod_audio_get_is_channel_playing(system, uid) : 0;
  if (fmod_audio_trace_enabled()) {
    FModChannelTrace *entry = fmod_audio_trace_channel(uid);
    if (entry->last_playing != result) {
      debugPrintf("[FMODWRAP +%llums] ChannelPlaying uid=%lu -> %d\n",
                  (unsigned long long)fmod_audio_trace_ms(), uid, result);
      entry->last_playing = result;
    }
  }
  return result;
}

static int fmod_audio_get_channel_position_trace(
    void *system, unsigned long uid) {
  int result = g_fmod_audio_get_channel_position
                 ? g_fmod_audio_get_channel_position(system, uid) : 0;
  if (fmod_audio_trace_enabled()) {
    FModChannelTrace *entry = fmod_audio_trace_channel(uid);
    int bucket = result >= 0 ? result / 500 : result;
    if (entry->last_position_bucket != bucket) {
      debugPrintf("[FMODWRAP +%llums] ChannelPosition uid=%lu -> %dms\n",
                  (unsigned long long)fmod_audio_trace_ms(), uid, result);
      entry->last_position_bucket = bucket;
    }
  }
  return result;
}

void kotor_bind_fmod_audio_system_api(
    void *create_sound, void *release_sound, void *play_sound,
    void *get_sound_length, void *get_sound_sample_rate, void *create_stream,
    void *close_stream, void *play_stream, void *get_stream_length,
    void *get_is_channel_playing, void *get_channel_position) {
  g_fmod_audio_create_sound = (fmod_audio_create_sound_fn)create_sound;
  g_fmod_audio_release_sound = (fmod_audio_release_sound_fn)release_sound;
  g_fmod_audio_play_sound = (fmod_audio_play_sound_fn)play_sound;
  g_fmod_audio_get_sound_length =
      (fmod_audio_get_sound_int_fn)get_sound_length;
  g_fmod_audio_get_sound_sample_rate =
      (fmod_audio_get_sound_int_fn)get_sound_sample_rate;
  g_fmod_audio_create_stream = (fmod_audio_create_stream_fn)create_stream;
  g_fmod_audio_close_stream = (fmod_audio_close_stream_fn)close_stream;
  g_fmod_audio_play_stream = (fmod_audio_play_stream_fn)play_stream;
  g_fmod_audio_get_stream_length =
      (fmod_audio_get_stream_int_fn)get_stream_length;
  g_fmod_audio_get_is_channel_playing =
      (fmod_audio_channel_int_fn)get_is_channel_playing;
  g_fmod_audio_get_channel_position =
      (fmod_audio_channel_int_fn)get_channel_position;
}

void kotor_install_fmod_audio_system_hooks(
    DynLibFunction *functions, int count) {
  static const struct {
    const char *symbol;
    uintptr_t hook;
  } hooks[] = {
    { "_ZN15FModAudioSystem11CreateSoundEPciPvmii",
      (uintptr_t)&fmod_audio_create_sound_trace },
    { "_ZN15FModAudioSystem12ReleaseSoundEi",
      (uintptr_t)&fmod_audio_release_sound_trace },
    { "_ZN15FModAudioSystem9PlaySoundEi",
      (uintptr_t)&fmod_audio_play_sound_trace },
    { "_ZN15FModAudioSystem14GetSoundLengthEi",
      (uintptr_t)&fmod_audio_get_sound_length_trace },
    { "_ZN15FModAudioSystem18GetSoundSampleRateEi",
      (uintptr_t)&fmod_audio_get_sound_sample_rate_trace },
    { "_ZN15FModAudioSystem12CreateStreamEPcP9SDL_RWopsiiiii",
      (uintptr_t)&fmod_audio_create_stream_trace },
    { "_ZN15FModAudioSystem11CloseStreamEm",
      (uintptr_t)&fmod_audio_close_stream_trace },
    { "_ZN15FModAudioSystem10PlayStreamEmi",
      (uintptr_t)&fmod_audio_play_stream_trace },
    { "_ZN15FModAudioSystem15GetStreamLengthEm",
      (uintptr_t)&fmod_audio_get_stream_length_trace },
    { "_ZN15FModAudioSystem19GetIsChannelPlayingEm",
      (uintptr_t)&fmod_audio_get_is_channel_playing_trace },
    { "_ZN15FModAudioSystem26GetChannelPlaybackPositionEm",
      (uintptr_t)&fmod_audio_get_channel_position_trace },
  };
  if (!fmod_audio_trace_enabled()) {
    debugPrintf("FModAudioSystem trace hooks: disabled\n");
    return;
  }
  int installed = 0;
  for (int i = 0; i < count; i++) {
    for (size_t j = 0; j < sizeof(hooks) / sizeof(hooks[0]); j++) {
      if (!strcmp(functions[i].symbol, hooks[j].symbol)) {
        functions[i].func = hooks[j].hook;
        installed++;
        break;
      }
    }
  }
  debugPrintf("FModAudioSystem trace hooks: %d/%zu installed (enabled)\n",
              installed, sizeof(hooks) / sizeof(hooks[0]));
}

static int android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  debugPrintf("[alog:%s] %s\n", tag ? tag : "?", b); return 0;
}
static int android_log_write_fake(int prio, const char *tag, const char *msg) {
  (void)prio; debugPrintf("[alog:%s] %s\n", tag ? tag : "?", msg ? msg : ""); return 0;
}
static void assert2_fake(const char *f, int l, const char *fn, const char *e) {
  debugPrintf("!!! __assert2 %s:%d %s: %s\n", f ? f : "?", l, fn ? fn : "?", e ? e : "?"); abort();
}

/* KOTOR uses SDL2 for GL; there is no cooperative GL-context handover, so these
 * park/handover hooks referenced by the shared shims (pthr.c, libc_shim.c) are
 * no-ops. egl_gl_thread_holds_context()==0 makes the park paths early-out. */
void egl_gl_ownership_park(void) {}
void egl_gl_service_handover(void) {}
void egl_gl_ownership_release(void) {}
int  egl_gl_thread_holds_context(void) { return 0; }

/* Opt-in allocator accounting used to distinguish CPU resource growth from
 * Mali texture mappings while a module is loading. Calls made by the loader
 * itself still go straight to glibc; only Android-module imports pass here. */
static size_t alloc_live;
static size_t alloc_next_report = 128u * 1024u * 1024u;
static size_t alloc_calls;
static size_t free_calls;

typedef struct {
  void *caller;
  size_t total;
  size_t next_report;
  size_t calls;
} AllocCaller;
static AllocCaller alloc_callers[1024];

static void alloc_caller_added(void *caller, size_t requested) {
  size_t slot = ((uintptr_t)caller >> 2) & 1023u;
  for (size_t probe = 0; probe < 1024; probe++) {
    AllocCaller *entry = &alloc_callers[(slot + probe) & 1023u];
    if (!entry->caller)
      entry->caller = caller;
    if (entry->caller != caller)
      continue;
    entry->total += requested;
    entry->calls++;
    if (!entry->next_report)
      entry->next_report = 64u * 1024u * 1024u;
    if (entry->total >= entry->next_report) {
      debugPrintf("ALLOC caller=%p cumulative=%zu MB calls=%zu\n",
                  caller, entry->total / (1024u * 1024u), entry->calls);
      entry->next_report += 64u * 1024u * 1024u;
    }
    return;
  }
}

static void alloc_added(const char *kind, void *pointer, size_t requested,
                        void *caller) {
  if (!getenv("KOTOR_ALLOC_TRACE") || !pointer)
    return;
  const size_t actual = malloc_usable_size(pointer);
  const size_t live = __atomic_add_fetch(&alloc_live, actual, __ATOMIC_RELAXED);
  __atomic_add_fetch(&alloc_calls, 1, __ATOMIC_RELAXED);
  alloc_caller_added(caller, requested);
  if (requested >= 4u * 1024u * 1024u)
    debugPrintf("ALLOC %s req=%zu actual=%zu live=%zu caller=%p ptr=%p\n",
                kind, requested, actual, live, caller, pointer);
  size_t next = __atomic_load_n(&alloc_next_report, __ATOMIC_RELAXED);
  if (live >= next &&
      __atomic_compare_exchange_n(&alloc_next_report, &next,
                                  next + 64u * 1024u * 1024u, 0,
                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    debugPrintf("ALLOC live crossed %zu MB allocs=%zu frees=%zu\n",
                live / (1024u * 1024u),
                __atomic_load_n(&alloc_calls, __ATOMIC_RELAXED),
                __atomic_load_n(&free_calls, __ATOMIC_RELAXED));
}

static void *malloc_android(size_t size) {
  void *pointer = malloc(size);
  alloc_added("malloc", pointer, size, __builtin_return_address(0));
  return pointer;
}
static void *calloc_android(size_t count, size_t size) {
  void *pointer = calloc(count, size);
  const size_t requested =
      size && count > SIZE_MAX / size ? SIZE_MAX : count * size;
  alloc_added("calloc", pointer, requested, __builtin_return_address(0));
  return pointer;
}
static void free_android(void *pointer) {
  if (getenv("KOTOR_ALLOC_TRACE") && pointer) {
    __atomic_add_fetch(&free_calls, 1, __ATOMIC_RELAXED);
    const size_t actual = malloc_usable_size(pointer);
    size_t live = __atomic_load_n(&alloc_live, __ATOMIC_RELAXED);
    if (actual <= live)
      __atomic_sub_fetch(&alloc_live, actual, __ATOMIC_RELAXED);
  }
  free(pointer);
}
static void *realloc_android(void *old_pointer, size_t size) {
  size_t old_size = 0;
  if (getenv("KOTOR_ALLOC_TRACE") && old_pointer)
    old_size = malloc_usable_size(old_pointer);
  void *pointer = realloc(old_pointer, size);
  if (getenv("KOTOR_ALLOC_TRACE") && pointer && old_size) {
    size_t live = __atomic_load_n(&alloc_live, __ATOMIC_RELAXED);
    if (old_size <= live)
      __atomic_sub_fetch(&alloc_live, old_size, __ATOMIC_RELAXED);
  }
  alloc_added("realloc", pointer, size, __builtin_return_address(0));
  return pointer;
}

/* fprintf over the fake bionic FILE* (libc_shim provides only vfprintf_fake). */
int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vfprintf_fake(f, fmt, ap);
  va_end(ap); return r;
}

/* ---- __aeabi_mem* (note bionic's reversed arg order for memset/memclr) ----- */
static void aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
static void aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }   /* (dest, n, c) */
static void aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }

/* Android ARM reserves 256 bytes for jmp_buf.  The host armhf glibc setjmp
 * writes its larger private representation past that boundary (including the
 * signal-mask fields), which corrupts FreeType's stack frame while loading the
 * font cmap.  Keep the guest's setjmp/longjmp pair self-contained and save the
 * complete AAPCS callee-saved core/VFP state in 112 bytes.  Signal masks are
 * intentionally not part of this private representation. */
__attribute__((naked, returns_twice))
static int bionic_setjmp_fake(void *env) {
  __asm__ volatile(
      "mov ip, sp            \n"
      "str ip, [r0, #0]      \n"
      "str lr, [r0, #4]      \n"
      "add ip, r0, #8        \n"
      "stmia ip, {r4-r11}    \n"
      "vmrs r2, fpscr        \n"
      "str r2, [r0, #40]     \n"
      "movw r2, #0x7062      \n"
      "movt r2, #0x6a6d      \n"
      "str r2, [r0, #44]     \n"
      "add ip, r0, #48       \n"
      "vstmia ip, {d8-d15}   \n"
      "mov r0, #0            \n"
      "bx lr                 \n");
}

__attribute__((naked, noreturn))
static void bionic_longjmp_fake(void *env, int value) {
  __asm__ volatile(
      "add ip, r0, #48       \n"
      "vldmia ip, {d8-d15}   \n"
      "ldr r2, [r0, #40]     \n"
      "vmsr fpscr, r2        \n"
      "add ip, r0, #8        \n"
      "ldmia ip, {r4-r11}    \n"
      "ldr r2, [r0, #0]      \n"
      "mov sp, r2            \n"
      "ldr lr, [r0, #4]      \n"
      "cmp r1, #0            \n"
      "bne 1f                \n"
      "mov r1, #1            \n"
      "1:                    \n"
      "mov r0, r1            \n"
      "bx lr                 \n");
}

/* ---- Android-only SDL funcs native SDL2 lacks ----------------------------- */
static const char *SDL_AndroidGetExternalStoragePath_fake(void) { return "."; }
static const char *SDL_AndroidGetInternalStoragePath_fake(void) { return "."; }
static int SDL_IsChromebook_fake(void) { return 0; }

/* SDL's Android backend resolves relative SDL_RWFromFile paths against the
 * APK AssetManager.  Native fbdev SDL has no APK backend, so preserve the same
 * lookup order and fall back to the extracted APK assets directory. */
static SDL_RWops *SDL_RWFromFile_android(const char *path, const char *mode) {
  SDL_RWops *rw = SDL_RWFromFile(path, mode);
  char case_path[1024];
  const int trace_party =
      path && (strcasestr(path, "gameinprogress") ||
               strcasestr(path, "availnpc"));
  if (!rw && path && mode && strchr(mode, 'r') &&
      resolve_case_path(path, case_path, sizeof(case_path))) {
    rw = SDL_RWFromFile(case_path, mode);
    if (trace_party || getenv("KOTOR_ASSET_TRACE"))
      debugPrintf("SDL_RWFromFile casefold(%s -> %s, %s) -> %s\n",
                  path, case_path, mode, rw ? "OK" : "MISSING");
  }
  if (!rw && path && path[0] != '/') {
    char asset_path[1024];
    snprintf(asset_path, sizeof(asset_path), "%s/%s", GAMEDATA_DIR, path);
    rw = SDL_RWFromFile(asset_path, mode);
    if (trace_party || getenv("KOTOR_ASSET_TRACE"))
      debugPrintf("SDL_RWFromFile(%s -> %s, %s) -> %s\n",
                  path, asset_path, mode ? mode : "?",
                  rw ? "OK" : "MISSING");
  } else if (trace_party) {
    debugPrintf("SDL_RWFromFile(%s, %s) -> %s\n",
                path, mode ? mode : "?", rw ? "OK" : "MISSING");
  }
  return rw;
}

typedef void (*android_compressed_tex_fn)(GLenum, GLint, GLenum, GLsizei,
                                          GLsizei, GLint, GLsizei,
                                          const void *);
static android_compressed_tex_fn real_android_compressed_tex;
static _Thread_local int android_compressed_upload;

void kotor_set_android_compressed_tex(void *function) {
  real_android_compressed_tex = (android_compressed_tex_fn)function;
}

static void android_port_glCompressedTexImage2D_mali(
    GLenum target, GLint level, GLenum internalformat, GLsizei width,
    GLsizei height, GLint border, GLsizei image_size, const void *pixels) {
  if (!real_android_compressed_tex)
    return;
  android_compressed_upload++;
  real_android_compressed_tex(target, level, internalformat, width, height,
                              border, image_size, pixels);
  android_compressed_upload--;
}

/* Mali-450 has no S3TC. Aspyr consequently expands KOTOR's DXT textures to
 * RGBA8888 before upload, which exhausts a 1 GiB device while entering the
 * first module. Android GLES2 accepts the native 16-bit transfer types, so
 * repack immutable RGBA/RGB uploads at the API boundary. This preserves the
 * engine's texture/resource flow and halves the driver-side texture storage.
 * Binary/opaque alpha uses RGB5_A1; genuinely graded alpha uses RGBA4. */
static void glTexImage2D_mali(GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void *pixels) {
  const int enabled = getenv("KOTOR_TEX_16BIT") != NULL;
  if (!enabled || !android_compressed_upload || !pixels ||
      width <= 0 || height <= 0 ||
      type != GL_UNSIGNED_BYTE ||
      (format != GL_RGBA && format != GL_RGB)) {
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, pixels);
    return;
  }

  const size_t count = (size_t)width * (size_t)height;
  if (count > SIZE_MAX / 2) {
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, pixels);
    return;
  }
  uint16_t *packed = malloc(count * sizeof(*packed));
  if (!packed) {
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, pixels);
    return;
  }

  GLenum packed_type;
  if (format == GL_RGB) {
    const uint8_t *src = pixels;
    packed_type = GL_UNSIGNED_SHORT_5_6_5;
    for (size_t i = 0; i < count; i++, src += 3)
      packed[i] = (uint16_t)(((src[0] >> 3) << 11) |
                             ((src[1] >> 2) << 5) | (src[2] >> 3));
  } else {
    const uint8_t *src = pixels;
    int graded_alpha = 0;
    for (size_t i = 0; i < count; i++)
      if (src[i * 4 + 3] != 0 && src[i * 4 + 3] != 255) {
        graded_alpha = 1;
        break;
      }
    if (graded_alpha) {
      packed_type = GL_UNSIGNED_SHORT_4_4_4_4;
      for (size_t i = 0; i < count; i++, src += 4)
        packed[i] = (uint16_t)(((src[0] >> 4) << 12) |
                               ((src[1] >> 4) << 8) |
                               ((src[2] >> 4) << 4) | (src[3] >> 4));
    } else {
      packed_type = GL_UNSIGNED_SHORT_5_5_5_1;
      for (size_t i = 0; i < count; i++, src += 4)
        packed[i] = (uint16_t)(((src[0] >> 3) << 11) |
                               ((src[1] >> 3) << 6) |
                               ((src[2] >> 3) << 1) | (src[3] >> 7));
    }
  }

  if (getenv("KOTOR_TEX_TRACE") && width >= 256 && height >= 256)
    debugPrintf("TEX16 %dx%d level=%d %s -> type=0x%x\n",
                width, height, level, format == GL_RGBA ? "RGBA8" : "RGB8",
                packed_type);
  glTexImage2D(target, level, format, width, height, border,
               format, packed_type, packed);
  if (getenv("KOTOR_TEX_TRACE")) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
      debugPrintf("TEX16 error=0x%x %dx%d level=%d format=0x%x type=0x%x\n",
                  error, width, height, level, format, packed_type);
  }
  free(packed);
}

static unsigned gl_draw_arrays_count;
static unsigned gl_draw_elements_count;
static unsigned gl_clear_count;
static GLuint gl_last_draw_program;

static GLuint glCreateShader_trace(GLenum type) {
  GLuint shader = glCreateShader(type);
  if (getenv("KOTOR_GL_TRACE"))
    debugPrintf("GL create shader=%u type=0x%x\n", shader, (unsigned)type);
  return shader;
}

static void glShaderSource_trace(GLuint shader, GLsizei count,
                                 const GLchar *const *strings,
                                 const GLint *lengths) {
  if (getenv("KOTOR_GL_TRACE")) {
    debugPrintf("GL shader source id=%u count=%d strings=%p lengths=%p\n",
                shader, count, strings, lengths);
    for (GLsizei i = 0; i < count; i++) {
      int length = 0;
      if (strings && strings[i]) {
        length = lengths && lengths[i] >= 0
                   ? lengths[i] : (int)strlen(strings[i]);
      }
      int shown = length < 160 ? length : 160;
      debugPrintf("  source[%d] ptr=%p len=%d explicit=%d text='%.*s'\n",
                  i, strings ? strings[i] : NULL, length,
                  lengths ? lengths[i] : -1, shown,
                  strings && strings[i] ? strings[i] : "");
    }
  }
  glShaderSource(shader, count, strings, lengths);
}

static void glAttachShader_trace(GLuint program, GLuint shader) {
  if (getenv("KOTOR_GL_TRACE"))
    debugPrintf("GL attach program=%u shader=%u\n", program, shader);
  glAttachShader(program, shader);
}

static void glUseProgram_trace(GLuint program) {
  glUseProgram(program);
}

static void glDrawArrays_trace(GLenum mode, GLint first, GLsizei count) {
  gl_draw_arrays_count++;
  if (getenv("KOTOR_GL_TRACE")) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl_last_draw_program = (GLuint)program;
  }
  glDrawArrays(mode, first, count);
}

static void glDrawElements_trace(GLenum mode, GLsizei count, GLenum type,
                                 const void *indices) {
  gl_draw_elements_count++;
  if (getenv("KOTOR_GL_TRACE")) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl_last_draw_program = (GLuint)program;
  }
  glDrawElements(mode, count, type, indices);
}

static void glClear_trace(GLbitfield mask) {
  gl_clear_count++;
  glClear(mask);
}

static void glCompileShader_trace(GLuint shader) {
  glCompileShader(shader);
  if (getenv("KOTOR_GL_TRACE")) {
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[1024] = {0};
      GLsizei length = 0;
      glGetShaderInfoLog(shader, sizeof(log) - 1, &length, log);
      debugPrintf("GL shader %u compile failed: %s\n", shader, log);
    }
  }
}

static void glLinkProgram_trace(GLuint program) {
  glLinkProgram(program);
  if (getenv("KOTOR_GL_TRACE")) {
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[1024] = {0};
      GLsizei length = 0;
      glGetProgramInfoLog(program, sizeof(log) - 1, &length, log);
      debugPrintf("GL program %u link failed: %s\n", program, log);
    }
  }
}

/* Short, opt-in probe at the native SDL presentation boundary.  This is kept
 * silent in release runs and does not alter GL state. */
static void SDL_GL_SwapWindow_trace(SDL_Window *window) {
  static unsigned frame;
  frame++;
  if (getenv("KOTOR_GL_TRACE") &&
      (frame <= 10 || frame == 30 || frame == 60 || frame == 120 ||
       frame == 300)) {
    GLint viewport[4] = {0}, scissor[4] = {0}, fbo = 0;
    GLubyte center[4] = {0}, quarter[4] = {0};
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    if (fbo == 0 && w > 0 && h > 0) {
      glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
      glReadPixels(w / 4, h / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, quarter);
    }
    debugPrintf("GLSWAP #%u draws=%u/%u clears=%u lastprog=%u drawable=%dx%d viewport=%d,%d %dx%d "
                "scissor=%d,%d %dx%d fbo=%d center=%u,%u,%u,%u "
                "quarter=%u,%u,%u,%u err=0x%x\n",
                frame, gl_draw_arrays_count, gl_draw_elements_count,
                gl_clear_count, gl_last_draw_program, w, h,
                viewport[0], viewport[1], viewport[2], viewport[3],
                scissor[0], scissor[1], scissor[2], scissor[3], fbo,
                center[0], center[1], center[2], center[3],
                quarter[0], quarter[1], quarter[2], quarter[3],
                (unsigned)glGetError());
  }
  kotor_framework_observe_swap(window);
  SDL_GL_SwapWindow(window);
}

static int SDL_PollEvent_trace(SDL_Event *event) {
  int rc = SDL_PollEvent(event);
  if (rc && event && getenv("KOTOR_GL_TRACE")) {
    if (event->type == SDL_WINDOWEVENT)
      debugPrintf("SDLEVENT type=0x%x window=%u data=%d,%d\n",
                  event->type, event->window.event,
                  event->window.data1, event->window.data2);
    else
      debugPrintf("SDLEVENT type=0x%x\n", event->type);
  }
  return rc;
}

/* ---- Aspyr-SDL internal symbols the game links against -------------------- *
 * The Aspyr port links against modified-SDL2 internals, not just the public
 * API: Android_JNI_GetEnv (returns the JNIEnv), the Android_Window native
 * window pointer, and the g_SDL_BufferGeometry_w/h surface size globals. Native
 * SDL2 has none of these, so we provide stand-ins. */
extern void *fake_env;
void *Android_JNI_GetEnv(void) { return fake_env; }
void *Android_Window = NULL;               /* set after the SDL window exists */
int g_SDL_BufferGeometry_w = 0;            /* surface size; also set in main */
int g_SDL_BufferGeometry_h = 0;

/* ---- GLES2 *OES framebuffer aliases (core on ES2) ------------------------- */
typedef void *(*mapbuf_oes_t)(unsigned target, unsigned access);
typedef unsigned char (*unmapbuf_oes_t)(unsigned target);
static mapbuf_oes_t   p_glMapBufferOES;
static unmapbuf_oes_t p_glUnmapBufferOES;
static void *glMapBufferOES_w(unsigned target, unsigned access) {
  if (!p_glMapBufferOES) p_glMapBufferOES = (mapbuf_oes_t)eglGetProcAddress("glMapBufferOES");
  return p_glMapBufferOES ? p_glMapBufferOES(target, access) : NULL;
}
static unsigned char glUnmapBufferOES_w(unsigned target) {
  if (!p_glUnmapBufferOES) p_glUnmapBufferOES = (unmapbuf_oes_t)eglGetProcAddress("glUnmapBufferOES");
  return p_glUnmapBufferOES ? p_glUnmapBufferOES(target) : 0;
}

/* ---- C++ unwinder registry: exceptions inside custom-loaded modules ------- */
#define MAX_EXIDX 12
static struct { uintptr_t lo, hi, exidx; int count; } exidx_reg[MAX_EXIDX];
static int exidx_n = 0;
void exidx_register(uintptr_t lo, uintptr_t hi, uintptr_t exidx, int count) {
  if (exidx_n < MAX_EXIDX && exidx) {
    exidx_reg[exidx_n].lo = lo; exidx_reg[exidx_n].hi = hi;
    exidx_reg[exidx_n].exidx = exidx; exidx_reg[exidx_n].count = count; exidx_n++;
  }
}
static void *find_exidx_fake(uintptr_t pc, int *pcount) {
  for (int i = 0; i < exidx_n; i++)
    if (pc >= exidx_reg[i].lo && pc < exidx_reg[i].hi) {
      if (pcount) *pcount = exidx_reg[i].count;
      return (void *)exidx_reg[i].exidx;
    }
  static void *(*real)(uintptr_t, int *) = NULL; static int tried = 0;
  if (!tried) { tried = 1; real = dlsym(RTLD_DEFAULT, "__gnu_Unwind_Find_exidx"); }
  if (real && real != (void *)find_exidx_fake) return real(pc, pcount);
  if (pcount) *pcount = 0;
  return NULL;
}

/* ---- bionic ctype tables (game inlines isXXX/toXXX against these) ---------- */
static unsigned char bionic_ctype[257];
static short bionic_tolower[257], bionic_toupper[257];
const unsigned char *_ctype_ptr = bionic_ctype;
const short *_tolower_tab_ptr = bionic_tolower;
const short *_toupper_tab_ptr = bionic_toupper;
static void build_ctype(void) {
  bionic_ctype[0] = 0; bionic_tolower[0] = -1; bionic_toupper[0] = -1;
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= 0x01; if (islower(c)) f |= 0x02; if (isdigit(c)) f |= 0x04;
    if (isspace(c)) f |= 0x08; if (ispunct(c)) f |= 0x10; if (iscntrl(c)) f |= 0x20;
    if (isxdigit(c)) f |= 0x40; if (c == ' ') f |= 0x80;
    bionic_ctype[c + 1] = f;
    bionic_tolower[c + 1] = (short)tolower(c);
    bionic_toupper[c + 1] = (short)toupper(c);
  }
}

void update_imports(void) { build_ctype(); }

/* ========================================================================== */
DynLibFunction dynlib_functions[] = {
  /* runtime / fortify / abi */
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__aeabi_atexit", (uintptr_t)&aeabi_atexit_fake },
  { "__errno", (uintptr_t)&errno_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "__assert2", (uintptr_t)&assert2_fake },
  { "__stack_chk_fail", (uintptr_t)&stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_val },
  { "__gnu_Unwind_Find_exidx", (uintptr_t)&find_exidx_fake },
  { "setjmp", (uintptr_t)&bionic_setjmp_fake },
  { "longjmp", (uintptr_t)&bionic_longjmp_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "raise", (uintptr_t)&raise_fake },
  { "abort", (uintptr_t)&abort_log },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dladdr", (uintptr_t)&dladdr_fake },
  { "gettid", (uintptr_t)&gettid_shim },
  { "__android_log_print", (uintptr_t)&android_log_print_fake },
  { "__android_log_write", (uintptr_t)&android_log_write_fake },

  /* bionic modules -> glibc allocator (opt-in accounting for load diagnosis) */
  { "malloc", (uintptr_t)&malloc_android },
  { "calloc", (uintptr_t)&calloc_android },
  { "realloc", (uintptr_t)&realloc_android },
  { "free", (uintptr_t)&free_android },

  /* __aeabi_mem* */
  { "__aeabi_memcpy", (uintptr_t)&aeabi_memcpy }, { "__aeabi_memcpy4", (uintptr_t)&aeabi_memcpy }, { "__aeabi_memcpy8", (uintptr_t)&aeabi_memcpy },
  { "__aeabi_memmove", (uintptr_t)&aeabi_memmove }, { "__aeabi_memmove4", (uintptr_t)&aeabi_memmove }, { "__aeabi_memmove8", (uintptr_t)&aeabi_memmove },
  { "__aeabi_memset", (uintptr_t)&aeabi_memset }, { "__aeabi_memset4", (uintptr_t)&aeabi_memset }, { "__aeabi_memset8", (uintptr_t)&aeabi_memset },
  { "__aeabi_memclr", (uintptr_t)&aeabi_memclr }, { "__aeabi_memclr4", (uintptr_t)&aeabi_memclr }, { "__aeabi_memclr8", (uintptr_t)&aeabi_memclr },

  /* bionic ctype tables */
  { "_ctype_", (uintptr_t)&_ctype_ptr },
  { "_tolower_tab_", (uintptr_t)&_tolower_tab_ptr },
  { "_toupper_tab_", (uintptr_t)&_toupper_tab_ptr },

  /* misc bionic libc via libc_shim */
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },

  /* Android-only SDL functions missing from native SDL2 */
  { "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath_fake },
  { "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath_fake },
  { "SDL_IsChromebook", (uintptr_t)&SDL_IsChromebook_fake },
  { "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_android },
  { "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow_trace },
  { "SDL_PollEvent", (uintptr_t)&SDL_PollEvent_trace },
  { "android_port_glCompressedTexImage2D",
    (uintptr_t)&android_port_glCompressedTexImage2D_mali },
  { "glTexImage2D", (uintptr_t)&glTexImage2D_mali },
  { "glDrawArrays", (uintptr_t)&glDrawArrays_trace },
  { "glDrawElements", (uintptr_t)&glDrawElements_trace },
  { "glClear", (uintptr_t)&glClear_trace },
  { "glCreateShader", (uintptr_t)&glCreateShader_trace },
  { "glShaderSource", (uintptr_t)&glShaderSource_trace },
  { "glAttachShader", (uintptr_t)&glAttachShader_trace },
  { "glUseProgram", (uintptr_t)&glUseProgram_trace },
  { "glCompileShader", (uintptr_t)&glCompileShader_trace },
  { "glLinkProgram", (uintptr_t)&glLinkProgram_trace },

  /* Aspyr-SDL internal symbols */
  { "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
  { "Android_Window", (uintptr_t)&Android_Window },
  { "g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w },
  { "g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h },

  /* filesystem / stdio: fix_path + bionic struct stat */
  { "fopen", (uintptr_t)&fopen_fake }, { "_fopen", (uintptr_t)&fopen_fake },
  { "open", (uintptr_t)&open_fake }, { "__open_2", (uintptr_t)&open2_fake },
  { "stat", (uintptr_t)&stat_fake }, { "fstat", (uintptr_t)&fstat_fake }, { "lstat", (uintptr_t)&lstat_fake },
  { "mkdir", (uintptr_t)&mkdir_fake }, { "remove", (uintptr_t)&remove_fake }, { "rename", (uintptr_t)&rename_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "_Z14FindFirstFileAPKcP17_WIN32_FIND_DATAA", (uintptr_t)&FindFirstFileA_fake },
  { "_Z13FindNextFileAPvP17_WIN32_FIND_DATAA", (uintptr_t)&FindNextFileA_fake },
  { "_Z9FindClosePv", (uintptr_t)&FindClose_fake },
  { "realpath", (uintptr_t)&realpath_fake }, { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "fclose", (uintptr_t)&fclose_fake }, { "fread", (uintptr_t)&fread_fake }, { "fwrite", (uintptr_t)&fwrite_fake },
  { "fputc", (uintptr_t)&fputc_fake }, { "fputs", (uintptr_t)&fputs_fake }, { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake }, { "fileno", (uintptr_t)&fileno_fake }, { "fseek", (uintptr_t)&fseek_fake },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "vfprintf", (uintptr_t)&vfprintf_fake }, { "fprintf", (uintptr_t)&fprintf_fake },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },

  /* AAsset emulation (APK assets/ next to the binary) */
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },

  /* OpenSL ES -> SDL bridge */
  { "SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY },
  { "SL_IID_RECORD", (uintptr_t)&sl_IID_RECORD },
  { "SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE },
  { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&sl_IID_ANDROIDCONFIGURATION },
  { "slCreateEngine", (uintptr_t)&slCreateEngine_shim },
  { "_ZN4FMOD6System4initEijPv", (uintptr_t)&fmod_system_init_android },

  /* GLES2 float-by-value (softfp thunks) */
  { "glClearColor", (uintptr_t)&sf_glClearColor },
  { "glClearDepthf", (uintptr_t)&sf_glClearDepthf },
  { "glDepthRangef", (uintptr_t)&sf_glDepthRangef },
  { "glBlendColor", (uintptr_t)&sf_glBlendColor },
  { "glLineWidth", (uintptr_t)&sf_glLineWidth },
  { "glPolygonOffset", (uintptr_t)&sf_glPolygonOffset },
  { "glSampleCoverage", (uintptr_t)&sf_glSampleCoverage },
  { "glTexParameterf", (uintptr_t)&sf_glTexParameterf },
  { "glUniform1f", (uintptr_t)&sf_glUniform1f },
  { "glUniform2f", (uintptr_t)&sf_glUniform2f },
  { "glUniform3f", (uintptr_t)&sf_glUniform3f },
  { "glUniform4f", (uintptr_t)&sf_glUniform4f },
  { "glVertexAttrib1f", (uintptr_t)&sf_glVertexAttrib1f },
  { "glVertexAttrib2f", (uintptr_t)&sf_glVertexAttrib2f },
  { "glVertexAttrib3f", (uintptr_t)&sf_glVertexAttrib3f },
  { "glVertexAttrib4f", (uintptr_t)&sf_glVertexAttrib4f },

  /* GLES2 *OES framebuffer aliases -> core */
  { "glBindFramebufferOES", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbufferOES", (uintptr_t)&glBindRenderbuffer },
  { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus },
  { "glFramebufferRenderbufferOES", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D },
  { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffersOES", (uintptr_t)&glGenRenderbuffers },
  { "glRenderbufferStorageOES", (uintptr_t)&glRenderbufferStorage },
  { "glMapBufferOES", (uintptr_t)&glMapBufferOES_w },
  { "glUnmapBufferOES", (uintptr_t)&glUnmapBufferOES_w },

  /* pthread bridge (bionic 4-byte -> real glibc objects) */
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_cond_timedwait_monotonic_np", (uintptr_t)&pthread_cond_timedwait_monotonic_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },

  /* semaphores (pointer indirection behind bionic storage) */
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
};
size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);
