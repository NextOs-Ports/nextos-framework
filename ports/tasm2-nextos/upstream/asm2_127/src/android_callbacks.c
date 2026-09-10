#include "android_callbacks.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define ASM2_PACKAGE "com.gameloft.android.ANMP.GloftASHM"
#define ASM2_DATA_SHARING_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/DataSharing"
#define ASM2_SUTILS_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/GLUtils/SUtils"
#define ASM2_GAME_INSTALLER_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/installer/GameInstaller"
#define ASM2_BILLING_CLASS \
  "com/gameloft/android/ANMP/GloftASHM/iab/InAppBilling"
#define ASM2_GL2JNI_ACTIVITY_CLASS "com/gameloft/glf/GL2JNIActivity"
#define ASM2_RESOURCE_PATH \
  "/sdcard/Android/data/" ASM2_PACKAGE "/files"
#define ASM2_FILES_PATH \
  "/sdcard/Android/data/" ASM2_PACKAGE "/files/save"
#define ASM2_CACHE_PATH \
  "/sdcard/Android/data/" ASM2_PACKAGE "/cache"
#define ASM2_APK_PATH "/sdcard/base.apk"

#define ASM2_SHARED_DIRECTORY \
  "Android/data/" ASM2_PACKAGE "/files/save"
#define ASM2_SHARED_FILENAME "datasharing-v1.bin"
#define ASM2_SHARED_MAX_ENTRIES ASM2_ANDROID_SHARED_MAX_ENTRIES
#define ASM2_SHARED_MAX_KEY_SIZE 1024u
#define ASM2_SHARED_MAX_VALUE_SIZE (256u * 1024u)
#define ASM2_SHARED_HEADER_SIZE 24u
#define ASM2_PREFERENCE_KEY_PREFIX "__asm2_shared_pref_v1__"
#define ASM2_INSTALLER_PREFERENCES "GameActivityPrefs"
#define ASM2_INSTALLER_SD_KEY "SDFolder"
#define ASM2_SHARED_MAX_PAYLOAD_SIZE                                      \
  (ASM2_SHARED_MAX_ENTRIES *                                             \
   (8u + ASM2_SHARED_MAX_KEY_SIZE + ASM2_SHARED_MAX_VALUE_SIZE))

static const unsigned char asm2_shared_magic[8] = {
    'A', 'S', 'M', '2', 'D', 'S', '0', '1',
};

enum shared_load_status {
  SHARED_LOAD_OK,
  SHARED_LOAD_MISSING,
  SHARED_LOAD_CORRUPT,
  SHARED_LOAD_IO_ERROR,
};

enum game_options_exit_phase {
  GAME_OPTIONS_EXIT_IDLE = 0,
  GAME_OPTIONS_EXIT_RUNNING = 1,
  GAME_OPTIONS_EXIT_DONE = 2,
};

static int ensure_installer_preferences(struct asm2_android_state *state);

static void put_u32_le(unsigned char *destination, uint32_t value) {
  destination[0] = (unsigned char)value;
  destination[1] = (unsigned char)(value >> 8);
  destination[2] = (unsigned char)(value >> 16);
  destination[3] = (unsigned char)(value >> 24);
}

static uint32_t get_u32_le(const unsigned char *source) {
  return (uint32_t)source[0] | (uint32_t)source[1] << 8 |
         (uint32_t)source[2] << 16 | (uint32_t)source[3] << 24;
}

static uint32_t shared_crc32(const unsigned char *data, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  return ~crc;
}

static int write_all(int descriptor, const void *data, size_t size) {
  const unsigned char *cursor = data;
  while (size) {
    ssize_t written = write(descriptor, cursor,
                            size > (size_t)SSIZE_MAX ? SSIZE_MAX : size);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (written == 0) {
      errno = EIO;
      return -1;
    }
    cursor += (size_t)written;
    size -= (size_t)written;
  }
  return 0;
}

