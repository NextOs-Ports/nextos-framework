/* libc_shim.c -- bionic-compatible libc wrappers (Linux/glibc port)
 *
 * Where the bionic and glibc ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c. Android-absolute paths are collapsed
 * onto the game directory by fix_path().
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <time.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name; value[0] = '\0'; return 0;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) { return (int)syscall(SYS_gettid); }

#define ARM64_SYS_GETTID 178
long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
int pthread_setname_np_fake(void *thread, const char *name) { (void)thread; (void)name; return 0; }
int pthread_setschedparam_fake(void *thread, int policy, const void *param) { (void)thread; (void)policy; (void)param; return 0; }
void android_set_abort_message_fake(const char *msg) { debugPrintf("abort message: %s\n", msg ? msg : "(null)"); }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98
long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 4;
    case BIONIC_SC_PHYS_PAGES: return (1ll * 1024 * 1024 * 1024) / 0x1000;
    default: debugPrintf("libc: sysconf(%d) -> -1\n", name); return -1;
  }
}

// ---------------------------------------------------------------------------
// path remapping: collapse Android-absolute prefixes onto the game dir (cwd)
// ---------------------------------------------------------------------------

const char *fix_path(const char *path) {
  static _Thread_local char buf[2][1024];
  static _Thread_local int which = 0;

  if (!path || path[0] != '/')
    return path;

  static const char *const prefixes[] = { WRITE_PATH, CACHE_PATH, SAVE_PATH };
  for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    const char *rest = strstr(path, prefixes[i]);
    if (rest) {
      rest += strlen(prefixes[i]);
      if (*rest == '/') rest++;
      char *out = buf[which]; which ^= 1;
      if (*rest) snprintf(out, sizeof(buf[0]), "./%s", rest);
      else       snprintf(out, sizeof(buf[0]), ".");
      return out;
    }
  }
  {
    const char *r = strstr(path, "/" PACKAGE "/");
    if (r) {
      r += strlen("/" PACKAGE "/");
      char *out = buf[which]; which ^= 1;
      snprintf(out, sizeof(buf[0]), "./%s", r);
      return out;
    }
  }
  {
    const char *r = strstr(path, "/TTGames/");
    if (r) {
      char *out = buf[which]; which ^= 1;
      snprintf(out, sizeof(buf[0]), ".%s", r);
      return out;
    }
  }
  return path;
}

/* Android's emulated external storage is case-insensitive, while the NextOS
 * /storage filesystem is ext4.  Odyssey writes some transient resources with
 * an uppercase ResRef (for example AVAILNPC0.utc) and later asks SDL for the
 * lowercase spelling.  Resolve each component case-insensitively only after
 * the exact open has failed; callers keep exact-path semantics for writes. */
int resolve_case_path(const char *path, char *resolved, size_t resolved_size) {
  if (!path || !resolved || resolved_size < 2) {
    errno = EINVAL;
    return 0;
  }

  char normalized[1024];
  snprintf(normalized, sizeof(normalized), "%s", fix_path(path));
  for (char *p = normalized; *p; p++)
    if (*p == '\\') *p = '/';

  char current[1024];
  snprintf(current, sizeof(current), "%s", normalized[0] == '/' ? "/" : ".");

  char *cursor = normalized;
  if (*cursor == '/') cursor++;
  while (*cursor) {
    while (*cursor == '/') cursor++;
    if (!*cursor) break;

    char *end = strchr(cursor, '/');
    if (end) *end = '\0';

    if (strcmp(cursor, ".") == 0) {
      /* already represented by the relative-path root */
    } else if (strcmp(cursor, "..") == 0) {
      size_t used = strlen(current);
      if (used + 3 >= sizeof(current)) {
        errno = ENAMETOOLONG;
        return 0;
      }
      snprintf(current + used, sizeof(current) - used, "/..");
    } else {
      char candidate[1024];
      if (snprintf(candidate, sizeof(candidate), "%s%s%s", current,
                   strcmp(current, "/") == 0 ? "" : "/", cursor) >=
          (int)sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return 0;
      }

      struct stat st;
      const char *actual = cursor;
      char matched[256];
      if (lstat(candidate, &st) != 0) {
        DIR *directory = opendir(current);
        if (!directory) return 0;

        int found = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
          if (strcasecmp(entry->d_name, cursor) == 0) {
            snprintf(matched, sizeof(matched), "%s", entry->d_name);
            actual = matched;
            found = 1;
            break;
          }
        }
        closedir(directory);
        if (!found) {
          errno = ENOENT;
          return 0;
        }
      }

      size_t used = strlen(current);
      const char *separator = strcmp(current, "/") == 0 ? "" : "/";
      if (snprintf(current + used, sizeof(current) - used, "%s%s",
                   separator, actual) >= (int)(sizeof(current) - used)) {
        errno = ENAMETOOLONG;
        return 0;
      }
    }

    if (!end) break;
    cursor = end + 1;
  }

  if (snprintf(resolved, resolved_size, "%s", current) >=
      (int)resolved_size) {
    errno = ENAMETOOLONG;
    return 0;
  }
  return 1;
}

