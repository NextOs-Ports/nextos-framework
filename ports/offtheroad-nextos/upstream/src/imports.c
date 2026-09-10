#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#include "imports.h"
#include "so_util.h"
#include "opensles_shim.h"
#include "util.h"

#undef feof
#undef ferror

#undef feof
#undef ferror
static uint8_t fake_sF[3][0x100];
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

static char _ctype_table[1 + 256];
static short _toupper_table[1 + 256];
static short _tolower_table[1 + 256];
const char *_ctype_ = _ctype_table;
const short *_toupper_tab_ = _toupper_table;
const short *_tolower_tab_ = _tolower_table;
__attribute__((constructor)) static void ctype_init(void) {
  _ctype_table[0] = 0;
  _toupper_table[0] = -1;
  _tolower_table[0] = -1;
  for (int c = 0; c < 256; c++) {
    unsigned v = 0;
    if (isupper(c)) v |= 0x01;
    if (islower(c)) v |= 0x02;
    if (isdigit(c)) v |= 0x04;
    if (isspace(c)) v |= 0x08;
    if (ispunct(c)) v |= 0x10;
    if (iscntrl(c)) v |= 0x20;
    if (isxdigit(c)) v |= 0x40;
    if (c == ' ') v |= 0x80;
    _ctype_table[c + 1] = (char)v;
    _toupper_table[c + 1] = (short)toupper(c);
    _tolower_table[c + 1] = (short)tolower(c);
  }
}

/* A engine usa stdio BIONIC: stdin/out/err == &__sF[0/1/2] (mapeado p/ fake_sF).
 * Nossas fopen/fread/... sao do glibc e rejeitam um FILE* fake ("invalid stdio
 * handle" -> abort). Traduz qualquer ponteiro dentro de fake_sF p/ o stream
 * glibc real (proven SOTN). */
static FILE *map_sf(void *f) {
  uintptr_t p = (uintptr_t)f, base = (uintptr_t)fake_sF;
  if (p >= base && p < base + sizeof(fake_sF)) {
    int idx = (int)((p - base) / 0x100);
    return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
  }
  return (FILE *)f;
}
static int sf_fprintf(void *f, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vfprintf(map_sf(f), fmt, ap); va_end(ap); return r;
}
static int sf_vfprintf(void *f, const char *fmt, va_list ap) { return vfprintf(map_sf(f), fmt, ap); }
static int sf_fputc(int c, void *f) { return fputc(c, map_sf(f)); }
static int sf_fputs(const char *s, void *f) { return fputs(s, map_sf(f)); }
static size_t sf_fwrite(const void *p, size_t sz, size_t n, void *f) { return fwrite(p, sz, n, map_sf(f)); }
static int sf_fflush(void *f) { return fflush(f ? map_sf(f) : NULL); }
static int sf_ferror(void *f) { return ferror(map_sf(f)); }
static int sf_feof(void *f) { return feof(map_sf(f)); }
static int sf_fileno(void *f) { return fileno(map_sf(f)); }
static int sf_fgetc(void *f) { return fgetc(map_sf(f)); }
static char *sf_fgets(char *s, int n, void *f) { return fgets(s, n, map_sf(f)); }
extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;

static void __stack_chk_fail_stub(void) {
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  uintptr_t g = tls ? *(uintptr_t *)(tls + 0x28) : 0;
  void *ra = __builtin_return_address(0);
  debugPrintf("__stack_chk_fail! caller=%p (libMyGame+0x%lx) tls+0x28=0x%lx\n",
              ra, (unsigned long)((uintptr_t)ra - (uintptr_t)text_base),
              (unsigned long)g);
}

static int *__errno_fake(void) { return &errno; }

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("LOG [%s]: %s\n", tag, string);
  return 0;
}

int __android_log_write_fake(int prio, const char *tag, const char *text) {
  debugPrintf("LOG [%s]: %s\n", tag, text);
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memmove(dst, src, n);
}

char *__strcat_chk(char *dst, const char *src, size_t dst_buf_size) {
  (void)dst_buf_size;
  return strcat(dst, src);
}

char *__strcpy_chk(char *dst, const char *src, size_t dst_len) {
  (void)dst_len;
  return strcpy(dst, src);
}

size_t __strlen_chk(const char *s, size_t max_len) {
  (void)max_len;
  return strlen(s);
}

char *__strrchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strrchr(s, c);
}

char *__strchr_chk(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len,
                     size_t src_len) {
  (void)dst_len;
  (void)src_len;
  return strncpy(dst, src, n);
}

int __vsprintf_chk(char *dst, int flags, size_t dst_len, const char *fmt,
                   va_list ap) {
  (void)flags;
  (void)dst_len;
  return vsprintf(dst, fmt, ap);
}

int __vsnprintf_chk(char *dst, size_t supplied_size, int flags, size_t dst_len,
                    const char *fmt, va_list ap) {
  (void)flags;
  (void)dst_len;
  return vsnprintf(dst, supplied_size, fmt, ap);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

void __FD_CLR_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  FD_CLR(fd, set);
}
void __FD_SET_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  FD_SET(fd, set);
}
int __FD_ISSET_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  return FD_ISSET(fd, set);
}

