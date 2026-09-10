#include <fcntl.h>
#include <glob.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_callbacks.h"

#define DATA_SHARING_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/DataSharing"
#define SUTILS_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/GLUtils/SUtils"
#define GAME_INSTALLER_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/installer/GameInstaller"
#define RESOURCE_DIRECTORY \
  "/sdcard/Android/data/com.gameloft.android.ANMP.GloftASHM/files"

void debugPrintf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    exit(1);
  }
}

static void set_value(struct asm2_android_state *state, const char *key,
                      const char *value) {
  asm2_jni_value arguments[2];
  asm2_jni_value result;
  memset(arguments, 0, sizeof(arguments));
  memset(&result, 0, sizeof(result));
  arguments[0].l = asm2_jni_string(key);
  arguments[1].l = asm2_jni_string(value);
  require(asm2_android_method_callback(
              state, DATA_SHARING_CLASS, "setSharedValue",
              "(Ljava/lang/String;Ljava/lang/String;)V", 1, NULL, NULL,
              arguments, &result) == 1,
          "setSharedValue handled");
}

static const char *get_value(struct asm2_android_state *state,
                             const char *key) {
  asm2_jni_value argument;
  asm2_jni_value result;
  memset(&argument, 0, sizeof(argument));
  memset(&result, 0, sizeof(result));
  argument.l = asm2_jni_string(key);
  require(asm2_android_method_callback(
              state, DATA_SHARING_CLASS, "getSharedValue",
              "(Ljava/lang/String;)Ljava/lang/String;", 1, NULL, NULL,
              &argument, &result) == 1,
          "getSharedValue handled");
  return asm2_jni_string_chars(result.l);
}

static void delete_value(struct asm2_android_state *state, const char *key) {
  asm2_jni_value argument;
  asm2_jni_value result;
  memset(&argument, 0, sizeof(argument));
  memset(&result, 0, sizeof(result));
  argument.l = asm2_jni_string(key);
  require(asm2_android_method_callback(
              state, DATA_SHARING_CLASS, "deleteSharedValue",
              "(Ljava/lang/String;)V", 1, NULL, NULL, &argument,
              &result) == 1,
          "deleteSharedValue handled");
}

static float get_free_disk_kib(struct asm2_android_state *state) {
  asm2_jni_value result;
  memset(&result, 0, sizeof(result));
  require(asm2_android_method_callback(
              state, "com/gameloft/glf/GL2JNILib", "JGetFreeDiskSpace", "()F",
              1, NULL, NULL, NULL, &result) == 1,
          "JGetFreeDiskSpace handled");
  return result.f;
}

static void *get_preference(struct asm2_android_state *state, void *bundle) {
  asm2_jni_value argument;
  asm2_jni_value result;
  memset(&argument, 0, sizeof(argument));
  memset(&result, 0, sizeof(result));
  argument.l = bundle;
  require(asm2_android_method_callback(
              state, SUTILS_CLASS, "nativeGetPreference",
              "(Landroid/os/Bundle;)Landroid/os/Bundle;", 1, NULL, NULL,
              &argument, &result) == 1,
          "nativeGetPreference handled");
  require(result.l == bundle, "nativeGetPreference returns the same Bundle");
  return result.l;
}

static void set_preference(struct asm2_android_state *state, void *bundle) {
  asm2_jni_value argument;
  asm2_jni_value result;
  memset(&argument, 0, sizeof(argument));
  memset(&result, 0, sizeof(result));
  argument.l = bundle;
  require(asm2_android_method_callback(
              state, SUTILS_CLASS, "nativeSetPreference",
              "(Landroid/os/Bundle;)V", 1, NULL, NULL, &argument,
              &result) == 1,
          "nativeSetPreference handled");
}

