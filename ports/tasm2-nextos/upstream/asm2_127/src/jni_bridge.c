#include "jni_bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define ASM2_JNI_VERSION_1_1 0x00010001
#define ASM2_JNI_VERSION_1_2 0x00010002
#define ASM2_JNI_VERSION_1_4 0x00010004
#define ASM2_JNI_VERSION_1_6 0x00010006
#define ASM2_JNI_OK 0
#define ASM2_JNI_ERR (-1)
#define ASM2_JNI_EDETACHED (-2)
#define ASM2_JNI_EVERSION (-3)
#define ASM2_JNI_ABORT 2
#define ASM2_JNI_TABLE_SLOTS 233
#define ASM2_JVM_TABLE_SLOTS 8

typedef enum asm2_handle_kind {
  ASM2_HANDLE_CLASS,
  ASM2_HANDLE_OBJECT,
  ASM2_HANDLE_STRING,
  ASM2_HANDLE_METHOD,
  ASM2_HANDLE_FIELD,
  ASM2_HANDLE_ARRAY,
  ASM2_HANDLE_DIRECT_BUFFER,
  ASM2_HANDLE_THROWABLE
} asm2_handle_kind;

typedef enum asm2_bundle_value_kind {
  ASM2_BUNDLE_BOOLEAN,
  ASM2_BUNDLE_INT,
  ASM2_BUNDLE_LONG,
  ASM2_BUNDLE_OBJECT,
} asm2_bundle_value_kind;

typedef struct asm2_bundle_entry {
  struct asm2_bundle_entry *next;
  char *key;
  asm2_bundle_value_kind kind;
  asm2_jni_value value;
} asm2_bundle_entry;

typedef struct asm2_handle {
  struct asm2_handle *next;
  struct asm2_handle *hash_next;
  struct asm2_handle *intern_next;
  asm2_handle_kind kind;
  char *class_name;
  char *name;
  char *signature;
  char *text;
  asm2_jchar *utf16;
  size_t utf16_length;
  int is_static;
  int has_value;
  asm2_jni_value value;
  void *data;
  size_t length;
  size_t element_size;
  asm2_jlong capacity;
  asm2_bundle_entry *bundle_entries;
} asm2_handle;

typedef struct asm2_registered_native {
  struct asm2_registered_native *next;
  char *class_name;
  char *name;
  char *signature;
  void *function;
} asm2_registered_native;

/* ARM32 JNINativeMethod is three consecutive pointers. */
typedef struct asm2_native_method {
  const char *name;
  const char *signature;
  void *function;
} asm2_native_method;

static pthread_mutex_t asm2_jni_lock = PTHREAD_MUTEX_INITIALIZER;
static asm2_handle *asm2_handles;
#define ASM2_HANDLE_BUCKETS 1024u
static asm2_handle *asm2_handle_buckets[ASM2_HANDLE_BUCKETS];
static asm2_handle *asm2_intern_buckets[ASM2_HANDLE_BUCKETS];
static uint32_t asm2_handle_bucket_lengths[ASM2_HANDLE_BUCKETS];
static uint32_t asm2_intern_bucket_lengths[ASM2_HANDLE_BUCKETS];
static asm2_registered_native *asm2_natives;
static size_t asm2_native_count;
static struct asm2_jni_stats asm2_stats;
static asm2_jni_config asm2_config;
static int asm2_initialized;
static __thread void *asm2_pending_exception;

static void *asm2_env_table[ASM2_JNI_TABLE_SLOTS];
static void **asm2_env_functions = asm2_env_table;
static void *asm2_vm_table[ASM2_JVM_TABLE_SLOTS];
static void **asm2_vm_functions = asm2_vm_table;

static char *asm2_duplicate(const char *text) {
  if (!text)
    return NULL;
  size_t length = strlen(text) + 1;
  char *copy = malloc(length);
  if (copy)
    memcpy(copy, text, length);
  return copy;
}

static int asm2_text_equal(const char *left, const char *right) {
  if (left == right)
    return 1;
  return left && right && strcmp(left, right) == 0;
}

static unsigned int asm2_handle_bucket(const void *pointer) {
  uintptr_t value = (uintptr_t)pointer;
  value >>= 4;
  value = ((value >> 16) ^ value) * UINT32_C(0x45d9f3b);
  value = ((value >> 16) ^ value) * UINT32_C(0x45d9f3b);
  value ^= value >> 16;
  return (unsigned int)value & (ASM2_HANDLE_BUCKETS - 1u);
}

static uint32_t asm2_hash_text(uint32_t hash, const char *text) {
  const unsigned char *cursor =
      (const unsigned char *)(text ? text : "");
  while (*cursor) {
    hash ^= *cursor++;
    hash *= UINT32_C(16777619);
  }
  hash ^= 0xffu;
  return hash * UINT32_C(16777619);
}

static unsigned int asm2_intern_bucket(asm2_handle_kind kind,
                                       const char *class_name,
                                       const char *name,
                                       const char *signature,
                                       const char *text, int is_static) {
  uint32_t hash = UINT32_C(2166136261);
  hash ^= (uint32_t)kind;
  hash *= UINT32_C(16777619);
  hash ^= (uint32_t)!!is_static;
  hash *= UINT32_C(16777619);
  hash = asm2_hash_text(hash, class_name);
  hash = asm2_hash_text(hash, name);
  hash = asm2_hash_text(hash, signature);
  hash = asm2_hash_text(hash, text);
  hash ^= hash >> 16;
  return hash & (ASM2_HANDLE_BUCKETS - 1u);
}

static void asm2_publish_intern_locked(asm2_handle *item,
                                       unsigned int bucket) {
  item->intern_next = asm2_intern_buckets[bucket];
  asm2_intern_buckets[bucket] = item;
  uint32_t length = ++asm2_intern_bucket_lengths[bucket];
  if (length > asm2_stats.longest_intern_bucket)
    asm2_stats.longest_intern_bucket = length;
}

static asm2_handle *asm2_find_handle_locked(void *pointer) {
  for (asm2_handle *item =
           asm2_handle_buckets[asm2_handle_bucket(pointer)];
       item; item = item->hash_next) {
    if (item == pointer)
      return item;
  }
  return NULL;
}

