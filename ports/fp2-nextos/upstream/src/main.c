/*
 * main.c -- native Freedom Planet 2 bootstrap for NextOS.
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
#include "nxgl_frame_proof_adapter.h"
#include "fp2_graphics_contract.h"

char fp2_gamedir[1024];
char fp2_datadir[1024];
char fp2_apk[1024];
char fp2_home[1024];
long fp2_max_frames = 0;
#ifndef FP2_RELEASE_BUILD
int fp2_trace_gl = 0;
#endif
int fp2_capture_mode = 0;

extern void *fp2_gl_raw_sym(const char *name);

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Freedom Planet 2 1.2.8 is an IL2CPP build made with Unity 2018.4.36f1.
 * MEDIDO neste APK: libmain.so (6.144 B) so conhece "libunity.so" e registra
 * com/unity3d/player/NativeLoader.load(Ljava/lang/String;)Z; o libil2cpp e'
 * aberto depois, pelo proprio libunity.  Nao existe libfmod/libresonance
 * separada: o FMOD esta DENTRO do libunity.so (string org/fmod/FMODAudioDevice
 * medida la dentro), entao nao ha JNI_OnLoad de lib de audio para chamar.
 * Mantenha a ordem exata do NativeLoader e nao invente bootstrap sintetico. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
};

extern const nx_import *fp2_pthread_table(size_t *n);
extern const nx_import *fp2_android_table(size_t *n);
extern const nx_import *fp2_egl_table(size_t *n);
extern const nx_import *fp2_aaudio_table(size_t *n);
extern const nx_import *fp2_media_table(size_t *n);

/* One combined, sorted import table: bionic + pthread bridge + libandroid +
 * EGL + AAudio.  A Unity 6 nunca consulta OpenSL ES. */
static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne, naa, nm;
    const nx_import *p = fp2_pthread_table(&np);
    const nx_import *an = fp2_android_table(&na);
    const nx_import *eg = fp2_egl_table(&ne);
    const nx_import *aa = fp2_aaudio_table(&naa);
    const nx_import *md = fp2_media_table(&nm);

    size_t bn;
    extern nx_import *fp2_bionic_entries(size_t *n);
    nx_import *be = fp2_bionic_entries(&bn);
    all = calloc(bn + np + na + ne + naa + nm + 8, sizeof *all);
    all_n = 0;
    for (size_t i = 0; i < bn; i++)
        all[all_n++] = be[i];
    for (size_t i = 0; i < np; i++)
        all[all_n++] = p[i];
    for (size_t i = 0; i < na; i++)
        all[all_n++] = an[i];
    for (size_t i = 0; i < ne; i++)
        all[all_n++] = eg[i];
    for (size_t i = 0; i < naa; i++)
        all[all_n++] = aa[i];
    for (size_t i = 0; i < nm; i++)
        all[all_n++] = md[i];
    qsort(all, all_n, sizeof *all, imp_cmp);
    nx_set_imports(all, all_n);
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, "
           "egl %zu, aaudio %zu, mediandk %zu)", all_n, bn, np, na, ne, naa,
           nm);
}

int fp2_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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
const char *fp2_mod_at(const void *addr, void **base_out)
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
#ifdef FP2_RELEASE_BUILD
    nx_verbose = 0;
    fp2_max_frames = 0;
#else
    const char *v;
    nx_verbose   = (v = getenv("FP2_VERBOSE")) && *v != '0';
    fp2_log_level = (v = getenv("FP2_LOGCAT")) && *v != '0';
    fp2_trace_jni = (v = getenv("FP2_JNILOG")) && *v != '0';
    fp2_trace_gl  = (v = getenv("FP2_GLLOG")) && *v != '0';
    if ((v = getenv("FP2_FRAMES")))
        fp2_max_frames = strtol(v, NULL, 10);
#endif
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
        copy_path(fp2_gamedir, sizeof fp2_gamedir, arg, "game directory");
    else if (!getcwd(fp2_gamedir, sizeof fp2_gamedir))
        copy_path(fp2_gamedir, sizeof fp2_gamedir, ".", "game directory");
    join_path(fp2_datadir, sizeof fp2_datadir, fp2_gamedir, "assets", NULL);
    join_path(fp2_apk, sizeof fp2_apk, fp2_gamedir, "assets", NULL);
    join_path(fp2_home, sizeof fp2_home, fp2_gamedir, "home", NULL);
    mkdir(fp2_home, 0755);
}

