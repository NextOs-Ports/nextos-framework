/*
 * main.c -- bootstrap nativo do Suzy Cube (Unity 2017.4.40f1 IL2CPP arm64)
 * para o NextOS.
 *
 * Nao ha Android nem emulador neste caminho.  Carregamos os objetos arm64
 * originais, rodamos os init arrays / JNI_OnLoad de verdade e entao dirigimos
 * o ciclo de vida de surface e render do proprio Unity, na ordem nativa.
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
#include <sys/syscall.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/file.h>
#include <fcntl.h>

#include "nx_elf.h"
#include "sc.h"
#include "framework_bridge.h"
#include "nxgl_frame_proof_adapter.h"

char sc_gamedir[1024];
char sc_datadir[1024];
char sc_apk[1024];
char sc_home[1024];
long sc_max_frames = 0;
int sc_trace_gl = 0;
int sc_capture_mode = 0;

/* O codigo arm64 do Android le o stack guard direto de TPIDR_EL0+0x28.  Sob
 * glibc esse endereco pode pertencer ao TLS mutavel de outro modulo, e um
 * quadro perfeitamente valido do Unity chama __stack_chk_fail.  Manter este
 * objeto como o PRIMEIRO TLS inicializado na ordem de link: a glibc poe o
 * primeiro bloco TLS do executavel logo apos o TCB de 16 bytes, entao este pad
 * estavel cobre o slot inteiro do guard bionic em toda thread. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Suzy Cube 1.0.13: Unity 2017.4.40f1 IL2CPP, arm64, SEM protecao (metadata em
 * claro, sem PairIP/packer).  Ordem exata do NativeLoader; nada de bootstrap
 * sintetico. */
static const struct {
    const char *file, *soname;
    int required;
} LIBS[] = {
    { "libmain.so",   "libmain.so",   1 },
    { "libunity.so",  "libunity.so",  1 },
    { "libil2cpp.so", "libil2cpp.so", 1 },
};

extern const nx_import *sc_pthread_table(size_t *n);
extern const nx_import *sc_android_table(size_t *n);
extern const nx_import *sc_egl_table(size_t *n);

static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne;
    const nx_import *p = sc_pthread_table(&np);
    const nx_import *an = sc_android_table(&na);
    const nx_import *eg = sc_egl_table(&ne);

    size_t bn;
    extern nx_import *sc_bionic_entries(size_t *n);
    nx_import *be = sc_bionic_entries(&bn);
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

/* ------------------------------------------------------------- frame watchdog */

static volatile unsigned long watchdog_frame;
static pid_t watchdog_tid;
static int watchdog_seconds;

void sc_watchdog_frame(void) { watchdog_frame++; }

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
                "[sc] watchdog: o quadro %lu nao voltou em %ds; faltando a "
                "thread de render de proposito para ver a pilha dela\n",
                last, watchdog_seconds);
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void sc_arm_frame_watchdog(void)
{
    const char *v = getenv("SC_WATCHDOG");
    if (!v || !*v)
        return;
    watchdog_seconds = atoi(v);
    if (watchdog_seconds <= 0)
        return;
    watchdog_tid = (pid_t)syscall(SYS_gettid);
    pthread_t th;
    if (pthread_create(&th, NULL, watchdog_thread, NULL) != 0) {
        nx_log("watchdog: nao consegui criar a thread");
        return;
    }
    pthread_detach(th);
    nx_log("watchdog armado: %ds sem quadro faltam o tid %d",
           watchdog_seconds, (int)watchdog_tid);
}

int sc_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
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

const char *sc_mod_at(const void *addr, void **base_out)
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
    nx_verbose   = (v = getenv("SC_VERBOSE")) && *v != '0';
    sc_log_level = (v = getenv("SC_LOGCAT")) && *v != '0';
    sc_trace_jni = (v = getenv("SC_JNILOG")) && *v != '0';
    sc_trace_gl  = (v = getenv("SC_GLLOG")) && *v != '0';
    if ((v = getenv("SC_FRAMES")))
        sc_max_frames = strtol(v, NULL, 10);
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
        copy_path(sc_gamedir, sizeof sc_gamedir, arg, "game directory");
    else if (!getcwd(sc_gamedir, sizeof sc_gamedir))
        copy_path(sc_gamedir, sizeof sc_gamedir, ".", "game directory");
    join_path(sc_datadir, sizeof sc_datadir, sc_gamedir, "assets", NULL);
    join_path(sc_apk, sizeof sc_apk, sc_gamedir, "assets", NULL);
    join_path(sc_home, sizeof sc_home, sc_gamedir, "home", NULL);
    mkdir(sc_home, 0755);
}

