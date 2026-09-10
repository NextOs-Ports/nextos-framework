/*
 * bionic.c -- the libc/liblog/libdl surface that the Android objects import.
 *
 * Only the differences between bionic and glibc need real work; everything
 * whose ABI matches on arm64 (stat, dirent, most of stdio) is forwarded.
 * The differences that bite here:
 *   - FORTIFY (__*_chk) is bionic-only,
 *   - struct sigaction has a different field order,
 *   - __sF is an array of FILE, not three pointers,
 *   - the pthread objects are smaller than glibc's (see pthread_bridge.c),
 *   - __system_property_get / __android_log_* do not exist at all.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <time.h>
#include <math.h>
#include <locale.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <fnmatch.h>
#include <libgen.h>
#include <syslog.h>
#include <utime.h>
#include <zlib.h>
#include <malloc.h>
#include <pthread.h>

#include "nx_elf.h"
#include "probe_ring.h"
#include "tr.h"
#include "glibc_compat.h"

/* ------------------------------------------------------------------ logging */

int tr_log_level = 0;   /* TR_LOGCAT=1 mirrors the game's own log */

static const char lvl[] = "?????VDIWEFS";

int my___android_log_write(int prio, const char *tag, const char *msg)
{
    if (tr_log_level)
        fprintf(stderr, "[%c/%s] %s\n", lvl[prio & 15], tag ? tag : "?",
                msg ? msg : "");
    return 0;
}

int my___android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    if (!tr_log_level)
        return 0;
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return my___android_log_write(prio, tag, buf);
}

int my___android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = my___android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

void my___android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char buf[1024] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
    }
    nx_die("assert from %s: %s %s", tag ? tag : "?", cond ? cond : "", buf);
}

void my_android_set_abort_message(const char *m)
{
    fprintf(stderr, "[tr] abort message: %s\n", m ? m : "(null)");
}

/* -------------------------------------------------------- system properties */

/* Unity reads these to pick quality tiers and to decide whether it is running
 * on a known device; answering with a plausible modern phone keeps it out of
 * its own low-end fallbacks.  Nothing here is device specific. */
static const struct { const char *k, *v; } props[] = {
    { "ro.product.model",            "Pixel 6" },
    { "ro.product.manufacturer",     "Google" },
    { "ro.product.brand",            "google" },
    { "ro.product.device",           "oriole" },
    { "ro.product.name",             "oriole" },
    { "ro.product.board",            "oriole" },
    { "ro.product.cpu.abi",          "arm64-v8a" },
    { "ro.product.cpu.abilist",      "arm64-v8a" },
    { "ro.build.version.sdk",        "31" },
    { "ro.build.version.release",    "12" },
    { "ro.build.id",                 "SQ1D.220205.004" },
    { "ro.build.fingerprint",        "google/oriole/oriole:12/SQ1D.220205.004/1/user/release-keys" },
    { "ro.build.type",               "user" },
    { "ro.build.tags",               "release-keys" },
    { "ro.debuggable",               "0" },
    { "ro.secure",                   "1" },
    { "ro.hardware",                 "oriole" },
    { "ro.arch",                     "arm64" },
    { "debug.egl.hw",                "1" },
    { "ro.opengles.version",         "196608" },   /* 3.0 -- matches the APK */
    { "persist.sys.locale",          "en-US" },
    { "ro.product.locale",           "en-US" },
};

/* TR_FILELOG: every file the game opens.  Its own switch, because it fires
 * often enough that leaving it on TR_VERBOSE would cost frames. */
int tr_trace_files;

/* Verbose-only diagnostics for Android/libc compatibility calls. */
#define BIONIC_TRACE(...) nx_log(__VA_ARGS__)

int my___system_property_get(const char *key, char *value)
{
    BIONIC_TRACE("__system_property_get(\"%s\")", key ? key : "(null)");
    for (size_t i = 0; i < sizeof props / sizeof *props; i++)
        if (strcmp(props[i].k, key) == 0)
            return (int)strlen(strcpy(value, props[i].v));
    value[0] = 0;
    return 0;
}

const void *my___system_property_find(const char *key)
{
    for (size_t i = 0; i < sizeof props / sizeof *props; i++)
        if (strcmp(props[i].k, key) == 0)
            return &props[i];
    return NULL;
}

int my___system_property_read(const void *pi, char *name, char *value)
{
    const struct { const char *k, *v; } *p = pi;
    if (!p)
        return 0;
    if (name)
        strcpy(name, p->k);
    if (value)
        strcpy(value, p->v);
    return (int)strlen(p->v);
}

void my___system_property_read_callback(const void *pi,
                                        void (*cb)(void *, const char *,
                                                   const char *, uint32_t),
                                        void *ck)
{
    const struct { const char *k, *v; } *p = pi;
    if (p && cb)
        cb(ck, p->k, p->v, 1);
}

int my___system_property_foreach(void (*cb)(const void *, void *), void *ck)
{
    for (size_t i = 0; i < sizeof props / sizeof *props; i++)
        cb(&props[i], ck);
    return 0;
}

/* ----------------------------------------------------------------- __errno */

int *my___errno(void) { return &errno; }

/* ------------------------------------------------------------------- stdio */

/* bionic exports __sF as `FILE __sF[3]`, so the game computes &__sF[1] for
 * stdout.  We publish a same-shaped array of placeholders and translate them
 * back on every call that takes a FILE*. */
typedef struct { char pad[152]; } bionic_FILE;
bionic_FILE tr_sF[3];

static FILE *xf(void *f)
{
    if (f == (void *)&tr_sF[0])
        return stdin;
    if (f == (void *)&tr_sF[1])
        return stdout;
    if (f == (void *)&tr_sF[2])
        return stderr;
    return (FILE *)f;
}

