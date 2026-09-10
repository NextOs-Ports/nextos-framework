#include <errno.h>
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

#include "android_callbacks.h"
#include "audio_compat.h"
#include "bionic_compat.h"
#include "first_accept.h"
#include "imports.h"
#include "installer_compat.h"
#include "input.h"
#include "jni_bridge.h"
#include "opensl_bridge.h"
#include "pthread_bridge.h"
#include "platform_shims.h"
#include "shop_compat.h"
#include "so_util.h"
#include "startup_compat.h"
#include "util.h"
#include "video.h"

#if defined(__i386__)
#define ASM2_LIBRARY "libtasm2-x86.so"
#else
#define ASM2_LIBRARY "libtasm2.so"
#endif
#define ASM2_HEAP_MB 64u
#define ASM2_FIRST_RUN_TOUCH_STATE \
  "Android/data/com.gameloft.android.ANMP.GloftASHM/files/save/" \
  "asm2-first-run-touch-state-v3"
#define ASM2_FIRST_ACCEPT_V2_MARKER \
  "Android/data/com.gameloft.android.ANMP.GloftASHM/files/save/" \
  ".asm2-legal-accepted-v2"
#define ASM2_FIRST_ACCEPT_V1_MARKER \
  "Android/data/com.gameloft.android.ANMP.GloftASHM/files/save/" \
  ".asm2-legal-accepted-v1"
#define ASM2_EXISTING_PROFILE_MARKER \
  "Android/data/com.gameloft.android.ANMP.GloftASHM/files/ud_Control.sav"

enum asm2_first_accept_configuration {
  ASM2_FIRST_ACCEPT_LEGACY_V1 = 4,
  ASM2_FIRST_ACCEPT_LEGACY_V2 = 5,
};

typedef asm2_jint(ASM2_GUEST_PCS *asm2_jni_on_load_fn)(void *vm,
                                                        void *reserved);
typedef void(ASM2_GUEST_PCS *asm2_jni_void_fn)(void *environment,
                                               void *class_handle);
typedef void(ASM2_GUEST_PCS *asm2_jni_resize_fn)(void *environment,
                                                 void *class_handle,
                                                 asm2_jint width,
                                                 asm2_jint height);
typedef void(ASM2_GUEST_PCS *asm2_jni_boolean_fn)(void *environment,
                                                  void *class_handle,
                                                  asm2_jboolean value);
typedef void(ASM2_GUEST_PCS *asm2_jni_int_fn)(void *environment,
                                              void *class_handle,
                                              asm2_jint value);
typedef void(ASM2_GUEST_PCS *asm2_jni_controller_connected_fn)(
    void *environment, void *class_handle, void *name);
typedef void(ASM2_GUEST_PCS *asm2_jni_hid_fn)(void *environment,
                                              void *class_handle,
                                              asm2_jint input_id,
                                              asm2_jdouble value);
typedef void(ASM2_GUEST_PCS *asm2_jni_touch_fn)(
    void *environment, void *class_handle, asm2_jint action, asm2_jint x,
    asm2_jint y, asm2_jint pointer_id);
typedef void *(ASM2_GUEST_PCS *asm2_jni_bundle_fn)(void *environment,
                                                   void *class_handle,
                                                   void *bundle);

struct asm2_guest_input_context {
  void *gl2jni_class;
  void *hid_class;
  asm2_jni_controller_connected_fn controller_connected;
  asm2_jni_void_fn controller_disconnected;
  asm2_jni_hid_fn hid;
  asm2_jni_int_fn key_down;
  asm2_jni_int_fn key_up;
  asm2_jni_touch_fn touch;
};

static volatile sig_atomic_t exit_signal_requested;

static void exit_signal_handler(int signal_number) {
  (void)signal_number;
  exit_signal_requested = 1;
}

static void install_exit_handlers(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_handler = exit_signal_handler;
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGHUP, &action, NULL);
}

static void guest_controller_connected(const char *name, void *userdata) {
  struct asm2_guest_input_context *context = userdata;
  if (!context || !context->controller_connected)
    return;
  context->controller_connected(
      asm2_jni_env(), context->hid_class,
      asm2_jni_string(name && name[0] ? name : "NextOS Controller"));
}

static void guest_controller_disconnected(void *userdata) {
  struct asm2_guest_input_context *context = userdata;
  if (context && context->controller_disconnected)
    context->controller_disconnected(asm2_jni_env(), context->hid_class);
}

static void guest_hid_event(int input_id, double value, void *userdata) {
  struct asm2_guest_input_context *context = userdata;
  if (context && context->hid)
    context->hid(asm2_jni_env(), context->hid_class, input_id, value);
}

static void guest_key_event(int android_keycode, int pressed, void *userdata) {
  struct asm2_guest_input_context *context = userdata;
  if (!context)
    return;
  asm2_jni_int_fn function =
      pressed ? context->key_down : context->key_up;
  if (function)
    function(asm2_jni_env(), context->gl2jni_class, android_keycode);
}

static void guest_touch_event(int action, int x, int y, int pointer_id,
                              void *userdata) {
  struct asm2_guest_input_context *context = userdata;
  if (context && context->touch)
    context->touch(asm2_jni_env(), context->gl2jni_class, action, x, y,
                   pointer_id);
}

