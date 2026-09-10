/*
 * main.c -- native Retro City Rampage DX bootstrap for NextOS.
 *
 * There is no Android runtime and no emulator here.  The port maps the game's
 * own arm64 object and then follows the application's real sequence, with
 * nothing skipped and no state forced:
 *
 *   System.loadLibrary("RCRDX")   -> DT_INIT_ARRAY (the C++ constructors)
 *                                 -> JNI_OnLoad
 *   SDLSurface.surfaceCreated     -> onNativeSurfaceChanged()
 *   SDLSurface.surfaceChanged     -> onNativeResize(w, h, format)
 *   SDLActivity's SDLThread       -> nativeInit(args)
 *                                    -> SDL_Android_Init(), SDL_SetMainReady()
 *                                    -> SDL_main()
 *
 * From nativeInit onwards everything is the game's: SDL creates its window,
 * asks the Activity for the native surface, sizes itself from it, opens audio
 * through the Java AudioTrack methods and runs its own event loop.  The
 * splash, the menu and the city are its state machine, reached by playing.
 *
 * The thread split is Android's as well: SDL_main runs on its own thread, the
 * way SDLThread does, and the process's first thread stays the UI thread that
 * feeds controller events in.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <link.h>
#include <signal.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

#include "nx_elf.h"
#include "rcr.h"
#include "video.h"
#include "nxgl_frame_proof_adapter.h"

char rcr_gamedir[1024];
char rcr_datadir[1024];
char rcr_home[1024];
long rcr_max_frames = 0;
int rcr_trace_gl = 0;
int rcr_verbose_audio = 0;
int rcr_trace_plt = 0;
static volatile sig_atomic_t termination_requested;

/* One object, loaded the way System.loadLibrary would.  libstub.so (the
 * repack's own loader) is deliberately left on the floor: it is not part of
 * the game and the game never asks for it. */
#define GAME_SO "libRCRDX.so"

extern const nx_import *rcr_pthread_table(size_t *n);
extern const nx_import *rcr_android_table(size_t *n);
extern const nx_import *rcr_egl_table(size_t *n);
extern nx_import *rcr_bionic_entries(size_t *n);

static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne, bn;
    const nx_import *p = rcr_pthread_table(&np);
    const nx_import *an = rcr_android_table(&na);
    const nx_import *eg = rcr_egl_table(&ne);
    nx_import *be = rcr_bionic_entries(&bn);

    all = calloc(bn + np + na + ne + 8, sizeof *all);
    all_n = 0;
    for (size_t i = 0; i < bn; i++)  all[all_n++] = be[i];
    for (size_t i = 0; i < np; i++)  all[all_n++] = p[i];
    for (size_t i = 0; i < na; i++)  all[all_n++] = an[i];
    for (size_t i = 0; i < ne; i++)  all[all_n++] = eg[i];
    qsort(all, all_n, sizeof *all, imp_cmp);
    nx_set_imports(all, all_n);
    /* The GL entry points live in the driver blob; ask it by name rather than
     * restating its export list here. */
    nx_set_fallback(rcr_egl_sym);
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, "
           "egl %zu)", all_n, bn, np, na, ne);
}

int rcr_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
{
    nx_mod *m = nx_find_mod(GAME_SO);
    if (!m)
        return 0;
    struct dl_phdr_info info;
    memset(&info, 0, sizeof info);
    info.dlpi_addr = (ElfW(Addr))m->base;
    info.dlpi_name = m->path;
    info.dlpi_phdr = (const ElfW(Phdr) *)m->phdr;
    info.dlpi_phnum = (ElfW(Half))m->phnum;
    return cb(&info, sizeof info, data);
}

const char *rcr_mod_at(const void *addr, void **base_out)
{
    nx_mod *m = nx_find_mod(GAME_SO);
    const uint8_t *p = addr;
    if (m && p >= m->base && p < m->base + m->span) {
        if (base_out)
            *base_out = m->base;
        return m->path;
    }
    return NULL;
}

