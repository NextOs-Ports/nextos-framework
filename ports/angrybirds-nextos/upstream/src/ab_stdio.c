/* ab_stdio.c — stdio bionic sobre fds crus.
 *
 * O FILE do bionic é o `__sFILE` público do BSD e o jogo INLINA acessos a ele
 * (fileno lê `_file` no offset 14; feof/ferror leem `_flags`). Entregar um
 * FILE* opaco do glibc faz esses acessos lerem lixo. Aqui o FILE tem o layout
 * bionic EXATO (84 B, para o passo de `__sF[3]` bater) sobre um fd cru.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ab_port.h"

#define B_SRD 0x0004
#define B_SWR 0x0008
#define B_SEOF 0x0020
#define B_SERR 0x0040

typedef struct bFILE {
  unsigned char *_p;      /* 0  */
  int _r;                 /* 4  */
  int _w;                 /* 8  */
  short _flags;           /* 12 */
  short _file;            /* 14 fd — lido pelo fileno inlinado */
  void *_bf_base;         /* 16 */
  int _bf_size;           /* 20 */
  int _lbfsize;           /* 24 */
  void *_cookie;          /* 28 */
  void *_close;           /* 32 */
  void *_read;            /* 36 */
  void *_seek;            /* 40 */
  void *_write;           /* 44 */
  void *_ext_base;        /* 48 */
  int _ext_size;          /* 52 */
  unsigned char *_up;     /* 56 */
  int _ur;                /* 60 */
  unsigned char _ubuf[3]; /* 64 */
  unsigned char _nbuf[1]; /* 67 */
  void *_lb_base;         /* 68 */
  int _lb_size;           /* 72 */
  int _blksize;           /* 76 */
  long _offset;           /* 80 → 84 */
} bFILE;

#define AB_BUFSZ 8192
static bFILE g_std[3];
static int g_std_ready;

static void std_init(void) {
  if (g_std_ready)
    return;
  for (int i = 0; i < 3; i++) {
    memset(&g_std[i], 0, sizeof(bFILE));
    g_std[i]._file = (short)i;
    g_std[i]._bf_base = malloc(AB_BUFSZ);
    g_std[i]._bf_size = AB_BUFSZ;
    g_std[i]._p = g_std[i]._bf_base;
    g_std[i]._flags = (i == 0) ? B_SRD : B_SWR;
  }
  g_std_ready = 1;
}

void *ab_bstdio_sF(void) {
  std_init();
  return g_std;
}

static bFILE *alloc_file(int fd, int write_mode) {
  bFILE *f = calloc(1, sizeof(bFILE));
  if (!f)
    return NULL;
  f->_file = (short)fd;
  f->_bf_base = malloc(AB_BUFSZ);
  f->_bf_size = AB_BUFSZ;
  f->_p = f->_bf_base;
  f->_flags = write_mode ? B_SWR : B_SRD;
  return f;
}

