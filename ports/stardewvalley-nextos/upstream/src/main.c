/*
 * main.c -- Stardew Valley (Android, Mono/.NET AOT) so-loader para NextOS Mali-450.
 *
 * Cadeia de modulos Bionic (libc do Android) carregada pelo ELF loader custom
 * (so_util, linhagem max_arm64). Cada modulo eh mmap'd num heap RWX, relocado,
 * e seus imports resolvidos contra a tabela de shims + dlsym(RTLD_DEFAULT).
 *
 *   M1: libmonosgen-2.0.so   (runtime Mono)            -> snapshot mono_*
 *   M2: libxamarin-app.so    (ponte JNI Xamarin)        -> snapshot xamarin_*
 *   M3: libmonodroid.so      (Xamarin.Android runtime)  precisa mono_* + xamarin
 *
 * Depois constroi a fake JavaVM/JNIEnv (offsets Bionic) e chama
 * libmonodroid::JNI_OnLoad(vm) -> OSBridge::initialize_on_onLoad.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"
#include "util.h"
#include "jni_shim.h"
#include "sdv_egl_bridge.h"

#define MONO_SO    "libmonosgen-2.0.so"
#define XAMARIN_SO "libxamarin-app.so"
#define DROID_SO   "libmonodroid.so"

#define MONO_HEAP_MB    96
#define XAMARIN_HEAP_MB 32
#define DROID_HEAP_MB   64

/* TLS pad -- mesmo fix do Bully/GTALCS p/ stack-guard bionico (tpidr+0x28) */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

/* tabelas fornecidas pelos shims */
extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

static volatile uintptr_t g_last_base = 0;
static const char *g_last_name = "?";
static uintptr_t g_mono_base = 0;   /* text_base do libmonosgen (p/ stack scan) */
static volatile sig_atomic_t g_exit_requested;

static void exit_request_handler(int signal_number) {
    (void)signal_number;
    g_exit_requested = 1;
}

/* Tabela combinada (mono+xamarin+shims) persistente p/ resolver imports dos
 * .so Bionic carregados dinamicamente via sdv_so_dlopen (componentes mono,
 * libaot-*). Setada apos a cadeia principal em main(). */
DynLibFunction *g_resolv_tbl = NULL;
int g_resolv_n = 0;

/* ---- dlopen/dlsym via nosso so-loader p/ .so Bionic ----
 * Componentes mono (libmono-component-marshal-ilgen.so) e libaot-*.dll.so sao
 * ELF Bionic — glibc dlopen rejeita ("invalid ELF header"). Carregamos via
 * so_util (mesmo mecanismo da cadeia principal) e expomos dlsym via snapshot. */
#define SDV_HANDLE_MAGIC 0x53445648u
struct sdv_dlhandle {
    uint32_t magic;
    DynLibFunction *snap;
    int n;
    char *name;
    struct sdv_dlhandle *next;
};

static pthread_mutex_t g_so_loader_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_so_handles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sdv_dlhandle *g_so_handles;
static _Thread_local int g_so_loader_active;

void *sdv_so_dlopen(const char *name);
void *sdv_so_dlsym(void *handle, const char *name);
int sdv_so_is_handle(void *handle);
int sdv_so_dlclose(void *handle);

void *sdv_so_dlopen(const char *name) {
    if (!name || !g_resolv_tbl) return NULL;
    /* Imagens AOT (libaot-*.dll.so) tem PLT/IRELATIVE que nosso so-loader nao
     * trata — carregar causa lazy-PLT crash no ld-linux. Recusar aqui forca o
     * mono a JIT-ar a assembly (mais lento, mas evita o caminho AOT). */
    if (strstr(name, "libaot-")) {
        fprintf(stderr, "[so_dlopen] recusando AOT '%s' (forca JIT)\n", name);
        return NULL;
    }
    /* so_load usa fopen no caminho exato e nao pesquisa LD_LIBRARY_PATH.
     * O runtime primeiro sonda nomes curtos e depois tenta o path completo;
     * rejeitar o miss antes do mmap evita reservar/desfazer 48 MiB em cada
     * uma dessas dezenas de sondagens normais do boot. */
    if (access(name, R_OK) != 0)
        return NULL;
    if (g_so_loader_active) {
        fprintf(stderr,
                "[so_dlopen] carga reentrante recusada enquanto abre %s\n",
                name);
        return NULL;
    }
    /* so_util mantem a imagem atual em globais. Componentes gerenciados podem
     * pedir dlopen em workers diferentes, portanto uma carga deve terminar
     * completamente antes da seguinte iniciar. */
    pthread_mutex_lock(&g_so_loader_lock);
    g_so_loader_active = 1;
    size_t hs = (size_t)48 * 1024 * 1024;   /* 48MB heap por componente */
    void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) {
        fprintf(stderr, "[so_dlopen] mmap falhou p/ %s\n", name);
        g_so_loader_active = 0;
        pthread_mutex_unlock(&g_so_loader_lock);
        return NULL;
    }
    fprintf(stderr, "[so_dlopen] carregando %s (heap %p)\n", name, heap);
    if (so_load(name, heap, hs) < 0) {
        fprintf(stderr, "[so_dlopen] so_load(%s) falhou\n", name);
        munmap(heap, hs);
        g_so_loader_active = 0;
        pthread_mutex_unlock(&g_so_loader_lock);
        return NULL;
    }
    if (so_relocate() < 0) {
        fprintf(stderr, "[so_dlopen] so_relocate(%s) falhou\n", name);
        munmap(heap, hs);
        g_so_loader_active = 0;
        pthread_mutex_unlock(&g_so_loader_lock);
        return NULL;
    }
    so_resolve(g_resolv_tbl, g_resolv_n, 0);
    so_finalize();
    so_flush_caches();
    so_execute_init_array();
    int sn = 0;
    DynLibFunction *snap = so_snapshot_symbols(&sn);
    so_free_temp();
    struct sdv_dlhandle *h = malloc(sizeof(*h));
    if (!h) {
        fprintf(stderr, "[so_dlopen] handle sem memoria para %s\n", name);
        g_so_loader_active = 0;
        pthread_mutex_unlock(&g_so_loader_lock);
        return NULL;
    }
    h->magic = SDV_HANDLE_MAGIC;
    h->snap = snap;
    h->n = sn;
    h->name = strdup(name);
    pthread_mutex_lock(&g_so_handles_lock);
    h->next = g_so_handles;
    g_so_handles = h;
    pthread_mutex_unlock(&g_so_handles_lock);
    fprintf(stderr, "[so_dlopen] %s OK: %d simbolos exportados\n", name, sn);
    g_so_loader_active = 0;
    pthread_mutex_unlock(&g_so_loader_lock);
    return h;
}
void *sdv_so_dlsym(void *handle, const char *name) {
    struct sdv_dlhandle *h = (struct sdv_dlhandle *)handle;
    if (!name || !sdv_so_is_handle(handle) ||
        h->magic != SDV_HANDLE_MAGIC)
        return NULL;
    for (int i = 0; i < h->n; i++)
        if (h->snap[i].symbol && strcmp(h->snap[i].symbol, name) == 0)
            return (void *)h->snap[i].func;
    return NULL;
}
int sdv_so_is_handle(void *handle) {
    int found = 0;

    pthread_mutex_lock(&g_so_handles_lock);
    for (struct sdv_dlhandle *h = g_so_handles; h; h = h->next) {
        if (handle == h) {
            found = h->magic == SDV_HANDLE_MAGIC;
            break;
        }
    }
    pthread_mutex_unlock(&g_so_handles_lock);
    return found;
}
int sdv_so_dlclose(void *handle) {
    /* As relocacoes e snapshots podem continuar referenciados pelo Mono.
     * Reconhecemos o handle e mantemos a imagem ate o fim do processo. */
    return sdv_so_is_handle(handle) ? 0 : -1;
}