static int read_all(int descriptor, void *data, size_t size) {
  unsigned char *cursor = data;
  while (size) {
    ssize_t received =
        read(descriptor, cursor,
             size > (size_t)SSIZE_MAX ? SSIZE_MAX : size);
    if (received < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (received == 0) {
      errno = EINVAL;
      return -1;
    }
    cursor += (size_t)received;
    size -= (size_t)received;
  }
  return 0;
}

static int ensure_directory_tree(const char *path) {
  if (!path || !path[0]) {
    errno = EINVAL;
    return -1;
  }

  char *copy = strdup(path);
  if (!copy)
    return -1;
  size_t length = strlen(copy);
  while (length > 1 && copy[length - 1] == '/')
    copy[--length] = '\0';

  for (char *cursor = copy + (copy[0] == '/');; ++cursor) {
    if (*cursor != '/' && *cursor != '\0')
      continue;
    char saved = *cursor;
    *cursor = '\0';
    if (copy[0] && mkdir(copy, 0755) != 0 && errno != EEXIST) {
      int saved_errno = errno;
      free(copy);
      errno = saved_errno;
      return -1;
    }
    *cursor = saved;
    if (saved == '\0')
      break;
  }
  free(copy);
  return 0;
}

static char *join_storage_path(const char *left, const char *right) {
  if (!left || !left[0] || !right || !right[0]) {
    errno = EINVAL;
    return NULL;
  }
  size_t left_size = strlen(left);
  size_t right_size = strlen(right);
  int separator = left[left_size - 1] != '/';
  if (left_size > SIZE_MAX - right_size - (size_t)separator - 1) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  size_t size = left_size + (size_t)separator + right_size + 1;
  char *result = malloc(size);
  if (!result)
    return NULL;
  memcpy(result, left, left_size);
  size_t offset = left_size;
  if (separator)
    result[offset++] = '/';
  memcpy(result + offset, right, right_size + 1);
  return result;
}

static int sync_storage_directory(const char *directory) {
  int descriptor = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (descriptor < 0)
    return -1;
  int result;
  do {
    result = fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  int saved_errno = result == 0 ? 0 : errno;
  if (close(descriptor) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  errno = result == 0 ? 0 : saved_errno;
  return result;
}

static int atomic_write_shared_file(const char *path, const char *directory,
                                    const unsigned char *header,
                                    const unsigned char *payload,
                                    size_t payload_size) {
  size_t path_size = strlen(path);
  if (path_size > SIZE_MAX - 64u) {
    errno = ENAMETOOLONG;
    return -1;
  }
  char *temporary = malloc(path_size + 64u);
  if (!temporary)
    return -1;

  int descriptor = -1;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    int length = snprintf(temporary, path_size + 64u, "%s.tmp.%ld.%u", path,
                          (long)getpid(), attempt);
    if (length < 0 || (size_t)length >= path_size + 64u) {
      errno = ENAMETOOLONG;
      break;
    }
    descriptor = open(temporary,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor >= 0 || errno != EEXIST)
      break;
  }
  if (descriptor < 0) {
    int saved_errno = errno;
    free(temporary);
    errno = saved_errno;
    return -1;
  }

  int result = write_all(descriptor, header, ASM2_SHARED_HEADER_SIZE);
  if (result == 0 && payload_size)
    result = write_all(descriptor, payload, payload_size);
  if (result == 0) {
    while (fsync(descriptor) != 0) {
      if (errno != EINTR) {
        result = -1;
        break;
      }
    }
  }
  int saved_errno = result == 0 ? 0 : errno;
  if (close(descriptor) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result == 0 && rename(temporary, path) != 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result == 0) {
    /* The rename is already the commit point. Directory fsync supplies
     * reboot durability on filesystems which implement it. */
    (void)sync_storage_directory(directory);
  } else {
    (void)unlink(temporary);
  }
  free(temporary);
  errno = result == 0 ? 0 : saved_errno;
  return result;
}

static int text_equal(const char *left, const char *right) {
  return left && right && strcmp(left, right) == 0;
}

static void *object_argument(va_list *arguments,
                             const asm2_jni_value *array_arguments,
                             unsigned index) {
  if (array_arguments)
    return array_arguments[index].l;
  if (!arguments)
    return NULL;
  va_list copy;
  va_copy(copy, *arguments);
  void *result = NULL;
  for (unsigned current = 0; current <= index; ++current)
    result = va_arg(copy, void *);
  va_end(copy);
  return result;
}

static int int_argument(va_list *arguments,
                        const asm2_jni_value *array_arguments,
                        unsigned index) {
  if (array_arguments)
    return array_arguments[index].i;
  if (!arguments)
    return 0;
  va_list copy;
  va_copy(copy, *arguments);
  int result = 0;
  for (unsigned current = 0; current <= index; ++current)
    result = va_arg(copy, int);
  va_end(copy);
  return result;
}

static int64_t milliseconds_since_epoch(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    return 0;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int uptime_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return now.tv_sec > INT32_MAX ? INT32_MAX : (int)now.tv_sec;
}

static int native_connection_type(void) {
  /* The 2014 EVE profile transaction no longer completes even when its root
   * HTTPS endpoint is reachable. The guest retries forever at 45%, while zero
   * selects the game's complete native offline path. Keep that session mode
   * deterministic regardless of the host link state. */
  return 0;
}

static void shared_values_clear(struct asm2_android_state *state) {
  if (!state)
    return;
  for (size_t index = 0; index < state->shared_count; ++index) {
    free(state->shared_keys[index]);
    free(state->shared_values[index]);
    state->shared_keys[index] = NULL;
    state->shared_values[index] = NULL;
  }
  state->shared_count = 0;
}

static void shared_value_arrays_clear(char **keys, char **values,
                                      size_t count) {
  for (size_t index = 0; index < count; ++index) {
    free(keys[index]);
    free(values[index]);
    keys[index] = NULL;
    values[index] = NULL;
  }
}

static int shared_values_encoded_size(const struct asm2_android_state *state,
                                      size_t *result) {
  if (!state || !result || state->shared_count > ASM2_SHARED_MAX_ENTRIES) {
    errno = EINVAL;
    return -1;
  }
  size_t total = 0;
  for (size_t index = 0; index < state->shared_count; ++index) {
    const char *key = state->shared_keys[index];
    const char *value = state->shared_values[index];
    if (!key || !value) {
      errno = EINVAL;
      return -1;
    }
    size_t key_size = strnlen(key, ASM2_SHARED_MAX_KEY_SIZE + 1u);
    size_t value_size =
        strnlen(value, ASM2_SHARED_MAX_VALUE_SIZE + 1u);
    if (!key_size || key_size > ASM2_SHARED_MAX_KEY_SIZE ||
        value_size > ASM2_SHARED_MAX_VALUE_SIZE ||
        total > ASM2_SHARED_MAX_PAYLOAD_SIZE - 8u - key_size - value_size) {
      errno = EOVERFLOW;
      return -1;
    }
    total += 8u + key_size + value_size;
  }
  *result = total;
  return 0;
}

static int shared_values_persist(const struct asm2_android_state *state) {
  if (!state || !state->shared_storage_path)
    return 0;

  size_t payload_size;
  if (shared_values_encoded_size(state, &payload_size) != 0)
    return -1;
  unsigned char *payload = payload_size ? malloc(payload_size) : NULL;
  if (payload_size && !payload)
    return -1;

  size_t offset = 0;
  for (size_t index = 0; index < state->shared_count; ++index) {
    size_t key_size = strlen(state->shared_keys[index]);
    size_t value_size = strlen(state->shared_values[index]);
    put_u32_le(payload + offset, (uint32_t)key_size);
    put_u32_le(payload + offset + 4u, (uint32_t)value_size);
    offset += 8u;
    memcpy(payload + offset, state->shared_keys[index], key_size);
    offset += key_size;
    memcpy(payload + offset, state->shared_values[index], value_size);
    offset += value_size;
  }

  unsigned char header[ASM2_SHARED_HEADER_SIZE];
  memcpy(header, asm2_shared_magic, sizeof(asm2_shared_magic));
  put_u32_le(header + 8u, 1u);
  put_u32_le(header + 12u, (uint32_t)state->shared_count);
  put_u32_le(header + 16u, (uint32_t)payload_size);
  put_u32_le(header + 20u, shared_crc32(payload, payload_size));
  int result = atomic_write_shared_file(
      state->shared_storage_path, state->shared_storage_directory, header,
      payload, payload_size);
  free(payload);
  return result;
}

static enum shared_load_status shared_values_load_file(
    const char *path, char **keys, char **values, size_t *result_count) {
  *result_count = 0;
  int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ENOENT)
      return SHARED_LOAD_MISSING;
    return SHARED_LOAD_IO_ERROR;
  }

  struct stat status;
  if (fstat(descriptor, &status) != 0) {
    int saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    return SHARED_LOAD_IO_ERROR;
  }
  if (!S_ISREG(status.st_mode) ||
      status.st_size < (off_t)ASM2_SHARED_HEADER_SIZE ||
      (uintmax_t)status.st_size >
          (uintmax_t)ASM2_SHARED_HEADER_SIZE + ASM2_SHARED_MAX_PAYLOAD_SIZE) {
    close(descriptor);
    errno = EINVAL;
    return SHARED_LOAD_CORRUPT;
  }

  size_t file_size = (size_t)status.st_size;
  unsigned char *contents = malloc(file_size);
  if (!contents) {
    int saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    return SHARED_LOAD_IO_ERROR;
  }
  int result = read_all(descriptor, contents, file_size);
  if (result == 0) {
    unsigned char trailing;
    ssize_t received;
    do {
      received = read(descriptor, &trailing, 1);
    } while (received < 0 && errno == EINTR);
    if (received != 0) {
      result = -1;
      if (received > 0) {
        free(contents);
        close(descriptor);
        errno = EINVAL;
        return SHARED_LOAD_CORRUPT;
      }
    }
  }
  int saved_errno = errno;
  if (close(descriptor) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result != 0) {
    free(contents);
    errno = saved_errno;
    return SHARED_LOAD_IO_ERROR;
  }

  uint32_t version = get_u32_le(contents + 8u);
  uint32_t count = get_u32_le(contents + 12u);
  uint32_t payload_size = get_u32_le(contents + 16u);
  uint32_t expected_crc = get_u32_le(contents + 20u);
  const unsigned char *payload = contents + ASM2_SHARED_HEADER_SIZE;
  if (memcmp(contents, asm2_shared_magic, sizeof(asm2_shared_magic)) != 0 ||
      version != 1u || count > ASM2_SHARED_MAX_ENTRIES ||
      payload_size != file_size - ASM2_SHARED_HEADER_SIZE ||
      payload_size > ASM2_SHARED_MAX_PAYLOAD_SIZE ||
      shared_crc32(payload, payload_size) != expected_crc) {
    free(contents);
    errno = EINVAL;
    return SHARED_LOAD_CORRUPT;
  }

  size_t offset = 0;
  size_t loaded = 0;
  for (uint32_t entry = 0; entry < count; ++entry) {
    if (payload_size - offset < 8u)
      goto invalid_file;
    uint32_t key_size = get_u32_le(payload + offset);
    uint32_t value_size = get_u32_le(payload + offset + 4u);
    offset += 8u;
    if (!key_size || key_size > ASM2_SHARED_MAX_KEY_SIZE ||
        value_size > ASM2_SHARED_MAX_VALUE_SIZE ||
        (size_t)key_size > payload_size - offset ||
        (size_t)value_size > payload_size - offset - key_size ||
        memchr(payload + offset, '\0', key_size) ||
        memchr(payload + offset + key_size, '\0', value_size))
      goto invalid_file;

    keys[loaded] = malloc((size_t)key_size + 1u);
    values[loaded] = malloc((size_t)value_size + 1u);
    if (!keys[loaded] || !values[loaded]) {
      int allocation_errno = errno ? errno : ENOMEM;
      free(keys[loaded]);
      free(values[loaded]);
      keys[loaded] = NULL;
      values[loaded] = NULL;
      shared_value_arrays_clear(keys, values, loaded);
      free(contents);
      errno = allocation_errno;
      return SHARED_LOAD_IO_ERROR;
    }
    memcpy(keys[loaded], payload + offset, key_size);
    keys[loaded][key_size] = '\0';
    offset += key_size;
    memcpy(values[loaded], payload + offset, value_size);
    values[loaded][value_size] = '\0';
    offset += value_size;
    for (size_t previous = 0; previous < loaded; ++previous) {
      if (strcmp(keys[previous], keys[loaded]) == 0)
        goto invalid_file_with_current;
    }
    ++loaded;
  }
  if (offset != payload_size)
    goto invalid_file;

  free(contents);
  *result_count = loaded;
  return SHARED_LOAD_OK;

invalid_file_with_current:
  free(keys[loaded]);
  free(values[loaded]);
  keys[loaded] = NULL;
  values[loaded] = NULL;
invalid_file:
  shared_value_arrays_clear(keys, values, loaded);
  free(contents);
  errno = EINVAL;
  return SHARED_LOAD_CORRUPT;
}

