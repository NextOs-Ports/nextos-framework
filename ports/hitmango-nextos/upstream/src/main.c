/*
 * main.c -- native Hitman GO bootstrap for NextOS.
 *
 * There is no Android application or emulator in this path.  We load the
 * original arm64 Unity objects, run their real init arrays/JNI_OnLoad, then
 * drive Unity's native surface and render lifecycle directly.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <ucontext.h>

#include "nx_elf.h"
#include "hgo.h"
#include "nxgl_frame_proof_adapter.h"

char hgo_gamedir[1024];
char hgo_datadir[1024];
char hgo_apk[1024];
char hgo_home[1024];
long hgo_max_frames = 0;
int hgo_trace_gl = 0;
int hgo_capture_mode = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Hitman GO 1.18.1 is a normal, unprotected Unity IL2CPP build.  Keep the
 * exact NativeLoader order and do not introduce a synthetic bootstrap. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
    { "libFirebaseCppApp-12_10_1.so", "libFirebaseCppApp-12_10_1.so",
      1, 0 },
};

extern const nx_import *hgo_pthread_table(size_t *n);
extern const nx_import *hgo_android_table(size_t *n);
extern const nx_import *hgo_egl_table(size_t *n);

/* One combined, sorted import table: bionic + pthread bridge + libandroid +
 * EGL.  nx_resolve_import binary-searches it. */
static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne;
    const nx_import *p = hgo_pthread_table(&np);
    const nx_import *an = hgo_android_table(&na);
    const nx_import *eg = hgo_egl_table(&ne);

    size_t bn;
    extern nx_import *hgo_bionic_entries(size_t *n);
    nx_import *be = hgo_bionic_entries(&bn);
    all = calloc(bn + np + na + ne + 8, sizeof *all);
    all_n = 0;
    for (size_t i = 0; i < bn; i++)
        all[all_n++] = be[i];
    for (size_t i = 0; i < np; i++)
        all[all_n++] = p[i];
    for (size_t i = 0; i < na; i++)
        all[all_n++] = an[i];
    for (size_t i = 0; i < ne; i++)
        all[all_n++] = eg[i];
    qsort(all, all_n, sizeof *all, imp_cmp);
    nx_set_imports(all, all_n);
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, egl %zu)",
           all_n, bn, np, na, ne);
}

/* Report the modules mapped by the native loader. */
/* ------------------------------------------------------------- frame watchdog */

static volatile unsigned long watchdog_frame;
static pid_t watchdog_tid;
static int watchdog_seconds;

void hgo_watchdog_frame(void) { watchdog_frame++; }

static void *watchdog_thread(void *arg)
{
    (void)arg;
    unsigned long last = watchdog_frame;
    for (;;) {
        struct timespec t = { watchdog_seconds, 0 };
        nanosleep(&t, NULL);
        if (watchdog_frame != last) {
            last = watchdog_frame;
            continue;
        }
        fprintf(stderr,
                "[hgo] watchdog: frame %lu has not returned in %ds; faulting "
                "the render thread so its stack is reported\n",
                last, watchdog_seconds);
        /* Deliver to the render thread specifically, not to the process: any
         * other thread would report a stack we already know is idle. */
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void hgo_arm_frame_watchdog(void)
{
    const char *v = getenv("HGO_WATCHDOG");
    if (!v || !*v)
        return;
    watchdog_seconds = atoi(v);
    if (watchdog_seconds <= 0)
        return;
    watchdog_tid = (pid_t)syscall(SYS_gettid);
    pthread_t th;
    if (pthread_create(&th, NULL, watchdog_thread, NULL) != 0) {
        nx_log("watchdog: cannot start thread");
        return;
    }
    pthread_detach(th);
    nx_log("watchdog armed: %ds without a frame faults tid %d",
           watchdog_seconds, (int)watchdog_tid);
}

int hgo_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
{
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))m->base;
        info.dlpi_name = m->name;
        info.dlpi_phdr = (const ElfW(Phdr) *)m->phdr;
        info.dlpi_phnum = (ElfW(Half))m->phnum;
        int r = cb(&info, sizeof info, data);
        if (r)
            return r;
    }
    return 0;
}

