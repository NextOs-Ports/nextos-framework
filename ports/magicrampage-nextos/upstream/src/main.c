#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "audio_backend.h"
#include "jni_min.h"
#include "so_util.h"
#include "util.h"

#define CXX_SO "libc++_shared.so"
#define CRYPTO_SO "libcrypto.so"
#define FMOD_SO "libfmod.so"
#define GAME_SO "libmachine.so"

#define CXX_HEAP_MB 64
#define CRYPTO_HEAP_MB 96
#define FMOD_HEAP_MB 48
#define GAME_HEAP_MB 160

extern DynLibFunction magic_stub_table[];
extern const int magic_stub_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

static DynLibFunction *g_base;
static int g_base_n;
static void *g_game_text;
static size_t g_game_text_size;
static SDL_GameController *g_controller;
static unsigned g_input_evidence;
static char **g_process_argv;

typedef void (*fn_void0)(void *, void *);
typedef void (*fn_resize)(void *, void *, int, int);
typedef void (*fn_start)(void *, void *, void *, void *, void *, int, int);
typedef void *(*fn_main_loop)(void *, void *, void *);
typedef void (*fn_set_shared)(void *, void *, void *, void *);
typedef struct {
  void *ptr;
  void *ctrl;
} magic_shared_ptr;
typedef float (*fn_compute_elapsed_f)(magic_shared_ptr *);
typedef unsigned char (*fn_engine_is_loaded)(void *);
typedef void (*fn_key_state_update)(void *, int);

static fn_key_state_update g_key_state_update;

enum {
  GS_KEY_UP = 0,
  GS_KEY_DOWN = 1,
  GS_KEY_LEFT = 2,
  GS_KEY_RIGHT = 3,
  GS_KEY_SPACE = 6,
  GS_KEY_ENTER = 7,
  GS_KEY_ESCAPE = 13,
  GS_KEY_A = 43,
  GS_KEY_D = 46,
  GS_KEY_S = 61,
  GS_KEY_W = 65,
};

static void audio_noop(void) {}
static int audio_true(void) { return 1; }
static int audio_false(void) { return 0; }
static float audio_one(void) { return 1.0f; }

static int ptr_range_readable(const void *ptr, size_t len) {
  uintptr_t start = (uintptr_t)ptr;
  uintptr_t end = start + len;
  char line[256];
  FILE *f;

  if (!ptr || !len || end < start || start < 0x10000)
    return 0;

  f = fopen("/proc/self/maps", "r");
  if (!f)
    return 1;

  while (fgets(line, sizeof(line), f)) {
    unsigned long lo = 0, hi = 0;
    char perms[5] = {0};
    if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3 &&
        perms[0] == 'r' && start >= (uintptr_t)lo && end <= (uintptr_t)hi) {
      fclose(f);
      return 1;
    }
  }
  fclose(f);
  return 0;
}

static int cxx_string_copy_candidate(char *out, size_t out_sz, const char *p,
                                     size_t n) {
  size_t copy_n;
  if (!out_sz)
    return 0;
  out[0] = 0;
  if (n >= 512 || (!p && n))
    return 0;
  if (n && !ptr_range_readable(p, n))
    return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x20 || c >= 0x7f)
      return 0;
  }
  copy_n = n < out_sz - 1 ? n : out_sz - 1;
  if (copy_n)
    memcpy(out, p, copy_n);
  out[copy_n] = 0;
  return 1;
}

static int cxx_string_read(const void *obj, char *out, size_t out_sz) {
  const uint8_t *s = (const uint8_t *)obj;
  const uintptr_t *q = (const uintptr_t *)obj;
  if (!out_sz)
    return 0;
  out[0] = 0;
  if (!s || !ptr_range_readable(obj, 24))
    return 0;

  if ((q[0] & 1) && q[1] < 512 && ptr_range_readable((const void *)q[2], q[1]))
    return cxx_string_copy_candidate(out, out_sz, (const char *)q[2], q[1]);

  if (q[0] >= 0x10000 && q[0] < (1ULL << 48) && q[1] < 512 &&
      ptr_range_readable((const void *)q[0], q[1]))
    return cxx_string_copy_candidate(out, out_sz, (const char *)q[0], q[1]);

  uint8_t b0 = s[0];
  if (!(b0 & 1)) {
    size_t n = b0 >> 1;
    if (n < 23)
      return cxx_string_copy_candidate(out, out_sz, (const char *)s + 1, n);
  }

  if (s[23] < 23) {
    size_t n = s[23];
    return cxx_string_copy_candidate(out, out_sz, (const char *)s, n);
  }
  return 0;
}

static void audio_log_bad_name(const void *name) {
  static unsigned bad_name_logs;
  uint8_t bytes[24];
  const uintptr_t *q = (const uintptr_t *)bytes;

  if (bad_name_logs++ >= 12)
    return;
  if (!name || !ptr_range_readable(name, sizeof(bytes))) {
    fprintf(stderr, "[audio] nome invalido ptr=%p\n", name);
    return;
  }
  memcpy(bytes, name, sizeof(bytes));
  fprintf(stderr,
          "[audio] nome nao decodificado ptr=%p bytes="
          "%02x %02x %02x %02x %02x %02x %02x %02x "
          "%02x %02x %02x %02x %02x %02x %02x %02x "
          "%02x %02x %02x %02x %02x %02x %02x %02x q=[%lx,%lx,%lx]\n",
          name, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
          bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
          bytes[12], bytes[13], bytes[14], bytes[15], bytes[16], bytes[17],
          bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
          (unsigned long)q[0], (unsigned long)q[1], (unsigned long)q[2]);
}

