/*
 * main.c -- native Tightrope Theatre bootstrap for NextOS.
 *
 * There is no Android runtime and no emulator here.  The port loads the
 * original arm64 objects, runs PairIP's own bootstrap so the protected code
 * decrypts itself, and then follows the application's real sequence:
 *
 *   Application.onCreate -> StartupLauncher.launch()
 *   GameActivity.onCreate -> System.loadLibrary("lime"), then "ApplicationMain"
 *                         -> SDLActivity.nativeSetupJNI()
 *                         -> SDLAudioManager.nativeSetupJNI()
 *                         -> SDLControllerManager.nativeSetupJNI()
 *   SDLSurface.surfaceCreated -> onNativeSurfaceCreated()
 *   SDLSurface.surfaceChanged -> nativeSetScreenResolution(), onNativeResize(),
 *                                onNativeSurfaceChanged()
 *   SDLMain thread            -> nativeRunMain("libApplicationMain.so",
 *                                              "hxcpp_main", args)
 *
 * The thread split is the Android one as well: hxcpp_main runs on its own
 * thread, exactly as SDLMain does, and this thread stays the UI thread that
 * feeds controller events in.  No scene is forced and no entry point is called
 * out of order.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <link.h>
#include <signal.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/syscall.h>

#include "nx_elf.h"
#include "tr.h"
#include "video.h"
#include "probe_ring.h"
#include "opensles_shim.h"

char tr_gamedir[1024];
char tr_datadir[1024];
char tr_home[1024];
long tr_max_frames = 0;
int tr_trace_gl = 0;
int tr_capture_mode = 0;
static volatile sig_atomic_t shutdown_signal;

/* Load order is System.loadLibrary's: the interpreter first (VMRunner's static
 * initialiser), then lime, then the hxcpp application. */
static const struct {
    const char *file, *soname;
    int capture_only;
} LIBS[] = {
    /* The interpreter is admitted only for the one-shot recovery run that
     * produced the pinned plaintext; normal play never maps it. */
    { "libpairipcore.so",      "libpairipcore.so",      1 },
    { "liblime.so",            "liblime.so",            0 },
    { "libApplicationMain.so", "libApplicationMain.so", 0 },
};
#define LIBS_N (sizeof LIBS / sizeof *LIBS)

extern const nx_import *tr_pthread_table(size_t *n);
extern const nx_import *tr_android_table(size_t *n);
extern const nx_import *tr_egl_table(size_t *n);
extern const nx_import *tr_audio_table(size_t *n);
extern nx_import *tr_bionic_entries(size_t *n);
extern int tr_pairip_startup(void);
extern void tr_jni_bind_pairip(void);

static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne, nau, bn;
    const nx_import *p = tr_pthread_table(&np);
    const nx_import *an = tr_android_table(&na);
    const nx_import *eg = tr_egl_table(&ne);
    const nx_import *au = tr_audio_table(&nau);
    nx_import *be = tr_bionic_entries(&bn);

    all = calloc(bn + np + na + ne + nau + 8, sizeof *all);
    all_n = 0;
    for (size_t i = 0; i < bn; i++)  all[all_n++] = be[i];
    for (size_t i = 0; i < np; i++)  all[all_n++] = p[i];
    for (size_t i = 0; i < na; i++)  all[all_n++] = an[i];
    for (size_t i = 0; i < ne; i++)  all[all_n++] = eg[i];
    for (size_t i = 0; i < nau; i++) all[all_n++] = au[i];
    all[all_n++] = (nx_import){ "ExecuteProgram",
                                tr_protected_sym("ExecuteProgram") };
    qsort(all, all_n, sizeof *all, imp_cmp);
    nx_set_imports(all, all_n);
    /* The ~180 GL entry points liblime imports live in the driver blob; ask
     * it by name rather than restating its export list here. */
    nx_set_fallback(tr_egl_sym);
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, "
           "egl %zu, opensl %zu)", all_n, bn, np, na, ne, nau);
}

int tr_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
{
    for (size_t i = 0; i < LIBS_N; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))m->base;
        /* The real loader reports the path the object was loaded from, and
         * PairIP re-opens it to hash the file it is running.  A bare soname
         * is not openable, and a failed open reads as tampering. */
        info.dlpi_name = m->path;
        info.dlpi_phdr = (const ElfW(Phdr) *)m->phdr;
        info.dlpi_phnum = (ElfW(Half))m->phnum;
        int r = cb(&info, sizeof info, data);
        if (r)
            return r;
    }
    return 0;
}