static int trace_party_resource_path(const char *path) {
  return path &&
         (strcasestr(path, "gameinprogress") ||
          strcasestr(path, "availnpc"));
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> glibc; identical on aarch64 Linux)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  const char *fixed = fix_path(path);
  const int fd = open(fixed, convert_open_flags(flags), mode);
  if (trace_party_resource_path(path))
    debugPrintf("PARTYFS open(%s => %s, flags=0x%x) -> %d errno=%d\n",
                path, fixed, flags, fd, fd < 0 ? errno : 0);
  return fd;
}
int open2_fake(const char *path, int flags) {
  const char *fixed = fix_path(path);
  const int fd = open(fixed, convert_open_flags(flags), 0666);
  if (trace_party_resource_path(path))
    debugPrintf("PARTYFS open2(%s => %s, flags=0x%x) -> %d errno=%d\n",
                path, fixed, flags, fd, fd < 0 ? errno : 0);
  return fd;
}
int mkdir_fake(const char *path, unsigned int mode) { return mkdir(fix_path(path), mode); }
int remove_fake(const char *path) { return remove(fix_path(path)); }
int rename_fake(const char *from, const char *to) {
  const char *f = fix_path(from);
  const char *t = fix_path(to);
  remove(t);
  return rename(f, t);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic 32-bit ARM layout = kernel stat64, 104 bytes)
// ---------------------------------------------------------------------------
// CRITICAL: this MUST be the arm32 layout, not aarch64. The game reserves a
// 104-byte (0x68) stack frame for its statbuf; writing the 128-byte aarch64
// struct here overflows it by 24 bytes and corrupts the saved return address
// (observed as a jump to pc=0 inside fnOBBPackages_AddFile).

struct bionic_stat {
  uint64_t st_dev;
  uint8_t  __pad0[4];
  uint32_t __st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint8_t  __pad3[4];
  int64_t  st_size;
  uint32_t st_blksize;
  uint64_t st_blocks;
  // named without the st_ prefix: glibc #defines st_atime/st_mtime/st_ctime to
  // st_atim.tv_sec, which would rewrite these field names.
  uint32_t a_time, a_time_ns;
  uint32_t m_time, m_time_ns;
  uint32_t c_time, c_time_ns;
  uint64_t st_ino;
};
_Static_assert(sizeof(struct bionic_stat) == 104, "arm32 bionic stat must be 104 bytes");

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->__st_ino = (uint32_t)in->st_ino;
  out->st_mode = in->st_mode; out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size;
  out->st_blksize = in->st_blksize; out->st_blocks = in->st_blocks;
  out->a_time = in->st_atime; out->m_time = in->st_mtime; out->c_time = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  const char *fixed = fix_path(path);
  const int ret = stat(fixed, &real);
  if (ret == 0) convert_stat(&real, st);
  if (trace_party_resource_path(path))
    debugPrintf("PARTYFS stat(%s => %s) -> %d size=%lld errno=%d\n",
                path, fixed, ret,
                ret == 0 ? (long long)real.st_size : (long long)-1,
                ret < 0 ? errno : 0);
  return ret;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// ---------------------------------------------------------------------------
// directory ABI + Win32 enumeration used by Odyssey's resource manager
// ---------------------------------------------------------------------------

/* Android ARM LP32 uses the kernel dirent64 layout even though plain armhf
 * glibc's struct dirent has 32-bit ino/off fields.  In particular, the guest
 * reads d_name at +19. */
struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};
_Static_assert(offsetof(struct bionic_dirent, d_name) == 19,
               "arm32 bionic dirent name must be at +19");
_Static_assert(sizeof(struct bionic_dirent) == 280,
               "arm32 bionic dirent must be 280 bytes");