static int audio_name_arg(const void *name, char *buf, size_t buf_sz) {
  if (cxx_string_read(name, buf, buf_sz))
    return 1;
  if (buf_sz)
    buf[0] = 0;
  audio_log_bad_name(name);
  return 0;
}

static int audio_load_music_hook(const void *name) {
  char buf[512];
  if (!audio_name_arg(name, buf, sizeof(buf)) || !buf[0])
    return 1;
  return audio_backend_load_music(buf);
}

static int audio_load_sound_hook(const void *name) {
  char buf[512];
  if (!audio_name_arg(name, buf, sizeof(buf)) || !buf[0])
    return 1;
  return audio_backend_load_sound(buf);
}

static void audio_play_sample_hook(const void *name) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_play_sample(buf, 0);
}

static void audio_loop_sample_hook(const void *name, int loop) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_play_sample(buf, loop ? -1 : 0);
}

static void audio_stop_sample_hook(const void *name) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_stop_sample(buf);
}

static void audio_pause_sample_hook(const void *name) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_pause_sample(buf);
}

static void audio_set_sample_volume_hook(const void *name, float volume) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_set_sample_volume(buf, volume);
}

static void audio_set_sample_pan_hook(const void *name, float pan) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_set_sample_pan(buf, pan);
}

static void audio_set_sample_speed_hook(const void *name, float speed) {
  char buf[512];
  if (audio_name_arg(name, buf, sizeof(buf)) && buf[0])
    audio_backend_set_sample_speed(buf, speed);
}

static int audio_sample_exists_hook(const void *name) {
  char buf[512];
  if (!audio_name_arg(name, buf, sizeof(buf)) || !buf[0])
    return 0;
  return audio_backend_sample_exists(buf);
}

static int audio_is_sample_playing_hook(const void *name) {
  char buf[512];
  if (!audio_name_arg(name, buf, sizeof(buf)) || !buf[0])
    return 0;
  return audio_backend_is_sample_playing(buf);
}

static void audio_set_global_volume_hook(float volume) {
  audio_backend_set_global_volume(volume);
}

static float audio_get_global_volume_hook(void) {
  return audio_backend_get_global_volume();
}

static void audio_set_sfx_volume_hook(float volume) {
  audio_backend_set_sound_effect_volume(volume);
}

static float audio_get_sfx_volume_hook(void) {
  return audio_backend_get_sound_effect_volume();
}

static int video_handle_events_shim(void *self) {
  if (self)
    ((uint8_t *)self)[328] = 0;
  return 0;
}

static void update_android_key(void *input, unsigned key, int down) {
  if (!input || !g_key_state_update)
    return;
  g_key_state_update((uint8_t *)input + 0xc0 + key * 8, down ? 1 : 0);
}

static int controller_button_down(SDL_GameControllerButton button) {
  return g_controller && SDL_GameControllerGetButton(g_controller, button);
}

static int controller_axis_pressed(SDL_GameControllerAxis axis, int sign) {
  if (!g_controller)
    return 0;
  int value = SDL_GameControllerGetAxis(g_controller, axis);
  return sign < 0 ? value < -12000 : value > 12000;
}