const char *tr_mod_at(const void *addr, void **base_out)
{
    const uint8_t *p = addr;
    for (size_t i = 0; i < LIBS_N; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        if (p >= m->base && p < m->base + m->span) {
            if (base_out)
                *base_out = m->base;
            return m->path;
        }
    }
    return NULL;
}

static void read_env(void)
{
    const char *v;
    nx_verbose    = (v = getenv("TR_VERBOSE")) && *v != '0';
    if (getenv("TR_PROBE"))
        tr_probe_init(getenv("TR_PROBE"));
    tr_log_level  = (v = getenv("TR_LOGCAT")) && *v != '0';
    tr_trace_jni  = (v = getenv("TR_JNILOG")) && *v != '0';
    tr_trace_gl   = (v = getenv("TR_GLLOG")) && *v != '0';
    tr_trace_plt  = (v = getenv("TR_PLTLOG")) && *v != '0';
    tr_trace_files = (v = getenv("TR_FILELOG")) && *v != '0';
    tr_capture_mode = (v = getenv("TR_VM_CAPTURE")) && *v != '0';
    if ((v = getenv("TR_FRAMES")))
        tr_max_frames = strtol(v, NULL, 10);
}

static void setup_paths(const char *arg)
{
    char given[1024];
    if (arg && *arg)
        snprintf(given, sizeof given, "%s", arg);
    else if (!getcwd(given, sizeof given))
        snprintf(given, sizeof given, ".");
    /* Absolute from here on: the game runs with its working directory moved
     * to the asset root, so a relative game directory would stop resolving. */
    if (!realpath(given, tr_gamedir))
        snprintf(tr_gamedir, sizeof tr_gamedir, "%s", given);
    snprintf(tr_datadir, sizeof tr_datadir, "%s/assets", tr_gamedir);
    snprintf(tr_home, sizeof tr_home, "%s/home", tr_gamedir);
    /* The Activity creates the app's files directory before any native code
     * asks for it; Lime writes its saves there and fails silently otherwise. */
    mkdir(tr_home, 0755);
}

int tr_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < LIBS_N; i++) {
        if (LIBS[i].capture_only && !tr_capture_mode)
            continue;
        snprintf(path, sizeof path, "%s/lib/%s", tr_gamedir, LIBS[i].file);
        if (!nx_load(path, LIBS[i].soname))
            nx_die("cannot load %s (expected at %s)", LIBS[i].file, path);
    }
    int missing = 0;
    for (size_t i = 0; i < LIBS_N; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m)
            missing += nx_relocate(m);
    }
    return missing;
}

/* A frame can also land in a library the system loader owns (SDL, the Mali
 * blob, libc).  Those have no unwind info we can read from inside the handler
 * either, but /proc/self/maps names them and gives the offset -- which is what
 * turns "#3 0x7f93b7ef80" into a line an objdump can be pointed at. */
static void place_addr(unsigned long addr, char *out, size_t n)
{
    out[0] = '\0';
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return;
    char line[512];
    while (fgets(line, sizeof line, maps)) {
        unsigned long lo = 0, hi = 0, off = 0;
        char perms[8], path[300];
        path[0] = '\0';
        int got = sscanf(line, "%lx-%lx %7s %lx %*s %*s %299[^\n]",
                         &lo, &hi, perms, &off, path);
        if (got >= 4 && addr >= lo && addr < hi && path[0]) {
            char *p = path;
            while (*p == ' ')
                p++;
            snprintf(out, n, " %s+%#lx", p, addr - lo + off);
            break;
        }
    }
    fclose(maps);
}

