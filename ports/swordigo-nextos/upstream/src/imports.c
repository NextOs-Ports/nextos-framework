/* imports.c — libswordigo.so import table (Swordigo 1.4.12 arm64)
 * GLES1 + OpenAL host + AAsset→assets/ + bionic pthread/stdio shims.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <locale.h>
#include <pthread.h>
#include <dirent.h>
#include <poll.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <zlib.h>

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "imports.h"
#include "so_util.h"
#include "util.h"

extern int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
extern void __cxa_finalize(void *d);
/* Insurance for the Bionic canary mismatch: the guard pad in main.c keeps
 * TLS_SLOT_STACK_GUARD stable, but if a firmware still lands a mismatch the
 * game must not be killed by glibc's aborting __stack_chk_fail. */
static void b_stack_chk_fail(void) {
  debugPrintf("stack_chk: bionic canary mismatch ignored\n");
}

/* pthread_bridge.c */
extern int b_mutex_init(void *m, const void *attr);
extern int b_mutex_destroy(void *m);
extern int b_mutex_lock(void *m);
extern int b_mutex_unlock(void *m);
extern int b_once(void *once_ctl, void (*init)(void));

static int *bionic_errno(void) { return __errno_location(); }
static uint64_t __stack_chk_guard_fake = 0x4242424242424242ULL;
static const unsigned char fake_ctype[256] = {0};
static char *__ctype_ptr_ = (char *)fake_ctype;
static size_t __ctype_get_mb_cur_max_shim(void) { return 1; }
static void google_region_noop(void) {}
static void abort_hook(void) {
  void *ra = __builtin_return_address(0);
  extern void *text_base;
  extern size_t text_size;
  uintptr_t r = (uintptr_t)ra;
  uintptr_t t = (uintptr_t)text_base;
  uintptr_t off = (t && r >= t && r < t + text_size) ? (r - t) : 0;
  /* Unwind context init (.so+0x58128c) aborts when dl_iterate_phdr cannot see
   * our mmap'd module. With so_record_phdr this should be rare; still skip so
   * a single bad frame cannot kill the process during bring-up. */
  if (off == 0x5812e4) {
    debugPrintf("abort() suppressed at .so+0x%lx (unwind)\n", (unsigned long)off);
    return;
  }
  if (off)
    debugPrintf("abort() from game ra=%p (.so+0x%lx)\n", ra, (unsigned long)off);
  else
    debugPrintf("abort() from game ra=%p\n", ra);
  fflush(NULL);
  _exit(1);
}
static void alc_noop(void) {}
static int poll_shim(struct pollfd *fds, nfds_t n, int timeout) {
  return poll(fds, n, timeout);
}

/* ---- __sF stdio redirect (ducktales pattern) ---- */
#define SF_REGION_SZ 1024
static char g_sF_region[SF_REGION_SZ] __attribute__((aligned(16)));
static int in_sF(const void *p) {
  const char *c = p;
  return c >= g_sF_region && c < g_sF_region + SF_REGION_SZ;
}
static FILE *real_file(void *fp) {
  if (!in_sF(fp)) return (FILE *)fp;
  size_t off = (char *)fp - g_sF_region;
  return off == 0 ? stdin : stderr;
}
static int sw_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vfprintf(real_file(fp), fmt, ap);
  va_end(ap); return r;
}
static size_t sw_fwrite(const void *p, size_t s, size_t n, void *fp) {
  return fwrite(p, s, n, real_file(fp));
}
static size_t sw_fread(void *p, size_t s, size_t n, void *fp) {
  return fread(p, s, n, real_file(fp));
}
static int sw_fputs(const char *s, void *fp) { return fputs(s, real_file(fp)); }
static int sw_fputc(int c, void *fp) { return fputc(c, real_file(fp)); }
static int sw_putc(int c, void *fp) { return fputc(c, real_file(fp)); }
static int sw_fflush(void *fp) { return fflush(fp && in_sF(fp) ? real_file(fp) : (FILE *)fp); }
static int sw_fileno(void *fp) { return fileno(real_file(fp)); }
static int sw_ferror(void *fp) { return ferror(real_file(fp)); }
static int sw_feof(void *fp) { return feof(real_file(fp)); }
static int sw_fclose(void *fp) { return in_sF(fp) ? 0 : fclose((FILE *)fp); }
static int sw_fseek(void *fp, long off, int wh) { return fseek(real_file(fp), off, wh); }
static long sw_ftell(void *fp) { return ftell(real_file(fp)); }
static int sw_setvbuf(void *fp, char *buf, int mode, size_t sz) {
  return setvbuf(real_file(fp), buf, mode, sz);
}
static int sw_getc(void *fp) { return getc(real_file(fp)); }