static int input_update_sdl(void *self) {
  if (self) {
    float *accel_x = (float *)((uint8_t *)self + 168);
    float *accel_y = (float *)((uint8_t *)self + 172);
    float *accel_z = (float *)((uint8_t *)self + 176);
    *accel_x = 0.0f;
    *accel_y = 0.0f;
    *accel_z = 0.0f;
  }

  const Uint8 *keys = SDL_GetKeyboardState(NULL);
  int left = keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A] ||
             controller_button_down(SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
             controller_axis_pressed(SDL_CONTROLLER_AXIS_LEFTX, -1);
  int right = keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D] ||
              controller_button_down(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
              controller_axis_pressed(SDL_CONTROLLER_AXIS_LEFTX, 1);
  int up = keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W] ||
           controller_button_down(SDL_CONTROLLER_BUTTON_DPAD_UP) ||
           controller_button_down(SDL_CONTROLLER_BUTTON_A) ||
           controller_axis_pressed(SDL_CONTROLLER_AXIS_LEFTY, -1);
  int down = keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S] ||
             controller_button_down(SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
             controller_axis_pressed(SDL_CONTROLLER_AXIS_LEFTY, 1);
  int attack = keys[SDL_SCANCODE_SPACE] ||
               controller_button_down(SDL_CONTROLLER_BUTTON_X) ||
               controller_button_down(SDL_CONTROLLER_BUTTON_Y) ||
               controller_button_down(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  int accept = keys[SDL_SCANCODE_RETURN] ||
               controller_button_down(SDL_CONTROLLER_BUTTON_START) ||
               controller_button_down(SDL_CONTROLLER_BUTTON_B);
  int cancel = keys[SDL_SCANCODE_ESCAPE] ||
               controller_button_down(SDL_CONTROLLER_BUTTON_BACK);

  if ((left || right || up || down) && !(g_input_evidence & 1u)) {
    fprintf(stderr, "[input] evidence movement=OK\n");
    g_input_evidence |= 1u;
  }
  if (up && !(g_input_evidence & 2u)) {
    fprintf(stderr, "[input] evidence jump=OK\n");
    g_input_evidence |= 2u;
  }
  if (attack && !(g_input_evidence & 4u)) {
    fprintf(stderr, "[input] evidence attack=OK\n");
    g_input_evidence |= 4u;
  }
  if (accept && !(g_input_evidence & 8u)) {
    fprintf(stderr, "[input] evidence accept=OK\n");
    g_input_evidence |= 8u;
  }
  if (cancel && !(g_input_evidence & 16u)) {
    fprintf(stderr, "[input] evidence cancel=OK\n");
    g_input_evidence |= 16u;
  }

  update_android_key(self, GS_KEY_LEFT, left);
  update_android_key(self, GS_KEY_RIGHT, right);
  update_android_key(self, GS_KEY_UP, up);
  update_android_key(self, GS_KEY_DOWN, down);
  update_android_key(self, GS_KEY_A, left);
  update_android_key(self, GS_KEY_D, right);
  update_android_key(self, GS_KEY_W, up);
  update_android_key(self, GS_KEY_S, down);
  update_android_key(self, GS_KEY_SPACE, attack);
  update_android_key(self, GS_KEY_ENTER, accept);
  update_android_key(self, GS_KEY_ESCAPE, cancel);
  return 1;
}

static magic_shared_ptr *g_video;
static magic_shared_ptr *g_audio;
static magic_shared_ptr *g_input;
static magic_shared_ptr *g_engine;
static magic_shared_ptr *g_application;
static void *g_last_camera_pos;
static void *g_last_background_color;
static unsigned char *g_engine_loaded;
static fn_compute_elapsed_f g_compute_elapsed_f;
static fn_engine_is_loaded g_engine_is_loaded_fn;
static unsigned g_native_frame_log_state;

static void *sp_get(const magic_shared_ptr *sp) {
  return sp ? sp->ptr : NULL;
}

static void *vtable_fn(void *self, size_t byte_offset) {
  if (!self)
    return NULL;
  void **vt = *(void ***)self;
  if (!vt)
    return NULL;
  return vt[byte_offset / sizeof(void *)];
}

static void vcall_void0(void *self, size_t byte_offset) {
  void *fn = vtable_fn(self, byte_offset);
  if (fn)
    ((void (*)(void *))fn)(self);
}

static int vcall_int0(void *self, size_t byte_offset) {
  void *fn = vtable_fn(self, byte_offset);
  if (!fn)
    return 0;
  return ((int (*)(void *))fn)(self);
}

static void vcall_float1(void *self, size_t byte_offset, float arg) {
  void *fn = vtable_fn(self, byte_offset);
  if (fn)
    ((void (*)(void *, float))fn)(self, arg);
}

static void vcall_sret0(void *self, size_t byte_offset, void *out) {
  void *fn = vtable_fn(self, byte_offset);
  if (!fn || !out)
    return;
#if defined(__aarch64__)
  register void *x0 __asm__("x0") = self;
  register void *x8 __asm__("x8") = out;
  register void *x16 __asm__("x16") = fn;
  __asm__ volatile("blr %2"
                   : "+r"(x0), "+r"(x8)
                   : "r"(x16)
                   : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9",
                     "x10", "x11", "x12", "x13", "x14", "x15", "x17",
                     "x18", "v0", "v1", "v2", "v3", "v4", "v5", "v6",
                     "v7", "memory", "cc");
#else
  (void)fn;
#endif
}

static void resolve_native_frame_symbols(void) {
  g_video = (magic_shared_ptr *)so_find_addr_safe("video");
  g_audio = (magic_shared_ptr *)so_find_addr_safe("audio");
  g_input = (magic_shared_ptr *)so_find_addr_safe("input");
  g_engine = (magic_shared_ptr *)so_find_addr_safe("engine");
  g_application = (magic_shared_ptr *)so_find_addr_safe("application");
  g_last_camera_pos = (void *)so_find_addr_safe("lastCameraPos");
  g_last_background_color = (void *)so_find_addr_safe("lastBackgroundColor");
  g_engine_loaded = (unsigned char *)so_find_addr_safe("engineLoaded");
  g_compute_elapsed_f =
      (fn_compute_elapsed_f)so_find_addr_safe(
          "_ZN4gs2d19ComputeElapsedTimeFEN5boost10shared_ptrINS_11ApplicationEEE");
  g_engine_is_loaded_fn =
      (fn_engine_is_loaded)so_find_addr_safe(
          "_ZNK4gs2d9ETHEngine20IsScriptEngineLoadedEv");
  g_key_state_update =
      (fn_key_state_update)so_find_addr_safe("_ZN4gs2d15KeyStateManager6UpdateEb");
  fprintf(stderr,
          "[native-frame] video=%p audio=%p input=%p engine=%p app=%p "
          "camera=%p bg=%p engineLoaded=%p elapsed=%p isLoaded=%p keyUpdate=%p\n",
          (void *)g_video, (void *)g_audio, (void *)g_input, (void *)g_engine,
          (void *)g_application, g_last_camera_pos, g_last_background_color,
          (void *)g_engine_loaded, (void *)g_compute_elapsed_f,
          (void *)g_engine_is_loaded_fn, (void *)g_key_state_update);
}

static void *main_loop_native(void *env, void *cls, void *cmd) {
  (void)env;
  (void)cls;
  (void)cmd;

  void *video = sp_get(g_video);
  void *audio = sp_get(g_audio);
  void *input = sp_get(g_input);
  void *engine = sp_get(g_engine);
  void *application = sp_get(g_application);

  vcall_int0(video, 32);   /* Video::HandleEvents */
  vcall_void0(audio, 80);  /* Audio::Update */
  vcall_int0(input, 136);  /* Input::Update */

  if (!engine || !application || !g_compute_elapsed_f || !g_engine_is_loaded_fn) {
    if (g_native_frame_log_state == 0) {
      fprintf(stderr,
              "[native-frame] aguardando objetos: video=%p audio=%p input=%p "
              "engine=%p app=%p elapsed=%p isLoaded=%p\n",
              video, audio, input, engine, application,
              (void *)g_compute_elapsed_f, (void *)g_engine_is_loaded_fn);
      g_native_frame_log_state = 1;
    }
    return NULL;
  }
  if (!g_engine_is_loaded_fn(engine)) {
    if (g_native_frame_log_state < 2) {
      fprintf(stderr, "[native-frame] engine ainda sem script carregado\n");
      g_native_frame_log_state = 2;
    }
    return NULL;
  }

  if (g_native_frame_log_state < 3) {
    fprintf(stderr,
            "[native-frame] primeiro frame carregado: video=%p engine=%p "
            "app=%p\n",
            video, engine, application);
    g_native_frame_log_state = 3;
  }

  if (g_engine_loaded)
    *g_engine_loaded = 1;

  magic_shared_ptr elapsed_source = {video, g_video ? g_video->ctrl : NULL};
  float dt = g_compute_elapsed_f(&elapsed_source);
  if (!(dt > 0.0f))
    dt = 16.666667f;
  if (dt > 1000.0f)
    dt = 1000.0f;

  vcall_float1(application, 8, dt); /* Application::Update */
  if (g_last_camera_pos) {
    uint8_t camera[16] = {0};
    vcall_sret0(video, 288, camera); /* Video::GetCameraPos */
    memcpy(g_last_camera_pos, camera, 8);
  }
  if (g_last_background_color) {
    uint8_t color[16] = {0};
    vcall_sret0(video, 200, color); /* Video::GetBackgroundColor */
    memcpy(g_last_background_color, color, 16);
  }
  vcall_void0(application, 16);     /* Application::Render */
  return NULL;
}

static void hook_game_symbol(const char *sym, void *fn, const char *label) {
  uintptr_t addr = so_find_addr_safe(sym);
  if (!addr) {
    fprintf(stderr, "[runtime-hooks] simbolo ausente: %s\n", label);
    return;
  }
  hook_arm64(addr, (uintptr_t)fn);
  fprintf(stderr, "[runtime-hooks] %s @ 0x%lx\n", label,
          (unsigned long)addr);
}

static void patch_game_vtable_slot(const char *sym, size_t offset, void *fn,
                                   const char *label) {
  uintptr_t vtable = so_find_addr_safe(sym);
  if (!vtable) {
    fprintf(stderr, "[runtime-hooks] vtable ausente: %s\n", label);
    return;
  }
  uintptr_t *slot = (uintptr_t *)(vtable + offset);
  fprintf(stderr, "[runtime-hooks] %s vslot+0x%zx %p -> %p\n", label, offset,
          (void *)*slot, fn);
  *slot = (uintptr_t)fn;
}

static void install_runtime_hooks(void) {
  fprintf(stderr, "[runtime-hooks] instalando hooks nativos\n");
  so_make_text_writable();
  hook_game_symbol(
      "_ZN16ETHScriptWrapper9LoadMusicERKNSt6__ndk112basic_stringIcNS0_11char_"
      "traitsIcEENS0_9allocatorIcEEEE",
      audio_load_music_hook, "LoadMusic");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper15LoadSoundEffectERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEE",
      audio_load_sound_hook, "LoadSoundEffect");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper10PlaySampleERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEE",
      audio_play_sample_hook, "PlaySample");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper10LoopSampleERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEEb",
      audio_loop_sample_hook, "LoopSample");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper10StopSampleERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEE",
      audio_stop_sample_hook, "StopSample");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper11PauseSampleERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEE",
      audio_pause_sample_hook, "PauseSample");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper15SetSampleVolumeERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEEf",
      audio_set_sample_volume_hook, "SetSampleVolume");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper12SetSamplePanERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEEf",
      audio_set_sample_pan_hook, "SetSamplePan");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper14SetSampleSpeedERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEEf",
      audio_set_sample_speed_hook, "SetSampleSpeed");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper12SampleExistsERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEE",
      audio_sample_exists_hook, "SampleExists");
  hook_game_symbol(
      "_ZN16ETHScriptWrapper15IsSamplePlayingERKNSt6__ndk112basic_stringIcNS0_"
      "11char_traitsIcEENS0_9allocatorIcEEEE",
      audio_is_sample_playing_hook, "IsSamplePlaying");
  hook_game_symbol("_ZN16ETHScriptWrapper15SetGlobalVolumeEf",
                   audio_set_global_volume_hook, "SetGlobalVolume");
  hook_game_symbol("_ZN16ETHScriptWrapper15GetGlobalVolumeEv",
                   audio_get_global_volume_hook, "GetGlobalVolume");
  hook_game_symbol("_ZN16ETHScriptWrapper20SetSoundEffectVolumeEf",
                   audio_set_sfx_volume_hook, "SetSoundEffectVolume");
  hook_game_symbol("_ZN16ETHScriptWrapper20GetSoundEffectVolumeEv",
                   audio_get_sfx_volume_hook, "GetSoundEffectVolume");

  hook_game_symbol("_ZN4gs2d14FMAudioContext6UpdateEv", audio_noop,
                   "FMAudioContext::Update");
  hook_game_symbol("_ZN4gs2d14FMAudioContext7SuspendEv", audio_noop,
                   "FMAudioContext::Suspend");
  hook_game_symbol("_ZN4gs2d14FMAudioContext6ResumeEv", audio_noop,
                   "FMAudioContext::Resume");
  hook_game_symbol("_ZN4gs2d14FMAudioContext7SetMuteEb", audio_noop,
                   "FMAudioContext::SetMute");
  hook_game_symbol("_ZNK4gs2d14FMAudioContext6IsMuteEv", audio_false,
                   "FMAudioContext::IsMute");
  hook_game_symbol("_ZN4gs2d14FMAudioContext15SetGlobalVolumeEf", audio_noop,
                   "FMAudioContext::SetGlobalVolume");
  hook_game_symbol("_ZNK4gs2d14FMAudioContext15GetGlobalVolumeEv", audio_one,
                   "FMAudioContext::GetGlobalVolume");
  hook_game_symbol("_ZN4gs2d14FMAudioContext20SetSoundEffectVolumeEf",
                   audio_noop, "FMAudioContext::SetSoundEffectVolume");
  hook_game_symbol("_ZNK4gs2d14FMAudioContext20GetSoundEffectVolumeEv",
                   audio_one, "FMAudioContext::GetSoundEffectVolume");

  hook_game_symbol("_ZN4gs2d12AndroidInput6UpdateEv", input_update_sdl,
                   "AndroidInput::Update");
  hook_game_symbol("_ZN4gs2d10GLES2Video12HandleEventsEv",
                   video_handle_events_shim, "GLES2Video::HandleEvents");
  patch_game_vtable_slot("_ZTVN4gs2d10GLES2VideoE", 0x30,
                         video_handle_events_shim,
                         "GLES2Video::HandleEvents");
  patch_game_vtable_slot("_ZTVN4gs2d17AndroidGLES2VideoE", 0x30,
                         video_handle_events_shim,
                         "AndroidGLES2Video::HandleEvents");
  so_flush_caches();
  so_make_text_executable();
}

