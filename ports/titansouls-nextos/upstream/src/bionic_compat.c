#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif

#include "bionic_compat.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "util.h"

enum {
  ASM2_CTYPE_U = 0x01,
  ASM2_CTYPE_L = 0x02,
  ASM2_CTYPE_N = 0x04,
  ASM2_CTYPE_S = 0x08,
  ASM2_CTYPE_P = 0x10,
  ASM2_CTYPE_C = 0x20,
  ASM2_CTYPE_X = 0x40,
  ASM2_CTYPE_B = 0x80,
};

static char asm2_ctype_storage[257];
static short asm2_tolower_storage[257];
static short asm2_toupper_storage[257];
const char *asm2_ctype_ptr = asm2_ctype_storage;
const short *asm2_tolower_ptr = asm2_tolower_storage;
const short *asm2_toupper_ptr = asm2_toupper_storage;
int asm2_page_size = 4096;
uintptr_t asm2_stack_chk_guard = 0x9e3779b9u;
unsigned char asm2_bionic_sF[3][84] __attribute__((aligned(4)));

static char asm2_storage_root[PATH_MAX];

enum {
  ASM2_FILE_SRD = 0x0004,
  ASM2_FILE_SWR = 0x0008,
  ASM2_FILE_SRW = 0x0010,
  ASM2_FILE_SEOF = 0x0020,
  ASM2_FILE_SERR = 0x0040,
  ASM2_FILE_SAPP = 0x0100,
};

struct asm2_file_wrapper {
  unsigned char guest[84];
  FILE *host;
  uint16_t base_flags;
  unsigned int active_operations;
  int state;
  struct asm2_file_wrapper *hash_next;
  struct asm2_file_wrapper *retired_next;
};

enum {
  ASM2_FILE_OPEN = 1,
  ASM2_FILE_CLOSING,
  ASM2_FILE_CLOSED,
};

struct asm2_stream_reference {
  FILE *host;
  struct asm2_file_wrapper *wrapper;
};

static pthread_mutex_t asm2_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t asm2_file_condition = PTHREAD_COND_INITIALIZER;
#define ASM2_FILE_BUCKETS 1024u
static struct asm2_file_wrapper *asm2_active_files[ASM2_FILE_BUCKETS];
static struct asm2_file_wrapper *asm2_retired_files;
static uint64_t asm2_file_created_count;
static uint64_t asm2_file_open_count;
static uint64_t asm2_file_closed_count;

struct asm2_stat {
  uint64_t st_dev;
  uint8_t pad0[4];
  uint32_t old_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint8_t pad3[4];
  int64_t st_size __attribute__((aligned(8)));
  uint32_t st_blksize;
  uint64_t st_blocks __attribute__((aligned(8)));
  uint32_t st_atime_sec;
  uint32_t st_atime_nsec;
  uint32_t st_mtime_sec;
  uint32_t st_mtime_nsec;
  uint32_t st_ctime_sec;
  uint32_t st_ctime_nsec;
  uint64_t st_ino;
};

/* Android ARM32 struct statfs: the block and file counters are 64-bit, unlike
 * the 32-bit glibc layout for the same architecture.  Handing the host struct
 * straight to the guest lands f_bfree/f_bavail on the wrong words, so the game
 * reads a nonsense free-space value and refuses to start. */
struct asm2_statfs {
  uint32_t f_type;
  uint32_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint32_t f_fsid[2];
  uint32_t f_namelen;
  uint32_t f_frsize;
  uint32_t f_flags;
  uint32_t f_spare[4];
} __attribute__((packed, aligned(4)));

struct asm2_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
} __attribute__((aligned(8)));

struct asm2_sigaction {
  union {
    void (*handler)(int);
    void (*sigaction)(int, void *, void *);
  } callback;
  uint32_t mask;
  uint32_t flags;
  void (*restorer)(void);
};