static void read_env(void)
{
    const char *v;
    nx_verbose      = (v = getenv("RCR_VERBOSE")) && *v != '0';
    rcr_log_level   = (v = getenv("RCR_LOGCAT")) && *v != '0';
    rcr_trace_jni   = (v = getenv("RCR_JNILOG")) && *v != '0';
    rcr_trace_gl    = (v = getenv("RCR_GLLOG")) && *v != '0';
    rcr_trace_plt   = (v = getenv("RCR_PLTLOG")) && *v != '0';
    rcr_trace_files = (v = getenv("RCR_FILELOG")) && *v != '0';
    rcr_verbose_audio = (v = getenv("RCR_AUDIOLOG")) && *v != '0';
    if ((v = getenv("RCR_FRAMES")))
        rcr_max_frames = strtol(v, NULL, 10);
}

static void setup_paths(const char *arg)
{
    char given[1024];
    if (arg && *arg)
        snprintf(given, sizeof given, "%s", arg);
    else if (!getcwd(given, sizeof given))
        snprintf(given, sizeof given, ".");
    if (!realpath(given, rcr_gamedir))
        snprintf(rcr_gamedir, sizeof rcr_gamedir, "%s", given);
    /* The APK's assets/ tree, which is where the game's three data files live
     * and where its own relative paths resolve. */
    snprintf(rcr_datadir, sizeof rcr_datadir, "%s/gamedata", rcr_gamedir);
    snprintf(rcr_home, sizeof rcr_home, "%s/home", rcr_gamedir);
    /* The Activity creates the app's files directory before any native code
     * asks for it; the game writes its save there. */
    mkdir(rcr_home, 0755);
}

/* One instance, and the lock is taken on the BINARY -- the house rule.  Two
 * copies of a game on this box wedge the display. */
static void single_instance(void)
{
    char self[1024];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0)
        return;
    self[n] = '\0';
    int fd = open(self, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[rcr] another copy of this port is already running "
                        "(lock on %s)\n", self);
        _exit(1);
    }
    /* fd stays open for the life of the process; the lock dies with it. */
}

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

/* A fault inside the module we mapped ourselves has no symbols and no link
 * map, so the only way to place it is against the module base. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    fprintf(stderr, "\n[rcr] signal %d on tid=%d at pc=%#lx addr=%p code=%d\n",
            sig, (int)syscall(SYS_gettid), pc, si ? si->si_addr : NULL,
            si ? si->si_code : 0);
    nx_mod *m = nx_find_mod(GAME_SO);
    if (m) {
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[rcr]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[rcr]   %-16s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[rcr]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[rcr]   x28=%016lx x29=%016lx lr=%016lx sp=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp);
    unsigned long fp = (unsigned long)u->uc_mcontext.regs[29];
    fprintf(stderr, "[rcr]   frames:\n");
    for (int depth = 0; depth < 24 && fp; depth++) {
        unsigned long next = 0, ret = 0;
        memcpy(&next, (void *)fp, sizeof next);
        memcpy(&ret, (void *)(fp + 8), sizeof ret);
        if (!ret)
            break;
        void *base = NULL;
        const char *mod = rcr_mod_at((void *)ret, &base);
        if (mod) {
            fprintf(stderr, "[rcr]     #%-2d %s+%#lx\n", depth, mod,
                    ret - (unsigned long)base);
        } else {
            char where[360];
            place_addr(ret, where, sizeof where);
            fprintf(stderr, "[rcr]     #%-2d %#lx%s\n", depth, ret, where);
        }
        if (next <= fp)
            break;
        fp = next;
    }
    fflush(stderr);
    _exit(2);
}

static void install_fault_handler(void)
{
    if (getenv("RCR_NOSEGV"))
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

/* Launcher/frontend signals are consumed by the normal UI loop so the guest
 * can execute nativePause/nativeQuit and persist its state.  The handler does
 * no JNI, allocation, locking or stdio. */
static void request_terminal_signal(int signal_number)
{
    (void)signal_number;
    termination_requested = 1;
}

static void install_terminal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = request_terminal_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

int rcr_load_modules(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/lib/%s", rcr_gamedir, GAME_SO);
    if (!nx_load(path, GAME_SO))
        nx_die("cannot load %s (expected at %s)", GAME_SO, path);
    nx_mod *m = nx_find_mod(GAME_SO);
    return m ? nx_relocate(m) : 0;
}

/* --- the SDL side of the Activity lifecycle ------------------------------ */

static void *sdl_static(const char *name)
{
    void *fn = rcr_jni_native("org/libsdl/app/SDLActivity", name);
    if (!fn)
        fprintf(stderr, "[rcr] the game's SDL has no SDLActivity.%s\n", name);
    return fn;
}

