/*
 * host_libs.c -- resolucao de biblioteca do HOST por nome nao versionado.
 *
 * O Android resolve `dlopen("libicuuc.so")` porque la o SONAME e' sempre o
 * nome curto. Numa distribuicao Linux o arquivo instalado e' versionado
 * (`libicuuc.so.76`) e o link nao versionado so vem no pacote -dev, que nao
 * existe num CFW. Resultado no .137: o `libSystem.Globalization.Native.so` do
 * .NET nao acha ICU nenhum e o runtime aborta com
 *
 *     Unable to load required ICU Globalization data.
 *
 * Isto NAO e' especifico do ICU nem de um CFW: vale para qualquer biblioteca
 * do sistema que o lado Android peca pelo nome curto. Por isso a busca aqui e'
 * por CAPACIDADE -- procura o que o aparelho realmente tem e escolhe a maior
 * versao -- e nunca por nome de CFW, distribuicao ou caminho fixo de device.
 */
#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Diretorios candidatos. O primeiro grupo vem do ambiente (o CFW pode apontar
 * para um prefixo proprio); o resto e' o layout comum de qualquer aarch64. */
static const char *const NX_LIBDIRS[] = {
    "/usr/lib/aarch64-linux-gnu",
    "/lib/aarch64-linux-gnu",
    "/usr/lib64",
    "/usr/lib",
    "/lib",
    "/usr/local/lib",
    NULL
};

/* "libicuuc.so.76" -> 76 ; devolve -1 quando o sufixo nao e' numerico. */
static long nx_soversion(const char *path, const char *base)
{
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    size_t bl = strlen(base);
    if (strncmp(leaf, base, bl) != 0 || leaf[bl] != '.') return -1;
    const char *v = leaf + bl + 1;
    if (*v < '0' || *v > '9') return -1;
    return strtol(v, NULL, 10);
}

/* Tenta abrir `name` procurando a maior versao disponivel no aparelho.
 * Devolve NULL quando nao existe nada com esse nome. */
void *nx_dlopen_host_versioned(const char *name, int flag)
{
    if (!name || strchr(name, '/')) return NULL;
    size_t n = strlen(name);
    if (n < 4 || strcmp(name + n - 3, ".so") != 0) return NULL;

    char best[512];
    long best_v = -1;
    best[0] = '\0';

    for (int i = 0; NX_LIBDIRS[i]; i++) {
        char pattern[512];
        if ((size_t)snprintf(pattern, sizeof(pattern), "%s/%s.*",
                             NX_LIBDIRS[i], name) >= sizeof(pattern))
            continue;
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++) {
                long v = nx_soversion(g.gl_pathv[k], name);
                if (v > best_v) {
                    best_v = v;
                    snprintf(best, sizeof(best), "%s", g.gl_pathv[k]);
                }
            }
        }
        globfree(&g);
    }

    if (best_v < 0) return NULL;
    void *h = dlopen(best, flag);
    fprintf(stderr, "[host-lib] '%s' -> '%s' %s\n", name, best,
            h ? "OK" : dlerror());
    return h;
}