/* ---- path / AAsset ---- */
static int is_absolute(const char *p) {
  return p && p[0] == '/';
}
static void resolve_path(const char *in, char *out, size_t n) {
  if (is_absolute(in)) snprintf(out, n, "%s", in);
  else snprintf(out, n, "assets/%s", in);
}
static FILE *fopen_hook(const char *path, const char *mode) {
  if (path && !strcmp(path, "/dev/urandom"))
    return fopen("/dev/urandom", mode);
  char real[512];
  resolve_path(path, real, sizeof(real));
  FILE *f = fopen(real, mode);
  if (!f && path && !is_absolute(path)) {
    /* try as-is from cwd (saves under files dir etc.) */
    f = fopen(path, mode);
  }
  if (!f && path) {
    static int nfail;
    if (nfail++ < 40)
      debugPrintf("fopen FAIL '%s' (tried '%s'): %s\n", path, real,
                  strerror(errno));
  }
  return f;
}

/* Present mmap'd libswordigo.so to the in-binary C++ unwinder. */
static int my_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *),
                              void *data) {
  for (int i = 0; i < g_so_nmods; i++) {
    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr = (ElfW(Addr))g_so_mods[i].base;
    info.dlpi_name = g_so_mods[i].name;
    info.dlpi_phdr = (const ElfW(Phdr) *)g_so_mods[i].ph;
    info.dlpi_phnum = (ElfW(Half))g_so_mods[i].phnum;
    int r = cb(&info, sizeof(info), data);
    if (r)
      return r;
  }
  return dl_iterate_phdr(cb, data);
}

typedef struct { FILE *f; long size; char path[512]; } Asset;
static void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env; (void)mgr; return (void *)1;
}
static void *AAssetManager_open_fake(void *mgr, const char *name, int mode) {
  (void)mgr; (void)mode;
  char real[512];
  snprintf(real, sizeof(real), "assets/%s", name);
  FILE *f = fopen(real, "rb");
  if (!f) {
    static int nfail;
    if (nfail++ < 40)
      debugPrintf("AAsset open FAIL '%s'\n", name ? name : "(null)");
    return NULL;
  }
  setvbuf(f, NULL, _IOFBF, 32 * 1024);
  Asset *a = malloc(sizeof(*a));
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", real);
  return a;
}
static void AAsset_close_fake(void *asset) {
  Asset *a = asset; if (a) { fclose(a->f); free(a); }
}
static int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset; return a ? (int)fread(buf, 1, count, a->f) : -1;
}
static long AAsset_getLength_fake(void *asset) {
  Asset *a = asset; return a ? a->size : 0;
}
static int AAsset_openFileDescriptor_fake(void *asset, off_t *out_start, off_t *out_len) {
  Asset *a = asset;
  if (!a) return -1;
  int fd = open(a->path, O_RDONLY);
  if (fd < 0) return -1;
  if (out_start) *out_start = 0;
  if (out_len) *out_len = a->size;
  return fd;
}

/* ---- OpenAL pre-create ---- */
static ALCdevice *g_al_device;
static ALCcontext *g_al_context;
static int g_al_active;

static int al_device_frequency(void) {
  ALCint frequency = 0;
  if (g_al_device)
    alcGetIntegerv(g_al_device, ALC_FREQUENCY, 1, &frequency);
  return (int)frequency;
}

