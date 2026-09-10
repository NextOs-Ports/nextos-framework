/*
 * main.c -- native Sally Face bootstrap for NextOS.
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
#include <sys/mman.h>

#include "egl_sdl.h"
#include "nxgl_frame_proof_adapter.h"
#include "nx_elf.h"
#include "quality.h"
#include "sf.h"

char sf_gamedir[1024];
char sf_datadir[1024];
char sf_apk[1024];
char sf_apkdir[1024];
char sf_home[1024];
long sf_max_frames = 0;
int sf_trace_gl = 0;
int sf_capture_mode = 0;

/* Telemetria do heap gerenciado.  O port do Vita usa um controlador de GC
 * baseado exatamente nestes dois numeros (heap usado e heap reservado).  Por
 * enquanto isto e' deliberadamente somente leitura: primeiro medimos o
 * comportamento real do Android/IL2CPP no aparelho, depois comparamos uma
 * politica nova contra o mesmo caminho de jogo. */
typedef size_t (*sf_gc_size_fn)(void);
typedef int (*sf_gc_int_fn)(void);
typedef unsigned long long (*sf_gc_u64_fn)(void);

static sf_gc_size_fn sf_gc_used_size;
static sf_gc_size_fn sf_gc_heap_size;
static sf_gc_int_fn sf_gc_is_incremental;
static sf_gc_int_fn sf_gc_is_disabled;
static sf_gc_u64_fn sf_gc_max_time_slice_ns;
static int sf_gc_probe_state;

static void sf_gc_probe(unsigned long frame)
{
    if (frame != 1 && frame % 300 != 0)
        return;

    if (sf_gc_probe_state == 0) {
        nx_mod *il2 = nx_find_mod("libil2cpp.so");
        if (il2) {
            sf_gc_used_size =
                (sf_gc_size_fn)nx_lookup_in(il2, "il2cpp_gc_get_used_size");
            sf_gc_heap_size =
                (sf_gc_size_fn)nx_lookup_in(il2, "il2cpp_gc_get_heap_size");
            sf_gc_is_incremental =
                (sf_gc_int_fn)nx_lookup_in(il2, "il2cpp_gc_is_incremental");
            sf_gc_is_disabled =
                (sf_gc_int_fn)nx_lookup_in(il2, "il2cpp_gc_is_disabled");
            sf_gc_max_time_slice_ns =
                (sf_gc_u64_fn)nx_lookup_in(
                    il2, "il2cpp_gc_get_max_time_slice_ns");
        }
        sf_gc_probe_state = sf_gc_used_size && sf_gc_heap_size ? 1 : -1;
        if (sf_gc_probe_state < 0) {
            fprintf(stderr,
                    "[sf/gc] exports de telemetria indisponiveis\n");
            return;
        }
    }
    if (sf_gc_probe_state < 0)
        return;

    size_t used = sf_gc_used_size();
    size_t heap = sf_gc_heap_size();
    int incremental = sf_gc_is_incremental ? sf_gc_is_incremental() : -1;
    int disabled = sf_gc_is_disabled ? sf_gc_is_disabled() : -1;
    unsigned long long slice_ns =
        sf_gc_max_time_slice_ns ? sf_gc_max_time_slice_ns() : 0;
    fprintf(stderr,
            "[sf/gc] frame=%lu used=%zuMB heap=%zuMB incremental=%d "
            "disabled=%d slice=%.3fms env_disable=%s\n",
            frame, used / (1024 * 1024), heap / (1024 * 1024),
            incremental, disabled, slice_ns / 1000000.0,
            getenv("GC_DISABLE_INCREMENTAL") ? "sim" : "nao");
}

/* Teste opt-in do mesmo aviso que Android envia quando onTrimMemory chega ao
 * UnityPlayer.  A chamada precisa acontecer na UnityMain, entre dois renders;
 * por isso o arquivo apenas solicita o evento e esta funcao o entrega aqui.
 * Nada e' automatico nem fica ativo numa execucao normal. */