/* contadores de chamadas de desenho (diagnostico da tela preta) */
unsigned long g_draw_calls = 0, g_draw_verts = 0;
int g_draw_probe = 0; /* ligado por 1 frame p/ medir o efeito de cada desenho */
static void probe_after_draw(const char *who, int count) {
  if (!g_draw_probe) return;
  static unsigned char *buf = NULL;
  if (!buf) buf = (unsigned char *)malloc(1280 * 720 * 4);
  if (!buf) return;
  glReadPixels(0, 0, 1280, 720, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  long nz = 0;
  for (long i = 0; i < 1280L * 720L; i++)
    if (buf[i * 4] + buf[i * 4 + 1] + buf[i * 4 + 2] > 12) nz++;
  GLint prog = 0, tex = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  debugPrintf("[draw] %s count=%d prog=%d tex=%d -> pixels=%ld\n", who, count, prog, tex, nz);
}
static void count_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  g_draw_calls++; g_draw_verts += (unsigned long)count;
  glDrawArrays(mode, first, count);
  probe_after_draw("glDrawArrays", count);
}
static void count_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  g_draw_calls++; g_draw_verts += (unsigned long)count;
  glDrawElements(mode, count, type, idx);
  probe_after_draw("glDrawElements", count);
}

static FILE *my_fopen(const char *pathname, const char *mode) {
  const char *res = resolve_android_path(pathname);
  FILE *f = fopen(res, mode);
  static int filelog = -1;
  if (filelog < 0) filelog = getenv("HW_FILELOG") ? 1 : 0;
  if (filelog && pathname)
    debugPrintf("[file] fopen(\"%s\" -> \"%s\", \"%s\") = %p\n", pathname, res, mode, (void *)f);
  return f;
}

static int my_open(const char *pathname, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    return open(resolve_android_path(pathname), flags, mode);
  }
  return open(resolve_android_path(pathname), flags);
}

static int my_open_2(const char *pathname, int flags) {
  return open(resolve_android_path(pathname), flags);
}

/* glibc <= 2.32 does not export the plain stat/lstat/fstat symbols used by
 * this Bionic guest.  The approved AArch64 Sonic adapter uses the raw kernel
 * entry points as well: on this ABI the kernel stat layout is the layout the
 * guest expects.  Resolve Android paths before path-based calls. */
static int bionic_stat_fake(const char *pathname, void *buffer) {
  return (int)syscall(SYS_newfstatat, AT_FDCWD,
                      resolve_android_path(pathname), buffer, 0);
}

static int bionic_lstat_fake(const char *pathname, void *buffer) {
  return (int)syscall(SYS_newfstatat, AT_FDCWD,
                      resolve_android_path(pathname), buffer,
                      AT_SYMLINK_NOFOLLOW);
}

static int bionic_fstat_fake(int fd, void *buffer) {
  return (int)syscall(SYS_fstat, fd, buffer);
}

static int mkdir_fake(const char *pathname, mode_t mode) {
  const char *resolved = resolve_android_path(pathname);
  int ret = mkdir(resolved, mode);
  if (ret == 0)
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = 0\n", pathname, resolved,
                (unsigned)mode);
  else
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = -1 (errno=%d: %s)\n", pathname,
                resolved, (unsigned)mode, errno, strerror(errno));
  return ret;
}

static int remove_fake(const char *pathname) {
  const char *resolved = resolve_android_path(pathname);
  int ret = remove(resolved);
  if (ret == 0)
    debugPrintf("remove(\"%s\" -> \"%s\") = 0\n", pathname, resolved);
  else
    debugPrintf("remove(\"%s\" -> \"%s\") = -1 (errno=%d: %s)\n", pathname,
                resolved, errno, strerror(errno));
  return ret;
}

static int rename_fake(const char *oldpath, const char *newpath) {
  char resolved_old[2048];
  char resolved_new[2048];
  const char *resolved_old_src = resolve_android_path(oldpath);
  const char *resolved_new_src;
  int ret;

  SDL_strlcpy(resolved_old, resolved_old_src, sizeof(resolved_old));
  resolved_new_src = resolve_android_path(newpath);
  SDL_strlcpy(resolved_new, resolved_new_src, sizeof(resolved_new));
  ret = rename(resolved_old, resolved_new);
  if (ret == 0) {
    debugPrintf("rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = 0\n", oldpath,
                resolved_old, newpath, resolved_new);
  } else {
    debugPrintf(
        "rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = -1 (errno=%d: %s)\n",
        oldpath, resolved_old, newpath, resolved_new, errno, strerror(errno));
  }
  return ret;
}

// LSW PTHREAD HACKS

typedef struct HostMutexEntry {
  void *guest_addr;
  pthread_mutex_t mutex;
  struct HostMutexEntry *next;
} HostMutexEntry;

typedef struct HostCondEntry {
  void *guest_addr;
  pthread_cond_t cond;
  struct HostCondEntry *next;
} HostCondEntry;

