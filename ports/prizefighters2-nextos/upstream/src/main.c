/*
 * main.c -- native Prizefighters 2 bootstrap for NextOS.
 *
 * There is no Android application or emulator in this path.  We load the
 * original arm64 Unity objects, restore their version-pinned protected ranges,
 * run their real init arrays/JNI_OnLoad, then drive Unity's native surface and
 * render lifecycle directly.
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
#include "pf2.h"

char pf2_gamedir[1024];
char pf2_datadir[1024];
char pf2_apk[1024];
char pf2_home[1024];
long pf2_max_frames = 0;
int pf2_trace_gl = 0;
int pf2_capture_mode = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* The production path loads only the three game objects.  libpairipcore is
 * admitted solely for PF2_VM_CAPTURE, a one-shot arm64 extraction on NextOS;
 * it is neither required nor loaded during normal gameplay. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libpairipcore.so", "libpairipcore.so", 1, 1 },
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    /* PairIP's startup program enumerates the already mapped objects.  Keep
     * the optional Burst image in the analysis process because it was present
     * in every known-good interpreter trace; production still omits it. */
    { "lib_burst_generated.so", "lib_burst_generated.so", 0, 1 },
};

extern const nx_import *pf2_pthread_table(size_t *n);
extern const nx_import *pf2_android_table(size_t *n);
extern const nx_import *pf2_egl_table(size_t *n);

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
    const nx_import *p = pf2_pthread_table(&np);
    const nx_import *an = pf2_android_table(&na);
    const nx_import *eg = pf2_egl_table(&ne);

    size_t bn;
    extern nx_import *pf2_bionic_entries(size_t *n);
    nx_import *be = pf2_bionic_entries(&bn);
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
    /* DT_INIT is not executed for the protected objects, but keeping this
     * relocation valid makes the mapped ELF internally complete. */
    all[all_n++] = (nx_import){
        "ExecuteProgram", pf2_protected_sym("ExecuteProgram")
    };
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

void pf2_watchdog_frame(void) { watchdog_frame++; }

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
                "[pf2] watchdog: frame %lu has not returned in %ds; faulting "
                "the render thread so its stack is reported\n",
                last, watchdog_seconds);
        /* Deliver to the render thread specifically, not to the process: any
         * other thread would report a stack we already know is idle. */
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void pf2_arm_frame_watchdog(void)
{
    const char *v = getenv("PF2_WATCHDOG");
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

int pf2_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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
const char *pf2_mod_at(const void *addr, void **base_out)
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
    nx_verbose   = (v = getenv("PF2_VERBOSE")) && *v != '0';
    pf2_log_level = (v = getenv("PF2_LOGCAT")) && *v != '0';
    pf2_trace_jni = (v = getenv("PF2_JNILOG")) && *v != '0';
    pf2_trace_gl  = (v = getenv("PF2_GLLOG")) && *v != '0';
    pf2_trace_plt = (v = getenv("PF2_PLTLOG")) && *v != '0';
    pf2_capture_mode = (v = getenv("PF2_VM_CAPTURE")) && *v != '0';
    if ((v = getenv("PF2_FRAMES")))
        pf2_max_frames = strtol(v, NULL, 10);
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
        copy_path(pf2_gamedir, sizeof pf2_gamedir, arg, "game directory");
    else if (!getcwd(pf2_gamedir, sizeof pf2_gamedir))
        copy_path(pf2_gamedir, sizeof pf2_gamedir, ".", "game directory");
    join_path(pf2_datadir, sizeof pf2_datadir, pf2_gamedir, "assets", NULL);
    join_path(pf2_apk, sizeof pf2_apk, pf2_gamedir, "assets", NULL);
    join_path(pf2_home, sizeof pf2_home, pf2_gamedir, "home", NULL);
    mkdir(pf2_home, 0755);
}

int pf2_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !pf2_capture_mode)
            continue;
        join_path(path, sizeof path, pf2_gamedir, "lib", LIBS[i].file);
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
        if (LIBS[i].capture_only && !pf2_capture_mode)
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
    fprintf(stderr, "\n[pf2] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[pf2]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[pf2]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[pf2]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[pf2]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[pf2]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
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
}

