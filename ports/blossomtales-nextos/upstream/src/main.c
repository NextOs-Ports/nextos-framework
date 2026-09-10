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
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"
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

/* Handler nativo do Activity1.onPause (marshal method do libxamarin-app) e a
 * Activity a que ele pertence. O SELECT+START entrega onPause antes de sair:
 * e nele que o MonoGame pausa o game loop e o jogo grava o save. */
static void *g_on_pause_handler;
static void *g_on_pause_env;
static int g_instance_lock_fd = -1;

/* Trava no proprio processo: scripts e nomes em /proc podem mudar, mas o
 * kernel nunca concede este flock a duas instancias. Falha fechada antes de
 * SDL, Mono, framebuffer ou alocacoes grandes. O lock e liberado inclusive
 * em crash/SIGKILL quando o kernel fecha o fd. */
static int sb_acquire_instance_lock(void) {
    const char *path = "/tmp/blossomtales.instance.lock";
    g_instance_lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_instance_lock_fd < 0) {
        fprintf(stderr, "[instance-lock] open falhou: %s; abortando\n",
                strerror(errno));
        return 0;
    }
    if (flock(g_instance_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr,
                "[instance-lock] outra instancia de blossomtales ja esta ativa; abortando\n");
        close(g_instance_lock_fd);
        g_instance_lock_fd = -1;
        return 0;
    }
    if (ftruncate(g_instance_lock_fd, 0) == 0) {
        dprintf(g_instance_lock_fd, "%ld\n", (long)getpid());
        (void)lseek(g_instance_lock_fd, 0, SEEK_SET);
    }
    fprintf(stderr, "[instance-lock] adquirido pid=%ld\n", (long)getpid());
    return 1;
}

static void exit_request_handler(int signal_number) {
    (void)signal_number;
    g_exit_requested = 1;
}

/* Tabela combinada (mono+xamarin+shims) persistente p/ resolver imports dos
 * .so Bionic carregados dinamicamente via sdv_so_dlopen (componentes mono,
 * libaot-*). Setada apos a cadeia principal em main(). */
DynLibFunction *g_resolv_tbl = NULL;
int g_resolv_n = 0;

/* Snapshot de simbolos do libxamarin-app.so. Esta build usa MARSHAL METHODS
 * (.NET for Android 9+): os corpos nativos dos ACW sao exportados diretamente
 * pelo libxamarin-app com o nome JNI curto, e nao ha mono.android.Runtime
 * .register(). O jni_shim usa isto para achar handlers (ex.: o n_onComplete do
 * PlayGamesManager_AuthListener) a partir da classe do objeto fake. */
static DynLibFunction *g_xam_tbl = NULL;
static int g_xam_n = 0;

void *sb_xam_sym_prefix(const char *prefix) {
    if (!prefix || !g_xam_tbl) return NULL;
    size_t n = strlen(prefix);
    for (int i = 0; i < g_xam_n; i++)
        if (g_xam_tbl[i].symbol && strncmp(g_xam_tbl[i].symbol, prefix, n) == 0)
            return (void *)g_xam_tbl[i].func;
    return NULL;
}

/* Java_mono_android_Runtime_register do libmonodroid — usado pelo jni_shim
 * p/ registrar tipos ACW sob demanda (ex.: ICallback do SDK Netflix). */
uintptr_t g_runtime_register = 0;

/* ---- dlopen/dlsym via nosso so-loader p/ .so Bionic ----
 * Componentes mono (libmono-component-marshal-ilgen.so) e libaot-*.dll.so sao
 * ELF Bionic — glibc dlopen rejeita ("invalid ELF header"). Carregamos via
 * so_util (mesmo mecanismo da cadeia principal) e expomos dlsym via snapshot. */
#define SB_HANDLE_MAGIC 0x53445648u
struct sdv_dlhandle {
    uint32_t magic;
    DynLibFunction *snap;
    int n;
    char *name;
    struct sdv_dlhandle *next;
};

/* JavaVM falsa, criada em main() antes do JNI_OnLoad do libmonodroid. Modulos
 * carregados depois recebem a mesma VM no proprio JNI_OnLoad. */
static void *g_fake_vm;

static void sb_register_module(const char *name, uintptr_t base, size_t size);

static pthread_mutex_t g_so_loader_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_so_handles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sdv_dlhandle *g_so_handles;
static _Thread_local int g_so_loader_active;

void *sdv_so_dlopen(const char *name);
void *sdv_so_dlsym(void *handle, const char *name);
int sdv_so_is_handle(void *handle);
int sdv_so_dlclose(void *handle);

/* Tabela acumulada com os simbolos de tudo que ja foi carregado por
 * sdv_so_dlopen. O linker do Android resolve um .so contra as suas dependencias
 * ja mapeadas; sem isso, libfmodstudio.so entrava com TODOS os simbolos FMOD
 * (definidos em libfmod.so) nao resolvidos e o primeiro playSound crashava. */
static DynLibFunction *g_dyn_tbl = NULL;
static int g_dyn_n = 0;

static void dyn_tbl_append(DynLibFunction *snap, int n) {
    if (!snap || n <= 0) return;
    DynLibFunction *t = realloc(g_dyn_tbl, sizeof(DynLibFunction) * (size_t)(g_dyn_n + n));
    if (!t) return;
    memcpy(t + g_dyn_n, snap, sizeof(DynLibFunction) * (size_t)n);
    g_dyn_tbl = t;
    g_dyn_n += n;
}

static struct sdv_dlhandle *so_handle_by_name(const char *name) {
    struct sdv_dlhandle *found = NULL;
    pthread_mutex_lock(&g_so_handles_lock);
    for (struct sdv_dlhandle *h = g_so_handles; h; h = h->next)
        if (h->name && strcmp(h->name, name) == 0) { found = h; break; }
    pthread_mutex_unlock(&g_so_handles_lock);
    return found;
}

