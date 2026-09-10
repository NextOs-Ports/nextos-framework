/*
 * fmod_shim.c -- ponte entre o FMOD do APK e o audio do NextOS.
 *
 * O BlossomTales usa FMOD Studio para TODO o som. No Android o FMOD abre a
 * saida por AAudio (`libaaudio.so`) ou OpenSL ES (`libOpenSLES.so`); nenhum dos
 * dois existe aqui. Sem saida, `Studio::System::initialize` devolve
 * ERR_INTERNAL, `SoundHelper.Initialize()` lanca e o jogo morre antes do
 * primeiro frame.
 *
 * `aaudio_shim.c` fornece um `libaaudio.so` de verdade (backend SDL), entao o
 * caminho normal do FMOD funciona. Este arquivo cuida do contorno: logo apos o
 * Create escolhemos a saida e, se mesmo assim o init falhar, forcamos NOSOUND
 * para o jogo continuar jogavel em vez de morrer.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FMOD_OK 0
#define FMOD_OUTPUTTYPE_AUTODETECT 0
#define FMOD_OUTPUTTYPE_NOSOUND 2

typedef int (*fmod_system_create_t)(void **, unsigned int);
typedef int (*fmod_studio_create_t)(void **, unsigned int);
typedef int (*fmod_system_init_t)(void *, int, unsigned int, void *);
typedef int (*fmod_studio_init_t)(void *, int, unsigned int, unsigned int, void *);
typedef int (*fmod_setoutput_t)(void *, int);
typedef int (*fmod_getcore_t)(void *, void **);
typedef int (*fmod_loadbank_t)(void *, const char *, unsigned int, void **);

static fmod_system_create_t g_real_system_create;
static fmod_studio_create_t g_real_studio_create;
static fmod_system_init_t   g_real_system_init;
static fmod_studio_init_t   g_real_studio_init;
static fmod_setoutput_t     g_setoutput;
static fmod_getcore_t       g_getcore;

static fmod_loadbank_t g_real_loadbank;

static void *(*g_resolve)(const char *);

/* Saida escolhida pelo port. AUTODETECT deixa o FMOD achar o libaaudio.so do
 * shim; se o shim nao estiver disponivel, o fallback abaixo entra em NOSOUND. */
static int sb_preferred_output(void) { return FMOD_OUTPUTTYPE_AUTODETECT; }

static const char *fmod_err(int r) {
    switch (r) {
        case 0:  return "OK";
        case 26: return "ERR_INITIALIZATION";
        case 27: return "ERR_INITIALIZED";
        case 18: return "ERR_FILE_NOTFOUND";
        case 30: return "ERR_INVALID_HANDLE";
        case 28: return "ERR_INTERNAL";
        case 49: return "ERR_OUTPUT_DRIVERCALL";
        case 50: return "ERR_OUTPUT_FORMAT";
        case 51: return "ERR_OUTPUT_INIT";
        case 52: return "ERR_OUTPUT_NODRIVERS";
        case 54: return "ERR_PLUGIN_MISSING";
        default: return "?";
    }
}

static void resolve_helpers(void) {
    if (!g_resolve) return;
    if (!g_setoutput) g_setoutput = (fmod_setoutput_t)g_resolve("FMOD_System_SetOutput");
    if (!g_getcore) g_getcore = (fmod_getcore_t)g_resolve("FMOD_Studio_System_GetCoreSystem");
}

static void *g_core_system;   /* guardado no Create: apos um init falho o
                               * GetCoreSystem do Studio ja nao devolve nada */

static void apply_output(void *core, const char *who) {
    resolve_helpers();
    if (!core || !g_setoutput) {
        fprintf(stderr, "[fmod] %s: sem core(%p)/SetOutput(%p)\n", who, core,
                (void *)g_setoutput);
        return;
    }
    g_core_system = core;
    int want = sb_preferred_output();
    int r = g_setoutput(core, want);
    fprintf(stderr, "[fmod] %s: SetOutput(%d) -> %d (%s)\n", who, want, r, fmod_err(r));
    if (r != FMOD_OK) {
        /* Sem plugin de saida (ERR_PLUGIN_MISSING) o init inteiro falharia e o
         * SoundHelper do jogo lancaria. NOSOUND mantem eventos, banks e
         * callbacks do FMOD funcionando — o jogo continua jogavel. */
        r = g_setoutput(core, FMOD_OUTPUTTYPE_NOSOUND);
        fprintf(stderr, "[fmod] %s: SetOutput(NOSOUND) -> %d (%s)\n", who, r, fmod_err(r));
    }
}

static int sb_system_create(void **out, unsigned int headerversion) {
    int r = g_real_system_create(out, headerversion);
    fprintf(stderr, "[fmod] System::create -> %d (%s) system=%p\n", r, fmod_err(r),
            out ? *out : NULL);
    if (r == FMOD_OK && out) apply_output(*out, "core");
    return r;
}