/* A fault inside a module we mapped ourselves has no symbols and no link map,
 * so the only way to place it is to print the PC against the module bases. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    fprintf(stderr, "\n[tr] signal %d on tid=%d at pc=%#lx addr=%p\n", sig,
            (int)syscall(SYS_gettid), pc, si ? si->si_addr : NULL);
    for (size_t i = 0; i < LIBS_N; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[tr]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[tr]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[tr]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[tr]   x28=%016lx x29=%016lx lr=%016lx sp=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp);
    /* Walk the frame-pointer chain.  The mapped objects have no unwind tables
     * we can use, but they are all compiled with a frame pointer, so x29 gives
     * the caller chain -- which is the only way to see which of the game's
     * call sites reached a fault in a shim. */
    unsigned long fp = (unsigned long)u->uc_mcontext.regs[29];
    fprintf(stderr, "[tr]   frames:\n");
    for (int depth = 0; depth < 24 && fp; depth++) {
        unsigned long next = 0, ret = 0;
        memcpy(&next, (void *)fp, sizeof next);
        memcpy(&ret, (void *)(fp + 8), sizeof ret);
        if (!ret)
            break;
        void *base = NULL;
        const char *mod = tr_mod_at((void *)ret, &base);
        if (mod) {
            fprintf(stderr, "[tr]     #%-2d %s+%#lx\n", depth, mod,
                    ret - (unsigned long)base);
        } else {
            char where[360];
            place_addr(ret, where, sizeof where);
            fprintf(stderr, "[tr]     #%-2d %#lx%s\n", depth, ret, where);
        }
        if (next <= fp)
            break;
        fp = next;
    }

    /* The frame chain stops at any leaf that keeps no frame pointer, so also
     * report every stack word that points into one of the mapped objects.
     * That is noisier, but it does name the call site. */
    unsigned long sp = (unsigned long)u->uc_mcontext.sp;
    fprintf(stderr, "[tr]   stack words inside the game objects:\n");
    for (int i = 0; i < 96; i++) {
        unsigned long w = 0;
        memcpy(&w, (void *)(sp + (unsigned long)i * 8), sizeof w);
        void *base = NULL;
        const char *mod = tr_mod_at((void *)w, &base);
        if (mod)
            fprintf(stderr, "[tr]     sp+%-4d %s+%#lx\n", i * 8, mod,
                    w - (unsigned long)base);
    }

    /* The PairIP interpreter runs generated code in an anonymous mapping, so
     * "which module" is not enough to place a fault: print the mapping and the
     * instructions around the PC as well. */
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        while (fgets(line, sizeof line, maps)) {
            unsigned long lo = 0, hi = 0;
            if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && pc >= lo && pc < hi) {
                fprintf(stderr, "[tr]   mapping: %s", line);
                break;
            }
        }
        fclose(maps);
    }
    const uint32_t *code = (const uint32_t *)(pc & ~3UL);
    for (int i = -6; i <= 4; i++)
        fprintf(stderr, "[tr]   %s%#lx: %08x\n", i ? "  " : "> ",
                (unsigned long)(code + i), code[i]);

    tr_patch_protected_plt();
    fflush(stderr);
    _exit(2);
}

static void install_fault_handler(void)
{
    /* Diagnosis knob: some GPU blobs handle faults of their own, and a
     * handler of ours on top of theirs turns a normal event into a crash. */
    if (getenv("TR_NOSEGV"))
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

static void on_shutdown_signal(int signal_number)
{
    shutdown_signal = signal_number;
}

/* The launcher supervises the child with SIGTERM.  Convert that asynchronous
 * request into SDL's native quit event from the input thread, where JNI and
 * the game's save path are safe to run. */
static void install_shutdown_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

/* --- the SDL side of the Activity lifecycle ------------------------------ */

static void *sdl_static(const char *cls, const char *name)
{
    void *fn = tr_jni_native(cls, name);
    if (!fn)
        fprintf(stderr, "[tr] the game's SDL has no %s.%s\n", cls, name);
    return fn;
}

static void call_setup_jni(const char *cls)
{
    void *fn = sdl_static(cls, "nativeSetupJNI");
    if (!fn)
        return;
    int r = ((int (*)(void *, void *))fn)(tr_jni_env(), tr_jret_class(cls));
    fprintf(stderr, "[tr] %s.nativeSetupJNI() -> %d\n", cls, r);
}

static void surface_lifecycle(void)
{
    void *env = tr_jni_env();
    void *cls = tr_jret_class("org/libsdl/app/SDLActivity");
    int w = tr_screen_width(), h = tr_screen_height();
    void *fn;

    fprintf(stderr, "[tr] framebuffer surface %dx%d\n", w, h);

    fn = sdl_static("org/libsdl/app/SDLActivity", "onNativeSurfaceCreated");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);

    fn = sdl_static("org/libsdl/app/SDLActivity", "nativeSetScreenResolution");
    if (fn)
        /* (surfaceWidth, surfaceHeight, deviceWidth, deviceHeight,
         *  format, refreshRate).  Format 0x16362004 is
         *  SDL_PIXELFORMAT_RGB565, what the fbdev panel presents. */
        ((void (*)(void *, void *, int, int, int, int, int, float))fn)(
            env, cls, w, h, w, h, 0x16362004, 60.0f);

    fn = sdl_static("org/libsdl/app/SDLActivity", "onNativeResize");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);

    fn = sdl_static("org/libsdl/app/SDLActivity", "onNativeSurfaceChanged");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);

    fn = sdl_static("org/libsdl/app/SDLActivity", "nativeResume");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);

    fn = sdl_static("org/libsdl/app/SDLActivity", "nativeFocusChanged");
    if (fn)
        ((void (*)(void *, void *, uint8_t))fn)(env, cls, 1);
}

