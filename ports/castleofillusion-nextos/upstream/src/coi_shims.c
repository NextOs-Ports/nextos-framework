/*
 * coi_shims.c -- resolvers que faltavam pro Castle of Illusion (libViewer_GP.so)
 * e que nem o imports.c (base do scaffold) nem o android_shim tinham:
 *   - _ctype_ bionic (tabela de ponteiro)
 *   - AConfiguration_* que faltavam (defaults sensatos)
 *   - fortify bionic __*_chk -> versoes reais glibc
 *   - ANativeActivity_setWindowFlags / ANativeWindow_setBuffersGeometry
 * Exporta coi_extra[] p/ o main concatenar na tabela base.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "so_util.h"

/* ---- bionic _ctype_ (ponteiro p/ tabela [(c)+1], c em -1..255) ---- */
static char coi_ctype_table[1 + 256];
const char *coi_bionic_ctype = coi_ctype_table;
__attribute__((constructor)) static void coi_ctype_init(void) {
  coi_ctype_table[0] = 0;
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
    coi_ctype_table[c + 1] = (char)v;
  }
}

/* ---- AConfiguration_* que faltavam (defaults) ---- */
static int c_getMcc(void *c)          { (void)c; return 0; }
static int c_getMnc(void *c)          { (void)c; return 0; }
static int c_getKeyboard(void *c)     { (void)c; return 1; } /* NOKEYS */
static int c_getKeysHidden(void *c)   { (void)c; return 1; } /* NO */
static int c_getNavHidden(void *c)    { (void)c; return 1; } /* NO */
static int c_getNavigation(void *c)   { (void)c; return 1; } /* NONAV */
static int c_getScreenLong(void *c)   { (void)c; return 0; }
static int c_getSdkVersion(void *c)   { (void)c; return 26; }
static int c_getTouchscreen(void *c)  { (void)c; return 3; } /* FINGER */
static int c_getUiModeNight(void *c)  { (void)c; return 0; }
static int c_getUiModeType(void *c)   { (void)c; return 1; } /* NORMAL */

/* ---- fortify bionic -> real ---- */
static ssize_t x_write_chk(int fd, const void *buf, size_t n, size_t blen) {
  (void)blen; return write(fd, buf, n);
}
static size_t x_fwrite_chk(const void *buf, size_t sz, size_t n, FILE *f, size_t blen) {
  (void)blen; return fwrite(buf, sz, n, f);
}
static ssize_t x_sendto_chk(int s, const void *buf, size_t len, size_t blen, int flags,
                            const struct sockaddr *dst, socklen_t alen) {
  (void)blen; return sendto(s, buf, len, flags, dst, alen);
}
static char *x_strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) {
  (void)dl; (void)sl; return strncpy(d, s, n);
}
static void x_FD_SET_chk(int fd, fd_set *set, size_t sz)   { (void)sz; FD_SET(fd, set); }
static int  x_FD_ISSET_chk(int fd, fd_set *set, size_t sz) { (void)sz; return FD_ISSET(fd, set); }

/* ---- NativeActivity/NativeWindow que faltavam ---- */
static void x_setWindowFlags(void *a, unsigned add, unsigned rem) { (void)a; (void)add; (void)rem; }
static int  x_setBuffersGeometry(void *w, int wd, int ht, int fmt) {
  (void)w; (void)wd; (void)ht; (void)fmt; return 0;
}

DynLibFunction coi_extra[] = {
  {"_ctype_", (uintptr_t)&coi_bionic_ctype},
  {"AConfiguration_getMcc",         (uintptr_t)c_getMcc},
  {"AConfiguration_getMnc",         (uintptr_t)c_getMnc},
  {"AConfiguration_getKeyboard",    (uintptr_t)c_getKeyboard},
  {"AConfiguration_getKeysHidden",  (uintptr_t)c_getKeysHidden},
  {"AConfiguration_getNavHidden",   (uintptr_t)c_getNavHidden},
  {"AConfiguration_getNavigation",  (uintptr_t)c_getNavigation},
  {"AConfiguration_getScreenLong",  (uintptr_t)c_getScreenLong},
  {"AConfiguration_getSdkVersion",  (uintptr_t)c_getSdkVersion},
  {"AConfiguration_getTouchscreen", (uintptr_t)c_getTouchscreen},
  {"AConfiguration_getUiModeNight", (uintptr_t)c_getUiModeNight},
  {"AConfiguration_getUiModeType",  (uintptr_t)c_getUiModeType},
  {"__write_chk",    (uintptr_t)x_write_chk},
  {"__fwrite_chk",   (uintptr_t)x_fwrite_chk},
  {"__sendto_chk",   (uintptr_t)x_sendto_chk},
  {"__strncpy_chk2", (uintptr_t)x_strncpy_chk2},
  {"__FD_SET_chk",   (uintptr_t)x_FD_SET_chk},
  {"__FD_ISSET_chk", (uintptr_t)x_FD_ISSET_chk},
  {"ANativeActivity_setWindowFlags",   (uintptr_t)x_setWindowFlags},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)x_setBuffersGeometry},
};
const int coi_extra_count = sizeof(coi_extra) / sizeof(coi_extra[0]);