int sc_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        join_path(path, sizeof path, sc_gamedir, "lib", LIBS[i].file);
        nx_mod *m = nx_load(path, LIBS[i].soname);
        if (!m) {
            if (LIBS[i].required)
                nx_die("cannot load %s (expected at %s)", LIBS[i].file, path);
            nx_log("optional %s missing", LIBS[i].file);
        }
    }
    int missing = 0;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m)
            missing += nx_relocate(m);
    }
    return missing;
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    fprintf(stderr, "\n[sc] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[sc]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[sc]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[sc]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[sc]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[sc]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    sc_input_request_exit();
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

    /* SIGTERM/SIGINT seguem o caminho do SELECT+START (pause/save/saida),
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
    void *env = sc_jni_env();
    void *player = sc_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = sc_jni_activity();
    void *surface = sc_jret_obj("android/view/Surface");
    void *fn;

    sc_jni_set_unity_player(player);

    fn = sc_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity nao registrou initJni");
    fprintf(stderr, "[sc] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[sc] initJni OK\n");

    fn = sc_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity nao registrou nativeRecreateGfxState");
    fprintf(stderr, "[sc] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[sc] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* O callback SurfaceHolder do UnityPlayer repete updateGLDisplay para o
     * surfaceChanged inicial antes de encaminhar a mudanca de tamanho.
     * Preservamos essa ordem mesmo com a mesma Surface nativa do fbdev. */
    fprintf(stderr, "[sc] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[sc] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = sc_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeRestartActivityIndicator");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[sc] nativeRestartActivityIndicator OK\n");
    }

    fn = sc_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[sc] nativeFocusChanged(true) OK\n");
    }
    fn = sc_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[sc] nativeResume OK\n");
    }

    sc_audio_start(env);

    void *render = sc_jni_native("com/unity3d/player/UnityPlayer",
                                 "nativeRender");
    if (!render)
        nx_die("Unity nao registrou nativeRender");
    fprintf(stderr, "[sc] loop de nativeRender%s\n",
            sc_max_frames > 0 ? " (limite de quadros de teste ativo)" : "");

    sc_input_init();
    sc_arm_frame_watchdog();

    /* Video e entrada ja publicaram recibo aqui.  O audio ainda NAO: o FMOD
     * abre a saida na thread dele, depois que os natives aparecem -- por isso
     * este ponto e' um relatorio de progresso, e quem fecha o contrato de
     * verdade e' a reavaliacao em audio.c quando o ultimo recibo chega. */
    if (sc_framework_ready() != 0)
        fprintf(stderr,
                "[sc/framework] contrato pendente no 1o quadro (audio abre depois)\n");

    unsigned long frame = 0;
    const char *frame_us_env = getenv("SC_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
    struct timespec frame_start;
    int report_fps = getenv("SC_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        sc_watchdog_frame();
        sc_input_poll(env, player, frame);
        if (sc_input_exit_requested()) {
            fprintf(stderr, "[sc] saida pedida pelo controle/sinal\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[sc] frame %lu keep=%u\n", frame, keep);
        if (report_fps && frame % 300 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - fps_mark.tv_sec) +
                        (now.tv_nsec - fps_mark.tv_nsec) / 1e9;
            if (dt > 0)
                fprintf(stderr, "[sc/fps] %.1f fps (300 quadros em %.2fs)\n",
                        300.0 / dt, dt);
            fps_mark = now;
        }
        if (!keep) {
            fprintf(stderr, "[sc] Unity pediu parar o loop no quadro %lu\n",
                    frame);
            break;
        }
        if (sc_max_frames > 0 && frame >= (unsigned long)sc_max_frames) {
            fprintf(stderr, "[sc] limite de quadros de teste atingido (%lu)\n",
                    frame);
            break;
        }
        /* Pacing pelo tempo QUE SOBRA do orcamento do quadro, nunca um sleep
         * fixo somado ao trabalho. */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long spent_us = (now.tv_sec - frame_start.tv_sec) * 1000000L +
                            (now.tv_nsec - frame_start.tv_nsec) / 1000L;
            if (frame_budget_us > 0 && spent_us < frame_budget_us)
                usleep((useconds_t)(frame_budget_us - spent_us));
        }
    }

    fn = sc_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[sc] nativeFocusChanged(false) OK\n");
    }
    fn = sc_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[sc] nativePause OK\n");
    }
    sc_input_close();
    sc_audio_stop();
    fflush(stderr);
    /* Licao do Chrono Trigger: o SIGSEGV do Suzy Cube, se aparecer, sai depois
     * do jogo terminar, no desmonte do GL do Unity.  Save ja' foi feito no
     * nativePause; sair aqui e' correto e devolve status 0 ao frontend. */
    _exit(0);
}