/* nativeRunMain dlopens libApplicationMain, resolves hxcpp_main and calls it.
 * Everything after this point is the game's own state machine -- splash, menu,
 * theatre -- driven by Lime.
 *
 * This runs on the process's first thread, not on a helper.  hxcpp decides
 * which thread owns the Haxe main event loop from the thread that boots the
 * program, and Lime's NativeApplication.updateTimer reads that loop on every
 * frame: boot it anywhere else and the first frame throws "Event loop is not
 * available".  The controller pump is the thread that moves instead, which is
 * the same division of labour Android has, only mirrored. */
static void *run_hxcpp_main(void *arg)
{
    (void)arg;
    void *fn = sdl_static("org/libsdl/app/SDLActivity", "nativeRunMain");
    if (!fn)
        nx_die("liblime does not export nativeRunMain");
    void *library = tr_jret_str("libApplicationMain.so");
    void *function = tr_jret_str("hxcpp_main");
    fprintf(stderr, "[tr] nativeRunMain(libApplicationMain.so, hxcpp_main) on tid=%d\n",
            (int)syscall(SYS_gettid));
    int status = ((int (*)(void *, void *, void *, void *, void *))fn)(
        tr_jni_env(), tr_jret_class("org/libsdl/app/SDLActivity"),
        library, function, NULL);
    fprintf(stderr, "[tr] hxcpp_main returned %d\n", status);
    return NULL;
}

/* On Android the Activity's onResume and onWindowFocusChanged arrive after the
 * surface exists and after SDL has built its window -- not before.  Sent too
 * early they land on a window that does not exist yet, SDL never marks the app
 * focused, and Lime stops rendering after the preloader while its update loop
 * and audio keep running.  Deliver them once the first frame has been
 * presented, which is the moment the window is provably up. */
static void deliver_activity_resume(void)
{
    void *env = tr_jni_env();
    void *cls = tr_jret_class("org/libsdl/app/SDLActivity");
    void *fn;

    fn = tr_jni_native("org/libsdl/app/SDLActivity", "onNativeSurfaceChanged");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);
    fn = tr_jni_native("org/libsdl/app/SDLActivity", "nativeResume");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);
    fn = tr_jni_native("org/libsdl/app/SDLActivity", "nativeFocusChanged");
    if (fn)
        ((void (*)(void *, void *, uint8_t))fn)(env, cls, 1);
    fprintf(stderr, "[tr] activity resumed and focused\n");
}

/* The UI thread: on Android this is the thread that feeds View input into
 * SDL, and it is the only thing it does here too. */
static void *input_thread(void *arg)
{
    (void)arg;
    int resumed = 0;
    int shutdown_delivered = 0;
    fprintf(stderr, "[tr] input pump on tid=%d\n", (int)syscall(SYS_gettid));
    /* Keeps pumping after the quit request: the game needs input events to
     * keep flowing while it shuts down and writes its save. */
    for (;;) {
        if (shutdown_signal && !shutdown_delivered) {
            shutdown_delivered = 1;
            fprintf(stderr, "[tr] signal %d: asking the game to quit\n",
                    (int)shutdown_signal);
            tr_input_request_quit();
        }
        if (!resumed && tr_egl_swap_count() > 0) {
            deliver_activity_resume();
            resumed = 1;
        }
        tr_input_poll();
        tr_metronome_tick();
        usleep(4000);
    }
    return NULL;
}