static const char *get_preference_string_direct(
    struct asm2_android_state *state, const char *key,
    const char *default_value, const char *preference_name) {
  asm2_jni_value arguments[3];
  asm2_jni_value result;
  memset(arguments, 0, sizeof(arguments));
  memset(&result, 0, sizeof(result));
  arguments[0].l = asm2_jni_string(key);
  const char *signature;
  if (default_value) {
    arguments[1].l = asm2_jni_string(default_value);
    arguments[2].l = asm2_jni_string(preference_name);
    signature =
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
        "Ljava/lang/String;";
  } else {
    arguments[1].l = asm2_jni_string(preference_name);
    signature =
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;";
  }
  require(asm2_android_method_callback(
              state, SUTILS_CLASS, "getPreferenceString", signature, 1, NULL,
              NULL, arguments, &result) == 1,
          "getPreferenceString overload handled");
  return asm2_jni_string_chars(result.l);
}

static void *preference_bundle(asm2_jint type, const char *preference_name,
                               const char *key) {
  void *bundle = asm2_jni_bundle();
  require(bundle != NULL, "create preference Bundle");
  require(asm2_jni_bundle_put_int(bundle, "npDataType", type),
          "set preference type");
  require(asm2_jni_bundle_put_string(bundle, "npPrefName", preference_name),
          "set preference name");
  require(asm2_jni_bundle_put_string(bundle, "npKey", key),
          "set preference key");
  return bundle;
}

static void exercise_preferences(struct asm2_android_state *state,
                                 int expect_stored) {
  void *integer = preference_bundle(0, "profile", "integer");
  require(asm2_jni_bundle_put_int(integer, "npDefaultValue", -17),
          "set integer default");
  get_preference(state, integer);
  asm2_jint integer_result = 0;
  require(asm2_jni_bundle_get_int(integer, "npResult", &integer_result),
          "get integer result");
  require(integer_result == (expect_stored ? 2147483000 : -17),
          "integer preference value");

  void *long_value = preference_bundle(1, "profile", "long");
  require(asm2_jni_bundle_put_long(long_value, "npDefaultValue", -19),
          "set long default");
  get_preference(state, long_value);
  asm2_jlong long_result = 0;
  require(asm2_jni_bundle_get_long(long_value, "npResult", &long_result),
          "get long result");
  require(long_result ==
              (expect_stored ? INT64_C(8589934591) : INT64_C(-19)),
          "long preference value");

  void *boolean = preference_bundle(2, "profile", "boolean");
  require(asm2_jni_bundle_put_boolean(boolean, "npDefaultValue", 1),
          "set boolean default");
  get_preference(state, boolean);
  asm2_jboolean boolean_result = 0;
  require(asm2_jni_bundle_get_boolean(boolean, "npResult", &boolean_result),
          "get boolean result");
  require(boolean_result == (expect_stored ? 0 : 1),
          "boolean preference value");

  void *string = preference_bundle(3, "profile", "string");
  require(asm2_jni_bundle_put_string(string, "npDefaultValue", "fallback"),
          "set string default");
  get_preference(state, string);
  require(strcmp(asm2_jni_bundle_get_string(string, "npResult"),
                 expect_stored ? "persisted:with:colons" : "fallback") == 0,
          "string preference value");
}

static void store_preferences(struct asm2_android_state *state) {
  void *integer = preference_bundle(0, "profile", "integer");
  require(asm2_jni_bundle_put_int(integer, "npData", 2147483000),
          "set integer data");
  set_preference(state, integer);

  void *long_value = preference_bundle(1, "profile", "long");
  require(asm2_jni_bundle_put_long(long_value, "npData",
                                   INT64_C(8589934591)),
          "set long data");
  set_preference(state, long_value);

  void *boolean = preference_bundle(2, "profile", "boolean");
  require(asm2_jni_bundle_put_boolean(boolean, "npData", 0),
          "set boolean data");
  set_preference(state, boolean);

  void *string = preference_bundle(3, "profile", "string");
  require(asm2_jni_bundle_put_string(string, "npData",
                                     "persisted:with:colons"),
          "set string data");
  set_preference(state, string);
}

