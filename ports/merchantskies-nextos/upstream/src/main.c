/*
 * main.c -- native Merchant of the Skies bootstrap for NextOS.
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
#include <sys/time.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/file.h>
#include <fcntl.h>

#include "nx_elf.h"
#include "gb.h"
#include "contract.h"

char st_gamedir[1024];
char st_datadir[1024];
char st_apk[1024];
char st_home[1024];
long st_max_frames = 0;
int st_trace_gl = 0;
int st_capture_mode = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Merchant of the Skies 2.0 is an IL2CPP build made with Unity 6000.4.2f1.
 * Keep the exact NativeLoader order and do not introduce a synthetic
 * bootstrap.  The repack's libAPKVISION/libstub/libgpdeku are never listed
 * here: neither libunity nor libil2cpp has them in DT_NEEDED, so the crack is
 * a Java/dex-side injection and is inert for a so-loader that runs no dex. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
};

extern const nx_import *st_pthread_table(size_t *n);
extern const nx_import *st_android_table(size_t *n);
extern const nx_import *st_egl_table(size_t *n);

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
    const nx_import *p = st_pthread_table(&np);
    const nx_import *an = st_android_table(&na);
    const nx_import *eg = st_egl_table(&ne);

    size_t bn;
    extern nx_import *st_bionic_entries(size_t *n);
    nx_import *be = st_bionic_entries(&bn);
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

int st_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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
const char *st_mod_at(const void *addr, void **base_out)
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
    nx_verbose   = (v = getenv("ST_VERBOSE")) && *v != '0';
    st_log_level = (v = getenv("ST_LOGCAT")) && *v != '0';
    st_trace_jni = (v = getenv("ST_JNILOG")) && *v != '0';
    st_trace_gl  = (v = getenv("ST_GLLOG")) && *v != '0';
    if ((v = getenv("ST_FRAMES")))
        st_max_frames = strtol(v, NULL, 10);
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
        copy_path(st_gamedir, sizeof st_gamedir, arg, "game directory");
    else if (!getcwd(st_gamedir, sizeof st_gamedir))
        copy_path(st_gamedir, sizeof st_gamedir, ".", "game directory");
    join_path(st_datadir, sizeof st_datadir, st_gamedir, "assets", NULL);
    join_path(st_apk, sizeof st_apk, st_gamedir, "assets", NULL);
    join_path(st_home, sizeof st_home, st_gamedir, "home", NULL);
    mkdir(st_home, 0755);
}

int st_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !st_capture_mode)
            continue;
        join_path(path, sizeof path, st_gamedir, "lib", LIBS[i].file);
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
        if (LIBS[i].capture_only && !st_capture_mode)
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
/* Name an address against the modules we mapped ourselves first (they have no
 * link map, so nothing else can place them), then against /proc/self/maps for
 * everything the host loader owns. */
static void place_addr(unsigned long a, char *out, size_t n)
{
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (a >= b && a < b + m->span) {
            snprintf(out, n, "%s+%#lx", m->name, a - b);
            return;
        }
    }
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long lo = 0, hi = 0;
            if (sscanf(line, "%lx-%lx", &lo, &hi) != 2 || a < lo || a >= hi)
                continue;
            char *path = strchr(line, '/');
            char *nl = path ? strchr(path, '\n') : NULL;
            if (nl)
                *nl = 0;
            snprintf(out, n, "%s+%#lx", path ? path : "[anon]", a - lo);
            fclose(f);
            return;
        }
        fclose(f);
    }
    snprintf(out, n, "?");
}

/* ST_SAMPLE=<ms>: a CPU-time profiling tick that prints where the thread that
 * is burning the CPU actually is.  A spin inside guest code blocks nothing, so
 * /proc says only "running" and an external signal is swallowed once the guest
 * installs its own handlers -- sampling from inside is the only view left. */

/* Unity builds without frame pointers, so an x29 walk yields stale stack junk
 * past the first frame or two (it fooled us once already).  Scan the raw stack
 * instead and keep only words that are VERIFIED return addresses: the value
 * must land inside a module we mapped, and the instruction right before it
 * must actually be a bl/blr.  That turns "some pointer shaped like code" into
 * "somebody really called here". */