/* Which mapped module contains an address, for dladdr. */
const char *hgo_mod_at(const void *addr, void **base_out)
{
    const uint8_t *p = addr;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        if (p >= m->base && p < m->base + m->span) {
            if (base_out)
                *base_out = m->base;
            return m->name;
        }
    }
    return NULL;
}

static void read_env(void)
{
    const char *v;
    nx_verbose   = (v = getenv("HGO_VERBOSE")) && *v != '0';
    hgo_log_level = (v = getenv("HGO_LOGCAT")) && *v != '0';
    hgo_trace_jni = (v = getenv("HGO_JNILOG")) && *v != '0';
    hgo_trace_gl  = (v = getenv("HGO_GLLOG")) && *v != '0';
    if ((v = getenv("HGO_FRAMES")))
        hgo_max_frames = strtol(v, NULL, 10);
}

static void copy_path(char *out, size_t capacity, const char *value,
                      const char *description)
{
    size_t length = strlen(value);
    if (length >= capacity)
        nx_die("%s path is too long", description);
    memcpy(out, value, length + 1);
}

static void join_path(char *out, size_t capacity, const char *base,
                      const char *first, const char *second)
{
    int written;
    if (second)
        written = snprintf(out, capacity, "%s/%s/%s", base, first, second);
    else
        written = snprintf(out, capacity, "%s/%s", base, first);
    if (written < 0 || (size_t)written >= capacity)
        nx_die("game path is too long");
}

static void setup_paths(const char *arg)
{
    if (arg && *arg)
        copy_path(hgo_gamedir, sizeof hgo_gamedir, arg, "game directory");
    else if (!getcwd(hgo_gamedir, sizeof hgo_gamedir))
        copy_path(hgo_gamedir, sizeof hgo_gamedir, ".", "game directory");
    join_path(hgo_datadir, sizeof hgo_datadir, hgo_gamedir, "assets", NULL);
    join_path(hgo_apk, sizeof hgo_apk, hgo_gamedir, "assets", NULL);
    join_path(hgo_home, sizeof hgo_home, hgo_gamedir, "home", NULL);
    mkdir(hgo_home, 0755);
}

int hgo_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !hgo_capture_mode)
            continue;
        join_path(path, sizeof path, hgo_gamedir, "lib", LIBS[i].file);
        nx_mod *m = nx_load(path, LIBS[i].soname);
        if (!m) {
            if (LIBS[i].required)
                nx_die("cannot load %s (expected at %s)", LIBS[i].file, path);
            nx_log("optional %s missing", LIBS[i].file);
        }
    }
    /* Relocate in the same order; by the time libunity is relocated the other
     * modules can satisfy its cross-module imports. */
    int missing = 0;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !hgo_capture_mode)
            continue;
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m)
            missing += nx_relocate(m);
    }
    return missing;
}

/* A fault inside a module we mapped ourselves has no symbols and no link map,
 * so the only way to place it is to print the PC against the module bases.
 * Always on: it costs nothing until something goes wrong. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    fprintf(stderr, "\n[hgo] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[hgo]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[hgo]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[hgo]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[hgo]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[hgo]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    hgo_input_request_exit();
}

static void install_fault_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    /* SIGTERM/SIGINT seguem o caminho do SELECT+START (pause/save/saída),
     * nunca morte seca: frontends e supervisores mandam TERM primeiro. */
    struct sigaction quit;
    memset(&quit, 0, sizeof quit);
    quit.sa_handler = on_exit_signal;
    sigemptyset(&quit.sa_mask);
    sigaction(SIGTERM, &quit, NULL);
    sigaction(SIGINT, &quit, NULL);
}