static void exercise_preference_edges(struct asm2_android_state *state,
                                      int after_reload) {
  require(strcmp(get_preference_string_direct(
                     state, "string", NULL, "profile"),
                 "persisted:with:colons") == 0,
          "two-argument getPreferenceString reads persisted data");
  require(strcmp(get_preference_string_direct(
                     state, "string", "wrong-default", "profile"),
                 "persisted:with:colons") == 0,
          "three-argument getPreferenceString reads persisted data");
  require(strcmp(get_preference_string_direct(
                     state, "missing", NULL, "profile"),
                 "") == 0,
          "two-argument getPreferenceString uses empty default");
  require(strcmp(get_preference_string_direct(
                     state, "missing", "fallback-direct", "profile"),
                 "fallback-direct") == 0,
          "three-argument getPreferenceString uses supplied default");

  void *separate = preference_bundle(3, "other-profile", "string");
  require(asm2_jni_bundle_put_string(separate, "npDefaultValue", "isolated"),
          "set other namespace default");
  get_preference(state, separate);
  require(strcmp(asm2_jni_bundle_get_string(separate, "npResult"),
                 "isolated") == 0,
          "preference namespaces are isolated");

  void *unknown = preference_bundle(99, "profile", "unknown");
  get_preference(state, unknown);
  asm2_jint unexpected = 0;
  require(!asm2_jni_bundle_get_int(unknown, "npResult", &unexpected),
          "unknown preference type leaves Bundle unchanged");

  if (!after_reload) {
    void *missing_primitive =
        preference_bundle(0, "profile", "missing-primitive");
    set_preference(state, missing_primitive);
    require(asm2_jni_bundle_put_int(missing_primitive, "npDefaultValue", 77),
            "set missing primitive default");
    get_preference(state, missing_primitive);
    asm2_jint primitive_result = 77;
    require(asm2_jni_bundle_get_int(missing_primitive, "npResult",
                                    &primitive_result),
            "get missing primitive result");
    require(primitive_result == 0,
            "missing primitive npData follows Bundle zero semantics");

    set_value(state, "__asm2_shared_pref_v1__blocked", "collision");
    require(strcmp(get_value(state, "__asm2_shared_pref_v1__blocked"), "") ==
                0,
            "public DataSharing cannot enter reserved preference namespace");
  }
}

static void exercise_installer_contract(struct asm2_android_state *state) {
  asm2_jni_config config;
  memset(&config, 0, sizeof(config));
  config.field_get_callback = asm2_android_field_get_callback;
  config.user = state;
  asm2_jni_init(&config);

  void *environment = asm2_jni_env();
  void **table = *(void ***)environment;
  typedef void *(*find_class_function)(void *, const char *);
  typedef void *(*get_static_field_id_function)(
      void *, void *, const char *, const char *);
  typedef void *(*get_static_object_field_function)(void *, void *, void *);
  find_class_function find_class = (find_class_function)table[6];
  get_static_field_id_function get_static_field_id =
      (get_static_field_id_function)table[144];
  get_static_object_field_function get_static_object_field =
      (get_static_object_field_function)table[145];
  require(find_class && get_static_field_id && get_static_object_field,
          "JNI static-field vtable entries are installed");
  void *installer_class = find_class(environment, GAME_INSTALLER_CLASS);
  void *preference_field = get_static_field_id(
      environment, installer_class, "mPreferencesName",
      "Ljava/lang/String;");
  void *preference_value = get_static_object_field(
      environment, installer_class, preference_field);
  require(strcmp(asm2_jni_string_chars(preference_value),
                 "GameActivityPrefs") == 0,
          "JNI vtable dispatches GameInstaller.mPreferencesName");

  asm2_jni_value result;
  memset(&result, 0, sizeof(result));
  require(asm2_android_field_get_callback(
              state, GAME_INSTALLER_CLASS, "mPreferencesName",
              "Ljava/lang/String;", 1, NULL, &result) == 1,
          "GameInstaller.mPreferencesName field handled");
  require(strcmp(asm2_jni_string_chars(result.l), "GameActivityPrefs") == 0,
          "GameInstaller preference namespace matches the APK");
  memset(&result, 0, sizeof(result));
  require(asm2_android_field_get_callback(
              state, GAME_INSTALLER_CLASS, "mPreferencesName",
              "Ljava/lang/String;", 0, NULL, &result) == 0,
          "GameInstaller preference field must be static");

  require(strcmp(get_preference_string_direct(
                     state, "SDFolder", NULL, "GameActivityPrefs"),
                 RESOURCE_DIRECTORY) == 0,
          "post-installer profile has the guest SD folder");
  require(strcmp(get_preference_string_direct(
                     state, "SDFolder", "wrong", "GameActivityPrefs"),
                 RESOURCE_DIRECTORY) == 0,
          "stored SD folder overrides a caller default");
}

