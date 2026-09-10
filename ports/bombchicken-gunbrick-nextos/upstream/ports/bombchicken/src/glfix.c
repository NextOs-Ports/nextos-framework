/* glfix.c -- GL provider repair by observed capability (no device names).
 *
 * Field evidence (ArkOS R36S, 2026-08-13, core dump analisado): os SONAMEs
 * versionados do firmware sao cruzados -- libEGL.so.1 resolve para uma Mesa
 * sem driver util enquanto o blob Mali real fica atras dos nomes libmali*.
 * O SDL falha a janela com "Can't load EGL/GL library", o loader cai no EGL
 * cru, e a Unity morre com "Unable to initialize EGL!" + raise(SIGTRAP)
 * (tgkill visto no core, pilha em libunity -> libEGL_mesa).
 *
 * Receita herdada do swordigo (glfix 1.0.9, provado neste mesmo cartao):
 * ANTES de qualquer video, provar eglGetDisplay+eglInitialize no provider que
 * o linker resolve.  Se inicializa, o sistema e' saudavel (NextOS Mali fbdev,
 * ROCKNIX Panfrost) e NADA muda.  Se falha, procurar um blob Mali que
 * inicialize EGL contra o kernel vivo (padrao *-gbm primeiro -- o -dummy
 * aceita tudo e nao desenha) e re-exec UMA vez com LD_PRELOAD apontando para
 * ele, de modo que SDL, loader e a propria Unity resolvam o mesmo objeto.
 *
 * Controles:
 *   BC_GLFIX=0            desliga (quirk de manifesto liga: BC_GLFIX=1).
 *   BC_GLFIX_BLOB=/path   forca um provider especifico.
 *   BC_GLFIX_APPLIED      marker interno; impede loop de re-exec.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <limits.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bc.h"

#define GLFIX_MARKER "BC_GLFIX_APPLIED"

static char **g_saved_argv;

void bc_glfix_set_argv(char **argv)
{
    g_saved_argv = argv;
}

/* O probe respeita o padrao do port: nasce desligado, liga pelo manifesto
 * (contract.c -> BC_GLFIX=1) e BC_GLFIX=0 devolve o comportamento antigo. */
static int glfix_enabled(void)
{
    const char *flag = getenv("BC_GLFIX");
    return flag && *flag && strcmp(flag, "0") != 0;
}

/* Providers ja mapeados neste processo: um candidato que e' o mesmo arquivo
 * ja provou que falha ao nos trazer ate aqui. */
#define GLFIX_MAX_LOADED 8
static char g_loaded_providers[GLFIX_MAX_LOADED][PATH_MAX];
static int g_loaded_provider_count;

static int glfix_collect_loaded_cb(struct dl_phdr_info *info, size_t size,
                                   void *data)
{
    (void)size;
    (void)data;
    const char *name = info->dlpi_name;
    if (!name || !name[0])
        return 0;
    if (!strstr(name, "libEGL.so") && !strstr(name, "libGLES") &&
        !strstr(name, "mali") && !strstr(name, "Mali"))
        return 0;
    if (g_loaded_provider_count >= GLFIX_MAX_LOADED)
        return 0;
    if (realpath(name, g_loaded_providers[g_loaded_provider_count]))
        g_loaded_provider_count++;
    return 0;
}

static int glfix_candidate_is_loaded(const char *path)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return 0;
    for (int i = 0; i < g_loaded_provider_count; i++)
        if (strcmp(resolved, g_loaded_providers[i]) == 0)
            return 1;
    return 0;
}

/* O candidato precisa exportar o par EGL + GLES2 que esta Unity usa, num
 * objeto SO.  Nao se exige eglInitialize(EGL_DEFAULT_DISPLAY) aqui: um blob
 * -gbm coerente so inicializa em cima de um device GBM, e quem monta esse
 * device e' o SDL depois do re-exec (mesma prova do glfix do swordigo neste
 * mesmo cartao).  dlsym prova em vez de confiar no nome do arquivo. */
static int glfix_blob_usable(const char *path)
{
    if (glfix_candidate_is_loaded(path)) {
        fprintf(stderr, "[bc/glfix] %s pulado (ja carregado e falhando)\n",
                path);
        return 0;
    }
    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "[bc/glfix] %s rejeitado (dlopen: %s)\n", path,
                dlerror());
        return 0;
    }
    int ok = dlsym(handle, "glCreateShader") != NULL &&
             dlsym(handle, "eglGetDisplay") != NULL &&
             dlsym(handle, "eglInitialize") != NULL;
    dlclose(handle);
    if (!ok)
        fprintf(stderr,
                "[bc/glfix] %s rejeitado (sem glCreateShader/eglGetDisplay)\n",
                path);
    return ok;
}

/* Ordem deliberada: a variante -gbm casa com o backend KMSDRM do SDL e tem de
 * vencer a varredura alfabetica (que acharia -dummy primeiro). */