static void run_unity(void)
{
    void *env = hgo_jni_env();
    void *player = hgo_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = hgo_jni_activity();
    void *surface = hgo_jret_obj("android/view/Surface");
    void *fn;

    hgo_jni_set_unity_player(player);

    fn = hgo_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[hgo] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[hgo] initJni OK\n");

    fn = hgo_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[hgo] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[hgo] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[hgo] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[hgo] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = hgo_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[hgo] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = hgo_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[hgo] nativeFocusChanged(true) OK\n");
    }
    fn = hgo_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[hgo] nativeResume OK\n");
    }

    hgo_audio_start(env);

    void *render = hgo_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[hgo] nativeRender loop%s\n",
            hgo_max_frames > 0 ? " (test frame limit active)" : "");

    hgo_input_init();

    /* Watchdog for a hung frame.  Unity installs its own crash handler, which
     * prints a symbolised backtrace of whichever thread faults -- so the way to
     * see where a stuck frame is stuck is to fault that exact thread on purpose.
     * Off unless HGO_WATCHDOG names a timeout in seconds. */
    hgo_arm_frame_watchdog();

    unsigned long frame = 0;
    for (;;) {
        hgo_watchdog_frame();
        hgo_input_poll(env, player, frame);
        if (hgo_input_exit_requested()) {
            fprintf(stderr, "[hgo] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[hgo] frame %lu keep=%u\n", frame, keep);
        if (!keep) {
            fprintf(stderr, "[hgo] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (hgo_max_frames > 0 && frame >= (unsigned long)hgo_max_frames) {
            fprintf(stderr, "[hgo] test frame limit reached (%lu)\n", frame);
            break;
        }
        const char *frame_us = getenv("HGO_FRAME_US");
        usleep(frame_us ? (useconds_t)strtoul(frame_us, NULL, 10) : 16667);
    }

    hgo_input_cancel_touch(env, player);
    fn = hgo_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[hgo] nativeFocusChanged(false) OK\n");
    }
    fn = hgo_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[hgo] nativePause OK\n");
    }
    hgo_input_close();
    hgo_audio_stop();
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* Emitted before anything can fail: a run that dies during startup is
     * exactly the run whose launch context a reader needs in order to know
     * whether the failure says anything about the port. */
    nxgl_frame_proof_launch_receipt();

    /* EmulationStation's application wrapper exports C.UTF-8.  This Android
     * Unity player was built against Bionic's locale ABI; when its native
     * startup crosses the host glibc C.UTF-8 locale, a small-string object is
     * overwritten and its stack canary fires before frame one.  Android's
     * invariant/POSIX locale is the matching behaviour for this port. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
    setenv("GC_DISABLE_INCREMENTAL", "1", 0);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    fprintf(stderr, "[hgo] Hitman GO for NextOS -- gamedir %s\n", hgo_gamedir);

    hgo_jni_init();
    hgo_egl_init();
    build_imports();

    int missing = hgo_load_modules();
    fprintf(stderr, "[hgo] modules loaded, %d relocations unresolved\n", missing);

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("required Unity module disappeared after relocation");

    /* System.load(libmain.so): its constructors run before JNI_OnLoad. */
    nx_run_init(main_mod);
    typedef int (*onload)(void *vm, void *reserved);
    onload main_onload = (onload)nx_lookup_in(main_mod, "JNI_OnLoad");
    if (!main_onload)
        nx_die("libmain.so has no JNI_OnLoad");
    int main_version = main_onload(hgo_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[hgo] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        hgo_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, hgo_gamedir, "lib", NULL);
    void *loader_class =
        hgo_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = hgo_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        hgo_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited || !il2->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d il2cpp_init=%d)",
               loaded, uni->inited, il2->inited);

    fprintf(stderr,
            "[hgo] NativeLoader.load completed: libunity -> libil2cpp\n");
    run_unity();

    /* Idempotent: normally already published at the last sample, but a
     * run that ends early still owes the reader a verdict. */
    nxgl_frame_proof_publish();
    return 0;
}