static void write_exact_file(const char *path, const void *contents,
                             size_t size) {
  int descriptor = open(path, O_WRONLY | O_TRUNC);
  require(descriptor >= 0, "open shared store for corruption test");
  require(write(descriptor, contents, size) == (ssize_t)size,
          "write intentionally corrupted shared store");
  require(close(descriptor) == 0, "close corrupted shared store");
}

static void require_file_equals(const char *path, const void *contents,
                                size_t size) {
  unsigned char buffer[128];
  require(size <= sizeof(buffer), "test fixture fits comparison buffer");
  int descriptor = open(path, O_RDONLY);
  require(descriptor >= 0, "open quarantine for byte comparison");
  require(read(descriptor, buffer, sizeof(buffer)) == (ssize_t)size,
          "quarantine retained the exact corrupt file size");
  require(memcmp(buffer, contents, size) == 0,
          "quarantine retained the exact corrupt file bytes");
  require(close(descriptor) == 0, "close compared quarantine");
}

static size_t collect_quarantines(const char *pattern, glob_t *matches) {
  memset(matches, 0, sizeof(*matches));
  int result = glob(pattern, 0, NULL, matches);
  if (result == GLOB_NOMATCH)
    return 0;
  require(result == 0, "enumerate corrupt-store quarantines");
  return matches->gl_pathc;
}

static void corrupt_shared_store_and_require_recovery(
    const char *storage_root) {
  static const unsigned char first_corruption[] = {
      'b', 'r', 'o', 'k', 'e', 'n', '-', 'f', 'i', 'r', 's', 't',
  };
  static const unsigned char second_corruption[] = {
      'b', 'r', 'o', 'k', 'e', 'n', '-', 's', 'e', 'c', 'o', 'n', 'd',
  };
  char path[4096];
  char pattern[4096];
  int size = snprintf(
      path, sizeof(path),
      "%s/Android/data/com.gameloft.android.ANMP.GloftASHM/files/save/"
      "datasharing-v1.bin",
      storage_root);
  require(size > 0 && (size_t)size < sizeof(path), "shared store path");
  write_exact_file(path, first_corruption, sizeof(first_corruption));

  struct asm2_android_state first_recovery;
  asm2_android_state_init(&first_recovery);
  require(asm2_android_set_storage_root(&first_recovery, storage_root) == 0,
          "quarantine invalid shared store and continue cleanly");
  exercise_installer_contract(&first_recovery);
  require(strcmp(get_value(&first_recovery, "worker-0"), "") == 0,
          "invalid shared values are not trusted after recovery");

  size = snprintf(pattern, sizeof(pattern), "%s.corrupt-*", path);
  require(size > 0 && (size_t)size < sizeof(pattern),
          "corrupt quarantine glob");
  glob_t first_matches;
  require(collect_quarantines(pattern, &first_matches) == 1,
          "exactly one recoverable corrupt-store quarantine exists");
  require_file_equals(first_matches.gl_pathv[0], first_corruption,
                      sizeof(first_corruption));
  char first_quarantine[4096];
  require(strlen(first_matches.gl_pathv[0]) < sizeof(first_quarantine),
          "first quarantine path fits");
  strcpy(first_quarantine, first_matches.gl_pathv[0]);
  globfree(&first_matches);

  struct asm2_android_state clean_reload;
  asm2_android_state_init(&clean_reload);
  require(asm2_android_set_storage_root(&clean_reload, storage_root) == 0,
          "reload primary rebuilt after corruption");
  exercise_installer_contract(&clean_reload);
  glob_t reload_matches;
  require(collect_quarantines(pattern, &reload_matches) == 1,
          "valid rebuilt primary creates no additional quarantine");
  globfree(&reload_matches);

  write_exact_file(path, second_corruption, sizeof(second_corruption));
  struct asm2_android_state second_recovery;
  asm2_android_state_init(&second_recovery);
  require(asm2_android_set_storage_root(&second_recovery, storage_root) == 0,
          "quarantine a second independent corruption");
  exercise_installer_contract(&second_recovery);
  glob_t second_matches;
  require(collect_quarantines(pattern, &second_matches) == 2,
          "two corruptions create two non-overwriting quarantines");
  int found_first = 0;
  int found_second = 0;
  for (size_t index = 0; index < second_matches.gl_pathc; ++index) {
    if (strcmp(second_matches.gl_pathv[index], first_quarantine) == 0) {
      require_file_equals(second_matches.gl_pathv[index], first_corruption,
                          sizeof(first_corruption));
      found_first = 1;
    } else {
      require_file_equals(second_matches.gl_pathv[index], second_corruption,
                          sizeof(second_corruption));
      found_second = 1;
    }
  }
  require(found_first && found_second,
          "both original corrupt byte streams remain recoverable");
  globfree(&second_matches);

  struct asm2_android_state final_reload;
  asm2_android_state_init(&final_reload);
  require(asm2_android_set_storage_root(&final_reload, storage_root) == 0,
          "reload primary rebuilt after second corruption");
  glob_t final_matches;
  require(collect_quarantines(pattern, &final_matches) == 2,
          "second valid rebuilt primary creates no quarantine");
  globfree(&final_matches);
}