_Static_assert(sizeof(struct asm2_stat) == 104,
               "Android ARM/i386 struct stat ABI changed");
_Static_assert(sizeof(struct asm2_statfs) == 84,
               "Android ARM struct statfs ABI changed");
_Static_assert(sizeof(struct asm2_dirent) == 280,
               "Android ARM/i386 struct dirent ABI changed");
_Static_assert(sizeof(struct asm2_sigaction) == 16,
               "Android ARM struct sigaction ABI changed");

static void asm2_file_image_set_flags(unsigned char image[84],
                                      uint16_t flags) {
  memcpy(image + 12, &flags, sizeof(flags));
}

static void asm2_file_image_set_descriptor(unsigned char image[84], int fd) {
  int16_t descriptor = fd >= INT16_MIN && fd <= INT16_MAX ? (int16_t)fd : -1;
  memcpy(image + 14, &descriptor, sizeof(descriptor));
}

static uint16_t asm2_file_mode_flags(const char *mode) {
  if (!mode)
    return 0;
  uint16_t flags = strchr(mode, '+') ? ASM2_FILE_SRW
                   : mode[0] == 'r' ? ASM2_FILE_SRD
                                    : ASM2_FILE_SWR;
  if (strchr(mode, 'a'))
    flags |= ASM2_FILE_SAPP;
  return flags;
}

static unsigned int asm2_file_bucket(const void *stream) {
  uintptr_t value = (uintptr_t)stream;
  value ^= value >> 16;
  value ^= value >> 8;
  return (unsigned int)value & (ASM2_FILE_BUCKETS - 1u);
}

static struct asm2_file_wrapper *asm2_find_active_file_locked(void *stream) {
  for (struct asm2_file_wrapper *wrapper =
           asm2_active_files[asm2_file_bucket(stream)];
       wrapper; wrapper = wrapper->hash_next) {
    if ((void *)wrapper->guest == stream)
      return wrapper;
  }
  return NULL;
}

static struct asm2_file_wrapper *asm2_find_file_wrapper(void *stream) {
  pthread_mutex_lock(&asm2_file_mutex);
  struct asm2_file_wrapper *result = asm2_find_active_file_locked(stream);
  pthread_mutex_unlock(&asm2_file_mutex);
  return result;
}

static FILE *asm2_take_file_for_close(void *stream) {
  FILE *host = NULL;
  pthread_mutex_lock(&asm2_file_mutex);
  const unsigned int bucket = asm2_file_bucket(stream);
  struct asm2_file_wrapper **link = &asm2_active_files[bucket];
  while (*link && (void *)(*link)->guest != stream)
    link = &(*link)->hash_next;
  struct asm2_file_wrapper *wrapper = *link;
  if (!wrapper || wrapper->state != ASM2_FILE_OPEN) {
    errno = EBADF;
  } else {
    wrapper->state = ASM2_FILE_CLOSING;
    while (wrapper->active_operations != 0)
      pthread_cond_wait(&asm2_file_condition, &asm2_file_mutex);
    host = wrapper->host;
    wrapper->host = NULL;
    wrapper->state = ASM2_FILE_CLOSED;
    *link = wrapper->hash_next;
    wrapper->hash_next = NULL;
    wrapper->retired_next = asm2_retired_files;
    asm2_retired_files = wrapper;
    if (asm2_file_open_count != 0)
      --asm2_file_open_count;
    ++asm2_file_closed_count;
  }
  pthread_mutex_unlock(&asm2_file_mutex);
  return host;
}