static int rename_no_replace(const char *source, const char *destination) {
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
  if (syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, destination,
              RENAME_NOREPLACE) == 0)
    return 0;
  if (errno != ENOSYS && errno != EINVAL)
    return -1;
#endif
  struct stat status;
  if (lstat(destination, &status) == 0) {
    errno = EEXIST;
    return -1;
  }
  if (errno != ENOENT)
    return -1;
  return rename(source, destination);
}

static int quarantine_invalid_shared_store(const char *path,
                                           const char *directory) {
  struct timespec now = {0, 0};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    now.tv_sec = time(NULL);
  size_t path_size = strlen(path);
  if (path_size > SIZE_MAX - 96u) {
    errno = ENAMETOOLONG;
    return -1;
  }
  char *quarantine = malloc(path_size + 96u);
  if (!quarantine)
    return -1;
  int moved = 0;
  for (unsigned attempt = 0; attempt < 100u; ++attempt) {
    int size = snprintf(quarantine, path_size + 96u,
                        "%s.corrupt-%lld-%09ld-%ld-%02u", path,
                        (long long)now.tv_sec, now.tv_nsec, (long)getpid(),
                        attempt);
    if (size < 0 || (size_t)size >= path_size + 96u) {
      errno = ENAMETOOLONG;
      break;
    }
    if (rename_no_replace(path, quarantine) == 0) {
      moved = 1;
      break;
    }
    if (errno != EEXIST)
      break;
  }
  if (!moved) {
    int saved_errno = errno ? errno : EEXIST;
    free(quarantine);
    errno = saved_errno;
    return -1;
  }
  if (sync_storage_directory(directory) != 0) {
    int saved_errno = errno;
    debugPrintf("ASM2_SHARED_RECOVER directory sync warning error=%d\n",
                saved_errno);
  }
  const char *name = strrchr(quarantine, '/');
  debugPrintf("ASM2_SHARED_RECOVER quarantined=%s\n",
              name ? name + 1 : quarantine);
  free(quarantine);
  return 0;
}

int asm2_android_set_storage_root(struct asm2_android_state *state,
                                  const char *host_gamefiles_root) {
  if (!state || !host_gamefiles_root || !host_gamefiles_root[0]) {
    errno = EINVAL;
    return -1;
  }

  char *directory =
      join_storage_path(host_gamefiles_root, ASM2_SHARED_DIRECTORY);
  if (!directory)
    return -1;
  if (ensure_directory_tree(directory) != 0) {
    int saved_errno = errno;
    free(directory);
    errno = saved_errno;
    return -1;
  }
  char *path = join_storage_path(directory, ASM2_SHARED_FILENAME);
  if (!path) {
    int saved_errno = errno;
    free(directory);
    errno = saved_errno;
    return -1;
  }

  char *loaded_keys[ASM2_SHARED_MAX_ENTRIES] = {0};
  char *loaded_values[ASM2_SHARED_MAX_ENTRIES] = {0};
  size_t loaded_count = 0;
  enum shared_load_status load_status =
      shared_values_load_file(path, loaded_keys, loaded_values,
                              &loaded_count);
  if (load_status == SHARED_LOAD_IO_ERROR ||
      (load_status == SHARED_LOAD_CORRUPT &&
       quarantine_invalid_shared_store(path, directory) != 0)) {
    int saved_errno = errno;
    free(path);
    free(directory);
    errno = saved_errno;
    return -1;
  }

  if (state->shared_mutex_initialized)
    pthread_mutex_lock(&state->shared_mutex);
  shared_values_clear(state);
  free(state->shared_storage_path);
  free(state->shared_storage_directory);
  state->shared_storage_path = path;
  state->shared_storage_directory = directory;
  for (size_t index = 0; index < loaded_count; ++index) {
    state->shared_keys[index] = loaded_keys[index];
    state->shared_values[index] = loaded_values[index];
  }
  state->shared_count = loaded_count;
  if (state->shared_mutex_initialized)
    pthread_mutex_unlock(&state->shared_mutex);
  if (!ensure_installer_preferences(state)) {
    if (!errno)
      errno = EIO;
    debugPrintf("ASM2_PREF seed failed pref=%s key=%s\n",
                ASM2_INSTALLER_PREFERENCES, ASM2_INSTALLER_SD_KEY);
    return -1;
  }
  return 0;
}

