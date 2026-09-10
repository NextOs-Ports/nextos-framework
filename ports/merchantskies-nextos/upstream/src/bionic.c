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
#include "language.h"
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
#include <sys/inotify.h>
#include <sys/file.h>
#include <sys/prctl.h>
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
#include "gb.h"

/* ------------------------------------------------------------------ logging */

int st_log_level = 0;   /* ST_LOGCAT=1 mirrors the game's own log */

static const char lvl[] = "?????VDIWEFS";

int my___android_log_write(int prio, const char *tag, const char *msg)
{
    if (st_log_level)
        fprintf(stderr, "[%c/%s] %s\n", lvl[prio & 15], tag ? tag : "?",
                msg ? msg : "");
    return 0;
}

int my___android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    if (!st_log_level)
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
    fprintf(stderr, "[st] abort message: %s\n", m ? m : "(null)");
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
    { "ro.build.version.sdk",        "31" },   /* overridden by st_api_level */
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
    { "persist.sys.locale",          "" },  /* V3 b6: filled from snapshot */
    { "ro.product.locale",           "" },  /* V3 b6: filled from snapshot */
};

/* Verbose-only diagnostics for Android/libc compatibility calls. */
#define BIONIC_TRACE(...) nx_log(__VA_ARGS__)

/* The Android API level this host claims.  It is ONE number: the native
 * property and android/os/Build$VERSION.SDK_INT must agree, because Unity
 * reads the property from native code and the field from managed code and
 * would otherwise take two different code paths in the same process.
 *
 * It matters for audio.  Unity's embedded FMOD selects its output backend by
 * API level: >= 26 picks "FMOD AAudio Output", below that it picks "FMOD
 * OpenSL ES Output".  This host has no AAudio at all, and it does implement a
 * full OpenSL ES bridge, so the level we report is what routes FMOD onto a
 * backend that actually exists.  ST_API_LEVEL overrides it for bring-up. */
int st_api_level(void)
{
    static int cached;
    if (!cached) {
        const char *v = getenv("ST_API_LEVEL");
        long n = (v && *v) ? strtol(v, NULL, 10) : 0;
        cached = (n >= 16 && n <= 40) ? (int)n : 25;
    }
    return cached;
}

int my___system_property_get(const char *key, char *value)
{
    BIONIC_TRACE("__system_property_get(\"%s\")", key ? key : "(null)");
    if (key && strcmp(key, "ro.build.version.sdk") == 0)
        return snprintf(value, 8, "%d", st_api_level());
    /* V3 blocker 6: the locale properties come from the ONE canonical
     * snapshot, not a hardcoded string, so they can never disagree with the
     * Java Locale or the Netflix preferred language. */
    if (key && (strcmp(key, "persist.sys.locale") == 0 ||
                strcmp(key, "ro.product.locale") == 0))
        return (int)strlen(strcpy(value, mos_language_get()->tag));
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
    const char *v;
    if (!p)
        return 0;
    /* V3 blocker 6: locale keys resolve to the canonical snapshot. */
    v = p->v;
    if (strcmp(p->k, "persist.sys.locale") == 0 ||
        strcmp(p->k, "ro.product.locale") == 0)
        v = mos_language_get()->tag;
    if (name)
        strcpy(name, p->k);
    if (value)
        strcpy(value, v);
    return (int)strlen(v);
}