static void *asm2_wrap_file(FILE *host, const char *mode) {
  if (!host)
    return NULL;
  struct asm2_file_wrapper *wrapper = calloc(1, sizeof(*wrapper));
  if (!wrapper) {
    int saved_errno = errno;
    fclose(host);
    errno = saved_errno ? saved_errno : ENOMEM;
    return NULL;
  }
  wrapper->host = host;
  wrapper->base_flags = asm2_file_mode_flags(mode);
  wrapper->state = ASM2_FILE_OPEN;
  asm2_file_image_set_flags(wrapper->guest, wrapper->base_flags);
  asm2_file_image_set_descriptor(wrapper->guest, fileno(host));
  pthread_mutex_lock(&asm2_file_mutex);
  const unsigned int bucket = asm2_file_bucket(wrapper->guest);
  wrapper->hash_next = asm2_active_files[bucket];
  asm2_active_files[bucket] = wrapper;
  ++asm2_file_created_count;
  ++asm2_file_open_count;
  pthread_mutex_unlock(&asm2_file_mutex);
  return wrapper->guest;
}

static unsigned char *asm2_stream_image(void *stream) {
  uintptr_t address = (uintptr_t)stream;
  uintptr_t base = (uintptr_t)&asm2_bionic_sF[0][0];
  if (address == base)
    return asm2_bionic_sF[0];
  if (address == base + 84)
    return asm2_bionic_sF[1];
  if (address == base + 168)
    return asm2_bionic_sF[2];
  struct asm2_file_wrapper *wrapper = asm2_find_file_wrapper(stream);
  return wrapper ? wrapper->guest : NULL;
}

static void asm2_apply_guest_cleared_flags(void *stream, FILE *host) {
  unsigned char *image = asm2_stream_image(stream);
  if (!image || !host)
    return;
  uint16_t guest_flags = 0;
  memcpy(&guest_flags, image + 12, sizeof(guest_flags));
  if (((guest_flags & ASM2_FILE_SEOF) == 0 && feof(host)) ||
      ((guest_flags & ASM2_FILE_SERR) == 0 && ferror(host)))
    clearerr(host);
}

static FILE *asm2_standard_stream(void *stream) {
  uintptr_t address = (uintptr_t)stream;
  uintptr_t base = (uintptr_t)&asm2_bionic_sF[0][0];
  if (address == base)
    return stdin;
  if (address == base + 84)
    return stdout;
  if (address == base + 168)
    return stderr;
  return NULL;
}

static int asm2_acquire_stream(void *stream,
                               struct asm2_stream_reference *reference) {
  memset(reference, 0, sizeof(*reference));
  FILE *standard = asm2_standard_stream(stream);
  if (standard) {
    reference->host = standard;
    asm2_apply_guest_cleared_flags(stream, standard);
    return 1;
  }

  pthread_mutex_lock(&asm2_file_mutex);
  struct asm2_file_wrapper *wrapper = asm2_find_active_file_locked(stream);
  if (wrapper && wrapper->state == ASM2_FILE_OPEN && wrapper->host) {
    ++wrapper->active_operations;
    reference->host = wrapper->host;
    reference->wrapper = wrapper;
  }
  pthread_mutex_unlock(&asm2_file_mutex);
  if (reference->host) {
    asm2_apply_guest_cleared_flags(stream, reference->host);
    return 1;
  }
  errno = EBADF;
  return 0;
}

static void asm2_release_stream(struct asm2_stream_reference *reference) {
  if (!reference || !reference->wrapper)
    return;
  pthread_mutex_lock(&asm2_file_mutex);
  if (reference->wrapper->active_operations > 0)
    --reference->wrapper->active_operations;
  if (reference->wrapper->state == ASM2_FILE_CLOSING &&
      reference->wrapper->active_operations == 0)
    pthread_cond_broadcast(&asm2_file_condition);
  pthread_mutex_unlock(&asm2_file_mutex);
  reference->host = NULL;
  reference->wrapper = NULL;
}