static asm2_handle *asm2_lookup_handle(void *pointer) {
  if (!pointer)
    return NULL;
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *item = asm2_find_handle_locked(pointer);
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static asm2_handle *asm2_allocate_handle_locked(asm2_handle_kind kind,
                                                 const char *class_name) {
  asm2_handle *item = calloc(1, sizeof(*item));
  if (!item)
    return NULL;
  item->kind = kind;
  item->class_name = asm2_duplicate(class_name);
  item->next = asm2_handles;
  asm2_handles = item;
  unsigned int bucket = asm2_handle_bucket(item);
  item->hash_next = asm2_handle_buckets[bucket];
  asm2_handle_buckets[bucket] = item;
  uint32_t length = ++asm2_handle_bucket_lengths[bucket];
  if (length > asm2_stats.longest_handle_bucket)
    asm2_stats.longest_handle_bucket = length;
  ++asm2_stats.handles;
  switch (kind) {
  case ASM2_HANDLE_CLASS:
    ++asm2_stats.classes;
    break;
  case ASM2_HANDLE_OBJECT:
    ++asm2_stats.objects;
    break;
  case ASM2_HANDLE_STRING:
    ++asm2_stats.strings;
    break;
  case ASM2_HANDLE_METHOD:
    ++asm2_stats.method_ids;
    break;
  case ASM2_HANDLE_FIELD:
    ++asm2_stats.field_ids;
    break;
  case ASM2_HANDLE_ARRAY:
    ++asm2_stats.arrays;
    break;
  case ASM2_HANDLE_DIRECT_BUFFER:
    ++asm2_stats.direct_buffers;
    break;
  case ASM2_HANDLE_THROWABLE:
    ++asm2_stats.throwables;
    break;
  }
  return item;
}

static asm2_handle *asm2_intern_class(const char *class_name) {
  if (!class_name || !class_name[0])
    class_name = "java/lang/Object";
  unsigned int bucket = asm2_intern_bucket(
      ASM2_HANDLE_CLASS, class_name, NULL, NULL, NULL, 0);
  pthread_mutex_lock(&asm2_jni_lock);
  for (asm2_handle *item = asm2_intern_buckets[bucket]; item;
       item = item->intern_next) {
    if (item->kind == ASM2_HANDLE_CLASS &&
        asm2_text_equal(item->class_name, class_name)) {
      pthread_mutex_unlock(&asm2_jni_lock);
      return item;
    }
  }
  asm2_handle *item =
      asm2_allocate_handle_locked(ASM2_HANDLE_CLASS, class_name);
  if (item)
    asm2_publish_intern_locked(item, bucket);
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static asm2_handle *asm2_intern_object(const char *class_name) {
  if (!class_name || !class_name[0])
    class_name = "java/lang/Object";
  unsigned int bucket = asm2_intern_bucket(
      ASM2_HANDLE_OBJECT, class_name, NULL, NULL, NULL, 0);
  pthread_mutex_lock(&asm2_jni_lock);
  for (asm2_handle *item = asm2_intern_buckets[bucket]; item;
       item = item->intern_next) {
    if (item->kind == ASM2_HANDLE_OBJECT &&
        asm2_text_equal(item->class_name, class_name)) {
      pthread_mutex_unlock(&asm2_jni_lock);
      return item;
    }
  }
  asm2_handle *item =
      asm2_allocate_handle_locked(ASM2_HANDLE_OBJECT, class_name);
  if (item)
    asm2_publish_intern_locked(item, bucket);
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static asm2_handle *asm2_new_object_handle(const char *class_name) {
  if (!class_name || !class_name[0])
    class_name = "java/lang/Object";
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *item =
      asm2_allocate_handle_locked(ASM2_HANDLE_OBJECT, class_name);
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static int asm2_is_bundle(const asm2_handle *item) {
  return item && item->kind == ASM2_HANDLE_OBJECT &&
         asm2_text_equal(item->class_name, "android/os/Bundle");
}

static asm2_bundle_entry *asm2_bundle_find_locked(asm2_handle *bundle,
                                                   const char *key) {
  if (!asm2_is_bundle(bundle) || !key)
    return NULL;
  for (asm2_bundle_entry *entry = bundle->bundle_entries; entry;
       entry = entry->next) {
    if (asm2_text_equal(entry->key, key))
      return entry;
  }
  return NULL;
}

static int asm2_bundle_set(void *bundle_pointer, const char *key,
                           asm2_bundle_value_kind kind,
                           asm2_jni_value value) {
  if (!key || !key[0])
    return 0;
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *bundle = asm2_find_handle_locked(bundle_pointer);
  if (!asm2_is_bundle(bundle)) {
    pthread_mutex_unlock(&asm2_jni_lock);
    return 0;
  }
  asm2_bundle_entry *entry = asm2_bundle_find_locked(bundle, key);
  if (!entry) {
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
      pthread_mutex_unlock(&asm2_jni_lock);
      return 0;
    }
    entry->key = asm2_duplicate(key);
    if (!entry->key) {
      free(entry);
      pthread_mutex_unlock(&asm2_jni_lock);
      return 0;
    }
    entry->next = bundle->bundle_entries;
    bundle->bundle_entries = entry;
    ++asm2_stats.bundle_entries;
  }
  entry->kind = kind;
  entry->value = value;
  pthread_mutex_unlock(&asm2_jni_lock);
  return 1;
}

static int asm2_bundle_get(void *bundle_pointer, const char *key,
                           asm2_bundle_value_kind *kind,
                           asm2_jni_value *value) {
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *bundle = asm2_find_handle_locked(bundle_pointer);
  asm2_bundle_entry *entry = asm2_bundle_find_locked(bundle, key);
  if (!entry) {
    pthread_mutex_unlock(&asm2_jni_lock);
    return 0;
  }
  if (kind)
    *kind = entry->kind;
  if (value)
    *value = entry->value;
  pthread_mutex_unlock(&asm2_jni_lock);
  return 1;
}

static void asm2_bundle_clear(void *bundle_pointer) {
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *bundle = asm2_find_handle_locked(bundle_pointer);
  if (!asm2_is_bundle(bundle)) {
    pthread_mutex_unlock(&asm2_jni_lock);
    return;
  }
  asm2_bundle_entry *entry = bundle->bundle_entries;
  bundle->bundle_entries = NULL;
  while (entry) {
    asm2_bundle_entry *next = entry->next;
    free(entry->key);
    free(entry);
    if (asm2_stats.bundle_entries != 0)
      --asm2_stats.bundle_entries;
    entry = next;
  }
  pthread_mutex_unlock(&asm2_jni_lock);
}

static asm2_handle *asm2_intern_id(asm2_handle_kind kind, void *class_handle,
                                   const char *name, const char *signature,
                                   int is_static) {
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  const char *class_name =
      class_item && class_item->class_name ? class_item->class_name
                                          : "java/lang/Object";
  unsigned int bucket = asm2_intern_bucket(
      kind, class_name, name, signature, NULL, is_static);
  pthread_mutex_lock(&asm2_jni_lock);
  for (asm2_handle *item = asm2_intern_buckets[bucket]; item;
       item = item->intern_next) {
    if (item->kind == kind && item->is_static == !!is_static &&
        asm2_text_equal(item->class_name, class_name) &&
        asm2_text_equal(item->name, name) &&
        asm2_text_equal(item->signature, signature)) {
      pthread_mutex_unlock(&asm2_jni_lock);
      return item;
    }
  }
  asm2_handle *item = asm2_allocate_handle_locked(kind, class_name);
  if (item) {
    item->name = asm2_duplicate(name ? name : "");
    item->signature = asm2_duplicate(signature ? signature : "");
    item->is_static = !!is_static;
    asm2_publish_intern_locked(item, bucket);
  }
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static size_t asm2_utf8_codepoint(const unsigned char **input) {
  const unsigned char *text = *input;
  size_t codepoint;
  if (text[0] < 0x80) {
    codepoint = text[0];
    *input = text + 1;
  } else if ((text[0] & 0xe0) == 0xc0 && (text[1] & 0xc0) == 0x80) {
    codepoint = ((size_t)(text[0] & 0x1f) << 6) | (text[1] & 0x3f);
    *input = text + 2;
  } else if ((text[0] & 0xf0) == 0xe0 && (text[1] & 0xc0) == 0x80 &&
             (text[2] & 0xc0) == 0x80) {
    codepoint = ((size_t)(text[0] & 0x0f) << 12) |
                ((size_t)(text[1] & 0x3f) << 6) | (text[2] & 0x3f);
    *input = text + 3;
  } else if ((text[0] & 0xf8) == 0xf0 && (text[1] & 0xc0) == 0x80 &&
             (text[2] & 0xc0) == 0x80 && (text[3] & 0xc0) == 0x80) {
    codepoint = ((size_t)(text[0] & 0x07) << 18) |
                ((size_t)(text[1] & 0x3f) << 12) |
                ((size_t)(text[2] & 0x3f) << 6) | (text[3] & 0x3f);
    *input = text + 4;
  } else {
    codepoint = 0xfffd;
    *input = text + 1;
  }
  return codepoint;
}

static void asm2_fill_utf16(asm2_handle *string) {
  if (!string || string->utf16)
    return;
  const unsigned char *cursor =
      (const unsigned char *)(string->text ? string->text : "");
  size_t maximum = strlen((const char *)cursor) * 2 + 1;
  asm2_jchar *output = calloc(maximum, sizeof(*output));
  if (!output)
    return;
  size_t count = 0;
  while (*cursor) {
    size_t codepoint = asm2_utf8_codepoint(&cursor);
    if (codepoint <= 0xffff) {
      output[count++] = (asm2_jchar)codepoint;
    } else {
      codepoint -= 0x10000;
      output[count++] = (asm2_jchar)(0xd800 | (codepoint >> 10));
      output[count++] = (asm2_jchar)(0xdc00 | (codepoint & 0x3ff));
    }
  }
  string->utf16 = output;
  string->utf16_length = count;
}

static asm2_handle *asm2_intern_string(const char *text) {
  if (!text)
    text = "";
  unsigned int bucket = asm2_intern_bucket(
      ASM2_HANDLE_STRING, NULL, NULL, NULL, text, 0);
  pthread_mutex_lock(&asm2_jni_lock);
  for (asm2_handle *item = asm2_intern_buckets[bucket]; item;
       item = item->intern_next) {
    if (item->kind == ASM2_HANDLE_STRING && asm2_text_equal(item->text, text)) {
      pthread_mutex_unlock(&asm2_jni_lock);
      return item;
    }
  }
  asm2_handle *item =
      asm2_allocate_handle_locked(ASM2_HANDLE_STRING, "java/lang/String");
  if (item) {
    item->text = asm2_duplicate(text);
    asm2_fill_utf16(item);
    asm2_publish_intern_locked(item, bucket);
  }
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static asm2_handle *asm2_new_array(const char *class_name, size_t length,
                                    size_t element_size) {
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *item =
      asm2_allocate_handle_locked(ASM2_HANDLE_ARRAY, class_name);
  if (item) {
    item->length = length;
    item->element_size = element_size;
    if (length && element_size <= SIZE_MAX / length) {
      item->data = calloc(length, element_size);
      if (item->data)
        asm2_stats.array_bytes += length * element_size;
    }
  }
  pthread_mutex_unlock(&asm2_jni_lock);
  if (item && length && !item->data)
    asm2_pending_exception = asm2_intern_object("java/lang/OutOfMemoryError");
  return item;
}

static asm2_jni_value asm2_zero_value(void) {
  asm2_jni_value result;
  memset(&result, 0, sizeof(result));
  return result;
}

void asm2_jni_get_stats(struct asm2_jni_stats *stats) {
  if (!stats)
    return;
  pthread_mutex_lock(&asm2_jni_lock);
  *stats = asm2_stats;
  pthread_mutex_unlock(&asm2_jni_lock);
}

void *asm2_jni_class(const char *slash_name) {
  return asm2_intern_class(slash_name);
}

void *asm2_jni_object(const char *slash_name) {
  return asm2_intern_object(slash_name);
}

void *asm2_jni_string(const char *utf8) { return asm2_intern_string(utf8); }

void *asm2_jni_bundle(void) {
  return asm2_new_object_handle("android/os/Bundle");
}

int asm2_jni_bundle_put_boolean(void *bundle, const char *key,
                                asm2_jboolean value) {
  asm2_jni_value stored = asm2_zero_value();
  stored.z = value ? 1 : 0;
  return asm2_bundle_set(bundle, key, ASM2_BUNDLE_BOOLEAN, stored);
}

int asm2_jni_bundle_get_boolean(void *bundle, const char *key,
                                asm2_jboolean *value) {
  asm2_jni_value stored = asm2_zero_value();
  asm2_bundle_value_kind kind;
  if (!value || !asm2_bundle_get(bundle, key, &kind, &stored) ||
      kind != ASM2_BUNDLE_BOOLEAN)
    return 0;
  *value = stored.z ? 1 : 0;
  return 1;
}

int asm2_jni_bundle_put_int(void *bundle, const char *key, asm2_jint value) {
  asm2_jni_value stored = asm2_zero_value();
  stored.i = value;
  return asm2_bundle_set(bundle, key, ASM2_BUNDLE_INT, stored);
}

int asm2_jni_bundle_get_int(void *bundle, const char *key,
                            asm2_jint *value) {
  asm2_jni_value stored = asm2_zero_value();
  asm2_bundle_value_kind kind;
  if (!value || !asm2_bundle_get(bundle, key, &kind, &stored) ||
      kind != ASM2_BUNDLE_INT)
    return 0;
  *value = stored.i;
  return 1;
}

int asm2_jni_bundle_put_long(void *bundle, const char *key,
                             asm2_jlong value) {
  asm2_jni_value stored = asm2_zero_value();
  stored.j = value;
  return asm2_bundle_set(bundle, key, ASM2_BUNDLE_LONG, stored);
}

int asm2_jni_bundle_get_long(void *bundle, const char *key,
                             asm2_jlong *value) {
  asm2_jni_value stored = asm2_zero_value();
  asm2_bundle_value_kind kind;
  if (!value || !asm2_bundle_get(bundle, key, &kind, &stored) ||
      kind != ASM2_BUNDLE_LONG)
    return 0;
  *value = stored.j;
  return 1;
}

int asm2_jni_bundle_put_string(void *bundle, const char *key,
                               const char *value) {
  asm2_jni_value stored = asm2_zero_value();
  stored.l = value ? asm2_jni_string(value) : NULL;
  return asm2_bundle_set(bundle, key, ASM2_BUNDLE_OBJECT, stored);
}

int asm2_jni_bundle_put_byte_array(void *bundle, const char *key,
                                   const void *bytes, size_t count) {
  asm2_handle *array = asm2_new_array("[B", count, sizeof(asm2_jbyte));
  if (!array || (count && !array->data))
    return 0;
  if (bytes && count)
    memcpy(array->data, bytes, count);
  asm2_jni_value stored = asm2_zero_value();
  stored.l = array;
  return asm2_bundle_set(bundle, key, ASM2_BUNDLE_OBJECT, stored);
}

const char *asm2_jni_bundle_get_string(void *bundle, const char *key) {
  asm2_jni_value stored = asm2_zero_value();
  asm2_bundle_value_kind kind;
  if (!asm2_bundle_get(bundle, key, &kind, &stored) ||
      kind != ASM2_BUNDLE_OBJECT)
    return NULL;
  return asm2_jni_string_chars(stored.l);
}

void *asm2_jni_int_array(const asm2_jint *values, size_t count) {
  asm2_handle *array = asm2_new_array("[I", count, sizeof(asm2_jint));
  if (array && array->data && values && count)
    memcpy(array->data, values, count * sizeof(asm2_jint));
  return array;
}

const char *asm2_jni_string_chars(void *string) {
  asm2_handle *item = asm2_lookup_handle(string);
  return item && item->kind == ASM2_HANDLE_STRING && item->text ? item->text
                                                                : NULL;
}

void *asm2_jni_vm(void) {
  if (!asm2_initialized)
    asm2_jni_init(NULL);
  return &asm2_vm_functions;
}

void *asm2_jni_env(void) {
  if (!asm2_initialized)
    asm2_jni_init(NULL);
  return &asm2_env_functions;
}

void *asm2_jni_activity(void) {
  return asm2_intern_object("com/gameloft/glf/GL2JNIActivity");
}

static const char *asm2_receiver_class_name(void *receiver) {
  asm2_handle *item = asm2_lookup_handle(receiver);
  return item && item->class_name ? item->class_name : "java/lang/Object";
}

static void *asm2_return_object_for_signature(const char *signature) {
  const char *return_type = signature ? strrchr(signature, ')') : NULL;
  if (!return_type || !return_type[1])
    return NULL;
  ++return_type;
  if (*return_type == 'L') {
    const char *end = strchr(return_type, ';');
    if (!end || end == return_type + 1)
      return NULL;
    size_t length = (size_t)(end - (return_type + 1));
    char *class_name = malloc(length + 1);
    if (!class_name)
      return NULL;
    memcpy(class_name, return_type + 1, length);
    class_name[length] = '\0';
    void *result = asm2_text_equal(class_name, "java/lang/String")
                       ? asm2_jni_string("")
                       : asm2_jni_object(class_name);
    free(class_name);
    return result;
  }
  if (*return_type == '[') {
    size_t element_size = sizeof(void *);
    switch (return_type[1]) {
    case 'Z':
      element_size = sizeof(asm2_jboolean);
      break;
    case 'B':
      element_size = sizeof(asm2_jbyte);
      break;
    case 'C':
      element_size = sizeof(asm2_jchar);
      break;
    case 'S':
      element_size = sizeof(asm2_jshort);
      break;
    case 'I':
      element_size = sizeof(asm2_jint);
      break;
    case 'J':
      element_size = sizeof(asm2_jlong);
      break;
    case 'F':
      element_size = sizeof(asm2_jfloat);
      break;
    case 'D':
      element_size = sizeof(asm2_jdouble);
      break;
    default:
      break;
    }
    return asm2_new_array(return_type, 0, element_size);
  }
  return NULL;
}

static void *asm2_first_object_argument(const char *signature, va_list *ap,
                                        const asm2_jni_value *args) {
  if (!signature || signature[0] != '(')
    return NULL;
  const char *type = signature + 1;
  if (*type != 'L' && *type != '[')
    return NULL;
  if (args)
    return args[0].l;
  if (ap) {
    va_list copy;
    va_copy(copy, *ap);
    void *result = va_arg(copy, void *);
    va_end(copy);
    return result;
  }
  return NULL;
}

static asm2_jni_value asm2_default_method(asm2_handle *method, void *receiver,
                                           va_list *ap,
                                           const asm2_jni_value *args) {
  asm2_jni_value result = asm2_zero_value();
  const char *class_name = method->class_name ? method->class_name : "";
  const char *name = method->name ? method->name : "";

  if (asm2_text_equal(class_name, "android/os/Bundle")) {
    if (asm2_text_equal(name, "<init>"))
      return result;
    if (asm2_text_equal(name, "clear")) {
      asm2_bundle_clear(receiver);
      return result;
    }

    asm2_jni_value first = asm2_zero_value();
    if (args) {
      first = args[0];
    } else if (ap) {
      va_list copy;
      va_copy(copy, *ap);
      first.l = va_arg(copy, void *);
      va_end(copy);
    }
    const char *key = asm2_jni_string_chars(first.l);
    asm2_bundle_value_kind kind;
    asm2_jni_value stored = asm2_zero_value();
    int found = asm2_bundle_get(receiver, key, &kind, &stored);

    if (asm2_text_equal(name, "putInt") ||
        asm2_text_equal(name, "putBoolean")) {
      asm2_jni_value input = asm2_zero_value();
      if (args) {
        input.i = args[1].i;
      } else if (ap) {
        va_list copy;
        va_copy(copy, *ap);
        (void)va_arg(copy, void *);
        input.i = va_arg(copy, asm2_jint);
        va_end(copy);
      }
      asm2_bundle_set(receiver, key,
                      asm2_text_equal(name, "putInt")
                          ? ASM2_BUNDLE_INT
                          : ASM2_BUNDLE_BOOLEAN,
                      input);
    } else if (asm2_text_equal(name, "putLong")) {
      asm2_jni_value input = asm2_zero_value();
      if (args) {
        input.j = args[1].j;
      } else if (ap) {
        va_list copy;
        va_copy(copy, *ap);
        (void)va_arg(copy, void *);
        input.j = va_arg(copy, asm2_jlong);
        va_end(copy);
      }
      asm2_bundle_set(receiver, key, ASM2_BUNDLE_LONG, input);
    } else if (asm2_text_equal(name, "putString") ||
               asm2_text_equal(name, "putByteArray")) {
      asm2_jni_value input = asm2_zero_value();
      if (args) {
        input.l = args[1].l;
      } else if (ap) {
        va_list copy;
        va_copy(copy, *ap);
        (void)va_arg(copy, void *);
        input.l = va_arg(copy, void *);
        va_end(copy);
      }
      asm2_bundle_set(receiver, key, ASM2_BUNDLE_OBJECT, input);
    } else if (asm2_text_equal(name, "getInt")) {
      result.i = found && kind == ASM2_BUNDLE_INT ? stored.i : 0;
    } else if (asm2_text_equal(name, "getBoolean")) {
      result.z = found && kind == ASM2_BUNDLE_BOOLEAN ? stored.z : 0;
    } else if (asm2_text_equal(name, "getLong")) {
      result.j = found && kind == ASM2_BUNDLE_LONG ? stored.j : 0;
    } else if (asm2_text_equal(name, "getString") ||
               asm2_text_equal(name, "getByteArray")) {
      result.l = found && kind == ASM2_BUNDLE_OBJECT ? stored.l : NULL;
    } else if (asm2_text_equal(name, "containsKey")) {
      result.z = found;
    }
    return result;
  }

  if (asm2_text_equal(name, "getPackageName")) {
    result.l = asm2_jni_string(asm2_config.package_name);
  } else if (asm2_text_equal(name, "getFilesDir")) {
    result.l = asm2_jni_string(asm2_config.files_directory);
  } else if (asm2_text_equal(name, "getCacheDir")) {
    result.l = asm2_jni_string(asm2_config.cache_directory);
  } else if (asm2_text_equal(name, "getExternalStorageDirectory") ||
             asm2_text_equal(name, "getExternalFilesDir")) {
    result.l = asm2_jni_string(asm2_config.external_storage);
  } else if (asm2_text_equal(name, "getExternalStorageState")) {
    result.l = asm2_jni_string("mounted");
  } else if (asm2_text_equal(name, "getAbsolutePath") ||
             asm2_text_equal(name, "getCanonicalPath") ||
             asm2_text_equal(name, "getPath") ||
             asm2_text_equal(name, "toString")) {
    const char *text = asm2_jni_string_chars(receiver);
    result.l = text ? receiver : asm2_jni_string(class_name);
  } else if (asm2_text_equal(name, "getApplicationContext") ||
             asm2_text_equal(name, "getApplication") ||
             asm2_text_equal(name, "getBaseContext") ||
             asm2_text_equal(name, "getActivity")) {
    result.l = asm2_jni_activity();
  } else if (asm2_text_equal(name, "getClass")) {
    result.l = asm2_jni_class(asm2_receiver_class_name(receiver));
  } else if (asm2_text_equal(class_name, "java/lang/Class") &&
             asm2_text_equal(name, "getName")) {
    asm2_handle *class_item = asm2_lookup_handle(receiver);
    const char *slash_name = class_item && class_item->class_name
                                 ? class_item->class_name
                                 : "java/lang/Object";
    char *dot_name = asm2_duplicate(slash_name);
    if (dot_name) {
      for (char *cursor = dot_name; *cursor; ++cursor) {
        if (*cursor == '/')
          *cursor = '.';
      }
      result.l = asm2_jni_string(dot_name);
      free(dot_name);
    }
  } else if (asm2_text_equal(class_name, "java/lang/String") &&
             (asm2_text_equal(name, "length") ||
              asm2_text_equal(name, "hashCode"))) {
    const char *text = asm2_jni_string_chars(receiver);
    if (asm2_text_equal(name, "length")) {
      result.i = text ? (asm2_jint)strlen(text) : 0;
    } else {
      uint32_t hash = 0;
      for (const unsigned char *cursor = (const unsigned char *)(text ? text : "");
           *cursor; ++cursor)
        hash = hash * 31u + *cursor;
      result.i = (asm2_jint)hash;
    }
  } else if (asm2_text_equal(class_name, "java/lang/String") &&
             asm2_text_equal(name, "isEmpty")) {
    const char *text = asm2_jni_string_chars(receiver);
    result.z = !text || !text[0];
  } else if (asm2_text_equal(name, "equals")) {
    void *other = asm2_first_object_argument(method->signature, ap, args);
    const char *left_text = asm2_jni_string_chars(receiver);
    const char *right_text = asm2_jni_string_chars(other);
    result.z = left_text && right_text ? strcmp(left_text, right_text) == 0
                                      : receiver == other;
  } else if (asm2_text_equal(class_name, "java/lang/System") &&
             asm2_text_equal(name, "getProperty")) {
    const char *key = asm2_jni_string_chars(
        asm2_first_object_argument(method->signature, ap, args));
    if (asm2_text_equal(key, "os.name"))
      result.l = asm2_jni_string("Linux");
    else if (asm2_text_equal(key, "os.version"))
      result.l = asm2_jni_string(asm2_config.android_release);
    else if (asm2_text_equal(key, "user.language"))
      result.l = asm2_jni_string(asm2_config.locale_language);
    else
      result.l = asm2_jni_string("");
  } else if (asm2_text_equal(class_name, "java/util/Locale") &&
             asm2_text_equal(name, "getDefault")) {
    result.l = asm2_jni_object("java/util/Locale");
  } else if (asm2_text_equal(class_name, "java/util/Locale") &&
             asm2_text_equal(name, "getLanguage")) {
    result.l = asm2_jni_string(asm2_config.locale_language);
  } else if (asm2_text_equal(class_name, "java/util/Locale") &&
             asm2_text_equal(name, "getCountry")) {
    result.l = asm2_jni_string(asm2_config.locale_country);
  } else if (asm2_text_equal(name, "getWidth") ||
             asm2_text_equal(name, "getMeasuredWidth")) {
    result.i = asm2_config.screen_width;
  } else if (asm2_text_equal(name, "getHeight") ||
             asm2_text_equal(name, "getMeasuredHeight")) {
    result.i = asm2_config.screen_height;
  } else if (asm2_text_equal(name, "getResources")) {
    result.l = asm2_jni_object("android/content/res/Resources");
  } else if (asm2_text_equal(name, "getDisplayMetrics")) {
    result.l = asm2_jni_object("android/util/DisplayMetrics");
  } else if (asm2_text_equal(name, "getWindowManager")) {
    result.l = asm2_jni_object("android/view/WindowManager");
  } else if (asm2_text_equal(name, "getDefaultDisplay")) {
    result.l = asm2_jni_object("android/view/Display");
  } else if (asm2_text_equal(name, "getPackageManager")) {
    result.l = asm2_jni_object("android/content/pm/PackageManager");
  } else if (asm2_text_equal(name, "getApplicationInfo")) {
    result.l = asm2_jni_object("android/content/pm/ApplicationInfo");
  } else if (asm2_text_equal(name, "getAssets")) {
    result.l = asm2_jni_object("android/content/res/AssetManager");
  } else if (asm2_text_equal(name, "checkCallingOrSelfPermission") ||
             asm2_text_equal(name, "checkSelfPermission")) {
    result.i = 0; /* PackageManager.PERMISSION_GRANTED */
  } else if (asm2_text_equal(name, "booleanValue")) {
    asm2_handle *item = asm2_lookup_handle(receiver);
    result.z = item ? item->value.z : 0;
  } else if (asm2_text_equal(name, "intValue")) {
    asm2_handle *item = asm2_lookup_handle(receiver);
    result.i = item ? item->value.i : 0;
  } else if (asm2_text_equal(name, "floatValue")) {
    asm2_handle *item = asm2_lookup_handle(receiver);
    result.f = item ? item->value.f : 0.0f;
  } else if (asm2_text_equal(name, "doubleValue")) {
    asm2_handle *item = asm2_lookup_handle(receiver);
    result.d = item ? item->value.d : 0.0;
  } else if (method->signature) {
    result.l = asm2_return_object_for_signature(method->signature);
  }
  return result;
}

static asm2_jni_value asm2_dispatch_method(void *receiver, void *method_id,
                                            va_list *ap,
                                            const asm2_jni_value *args) {
  asm2_handle *method = asm2_lookup_handle(method_id);
  if (!method || method->kind != ASM2_HANDLE_METHOD)
    return asm2_zero_value();

  asm2_jni_value result = asm2_zero_value();
  int handled = 0;
  if (asm2_config.method_callback) {
    if (ap) {
      va_list copy;
      va_copy(copy, *ap);
      handled = asm2_config.method_callback(
          asm2_config.user, method->class_name, method->name,
          method->signature, method->is_static, receiver, &copy, NULL, &result);
      va_end(copy);
    } else {
      handled = asm2_config.method_callback(
          asm2_config.user, method->class_name, method->name,
          method->signature, method->is_static, receiver, NULL, args, &result);
    }
  }
  if (handled && getenv("ASM2_JNI_TRACE"))
    debugPrintf("ASM2_JNI_CALL %s.%s%s\n",
                method->class_name ? method->class_name : "?",
                method->name ? method->name : "?",
                method->signature ? method->signature : "");
  if (!handled && getenv("ASM2_JNI_DEBUG"))
    debugPrintf("ASM2_JNI_UNHANDLED %s.%s%s static=%d\n",
                method->class_name ? method->class_name : "?",
                method->name ? method->name : "?",
                method->signature ? method->signature : "",
                method->is_static);
  return handled ? result : asm2_default_method(method, receiver, ap, args);
}

static asm2_jni_value asm2_default_field(asm2_handle *field) {
  asm2_jni_value result = field->value;
  if (field->has_value)
    return result;
  const char *class_name = field->class_name ? field->class_name : "";
  const char *name = field->name ? field->name : "";

  if (asm2_text_equal(class_name, "android/os/Build$VERSION") &&
      asm2_text_equal(name, "SDK_INT")) {
    result.i = asm2_config.sdk_int;
  } else if (asm2_text_equal(class_name, "android/os/Build$VERSION") &&
             asm2_text_equal(name, "RELEASE")) {
    result.l = asm2_jni_string(asm2_config.android_release);
  } else if (asm2_text_equal(class_name, "android/os/Build") &&
             asm2_text_equal(name, "MODEL")) {
    result.l = asm2_jni_string(asm2_config.device_model);
  } else if (asm2_text_equal(class_name, "android/os/Build") &&
             asm2_text_equal(name, "MANUFACTURER")) {
    result.l = asm2_jni_string(asm2_config.device_manufacturer);
  } else if (asm2_text_equal(class_name, "android/os/Build") &&
             (asm2_text_equal(name, "BRAND") ||
              asm2_text_equal(name, "DEVICE") ||
              asm2_text_equal(name, "PRODUCT"))) {
    result.l = asm2_jni_string("NextOS");
  } else if (asm2_text_equal(class_name, "android/util/DisplayMetrics") &&
             (asm2_text_equal(name, "widthPixels") ||
              asm2_text_equal(name, "heightPixels"))) {
    result.i = asm2_text_equal(name, "widthPixels") ? asm2_config.screen_width
                                                     : asm2_config.screen_height;
  } else if (asm2_text_equal(class_name, "android/util/DisplayMetrics") &&
             (asm2_text_equal(name, "density") ||
              asm2_text_equal(name, "scaledDensity"))) {
    result.f = asm2_config.screen_density;
  } else if (field->signature && field->signature[0] == 'L' && !result.l) {
    char fake_signature[512];
    if (snprintf(fake_signature, sizeof(fake_signature), "()%s",
                 field->signature) > 0)
      result.l = asm2_return_object_for_signature(fake_signature);
  }
  return result;
}

static asm2_jni_value asm2_get_field(void *receiver, void *field_id) {
  asm2_handle *field = asm2_lookup_handle(field_id);
  if (!field || field->kind != ASM2_HANDLE_FIELD)
    return asm2_zero_value();
  asm2_jni_value result = asm2_zero_value();
  if (asm2_config.field_get_callback &&
      asm2_config.field_get_callback(
          asm2_config.user, field->class_name, field->name, field->signature,
          field->is_static, receiver, &result))
    return result;
  return asm2_default_field(field);
}

static void asm2_set_field(void *receiver, void *field_id,
                           asm2_jni_value value) {
  asm2_handle *field = asm2_lookup_handle(field_id);
  if (!field || field->kind != ASM2_HANDLE_FIELD)
    return;
  field->value = value;
  field->has_value = 1;
  if (asm2_config.field_set_callback)
    asm2_config.field_set_callback(
        asm2_config.user, field->class_name, field->name, field->signature,
        field->is_static, receiver, value);
}

static asm2_jint ASM2_GUEST_PCS asm2_vm_destroy(void *vm) {
  (void)vm;
  return ASM2_JNI_OK;
}

static asm2_jint ASM2_GUEST_PCS asm2_vm_attach(void *vm, void **environment,
                                                void *arguments) {
  (void)vm;
  (void)arguments;
  if (!environment)
    return ASM2_JNI_ERR;
  *environment = asm2_jni_env();
  return ASM2_JNI_OK;
}

static asm2_jint ASM2_GUEST_PCS asm2_vm_detach(void *vm) {
  (void)vm;
  asm2_pending_exception = NULL;
  return ASM2_JNI_OK;
}

static int asm2_valid_jni_version(asm2_jint version) {
  return version == ASM2_JNI_VERSION_1_1 ||
         version == ASM2_JNI_VERSION_1_2 ||
         version == ASM2_JNI_VERSION_1_4 ||
         version == ASM2_JNI_VERSION_1_6;
}

static asm2_jint ASM2_GUEST_PCS asm2_vm_get_env(void *vm, void **environment,
                                                 asm2_jint version) {
  (void)vm;
  if (!environment)
    return ASM2_JNI_ERR;
  if (!asm2_valid_jni_version(version)) {
    *environment = NULL;
    return ASM2_JNI_EVERSION;
  }
  *environment = asm2_jni_env();
  return ASM2_JNI_OK;
}

static asm2_jint ASM2_GUEST_PCS asm2_get_version(void *environment) {
  (void)environment;
  return ASM2_JNI_VERSION_1_6;
}

static void *ASM2_GUEST_PCS asm2_define_class(void *environment,
                                              const char *name, void *loader,
                                              const asm2_jbyte *data,
                                              asm2_jint length) {
  (void)environment;
  (void)loader;
  (void)data;
  (void)length;
  return asm2_jni_class(name);
}

static void *ASM2_GUEST_PCS asm2_find_class(void *environment,
                                            const char *name) {
  (void)environment;
  return asm2_jni_class(name);
}

static void *ASM2_GUEST_PCS asm2_from_reflected(void *environment,
                                                void *reflection) {
  (void)environment;
  return reflection;
}

static void *ASM2_GUEST_PCS asm2_to_reflected_method(
    void *environment, void *class_handle, void *method_id,
    asm2_jboolean is_static) {
  (void)environment;
  (void)class_handle;
  (void)is_static;
  return method_id;
}

static void *ASM2_GUEST_PCS asm2_get_superclass(void *environment,
                                                void *class_handle) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(class_handle);
  if (item && asm2_text_equal(item->class_name, "java/lang/Object"))
    return NULL;
  return asm2_jni_class("java/lang/Object");
}

static asm2_jboolean ASM2_GUEST_PCS asm2_is_assignable(void *environment,
                                                        void *subclass,
                                                        void *superclass) {
  (void)environment;
  asm2_handle *sub = asm2_lookup_handle(subclass);
  asm2_handle *super = asm2_lookup_handle(superclass);
  return sub && super &&
         (asm2_text_equal(sub->class_name, super->class_name) ||
          asm2_text_equal(super->class_name, "java/lang/Object"));
}

static void *ASM2_GUEST_PCS asm2_to_reflected_field(
    void *environment, void *class_handle, void *field_id,
    asm2_jboolean is_static) {
  (void)environment;
  (void)class_handle;
  (void)is_static;
  return field_id;
}

static asm2_jint ASM2_GUEST_PCS asm2_throw(void *environment,
                                           void *throwable) {
  (void)environment;
  asm2_pending_exception = throwable ? throwable
                                     : asm2_jni_object("java/lang/Throwable");
  return ASM2_JNI_OK;
}

static asm2_jint ASM2_GUEST_PCS asm2_throw_new(void *environment,
                                               void *class_handle,
                                               const char *message) {
  (void)environment;
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  const char *class_name = class_item && class_item->class_name
                               ? class_item->class_name
                               : "java/lang/RuntimeException";
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *throwable =
      asm2_allocate_handle_locked(ASM2_HANDLE_THROWABLE, class_name);
  if (throwable)
    throwable->text = asm2_duplicate(message ? message : "");
  pthread_mutex_unlock(&asm2_jni_lock);
  asm2_pending_exception = throwable;
  return throwable ? ASM2_JNI_OK : ASM2_JNI_ERR;
}

static void *ASM2_GUEST_PCS asm2_exception_occurred(void *environment) {
  (void)environment;
  return asm2_pending_exception;
}

static void ASM2_GUEST_PCS asm2_exception_describe(void *environment) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(asm2_pending_exception);
  if (item)
    fprintf(stderr, "ASM2_JNI exception %s: %s\n", item->class_name,
            item->text ? item->text : "");
}

static void ASM2_GUEST_PCS asm2_exception_clear(void *environment) {
  (void)environment;
  asm2_pending_exception = NULL;
}

static void ASM2_GUEST_PCS asm2_fatal_error(void *environment,
                                            const char *message) {
  (void)environment;
  fprintf(stderr, "ASM2_JNI FatalError: %s\n", message ? message : "");
  asm2_throw_new(environment, asm2_jni_class("java/lang/InternalError"),
                 message);
}

static asm2_jint ASM2_GUEST_PCS asm2_push_local_frame(void *environment,
                                                       asm2_jint capacity) {
  (void)environment;
  (void)capacity;
  return ASM2_JNI_OK;
}

static void *ASM2_GUEST_PCS asm2_pop_local_frame(void *environment,
                                                 void *result) {
  (void)environment;
  return result;
}

static void *ASM2_GUEST_PCS asm2_identity_ref(void *environment, void *object) {
  (void)environment;
  return object;
}

static void ASM2_GUEST_PCS asm2_delete_ref(void *environment, void *object) {
  (void)environment;
  (void)object;
}

static asm2_jboolean ASM2_GUEST_PCS asm2_is_same_object(void *environment,
                                                        void *left,
                                                        void *right) {
  (void)environment;
  return left == right;
}

static asm2_jint ASM2_GUEST_PCS asm2_ensure_local_capacity(
    void *environment, asm2_jint capacity) {
  (void)environment;
  return capacity < 0 ? ASM2_JNI_ERR : ASM2_JNI_OK;
}

static void *ASM2_GUEST_PCS asm2_alloc_object(void *environment,
                                              void *class_handle) {
  (void)environment;
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  if (class_item &&
      asm2_text_equal(class_item->class_name, "android/os/Bundle"))
    return asm2_jni_bundle();
  return asm2_jni_object(class_item ? class_item->class_name
                                    : "java/lang/Object");
}

static void *ASM2_GUEST_PCS asm2_get_object_class(void *environment,
                                                  void *object) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(object);
  if (!item)
    return asm2_jni_class("java/lang/Object");
  if (item->kind == ASM2_HANDLE_CLASS)
    return asm2_jni_class("java/lang/Class");
  return asm2_jni_class(item->class_name);
}

static asm2_jboolean ASM2_GUEST_PCS asm2_is_instance_of(void *environment,
                                                        void *object,
                                                        void *class_handle) {
  (void)environment;
  if (!object)
    return 1;
  asm2_handle *item = asm2_lookup_handle(object);
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  return item && class_item &&
         (asm2_text_equal(item->class_name, class_item->class_name) ||
          asm2_text_equal(class_item->class_name, "java/lang/Object"));
}

static void *ASM2_GUEST_PCS asm2_get_method_id(void *environment,
                                               void *class_handle,
                                               const char *name,
                                               const char *signature) {
  (void)environment;
  return asm2_intern_id(ASM2_HANDLE_METHOD, class_handle, name, signature, 0);
}

static void *ASM2_GUEST_PCS asm2_get_static_method_id(
    void *environment, void *class_handle, const char *name,
    const char *signature) {
  (void)environment;
  return asm2_intern_id(ASM2_HANDLE_METHOD, class_handle, name, signature, 1);
}

static asm2_jni_value asm2_dispatch_method_v(void *receiver, void *method_id,
                                              va_list arguments) {
  va_list copy;
  va_copy(copy, arguments);
  asm2_jni_value result =
      asm2_dispatch_method(receiver, method_id, &copy, NULL);
  va_end(copy);
  return result;
}

static void *ASM2_GUEST_PCS asm2_new_object(void *environment,
                                            void *class_handle,
                                            void *method_id, ...) {
  (void)environment;
  va_list arguments;
  va_start(arguments, method_id);
  asm2_jni_value value =
      asm2_dispatch_method(class_handle, method_id, &arguments, NULL);
  va_end(arguments);
  return value.l ? value.l : asm2_alloc_object(environment, class_handle);
}

static void *ASM2_GUEST_PCS asm2_new_object_v(void *environment,
                                              void *class_handle,
                                              void *method_id,
                                              va_list arguments) {
  (void)environment;
  asm2_jni_value value =
      asm2_dispatch_method_v(class_handle, method_id, arguments);
  return value.l ? value.l : asm2_alloc_object(environment, class_handle);
}

static void *ASM2_GUEST_PCS asm2_new_object_a(
    void *environment, void *class_handle, void *method_id,
    const asm2_jni_value *arguments) {
  (void)environment;
  asm2_jni_value value =
      asm2_dispatch_method(class_handle, method_id, NULL, arguments);
  return value.l ? value.l : asm2_alloc_object(environment, class_handle);
}

#define ASM2_DEFINE_INSTANCE_CALL(stem, return_type, member)                  \
  static return_type ASM2_GUEST_PCS asm2_call_##stem(                        \
      void *environment, void *receiver, void *method_id, ...) {             \
    (void)environment;                                                        \
    va_list arguments;                                                        \
    va_start(arguments, method_id);                                           \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method(receiver, method_id, &arguments, NULL);          \
    va_end(arguments);                                                        \
    return (return_type)result.member;                                        \
  }                                                                           \
  static return_type ASM2_GUEST_PCS asm2_call_##stem##_v(                    \
      void *environment, void *receiver, void *method_id,                    \
      va_list arguments) {                                                    \
    (void)environment;                                                        \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method_v(receiver, method_id, arguments);               \
    return (return_type)result.member;                                        \
  }                                                                           \
  static return_type ASM2_GUEST_PCS asm2_call_##stem##_a(                    \
      void *environment, void *receiver, void *method_id,                    \
      const asm2_jni_value *arguments) {                                      \
    (void)environment;                                                        \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method(receiver, method_id, NULL, arguments);           \
    return (return_type)result.member;                                        \
  }

