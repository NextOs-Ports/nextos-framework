#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdio.h>
#include <stdarg.h>
#include <link.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <inttypes.h>
#include <dlfcn.h>
#include "platform.h"
#include "so_util.h"

#include "bionic_file.h"

extern "C" ABI_ATTR int dl_iterate_phdr_impl(
                 int (*callback)(struct dl_phdr_info *info,
                                 size_t size, void *data),
                 void *data)
{
    // TODO:: Implement a reasonable version of this.
    fatal_error("-- dl_iterate_phdr was called! --\n");
    return -1;
}

extern "C" ABI_ATTR int login_tty_impl(int fd)
{
    return -1;
}

extern "C" ABI_ATTR long syscall_impl(long number, ...)
{
#ifdef gettid
    if (number == 0xb2)
        return gettid();
#else
    if (number == 0xb2)
        return syscall(SYS_gettid);
#endif
    return 0;
}

extern "C" ABI_ATTR void abort_impl(void)
{
    fatal_error("Guest called abort!\n");
    exit(-1);
}

extern "C" ABI_ATTR void *dlopen_impl(const char *filename, int flags)
{
    if (filename == NULL)
        return NULL;

    char *fn = strdup(filename);
    char *ex = basename(fn);
    int ret = strncmp(ex, "libEGL", 6) == 0 ||
              strncmp(ex, "libGL", 5) == 0;

    if (getenv("GMLOADER_AUDIO_DEBUG"))
        printf("[dl] dlopen('%s') -> %p\n", filename, (ret) ? (void*)0xDEAD : NULL);
    return (ret) ? (void*)0xDEAD : NULL;
}

extern "C" ABI_ATTR char *dlerror_impl(void)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int dlclose_impl(void *handle)
{
    /* ... */
    return 0;
}

extern "C" ABI_ATTR void *dlsym_impl(void *handle, const char *name)
{
    void *r = (void*)so_resolve_link(NULL, name);
    if (getenv("GMLOADER_AUDIO_DEBUG"))
        printf("[dl] dlsym(%p, '%s') -> %p\n", handle, name, r);
    return r;
}

extern "C" ABI_ATTR const void *
memchr_impl (const void *__s, int __c, size_t __n)
{
  return __builtin_memchr (__s, __c, __n);
}

extern "C" ABI_ATTR int sigsetmask_impl(int mask)
{
    WARN_STUB
    return -1;
}

extern "C" ABI_ATTR char *tempnam_impl(const char *dir, const char *pfx)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char *tmpnam_impl(char *s)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char *mktemp_impl(char *_template)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int* __errno_impl(void)
{
    return __errno_location();
}

extern "C" ABI_ATTR int __android_log_write_impl(int prio, const char *tag, const char *text)
{
    char andlog[2048] = {};
    warning("LOG[%s]: %s\n", tag, text);
    return 1;
}

extern "C" ABI_ATTR int __android_log_print_impl(int prio, const char *tag, const char *fmt, ...)
{
    char andlog[2048] = {};
    va_list va;
    va_start(va, fmt);
    warning("LOG[%s]: ", tag);
    int r = vsnprintf(andlog, 2047, fmt, va);
    warning("%s\n", andlog);
    va_end(va);
    return r;
}

extern "C" ABI_ATTR int __android_log_vprint_impl(int prio, const char *tag, const char *fmt, va_list va)
{
    char andlog[2048] = {};
    warning("LOG[%s]: ", tag);
    int r = vsnprintf(andlog, 2047, fmt, va);
    warning("%s\n", andlog);
    return r;
}

extern "C" ABI_ATTR const char* __strchr_chk(const char* __s, int __ch, size_t __n) { return strchr(__s, __ch); }
extern "C" ABI_ATTR const char* __strrchr_chk(const char* __s, int __ch, size_t __n) { return strrchr(__s, __ch); }
extern "C" ABI_ATTR size_t __strlen_chk(const char* __s, size_t __n) { return strnlen(__s, __n); }

extern "C" ABI_ATTR void android_set_abort_message_impl(const char* msg)
{
    fatal_error("%s", msg);
    abort();
}

extern "C" ABI_ATTR int __system_property_get_impl(const char *name, char *value)
{
    WARN_STUB;
    value[0] = 0;
    return 0;
}

extern "C" ABI_ATTR int __open_2_impl(const char* pathname, int flags) {
  return open(pathname, flags);
}