static int flags_for(const char *mode) {
  int f = O_RDONLY;
  if (strchr(mode, 'w'))
    f = O_WRONLY | O_CREAT | O_TRUNC;
  else if (strchr(mode, 'a'))
    f = O_WRONLY | O_CREAT | O_APPEND;
  if (strchr(mode, '+'))
    f = (f & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
  return f;
}
static int is_write(const char *mode) {
  return strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+') != NULL;
}

static void *b_fopen(const char *path, const char *mode) {
  int fd;
  if (!path || !mode)
    return NULL;
  fd = open(path, flags_for(mode), 0644);
  if (fd < 0) {
    if (ab_env_int("AB_FOPEN_LOG", 0))
      ab_log("[stdio] fopen MISS %s (%s)", path, mode);
    return NULL;
  }
  if (ab_env_int("AB_FOPEN_LOG", 0))
    ab_log("[stdio] fopen OK %s (%s)", path, mode);
  return alloc_file(fd, is_write(mode));
}
static void *b_fdopen(int fd, const char *mode) {
  return alloc_file(fd, is_write(mode ? mode : "r"));
}
static void *b_freopen(const char *path, const char *mode, void *stream) {
  bFILE *f = stream;
  int fd;
  if (f && f->_file >= 3)
    close(f->_file);
  fd = open(path, flags_for(mode), 0644);
  if (fd < 0)
    return NULL;
  if (!f)
    return alloc_file(fd, is_write(mode));
  f->_file = (short)fd;
  f->_p = f->_bf_base;
  f->_r = 0;
  f->_flags = is_write(mode) ? B_SWR : B_SRD;
  return f;
}
static int b_fclose(void *stream) {
  bFILE *f = stream;
  if (!f || f->_file < 0)
    return 0;
  if (f->_file >= 3)
    close(f->_file);
  f->_file = -1;
  if (f >= g_std && f < g_std + 3)
    return 0;
  free(f->_bf_base);
  free(f);
  return 0;
}

static int b_srget(void *stream) {
  bFILE *f = stream;
  ssize_t n = read(f->_file, f->_bf_base, (size_t)f->_bf_size);
  if (n <= 0) {
    f->_flags |= (n == 0) ? B_SEOF : B_SERR;
    f->_r = 0;
    f->_p = f->_bf_base;
    return -1;
  }
  f->_p = f->_bf_base;
  f->_r = (int)n - 1;
  return *f->_p++;
}
static int b_fgetc(void *stream) {
  bFILE *f = stream;
  if (--f->_r < 0)
    return b_srget(f);
  return *f->_p++;
}
static int b_ungetc(int c, void *stream) {
  bFILE *f = stream;
  if (c == -1)
    return -1;
  if (f->_p > (unsigned char *)f->_bf_base) {
    f->_p--;
    f->_r++;
    *f->_p = (unsigned char)c;
    f->_flags &= ~B_SEOF;
    return c;
  }
  return -1;
}

static size_t b_fread(void *ptr, size_t size, size_t nmemb, void *stream) {
  bFILE *f = stream;
  size_t total = size * nmemb, done = 0;
  unsigned char *out = ptr;
  if (total == 0)
    return 0;
  if (f->_r > 0) {
    size_t k = (size_t)f->_r < total ? (size_t)f->_r : total;
    memcpy(out, f->_p, k);
    f->_p += k;
    f->_r -= (int)k;
    out += k;
    done += k;
  }
  while (done < total) {
    ssize_t n = read(f->_file, out, total - done);
    if (n <= 0) {
      f->_flags |= (n == 0) ? B_SEOF : B_SERR;
      break;
    }
    out += n;
    done += (size_t)n;
  }
  return done / size;
}

static size_t b_fwrite(const void *ptr, size_t size, size_t nmemb,
                       void *stream) {
  bFILE *f = stream;
  size_t total = size * nmemb, done = 0;
  const unsigned char *in = ptr;
  if (total == 0)
    return 0;
  while (done < total) {
    ssize_t n = write(f->_file, in + done, total - done);
    if (n <= 0) {
      f->_flags |= B_SERR;
      break;
    }
    done += (size_t)n;
  }
  return done / size;
}

/* fd 1/2 é o console do jogo: drenar pro cartão a cada frame mata o fps. */
static int game_console(int fd) {
  return (fd == 1 || fd == 2) && !ab_env_int("AB_GAMELOG", 0);
}

static int b_fputc(int c, void *stream) {
  bFILE *f = stream;
  unsigned char ch = (unsigned char)c;
  if (game_console(f->_file))
    return c;
  return write(f->_file, &ch, 1) == 1 ? c : -1;
}
static int b_fputs(const char *s, void *stream) {
  bFILE *f = stream;
  size_t len = strlen(s);
  if (game_console(f->_file))
    return (int)len;
  return write(f->_file, s, len) == (ssize_t)len ? (int)len : -1;
}
static int b_puts(const char *s) {
  if (ab_env_int("AB_GAMELOG", 0))
    ab_log("%s", s ? s : "");
  return (int)strlen(s ? s : "") + 1;
}

static long logical_pos(bFILE *f) {
  return (long)lseek(f->_file, 0, SEEK_CUR) - f->_r;
}
static int b_fseek(void *stream, long offset, int whence) {
  bFILE *f = stream;
  long target;
  if (whence == SEEK_SET)
    target = offset;
  else if (whence == SEEK_CUR)
    target = logical_pos(f) + offset;
  else
    target = (long)lseek(f->_file, 0, SEEK_END) + offset;
  long r = (long)lseek(f->_file, target, SEEK_SET);
  f->_p = f->_bf_base;
  f->_r = 0;
  f->_flags &= ~B_SEOF;
  return r < 0 ? -1 : 0;
}
static long b_ftell(void *stream) { return logical_pos((bFILE *)stream); }
static void b_rewind(void *stream) {
  b_fseek(stream, 0, SEEK_SET);
  ((bFILE *)stream)->_flags &= ~(B_SEOF | B_SERR);
}

static char *b_fgets(char *s, int size, void *stream) {
  int i = 0;
  if (size <= 0)
    return NULL;
  while (i < size - 1) {
    int c = b_fgetc(stream);
    if (c < 0) {
      if (i == 0)
        return NULL;
      break;
    }
    s[i++] = (char)c;
    if (c == '\n')
      break;
  }
  s[i] = 0;
  return s;
}

static int b_feof(void *stream) {
  return (((bFILE *)stream)->_flags & B_SEOF) ? 1 : 0;
}
static int b_ferror(void *stream) {
  return (((bFILE *)stream)->_flags & B_SERR) ? 1 : 0;
}
static void b_clearerr(void *stream) {
  ((bFILE *)stream)->_flags &= ~(B_SEOF | B_SERR);
}
static int b_fflush(void *stream) {
  (void)stream;
  return 0; /* escrita é sem buffer */
}
static int b_setvbuf(void *stream, char *buf, int mode, size_t size) {
  (void)stream;
  (void)buf;
  (void)mode;
  (void)size;
  return 0;
}
static int b_fileno(void *stream) { return ((bFILE *)stream)->_file; }

static int vwrite_fd(int fd, const char *fmt, va_list ap) {
  char stackbuf[2048];
  va_list copy;
  int n;
  va_copy(copy, ap);
  n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, copy);
  va_end(copy);
  if (n < 0)
    return -1;
  if (n < (int)sizeof(stackbuf)) {
    ssize_t w = write(fd, stackbuf, (size_t)n);
    (void)w;
    return n;
  }
  {
    char *big = malloc((size_t)n + 1);
    if (!big)
      return -1;
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    ssize_t w = write(fd, big, (size_t)n);
    (void)w;
    free(big);
  }
  return n;
}

