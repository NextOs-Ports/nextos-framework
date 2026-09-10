/*
 * main.c -- native Gunbrick bootstrap for NextOS.
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
#include <time.h>
#include <ucontext.h>
#include <sys/file.h>
#include <fcntl.h>
#include <malloc.h>

#include "nx_elf.h"
#include "gb.h"
#include "jni_refs.h"
#include "framework_bridge.h"

char gb_gamedir[1024];
char gb_datadir[1024];
char gb_apk[1024];
char gb_home[1024];
long gb_max_frames = 0;
int gb_trace_gl = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Gunbrick v44 is a normal, unprotected Unity 2022.3.39f1 IL2CPP build
 * (arm64 only, no PairIP/packer).  Keep the exact NativeLoader order and do
 * not introduce a synthetic bootstrap. */
static const struct {
    const char *file, *soname;
    int required;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1 },
    { "libunity.so",      "libunity.so",      1 },
    { "libil2cpp.so",     "libil2cpp.so",     1 },
};

extern const nx_import *gb_pthread_table(size_t *n);
extern const nx_import *gb_android_table(size_t *n);
extern const nx_import *gb_egl_table(size_t *n);

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
    const nx_import *p = gb_pthread_table(&np);
    const nx_import *an = gb_android_table(&na);
    const nx_import *eg = gb_egl_table(&ne);

    size_t bn;
    extern nx_import *gb_bionic_entries(size_t *n);
    nx_import *be = gb_bionic_entries(&bn);
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
    if (nx_set_imports(all, all_n) != 0)
        nx_die("host import table is ambiguous or invalid");
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, egl %zu)",
           all_n, bn, np, na, ne);
}

/* Report the modules mapped by the native loader. */
int gb_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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
const char *gb_mod_at(const void *addr, void **base_out)
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
    nx_verbose   = (v = getenv("GB_VERBOSE")) && *v != '0';
    gb_log_level = (v = getenv("GB_LOGCAT")) && *v != '0';
    gb_trace_jni = (v = getenv("GB_JNILOG")) && *v != '0';
    gb_trace_gl  = (v = getenv("GB_GLLOG")) && *v != '0';
    if ((v = getenv("GB_FRAMES")))
        gb_max_frames = strtol(v, NULL, 10);
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
    char resolved[4096];
    struct stat status;

    if (arg && *arg) {
        if (!realpath(arg, resolved))
            nx_die("cannot resolve game directory '%s': %s", arg,
                   strerror(errno));
        copy_path(gb_gamedir, sizeof gb_gamedir, resolved, "game directory");
    } else if (!getcwd(gb_gamedir, sizeof gb_gamedir)) {
        copy_path(gb_gamedir, sizeof gb_gamedir, ".", "game directory");
    }
    if (stat(gb_gamedir, &status) != 0 || !S_ISDIR(status.st_mode))
        nx_die("game directory is unavailable: %s", gb_gamedir);
    join_path(gb_datadir, sizeof gb_datadir, gb_gamedir, "assets", NULL);
    join_path(gb_apk, sizeof gb_apk, gb_gamedir, "assets", NULL);
    join_path(gb_home, sizeof gb_home, gb_gamedir, "home", NULL);
    if (mkdir(gb_home, 0755) != 0 && errno != EEXIST)
        nx_die("cannot create owner-data directory %s: %s", gb_home,
               strerror(errno));
}

int gb_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        join_path(path, sizeof path, gb_gamedir, "lib", LIBS[i].file);
        nx_mod *m = nx_load(path, LIBS[i].soname);
        if (!m) {
            if (LIBS[i].required)
                nx_die("cannot load %s (expected at %s)", LIBS[i].file, path);
            nx_log("optional %s missing", LIBS[i].file);
        }
    }
    /* nxloader separates local relocation from import resolution.  Register
     * every relocated module as a provider before resolving any consumer so
     * libunity/libil2cpp cross-imports never depend on accidental load order. */
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m && nx_relocate(m) != 0)
            return -1;
    }
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m && nx_resolve(m) != 0)
            return -1;
    }
    return 0;
}

/* A fault inside a module we mapped ourselves has no symbols and no link map,
 * so the only way to place it is to print the PC against the module bases.
 * Always on: it costs nothing until something goes wrong. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    fprintf(stderr, "\n[gunbrick] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[gunbrick]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[gunbrick]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[gunbrick]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[gunbrick]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[gunbrick]   lr=%016lx sp=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    gb_input_request_exit();
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
static void gb_shutdown_terminal(int sig)
{
    const char *m = (sig == SIGALRM)
        ? "[gunbrick] teardown deadline; exiting\n"
        : "[gunbrick] fault during teardown after save; exiting clean\n";
    ssize_t ignored = write(2, m, strlen(m));
    (void)ignored;
    _exit(0);
}

static void gb_install_shutdown_guards(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = gb_shutdown_terminal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    alarm(5); /* the safe part already ran; never hang the frontend */
}