void asm2_android_state_init(struct asm2_android_state *state) {
  memset(state, 0, sizeof(*state));
  atomic_init(&state->lifecycle_request, ASM2_ANDROID_LIFECYCLE_NONE);
  atomic_init(&state->game_options_exit_phase, GAME_OPTIONS_EXIT_IDLE);
  int mutex_result = pthread_mutex_init(&state->shared_mutex, NULL);
  if (mutex_result == 0)
    state->shared_mutex_initialized = 1;
  else
    debugPrintf("ASM2_CALLBACK shared mutex init failed error=%d\n",
                mutex_result);
  state->screen_width = 1280;
  state->screen_height = 720;
  state->resource_directory = ASM2_RESOURCE_PATH;
  state->files_directory = ASM2_FILES_PATH;
  state->cache_directory = ASM2_CACHE_PATH;
}

void asm2_android_set_display(struct asm2_android_state *state, int width,
                              int height) {
  if (!state)
    return;
  if (width > 0)
    state->screen_width = width;
  if (height > 0)
    state->screen_height = height;
}

void asm2_android_bind_paths(struct asm2_android_state *state,
                             asm2_guest_set_paths_fn set_paths,
                             void *gl2jni_class) {
  if (!state)
    return;
  state->set_paths = set_paths;
  state->gl2jni_class = gl2jni_class;
}

void asm2_android_bind_game_options_exit(
    struct asm2_android_state *state, asm2_guest_lifecycle_fn function,
    void *environment, void *class_handle) {
  if (!state)
    return;
  state->game_options_exit = function;
  state->game_options_environment = environment;
  state->game_options_class = class_handle;
}

enum asm2_android_lifecycle_request asm2_android_get_lifecycle_request(
    const struct asm2_android_state *state) {
  if (!state)
    return ASM2_ANDROID_LIFECYCLE_NONE;
  return atomic_load_explicit(&state->lifecycle_request,
                              memory_order_acquire);
}

int asm2_android_lifecycle_requested(
    const struct asm2_android_state *state) {
  return asm2_android_get_lifecycle_request(state) !=
         ASM2_ANDROID_LIFECYCLE_NONE;
}

int asm2_android_game_options_exit_done(
    const struct asm2_android_state *state) {
  if (!state)
    return 0;
  return atomic_load_explicit(&state->game_options_exit_phase,
                              memory_order_acquire) ==
         GAME_OPTIONS_EXIT_DONE;
}

static int lifecycle_request_once(
    struct asm2_android_state *state,
    enum asm2_android_lifecycle_request request) {
  enum asm2_android_lifecycle_request expected =
      ASM2_ANDROID_LIFECYCLE_NONE;
  return atomic_compare_exchange_strong_explicit(
      &state->lifecycle_request, &expected, request, memory_order_acq_rel,
      memory_order_acquire);
}

static void dispatch_game_options_exit_once(
    struct asm2_android_state *state) {
  int expected = GAME_OPTIONS_EXIT_IDLE;
  if (!atomic_compare_exchange_strong_explicit(
          &state->game_options_exit_phase, &expected,
          GAME_OPTIONS_EXIT_RUNNING, memory_order_acq_rel,
          memory_order_acquire))
    return;

  if (state->game_options_exit) {
    debugPrintf("ASM2_CALLBACK GameOptions.ExitGame begin\n");
    state->game_options_exit(state->game_options_environment,
                             state->game_options_class);
    debugPrintf("ASM2_CALLBACK GameOptions.ExitGame returned\n");
  } else {
    /* A mismatched guest must still be able to leave without a NULL branch. */
    debugPrintf("ASM2_CALLBACK GameOptions.ExitGame unavailable; exit safe\n");
  }

  atomic_store_explicit(&state->game_options_exit_phase,
                        GAME_OPTIONS_EXIT_DONE, memory_order_release);
}

static int return_string(asm2_jni_value *result, const char *text) {
  result->l = asm2_jni_string(text);
  return 1;
}

static size_t shared_value_index(const struct asm2_android_state *state,
                                 const char *key) {
  if (!state || !key)
    return SIZE_MAX;
  for (size_t index = 0; index < state->shared_count; ++index) {
    if (state->shared_keys[index] &&
        strcmp(state->shared_keys[index], key) == 0)
      return index;
  }
  return SIZE_MAX;
}

static int shared_value_set_locked(struct asm2_android_state *state,
                                   const char *key, const char *value) {
  if (!state || !key || !key[0])
    return 0;
  if (!value)
    value = "";
  size_t key_size = strnlen(key, ASM2_SHARED_MAX_KEY_SIZE + 1u);
  size_t value_size =
      strnlen(value, ASM2_SHARED_MAX_VALUE_SIZE + 1u);
  if (!key_size || key_size > ASM2_SHARED_MAX_KEY_SIZE ||
      value_size > ASM2_SHARED_MAX_VALUE_SIZE)
    return 0;

  size_t index = shared_value_index(state, key);
  if (index != SIZE_MAX && state->shared_values[index] &&
      strcmp(state->shared_values[index], value) == 0)
    return 1;
  size_t current_size;
  if (shared_values_encoded_size(state, &current_size) != 0)
    return 0;
  size_t previous_size = 0;
  if (index != SIZE_MAX)
    previous_size = 8u + strlen(state->shared_keys[index]) +
                    strlen(state->shared_values[index]);
  if (current_size - previous_size >
      ASM2_SHARED_MAX_PAYLOAD_SIZE - 8u - key_size - value_size)
    return 0;

  char *value_copy = strdup(value);
  if (!value_copy)
    return 0;
  if (index == SIZE_MAX) {
    if (state->shared_count >= ASM2_SHARED_MAX_ENTRIES) {
      free(value_copy);
      return 0;
    }
    char *key_copy = strdup(key);
    if (!key_copy) {
      free(value_copy);
      return 0;
    }
    index = state->shared_count;
    state->shared_keys[index] = key_copy;
    state->shared_values[index] = value_copy;
    ++state->shared_count;
    if (shared_values_persist(state) != 0) {
      --state->shared_count;
      state->shared_keys[index] = NULL;
      state->shared_values[index] = NULL;
      free(key_copy);
      free(value_copy);
      debugPrintf("ASM2_CALLBACK shared-value write failed error=%d\n",
                  errno);
      return 0;
    }
    return 1;
  }

  char *old_value = state->shared_values[index];
  state->shared_values[index] = value_copy;
  if (shared_values_persist(state) != 0) {
    state->shared_values[index] = old_value;
    free(value_copy);
    debugPrintf("ASM2_CALLBACK shared-value write failed error=%d\n", errno);
    return 0;
  }
  free(old_value);
  return 1;
}