ASM2_DEFINE_INSTANCE_CALL(object, void *, l)
ASM2_DEFINE_INSTANCE_CALL(boolean, asm2_jboolean, z)
ASM2_DEFINE_INSTANCE_CALL(byte, asm2_jbyte, b)
ASM2_DEFINE_INSTANCE_CALL(char, asm2_jchar, c)
ASM2_DEFINE_INSTANCE_CALL(short, asm2_jshort, s)
ASM2_DEFINE_INSTANCE_CALL(int, asm2_jint, i)
ASM2_DEFINE_INSTANCE_CALL(long, asm2_jlong, j)
ASM2_DEFINE_INSTANCE_CALL(float, asm2_jfloat, f)
ASM2_DEFINE_INSTANCE_CALL(double, asm2_jdouble, d)

static void ASM2_GUEST_PCS asm2_call_void(void *environment, void *receiver,
                                          void *method_id, ...) {
  (void)environment;
  va_list arguments;
  va_start(arguments, method_id);
  (void)asm2_dispatch_method(receiver, method_id, &arguments, NULL);
  va_end(arguments);
}

static void ASM2_GUEST_PCS asm2_call_void_v(void *environment, void *receiver,
                                            void *method_id,
                                            va_list arguments) {
  (void)environment;
  (void)asm2_dispatch_method_v(receiver, method_id, arguments);
}