/* ---- crash handler ---- */
static void print_addr(const char *label, uintptr_t addr) {
    fprintf(stderr, "  %s=%p", label, (void *)addr);
    Dl_info di;
    if (dladdr((void *)addr, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        b = b ? b + 1 : di.dli_fname;
        fprintf(stderr, " = %s+0x%lx", b, (unsigned long)(addr - (uintptr_t)di.dli_fbase));
    }
    fprintf(stderr, "\n");
}
/* Exposto p/ os shims de imports.gen.c: resolve endereco -> modulo do loader
 * (dladdr nao enxerga o que o nosso so-loader mapeia). */
void sdv_print_addr(const char *label, uintptr_t addr) { print_addr(label, addr); }

static void crash_handler(int sig, siginfo_t *info, void *uc) {
    uintptr_t fault = (uintptr_t)info->si_addr;
    /* si_code < 0 = sinal ENVIADO por alguem (SI_USER/SI_TKILL): nesse caso
     * si_addr nao e' endereco de falha, e' a uniao com si_pid/si_uid. */
    fprintf(stderr, "\n=== CRASH sig=%d code=%d addr=%p (mod=%s) ===\n",
            sig, info->si_code, info->si_addr, g_last_name);
    if (info->si_code <= 0)
        fprintf(stderr, "  (sinal RAISED pelo proprio processo: pid=%d uid=%d)\n",
                (int)info->si_pid, (int)info->si_uid);
#if defined(__aarch64__)
    ucontext_t *u = (ucontext_t *)uc;
    uintptr_t pc = u->uc_mcontext.pc, lr = u->uc_mcontext.regs[30];
    print_addr("PC", pc); print_addr("LR", lr);
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "  x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i,   (unsigned long)u->uc_mcontext.regs[i],
                i+1, (unsigned long)u->uc_mcontext.regs[i+1],
                i+2, (unsigned long)u->uc_mcontext.regs[i+2],
                i+3, (unsigned long)u->uc_mcontext.regs[i+3]);
    fprintf(stderr, "  x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "  sp=%lx fp=%lx\n",
            (unsigned long)u->uc_mcontext.sp, (unsigned long)u->uc_mcontext.regs[29]);
    uintptr_t tb = g_last_base;
    if (tb && fault >= tb && fault < tb + text_size)
        fprintf(stderr, "  fault em %s+0x%lx\n", g_last_name, (unsigned long)(fault - tb));
    if (tb && pc >= tb && pc < tb + text_size)
        fprintf(stderr, "  PC em %s+0x%lx\n", g_last_name, (unsigned long)(pc - tb));
    uintptr_t fp = u->uc_mcontext.regs[29];
    for (int f = 0; f < 16 && fp; f++) {
        uintptr_t *p = (uintptr_t *)fp;
        uintptr_t rlr = p[1]; if (!rlr) break;
        char lb[16]; snprintf(lb, sizeof lb, "#%-2d lr", f);
        print_addr(lb, rlr);
        uintptr_t nfp = p[0]; if (nfp <= fp) break; fp = nfp;
    }
    /* stack scan: procura return addresses nas faixas de texto do mono/droid
     * (o frame-walk falha sem frame pointers; isso acha os LR empilhados). */
    {
        uintptr_t sp = u->uc_mcontext.sp;
        fprintf(stderr, "  -- stack scan (ret addrs em mono/droid) --\n");
        for (int i = 0; i < 1024; i++) {
            uintptr_t v = ((uintptr_t *)sp)[i];
            if ((g_mono_base && v >= g_mono_base && v < g_mono_base + 0x320000) ||
                (g_last_base && v >= g_last_base && v < g_last_base + 0x60000)) {
                uintptr_t b = (v >= g_mono_base && v < g_mono_base + 0x320000) ? g_mono_base : g_last_base;
                const char *m = (b == g_mono_base) ? "monosgen" : "monodroid";
                fprintf(stderr, "    [sp+%#5x] %p = %s+0x%lx\n",
                        i * 8, (void *)v, m, (unsigned long)(v - b));
            }
        }
    }
#endif
    fflush(stderr); _exit(128 + sig);
}
static void install_crash_handler(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);  sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
}

static void preload_device_libs(void) {
    static const char *libs[] = {
        "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so", "libm.so.6",
        "libstdc++.so.6", "libz.so.1", NULL
    };
    for (int i = 0; libs[i]; i++) {
        void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        fprintf(stderr, "preload %s: %s\n", libs[i], h ? "OK" : dlerror());
    }
}

static DynLibFunction *make_base_table(int *out_n) {
    int n = dynlib_functions_count + revc_pthread_count;
    DynLibFunction *t = malloc(sizeof(DynLibFunction) * n);
    memcpy(t, dynlib_functions, sizeof(DynLibFunction) * dynlib_functions_count);
    memcpy(t + dynlib_functions_count, revc_pthread_table,
           sizeof(DynLibFunction) * revc_pthread_count);
    *out_n = n;
    return t;
}

static uintptr_t tbl_find(DynLibFunction *t, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (t[i].symbol && strcmp(t[i].symbol, name) == 0) return t[i].func;
    return 0;
}

void sdv_promote_current_mono_thread(void) {
    static _Thread_local int attempted;
    if (attempted || getenv("SDV_NO_THREAD_PROMOTE")) return;
    attempted = 1;

    typedef void *(*mono_thread_current_t)(void);
    typedef void *(*mono_object_get_class_t)(void *);
    typedef void *(*mono_class_get_method_from_name_t)(void *, const char *, int);
    typedef void *(*mono_runtime_invoke_t)(void *, void *, void **, void **);

    mono_thread_current_t thread_current = (mono_thread_current_t)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_thread_current");
    mono_object_get_class_t object_get_class = (mono_object_get_class_t)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_object_get_class");
    mono_class_get_method_from_name_t class_get_method =
        (mono_class_get_method_from_name_t)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_class_get_method_from_name");
    mono_runtime_invoke_t runtime_invoke = (mono_runtime_invoke_t)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_runtime_invoke");
    if (!thread_current || !object_get_class || !class_get_method ||
        !runtime_invoke) {
        fprintf(stderr, "[mono-thread] embedding API incompleta\n");
        return;
    }

    void *thread = thread_current();
    void *klass = thread ? object_get_class(thread) : NULL;
    void *setter = klass ? class_get_method(klass, "set_IsBackground", 1) : NULL;
    if (!setter) {
        fprintf(stderr, "[mono-thread] Thread.set_IsBackground nao encontrado\n");
        return;
    }

    int32_t background = 0;
    void *args[] = { &background };
    void *exception = NULL;
    runtime_invoke(setter, thread, args, &exception);
    fprintf(stderr, "[mono-thread] render worker promovida para foreground%s\n",
            exception ? " (com excecao gerenciada)" : "");
}