static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc ? (uintptr_t)uc->uc_mcontext.pc : 0;
  uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
  uintptr_t lr = uc ? (uintptr_t)uc->uc_mcontext.regs[30] : 0;
  fprintf(stderr, "\n=== MAGIC CRASH sig=%d fault=%p pc=%p ===\n", sig,
          (void *)fault, (void *)pc);
  if (g_game_text && pc >= (uintptr_t)g_game_text &&
      pc < (uintptr_t)g_game_text + g_game_text_size) {
    fprintf(stderr, "PC em %s + 0x%lx\n", GAME_SO,
            (unsigned long)(pc - (uintptr_t)g_game_text));
  } else {
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
      fprintf(stderr, "PC em %s", di.dli_fname);
      if (di.dli_sname)
        fprintf(stderr, " %s+0x%lx", di.dli_sname,
                (unsigned long)(pc - (uintptr_t)di.dli_saddr));
      fprintf(stderr, "\n");
    }
    else
      fprintf(stderr, "PC fora do %s text\n", GAME_SO);
  }
  if (lr) {
    Dl_info di;
    fprintf(stderr, "LR=%p", (void *)lr);
    if (g_game_text && lr >= (uintptr_t)g_game_text &&
        lr < (uintptr_t)g_game_text + g_game_text_size)
      fprintf(stderr, " (%s+0x%lx)", GAME_SO,
              (unsigned long)(lr - (uintptr_t)g_game_text));
    else if (dladdr((void *)lr, &di) && di.dli_fname) {
      fprintf(stderr, " (%s", di.dli_fname);
      if (di.dli_sname)
        fprintf(stderr, " %s+0x%lx", di.dli_sname,
                (unsigned long)(lr - (uintptr_t)di.dli_saddr));
      fprintf(stderr, ")");
    }
    fputc('\n', stderr);
  }