/* Le os DT_NEEDED de um ELF Android sem mapea-lo. Usado para carregar as
 * dependencias antes do modulo, como faz o linker do Android. */
static void sb_load_needed(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char eh[64];
    if (fread(eh, 1, sizeof eh, f) != sizeof eh || memcmp(eh, "\177ELF", 4) != 0 ||
        eh[4] != 2) { fclose(f); return; }
    uint64_t phoff = *(uint64_t *)(eh + 32);
    uint16_t phentsize = *(uint16_t *)(eh + 54), phnum = *(uint16_t *)(eh + 56);
    uint64_t dyn_off = 0, dyn_size = 0;
    for (int i = 0; i < phnum; i++) {
        unsigned char ph[56];
        if (fseeko(f, (off_t)(phoff + (uint64_t)i * phentsize), SEEK_SET) != 0) break;
        if (fread(ph, 1, sizeof ph, f) != sizeof ph) break;
        if (*(uint32_t *)ph == 2 /* PT_DYNAMIC */) {
            dyn_off = *(uint64_t *)(ph + 8);
            dyn_size = *(uint64_t *)(ph + 32);
            break;
        }
    }
    if (!dyn_off || !dyn_size || dyn_size > (16u << 20)) { fclose(f); return; }
    unsigned char *dyn = malloc((size_t)dyn_size);
    if (!dyn) { fclose(f); return; }
    if (fseeko(f, (off_t)dyn_off, SEEK_SET) != 0 ||
        fread(dyn, 1, (size_t)dyn_size, f) != dyn_size) {
        free(dyn); fclose(f); return;
    }
    /* DT_STRTAB e um endereco virtual; nos ELF do Android o segmento inicial e
     * mapeado em vaddr 0, entao o offset no arquivo coincide. */
    uint64_t strtab = 0, strsz = 0;
    for (uint64_t o = 0; o + 16 <= dyn_size; o += 16) {
        uint64_t tag = *(uint64_t *)(dyn + o), val = *(uint64_t *)(dyn + o + 8);
        if (tag == 0) break;
        if (tag == 5) strtab = val;
        else if (tag == 10) strsz = val;
    }
    char *strs = NULL;
    if (strtab && strsz && strsz < (16u << 20)) {
        strs = malloc((size_t)strsz + 1);
        if (strs) {
            if (fseeko(f, (off_t)strtab, SEEK_SET) != 0 ||
                fread(strs, 1, (size_t)strsz, f) != strsz) { free(strs); strs = NULL; }
            else strs[strsz] = 0;
        }
    }
    fclose(f);
    if (strs) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0; else dir[0] = 0;
        for (uint64_t o = 0; o + 16 <= dyn_size; o += 16) {
            uint64_t tag = *(uint64_t *)(dyn + o), val = *(uint64_t *)(dyn + o + 8);
            if (tag == 0) break;
            if (tag != 1 /* DT_NEEDED */ || val >= strsz) continue;
            const char *dep = strs + val;
            if (strncmp(dep, "lib", 3) != 0) continue;
            char dep_path[2048];
            snprintf(dep_path, sizeof dep_path, "%s/%s", dir, dep);
            if (access(dep_path, R_OK) != 0) continue;
            if (so_handle_by_name(dep_path)) continue;
            fprintf(stderr, "[so_dlopen] dependencia DT_NEEDED de %s: %s\n", path, dep);
            sdv_so_dlopen(dep_path);
        }
    }
    free(strs);
    free(dyn);
}

void *sdv_so_dlopen(const char *name) {
    if (!name || !g_resolv_tbl) return NULL;
    /* Imagens AOT (libaot-*.dll.so) tem PLT/IRELATIVE que nosso so-loader nao
     * trata — carregar causa lazy-PLT crash no ld-linux. Recusar aqui forca o
     * mono a JIT-ar a assembly (mais lento, mas evita o caminho AOT). */
    if (strstr(name, "libaot-")) {
        fprintf(stderr, "[so_dlopen] recusando AOT '%s' (forca JIT)\n", name);
        return NULL;
    }
    /* EOS (Epic Online Services, multiplayer online) crasha no init_array
     * (56 ctors, imports OpenSL/sigsetjmp nao resolvidos). O jogo e co-op
     * local no device; recusar deixa o managed seguir sem online. */
    if (strstr(name, "libEOSSDK")) {
        fprintf(stderr, "[so_dlopen] recusando EOS '%s' (sem online)\n", name);
        return NULL;
    }
    /* so_load usa fopen no caminho exato e nao pesquisa LD_LIBRARY_PATH.
     * O runtime primeiro sonda nomes curtos e depois tenta o path completo;
     * rejeitar o miss antes do mmap evita reservar/desfazer 48 MiB em cada
     * uma dessas dezenas de sondagens normais do boot. */
    if (access(name, R_OK) != 0)
        return NULL;
    {   /* Mesmo .so pedido duas vezes devolve o mesmo handle (semantica dlopen). */
        struct sdv_dlhandle *prev = so_handle_by_name(name);
        if (prev) return prev;
    }
    /* Dependencias primeiro, fora do lock (o linker do Android faz o mesmo). */
    if (!g_so_loader_active) sb_load_needed(name);
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
    {   /* Resolve contra shims+mono+xamarin E contra tudo que ja foi carregado
         * dinamicamente (dependencias como libfmod.so). */
        int total = g_resolv_n + g_dyn_n;
        DynLibFunction *t = malloc(sizeof(DynLibFunction) * (size_t)total);
        if (t) {
            memcpy(t, g_resolv_tbl, sizeof(DynLibFunction) * (size_t)g_resolv_n);
            if (g_dyn_n)
                memcpy(t + g_resolv_n, g_dyn_tbl, sizeof(DynLibFunction) * (size_t)g_dyn_n);
            so_resolve(t, total, 0);
            free(t);
        } else {
            so_resolve(g_resolv_tbl, g_resolv_n, 0);
        }
    }
    so_finalize();
    so_flush_caches();
    int sn = 0;
    DynLibFunction *snap = so_snapshot_symbols(&sn);
    dyn_tbl_append(snap, sn);
    so_execute_init_array();
    so_free_temp();
    struct sdv_dlhandle *h = malloc(sizeof(*h));
    if (!h) {
        fprintf(stderr, "[so_dlopen] handle sem memoria para %s\n", name);
        g_so_loader_active = 0;
        pthread_mutex_unlock(&g_so_loader_lock);
        return NULL;
    }
    h->magic = SB_HANDLE_MAGIC;
    h->snap = snap;
    h->n = sn;
    h->name = strdup(name);
    pthread_mutex_lock(&g_so_handles_lock);
    h->next = g_so_handles;
    g_so_handles = h;
    pthread_mutex_unlock(&g_so_handles_lock);
    sb_register_module(name, (uintptr_t)text_base, text_size);
    fprintf(stderr, "[so_dlopen] %s OK: %d simbolos exportados\n", name, sn);
    g_so_loader_active = 0;
    pthread_mutex_unlock(&g_so_loader_lock);
    /* No Android, System.loadLibrary chama JNI_OnLoad do modulo recem-carregado.
     * O libfmod.so depende disso: e no JNI_OnLoad que ele guarda a JavaVM e
     * prepara org.fmod.FMOD. Sem esta chamada, Studio::initialize devolve
     * ERR_INTERNAL. Feito fora do lock porque o JNI_OnLoad pode carregar mais
     * modulos. */
    if (g_fake_vm) {
        void *onload = sdv_so_dlsym(h, "JNI_OnLoad");
        if (onload) {
            fprintf(stderr, "[so_dlopen] chamando JNI_OnLoad de %s\n", name);
            int v = ((int (*)(void *, void *))onload)(g_fake_vm, NULL);
            fprintf(stderr, "[so_dlopen] JNI_OnLoad(%s) -> 0x%x\n", name, v);
        }
    }
    return h;
}
/* Busca em TODOS os modulos Bionic carregados dinamicamente. Usada pelos shims
 * que precisam de um simbolo de outro modulo (ex.: o init do FMOD Studio
 * precisa de FMOD_System_SetOutput, que vive em libfmod.so). */