/* ---- Zoom em tempo real no D-pad (estilo Terraria) -------------------------
 * O zoom do Stardew mobile e' o gesto de pinca, que nao existe aqui (nosso
 * MotionEvent fake e' single-pointer). Em vez de sintetizar pinca, mexemos no
 * mesmo estado gerenciado que a pinca mexeria, via mono embedding (a thread
 * principal ja executa codigo managed, entao mono_runtime_invoke e' seguro):
 *
 *   PinchZoom.Instance.SetZoomLevel(z)   (estado da pinca coerente)
 *   Game1.options.desiredBaseZoomLevel=z (SO o desired!)
 *
 * O Game1._update ve desired != base e faz ELE MESMO: base = desired,
 * forceSnapOnNextViewportUpdate = true e refreshWindowSettings() — que
 * redimensiona viewport, render target e lightmap. Escrever baseZoomLevel
 * direto PULA esse refresh e o mundo fica renderizado num retangulo do
 * tamanho antigo (canto superior esquerdo + resto preto).
 * Limites: a pinca real permite ate' clientBounds/4096 (~0.16 em 640x480),
 * mas zoom muito aberto explode custo de draw no handheld; piso conservador. */
#define SDV_ZOOM_BASE_MIN   0.40f    /* piso ao ar livre (custo de draw) */
#define SDV_ZOOM_BASE_MAX   2.5f
#define SDV_ZOOM_PERIOD     0.05     /* 20 aplicacoes/s enquanto segurado */
#define SDV_ZOOM_STEP       1.038f   /* por aplicacao = ~2.1x/s, como antes */

struct sdv_zoom_ctx {
    int state;                      /* 0=nao inicializado, 1=ok, -1=falhou */
    void *(*image_loaded)(const char *);
    void *(*class_from_name)(void *, const char *, const char *);
    void *(*class_get_method)(void *, const char *, int);
    void *(*runtime_invoke)(void *, void *, void **, void **);
    void *(*object_unbox)(void *);
    void *get_options;              /* Game1.get_options (static) */
    void *get_desired;              /* Options.get_desiredBaseZoomLevel */
    void *set_desired;              /* Options.set_desiredBaseZoomLevel */
    void *pz_get_instance;          /* PinchZoom.get_Instance (static) */
    void *pz_set_zoom;              /* PinchZoom.SetZoomLevel */
    float level;                    /* zoom que NOS escrevemos por ultimo */
};

static struct sdv_zoom_ctx g_zoom;

static int sdv_zoom_init(void) {
    if (g_zoom.state) return g_zoom.state > 0;
    g_zoom.state = -1;

    g_zoom.image_loaded = (void *(*)(const char *))
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_image_loaded");
    g_zoom.class_from_name = (void *(*)(void *, const char *, const char *))
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_from_name");
    g_zoom.class_get_method = (void *(*)(void *, const char *, int))
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_get_method_from_name");
    g_zoom.runtime_invoke = (void *(*)(void *, void *, void **, void **))
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_runtime_invoke");
    g_zoom.object_unbox = (void *(*)(void *))
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_object_unbox");
    if (!g_zoom.image_loaded || !g_zoom.class_from_name ||
        !g_zoom.class_get_method || !g_zoom.runtime_invoke ||
        !g_zoom.object_unbox) {
        fprintf(stderr, "[sdv-zoom] embedding API incompleta\n");
        return 0;
    }

    void *image = g_zoom.image_loaded("StardewValley");
    if (!image) {
        /* assembly ainda nao carregada; tenta de novo no proximo aperto */
        g_zoom.state = 0;
        return 0;
    }
    void *game1 = g_zoom.class_from_name(image, "StardewValley", "Game1");
    void *options = g_zoom.class_from_name(image, "StardewValley", "Options");
    void *pinch = g_zoom.class_from_name(image, "StardewValley.Mobile",
                                         "PinchZoom");
    if (!game1 || !options || !pinch) {
        fprintf(stderr, "[sdv-zoom] classes nao encontradas (g=%p o=%p p=%p)\n",
                game1, options, pinch);
        return 0;
    }
    g_zoom.get_options = g_zoom.class_get_method(game1, "get_options", 0);
    g_zoom.get_desired =
        g_zoom.class_get_method(options, "get_desiredBaseZoomLevel", 0);
    g_zoom.set_desired =
        g_zoom.class_get_method(options, "set_desiredBaseZoomLevel", 1);
    g_zoom.pz_get_instance = g_zoom.class_get_method(pinch, "get_Instance", 0);
    g_zoom.pz_set_zoom = g_zoom.class_get_method(pinch, "SetZoomLevel", 1);
    if (!g_zoom.get_options || !g_zoom.get_desired || !g_zoom.set_desired ||
        !g_zoom.pz_get_instance || !g_zoom.pz_set_zoom) {
        fprintf(stderr, "[sdv-zoom] membros nao encontrados\n");
        return 0;
    }
    g_zoom.state = 1;
    fprintf(stderr, "[sdv-zoom] pronto (D-pad cima/baixo = zoom)\n");
    return 1;
}

/* Ressincroniza o nosso valor com o do jogo. So' no INICIO de cada aperto: o
 * getter e' um mono_runtime_invoke a mais, e o menu de opcoes/pinca pode ter
 * mexido no zoom enquanto o D-pad estava solto. */
static void sdv_zoom_begin(void) {
    if (!sdv_zoom_init()) return;

    void *exc = NULL;
    void *opts = g_zoom.runtime_invoke(g_zoom.get_options, NULL, NULL, &exc);
    if (exc || !opts) return;   /* jogo ainda no boot/titulo sem options */
    exc = NULL;
    void *boxed = g_zoom.runtime_invoke(g_zoom.get_desired, opts, NULL, &exc);
    if (exc || !boxed) return;
    float zoom = *(float *)g_zoom.object_unbox(boxed);
    g_zoom.level = zoom > 0.0f ? zoom : 1.0f;
}

/* direction: +1 aproxima, -1 afasta.
 *
 * O input loop roda na thread principal, mas o Game1.Update/Draw roda na render
 * worker. Cada mono_runtime_invoke daqui obriga o runtime a coordenar as duas
 * threads, entao chamar isso a 125 Hz (todo tick de 8 ms) e' o que deixava o
 * zoom instavel: a cada segundo segurando eram ~1100 invokes disputando com o
 * game loop. Agora aplicamos no maximo 20x/s, partindo do valor que NOS mesmos
 * escrevemos por ultimo (sem getter por aplicacao) — 2 invokes por aplicacao.
 *
 * Limites fixos, sem consultar o mapa da area atual: descobrir o piso real
 * exigia mais 5 invokes por tick (currentLocation, IsOutdoors, Map,
 * DisplayWidth, DisplayHeight) e era a maior fonte da instabilidade. */
static void sdv_zoom_step(int direction, double now) {
    static double last_apply;
    if (g_zoom.state <= 0 || !(g_zoom.level > 0.0f)) return;
    if (now - last_apply < SDV_ZOOM_PERIOD) return;
    last_apply = now;

    float zoom = g_zoom.level;
    zoom *= direction > 0 ? SDV_ZOOM_STEP : 1.0f / SDV_ZOOM_STEP;
    if (zoom > SDV_ZOOM_BASE_MAX) zoom = SDV_ZOOM_BASE_MAX;
    if (zoom < SDV_ZOOM_BASE_MIN) zoom = SDV_ZOOM_BASE_MIN;
    if (zoom == g_zoom.level) return;   /* ja' no batente: nao invoca nada */
    g_zoom.level = zoom;

    void *exc = NULL;
    void *opts = g_zoom.runtime_invoke(g_zoom.get_options, NULL, NULL, &exc);
    if (exc || !opts) return;

    void *args[1] = { &zoom };
    exc = NULL;
    void *pinch = g_zoom.runtime_invoke(g_zoom.pz_get_instance, NULL, NULL,
                                        &exc);
    if (!exc && pinch) {
        exc = NULL;
        g_zoom.runtime_invoke(g_zoom.pz_set_zoom, pinch, args, &exc);
    }
    exc = NULL;
    g_zoom.runtime_invoke(g_zoom.set_desired, opts, args, &exc);
}

