/*
 * main.c -- native Bomb Chicken bootstrap for NextOS.
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
#include <sys/file.h>
#include <fcntl.h>

#include "nx_elf.h"
#include "bc.h"
#include "contract.h"

char bc_gamedir[1024];
char bc_datadir[1024];
char bc_apk[1024];
char bc_home[1024];
long bc_max_frames = 0;
int bc_trace_gl = 0;
int bc_capture_mode = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Bomb Chicken v44 is a normal, unprotected Unity 2022.3.39f1 IL2CPP build
 * (arm64 only, no PairIP/packer).  Keep the exact NativeLoader order and do
 * not introduce a synthetic bootstrap. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
};

extern const nx_import *bc_pthread_table(size_t *n);
extern const nx_import *bc_android_table(size_t *n);
extern const nx_import *bc_egl_table(size_t *n);

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
    const nx_import *p = bc_pthread_table(&np);
    const nx_import *an = bc_android_table(&na);
    const nx_import *eg = bc_egl_table(&ne);

    size_t bn;
    extern nx_import *bc_bionic_entries(size_t *n);
    nx_import *be = bc_bionic_entries(&bn);
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

void bc_watchdog_frame(void) { watchdog_frame++; }

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
                "[bc] watchdog: frame %lu has not returned in %ds; faulting "
                "the render thread so its stack is reported\n",
                last, watchdog_seconds);
        /* Deliver to the render thread specifically, not to the process: any
         * other thread would report a stack we already know is idle. */
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void bc_arm_frame_watchdog(void)
{
    const char *v = getenv("BC_WATCHDOG");
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

int bc_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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
const char *bc_mod_at(const void *addr, void **base_out)
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
    nx_verbose   = (v = getenv("BC_VERBOSE")) && *v != '0';
    bc_log_level = (v = getenv("BC_LOGCAT")) && *v != '0';
    bc_trace_jni = (v = getenv("BC_JNILOG")) && *v != '0';
    bc_trace_gl  = (v = getenv("BC_GLLOG")) && *v != '0';
    if ((v = getenv("BC_FRAMES")))
        bc_max_frames = strtol(v, NULL, 10);
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
        copy_path(bc_gamedir, sizeof bc_gamedir, arg, "game directory");
    else if (!getcwd(bc_gamedir, sizeof bc_gamedir))
        copy_path(bc_gamedir, sizeof bc_gamedir, ".", "game directory");
    join_path(bc_datadir, sizeof bc_datadir, bc_gamedir, "assets", NULL);
    join_path(bc_apk, sizeof bc_apk, bc_gamedir, "assets", NULL);
    join_path(bc_home, sizeof bc_home, bc_gamedir, "home", NULL);
    mkdir(bc_home, 0755);
}

