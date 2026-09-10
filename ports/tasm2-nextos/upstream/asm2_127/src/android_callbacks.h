#ifndef ASM2_ANDROID_CALLBACKS_H
#define ASM2_ANDROID_CALLBACKS_H

#include <stdatomic.h>
#include <pthread.h>

#include "jni_bridge.h"

#define ASM2_ANDROID_SHARED_MAX_ENTRIES 128u
#define ASM2_ANDROID_RESTART_EXIT_CODE 75

typedef void(ASM2_GUEST_PCS *asm2_guest_set_paths_fn)(
    void *environment, void *class_handle, void *resource_directory,
    void *files_directory, void *cache_directory);
typedef void(ASM2_GUEST_PCS *asm2_guest_lifecycle_fn)(
    void *environment, void *class_handle);

enum asm2_android_lifecycle_request {
  ASM2_ANDROID_LIFECYCLE_NONE = 0,
  ASM2_ANDROID_LIFECYCLE_EXIT = 1,
  ASM2_ANDROID_LIFECYCLE_RESTART = 2,
};

struct asm2_android_state {
  int screen_width;
  int screen_height;
  const char *resource_directory;
  const char *files_directory;
  const char *cache_directory;
  asm2_guest_set_paths_fn set_paths;
  void *gl2jni_class;
  int paths_applied;
  int view_settings[5];
  int view_settings_received;
  _Atomic(enum asm2_android_lifecycle_request) lifecycle_request;
  _Atomic int game_options_exit_phase;
  asm2_guest_lifecycle_fn game_options_exit;
  void *game_options_environment;
  void *game_options_class;
  pthread_mutex_t shared_mutex;
  int shared_mutex_initialized;
  char *shared_keys[ASM2_ANDROID_SHARED_MAX_ENTRIES];
  char *shared_values[ASM2_ANDROID_SHARED_MAX_ENTRIES];
  size_t shared_count;
  char *shared_storage_directory;
  char *shared_storage_path;
};

void asm2_android_state_init(struct asm2_android_state *state);
int asm2_android_set_storage_root(struct asm2_android_state *state,
                                  const char *host_gamefiles_root);
void asm2_android_set_display(struct asm2_android_state *state, int width,
                              int height);
void asm2_android_bind_paths(struct asm2_android_state *state,
                             asm2_guest_set_paths_fn set_paths,
                             void *gl2jni_class);
void asm2_android_bind_game_options_exit(
    struct asm2_android_state *state, asm2_guest_lifecycle_fn function,
    void *environment, void *class_handle);
enum asm2_android_lifecycle_request asm2_android_get_lifecycle_request(
    const struct asm2_android_state *state);
int asm2_android_lifecycle_requested(
    const struct asm2_android_state *state);
int asm2_android_game_options_exit_done(
    const struct asm2_android_state *state);

int asm2_android_field_get_callback(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver,
    asm2_jni_value *result);

int asm2_android_method_callback(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver, va_list *arguments,
    const asm2_jni_value *array_arguments, asm2_jni_value *result);

#endif