#if defined(__aarch64__)
  if (uc) {
    for (int i = 0; i < 31; i++) {
      fprintf(stderr, " x%-2d=%016lx", i,
              (unsigned long)uc->uc_mcontext.regs[i]);
      if (i % 3 == 2)
        fputc('\n', stderr);
    }
    fprintf(stderr, "\n sp=%016lx pc=%016lx\n",
            (unsigned long)uc->uc_mcontext.sp, (unsigned long)pc);

    fprintf(stderr, "Backtrace FP/LR:\n");
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int f = 0; f < 24 && fp; f++) {
      uintptr_t *p = (uintptr_t *)fp;
      uintptr_t next = p[0];
      uintptr_t ret = p[1];
      if (!ret)
        break;
      fprintf(stderr, "  #%-2d lr %p", f, (void *)ret);
      if (g_game_text && ret >= (uintptr_t)g_game_text &&
          ret < (uintptr_t)g_game_text + g_game_text_size)
        fprintf(stderr, " (%s+0x%lx)", GAME_SO,
                (unsigned long)(ret - (uintptr_t)g_game_text));
      else {
        Dl_info di;
        if (dladdr((void *)ret, &di) && di.dli_fname) {
          fprintf(stderr, " (%s", di.dli_fname);
          if (di.dli_sname)
            fprintf(stderr, " %s+0x%lx", di.dli_sname,
                    (unsigned long)(ret - (uintptr_t)di.dli_saddr));
          fprintf(stderr, ")");
        }
      }
      fputc('\n', stderr);
      if (next <= fp || next - fp > (1UL << 20))
        break;
      fp = next;
    }
  }