static int jni_scope_begin_or_die(void)
{
    int token = gb_jni_ref_scope_begin();
    if (token < 0)
        nx_die("fake JNI local-frame stack exhausted");
    return token;
}

typedef struct gb_memory_snapshot {
    unsigned long available_kib;
    unsigned long swap_free_kib;
    unsigned long swap_total_kib;
} gb_memory_snapshot;

/* Android delivers Activity.onLowMemory/onTrimMemory to Unity when the host
 * approaches its memory limit.  A native Linux launcher has no Activity, so
 * reproduce that platform callback from live pressure evidence instead of
 * letting the Mali allocation which loads the next scene become the OOM
 * trigger.  This is deliberately sampled between render frames; Unity's
 * nativeLowMemory must run on the same player/GL thread as nativeRender. */
static int gb_memory_snapshot_read(gb_memory_snapshot *snapshot)
{
    FILE *stream;
    char line[160];

    if (!snapshot)
        return -1;
    memset(snapshot, 0, sizeof *snapshot);
    stream = fopen("/proc/meminfo", "r");
    if (!stream)
        return -1;
    while (fgets(line, sizeof line, stream)) {
        if (strncmp(line, "MemAvailable:", 13) == 0)
            snapshot->available_kib = strtoul(line + 13, NULL, 10);
        else if (strncmp(line, "SwapFree:", 9) == 0)
            snapshot->swap_free_kib = strtoul(line + 9, NULL, 10);
        else if (strncmp(line, "SwapTotal:", 10) == 0)
            snapshot->swap_total_kib = strtoul(line + 10, NULL, 10);
    }
    fclose(stream);
    return snapshot->available_kib != 0 ? 0 : -1;
}

static unsigned long gb_env_kib(const char *name, unsigned long fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (!value || !*value)
        return fallback;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    return errno == 0 && end && *end == '\0' ? parsed : fallback;
}