void my___system_property_read_callback(const void *pi,
                                        void (*cb)(void *, const char *,
                                                   const char *, uint32_t),
                                        void *ck)
{
    const struct { const char *k, *v; } *p = pi;
    if (p && cb) {
        const char *v = p->v;
        if (strcmp(p->k, "persist.sys.locale") == 0 ||
            strcmp(p->k, "ro.product.locale") == 0)
            v = mos_language_get()->tag; /* V3 blocker 6: canonical snapshot */
        cb(ck, p->k, v, 1);
    }
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
bionic_FILE st_sF[3];

static FILE *xf(void *f)
{
    if (f == (void *)&st_sF[0])
        return stdin;
    if (f == (void *)&st_sF[1])
        return stdout;
    if (f == (void *)&st_sF[2])
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
static int my_fclose(void *f)               { return f == (void *)st_sF ? 0 : fclose(xf(f)); }
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
static char *my___strchr_chk(const char *s, int c, size_t sl)
{
    (void)sl;
    /* bionic returns char*; the host prototype is const-correct. */
    return (char *)strchr(s, c);
}
static mode_t my___umask_chk(mode_t mask) { return umask(mask); }

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

static void my___stack_chk_fail(void)
{
    /* This should never be reached.  Keep the Android caller visible in the
     * fatal message, since glibc's unwinder does not know about nx_elf's
     * manually mapped modules. */
    void *caller = __builtin_return_address(0);
    void *base = NULL;
    extern const char *st_mod_at(const void *addr, void **base_out);
    const char *module = st_mod_at(caller, &base);
    if (module && base)
        nx_die("__stack_chk_fail caller=%p %s+%#zx", caller, module,
               (size_t)((const unsigned char *)caller -
                        (const unsigned char *)base));
    nx_die("__stack_chk_fail caller=%p", caller);
}

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
static const unsigned char st_ctype_tab[1 + 256] = { 0 };
static const unsigned char *st_ctype_ptr = st_ctype_tab + 1;

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
extern void *st_android_sym(const char *name);
extern void *st_egl_sym(const char *name);
extern void *st_jni_sym(const char *name);

static void *my_memalign(size_t a, size_t n)
{
    void *p = NULL;
    if (a < sizeof(void *))
        a = sizeof(void *);
    if (posix_memalign(&p, a, n) != 0)
        return NULL;
    return p;
}

/* glibc só ganhou o wrapper reallocarray em 2.26; o piso universal é 2.17. */
static void *my_reallocarray(void *p, size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, nmemb * size);
}

static size_t my_malloc_usable_size(void *p) { return p ? malloc_usable_size(p) : 0; }

/* strlcpy only entered glibc in 2.38, while ArkOS/R36S ships glibc 2.30.
 * Android exposes it on every supported API level, so keep the bionic-facing
 * implementation inside the loader instead of inheriting the host libc
 * version.  The return value is the full source length, including when the
 * destination is truncated. */
static size_t my_strlcpy(char *dst, const char *src, size_t size)
{
    size_t source_length = strlen(src);
    if (size != 0) {
        size_t copy_length = source_length < size - 1 ? source_length : size - 1;
        memcpy(dst, src, copy_length);
        dst[copy_length] = '\0';
    }
    return source_length;
}

/* gettid was exported by glibc only from 2.30 onward.  The kernel ABI is the
 * same syscall on every AArch64 firmware we target, and this avoids making the
 * universal build depend on a libc wrapper merely because it exists. */
static pid_t my_gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}


/* bionic __assert2: newer il2cpp builds import it directly. */
static void my___assert2(const char *file, int line, const char *function,
                         const char *failed)
{
    nx_die("assert2 %s:%d %s: %s", file ? file : "?", line,
           function ? function : "?", failed ? failed : "?");
}

/* ---------------------------------------------------------- import table */


/* ---- signal-raise tracing -------------------------------------------------
 * Unity 6 boots and then the process sends ITSELF SIGSEGV (si_code=SI_TKILL).
 * libunity imports raise(); libil2cpp imports pthread_kill()/abort().  These
 * wrappers name the caller by return address so the shooter can be placed
 * against the module bases printed by the fault handler. */
static int my_raise(int sig)
{
    fprintf(stderr, "[st/sig] raise(%d) from ra=%p\n", sig,
            __builtin_return_address(0));
    fflush(stderr);
    return raise(sig);
}

static void my_abort(void)
{
    fprintf(stderr, "[st/sig] abort() from ra=%p\n",
            __builtin_return_address(0));
    fflush(stderr);
    abort();
}

static int my_pthread_kill_traced(void *th, int sig)
{
    fprintf(stderr, "[st/sig] pthread_kill(%p,%d) from ra=%p\n", th, sig,
            __builtin_return_address(0));
    fflush(stderr);
    return pthread_kill((pthread_t)(uintptr_t)th, sig);
}


/* aarch64: 130=tkill, 131=tgkill, 129=kill.  Unity reaches them through the
 * variadic syscall() rather than raise(), so this is the only place the
 * shooter can be named. */