/* What the Java SDLSurface passes as the "sdlFormat": the panel's own pixel
 * format, read from the framebuffer instead of assumed.  This only tells SDL
 * what the surface looks like -- the GL config is the game's own
 * eglChooseConfig, and no resolution is ever pinned here. */
static int surface_format(void)
{
    int fmt = 0x16161804;               /* SDL_PIXELFORMAT_RGB888 */
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct fb_var_screeninfo v;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.bits_per_pixel == 16)
            fmt = 0x15151002;           /* SDL_PIXELFORMAT_RGB565 */
        close(fd);
    }
    return fmt;
}

static void surface_lifecycle(void)
{
    void *env = rcr_jni_env();
    void *cls = rcr_jret_class("org/libsdl/app/SDLActivity");
    int w = rcr_screen_width(), h = rcr_screen_height();
    void *fn;

    fprintf(stderr, "[rcr] framebuffer surface %dx%d\n", w, h);

    /* SDLSurface.surfaceCreated */
    fn = sdl_static("onNativeSurfaceChanged");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);

    /* SDLSurface.surfaceChanged -- this vintage takes (w, h, format) and no
     * refresh rate; measured at Android_SetScreenResolution. */
    fn = sdl_static("onNativeResize");
    if (fn)
        ((void (*)(void *, void *, int, int, int))fn)(env, cls, w, h,
                                                      surface_format());
}

/* nativeInit is SDLThread's body: it calls SDL_Android_Init, SDL_SetMainReady
 * and then SDL_main.  Everything after this point is the game's own state
 * machine -- splash, menu, city -- driven by its own event loop. */
static void *run_native_init(void *arg)
{
    (void)arg;
    void *fn = sdl_static("nativeInit");
    if (!fn)
        nx_die("%s does not export nativeInit", GAME_SO);
    fprintf(stderr, "[rcr] SDLActivity.nativeInit() on tid=%d\n",
            (int)syscall(SYS_gettid));
    /* The argument is the String[] of command-line arguments; the Activity
     * passes an empty list, and SDL then builds argv = { "app_process" }. */
    int status = ((int (*)(void *, void *, void *))fn)(
        rcr_jni_env(), rcr_jret_class("org/libsdl/app/SDLActivity"), NULL);
    fprintf(stderr, "[rcr] SDL_main returned %d\n", status);
    return (void *)(intptr_t)status;
}

/* On Android onResume and onWindowFocusChanged arrive after the surface exists
 * and after SDL has built its window -- not before.  Sent too early they land
 * on a window that does not exist yet.  Deliver them once the first frame has
 * been presented, which is the moment the window is provably up. */
static void deliver_activity_resume(void)
{
    void *env = rcr_jni_env();
    void *cls = rcr_jret_class("org/libsdl/app/SDLActivity");
    void *fn = rcr_jni_native("org/libsdl/app/SDLActivity", "nativeResume");
    if (fn)
        ((void (*)(void *, void *))fn)(env, cls);
    fprintf(stderr, "[rcr] activity resumed\n");
}