void init_openal(void) {
  if (g_al_active) return;
  g_al_device = alcOpenDevice(NULL);
  if (!g_al_device) { debugPrintf("openal: alcOpenDevice failed\n"); return; }
  const ALCint attr[] = { ALC_FREQUENCY, 48000, 0 };
  g_al_context = alcCreateContext(g_al_device, attr);
  if (!g_al_context) { alcCloseDevice(g_al_device); g_al_device = NULL; return; }
  if (!alcMakeContextCurrent(g_al_context)) {
    alcDestroyContext(g_al_context); alcCloseDevice(g_al_device);
    g_al_context = NULL; g_al_device = NULL; return;
  }
  g_al_active = 1;
  // The device specifier names the backend that actually won (PipeWire, Pulse,
  // ALSA...).  Release 1.0.0 logged only "ok", so a mute report could not be
  // told apart from working audio: the backend has to be in the log.
  // ALC_ALL_DEVICES_SPECIFIER carries the backend's own device name, so a mute
  // "No Output" fallback is obvious in a bug report instead of looking healthy.
  const ALCchar *specifier = alcGetString(g_al_device, ALC_DEVICE_SPECIFIER);
  const ALCchar *device_name = alcGetString(g_al_device, ALC_ALL_DEVICES_SPECIFIER);
  debugPrintf("openal: ok device=%p ctx=%p spec='%s' out='%s' rate=%d\n",
              (void *)g_al_device, (void *)g_al_context,
              specifier ? specifier : "?", device_name ? device_name : "?",
              al_device_frequency());
}
void deinit_openal(void) {
  if (!g_al_active) return;
  alcMakeContextCurrent(NULL);
  if (g_al_context) alcDestroyContext(g_al_context);
  if (g_al_device) alcCloseDevice(g_al_device);
  g_al_active = 0;
}
static ALCdevice *alcOpenDevice_hook(const ALCchar *name) {
  (void)name; return g_al_active ? g_al_device : NULL;
}
static ALCcontext *alcCreateContext_hook(ALCdevice *dev, const ALCint *attr) {
  (void)attr; return (g_al_active && dev == g_al_device) ? g_al_context : NULL;
}
static ALCboolean alcCloseDevice_hook(ALCdevice *dev) {
  if (dev == g_al_device) return ALC_TRUE;
  return alcCloseDevice(dev);
}
static void alcDestroyContext_hook(ALCcontext *ctx) {
  if (ctx == g_al_context) return;
  alcDestroyContext(ctx);
}