static void asm2_sync_stream_flags(void *stream, FILE *host) {
  unsigned char *image = asm2_stream_image(stream);
  if (!image || !host)
    return;
  uint16_t flags = 0;
  memcpy(&flags, image + 12, sizeof(flags));
  flags &= (uint16_t)~(ASM2_FILE_SEOF | ASM2_FILE_SERR);
  if (feof(host))
    flags |= ASM2_FILE_SEOF;
  if (ferror(host))
    flags |= ASM2_FILE_SERR;
  asm2_file_image_set_flags(image, flags);
}

void asm2_bionic_init(const char *storage_root) {
  memset(asm2_bionic_sF, 0, sizeof(asm2_bionic_sF));
  asm2_file_image_set_flags(asm2_bionic_sF[0], ASM2_FILE_SRD);
  asm2_file_image_set_descriptor(asm2_bionic_sF[0], STDIN_FILENO);
  asm2_file_image_set_flags(asm2_bionic_sF[1], ASM2_FILE_SWR);
  asm2_file_image_set_descriptor(asm2_bionic_sF[1], STDOUT_FILENO);
  /* Bionic marks stderr writable and unbuffered (_SWR | _SNBF). */
  asm2_file_image_set_flags(asm2_bionic_sF[2], ASM2_FILE_SWR | 0x0002u);
  asm2_file_image_set_descriptor(asm2_bionic_sF[2], STDERR_FILENO);
  memset(asm2_ctype_storage, 0, sizeof(asm2_ctype_storage));
  asm2_tolower_storage[0] = -1;
  asm2_toupper_storage[0] = -1;
  for (int value = 0; value <= UCHAR_MAX; ++value) {
    unsigned char flags = 0;
    if (isupper(value)) flags |= ASM2_CTYPE_U;
    if (islower(value)) flags |= ASM2_CTYPE_L;
    if (isdigit(value)) flags |= ASM2_CTYPE_N;
    if (isspace(value)) flags |= ASM2_CTYPE_S;
    if (ispunct(value)) flags |= ASM2_CTYPE_P;
    if (iscntrl(value)) flags |= ASM2_CTYPE_C;
    if (isxdigit(value) && !isdigit(value)) flags |= ASM2_CTYPE_X;
    if (value == ' ' || value == '\t') flags |= ASM2_CTYPE_B;
    asm2_ctype_storage[value + 1] = (char)flags;
    asm2_tolower_storage[value + 1] = (short)tolower(value);
    asm2_toupper_storage[value + 1] = (short)toupper(value);
  }

  long host_page_size = sysconf(_SC_PAGESIZE);
  if (host_page_size > 0 && host_page_size <= INT_MAX)
    asm2_page_size = (int)host_page_size;

  asm2_storage_root[0] = '\0';
  if (storage_root && storage_root[0]) {
    int written = snprintf(asm2_storage_root, sizeof(asm2_storage_root), "%s",
                           storage_root);
    if (written < 0 || (size_t)written >= sizeof(asm2_storage_root)) {
      debugPrintf("ASM2_BIONIC storage root too long; path bridge disabled\n");
      asm2_storage_root[0] = '\0';
    }
  }
}

void asm2_bionic_get_file_stats(struct asm2_bionic_file_stats *stats) {
  if (!stats)
    return;
  memset(stats, 0, sizeof(*stats));
  pthread_mutex_lock(&asm2_file_mutex);
  stats->created = asm2_file_created_count;
  stats->open = asm2_file_open_count;
  stats->closed = asm2_file_closed_count;
  for (unsigned int bucket = 0; bucket < ASM2_FILE_BUCKETS; ++bucket) {
    uint32_t length = 0;
    for (struct asm2_file_wrapper *wrapper = asm2_active_files[bucket]; wrapper;
         wrapper = wrapper->hash_next)
      ++length;
    if (length > stats->longest_active_bucket)
      stats->longest_active_bucket = length;
  }
  pthread_mutex_unlock(&asm2_file_mutex);
}