static void ASM2_GUEST_PCS asm2_call_void_a(
    void *environment, void *receiver, void *method_id,
    const asm2_jni_value *arguments) {
  (void)environment;
  (void)asm2_dispatch_method(receiver, method_id, NULL, arguments);
}

#define ASM2_DEFINE_NONVIRTUAL_CALL(stem, return_type, member)                \
  static return_type ASM2_GUEST_PCS asm2_call_nonvirtual_##stem(             \
      void *environment, void *receiver, void *class_handle,                 \
      void *method_id, ...) {                                                 \
    (void)environment;                                                        \
    (void)class_handle;                                                       \
    va_list arguments;                                                        \
    va_start(arguments, method_id);                                           \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method(receiver, method_id, &arguments, NULL);          \
    va_end(arguments);                                                        \
    return (return_type)result.member;                                        \
  }                                                                           \
  static return_type ASM2_GUEST_PCS asm2_call_nonvirtual_##stem##_v(         \
      void *environment, void *receiver, void *class_handle,                 \
      void *method_id, va_list arguments) {                                   \
    (void)environment;                                                        \
    (void)class_handle;                                                       \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method_v(receiver, method_id, arguments);               \
    return (return_type)result.member;                                        \
  }                                                                           \
  static return_type ASM2_GUEST_PCS asm2_call_nonvirtual_##stem##_a(         \
      void *environment, void *receiver, void *class_handle,                 \
      void *method_id, const asm2_jni_value *arguments) {                     \
    (void)environment;                                                        \
    (void)class_handle;                                                       \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method(receiver, method_id, NULL, arguments);           \
    return (return_type)result.member;                                        \
  }