static int my_fprintf(void *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(xf(f), fmt, ap);
    va_end(ap);
    return r;
}
static int my_vfprintf(void *f, const char *fmt, va_list ap) { return vfprintf(xf(f), fmt, ap); }
static int my_fputc(int c, void *f)         { return fputc(c, xf(f)); }
static int my_fputs(const char *s, void *f) { return fputs(s, xf(f)); }
static int my_fgetc(void *f)                { return fgetc(xf(f)); }
static char *my_fgets(char *s, int n, void *f) { return fgets(s, n, xf(f)); }
static size_t my_fwrite(const void *p, size_t a, size_t b, void *f) { return fwrite(p, a, b, xf(f)); }
static size_t my_fread(void *p, size_t a, size_t b, void *f) { return fread(p, a, b, xf(f)); }
static int my_fflush(void *f)               { return fflush(f ? xf(f) : NULL); }
static int my_fclose(void *f)               { return f == (void *)tr_sF ? 0 : fclose(xf(f)); }
static int my_feof(void *f)                 { return feof(xf(f)); }
static int my_ferror(void *f)               { return ferror(xf(f)); }
static void my_clearerr(void *f)            { clearerr(xf(f)); }
static int my_fileno(void *f)               { return fileno(xf(f)); }
static void my_setbuf(void *f, char *b)     { setbuf(xf(f), b); }
static int my_setvbuf(void *f, char *b, int m, size_t s) { return setvbuf(xf(f), b, m, s); }
static int my_fseek(void *f, long o, int w)  { return fseek(xf(f), o, w); }
static long my_ftell(void *f)                { return ftell(xf(f)); }
static void my_rewind(void *f)               { rewind(xf(f)); }
static int my_ungetc(int c, void *f)         { return ungetc(c, xf(f)); }
static int my_vprintf(const char *f, va_list ap) { return vfprintf(stdout, f, ap); }

/* --------------------------------------------------------------- FORTIFY */

static void *my___memcpy_chk(void *d, const void *s, size_t n, size_t dl)
{
    if (n > dl)
        nx_die("__memcpy_chk overflow (%zu > %zu)", n, dl);
    return memcpy(d, s, n);
}
static void *my___memmove_chk(void *d, const void *s, size_t n, size_t dl)
{
    if (n > dl)
        nx_die("__memmove_chk overflow");
    return memmove(d, s, n);
}
static void *my___memset_chk(void *d, int c, size_t n, size_t dl)
{
    if (n > dl)
        nx_die("__memset_chk overflow");
    return memset(d, c, n);
}
static size_t my___strlen_chk(const char *s, size_t dl) { (void)dl; return strlen(s); }
static char *my___strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
static char *my___strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
static char *my___strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
static char *my___strncat_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncat(d, s, n); }

/* bionic's __vsnprintf_chk truncates like vsnprintf; the __*_chk family takes
 * two extra leading arguments (dest length and fortify flags). */
static int my___vsnprintf_chk(char *d, size_t n, int flag, size_t dl,
                              const char *fmt, va_list ap)
{
    (void)flag; (void)dl;
    return vsnprintf(d, n, fmt, ap);
}
static int my___snprintf_chk(char *d, size_t n, int flag, size_t dl,
                             const char *fmt, ...)
{
    (void)flag; (void)dl;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(d, n, fmt, ap);
    va_end(ap);
    return r;
}
static int my___vsprintf_chk(char *d, int flag, size_t dl, const char *fmt, va_list ap)
{
    (void)flag;
    return vsnprintf(d, dl, fmt, ap);
}
static int my___sprintf_chk(char *d, int flag, size_t dl, const char *fmt, ...)
{
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(d, dl, fmt, ap);
    va_end(ap);
    return r;
}
static void my___FD_SET_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_SET(fd, s); }
static void my___FD_CLR_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_CLR(fd, s); }
static int my___FD_ISSET_chk(int fd, fd_set *s, size_t sz) { (void)sz; return FD_ISSET(fd, s); }
static ssize_t my___read_chk(int fd, void *b, size_t n, size_t dl) { (void)dl; return read(fd, b, n); }

static void my___stack_chk_fail(void) { nx_die("__stack_chk_fail"); }

/* --------------------------------------------------------------- atexit */

/* Handing the game's destructors to glibc's __cxa_atexit would register them
 * against our DSO handle and run them from exit(); the port leaves through
 * _exit() precisely because the engine's threads never stop, so keep the list
 * ourselves and never run it. */
static struct { void (*fn)(void *); void *arg; } dtors[256];
static int dtors_n;

static int my___cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (dtors_n < (int)(sizeof dtors / sizeof *dtors)) {
        dtors[dtors_n].fn = fn;
        dtors[dtors_n].arg = arg;
        dtors_n++;
    }
    return 0;
}
static void my___cxa_finalize(void *dso) { (void)dso; }
static void my___cxa_pure_virtual(void) { nx_die("pure virtual call"); }

/* bionic's fork bookkeeping entry point; glibc keeps it private, and nothing
 * here forks, so recording nothing is correct rather than merely harmless. */
static int my___register_atfork(void (*prepare)(void), void (*parent)(void),
                                void (*child)(void), void *dso)
{
    (void)prepare; (void)parent; (void)child; (void)dso;
    return 0;
}

/* --------------------------------------------------------------- sigaction */

/* bionic/arm64: { int sa_flags; handler; sigset64_t mask(8); restorer; }
 * glibc/arm64:  { handler; sigset_t mask(128); int sa_flags; restorer; }     */
struct bionic_sigaction {
    int sa_flags;
    union {
        void (*handler)(int);
        void (*action)(int, void *, void *);
    } u;
    unsigned long sa_mask;
    void (*sa_restorer)(void);
};

static int my_sigaction(int sig, const struct bionic_sigaction *na,
                        struct bionic_sigaction *oa)
{
    struct sigaction g, og;
    if (na) {
        memset(&g, 0, sizeof g);
        g.sa_flags = na->sa_flags;
        if (na->sa_flags & SA_SIGINFO)
            g.sa_sigaction = (void (*)(int, siginfo_t *, void *))na->u.action;
        else
            g.sa_handler = na->u.handler;
        sigemptyset(&g.sa_mask);
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (na->sa_mask & (1UL << (i - 1)))
                sigaddset(&g.sa_mask, i);
    }
    int r = sigaction(sig, na ? &g : NULL, oa ? &og : NULL);
    if (oa && r == 0) {
        memset(oa, 0, sizeof *oa);
        oa->sa_flags = og.sa_flags;
        oa->u.handler = og.sa_handler;
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (sigismember(&og.sa_mask, i))
                oa->sa_mask |= 1UL << (i - 1);
    }
    return r;
}