const char *asm2_translate_path(const char *path, char *buffer,
                                size_t buffer_size) {
  if (!path || !asm2_storage_root[0])
    return path;

  static const char *const prefixes[] = {
      "/sdcard", "sdcard", "/mnt/sdcard", "/storage/sdcard0",
      "/storage/emulated/0"};
  for (size_t index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]);
       ++index) {
    size_t prefix_length = strlen(prefixes[index]);
    if (strncmp(path, prefixes[index], prefix_length) != 0 ||
        (path[prefix_length] != '\0' && path[prefix_length] != '/'))
      continue;
    int written = snprintf(buffer, buffer_size, "%s%s", asm2_storage_root,
                           path + prefix_length);
    if (written < 0 || (size_t)written >= buffer_size) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    return buffer;
  }
  return path;
}

int *ASM2_GUEST_PCS asm2_errno(void) { return &errno; }

int ASM2_GUEST_PCS asm2_android_log_vprint(int priority, const char *tag,
                                            const char *format, va_list args) {
  int result = fprintf(stderr, "ANDROID_LOG[%d][%s] ", priority,
                       tag ? tag : "");
  int body = vfprintf(stderr, format ? format : "", args);
  fputc('\n', stderr);
  fflush(stderr);
  return body >= 0 ? body : result;
}

int ASM2_GUEST_PCS asm2_android_log_print(int priority, const char *tag,
                                           const char *format, ...) {
  va_list args;
  va_start(args, format);
  int result = asm2_android_log_vprint(priority, tag, format, args);
  va_end(args);
  return result;
}

void ASM2_GUEST_PCS asm2_assert2(const char *file, int line,
                                  const char *function,
                                  const char *expression) {
  debugPrintf("ASM2_BIONIC_ASSERT %s:%d %s: %s\n", file ? file : "?", line,
              function ? function : "?", expression ? expression : "?");
  abort();
}