ASM2_DEFINE_NONVIRTUAL_CALL(object, void *, l)
ASM2_DEFINE_NONVIRTUAL_CALL(boolean, asm2_jboolean, z)
ASM2_DEFINE_NONVIRTUAL_CALL(byte, asm2_jbyte, b)
ASM2_DEFINE_NONVIRTUAL_CALL(char, asm2_jchar, c)
ASM2_DEFINE_NONVIRTUAL_CALL(short, asm2_jshort, s)
ASM2_DEFINE_NONVIRTUAL_CALL(int, asm2_jint, i)
ASM2_DEFINE_NONVIRTUAL_CALL(long, asm2_jlong, j)
ASM2_DEFINE_NONVIRTUAL_CALL(float, asm2_jfloat, f)
ASM2_DEFINE_NONVIRTUAL_CALL(double, asm2_jdouble, d)

static void ASM2_GUEST_PCS asm2_call_nonvirtual_void(
    void *environment, void *receiver, void *class_handle, void *method_id,
  ...) {
  (void)environment;
  (void)class_handle;
  va_list arguments;
  va_start(arguments, method_id);
  (void)asm2_dispatch_method(receiver, method_id, &arguments, NULL);
  va_end(arguments);
}

static void ASM2_GUEST_PCS asm2_call_nonvirtual_void_v(
    void *environment, void *receiver, void *class_handle, void *method_id,
    va_list arguments) {
  (void)environment;
  (void)class_handle;
  (void)asm2_dispatch_method_v(receiver, method_id, arguments);
}

static void ASM2_GUEST_PCS asm2_call_nonvirtual_void_a(
    void *environment, void *receiver, void *class_handle, void *method_id,
    const asm2_jni_value *arguments) {
  (void)environment;
  (void)class_handle;
  (void)asm2_dispatch_method(receiver, method_id, NULL, arguments);
}

#define ASM2_DEFINE_STATIC_CALL(stem, return_type, member)                    \
  static return_type ASM2_GUEST_PCS asm2_call_static_##stem(                 \
      void *environment, void *class_handle, void *method_id, ...) {         \
    (void)environment;                                                        \
    va_list arguments;                                                        \
    va_start(arguments, method_id);                                           \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method(class_handle, method_id, &arguments, NULL);      \
    va_end(arguments);                                                        \
    return (return_type)result.member;                                        \
  }                                                                           \
  static return_type ASM2_GUEST_PCS asm2_call_static_##stem##_v(             \
      void *environment, void *class_handle, void *method_id,                \
      va_list arguments) {                                                    \
    (void)environment;                                                        \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method_v(class_handle, method_id, arguments);           \
    return (return_type)result.member;                                        \
  }                                                                           \
  static return_type ASM2_GUEST_PCS asm2_call_static_##stem##_a(             \
      void *environment, void *class_handle, void *method_id,                \
      const asm2_jni_value *arguments) {                                      \
    (void)environment;                                                        \
    asm2_jni_value result =                                                   \
        asm2_dispatch_method(class_handle, method_id, NULL, arguments);       \
    return (return_type)result.member;                                        \
  }

ASM2_DEFINE_STATIC_CALL(object, void *, l)
ASM2_DEFINE_STATIC_CALL(boolean, asm2_jboolean, z)
ASM2_DEFINE_STATIC_CALL(byte, asm2_jbyte, b)
ASM2_DEFINE_STATIC_CALL(char, asm2_jchar, c)
ASM2_DEFINE_STATIC_CALL(short, asm2_jshort, s)
ASM2_DEFINE_STATIC_CALL(int, asm2_jint, i)
ASM2_DEFINE_STATIC_CALL(long, asm2_jlong, j)
ASM2_DEFINE_STATIC_CALL(float, asm2_jfloat, f)
ASM2_DEFINE_STATIC_CALL(double, asm2_jdouble, d)

static void ASM2_GUEST_PCS asm2_call_static_void(
    void *environment, void *class_handle, void *method_id, ...) {
  (void)environment;
  va_list arguments;
  va_start(arguments, method_id);
  (void)asm2_dispatch_method(class_handle, method_id, &arguments, NULL);
  va_end(arguments);
}

static void ASM2_GUEST_PCS asm2_call_static_void_v(
    void *environment, void *class_handle, void *method_id,
    va_list arguments) {
  (void)environment;
  (void)asm2_dispatch_method_v(class_handle, method_id, arguments);
}

static void ASM2_GUEST_PCS asm2_call_static_void_a(
    void *environment, void *class_handle, void *method_id,
    const asm2_jni_value *arguments) {
  (void)environment;
  (void)asm2_dispatch_method(class_handle, method_id, NULL, arguments);
}

static void *ASM2_GUEST_PCS asm2_get_field_id(void *environment,
                                              void *class_handle,
                                              const char *name,
                                              const char *signature) {
  (void)environment;
  return asm2_intern_id(ASM2_HANDLE_FIELD, class_handle, name, signature, 0);
}

static void *ASM2_GUEST_PCS asm2_get_static_field_id(
    void *environment, void *class_handle, const char *name,
    const char *signature) {
  (void)environment;
  return asm2_intern_id(ASM2_HANDLE_FIELD, class_handle, name, signature, 1);
}

#define ASM2_DEFINE_FIELD_ACCESS(stem, value_type, member)                    \
  static value_type ASM2_GUEST_PCS asm2_get_##stem##_field(                  \
      void *environment, void *receiver, void *field_id) {                   \
    (void)environment;                                                        \
    asm2_jni_value result = asm2_get_field(receiver, field_id);               \
    return (value_type)result.member;                                         \
  }                                                                           \
  static void ASM2_GUEST_PCS asm2_set_##stem##_field(                        \
      void *environment, void *receiver, void *field_id, value_type input) { \
    (void)environment;                                                        \
    asm2_jni_value value = asm2_zero_value();                                 \
    value.member = input;                                                     \
    asm2_set_field(receiver, field_id, value);                                \
  }                                                                           \
  static value_type ASM2_GUEST_PCS asm2_get_static_##stem##_field(           \
      void *environment, void *class_handle, void *field_id) {               \
    (void)environment;                                                        \
    asm2_jni_value result = asm2_get_field(class_handle, field_id);           \
    return (value_type)result.member;                                         \
  }                                                                           \
  static void ASM2_GUEST_PCS asm2_set_static_##stem##_field(                 \
      void *environment, void *class_handle, void *field_id,                 \
      value_type input) {                                                     \
    (void)environment;                                                        \
    asm2_jni_value value = asm2_zero_value();                                 \
    value.member = input;                                                     \
    asm2_set_field(class_handle, field_id, value);                            \
  }