static int b_fprintf(void *stream, const char *fmt, ...) {
  bFILE *f = stream;
  va_list ap;
  int r;
  if (game_console(f->_file))
    return 0;
  va_start(ap, fmt);
  r = vwrite_fd(f->_file, fmt, ap);
  va_end(ap);
  return r;
}
static int b_vfprintf(void *stream, const char *fmt, va_list ap) {
  bFILE *f = stream;
  if (game_console(f->_file))
    return 0;
  return vwrite_fd(f->_file, fmt, ap);
}
static int b_printf(const char *fmt, ...) {
  va_list ap;
  char line[2048];
  if (!ab_env_int("AB_GAMELOG", 0))
    return 0;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  ab_log("%s", line);
  return (int)strlen(line);
}
static int b_vprintf(const char *fmt, va_list ap) {
  char line[2048];
  if (!ab_env_int("AB_GAMELOG", 0))
    return 0;
  vsnprintf(line, sizeof(line), fmt, ap);
  ab_log("%s", line);
  return (int)strlen(line);
}
static void b_perror(const char *s) {
  ab_log("[stdio] perror: %s", s ? s : "");
}

static int b_remove(const char *path) { return unlink(path); }

static void *b_tmpfile(void) {
  char template[1200];
  int fd;
  snprintf(template, sizeof(template), "%s/abXXXXXX", ab_cachedir());
  fd = mkstemp(template);
  if (fd < 0)
    return NULL;
  unlink(template);
  return alloc_file(fd, 1);
}

/* wide-char sobre FILE: a STL importa, o jogo não usa. Stubs seguros. */
static int b_putwc(unsigned wc, void *stream) {
  (void)stream;
  return (int)wc;
}
static int b_getwc(void *stream) {
  (void)stream;
  return -1;
}
static int b_ungetwc(unsigned wc, void *stream) {
  (void)stream;
  return (int)wc;
}

static int b_fseeko(void *stream, long offset, int whence) {
  return b_fseek(stream, offset, whence);
}
static long b_ftello(void *stream) { return b_ftell(stream); }

nxloader_result ab_add_stdio_provider(nxloader_registry *registry) {
  static nxloader_symbol symbols[48];
  size_t count = 0;
  nxloader_provider provider;
  std_init();

#define ADD(sym_name, sym_addr)                     \
  do {                                              \
    symbols[count].name = (sym_name);               \
    symbols[count].address = (uintptr_t)(sym_addr); \
    symbols[count].flags = 0;                       \
    count++;                                        \
  } while (0)

  ADD("fopen", b_fopen);
  ADD("fdopen", b_fdopen);
  ADD("freopen", b_freopen);
  ADD("fclose", b_fclose);
  ADD("fread", b_fread);
  ADD("fwrite", b_fwrite);
  ADD("fgetc", b_fgetc);
  ADD("getc", b_fgetc);
  ADD("fputc", b_fputc);
  ADD("putc", b_fputc);
  ADD("fputs", b_fputs);
  ADD("puts", b_puts);
  ADD("fgets", b_fgets);
  ADD("ungetc", b_ungetc);
  ADD("__srget", b_srget);
  ADD("fseek", b_fseek);
  ADD("fseeko", b_fseeko);
  ADD("ftell", b_ftell);
  ADD("ftello", b_ftello);
  ADD("rewind", b_rewind);
  ADD("feof", b_feof);
  ADD("ferror", b_ferror);
  ADD("clearerr", b_clearerr);
  ADD("fflush", b_fflush);
  ADD("setvbuf", b_setvbuf);
  ADD("fileno", b_fileno);
  ADD("fprintf", b_fprintf);
  ADD("vfprintf", b_vfprintf);
  ADD("printf", b_printf);
  ADD("vprintf", b_vprintf);
  ADD("perror", b_perror);
  ADD("remove", b_remove);
  ADD("tmpfile", b_tmpfile);
  ADD("putwc", b_putwc);
  ADD("getwc", b_getwc);
  ADD("ungetwc", b_ungetwc);
#undef ADD

  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "angrybirds-stdio";
  provider.symbols = symbols;
  provider.symbol_count = count;
  provider.priority = 30;
  return nxloader_registry_add_provider(registry, &provider, NULL);
}