void *sdv_so_dlsym_global(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_dyn_n; i++)
        if (g_dyn_tbl[i].symbol && strcmp(g_dyn_tbl[i].symbol, name) == 0)
            return (void *)g_dyn_tbl[i].func;
    return NULL;
}

void *sb_fmod_intercept(const char *name, void *real,
                        void *(*resolve)(const char *));

void *sdv_so_dlsym(void *handle, const char *name) {
    struct sdv_dlhandle *h = (struct sdv_dlhandle *)handle;
    if (!name || !sdv_so_is_handle(handle) ||
        h->magic != SB_HANDLE_MAGIC)
        return NULL;
    for (int i = 0; i < h->n; i++)
        if (h->snap[i].symbol && strcmp(h->snap[i].symbol, name) == 0) {
            void *real = (void *)h->snap[i].func;
            void *hook = sb_fmod_intercept(name, real, sdv_so_dlsym_global);
            return hook ? hook : real;
        }
    return NULL;
}
int sdv_so_is_handle(void *handle) {
    int found = 0;

    pthread_mutex_lock(&g_so_handles_lock);
    for (struct sdv_dlhandle *h = g_so_handles; h; h = h->next) {
        if (handle == h) {
            found = h->magic == SB_HANDLE_MAGIC;
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
/* Registro dos modulos Bionic mapeados pelo so-loader. O dladdr da glibc nao
 * conhece essas imagens e responde com o objeto mapeado mais proximo, o que
 * atribui o endereco ao modulo errado (ja mandou procurar bug em ld-linux). */
struct sb_module { const char *name; uintptr_t base; size_t size; };
#define SB_MAX_MODULES 64
static struct sb_module g_modules[SB_MAX_MODULES];
static int g_module_n;

static void sb_register_module(const char *name, uintptr_t base, size_t size) {
    if (g_module_n >= SB_MAX_MODULES) return;
    const char *b = strrchr(name, '/');
    g_modules[g_module_n].name = strdup(b ? b + 1 : name);
    g_modules[g_module_n].base = base;
    g_modules[g_module_n].size = size;
    g_module_n++;
}

static const struct sb_module *sb_module_for(uintptr_t addr) {
    for (int i = 0; i < g_module_n; i++)
        if (addr >= g_modules[i].base && addr < g_modules[i].base + g_modules[i].size)
            return &g_modules[i];
    return NULL;
}

/* Ultimo recurso: quem e o dono do endereco segundo o kernel. */
static void print_maps_owner(uintptr_t addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (addr >= lo && addr < hi) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            fprintf(stderr, " [maps %s +0x%lx]", line, (unsigned long)(addr - lo));
            break;
        }
    }
    fclose(f);
}

static void print_addr(const char *label, uintptr_t addr) {
    fprintf(stderr, "  %s=%p", label, (void *)addr);
    const struct sb_module *m = sb_module_for(addr);
    if (m) {
        fprintf(stderr, " = %s+0x%lx", m->name, (unsigned long)(addr - m->base));
        fprintf(stderr, "\n");
        return;
    }
    Dl_info di;
    if (dladdr((void *)addr, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        b = b ? b + 1 : di.dli_fname;
        fprintf(stderr, " = %s+0x%lx", b, (unsigned long)(addr - (uintptr_t)di.dli_fbase));
    }
    print_maps_owner(addr);
    fprintf(stderr, "\n");
}
static void crash_handler(int sig, siginfo_t *info, void *uc) {
    uintptr_t fault = (uintptr_t)info->si_addr;
    fprintf(stderr, "\n=== CRASH sig=%d addr=%p (mod=%s) ===\n",
            sig, info->si_addr, g_last_name);
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

/* mono_trace.c -- sondas diagnostico M2 (SB_JIT_TRACE / SB_THREAD_TEST) */
void sb_mono_trace_install(DynLibFunction *tbl, int n);
void sb_thread_test(void);

void sdv_promote_current_mono_thread(void) {
    static _Thread_local int attempted;
    if (attempted || getenv("SB_NO_THREAD_PROMOTE")) return;
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

/* Contexto provado pela engine para o GPTK vivo (bt_input): Game1.instance ->
 * currentState (classe GameState_*) e Game1.Pauser.IsPaused, lidos pelo
 * embedding do Mono. Objetos de vida longa (singletons criados no boot);
 * nenhuma alocação gerenciada, nenhum invoke: só leituras de campo. Devolve 0
 * enquanto o assembly/instância não existem — o adapter fica em passthrough. */
int sb_engine_state_probe(char *state, size_t state_cap, int *paused) {
    typedef void *(*fn_str)(const char *);
    typedef void *(*fn_ptr_str_str)(void *, const char *, const char *);
    typedef void *(*fn_ptr_str)(void *, const char *);
    typedef void *(*fn_ptr_ptr)(void *, void *);
    typedef void *(*fn_void)(void);
    typedef void (*fn_ptr_ptr_ptr)(void *, void *, void *);
    typedef const char *(*fn_name)(void *);
    static int resolved, failed;
    static fn_str image_loaded; static fn_ptr_str_str class_from_name;
    static fn_ptr_str field_from_name; static fn_ptr_ptr class_vtable;
    static fn_void domain_get; static fn_ptr_ptr_ptr static_get, field_get;
    static fn_name class_get_name; static fn_str object_get_class_s;
    static void *klass_game1, *f_instance, *f_state, *f_pauser, *klass_pause, *f_ispaused;
    if (state && state_cap) state[0] = 0;
    if (paused) *paused = 0;
    if (failed) return 0;
    if (!resolved) {
        image_loaded = (fn_str)tbl_find(g_resolv_tbl, g_resolv_n, "mono_image_loaded");
        class_from_name = (fn_ptr_str_str)tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_from_name");
        field_from_name = (fn_ptr_str)tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_get_field_from_name");
        class_vtable = (fn_ptr_ptr)tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_vtable");
        domain_get = (fn_void)tbl_find(g_resolv_tbl, g_resolv_n, "mono_domain_get");
        static_get = (fn_ptr_ptr_ptr)tbl_find(g_resolv_tbl, g_resolv_n, "mono_field_static_get_value");
        field_get = (fn_ptr_ptr_ptr)tbl_find(g_resolv_tbl, g_resolv_n, "mono_field_get_value");
        class_get_name = (fn_name)tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_get_name");
        object_get_class_s = (fn_str)tbl_find(g_resolv_tbl, g_resolv_n, "mono_object_get_class");
        if (!image_loaded || !class_from_name || !field_from_name || !class_vtable ||
            !domain_get || !static_get || !field_get || !class_get_name || !object_get_class_s) {
            fprintf(stderr, "[bt/engine] embedding API incompleta; contexto fica não provado\n");
            failed = 1;
            return 0;
        }
        void *image = image_loaded("BlossomTalesAndroid");
        if (!image) return 0;            /* assembly ainda não carregou */
        klass_game1 = class_from_name(image, "BlossomTales", "Game1");
        klass_pause = class_from_name(image, "BlossomTales", "PauseScreen");
        f_instance = klass_game1 ? field_from_name(klass_game1, "instance") : NULL;
        f_state = klass_game1 ? field_from_name(klass_game1, "currentState") : NULL;
        f_pauser = klass_game1 ? field_from_name(klass_game1, "Pauser") : NULL;
        f_ispaused = klass_pause ? field_from_name(klass_pause, "IsPaused") : NULL;
        if (!f_instance || !f_state || !f_pauser || !f_ispaused) {
            fprintf(stderr, "[bt/engine] contrato Game1/PauseScreen indisponível "
                            "(instance=%d currentState=%d Pauser=%d IsPaused=%d)\n",
                    f_instance != NULL, f_state != NULL, f_pauser != NULL, f_ispaused != NULL);
            failed = 1;
            return 0;
        }
        fprintf(stderr, "[bt/engine] state contract: ready (Game1.instance/currentState, Pauser.IsPaused)\n");
        resolved = 1;
    }
    void *vtable = class_vtable(domain_get(), klass_game1);
    if (!vtable) return 0;
    void *game = NULL, *cur = NULL, *pauser = NULL;
    static_get(vtable, f_instance, &game);
    if (!game) return 0;                 /* ctor do Game1 ainda não rodou */
    field_get(game, f_state, &cur);
    static_get(vtable, f_pauser, &pauser);
    if (cur) {
        void *k = ((void *(*)(void *))object_get_class_s)(cur);
        const char *n = k ? class_get_name(k) : NULL;
        if (n && state) snprintf(state, state_cap, "%s", n);
    }
    if (pauser && paused) {
        uint8_t v = 0;
        field_get(pauser, f_ispaused, &v);
        *paused = v != 0;
    }
    return 1;
}

/* No retail, RenderOnUIThread=true faz cada RunIteration rodar via Looper na
 * MESMA thread do onCreate — a thread capturada em ContextManager.mainThread
 * (ctor do Game). Sem Looper aqui, o SyncContext.Send roda inline na thread do
 * dispatcher, entao mainThread != thread do loop e EnsureLock() do
 * ParisContentManager cai no Monitor.Enter cego em vez do TryEnter que bombeia
 * ProcessThreadingBlockedActions() — deadlock com o BlockOnUIThread das
 * threads de Preload (provado por gdb+mono_pmip na tela de loading). Reapontar
 * mainThread para a thread do loop restaura a semantica do retail. */
void sdv_fix_paris_mainthread(void) {
    static int done;
    if (done || getenv("SB_NO_MAINTHREAD_FIX")) return;
    /* ParisEngine e a engine do TMNT; o BlossomTales usa MonoGame puro e nao
     * tem ContextManager. Sai de vez para nao repetir lookups a cada swap. */
    done = 1;

    typedef void *(*fn_void)(void);
    typedef void *(*fn_str)(const char *);
    typedef void *(*fn_ptr_str_str)(void *, const char *, const char *);
    typedef void *(*fn_ptr_str)(void *, const char *);
    typedef void *(*fn_ptr_ptr)(void *, void *);
    typedef void (*fn_ptr_ptr_ptr)(void *, void *, void *);

    fn_void thread_current = (fn_void)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_thread_current");
    fn_str image_loaded = (fn_str)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_image_loaded");
    fn_ptr_str_str class_from_name = (fn_ptr_str_str)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_from_name");
    fn_ptr_str field_from_name = (fn_ptr_str)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_get_field_from_name");
    fn_ptr_ptr class_vtable = (fn_ptr_ptr)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_class_vtable");
    fn_void domain_get = (fn_void)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_domain_get");
    fn_ptr_ptr_ptr static_get = (fn_ptr_ptr_ptr)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_field_static_get_value");
    fn_ptr_ptr_ptr field_set = (fn_ptr_ptr_ptr)
        tbl_find(g_resolv_tbl, g_resolv_n, "mono_field_set_value");
    if (!thread_current || !image_loaded || !class_from_name ||
        !field_from_name || !class_vtable || !domain_get || !static_get ||
        !field_set) {
        fprintf(stderr, "[paris-mainthread] embedding API incompleta\n");
        done = 1;
        return;
    }

    void *image = image_loaded("ParisEngine");
    if (!image) return;   /* assembly ainda nao carregou; reentra no proximo swap */
    void *klass = class_from_name(image, "Paris.Engine.Context", "ContextManager");
    if (!klass) {
        fprintf(stderr, "[paris-mainthread] ContextManager nao encontrado\n");
        done = 1;
        return;
    }
    void *f_singleton = field_from_name(klass, "_singleton");
    void *f_mainthread = field_from_name(klass, "mainThread");
    if (!f_singleton || !f_mainthread) {
        fprintf(stderr, "[paris-mainthread] campos _singleton/mainThread nao encontrados\n");
        done = 1;
        return;
    }
    void *vtable = class_vtable(domain_get(), klass);
    if (!vtable) return;
    void *singleton = NULL;
    static_get(vtable, f_singleton, &singleton);
    if (!singleton) return;   /* ctor do Game ainda nao rodou; reentra no proximo swap */
    field_set(singleton, f_mainthread, thread_current());
    fprintf(stderr, "[paris-mainthread] ContextManager.mainThread = thread do "
                    "game loop (EnsureLock volta a bombear a fila GL)\n");
    done = 1;
}

/* Saida limpa por SELECT+START.
 *
 * 1. onPause: e o gancho onde o MonoGame para o game loop e o jogo grava o
 *    save. Sem ele o progresso da sessao se perde.
 * 2. _exit(0): se main retornar, o atexit da glibc desmonta o contexto GL e o
 *    Mali-450 trava o KERNEL. Nunca trocar por return/exit().
 * 3. alarm: se o managed travar dentro do onPause (o game loop pode estar
 *    segurando um lock), o SIGALRM sai assim mesmo — a saida nunca pendura. */
#include "nxgl_frame_proof_adapter.h"
#include "bt_input.h"

static void sb_exit_alarm(int sig) {
    (void)sig;
    fprintf(stderr, "[sdv-input] onPause nao retornou a tempo; saindo\n");
    _exit(0);
}

void sb_pause_and_exit(void) {
    static volatile int already;
    if (!already) {
        already = 1;
        if (g_on_pause_handler && jni_activity()) {
            typedef void (*pause_t)(void *, void *);
            signal(SIGALRM, sb_exit_alarm);
            alarm(4);
            fprintf(stderr, "[sdv-input] entregando onPause antes de sair\n");
            ((pause_t)g_on_pause_handler)(g_on_pause_env, jni_activity());
            alarm(0);
            fprintf(stderr, "[sdv-input] onPause RETORNOU; saindo com status 0\n");
        }
    }
    /* Veredito de imagem antes de sair. `publish` e' idempotente e o adapter
     * pede que seja chamado tambem no desligamento. */
    nxgl_frame_proof_publish();
    _exit(0);
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

/* so_load faz fopen(name) no path exato (nao pesquisa LD_LIBRARY_PATH). Os .so
 * do APK ficam em SB_LIBDIR (ex.: $GAMEDIR/libs). Resolve o nome curto contra
 * o CWD e depois contra SB_LIBDIR. */
static const char *resolve_lib(const char *name) {
    static char path[1024];   /* so-loader carrega single-thread; sem TLS */
    if (access(name, R_OK) == 0) return name;
    const char *ld = getenv("SB_LIBDIR");
    if (ld && *ld) {
        snprintf(path, sizeof path, "%s/%s", ld, name);
        if (access(path, R_OK) == 0) return path;
    }
    return name;  /* deixa so_load falhar com o nome original */
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
    g_last_name = name;
    size_t hs = (size_t)heap_mb * 1024 * 1024;
    void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) { fprintf(stderr, "mmap %s falhou\n", name); exit(1); }
    fprintf(stderr, "\n== carregando %s (heap %p, %dMB) ==\n", name, heap, heap_mb);
    if (so_load(resolve_lib(name), heap, hs) < 0) { fprintf(stderr, "so_load(%s) falhou\n", name); exit(1); }
    if (so_relocate() < 0) { fprintf(stderr, "so_relocate(%s) falhou\n", name); exit(1); }
    so_resolve(tbl, n, 0);
    so_finalize();
    so_flush_caches();
    g_last_base = (uintptr_t)text_base;
    sb_register_module(name, (uintptr_t)text_base, text_size);
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
    if (!sb_acquire_instance_lock()) return 73;
    install_crash_handler();

    fprintf(stderr, "=== BlossomTales (MonoGame/.NET Android) Mono so-loader / NextOS aarch64 ===\n");
    /* Recibo de lancamento do nxgl: emitido ANTES de qualquer coisa poder
     * falhar, para que uma tela preta nao vire um log limpo. */
    nxgl_frame_proof_launch_receipt();
    preload_device_libs();

    /* nxinput 0.10.2: owner/default do NEXTOSCONTROLLERS.gptk + costura C6
     * lidos/encenados UMA vez, antes de qualquer SDL_Init (o bootstrap gráfico
     * abaixo inicializa o GAMECONTROLLER). Sem mapa válido não há palpite. */
    if (getenv("SB_START_ACTIVITY")) {
        /* O launcher entra no GAMEDIR antes de executar o port (cwd = pasta do
         * jogo, onde vivem NEXTOSCONTROLLERS.gptk e controllers*.nxb). */
        static char gamedir_cwd[1024];
        const char *gamedir = getenv("GAMEDIR");
        if ((!gamedir || !*gamedir) && getcwd(gamedir_cwd, sizeof gamedir_cwd))
            gamedir = gamedir_cwd;
        if (bt_input_preinit(gamedir && *gamedir ? gamedir : ".") != 0) {
            fprintf(stderr, "[bt/input] pre-init failed closed (NEXTOSCONTROLLERS / NXC6 seam)\n");
            _exit(70);
        }
    }

    /* A janela/contexto precisam nascer na main thread. O contexto e liberado
     * aqui e adquirido depois pela worker do MonoGame via o fake EGL10. */
    if (getenv("SB_START_ACTIVITY") && !sdv_egl_init())
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

    /* Sondas M2: profiler JIT/exc/thread — instalar antes do Runtime_init */
    sb_mono_trace_install(mono_tbl, mono_n);

    /* M2: libxamarin-app */
    load_module(XAMARIN_SO, XAMARIN_HEAP_MB, base, base_n);
    int xam_n = 0;
    DynLibFunction *xam_tbl = so_snapshot_symbols(&xam_n);
    fprintf(stderr, "xamarin-app: %d simbolos exportados\n", xam_n);
    g_xam_tbl = xam_tbl;
    g_xam_n = xam_n;

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
    g_runtime_register = p_rtregister;
    fprintf(stderr, "\nJNI_OnLoad            = %p\n", (void *)p_onload);
    fprintf(stderr, "Java_...Runtime_init   = %p\n", (void *)p_rtinit);
    fprintf(stderr, "Java_...initInternal   = %p\n", (void *)p_rtinit_i);
    fprintf(stderr, "Java_...Runtime_register = %p\n", (void *)p_rtregister);

    if (!p_onload) { fprintf(stderr, "JNI_OnLoad nao encontrado\n"); _exit(2); }

    /* fake JavaVM/JNIEnv e chama JNI_OnLoad */
    void *vm = jni_build_env();
    g_fake_vm = vm;
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
    if (getenv("SB_MONO_JIT")) {
        /* Pula a assertion power-of-2 do lock-free-alloc (init_size_class @0x1e7954
         * b.ne -> NOP). block_size runtime vem corrompido pela ABI; veremos o
         * proximo muro apos este. */
        if (getenv("SB_PATCH_ASSERT") && g_mono_base) {
            patch4(g_mono_base, 0x1e7954, 0xd503201f);  /* NOP */
        }
        uintptr_t p_jit = tbl_find(mono_tbl, mono_n, "mono_jit_init_version");
        fprintf(stderr, "\nmono_jit_init_version = %p\n", (void *)p_jit);
        if (p_jit) {
            void *(*jit)(const char *, const char *) = (void *(*)(const char *, const char *))p_jit;
            fprintf(stderr, "=== chamando mono_jit_init_version(\"BlossomTales.Android\",\"v4.0.30319\") ===\n");
            void *domain = jit("BlossomTales.Android", "v4.0.30319");
            fprintf(stderr, "=== domain = %p ===\n", domain);
            if (domain) fprintf(stderr, ">>> MONO RUNTION INIT COM SUCESSO <<<\n");
        }
    }

    /* --- milestone Runtime_init: tenta bootar o Mono de verdade --- */
    if (p_rtinit && getenv("SB_RUNTIME_INIT")) {
        /* Os patches de offset fixo do TMNT eram do libmonodroid daquela versao.
         * Aqui o libmonodroid e outro build (.NET for Android 13.2.99) e o
         * libFolders nao-vazio ja propaga env, entao nao ha patch cego. */
        fprintf(stderr, "\n=== SB_RUNTIME_INIT: chamando Runtime_init ===\n");
        /* Assinatura REAL desta build (.NET for Android 10 / MonoVM 10.0.7),
         * derivada do disassembly do wrapper Java_mono_android_Runtime_init
         * @0x84de0 e do nome mangled de
         * MonodroidRuntime::Java_mono_android_Runtime_initInternal
         *   (JNIEnv*, jclass, jstring, jobjectArray, jstring, jobjectArray,
         *    int localDateTimeOffset, jobject loader, jobjectArray assemblies,
         *    uchar isEmulator, uchar haveSplitApks).
         *
         * O wrapper Runtime_init toma 8 argumentos (x0..x6 + stack[0]) e
         * preenche o resto: x6(loader)->x7, localDateTimeOffset=0,
         * stack[0](assemblies)->stack[0], isEmulator=0, haveSplitApks=0.
         *
         *   p0 env        -> x0        p4 nativeLibDir -> x4
         *   p1 klass      -> x1        p5 appDirs      -> x5
         *   p2 lang       -> x2        p6 loader       -> x6
         *   p3 runtimeApks-> x3        p7 assemblies   -> stack[0]
         *
         * Nao ha apiLevel nesta build (o scourgebringer, .NET 7, tinha 11 args).
         * Strings via strdup (GetStringUTFChars trata); arrays vazios usam o
         * token 0x4000 (GetArrayLength devolve 0). */
        void *env = jni_env_ptr();
        void *klass = (void *)0xC1A500;
        void *jstr_en = strdup("en");
        void *empty_arr = (void *)0x4000;
        /* appDirs: no Android real vem do Context (dataDir/cacheDir/
         * nativeLibraryDir). Passamos nosso dir (token 0x4001) para que o Mono
         * (a) propague env pro wrapper e (b) ache assemblies/AOT/DSO em vez de
         * procurar em "(null)". */
        const char *libdir = getenv("SB_LIBDIR");
        if (!libdir || !*libdir) libdir = "/storage/roms/ports/blossomtales/libs";
        jni_set_libdir(libdir);
        {   /* dataDir/cacheDir do app. Sem SB_DATADIR o comportamento fica
             * identico ao de antes (tudo no dir das libs). */
            const char *datadir = getenv("SB_DATADIR");
            if (datadir && *datadir) {
                char cache[1024];
                mkdir(datadir, 0755);
                snprintf(cache, sizeof(cache), "%s/cache", datadir);
                mkdir(cache, 0755);
                jni_set_datadir(datadir);
                fprintf(stderr, "[appdirs] dataDir=%s (fora do payload selado)\n",
                        datadir);
            }
        }
        void *libfolders = (void *)0x4001;   /* LIBFOLDERS_TOKEN */
        void *jstr_libdir = strdup(libdir);
        /* runtimeApks: as libs deste jogo sao STORED dentro do split APK
         * (extractNativeLibs=false), e o assembly store e
         * lib/arm64-v8a/libassembly-store.so. O monodroid abre os APKs desta
         * lista por mmap e le as entradas por offset. Passamos o
         * split_config.arm64_v8a.apk inteiro — nao ha necessidade do APK
         * reduzido que o scourgebringer teve de fabricar. */
        const char *apk = getenv("SB_APK");
        if (!apk || !*apk) apk = "/storage/roms/ports/blossomtales/game.apk";
        jni_set_apk_path(apk);
        void *apks = (void *)0x4002;         /* APKS_TOKEN */
        typedef void (*runtime_init_t)(void *, void *, void *, void *, void *,
                                       void *, void *, void *);
        runtime_init_t rt = (runtime_init_t)p_rtinit;
        fprintf(stderr, "Runtime_init(8 args): env=%p klass=%p lang=%p libdir=%s apk=%s\n",
                env, klass, jstr_en, libdir, apk);
        {   /* DIAG stack-guard Bionic: tp+0x28 deve cair DENTRO do pad e ser
             * estavel. Se off>0x28 ou valor != o do pad, o guard aliasa TLS glibc. */
            uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
            unsigned long padoff = (unsigned long)((uintptr_t)g_bionic_guard_pad - tp);
            fprintf(stderr,
                "[tls-diag] tpidr=%p &pad=%p pad_off=0x%lx cobre_0x28=%d "
                "*(tp+0x28)=0x%lx pad[0x28-off]=0x%lx\n",
                (void *)tp, (void *)g_bionic_guard_pad, padoff,
                (padoff <= 0x28 && 0x28 < padoff + 256),
                *(unsigned long *)(tp + 0x28),
                (padoff <= 0x28) ? *(unsigned long *)((uintptr_t)g_bionic_guard_pad + (0x28 - padoff)) : 0xdeadUL);
        }
        rt(env, klass, jstr_en, apks, jstr_libdir, libfolders,
           (void *)0xC1A501 /* loader */, empty_arr /* assemblies */);
        fprintf(stderr, "=== Runtime_init RETORNOU ===\n");

        /* Sonda M2: mono_thread_create isolado (SB_THREAD_TEST=1) */
        sb_thread_test();

        /* A JVM normalmente executa os <clinit> das classes Java geradas pelo
         * Xamarin, que chamam Runtime.register(), e depois despacha
         * MainActivity.onCreate() ao handler n_onCreate registrado. Sem JVM,
         * reproduzimos somente essa sequencia. O DEX do APK 1.6.15.3 confirma
         * os nomes/assinaturas abaixo. Opt-in para preservar o milestone puro. */
        if (getenv("SB_START_ACTIVITY")) {
            /* ---------------------------------------------------------------
             * Marshal methods (.NET for Android 9+): esta build NAO usa
             * mono.android.Runtime.register(). O libxamarin-app.so exporta
             * diretamente os corpos nativos dos ACW, com o nome JNI curto:
             *
             *   Java_com_castlepixel_blossomtales_Activity1_n_1onCreate__Landroid_os_Bundle_2
             *   Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1surfaceCreated
             *   Java_mono_android_TypeManager_n_1activate
             *
             * (`_1` e o escape JNI para `_`). Por isso nao existe aqui a
             * armadilha do scourgebringer de "um metodo a mais derruba o
             * registro inteiro": nao ha registro, so resolucao de simbolo.
             * A lista do dex continua sendo a referencia do que EXISTE — a
             * Activity1 deste jogo declara exatamente 7 metodos nativos e NAO
             * declara onActivityResult nem onWindowFocusChanged.
             * --------------------------------------------------------------- */
            #define XAM(sym) ((void *)tbl_find(xam_tbl, xam_n, (sym)))
            void *p_activate = XAM("Java_mono_android_TypeManager_n_1activate");
            void *p_act_oncreate = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1onCreate__Landroid_os_Bundle_2");
            void *p_act_onresume = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1onResume__");
            void *p_act_onpause = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1onPause__");
            void *p_act_ondestroy = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1onDestroy__");
            void *p_act_onkeydown = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1onKeyDown");
            void *p_act_dispatchkey = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1dispatchKeyEvent");
            void *p_act_dispatchmotion = XAM("Java_com_castlepixel_blossomtales_Activity1_n_1dispatchGenericMotionEvent");
            void *p_view_keydown = XAM("Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1onKeyDown");
            void *p_view_keyup = XAM("Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1onKeyUp");
            void *p_view_motion = XAM("Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1onGenericMotionEvent");
            void *p_view_touch = XAM("Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1onTouch");
            void *p_view_screated = XAM("Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1surfaceCreated");
            void *p_view_schanged = XAM("Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1surfaceChanged");
            #undef XAM

            fprintf(stderr,
                "\n=== marshal methods: activate=%p onCreate=%p onResume=%p "
                "keyDown(view)=%p motion(view)=%p surfaceCreated=%p surfaceChanged=%p ===\n",
                p_activate, p_act_oncreate, p_act_onresume, p_view_keydown,
                p_view_motion, p_view_screated, p_view_schanged);
            fprintf(stderr,
                "=== marshal methods (Activity1): pause=%p destroy=%p keyDown=%p "
                "dispatchKey=%p dispatchMotion=%p; view keyUp=%p touch=%p ===\n",
                p_act_onpause, p_act_ondestroy, p_act_onkeydown,
                p_act_dispatchkey, p_act_dispatchmotion, p_view_keyup,
                p_view_touch);

            if (!p_activate || !p_act_oncreate) {
                fprintf(stderr, "[activity] marshal methods ausentes no libxamarin-app; abortando bootstrap\n");
            } else {
                void *type_manager_class = jni_make_class("mono.android.TypeManager");
                void *main_class = jni_make_class("com.castlepixel.blossomtales.Activity1");
                (void)jni_make_class("crc64493ac3851fab1842.AndroidGameActivity");

                /* O ctor Java gerado chama TypeManager.Activate somente na
                 * classe concreta. Isso associa o jobject Activity ao objeto
                 * gerenciado; sem a ativacao, n_onCreate cai em CreateProxy.
                 * Tipo gerenciado lido do smali do ACW (const-string do ctor). */
                void *activity = jni_make_object(main_class);
                jni_set_activity(activity);
                typedef void (*activate_t)(void *, void *, void *, void *, void *, void *);
                fprintf(stderr, "=== chamando TypeManager.n_activate(this=%p) ===\n", activity);
                ((activate_t)p_activate)(env, type_manager_class,
                    (void *)"BlossomTalesAndroid.Activity1, BlossomTalesAndroid",
                    (void *)"", activity, empty_arr);
                fprintf(stderr, "=== TypeManager.n_activate RETORNOU ===\n");

                typedef void (*on_create_t)(void *, void *, void *);
                fprintf(stderr, "=== chamando Activity1.n_onCreate(this=%p, bundle=NULL) ===\n",
                        activity);
                ((on_create_t)p_act_oncreate)(env, activity, NULL);
                fprintf(stderr, "=== Activity1.n_onCreate RETORNOU ===\n");

                void *view = jni_find_object("crc64493ac3851fab1842.MonoGameAndroidGameView");
                void *holder = jni_find_object("android.view.SurfaceHolder");
                fprintf(stderr, "[surface] view=%p holder=%p\n", view, holder);
                if (view && holder && p_view_screated && p_view_schanged) {
                    typedef void (*surface_created_t)(void *, void *, void *);
                    typedef void (*surface_changed_t)(void *, void *, void *, int, int, int);
                    fprintf(stderr, "=== chamando surfaceCreated ===\n");
                    ((surface_created_t)p_view_screated)(env, view, holder);
                    int surface_width = sdv_egl_width();
                    int surface_height = sdv_egl_height();
                    fprintf(stderr, "=== chamando surfaceChanged(%dx%d) ===\n",
                            surface_width, surface_height);
                    ((surface_changed_t)p_view_schanged)(env, view, holder, 1,
                                                         surface_width,
                                                         surface_height);
                    fprintf(stderr, "=== surface callbacks RETORNARAM ===\n");
                }

                g_on_pause_handler = p_act_onpause;
                g_on_pause_env = env;

                if (p_act_onresume) {
                    typedef void (*resume_t)(void *, void *);
                    fprintf(stderr, "=== chamando Activity1.n_onResume ===\n");
                    ((resume_t)p_act_onresume)(env, activity);
                    fprintf(stderr, "=== Activity1.n_onResume RETORNOU ===\n");
                    /* Libera SyncContext.Send somente depois de View.Resume
                     * mudar o worker de Exited para Resuming. */
                    jni_set_main_looper_ready(1);
                }

                /* A Activity1 deste jogo NAO declara n_onWindowFocusChanged
                 * (o Program do scourgebringer declarava). Nada a disparar. */

                if (getenv("SB_HOLD")) {
                    fprintf(stderr, "=== SB_HOLD: mantendo processo vivo ===\n");
                    if (view && p_view_keydown && p_view_keyup) {
                        /* GPTK vivo + costura C6 já encenados antes do bootstrap
                         * gráfico (bt_input_preinit); aqui abre os pads admitidos,
                         * sela os sinks e entra no laço. Fail-closed. */
                        if (bt_input_init() != 0) {
                            fprintf(stderr, "[bt/input] controller initialization failed closed\n");
                            _exit(70);
                        }
                        bt_input_run_loop(env, view, p_view_keydown, p_view_keyup,
                                          p_view_motion, p_view_touch, &g_exit_requested);
                        sb_pause_and_exit();
                    } else
                        for (;;) pause();
                }
            }
        }
    } else if (!getenv("SB_RUNTIME_INIT")) {
        fprintf(stderr, "(set SB_RUNTIME_INIT=1 para tentar bootar o Mono)\n");
    }

    fprintf(stderr, "\n=== main: alcancado fim do bootstrap (milestone JNI_OnLoad) ===\n");
    _exit(0);
}