int bc_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !bc_capture_mode)
            continue;
        join_path(path, sizeof path, bc_gamedir, "lib", LIBS[i].file);
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
        if (LIBS[i].capture_only && !bc_capture_mode)
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
    fprintf(stderr, "\n[bc] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[bc]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[bc]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[bc]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[bc]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[bc]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    bc_input_request_exit();
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

/* After the safe teardown (pause+save), any fault in the engine's thread
 * teardown is not a crash and a hung driver thread must not keep the display;
 * both converge on a clean _exit(0). Chrono Trigger recipe, proven on R36S. */
static void bc_shutdown_terminal(int sig)
{
    const char *m = (sig == SIGALRM)
        ? "[bc] teardown deadline; exiting\n"
        : "[bc] fault during teardown after save; exiting clean\n";
    ssize_t ignored = write(2, m, strlen(m));
    (void)ignored;
    _exit(0);
}

static void bc_install_shutdown_guards(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = bc_shutdown_terminal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    alarm(5); /* the safe part already ran; never hang the frontend */
}

static void run_unity(void)
{
    void *env = bc_jni_env();
    void *player = bc_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = bc_jni_activity();
    void *surface = bc_jret_obj("android/view/Surface");
    void *fn;

    bc_jni_set_unity_player(player);

    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[bc] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[bc] initJni OK\n");

    fn = bc_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = bc_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[bc] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = bc_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[bc] nativeFocusChanged(true) OK\n");
    }
    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[bc] nativeResume OK\n");
    }

    bc_audio_start(env);

    void *render = bc_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[bc] nativeRender loop%s\n",
            bc_max_frames > 0 ? " (test frame limit active)" : "");

    bc_input_init();

    /* Watchdog for a hung frame.  Unity installs its own crash handler, which
     * prints a symbolised backtrace of whichever thread faults -- so the way to
     * see where a stuck frame is stuck is to fault that exact thread on purpose.
     * Off unless BC_WATCHDOG names a timeout in seconds. */
    bc_arm_frame_watchdog();

    unsigned long frame = 0;
    const char *frame_us_env = getenv("BC_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
    struct timespec frame_start;
    int report_fps = getenv("BC_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        bc_watchdog_frame();
        bc_input_poll(env, player, frame);
        if (bc_input_exit_requested()) {
            fprintf(stderr, "[bc] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[bc] frame %lu keep=%u\n", frame, keep);
        if (report_fps && frame % 300 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - fps_mark.tv_sec) +
                        (now.tv_nsec - fps_mark.tv_nsec) / 1e9;
            if (dt > 0)
                fprintf(stderr, "[bc/fps] %.1f fps (300 frames in %.2fs)\n",
                        300.0 / dt, dt);
            fps_mark = now;
        }
        if (!keep) {
            fprintf(stderr, "[bc] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (bc_max_frames > 0 && frame >= (unsigned long)bc_max_frames) {
            fprintf(stderr, "[bc] test frame limit reached (%lu)\n", frame);
            break;
        }
        /* Pacing pelo TEMPO QUE SOBRA do orcamento do quadro, nunca um sleep
         * fixo somado ao trabalho: com swap bloqueando no vsync um sleep
         * cru de 16,67 ms derruba um jogo de acao para metade da taxa. */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long spent_us = (now.tv_sec - frame_start.tv_sec) * 1000000L +
                            (now.tv_nsec - frame_start.tv_nsec) / 1000L;
            long budget_us = frame_budget_us;
            if (budget_us > 0 && spent_us < budget_us)
                usleep((useconds_t)(budget_us - spent_us));
        }
    }

    /* Terminal teardown (Chrono Trigger recipe, proven on R36S/ArkOS). The
     * safe part -- focus-out and nativePause, which drives Unity's own save --
     * runs first. After it, we leave the process for good with _exit(0):
     * letting the Unity/IL2CPP static destructors run on `return 0` stalls or
     * dirties the Mali KMSDRM display and audio, so the frontend comes back to
     * a black screen even though the game exited 0. A fault in the engine's
     * thread teardown after the save must not read as a crash, and a hung
     * driver thread must not keep owning the display, so a fault handler and a
     * watchdog both converge on a clean _exit(0). */
    bc_install_shutdown_guards();

    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[bc] nativeFocusChanged(false) OK\n");
    }
    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[bc] nativePause OK\n");
    }
    bc_input_close();
    bc_audio_stop();
    fprintf(stderr, "[bc] safe teardown done; exiting cleanly\n");
    _exit(0);
}

/* UM JOGO SO: a trava vai no BINARIO, nunca so no script do launcher.  Um
 * script pode ser copiado, renomeado ou lancado por outro caminho; o executavel
 * e' o unico recurso que toda instancia tem em comum. */
static void claim_single_instance(void)
{
    static int lock_fd = -1;
    lock_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (lock_fd < 0)
        return;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr,
                "[bc] outra instancia do Bomb Chicken ja esta rodando; saindo\n");
        _exit(1);
    }
    /* Intencionalmente sem close(): a trava vale enquanto o processo viver. */
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    claim_single_instance();

    /* EmulationStation's application wrapper exports C.UTF-8.  This Android
     * Unity player was built against Bionic's locale ABI; when its native
     * startup crosses the host glibc C.UTF-8 locale, a small-string object is
     * overwritten and its stack canary fires before frame one.  Android's
     * invariant/POSIX locale is the matching behaviour for this port. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
    setenv("GC_DISABLE_INCREMENTAL", "1", 0);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    bc_apply_declared_contract();
    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    /* Firmwares com SONAME cruzado (libEGL.so.1 = Mesa sem driver) matam a
     * Unity com "Unable to initialize EGL!" + SIGTRAP.  O probe roda antes de
     * qualquer video e re-executa UMA vez com o blob coerente preloadado;
     * num sistema saudavel ele inicializa, termina e nao muda nada. */
    bc_glfix_set_argv(argv);
    bc_glfix_probe_before_video();

    fprintf(stderr, "[bc] Bomb Chicken compatibility loader -- gamedir %s\n",
            bc_gamedir);

    bc_jni_init();
    bc_egl_init();
    build_imports();

    int missing = bc_load_modules();
    fprintf(stderr, "[bc] modules loaded, %d relocations unresolved\n", missing);

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
    int main_version = main_onload(bc_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[bc] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        bc_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, bc_gamedir, "lib", NULL);
    void *loader_class =
        bc_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = bc_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        bc_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited || !il2->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d il2cpp_init=%d)",
               loaded, uni->inited, il2->inited);

    fprintf(stderr,
            "[bc] NativeLoader.load completed: libunity -> libil2cpp\n");
    run_unity();
    return 0;
}