void *opendir_fake(const char *path) {
  if (!path) {
    errno = EINVAL;
    return NULL;
  }

  char normalized[1024];
  snprintf(normalized, sizeof(normalized), "%s", fix_path(path));
  for (char *p = normalized; *p; p++)
    if (*p == '\\') *p = '/';

  const char *host_path = normalized;
  char drive_path[1024];
  if (isalpha((unsigned char)normalized[0]) && normalized[1] == ':') {
    const char *rest = normalized + 2;
    while (*rest == '/') rest++;
    snprintf(drive_path, sizeof(drive_path), "./%s", rest);
    host_path = drive_path;
  }

  DIR *directory = opendir(host_path);
  if (trace_party_resource_path(path))
    debugPrintf("PARTYFS opendir(%s => %s) -> %p errno=%d\n",
                path, host_path, directory, directory ? 0 : errno);
  return directory;
}

void *readdir_fake(void *dirp) {
  static _Thread_local struct bionic_dirent out;
  struct dirent *in = readdir((DIR *)dirp);
  if (!in) return NULL;

  memset(&out, 0, sizeof(out));
  out.d_ino = (uint64_t)in->d_ino;
  out.d_off = (int64_t)in->d_off;
  out.d_type = in->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", in->d_name);
  if (strcasestr(out.d_name, "availnpc"))
    debugPrintf("PARTYFS readdir -> %s\n", out.d_name);
  size_t record_size = offsetof(struct bionic_dirent, d_name) +
                       strlen(out.d_name) + 1;
  record_size = (record_size + 7u) & ~7u;
  if (record_size > sizeof(out)) record_size = sizeof(out);
  out.d_reclen = (uint16_t)record_size;
  return &out;
}

typedef struct {
  uint32_t low;
  uint32_t high;
} WinFileTime;

struct win32_find_data_a {
  uint32_t attributes;
  WinFileTime creation_time;
  WinFileTime access_time;
  WinFileTime write_time;
  uint32_t size_high;
  uint32_t size_low;
  uint32_t reserved0;
  uint32_t reserved1;
  char filename[260];
  char alternate_filename[14];
};
_Static_assert(offsetof(struct win32_find_data_a, filename) == 44,
               "WIN32_FIND_DATAA filename must be at +44");
_Static_assert(sizeof(struct win32_find_data_a) == 320,
               "WIN32_FIND_DATAA must be 320 bytes");

typedef struct {
  DIR *directory;
  char directory_path[1024];
  char wildcard[260];
} WinFindHandle;

/* O engine encerra a enumeracao com o par FindNextFileA()==0 +
 * GetLastError()==ERROR_NO_MORE_FILES.  O GetLastError que o libKOTOR importa
 * e o do layer Win32 da Aspyr (libandroid_port), entao os fakes escrevem no
 * last-error DELE via SetLastError.  Sem isso o engine repete FindNextFileA
 * para sempre — era o freeze do segundo Quick Save (o primeiro passa porque a
 * pasta saves/ vazia falha ja no FindFirstFileA). */
#define WIN32_ERROR_SUCCESS        0ul
#define WIN32_ERROR_FILE_NOT_FOUND 2ul
#define WIN32_ERROR_NO_MORE_FILES  18ul

static void (*win32_set_last_error)(unsigned long);

void kotor_set_win32_set_last_error(void *fn) {
  win32_set_last_error = fn;
}

static void set_win32_last_error(unsigned long code) {
  if (win32_set_last_error)
    win32_set_last_error(code);
}

static int wildcard_match_ci(const char *pattern, const char *name) {
  const char *star = NULL;
  const char *retry = NULL;
  while (*name) {
    if (*pattern == '?' ||
        (*pattern && tolower((unsigned char)*pattern) ==
                     tolower((unsigned char)*name))) {
      pattern++;
      name++;
    } else if (*pattern == '*') {
      star = pattern++;
      retry = name;
    } else if (star) {
      pattern = star + 1;
      name = ++retry;
    } else {
      return 0;
    }
  }
  while (*pattern == '*') pattern++;
  return *pattern == '\0';
}

static void unix_time_to_filetime(time_t unix_time, WinFileTime *out) {
  const uint64_t windows_epoch = UINT64_C(11644473600);
  uint64_t ticks = 0;
  if (unix_time >= -(time_t)windows_epoch)
    ticks = ((uint64_t)((int64_t)unix_time + (int64_t)windows_epoch)) *
            UINT64_C(10000000);
  out->low = (uint32_t)ticks;
  out->high = (uint32_t)(ticks >> 32);
}

