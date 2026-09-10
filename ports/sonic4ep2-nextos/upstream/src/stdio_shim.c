/* stdio_shim.c -- ponte stdio bionic -> glibc + tabelas ctype + stubs.
 *
 * O jogo (bionic) referencia `__sF` (array de FILE p/ stdin/stdout/stderr,
 * stride sizeof(bionic FILE)=84) e chama fwrite/fputs/fputc/fread/fseek/ftell/
 * fclose com esses ponteiros. Sob glibc esses FILE* não são glibc FILE* válidos
 * -> deref/crash (o construtor de std::ios_base::Init lia __sF[1]+off=0x70 com
 * base 0 -> SIGSEGV). Provemos __sF como storage real e interceptamos as funções
 * stdio: se o FILE* cai dentro de __sF, mapeamos p/ a stream glibc; senão passa
 * direto (ex.: FILE* de fdopen). Também provê _ctype_/_toupper_tab_ (bionic) e
 * stubs de profiling.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "so_util.h"

/* bionic __sFILE em 32-bit = 84 bytes; 3 entradas (stdin/stdout/stderr). */
#define BIONIC_FILE_SZ 84
unsigned char __sF[BIONIC_FILE_SZ * 3];

static FILE *map_file(void *f) {
  uintptr_t p = (uintptr_t)f, base = (uintptr_t)__sF;
  if (p >= base && p < base + sizeof(__sF)) {
    int idx = (int)((p - base) / BIONIC_FILE_SZ);
    if (idx == 0) return stdin;
    if (idx == 1) return stdout;
    return stderr;
  }
  return (FILE *)f; /* FILE* real do glibc (fdopen etc.) */
}

/* 💾 durabilidade do save: rastreia o FILE* do foxsave aberto p/ escrita e dá
   fsync no fclose -> save chega ao SD antes de um power-off (handheld). Só esse
   arquivo, sem pesar o I/O dos demais. */
static FILE *g_save_fp;
void *b_fopen(const char *path, const char *mode) {
  FILE *fp = fopen(path, mode);
  if (fp && path && strstr(path, "foxsave") && mode && (strchr(mode, 'w') || strchr(mode, 'a')))
    g_save_fp = fp;
  if (getenv("SONIC_IOLOG"))
    fprintf(stderr, "[b_fopen] %s mode=%s -> %p%s\n",
            path ? path : "(null)", mode ? mode : "(null)", (void *)fp,
            (fp == g_save_fp && fp) ? " [SAVE]" : "");
  return fp;
}
size_t b_fwrite(const void *ptr, size_t sz, size_t n, void *f) {
  return fwrite(ptr, sz, n, map_file(f));
}
size_t b_fread(void *ptr, size_t sz, size_t n, void *f) {
  FILE *m = map_file(f);
  size_t r = fread(ptr, sz, n, m);
  if (getenv("SONIC_IOLOG") && (sz * n) <= 64)
    fprintf(stderr, "[b_fread] f=%p map=%p sz=%zu n=%zu -> %zu (err=%d eof=%d)\n",
            f, (void *)m, sz, n, r, m ? ferror(m) : -1, m ? feof(m) : -1);
  return r;
}
int b_fputs(const char *s, void *f) { return fputs(s, map_file(f)); }
int b_fputc(int c, void *f) { return fputc(c, map_file(f)); }
int b_fgetc(void *f) { return fgetc(map_file(f)); }
int b_fseek(void *f, long off, int wh) { return fseek(map_file(f), off, wh); }
long b_ftell(void *f) { return ftell(map_file(f)); }
int b_fflush(void *f) { return fflush(f ? map_file(f) : NULL); }
int b_fclose(void *f) {
  FILE *r = map_file(f);
  if (r == stdin || r == stdout || r == stderr) return 0; /* nunca fechar std */
  if (r == g_save_fp && r) { /* save durável: força pro SD antes de fechar */
    fflush(r);
    int fd = fileno(r);
    if (fd >= 0) fsync(fd);
    g_save_fp = NULL;
    if (getenv("SONIC_IOLOG")) fprintf(stderr, "[b_fclose] SAVE fsync ok\n");
  }
  return fclose(r);
}

/* ---------------- ctype (bionic) ----------------
 * bionic declara `extern const char* _ctype_;` e `const short* _toupper_tab_;`
 * (PONTEIROS p/ tabelas; macro indexa [(c)+1], c em -1..255). bits do _ctype_:
 * _U=1 _L=2 _N=4 _S=8 _P=16 _C=32 _X=64 _B=128. */
static char _ctype_table[1 + 256];
static short _toupper_table[1 + 256];
static short _tolower_table[1 + 256];
const char *_ctype_ = _ctype_table;
const short *_toupper_tab_ = _toupper_table;
const short *_tolower_tab_ = _tolower_table;
__attribute__((constructor)) static void ctype_init(void) {
  _ctype_table[0] = 0;
  _toupper_table[0] = -1;
  _tolower_table[0] = -1;
  for (int c = 0; c < 256; c++) {
    unsigned v = 0;
    if (isupper(c)) v |= 0x01;
    if (islower(c)) v |= 0x02;
    if (isdigit(c)) v |= 0x04;
    if (isspace(c)) v |= 0x08;
    if (ispunct(c)) v |= 0x10;
    if (iscntrl(c)) v |= 0x20;
    if (isxdigit(c)) v |= 0x40;
    if (c == ' ') v |= 0x80;
    _ctype_table[c + 1] = (char)v;
    _toupper_table[c + 1] = (short)toupper(c);
    _tolower_table[c + 1] = (short)tolower(c);
  }
}

/* ---------------- stubs ---------------- */
void __google_potentially_blocking_region_begin(void) {}
void __google_potentially_blocking_region_end(void) {}
int AKeyEvent_getRepeatCount(void *event) { (void)event; return 0; }