static long my_syscall(long n, ...)
{
    va_list ap;
    va_start(ap, n);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);
    if (n == 129 || n == 130 || n == 131)
        fprintf(stderr, "[st/sig] syscall(%ld) kill-family a=%ld b=%ld c=%ld "
                        "from ra=%p\n",
                n, a, b, c, __builtin_return_address(0)), fflush(stderr);
    return syscall(n, a, b, c, d, e, f);
}


/* ---- pthread key 0 ---------------------------------------------------------
 * In a bionic process the low TLS keys are already taken by the Android
 * runtime, so an app's first pthread_key_create NEVER returns 0.  Under glibc
 * inside the so-loader it can, and guest code that caches the key in a lazily
 * initialised global (`if (!key) create();`) reads 0 as "not created yet".
 * Reserve the first key for ourselves before the guest ever asks, and alias
 * the guest's key 0 to it if a host library had already taken key 0.
 * Ported from ports/tightrope/src/bionic.c (finished port). */
static pthread_key_t key0_alias;
static int key0_alias_ready;

static void reserve_zero_key_now(void)
{
    pthread_key_t k;
    if (pthread_key_create(&k, NULL) != 0) {
        nx_log("could not reserve a key for the guest's key 0");
        return;
    }
    key0_alias = k;
    key0_alias_ready = 1;
    if (k == 0)
        nx_log("reserved glibc key 0; the guest can no longer be handed 0");
    else
        nx_log("key 0 was already taken before main; aliasing guest key 0 "
               "to glibc key %u", (unsigned)k);
}

static void st_reserve_zero_key(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, reserve_zero_key_now);
}

static unsigned real_key(unsigned key)
{
    if (key == 0 && key0_alias_ready)
        return (unsigned)key0_alias;
    return key;
}

static int my_pthread_key_create(unsigned *key, void (*dtor)(void *))
{
    st_reserve_zero_key();
    int r = pthread_key_create(key, dtor);
    BIONIC_TRACE("pthread_key_create -> r=%d key=%u", r, key ? *key : 0u);
    return r;
}

static int my_pthread_key_delete(unsigned key)
{
    return pthread_key_delete(real_key(key));
}

static int st_tls_log;

static int my_pthread_setspecific(unsigned key, const void *value)
{
    /* Unity keeps the GfxDevice in TLS and reads it with pthread_getspecific;
     * "Graphics device is null." means this thread's slot is empty.  Tracing
     * key/value/tid answers the only two questions that matter: was it ever
     * stored, and by which thread. */
    if (st_tls_log && value)
        fprintf(stderr, "[st/tls] set key=%u value=%p tid=%d\n", key,
                (void *)value, (int)syscall(SYS_gettid));
    return pthread_setspecific(real_key(key), value);
}

void st_tls_trace_enable(void) { st_tls_log = 1; }

/* Read a TLS slot on the CALLING thread with the guest's own key. */
void *st_tls_peek(unsigned key) { return pthread_getspecific(real_key(key)); }

static void *my_pthread_getspecific(unsigned key)
{
    return pthread_getspecific(real_key(key));
}


/* ---- data symbols the Unity 6 NDK imports ---------------------------------
 * Unity 2022 (Bomb Chicken) reached the standard streams through bionic's
 * `FILE __sF[3]`.  The newer NDK behind Unity 6 imports `stdin`/`stdout`/
 * `stderr` as DATA symbols instead: the guest loads the GOT slot to get the
 * address of the variable and then dereferences it for the FILE*.  Leaving
 * them unresolved parks a 0 in the GOT, and libunity dies on `ldr x8,[x8]`
 * with a NULL deref that looks nothing like a missing import.
 *
 * These variables hold the same placeholders xf() already translates, so the
 * whole stdio surface keeps working unchanged. */
void *st_stdin_ptr  = (void *)&st_sF[0];
void *st_stdout_ptr = (void *)&st_sF[1];
void *st_stderr_ptr = (void *)&st_sF[2];

/* glibc keeps the *64 names as aliases on LP64; the guest still imports them. */
static int my_stat64(const char *p, struct stat *st)   { return stat(p, st); }
static int my_fstat64(int fd, struct stat *st)         { return fstat(fd, st); }
static int my_statfs64(const char *p, struct statfs *b){ return statfs(p, b); }

/* bionic's __gnu_strerror_r is the GNU-flavoured strerror_r (returns char*). */
static char *my___gnu_strerror_r(int e, char *buf, size_t n)
{
    return strerror_r(e, buf, n);
}