/* bionic's sigset_t for the plain (non-64) calls is also a single word. */
static int my_sigemptyset(unsigned long *s) { if (s) *s = 0; return 0; }
static int my_sigfillset(unsigned long *s) { if (s) *s = ~0UL; return 0; }
static int my_sigaddset(unsigned long *s, int n) { if (s && n > 0) *s |= 1UL << (n - 1); return 0; }
static int my_sigdelset(unsigned long *s, int n) { if (s && n > 0) *s &= ~(1UL << (n - 1)); return 0; }
static int my_sigismember(const unsigned long *s, int n) { return s && n > 0 && (*s >> (n - 1)) & 1; }

static int my_sigsuspend(const unsigned long *m)
{
    sigset_t g;
    sigemptyset(&g);
    if (m)
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (*m & (1UL << (i - 1)))
                sigaddset(&g, i);
    return sigsuspend(&g);
}

static int my_pthread_sigmask(int how, const unsigned long *set,
                              unsigned long *old)
{
    sigset_t g, go;
    sigset_t *gp = NULL;
    if (set) {
        sigemptyset(&g);
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (*set & (1UL << (i - 1)))
                sigaddset(&g, i);
        gp = &g;
    }
    int r = pthread_sigmask(how, gp, old ? &go : NULL);
    if (old && r == 0) {
        *old = 0;
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (sigismember(&go, i))
                *old |= 1UL << (i - 1);
    }
    return r;
}

static int my_pthread_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void))
{
    (void)prepare;
    (void)parent;
    (void)child;
    return 0;
}

/* ------------------------------------------------------------------ ctype */

/* bionic's _ctype_ is a 1+256 byte table indexed as _ctype_[c+1]. */
static const unsigned char tr_ctype_tab[1 + 256] = { 0 };
static const unsigned char *tr_ctype_ptr = tr_ctype_tab + 1;

static int my___ctype_get_mb_cur_max(void) { return MB_CUR_MAX; }

/* -------------------------------------------------------------------- misc */

static int my_ptrace(int req, ...)
{
    BIONIC_TRACE("ptrace(%d) -> EPERM", req);
    errno = EPERM;
    return -1;
}

static unsigned long my_getauxval(unsigned long t)
{
    unsigned long v = getauxval(t);
    BIONIC_TRACE("getauxval(%#lx) -> %#lx", t, v);
    return v;
}

/* Traced so a missing asset says which path the game actually asked for;
 * silent unless TR_VERBOSE. */
static int my_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned);
        va_end(ap);
    }
    int fd = open(path, flags, mode);
    if (tr_trace_files)
        nx_log("open(\"%s\", %#x) -> %d", path ? path : "(null)", flags, fd);
    return fd;
}

static FILE *my_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (tr_trace_files)
        nx_log("fopen(\"%s\", \"%s\") -> %p", path ? path : "(null)",
               mode ? mode : "", (void *)f);
    if (f && path) {
        const char *base = strrchr(path, '/');
        size_t path_len = strlen(path);
        base = base ? base + 1 : path;
        if (strncmp(base, "scene-", 6) == 0 && path_len >= 4 &&
            strcmp(path + path_len - 4, ".mbs") == 0) {
            /* Every accepted MBS scene stores its plain scene name at this
             * fixed format-v4 offset.  pread observes it without moving the
             * stream the game just opened.  Level Select* deliberately does
             * not match the Level<digit> gameplay contract. */
            char scene_name[64] = {0};
            ssize_t got = pread(fileno(f), scene_name,
                                sizeof scene_name - 1, 145);
            if (got > 0) {
                size_t i;
                for (i = 0; i < (size_t)got && scene_name[i]; i++)
                    if (!isprint((unsigned char)scene_name[i])) {
                        scene_name[i] = '\0';
                        break;
                    }
                if (scene_name[0])
                    tr_input_scene_loaded(scene_name);
            }
        }
    }
    return f;
}

static int my_stat(const char *path, struct stat *st)
{
    int r = stat(path, st);
    BIONIC_TRACE("stat(\"%s\") -> %d", path ? path : "(null)", r);
    return r;
}

static DIR *my_opendir(const char *path)
{
    DIR *d = opendir(path);
    BIONIC_TRACE("opendir(\"%s\") -> %p", path ? path : "(null)", (void *)d);
    return d;
}



static long my_sysconf(int name)
{
    /* bionic and glibc disagree on the _SC_* numbers, and Unity asks for the
     * page size, the core count and the cache line size while sizing its job
     * pool.  Translate the handful that matter and forward the rest. */
    switch (name) {
    case 0x27: return getpagesize();                    /* _SC_PAGESIZE */
    case 0x61: return sysconf(_SC_NPROCESSORS_CONF);    /* _SC_NPROCESSORS_CONF */
    case 0x62: return sysconf(_SC_NPROCESSORS_ONLN);    /* _SC_NPROCESSORS_ONLN */
    case 0x0a: return sysconf(_SC_OPEN_MAX);
    default:   return sysconf(name);
    }
}



static void *my_dlopen(const char *name, int flags);
static void *my_dlsym(void *h, const char *sym);
static int my_dlclose(void *h);
static const char *my_dlerror(void);
static int my_dladdr(const void *addr, void *info);
static int my_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data);

/* Declared in the other translation units of the port. */
extern void *tr_android_sym(const char *name);
extern void *tr_egl_sym(const char *name);
extern void *tr_jni_sym(const char *name);
extern void *tr_protected_sym(const char *name);
extern void *tr_audio_sym(const char *name);

static void *my_memalign(size_t a, size_t n)
{
    void *p = NULL;
    if (a < sizeof(void *))
        a = sizeof(void *);
    if (posix_memalign(&p, a, n) != 0)
        return NULL;
    return p;
}

static size_t my_malloc_usable_size(void *p) { return p ? malloc_usable_size(p) : 0; }