typedef unsigned char (*sdv_key_callback_t)(void *, void *, int, void *);
typedef unsigned char (*sdv_touch_callback_t)(void *, void *, void *, void *);

static int sdv_input_trace(void) {
    static int initialized;
    static int enabled;

    if (!initialized) {
        const char *value = getenv("SDV_INPUT_TRACE");
        enabled = value && value[0] && value[0] != '0';
        initialized = 1;
    }
    return enabled;
}

static double sdv_monotonic_seconds(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void sdv_send_key(void *env, void *view, sdv_key_callback_t callback,
                         void *event, int keycode, const char *edge) {
    if (!callback || !view || !event) return;
    jni_set_key_event_keycode(event, keycode);
    unsigned char handled = callback(env, view, keycode, event);
    if (sdv_input_trace())
        fprintf(stderr, "[sdv-input] key %s android=%d handled=%u\n",
                edge, keycode, handled);
}

static void sdv_send_touch(void *env, void *view,
                           sdv_touch_callback_t callback, void *event,
                           int action, float x, float y) {
    if (!callback || !view || !event) return;
    jni_set_motion_event(event, action, x, y);
    unsigned char handled = callback(env, view, view, event);
    if (sdv_input_trace() && action != 2)
        fprintf(stderr,
                "[sdv-input] right-cursor touch action=%d x=%.1f y=%.1f handled=%u\n",
                action, x, y, handled);
}

static void sdv_run_input_loop(void *env, void *view, void *down_handler,
                               void *up_handler, void *touch_handler) {
    enum {
        SDV_KEY_UP = 1u << 0,
        SDV_KEY_DOWN = 1u << 1,
        SDV_KEY_LEFT = 1u << 2,
        SDV_KEY_RIGHT = 1u << 3,
        SDV_KEY_ACTION = 1u << 4,
        SDV_KEY_CANCEL = 1u << 5,
        SDV_KEY_TOOL = 1u << 6,
        SDV_KEY_MENU = 1u << 7,
        SDV_KEY_ESCAPE = 1u << 8,
        SDV_KEY_PREV = 1u << 9,
        SDV_KEY_NEXT = 1u << 10,
        SDV_KEY_JOURNAL = 1u << 11,
        SDV_KEY_MAP = 1u << 12,
        SDV_KEY_START = 1u << 13,
        SDV_KEY_L2 = 1u << 14,
        SDV_KEY_R2 = 1u << 15
    };
    static const struct {
        unsigned int bit;
        int keycode;
    } keys[] = {
        {SDV_KEY_UP, 19},       /* Android DPAD_UP */
        {SDV_KEY_DOWN, 20},     /* Android DPAD_DOWN */
        {SDV_KEY_LEFT, 21},     /* Android DPAD_LEFT */
        {SDV_KEY_RIGHT, 22},    /* Android DPAD_RIGHT */
        {SDV_KEY_ACTION, 96},   /* Android BUTTON_A */
        {SDV_KEY_CANCEL, 97},   /* Android BUTTON_B */
        {SDV_KEY_TOOL, 99},     /* Android BUTTON_X */
        {SDV_KEY_MENU, 100},    /* Android BUTTON_Y */
        {SDV_KEY_ESCAPE, 4},    /* Android BACK */
        {SDV_KEY_PREV, 102},    /* Android BUTTON_L1 */
        {SDV_KEY_NEXT, 103},    /* Android BUTTON_R1 */
        {SDV_KEY_JOURNAL, 106}, /* Android BUTTON_THUMBL */
        {SDV_KEY_MAP, 107},     /* Android BUTTON_THUMBR */
        {SDV_KEY_START, 108},   /* Android BUTTON_START */
        {SDV_KEY_L2, 104},      /* Android BUTTON_L2 */
        {SDV_KEY_R2, 105}       /* Android BUTTON_R2 */
    };
    sdv_key_callback_t key_down = (sdv_key_callback_t)down_handler;
    sdv_key_callback_t key_up = (sdv_key_callback_t)up_handler;
    sdv_touch_callback_t touch = (sdv_touch_callback_t)touch_handler;
    void *event = jni_make_object(jni_make_class("android.view.KeyEvent"));
    void *motion_event = touch
        ? jni_make_object(jni_make_class("android.view.MotionEvent")) : NULL;
    unsigned int previous = 0;
    unsigned long ticks = 0;
    int cursor_width = sdv_egl_width();
    int cursor_height = sdv_egl_height();
    if (cursor_width <= 0) cursor_width = 1280;
    if (cursor_height <= 0) cursor_height = 720;
    float cursor_x = (float)cursor_width * 0.5f;
    float cursor_y = (float)cursor_height * 0.5f;
    int cursor_visible = 0;
    int cursor_touch_down = 0;
    int r3_was_down = 0;
    int r3_mode = 0; /* 1=BUTTON_THUMBR nativo, 2=clique do cursor */
    double last_cursor_activity = 0.0;
    double last_tick = sdv_monotonic_seconds();
    const char *cursor_env = getenv("SDV_RIGHT_CURSOR");
    int right_cursor_enabled = touch &&
        !(cursor_env && cursor_env[0] == '0');
    /* D-pad = zoom (estilo Terraria); movimento fica no analogico esquerdo.
     * SDV_DPAD_ZOOM=0 restaura o D-pad como movimento (comportamento antigo). */
    const char *dpad_zoom_env = getenv("SDV_DPAD_ZOOM");
    int dpad_zoom_enabled = !(dpad_zoom_env && dpad_zoom_env[0] == '0');
    int zoom_prev_dir = 0;
    int test_key = getenv("SDV_TEST_KEY") ? atoi(getenv("SDV_TEST_KEY")) : 0;
    unsigned long test_at = getenv("SDV_TEST_KEY_DELAY")
        ? strtoul(getenv("SDV_TEST_KEY_DELAY"), NULL, 10) * 125ul : 0;

    sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
    fprintf(stderr,
            "[sdv-input] loop ativo; cursor direito=%s (R3=clique, timeout=2s)\n",
            right_cursor_enabled ? "ativo" : "inativo");
    signal(SIGTERM, exit_request_handler);
    signal(SIGINT, exit_request_handler);
    while (!g_exit_requested) {
        SdvGamepadState pad;
        unsigned int logical = 0;
        double now = sdv_monotonic_seconds();
        double elapsed = now - last_tick;
        int r3_down = 0;
        if (elapsed < 0.0 || elapsed > 0.05) elapsed = 0.05;
        last_tick = now;
        int pad_status = sdv_egl_poll_gamepad(&pad);
        if (pad_status < 0) {
            fprintf(stderr, "[sdv-input] SDL_QUIT\n");
            break;
        }
        if (pad_status > 0) {
            const int deadzone = 12000;
#define SDV_SDL_BUTTON(n) ((pad.buttons & (1u << (n))) != 0)
            r3_down = SDV_SDL_BUTTON(8);

            if (right_cursor_enabled) {
                const float stick_max = 32767.0f;
                const float cursor_deadzone = 7500.0f;
                float rx = (float)pad.right_x;
                float ry = (float)pad.right_y;
                float magnitude = sqrtf(rx * rx + ry * ry);

                if (magnitude > cursor_deadzone) {
                    float amount = (magnitude - cursor_deadzone) /
                        (stick_max - cursor_deadzone);
                    if (amount > 1.0f) amount = 1.0f;
                    float speed = 120.0f * amount +
                        1000.0f * amount * amount;
                    cursor_x += rx / magnitude * speed * (float)elapsed;
                    cursor_y += ry / magnitude * speed * (float)elapsed;
                    if (cursor_x < 0.0f) cursor_x = 0.0f;
                    if (cursor_y < 0.0f) cursor_y = 0.0f;
                    if (cursor_x > (float)(cursor_width - 1))
                        cursor_x = (float)(cursor_width - 1);
                    if (cursor_y > (float)(cursor_height - 1))
                        cursor_y = (float)(cursor_height - 1);
                    cursor_visible = 1;
                    last_cursor_activity = now;
                    sdv_egl_set_right_cursor(cursor_x, cursor_y, 1);
                    if (cursor_touch_down)
                        sdv_send_touch(env, view, touch, motion_event, 2,
                                       cursor_x, cursor_y);
                }
            }

            if (right_cursor_enabled && r3_down && !r3_was_down) {
                /* R3 e decidido na borda: com cursor visivel vira toque;
                 * sem cursor preserva BUTTON_THUMBR. A nunca e interceptado. */
                if (cursor_visible) {
                    r3_mode = 2;
                    cursor_touch_down = 1;
                    last_cursor_activity = now;
                    sdv_send_touch(env, view, touch, motion_event, 0,
                                   cursor_x, cursor_y);
                } else {
                    r3_mode = 1;
                }
            }
            if (right_cursor_enabled && !r3_down && r3_was_down) {
                if (r3_mode == 2) {
                    sdv_send_touch(env, view, touch, motion_event, 1,
                                   cursor_x, cursor_y);
                    cursor_touch_down = 0;
                    last_cursor_activity = now;
                }
                r3_mode = 0;
            }

            if (SDV_SDL_BUTTON(4) && SDV_SDL_BUTTON(6)) {
                fprintf(stderr, "[sdv-input] SELECT+START -> saindo do jogo\n");
                _exit(0);
            }

            if (pad.left_y < -deadzone) logical |= SDV_KEY_UP;
            if (pad.left_y > deadzone)  logical |= SDV_KEY_DOWN;
            if (pad.left_x < -deadzone) logical |= SDV_KEY_LEFT;
            if (pad.left_x > deadzone)  logical |= SDV_KEY_RIGHT;
            if (dpad_zoom_enabled) {
                int zoom_dir = 0;
                if (SDV_SDL_BUTTON(11)) zoom_dir += 1;   /* cima = aproxima */
                if (SDV_SDL_BUTTON(12)) zoom_dir -= 1;   /* baixo = afasta */
                if (zoom_dir && !zoom_prev_dir) {
                    fprintf(stderr, "[sdv-zoom] dpad %s\n",
                            zoom_dir > 0 ? "zoom-in" : "zoom-out");
                    sdv_zoom_begin();   /* le' o zoom atual uma unica vez */
                }
                if (zoom_dir) sdv_zoom_step(zoom_dir, now);
                zoom_prev_dir = zoom_dir;
                /* esquerda/direita do D-pad ficam reservados */
            } else {
                if (SDV_SDL_BUTTON(11)) logical |= SDV_KEY_UP;
                if (SDV_SDL_BUTTON(12)) logical |= SDV_KEY_DOWN;
                if (SDV_SDL_BUTTON(13)) logical |= SDV_KEY_LEFT;
                if (SDV_SDL_BUTTON(14)) logical |= SDV_KEY_RIGHT;
            }
            if (SDV_SDL_BUTTON(0)) logical |= SDV_KEY_ACTION;
            if (SDV_SDL_BUTTON(1)) logical |= SDV_KEY_CANCEL;
            if (SDV_SDL_BUTTON(2)) logical |= SDV_KEY_TOOL;
            if (SDV_SDL_BUTTON(3)) logical |= SDV_KEY_MENU;
            if (SDV_SDL_BUTTON(6)) logical |= SDV_KEY_START;
            if (SDV_SDL_BUTTON(4)) logical |= SDV_KEY_ESCAPE;
            if (SDV_SDL_BUTTON(9)) logical |= SDV_KEY_PREV;
            if (SDV_SDL_BUTTON(10)) logical |= SDV_KEY_NEXT;
            if (SDV_SDL_BUTTON(7)) logical |= SDV_KEY_JOURNAL;
            if ((!right_cursor_enabled && r3_down) || r3_mode == 1)
                logical |= SDV_KEY_MAP;
            if (pad.left_trigger > deadzone) logical |= SDV_KEY_L2;
            if (pad.right_trigger > deadzone) logical |= SDV_KEY_R2;
#undef SDV_SDL_BUTTON
        } else if (right_cursor_enabled && r3_was_down) {
            if (r3_mode == 2) {
                sdv_send_touch(env, view, touch, motion_event, 3,
                               cursor_x, cursor_y);
                cursor_touch_down = 0;
            }
            r3_mode = 0;
        }
        r3_was_down = r3_down;

        if (right_cursor_enabled && cursor_visible && !cursor_touch_down &&
            now - last_cursor_activity >= 2.0) {
            cursor_visible = 0;
            sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
        }
        unsigned int changed = logical ^ previous;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (!(changed & keys[i].bit)) continue;
            if (logical & keys[i].bit)
                sdv_send_key(env, view, key_down, event, keys[i].keycode, "down");
            else
                sdv_send_key(env, view, key_up, event, keys[i].keycode, "up");
        }
        previous = logical;

        if (test_key && test_at && ticks == test_at)
            sdv_send_key(env, view, key_down, event, test_key, "test-down");
        if (test_key && test_at && ticks == test_at + 25)
            sdv_send_key(env, view, key_up, event, test_key, "test-up");
        ++ticks;
        usleep(8000);
    }
    if (cursor_touch_down)
        sdv_send_touch(env, view, touch, motion_event, 3,
                       cursor_x, cursor_y);
    sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
    fprintf(stderr, "[sdv-input] encerramento solicitado\n");
}