ASM2_DEFINE_FIELD_ACCESS(object, void *, l)
ASM2_DEFINE_FIELD_ACCESS(boolean, asm2_jboolean, z)
ASM2_DEFINE_FIELD_ACCESS(byte, asm2_jbyte, b)
ASM2_DEFINE_FIELD_ACCESS(char, asm2_jchar, c)
ASM2_DEFINE_FIELD_ACCESS(short, asm2_jshort, s)
ASM2_DEFINE_FIELD_ACCESS(int, asm2_jint, i)
ASM2_DEFINE_FIELD_ACCESS(long, asm2_jlong, j)
ASM2_DEFINE_FIELD_ACCESS(float, asm2_jfloat, f)
ASM2_DEFINE_FIELD_ACCESS(double, asm2_jdouble, d)

static char *asm2_utf16_to_utf8(const asm2_jchar *unicode, size_t length) {
  if (!unicode && length)
    return NULL;
  char *output = malloc(length * 4 + 1);
  if (!output)
    return NULL;
  size_t offset = 0;
  for (size_t index = 0; index < length; ++index) {
    uint32_t codepoint = unicode[index];
    if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 1 < length &&
        unicode[index + 1] >= 0xdc00 && unicode[index + 1] <= 0xdfff) {
      codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) +
                  (unicode[++index] - 0xdc00u);
    }
    if (codepoint < 0x80) {
      output[offset++] = (char)codepoint;
    } else if (codepoint < 0x800) {
      output[offset++] = (char)(0xc0 | (codepoint >> 6));
      output[offset++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
      output[offset++] = (char)(0xe0 | (codepoint >> 12));
      output[offset++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
      output[offset++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
      output[offset++] = (char)(0xf0 | (codepoint >> 18));
      output[offset++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
      output[offset++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
      output[offset++] = (char)(0x80 | (codepoint & 0x3f));
    }
  }
  output[offset] = '\0';
  return output;
}

static void *ASM2_GUEST_PCS asm2_new_string(void *environment,
                                            const asm2_jchar *unicode,
                                            asm2_jint length) {
  (void)environment;
  if (length < 0 || (!unicode && length)) {
    asm2_throw_new(environment,
                   asm2_jni_class("java/lang/IllegalArgumentException"),
                   "invalid NewString input");
    return NULL;
  }
  char *utf8 = asm2_utf16_to_utf8(unicode, (size_t)length);
  void *result = utf8 ? asm2_jni_string(utf8) : NULL;
  free(utf8);
  return result;
}

static asm2_jint ASM2_GUEST_PCS asm2_get_string_length(void *environment,
                                                       void *string) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(string);
  return item && item->kind == ASM2_HANDLE_STRING
             ? (asm2_jint)item->utf16_length
             : 0;
}

static const asm2_jchar *ASM2_GUEST_PCS asm2_get_string_chars(
    void *environment, void *string, asm2_jboolean *is_copy) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(string);
  if (is_copy)
    *is_copy = 0;
  return item && item->kind == ASM2_HANDLE_STRING ? item->utf16 : NULL;
}

static void ASM2_GUEST_PCS asm2_release_string_chars(
    void *environment, void *string, const asm2_jchar *characters) {
  (void)environment;
  (void)string;
  (void)characters;
}

static void *ASM2_GUEST_PCS asm2_new_string_utf(void *environment,
                                                const char *utf8) {
  (void)environment;
  return asm2_jni_string(utf8 ? utf8 : "");
}

static asm2_jint ASM2_GUEST_PCS asm2_get_string_utf_length(
    void *environment, void *string) {
  (void)environment;
  const char *text = asm2_jni_string_chars(string);
  return text ? (asm2_jint)strlen(text) : 0;
}

static const char *ASM2_GUEST_PCS asm2_get_string_utf_chars(
    void *environment, void *string, asm2_jboolean *is_copy) {
  (void)environment;
  if (is_copy)
    *is_copy = 0;
  return asm2_jni_string_chars(string);
}

static void ASM2_GUEST_PCS asm2_release_string_utf_chars(
    void *environment, void *string, const char *characters) {
  (void)environment;
  (void)string;
  (void)characters;
}

static asm2_jint ASM2_GUEST_PCS asm2_get_array_length(void *environment,
                                                      void *array) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(array);
  return item && item->kind == ASM2_HANDLE_ARRAY ? (asm2_jint)item->length : 0;
}

static void *ASM2_GUEST_PCS asm2_new_object_array(void *environment,
                                                  asm2_jint length,
                                                  void *class_handle,
                                                  void *initial) {
  (void)environment;
  if (length < 0)
    return NULL;
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  const char *component = class_item && class_item->class_name
                              ? class_item->class_name
                              : "java/lang/Object";
  char descriptor[512];
  snprintf(descriptor, sizeof(descriptor), "[L%s;", component);
  asm2_handle *array =
      asm2_new_array(descriptor, (size_t)length, sizeof(void *));
  if (array && array->data && initial) {
    void **elements = array->data;
    for (asm2_jint index = 0; index < length; ++index)
      elements[index] = initial;
  }
  return array;
}

static int asm2_array_index(asm2_handle *array, asm2_jint index) {
  return array && array->kind == ASM2_HANDLE_ARRAY && index >= 0 &&
         (size_t)index < array->length;
}

static void *ASM2_GUEST_PCS asm2_get_object_array_element(
    void *environment, void *array_handle, asm2_jint index) {
  asm2_handle *array = asm2_lookup_handle(array_handle);
  if (!asm2_array_index(array, index) || array->element_size != sizeof(void *)) {
    asm2_throw_new(environment,
                   asm2_jni_class("java/lang/ArrayIndexOutOfBoundsException"),
                   "GetObjectArrayElement");
    return NULL;
  }
  return ((void **)array->data)[index];
}

static void ASM2_GUEST_PCS asm2_set_object_array_element(
    void *environment, void *array_handle, asm2_jint index, void *value) {
  asm2_handle *array = asm2_lookup_handle(array_handle);
  if (!asm2_array_index(array, index) || array->element_size != sizeof(void *)) {
    asm2_throw_new(environment,
                   asm2_jni_class("java/lang/ArrayIndexOutOfBoundsException"),
                   "SetObjectArrayElement");
    return;
  }
  ((void **)array->data)[index] = value;
}

#define ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(stem, descriptor, element_type)       \
  static void *ASM2_GUEST_PCS asm2_new_##stem##_array(void *environment,     \
                                                       asm2_jint length) {    \
    (void)environment;                                                        \
    return length < 0                                                         \
               ? NULL                                                         \
               : asm2_new_array(descriptor, (size_t)length,                  \
                                sizeof(element_type));                        \
  }

ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(boolean, "[Z", asm2_jboolean)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(byte, "[B", asm2_jbyte)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(char, "[C", asm2_jchar)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(short, "[S", asm2_jshort)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(int, "[I", asm2_jint)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(long, "[J", asm2_jlong)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(float, "[F", asm2_jfloat)
ASM2_DEFINE_NEW_PRIMITIVE_ARRAY(double, "[D", asm2_jdouble)

#define ASM2_DEFINE_ARRAY_ACCESS(stem, element_type)                          \
  static element_type *ASM2_GUEST_PCS asm2_get_##stem##_array_elements(      \
      void *environment, void *array_handle, asm2_jboolean *is_copy) {       \
    (void)environment;                                                        \
    asm2_handle *array = asm2_lookup_handle(array_handle);                    \
    if (is_copy)                                                              \
      *is_copy = 0;                                                           \
    return array && array->kind == ASM2_HANDLE_ARRAY &&                       \
                   array->element_size == sizeof(element_type)                \
               ? (element_type *)array->data                                 \
               : NULL;                                                        \
  }                                                                           \
  static void ASM2_GUEST_PCS asm2_release_##stem##_array_elements(           \
      void *environment, void *array_handle, element_type *elements,         \
      asm2_jint mode) {                                                       \
    (void)environment;                                                        \
    (void)array_handle;                                                       \
    (void)elements;                                                           \
    (void)mode;                                                               \
  }                                                                           \
  static void ASM2_GUEST_PCS asm2_get_##stem##_array_region(                 \
      void *environment, void *array_handle, asm2_jint start,                \
      asm2_jint length, element_type *output) {                               \
    asm2_handle *array = asm2_lookup_handle(array_handle);                    \
    if (!array || array->kind != ASM2_HANDLE_ARRAY || start < 0 ||           \
        length < 0 || (size_t)start > array->length ||                        \
        (size_t)length > array->length - (size_t)start || !output ||          \
        array->element_size != sizeof(element_type)) {                        \
      asm2_throw_new(environment,                                             \
                     asm2_jni_class("java/lang/ArrayIndexOutOfBoundsException"), \
                     "GetArrayRegion");                                      \
      return;                                                                 \
    }                                                                         \
    memcpy(output, (element_type *)array->data + start,                       \
           (size_t)length * sizeof(element_type));                            \
  }                                                                           \
  static void ASM2_GUEST_PCS asm2_set_##stem##_array_region(                 \
      void *environment, void *array_handle, asm2_jint start,                \
      asm2_jint length, const element_type *input) {                          \
    asm2_handle *array = asm2_lookup_handle(array_handle);                    \
    if (!array || array->kind != ASM2_HANDLE_ARRAY || start < 0 ||           \
        length < 0 || (size_t)start > array->length ||                        \
        (size_t)length > array->length - (size_t)start || !input ||           \
        array->element_size != sizeof(element_type)) {                        \
      asm2_throw_new(environment,                                             \
                     asm2_jni_class("java/lang/ArrayIndexOutOfBoundsException"), \
                     "SetArrayRegion");                                      \
      return;                                                                 \
    }                                                                         \
    memcpy((element_type *)array->data + start, input,                        \
           (size_t)length * sizeof(element_type));                            \
  }

ASM2_DEFINE_ARRAY_ACCESS(boolean, asm2_jboolean)
ASM2_DEFINE_ARRAY_ACCESS(byte, asm2_jbyte)
ASM2_DEFINE_ARRAY_ACCESS(char, asm2_jchar)
ASM2_DEFINE_ARRAY_ACCESS(short, asm2_jshort)
ASM2_DEFINE_ARRAY_ACCESS(int, asm2_jint)
ASM2_DEFINE_ARRAY_ACCESS(long, asm2_jlong)
ASM2_DEFINE_ARRAY_ACCESS(float, asm2_jfloat)
ASM2_DEFINE_ARRAY_ACCESS(double, asm2_jdouble)

static asm2_jint ASM2_GUEST_PCS asm2_register_natives(
    void *environment, void *class_handle, const asm2_native_method *methods,
    asm2_jint method_count) {
  (void)environment;
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  if (!class_item || !methods || method_count < 0)
    return ASM2_JNI_ERR;

  for (asm2_jint index = 0; index < method_count; ++index) {
    const char *name = methods[index].name ? methods[index].name : "";
    const char *signature =
        methods[index].signature ? methods[index].signature : "";
    void *function = methods[index].function;

    pthread_mutex_lock(&asm2_jni_lock);
    asm2_registered_native *record = NULL;
    for (asm2_registered_native *item = asm2_natives; item; item = item->next) {
      if (asm2_text_equal(item->class_name, class_item->class_name) &&
          asm2_text_equal(item->name, name) &&
          asm2_text_equal(item->signature, signature)) {
        record = item;
        break;
      }
    }
    int was_active = record && record->function;
    if (!record) {
      record = calloc(1, sizeof(*record));
      if (record) {
        record->class_name = asm2_duplicate(class_item->class_name);
        record->name = asm2_duplicate(name);
        record->signature = asm2_duplicate(signature);
        record->next = asm2_natives;
        asm2_natives = record;
      }
    }
    if (record) {
      record->function = function;
      if (!was_active && function)
        ++asm2_native_count;
      else if (was_active && !function && asm2_native_count)
        --asm2_native_count;
    }
    pthread_mutex_unlock(&asm2_jni_lock);

    if (!record)
      return ASM2_JNI_ERR;
    if (asm2_config.native_callback)
      asm2_config.native_callback(asm2_config.user, class_item->class_name,
                                  name, signature, function);
  }
  return ASM2_JNI_OK;
}