static int shared_value_set(struct asm2_android_state *state, const char *key,
                            const char *value) {
  if (!state)
    return 0;
  if (state->shared_mutex_initialized)
    pthread_mutex_lock(&state->shared_mutex);
  int result = shared_value_set_locked(state, key, value);
  if (state->shared_mutex_initialized)
    pthread_mutex_unlock(&state->shared_mutex);
  return result;
}

static int shared_value_delete_locked(struct asm2_android_state *state,
                                      const char *key) {
  size_t index = shared_value_index(state, key);
  if (index == SIZE_MAX)
    return 0;
  char *removed_key = state->shared_keys[index];
  char *removed_value = state->shared_values[index];
  for (size_t current = index + 1; current < state->shared_count; ++current) {
    state->shared_keys[current - 1] = state->shared_keys[current];
    state->shared_values[current - 1] = state->shared_values[current];
  }
  --state->shared_count;
  state->shared_keys[state->shared_count] = NULL;
  state->shared_values[state->shared_count] = NULL;
  if (shared_values_persist(state) != 0) {
    for (size_t current = state->shared_count; current > index; --current) {
      state->shared_keys[current] = state->shared_keys[current - 1];
      state->shared_values[current] = state->shared_values[current - 1];
    }
    state->shared_keys[index] = removed_key;
    state->shared_values[index] = removed_value;
    ++state->shared_count;
    debugPrintf("ASM2_CALLBACK shared-value write failed error=%d\n", errno);
    return 0;
  }
  free(removed_key);
  free(removed_value);
  return 1;
}

static int shared_value_delete(struct asm2_android_state *state,
                               const char *key) {
  if (!state)
    return 0;
  if (state->shared_mutex_initialized)
    pthread_mutex_lock(&state->shared_mutex);
  int result = shared_value_delete_locked(state, key);
  if (state->shared_mutex_initialized)
    pthread_mutex_unlock(&state->shared_mutex);
  return result;
}

static int shared_value_get(struct asm2_android_state *state, const char *key,
                            asm2_jni_value *result) {
  if (!state || !result)
    return 0;
  if (state->shared_mutex_initialized)
    pthread_mutex_lock(&state->shared_mutex);
  size_t index = shared_value_index(state, key);
  const char *value = index == SIZE_MAX ? "" : state->shared_values[index];
  result->l = asm2_jni_string(value ? value : "");
  if (state->shared_mutex_initialized)
    pthread_mutex_unlock(&state->shared_mutex);
  return 1;
}

static int shared_value_exists(struct asm2_android_state *state,
                               const char *key) {
  if (!state)
    return 0;
  if (state->shared_mutex_initialized)
    pthread_mutex_lock(&state->shared_mutex);
  int result = shared_value_index(state, key) != SIZE_MAX;
  if (state->shared_mutex_initialized)
    pthread_mutex_unlock(&state->shared_mutex);
  return result;
}

/*
 * SUtils.nativeGetPreference/nativeSetPreference are the Java side of the
 * native preference bridge used by libtasm2.so after the Java installer.
 * Keep these values in the already crash-safe DataSharing store, with a
 * private namespace so public DataSharing keys cannot collide.
 */
static char *preference_storage_key(const char *preference_name,
                                    const char *key) {
  if (!preference_name)
    preference_name = "";
  if (!key)
    return NULL;
  size_t prefix_size = sizeof(ASM2_PREFERENCE_KEY_PREFIX) - 1u;
  size_t preference_size = strlen(preference_name);
  size_t key_size = strlen(key);
  char length_text[32];
  int length_size =
      snprintf(length_text, sizeof(length_text), "%zu:", preference_size);
  if (length_size < 0 || (size_t)length_size >= sizeof(length_text) ||
      prefix_size > SIZE_MAX - (size_t)length_size ||
      prefix_size + (size_t)length_size > SIZE_MAX - preference_size - 1u ||
      prefix_size + (size_t)length_size + preference_size + 1u >
          SIZE_MAX - key_size - 1u)
    return NULL;
  size_t total = prefix_size + (size_t)length_size + preference_size + 1u +
                 key_size + 1u;
  if (total - 1u > ASM2_SHARED_MAX_KEY_SIZE)
    return NULL;
  char *storage_key = malloc(total);
  if (!storage_key)
    return NULL;
  size_t offset = 0;
  memcpy(storage_key + offset, ASM2_PREFERENCE_KEY_PREFIX, prefix_size);
  offset += prefix_size;
  memcpy(storage_key + offset, length_text, (size_t)length_size);
  offset += (size_t)length_size;
  memcpy(storage_key + offset, preference_name, preference_size);
  offset += preference_size;
  storage_key[offset++] = ':';
  memcpy(storage_key + offset, key, key_size + 1u);
  return storage_key;
}

static char *shared_value_copy(struct asm2_android_state *state,
                               const char *key) {
  if (!state || !key)
    return NULL;
  if (state->shared_mutex_initialized)
    pthread_mutex_lock(&state->shared_mutex);
  size_t index = shared_value_index(state, key);
  char *copy =
      index == SIZE_MAX || !state->shared_values[index]
          ? NULL
          : strdup(state->shared_values[index]);
  if (state->shared_mutex_initialized)
    pthread_mutex_unlock(&state->shared_mutex);
  return copy;
}

static int ensure_installer_preferences(struct asm2_android_state *state) {
  if (!state || !state->resource_directory)
    return 0;
  char *storage_key =
      preference_storage_key(ASM2_INSTALLER_PREFERENCES,
                             ASM2_INSTALLER_SD_KEY);
  if (!storage_key)
    return 0;
  char *stored = shared_value_copy(state, storage_key);
  if (stored && stored[0] == 's' && stored[1] == ':') {
    free(stored);
    free(storage_key);
    return 1;
  }
  free(stored);
  size_t path_size = strlen(state->resource_directory);
  if (path_size > ASM2_SHARED_MAX_VALUE_SIZE - 2u) {
    free(storage_key);
    return 0;
  }
  char *encoded = malloc(path_size + 3u);
  if (!encoded) {
    free(storage_key);
    return 0;
  }
  encoded[0] = 's';
  encoded[1] = ':';
  memcpy(encoded + 2, state->resource_directory, path_size + 1u);
  errno = 0;
  int result = shared_value_set(state, storage_key, encoded);
  if (!result && !errno)
    errno = ENOSPC;
  if (result)
    debugPrintf("ASM2_PREF seeded pref=%s key=%s\n",
                ASM2_INSTALLER_PREFERENCES, ASM2_INSTALLER_SD_KEY);
  free(encoded);
  free(storage_key);
  return result;
}