#include "nx_symver.h"

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
    A("__assert2", my___assert2),
    M(__cxa_atexit), M(__cxa_finalize), M(__cxa_pure_virtual),
    A("__register_atfork", my___register_atfork),
    A("__sF", st_sF), A("_ctype_", &st_ctype_ptr),
    A("environ", &environ),
    M(__ctype_get_mb_cur_max),

    /* FORTIFY */
    M(__memcpy_chk), M(__memmove_chk), M(__memset_chk), M(__strlen_chk),
    M(__strcpy_chk), M(__strcat_chk), M(__strncpy_chk), M(__strncat_chk),
    M(__strchr_chk), M(__umask_chk),
    M(__vsnprintf_chk), M(__snprintf_chk), M(__vsprintf_chk), M(__sprintf_chk),
    M(__FD_SET_chk), M(__FD_CLR_chk), M(__FD_ISSET_chk), M(__read_chk),

    /* stdio through the __sF translation */
    M(fprintf), M(vfprintf), M(fputc), M(fputs), M(fgetc), M(fgets),
    M(fwrite), M(fread), M(fflush), M(fclose), M(feof), M(ferror),
    M(clearerr), M(fileno), M(setbuf), M(setvbuf), M(fseek), M(ftell),
    M(rewind), M(ungetc), M(vprintf),
    E(fopen), E(fdopen), E(freopen), E(printf), E(snprintf), E(sprintf),
    E(vsnprintf), E(vsprintf), E(asprintf), E(vasprintf), E(sscanf),
    E(vsscanf), E(fscanf), E(puts), E(putchar), E(remove), E(rename), E(tmpfile),
    E(fseeko), E(ftello), E(getline), E(getdelim), E(swprintf),

    /* memory */
    E(malloc), E(free), E(calloc), E(realloc), E(posix_memalign),
    A("memalign", my_memalign), A("malloc_usable_size", my_malloc_usable_size),
    E(aligned_alloc), E(valloc), A("reallocarray", my_reallocarray),

    /* strings */
    E(memcpy), E(memmove), E(memset), E(memcmp), E(memchr), E(memrchr),
    E(mempcpy), E(strlen), E(strnlen), E(strcpy), E(strncpy), E(strcat),
    E(strncat), E(strcmp), E(strncmp), E(strcasecmp), E(strncasecmp),
    E(strchr), E(strrchr), E(strstr), E(strcasestr), E(strdup), E(strndup),
    M(strlcpy),
    E(strtok), E(strtok_r), E(strspn), E(strcspn), E(strpbrk), E(strerror),
    E(strerror_r), { "__gnu_strerror_r", (void *)(uintptr_t)my___gnu_strerror_r },
    { "stdin",  (void *)(uintptr_t)&st_stdin_ptr },
    { "stdout", (void *)(uintptr_t)&st_stdout_ptr },
    { "stderr", (void *)(uintptr_t)&st_stderr_ptr },
    E(strsignal), E(strsep), E(strcoll), E(strxfrm),
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
    E(towupper), E(towlower), E(iswalpha), E(iswcntrl), E(iswdigit),
    E(iswlower), E(iswprint), E(iswpunct), E(iswspace), E(iswupper),
    E(iswxdigit),
    E(isspace), E(isdigit), E(isalpha),
    E(isalnum), E(isupper), E(islower), E(isprint), E(ispunct),
    E(isxdigit), E(toupper), E(tolower), E(strftime), E(strftime_l),
    E(wcstol), E(wcstoul), E(wcstoll), E(wcstoull), E(wcstof), E(wcstod),
    E(wcstold), E(wcscoll), E(wcsxfrm), E(iswblank),

    /* process / signals / time */
    { "abort", (void *)(uintptr_t)my_abort }, E(exit), E(_exit), E(atexit), E(getenv), E(setenv), E(putenv),
    E(unsetenv), E(system), { "raise", (void *)(uintptr_t)my_raise }, E(signal), E(setjmp), E(longjmp),
    E(siglongjmp), E(sigaltstack), E(getpid), M(gettid), E(getppid), E(getuid),
    E(geteuid), E(getgid), E(getegid), E(getpwuid_r), E(getpwuid),
    E(getpagesize), E(setpriority), E(getpriority),
    E(prctl), { "syscall", (void *)(uintptr_t)my_syscall }, E(uname), E(getrusage), E(getrlimit), E(setrlimit),
    E(sched_yield), E(sched_getaffinity), E(sched_setaffinity),
    E(sched_get_priority_min), E(sched_get_priority_max),
    E(gettimeofday), E(clock_gettime), E(clock_getres), E(nanosleep),
    E(usleep), E(sleep), E(time), E(clock), E(localtime), E(localtime_r),
    E(gmtime), E(gmtime_r), E(mktime), E(timegm), E(difftime),
    E(srand48), E(lrand48), E(drand48), E(rand), E(srand), E(rand_r),
    E(random), E(srandom), E(utime), E(utimes), E(gethostname),
    M(sigaction), M(sigemptyset), M(sigfillset), M(sigaddset), M(sigdelset),
    M(sigismember), M(sigsuspend), M(ptrace), M(sysconf),
    M(getauxval), M(stat), M(opendir),

    /* files */
    E(open), E(open64), E(openat), E(close), E(read), E(write), E(pread),
    E(pwrite), E(pread64), E(pwrite64), E(readv), E(writev), E(lseek),
    E(lseek64), E(fstat), E(lstat), E(fstatat), E(statfs),
    { "stat64", (void *)(uintptr_t)my_stat64 },
    { "fstat64", (void *)(uintptr_t)my_fstat64 },
    { "statfs64", (void *)(uintptr_t)my_statfs64 },
    E(process_vm_readv),
    E(fstatfs), E(statvfs), E(fsync), E(fdatasync), E(ftruncate),
    E(truncate), E(access), E(faccessat), E(mkdir), E(mkdirat), E(rmdir),
    E(unlink), E(unlinkat), E(link), E(readlink), E(symlink), E(chmod),
    E(fchmod), E(futimens), E(sendfile),
    E(umask), E(chown), E(readdir), E(closedir), E(rewinddir),
    E(dirfd), E(realpath), E(getcwd), E(chdir), E(dup), E(dup2), E(dup3),
    E(pipe), E(pipe2), E(fcntl), E(ioctl), E(isatty), E(poll), E(select), E(flock),
    E(inotify_init), E(inotify_add_watch),
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
    E(atan2f), E(cos), E(cosf), E(sin), E(sinf), E(tan), E(tanf), { "cosh", (void *)(uintptr_t)NX_SYMVER_COSH },
    { "sinh", (void *)(uintptr_t)NX_SYMVER_SINH }, E(tanh), E(tanhf), E(acosh), E(asinh), E(atanh), E(exp), E(expf), E(exp2),
    E(exp2f), E(expm1), E(log), E(logf), E(log2), E(log2f), E(log10),
    E(log10f), E(log1p), E(logb), E(ilogb), E(pow), E(powf), E(sqrt),
    E(sqrtf), E(cbrt), E(cbrtf), E(hypot), E(hypotf), E(fmod), E(fmodf),
    E(modf), E(modff), E(frexp), E(frexpf), E(ldexp), E(ldexpf), E(scalbn),
    E(scalbnf), E(ceil), E(ceilf), E(floor), E(floorf), E(round), E(roundf),
    E(trunc), E(truncf), E(rint), E(nearbyint), E(fabs), E(fabsf), E(fmin),
    E(fmax), E(fdim), E(copysign), E(lgamma), E(tgamma), E(erf), E(erfc),
    E(remainder), E(remquo), E(sincos), E(sincosf), E(nan), E(finite),
    E(nextafter),

    /* zlib -- libunity links libz for its bundle reader */
    E(inflate), E(inflateInit_), E(inflateInit2_), E(inflateEnd),
    E(inflateReset), E(inflateSetDictionary), E(inflateCopy), E(inflateSync),
    E(deflate), E(deflateInit_), E(deflateInit2_), E(deflateEnd),
    E(deflateReset), E(deflateBound), E(compress), E(compress2),
    E(uncompress), E(crc32), E(adler32), E(zlibVersion), E(zError),

    /* pthread calls whose objects are plain scalars on arm64: pthread_t is a
     * pointer and pthread_key_t an unsigned int in both libcs, so no bridge. */
    E(pthread_self), E(pthread_join), E(pthread_detach), E(pthread_equal),
    E(pthread_exit), { "pthread_kill", (void *)(uintptr_t)my_pthread_kill_traced }, { "pthread_key_create", (void *)(uintptr_t)my_pthread_key_create },
    { "pthread_key_delete", (void *)(uintptr_t)my_pthread_key_delete },
    { "pthread_getspecific", (void *)(uintptr_t)my_pthread_getspecific },
    { "pthread_setspecific", (void *)(uintptr_t)my_pthread_setspecific },
    M(pthread_sigmask), M(pthread_atfork), E(pthread_getcpuclockid),

    /* libdl -- our own, so the game only ever sees modules we loaded */
    M(dlopen), M(dlsym), M(dlclose), M(dlerror), M(dladdr), M(dl_iterate_phdr),
};