static HostMutexEntry *g_mutex_entries = NULL;
static HostCondEntry *g_cond_entries = NULL;
static pthread_mutex_t g_mutex_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cond_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *lookup_host_mutex(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;
  pthread_mutex_lock(&g_mutex_registry_lock);
  for (HostMutexEntry *entry = g_mutex_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_mutex_registry_lock);
      return &entry->mutex;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_mutex_registry_lock);
    return NULL;
  }
  HostMutexEntry *entry = calloc(1, sizeof(*entry));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&entry->mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  entry->guest_addr = guest_addr;
  entry->next = g_mutex_entries;
  g_mutex_entries = entry;
  pthread_mutex_unlock(&g_mutex_registry_lock);
  return &entry->mutex;
}

static int destroy_host_mutex(void *guest_addr) {
  if (!guest_addr)
    return 0;
  pthread_mutex_lock(&g_mutex_registry_lock);
  HostMutexEntry **link = &g_mutex_entries;
  while (*link) {
    HostMutexEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
      *link = entry->next;
      pthread_mutex_unlock(&g_mutex_registry_lock);
      pthread_mutex_destroy(&entry->mutex);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_mutex_registry_lock);
  return 0;
}

static pthread_cond_t *lookup_host_cond(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;
  pthread_mutex_lock(&g_cond_registry_lock);
  for (HostCondEntry *entry = g_cond_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_cond_registry_lock);
      return &entry->cond;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_cond_registry_lock);
    return NULL;
  }
  HostCondEntry *entry = calloc(1, sizeof(*entry));
  pthread_cond_init(&entry->cond, NULL);
  entry->guest_addr = guest_addr;
  entry->next = g_cond_entries;
  g_cond_entries = entry;
  pthread_mutex_unlock(&g_cond_registry_lock);
  return &entry->cond;
}

static int destroy_host_cond(void *guest_addr) {
  if (!guest_addr)
    return 0;
  pthread_mutex_lock(&g_cond_registry_lock);
  HostCondEntry **link = &g_cond_entries;
  while (*link) {
    HostCondEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
      *link = entry->next;
      pthread_mutex_unlock(&g_cond_registry_lock);
      pthread_cond_destroy(&entry->cond);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_cond_registry_lock);
  return 0;
}

int pthread_mutex_init_fake(pthread_mutex_t *uid, const int *mutexattr) {
  return lookup_host_mutex(uid, 1) ? 0 : -1;
}
int pthread_mutex_destroy_fake(pthread_mutex_t *uid) {
  return destroy_host_mutex(uid);
}
int pthread_mutex_lock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_lock(lookup_host_mutex(uid, 1));
}
int pthread_mutex_trylock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_trylock(lookup_host_mutex(uid, 1));
}
int pthread_mutex_unlock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_unlock(lookup_host_mutex(uid, 1));
}

int pthread_cond_init_fake(pthread_cond_t *cnd, const int *condattr) {
  return lookup_host_cond(cnd, 1) ? 0 : -1;
}
int pthread_cond_destroy_fake(pthread_cond_t *cnd) {
  return destroy_host_cond(cnd);
}
int pthread_cond_wait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx) {
  return pthread_cond_wait(lookup_host_cond(cnd, 1), lookup_host_mutex(mtx, 1));
}
int pthread_cond_timedwait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx,
                                const struct timespec *t) {
  return pthread_cond_timedwait(lookup_host_cond(cnd, 1),
                                lookup_host_mutex(mtx, 1), t);
}
int pthread_cond_signal_fake(pthread_cond_t *cnd) {
  return pthread_cond_signal(lookup_host_cond(cnd, 1));
}
int pthread_cond_broadcast_fake(pthread_cond_t *cnd) {
  return pthread_cond_broadcast(lookup_host_cond(cnd, 1));
}

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadWrapper;
static void *thread_wrapper_func(void *data) {
  ThreadWrapper *w = (ThreadWrapper *)data;
  void *(*entry)(void *) = w->entry;
  void *arg = w->arg;
  free(w);
  return entry(arg);
}
int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                        void *arg) {
  ThreadWrapper *w = malloc(sizeof(ThreadWrapper));
  w->entry = entry;
  w->arg = arg;
  pthread_attr_t real_attr;
  pthread_attr_init(&real_attr);
  pthread_attr_setstacksize(&real_attr, 2 * 1024 * 1024);
  int ret = pthread_create(thread, &real_attr, thread_wrapper_func, w);
  pthread_attr_destroy(&real_attr);
  if (ret != 0)
    free(w);
  return ret;
}

/* pthread_attr_t bionic (56B) < glibc (64B). A engine aloca o attr na PILHA
 * (tamanho bionic); o glibc pthread_attr_init/setschedparam escreveria 64B ->
 * estoura o buffer -> corrompe a stack-canary (visto em SQEX sead
 * DelegateManager::Initialize). Como pthread_create_fake IGNORA o attr passado
 * (usa um attr glibc proprio), estes podem ser no-ops seguros. */