static void crash_handler(int signal_number, siginfo_t *info, void *context) {
  ucontext_t *uc = context;
  mcontext_t *machine = &uc->uc_mcontext;
#if defined(__i386__)
  fprintf(stderr,
          "ASM2_CRASH signal=%d code=%d address=%p eip=%08lx esp=%08lx\n",
          signal_number, info->si_code, info->si_addr,
          (unsigned long)machine->gregs[REG_EIP],
          (unsigned long)machine->gregs[REG_ESP]);
#else
  fprintf(stderr,
          "ASM2_CRASH signal=%d code=%d address=%p pc=%08lx lr=%08lx\n",
          signal_number, info->si_code, info->si_addr,
          (unsigned long)machine->arm_pc,
          (unsigned long)machine->arm_lr);
#endif
  fflush(stderr);
  _exit(128 + signal_number);
}

static void install_crash_handler(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = crash_handler;
  action.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &action, NULL);
  sigaction(SIGBUS, &action, NULL);
  sigaction(SIGILL, &action, NULL);
  sigaction(SIGABRT, &action, NULL);
  sigaction(SIGFPE, &action, NULL);
}

static int join_path(char *output, size_t output_size, const char *directory,
                     const char *name) {
  int written = snprintf(output, output_size, "%s/%s", directory, name);
  return written >= 0 && (size_t)written < output_size ? 0 : -1;
}

static int environment_enabled(const char *name) {
  const char *value = getenv(name);
  return value && value[0] && strcmp(value, "0") != 0;
}

static uint64_t monotonic_milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static unsigned long status_value_kb(const char *path, const char *key) {
  FILE *input = fopen(path, "r");
  if (!input)
    return 0;
  const size_t key_length = strlen(key);
  unsigned long result = 0;
  char line[256];
  while (fgets(line, sizeof(line), input)) {
    if (strncmp(line, key, key_length) != 0 || line[key_length] != ':')
      continue;
    char *value = line + key_length + 1;
    while (*value == ' ' || *value == '\t')
      ++value;
    result = strtoul(value, NULL, 10);
    break;
  }
  fclose(input);
  return result;
}

static void runtime_diagnostics_tick(size_t rendered_frames) {
  static uint64_t last_report_ms;
  const uint64_t now = monotonic_milliseconds();
  if (!now || (last_report_ms && now - last_report_ms < 15000u))
    return;
  if (!last_report_ms) {
    last_report_ms = now;
    return;
  }
  last_report_ms = now;

  struct asm2_bionic_file_stats files;
  struct asm2_jni_stats jni;
  struct asm2_pthread_bridge_stats pthreads;
  struct asm2_platform_stats platform;
  struct asm2_opensl_stats opensl;
  asm2_bionic_get_file_stats(&files);
  asm2_jni_get_stats(&jni);
  asm2_pthread_bridge_get_stats(&pthreads);
  asm2_platform_get_stats(&platform);
  asm2_opensl_get_stats(&opensl);
  debugPrintf("ASM2_RUNTIME_STATE frames=%zu rss_kb=%lu vmsize_kb=%lu "
              "process_swap_kb=%lu mem_available_kb=%lu swap_free_kb=%lu "
              "files_created=%llu files_open=%llu files_closed=%llu "
              "file_bucket_max=%u jni_handles=%llu jni_arrays=%llu "
              "jni_array_bytes=%llu jni_strings=%llu jni_objects=%llu "
              "jni_bundle_entries=%llu jni_bucket_max=%u/%u "
              "sync_created=%llu "
              "sync_active=%llu sync_retired=%llu sync_handoffs=%llu "
              "sync_bucket_max=%u "
              "guest_glfinish=%llu finish_fence=%llu fallback_glfinish=%llu "
              "native_finish=%d "
              "test_fence=%llu fallback_test_true=%llu native_test=%d "
              "opensl_players=%llu/%llu opensl_workers=%llu/%llu "
              "opensl_self_deferred=%llu\n",
              rendered_frames, status_value_kb("/proc/self/status", "VmRSS"),
              status_value_kb("/proc/self/status", "VmSize"),
              status_value_kb("/proc/self/status", "VmSwap"),
              status_value_kb("/proc/meminfo", "MemAvailable"),
              status_value_kb("/proc/meminfo", "SwapFree"),
              (unsigned long long)files.created,
              (unsigned long long)files.open,
              (unsigned long long)files.closed,
              files.longest_active_bucket,
              (unsigned long long)jni.handles,
              (unsigned long long)jni.arrays,
              (unsigned long long)jni.array_bytes,
              (unsigned long long)jni.strings,
              (unsigned long long)jni.objects,
              (unsigned long long)jni.bundle_entries,
              jni.longest_handle_bucket,
              jni.longest_intern_bucket,
              (unsigned long long)pthreads.created,
              (unsigned long long)pthreads.active,
              (unsigned long long)pthreads.retired,
              (unsigned long long)pthreads.mutex_handoffs,
              pthreads.longest_active_bucket,
              (unsigned long long)platform.guest_glfinish_calls,
              (unsigned long long)platform.finish_fence_calls,
              (unsigned long long)platform.fallback_glfinish_calls,
              platform.native_finish_fence,
              (unsigned long long)platform.test_fence_calls,
              (unsigned long long)platform.fallback_test_true_calls,
              platform.native_test_fence,
              (unsigned long long)opensl.players_created,
              (unsigned long long)opensl.players_released,
              (unsigned long long)opensl.workers_started,
              (unsigned long long)opensl.workers_exited,
              (unsigned long long)opensl.self_deferred_destroys);
}