/* Modules whose exports we hand out through the fake dlopen/dlsym. */
static const char *const fake_libs[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
    "libandroid.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so",
    "libmediandk.so", "libOpenSLES.so",
    "libunity.so", "libil2cpp.so", "libmain.so",
};
/* Libraries the guest may probe but that this host does not implement.  They
 * must report failure instead of resolving to an empty global scope. */
static const char *const absent_libs[] = {
    "libaaudio.so",
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
            nx_mod *module = nx_find_mod(b);
            if (module)
                nx_run_init(module);
            BIONIC_TRACE("dlopen(\"%s\") -> handle %zu", name, i);
            return FAKE_HANDLE(i);
        }
    /* Libraries this host genuinely does not provide must FAIL to open, the
     * way they fail on an Android device that predates them.  Unity's FMOD
     * probes libaaudio.so first and only falls back to its OpenSL ES output
     * when the dlopen itself returns NULL; handing back the global-scope
     * handle makes FMOD commit to AAudio, find no AAudio_createStreamBuilder,
     * and die with "Error initializing output device" (60) -- a silent game.
     * Refusing the open is the honest emulation and routes FMOD onto the
     * OpenSL ES bridge this port implements. */
    for (size_t i = 0; i < sizeof absent_libs / sizeof *absent_libs; i++)
        if (strcmp(absent_libs[i], b) == 0) {
            nx_log("dlopen(%s) -> NULL (not provided by this host)", name);
            return NULL;
        }
    nx_log("dlopen(%s) -> global scope", name);
    return FAKE_HANDLE(0);
}