/* Thread-local storage the objects reach through pthread keys.  hxcpp keeps
 * its "which Haxe thread am I" pointer here, and a key that silently loses its
 * value turns into "the main thread is not the main thread" much later. */
/* bionic hands the first application key out well above zero, because its own
 * runtime has already taken the low ones.  glibc in this process has not, so
 * the game's very first key comes back as 0 -- and a zero key reads as "no key
 * yet" to code that stores it in a lazily initialised global.  hxcpp does
 * exactly that for its current-thread pointer, and the symptom is that every
 * lookup builds a fresh thread object: the Haxe main thread stops being
 * recognised and the first frame dies on "Event loop is not available".
 *
 * Burn key 0 once, so the game only ever sees keys a bionic process could
 * have produced. */
/* ------------------------------------------------------- the game's TLS keys
 *
 * Bionic and glibc hand out pthread keys from different pools, and this game
 * leans on the bionic side of that difference in two ways.  Both were measured
 * on hardware, not guessed.
 *
 * 1. It USES key 0 without ever creating one -- `pthread_getspecific(0)` and
 *    `pthread_setspecific(0, ...)` straight out of a lazily initialised global
 *    that reads zero as "already set up".  On Android that is harmless.
 *
 * 2. Under glibc, key 0 belongs to whoever asked first.  On the Amlogic image
 *    nobody asks, so key 0 is free.  On the R36S the Mali driver and SDL take
 *    keys 0, 1 and 2 in their ELF constructors -- before main() runs, so there
 *    is no "get there first".  The game then wrote its own pointer into the
 *    DRIVER's thread slot, and the driver faulted on the next eglMakeCurrent
 *    with a small integer where a surface should have been.
 *
 * So the game's key 0 is not passed through: it is ALIASED to a key of ours,
 * created once at start-up.  If key 0 happens to be free the alias is key 0
 * itself and nothing changes; if it is not, the game gets a private slot and
 * the host library keeps its own.  Every other key the game creates is real
 * and passes straight through.
 */
#define TR_KEYS_MAX 64
static unsigned guest_keys[TR_KEYS_MAX];
static unsigned guest_key_n;
static pthread_key_t key0_alias;
static int key0_alias_ready;

void reserve_zero_key_now(void)
{
    pthread_key_t k;
    if (pthread_key_create(&k, NULL) != 0) {
        nx_log("could not reserve a key for the game's key 0");
        return;
    }
    key0_alias = k;
    key0_alias_ready = 1;
    if (k == 0)
        nx_log("the game's key 0 is glibc key 0: nothing to alias");
    else
        nx_log("the game's key 0 is aliased to glibc key %u "
               "(key 0 was taken before main by a host library)", (unsigned)k);
}

void tr_reserve_zero_key(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, reserve_zero_key_now);
}

/* The game's view of a key number -> the real one. */
static unsigned real_key(unsigned key)
{
    if (key == 0 && key0_alias_ready)
        return (unsigned)key0_alias;
    return key;
}

static int my_pthread_setspecific(unsigned key, const void *value)
{
    return pthread_setspecific(real_key(key), value);
}

static void *my_pthread_getspecific(unsigned key)
{
    return pthread_getspecific(real_key(key));
}

static int my_pthread_key_delete(unsigned key)
{
    return pthread_key_delete(real_key(key));
}

/* Real keys, recorded so the log can say when the game touches a slot it never
 * created -- the symptom that led to the alias above. */
static int my_pthread_key_create(unsigned *key, void (*dtor)(void *))
{
    tr_reserve_zero_key();
    int r = pthread_key_create(key, dtor);
    if (r == 0 && key) {
        if (guest_key_n < TR_KEYS_MAX)
            guest_keys[guest_key_n++] = *key;
        BIONIC_TRACE("pthread_key_create -> key %u (%u so far)", *key,
                     guest_key_n);
    }
    return r;
}


/* The frame limiter shows up here: SDL_Delay is nanosleep, so a frame period
 * that has gone wrong is visible as the sleep the game asks for.
 *
 * [SONOAGG] o frame de ~2,4s da segunda tela e uma soma de sonos PEQUENOS
 * (nenhum pedido grande aparece), entao o que interessa nao e o tamanho de um
 * sono e sim QUEM dorme e quanto ACUMULA.  Agrega por endereco de retorno e
 * despeja a tabela a cada ~3s, com modulo+RVA para nomear o laco no binario.
 * Diagnostico: so fala com TR_VERBOSE=1 (nx_log ja e mudo sem ele). */
static void sleep_note(void *caller, long ms)
{
    static struct { void *caller; unsigned count; unsigned long total_ms;
                    int tid; } tab[24];
    static struct timespec last_report;
    for (int i = 0; i < 24; i++) {
        if (tab[i].caller == caller || tab[i].caller == NULL) {
            tab[i].caller = caller;
            tab[i].count++;
            tab[i].total_ms += (unsigned long)ms;
            tab[i].tid = (int)syscall(SYS_gettid);
            break;
        }
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!last_report.tv_sec) {
        last_report = now;
        return;
    }
    if (now.tv_sec - last_report.tv_sec < 3)
        return;
    last_report = now;
    for (int i = 0; i < 24 && tab[i].caller; i++) {
        const char *mod = "?";
        unsigned long rva = (unsigned long)tab[i].caller;
        static const char *known[] = { "liblime.so", "libApplicationMain.so" };
        for (size_t k = 0; k < sizeof known / sizeof *known; k++) {
            nx_mod *m = nx_find_mod(known[k]);
            if (m && (uint8_t *)tab[i].caller >= m->base &&
                (uint8_t *)tab[i].caller < m->base + m->span) {
                mod = known[k];
                rva = (unsigned long)((uint8_t *)tab[i].caller - m->base);
                break;
            }
        }
        nx_log("[SONOAGG] %s+0x%lx: %u sonos, %lums acumulados (tid %d)",
               mod, rva, tab[i].count, tab[i].total_ms, tab[i].tid);
        tab[i].caller = NULL;
        tab[i].count = 0;
        tab[i].total_ms = 0;
    }
}