static int ensure_directory_tree(const char *path) {
  char copy[4096];
  size_t length = strlen(path);
  if (!length || length >= sizeof(copy))
    return -1;
  memcpy(copy, path, length + 1);
  for (char *cursor = copy + 1; *cursor; ++cursor) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (mkdir(copy, 0755) != 0 && errno != EEXIST)
      return -1;
    *cursor = '/';
  }
  return mkdir(copy, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int ensure_storage_directories(const char *storage_root) {
  static const char *const suffixes[] = {
      "Android/data/com.gameloft.android.ANMP.GloftASHM/files/save",
      "Android/data/com.gameloft.android.ANMP.GloftASHM/cache",
  };
  char path[4096];
  for (size_t index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]);
       ++index) {
    if (join_path(path, sizeof(path), storage_root, suffixes[index]) != 0 ||
        ensure_directory_tree(path) != 0) {
      fprintf(stderr, "ASM2_STORAGE_ERROR path=%s error=%s\n", path,
              strerror(errno));
      return -1;
    }
  }
  return 0;
}

static int configure_first_run_touch(const char *storage_root, char *state_path,
                                     size_t state_path_size) {
  char v1_marker[4096];
  char v2_marker[4096];
  char existing_profile[4096];
  if (join_path(state_path, state_path_size, storage_root,
                ASM2_FIRST_RUN_TOUCH_STATE) != 0 ||
      join_path(v1_marker, sizeof(v1_marker), storage_root,
                ASM2_FIRST_ACCEPT_V1_MARKER) != 0 ||
      join_path(v2_marker, sizeof(v2_marker), storage_root,
                ASM2_FIRST_ACCEPT_V2_MARKER) != 0 ||
      join_path(existing_profile, sizeof(existing_profile), storage_root,
                ASM2_EXISTING_PROFILE_MARKER) != 0) {
    fprintf(stderr, "ASM2_INPUT first-run touch path too long\n");
    state_path[0] = '\0';
    return ASM2_FIRST_RUN_TOUCH_COMPLETE;
  }

  /* A completed controls profile is game-owned proof that every first-run
   * modal has already been traversed. */
  if (access(existing_profile, F_OK) == 0) {
    fprintf(stderr, "ASM2_INPUT first-run touch profile completed\n");
    return ASM2_FIRST_RUN_TOUCH_COMPLETE;
  }

  FILE *state = fopen(state_path, "rb");
  if (state) {
    int next_stage = -1;
    int parsed = fscanf(state, "next=%d", &next_stage);
    fclose(state);
    if (parsed == 1 && next_stage >= ASM2_FIRST_RUN_TOUCH_LEGAL &&
        next_stage <= ASM2_FIRST_RUN_TOUCH_COMPLETE) {
      int resume_stage = asm2_first_run_resume_stage(next_stage);
      if (resume_stage != next_stage) {
        fprintf(stderr,
                "ASM2_INPUT first-run touch attempts completed without "
                "profile; restarting recovery sequence\n");
      }
      fprintf(stderr, "ASM2_INPUT first-run touch resume next=%d\n",
              resume_stage);
      return resume_stage;
    }
    fprintf(stderr,
            "ASM2_INPUT first-run touch state invalid; restarting sequence\n");
    return ASM2_FIRST_RUN_TOUCH_LEGAL;
  }

  if (access(v2_marker, F_OK) == 0) {
    fprintf(stderr, "ASM2_INPUT first-accept v2 marker migration\n");
    return ASM2_FIRST_ACCEPT_LEGACY_V2;
  }
  if (access(v1_marker, F_OK) == 0) {
    fprintf(stderr, "ASM2_INPUT first-accept v1 marker migration\n");
    return ASM2_FIRST_ACCEPT_LEGACY_V1;
  }

  fprintf(stderr, "ASM2_INPUT first-run touch required next=0\n");
  return ASM2_FIRST_RUN_TOUCH_LEGAL;
}

static void initialize_jni(struct asm2_android_state *android_state) {
  asm2_jni_config config;
  memset(&config, 0, sizeof(config));
  config.package_name = "com.gameloft.android.ANMP.GloftASHM";
  config.external_storage = "/sdcard";
  config.files_directory =
      "/sdcard/Android/data/com.gameloft.android.ANMP.GloftASHM/files";
  config.cache_directory =
      "/sdcard/Android/data/com.gameloft.android.ANMP.GloftASHM/cache";
  config.locale_language = "en";
  config.locale_country = "BR";
#if defined(__i386__)
  config.device_model = "NextOS X5M Box32";
#else
  config.device_model = "NextOS Mali-450";
#endif
  config.device_manufacturer = "NextOS";
  config.android_release = "8.1";
  config.sdk_int = 27;
  config.screen_width = 1280;
  config.screen_height = 720;
  config.screen_density = 1.0f;
  config.method_callback = asm2_android_method_callback;
  config.field_get_callback = asm2_android_field_get_callback;
  config.user = android_state;
  asm2_jni_init(&config);
}