struct writer_context {
  struct asm2_android_state *state;
  unsigned int index;
};

static void *writer_thread(void *opaque) {
  struct writer_context *context = opaque;
  char key[32];
  char value[64];
  snprintf(key, sizeof(key), "worker-%u", context->index);
  for (unsigned int iteration = 0; iteration < 20; ++iteration) {
    snprintf(value, sizeof(value), "value-%u-%u", context->index,
             iteration);
    set_value(context->state, key, value);
    require(strcmp(get_value(context->state, key), value) == 0,
            "concurrent value round-trip");
  }
  return NULL;
}

int main(void) {
  char storage_root[] = "/tmp/asm2-shared-values-XXXXXX";
  require(mkdtemp(storage_root) != NULL, "create storage root");

  struct asm2_android_state state;
  asm2_android_state_init(&state);
  require(asm2_android_set_storage_root(&state, storage_root) == 0,
          "bind initial storage root");
  require(get_free_disk_kib(&state) > 256.0f * 1024.0f,
          "free disk is reported in KiB");
  exercise_installer_contract(&state);
  exercise_preferences(&state, 0);
  store_preferences(&state);
  exercise_preferences(&state, 1);
  exercise_preference_edges(&state, 0);

  pthread_t threads[4];
  struct writer_context contexts[4];
  for (unsigned int index = 0; index < 4; ++index) {
    contexts[index].state = &state;
    contexts[index].index = index;
    require(pthread_create(&threads[index], NULL, writer_thread,
                           &contexts[index]) == 0,
            "start concurrent writer");
  }
  for (unsigned int index = 0; index < 4; ++index)
    require(pthread_join(threads[index], NULL) == 0, "join writer");

  for (unsigned int index = 0; index < 4; ++index) {
    char key[32];
    char value[64];
    snprintf(key, sizeof(key), "worker-%u", index);
    snprintf(value, sizeof(value), "value-%u-19", index);
    require(strcmp(get_value(&state, key), value) == 0,
            "final concurrent value");
  }

  struct asm2_android_state reloaded;
  asm2_android_state_init(&reloaded);
  require(asm2_android_set_storage_root(&reloaded, storage_root) == 0,
          "reload persisted storage");
  exercise_installer_contract(&reloaded);
  exercise_preferences(&reloaded, 1);
  exercise_preference_edges(&reloaded, 1);
  require(strcmp(get_value(&reloaded, "worker-2"), "value-2-19") == 0,
          "persisted value survived reload");
  delete_value(&reloaded, "worker-2");

  struct asm2_android_state after_delete;
  asm2_android_state_init(&after_delete);
  require(asm2_android_set_storage_root(&after_delete, storage_root) == 0,
          "reload after delete");
  require(strcmp(get_value(&after_delete, "worker-2"), "") == 0,
          "delete survived reload");

  corrupt_shared_store_and_require_recovery(storage_root);

  puts("Android shared values: concurrent access and persistence OK");
  return 0;
}