static void sf_lowmem_test(void *env, void *player, unsigned long frame)
{
    static int enabled = -1;
    static void *native_low_memory;
    if (enabled < 0) {
        const char *value = getenv("SF_LOWMEM_TEST");
        enabled = value && strcmp(value, "0") != 0;
    }
    if (!enabled || access("/tmp/bclowmem", F_OK) != 0)
        return;
    unlink("/tmp/bclowmem");

    if (!native_low_memory)
        native_low_memory =
            sf_jni_native("com/unity3d/player/UnityPlayer",
                          "nativeLowMemory");
    if (!native_low_memory) {
        fprintf(stderr,
                "[sf/lowmem] nativeLowMemory indisponivel no frame %lu\n",
                frame);
        return;
    }
    fprintf(stderr, "[sf/lowmem] aviso Android no frame %lu\n", frame);
    ((void (*)(void *, void *))native_low_memory)(env, player);
    fprintf(stderr, "[sf/lowmem] nativeLowMemory retornou\n");
}

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  This must remain
 * the executable's only initialized TLS object: glibc places its TLS block
 * immediately after the 16-byte TCB, so the stable pad covers the complete
 * Bionic guard slot on every thread.  Mutable per-thread state stays in .tbss,
 * and build.sh rejects the ELF if this guard stops being TLS offset zero. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* The supported Sally Face family is Unity 2022.3 IL2CPP, arm64-v8a only.
 * Only these three objects belong to the game; the container also carries a
 * repack layer (libstub.so / libhook.so) that the port deliberately never
 * loads -- one only exports JNI_OnLoad to check the APK signature, the other
 * installs inline code hooks.  Keep the exact NativeLoader order and do not
 * introduce a synthetic bootstrap. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
};

extern const nx_import *sf_pthread_table(size_t *n);
extern const nx_import *sf_android_table(size_t *n);
extern const nx_import *sf_egl_table(size_t *n);

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
    const nx_import *p = sf_pthread_table(&np);
    const nx_import *an = sf_android_table(&na);
    const nx_import *eg = sf_egl_table(&ne);

    size_t bn;
    extern nx_import *sf_bionic_entries(size_t *n);
    nx_import *be = sf_bionic_entries(&bn);
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

void sf_watchdog_frame(void) { watchdog_frame++; }

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
                "[sf] watchdog: frame %lu has not returned in %ds; faulting "
                "the render thread so its stack is reported\n",
                last, watchdog_seconds);
        /* Deliver to the render thread specifically, not to the process: any
         * other thread would report a stack we already know is idle. */
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void sf_arm_frame_watchdog(void)
{
    const char *v = getenv("SF_WATCHDOG");
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

int sf_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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
const char *sf_mod_at(const void *addr, void **base_out)
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
    nx_verbose   = (v = getenv("SF_VERBOSE")) && *v != '0';
    sf_log_level = (v = getenv("SF_LOGCAT")) && *v != '0';
    sf_trace_jni = (v = getenv("SF_JNILOG")) && *v != '0';
    sf_trace_gl  = (v = getenv("SF_GLLOG")) && *v != '0';
    if ((v = getenv("SF_FRAMES")))
        sf_max_frames = strtol(v, NULL, 10);
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
        copy_path(sf_gamedir, sizeof sf_gamedir, arg, "game directory");
    else if (!getcwd(sf_gamedir, sizeof sf_gamedir))
        copy_path(sf_gamedir, sizeof sf_gamedir, ".", "game directory");
    /* O conteudo do asset pack ja vem DESMONTADO no layout solto que a Unity
     * procura sozinha (assets/bin/Data/resources.assets, level1..N,
     * sharedassetsN.assets), entao a raiz do pacote e' simplesmente a pasta do
     * port -- nao ha Play Core para emular. */
    snprintf(sf_apkdir, sizeof sf_apkdir, "%s", sf_gamedir);
    join_path(sf_datadir, sizeof sf_datadir, sf_gamedir, "assets", NULL);
    join_path(sf_apk, sizeof sf_apk, sf_gamedir, "assets", NULL);
    join_path(sf_home, sizeof sf_home, sf_gamedir, "home", NULL);
    mkdir(sf_home, 0755);
    /* A Unity abre a base por caminho RELATIVO ("assets/bin/Data/..."), entao
     * o diretorio corrente TEM que ser a raiz do pacote.  Sem isto ela nao le
     * o unity_app_guid nem o globalgamemanagers e morre em seguida com o
     * PlayerSettings nulo -- e o launcher nao pode ser a unica garantia. */
    if (chdir(sf_apkdir) != 0)
        nx_die("nao consegui entrar em %s", sf_apkdir);
}

