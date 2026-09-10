/*
 * as_shims.c — overrides EXCLUSIVOS do Door Kickers: Action Squad.
 *
 * Tudo aqui saiu de medição no binário/device (MEDIDAS.md).  A tabela
 * `as_overrides` é concatenada ANTES da tabela do esqueleto, então o que está
 * aqui vence o override genérico de mesmo nome.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "so_util.h"

/* ============================================================
 * 1. funopen() — o muro nº1 do port
 * A engine embrulha TODO asset em funopen(AAsset_read/seek/close)
 * (android_fopen).  funopen é BSD/bionic e NÃO existe na glibc; sem a ponte o
 * AAssetManager entrega um asset válido e mesmo assim o jogo grita
 * "file not found" / "GetFileHash::file size 0" e morre no Shop.cpp:19.
 * fopencookie é o equivalente da glibc.
 * ============================================================ */

typedef int (*bionic_readfn)(void *cookie, char *buf, int n);
typedef int (*bionic_writefn)(void *cookie, const char *buf, int n);
typedef long (*bionic_seekfn)(void *cookie, long offset, int whence);
typedef int (*bionic_closefn)(void *cookie);

typedef struct {
  void *cookie;
  bionic_readfn readfn;
  bionic_writefn writefn;
  bionic_seekfn seekfn;
  bionic_closefn closefn;
} FunopenCookie;

static ssize_t funopen_read(void *opaque, char *buf, size_t size) {
  FunopenCookie *fc = (FunopenCookie *)opaque;
  if (!fc->readfn)
    return 0;
  int n = fc->readfn(fc->cookie, buf, (int)size);
  return n < 0 ? 0 : (ssize_t)n;
}

static ssize_t funopen_write(void *opaque, const char *buf, size_t size) {
  FunopenCookie *fc = (FunopenCookie *)opaque;
  if (!fc->writefn)
    return 0;
  int n = fc->writefn(fc->cookie, buf, (int)size);
  return n < 0 ? 0 : (ssize_t)n;
}

static int funopen_seek(void *opaque, off64_t *offset, int whence) {
  FunopenCookie *fc = (FunopenCookie *)opaque;
  if (!fc->seekfn)
    return -1;
  long r = fc->seekfn(fc->cookie, (long)*offset, whence);
  if (r < 0)
    return -1;
  *offset = (off64_t)r;
  return 0;
}

static int funopen_close(void *opaque) {
  FunopenCookie *fc = (FunopenCookie *)opaque;
  int r = fc->closefn ? fc->closefn(fc->cookie) : 0;
  free(fc);
  return r;
}

static FILE *as_funopen(void *cookie, bionic_readfn readfn,
                        bionic_writefn writefn, bionic_seekfn seekfn,
                        bionic_closefn closefn) {
  FunopenCookie *fc = calloc(1, sizeof(*fc));
  if (!fc)
    return NULL;
  fc->cookie = cookie;
  fc->readfn = readfn;
  fc->writefn = writefn;
  fc->seekfn = seekfn;
  fc->closefn = closefn;

  cookie_io_functions_t io = {
      .read = readfn ? funopen_read : NULL,
      .write = writefn ? funopen_write : NULL,
      .seek = seekfn ? funopen_seek : NULL,
      .close = funopen_close,
  };
  const char *mode = writefn ? (readfn ? "r+" : "w") : "r";
  FILE *f = fopencookie(fc, mode, io);
  if (!f)
    free(fc);
  return f;
}

/* ============================================================
 * 2. stat("/system/lib64/libOpenSLES.so") — caminho ABSOLUTO
 * alc_opensles_probe do OpenAL-soft embutido STATA o caminho antes de tentar o
 * dlopen; se o stat falha ele nem tenta e o backend cai no null.c (jogo mudo
 * sem reclamar).  O dlopen desse mesmo caminho já é roteado pro shim de OpenSL
 * pelo my_dlopen do esqueleto (casa por "OpenSLES" em qualquer caminho).
 * AS_NO_OPENSLES=1 desliga o áudio de propósito (bancada).
 * ============================================================ */

static int is_opensles_path(const char *path) {
  if (!path)
    return 0;
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  return strncmp(base, "libOpenSLES.so", 14) == 0;
}

static int as_stat(const char *pathname, struct stat *buf) {
  if (is_opensles_path(pathname)) {
    if (getenv("AS_NO_OPENSLES")) {
      errno = ENOENT;
      return -1;
    }
    if (access(pathname, F_OK) != 0)
      return stat("/proc/self/exe", buf); /* existe, é regular, tem tamanho */
  }
  return stat(pathname, buf);
}

static int as_lstat(const char *pathname, struct stat *buf) {
  if (is_opensles_path(pathname))
    return as_stat(pathname, buf);
  return lstat(pathname, buf);
}

static int as_access(const char *pathname, int mode) {
  if (is_opensles_path(pathname) && !getenv("AS_NO_OPENSLES"))
    return 0;
  return access(pathname, mode);
}

/* ============================================================
 * 3. sysconf — os _SC_* do bionic não são os da glibc
 * (_SC_NPROCESSORS_ONLN = 97 no bionic, 84 na glibc).  Sem tradução o
 * OpenAL-soft loga "_SC_NPROCESSORS_ONLN=-1".
 * ============================================================ */

static long as_sysconf(int name) {
  switch (name) {
  case 0x0000: return sysconf(_SC_ARG_MAX);
  case 0x0005: return sysconf(_SC_CHILD_MAX);
  case 0x0006: return sysconf(_SC_CLK_TCK);
  case 0x000b: return sysconf(_SC_OPEN_MAX);
  case 0x0027:
  case 0x0028: return sysconf(_SC_PAGESIZE);
  case 0x0060: return sysconf(_SC_NPROCESSORS_CONF);
  case 0x0061: return sysconf(_SC_NPROCESSORS_ONLN);
  case 0x0062: return sysconf(_SC_PHYS_PAGES);
  case 0x0063: return sysconf(_SC_AVPHYS_PAGES);
  default: {
    long v = sysconf(name);
    if (v < 0)
      errno = 0;
    return v;
  }
  }
}

/* ============================================================ */

DynLibFunction as_overrides[] = {
    {"funopen", (uintptr_t)&as_funopen},
    {"stat", (uintptr_t)&as_stat},
    {"lstat", (uintptr_t)&as_lstat},
    {"access", (uintptr_t)&as_access},
    {"sysconf", (uintptr_t)&as_sysconf},
};

const int as_overrides_count =
    sizeof(as_overrides) / sizeof(as_overrides[0]);