/* [SONOAGG] nivel 2: o agregado apontou liblime+0x43a634, que e DENTRO do
 * SDL_Delay (laco nanosleep+EINTR) -- raso demais, todo Delay do jogo passa
 * ali.  O prologo do SDL_Delay desta liblime e fixo (sub sp,#0x50; stp
 * x23,x30,[sp,#32]), entao no instante em que ele chama nanosleep o retorno
 * DELE esta em [sp+0x28].  O trampolim abaixo captura esse valor antes do
 * prologo do C mexer no sp; my_nanosleep so o usa quando o chamador direto e
 * mesmo o interior do SDL_Delay.  Especifico deste build do jogo, e de
 * proposito: e instrumento, nao release. */
static __thread uintptr_t tr_delay_caller;
uintptr_t *tr_delay_caller_slot(void) { return &tr_delay_caller; }

int my_nanosleep(const struct timespec *req, struct timespec *rem);
__asm__(
    ".text\n"
    ".globl my_nanosleep_tramp\n"
    "my_nanosleep_tramp:\n"
    "  stp x0, x1, [sp, #-32]!\n"
    "  stp x2, x30, [sp, #16]\n"
    "  ldr x0, [sp, #32+0x28]\n"     /* LR salvo do SDL_Delay, se for ele */
    "  bl tr_delay_caller_slot\n"
    "  ldr x2, [sp, #32+0x28]\n"
    "  str x2, [x0]\n"
    "  ldp x2, x30, [sp, #16]\n"
    "  ldp x0, x1, [sp], #32\n"
    "  b my_nanosleep\n");
int my_nanosleep_tramp(const struct timespec *req, struct timespec *rem);

int my_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req && (req->tv_sec > 0 || req->tv_nsec > 50000000L))
        BIONIC_TRACE("nanosleep %ld.%09ld", (long)req->tv_sec,
                     (long)req->tv_nsec);
    /* 🚨 CAMINHO QUENTE: nanosleep e chamado milhares de vezes por segundo (os
     * lacos de Delay(1) do SDL).  Todo o diagnostico daqui fica atras de
     * tr_probe_on -- antes ele custava, POR CHAMADA, uma busca de modulo por
     * string, um syscall gettid e um clock_gettime, no jogo inteiro e nao so
     * na tela investigada.  Desligado, sobra um teste de inteiro. */
    if (tr_probe_on && req) {
        static nx_mod *lime;            /* resolvido uma vez, nao por chamada */
        if (!lime)
            lime = nx_find_mod("liblime.so");
        void *caller = __builtin_return_address(0);
        if (lime && (uint8_t *)caller == lime->base + 0x43a634)
            caller = (void *)tr_delay_caller;   /* um nivel acima */
        long ms = req->tv_sec * 1000L + req->tv_nsec / 1000000L;
        /* Os dois baldes sao os lacos identificados pelo [SONOAGG]:
         * 0x4c54bc = WaitEvent (thread principal), 0x48cbf8 = SemWaitTimeout
         * (thread de timer do SDL). */
        uint32_t bucket = TR_SLEEP_OUTRO;
        if (lime) {
            uintptr_t rva = (uintptr_t)((uint8_t *)caller - lime->base);
            if (rva == 0x4c54bc)
                bucket = TR_SLEEP_WAITEVENT;
            else if (rva == 0x48cbf8)
                bucket = TR_SLEEP_TIMER;
        }
        tr_probe_note(TR_PROBE_SLEEP, (uint32_t)ms, bucket);
        sleep_note(caller, ms);
    }
    return nanosleep(req, rem);
}

static int my_usleep(unsigned long us)
{
    if (us > 50000)
        BIONIC_TRACE("usleep %lu", us);
    if (tr_probe_on)
        sleep_note(__builtin_return_address(0), (long)(us / 1000));
    return usleep(us);
}

static FILE *my_fopen(const char *path, const char *mode);
static int my_open(const char *path, int flags, ...);

/* bionic's LFS spellings are the plain functions on a 64-bit ABI. */
static int my_stat64(const char *p, struct stat *st) { return stat(p, st); }
static int my_fstat64(int fd, struct stat *st) { return fstat(fd, st); }
static int my_lstat64(const char *p, struct stat *st) { return lstat(p, st); }
static FILE *my_fopen64(const char *p, const char *m) { return my_fopen(p, m); }
static struct dirent *my_readdir64(DIR *d) { return readdir(d); }

/* bionic-only string helpers. */
static size_t my_strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n), sl = strlen(s);
    if (dl == n)
        return n + sl;
    size_t room = n - dl - 1;
    size_t copy = sl < room ? sl : room;
    memcpy(d + dl, s, copy);
    d[dl + copy] = 0;
    return dl + sl;
}

/* PairIP's interpreter does not report a failed integrity check by returning
 * an error: on the telemetry path it calls memcpy with a count that is a
 * negative signed number, so the process dies inside libc with a stack that
 * says nothing.  A count that large is never a real copy -- the device has
 * 916 MB -- so refuse it, say so, and let the interpreter carry on.  The same
 * guard costs one compare on every other copy the game makes.
 *
 * This is the behaviour PF2 established for the same protection: neutralise
 * the deliberate abort, do not skip any of the work around it. */
#define ABSURD_COPY (1UL << 31)

static void *my_memcpy(void *d, const void *s, size_t n)
{
    if (n >= ABSURD_COPY) {
        nx_log("refused a %#zx-byte memcpy (PairIP's deliberate abort)", n);
        return d;
    }
    return memcpy(d, s, n);
}

static void *my_memmove(void *d, const void *s, size_t n)
{
    if (n >= ABSURD_COPY) {
        nx_log("refused a %#zx-byte memmove (PairIP's deliberate abort)", n);
        return d;
    }
    return memmove(d, s, n);
}

static void *my_memset(void *d, int c, size_t n)
{
    if (n >= ABSURD_COPY) {
        nx_log("refused a %#zx-byte memset (PairIP's deliberate abort)", n);
        return d;
    }
    return memset(d, c, n);
}