static int my_pthread_attr_init_noop(void *attr) {
  (void)attr;
  return 0;
}
static int my_pthread_attr_setschedparam_noop(void *attr, const void *param) {
  (void)attr; (void)param;
  return 0;
}

static void *pthread_getspecific_fake(pthread_key_t key) {
  return pthread_getspecific(key);
}
static int pthread_setspecific_fake(pthread_key_t key, const void *value) {
  return pthread_setspecific(key, value);
}
int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  return pthread_once((pthread_once_t *)once_control, init_routine);
}

// AASSETMANAGER EMULATION
// this bit here redirects apk reads directly to the assets folder

typedef struct {
  FILE *f;
  size_t size;
} FakeAsset;

typedef struct {
  DIR *d;
} FakeAssetDir;

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env;
  (void)assetManager;
  return (void *)0x1337;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr;
  (void)mode;
  char path[1024];

  const char *relative_path = filename;
  if (strncmp(filename, "assets/", 7) == 0) {
    relative_path = filename + 7;
  } else if (strncmp(filename, "./assets/", 9) == 0) {
    relative_path = filename + 9;
  }

  snprintf(path, sizeof(path), "./assets/%s", relative_path);
  FILE *f = fopen(path, "rb");
  if (!f) {
    f = fopen(resolve_android_path(filename), "rb");
  }

  /* NAO logar por padrao: o jogo abre resources.bin centenas de vezes/frame e
     debugPrintf faz fopen+fclose de debug.log a cada chamada -> stall brutal de
     I/O no main loop -> decode de audio morre de fome (gagueira). CHRONO_ASSETLOG=1
     reativa p/ debug. */
  if (getenv("CHRONO_ASSETLOG"))
    debugPrintf("AAssetManager_open(\"%s\" -> \"%s\") = %p\n", filename, path, f);

  if (!f)
    return NULL;

  FakeAsset *asset = malloc(sizeof(FakeAsset));
  asset->f = f;
  fseek(f, 0, SEEK_END);
  asset->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  return asset;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  return fread(buf, 1, count, a->f);
}

void AAsset_close_fake(void *asset) {
  if (!asset)
    return;
  FakeAsset *a = (FakeAsset *)asset;
  fclose(a->f);
  free(a);
}

off_t AAsset_getLength_fake(void *asset) {
  if (!asset)
    return 0;
  FakeAsset *a = (FakeAsset *)asset;
  return a->size;
}

off_t AAsset_getRemainingLength_fake(void *asset) {
  if (!asset)
    return 0;
  FakeAsset *a = (FakeAsset *)asset;
  off_t cur = ftell(a->f);
  return a->size - cur;
}

off_t AAsset_seek_fake(void *asset, off_t offset, int whence) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  fseek(a->f, offset, whence);
  return ftell(a->f);
}

int AAsset_openFileDescriptor_fake(void *asset, off_t *outStart, off_t *outLength) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  if (outStart) *outStart = 0;
  if (outLength) *outLength = a->size;
  return fileno(a->f);
}

void __assert2_fake(const char *file, int line, const char *function, const char *failed_expression) {
  fprintf(stderr, "Assertion failed: %s (%s:%d: %s)\n", failed_expression, file, line, function);
}

int __register_atfork_fake(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso) {
  return pthread_atfork(prepare, parent, child);
}

void *my_eglGetProcAddress(const char *procname) {
  return SDL_GL_GetProcAddress(procname);
}

void *AAssetManager_openDir_fake(void *mgr, const char *dirName) {
  (void)mgr;
  char path[1024];
  snprintf(path, sizeof(path), "./assets/%s", dirName);
  DIR *d = opendir(path);
  if (!d)
    return NULL;
  FakeAssetDir *adir = malloc(sizeof(FakeAssetDir));
  adir->d = d;
  return adir;
}

const char *AAssetDir_getNextFileName_fake(void *assetDir) {
  if (!assetDir)
    return NULL;
  FakeAssetDir *ad = (FakeAssetDir *)assetDir;
  struct dirent *ent = readdir(ad->d);
  if (!ent)
    return NULL;
  return ent->d_name;
}

void AAssetDir_close_fake(void *assetDir) {
  if (!assetDir)
    return;
  FakeAssetDir *ad = (FakeAssetDir *)assetDir;
  closedir(ad->d);
  free(ad);
}

size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

int dl_iterate_phdr_fake(void *callback, void *data) { return 0; }
static unsigned long getauxval_stub(unsigned long type) { return 0; }
static void sincos_stub(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}
static void sincosf_stub(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}
static long double strtold_l_stub(const char *nptr, char **endptr, void *loc) {
  return strtold(nptr, endptr);
}
static long long strtoll_l_stub(const char *nptr, char **endptr, int base,
                                void *loc) {
  return strtoll(nptr, endptr, base);
}
static unsigned long long strtoull_l_stub(const char *nptr, char **endptr,
                                          int base, void *loc) {
  return strtoull(nptr, endptr, base);
}
/* locale REAL, nao stub. A libc++ do NDK guarda o retorno de newlocale() como
 * "o locale C" e passa esse ponteiro para isdigit_l/tolower_l/... da glibc.
 * Devolvendo NULL, a primeira formatacao de float com iostream
 * (patch::to_string<float> em UserProgress::addCompletionTime, no fim da fase)
 * derefenciava NULL -> SIGSEGV. As mascaras de categoria da bionic nao batem
 * com as da glibc, entao criamos sempre o locale "C" inteiro. */