/* The firmware SDL and liblime's Android SDL live in the same process but
 * support different video drivers.  ROCKNIX legitimately exports
 * SDL_VIDEODRIVER=wayland for the former; letting that host-only hint cross
 * into the Android copy makes its SDL_Init fail with "wayland not available"
 * after our Wayland window is already alive.  Consume the host hint first,
 * then remove only the video selection before entering the guest lifecycle.
 * Audio remains inherited because the OpenSL bridge opens it through the
 * firmware SDL later, on the game's native request. */
static void isolate_guest_sdl_environment(void)
{
    const char *video = getenv("SDL_VIDEODRIVER");
    const char *video_alias = getenv("SDL_VIDEO_DRIVER");
    if ((video && *video) || (video_alias && *video_alias))
        fprintf(stderr,
                "[tr] host SDL video hint consumed (%.*s); hiding it from "
                "the Android SDL\n",
                64, video && *video ? video : video_alias);
    unsetenv("SDL_VIDEODRIVER");
    unsetenv("SDL_VIDEO_DRIVER");
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* The frontend wrapper exports C.UTF-8.  These objects were built against
     * bionic's locale ABI, and the game ships its own translations, so the
     * invariant locale is both the matching behaviour and the one that keeps
     * the text in English (never Japanese). */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);

    /* Lime's OpenAL reports every "unqueue from a non-streaming source" as an
     * error, thousands of times a second, and each one is a write to stderr.
     * On Android that goes to a ring buffer nobody reads; here it is a real
     * file write in the middle of the frame loop and it costs most of the
     * frame rate.  The messages are not about this port -- the same source
     * pattern spams on a phone -- so turn the library's own log off.  Set
     * ALSOFT_LOGLEVEL in the launcher to get it back while debugging. */
    if (!getenv("ALSOFT_LOGLEVEL"))
        setenv("ALSOFT_LOGLEVEL", "0", 1);

    /* SDL's Android joystick backend publishes the phone's accelerometer as
     * joystick 0 before any real pad.  This box has no accelerometer, and a
     * game that reads "the first connected controller" reads that phantom
     * instead of the pad.  Saying so is not forcing a driver -- it is telling
     * SDL the truth about the hardware. */
    if (!getenv("SDL_ACCELEROMETER_AS_JOYSTICK"))
        setenv("SDL_ACCELEROMETER_AS_JOYSTICK", "0", 1);

    /* BEFORE any other library initialises: this game uses pthread key 0
     * without creating it, so key 0 has to be ours from the first instruction
     * -- otherwise SDL or the GPU driver takes it and the game overwrites
     * their thread state.  See bionic.c for the measurement. */
    tr_reserve_zero_key();

    read_env();
    install_fault_handler();
    install_shutdown_handlers();
    setup_paths(argc > 1 ? argv[1] : NULL);

    fprintf(stderr, "[tr] Tightrope Theatre for NextOS -- gamedir %s\n",
            tr_gamedir);

    /* Who owns the drawable is decided before anything asks for a surface:
     * the EGL table the game gets depends on the answer. */
    tr_video_init();
    isolate_guest_sdl_environment();

    tr_jni_init();
    tr_jni_bind_sdl();
    tr_jni_bind_pairip();
    tr_egl_init();
    build_imports();

    int missing = tr_load_modules();
    fprintf(stderr, "[tr] modules loaded, %d relocations unresolved\n", missing);

    nx_mod *pairip = nx_find_mod("libpairipcore.so");
    nx_mod *lime = nx_find_mod("liblime.so");
    nx_mod *app = nx_find_mod("libApplicationMain.so");
    opensles_shim_set_text_range(lime->base, lime->span);

    typedef int (*onload)(void *vm, void *reserved);
    onload f;

    if (tr_capture_mode) {
        /* The recovery run only: reproduce Android's order --
         * System.loadLibrary("pairipcore"), StartupLauncher.launch(), then
         * each object's own DT_INIT -- and record what the interpreter
         * decrypted. */
        nx_run_init(pairip);
        f = (onload)nx_lookup_in(pairip, "JNI_OnLoad");
        if (f)
            fprintf(stderr, "[tr] JNI_OnLoad(libpairipcore.so) -> %#x\n",
                    f(tr_jni_vm(), NULL));
        if (!getenv("TR_NO_STARTUP"))
            tr_pairip_startup();
        int rc = tr_pairip_capture();
        fprintf(stderr, "[tr] native capture %s\n", rc ? "failed" : "complete");
        return rc ? 3 : 0;
    }

    /* Restore what PairIP would have produced, in the order the loader would
     * have produced it: plaintext first, then the hidden PLT slots. */
    tr_apply_text_overlays();
    int unfilled = tr_patch_protected_plt();
    if (unfilled)
        nx_die("%d named PLT slots have no native implementation", unfilled);

    /* Relocation needed the whole image writable.  Put every segment back to
     * its real ELF protections before any of this code runs. */
    nx_protect(lime);
    nx_protect(app);

    /* DT_INIT in both objects is PairIP's stub and only re-enters the
     * interpreter; their real C++ constructors are in DT_INIT_ARRAY and still
     * run in full, in load order. */
    lime->init_func = NULL;
    app->init_func = NULL;
    nx_run_init(lime);
    f = (onload)nx_lookup_in(lime, "JNI_OnLoad");
    if (f)
        fprintf(stderr, "[tr] JNI_OnLoad(liblime.so) -> %#x\n",
                f(tr_jni_vm(), NULL));
    nx_run_init(app);
    f = (onload)nx_lookup_in(app, "JNI_OnLoad");
    if (f)
        fprintf(stderr, "[tr] JNI_OnLoad(libApplicationMain.so) -> %#x\n",
                f(tr_jni_vm(), NULL));

    /* 4. SDLActivity.onCreate -> SDL.setupJNI() */
    call_setup_jni("org/libsdl/app/SDLActivity");
    call_setup_jni("org/libsdl/app/SDLAudioManager");
    call_setup_jni("org/libsdl/app/SDLControllerManager");

    /* 5. the surface, in SDLSurface's order */
    surface_lifecycle();

    /* 6. controllers, before the game asks SDL for them */
    tr_input_init();

    /* SDL's Linux evdev keyboard support installs its OWN handlers for SIGSEGV,
     * SIGBUS and friends (it restores the console mode and re-raises).  Ours
     * was installed at start-up, so from SDL_Init onwards a real crash would be
     * reported by us with the re-raise's context -- pc inside raise(), the
     * actual fault site gone.  Reinstall on top now that every SDL subsystem is
     * up, so the first report is the true one. */
    install_fault_handler();
    install_shutdown_handlers();

    /* 7. the input pump, then the game itself on this thread */
    pthread_t pump;
    if (pthread_create(&pump, NULL, input_thread, NULL) != 0)
        nx_die("cannot start the input thread: %s", strerror(errno));

    /* Android roots the asset paths at the APK's assets/ directory, and Lime
     * asks for "manifest/default.json" relative to it.  The extracted tree
     * reproduces that layout, so make it the working directory before the game
     * starts.  Everything the port itself opens is already absolute. */
    if (chdir(tr_datadir) != 0)
        nx_die("cannot enter the asset directory %s: %s", tr_datadir,
               strerror(errno));

    /* hxcpp_main runs on the process's first thread, on every device: hxcpp
     * decides which thread owns the Haxe event loop from the thread that boots
     * the program, and Lime reads that loop every frame -- booting it anywhere
     * else makes the first frame throw "Event loop is not available".  It is
     * also the thread that owns the GL context here, which the Mali drivers
     * insist on.  TR_GAME_THREAD moves it, for diagnosis only. */
    int own_thread = getenv("TR_GAME_THREAD") != NULL;   /* diagnosis only */
    fprintf(stderr, "[tr] running hxcpp_main on %s\n",
            own_thread ? "its own thread (SDLMain's arrangement)"
                       : "the process main thread");
    if (own_thread) {
        pthread_t game;
        if (pthread_create(&game, NULL, run_hxcpp_main, NULL) != 0)
            nx_die("cannot start the game thread: %s", strerror(errno));
        pthread_join(game, NULL);
    } else {
        run_hxcpp_main(NULL);
    }

    fprintf(stderr, "[tr] the game has exited; leaving to the frontend\n");
    tr_audio_shutdown();
    tr_input_close();
    tr_video_shutdown();
    /* Leaving through _exit avoids the proprietary Mali teardown deadlock the
     * other NextOS ports hit on the way out. */
    fflush(NULL);
    _exit(0);
}