/* bionic publishes the stack guard as data; glibc keeps it in the TCB, so the
 * objects get their own copy.  Its only use is the compare in
 * __stack_chk_fail, which we also provide. */
static uintptr_t tr_stack_chk_guard = 0x00000aff;

/* bionic spells these without the glibc underscore prefix. */
static int my_sigsetjmp(void *env, int savemask)
{
    return __sigsetjmp(env, savemask);
}
static long my_lrint(double x) { return lrint(x); }
static long my_lrintf(float x) { return lrintf(x); }
static float my_sinhf(float x) { return sinhf(x); }
static char *my_stpcpy(char *d, const char *s) { return stpcpy(d, s); }
static int my___isfinitef(float x) { return isfinite(x); }
static void *my_reallocarray(void *ptr, size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, count * size);
}

/* ---------------------------------------------------------- import table */

#define E(n)      { #n, (void *)(uintptr_t)n }
#define M(n)      { #n, (void *)(uintptr_t)my_##n }
#define A(n, f)   { n,  (void *)(uintptr_t)(f) }

static nx_import tab[] = {
    /* logging / properties / bionic-only */
    M(__android_log_print), M(__android_log_write), M(__android_log_vprint),
    M(__android_log_assert), M(android_set_abort_message),
    M(__system_property_get), M(__system_property_find),
    M(__system_property_read), M(__system_property_read_callback),
    M(__system_property_foreach),
    M(__errno), M(__stack_chk_fail),
    M(__cxa_atexit), M(__cxa_finalize), M(__cxa_pure_virtual),
    A("__register_atfork", my___register_atfork),
    A("__sF", tr_sF), A("_ctype_", &tr_ctype_ptr),
    A("__stack_chk_guard", &tr_stack_chk_guard),
    A("environ", &environ),
    M(sigsetjmp), M(lrint), M(lrintf), M(sinhf), M(stpcpy), M(__isfinitef),
    M(stat64), M(fstat64), M(lstat64), M(fopen64), M(readdir64), M(strlcat),
    E(wcsncpy), E(wcstombs), E(atof), E(alarm), E(isgraph), E(iscntrl),
    E(getchar), E(times), E(inet_ntoa), E(execvp), E(waitpid), E(fork),
    E(gethostbyname_r),
    E(pthread_getschedparam), E(pthread_setschedparam),
    M(__ctype_get_mb_cur_max),

    /* FORTIFY */
    M(__memcpy_chk), M(__memmove_chk), M(__memset_chk), M(__strlen_chk),
    M(__strcpy_chk), M(__strcat_chk), M(__strncpy_chk), M(__strncat_chk),
    M(__vsnprintf_chk), M(__snprintf_chk), M(__vsprintf_chk), M(__sprintf_chk),
    M(__FD_SET_chk), M(__FD_CLR_chk), M(__FD_ISSET_chk), M(__read_chk),

    /* stdio through the __sF translation */
    M(fprintf), M(vfprintf), M(fputc), M(fputs), M(fgetc), M(fgets),
    M(fwrite), M(fread), M(fflush), M(fclose), M(feof), M(ferror),
    M(clearerr), M(fileno), M(setbuf), M(setvbuf), M(fseek), M(ftell),
    M(rewind), M(ungetc), M(vprintf),
    M(fopen), E(fdopen), E(freopen), E(printf), E(snprintf), E(sprintf),
    E(vsnprintf), E(vsprintf), E(asprintf), E(vasprintf), E(sscanf),
    E(vsscanf), E(fscanf), E(puts), E(putchar), E(remove), E(rename), E(tmpfile),
    E(fseeko), E(ftello), E(getline), E(getdelim), E(swprintf),

    /* memory */
    E(malloc), E(free), E(calloc), E(realloc), E(posix_memalign),
    A("memalign", my_memalign), A("malloc_usable_size", my_malloc_usable_size),
    E(aligned_alloc), E(valloc), M(reallocarray),

    /* strings */
    M(memcpy), M(memmove), M(memset), E(memcmp), E(memchr), E(memrchr),
    E(mempcpy), E(strlen), E(strnlen), E(strcpy), E(strncpy), E(strcat),
    E(strncat), E(strcmp), E(strncmp), E(strcasecmp), E(strncasecmp),
    E(strchr), E(strrchr), E(strstr), E(strcasestr), E(strdup), E(strndup),
    E(strlcpy),
    E(strtok), E(strtok_r), E(strspn), E(strcspn), E(strpbrk), E(strerror),
    E(strerror_r), E(strsignal), E(strsep), E(strcoll), E(strxfrm),
    E(basename), E(dirname),
    E(wmemcpy), E(wmemmove), E(wmemset), E(wmemcmp), E(wmemchr),
    E(wcslen), E(wcscpy), E(wcscmp), E(wcsncmp), E(wcschr), E(wcsrchr),
    E(wcsstr), E(wcsdup), E(wcrtomb), E(wcsnrtombs), E(wcsrtombs),
    E(mbrlen), E(mbrtowc), E(mbsrtowcs), E(mbsnrtowcs), E(mbtowc), E(mblen),
    E(wctomb), E(btowc), E(wctob),

    /* conversion / locale */
    E(atoi), E(atol), E(atoll), E(strtol), E(strtoll), E(strtoul),
    E(strtoull), E(strtof), E(strtod), E(strtold), E(strtoll_l),
    E(strtoull_l), E(strtold_l), E(abs), E(labs), E(llabs), E(div), E(ldiv),
    E(lldiv), E(qsort), E(bsearch), E(newlocale), E(freelocale),
    E(duplocale), E(uselocale), E(setlocale), E(localeconv),
    E(strcoll_l), E(strxfrm_l), E(wcscoll_l), E(wcsxfrm_l),
    E(isdigit_l), E(islower_l), E(isupper_l), E(isxdigit_l), E(iswlower_l),
    E(iswupper_l), E(iswprint_l), E(iswspace_l), E(iswalpha_l),
    E(iswdigit_l), E(iswxdigit_l), E(iswblank_l), E(iswcntrl_l),
    E(iswpunct_l), E(toupper_l), E(tolower_l), E(towupper_l), E(towlower_l),
    E(towupper), E(towlower),
    E(isspace), E(isdigit), E(isalpha),
    E(isalnum), E(isupper), E(islower), E(isprint), E(ispunct),
    E(isxdigit), E(toupper), E(tolower), E(strftime), E(strftime_l),
    E(wcstol), E(wcstoul), E(wcstoll), E(wcstoull), E(wcstof), E(wcstod),
    E(wcstold),

    /* process / signals / time */
    E(abort), E(exit), E(_exit), E(atexit), E(getenv), E(setenv), E(putenv),
    E(unsetenv), E(system), E(raise), E(signal), E(setjmp), E(longjmp),
    E(siglongjmp), E(sigaltstack), E(getpid), E(gettid), E(getppid), E(getuid),
    E(geteuid), E(getgid), E(getegid), E(getpwuid_r), E(getpwuid),
    E(getpagesize), E(setpriority), E(getpriority),
    E(prctl), E(syscall), E(uname), E(getrusage), E(getrlimit), E(setrlimit),
    E(sched_yield), E(sched_getaffinity), E(sched_setaffinity),
    E(sched_get_priority_min), E(sched_get_priority_max),
    E(gettimeofday), E(clock_gettime), E(clock_getres),
    { "nanosleep", (void *)(uintptr_t)my_nanosleep_tramp },
    M(usleep), E(sleep), E(time), E(clock), E(localtime), E(localtime_r),
    E(gmtime), E(gmtime_r), E(mktime), E(timegm), E(difftime),
    E(srand48), E(lrand48), E(drand48), E(rand), E(srand), E(rand_r),
    E(random), E(srandom), E(utime), E(utimes), E(gethostname),
    M(sigaction), M(sigemptyset), M(sigfillset), M(sigaddset), M(sigdelset),
    M(sigismember), M(sigsuspend), M(ptrace), M(sysconf),
    M(getauxval), M(stat), M(opendir),

    /* files */
    M(open), A("open64", my_open), E(openat), E(close), E(read), E(write), E(pread),
    E(pwrite), E(pread64), E(pwrite64), E(readv), E(writev), E(lseek),
    E(lseek64), E(fstat), E(lstat), E(fstatat), E(statfs),
    E(fstatfs), E(statvfs), E(fsync), E(fdatasync), E(ftruncate),
    E(truncate), E(access), E(faccessat), E(mkdir), E(mkdirat), E(rmdir),
    E(unlink), E(unlinkat), E(link), E(readlink), E(symlink), E(chmod),
    E(fchmod), E(futimens), E(sendfile),
    E(umask), E(chown), E(readdir), E(closedir), E(rewinddir),
    E(dirfd), E(realpath), E(getcwd), E(chdir), E(dup), E(dup2), E(dup3),
    E(pipe), E(pipe2), E(fcntl), E(ioctl), E(isatty), E(poll), E(select), E(flock),
    E(fnmatch), E(mmap), E(mmap64), E(munmap), E(mprotect), E(madvise),
    E(msync), E(mremap),

    /* sockets */
    E(socket), E(connect), E(bind), E(listen), E(accept), E(send), E(recv),
    E(sendto), E(recvfrom), E(sendmsg), E(recvmsg), E(setsockopt),
    E(getsockopt), E(shutdown),
    E(getsockname), E(getpeername), E(inet_addr), E(inet_pton), E(inet_ntop),
    E(gethostbyname), E(gethostbyaddr), E(getaddrinfo), E(freeaddrinfo),
    E(getnameinfo), E(gai_strerror), E(if_nametoindex), E(htons), E(htonl),
    E(ntohs), E(ntohl),

    /* syslog (libunity links it but never enables it) */
    E(openlog), E(closelog), E(syslog),

    /* libm */
    E(acos), E(acosf), E(asin), E(asinf), E(atan), E(atanf), E(atan2),
    E(atan2f), E(cos), E(cosf), E(sin), E(sinf), E(tan), E(tanf), E(cosh),
    E(sinh), E(tanh), E(acosh), E(asinh), E(atanh), E(exp), E(expf), E(exp2),
    E(exp2f), E(expm1), E(log), E(logf), E(log2), E(log2f), E(log10),
    E(log10f), E(log1p), E(logb), E(ilogb), E(pow), E(powf), E(sqrt),
    E(sqrtf), E(cbrt), E(cbrtf), E(hypot), E(hypotf), E(fmod), E(fmodf),
    E(modf), E(modff), E(frexp), E(frexpf), E(ldexp), E(ldexpf), E(scalbn),
    E(scalbnf), E(ceil), E(ceilf), E(floor), E(floorf), E(round), E(roundf),
    E(trunc), E(truncf), E(rint), E(nearbyint), E(fabs), E(fabsf), E(fmin),
    E(fmax), E(fdim), E(copysign), E(lgamma), E(tgamma), E(erf), E(erfc),
    E(remainder), E(remquo), E(sincos), E(sincosf), E(nan), E(finite),

    /* zlib -- libunity links libz for its bundle reader */
    E(inflate), E(inflateInit_), E(inflateInit2_), E(inflateEnd),
    E(inflateReset), E(inflateSetDictionary), E(inflateCopy), E(inflateSync),
    E(deflate), E(deflateInit_), E(deflateInit2_), E(deflateEnd),
    E(deflateReset), E(deflateBound), E(compress), E(compress2),
    E(uncompress), E(crc32), E(adler32), E(zlibVersion), E(zError),

    /* pthread calls whose objects are plain scalars on arm64: pthread_t is a
     * pointer and pthread_key_t an unsigned int in both libcs, so no bridge. */
    E(pthread_self), E(pthread_join), E(pthread_detach), E(pthread_equal),
    E(pthread_exit), E(pthread_kill), M(pthread_key_create),
    M(pthread_key_delete), M(pthread_getspecific), M(pthread_setspecific),
    M(pthread_sigmask), M(pthread_atfork), E(pthread_getcpuclockid),

    /* libdl -- our own, so the game only ever sees modules we loaded */
    M(dlopen), M(dlsym), M(dlclose), M(dlerror), M(dladdr), M(dl_iterate_phdr),
};