int sf_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !sf_capture_mode)
            continue;
        join_path(path, sizeof path, sf_gamedir, "lib", LIBS[i].file);
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
        if (LIBS[i].capture_only && !sf_capture_mode)
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
    fprintf(stderr, "\n[sf] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[sf]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[sf]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[sf]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[sf]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[sf]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    sf_input_request_exit();
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

typedef struct {
    void *player;
    void *surface;
} sf_unity_thread_args;

static int unity_input_ready;
static int unity_thread_done;

/* UnityPlayer creates U0/"UnityMain" after initJni.  Surface, focus, resume,
 * nativeRender and pause are all dispatched by that thread's Handler; Android
 * UI/input callbacks stay on the Activity thread.  Keeping those two roles on
 * one Linux thread happened to boot, but it is not the APK's lifecycle. */
static void *unity_main_thread(void *opaque)
{
    sf_unity_thread_args *args = opaque;
    void *env = sf_jni_env();
    void *player = args->player;
    void *surface = args->surface;
    void *fn;

    pthread_setname_np(pthread_self(), "UnityMain");
    fprintf(stderr, "[sf] UnityMain started (tid=%ld)\n",
            (long)syscall(SYS_gettid));

    fn = sf_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[sf] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[sf] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[sf] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[sf] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = sf_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[sf] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = sf_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[sf] nativeFocusChanged(true) OK\n");
    }
    fn = sf_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[sf] nativeResume OK\n");
    }

    sf_audio_start(env);

    void *render = sf_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[sf] nativeRender loop%s\n",
            sf_max_frames > 0 ? " (test frame limit active)" : "");

    sf_mem_guard_start();

    /* Watchdog for a hung frame.  Unity installs its own crash handler, which
     * prints a symbolised backtrace of whichever thread faults -- so the way to
     * see where a stuck frame is stuck is to fault that exact thread on purpose.
     * Off unless SF_WATCHDOG names a timeout in seconds. */
    sf_arm_frame_watchdog();

    unsigned long frame = 0;
    const char *frame_us_env = getenv("SF_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
    struct timespec frame_start;
    int report_fps = getenv("SF_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
    __atomic_store_n(&unity_input_ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        sf_watchdog_frame();
        if (sf_input_exit_requested()) {
            fprintf(stderr, "[sf] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        sf_quality_tick(frame);
        sf_lowmem_test(env, player, frame);
        sf_gc_probe(frame);
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[sf] frame %lu keep=%u\n", frame, keep);
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
            fprintf(stderr, "[sf] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (sf_max_frames > 0 && frame >= (unsigned long)sf_max_frames) {
            fprintf(stderr, "[sf] test frame limit reached (%lu)\n", frame);
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

    __atomic_store_n(&unity_input_ready, 0, __ATOMIC_RELEASE);
    fn = sf_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[sf] nativeFocusChanged(false) OK\n");
    }
    fn = sf_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[sf] nativePause OK\n");
    }
    sf_audio_stop();
    __atomic_store_n(&unity_thread_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void run_unity(void)
{
    void *env = sf_jni_env();
    void *player = sf_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = sf_jni_activity();
    void *surface = sf_jret_obj("android/view/Surface");
    void *fn;

    sf_jni_set_unity_player(player);

    /* UnityPlayer's constructor invokes initJni on the Android UI thread,
     * before U0/UnityMain is started. */
    fn = sf_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[sf] initJni on UI thread (tid=%ld)...\n",
            (long)syscall(SYS_gettid));
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[sf] initJni OK\n");

    /* A physical Android device already exists when UnityMain performs its
     * startup scan.  Discover the NextOS controller before starting that
     * thread, then forward events from this UI/main thread while it renders. */
    sf_input_init();

    sf_unity_thread_args args = {
        .player = player,
        .surface = surface,
    };
    __atomic_store_n(&unity_input_ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&unity_thread_done, 0, __ATOMIC_RELAXED);
    pthread_t unity_thread;
    int rc = pthread_create(&unity_thread, NULL, unity_main_thread, &args);
    if (rc != 0)
        nx_die("cannot start UnityMain: %s", strerror(rc));

    unsigned long input_tick = 0;
    const char *input_us_env = getenv("SF_INPUT_US");
    long input_budget_us = input_us_env && *input_us_env
                         ? strtol(input_us_env, NULL, 10) : 8333;
    if (input_budget_us < 1000)
        input_budget_us = 1000;
    while (!__atomic_load_n(&unity_thread_done, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&unity_input_ready, __ATOMIC_ACQUIRE))
            sf_input_poll(env, player, input_tick++);
        usleep((useconds_t)input_budget_us);
    }
    pthread_join(unity_thread, NULL);
    sf_input_close();
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
                "[sf] outra instancia do Sally Face ja esta rodando; saindo\n");
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
    /* A/B control for the Unity/IL2CPP collector.  Keep the proven Android
     * baseline as the default until the same route has been measured with
     * incremental GC on the Mali device.  The override is consumed before
     * libil2cpp is loaded because the collector reads this environment flag
     * during its own startup. */
    const char *gc_incremental = getenv("SF_GC_INCREMENTAL");
    if (gc_incremental && strcmp(gc_incremental, "0") != 0)
        unsetenv("GC_DISABLE_INCREMENTAL");
    else
        setenv("GC_DISABLE_INCREMENTAL", "1", 1);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    fprintf(stderr, "[sf] Sally Face for NextOS -- gamedir %s\n", sf_gamedir);

    /* Resolve before EGL/Unity sees the texture policy environment. */
    sf_quality_init(sf_gamedir);
    sf_jni_init();
    sf_egl_init();
    build_imports();

    int missing = sf_load_modules();
    fprintf(stderr, "[sf] modules loaded, %d relocations unresolved\n", missing);

    /* Sally Face uses the FMOD engine EMBEDDED in libunity.so (the
     * fmod_output_opensl / fmod_output_audiotrack backends), not a standalone
     * libfmod.so -- there is no FMOD Studio object in the container, so there
     * is nothing extra to load here. */

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
    int main_version = main_onload(sf_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[sf] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        sf_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, sf_gamedir, "lib", NULL);
    void *loader_class =
        sf_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = sf_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        sf_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited || !il2->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d il2cpp_init=%d)",
               loaded, uni->inited, il2->inited);

    fprintf(stderr,
            "[sf] NativeLoader.load completed: libunity -> libil2cpp\n");
    run_unity();
    nxgl_frame_proof_publish();
    /* Saida do port: no Mali-450 (fbdev/Utgard) o teardown do SDL/EGL depois do
     * desmonte da Unity trava o driver, entao nada de atexit toca em GL.  Mas no
     * caminho SDL-owned (KMSDRM, ex.: dArkOS/Mali-G31) e' o OPOSTO: sem o
     * SDL_QuitSubSystem(VIDEO) o CRTC nao e' restaurado, o framebuffer do jogo
     * fica congelado e o frontend nao volta.  sf_sdl_video_shutdown() faz a
     * coisa certa em cada caso (no-op no raw/fbdev). */
    sf_sdl_video_shutdown();
    fflush(NULL);
    _exit(0);
}