static asm2_jint ASM2_GUEST_PCS asm2_unregister_natives(
    void *environment, void *class_handle) {
  (void)environment;
  asm2_handle *class_item = asm2_lookup_handle(class_handle);
  if (!class_item)
    return ASM2_JNI_ERR;
  pthread_mutex_lock(&asm2_jni_lock);
  for (asm2_registered_native *item = asm2_natives; item; item = item->next) {
    if (item->function &&
        asm2_text_equal(item->class_name, class_item->class_name)) {
      item->function = NULL;
      if (asm2_native_count)
        --asm2_native_count;
    }
  }
  pthread_mutex_unlock(&asm2_jni_lock);
  return ASM2_JNI_OK;
}

void *asm2_jni_find_registered_native(const char *class_name,
                                      const char *name,
                                      const char *signature) {
  void *result = NULL;
  pthread_mutex_lock(&asm2_jni_lock);
  for (asm2_registered_native *item = asm2_natives; item; item = item->next) {
    if (item->function && asm2_text_equal(item->class_name, class_name) &&
        asm2_text_equal(item->name, name) &&
        asm2_text_equal(item->signature, signature)) {
      result = item->function;
      break;
    }
  }
  pthread_mutex_unlock(&asm2_jni_lock);
  return result;
}

size_t asm2_jni_registered_native_count(void) {
  pthread_mutex_lock(&asm2_jni_lock);
  size_t result = asm2_native_count;
  pthread_mutex_unlock(&asm2_jni_lock);
  return result;
}

static asm2_jint ASM2_GUEST_PCS asm2_monitor(void *environment, void *object) {
  (void)environment;
  (void)object;
  return ASM2_JNI_OK;
}

static asm2_jint ASM2_GUEST_PCS asm2_get_java_vm(void *environment,
                                                 void **vm) {
  (void)environment;
  if (!vm)
    return ASM2_JNI_ERR;
  *vm = asm2_jni_vm();
  return ASM2_JNI_OK;
}

static void ASM2_GUEST_PCS asm2_get_string_region(
    void *environment, void *string, asm2_jint start, asm2_jint length,
    asm2_jchar *output) {
  asm2_handle *item = asm2_lookup_handle(string);
  if (!item || item->kind != ASM2_HANDLE_STRING || start < 0 || length < 0 ||
      (size_t)start > item->utf16_length ||
      (size_t)length > item->utf16_length - (size_t)start || !output) {
    asm2_throw_new(environment,
                   asm2_jni_class("java/lang/StringIndexOutOfBoundsException"),
                   "GetStringRegion");
    return;
  }
  memcpy(output, item->utf16 + start, (size_t)length * sizeof(*output));
}

static void ASM2_GUEST_PCS asm2_get_string_utf_region(
    void *environment, void *string, asm2_jint start, asm2_jint length,
    char *output) {
  asm2_handle *item = asm2_lookup_handle(string);
  if (!item || item->kind != ASM2_HANDLE_STRING || start < 0 || length < 0 ||
      (size_t)start > item->utf16_length ||
      (size_t)length > item->utf16_length - (size_t)start || !output) {
    asm2_throw_new(environment,
                   asm2_jni_class("java/lang/StringIndexOutOfBoundsException"),
                   "GetStringUTFRegion");
    return;
  }
  char *utf8 = asm2_utf16_to_utf8(item->utf16 + start, (size_t)length);
  if (utf8) {
    memcpy(output, utf8, strlen(utf8)); /* JNI region output has no terminator. */
    free(utf8);
  }
}

static void *ASM2_GUEST_PCS asm2_get_primitive_array_critical(
    void *environment, void *array_handle, asm2_jboolean *is_copy) {
  (void)environment;
  asm2_handle *array = asm2_lookup_handle(array_handle);
  if (is_copy)
    *is_copy = 0;
  return array && array->kind == ASM2_HANDLE_ARRAY ? array->data : NULL;
}

static void ASM2_GUEST_PCS asm2_release_primitive_array_critical(
    void *environment, void *array_handle, void *data, asm2_jint mode) {
  (void)environment;
  (void)array_handle;
  (void)data;
  (void)mode;
}

static const asm2_jchar *ASM2_GUEST_PCS asm2_get_string_critical(
    void *environment, void *string, asm2_jboolean *is_copy) {
  return asm2_get_string_chars(environment, string, is_copy);
}

static void ASM2_GUEST_PCS asm2_release_string_critical(
    void *environment, void *string, const asm2_jchar *characters) {
  asm2_release_string_chars(environment, string, characters);
}

static asm2_jboolean ASM2_GUEST_PCS asm2_exception_check(void *environment) {
  (void)environment;
  return asm2_pending_exception != NULL;
}

static void *ASM2_GUEST_PCS asm2_new_direct_byte_buffer(
    void *environment, void *address, asm2_jlong capacity) {
  (void)environment;
  if (!address || capacity < 0)
    return NULL;
  pthread_mutex_lock(&asm2_jni_lock);
  asm2_handle *item = asm2_allocate_handle_locked(
      ASM2_HANDLE_DIRECT_BUFFER, "java/nio/DirectByteBuffer");
  if (item) {
    item->data = address;
    item->capacity = capacity;
  }
  pthread_mutex_unlock(&asm2_jni_lock);
  return item;
}

static void *ASM2_GUEST_PCS asm2_get_direct_buffer_address(void *environment,
                                                           void *buffer) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(buffer);
  return item && item->kind == ASM2_HANDLE_DIRECT_BUFFER ? item->data : NULL;
}

static asm2_jlong ASM2_GUEST_PCS asm2_get_direct_buffer_capacity(
    void *environment, void *buffer) {
  (void)environment;
  asm2_handle *item = asm2_lookup_handle(buffer);
  return item && item->kind == ASM2_HANDLE_DIRECT_BUFFER ? item->capacity : -1;
}

static asm2_jint ASM2_GUEST_PCS asm2_get_object_ref_type(void *environment,
                                                         void *object) {
  (void)environment;
  return object ? 1 : 0; /* JNILocalRefType / JNIInvalidRefType */
}

#define ASM2_TABLE_ENTRY(table, index, function) \
  ((table)[(index)] = (void *)(function))

static void asm2_fill_vm_table(void) {
  memset(asm2_vm_table, 0, sizeof(asm2_vm_table));
  ASM2_TABLE_ENTRY(asm2_vm_table, 3, asm2_vm_destroy);
  ASM2_TABLE_ENTRY(asm2_vm_table, 4, asm2_vm_attach);
  ASM2_TABLE_ENTRY(asm2_vm_table, 5, asm2_vm_detach);
  ASM2_TABLE_ENTRY(asm2_vm_table, 6, asm2_vm_get_env);
  ASM2_TABLE_ENTRY(asm2_vm_table, 7, asm2_vm_attach);
}