/* Modules whose exports we hand out through the fake dlopen/dlsym. */
static const char *const fake_libs[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
    "libandroid.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so",
    "libOpenSLES.so", "libGLESv1_CM.so",
    "liblime.so", "libApplicationMain.so", "libpairipcore.so",
};
#define FAKE_HANDLE(i) ((void *)(uintptr_t)(0xD1000000u + (unsigned)(i)))

static void *my_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name || !*name)
        return FAKE_HANDLE(0);            /* self / global scope */
    const char *slash = strrchr(name, '/');
    const char *b = slash ? slash + 1 : name;
    for (size_t i = 0; i < sizeof fake_libs / sizeof *fake_libs; i++)
        if (strcmp(fake_libs[i], b) == 0) {
            BIONIC_TRACE("dlopen(\"%s\") -> handle %zu", name, i);
            return FAKE_HANDLE(i);
        }
    nx_log("dlopen(%s) -> global scope", name);
    return FAKE_HANDLE(0);
}

/* [FRAMERATE] a segunda tela roda a 1 frame/2,4s porque o pacing do Lime
 * passa a acreditar num frameRate de ~0,42.  O jogo chega no setter por
 * dlsym("lime_application_set_frame_rate*"), que passa por aqui -- entao da
 * para VER o valor pedido e, se vier podre, corrigir na fonte.  O clamp so
 * age abaixo de 30: o telefone roda essa mesma tela lisa, entao qualquer
 * pedido baixo desses e defeito do ambiente, nao intencao do jogo. */