int fp2_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !fp2_capture_mode)
            continue;
        join_path(path, sizeof path, fp2_gamedir, "lib", LIBS[i].file);
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
        if (LIBS[i].capture_only && !fp2_capture_mode)
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

/* FP2_SAMPLE=<ms>: a CPU-time profiling tick that prints where the thread that
 * is burning the CPU actually is.  A spin inside guest code blocks nothing, so
 * /proc says only "running" and an external signal is swallowed once the guest
 * installs its own handlers -- sampling from inside is the only view left. */

/* Unity builds without frame pointers, so an x29 walk yields stale stack junk
 * past the first frame or two (it fooled us once already).  Scan the raw stack
 * instead and keep only words that are VERIFIED return addresses: the value
 * must land inside a module we mapped, and the instruction right before it
 * must actually be a bl/blr.  That turns "some pointer shaped like code" into
 * "somebody really called here". */
#ifndef FP2_RELEASE_BUILD
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
        fprintf(stderr, "[fp2/stack]    %s  (via %s)\n", where,
                is_bl ? "bl" : "blr");
        found++;
    }
}

/* Available to any shim that needs to name who called it: the verified-return
 * address scan above is the only reliable way, because Unity ships without
 * frame pointers and an x29 walk yields stale stack junk. */
void fp2_dump_guest_callers(const char *tag, int max)
{
    unsigned long sp = (unsigned long)__builtin_frame_address(0);
    fprintf(stderr, "[fp2/who] %s callers:\n", tag ? tag : "?");
    scan_stack_for_callers(sp, max);
    fflush(stderr);
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
    fprintf(stderr, "[fp2/sample] pc=%s  lr=%s\n", a, b);
    scan_stack_for_callers((unsigned long)u->uc_mcontext.sp, 12);
    fflush(stderr);
}