static int sb_studio_create(void **out, unsigned int headerversion) {
    int r = g_real_studio_create(out, headerversion);
    fprintf(stderr, "[fmod] Studio::System::create -> %d (%s) system=%p\n", r,
            fmod_err(r), out ? *out : NULL);
    resolve_helpers();
    if (r == FMOD_OK && out && *out && g_getcore) {
        void *core = NULL;
        int cr = g_getcore(*out, &core);
        fprintf(stderr, "[fmod] GetCoreSystem -> %d core=%p\n", cr, core);
        if (cr == FMOD_OK) apply_output(core, "studio");
    }
    return r;
}

static int sb_system_init(void *system, int maxchannels, unsigned int flags,
                          void *extra) {
    int r = g_real_system_init(system, maxchannels, flags, extra);
    fprintf(stderr, "[fmod] System::init(max=%d flags=0x%x) -> %d (%s)\n",
            maxchannels, flags, r, fmod_err(r));
    if (r != FMOD_OK && g_setoutput) {
        fprintf(stderr, "[fmod] caindo para NOSOUND (core)\n");
        g_setoutput(system, FMOD_OUTPUTTYPE_NOSOUND);
        r = g_real_system_init(system, maxchannels, flags, extra);
        fprintf(stderr, "[fmod] System::init NOSOUND -> %d (%s)\n", r, fmod_err(r));
    }
    return r;
}

static int sb_studio_init(void *system, int maxchannels, unsigned int studioflags,
                          unsigned int flags, void *extra) {
    fprintf(stderr, "[fmod] Studio::initialize pedido: max=%d studioflags=0x%x core_flags=0x%x extra=%p\n",
            maxchannels, studioflags, flags, extra);
    int r = g_real_studio_init(system, maxchannels, studioflags, flags, extra);
    fprintf(stderr, "[fmod] Studio::System::initialize(max=%d) -> %d (%s)\n",
            maxchannels, r, fmod_err(r));
    if (r != FMOD_OK) {
        resolve_helpers();
        void *core = g_core_system;
        if (!core && g_getcore) g_getcore(system, &core);
        if (core && g_setoutput) {
            fprintf(stderr, "[fmod] caindo para NOSOUND (studio)\n");
            g_setoutput(core, FMOD_OUTPUTTYPE_NOSOUND);
            r = g_real_studio_init(system, maxchannels, studioflags, flags, extra);
            fprintf(stderr, "[fmod] Studio::System::initialize NOSOUND -> %d (%s)\n",
                    r, fmod_err(r));
        } else {
            fprintf(stderr, "[fmod] fallback indisponivel: core=%p getcore=%p setoutput=%p\n",
                    core, (void *)g_getcore, (void *)g_setoutput);
        }
    }
    return r;
}

/* No Android os banks do FMOD sao lidos pelo AssetManager, com caminho relativo
 * a `assets/`. Aqui os assets estao no disco, entao resolvemos o caminho
 * relativo contra SB_ASSET_DIR antes de entregar ao FMOD. */
static int sb_studio_loadbankfile(void *system, const char *filename,
                                  unsigned int flags, void **bank) {
    static const char kAssetUrl[] = "file:///android_asset/";
    char resolved[1024];
    const char *use = filename;
    const char *rel = NULL;
    if (filename && strncmp(filename, kAssetUrl, sizeof kAssetUrl - 1) == 0)
        rel = filename + sizeof kAssetUrl - 1;
    else if (filename && filename[0] != '/')
        rel = filename;
    if (rel) {
        const char *root = getenv("SB_ASSET_DIR");
        if (root && *root) {
            snprintf(resolved, sizeof resolved, "%s/%s", root, rel);
            if (access(resolved, R_OK) == 0) use = resolved;
        }
    }
    int r = g_real_loadbank(system, use, flags, bank);
    fprintf(stderr, "[fmod] loadBankFile(\"%s\") -> %d (%s)%s\n", filename, r,
            fmod_err(r), use == filename ? "" : " [via SB_ASSET_DIR]");
    return r;
}

/* Chamado por sdv_so_dlsym para cada simbolo pedido. Devolve o wrapper quando
 * ha um, ou NULL para deixar passar o simbolo real. */
void *sb_fmod_intercept(const char *name, void *real,
                        void *(*resolve)(const char *)) {
    if (!name || !real || !resolve) return NULL;
    g_resolve = resolve;
    if (strcmp(name, "FMOD_System_Create") == 0) {
        g_real_system_create = (fmod_system_create_t)real;
        return (void *)sb_system_create;
    }
    if (strcmp(name, "FMOD_Studio_System_Create") == 0) {
        g_real_studio_create = (fmod_studio_create_t)real;
        return (void *)sb_studio_create;
    }
    if (strcmp(name, "FMOD_System_Init") == 0) {
        g_real_system_init = (fmod_system_init_t)real;
        return (void *)sb_system_init;
    }
    if (strcmp(name, "FMOD_Studio_System_LoadBankFile") == 0) {
        g_real_loadbank = (fmod_loadbank_t)real;
        return (void *)sb_studio_loadbankfile;
    }
    if (strcmp(name, "FMOD_Studio_System_Initialize") == 0) {
        g_real_studio_init = (fmod_studio_init_t)real;
        return (void *)sb_studio_init;
    }
    return NULL;
}