static double (*real_sfr_prime)(void *, double);
void *tr_lime_app;                     /* handle do Application, p/ o watchdog */
static double sfr_prime_wrap(void *app, double fps)
{
    tr_lime_app = app;
    nx_log("[FRAMERATE] set_frame_rate(%f)", fps);
    if (fps < 30.0) {
        nx_log("[FRAMERATE] valor podre %f -> cravando 60", fps);
        fps = 60.0;
    }
    return real_sfr_prime(app, fps);
}

/* Re-crava 60 no pacing do Lime.  Chamado pelo watchdog do metronomo quando o
 * jogo fica sem apresentar frame: reseta framePeriod E nextUpdate por dentro,
 * o que mata qualquer deriva que o pacing tenha acumulado. */
int tr_force_frame_rate_60(void)
{
    if (!real_sfr_prime || !tr_lime_app)
        return 0;
    real_sfr_prime(tr_lime_app, 60.0);
    return 1;
}

static void *(*real_sfr_value)(void *, void *);
static void *sfr_value_wrap(void *app, void *fps_value)
{
    nx_log("[FRAMERATE] set_frame_rate(value %p) via variante nao-prime",
           fps_value);
    return real_sfr_value(app, fps_value);
}

static void *my_dlsym(void *h, const char *sym)
{
    (void)h;
    if (!sym)
        return NULL;
    /* Data symbols the PairIP VM looks up while it resolves libunity's GOT. */
    if (strcmp(sym, "environ") == 0)
        return &environ;
    if (strcmp(sym, "lime_application_set_frame_rate__prime") == 0) {
        nx_mod *lime = nx_find_mod("liblime.so");
        if (lime && !real_sfr_prime)
            real_sfr_prime = nx_lookup_in(lime, sym);
        if (real_sfr_prime) {
            nx_log("[FRAMERATE] interceptando %s", sym);
            return (void *)sfr_prime_wrap;
        }
    }
    if (strcmp(sym, "lime_application_set_frame_rate") == 0 ||
        strcmp(sym, "lime_application_set_frame_rate__2") == 0) {
        nx_mod *lime = nx_find_mod("liblime.so");
        if (lime && !real_sfr_value)
            real_sfr_value = nx_lookup_in(lime, sym);
        if (real_sfr_value) {
            nx_log("[FRAMERATE] interceptando %s", sym);
            return (void *)sfr_value_wrap;
        }
    }
    void *a = nx_resolve_import(sym);
    if (!a) a = tr_android_sym(sym);
    if (!a) a = tr_egl_sym(sym);
    if (!a) a = tr_jni_sym(sym);
    if (!a) a = tr_protected_sym(sym);
    if (!a) a = tr_audio_sym(sym);
    if (!a) a = nx_lookup(sym);
    if (!a)
        nx_log("dlsym(%s) -> NULL", sym);
    else
        /* Traced on success too, not just on failure: the VM resolves what it
         * needs through here, so a wrong answer is indistinguishable from a
         * missing one in the crash that follows, and only the trace separates
         * them. */
        BIONIC_TRACE("dlsym(\"%s\") -> %p", sym, a);
    return a;
}

static int my_dlclose(void *h) { (void)h; return 0; }
static const char *my_dlerror(void) { return NULL; }

static int my_dladdr(const void *addr, void *info)
{
    struct { const char *fname; void *fbase; const char *sname; void *saddr; } *i = info;
    extern const char *tr_mod_at(const void *addr, void **base_out);
    void *base = NULL;
    const char *name = tr_mod_at(addr, &base);

    BIONIC_TRACE("dladdr(%p) -> %s", addr, name ? name : "(none)");
    if (!i || !name)
        return 0;                 /* zero is failure, and then info is untouched */
    i->fname = name;
    i->fbase = base;
    i->sname = NULL;              /* bionic leaves these null for an address
                                   * that is not exactly a symbol */
    i->saddr = NULL;
    return 1;                     /* nonzero is success */
}

static int my_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data)
{
    /* PairIP walks the link map to find the module it is patching, so the
     * modules we loaded have to be visible here. */
    extern int tr_iterate_mods(int (*cb)(void *, size_t, void *), void *data);
    return tr_iterate_mods(cb, data);
}

/* --------------------------------------------------------------- publish */

static int cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

void tr_bionic_init(void)
{
    qsort(tab, sizeof tab / sizeof *tab, sizeof *tab, cmp);
    nx_log("bionic table: %zu symbols", sizeof tab / sizeof *tab);
}

size_t tr_bionic_count(void) { return sizeof tab / sizeof *tab; }

nx_import *tr_bionic_entries(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}