void ASM2_GUEST_PCS asm2_clearerr(void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return;
  clearerr(reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
}
int ASM2_GUEST_PCS asm2_fclose(void *stream) {
  FILE *standard = asm2_standard_stream(stream);
  if (standard)
    return standard == stdin ? 0 : fflush(standard);
  errno = EBADF;
  FILE *host_stream = asm2_take_file_for_close(stream);
  if (!host_stream) {
    debugPrintf("ASM2_FILE stale close rejected stream=%p\n", stream);
    return EOF;
  }
  return fclose(host_stream);
}
void *ASM2_GUEST_PCS asm2_fdopen(int fd, const char *mode) {
  return asm2_wrap_file(fdopen(fd, mode), mode);
}
int ASM2_GUEST_PCS asm2_fflush(void *stream) {
  if (!stream)
    return fflush(NULL);
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return EOF;
  int result = fflush(reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
char *ASM2_GUEST_PCS asm2_fgets(char *text, int size, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return NULL;
  char *result = fgets(text, size, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
void *ASM2_GUEST_PCS asm2_fopen(const char *path, const char *mode) {
  char translated[PATH_MAX];
  const char *host_path = asm2_translate_path(path, translated,
                                               sizeof(translated));
  FILE *host = host_path ? fopen(host_path, mode) : NULL;
  return asm2_wrap_file(host, mode);
}
int ASM2_GUEST_PCS asm2_fprintf(void *stream, const char *format, ...) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return -1;
  va_list args;
  va_start(args, format);
  int result = vfprintf(reference.host, format, args);
  va_end(args);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_fputc(int character, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return EOF;
  int result = fputc(character, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_fputs(const char *text, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return EOF;
  int result = fputs(text, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
size_t ASM2_GUEST_PCS asm2_fread(void *data, size_t size, size_t count,
                                  void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return 0;
  size_t result = fread(data, size, count, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_fseek(void *stream, long offset, int origin) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return -1;
  int result = fseek(reference.host, offset, origin);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_fseeko(void *stream, off_t offset, int origin) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return -1;
  int result = fseeko(reference.host, offset, origin);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
long ASM2_GUEST_PCS asm2_ftell(void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return -1L;
  long result = ftell(reference.host);
  asm2_release_stream(&reference);
  return result;
}
off_t ASM2_GUEST_PCS asm2_ftello(void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return (off_t)-1;
  off_t result = ftello(reference.host);
  asm2_release_stream(&reference);
  return result;
}
size_t ASM2_GUEST_PCS asm2_fwrite(const void *data, size_t size, size_t count,
                                   void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return 0;
  size_t result = fwrite(data, size, count, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_getc(void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return EOF;
  int result = getc(reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
wint_t ASM2_GUEST_PCS asm2_getwc(void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return WEOF;
  wint_t result = getwc(reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_putc(int character, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return EOF;
  int result = putc(character, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
wint_t ASM2_GUEST_PCS asm2_putwc(wchar_t character, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return WEOF;
  wint_t result = putwc(character, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_setvbuf(void *stream, char *buffer, int mode,
                                 size_t size) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return -1;
  int result = setvbuf(reference.host, buffer, mode, size);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_ungetc(int character, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return EOF;
  int result = ungetc(character, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
wint_t ASM2_GUEST_PCS asm2_ungetwc(wint_t character, void *stream) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return WEOF;
  wint_t result = ungetwc(character, reference.host);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}
int ASM2_GUEST_PCS asm2_vfprintf(void *stream, const char *format,
                                  va_list args) {
  struct asm2_stream_reference reference;
  if (!asm2_acquire_stream(stream, &reference))
    return -1;
  int result = vfprintf(reference.host, format, args);
  asm2_sync_stream_flags(stream, reference.host);
  asm2_release_stream(&reference);
  return result;
}

static const char *path_for_host(const char *path, char translated[PATH_MAX]) {
  return asm2_translate_path(path, translated, PATH_MAX);
}

int ASM2_GUEST_PCS asm2_access(const char *path, int mode) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? access(host_path, mode) : -1;
}
int ASM2_GUEST_PCS asm2_chdir(const char *path) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? chdir(host_path) : -1;
}
int ASM2_GUEST_PCS asm2_chmod(const char *path, mode_t mode) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? chmod(host_path, mode) : -1;
}
char *ASM2_GUEST_PCS asm2_getcwd(char *buffer, size_t size) {
  char host_path[PATH_MAX];
  if (!getcwd(host_path, sizeof(host_path)))
    return NULL;
  size_t root_length = strlen(asm2_storage_root);
  const char *result = host_path;
  char virtual_path[PATH_MAX];
  if (root_length && strncmp(host_path, asm2_storage_root, root_length) == 0 &&
      (host_path[root_length] == '\0' || host_path[root_length] == '/')) {
    int written = snprintf(virtual_path, sizeof(virtual_path), "/sdcard%s",
                           host_path + root_length);
    if (written < 0 || (size_t)written >= sizeof(virtual_path)) {
      errno = ERANGE;
      return NULL;
    }
    result = virtual_path;
  }
  size_t needed = strlen(result) + 1;
  if (!buffer) {
    if (size != 0) {
      errno = EINVAL;
      return NULL;
    }
    return strdup(result);
  }
  if (needed > size) {
    errno = ERANGE;
    return NULL;
  }
  memcpy(buffer, result, needed);
  return buffer;
}
int ASM2_GUEST_PCS asm2_mkdir(const char *path, mode_t mode) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? mkdir(host_path, mode) : -1;
}
int ASM2_GUEST_PCS asm2_mkstemp(char *path_template) {
  if (!path_template) {
    errno = EINVAL;
    return -1;
  }
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path_template, translated);
  if (!host_path)
    return -1;
  if (host_path == path_template)
    return mkstemp(path_template);
  int fd = mkstemp(translated);
  if (fd >= 0) {
    size_t guest_length = strlen(path_template);
    size_t host_length = strlen(translated);
    if (guest_length >= 6 && host_length >= 6)
      memcpy(path_template + guest_length - 6, translated + host_length - 6, 6);
  }
  return fd;
}
int ASM2_GUEST_PCS asm2_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
  }
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  if (!host_path)
    return -1;
  return (flags & O_CREAT) ? open(host_path, flags, mode)
                           : open(host_path, flags);
}
void *ASM2_GUEST_PCS asm2_opendir(const char *path) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? opendir(host_path) : NULL;
}
int ASM2_GUEST_PCS asm2_remove(const char *path) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? remove(host_path) : -1;
}
int ASM2_GUEST_PCS asm2_rename(const char *old_path, const char *new_path) {
  char old_translated[PATH_MAX];
  char new_translated[PATH_MAX];
  const char *host_old = path_for_host(old_path, old_translated);
  const char *host_new = path_for_host(new_path, new_translated);
  return host_old && host_new ? rename(host_old, host_new) : -1;
}
int ASM2_GUEST_PCS asm2_rmdir(const char *path) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? rmdir(host_path) : -1;
}

static void stat_to_android(const struct stat *host, struct asm2_stat *guest) {
  memset(guest, 0, sizeof(*guest));
  guest->st_dev = host->st_dev;
  guest->old_ino = (uint32_t)host->st_ino;
  guest->st_mode = host->st_mode;
  guest->st_nlink = host->st_nlink;
  guest->st_uid = host->st_uid;
  guest->st_gid = host->st_gid;
  guest->st_rdev = host->st_rdev;
  guest->st_size = host->st_size;
  guest->st_blksize = host->st_blksize;
  guest->st_blocks = host->st_blocks;
  guest->st_atime_sec = host->st_atim.tv_sec;
  guest->st_atime_nsec = host->st_atim.tv_nsec;
  guest->st_mtime_sec = host->st_mtim.tv_sec;
  guest->st_mtime_nsec = host->st_mtim.tv_nsec;
  guest->st_ctime_sec = host->st_ctim.tv_sec;
  guest->st_ctime_nsec = host->st_ctim.tv_nsec;
  guest->st_ino = host->st_ino;
}

int ASM2_GUEST_PCS asm2_stat(const char *path, void *guest_stat) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  struct stat host;
  int result = host_path ? stat(host_path, &host) : -1;
  if (result == 0 && guest_stat)
    stat_to_android(&host, guest_stat);
  return result;
}
int ASM2_GUEST_PCS asm2_fstat(int fd, void *guest_stat) {
  struct stat host;
  int result = fstat(fd, &host);
  if (result == 0 && guest_stat)
    stat_to_android(&host, guest_stat);
  return result;
}
int ASM2_GUEST_PCS asm2_statfs(const char *path, void *guest_statfs) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  const int debug = getenv("ASM2_FS_DEBUG") != NULL;
  if (!host_path) {
    if (debug)
      debugPrintf("ASM2_STATFS untranslated guest_path=%s\n",
                  path ? path : "?");
    return -1;
  }
  struct statfs64 host;
  int result = statfs64(host_path, &host);
  if (result != 0 || !guest_statfs) {
    if (debug)
      debugPrintf("ASM2_STATFS failed guest_path=%s host_path=%s errno=%d\n",
                  path ? path : "?", host_path, errno);
    return result;
  }

  struct asm2_statfs *guest = guest_statfs;
  memset(guest, 0, sizeof(*guest));
  guest->f_type = (uint32_t)host.f_type;
  guest->f_bsize = (uint32_t)host.f_bsize;
  guest->f_blocks = host.f_blocks;
  guest->f_bfree = host.f_bfree;
  guest->f_bavail = host.f_bavail;
  guest->f_files = host.f_files;
  guest->f_ffree = host.f_ffree;
  memcpy(guest->f_fsid, &host.f_fsid, sizeof(guest->f_fsid));
  guest->f_namelen = (uint32_t)host.f_namelen;
  guest->f_frsize = (uint32_t)host.f_frsize;
  guest->f_flags = (uint32_t)host.f_flags;
  if (debug)
    debugPrintf("ASM2_STATFS guest_path=%s host_path=%s bsize=%u bavail=%llu "
                "free_mb=%llu\n",
                path ? path : "?", host_path, guest->f_bsize,
                (unsigned long long)guest->f_bavail,
                (unsigned long long)(guest->f_bavail *
                                     (uint64_t)guest->f_bsize /
                                     (1024u * 1024u)));
  return 0;
}
int ASM2_GUEST_PCS asm2_unlink(const char *path) {
  char translated[PATH_MAX];
  const char *host_path = path_for_host(path, translated);
  return host_path ? unlink(host_path) : -1;
}

void *ASM2_GUEST_PCS asm2_readdir(void *directory) {
  static __thread struct asm2_dirent guest;
  struct dirent *host = readdir((DIR *)directory);
  if (!host)
    return NULL;
  memset(&guest, 0, sizeof(guest));
  guest.d_ino = host->d_ino;
  guest.d_off = host->d_off;
  guest.d_reclen = sizeof(guest);
  guest.d_type = host->d_type;
  snprintf(guest.d_name, sizeof(guest.d_name), "%s", host->d_name);
  return &guest;
}

int ASM2_GUEST_PCS asm2_sigaction(int signal_number, const void *guest_action,
                                   void *guest_old_action) {
  const struct asm2_sigaction *input = guest_action;
  struct asm2_sigaction *output = guest_old_action;
  struct sigaction host_input;
  struct sigaction host_output;
  struct sigaction *host_input_ptr = NULL;
  struct sigaction *host_output_ptr = output ? &host_output : NULL;

  if (input) {
    memset(&host_input, 0, sizeof(host_input));
    if (input->flags & SA_SIGINFO)
      host_input.sa_sigaction =
          (void (*)(int, siginfo_t *, void *))input->callback.sigaction;
    else
      host_input.sa_handler = input->callback.handler;
    sigemptyset(&host_input.sa_mask);
    for (int signal_index = 1; signal_index <= 32; ++signal_index) {
      if (input->mask & (1u << (signal_index - 1)))
        sigaddset(&host_input.sa_mask, signal_index);
    }
    host_input.sa_flags = input->flags;
    host_input_ptr = &host_input;
  }

  int result = sigaction(signal_number, host_input_ptr, host_output_ptr);
  if (result == 0 && output) {
    memset(output, 0, sizeof(*output));
    if (host_output.sa_flags & SA_SIGINFO)
      output->callback.sigaction =
          (void (*)(int, void *, void *))host_output.sa_sigaction;
    else
      output->callback.handler = host_output.sa_handler;
    for (int signal_index = 1; signal_index <= 32; ++signal_index) {
      if (sigismember(&host_output.sa_mask, signal_index) == 1)
        output->mask |= 1u << (signal_index - 1);
    }
    output->flags = host_output.sa_flags;
  }
  return result;
}

int ASM2_GUEST_PCS asm2_strerror_r(int error_number, char *buffer,
                                    size_t buffer_size) {
  if (!buffer || buffer_size == 0)
    return ERANGE;
  const char *message = strerror(error_number);
  size_t length = strlen(message);
  if (length >= buffer_size) {
    memcpy(buffer, message, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    return ERANGE;
  }
  memcpy(buffer, message, length + 1);
  return 0;
}

long ASM2_GUEST_PCS asm2_sysconf(int android_name) {
  /* Old Bionic assigned PAGE_SIZE the value 0x28; glibc's enum is different.
   * This is the only sysconf selector emitted by the 1.2.7d ELF. */
  if (android_name == 0x27 || android_name == 0x28)
    return asm2_page_size;
  errno = ENOSYS;
  return -1;
}