static int find_next_file(WinFindHandle *handle,
                          struct win32_find_data_a *out) {
  struct dirent *entry;
  while ((entry = readdir(handle->directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (!wildcard_match_ci(handle->wildcard, entry->d_name))
      continue;

    char full_path[sizeof(handle->directory_path) + 260];
    snprintf(full_path, sizeof(full_path), "%s/%s",
             handle->directory_path, entry->d_name);
    struct stat st;
    if (stat(full_path, &st) != 0)
      continue;

    memset(out, 0, sizeof(*out));
    out->attributes = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
    unix_time_to_filetime(st.st_ctime, &out->creation_time);
    unix_time_to_filetime(st.st_atime, &out->access_time);
    unix_time_to_filetime(st.st_mtime, &out->write_time);
    const uint64_t size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    out->size_low = (uint32_t)size;
    out->size_high = (uint32_t)(size >> 32);
    snprintf(out->filename, sizeof(out->filename), "%s", entry->d_name);
    return 1;
  }
  return 0;
}

void *FindFirstFileA_fake(const char *pattern, void *find_data) {
  if (!pattern || !find_data) return NULL;

  char normalized[1280];
  snprintf(normalized, sizeof(normalized), "%s", fix_path(pattern));
  for (char *p = normalized; *p; p++)
    if (*p == '\\') *p = '/';

  char *slash = strrchr(normalized, '/');
  const char *wildcard;
  char directory_path[1024];
  if (slash) {
    *slash = '\0';
    snprintf(directory_path, sizeof(directory_path), "%s",
             normalized[0] ? normalized : "/");
    wildcard = slash + 1;
  } else {
    snprintf(directory_path, sizeof(directory_path), ".");
    wildcard = normalized;
  }
  if (strcmp(wildcard, "*.*") == 0)
    wildcard = "*";

  WinFindHandle *handle = calloc(1, sizeof(*handle));
  if (!handle) return NULL;
  snprintf(handle->directory_path, sizeof(handle->directory_path), "%s",
           directory_path);
  snprintf(handle->wildcard, sizeof(handle->wildcard), "%s", wildcard);
  handle->directory = opendir(handle->directory_path);
  if (!handle->directory ||
      !find_next_file(handle, (struct win32_find_data_a *)find_data)) {
    if (trace_party_resource_path(pattern))
      debugPrintf("FindFirstFileA(%s => %s/%s) FAILED errno=%d\n",
                  pattern, handle->directory_path, handle->wildcard, errno);
    if (handle->directory) closedir(handle->directory);
    free(handle);
    set_win32_last_error(WIN32_ERROR_FILE_NOT_FOUND);
    return NULL;
  }
  if (getenv("KOTOR_ASSET_TRACE") || trace_party_resource_path(pattern))
    debugPrintf("FindFirstFileA(%s) -> %s\n", pattern,
                ((struct win32_find_data_a *)find_data)->filename);
  set_win32_last_error(WIN32_ERROR_SUCCESS);
  return handle;
}

int FindNextFileA_fake(void *opaque, void *find_data) {
  if (!opaque || !find_data) return 0;
  WinFindHandle *handle = opaque;
  const int found =
      find_next_file(handle, (struct win32_find_data_a *)find_data);
  set_win32_last_error(found ? WIN32_ERROR_SUCCESS
                             : WIN32_ERROR_NO_MORE_FILES);
  if (found && (getenv("KOTOR_ASSET_TRACE") ||
                trace_party_resource_path(handle->directory_path)))
    debugPrintf("FindNextFileA -> %s\n",
                ((struct win32_find_data_a *)find_data)->filename);
  return found;
}

int FindClose_fake(void *opaque) {
  if (!opaque) return 0;
  WinFindHandle *handle = opaque;
  if (handle->directory) closedir(handle->directory);
  free(handle);
  return 1;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p; return 0;
}

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr)
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; }
  return fputs(s, f);
}
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0; return fclose(f); }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int fileno_fake(FILE *f) {
  if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
    char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
  }
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }

// ---------------------------------------------------------------------------
// AAsset emulation + game-archive fopen
// ---------------------------------------------------------------------------

typedef struct { FILE *f; long size; } Asset;

void *AAssetManager_fromJava_fake(void *env, void *mgr) { (void)env; (void)mgr; return (void *)1; }

static void mkdir_p(const char *filepath) {
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *slash = strrchr(tmp, '/');
  if (!slash) return;
  *slash = '\0';
  for (char *q = tmp + 1; *q; q++) {
    if (*q == '/') { *q = '\0'; mkdir(tmp, 0777); *q = '/'; }
  }
  mkdir(tmp, 0777);
}