/* Patchar 4 bytes no text de um modulo (mprotect RW, escreve, clear cache). */
static void patch4(uintptr_t base, uintptr_t off, uint32_t val) {
    uintptr_t addr = base + off;
    uintptr_t page = addr & ~0xffful;
    if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[patch] mprotect falhou @%p\n", (void *)addr); return;
    }
    *(uint32_t *)addr = val;
    __builtin___clear_cache((char *)addr, (char *)addr + 4);
    fprintf(stderr, "[patch] wrote 0x%08x @ %p (mod+0x%lx)\n", val, (void *)addr,
            (unsigned long)off);
}

static DynLibFunction *cat_table(DynLibFunction *a, int an, DynLibFunction *b, int bn, int *out_n) {
    int n = an + bn;
    DynLibFunction *t = malloc(sizeof(DynLibFunction) * n);
    memcpy(t, a, sizeof(DynLibFunction) * an);
    memcpy(t + an, b, sizeof(DynLibFunction) * bn);
    *out_n = n;
    return t;
}

/* Acha o .so do modulo. O so_load abre pelo caminho exato (nao pesquisa
 * LD_LIBRARY_PATH), e o APK guarda TODAS as libs juntas em lib/arm64-v8a/.
 * Procurando tambem em <base>/libs/, o extrator so' precisa descompactar o APK
 * tal como ele e' — sem copiar libmonosgen/libmonodroid/libxamarin-app pra
 * raiz, que era um passo manual facil de esquecer (e quebrava o boot com
 * "so_load: Failed to open file"). */