static void *my_dlsym(void *h, const char *sym)
{
    if (!sym)
        return NULL;
    /* Data symbols requested through Android's global dlsym scope. */
    if (strcmp(sym, "environ") == 0)
        return &environ;

    /* Fake handles for original game objects retain normal dlsym(handle, ...)
     * scope.  This matters for JNI_OnLoad: all three libraries export that
     * name, and NativeLoader must receive the one belonging to the handle it
     * just opened. */
    void *a = NULL;
    uintptr_t handle = (uintptr_t)h;
    uintptr_t first = (uintptr_t)FAKE_HANDLE(0);
    size_t fake_count = sizeof fake_libs / sizeof *fake_libs;
    if (handle >= first && handle < first + fake_count) {
        size_t index = (size_t)(handle - first);
        nx_mod *module = nx_find_mod(fake_libs[index]);
        if (module)
            a = nx_lookup_in(module, sym);
    }
    if (!a) a = nx_resolve_import(sym);
    if (!a) a = st_android_sym(sym);
    if (!a) a = st_egl_sym(sym);
    if (!a) a = st_jni_sym(sym);
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
    extern const char *st_mod_at(const void *addr, void **base_out);
    void *base = NULL;
    const char *name = st_mod_at(addr, &base);

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
    /* Android code can enumerate the native link map, so the modules mapped by
     * this loader have to be visible here. */
    extern int st_iterate_mods(int (*cb)(void *, size_t, void *), void *data);
    return st_iterate_mods(cb, data);
}

/* --------------------------------------------------------------- publish */

static int cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

void st_bionic_init(void)
{
    qsort(tab, sizeof tab / sizeof *tab, sizeof *tab, cmp);
    nx_log("bionic table: %zu symbols", sizeof tab / sizeof *tab);
}

size_t st_bionic_count(void) { return sizeof tab / sizeof *tab; }

nx_import *st_bionic_entries(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}