FILE *fopen_fake(const char *path, const char *mode) {
  const char *p = fix_path(path);
  FILE *f = fopen(p, mode);
  if (!f && (strchr(mode, 'w') || strchr(mode, 'a'))) {
    mkdir_p(p);
    f = fopen(p, mode);
  }
  if (f && strchr(mode, 'r')) {
    const char *ext = strrchr(p, '.');
    if (ext && (strcasecmp(ext, ".fib") == 0 || strcasecmp(ext, ".dat") == 0))
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  // Make save I/O observable: log every open of savegame.dat/config.dat with the
  // resulting size so a Continue/load can be confirmed from the log.
  if (f && strstr(p, "savegame.dat")) {
    long cur = ftell(f); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, cur, SEEK_SET);
    debugPrintf("SAVE: fopen savegame.dat mode=%s -> OK size=%ld\n", mode, sz);
  } else if (f && strstr(p, "config.dat")) {
    debugPrintf("SAVE: fopen config.dat mode=%s -> OK\n", mode);
  }
  if (trace_party_resource_path(path))
    debugPrintf("PARTYFS fopen(%s => %s, %s) -> %s errno=%d\n",
                path, p, mode, f ? "OK" : "FAILED", f ? 0 : errno);
  else if (!f)
    debugPrintf("fopen(%s => %s, %s) FAILED\n", path, p, mode);
  else if (getenv("KOTOR_ASSET_TRACE"))
    debugPrintf("fopen(%s => %s, %s) OK\n", path, p, mode);
  return f;
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", GAMEDATA_DIR, path);
  FILE *f = fopen(full, "rb");
  if (!f) { snprintf(full, sizeof(full), "assets/%s", path); f = fopen(full, "rb"); }
  debugPrintf("AAsset: open(%s) -> %s\n", path, f ? "ok" : "MISSING");
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, 16 * 1024);
  Asset *a = calloc(1, sizeof(*a));
  a->f = f;
  fseek(f, 0, SEEK_END); a->size = ftell(f); fseek(f, 0, SEEK_SET);
  return a;
}

int AAsset_openFileDescriptor_fake(void *asset, long *outStart, long *outLength) {
  Asset *a = asset;
  if (!a) return -1;
  fflush(a->f);
  int fd = dup(fileno(a->f));
  if (fd < 0) return -1;
  if (outStart)  *outStart = 0;
  if (outLength) *outLength = a->size;
  return fd;
}
void AAsset_close_fake(void *asset) { Asset *a = asset; if (a) { fclose(a->f); free(a); } }
int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset; return a ? (int)fread(buf, 1, count, a->f) : -1;
}
long AAsset_seek_fake(void *asset, long off, int whence) {
  Asset *a = asset;
  if (!a || fseek(a->f, off, whence) < 0) return -1;
  return ftell(a->f);
}
long AAsset_getLength_fake(void *asset) { Asset *a = asset; return a ? a->size : 0; }
long AAsset_getRemainingLength_fake(void *asset) { Asset *a = asset; return a ? a->size - ftell(a->f) : 0; }

// ---------------------------------------------------------------------------
// ANativeWindow: the wrapper owns the real window (SDL); hand back dummy tokens
// ---------------------------------------------------------------------------

void *ANativeWindow_fromSurface_fake(void *env, void *surface) { (void)env; (void)surface; return (void *)0x414e5731; }
int ANativeWindow_getWidth_fake(void *win) { (void)win; return screen_width; }
int ANativeWindow_getHeight_fake(void *win) { (void)win; return screen_height; }
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)win; (void)format;
  debugPrintf("ANativeWindow_setBuffersGeometry(%d, %d)\n", w, h);
  return 0;
}

// ---------------------------------------------------------------------------
// semaphores via pointer indirection (POSIX sem_t behind the bionic storage)
// ---------------------------------------------------------------------------

int sem_init_fake(void **s, int pshared, unsigned int value) {
  sem_t *real = malloc(sizeof(sem_t));
  sem_init(real, pshared, value);
  *s = real;
  return 0;
}
int sem_destroy_fake(void **s) {
  if (s && *s) { sem_destroy((sem_t *)*s); free(*s); *s = NULL; }
  return 0;
}
int sem_post_fake(void **s) { if (s && *s) sem_post((sem_t *)*s); return 0; }
int sem_wait_fake(void **s) {
  // never block while holding the single GL context
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  if (s && *s) sem_wait((sem_t *)*s);
  return 0;
}
int sem_trywait_fake(void **s) {
  if (s && *s && sem_trywait((sem_t *)*s) == 0) return 0;
  errno = EAGAIN; return -1;
}