static const char *const glfix_patterns[] = {
    "libmali*-gbm.so*",  "libmali-bifrost-*.so*", "libmali-midgard-*.so*",
    "libmali.so*",       "libMali.so*",           "libGLES_mali.so*",
};
static const char *const glfix_dirs[] = {
    "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu",
    "/usr/lib64", "/usr/lib", "/lib",
};

static int glfix_find_blob(char *out, size_t out_size)
{
    const char *forced = getenv("BC_GLFIX_BLOB");
    if (forced && *forced) {
        if (access(forced, R_OK) == 0 && glfix_blob_usable(forced)) {
            snprintf(out, out_size, "%s", forced);
            return 1;
        }
        fprintf(stderr, "[bc/glfix] blob forcado inutilizavel: %s\n", forced);
        return 0;
    }
    for (size_t p = 0; p < sizeof glfix_patterns / sizeof *glfix_patterns;
         p++) {
        for (size_t d = 0; d < sizeof glfix_dirs / sizeof *glfix_dirs; d++) {
            DIR *dir = opendir(glfix_dirs[d]);
            if (!dir)
                continue;
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (fnmatch(glfix_patterns[p], entry->d_name, 0) != 0)
                    continue;
                /* Sufixos que nao desenham em KMSDRM nem em fbdev. */
                if (strstr(entry->d_name, "dummy") ||
                    strstr(entry->d_name, "-x11") ||
                    strstr(entry->d_name, "wayland"))
                    continue;
                char candidate[1024];
                snprintf(candidate, sizeof candidate, "%s/%s", glfix_dirs[d],
                         entry->d_name);
                if (glfix_blob_usable(candidate)) {
                    snprintf(out, out_size, "%s", candidate);
                    closedir(dir);
                    return 1;
                }
            }
            closedir(dir);
        }
    }
    return 0;
}

/* Uma tentativa para sempre: o marker sobrevive ao exec e para o loop.
 * Tres amarras no mesmo objeto para nao restar caminho cruzado:
 *   - SDL_VIDEO_EGL_DRIVER / SDL_VIDEO_GL_DRIVER: o SDL abre GBM/KMSDRM e o
 *     EGL/GLES do blob (par coerente provado pelo swordigo neste firmware);
 *   - BC_GLFIX_PROVIDER: o sys()/gl_raw() do egl.c dlopen'a por SONAME, que o
 *     LD_PRELOAD nao cobre -- eles preferem este caminho depois do re-exec;
 *   - LD_PRELOAD: cobre qualquer referencia direta restante. */
static void glfix_reexec_with_provider(const char *blob)
{
    const char *previous = getenv("LD_PRELOAD");
    char preload[2048];
    if (previous && *previous)
        snprintf(preload, sizeof preload, "%s:%s", blob, previous);
    else
        snprintf(preload, sizeof preload, "%s", blob);
    fprintf(stderr,
            "[bc/glfix] re-executando com provider coerente %s\n", blob);
    setenv("SDL_VIDEO_EGL_DRIVER", blob, 1);
    setenv("SDL_VIDEO_GL_DRIVER", blob, 1);
    setenv("BC_GLFIX_PROVIDER", blob, 1);
    setenv("LD_PRELOAD", preload, 1);
    setenv(GLFIX_MARKER, "1", 1);
    if (g_saved_argv)
        execv("/proc/self/exe", g_saved_argv);
    fprintf(stderr, "[bc/glfix] execv falhou; seguindo sem reparo\n");
}

/* Chamado uma vez, antes de qualquer video.  So' age quando o provider que o
 * linker resolve nao inicializa EGL neste kernel; um sistema saudavel passa
 * no probe e sai intocado. */
void bc_glfix_probe_before_video(void)
{
    if (!glfix_enabled())
        return;
    if (getenv(GLFIX_MARKER)) {
        fprintf(stderr,
                "[bc/glfix] provider ja reparado nesta execucao (marker)\n");
        return;
    }

    void *handle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        handle = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    int healthy = 0;
    if (handle) {
        void *(*get_display)(void *) =
            (void *(*)(void *))dlsym(handle, "eglGetDisplay");
        unsigned (*initialize)(void *, int *, int *) =
            (unsigned (*)(void *, int *, int *))dlsym(handle, "eglInitialize");
        unsigned (*terminate)(void *) =
            (unsigned (*)(void *))dlsym(handle, "eglTerminate");
        if (get_display && initialize) {
            void *display = get_display(NULL);
            int major = 0, minor = 0;
            if (display && initialize(display, &major, &minor)) {
                healthy = 1;
                if (terminate)
                    terminate(display);
            }
        }
        dlclose(handle);
    }
    if (healthy)
        return; /* Mesa/Mali saudavel: caminho aprovado, nao tocar. */

    fprintf(stderr, "[bc/glfix] o libEGL do sistema nao inicializa neste "
                    "kernel; procurando provider coerente\n");
    dl_iterate_phdr(glfix_collect_loaded_cb, NULL);
    char blob[1024];
    if (!glfix_find_blob(blob, sizeof blob)) {
        fprintf(stderr, "[bc/glfix] nenhum provider alternativo inicializa; "
                        "mantendo a falha original\n");
        return;
    }
    glfix_reexec_with_provider(blob);
}