/* UM JOGO SO: a trava vai no BINARIO, nunca so' no script do launcher. */
static void claim_single_instance(void)
{
    static int lock_fd = -1;
    lock_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (lock_fd < 0)
        return;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr,
                "[sc] outra instancia do Suzy Cube ja esta rodando; saindo\n");
        _exit(1);
    }
    /* Intencionalmente sem close(): a trava vale enquanto o processo viver. */
}

int main(int argc, char **argv)
{

    /* Emitted before anything can fail: a run that dies during startup is
     * exactly the run whose launch context a reader needs. */
    nxgl_frame_proof_launch_receipt();
    setvbuf(stderr, NULL, _IOLBF, 0);
    claim_single_instance();

    /* O wrapper do EmulationStation exporta C.UTF-8.  Este player Unity Android
     * foi compilado contra a ABI de locale do Bionic; ao cruzar o C.UTF-8 da
     * glibc do host, um objeto de string curta e' sobrescrito e o canario
     * dispara antes do primeiro quadro.  O locale invariante do Android e' o
     * comportamento equivalente aqui. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
    setenv("GC_DISABLE_INCREMENTAL", "1", 0);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    fprintf(stderr, "[sc] Suzy Cube para NextOS -- gamedir %s\n", sc_gamedir);

    /* O preflight do nxcompat descreve o host e planeja o ambiente.  Ele
     * observa e relata; nao decide se o jogo abre.  Um host que ainda nao
     * publicou tudo continua valendo -- quem fecha a conta e' sc_framework_ready
     * depois que video, audio e entrada tiverem recibo. */
    if (sc_framework_preflight(sc_gamedir) != 0)
        fprintf(stderr, "[sc/framework] preflight incompleto (segue mesmo assim)\n");

    sc_jni_init();
    sc_egl_init();
    build_imports();

    int missing = sc_load_modules();
    fprintf(stderr, "[sc] modulos carregados, %d relocacoes sem resolver\n",
            missing);

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("modulo Unity obrigatorio sumiu depois da relocacao");

    /* System.load(libmain.so): os construtores dele rodam antes do JNI_OnLoad. */
    nx_run_init(main_mod);
    typedef int (*onload)(void *vm, void *reserved);
    onload main_onload = (onload)nx_lookup_in(main_mod, "JNI_OnLoad");
    if (!main_onload)
        nx_die("libmain.so nao tem JNI_OnLoad");
    int main_version = main_onload(sc_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) falhou: %#x", main_version);
    fprintf(stderr, "[sc] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative chama o metodo nativo exato registrado pela
     * libmain.  Esse metodo faz dlopen da libunity primeiro e da libil2cpp
     * depois; a nossa ponte de dlopen roda o init array real de cada uma
     * imediatamente antes do JNI_OnLoad dela. */
    void *native_load =
        sc_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain nao registrou NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, sc_gamedir, "lib", NULL);
    void *loader_class = sc_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = sc_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        sc_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited)
        nx_die("NativeLoader.load falhou (result=%d unity_init=%d)",
               loaded, uni->inited);

    /* Na 2017.4 o libmain so' carrega o libunity; quem faz dlopen do libil2cpp
     * e' o proprio libunity, ja' dentro do initJni.  Por isso il2->inited ainda
     * e' 0 aqui e checar isso agora seria uma falha inventada. */
    fprintf(stderr, "[sc] NativeLoader.load concluido: libunity (il2cpp vem no initJni)\n");
    run_unity();
    return 0;
}
