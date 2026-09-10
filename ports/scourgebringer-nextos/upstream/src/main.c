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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"
#include "jni_shim.h"
#include "language_policy.h"
#include "save_migration.h"
#include "sdv_egl_bridge.h"
#include "title_menu_guard.h"
#include "nx_port_framework.h"
#include "nxgl_frame_proof_adapter.h"

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
static int g_instance_lock_fd = -1;

/* Trava no proprio processo: scripts e nomes em /proc podem mudar, mas o
 * kernel nunca concede este flock a duas instancias. Falha fechada antes de
 * SDL, Mono, framebuffer ou alocacoes grandes. O lock e liberado inclusive
 * em crash/SIGKILL quando o kernel fecha o fd. */
static int sb_acquire_instance_lock(void) {
    const char *path = "/tmp/scourgebringer.instance.lock";
    g_instance_lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_instance_lock_fd < 0) {
        fprintf(stderr, "[instance-lock] open falhou: %s; abortando\n",
                strerror(errno));
        return 0;
    }
    if (flock(g_instance_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr,
                "[instance-lock] outra instancia de scourgebringer ja esta ativa; abortando\n");
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

/* The crash path must remain async-signal-safe.  The old reporter called
 * fprintf/fopen/dladdr and blindly dereferenced frame pointers plus 8 KiB of
 * the damaged stack.  On ArkOS that reporter caught Mono's re-raised SIGBUS,
 * faulted during its own walk and emitted a misleading second SIGSEGV.
 *
 * This fixed-buffer writer runs on an alternate stack and uses only write(2)
 * and _exit(2).  Raw registers and a loader-module offset are enough to
 * symbolize a future first fault offline without risking another fault here. */
#define SB_CRASH_BUFFER_SIZE 4096
#define SB_CRASH_ALTSTACK_SIZE (64 * 1024)
static char g_crash_buffer[SB_CRASH_BUFFER_SIZE];
static unsigned char g_crash_altstack[SB_CRASH_ALTSTACK_SIZE]
    __attribute__((aligned(16)));
static volatile sig_atomic_t g_crash_in_progress;

static void crash_append_char(size_t *length, char value) {
    if (*length < SB_CRASH_BUFFER_SIZE) g_crash_buffer[(*length)++] = value;
}

static void crash_append_text(size_t *length, const char *text) {
    while (*text && *length < SB_CRASH_BUFFER_SIZE)
        g_crash_buffer[(*length)++] = *text++;
}

static void crash_append_u64(size_t *length, uint64_t value) {
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof digits);
    while (count) crash_append_char(length, digits[--count]);
}