#endif
  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
      unsigned long a, b;
      if (sscanf(line, "%lx-%lx", &a, &b) == 2 && pc >= a && pc < b) {
        fprintf(stderr, ">>> PC map: %s", line);
        break;
      }
    }
    fclose(maps);
  }
  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {"libSDL2-2.0.so.0", "libGLESv2.so",
                               "libEGL.so",        "libz.so.1",
                               "libm.so.6",        NULL};
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static void build_base_table(void) {
  g_base_n = magic_stub_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  if (!g_base) {
    fprintf(stderr, "malloc base table falhou\n");
    exit(1);
  }
  memcpy(g_base, magic_stub_table, sizeof(DynLibFunction) * magic_stub_count);
  memcpy(g_base + magic_stub_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

static DynLibFunction *combine_tables(int *out_n, DynLibFunction *a, int an,
                                      DynLibFunction *b, int bn,
                                      DynLibFunction *c, int cn,
                                      DynLibFunction *d, int dn) {
  int n = an + bn + cn + dn;
  DynLibFunction *out = malloc(sizeof(DynLibFunction) * (n ? n : 1));
  if (!out) {
    fprintf(stderr, "malloc combined table falhou\n");
    exit(1);
  }
  int k = 0;
  if (a && an) {
    memcpy(out + k, a, sizeof(DynLibFunction) * an);
    k += an;
  }
  if (b && bn) {
    memcpy(out + k, b, sizeof(DynLibFunction) * bn);
    k += bn;
  }
  if (c && cn) {
    memcpy(out + k, c, sizeof(DynLibFunction) * cn);
    k += cn;
  }
  if (d && dn) {
    memcpy(out + k, d, sizeof(DynLibFunction) * dn);
    k += dn;
  }
  *out_n = k;
  return out;
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl,
                        int n) {
  size_t heap_size = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    fprintf(stderr, "mmap %s %dMB falhou: %s\n", name, heap_mb,
            strerror(errno));
    exit(1);
  }

  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", name, heap,
          heap_mb);
  if (so_load(name, heap, heap_size) < 0) {
    fprintf(stderr, "so_load(%s) falhou\n", name);
    exit(1);
  }
  if (so_relocate() < 0) {
    fprintf(stderr, "so_relocate(%s) falhou\n", name);
    exit(1);
  }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name, text_base,
          text_size, data_base, data_size);
}

static void mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0775);
      *p = '/';
    }
  }
  mkdir(tmp, 0775);
}

static void path_join(char *out, size_t n, const char *a, const char *b) {
  snprintf(out, n, "%s/%s", a, b);
}

static void *exit_deadline_thread(void *arg) {
  (void)arg;
  sleep(2);
  _exit(0);
  return NULL;
}

static void retry_with_sdl_provider(const char *stage) {
  const char *candidate = getenv("MAGICRAMPAGE_SDL_PROVIDER");
  struct stat provider_stat;
  void *provider;

  if (!candidate || !*candidate || !g_process_argv ||
      getenv("MAGICRAMPAGE_PROVIDER_RECOVERED") ||
      getenv("SDL_VIDEO_EGL_DRIVER") || getenv("SDL_VIDEO_GL_DRIVER"))
    return;
  if (lstat(candidate, &provider_stat) != 0 ||
      !S_ISREG(provider_stat.st_mode)) {
    fprintf(stderr, "[video] provider recovery rejected: unsafe candidate\n");
    return;
  }
  provider = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
  if (!provider || !dlsym(provider, "eglInitialize") ||
      !dlsym(provider, "glGetString")) {
    fprintf(stderr,
            "[video] provider recovery rejected: missing EGL/GLES symbols\n");
    if (provider)
      dlclose(provider);
    return;
  }
  dlclose(provider);
  if (setenv("SDL_VIDEO_EGL_DRIVER", candidate, 1) != 0 ||
      setenv("SDL_VIDEO_GL_DRIVER", candidate, 1) != 0 ||
      setenv("MAGICRAMPAGE_PROVIDER_RECOVERED", "1", 1) != 0)
    return;
  fprintf(stderr, "[video] provider recovery retry stage=%s\n", stage);
  fflush(stderr);
  execv("/proc/self/exe", g_process_argv);
  fprintf(stderr, "[video] provider recovery exec failed: %s\n",
          strerror(errno));
}