static int preference_put_default(void *bundle, asm2_jint type) {
  switch (type) {
    case 0: {
      asm2_jint value = 0;
      (void)asm2_jni_bundle_get_int(bundle, "npDefaultValue", &value);
      return asm2_jni_bundle_put_int(bundle, "npResult", value);
    }
    case 1: {
      asm2_jlong value = 0;
      (void)asm2_jni_bundle_get_long(bundle, "npDefaultValue", &value);
      return asm2_jni_bundle_put_long(bundle, "npResult", value);
    }
    case 2: {
      asm2_jboolean value = 0;
      (void)asm2_jni_bundle_get_boolean(bundle, "npDefaultValue", &value);
      return asm2_jni_bundle_put_boolean(bundle, "npResult", value);
    }
    case 3:
      return asm2_jni_bundle_put_string(
          bundle, "npResult",
          asm2_jni_bundle_get_string(bundle, "npDefaultValue"));
    default:
      return 1;
  }
}

static int parse_preference_int(const char *text, asm2_jint *result) {
  if (!text || text[0] != 'i' || text[1] != ':' || !text[2])
    return 0;
  errno = 0;
  char *end = NULL;
  long value = strtol(text + 2, &end, 10);
  if (errno || !end || *end || value < INT32_MIN || value > INT32_MAX)
    return 0;
  *result = (asm2_jint)value;
  return 1;
}

static int parse_preference_long(const char *text, asm2_jlong *result) {
  if (!text || text[0] != 'j' || text[1] != ':' || !text[2])
    return 0;
  errno = 0;
  char *end = NULL;
  long long value = strtoll(text + 2, &end, 10);
  if (errno || !end || *end)
    return 0;
  *result = (asm2_jlong)value;
  return 1;
}

static int preference_get(struct asm2_android_state *state, void *bundle,
                          asm2_jni_value *result) {
  asm2_jint type = 0;
  (void)asm2_jni_bundle_get_int(bundle, "npDataType", &type);
  const char *key = asm2_jni_bundle_get_string(bundle, "npKey");
  const char *preference_name =
      asm2_jni_bundle_get_string(bundle, "npPrefName");
  char *storage_key = preference_storage_key(preference_name, key);
  char *stored = storage_key ? shared_value_copy(state, storage_key) : NULL;
  int valid = 0;

  if (stored) {
    switch (type) {
      case 0: {
        asm2_jint value;
        valid = parse_preference_int(stored, &value);
        if (valid)
          valid = asm2_jni_bundle_put_int(bundle, "npResult", value);
        break;
      }
      case 1: {
        asm2_jlong value;
        valid = parse_preference_long(stored, &value);
        if (valid)
          valid = asm2_jni_bundle_put_long(bundle, "npResult", value);
        break;
      }
      case 2:
        valid = stored[0] == 'z' && stored[1] == ':' &&
                (stored[2] == '0' || stored[2] == '1') && stored[3] == '\0';
        if (valid)
          valid = asm2_jni_bundle_put_boolean(
              bundle, "npResult", (asm2_jboolean)(stored[2] == '1'));
        break;
      case 3:
        valid = stored[0] == 's' && stored[1] == ':';
        if (valid)
          valid = asm2_jni_bundle_put_string(bundle, "npResult", stored + 2);
        break;
      default:
        valid = 1;
        break;
    }
  }
  if (!valid)
    (void)preference_put_default(bundle, type);
  if (getenv("ASM2_PREF_DEBUG"))
    debugPrintf("ASM2_PREF get pref=%s key=%s type=%d source=%s\n",
                preference_name ? preference_name : "",
                key ? key : "(null)", type, valid ? "stored" : "default");
  free(stored);
  free(storage_key);
  result->l = bundle;
  return 1;
}

static int preference_set(struct asm2_android_state *state, void *bundle) {
  asm2_jint type = 0;
  (void)asm2_jni_bundle_get_int(bundle, "npDataType", &type);
  const char *key = asm2_jni_bundle_get_string(bundle, "npKey");
  const char *preference_name =
      asm2_jni_bundle_get_string(bundle, "npPrefName");
  char *storage_key = preference_storage_key(preference_name, key);
  char number[64];
  char *encoded = NULL;

  switch (type) {
    case 0: {
      asm2_jint value = 0;
      (void)asm2_jni_bundle_get_int(bundle, "npData", &value);
      int size = snprintf(number, sizeof(number), "i:%d", value);
      if (size > 0 && (size_t)size < sizeof(number))
        encoded = strdup(number);
      break;
    }
    case 1: {
      asm2_jlong value = 0;
      (void)asm2_jni_bundle_get_long(bundle, "npData", &value);
      int size =
          snprintf(number, sizeof(number), "j:%lld", (long long)value);
      if (size > 0 && (size_t)size < sizeof(number))
        encoded = strdup(number);
      break;
    }
    case 2: {
      asm2_jboolean value = 0;
      (void)asm2_jni_bundle_get_boolean(bundle, "npData", &value);
      encoded = strdup(value ? "z:1" : "z:0");
      break;
    }
    case 3: {
      const char *value = asm2_jni_bundle_get_string(bundle, "npData");
      if (value) {
        size_t size = strlen(value);
        if (size <= ASM2_SHARED_MAX_VALUE_SIZE - 2u) {
          encoded = malloc(size + 3u);
          if (encoded) {
            encoded[0] = 's';
            encoded[1] = ':';
            memcpy(encoded + 2, value, size + 1u);
          }
        }
      }
      break;
    }
    default:
      break;
  }

  int stored = storage_key && encoded &&
               shared_value_set(state, storage_key, encoded);
  if (getenv("ASM2_PREF_DEBUG"))
    debugPrintf("ASM2_PREF set pref=%s key=%s type=%d result=%s\n",
                preference_name ? preference_name : "",
                key ? key : "(null)", type, stored ? "stored" : "ignored");
  free(encoded);
  free(storage_key);
  return 1;
}

static int preference_get_string_method(
    struct asm2_android_state *state, const char *signature,
    va_list *arguments, const asm2_jni_value *array_arguments,
    asm2_jni_value *result) {
  const char *key = asm2_jni_string_chars(
      object_argument(arguments, array_arguments, 0));
  const char *default_value = "";
  const char *preference_name = NULL;
  if (text_equal(
          signature,
          "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")) {
    preference_name = asm2_jni_string_chars(
        object_argument(arguments, array_arguments, 1));
  } else if (text_equal(
                 signature,
                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
                 "Ljava/lang/String;")) {
    default_value = asm2_jni_string_chars(
        object_argument(arguments, array_arguments, 1));
    preference_name = asm2_jni_string_chars(
        object_argument(arguments, array_arguments, 2));
  } else {
    return 0;
  }

  char *storage_key = preference_storage_key(preference_name, key);
  char *stored = storage_key ? shared_value_copy(state, storage_key) : NULL;
  const char *value =
      stored && stored[0] == 's' && stored[1] == ':' ? stored + 2
                                                      : default_value;
  result->l = value ? asm2_jni_string(value) : NULL;
  free(stored);
  free(storage_key);
  return 1;
}