static void *newlocale_stub(int category_mask, const char *locale, void *base) {
  (void)category_mask;
  (void)base;
  locale_t l = newlocale(LC_ALL_MASK, (locale && *locale) ? locale : "C", (locale_t)0);
  if (!l) l = newlocale(LC_ALL_MASK, "C", (locale_t)0);
  if (!l) debugPrintf("[locale] AVISO: newlocale falhou (errno=%d)\n", errno);
  return (void *)l;
}
static void freelocale_stub(void *locobj) {
  if (locobj && locobj != (void *)-1) freelocale((locale_t)locobj);
}
static void *uselocale_stub(void *newloc) {
  return (void *)uselocale((locale_t)newloc);
}
static int initgroups_stub(const char *user, gid_t group) { return 0; }
static int sysinfo_stub(void *info) { return 0; }
static void syslog_stub(int priority, const char *format, ...) {}
static void closelog_stub(void) {}
static void openlog_stub(const char *ident, int option, int facility) {}
static int tcgetattr_stub(int fd, void *termios_p) { return 0; }
static int tcsetattr_stub(int fd, int optional_actions, const void *termios_p) {
  return 0;
}
static int mlock_stub(const void *addr, size_t len) { return 0; }
static void *funopen_stub(const void *cookie,
                          int (*readfn)(void *, char *, int),
                          int (*writefn)(void *, const char *, int),
                          long (*seekfn)(void *, long, int),
                          int (*closefn)(void *)) {
  return NULL;
}
static int sigsetjmp_stub(void *env, int savesigs) { return 0; }
static void siglongjmp_stub(void *env, int val) {}

static int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum;
  (void)act;
  (void)oldact;
  return 0;
}

/* ---- stubs bionic-only (glibc nao tem; senao slot vira PLT0 -> crash) ---- */
static int my___system_property_get(const char *name, char *value) {
  (void)name;
  if (value) value[0] = '\0';
  return 0;
}
static void my___android_log_assert(const char *cond, const char *tag,
                                    const char *fmt, ...) {
  (void)cond; (void)tag; (void)fmt;
  debugPrintf("[android_log_assert] cond=%s tag=%s\n", cond ? cond : "?",
              tag ? tag : "?");
  /* nao aborta: deixa o engine seguir (boot). */
}


/* ---- offtheroad: ABI Bionic que a happywheels nao precisava (ot_bionic.c) ---- */
extern int ot_sem_init(void *, int, unsigned);
extern int ot_sem_destroy(void *);
extern int ot_sem_wait(void *);
extern int ot_sem_trywait(void *);
extern int ot_sem_post(void *);
extern int ot_sem_getvalue(void *, int *);
extern int ot_gettid(void);
extern void *__emutls_get_address(void *);
extern int ot_setjmp(void *);
extern void ot_longjmp(void *, int);