static void run_unity(void)
{
    void *env = gb_jni_env();
    void *player = gb_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = gb_jni_activity();
    void *surface = gb_jret_obj("android/view/Surface");
    void *fn;

    gb_jni_set_unity_player(player);

    fn = gb_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[gunbrick] initJni...\n");
    int scope = jni_scope_begin_or_die();
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    gb_jni_ref_scope_end(scope);
    fprintf(stderr, "[gunbrick] initJni OK\n");

    fn = gb_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[gunbrick] nativeRecreateGfxState(surfaceCreated)...\n");
    scope = jni_scope_begin_or_die();
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    gb_jni_ref_scope_end(scope);
    fprintf(stderr, "[gunbrick] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[gunbrick] nativeRecreateGfxState(surfaceChanged)...\n");
    scope = jni_scope_begin_or_die();
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    gb_jni_ref_scope_end(scope);
    fprintf(stderr, "[gunbrick] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = gb_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        scope = jni_scope_begin_or_die();
        ((void (*)(void *, void *))fn)(env, player);
        gb_jni_ref_scope_end(scope);
        fprintf(stderr, "[gunbrick] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = gb_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        scope = jni_scope_begin_or_die();
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        gb_jni_ref_scope_end(scope);
        fprintf(stderr, "[gunbrick] nativeFocusChanged(true) OK\n");
    }
    fn = gb_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        scope = jni_scope_begin_or_die();
        ((void (*)(void *, void *))fn)(env, player);
        gb_jni_ref_scope_end(scope);
        fprintf(stderr, "[gunbrick] nativeResume OK\n");
    }

    gb_audio_start(env);

    void *render = gb_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    void *low_memory = gb_jni_native("com/unity3d/player/UnityPlayer",
                                     "nativeLowMemory");
    fprintf(stderr, "[gunbrick] nativeRender loop%s\n",
            gb_max_frames > 0 ? " (test frame limit active)" : "");
    fprintf(stderr, "[gunbrick/memory] Android low-memory callback %s\n",
            low_memory ? "ready" : "not registered");

    if (gb_input_init() != 0)
        nx_die("nxinput initialization failed");

    unsigned long frame = 0;
    unsigned long last_low_memory_frame = 0;
    const unsigned long low_memory_kib =
        gb_env_kib("GB_LOWMEM_KB", 128UL * 1024UL);
    const unsigned long low_swap_kib =
        gb_env_kib("GB_LOWMEM_SWAP_KB", 64UL * 1024UL);
    const unsigned long low_memory_cooldown =
        gb_env_kib("GB_LOWMEM_COOLDOWN_FRAMES", 900UL);
    int framework_ready = 0;
    const char *frame_us_env = getenv("GB_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
    struct timespec frame_start;
    int report_fps = getenv("GB_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        scope = jni_scope_begin_or_die();
        int requested_low_memory = 0;
        gb_memory_snapshot memory;
        if (low_memory && low_memory_kib != 0 && frame != 0 &&
            frame % 120UL == 0 &&
            gb_memory_snapshot_read(&memory) == 0) {
            int ram_low = memory.available_kib < low_memory_kib;
            int swap_low = memory.swap_total_kib != 0 &&
                           memory.swap_free_kib < low_swap_kib;
            int cooled_down = last_low_memory_frame == 0 ||
                              frame - last_low_memory_frame >=
                                  low_memory_cooldown;
            if ((ram_low || swap_low) && cooled_down) {
                fprintf(stderr,
                        "[gunbrick/memory] pressure avail=%luMiB "
                        "swap=%lu/%luMiB -> nativeLowMemory (frame %lu)\n",
                        memory.available_kib / 1024UL,
                        memory.swap_free_kib / 1024UL,
                        memory.swap_total_kib / 1024UL, frame);
                ((void (*)(void *, void *))low_memory)(env, player);
                last_low_memory_frame = frame;
                requested_low_memory = 1;
            }
        }
        if (requested_low_memory)
            (void)malloc_trim(0);
        gb_input_poll(env, player, frame);
        if (gb_input_exit_requested()) {
            gb_jni_ref_scope_end(scope);
            fprintf(stderr, "[gunbrick] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        gb_jni_ref_scope_end(scope);
        frame++;
        if (!framework_ready &&
            (frame == 1 || frame == 30 || frame == 120)) {
            framework_ready = gb_framework_ready() == 0;
            if (framework_ready)
                fprintf(stderr,
                        "[gunbrick/framework] runtime requirements ready at frame %lu\n",
                        frame);
            else if (frame == 120)
                nx_die("runtime capability requirements are incomplete");
        }
        if (nx_verbose && (frame <= 10 || frame % 300 == 0))
            fprintf(stderr, "[gunbrick] frame %lu keep=%u\n", frame, keep);
        if (report_fps && frame % 300 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - fps_mark.tv_sec) +
                        (now.tv_nsec - fps_mark.tv_nsec) / 1e9;
            if (dt > 0)
                fprintf(stderr, "[gunbrick/fps] %.1f fps (300 frames in %.2fs)\n",
                        300.0 / dt, dt);
            fps_mark = now;
        }
        if (!keep) {
            fprintf(stderr, "[gunbrick] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (gb_max_frames > 0 && frame >= (unsigned long)gb_max_frames) {
            fprintf(stderr, "[gunbrick] test frame limit reached (%lu)\n", frame);
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
     * runs first; then _exit(0). Letting the Unity/IL2CPP static destructors
     * run on `return 0` stalls or dirties the Mali KMSDRM display and audio,
     * so the frontend returns to a black screen even on exit 0. A fault in the
     * engine's thread teardown after the save and a hung driver thread both
     * converge on a clean _exit(0). */
    gb_install_shutdown_guards();

    fn = gb_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        scope = jni_scope_begin_or_die();
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        gb_jni_ref_scope_end(scope);
        fprintf(stderr, "[gunbrick] nativeFocusChanged(false) OK\n");
    }
    fn = gb_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        scope = jni_scope_begin_or_die();
        ((void (*)(void *, void *))fn)(env, player);
        gb_jni_ref_scope_end(scope);
        fprintf(stderr, "[gunbrick] nativePause OK\n");
    }
    gb_input_close();
    gb_audio_stop();
    fprintf(stderr, "[gunbrick] safe teardown done; exiting cleanly\n");
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
                "[gunbrick] outra instancia do Gunbrick ja esta rodando; saindo\n");
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
    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    if (gb_framework_preflight(gb_gamedir) != 0)
        nx_die("framework preflight/requirements failed");

    fprintf(stderr, "[gunbrick] Gunbrick for NextOS -- gamedir %s\n", gb_gamedir);

    gb_jni_init();
    gb_egl_init();
    build_imports();

    if (gb_load_modules() != 0)
        nx_die("strict guest relocation/provider resolution failed");
    fprintf(stderr,
            "[gunbrick] modules loaded; all strong imports resolved transactionally\n");

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("required Unity module disappeared after relocation");

    /* System.load(libmain.so): its constructors run before JNI_OnLoad. */
    if (nx_run_init(main_mod) != 0)
        nx_die("libmain.so initializer transaction failed");
    int32_t main_version = 0;
    int jni_scope = jni_scope_begin_or_die();
    int onload_result =
        nx_call_jni_onload(main_mod, gb_jni_vm(), &main_version);
    gb_jni_ref_scope_end(jni_scope);
    if (onload_result != 0)
        nx_die("JNI_OnLoad(libmain.so) rejected");
    fprintf(stderr, "[gunbrick] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        gb_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, gb_gamedir, "lib", NULL);
    jni_scope = jni_scope_begin_or_die();
    void *loader_class =
        gb_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = gb_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        gb_jni_env(), loader_class, loader_path);
    gb_jni_ref_scope_end(jni_scope);
    if (!loaded || !uni->ready || !il2->ready)
        nx_die("NativeLoader.load failed (result=%d unity_ready=%d il2cpp_ready=%d)",
               loaded, uni->ready, il2->ready);

    fprintf(stderr,
            "[gunbrick] NativeLoader.load completed: libunity -> libil2cpp\n");
    run_unity();
    return 0;
}