// Taken from https://github.com/libhybris/libhybris/blob/master/hybris/common/hooks.c
ABI_ATTR int scandirat_impl(int fd, const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    struct dirent **namelist_r;
    struct bionic_dirent **result;
    struct bionic_dirent *filter_r;

    int i = 0;
    size_t nItems = 0;

    int res = scandirat(fd, dir, &namelist_r, NULL, NULL);

    if (res > 0 && namelist_r != NULL) {
        result = (bionic_dirent**)malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = (bionic_dirent*)malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                    free(result[i]);
                free(result);
                return -1;
            }

            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) {//apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }
        
        if (nItems && compar != NULL) // sort
            qsort(result, nItems, sizeof(struct bionic_dirent *), (__compar_fn_t)compar);

        *namelist = result;
    } else {
        return res;
    }

    return nItems;
}

ABI_ATTR int scandir_impl(const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    return scandirat_impl(AT_FDCWD, dir, namelist, filter, compar);
}

/* =====================================================================================
 * [NextOS/forager] Simbolos que o gerador de thunks NAO exportava e que a libyoyo /
 * libc++_shared do Forager IMPORTAM de verdade (vistos como "Missing: X" no log do
 * device). Sem eles a resolucao cai em plt0_stub e o I/O de arquivo do jogo devolve
 * lixo -> o GML acaba recebendo `undefined`.
 *
 * open/open64/openat/openat64/fcntl/ioctl sao VARIADICAS: o generate_libc.py so sabe
 * emitir THUNK para funcao de aridade fixa, entao elas sairiam comentadas como
 * "(TODO impl variadic)". Definir <nome>_impl faz o gerador emitir NO_THUNK apontando
 * pra ca (ponteiro direto), que e o certo: em ARM, argumento variadico SEMPRE viaja
 * pela convencao base (registrador de inteiro), entao nao ha divergencia softfp/hardfp.
 *
 * As constantes O_xxx, F_xxx e de ioctl do bionic ARM e da glibc ARM sao as mesmas (ambos usam o
 * asm-generic do kernel), entao repassar direto e seguro.
 * ===================================================================================== */

extern "C" ABI_ATTR int open_impl(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);   /* mode_t promove pra int */
        va_end(ap);
    }
    return open(path, flags, mode);
}

extern "C" ABI_ATTR int open64_impl(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags | O_LARGEFILE, mode);
}

extern "C" ABI_ATTR int openat_impl(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openat(dirfd, path, flags, mode);
}

extern "C" ABI_ATTR int openat64_impl(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openat(dirfd, path, flags | O_LARGEFILE, mode);
}

extern "C" ABI_ATTR int fcntl_impl(int fd, int cmd, ...)
{
    /* Todos os comandos de fcntl usados na pratica levam 0 ou 1 argumento extra, e esse
     * argumento e um int ou um ponteiro — nos dois casos cabe em `void *`. */
    va_list ap; va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return fcntl(fd, cmd, arg);
}

extern "C" ABI_ATTR int ioctl_impl(int fd, int request, ...)
{
    va_list ap; va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return ioctl(fd, (unsigned long)request, arg);
}

/* --- nao-variadicas que tambem apareceram como "Missing:" --- */

extern "C" ABI_ATTR int if_nametoindex_impl(const char *ifname)
{
    /* Sem rede neste port: 0 = "interface nao encontrada", que e resposta valida e
     * NUNCA um erro fatal (regra da casa: shim nunca devolve falha dura). */
    return 0;
}

extern "C" ABI_ATTR void __assert2_impl(const char *file, int line,
                                        const char *function, const char *msg)
{
    /* __assert2 e a forma do bionic. Nao pode virar no-op: e um caminho de erro real. */
    fatal_error("assert do guest: %s:%d: %s: %s\n",
                file ? file : "?", line, function ? function : "?", msg ? msg : "?");
}

extern "C" ABI_ATTR int dladdr_impl(const void *addr, void *info)
{
    /* O guest nao esta no link map do host (nos mesmos carregamos os modulos), entao o
     * dladdr do host nunca acharia o endereco. 0 = "nao encontrado" e a resposta correta
     * da API e e o que o libyoyo espera no caminho de relatorio de crash. */
    return 0;
}

/* strtoimax/strtoumax saem como "(TODO impl ambiguous)" no gerador (o header C++ tem
 * sobrecarga), entao precisam de _impl explicito. */
extern "C" ABI_ATTR intmax_t strtoimax_impl(const char *nptr, char **endptr, int base)
{
    return strtoimax(nptr, endptr, base);
}

extern "C" ABI_ATTR uintmax_t strtoumax_impl(const char *nptr, char **endptr, int base)
{
    return strtoumax(nptr, endptr, base);
}