static int is_guest_text(unsigned long a, char *out, size_t n)
{
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (a > b + 8 && a < b + m->span) {
            snprintf(out, n, "%s+%#lx", m->name, a - b);
            return 1;
        }
    }
    return 0;
}

/* Upper bound for the scan.  Recorded on the main thread when the sampler is
 * armed: everything between the sampled sp and this address is stack that has
 * really been used, so it is mapped.  Walking a fixed 64K past sp instead runs
 * off the end of the stack and faults inside the signal handler. */
static unsigned long stack_top_hint;

static void scan_stack_for_callers(unsigned long sp, int max)
{
    char where[256];
    int found = 0;
    unsigned long end = stack_top_hint;
    if (end <= sp || end - sp > (256UL << 10))
        end = sp + (16UL << 10);
    for (unsigned long p = sp; p < end && found < max; p += 8) {
        unsigned long v = *(unsigned long *)p;
        if ((v & 3) || !is_guest_text(v, where, sizeof where))
            continue;
        uint32_t prev = *(uint32_t *)(v - 4);
        int is_bl  = (prev & 0xFC000000u) == 0x94000000u;
        int is_blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
        if (!is_bl && !is_blr)
            continue;
        fprintf(stderr, "[st/stack]    %s  (via %s)\n", where,
                is_bl ? "bl" : "blr");
        found++;
    }
}

static void on_sample(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    unsigned long lr = (unsigned long)u->uc_mcontext.regs[30];
    char a[256], b[256];
    place_addr(pc, a, sizeof a);
    place_addr(lr, b, sizeof b);
    fprintf(stderr, "[st/sample] pc=%s  lr=%s\n", a, b);
    scan_stack_for_callers((unsigned long)u->uc_mcontext.sp, 12);
    fflush(stderr);
}