static void fp2_arm_sampler(void)
{
    const char *v = getenv("FP2_SAMPLE");
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
    fprintf(stderr, "[fp2] sampler armed at %d ms of CPU time\n", ms);
}
#endif

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;

    /* A self-sent signal (si_code <= 0) is not a CPU fault: something in the
     * process deliberately queued it.  Guest runtimes do that to probe their
     * own handler, so swallowing it with _exit turns a survivable probe into a
     * dead boot.  Let a bounded number through and see whether the boot
     * continues; FP2_SELFSIG=0 restores the old fatal behaviour. */
    if (si && si->si_code <= 0 && si->si_pid == getpid()) {
        static volatile int passed;
#ifdef FP2_RELEASE_BUILD
        int resume_self_signal = 1;
#else
        const char *off = getenv("FP2_SELFSIG");
        int resume_self_signal = !off || strcmp(off, "0") != 0;
#endif
        if (resume_self_signal && passed < 16) {
            passed++;
            fprintf(stderr, "[fp2] self-sent signal %d (#%d) resumed, not fatal\n",
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
    fprintf(stderr, "\n[fp2] signal %d at pc=%#lx si_code=%d (%s)\n", sig, pc,
            code, code <= 0 ? "SELF-SENT / queued" : "CPU fault");
    if (code <= 0)
        fprintf(stderr, "[fp2]   raised by pid=%d uid=%d  (si_addr is NOT a "
                        "fault address here)\n",
                si ? (int)si->si_pid : -1, si ? (int)si->si_uid : -1);
    else
        fprintf(stderr, "[fp2]   fault addr=%p\n", si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[fp2]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[fp2]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[fp2]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[fp2]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[fp2]   lr=%016lx sp=%016lx probe_slot=%u\n",
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
            fprintf(stderr, "[fp2]   #%-2d %-3s %#018lx  %s\n", i,
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
            fprintf(stderr, "[fp2]   #%-2d %-3s %#018lx  %s\n", i, "", lr, where);
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
                    fprintf(stderr, "[fp2]   map %s%s%s\n", line,
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
    fp2_input_request_exit();
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
#ifndef FP2_RELEASE_BUILD
    struct sigaction sample;
    memset(&sample, 0, sizeof sample);
    sample.sa_sigaction = on_sample;
    sample.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sample.sa_mask);
    sigaction(SIGUSR2, &sample, NULL);
#endif

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
    void *fn = fp2_jni_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", name);
    if (!fn)
        fn = fp2_jni_native("com/unity3d/player/UnityPlayer", name);
    if (!fn)
        fn = fp2_jni_native(NULL, name);
    return fn;
}

/* Unity 6 registers initJni as (Landroid/content/Context;I)V, Unity 2021 as
 * (Landroid/content/Context;)V.  Reading the registered signature is the only
 * honest way to know which shape this build wants: guessing wrong leaves the
 * context-type register holding junk. */
static const char *unity_native_sig(const char *name)
{
    const char *sig = fp2_jni_native_sig(
        "com/unity3d/player/UnityPlayerForActivityOrService", name);
    if (!sig)
        sig = fp2_jni_native_sig("com/unity3d/player/UnityPlayer", name);
    if (!sig)
        sig = fp2_jni_native_sig(NULL, name);
    return sig;
}


/* The reference port used build-specific libunity offsets to force its GLES3
 * device. Freedom Planet 2 ships a different Unity build, so touching either
 * address would be memory corruption. The logical GLES3 facade is the only
 * renderer path allowed until a target-specific decision point is proven from
 * this exact libunity hash and guarded by opcode validation. */
static void fp2_force_gfx_device(void)
{
#ifndef FP2_RELEASE_BUILD
    if (getenv("FP2_FORCE_GFX"))
        fprintf(stderr, "[fp2/gfx] FP2_FORCE_GFX rejected: no verified offset "
                        "exists for this target build\n");
#endif
    fprintf(stderr, "[fp2/gfx] logical GLES3 facade; no binary patch applied\n");
}

static void *exit_deadline_thread(void *unused)
{
    (void)unused;
    usleep(1500000); /* 1.5 seconds deadline */
    fprintf(stderr, "[fp2] exit deadline expired; forcing process exit\n");
    _exit(0);
    return NULL;
}

static int fp2_video_fatal;

static void run_unity(void)
{
    void *env = fp2_jni_env();
    void *player = fp2_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = fp2_jni_activity();
    void *surface = fp2_jret_obj("android/view/Surface");
    void *fn;

    fp2_jni_set_unity_player(player);

    fp2_diag_phase("unity-gfx-selection");
    fp2_force_gfx_device();

    fp2_diag_phase("unity-init-jni");
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
    {
        const char *sig = unity_native_sig("initJni");
        int wants_context_type = sig && strstr(sig, ";I)") != NULL;
        fprintf(stderr, "[fp2] initJni sig=%s contextType=%s\n",
                sig ? sig : "(unknown)", wants_context_type ? "yes" : "no");
        if (wants_context_type)
            ((void (*)(void *, void *, void *, int))fn)(env, player, activity, 0);
        else
            ((void (*)(void *, void *, void *))fn)(env, player, activity);
    }
    fprintf(stderr, "[fp2] initJni OK\n");

    fp2_diag_phase("unity-create-gfx");
    fn = unity_native("nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[fp2] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[fp2] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[fp2] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[fp2] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = unity_native("nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[fp2] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = unity_native("nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[fp2] nativeFocusChanged(true) OK\n");
    }
    fn = unity_native("nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[fp2] nativeResume OK\n");
    }

    fp2_diag_phase("unity-resume-complete");
    fp2_audio_start(env);

    void *render = unity_native("nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[fp2] nativeRender loop%s\n",
            fp2_max_frames > 0 ? " (test frame limit active)" : "");

    if (fp2_input_init() != 0)
        nx_die("FP2 public release requires a connected controller");
    fp2_diag_render_ready();

    unsigned long frame = 0;
#ifdef FP2_RELEASE_BUILD
    const long frame_budget_us = 16667;
#else
    const char *frame_us_env = getenv("FP2_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
#endif
    struct timespec frame_start;
#ifndef FP2_RELEASE_BUILD
    int report_fps = getenv("FP2_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
#endif
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        fp2_input_poll(env, player, frame);
        {
#ifndef FP2_RELEASE_BUILD
            extern void fp2_il2_probe(unsigned long frame);
#endif
            extern void fp2_video_bridge_tick(unsigned long frame);
#ifndef FP2_RELEASE_BUILD
            fp2_il2_probe(frame);
#endif
            fp2_video_bridge_tick(frame);
        }
        /* Android Tasks deliver listeners through the main Looper.  The JNI
         * bridge queues those callbacks and drains them here, on Unity's
         * render/main thread, before the next native frame. */
        fp2_jni_pump_callbacks();
        if (fp2_input_exit_requested()) {
            fprintf(stderr, "[fp2] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        /* BLACK/DEAD-CONTEXT conclusivo antes do present (nxgl 0.3.4) é
         * terminal: nenhum present a mais, nenhum health, status não zero.
         * O adapter já revogou o receipt de saúde ao consumir o fatal. */
        if (nxgl_frame_proof_is_fatal()) {
            (void)nxgl_frame_proof_consume_fatal();
            fprintf(stderr, "[fp2] VIDEO FATAL: frame proof conclusive black/"
                            "dead-context at frame %lu; closing with status "
                            "72\n", frame);
            fp2_video_fatal = 1;
            break;
        }
        fp2_diag_frame_complete(frame);
#ifndef FP2_RELEASE_BUILD
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[fp2] frame %lu keep=%u\n", frame, keep);
        if (report_fps && frame % 300 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - fps_mark.tv_sec) +
                        (now.tv_nsec - fps_mark.tv_nsec) / 1e9;
            if (dt > 0)
                fprintf(stderr, "[fp2/fps] %.1f fps (300 frames in %.2fs)\n",
                        300.0 / dt, dt);
            fps_mark = now;
        }
#endif
        if (!keep) {
            fprintf(stderr, "[fp2] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (fp2_max_frames > 0 && frame >= (unsigned long)fp2_max_frames) {
            fprintf(stderr, "[fp2] test frame limit reached (%lu)\n", frame);
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

    /* O censo de textura sai AQUI: a thread de deadline abaixo pode chamar
     * _exit(0) antes do fim do desmonte e levar o relatorio junto. */
#ifndef FP2_RELEASE_BUILD
    {
        extern void fp2_texture_census_report(void);
        fp2_texture_census_report();
    }
#endif

    /* Arm exit deadline thread to prevent hanging guest threads from blocking ES return */
    pthread_t d;
    if (pthread_create(&d, NULL, exit_deadline_thread, NULL) == 0)
        pthread_detach(d);

    /* Reproduce the explicit-finishing path from this APK.  UnityMain first
     * stops rendering, focus is lost, and pauseUnity selects shutdown() ->
     * nativeDone() directly when Activity.isFinishing() is true. */
    fn = unity_native("nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[fp2] nativeFocusChanged(false) OK\n");
    }
    /* Publish the last measured framebuffer verdict before Unity tears down
     * the current context.  The launcher remains the sole authority that
     * terminates a conclusive BLACK/DEAD-CONTEXT run. */
    nxgl_frame_proof_publish();

    fn = unity_native("nativeDone");
    if (fn) {
        int process_kill_requested =
            ((uint8_t (*)(void *, void *))fn)(env, player) != 0;
        fprintf(stderr, "[fp2] nativeDone OK (process-kill=%d)\n",
                process_kill_requested);
    }

    /* nativeDone posts the final UnityChoreographer STOP/QUIT messages.  Join
     * the emulated HandlerThread only after nativeDone has consumed that
     * native lifecycle, matching HandlerThread/Looper teardown on Android. */
    fp2_jni_finish_callbacks();

    fp2_input_close();
    fp2_audio_stop();
    fp2_diag_sync();

    fprintf(stderr, "[fp2] lifecycle complete; clean exit\n");
    /* Falha terminal do runtime vivo de controles nunca vira status 0. */
    _exit(fp2_video_fatal ? 72 : fp2_input_fatal() ? 70 : 0);
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
                "[fp2] outra instancia do Freedom Planet 2 ja esta rodando; saindo\n");
        _exit(1);
    }
    /* Intencionalmente sem close(): a trava vale enquanto o processo viver. */
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    fp2_diag_open_persistent_log();
    claim_single_instance();

    /* Capture the run-bound receipt before GL or Unity can fail.  Resolution
     * remains lazy: fp2_gl_raw_sym routes through the exact SDL/raw provider
     * selected later by the normal game lifecycle. */
    nxgl_frame_proof_set_resolver(fp2_gl_raw_sym);
    nxgl_frame_proof_launch_receipt();
    fp2_graphics_contract_prepare();

    /* EmulationStation's application wrapper exports C.UTF-8.  This Android
     * Unity player was built against Bionic's locale ABI; when its native
     * startup crosses the host glibc C.UTF-8 locale, a small-string object is
     * overwritten and its stack canary fires before frame one.  Android's
     * invariant/POSIX locale is the matching behaviour for this port. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    if (fp2_apply_declared_contract() != 0) {
        fprintf(stderr,
                "[fp2/contract] cannot enforce the runtime environment\n");
        return 1;
    }
    read_env();
#ifndef FP2_RELEASE_BUILD
    if (getenv("FP2_TLSLOG")) { extern void fp2_tls_trace_enable(void); fp2_tls_trace_enable(); }
    fp2_arm_sampler();
#endif
    extern void fp2_android_prepare_main_looper(void);
    fp2_android_prepare_main_looper();
    install_fault_handler();
#ifndef FP2_RELEASE_BUILD
    stack_top_hint = (unsigned long)__builtin_frame_address(0);
#endif
    setup_paths(argc > 1 ? argv[1] : NULL);
    fp2_diag_watchdog_start();
    fp2_diag_phase("bootstrap-paths-ready");

    fprintf(stderr, "[fp2] Freedom Planet 2 compatibility loader -- gamedir %s\n",
            fp2_gamedir);

    /* Fronteira pré-init do nxinput 0.10.0: o mapa de controles do dono e o
     * FACE_LAYOUT são lidos exatamente uma vez, antes de qualquer subsistema
     * SDL (o vídeo inicia em fp2_egl_init). */
    if (fp2_input_preinit() != 0)
        nx_die("controls pre-init failed closed");

    fp2_diag_phase("host-shims");
    fp2_jni_init();
#ifndef FP2_RELEASE_BUILD
    if (getenv("FP2_NETFLIX_SELFTEST"))
        return fp2_jni_netflix_selftest();
#endif
    fp2_egl_init();
    build_imports();

    fp2_diag_phase("module-map-relocate");
    int missing = fp2_load_modules();
    fprintf(stderr, "[fp2] modules loaded, %d relocations unresolved\n", missing);

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("required Unity module disappeared after relocation");

    fp2_hide_mobile_controls();

    /* System.load(libmain.so): its constructors run before JNI_OnLoad. */
    fp2_diag_phase("libmain-init-array");
    nx_run_init(main_mod);
    typedef int (*onload)(void *vm, void *reserved);
    onload main_onload = (onload)nx_lookup_in(main_mod, "JNI_OnLoad");
    if (!main_onload)
        nx_die("libmain.so has no JNI_OnLoad");
    fp2_diag_phase("libmain-jni-onload");
    int main_version = main_onload(fp2_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[fp2] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        fp2_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, fp2_gamedir, "lib", NULL);
    void *loader_class =
        fp2_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = fp2_jret_str(libdir);
    fp2_diag_phase("native-loader-unity-il2cpp");
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        fp2_jni_env(), loader_class, loader_path);
    /* MEDIDO: o libmain deste APK so conhece "libunity.so"; o libil2cpp nao e'
     * carregado por NativeLoader.load e sim mais tarde, pelo proprio libunity.
     * Exigir il2->inited aqui reprovaria um boot correto. */
    if (!loaded || !uni->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d)",
               loaded, uni->inited);

    fprintf(stderr,
            "[fp2] NativeLoader.load completed: libunity inited=%d il2cpp inited=%d\n",
            uni->inited, il2->inited);

    /* Nada de JNI_OnLoad de lib de audio aqui: neste build o FMOD e' interno
     * ao libunity.so e ja foi inicializado pelo JNI_OnLoad dele.  A ponte com
     * org/fmod/FMODAudioDevice vive no falso-JNI + audio.c. */
    fp2_diag_phase("unity-lifecycle");
    run_unity();
    return 0;
}