static int preference_storage_key_reserved(const char *key) {
  return key &&
         strncmp(key, ASM2_PREFERENCE_KEY_PREFIX,
                 sizeof(ASM2_PREFERENCE_KEY_PREFIX) - 1u) == 0;
}

int asm2_android_field_get_callback(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver,
    asm2_jni_value *result) {
  (void)user;
  (void)receiver;
  if (!result || !is_static ||
      !text_equal(class_name, ASM2_GAME_INSTALLER_CLASS) ||
      !text_equal(name, "mPreferencesName") ||
      !text_equal(signature, "Ljava/lang/String;"))
    return 0;
  result->l = asm2_jni_string(ASM2_INSTALLER_PREFERENCES);
  return result->l != NULL;
}

int asm2_android_method_callback(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver, va_list *arguments,
    const asm2_jni_value *array_arguments, asm2_jni_value *result) {
  struct asm2_android_state *state = user;
  if (!state || !name || !result)
    return 0;

  /* Exact static entry points from GL2JNIActivity in Android 1.2.7d.
   * GameActivity.c() calls GameOptions.ExitGame synchronously from
   * sExitGame, then schedules Activity finish/process death. */
  if (is_static && text_equal(class_name, ASM2_GL2JNI_ACTIVITY_CLASS) &&
      text_equal(name, "sExitGame") && text_equal(signature, "()V")) {
    if (lifecycle_request_once(state, ASM2_ANDROID_LIFECYCLE_EXIT)) {
      dispatch_game_options_exit_once(state);
      debugPrintf("ASM2_CALLBACK lifecycle=EXIT onExitDone=%d\n",
                  asm2_android_game_options_exit_done(state));
    } else {
      debugPrintf("ASM2_CALLBACK duplicate sExitGame lifecycle=%d\n",
                  (int)asm2_android_get_lifecycle_request(state));
    }
    return 1;
  }

  /* RestartGame sets the Android relaunch flag and kills the process; it does
   * not call GameOptions.ExitGame.  The launcher maps the reserved status to
   * a fresh process after this process completes its pause path. */
  if (is_static && text_equal(class_name, ASM2_GL2JNI_ACTIVITY_CLASS) &&
      text_equal(name, "RestartGame") && text_equal(signature, "()V")) {
    if (lifecycle_request_once(state, ASM2_ANDROID_LIFECYCLE_RESTART))
      debugPrintf("ASM2_CALLBACK lifecycle=RESTART exitCode=%d\n",
                  ASM2_ANDROID_RESTART_EXIT_CODE);
    else
      debugPrintf("ASM2_CALLBACK duplicate RestartGame lifecycle=%d\n",
                  (int)asm2_android_get_lifecycle_request(state));
    return 1;
  }

  if (text_equal(class_name, ASM2_SUTILS_CLASS) &&
      text_equal(name, "initCheckConnectionType")) {
    result->i = native_connection_type();
    debugPrintf("ASM2_CALLBACK connection-type=%d\n", result->i);
    return 1;
  }

  if (text_equal(class_name, ASM2_SUTILS_CLASS) &&
      text_equal(name, "nativeGetPreference") &&
      text_equal(signature,
                 "(Landroid/os/Bundle;)Landroid/os/Bundle;")) {
    void *bundle = object_argument(arguments, array_arguments, 0);
    if (!bundle)
      bundle = asm2_jni_bundle();
    return preference_get(state, bundle, result);
  }

  if (text_equal(class_name, ASM2_SUTILS_CLASS) &&
      text_equal(name, "nativeSetPreference") &&
      text_equal(signature, "(Landroid/os/Bundle;)V")) {
    void *bundle = object_argument(arguments, array_arguments, 0);
    return bundle ? preference_set(state, bundle) : 1;
  }

  if (text_equal(class_name, ASM2_BILLING_CLASS) &&
      text_equal(name, "getData")) {
    void *bundle = object_argument(arguments, array_arguments, 0);
    asm2_jint operation = -1;
    if (!asm2_jni_bundle_get_int(bundle, "O", &operation)) {
      debugPrintf("ASM2_BILLING getData missing operation\n");
      result->l = bundle;
      return 1;
    }
    if (operation == 7) {
      (void)asm2_jni_bundle_put_byte_array(bundle, "R", "", 0);
    } else if (operation == 8) {
      static const char locale_currency[] = "en_BR@currency=";
      (void)asm2_jni_bundle_put_byte_array(
          bundle, "R", locale_currency, sizeof(locale_currency) - 1u);
    } else if (operation == 10) {
      /* The original Java check reports that no supported Android market is
       * installed on this appliance. */
      (void)asm2_jni_bundle_put_int(bundle, "R", 1);
    }
    debugPrintf("ASM2_BILLING getData operation=%d\n", operation);
    result->l = bundle;
    return 1;
  }
  if (text_equal(class_name, ASM2_BILLING_CLASS))
    debugPrintf("ASM2_BILLING java method=%s signature=%s\n", name,
                signature ? signature : "");

  if (text_equal(class_name, ASM2_DATA_SHARING_CLASS)) {
    const char *key = asm2_jni_string_chars(
        object_argument(arguments, array_arguments, 0));
    int reserved = preference_storage_key_reserved(key);
    if (text_equal(name, "setSharedValue")) {
      const char *value = asm2_jni_string_chars(
          object_argument(arguments, array_arguments, 1));
      if (!reserved)
        shared_value_set(state, key, value);
      return 1;
    }
    if (text_equal(name, "getSharedValue")) {
      if (reserved)
        return return_string(result, "");
      return shared_value_get(state, key, result);
    }
    if (text_equal(name, "isSharedValue")) {
      result->z = reserved ? 0 : shared_value_exists(state, key);
      return 1;
    }
    if (text_equal(name, "deleteSharedValue")) {
      if (!reserved)
        shared_value_delete(state, key);
      return 1;
    }
  }

  if (text_equal(class_name, ASM2_SUTILS_CLASS) &&
      text_equal(name, "getPreferenceString"))
    return preference_get_string_method(state, signature, arguments,
                                        array_arguments, result);
  if (text_equal(name, "getPackage"))
    return return_string(result, ASM2_PACKAGE);
  if (text_equal(name, "getGameName"))
    return return_string(result, "ANMP.GloftASHM");
  if (text_equal(name, "getSDFolder"))
    return return_string(result, state->resource_directory);
  if (text_equal(name, "getSaveFolder"))
    return return_string(result, state->resource_directory);
  if (text_equal(name, "getContext")) {
    result->l = asm2_jni_activity();
    return 1;
  }
  if (text_equal(name, "GetApkPath"))
    return return_string(result, ASM2_APK_PATH);
  if (text_equal(name, "getGLUID")) {
    static const asm2_jint gluid[4] = {
        (asm2_jint)0xed1e84ca, (asm2_jint)0x8dd27181,
        (asm2_jint)0xba164bf8, (asm2_jint)0xc58a09ca,
    };
    result->l = asm2_jni_int_array(gluid, 4);
    return 1;
  }
  if (text_equal(name, "getGLDID"))
    return return_string(result, "4e4558544f533132");
  if (text_equal(name, "getPhoneCarrier") ||
      text_equal(name, "getMacAddress"))
    return return_string(result, "");
  if (text_equal(name, "getLocaleCountry"))
    return return_string(result, "BR");
  if (text_equal(name, "getLocaleLanguage"))
    return return_string(result, "en");
  if (text_equal(name, "getManufacturerModel"))
    return return_string(result, "NextOS_Mali-450");
  if (text_equal(name, "getUserAgent"))
    return return_string(result,
                         "Mozilla/5.0 (Linux; Android 8.1; NextOS Mali-450)");

  if (text_equal(name, "setupPaths")) {
    if (state->set_paths && !state->paths_applied) {
      void *resource = asm2_jni_string(state->resource_directory);
      void *files = asm2_jni_string(state->files_directory);
      void *cache = asm2_jni_string(state->cache_directory);
      state->set_paths(asm2_jni_env(),
                       state->gl2jni_class ? state->gl2jni_class : receiver,
                       resource, files, cache);
      state->paths_applied = 1;
      debugPrintf("ASM2_CALLBACK setupPaths applied\n");
    }
    return 1;
  }
  if (text_equal(name, "setViewSettings")) {
    for (unsigned index = 0; index < 5; ++index)
      state->view_settings[index] =
          int_argument(arguments, array_arguments, index);
    state->view_settings_received = 1;
    debugPrintf("ASM2_CALLBACK viewSettings=%d/%d/%d/%d/%d\n",
                state->view_settings[0], state->view_settings[1],
                state->view_settings[2], state->view_settings[3],
                state->view_settings[4]);
    return 1;
  }
  if (text_equal(name, "createView") ||
      text_equal(name, "NativeListenerRegistered") ||
      text_equal(name, "NativeListenerUnRegistered"))
    return 1;
  if (text_equal(name, "setCurrentContext")) {
    result->z = 1;
    return 1;
  }
  if (text_equal(name, "getResource")) {
    /* Class.getResourceAsStream returns null when an APK resource is absent. */
    result->l = NULL;
    return 1;
  }
  if (text_equal(name, "CheckHDMIState")) {
    result->i = 0;
    return 1;
  }
  if (text_equal(name, "GetHDMIName"))
    return return_string(result, "");
  if (text_equal(name, "getUDID") || text_equal(name, "getHDIDFV") ||
      text_equal(name, "getAndroidId"))
    return return_string(result, "4e4558544f533132");
  if (text_equal(name, "getCountry"))
    return return_string(result, "BR");
  if (text_equal(name, "GetWindowWidth") ||
      text_equal(name, "GetDeviceWidth")) {
    result->i = state->screen_width;
    return 1;
  }
  if (text_equal(name, "GetWindowHeight") ||
      text_equal(name, "GetDeviceHeight")) {
    result->i = state->screen_height;
    return 1;
  }
  if (text_equal(name, "GetManufacturer") ||
      text_equal(name, "getPhoneManufacturer"))
    return return_string(result, "NextOS");
  if (text_equal(name, "GetDeviceName") || text_equal(name, "getDeviceName"))
    return return_string(result, "NextOS Mali-450");
  if (text_equal(name, "GetDeviceFirmware") ||
      text_equal(name, "getDeviceFirmware"))
    return return_string(result, "8.1");
  if (text_equal(name, "GetDeviceLanguage") ||
      text_equal(name, "retrieveDeviceLanguage"))
    return return_string(result, "en");
  if (text_equal(name, "getPhoneModel"))
    return return_string(result, "Mali-450");
  if (text_equal(name, "getPhoneDevice") ||
      text_equal(name, "getPhoneProduct"))
    return return_string(result, "NextOS");
  if (text_equal(name, "retrieveDeviceCountry"))
    return return_string(result, "BR");
  if (text_equal(name, "retrieveDeviceCarrier"))
    return return_string(result, "");
  if (text_equal(name, "JGetMaxCPUSpeed")) {
    result->f = 1500.0f;
    return 1;
  }
  if (text_equal(name, "JGetCurrentCPUSpeed")) {
    result->f = 1200.0f;
    return 1;
  }
  if (text_equal(name, "JGetMaxAvailableRam")) {
    result->f = 1024.0f;
    return 1;
  }
  if (text_equal(name, "JGetFreeDiskSpace")) {
    struct statvfs filesystem;
    result->f = 0.0f;
    if (state->shared_storage_directory &&
        statvfs(state->shared_storage_directory, &filesystem) == 0) {
      const unsigned long fragment_size =
          filesystem.f_frsize ? filesystem.f_frsize : filesystem.f_bsize;
      const double available_kib =
          (double)filesystem.f_bavail * (double)fragment_size / 1024.0;
      result->f = (float)available_kib;
      if (getenv("ASM2_FS_DEBUG")) {
        static int reported;
        if (!reported) {
          reported = 1;
          debugPrintf("ASM2_FREE_DISK path=%s kib=%.0f mib=%.1f\n",
                      state->shared_storage_directory, result->f,
                      result->f / 1024.0f);
        }
      }
    } else if (getenv("ASM2_FS_DEBUG")) {
      debugPrintf("ASM2_FREE_DISK failed path=%s errno=%d\n",
                  state->shared_storage_directory
                      ? state->shared_storage_directory
                      : "?",
                  errno);
    }
    return 1;
  }
  if (text_equal(name, "JGetFreeRam")) {
    result->f = 512.0f;
    return 1;
  }
  if (text_equal(name, "GetUptimeSystem")) {
    result->i = uptime_seconds();
    return 1;
  }
  if (text_equal(name, "sGetMilliseconds")) {
    result->j = milliseconds_since_epoch();
    return 1;
  }

  if (text_equal(name, "sLaunchVideoPlayer")) {
    result->z = 0;
    return 1;
  }
  if (text_equal(name, "sIsKeyboardVisible")) {
    result->i = 0;
    return 1;
  }
  if (text_equal(name, "sGetKeyboardText")) {
    result->l = NULL;
    return 1;
  }
  if (text_equal(name, "sIGPLaunch") ||
      text_equal(name, "sBrowserLaunch") ||
      text_equal(name, "sBrowserLaunchEncrypt") ||
      text_equal(name, "sShowKeyboard") ||
      text_equal(name, "sWelcomeScreenLaunch") ||
      text_equal(name, "sWelcomeScreenSetIsPau") ||
      text_equal(name, "trackAndroidHits") ||
      text_equal(name, "sOnlineTracker") ||
      text_equal(name, "setAnonymousID") || text_equal(name, "sShowToast") ||
      text_equal(name, "sSetFixedSize"))
    return 1;

  (void)class_name;
  return 0;
}