static void asm2_fill_env_table(void) {
  memset(asm2_env_table, 0, sizeof(asm2_env_table));

  /* JNI 1.6 JNINativeInterface_, indices verified against the JNI spec. */
  ASM2_TABLE_ENTRY(asm2_env_table, 4, asm2_get_version);
  ASM2_TABLE_ENTRY(asm2_env_table, 5, asm2_define_class);
  ASM2_TABLE_ENTRY(asm2_env_table, 6, asm2_find_class);
  ASM2_TABLE_ENTRY(asm2_env_table, 7, asm2_from_reflected);
  ASM2_TABLE_ENTRY(asm2_env_table, 8, asm2_from_reflected);
  ASM2_TABLE_ENTRY(asm2_env_table, 9, asm2_to_reflected_method);
  ASM2_TABLE_ENTRY(asm2_env_table, 10, asm2_get_superclass);
  ASM2_TABLE_ENTRY(asm2_env_table, 11, asm2_is_assignable);
  ASM2_TABLE_ENTRY(asm2_env_table, 12, asm2_to_reflected_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 13, asm2_throw);
  ASM2_TABLE_ENTRY(asm2_env_table, 14, asm2_throw_new);
  ASM2_TABLE_ENTRY(asm2_env_table, 15, asm2_exception_occurred);
  ASM2_TABLE_ENTRY(asm2_env_table, 16, asm2_exception_describe);
  ASM2_TABLE_ENTRY(asm2_env_table, 17, asm2_exception_clear);
  ASM2_TABLE_ENTRY(asm2_env_table, 18, asm2_fatal_error);
  ASM2_TABLE_ENTRY(asm2_env_table, 19, asm2_push_local_frame);
  ASM2_TABLE_ENTRY(asm2_env_table, 20, asm2_pop_local_frame);
  ASM2_TABLE_ENTRY(asm2_env_table, 21, asm2_identity_ref);
  ASM2_TABLE_ENTRY(asm2_env_table, 22, asm2_delete_ref);
  ASM2_TABLE_ENTRY(asm2_env_table, 23, asm2_delete_ref);
  ASM2_TABLE_ENTRY(asm2_env_table, 24, asm2_is_same_object);
  ASM2_TABLE_ENTRY(asm2_env_table, 25, asm2_identity_ref);
  ASM2_TABLE_ENTRY(asm2_env_table, 26, asm2_ensure_local_capacity);
  ASM2_TABLE_ENTRY(asm2_env_table, 27, asm2_alloc_object);
  ASM2_TABLE_ENTRY(asm2_env_table, 28, asm2_new_object);
  ASM2_TABLE_ENTRY(asm2_env_table, 29, asm2_new_object_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 30, asm2_new_object_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 31, asm2_get_object_class);
  ASM2_TABLE_ENTRY(asm2_env_table, 32, asm2_is_instance_of);
  ASM2_TABLE_ENTRY(asm2_env_table, 33, asm2_get_method_id);

  ASM2_TABLE_ENTRY(asm2_env_table, 34, asm2_call_object);
  ASM2_TABLE_ENTRY(asm2_env_table, 35, asm2_call_object_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 36, asm2_call_object_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 37, asm2_call_boolean);
  ASM2_TABLE_ENTRY(asm2_env_table, 38, asm2_call_boolean_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 39, asm2_call_boolean_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 40, asm2_call_byte);
  ASM2_TABLE_ENTRY(asm2_env_table, 41, asm2_call_byte_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 42, asm2_call_byte_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 43, asm2_call_char);
  ASM2_TABLE_ENTRY(asm2_env_table, 44, asm2_call_char_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 45, asm2_call_char_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 46, asm2_call_short);
  ASM2_TABLE_ENTRY(asm2_env_table, 47, asm2_call_short_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 48, asm2_call_short_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 49, asm2_call_int);
  ASM2_TABLE_ENTRY(asm2_env_table, 50, asm2_call_int_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 51, asm2_call_int_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 52, asm2_call_long);
  ASM2_TABLE_ENTRY(asm2_env_table, 53, asm2_call_long_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 54, asm2_call_long_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 55, asm2_call_float);
  ASM2_TABLE_ENTRY(asm2_env_table, 56, asm2_call_float_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 57, asm2_call_float_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 58, asm2_call_double);
  ASM2_TABLE_ENTRY(asm2_env_table, 59, asm2_call_double_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 60, asm2_call_double_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 61, asm2_call_void);
  ASM2_TABLE_ENTRY(asm2_env_table, 62, asm2_call_void_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 63, asm2_call_void_a);

  ASM2_TABLE_ENTRY(asm2_env_table, 64, asm2_call_nonvirtual_object);
  ASM2_TABLE_ENTRY(asm2_env_table, 65, asm2_call_nonvirtual_object_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 66, asm2_call_nonvirtual_object_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 67, asm2_call_nonvirtual_boolean);
  ASM2_TABLE_ENTRY(asm2_env_table, 68, asm2_call_nonvirtual_boolean_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 69, asm2_call_nonvirtual_boolean_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 70, asm2_call_nonvirtual_byte);
  ASM2_TABLE_ENTRY(asm2_env_table, 71, asm2_call_nonvirtual_byte_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 72, asm2_call_nonvirtual_byte_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 73, asm2_call_nonvirtual_char);
  ASM2_TABLE_ENTRY(asm2_env_table, 74, asm2_call_nonvirtual_char_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 75, asm2_call_nonvirtual_char_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 76, asm2_call_nonvirtual_short);
  ASM2_TABLE_ENTRY(asm2_env_table, 77, asm2_call_nonvirtual_short_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 78, asm2_call_nonvirtual_short_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 79, asm2_call_nonvirtual_int);
  ASM2_TABLE_ENTRY(asm2_env_table, 80, asm2_call_nonvirtual_int_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 81, asm2_call_nonvirtual_int_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 82, asm2_call_nonvirtual_long);
  ASM2_TABLE_ENTRY(asm2_env_table, 83, asm2_call_nonvirtual_long_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 84, asm2_call_nonvirtual_long_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 85, asm2_call_nonvirtual_float);
  ASM2_TABLE_ENTRY(asm2_env_table, 86, asm2_call_nonvirtual_float_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 87, asm2_call_nonvirtual_float_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 88, asm2_call_nonvirtual_double);
  ASM2_TABLE_ENTRY(asm2_env_table, 89, asm2_call_nonvirtual_double_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 90, asm2_call_nonvirtual_double_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 91, asm2_call_nonvirtual_void);
  ASM2_TABLE_ENTRY(asm2_env_table, 92, asm2_call_nonvirtual_void_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 93, asm2_call_nonvirtual_void_a);

  ASM2_TABLE_ENTRY(asm2_env_table, 94, asm2_get_field_id);
  ASM2_TABLE_ENTRY(asm2_env_table, 95, asm2_get_object_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 96, asm2_get_boolean_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 97, asm2_get_byte_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 98, asm2_get_char_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 99, asm2_get_short_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 100, asm2_get_int_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 101, asm2_get_long_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 102, asm2_get_float_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 103, asm2_get_double_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 104, asm2_set_object_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 105, asm2_set_boolean_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 106, asm2_set_byte_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 107, asm2_set_char_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 108, asm2_set_short_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 109, asm2_set_int_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 110, asm2_set_long_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 111, asm2_set_float_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 112, asm2_set_double_field);

  ASM2_TABLE_ENTRY(asm2_env_table, 113, asm2_get_static_method_id);
  ASM2_TABLE_ENTRY(asm2_env_table, 114, asm2_call_static_object);
  ASM2_TABLE_ENTRY(asm2_env_table, 115, asm2_call_static_object_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 116, asm2_call_static_object_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 117, asm2_call_static_boolean);
  ASM2_TABLE_ENTRY(asm2_env_table, 118, asm2_call_static_boolean_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 119, asm2_call_static_boolean_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 120, asm2_call_static_byte);
  ASM2_TABLE_ENTRY(asm2_env_table, 121, asm2_call_static_byte_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 122, asm2_call_static_byte_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 123, asm2_call_static_char);
  ASM2_TABLE_ENTRY(asm2_env_table, 124, asm2_call_static_char_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 125, asm2_call_static_char_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 126, asm2_call_static_short);
  ASM2_TABLE_ENTRY(asm2_env_table, 127, asm2_call_static_short_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 128, asm2_call_static_short_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 129, asm2_call_static_int);
  ASM2_TABLE_ENTRY(asm2_env_table, 130, asm2_call_static_int_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 131, asm2_call_static_int_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 132, asm2_call_static_long);
  ASM2_TABLE_ENTRY(asm2_env_table, 133, asm2_call_static_long_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 134, asm2_call_static_long_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 135, asm2_call_static_float);
  ASM2_TABLE_ENTRY(asm2_env_table, 136, asm2_call_static_float_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 137, asm2_call_static_float_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 138, asm2_call_static_double);
  ASM2_TABLE_ENTRY(asm2_env_table, 139, asm2_call_static_double_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 140, asm2_call_static_double_a);
  ASM2_TABLE_ENTRY(asm2_env_table, 141, asm2_call_static_void);
  ASM2_TABLE_ENTRY(asm2_env_table, 142, asm2_call_static_void_v);
  ASM2_TABLE_ENTRY(asm2_env_table, 143, asm2_call_static_void_a);

  ASM2_TABLE_ENTRY(asm2_env_table, 144, asm2_get_static_field_id);
  ASM2_TABLE_ENTRY(asm2_env_table, 145, asm2_get_static_object_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 146, asm2_get_static_boolean_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 147, asm2_get_static_byte_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 148, asm2_get_static_char_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 149, asm2_get_static_short_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 150, asm2_get_static_int_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 151, asm2_get_static_long_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 152, asm2_get_static_float_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 153, asm2_get_static_double_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 154, asm2_set_static_object_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 155, asm2_set_static_boolean_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 156, asm2_set_static_byte_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 157, asm2_set_static_char_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 158, asm2_set_static_short_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 159, asm2_set_static_int_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 160, asm2_set_static_long_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 161, asm2_set_static_float_field);
  ASM2_TABLE_ENTRY(asm2_env_table, 162, asm2_set_static_double_field);

  ASM2_TABLE_ENTRY(asm2_env_table, 163, asm2_new_string);
  ASM2_TABLE_ENTRY(asm2_env_table, 164, asm2_get_string_length);
  ASM2_TABLE_ENTRY(asm2_env_table, 165, asm2_get_string_chars);
  ASM2_TABLE_ENTRY(asm2_env_table, 166, asm2_release_string_chars);
  ASM2_TABLE_ENTRY(asm2_env_table, 167, asm2_new_string_utf);
  ASM2_TABLE_ENTRY(asm2_env_table, 168, asm2_get_string_utf_length);
  ASM2_TABLE_ENTRY(asm2_env_table, 169, asm2_get_string_utf_chars);
  ASM2_TABLE_ENTRY(asm2_env_table, 170, asm2_release_string_utf_chars);
  ASM2_TABLE_ENTRY(asm2_env_table, 171, asm2_get_array_length);
  ASM2_TABLE_ENTRY(asm2_env_table, 172, asm2_new_object_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 173, asm2_get_object_array_element);
  ASM2_TABLE_ENTRY(asm2_env_table, 174, asm2_set_object_array_element);
  ASM2_TABLE_ENTRY(asm2_env_table, 175, asm2_new_boolean_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 176, asm2_new_byte_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 177, asm2_new_char_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 178, asm2_new_short_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 179, asm2_new_int_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 180, asm2_new_long_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 181, asm2_new_float_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 182, asm2_new_double_array);
  ASM2_TABLE_ENTRY(asm2_env_table, 183, asm2_get_boolean_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 184, asm2_get_byte_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 185, asm2_get_char_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 186, asm2_get_short_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 187, asm2_get_int_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 188, asm2_get_long_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 189, asm2_get_float_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 190, asm2_get_double_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 191, asm2_release_boolean_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 192, asm2_release_byte_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 193, asm2_release_char_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 194, asm2_release_short_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 195, asm2_release_int_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 196, asm2_release_long_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 197, asm2_release_float_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 198, asm2_release_double_array_elements);
  ASM2_TABLE_ENTRY(asm2_env_table, 199, asm2_get_boolean_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 200, asm2_get_byte_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 201, asm2_get_char_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 202, asm2_get_short_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 203, asm2_get_int_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 204, asm2_get_long_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 205, asm2_get_float_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 206, asm2_get_double_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 207, asm2_set_boolean_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 208, asm2_set_byte_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 209, asm2_set_char_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 210, asm2_set_short_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 211, asm2_set_int_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 212, asm2_set_long_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 213, asm2_set_float_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 214, asm2_set_double_array_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 215, asm2_register_natives);
  ASM2_TABLE_ENTRY(asm2_env_table, 216, asm2_unregister_natives);
  ASM2_TABLE_ENTRY(asm2_env_table, 217, asm2_monitor);
  ASM2_TABLE_ENTRY(asm2_env_table, 218, asm2_monitor);
  ASM2_TABLE_ENTRY(asm2_env_table, 219, asm2_get_java_vm);
  ASM2_TABLE_ENTRY(asm2_env_table, 220, asm2_get_string_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 221, asm2_get_string_utf_region);
  ASM2_TABLE_ENTRY(asm2_env_table, 222, asm2_get_primitive_array_critical);
  ASM2_TABLE_ENTRY(asm2_env_table, 223, asm2_release_primitive_array_critical);
  ASM2_TABLE_ENTRY(asm2_env_table, 224, asm2_get_string_critical);
  ASM2_TABLE_ENTRY(asm2_env_table, 225, asm2_release_string_critical);
  ASM2_TABLE_ENTRY(asm2_env_table, 226, asm2_identity_ref);
  ASM2_TABLE_ENTRY(asm2_env_table, 227, asm2_delete_ref);
  ASM2_TABLE_ENTRY(asm2_env_table, 228, asm2_exception_check);
  ASM2_TABLE_ENTRY(asm2_env_table, 229, asm2_new_direct_byte_buffer);
  ASM2_TABLE_ENTRY(asm2_env_table, 230, asm2_get_direct_buffer_address);
  ASM2_TABLE_ENTRY(asm2_env_table, 231, asm2_get_direct_buffer_capacity);
  ASM2_TABLE_ENTRY(asm2_env_table, 232, asm2_get_object_ref_type);
}

static const char *asm2_config_text(const char *value,
                                    const char *fallback) {
  return value && value[0] ? value : fallback;
}

void asm2_jni_init(const asm2_jni_config *config) {
  asm2_jni_config input;
  memset(&input, 0, sizeof(input));
  if (config)
    input = *config;

  memset(&asm2_config, 0, sizeof(asm2_config));
  asm2_config = input;
  asm2_config.package_name = asm2_duplicate(asm2_config_text(
      input.package_name, "com.gameloft.android.ANMP.GloftASHM"));
  asm2_config.external_storage = asm2_duplicate(
      asm2_config_text(input.external_storage, "/sdcard"));
  asm2_config.files_directory = asm2_duplicate(
      asm2_config_text(input.files_directory,
                       "/data/data/com.gameloft.android.ANMP.GloftASHM/files"));
  asm2_config.cache_directory = asm2_duplicate(
      asm2_config_text(input.cache_directory,
                       "/data/data/com.gameloft.android.ANMP.GloftASHM/cache"));
  asm2_config.locale_language =
      asm2_duplicate(asm2_config_text(input.locale_language, "en"));
  asm2_config.locale_country =
      asm2_duplicate(asm2_config_text(input.locale_country, "US"));
  asm2_config.device_model =
      asm2_duplicate(asm2_config_text(input.device_model, "NextOS ARMv7"));
  asm2_config.device_manufacturer = asm2_duplicate(
      asm2_config_text(input.device_manufacturer, "NextOS"));
  asm2_config.android_release =
      asm2_duplicate(asm2_config_text(input.android_release, "8.1"));
  if (asm2_config.sdk_int <= 0)
    asm2_config.sdk_int = 27;
  if (asm2_config.screen_width <= 0)
    asm2_config.screen_width = 1280;
  if (asm2_config.screen_height <= 0)
    asm2_config.screen_height = 720;
  if (asm2_config.screen_density <= 0.0f)
    asm2_config.screen_density = 1.0f;

  asm2_fill_vm_table();
  asm2_fill_env_table();
  asm2_initialized = 1;
  asm2_pending_exception = NULL;
}