static void st_arm_sampler(void)
{
    const char *v = getenv("ST_SAMPLE");
    if (!v || !*v)
        return;
    int ms = atoi(v);
    if (ms <= 0)
        ms = 500;
    stack_top_hint = (unsigned long)__builtin_frame_address(0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it;
    it.it_interval.tv_sec = ms / 1000;
    it.it_interval.tv_usec = (ms % 1000) * 1000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
    fprintf(stderr, "[st] sampler armed at %d ms of CPU time\n", ms);
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;

    /* A self-sent signal (si_code <= 0) is not a CPU fault: something in the
     * process deliberately queued it.  Guest runtimes do that to probe their
     * own handler, so swallowing it with _exit turns a survivable probe into a
     * dead boot.  Let a bounded number through and see whether the boot
     * continues; ST_SELFSIG=0 restores the old fatal behaviour. */
    if (si && si->si_code <= 0 && si->si_pid == getpid()) {
        static volatile int passed;
        const char *off = getenv("ST_SELFSIG");
        if ((!off || strcmp(off, "0") != 0) && passed < 16) {
            passed++;
            fprintf(stderr, "[st] self-sent signal %d (#%d) resumed, not fatal\n",
                    sig, passed);
            fflush(stderr);
            return;
        }
    }

    /* Walking the frame chain can itself fault; never recurse. */
    static volatile int in_handler;
    if (in_handler)
        _exit(3);
    in_handler = 1;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    /* si_code <= 0 means the signal was QUEUED BY A PROCESS (raise/tgkill),
     * not raised by the CPU.  In that case si_addr is not a fault address at
     * all -- the union holds the sender's pid/uid -- and printing it as a
     * pointer sends the reader hunting for a bad dereference that never
     * happened. */
    int code = si ? si->si_code : 0;
    fprintf(stderr, "\n[st] signal %d at pc=%#lx si_code=%d (%s)\n", sig, pc,
            code, code <= 0 ? "SELF-SENT / queued" : "CPU fault");
    if (code <= 0)
        fprintf(stderr, "[st]   raised by pid=%d uid=%d  (si_addr is NOT a "
                        "fault address here)\n",
                si ? (int)si->si_pid : -1, si ? (int)si->si_uid : -1);
    else
        fprintf(stderr, "[st]   fault addr=%p\n", si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[st]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[st]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[st]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[st]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[st]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);

    /* The frame chain is the only way to name the CALLER: a self-sent signal
     * says nothing about who sent it, and an import wrapper only sees calls
     * that go through the import table. */
    {
        char where[256];
        unsigned long pc_lr[2] = { pc, (unsigned long)u->uc_mcontext.regs[30] };
        for (int i = 0; i < 2; i++) {
            place_addr(pc_lr[i], where, sizeof where);
            fprintf(stderr, "[st]   #%-2d %-3s %#018lx  %s\n", i,
                    i ? "LR" : "PC", pc_lr[i], where);
        }
        unsigned long fp = (unsigned long)u->uc_mcontext.regs[29];
        unsigned long sp = (unsigned long)u->uc_mcontext.sp;
        for (int i = 2; i < 20 && fp; i++) {
            /* A frame record is { next_fp, lr } and the chain must climb the
             * stack in 16-byte aligned steps; anything else is garbage and
             * following it would fault. */
            if ((fp & 15) || fp < sp || fp - sp > (64UL << 20))
                break;
            unsigned long next = *(unsigned long *)fp;
            unsigned long lr = *(unsigned long *)(fp + 8);
            if (!lr)
                break;
            place_addr(lr, where, sizeof where);
            fprintf(stderr, "[st]   #%-2d %-3s %#018lx  %s\n", i, "", lr, where);
            if (next <= fp)
                break;
            fp = next;
        }
    }

    /* A PC outside our own mappings lands in a host library the loader never
     * placed, so the module bases above cannot name it.  Print the matching
     * /proc/self/maps rows for the PC and for LR instead. */
    {
        unsigned long lr = (unsigned long)u->uc_mcontext.regs[30];
        FILE *m = fopen("/proc/self/maps", "r");
        if (m) {
            char line[512];
            while (fgets(line, sizeof line, m)) {
                unsigned long lo = 0, hi = 0;
                if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
                    continue;
                if ((pc >= lo && pc < hi) || (lr >= lo && lr < hi)) {
                    char *nl = strchr(line, '\n');
                    if (nl)
                        *nl = 0;
                    fprintf(stderr, "[st]   map %s%s%s\n", line,
                            (pc >= lo && pc < hi) ? "   <-- PC" : "",
                            (lr >= lo && lr < hi) ? "   <-- LR" : "");
                }
            }
            fclose(m);
        }
    }
    fflush(stderr);
    (void)fdatasync(STDERR_FILENO);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    st_input_request_exit();
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

    /* Watchdog diagnostic sample.  Unlike the old implementation this signal
     * is never used as the terminal action; diagnostics.c owns the deadline. */
    struct sigaction sample;
    memset(&sample, 0, sizeof sample);
    sample.sa_sigaction = on_sample;
    sample.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sample.sa_mask);
    sigaction(SIGUSR2, &sample, NULL);

    /* SIGTERM/SIGINT seguem o caminho do SELECT+START (pause/save/saída),
     * nunca morte seca: frontends e supervisores mandam TERM primeiro. */
    struct sigaction quit;
    memset(&quit, 0, sizeof quit);
    quit.sa_handler = on_exit_signal;
    sigemptyset(&quit.sa_mask);
    sigaction(SIGTERM, &quit, NULL);
    sigaction(SIGINT, &quit, NULL);
}

/* Unity 6 split the player: UnityPlayer keeps the shared natives (initJni,
 * nativeUnitySendMessage, ...) while the lifecycle ones (nativeRender,
 * nativePause, nativeResume, nativeDone, nativeRecreateGfxState, ...) moved to
 * the concrete com/unity3d/player/UnityPlayerForActivityOrService.  Look in the
 * concrete class first and fall back to the base so a pre-6 layout still
 * resolves. */
static void *unity_native(const char *name)
{
    void *fn = st_jni_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", name);
    if (!fn)
        fn = st_jni_native("com/unity3d/player/UnityPlayer", name);
    return fn;
}


/* The reference port used build-specific libunity offsets to force its GLES3
 * device. Merchant of the Skies ships a different Unity build, so touching either
 * address would be memory corruption. The logical GLES3 facade is the only
 * renderer path allowed until a target-specific decision point is proven from
 * this exact libunity hash and guarded by opcode validation. */