static void crash_append_i64(size_t *length, int64_t value) {
    uint64_t magnitude;
    if (value < 0) {
        crash_append_char(length, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1;
    } else {
        magnitude = (uint64_t)value;
    }
    crash_append_u64(length, magnitude);
}

static void crash_append_hex(size_t *length, uintptr_t value) {
    static const char hex[] = "0123456789abcdef";
    char digits[sizeof(uintptr_t) * 2];
    unsigned count = 0;
    crash_append_text(length, "0x");
    do {
        digits[count++] = hex[value & 0xfu];
        value >>= 4;
    } while (value && count < sizeof digits);
    while (count) crash_append_char(length, digits[--count]);
}

static void crash_write_all(const char *buffer, size_t length) {
    while (length) {
        ssize_t written = write(STDERR_FILENO, buffer, length);
        if (written <= 0) break;
        buffer += written;
        length -= (size_t)written;
    }
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
    if (g_crash_in_progress) _exit(128 + sig);
    g_crash_in_progress = 1;

    size_t length = 0;
    uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
    crash_append_text(&length, "\n=== FIRST-FAULT sig=");
    crash_append_i64(&length, sig);
    crash_append_text(&length, " code=");
    crash_append_i64(&length, info ? info->si_code : 0);
    crash_append_text(&length, " addr=");
    crash_append_hex(&length, fault);
#if defined(__aarch64__)
    ucontext_t *u = (ucontext_t *)uc;
    if (u) {
        uintptr_t pc = u->uc_mcontext.pc;
        crash_append_text(&length, " pc=");
        crash_append_hex(&length, pc);
        crash_append_text(&length, " lr=");
        crash_append_hex(&length, u->uc_mcontext.regs[30]);
        crash_append_text(&length, " sp=");
        crash_append_hex(&length, u->uc_mcontext.sp);
        crash_append_text(&length, " fp=");
        crash_append_hex(&length, u->uc_mcontext.regs[29]);

        const struct sb_module *module = sb_module_for(pc);
        if (module) {
            crash_append_text(&length, " guest_module=");
            crash_append_u64(&length, (uint64_t)(module - g_modules));
            crash_append_text(&length, "+");
            crash_append_hex(&length, pc - module->base);
        }

        crash_append_char(&length, '\n');
        for (int index = 0; index < 31; ++index) {
            crash_append_char(&length, 'x');
            crash_append_u64(&length, (uint64_t)index);
            crash_append_char(&length, '=');
            crash_append_hex(&length, u->uc_mcontext.regs[index]);
            crash_append_char(&length, (index % 4 == 3) ? '\n' : ' ');
        }
    }
#endif
    crash_append_text(&length, "=== end first-fault ===\n");
    crash_write_all(g_crash_buffer, length);
    _exit(128 + sig);
}
static void install_crash_handler(void) {
    stack_t alternate;
    memset(&alternate, 0, sizeof alternate);
    alternate.ss_sp = g_crash_altstack;
    alternate.ss_size = sizeof g_crash_altstack;
    if (sigaltstack(&alternate, NULL) != 0)
        fprintf(stderr, "[crash-handler] sigaltstack unavailable: %s\n",
                strerror(errno));

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sigfillset(&sa.sa_mask);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
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
    /* ParisEngine e a engine do TMNT; o ScourgeBringer usa MonoGame puro e nao
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

typedef unsigned char (*sdv_key_callback_t)(void *, void *, int, void *);
typedef unsigned char (*sdv_touch_callback_t)(void *, void *, void *, void *);
typedef unsigned char (*sdv_motion_callback_t)(void *, void *, void *);

static int sdv_input_trace(void) {
    static int initialized;
    static int enabled;

    if (!initialized) {
        const char *value = getenv("SB_INPUT_TRACE");
        enabled = value && value[0] && value[0] != '0';
        initialized = 1;
    }
    return enabled;
}

typedef struct SdvTitleStateSnapshot {
    int state;
    int selected;
    int menu_flags;
    void *instance;
    void *selected_field;
} SdvTitleStateSnapshot;

/* A versao Android conserva uma entrada Discord na logica do menu, mas nao a
 * desenha. A sonda le somente o estado privado dessa tela; o reparo abaixo
 * nunca altera o estado do controle. */
static int sdv_get_title_state(SdvTitleStateSnapshot *snapshot) {
    static pthread_mutex_t inspect_lock = PTHREAD_MUTEX_INITIALIZER;
    static int resolved;
    static int unavailable;
    static void *klass;
    static void *f_instance;
    static void *f_state;
    static void *f_selected;
    static void *f_options;
    static void *f_credits;
    static void *f_exit;
    static void *f_outdated;
    static void *f_difficult;
    typedef void *(*fn_void)(void);
    typedef void *(*fn_str)(const char *);
    typedef void *(*fn_ptr_str_str)(void *, const char *, const char *);
    typedef void *(*fn_ptr_str)(void *, const char *);
    typedef void *(*fn_ptr_ptr)(void *, void *);
    typedef void (*fn_ptr_ptr_ptr)(void *, void *, void *);
    static fn_void domain_get;
    static fn_str image_loaded;
    static fn_ptr_str_str class_from_name;
    static fn_ptr_str field_from_name;
    static fn_ptr_ptr class_vtable;
    static fn_ptr_ptr_ptr static_get;
    static fn_ptr_ptr_ptr field_get;
    void *image;
    void *vtable;
    void *instance = NULL;
    int state = -1;
    int selected = -1;
    unsigned char in_options = 0;
    unsigned char in_credits = 0;
    unsigned char in_exit = 0;
    unsigned char is_outdated = 0;
    unsigned char is_difficult = 0;
    int result = 0;

    if (!snapshot || unavailable) return 0;
    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&inspect_lock);
    if (!resolved) {
        domain_get = (fn_void)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_domain_get");
        image_loaded = (fn_str)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_image_loaded");
        class_from_name = (fn_ptr_str_str)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_class_from_name");
        field_from_name = (fn_ptr_str)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_class_get_field_from_name");
        class_vtable = (fn_ptr_ptr)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_class_vtable");
        static_get = (fn_ptr_ptr_ptr)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_field_static_get_value");
        field_get = (fn_ptr_ptr_ptr)tbl_find(
            g_resolv_tbl, g_resolv_n, "mono_field_get_value");
        if (!domain_get || !image_loaded || !class_from_name ||
            !field_from_name || !class_vtable || !static_get || !field_get) {
            fprintf(stderr, "[title-menu] embedding API incompleta\n");
            unavailable = 1;
            goto out;
        }
        resolved = 1;
    }
    if (!klass) {
        image = image_loaded("ScourgeBringer");
        if (!image) goto out;
        klass = class_from_name(image, "ScourgeBringer", "TitleScreenManager");
        if (!klass) {
            fprintf(stderr, "[title-menu] TitleScreenManager ausente\n");
            unavailable = 1;
            goto out;
        }
        f_instance = field_from_name(klass, "_instance");
        f_state = field_from_name(klass, "_state");
        f_selected = field_from_name(klass, "_selectedMenuItem");
        f_options = field_from_name(klass, "_inOptions");
        f_credits = field_from_name(klass, "_inCredits");
        f_exit = field_from_name(klass, "_inExit");
        f_outdated = field_from_name(klass, "_isOutdated");
        f_difficult = field_from_name(klass, "_isDifficult");
        if (!f_instance || !f_state || !f_selected || !f_options ||
            !f_credits || !f_exit || !f_outdated || !f_difficult) {
            fprintf(stderr, "[title-menu] campos esperados ausentes\n");
            unavailable = 1;
            goto out;
        }
    }
    vtable = class_vtable(domain_get(), klass);
    if (!vtable) goto out;
    static_get(vtable, f_instance, &instance);
    if (!instance) goto out;
    field_get(instance, f_state, &state);
    field_get(instance, f_selected, &selected);
    field_get(instance, f_options, &in_options);
    field_get(instance, f_credits, &in_credits);
    field_get(instance, f_exit, &in_exit);
    field_get(instance, f_outdated, &is_outdated);
    field_get(instance, f_difficult, &is_difficult);
    snapshot->state = state;
    snapshot->selected = selected;
    snapshot->menu_flags = (in_options ? 1 : 0) | (in_credits ? 2 : 0) |
        (in_exit ? 4 : 0) | (is_outdated ? 8 : 0) |
        (is_difficult ? 16 : 0);
    snapshot->instance = instance;
    snapshot->selected_field = f_selected;
    result = 1;
out:
    pthread_mutex_unlock(&inspect_lock);
    return result;
}

/* InputRight seleciona MainMenuInput.Discord em todas as entradas visiveis,
 * embora o Draw Android nao desenhe Discord. Em vez de filtrar D-pad/analogico
 * (o TitleScreenManager conserva estado obsoleto durante gameplay), revertemos
 * exclusivamente esse campo privado para a ultima entrada que foi visivel.
 * Assim todos os eventos do controle chegam intactos ao jogo e aos submenus. */
void sdv_repair_hidden_title_selection(void) {
    static int have_last_visible;
    static int last_visible = SB_TITLE_MENU_NEW_GAME;
    static int missing_setter_logged;
    typedef void (*fn_ptr_ptr_ptr)(void *, void *, void *);
    SdvTitleStateSnapshot snapshot;
    int replacement;
    fn_ptr_ptr_ptr field_set;

    if (!sdv_get_title_state(&snapshot)) return;
    if (snapshot.state == SB_TITLE_STATE_MAIN_MENU &&
        snapshot.menu_flags == 0 &&
        sb_title_menu_visible_selection(snapshot.selected)) {
        last_visible = snapshot.selected;
        have_last_visible = 1;
        return;
    }
    if (!sb_title_menu_repair_target(
            snapshot.state, snapshot.menu_flags, snapshot.selected,
            have_last_visible, last_visible, &replacement))
        return;
    field_set = (fn_ptr_ptr_ptr)tbl_find(
        g_resolv_tbl, g_resolv_n, "mono_field_set_value");
    if (!field_set) {
        if (!missing_setter_logged) {
            fprintf(stderr, "[title-menu] mono_field_set_value ausente\n");
            missing_setter_logged = 1;
        }
        return;
    }
    field_set(snapshot.instance, snapshot.selected_field, &replacement);
    last_visible = replacement;
    have_last_visible = 1;
    fprintf(stderr,
            "[title-menu] selecao Discord invisivel restaurada para %d; "
            "entrada do controle preservada\n",
            replacement);
}

/* Sonda opt-in usada pelos testes fisicos; nao altera estado nem entrada. */
void sdv_trace_title_state(void) {
    static int initialized;
    static int enabled;
    static int have_previous;
    static SdvTitleStateSnapshot previous;
    SdvTitleStateSnapshot snapshot;

    if (!initialized) {
        const char *value = getenv("SB_TITLE_TRACE");
        enabled = value && value[0] && value[0] != '0';
        initialized = 1;
    }
    if (!enabled || !sdv_get_title_state(&snapshot)) return;
    if (!have_previous || snapshot.state != previous.state ||
        snapshot.selected != previous.selected ||
        snapshot.menu_flags != previous.menu_flags) {
        fprintf(stderr,
                "[title-trace] state=%d selected=%d flags=0x%x\n",
                snapshot.state, snapshot.selected, snapshot.menu_flags);
        previous = snapshot;
        have_previous = 1;
    }
}

static double sdv_monotonic_seconds(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void sdv_send_key(void *env, void *view, sdv_key_callback_t callback,
                         void *event, int keycode, int device_id,
                         const char *edge) {
    if (!callback || !view || !event) return;
    jni_set_key_event_keycode(event, keycode, device_id);
    unsigned char handled = callback(env, view, keycode, event);
    if (sdv_input_trace())
        fprintf(stderr,
                "[sdv-input] P%d key %s android=%d handled=%u\n",
                device_id, edge, keycode, handled);
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

static float sdv_normalize_stick(short value) {
    float result = value < 0 ? (float)value / 32768.0f
                             : (float)value / 32767.0f;
    if (result < -1.0f) result = -1.0f;
    if (result > 1.0f) result = 1.0f;
    return result;
}

static float sdv_normalize_trigger(short value) {
    if (value <= 0) return 0.0f;
    float result = (float)value / 32767.0f;
    return result > 1.0f ? 1.0f : result;
}

static void sdv_send_gamepad_motion(void *env, void *view,
                                    sdv_motion_callback_t callback,
                                    void *event, int device_id,
                                    const SdvGamepadState *pad) {
    if (!callback || !view || !event || !pad) return;
    float hat_x = 0.0f, hat_y = 0.0f;
    if (pad->buttons & (1u << 13)) hat_x = -1.0f;
    if (pad->buttons & (1u << 14)) hat_x = 1.0f;
    if (pad->buttons & (1u << 11)) hat_y = -1.0f;
    if (pad->buttons & (1u << 12)) hat_y = 1.0f;
    jni_set_gamepad_motion_event(
        event, device_id, sdv_normalize_stick(pad->left_x),
        sdv_normalize_stick(pad->left_y),
        sdv_normalize_stick(pad->right_x),
        sdv_normalize_stick(pad->right_y),
        sdv_normalize_trigger(pad->left_trigger),
        sdv_normalize_trigger(pad->right_trigger), hat_x, hat_y);
    unsigned char handled = callback(env, view, event);
    if (sdv_input_trace())
        fprintf(stderr,
                "[sdv-input] P%d native-motion lx=%d ly=%d rx=%d ry=%d lt=%d rt=%d handled=%u\n",
                device_id, pad->left_x, pad->left_y, pad->right_x,
                pad->right_y, pad->left_trigger, pad->right_trigger,
                handled);
}

static void sdv_run_input_loop(void *env, void *view, void *down_handler,
                               void *up_handler, void *motion_handler,
                               void *touch_handler) {
    enum {
        SB_KEY_UP = 1u << 0,
        SB_KEY_DOWN = 1u << 1,
        SB_KEY_LEFT = 1u << 2,
        SB_KEY_RIGHT = 1u << 3,
        SB_KEY_ACTION = 1u << 4,
        SB_KEY_CANCEL = 1u << 5,
        SB_KEY_TOOL = 1u << 6,
        SB_KEY_MENU = 1u << 7,
        SB_KEY_ESCAPE = 1u << 8,
        SB_KEY_PREV = 1u << 9,
        SB_KEY_NEXT = 1u << 10,
        SB_KEY_JOURNAL = 1u << 11,
        SB_KEY_MAP = 1u << 12,
        SB_KEY_START = 1u << 13
    };
    static const struct {
        unsigned int bit;
        int keycode;
    } keys[] = {
        {SB_KEY_UP, 19},       /* Android DPAD_UP */
        {SB_KEY_DOWN, 20},     /* Android DPAD_DOWN */
        {SB_KEY_LEFT, 21},     /* Android DPAD_LEFT */
        {SB_KEY_RIGHT, 22},    /* Android DPAD_RIGHT */
        {SB_KEY_ACTION, 96},   /* Android BUTTON_A */
        {SB_KEY_CANCEL, 97},   /* Android BUTTON_B */
        {SB_KEY_TOOL, 99},     /* Android BUTTON_X */
        {SB_KEY_MENU, 100},    /* Android BUTTON_Y */
        {SB_KEY_ESCAPE, 4},    /* Android BACK */
        {SB_KEY_PREV, 102},    /* Android BUTTON_L1 */
        {SB_KEY_NEXT, 103},    /* Android BUTTON_R1 */
        {SB_KEY_JOURNAL, 106}, /* Android BUTTON_THUMBL */
        {SB_KEY_MAP, 107},     /* Android BUTTON_THUMBR */
        {SB_KEY_START, 108}    /* Android BUTTON_START */
    };
    sdv_key_callback_t key_down = (sdv_key_callback_t)down_handler;
    sdv_key_callback_t key_up = (sdv_key_callback_t)up_handler;
    sdv_motion_callback_t generic_motion =
        (sdv_motion_callback_t)motion_handler;
    sdv_touch_callback_t touch = (sdv_touch_callback_t)touch_handler;
    void *key_events[4] = {NULL, NULL, NULL, NULL};
    void *gamepad_events[4] = {NULL, NULL, NULL, NULL};
    void *motion_event = touch
        ? jni_make_object(jni_make_class("android.view.MotionEvent")) : NULL;
    unsigned int previous[4] = {0, 0, 0, 0};
    SdvGamepadState previous_pads[4];
    unsigned char have_previous_pad[4] = {0, 0, 0, 0};
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
    const char *cursor_env = getenv("SB_RIGHT_CURSOR");
    int right_cursor_enabled = touch &&
        !(cursor_env && cursor_env[0] == '0');
    int test_key = getenv("SB_TEST_KEY") ? atoi(getenv("SB_TEST_KEY")) : 0;
    unsigned long test_at = getenv("SB_TEST_KEY_DELAY")
        ? strtoul(getenv("SB_TEST_KEY_DELAY"), NULL, 10) * 125ul : 0;
    const char *command_path = getenv("SB_INPUT_COMMANDS");

    memset(previous_pads, 0, sizeof previous_pads);
    for (int player = 0; player < 4; ++player) {
        key_events[player] =
            jni_make_object(jni_make_class("android.view.KeyEvent"));
        if (generic_motion)
            gamepad_events[player] = jni_make_object(
                jni_make_class("android.view.MotionEvent"));
    }

    sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
    fprintf(stderr,
            "[sdv-input] loop ativo; cursor direito=%s (R3=clique, timeout=2s)\n",
            right_cursor_enabled ? "ativo" : "inativo");
    signal(SIGTERM, exit_request_handler);
    signal(SIGINT, exit_request_handler);
    while (!g_exit_requested) {
        SdvGamepadState pads[4];
        unsigned int logical[4] = {0, 0, 0, 0};
        double now = sdv_monotonic_seconds();
        double elapsed = now - last_tick;
        int pad_mask;
        int p1_connected;
        int r3_down = 0;

        if (elapsed < 0.0 || elapsed > 0.05) elapsed = 0.05;
        last_tick = now;
        if (jni_activity_finish_requested()) {
            fprintf(stderr,
                    "[sdv-input] Activity.finish observado -> encerrando loop\n");
            g_exit_requested = 1;
            break;
        }
        pad_mask = sdv_egl_poll_gamepads(pads, 4);
        if (pad_mask < 0) {
            fprintf(stderr, "[sdv-input] SDL_QUIT\n");
            break;
        }
        if (nx_port_framework_poll_input()) {
            g_exit_requested = 1;
            break;
        }

#define SB_PAD_BUTTON(p, n) \
        ((pads[(p)].buttons & (1u << (n))) != 0)
        /* O primeiro MotionEvent, mesmo todo zerado, registra cada dispositivo
         * no MonoGame em ordem P1..P4 antes de qualquer tecla de JOIN. */
        for (int player = 0; player < 4; ++player) {
            if (!(pad_mask & (1u << player))) {
                have_previous_pad[player] = 0;
                continue;
            }
            if (!have_previous_pad[player] ||
                memcmp(&pads[player], &previous_pads[player],
                       sizeof pads[player]) != 0) {
                sdv_send_gamepad_motion(
                    env, view, generic_motion, gamepad_events[player],
                    player + 1, &pads[player]);
                previous_pads[player] = pads[player];
                have_previous_pad[player] = 1;
            }
            if (SB_PAD_BUTTON(player, 4) &&
                SB_PAD_BUTTON(player, 6)) {
                fprintf(stderr,
                        "[sdv-input] P%d SELECT+START -> saindo do jogo\n",
                        player + 1);
                g_exit_requested = 1;
                break;
            }
        }
        if (g_exit_requested) break;

        /* O cursor por analogico direito pertence somente ao P1. P2..P4
         * recebem R3 nativo, sem qualquer interceptacao por touch. */
        p1_connected = (pad_mask & 1) != 0;
        if (p1_connected) r3_down = SB_PAD_BUTTON(0, 8);
        if (p1_connected && right_cursor_enabled) {
            const float stick_max = 32767.0f;
            const float cursor_deadzone = 7500.0f;
            float rx = (float)pads[0].right_x;
            float ry = (float)pads[0].right_y;
            float magnitude = sqrtf(rx * rx + ry * ry);

            if (magnitude > cursor_deadzone) {
                float amount = (magnitude - cursor_deadzone) /
                    (stick_max - cursor_deadzone);
                float speed;
                if (amount > 1.0f) amount = 1.0f;
                speed = 120.0f * amount + 1000.0f * amount * amount;
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
                sdv_send_touch(env, view, touch, motion_event,
                               p1_connected ? 1 : 3, cursor_x, cursor_y);
                cursor_touch_down = 0;
                last_cursor_activity = now;
            }
            r3_mode = 0;
        }
        r3_was_down = r3_down;

        for (int player = 0; player < 4; ++player) {
            if (!(pad_mask & (1u << player))) continue;
            if (SB_PAD_BUTTON(player, 11)) logical[player] |= SB_KEY_UP;
            if (SB_PAD_BUTTON(player, 12)) logical[player] |= SB_KEY_DOWN;
            if (SB_PAD_BUTTON(player, 13)) logical[player] |= SB_KEY_LEFT;
            if (SB_PAD_BUTTON(player, 14)) logical[player] |= SB_KEY_RIGHT;
            if (SB_PAD_BUTTON(player, 0)) logical[player] |= SB_KEY_ACTION;
            if (SB_PAD_BUTTON(player, 1)) logical[player] |= SB_KEY_CANCEL;
            if (SB_PAD_BUTTON(player, 2)) logical[player] |= SB_KEY_TOOL;
            if (SB_PAD_BUTTON(player, 3)) logical[player] |= SB_KEY_MENU;
            if (SB_PAD_BUTTON(player, 6)) logical[player] |= SB_KEY_START;
            if (SB_PAD_BUTTON(player, 4)) logical[player] |= SB_KEY_ESCAPE;
            if (SB_PAD_BUTTON(player, 9)) logical[player] |= SB_KEY_PREV;
            if (SB_PAD_BUTTON(player, 10)) logical[player] |= SB_KEY_NEXT;
            if (SB_PAD_BUTTON(player, 7)) logical[player] |= SB_KEY_JOURNAL;
            if (player == 0) {
                if ((!right_cursor_enabled && r3_down) || r3_mode == 1)
                    logical[player] |= SB_KEY_MAP;
            } else if (SB_PAD_BUTTON(player, 8)) {
                logical[player] |= SB_KEY_MAP;
            }
        }
#undef SB_PAD_BUTTON

        if (right_cursor_enabled && cursor_visible && !cursor_touch_down &&
            now - last_cursor_activity >= 2.0) {
            cursor_visible = 0;
            sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
        }
        for (int player = 0; player < 4; ++player) {
            unsigned int changed = logical[player] ^ previous[player];
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
                if (!(changed & keys[i].bit)) continue;
                if (logical[player] & keys[i].bit)
                    sdv_send_key(env, view, key_down, key_events[player],
                                 keys[i].keycode, player + 1, "down");
                else
                    sdv_send_key(env, view, key_up, key_events[player],
                                 keys[i].keycode, player + 1, "up");
            }
            previous[player] = logical[player];
        }

        /* Canal de teste opt-in: permite atravessar menus remotamente sem
         * fingir um teclado Linux. "press <keycode>" mira P1; press2..press4
         * miram os demais IDs nativos. A tecla fica pressionada por 120 ms,
         * tempo suficiente para o game loop observar a borda. */
        if (command_path && command_path[0] && ticks % 25ul == 0) {
            FILE *commands = fopen(command_path, "r");
            if (commands) {
                unlink(command_path);
                char line[64];
                while (fgets(line, sizeof line, commands)) {
                    char action[16];
                    int keycode = -1;
                    int hold_ms = 120;   /* o bastante p/ o loop ver a borda */
                    int fields = sscanf(line, "%15s %d %d", action, &keycode,
                                        &hold_ms);
                    int device_id = 1;
                    if (fields < 2 || keycode < 0) continue;
                    if (strncmp(action, "press", 5) != 0) continue;
                    if (action[5] != '\0') {
                        if (action[5] < '1' || action[5] > '4' ||
                            action[6] != '\0')
                            continue;
                        device_id = action[5] - '0';
                    }
                    if (hold_ms < 1) hold_ms = 1;
                    if (hold_ms > 10000) hold_ms = 10000;
                    sdv_send_key(env, view, key_down,
                                 key_events[device_id - 1], keycode,
                                 device_id, "remote-down");
                    usleep((useconds_t)hold_ms * 1000u);
                    sdv_send_key(env, view, key_up,
                                 key_events[device_id - 1], keycode,
                                 device_id, "remote-up");
                }
                fclose(commands);
            }
        }

        if (test_key && test_at && ticks == test_at)
            sdv_send_key(env, view, key_down, key_events[0], test_key, 1,
                         "test-down");
        if (test_key && test_at && ticks == test_at + 25)
            sdv_send_key(env, view, key_up, key_events[0], test_key, 1,
                         "test-up");
        ++ticks;
        usleep(8000);
    }
    if (cursor_touch_down)
        sdv_send_touch(env, view, touch, motion_event, 3,
                       cursor_x, cursor_y);
    sdv_egl_set_right_cursor(cursor_x, cursor_y, 0);
    fprintf(stderr, "[sdv-input] encerramento solicitado\n");
}

static void sdv_finish_activity_lifecycle(void *env, void *activity)
{
    static const struct {
        const char *name;
        const char *label;
    } callbacks[] = {
        {"n_onPause", "onPause"},
        {"n_onStop", "onStop"},
        {"n_onDestroy", "onDestroy"},
    };

    if (!jni_activity_finish_requested() || !activity)
        return;
    fprintf(stderr,
            "[activity] executando lifecycle nativo de Activity.finish\n");
    for (size_t index = 0;
         index < sizeof callbacks / sizeof callbacks[0]; ++index) {
        void *callback = jni_find_registered_native(callbacks[index].name,
                                                    "()V");
        if (!callback) {
            fprintf(stderr, "[activity] %s nao registrado\n",
                    callbacks[index].label);
            continue;
        }
        fprintf(stderr, "=== chamando MainActivity.%s ===\n",
                callbacks[index].label);
        ((void (*)(void *, void *))callback)(env, activity);
        fprintf(stderr, "=== MainActivity.%s RETORNOU ===\n",
                callbacks[index].label);
    }
    jni_set_main_looper_ready(0);
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
 * do APK ficam em SB_LIBDIR (ex.: $GAMEDIR/runtime-libs). Resolve o nome curto
 * contra o CWD e depois contra SB_LIBDIR. */
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

static int scourgebringer_runtime_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    { volatile char c = g_bionic_guard_pad[0]; (void)c; }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!sb_acquire_instance_lock()) return 73;
    install_crash_handler();

    fprintf(stderr, "=== ScourgeBringer (MonoGame/.NET Android) Mono so-loader / NextOS aarch64 ===\n");
    preload_device_libs();

    /* A janela/contexto precisam nascer na main thread. O contexto e liberado
     * aqui e adquirido depois pela worker do MonoGame via o fake EGL10. */
    if (getenv("SB_START_ACTIVITY")) {
        if (!sdv_egl_init()) {
            fprintf(stderr, "[sdv-egl] bootstrap grafico indisponivel\n");
            return 1;
        }
        if (nx_port_framework_input_ready() != 0) {
            fprintf(stderr, "[nxfw] bootstrap de input indisponivel\n");
            return 1;
        }
    }

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
            fprintf(stderr, "=== chamando mono_jit_init_version(\"ScourgeBringer.Android\",\"v4.0.30319\") ===\n");
            void *domain = jit("ScourgeBringer.Android", "v4.0.30319");
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
        /* Assinatura REAL desta build (derivada do disassembly do wrapper
         * Java_mono_android_Runtime_init @0x263a4, que chama
         * MonodroidRuntime::Java_mono_android_Runtime_initInternal
         *   (JNIEnv*, jclass, jstring, jobjectArray, jstring, jobjectArray,
         *    int localDateTimeOffset, jobject, jobjectArray, int apiLevel,
         *    uchar isEmulator, uchar haveSplitApks)):
         *
         *   p0 env        -> x0        p6  loader        -> x6
         *   p1 klass      -> x1        p7  (ignorado)    -> x7
         *   p2 lang       -> x2        p8  assemblies    -> stack[0]
         *   p3 apks       -> x3        p9  (ignorado)    -> stack[1]
         *   p4 nativeLib  -> x4        p10 apiLevel      -> stack[2]
         *   p5 appDirs    -> x5
         *
         * localDateTimeOffset/isEmulator/haveSplitApks sao zerados pelo proprio
         * wrapper. Strings via strdup (GetStringUTFChars trata); arrays vazios
         * usam o token 0x4000 (GetArrayLength devolve 0). */
        void *env = jni_env_ptr();
        void *klass = (void *)0xC1A500;
        const struct sb_language_profile *language = sb_language_current();
        void *jstr_language = strdup(language->runtime_locale);
        void *empty_arr = (void *)0x4000;
        /* appDirs: no Android real vem do Context (dataDir/cacheDir/
         * nativeLibraryDir). Passamos nosso dir (token 0x4001) para que o Mono
         * (a) propague env pro wrapper e (b) ache assemblies/AOT/DSO em vez de
         * procurar em "(null)". */
        const char *libdir = getenv("SB_LIBDIR");
        const char *files_dir = getenv("SB_DATA_DIR");
        char cache_dir[2048];
        if (!libdir || !*libdir) libdir = "runtime-libs";
        if (!files_dir || !*files_dir) files_dir = "data";
        if (snprintf(cache_dir, sizeof cache_dir, "%s/cache", files_dir) >=
            (int)sizeof cache_dir) {
            fprintf(stderr, "[save-path] cacheDir excede limite\n");
            return 1;
        }
        jni_set_app_dirs(files_dir, cache_dir, libdir);
        void *appdirs = (void *)0x4001;      /* APPDIRS_TOKEN */
        void *jstr_libdir = strdup(libdir);
        /* runtimeApks: 1.61.16 usa assemblies/assemblies.blob; b19 usa
         * lib/arm64-v8a/libassemblies.arm64-v8a.blob.so. O NXExtract preserva
         * o formato detectado num APK reduzido, STORED e alinhado. */
        const char *apk = getenv("SB_APK");
        if (!apk || !*apk) apk = "assemblies.apk";
        jni_set_apk_path(apk);
        void *apks = (void *)0x4002;         /* APKS_TOKEN */
        typedef void (*runtime_init_t)(void *, void *, void *, void *, void *,
                                       void *, void *, void *, void *, void *,
                                       long);
        runtime_init_t rt = (runtime_init_t)p_rtinit;
        fprintf(stderr,
                "Runtime_init(11 args): env=%p klass=%p language=%s "
                "runtime_locale=%s content=%s files=%s cache=%s libdir=%s\n",
                env, klass, language->nx_code, language->runtime_locale,
                language->content_code, files_dir, cache_dir, libdir);
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
        rt(env, klass, jstr_language, apks, jstr_libdir, appdirs,
           (void *)0xC1A501, NULL, empty_arr, NULL, 34);
        fprintf(stderr, "=== Runtime_init RETORNOU ===\n");

        /* Sonda M2: mono_thread_create isolado (SB_THREAD_TEST=1) */
        sb_thread_test();

        /* A JVM normalmente executa os <clinit> das classes Java geradas pelo
         * Xamarin, que chamam Runtime.register(), e depois despacha
         * MainActivity.onCreate() ao handler n_onCreate registrado. Sem JVM,
         * reproduzimos somente essa sequencia. O DEX do APK 1.6.15.3 confirma
         * os nomes/assinaturas abaixo. Opt-in para preservar o milestone puro. */
        if (getenv("SB_START_ACTIVITY")) {
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
                /* Lista EXATA do ACW crc645d6a1e7bece73b70.Program, lida do
                 * dex. Um unico metodo a mais (ex.: onWindowFocusChanged, que
                 * o Program nao sobrescreve) faz o TypeManager nao achar o
                 * getter do handler e derruba o registro INTEIRO — foi assim
                 * que n_onActivityResult, a porta do modo offline do Google
                 * Play, ficou sem handler. */
                static const char main_methods[] =
                    "n_onCreate:(Landroid/os/Bundle;)V:GetOnCreate_Landroid_os_Bundle_Handler\n"
                    "n_onResume:()V:GetOnResumeHandler\n"
                    "n_onDestroy:()V:GetOnDestroyHandler\n"
                    "n_onStart:()V:GetOnStartHandler\n"
                    "n_onStop:()V:GetOnStopHandler\n"
                    "n_onPause:()V:GetOnPauseHandler\n"
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
                void *main_class = jni_make_class("crc645d6a1e7bece73b70.Program");
                void *game_view_class = jni_make_class("crc64493ac3851fab1842.MonoGameAndroidGameView");

                fprintf(stderr, "\n=== SB_START_ACTIVITY: Runtime.register(TypeManager) ===\n");
                reg(env, runtime_class,
                    (void *)"Java.Interop.TypeManager+JavaTypeManager, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null",
                    type_manager_class, (void *)type_manager_methods);
                fprintf(stderr, "\n=== SB_START_ACTIVITY: Runtime.register(base) ===\n");
                reg(env, runtime_class,
                    (void *)"Microsoft.Xna.Framework.AndroidGameActivity, MonoGame.Framework",
                    base_class, (void *)base_methods);
                fprintf(stderr, "=== SB_START_ACTIVITY: Runtime.register(MainActivity) ===\n");
                reg(env, runtime_class,
                    /* Activity gerenciada = ScourgeBringer.Program (extends AndroidGameActivity);
                     * ACW crc645d6a1e7bece73b70.Program, confirmado no AndroidManifest/dex. */
                    (void *)"ScourgeBringer.Program, ScourgeBringer",
                    main_class, (void *)main_methods);
                fprintf(stderr, "=== SB_START_ACTIVITY: Runtime.register(GameView) ===\n");
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
                        /* Activity gerenciada = ScourgeBringer.Program (extends AndroidGameActivity);
                     * ACW crc645d6a1e7bece73b70.Program, confirmado no AndroidManifest/dex. */
                    (void *)"ScourgeBringer.Program, ScourgeBringer",
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

                    /* MonoGame AndroidGameView frequentemente so inicia/libera o
                     * loop de render quando a janela ganha foco. Sem view system
                     * Android real, disparamos manualmente. */
                    void *focus = jni_find_registered_native(
                        "n_onWindowFocusChanged", "(Z)V");
                    if (focus) {
                        typedef void (*focus_t)(void *, void *, unsigned char);
                        fprintf(stderr, "=== chamando MainActivity.n_onWindowFocusChanged(true) ===\n");
                        ((focus_t)focus)(env, activity, 1);
                        fprintf(stderr, "=== onWindowFocusChanged RETORNOU ===\n");
                    }

                    if (getenv("SB_HOLD")) {
                        fprintf(stderr, "=== SB_HOLD: mantendo processo vivo ===\n");
                        void *key_down = jni_find_registered_native(
                            "n_onKeyDown", "(ILandroid/view/KeyEvent;)Z");
                        void *key_up = jni_find_registered_native(
                            "n_onKeyUp", "(ILandroid/view/KeyEvent;)Z");
                        void *motion = jni_find_registered_native(
                            "n_onGenericMotionEvent",
                            "(Landroid/view/MotionEvent;)Z");
                        void *touch = jni_find_registered_native(
                            "n_onTouch",
                            "(Landroid/view/View;Landroid/view/MotionEvent;)Z");
                        if (view && key_down && key_up)
                            sdv_run_input_loop(env, view, key_down, key_up,
                                               motion, touch);
                        else
                            for (;;) pause();
                        sdv_finish_activity_lifecycle(env, activity);
                    }
                }
            }
        }
    } else if (!getenv("SB_RUNTIME_INIT")) {
        fprintf(stderr, "(set SB_RUNTIME_INIT=1 para tentar bootar o Mono)\n");
    }

    fprintf(stderr, "\n=== main: alcancado fim do bootstrap (milestone JNI_OnLoad) ===\n");
    _exit(0);
}