static int init_sdl(SDL_Window **out_window, SDL_GLContext *out_context,
                    int *out_w, int *out_h) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
               SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
    return 0;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

  SDL_Window *window =
      SDL_CreateWindow("Magic Rampage", SDL_WINDOWPOS_UNDEFINED,
                       SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow falhou driver=%s: %s\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown",
            SDL_GetError());
    retry_with_sdl_provider("window-create");
    return 0;
  }
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  if (!ctx) {
    fprintf(stderr, "SDL_GL_CreateContext falhou: %s\n", SDL_GetError());
    return 0;
  }
  SDL_GL_MakeCurrent(window, ctx);
  SDL_GL_SetSwapInterval(1);
  {
    typedef const GLubyte *(*fn_gl_get_string)(GLenum);
    fn_gl_get_string get_string =
        (fn_gl_get_string)SDL_GL_GetProcAddress("glGetString");
    const char *vendor = get_string ? (const char *)get_string(GL_VENDOR) : NULL;
    const char *renderer = get_string ? (const char *)get_string(GL_RENDERER) : NULL;
    const char *version = get_string ? (const char *)get_string(GL_VERSION) : NULL;
    fprintf(stderr, "[video] driver=%s vendor=%s renderer=%s version=%s\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown",
            vendor ? vendor : "unknown", renderer ? renderer : "unknown",
            version ? version : "unknown");
  }
  fprintf(stderr, "[input] joysticks=%d mapping=%s\n", SDL_NumJoysticks(),
          getenv("SDL_GAMECONTROLLERCONFIG") ? "present" : "absent");
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_controller = SDL_GameControllerOpen(i);
      if (g_controller) {
        fprintf(stderr, "SDL controller: %s\n",
                SDL_GameControllerName(g_controller));
        break;
      }
    }
  }
  SDL_GetWindowSize(window, out_w, out_h);
  if (*out_w <= 0)
    *out_w = 1280;
  if (*out_h <= 0)
    *out_h = 720;
  *out_window = window;
  *out_context = ctx;
  fprintf(stderr, "SDL/GLES pronto: %dx%d\n", *out_w, *out_h);
  return 1;
}

static void call_set_shared(fn_set_shared set_shared, const char *key,
                            const char *value) {
  if (!set_shared)
    return;
  void *k = jni_string(key);
  void *v = jni_string(value);
  set_shared(jni_env(), jni_class(), k, v);
  jni_string_free(k);
  jni_string_free(v);
}