static void run_unity(void)
{
    void *env = pf2_jni_env();
    void *player = pf2_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = pf2_jret_obj("android/app/Activity");
    void *surface = pf2_jret_obj("android/view/Surface");
    void *fn;

    pf2_jni_set_unity_player(player);

    fn = pf2_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[pf2] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[pf2] initJni OK\n");

    fn = pf2_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[pf2] nativeRecreateGfxState...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[pf2] nativeRecreateGfxState OK\n");

    fn = pf2_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[pf2] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = pf2_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[pf2] nativeResume OK\n");
    }
    fn = pf2_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[pf2] nativeFocusChanged(true) OK\n");
    }

    pf2_audio_start(env);

    void *render = pf2_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[pf2] nativeRender loop%s\n",
            pf2_max_frames > 0 ? " (test frame limit active)" : "");

    pf2_input_init();

    /* Watchdog for a hung frame.  Unity installs its own crash handler, which
     * prints a symbolised backtrace of whichever thread faults -- so the way to
     * see where a stuck frame is stuck is to fault that exact thread on purpose.
     * Off unless PF2_WATCHDOG names a timeout in seconds. */
    pf2_arm_frame_watchdog();

    unsigned long frame = 0;
    for (;;) {
        pf2_watchdog_frame();
        pf2_input_poll(env, player, frame);
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[pf2] frame %lu keep=%u\n", frame, keep);
        if (!keep) {
            fprintf(stderr, "[pf2] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (pf2_max_frames > 0 && frame >= (unsigned long)pf2_max_frames) {
            fprintf(stderr, "[pf2] test frame limit reached (%lu)\n", frame);
            fflush(stderr);
            _exit(0);
        }
        usleep(1000);
    }
    pf2_input_close();
    pf2_audio_stop();
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* EmulationStation's application wrapper exports C.UTF-8.  This Android
     * Unity player was built against Bionic's locale ABI; when its native
     * startup crosses the host glibc C.UTF-8 locale, a small-string object is
     * overwritten and its stack canary fires before frame one.  Android's
     * invariant/POSIX locale is the matching behaviour for this port. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);

    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    fprintf(stderr, "[pf2] Prizefighters 2 for NextOS -- gamedir %s\n", pf2_gamedir);

    pf2_jni_init();
    pf2_egl_init();
    build_imports();

    int missing = pf2_load_modules();
    fprintf(stderr, "[pf2] modules loaded, %d relocations unresolved\n", missing);

    if (pf2_capture_mode) {
        fprintf(stderr,
                "[pf2] native arm64 capture mode; no Android runtime is involved\n");
        int rc = pf2_pairip_capture();
        fprintf(stderr, "[pf2] native capture %s\n", rc == 0 ? "complete" : "failed");
        return rc == 0 ? 0 : 3;
    }

    pf2_apply_text_overlays();
    pf2_neutralise_encrypted_data();
    int protected_missing = pf2_patch_protected_plt();
    if (protected_missing)
        nx_die("%d protected PLT slots have no native implementation",
               protected_missing);

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    nx_mod *uni = nx_find_mod("libunity.so");

    /* DT_INIT in the two protected objects only re-enters PairIP.  Their real
     * C++ constructors are in DT_INIT_ARRAY and still run in full. */
    if (il2)
        il2->init_func = NULL;
    if (uni)
        uni->init_func = NULL;
    if (main_mod) nx_run_init(main_mod);
    if (il2)      nx_run_init(il2);
    if (uni)      nx_run_init(uni);

    /* JNI_OnLoad order used by Unity's NativeLoader. */
    typedef int (*onload)(void *vm, void *reserved);
    const char *order[] = { "libmain.so", "libil2cpp.so", "libunity.so" };
    extern void *pf2_jni_vm(void);
    for (size_t i = 0; i < 3; i++) {
        nx_mod *m = nx_find_mod(order[i]);
        if (!m)
            continue;
        onload f = (onload)nx_lookup_in(m, "JNI_OnLoad");
        if (!f) {
            fprintf(stderr, "[pf2] %s has no JNI_OnLoad\n", order[i]);
            continue;
        }
        int v = f(pf2_jni_vm(), NULL);
        fprintf(stderr, "[pf2] JNI_OnLoad(%s) -> %#x\n", order[i], v);
    }

    fprintf(stderr, "[pf2] bootstrap reached the end of JNI_OnLoad.\n");
    run_unity();
    return 0;
}