static void st_force_gfx_device(void)
{
    if (getenv("ST_FORCE_GFX"))
        fprintf(stderr, "[st/gfx] ST_FORCE_GFX rejected: no verified offset "
                        "exists for this target build\n");
    fprintf(stderr, "[st/gfx] logical GLES3 facade; no binary patch applied\n");
}

static void run_unity(void)
{
    void *env = st_jni_env();
    void *player = st_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = st_jni_activity();
    void *surface = st_jret_obj("android/view/Surface");
    void *fn;

    st_jni_set_unity_player(player);

    st_diag_phase("unity-gfx-selection");
    st_force_gfx_device();

    st_diag_phase("unity-init-jni");
    fn = unity_native("initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    /* Unity 6 registers initJni as (Landroid/content/Context;I)V -- the trailing
     * int is the context type and 0 == ActivityOrService (1 == GameActivity).
     * Calling it with the pre-6 three-argument shape leaves that register
     * holding junk; Unity then logs "Unknown context type: <junk>" and its
     * error path falls through into a three-instruction infinite loop, because
     * the retry uses bl where the normal path tail-calls and the stale LR
     * points back at the return site.  The hang looks like a render deadlock
     * and is really this missing argument. */
    fprintf(stderr, "[st] initJni(contextType=0 ActivityOrService)...\n");
    ((void (*)(void *, void *, void *, int))fn)(env, player, activity, 0);
    fprintf(stderr, "[st] initJni OK\n");

    st_diag_phase("unity-create-gfx");
    fn = unity_native("nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[st] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[st] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[st] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[st] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = unity_native("nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[st] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = unity_native("nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[st] nativeFocusChanged(true) OK\n");
    }
    fn = unity_native("nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[st] nativeResume OK\n");
    }

    st_diag_phase("unity-resume-complete");
    st_audio_start(env);

    void *render = unity_native("nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[st] nativeRender loop%s\n",
            st_max_frames > 0 ? " (test frame limit active)" : "");

    st_input_init();
    st_diag_render_ready();

    unsigned long frame = 0;
    const char *frame_us_env = getenv("ST_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
    struct timespec frame_start;
    int report_fps = getenv("ST_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        st_input_poll(env, player, frame);
        /* Android Tasks deliver listeners through the main Looper.  The JNI
         * bridge queues those callbacks and drains them here, on Unity's
         * render/main thread, before the next native frame. */
        st_jni_pump_callbacks();
        if (st_input_exit_requested()) {
            fprintf(stderr, "[st] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        st_diag_frame_complete(frame);
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[st] frame %lu keep=%u\n", frame, keep);
        if (report_fps && frame % 300 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - fps_mark.tv_sec) +
                        (now.tv_nsec - fps_mark.tv_nsec) / 1e9;
            if (dt > 0)
                fprintf(stderr, "[st/fps] %.1f fps (300 frames in %.2fs)\n",
                        300.0 / dt, dt);
            fps_mark = now;
        }
        if (!keep) {
            fprintf(stderr, "[st] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (st_max_frames > 0 && frame >= (unsigned long)st_max_frames) {
            fprintf(stderr, "[st] test frame limit reached (%lu)\n", frame);
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

    /* Reproduce the Activity/UnityPlayerForActivityOrService teardown from
     * this APK: focus-out, nativePause(), then shutdown() -> nativeDone().
     * Android's normal standalone path kills the process when nativeDone()
     * returns true; the Unity-as-a-Library path instead calls
     * NativeLoader.unload(). Never conceal a fault or replace these calls with
     * an unconditional synthetic exit. */
    fn = unity_native("nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[st] nativeFocusChanged(false) OK\n");
    }
    fn = unity_native("nativePause");
    int pause_requested_shutdown = 0;
    if (fn) {
        pause_requested_shutdown =
            ((uint8_t (*)(void *, void *))fn)(env, player) != 0;
        fprintf(stderr, "[st] nativePause OK (shutdown=%d)\n",
                pause_requested_shutdown);
    }

    /* The save is already on disk at this point: nativePause() is what makes
     * the game's ObserverEngine flush session.sav, and the log confirms it.
     *
     * MEASURED on this target: calling nativeDone() here never returns.  The
     * process sat in it indefinitely after "nativePause OK", with the render
     * loop already stopped and the save written -- a hang in engine teardown,
     * not in the game.  Android hides this because the platform kills the
     * process anyway; on a Mali-450 fbdev host, tearing the GL stack down on
     * the way out is exactly the known kernel-wedging path.
     *
     * So the port exits the way the house rule requires: everything the OWNER
     * cares about is flushed first, then _exit(0).  Never a return from main
     * -- that would run atexit, let SDL unwind the GL context and can take the
     * device down with it.  ST_NATIVE_DONE=1 restores the full engine teardown
     * for anyone who wants to investigate the hang later; it is off by default
     * because it does not return. */
    st_input_close();
    st_audio_stop();
    st_diag_sync();

    if (!getenv("ST_NATIVE_DONE")) {
        fprintf(stderr, "[st] save flushed by nativePause; exiting without "
                        "engine teardown (ST_NATIVE_DONE=1 to force it)\n");
        st_diag_sync();
        _exit(0);
    }

    fn = unity_native("nativeDone");
    if (!fn)
        nx_die("Unity did not register nativeDone");
    int process_kill_requested =
        ((uint8_t (*)(void *, void *))fn)(env, player) != 0;
    fprintf(stderr, "[st] nativeDone OK (process-kill=%d)\n",
            process_kill_requested);
    st_diag_sync();

    if (process_kill_requested) {
        fprintf(stderr, "[st] standalone lifecycle complete; process exit\n");
        st_diag_sync();
        _exit(0); /* mirrors android.os.Process.killProcess after nativeDone */
    }

    void *native_unload =
        st_jni_native("com/unity3d/player/NativeLoader", "unload");
    if (!native_unload)
        nx_die("libmain did not register NativeLoader.unload");
    void *loader_class =
        st_jret_class("com/unity3d/player/NativeLoader");
    int unloaded = ((uint8_t (*)(void *, void *))native_unload)(
        env, loader_class) != 0;
    if (!unloaded)
        nx_die("NativeLoader.unload failed");
    fprintf(stderr, "[st] Unity-as-a-Library lifecycle unloaded cleanly\n");
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
                "[st] outra instancia do Merchant of the Skies ja esta rodando; saindo\n");
        _exit(1);
    }
    /* Intencionalmente sem close(): a trava vale enquanto o processo viver. */
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    st_diag_open_persistent_log();
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

    st_apply_declared_contract();
    read_env();
    if (getenv("ST_TLSLOG")) { extern void st_tls_trace_enable(void); st_tls_trace_enable(); }
    st_arm_sampler();
    extern void st_android_prepare_main_looper(void);
    st_android_prepare_main_looper();
    install_fault_handler();
    stack_top_hint = (unsigned long)__builtin_frame_address(0);
    setup_paths(argc > 1 ? argv[1] : NULL);
    st_diag_watchdog_start();
    st_diag_phase("bootstrap-paths-ready");

    fprintf(stderr, "[st] Merchant of the Skies compatibility loader -- gamedir %s\n",
            st_gamedir);

    st_diag_phase("host-shims");
    st_jni_init();
    if (getenv("ST_NETFLIX_SELFTEST"))
        return st_jni_netflix_selftest();
    st_egl_init();
    build_imports();

    st_diag_phase("module-map-relocate");
    int missing = st_load_modules();
    fprintf(stderr, "[st] modules loaded, %d relocations unresolved\n", missing);

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("required Unity module disappeared after relocation");

    /* System.load(libmain.so): its constructors run before JNI_OnLoad. */
    st_diag_phase("libmain-init-array");
    nx_run_init(main_mod);
    typedef int (*onload)(void *vm, void *reserved);
    onload main_onload = (onload)nx_lookup_in(main_mod, "JNI_OnLoad");
    if (!main_onload)
        nx_die("libmain.so has no JNI_OnLoad");
    st_diag_phase("libmain-jni-onload");
    int main_version = main_onload(st_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[st] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        st_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, st_gamedir, "lib", NULL);
    void *loader_class =
        st_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = st_jret_str(libdir);
    st_diag_phase("native-loader-unity-il2cpp");
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        st_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited || !il2->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d il2cpp_init=%d)",
               loaded, uni->inited, il2->inited);

    fprintf(stderr,
            "[st] NativeLoader.load completed: libunity -> libil2cpp\n");
    st_diag_phase("unity-lifecycle");
    run_unity();
    return 0;
}