DynLibFunction dynlib_functions[] = {
    /* sem_t do Bionic tem 4 bytes; o da glibc tem 32 -> shim proprio. */
    {"sem_init", (uintptr_t)&ot_sem_init},
    {"sem_destroy", (uintptr_t)&ot_sem_destroy},
    {"sem_wait", (uintptr_t)&ot_sem_wait},
    {"sem_trywait", (uintptr_t)&ot_sem_trywait},
    {"sem_post", (uintptr_t)&ot_sem_post},
    {"sem_getvalue", (uintptr_t)&ot_sem_getvalue},
    /* jmp_buf do Bionic tem 256 bytes; __jmp_buf_tag da glibc tem 312. */
    {"setjmp", (uintptr_t)&ot_setjmp},
    {"_setjmp", (uintptr_t)&ot_setjmp},
    {"longjmp", (uintptr_t)&ot_longjmp},
    {"_longjmp", (uintptr_t)&ot_longjmp},
    {"__emutls_get_address", (uintptr_t)&__emutls_get_address},
    {"gettid", (uintptr_t)&ot_gettid},

    {"__system_property_get", (uintptr_t)&my___system_property_get},
    {"__android_log_assert", (uintptr_t)&my___android_log_assert},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"abort", (uintptr_t)&abort},
    {"accept", (uintptr_t)&accept},
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"__android_log_write", (uintptr_t)&__android_log_write_fake},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_fake},
    {"asin", (uintptr_t)&asin},
    {"atan", (uintptr_t)&atan},
    {"atan2f", (uintptr_t)&atan2f},
    {"atanf", (uintptr_t)&atanf},
    {"atof", (uintptr_t)&atof},
    {"atoi", (uintptr_t)&atoi},
    {"atoll", (uintptr_t)&atoll},
    {"bind", (uintptr_t)&bind},
    {"btowc", (uintptr_t)&btowc},
    {"calloc", (uintptr_t)&calloc},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"close", (uintptr_t)&close},
    {"closedir", (uintptr_t)&closedir},
    {"closelog", (uintptr_t)&closelog_stub},
    {"connect", (uintptr_t)&connect},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"dlclose", (uintptr_t)&dlclose},
    {"dlerror", (uintptr_t)&dlerror},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"dlopen", (uintptr_t)&dlopen},
    {"dlsym", (uintptr_t)&dlsym},
    {"__errno", (uintptr_t)&__errno_fake},
    {"_exit", (uintptr_t)&_exit},
    {"exit", (uintptr_t)&exit},
    {"exp", (uintptr_t)&exp},
    {"expf", (uintptr_t)&expf},
    {"fclose", (uintptr_t)&fclose},
    {"fcntl", (uintptr_t)&fcntl},
    {"__FD_CLR_chk", (uintptr_t)&__FD_CLR_chk},
    {"__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk},
    {"__FD_SET_chk", (uintptr_t)&__FD_SET_chk},
    {"feof", (uintptr_t)&sf_feof},
    {"ferror", (uintptr_t)&sf_ferror},
    {"fflush", (uintptr_t)&sf_fflush},
    {"fgetc", (uintptr_t)&sf_fgetc},
    {"fgets", (uintptr_t)&sf_fgets},
    {"fileno", (uintptr_t)&sf_fileno},
    {"fmodf", (uintptr_t)&fmodf},
    {"fopen", (uintptr_t)&my_fopen},
    {"fprintf", (uintptr_t)&sf_fprintf},
    {"fputc", (uintptr_t)&sf_fputc},
    {"fputs", (uintptr_t)&sf_fputs},
    {"fread", (uintptr_t)&fread},
    {"free", (uintptr_t)&free},
    {"freeaddrinfo", (uintptr_t)&freeaddrinfo},
    {"freelocale", (uintptr_t)&freelocale_stub},
    {"fseek", (uintptr_t)&fseek},
    {"fseeko", (uintptr_t)&fseeko},
    {"fstat", (uintptr_t)&bionic_fstat_fake},
    {"ftell", (uintptr_t)&ftell},
    {"ftello", (uintptr_t)&ftello},
    {"ftruncate", (uintptr_t)&ftruncate},
    {"funopen", (uintptr_t)&funopen_stub},
    {"fwrite", (uintptr_t)&sf_fwrite},
    {"gai_strerror", (uintptr_t)&gai_strerror},
    {"getaddrinfo", (uintptr_t)&getaddrinfo},
    {"getauxval", (uintptr_t)&getauxval_stub},
    {"getenv", (uintptr_t)&getenv},
    {"gethostbyname", (uintptr_t)&gethostbyname},
    {"getnameinfo", (uintptr_t)&getnameinfo},
    {"getpagesize", (uintptr_t)&getpagesize},
    {"getpeername", (uintptr_t)&getpeername},
    {"getpid", (uintptr_t)&getpid},
    {"getpwuid", (uintptr_t)&getpwuid},
    {"getrlimit", (uintptr_t)&getrlimit},
    {"getsockname", (uintptr_t)&getsockname},
    {"getsockopt", (uintptr_t)&getsockopt},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"getuid", (uintptr_t)&getuid},

    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glBufferData", (uintptr_t)&glBufferData},
    {"glBufferSubData", (uintptr_t)&glBufferSubData},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&glClear},
    {"glClearColor", (uintptr_t)&glClearColor},
    {"glClearDepthf", (uintptr_t)&glClearDepthf},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D},
    {"glCreateProgram", (uintptr_t)&glCreateProgram},
    {"glCreateShader", (uintptr_t)&glCreateShader},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},
    {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
    {"glDeleteProgram", (uintptr_t)&glDeleteProgram},
    {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},
    {"glDeleteShader", (uintptr_t)&glDeleteShader},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},
    {"glDrawArrays", (uintptr_t)&count_glDrawArrays},
    {"glDrawElements", (uintptr_t)&count_glDrawElements},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
    {"glGenBuffers", (uintptr_t)&glGenBuffers},
    {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetFramebufferAttachmentParameteriv",
     (uintptr_t)&glGetFramebufferAttachmentParameteriv},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform2fv", (uintptr_t)&glUniform2fv},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform4fv", (uintptr_t)&glUniform4fv},
    {"glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},
    {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},
    {"glUseProgram", (uintptr_t)&glUseProgram},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport},

    {"gmtime_r", (uintptr_t)&gmtime_r},
    {"inet_ntop", (uintptr_t)&inet_ntop},
    {"inet_pton", (uintptr_t)&inet_pton},
    {"initgroups", (uintptr_t)&initgroups_stub},
    {"ioctl", (uintptr_t)&ioctl},
    {"isalnum", (uintptr_t)&isalnum},
    {"isalpha", (uintptr_t)&isalpha},
    {"islower", (uintptr_t)&islower},
    {"isspace", (uintptr_t)&isspace},
    {"isupper", (uintptr_t)&isupper},
    {"iswalpha", (uintptr_t)&iswalpha},
    {"iswblank", (uintptr_t)&iswblank},
    {"iswcntrl", (uintptr_t)&iswcntrl},
    {"iswdigit", (uintptr_t)&iswdigit},
    {"iswlower", (uintptr_t)&iswlower},
    {"iswprint", (uintptr_t)&iswprint},
    {"iswpunct", (uintptr_t)&iswpunct},
    {"iswspace", (uintptr_t)&iswspace},
    {"iswupper", (uintptr_t)&iswupper},
    {"iswxdigit", (uintptr_t)&iswxdigit},
    {"isxdigit", (uintptr_t)&isxdigit},
    {"kill", (uintptr_t)&kill},
    {"ldexp", (uintptr_t)&ldexp},
    {"ldexpf", (uintptr_t)&ldexpf},
    {"listen", (uintptr_t)&listen},
    {"localeconv", (uintptr_t)&localeconv},
    {"localtime", (uintptr_t)&localtime},
    {"localtime_r", (uintptr_t)&localtime_r},
    {"log", (uintptr_t)&log},
    {"log10", (uintptr_t)&log10},
    {"log10f", (uintptr_t)&log10f},
    {"logf", (uintptr_t)&logf},
    {"lseek", (uintptr_t)&lseek},
    {"madvise", (uintptr_t)&madvise},
    {"malloc", (uintptr_t)&malloc},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"mbstowcs", (uintptr_t)&mbstowcs},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"memchr", (uintptr_t)&memchr},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"memmove", (uintptr_t)&memmove},
    {"__memmove_chk", (uintptr_t)&__memmove_chk},
    {"memset", (uintptr_t)&memset},
    {"mkdir", (uintptr_t)&mkdir_fake},
    {"mktime", (uintptr_t)&mktime},
    {"mlock", (uintptr_t)&mlock_stub},
    {"mmap", (uintptr_t)&mmap},
    {"modf", (uintptr_t)&modf},
    {"mprotect", (uintptr_t)&mprotect},
    {"munmap", (uintptr_t)&munmap},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"newlocale", (uintptr_t)&newlocale_stub},
    {"open", (uintptr_t)&my_open},
    {"__open_2", (uintptr_t)&my_open_2},
    {"opendir", (uintptr_t)&opendir},
    {"openlog", (uintptr_t)&openlog_stub},
    {"perror", (uintptr_t)&perror},
    {"pipe", (uintptr_t)&pipe},
    {"poll", (uintptr_t)&poll},
    {"posix_memalign", (uintptr_t)&posix_memalign},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"printf", (uintptr_t)&printf},

    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_attr_init", (uintptr_t)&my_pthread_attr_init_noop},
    {"pthread_attr_setschedparam", (uintptr_t)&my_pthread_attr_setschedparam_noop},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_kill", (uintptr_t)&pthread_kill},
    {"pthread_mutexattr_destroy", (uintptr_t)&ret0},
    {"pthread_mutexattr_init", (uintptr_t)&ret0},
    {"pthread_mutexattr_settype", (uintptr_t)&ret0},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy},
    {"pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},

    {"puts", (uintptr_t)&puts},
    {"qsort", (uintptr_t)&qsort},
    {"rand", (uintptr_t)&rand},
    {"read", (uintptr_t)&read},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"readdir", (uintptr_t)&readdir},
    {"realloc", (uintptr_t)&realloc},
    {"recv", (uintptr_t)&recv},
    {"recvfrom", (uintptr_t)&recvfrom},
    {"remove", (uintptr_t)&remove_fake},
    {"rename", (uintptr_t)&rename_fake},
    {"rewind", (uintptr_t)&rewind},
    {"rmdir", (uintptr_t)&rmdir},
    {"sched_yield", (uintptr_t)&sched_yield},
    {"select", (uintptr_t)&select},
    {"send", (uintptr_t)&send},
    {"sendto", (uintptr_t)&sendto},
    {"setgid", (uintptr_t)&setgid},
    {"setlocale", (uintptr_t)&setlocale},
    {"setsockopt", (uintptr_t)&setsockopt},
    {"setuid", (uintptr_t)&setuid},
    {"__sF", (uintptr_t)&fake_sF},
    {"shutdown", (uintptr_t)&shutdown},
    {"sigaction", (uintptr_t)&sigaction_fake},
    {"sigaddset", (uintptr_t)&sigaddset},
    {"sigdelset", (uintptr_t)&sigdelset},
    {"sigemptyset", (uintptr_t)&sigemptyset},
    {"sigfillset", (uintptr_t)&sigfillset},
    {"siglongjmp", (uintptr_t)&siglongjmp_stub},
    {"signal", (uintptr_t)&signal},
    {"sigprocmask", (uintptr_t)&sigprocmask},
    {"sigsetjmp", (uintptr_t)&sigsetjmp_stub},
    {"sin", (uintptr_t)&sin},
    {"sincos", (uintptr_t)&sincos_stub},
    {"sincosf", (uintptr_t)&sincosf_stub},
    {"sinf", (uintptr_t)&sinf},

    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&sl_IID_ENVIRONMENTALREVERB},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},

    {"snprintf", (uintptr_t)&snprintf},
    {"socket", (uintptr_t)&socket},
    {"sprintf", (uintptr_t)&sprintf},
    {"srand", (uintptr_t)&srand},
    {"sscanf", (uintptr_t)&sscanf},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_stub},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"stat", (uintptr_t)&bionic_stat_fake},
    {"lstat", (uintptr_t)&bionic_lstat_fake},
    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strcat", (uintptr_t)&strcat},
    {"__strcat_chk", (uintptr_t)&__strcat_chk},
    {"strchr", (uintptr_t)&strchr},
    {"__strchr_chk", (uintptr_t)&__strchr_chk},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcoll", (uintptr_t)&strcoll},
    {"strcpy", (uintptr_t)&strcpy},
    {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
    {"strcspn", (uintptr_t)&strcspn},
    {"strdup", (uintptr_t)&strdup},
    {"strerror", (uintptr_t)&strerror},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"strftime", (uintptr_t)&strftime},
    {"strlen", (uintptr_t)&strlen},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"strncasecmp", (uintptr_t)&strncasecmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"__strncpy_chk2", (uintptr_t)&__strncpy_chk2},
    {"strrchr", (uintptr_t)&strrchr},
    {"__strrchr_chk", (uintptr_t)&__strrchr_chk},
    {"strspn", (uintptr_t)&strspn},
    {"strstr", (uintptr_t)&strstr},
    {"strtod", (uintptr_t)&strtod},
    {"strtof", (uintptr_t)&strtof},
    {"strtol", (uintptr_t)&strtol},
    {"strtold", (uintptr_t)&strtold},
    {"strtold_l", (uintptr_t)&strtold_l_stub},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoll_l", (uintptr_t)&strtoll_l_stub},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtoull_l", (uintptr_t)&strtoull_l_stub},
    {"strxfrm", (uintptr_t)&strxfrm},
    {"swprintf", (uintptr_t)&swprintf},
    {"syscall", (uintptr_t)&syscall},
    {"sysconf", (uintptr_t)&sysconf},
    {"sysinfo", (uintptr_t)&sysinfo_stub},
    {"syslog", (uintptr_t)&syslog_stub},
    {"system", (uintptr_t)&system},
    {"tan", (uintptr_t)&tan},
    {"tanf", (uintptr_t)&tanf},
    {"tcgetattr", (uintptr_t)&tcgetattr_stub},
    {"tcsetattr", (uintptr_t)&tcsetattr_stub},
    {"time", (uintptr_t)&time},
    {"tolower", (uintptr_t)&tolower},
    {"toupper", (uintptr_t)&toupper},
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"uname", (uintptr_t)&uname},
    {"unlink", (uintptr_t)&unlink},
    {"uselocale", (uintptr_t)&uselocale_stub},
    {"usleep", (uintptr_t)&usleep},
    {"vasprintf", (uintptr_t)&vasprintf},
    {"vfprintf", (uintptr_t)&sf_vfprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"vswprintf", (uintptr_t)&vswprintf},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"wcscat", (uintptr_t)&wcscat},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcscpy", (uintptr_t)&wcscpy},
    {"wcslen", (uintptr_t)&wcslen},
    {"wcsnrtombs", (uintptr_t)&wcsnrtombs},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstold", (uintptr_t)&wcstold},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstombs", (uintptr_t)&wcstombs},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"wctob", (uintptr_t)&wctob},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"write", (uintptr_t)&write},

    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake},
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
    {"AAsset_close", (uintptr_t)&AAsset_close_fake},
    {"AAsset_read", (uintptr_t)&AAsset_read_fake},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
    {"AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_fake},
    {"AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake},
    {"AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake},
    {"AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake},
    {"AAssetDir_close", (uintptr_t)&AAssetDir_close_fake},

    {"__assert2", (uintptr_t)&__assert2_fake},
    {"__register_atfork", (uintptr_t)&__register_atfork_fake},
    {"eglGetProcAddress", (uintptr_t)&my_eglGetProcAddress},

    {"_ctype_", (uintptr_t)&_ctype_},
    {"_toupper_tab_", (uintptr_t)&_toupper_tab_},
    {"_tolower_tab_", (uintptr_t)&_tolower_tab_},
    {"stderr", (uintptr_t)&fake_sF[2]},
    {"stdout", (uintptr_t)&fake_sF[1]},
    {"stdin", (uintptr_t)&fake_sF[0]},

    {"SL_IID_SEEK", (uintptr_t)&sl_IID_SEEK},
    {"SL_IID_PREFETCHSTATUS", (uintptr_t)&sl_IID_PREFETCHSTATUS},
    {"SL_IID_METADATAEXTRACTION", (uintptr_t)&sl_IID_METADATAEXTRACTION},

    {"ANativeWindow_fromSurface", (uintptr_t)&ret1},
    {"ANativeWindow_getWidth", (uintptr_t)&ret1},
    {"ANativeWindow_getHeight", (uintptr_t)&ret1},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ret1},
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