int main(int argc, char **argv)
{
    /* Emitted before anything can fail: the launch context is what a reader
     * needs to know whether a startup failure says anything about the port. */
    nxgl_frame_proof_launch_receipt();
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* The game ships its own translations and picks one from getLanguage();
     * the invariant locale is both the matching behaviour for objects built
     * against bionic and the one that keeps the text in English. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);

    /* This box has no accelerometer, and a game that reads "the first
     * connected controller" must not read a phantom one.  Saying so is not
     * forcing a driver -- it is telling SDL the truth about the hardware. */
    if (!getenv("SDL_ACCELEROMETER_AS_JOYSTICK"))
        setenv("SDL_ACCELEROMETER_AS_JOYSTICK", "0", 1);

    /* 🚨 SDL_AUDIODRIVER has to leave the game's environment before anything
     * of the game runs.  NextOS' /etc/profile.d/99-emuelec.conf exports the
     * EmuELEC list "pulseaudio,alsa" into everything EmulationStation
     * launches, and the SDL compiled INSIDE libRCRDX.so only knows the drivers
     * "android" and "dummy": it matches that string against its own list,
     * finds nothing, and aborts audio with its own message ("Couldn't
     * initialize SDL: Audio target ... not available"), so the game opens mute
     * when started from the ES and has sound over ssh (a non-login shell never
     * reads /etc/profile.d).  Other ports never see this because they use the
     * system SDL.  The value is not thrown away: audio.c hands it back to the
     * SYSTEM SDL if it really names one of its drivers, so the system keeps
     * the choice and nothing here forces a driver. */
    rcr_sys_audiodriver = getenv("SDL_AUDIODRIVER");
    if (rcr_sys_audiodriver && *rcr_sys_audiodriver) {
        rcr_sys_audiodriver = strdup(rcr_sys_audiodriver);
        unsetenv("SDL_AUDIODRIVER");
    } else {
        rcr_sys_audiodriver = NULL;
    }

    /* Before any other library initialises: see bionic.c for why key 0 has to
     * be ours from the first instruction. */
    rcr_reserve_zero_key();

    read_env();
    single_instance();
    install_fault_handler();
    install_terminal_handlers();
    setup_paths(argc > 1 ? argv[1] : NULL);

    fprintf(stderr, "[rcr] Retro City Rampage DX for NextOS -- gamedir %s\n",
            rcr_gamedir);

    /* Who owns the drawable is decided before anything asks for a surface. */
    rcr_video_init();
    unsetenv("SDL_VIDEODRIVER");

    rcr_jni_init();
    rcr_jni_bind_sdl();
    rcr_egl_init();
    build_imports();

    int missing = rcr_load_modules();
    fprintf(stderr, "[rcr] module loaded, %d relocations unresolved\n", missing);

    nx_mod *game = nx_find_mod(GAME_SO);
    nx_protect(game);

    /* System.loadLibrary: the C++ constructors first, then JNI_OnLoad. */
    nx_run_init(game);
    typedef int (*onload)(void *vm, void *reserved);
    onload f = (onload)nx_lookup_in(game, "JNI_OnLoad");
    if (f)
        fprintf(stderr, "[rcr] JNI_OnLoad(%s) -> %#x\n", GAME_SO,
                f(rcr_jni_vm(), NULL));

    /* The surface, in SDLSurface's order. */
    surface_lifecycle();

    /* Controllers, before the game asks SDL for them. */
    rcr_input_init();

    /* SDL's Linux evdev support installs its own fault handlers; ours was
     * installed at start-up, so reinstall on top now that the system SDL
     * subsystems are up, otherwise a real crash is reported from the re-raise
     * instead of the fault site. */
    install_fault_handler();

    /* The game's own working directory is the APK's assets root: the three
     * data files are opened by bare name through SDL_RWops. */
    if (chdir(rcr_datadir) != 0)
        nx_die("cannot enter the data directory %s: %s", rcr_datadir,
               strerror(errno));

    /* SDL_main on its own thread, the way SDLThread runs it on Android; this
     * thread stays the UI thread and feeds input in. */
    pthread_t game_thread;
    if (pthread_create(&game_thread, NULL, run_native_init, NULL) != 0)
        nx_die("cannot start the SDL thread: %s", strerror(errno));

    fprintf(stderr, "[rcr] input pump on tid=%d\n", (int)syscall(SYS_gettid));
    int resumed = 0;
    void *game_result = NULL;
    for (;;) {
        if (termination_requested && !rcr_input_should_quit()) {
            fprintf(stderr, "[rcr] frontend signal: asking the game to quit\n");
            rcr_input_request_quit();
        }
        if (!resumed && rcr_egl_swap_count() > 0) {
            deliver_activity_resume();
            resumed = 1;
        }
        rcr_input_poll();
        if (pthread_tryjoin_np(game_thread, &game_result) == 0)
            break;
        usleep(4000);
    }

    int game_status = (int)(intptr_t)game_result;
    fprintf(stderr, "[rcr] the game has exited with status %d; leaving to "
                    "the frontend\n", game_status);
    rcr_input_close();
    rcr_audio_close();
    /* Leaving through _exit avoids the proprietary Mali teardown deadlock the
     * other NextOS ports hit on the way out -- no GL context is torn down. */
    fflush(NULL);
    /* Preserve normal process semantics exactly: _exit() exposes the low
     * eight bits to the parent just as returning from main() would. */
    _exit(game_status);
}