int main(int argc, char **argv) {
  volatile char c = g_bionic_guard_pad[0];
  (void)c;
  install_crash_handler();
  g_process_argv = argv;

  char gamedir[PATH_MAX];
  if (argc > 1)
    snprintf(gamedir, sizeof(gamedir), "%s", argv[1]);
  else if (!getcwd(gamedir, sizeof(gamedir)))
    snprintf(gamedir, sizeof(gamedir), ".");
  chdir(gamedir);

  char apk_path[PATH_MAX];
  char user_path[PATH_MAX];
  char global_path[PATH_MAX];
  path_join(apk_path, sizeof(apk_path), gamedir, "game.apk");
  path_join(user_path, sizeof(user_path), gamedir, "user");
  path_join(global_path, sizeof(global_path), gamedir, "user-global");
  mkdir_p(user_path);
  mkdir_p(global_path);

  fprintf(stderr, "=== Magic Rampage Android so-loader / NextOS aarch64 ===\n");
  fprintf(stderr, "gamedir=%s\napk=%s\n", gamedir, apk_path);

  preload_device_libs();
  build_base_table();

  load_module(CXX_SO, CXX_HEAP_MB, g_base, g_base_n);
  int cxx_n = 0;
  DynLibFunction *cxx_tbl = so_snapshot_symbols(&cxx_n);
  fprintf(stderr, "libc++ symbols: %d\n", cxx_n);

  int cxx_comb_n = 0;
  DynLibFunction *cxx_comb =
      combine_tables(&cxx_comb_n, g_base, g_base_n, cxx_tbl, cxx_n, NULL, 0,
                     NULL, 0);

  load_module(CRYPTO_SO, CRYPTO_HEAP_MB, cxx_comb, cxx_comb_n);
  int crypto_n = 0;
  DynLibFunction *crypto_tbl = so_snapshot_symbols(&crypto_n);
  fprintf(stderr, "crypto symbols: %d\n", crypto_n);

  int fmod_comb_n = 0;
  DynLibFunction *fmod_comb = combine_tables(
      &fmod_comb_n, g_base, g_base_n, cxx_tbl, cxx_n, crypto_tbl, crypto_n,
      NULL, 0);
  load_module(FMOD_SO, FMOD_HEAP_MB, fmod_comb, fmod_comb_n);
  int fmod_n = 0;
  DynLibFunction *fmod_tbl = so_snapshot_symbols(&fmod_n);
  fprintf(stderr, "fmod symbols: %d\n", fmod_n);

  int game_comb_n = 0;
  DynLibFunction *game_comb = combine_tables(
      &game_comb_n, g_base, g_base_n, cxx_tbl, cxx_n, crypto_tbl, crypto_n,
      fmod_tbl, fmod_n);
  load_module(GAME_SO, GAME_HEAP_MB, game_comb, game_comb_n);
  g_game_text = text_base;
  g_game_text_size = text_size;
  install_runtime_hooks();
  resolve_native_frame_symbols();

  fn_start GS_start =
      (fn_start)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_start");
  fn_void0 GS_engineStartup =
      (fn_void0)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_engineStartup");
  fn_void0 GS_restore =
      (fn_void0)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_restore");
  fn_void0 GS_resume =
      (fn_void0)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_resume");
  fn_resize GS_resize =
      (fn_resize)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_resize");
  fn_main_loop GS_mainLoop =
      (fn_main_loop)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_mainLoop");
  GS_mainLoop = main_loop_native;
  fn_void0 GS_destroy =
      (fn_void0)so_find_addr("Java_net_asantee_gs2d_GS2DJNI_destroy");
  fn_void0 GS_initializeKeyProvider =
      (fn_void0)so_find_addr_safe(
          "Java_net_asantee_gs2d_GS2DJNI_initializeKeyProvider");
  fn_set_shared GS_setShared =
      (fn_set_shared)so_find_addr_safe(
          "Java_net_asantee_gs2d_GS2DJNI_setSharedData");

  SDL_Window *window = NULL;
  SDL_GLContext glctx = NULL;
  int width = 1280;
  int height = 720;
  if (!init_sdl(&window, &glctx, &width, &height))
    return 1;
  audio_backend_init(apk_path);

  jni_min_init();
  if (GS_initializeKeyProvider)
    GS_initializeKeyProvider(jni_env(), jni_class());
  call_set_shared(GS_setShared, "ethanon.system.language", "en");
  call_set_shared(GS_setShared, "ethanon.system.screenSizeInch", "7.0");
  call_set_shared(GS_setShared, "ethanon.system.deviceModel",
                  "NextOS Amlogic-old");
  call_set_shared(GS_setShared, "ethanon.system.osVersion", "Linux");
  call_set_shared(GS_setShared, "ethanon.system.isLowRamDevice", "true");
  call_set_shared(GS_setShared, "ethanon.system.isMidRamDevice", "true");

  void *j_apk = jni_string(apk_path);
  void *j_user = jni_string(user_path);
  void *j_global = jni_string(global_path);
  fprintf(stderr, "GS2DJNI.start(%dx%d)\n", width, height);
  GS_start(jni_env(), jni_class(), j_apk, j_user, j_global, width, height);
  jni_string_free(j_apk);
  jni_string_free(j_user);
  jni_string_free(j_global);

  fprintf(stderr, "GS2DJNI.engineStartup/restore/resize/resume\n");
  GS_engineStartup(jni_env(), jni_class());
  GS_restore(jni_env(), jni_class());
  GS_resize(jni_env(), jni_class(), width, height);
  GS_resume(jni_env(), jni_class());

  int running = 1;
  int back_down = 0;
  int start_down = 0;
  unsigned frame = 0;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        running = 0;
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
        running = 0;
      else if (e.type == SDL_CONTROLLERBUTTONDOWN ||
               e.type == SDL_CONTROLLERBUTTONUP) {
        int down = e.type == SDL_CONTROLLERBUTTONDOWN;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
          back_down = down;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START)
          start_down = down;
        if (back_down && start_down)
          running = 0;
      } else if (e.type == SDL_WINDOWEVENT &&
                 e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        width = e.window.data1;
        height = e.window.data2;
        GS_resize(jni_env(), jni_class(), width, height);
      }
    }

    void *ret = GS_mainLoop(jni_env(), jni_class(), NULL);
    (void)ret;

    SDL_GL_SwapWindow(window);
    if (frame == 0)
      fprintf(stderr, "[video] first_swap=OK drawable=%dx%d\n", width, height);
    frame++;
  }

  // saída: threads do .so carregado nunca terminam; GS_destroy/SDL_Quit podem
  // pendurar segurando GPU/audio e a tela fica travada sem voltar pro ES.
  // O deadline garante que o processo morre mesmo se o shutdown travar.
  fprintf(stderr, "saindo...\n");
  pthread_t deadline;
  if (pthread_create(&deadline, NULL, exit_deadline_thread, NULL) == 0)
    pthread_detach(deadline);
  audio_backend_shutdown();
  SDL_GL_DeleteContext(glctx);
  SDL_DestroyWindow(window);
  _exit(0);
}