DynLibFunction dynlib_functions[] = {
  {"AAsset_close", (uintptr_t)&AAsset_close_fake},
  {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
  {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake},
  {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
  {"AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake},
  {"AAsset_read", (uintptr_t)&AAsset_read_fake},
  {"abort", (uintptr_t)&abort_hook},
  {"acos", (uintptr_t)&acos},
  {"acosf", (uintptr_t)&acosf},
  {"alBufferData", (uintptr_t)&alBufferData},
  {"alcCloseDevice", (uintptr_t)&alcCloseDevice_hook},
  {"alcCreateContext", (uintptr_t)&alcCreateContext_hook},
  {"alcDestroyContext", (uintptr_t)&alcDestroyContext_hook},
  {"alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext},
  {"alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent},
  {"alcOpenDevice", (uintptr_t)&alcOpenDevice_hook},
  {"alcProcessContext", (uintptr_t)&alcProcessContext},
  {"alcResume", (uintptr_t)&alc_noop},
  {"alcSuspend", (uintptr_t)&alc_noop},
  {"alcSuspendContext", (uintptr_t)&alcSuspendContext},
  {"alDeleteBuffers", (uintptr_t)&alDeleteBuffers},
  {"alDeleteSources", (uintptr_t)&alDeleteSources},
  {"alDistanceModel", (uintptr_t)&alDistanceModel},
  {"alGenBuffers", (uintptr_t)&alGenBuffers},
  {"alGenSources", (uintptr_t)&alGenSources},
  {"alGetError", (uintptr_t)&alGetError},
  {"alGetSourcef", (uintptr_t)&alGetSourcef},
  {"alGetSourcei", (uintptr_t)&alGetSourcei},
  {"alListener3f", (uintptr_t)&alListener3f},
  {"alListenerf", (uintptr_t)&alListenerf},
  {"alListenerfv", (uintptr_t)&alListenerfv},
  {"alSource3f", (uintptr_t)&alSource3f},
  {"alSourcef", (uintptr_t)&alSourcef},
  {"alSourcei", (uintptr_t)&alSourcei},
  {"alSourcePause", (uintptr_t)&alSourcePause},
  {"alSourcePlay", (uintptr_t)&alSourcePlay},
  {"alSourceRewind", (uintptr_t)&alSourceRewind},
  {"alSourceStop", (uintptr_t)&alSourceStop},
  {"asin", (uintptr_t)&asin},
  {"atan2f", (uintptr_t)&atan2f},
  {"btowc", (uintptr_t)&btowc},
  {"calloc", (uintptr_t)&calloc},
  {"clock", (uintptr_t)&clock},
  {"closedir", (uintptr_t)&closedir},
  {"cos", (uintptr_t)&cos},
  {"cosf", (uintptr_t)&cosf},
  {"_ctype_", (uintptr_t)&__ctype_ptr_},
  {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_shim},
  {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
  {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
  {"dl_iterate_phdr", (uintptr_t)&my_dl_iterate_phdr},
  {"eglGetProcAddress", (uintptr_t)&eglGetProcAddress},
  {"__errno", (uintptr_t)&bionic_errno},
  {"exit", (uintptr_t)&exit},
  {"fclose", (uintptr_t)&sw_fclose},
  {"fdopen", (uintptr_t)&fdopen},
  {"feof", (uintptr_t)&sw_feof},
  {"ferror", (uintptr_t)&sw_ferror},
  {"fflush", (uintptr_t)&sw_fflush},
  {"fileno", (uintptr_t)&sw_fileno},
  {"fmodf", (uintptr_t)&fmodf},
  {"fopen", (uintptr_t)&fopen_hook},
  {"fprintf", (uintptr_t)&sw_fprintf},
  {"fputc", (uintptr_t)&sw_fputc},
  {"fputs", (uintptr_t)&sw_fputs},
  {"fread", (uintptr_t)&sw_fread},
  {"free", (uintptr_t)&free},
  {"freopen", (uintptr_t)&freopen},
  {"fseek", (uintptr_t)&sw_fseek},
  {"fstat", (uintptr_t)&fstat},
  {"ftell", (uintptr_t)&sw_ftell},
  {"fwrite", (uintptr_t)&sw_fwrite},
  {"getc", (uintptr_t)&sw_getc},
  {"gettimeofday", (uintptr_t)&gettimeofday},
  {"getwc", (uintptr_t)&getwc},
  {"glActiveTexture", (uintptr_t)&glActiveTexture},
  {"glBindBuffer", (uintptr_t)&glBindBuffer},
  {"glBindTexture", (uintptr_t)&glBindTexture},
  {"glBlendFunc", (uintptr_t)&glBlendFunc},
  {"glBufferData", (uintptr_t)&glBufferData},
  {"glBufferSubData", (uintptr_t)&glBufferSubData},
  {"glClear", (uintptr_t)&glClear},
  {"glClearColor", (uintptr_t)&glClearColor},
  {"glClearDepthf", (uintptr_t)&glClearDepthf},
  {"glClearStencil", (uintptr_t)&glClearStencil},
  {"glColor4f", (uintptr_t)&glColor4f},
  {"glColor4ub", (uintptr_t)&glColor4ub},
  {"glColorMask", (uintptr_t)&glColorMask},
  {"glColorPointer", (uintptr_t)&glColorPointer},
  {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
  {"glCullFace", (uintptr_t)&glCullFace},
  {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},
  {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
  {"glDepthFunc", (uintptr_t)&glDepthFunc},
  {"glDepthMask", (uintptr_t)&glDepthMask},
  {"glDisable", (uintptr_t)&glDisable},
  {"glDisableClientState", (uintptr_t)&glDisableClientState},
  {"glDrawArrays", (uintptr_t)&glDrawArrays},
  {"glDrawElements", (uintptr_t)&glDrawElements},
  {"glEnable", (uintptr_t)&glEnable},
  {"glEnableClientState", (uintptr_t)&glEnableClientState},
  {"glFlush", (uintptr_t)&glFlush},
  {"glGenBuffers", (uintptr_t)&glGenBuffers},
  {"glGenTextures", (uintptr_t)&glGenTextures},
  {"glGetError", (uintptr_t)&glGetError},
  {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
  {"glGetString", (uintptr_t)&glGetString},
  {"glLightf", (uintptr_t)&glLightf},
  {"glLightfv", (uintptr_t)&glLightfv},
  {"glLightModelfv", (uintptr_t)&glLightModelfv},
  {"glLoadIdentity", (uintptr_t)&glLoadIdentity},
  {"glLoadMatrixf", (uintptr_t)&glLoadMatrixf},
  {"glMaterialfv", (uintptr_t)&glMaterialfv},
  {"glMatrixMode", (uintptr_t)&glMatrixMode},
  {"glNormalPointer", (uintptr_t)&glNormalPointer},
  {"glPixelStorei", (uintptr_t)&glPixelStorei},
  {"glPopMatrix", (uintptr_t)&glPopMatrix},
  {"glPushMatrix", (uintptr_t)&glPushMatrix},
  {"glScalef", (uintptr_t)&glScalef},
  {"glScissor", (uintptr_t)&glScissor},
  {"glStencilFunc", (uintptr_t)&glStencilFunc},
  {"glStencilOp", (uintptr_t)&glStencilOp},
  {"glTexCoordPointer", (uintptr_t)&glTexCoordPointer},
  {"glTexEnvfv", (uintptr_t)&glTexEnvfv},
  {"glTexEnvi", (uintptr_t)&glTexEnvi},
  {"glTexImage2D", (uintptr_t)&glTexImage2D},
  {"glTexParameterf", (uintptr_t)&glTexParameterf},
  {"glTexParameteri", (uintptr_t)&glTexParameteri},
  {"glTranslatef", (uintptr_t)&glTranslatef},
  {"glVertexPointer", (uintptr_t)&glVertexPointer},
  {"glViewport", (uintptr_t)&glViewport},
  {"__google_potentially_blocking_region_begin", (uintptr_t)&google_region_noop},
  {"__google_potentially_blocking_region_end", (uintptr_t)&google_region_noop},
  {"gzclose", (uintptr_t)&gzclose},
  {"gzdopen", (uintptr_t)&gzdopen},
  {"gzread", (uintptr_t)&gzread},
  {"gzwrite", (uintptr_t)&gzwrite},
  {"ioctl", (uintptr_t)&ioctl},
  {"isalnum", (uintptr_t)&isalnum},
  {"isalpha", (uintptr_t)&isalpha},
  {"iscntrl", (uintptr_t)&iscntrl},
  {"islower", (uintptr_t)&islower},
  {"ispunct", (uintptr_t)&ispunct},
  {"isspace", (uintptr_t)&isspace},
  {"isupper", (uintptr_t)&isupper},
  {"iswctype", (uintptr_t)&iswctype},
  {"isxdigit", (uintptr_t)&isxdigit},
  {"localtime", (uintptr_t)&localtime},
  {"lseek", (uintptr_t)&lseek},
  {"malloc", (uintptr_t)&malloc},
  {"mbrtowc", (uintptr_t)&mbrtowc},
  {"memchr", (uintptr_t)&memchr},
  {"memcmp", (uintptr_t)&memcmp},
  {"memcpy", (uintptr_t)&memcpy},
  {"memmove", (uintptr_t)&memmove},
  {"memset", (uintptr_t)&memset},
  {"mkdir", (uintptr_t)&mkdir},
  {"opendir", (uintptr_t)&opendir},
  {"perror", (uintptr_t)&perror},
  {"poll", (uintptr_t)&poll_shim},
  {"pow", (uintptr_t)&pow},
  {"powf", (uintptr_t)&powf},
  {"printf", (uintptr_t)&printf},
  {"pthread_create", (uintptr_t)&pthread_create},
  {"pthread_getspecific", (uintptr_t)&pthread_getspecific},
  {"pthread_key_create", (uintptr_t)&pthread_key_create},
  {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
  {"pthread_mutex_destroy", (uintptr_t)&b_mutex_destroy},
  {"pthread_mutex_init", (uintptr_t)&b_mutex_init},
  {"pthread_mutex_lock", (uintptr_t)&b_mutex_lock},
  {"pthread_mutex_unlock", (uintptr_t)&b_mutex_unlock},
  {"pthread_once", (uintptr_t)&b_once},
  {"pthread_setspecific", (uintptr_t)&pthread_setspecific},
  {"putc", (uintptr_t)&sw_putc},
  {"puts", (uintptr_t)&puts},
  {"putwc", (uintptr_t)&putwc},
  {"rand", (uintptr_t)&rand},
  {"read", (uintptr_t)&read},
  {"readdir", (uintptr_t)&readdir},
  {"realloc", (uintptr_t)&realloc},
  {"remove", (uintptr_t)&remove},
  {"rename", (uintptr_t)&rename},
  {"setlocale", (uintptr_t)&setlocale},
  {"setvbuf", (uintptr_t)&sw_setvbuf},
  {"__sF", (uintptr_t)&g_sF_region},
  {"sin", (uintptr_t)&sin},
  {"sinf", (uintptr_t)&sinf},
  {"snprintf", (uintptr_t)&snprintf},
  {"sprintf", (uintptr_t)&sprintf},
  {"sqrt", (uintptr_t)&sqrt},
  {"sqrtf", (uintptr_t)&sqrtf},
  {"__stack_chk_fail", (uintptr_t)&b_stack_chk_fail},
  {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
  {"stat", (uintptr_t)&stat},
  {"strcat", (uintptr_t)&strcat},
  {"strchr", (uintptr_t)&strchr},
  {"strcmp", (uintptr_t)&strcmp},
  {"strcoll", (uintptr_t)&strcoll},
  {"strcpy", (uintptr_t)&strcpy},
  {"strcspn", (uintptr_t)&strcspn},
  {"strerror", (uintptr_t)&strerror},
  {"strftime", (uintptr_t)&strftime},
  {"strlen", (uintptr_t)&strlen},
  {"strncat", (uintptr_t)&strncat},
  {"strncmp", (uintptr_t)&strncmp},
  {"strncpy", (uintptr_t)&strncpy},
  {"strpbrk", (uintptr_t)&strpbrk},
  {"strstr", (uintptr_t)&strstr},
  {"strtod", (uintptr_t)&strtod},
  {"strtof", (uintptr_t)&strtof},
  {"strtold", (uintptr_t)&strtold},
  {"strtoul", (uintptr_t)&strtoul},
  {"strxfrm", (uintptr_t)&strxfrm},
  {"syscall", (uintptr_t)&syscall},
  {"tan", (uintptr_t)&tan},
  {"tanf", (uintptr_t)&tanf},
  {"time", (uintptr_t)&time},
  {"tolower", (uintptr_t)&tolower},
  {"toupper", (uintptr_t)&toupper},
  {"towlower", (uintptr_t)&towlower},
  {"towupper", (uintptr_t)&towupper},
  {"ungetc", (uintptr_t)&ungetc},
  {"ungetwc", (uintptr_t)&ungetwc},
  {"vsprintf", (uintptr_t)&vsprintf},
  {"wcrtomb", (uintptr_t)&wcrtomb},
  {"wcscoll", (uintptr_t)&wcscoll},
  {"wcsftime", (uintptr_t)&wcsftime},
  {"wcslen", (uintptr_t)&wcslen},
  {"wcsxfrm", (uintptr_t)&wcsxfrm},
  {"wctob", (uintptr_t)&wctob},
  {"wctype", (uintptr_t)&wctype},
  {"wmemchr", (uintptr_t)&wmemchr},
  {"wmemcmp", (uintptr_t)&wmemcmp},
  {"wmemcpy", (uintptr_t)&wmemcpy},
  {"wmemmove", (uintptr_t)&wmemmove},
  {"wmemset", (uintptr_t)&wmemset},
  {"write", (uintptr_t)&write},
  {"writev", (uintptr_t)&writev},
};
const int dynlib_functions_count =
    (int)(sizeof(dynlib_functions) / sizeof(dynlib_functions[0]));