static const char *sdv_find_module(const char *name, char *buf, size_t bufsz) {
    const char *libdir = getenv("SDV_LIBDIR");

    if (access(name, R_OK) == 0) return name;
    snprintf(buf, bufsz, "%s/%s", sdv_base_dir(), name);
    if (access(buf, R_OK) == 0) return buf;
    snprintf(buf, bufsz, "%s/libs/%s", sdv_base_dir(), name);
    if (access(buf, R_OK) == 0) return buf;
    if (libdir && *libdir) {
        snprintf(buf, bufsz, "%s/%s", libdir, name);
        if (access(buf, R_OK) == 0) return buf;
    }
    return name;   /* deixa o so_load falhar e reportar o nome original */
}

/* Flags de comportamento que os dois launchers SEMPRE ligavam. Viraram o
 * padrao (o launcher fica curto, como o do Sonic/Bully) e o env passa a servir
 * so' pra DESLIGAR na bisseccao: SDV_NO_RUNTIME_INIT=1, SDV_NO_HOLD=1, etc. */
static int sdv_flag(const char *name, const char *off_name) {
    if (off_name && getenv(off_name)) return 0;
    (void)name;
    return 1;
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
    char path[4096];
    const char *file = sdv_find_module(name, path, sizeof path);
    g_last_name = name;
    size_t hs = (size_t)heap_mb * 1024 * 1024;
    void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) { fprintf(stderr, "mmap %s falhou\n", name); exit(1); }
    fprintf(stderr, "\n== carregando %s (heap %p, %dMB) ==\n", file, heap, heap_mb);
    if (so_load(file, heap, hs) < 0) { fprintf(stderr, "so_load(%s) falhou\n", file); exit(1); }
    if (so_relocate() < 0) { fprintf(stderr, "so_relocate(%s) falhou\n", name); exit(1); }
    so_resolve(tbl, n, 0);
    so_finalize();
    so_flush_caches();
    g_last_base = (uintptr_t)text_base;
    fprintf(stderr, "== %s OK: text=%p+0x%zx data=%p+0x%zx ==\n",
            name, text_base, text_size, data_base, data_size);
    so_execute_init_array();
    /* Relocacoes, init e tabelas agora apontam para a imagem mapeada. A copia
     * integral do ELF usada apenas pelo parser nao deve permanecer no RSS. */
    so_free_temp();
    fprintf(stderr, "== %s init_array OK ==\n", name);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    { volatile char c = g_bionic_guard_pad[0]; (void)c; }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    install_crash_handler();

    fprintf(stderr, "=== Stardew Valley Mono so-loader / NextOS aarch64 ===\n");
    preload_device_libs();

    /* A janela/contexto precisam nascer na main thread. O contexto e liberado
     * aqui e adquirido depois pela worker do MonoGame via o fake EGL10. */
    if (sdv_flag("SDV_START_ACTIVITY", "SDV_NO_START_ACTIVITY") && !sdv_egl_init())
        fprintf(stderr, "[sdv-egl] bootstrap grafico indisponivel\n");

    int base_n;
    DynLibFunction *base = make_base_table(&base_n);

    /* M1: runtime Mono */
    load_module(MONO_SO, MONO_HEAP_MB, base, base_n);
    g_mono_base = (uintptr_t)text_base;
    int mono_n = 0;
    DynLibFunction *mono_tbl = so_snapshot_symbols(&mono_n);
    fprintf(stderr, "monosgen: %d simbolos exportados\n", mono_n);

    /* PROBE: mono_pagesize() — base do block_size do allocator SGen/GC.
     * Se nao for pot de 2, explica o assertion lock-free-alloc.c:608. */
    {
        uintptr_t pp = tbl_find(mono_tbl, mono_n, "mono_pagesize");
        if (pp) {
            long (*ps)(void) = (long (*)(void))pp;
            long v = ps();
            fprintf(stderr, "[probe] mono_pagesize() = %ld (0x%lx) pot2=%d\n",
                    v, v, (v > 0 && (v & (v - 1)) == 0));
        } else {
            fprintf(stderr, "[probe] mono_pagesize nao encontrado no snapshot\n");
        }
    }

    /* M2: libxamarin-app */
    load_module(XAMARIN_SO, XAMARIN_HEAP_MB, base, base_n);
    int xam_n = 0;
    DynLibFunction *xam_tbl = so_snapshot_symbols(&xam_n);
    fprintf(stderr, "xamarin-app: %d simbolos exportados\n", xam_n);

    /* M3: libmonodroid (precisa mono_* + xamarin + shims) */
    int mx_n;
    DynLibFunction *mx = cat_table(mono_tbl, mono_n, xam_tbl, xam_n, &mx_n);
    int comb_n;
    DynLibFunction *comb = cat_table(base, base_n, mx, mx_n, &comb_n);
    g_resolv_tbl = comb;   /* p/ resolver imports dos .so Bionic via sdv_so_dlopen */
    g_resolv_n = comb_n;
    load_module(DROID_SO, DROID_HEAP_MB, comb, comb_n);
    g_last_base = (uintptr_t)text_base;
    g_last_name = DROID_SO;

    /* entry points do libmonodroid (modulo corrente = DROID_SO) */
    uintptr_t p_onload  = so_find_addr_safe("JNI_OnLoad");
    uintptr_t p_rtinit   = so_find_addr_safe("Java_mono_android_Runtime_init");
    uintptr_t p_rtinit_i = so_find_addr_safe("Java_mono_android_Runtime_initInternal");
    uintptr_t p_rtregister = so_find_addr_safe("Java_mono_android_Runtime_register");
    fprintf(stderr, "\nJNI_OnLoad            = %p\n", (void *)p_onload);
    fprintf(stderr, "Java_...Runtime_init   = %p\n", (void *)p_rtinit);
    fprintf(stderr, "Java_...initInternal   = %p\n", (void *)p_rtinit_i);
    fprintf(stderr, "Java_...Runtime_register = %p\n", (void *)p_rtregister);

    if (!p_onload) { fprintf(stderr, "JNI_OnLoad nao encontrado\n"); _exit(2); }

    /* fake JavaVM/JNIEnv e chama JNI_OnLoad */
    void *vm = jni_build_env();
    fprintf(stderr, "\n=== chamando JNI_OnLoad(vm=%p) ===\n", vm);
    int (*JNI_OnLoad)(void *, void *) = (int (*)(void *, void *))p_onload;
    int jniv = JNI_OnLoad(vm, NULL);
    fprintf(stderr, "=== JNI_OnLoad retornou 0x%x ===\n", jniv);

    if (jniv != 0) {
        fprintf(stderr, "JNI_OnLoad OK (version 0x%x). OSBridge inicializado.\n", jniv);
    }

    /* --- bypass: init Mono direto via C API (pula o Runtime_init do Xamarin
     * que crasha em maquinario TLS/sinal do glibc). mono_jit_init_version eh
     * o init de baixo nivel que o Runtime_init eventualmente chama. --- */
    if (getenv("SDV_MONO_JIT")) {
        /* Pula a assertion power-of-2 do lock-free-alloc (init_size_class @0x1e7954
         * b.ne -> NOP). block_size runtime vem corrompido pela ABI; veremos o
         * proximo muro apos este. */
        if (sdv_flag("SDV_PATCH_ASSERT", "SDV_NO_PATCH_ASSERT") && g_mono_base) {
            patch4(g_mono_base, 0x1e7954, 0xd503201f);  /* NOP */
        }
        uintptr_t p_jit = tbl_find(mono_tbl, mono_n, "mono_jit_init_version");
        fprintf(stderr, "\nmono_jit_init_version = %p\n", (void *)p_jit);
        if (p_jit) {
            void *(*jit)(const char *, const char *) = (void *(*)(const char *, const char *))p_jit;
            fprintf(stderr, "=== chamando mono_jit_init_version(\"StardewValley.Android\",\"v4.0.30319\") ===\n");
            void *domain = jit("StardewValley.Android", "v4.0.30319");
            fprintf(stderr, "=== domain = %p ===\n", domain);
            if (domain) fprintf(stderr, ">>> MONO RUNTION INIT COM SUCESSO <<<\n");
        }
    }

    /* --- milestone Runtime_init: tenta bootar o Mono de verdade --- */
    if (p_rtinit && sdv_flag("SDV_RUNTIME_INIT", "SDV_NO_RUNTIME_INIT")) {
        /* Patches de Runtime_init (quebra muros de directory/env):
         * 0x37300: cbnz->b sempre (pula GetStringUTFChars via wrapper.env=NULL
         *         no setup do dir XDG / set_environment_variable_for_directory).
         * 0x48778: mesmo padrao c_str() em determine_primary_override_dir — o
         *         UNICO c_str() sem cbz-env-graceful. Branch B (libFolders vazio,
         *         nosso caso) nao propaga env pro wrapper -> env=NULL -> crash. */
        if (sdv_flag("SDV_PATCH_RT", "SDV_NO_PATCH_RT") && g_last_base) {
            patch4(g_last_base, 0x37300, 0x14000007);  /* b 0x3731c */
            patch4(g_last_base, 0x48778, 0x1400000a);  /* b 0x487a0 */
        }
        /* Pula o assertion lock-free-alloc.c:608 em init_size_class do libmonosgen
         * (b.ne @0x1e7954 -> NOP). block_size (w2) vem de mono_pagesize e deveria
         * ser pot de 2; se o assert dispara, block_size esta corrompido pelo
         * mismatch Bionic<->glibc. NOP deixa ver se mono avanca ou se corrompe. */
        if (sdv_flag("SDV_PATCH_ASSERT", "SDV_NO_PATCH_ASSERT") && g_mono_base) {
            patch4(g_mono_base, 0x1e7954, 0xd503201f);  /* NOP */
        }
        fprintf(stderr, "\n=== SDV_RUNTIME_INIT: chamando Runtime_init ===\n");
        /* Assinatura REAL (8 args — o wrapper Java_mono_android_Runtime_init
         * consome x0-x6 + 1 stack, ignora x7, e internamente poe int=0/uchar=0):
         *   (JNIEnv, jclass, jstring lang, jobjectArray runtimeArgs,
         *    jstring runtimeMode, jobjectArray libFolders,
         *    jobject overrides, jobjectArray libcScan)
         * Strings via strdup (GetStringUTFChars trata); arrays vazios (token
         * 0x4000, GetArrayLength devolve 0). */
        void *env = jni_env_ptr();
        void *klass = (void *)0xC1A500;
        void *jstr_en = strdup("en");
        void *jstr_empty = strdup("");
        void *empty_arr = (void *)0x4000;
        /* libFolders: no Android real vem do Context (nativeLibraryDir). Passamos
         * nosso dir de libs (token 0x4001 -> 1 elemento = path). Isso faz o Mono
         * (a) propagar env pro wrapper (branch A em 0x37284) e (b) achar os
         * assemblies/AOT/DSO em vez de procurar em "(null)". */
        const char *libdir = getenv("SDV_LIBDIR");
        static char libdir_buf[4096];
        if (!libdir || !*libdir)
            libdir = sdv_path_in_base(libdir_buf, sizeof libdir_buf, "libs");
        jni_set_libdir(libdir);
        void *libfolders = (void *)0x4001;   /* LIBFOLDERS_TOKEN */
        typedef void (*runtime_init_t)(void *, void *, void *, void *, void *,
                                       void *, void *, void *);
        runtime_init_t rt = (runtime_init_t)p_rtinit;
        fprintf(stderr, "Runtime_init(8 args): env=%p klass=%p lang=%p libdir=%s\n",
                env, klass, jstr_en, libdir);
        rt(env, klass, jstr_en, empty_arr, jstr_empty, libfolders,
           (void *)0xC1A501, empty_arr);
        fprintf(stderr, "=== Runtime_init RETORNOU ===\n");

        /* A JVM normalmente executa os <clinit> das classes Java geradas pelo
         * Xamarin, que chamam Runtime.register(), e depois despacha
         * MainActivity.onCreate() ao handler n_onCreate registrado. Sem JVM,
         * reproduzimos somente essa sequencia. O DEX do APK 1.6.15.3 confirma
         * os nomes/assinaturas abaixo. Opt-in para preservar o milestone puro. */
        if (sdv_flag("SDV_START_ACTIVITY", "SDV_NO_START_ACTIVITY")) {
            if (!p_rtregister) {
                fprintf(stderr, "[activity] Runtime_register nao encontrado\n");
            } else {
                static const char base_methods[] =
                    "n_onCreate:(Landroid/os/Bundle;)V:GetOnCreate_Landroid_os_Bundle_Handler\n"
                    "n_onConfigurationChanged:(Landroid/content/res/Configuration;)V:GetOnConfigurationChanged_Landroid_content_res_Configuration_Handler\n"
                    "n_onPause:()V:GetOnPauseHandler\n"
                    "n_onResume:()V:GetOnResumeHandler\n"
                    "n_onDestroy:()V:GetOnDestroyHandler\n";
                static const char type_manager_methods[] =
                    "n_activate:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Object;[Ljava/lang/Object;)V:GetActivateHandler\n";
                static const char main_methods[] =
                    "n_onCreate:(Landroid/os/Bundle;)V:GetOnCreate_Landroid_os_Bundle_Handler\n"
                    "n_onResume:()V:GetOnResumeHandler\n"
                    "n_onStop:()V:GetOnStopHandler\n"
                    "n_onDestroy:()V:GetOnDestroyHandler\n"
                    "n_onWindowFocusChanged:(Z)V:GetOnWindowFocusChanged_ZHandler\n"
                    "n_onPause:()V:GetOnPauseHandler\n"
                    "n_onRequestPermissionsResult:(I[Ljava/lang/String;[I)V:GetOnRequestPermissionsResult_IarrayLjava_lang_String_arrayIHandler\n"
                    "n_onActivityResult:(IILandroid/content/Intent;)V:GetOnActivityResult_IILandroid_content_Intent_Handler\n";
                static const char game_view_methods[] =
                    "n_onKeyDown:(ILandroid/view/KeyEvent;)Z:GetOnKeyDown_ILandroid_view_KeyEvent_Handler\n"
                    "n_onKeyUp:(ILandroid/view/KeyEvent;)Z:GetOnKeyUp_ILandroid_view_KeyEvent_Handler\n"
                    "n_onGenericMotionEvent:(Landroid/view/MotionEvent;)Z:GetOnGenericMotionEvent_Landroid_view_MotionEvent_Handler\n"
                    "n_surfaceChanged:(Landroid/view/SurfaceHolder;III)V:GetSurfaceChanged_Landroid_view_SurfaceHolder_IIIHandler:Android.Views.ISurfaceHolderCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
                    "n_surfaceCreated:(Landroid/view/SurfaceHolder;)V:GetSurfaceCreated_Landroid_view_SurfaceHolder_Handler:Android.Views.ISurfaceHolderCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
                    "n_surfaceDestroyed:(Landroid/view/SurfaceHolder;)V:GetSurfaceDestroyed_Landroid_view_SurfaceHolder_Handler:Android.Views.ISurfaceHolderCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
                    "n_onTouch:(Landroid/view/View;Landroid/view/MotionEvent;)Z:GetOnTouch_Landroid_view_View_Landroid_view_MotionEvent_Handler:Android.Views.View/IOnTouchListenerInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n";
                typedef void (*runtime_register_t)(void *, void *, void *, void *, void *);
                runtime_register_t reg = (runtime_register_t)p_rtregister;
                void *runtime_class = (void *)0xC1A500; /* x1 e ignorado pelo wrapper */
                void *type_manager_class = jni_make_class("mono.android.TypeManager");
                void *base_class = jni_make_class("crc64493ac3851fab1842.AndroidGameActivity");
                void *main_class = jni_make_class("com.chucklefish.stardewvalley.MainActivity");
                void *game_view_class = jni_make_class("crc64493ac3851fab1842.MonoGameAndroidGameView");

                fprintf(stderr, "\n=== SDV_START_ACTIVITY: Runtime.register(TypeManager) ===\n");
                reg(env, runtime_class,
                    (void *)"Java.Interop.TypeManager+JavaTypeManager, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null",
                    type_manager_class, (void *)type_manager_methods);
                fprintf(stderr, "\n=== SDV_START_ACTIVITY: Runtime.register(base) ===\n");
                reg(env, runtime_class,
                    (void *)"Microsoft.Xna.Framework.AndroidGameActivity, MonoGame.Framework",
                    base_class, (void *)base_methods);
                fprintf(stderr, "=== SDV_START_ACTIVITY: Runtime.register(MainActivity) ===\n");
                reg(env, runtime_class,
                    (void *)"StardewValley.MainActivity, StardewValley",
                    main_class, (void *)main_methods);
                fprintf(stderr, "=== SDV_START_ACTIVITY: Runtime.register(GameView) ===\n");
                reg(env, runtime_class,
                    (void *)"Microsoft.Xna.Framework.MonoGameAndroidGameView, MonoGame.Framework",
                    game_view_class, (void *)game_view_methods);

                /* O construtor Java gerado chama TypeManager.Activate somente
                 * na classe concreta (o ctor da base detecta subclass e pula).
                 * Isso associa o jobject Activity ao MainActivity gerenciado;
                 * sem a ativacao, n_onCreate cai em TypeManager.CreateProxy. */
                void *activity = jni_make_object(main_class);
                jni_set_activity(activity);
                void *activate = jni_find_registered_native(
                    "n_activate",
                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Object;[Ljava/lang/Object;)V");
                fprintf(stderr, "[activity] TypeManager.n_activate = %p\n", activate);
                if (activate) {
                    typedef void (*activate_t)(void *, void *, void *, void *, void *, void *);
                    fprintf(stderr, "=== chamando TypeManager.n_activate(this=%p) ===\n", activity);
                    ((activate_t)activate)(env, type_manager_class,
                        (void *)"StardewValley.MainActivity, StardewValley",
                        (void *)"", activity, empty_arr);
                    fprintf(stderr, "=== TypeManager.n_activate RETORNOU ===\n");
                }

                void *entry = jni_find_registered_native(
                    "n_onCreate", "(Landroid/os/Bundle;)V");
                fprintf(stderr, "[activity] MainActivity.n_onCreate = %p\n", entry);
                if (entry) {
                    typedef void (*on_create_t)(void *, void *, void *);
                    fprintf(stderr, "=== chamando MainActivity.n_onCreate(this=%p, bundle=NULL) ===\n",
                            activity);
                    ((on_create_t)entry)(env, activity, NULL);
                    fprintf(stderr, "=== MainActivity.n_onCreate RETORNOU ===\n");

                    void *view = jni_find_object("crc64493ac3851fab1842.MonoGameAndroidGameView");
                    void *holder = jni_find_object("android.view.SurfaceHolder");
                    void *surface_created = jni_find_registered_native(
                        "n_surfaceCreated", "(Landroid/view/SurfaceHolder;)V");
                    void *surface_changed = jni_find_registered_native(
                        "n_surfaceChanged", "(Landroid/view/SurfaceHolder;III)V");
                    fprintf(stderr, "[surface] view=%p holder=%p created=%p changed=%p\n",
                            view, holder, surface_created, surface_changed);
                    if (view && holder && surface_created && surface_changed) {
                        typedef void (*surface_created_t)(void *, void *, void *);
                        typedef void (*surface_changed_t)(void *, void *, void *, int, int, int);
                        fprintf(stderr, "=== chamando surfaceCreated ===\n");
                        ((surface_created_t)surface_created)(env, view, holder);
                        int surface_width = sdv_egl_width();
                        int surface_height = sdv_egl_height();
                        fprintf(stderr, "=== chamando surfaceChanged(%dx%d) ===\n",
                                surface_width, surface_height);
                        ((surface_changed_t)surface_changed)(env, view, holder, 1,
                                                             surface_width,
                                                             surface_height);
                        fprintf(stderr, "=== surface callbacks RETORNARAM ===\n");
                    }

                    void *resume = jni_find_registered_native("n_onResume", "()V");
                    if (resume) {
                        typedef void (*resume_t)(void *, void *);
                        fprintf(stderr, "=== chamando MainActivity.n_onResume ===\n");
                        ((resume_t)resume)(env, activity);
                        fprintf(stderr, "=== MainActivity.n_onResume RETORNOU ===\n");
                        /* Libera SyncContext.Send somente depois de View.Resume
                         * mudar o worker de Exited para Resuming. Antes disso o
                         * primeiro RunIteration cancelaria a task para sempre. */
                        jni_set_main_looper_ready(1);
                    }

                    if (sdv_flag("SDV_HOLD", "SDV_NO_HOLD")) {
                        fprintf(stderr, "=== SDV_HOLD: mantendo processo vivo ===\n");
                        void *key_down = jni_find_registered_native(
                            "n_onKeyDown", "(ILandroid/view/KeyEvent;)Z");
                        void *key_up = jni_find_registered_native(
                            "n_onKeyUp", "(ILandroid/view/KeyEvent;)Z");
                        void *touch = jni_find_registered_native(
                            "n_onTouch",
                            "(Landroid/view/View;Landroid/view/MotionEvent;)Z");
                        if (view && key_down && key_up)
                            sdv_run_input_loop(env, view, key_down, key_up,
                                               touch);
                        else
                            for (;;) pause();
                    }
                }
            }
        }
    } else if (!sdv_flag("SDV_RUNTIME_INIT", "SDV_NO_RUNTIME_INIT")) {
        fprintf(stderr, "(set SDV_RUNTIME_INIT=1 para tentar bootar o Mono)\n");
    }

    fprintf(stderr, "\n=== main: alcancado fim do bootstrap (milestone JNI_OnLoad) ===\n");
    _exit(0);
}