int main(int argc, char **argv) {
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  install_exit_handlers();
  int process_exit_code = 0;

  const char *game_directory = argc > 1 ? argv[1] : ".";
  char library_path[4096];
  char storage_root[4096];
  char first_run_touch_state_path[4096];
  if (join_path(library_path, sizeof(library_path), game_directory,
                ASM2_LIBRARY) != 0) {
    fprintf(stderr, "ASM2_F0_ERROR library path is too long\n");
    return 1;
  }
  if (join_path(storage_root, sizeof(storage_root), game_directory,
                "gamefiles") != 0) {
    fprintf(stderr, "ASM2_F0_ERROR storage path is too long\n");
    return 1;
  }
  asm2_bionic_init(storage_root);
  if (ensure_storage_directories(storage_root) != 0)
    return 1;
  int first_run_touch_configuration = configure_first_run_touch(
      storage_root, first_run_touch_state_path,
      sizeof(first_run_touch_state_path));

  const size_t heap_size = ASM2_HEAP_MB * 1024u * 1024u;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    fprintf(stderr, "ASM2_F0_ERROR mmap: %s\n", strerror(errno));
    return 1;
  }

  fprintf(stderr, "ASM2_F0_BEGIN library=%s heap=%p size=%zu\n", library_path,
          heap, heap_size);
  if (so_load(library_path, heap, heap_size) < 0) {
    fprintf(stderr, "ASM2_F0_ERROR so_load\n");
    return 1;
  }
  if (so_relocate() < 0) {
    fprintf(stderr, "ASM2_F0_ERROR so_relocate\n");
    return 1;
  }
  if (asm2_audio_compat_apply() != 0) {
    fprintf(stderr, "ASM2_F0_ERROR audio compatibility patch\n");
    return 16;
  }
  if (asm2_startup_compat_apply() != 0) {
    fprintf(stderr, "ASM2_F0_ERROR startup compatibility patch\n");
    return 14;
  }

#if defined(__i386__)
  asm2_imports_initialize();