static void sb_set_default_path(const char *name, const char *game_dir,
                                const char *leaf)
{
    char path[2048];

    if (getenv(name)) return;
    if (snprintf(path, sizeof(path), "%s/%s", game_dir, leaf) >=
        (int)sizeof(path))
        return;
    setenv(name, path, 0);
}

static void scourgebringer_runtime_defaults(void)
{
    char cwd[1536];
    char canonical_data_dir[2048];
    const char *game_dir = getenv("NXCOMPAT_GAME_DIR");
    const char *data_dir;

    if (!game_dir || game_dir[0] != '/') {
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
        game_dir = cwd;
    }
    if (!getenv("MALLOC_ARENA_MAX")) setenv("MALLOC_ARENA_MAX", "2", 0);
    if (!getenv("SB_RUNTIME_INIT")) setenv("SB_RUNTIME_INIT", "1", 0);
    if (!getenv("SB_START_ACTIVITY")) setenv("SB_START_ACTIVITY", "1", 0);
    if (!getenv("SB_HOLD")) setenv("SB_HOLD", "1", 0);
    if (!getenv("SB_PRESENT_FBO")) setenv("SB_PRESENT_FBO", "1", 0);
    if (!getenv("SB_FBO_TRACK")) setenv("SB_FBO_TRACK", "1", 0);
    if (!getenv("SB_RIGHT_CURSOR")) setenv("SB_RIGHT_CURSOR", "0", 0);
    if (!getenv("SB_JNI_VERBOSE")) setenv("SB_JNI_VERBOSE", "0", 0);
    sb_set_default_path("SB_LIBDIR", game_dir, "runtime-libs");
    sb_set_default_path("SB_ASSET_DIR", game_dir, "assets");
    sb_set_default_path("SB_APK", game_dir, "assemblies.apk");
    sb_set_default_path("SB_DATA_DIR", game_dir, "data");
    data_dir = getenv("SB_DATA_DIR");
    if (!data_dir || !data_dir[0] ||
        snprintf(canonical_data_dir, sizeof canonical_data_dir, "%s/data",
                 game_dir) >= (int)sizeof canonical_data_dir) {
        fprintf(stderr, "[save-path] preparacao/migracao falhou\n");
    } else if (strcmp(data_dir, canonical_data_dir) == 0) {
        if (!sb_migrate_legacy_saves(game_dir, data_dir))
            fprintf(stderr, "[save-path] preparacao/migracao falhou\n");
    } else if (!sb_prepare_data_dirs(data_dir)) {
        fprintf(stderr, "[save-path] diretorio alternativo invalido\n");
    } else {
        fprintf(stderr,
                "[save-path] data alternativo=%s; migracao legada ignorada\n",
                data_dir);
    }
}

int main(int argc, char **argv)
{
  /* Emitted before anything can fail: the launch context is what a reader
   * needs to know whether a startup failure says anything about the port. */
  nxgl_frame_proof_launch_receipt();
    scourgebringer_runtime_defaults();
    return nx_port_framework_run("scourgebringer", argc, argv,
                                 scourgebringer_runtime_main);
}