#endif
  int unresolved = so_resolve(dynlib_functions, (int)dynlib_numfunctions, 0);
  if (unresolved != 0) {
    fprintf(stderr,
            "ASM2_F1_RESOLVE_ERROR unresolved_strong_relocations=%d; "
            "guest code remains disabled\n",
            unresolved);
    return 2;
  }
  so_finalize();
  so_flush_caches();

  if (!environment_enabled("ASM2_RUN")) {
    fprintf(stderr, "ASM2_RUNTIME_ERROR ASM2_RUN is not enabled\n");
    return 15;
  }

  size_t constructor_count = so_init_array_count();
  int run_continuously = 1;
  int boot_video = 1;
  int boot_paths = 1;
  int boot_view_settings = 1;
  int boot_gl_init = 1;
  int boot_native_init = 1;
  int boot_jni = 1;
  fprintf(stderr, "ASM2_INIT_BEGIN total=%zu\n", constructor_count);
  size_t executed = so_execute_init_array_limit(constructor_count);
  fprintf(stderr, "ASM2_INIT_OK executed=%zu total=%zu\n", executed,
          constructor_count);

  uintptr_t jni_on_load = so_find_addr_safe("JNI_OnLoad");
  uintptr_t activity_native_init = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNIActivity_nativeInit");
  uintptr_t game_api_native_init = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_GameAPI_"
      "GameAPIAndroidGLSocialLib_nativeInit");
  uintptr_t platform_native_init = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit");
  uintptr_t facebook_native_init = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_facebook_"
      "FacebookAndroidGLSocialLib_nativeInit");
  uintptr_t send_info_init_methods = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_SendInfo_initMethods");
  uintptr_t device_native_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_Device_nativeInit");
  uintptr_t sutils_native_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_SUtils_nativeInit");
  uintptr_t billing_native_send_data = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_iab_InAppBilling_"
      "nativeSendData");
  uintptr_t notification_native_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_PushNotification_"
      "SimplifiedAndroidUtils_nativeInit");
  uintptr_t data_sharing_native_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_DataSharing_nativeInit");
  uintptr_t game_options_on_resume = so_find_addr_safe(
      "Java_com_gameloft_gameoptions_GameOptions_onResumeGame");
  uintptr_t native_init_gl =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_initGL");
  uintptr_t native_init_view_settings =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_InitViewSettings");
  uintptr_t native_set_paths =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_setPaths");
  uintptr_t native_init = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_init");
  uintptr_t native_step = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_step");
  uintptr_t native_resize = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_resize");
  uintptr_t native_state_changed = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_stateChanged");
  uintptr_t native_on_pause = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_onPause");
  uintptr_t game_options_on_pause = so_find_addr_safe(
      "Java_com_gameloft_gameoptions_GameOptions_onPauseGame");
  uintptr_t game_options_on_exit = so_find_addr_safe(
      "Java_com_gameloft_gameoptions_GameOptions_onExit");
  uintptr_t controller_connected = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_controller_"
      "NativeBridgeHIDControllers_NativeControllerConnected");
  uintptr_t controller_disconnected = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_controller_"
      "NativeBridgeHIDControllers_NativeControllerDisconnected");
  uintptr_t handle_hid_input = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_controller_"
      "NativeBridgeHIDControllers_NativeHandleInputEvents");
  uintptr_t native_key_down = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_OnKeyDown");
  uintptr_t native_key_up = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_OnKeyUp");
  uintptr_t native_touch = so_find_addr_safe(
      "Java_com_gameloft_glf_GL2JNILib_touchEvent");

  struct asm2_android_state android_state;
  asm2_android_state_init(&android_state);
  if (asm2_android_set_storage_root(&android_state, storage_root) != 0) {
    fprintf(stderr, "ASM2_SHARED_STORAGE_ERROR root=%s error=%s\n",
            storage_root, strerror(errno));
    return 13;
  }

  if (boot_jni) {
    if (!jni_on_load) {
      fprintf(stderr, "ASM2_JNI_ONLOAD_ERROR symbol not found\n");
      return 3;
    }
    initialize_jni(&android_state);
    void *game_options_class =
        asm2_jni_class("com/gameloft/gameoptions/GameOptions");
    asm2_android_bind_game_options_exit(
        &android_state, (asm2_guest_lifecycle_fn)game_options_on_exit,
        asm2_jni_env(), game_options_class);
    if (!game_options_on_exit)
      fprintf(stderr,
              "ASM2_LIFECYCLE_WARNING GameOptions.onExit unavailable; "
              "terminal callback remains fail-safe\n");
    fprintf(stderr, "ASM2_JNI_ONLOAD_BEGIN vm=%p env=%p function=%p\n",
            asm2_jni_vm(), asm2_jni_env(), (void *)jni_on_load);
    asm2_jint version =
        ((asm2_jni_on_load_fn)jni_on_load)(asm2_jni_vm(), NULL);
    if (version != 0x00010004) {
      fprintf(stderr,
              "ASM2_JNI_ONLOAD_ERROR returned=%08x expected=00010004\n",
              (unsigned)version);
      return 4;
    }
    fprintf(stderr,
            "ASM2_JNI_ONLOAD_OK returned=%08x registered_natives=%zu\n",
            (unsigned)version, asm2_jni_registered_native_count());

    if (boot_native_init) {
      if (asm2_installer_compat_initialize() != 0)
        return 5;
      if (!activity_native_init || !game_api_native_init ||
          !platform_native_init || !facebook_native_init ||
          !send_info_init_methods || !device_native_init ||
          !sutils_native_init || !billing_native_send_data ||
          !notification_native_init ||
          !data_sharing_native_init || !game_options_on_resume) {
        fprintf(stderr,
                "ASM2_PLATFORM_INIT_ERROR Activity=%p GameAPI=%p "
                "Platform=%p Facebook=%p SendInfo=%p Device=%p SUtils=%p "
                "Billing=%p Notification=%p DataSharing=%p GameOptions=%p\n",
                (void *)activity_native_init, (void *)game_api_native_init,
                (void *)platform_native_init, (void *)facebook_native_init,
                (void *)send_info_init_methods, (void *)device_native_init,
                (void *)sutils_native_init, (void *)billing_native_send_data,
                (void *)notification_native_init,
                (void *)data_sharing_native_init,
                (void *)game_options_on_resume);
        return 6;
      }

      void *activity_class =
          asm2_jni_class("com/gameloft/glf/GL2JNIActivity");
      fprintf(stderr,
              "ASM2_ACTIVITY_INIT_BEGIN env=%p class=%p function=%p\n",
              asm2_jni_env(), activity_class, (void *)activity_native_init);
      ((asm2_jni_void_fn)activity_native_init)(asm2_jni_env(),
                                               activity_class);
      fprintf(stderr, "ASM2_ACTIVITY_INIT_OK\n");

      void *game_api_class = asm2_jni_class(
          "com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib");
      void *platform_class =
          asm2_jni_class("com/gameloft/GLSocialLib/PlatformAndroid");
      void *facebook_class = asm2_jni_class(
          "com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib");
      fprintf(stderr, "ASM2_SOCIAL_INIT_BEGIN\n");
      ((asm2_jni_void_fn)game_api_native_init)(asm2_jni_env(),
                                               game_api_class);
      ((asm2_jni_void_fn)platform_native_init)(asm2_jni_env(),
                                               platform_class);
      ((asm2_jni_void_fn)facebook_native_init)(asm2_jni_env(),
                                               facebook_class);
      fprintf(stderr, "ASM2_SOCIAL_INIT_OK\n");

      void *gl2jni_class = asm2_jni_class("com/gameloft/glf/GL2JNILib");
      asm2_android_bind_paths(
          &android_state, (asm2_guest_set_paths_fn)native_set_paths,
          gl2jni_class);
      if (boot_gl_init) {
        if (!native_init) {
          fprintf(stderr, "ASM2_GL_INIT_ERROR symbol not found\n");
          return 9;
        }
        fprintf(stderr, "ASM2_GL_INIT_BEGIN function=%p\n",
                (void *)native_init);
        ((asm2_jni_void_fn)native_init)(asm2_jni_env(), gl2jni_class);
        fprintf(stderr, "ASM2_GL_INIT_OK\n");
      }

      void *send_info_class = asm2_jni_class(
          "com/gameloft/android/ANMP/GloftASHM/SendInfo");
      void *device_class = asm2_jni_class(
          "com/gameloft/android/ANMP/GloftASHM/GLUtils/Device");
      void *sutils_class = asm2_jni_class(
          "com/gameloft/android/ANMP/GloftASHM/GLUtils/SUtils");
      void *billing_class = asm2_jni_class(
          "com/gameloft/android/ANMP/GloftASHM/iab/InAppBilling");
      void *notification_class = asm2_jni_class(
          "com/gameloft/android/ANMP/GloftASHM/PushNotification/"
          "SimplifiedAndroidUtils");
      void *data_sharing_class = asm2_jni_class(
          "com/gameloft/android/ANMP/GloftASHM/DataSharing");

      fprintf(stderr, "ASM2_SEND_INFO_INIT_BEGIN function=%p\n",
              (void *)send_info_init_methods);
      ((asm2_jni_void_fn)send_info_init_methods)(asm2_jni_env(),
                                                 send_info_class);
      fprintf(stderr, "ASM2_SEND_INFO_INIT_OK\n");
      fprintf(stderr, "ASM2_DEVICE_INIT_BEGIN function=%p\n",
              (void *)device_native_init);
      ((asm2_jni_void_fn)device_native_init)(asm2_jni_env(), device_class);
      fprintf(stderr, "ASM2_DEVICE_INIT_OK\n");
      fprintf(stderr, "ASM2_SUTILS_INIT_BEGIN function=%p\n",
              (void *)sutils_native_init);
      ((asm2_jni_void_fn)sutils_native_init)(asm2_jni_env(), sutils_class);
      fprintf(stderr, "ASM2_SUTILS_INIT_OK\n");
      void *billing_request = asm2_jni_bundle();
      if (!billing_request ||
          !asm2_jni_bundle_put_int(billing_request, "O", 0) ||
          !asm2_jni_bundle_put_int(billing_request, "I", 119)) {
        fprintf(stderr, "ASM2_BILLING_INIT_ERROR bundle allocation\n");
        return 6;
      }
      fprintf(stderr, "ASM2_BILLING_INIT_BEGIN function=%p operation=0/119\n",
              (void *)billing_native_send_data);
      void *billing_result = ((asm2_jni_bundle_fn)billing_native_send_data)(
          asm2_jni_env(), billing_class, billing_request);
      const char *billing_response =
          asm2_jni_bundle_get_string(billing_result, "R");
      fprintf(stderr, "ASM2_BILLING_INIT_OK response_bytes=%zu\n",
              billing_response ? strlen(billing_response) : 0u);
      if (asm2_shop_compat_initialize(game_directory,
                                      billing_native_send_data,
                                      billing_class) != 0) {
        fprintf(stderr, "ASM2_SHOP_INIT_ERROR error=%s\n", strerror(errno));
        return 6;
      }
      fprintf(stderr, "ASM2_NOTIFICATION_INIT_BEGIN function=%p\n",
              (void *)notification_native_init);
      ((asm2_jni_void_fn)notification_native_init)(asm2_jni_env(),
                                                   notification_class);
      fprintf(stderr, "ASM2_NOTIFICATION_INIT_OK\n");
      fprintf(stderr,
              "ASM2_DATA_SHARING_INIT_BEGIN env=%p class=%p function=%p\n",
              asm2_jni_env(), data_sharing_class,
              (void *)data_sharing_native_init);
      ((asm2_jni_void_fn)data_sharing_native_init)(asm2_jni_env(),
                                                   data_sharing_class);
      fprintf(stderr, "ASM2_DATA_SHARING_INIT_OK\n");
      fprintf(stderr, "ASM2_GAME_OPTIONS_RESUME_BEGIN function=%p\n",
              (void *)game_options_on_resume);
      ((asm2_jni_void_fn)game_options_on_resume)(asm2_jni_env(),
                                                 game_options_class);
      fprintf(stderr, "ASM2_GAME_OPTIONS_RESUME_OK\n");

      if (boot_view_settings) {
        if (!native_init_gl || !native_init_view_settings) {
          fprintf(stderr,
                  "ASM2_VIEW_SETTINGS_ERROR initGL=%p InitViewSettings=%p\n",
                  (void *)native_init_gl, (void *)native_init_view_settings);
          return 10;
        }
        fprintf(stderr, "ASM2_INIT_GL_BEGIN function=%p\n",
                (void *)native_init_gl);
        ((asm2_jni_void_fn)native_init_gl)(asm2_jni_env(), gl2jni_class);
        fprintf(stderr, "ASM2_INIT_GL_OK\n");
        fprintf(stderr, "ASM2_VIEW_SETTINGS_BEGIN function=%p\n",
                (void *)native_init_view_settings);
        ((asm2_jni_void_fn)native_init_view_settings)(asm2_jni_env(),
                                                      gl2jni_class);
        fprintf(stderr, "ASM2_VIEW_SETTINGS_OK\n");

        if (boot_paths && !android_state.paths_applied) {
          if (!native_set_paths) {
            fprintf(stderr, "ASM2_SET_PATHS_ERROR symbol not found\n");
            return 9;
          }
          void *resource_directory = asm2_jni_string(
              "/sdcard/Android/data/"
              "com.gameloft.android.ANMP.GloftASHM/files");
          void *files_directory = asm2_jni_string(
              "/sdcard/Android/data/"
              "com.gameloft.android.ANMP.GloftASHM/files/save");
          void *cache_directory = asm2_jni_string(
              "/sdcard/Android/data/"
              "com.gameloft.android.ANMP.GloftASHM/cache");
          fprintf(stderr,
                  "ASM2_SET_PATHS_BEGIN function=%p resource=%s files=%s "
                  "cache=%s\n",
                  (void *)native_set_paths,
                  asm2_jni_string_chars(resource_directory),
                  asm2_jni_string_chars(files_directory),
                  asm2_jni_string_chars(cache_directory));
          ((asm2_guest_set_paths_fn)native_set_paths)(
              asm2_jni_env(), gl2jni_class, resource_directory,
              files_directory, cache_directory);
          android_state.paths_applied = 1;
          fprintf(stderr, "ASM2_SET_PATHS_OK\n");
        }

        if (boot_video) {
          struct asm2_video video;
          fprintf(stderr, "ASM2_VIDEO_BEGIN\n");
          if (asm2_video_init(&video) != 0) {
            fprintf(stderr, "ASM2_VIDEO_ERROR\n");
            return 10;
          }
          /* Some SDL builds replace fatal handlers during video startup. */
          install_crash_handler();
          install_exit_handlers();
          asm2_android_set_display(&android_state, video.width, video.height);

          if (run_continuously) {
            if (!native_resize || !native_step) {
              fprintf(stderr, "ASM2_FRAME_ERROR resize=%p step=%p\n",
                      (void *)native_resize, (void *)native_step);
              asm2_video_shutdown(&video);
              return 11;
            }
            if (!native_state_changed || !native_on_pause ||
                 !game_options_on_pause || !controller_connected ||
                 !controller_disconnected || !handle_hid_input ||
                 !native_key_down || !native_key_up || !native_touch) {
              fprintf(stderr,
                      "ASM2_RUNTIME_ERROR state=%p pause=%p "
                      "options_pause=%p options_exit=%p controller=%p/%p "
                      "hid=%p key=%p/%p touch=%p\n",
                      (void *)native_state_changed, (void *)native_on_pause,
                      (void *)game_options_on_pause,
                      (void *)game_options_on_exit, (void *)controller_connected,
                      (void *)controller_disconnected, (void *)handle_hid_input,
                      (void *)native_key_down, (void *)native_key_up,
                      (void *)native_touch);
              asm2_video_shutdown(&video);
              return 12;
            }
            int landscape_width =
                video.width >= video.height ? video.width : video.height;
            int landscape_height =
                video.width >= video.height ? video.height : video.width;
            int first_run_touch_stage = first_run_touch_configuration;
            if (first_run_touch_configuration ==
                ASM2_FIRST_ACCEPT_LEGACY_V1) {
              first_run_touch_stage =
                  asm2_first_accept_legacy_marker_requires_retry(
                      landscape_width, landscape_height)
                      ? ASM2_FIRST_RUN_TOUCH_LEGAL
                      : ASM2_FIRST_RUN_TOUCH_UPDATE_LOG;
              fprintf(stderr,
                      "ASM2_INPUT first-accept v1 migrated drawable=%dx%d "
                      "next=%d\n",
                      landscape_width, landscape_height,
                      first_run_touch_stage);
            } else if (first_run_touch_configuration ==
                       ASM2_FIRST_ACCEPT_LEGACY_V2) {
              first_run_touch_stage = ASM2_FIRST_RUN_TOUCH_UPDATE_LOG;
              fprintf(stderr,
                      "ASM2_INPUT first-accept v2 migrated drawable=%dx%d "
                      "next=%d\n",
                      landscape_width, landscape_height,
                      first_run_touch_stage);
            }
            if (native_state_changed)
              ((asm2_jni_boolean_fn)native_state_changed)(
                  asm2_jni_env(), gl2jni_class, 1);
            fprintf(stderr, "ASM2_RESIZE_BEGIN size=%dx%d function=%p\n",
                    landscape_width, landscape_height, (void *)native_resize);
            ((asm2_jni_resize_fn)native_resize)(
                asm2_jni_env(), gl2jni_class, landscape_width,
                landscape_height);
            fprintf(stderr, "ASM2_RESIZE_OK\n");
            struct asm2_input input;
            struct asm2_guest_input_context guest_input;
            memset(&guest_input, 0, sizeof(guest_input));
            int input_initialized = 0;
            size_t rendered_frames = 0;
            enum asm2_android_lifecycle_request lifecycle_request =
                ASM2_ANDROID_LIFECYCLE_NONE;
            if (run_continuously) {
              guest_input = (struct asm2_guest_input_context){
                  .gl2jni_class = gl2jni_class,
                  .hid_class = asm2_jni_class(
                      "com/gameloft/android/ANMP/GloftASHM/GLUtils/controller/"
                      "NativeBridgeHIDControllers"),
                  .controller_connected =
                      (asm2_jni_controller_connected_fn)controller_connected,
                  .controller_disconnected =
                      (asm2_jni_void_fn)controller_disconnected,
                  .hid = (asm2_jni_hid_fn)handle_hid_input,
                  .key_down = (asm2_jni_int_fn)native_key_down,
                  .key_up = (asm2_jni_int_fn)native_key_up,
                  .touch = (asm2_jni_touch_fn)native_touch,
              };
              const struct asm2_input_callbacks callbacks = {
                  .controller_connected = guest_controller_connected,
                  .controller_disconnected = guest_controller_disconnected,
                  .hid = guest_hid_event,
                  .key = guest_key_event,
                  .touch = guest_touch_event,
                  .userdata = &guest_input,
              };
              /* The controller listener reaches engine-owned objects that are
               * constructed by the first GL frame.  Android cannot deliver a
               * HID connection before that surface bootstrap has completed. */
              fprintf(stderr, "ASM2_RUN_BEGIN\n");
              ((asm2_jni_void_fn)native_step)(asm2_jni_env(), gl2jni_class);
              asm2_startup_compat_tick();
              asm2_shop_compat_tick();
              asm2_video_swap(&video);
              ++rendered_frames;
              runtime_diagnostics_tick(rendered_frames);
              asm2_input_init(
                  &input, &callbacks, landscape_width, landscape_height,
                  first_run_touch_stage < ASM2_FIRST_RUN_TOUCH_COMPLETE
                      ? first_run_touch_state_path
                      : NULL,
                  first_run_touch_stage);
              input_initialized = 1;
              while (!exit_signal_requested &&
                     !asm2_android_lifecycle_requested(&android_state) &&
                     asm2_input_pump(&input)) {
                ((asm2_jni_void_fn)native_step)(asm2_jni_env(), gl2jni_class);
                asm2_startup_compat_tick();
                asm2_shop_compat_tick();
                asm2_video_swap(&video);
                ++rendered_frames;
                runtime_diagnostics_tick(rendered_frames);
              }
              lifecycle_request =
                  asm2_android_get_lifecycle_request(&android_state);
              fprintf(stderr,
                      "ASM2_RUN_END frames=%zu signal=%d lifecycle=%d\n",
                      rendered_frames, exit_signal_requested != 0,
                      (int)lifecycle_request);
            }

            if (native_on_pause) {
              fprintf(stderr, "ASM2_LIFECYCLE GL_PAUSE_BEGIN\n");
              ((asm2_jni_void_fn)native_on_pause)(asm2_jni_env(), gl2jni_class);
              fprintf(stderr, "ASM2_LIFECYCLE GL_PAUSE_OK\n");
            }
            if (game_options_on_pause) {
              fprintf(stderr, "ASM2_LIFECYCLE OPTIONS_PAUSE_BEGIN\n");
              ((asm2_jni_void_fn)game_options_on_pause)(
                  asm2_jni_env(), game_options_class);
              fprintf(stderr, "ASM2_LIFECYCLE OPTIONS_PAUSE_OK\n");
            }
            if (input_initialized) {
              fprintf(stderr, "ASM2_LIFECYCLE INPUT_SHUTDOWN_BEGIN\n");
              asm2_input_shutdown(&input);
              fprintf(stderr, "ASM2_LIFECYCLE INPUT_SHUTDOWN_OK\n");
            }
            if (lifecycle_request == ASM2_ANDROID_LIFECYCLE_NONE &&
                game_options_on_exit) {
              fprintf(stderr, "ASM2_LIFECYCLE OPTIONS_EXIT_BEGIN\n");
              ((asm2_jni_void_fn)game_options_on_exit)(
                  asm2_jni_env(), game_options_class);
              fprintf(stderr, "ASM2_LIFECYCLE OPTIONS_EXIT_OK\n");
            } else if (lifecycle_request == ASM2_ANDROID_LIFECYCLE_EXIT) {
              fprintf(stderr,
                      "ASM2_LIFECYCLE OPTIONS_EXIT_ALREADY_DISPATCHED "
                      "done=%d\n",
                      asm2_android_game_options_exit_done(&android_state));
            } else if (lifecycle_request ==
                       ASM2_ANDROID_LIFECYCLE_RESTART) {
              fprintf(stderr,
                      "ASM2_LIFECYCLE OPTIONS_EXIT_SKIPPED_RESTART\n");
              process_exit_code = ASM2_ANDROID_RESTART_EXIT_CODE;
            }
            /* After an active session this 1.2.7d engine walks stale
             * process-lifetime objects from GL2JNILib_destroy. Android
             * normally lets the process reclaim them; preserve the safe
             * pause/exit path and let normal process exit do the same. */
            fprintf(stderr, "ASM2_LIFECYCLE GL_DESTROY_SKIPPED\n");
            if (lifecycle_request == ASM2_ANDROID_LIFECYCLE_NONE &&
                native_state_changed) {
              fprintf(stderr, "ASM2_LIFECYCLE STATE_FALSE_BEGIN\n");
              ((asm2_jni_boolean_fn)native_state_changed)(
                  asm2_jni_env(), gl2jni_class, 0);
              fprintf(stderr, "ASM2_LIFECYCLE STATE_FALSE_OK\n");
            } else if (lifecycle_request != ASM2_ANDROID_LIFECYCLE_NONE) {
              /* Both Java terminal methods finish and kill the Android
               * process after pause.  Do not manufacture an EGL-destruction
               * callback that Android 1.2.7d never gets to complete. */
              fprintf(stderr,
                      "ASM2_LIFECYCLE STATE_FALSE_SKIPPED_PROCESS_EXIT\n");
            }
            fprintf(stderr, "ASM2_LIFECYCLE_SHUTDOWN_OK\n");
          } else {
            asm2_video_swap(&video);
          }
          asm2_video_shutdown(&video);
          fprintf(stderr, "ASM2_VIDEO_SHUTDOWN_OK\n");
        }
      }
    }
  }

  fprintf(stderr,
          "ASM2_F1_RESOLVE_OK unresolved_relocations=%d constructors=%zu "
          "JNI_OnLoad=%p init=%p step=%p text=%p+%zu data=%p+%zu\n",
          unresolved, constructor_count, (void *)jni_on_load, (void *)native_init,
          (void *)native_step, text_base, text_size, data_base, data_size);
#if defined(__i386__)
  /*
   * The Android process is reclaimed after its terminal lifecycle callbacks;
   * it does not unload this process-lifetime native library.  Returning from
   * the emulated i386 main makes Box32 enter host/guest finalizer teardown
   * after every game-owned lifecycle callback and SDL shutdown have already
   * completed.  Keep that emulator boundary out of the Android flow.
   */
  fprintf(stderr, "ASM2_X86_PROCESS_EXIT code=%d mode=_exit\n",
          process_exit_code);
  fflush(NULL);
  _exit(process_exit_code);
#else
  return process_exit_code;
#endif
}
