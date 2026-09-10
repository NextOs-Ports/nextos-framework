/* ab_bionic.c — ponte de ABI bionic→glibc para libAngryBirdsClassic.so.
 *
 * A .so importa 351 símbolos. A esmagadora maioria tem ABI idêntica entre
 * bionic e glibc no ARM32 e passa direto. Este arquivo trata só o que DIVERGE:
 *
 *   struct stat/statfs   -> layout do kernel = struct stat64/statfs64 do glibc
 *   struct dirent        -> bionic usa d_ino/d_off de 64 bits
 *   struct addrinfo      -> bionic troca ai_canonname com ai_addr
 *   struct passwd        -> bionic não tem pw_gecos
 *   sysconf              -> as constantes _SC_* têm valores diferentes
 *   setjmp/longjmp       -> o jmp_buf do glibc (388 B) não cabe no do bionic
 *                           (256 B); salvamos os registradores nós mesmos
 *   sigaction/sigprocmask-> sigset_t de 4 B vs 128 B; o jogo só usa isso pro
 *                           breakpad do HockeyApp, que não queremos
 *   _ctype_/_tolower_tab_/_toupper_tab_ -> símbolos de DADO que guardam um
 *                           PONTEIRO pra tabela indexada a partir de -1 (BSD)
 *   __sF                 -> array de 3 FILE no layout __sFILE do BSD (84 B)
 *   __aeabi_memset       -> ordem de argumentos (dest, n, c)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <libgen.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <poll.h>
#include <locale.h>
#include <utime.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <signal.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "ab_port.h"

/* ==================== log do Android ==================== */

static int ab_android_log_print(int prio, const char *tag, const char *fmt,
                                ...) {
  char line[1024];
  va_list ap;
  (void)prio;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  ab_log("[%s] %s", tag ? tag : "?", line);
  return 0;
}

static int ab_android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  ab_log("[%s] %s", tag ? tag : "?", text ? text : "");
  return 0;
}

/* ==================== propriedades do sistema ==================== */

static int ab_system_property_get(const char *name, char *value) {
  const char *out = "";
  if (!name || !value)
    return 0;
  if (strcmp(name, "ro.build.version.sdk") == 0)
    out = "26";
  else if (strcmp(name, "ro.build.version.release") == 0)
    out = "8.0.0";
  else if (strcmp(name, "ro.product.model") == 0)
    out = "NextOS";
  else if (strcmp(name, "ro.product.manufacturer") == 0)
    out = "NextOS";
  else if (strcmp(name, "ro.product.device") == 0)
    out = "nextos";
  else if (strcmp(name, "ro.product.cpu.abi") == 0)
    out = "armeabi-v7a";
  strcpy(value, out);
  return (int)strlen(out);
}

/* ==================== struct stat / statfs ==================== */

static int ab_stat(const char *path, void *buf) {
  return stat64(path, (struct stat64 *)buf);
}
static int ab_fstat(int fd, void *buf) {
  return fstat64(fd, (struct stat64 *)buf);
}
static int ab_lstat(const char *path, void *buf) {
  return lstat64(path, (struct stat64 *)buf);
}
static int ab_statfs(const char *path, void *buf) {
  return statfs64(path, (struct statfs64 *)buf);
}

/* ==================== dirent ==================== */

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256];
};

static int ab_readdir_r(DIR *dir, struct bionic_dirent *entry,
                        struct bionic_dirent **result) {
  struct dirent *host;
  if (!dir || !entry || !result)
    return EINVAL;
  errno = 0;
  host = readdir(dir);
  if (!host) {
    *result = NULL;
    return errno;
  }
  entry->d_ino = (uint64_t)host->d_ino;
  entry->d_off = (int64_t)host->d_off;
  entry->d_reclen = (unsigned short)sizeof(*entry);
  entry->d_type = host->d_type;
  snprintf(entry->d_name, sizeof(entry->d_name), "%s", host->d_name);
  *result = entry;
  return 0;
}

/* ==================== addrinfo (campos trocados) ==================== */

struct bionic_addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  socklen_t ai_addrlen;
  char *ai_canonname;
  struct sockaddr *ai_addr;
  struct bionic_addrinfo *ai_next;
};

static void ab_freeaddrinfo(struct bionic_addrinfo *list) {
  while (list) {
    struct bionic_addrinfo *next = list->ai_next;
    free(list->ai_canonname);
    free(list->ai_addr);
    free(list);
    list = next;
  }
}

static int ab_getaddrinfo(const char *node, const char *service,
                          const struct bionic_addrinfo *hints,
                          struct bionic_addrinfo **out) {
  struct addrinfo host_hints;
  struct addrinfo *host_result = NULL;
  struct bionic_addrinfo *head = NULL, *tail = NULL;
  int rc;
  memset(&host_hints, 0, sizeof(host_hints));
  if (hints) {
    host_hints.ai_flags = hints->ai_flags;
    host_hints.ai_family = hints->ai_family;
    host_hints.ai_socktype = hints->ai_socktype;
    host_hints.ai_protocol = hints->ai_protocol;
  }
  rc = getaddrinfo(node, service, hints ? &host_hints : NULL, &host_result);
  if (rc != 0) {
    if (out)
      *out = NULL;
    return rc;
  }
  for (struct addrinfo *it = host_result; it; it = it->ai_next) {
    struct bionic_addrinfo *node_out = calloc(1, sizeof(*node_out));
    if (!node_out)
      break;
    node_out->ai_flags = it->ai_flags;
    node_out->ai_family = it->ai_family;
    node_out->ai_socktype = it->ai_socktype;
    node_out->ai_protocol = it->ai_protocol;
    node_out->ai_addrlen = it->ai_addrlen;
    if (it->ai_canonname)
      node_out->ai_canonname = strdup(it->ai_canonname);
    if (it->ai_addr && it->ai_addrlen) {
      node_out->ai_addr = malloc(it->ai_addrlen);
      if (node_out->ai_addr)
        memcpy(node_out->ai_addr, it->ai_addr, it->ai_addrlen);
    }
    if (tail)
      tail->ai_next = node_out;
    else
      head = node_out;
    tail = node_out;
  }
  freeaddrinfo(host_result);
  if (out)
    *out = head;
  else
    ab_freeaddrinfo(head);
  return 0;
}

/* ==================== passwd (sem pw_gecos no bionic) ==================== */

struct bionic_passwd {
  char *pw_name;
  char *pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  char *pw_dir;
  char *pw_shell;
};

static struct bionic_passwd g_passwd;

static struct bionic_passwd *ab_getpwuid(uid_t uid) {
  struct passwd *host = getpwuid(uid);
  if (!host)
    return NULL;
  g_passwd.pw_name = host->pw_name;
  g_passwd.pw_passwd = host->pw_passwd;
  g_passwd.pw_uid = host->pw_uid;
  g_passwd.pw_gid = host->pw_gid;
  g_passwd.pw_dir = host->pw_dir;
  g_passwd.pw_shell = host->pw_shell;
  return &g_passwd;
}

/* ==================== sysconf (constantes diferentes) ==================== */

static long ab_sysconf(int name) {
  switch (name) {
  case 0x0000: /* _SC_ARG_MAX */
    return sysconf(_SC_ARG_MAX);
  case 0x0005: /* _SC_CHILD_MAX */
    return sysconf(_SC_CHILD_MAX);
  case 0x0006: /* _SC_CLK_TCK */
    return sysconf(_SC_CLK_TCK);
  case 0x000b: /* _SC_OPEN_MAX */
    return sysconf(_SC_OPEN_MAX);
  case 0x0027: /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    return sysconf(_SC_PAGESIZE);
  case 0x0060: /* _SC_NPROCESSORS_CONF */
    return sysconf(_SC_NPROCESSORS_CONF);
  case 0x0061: /* _SC_NPROCESSORS_ONLN */
    return sysconf(_SC_NPROCESSORS_ONLN);
  case 0x0062: /* _SC_PHYS_PAGES */
    return sysconf(_SC_PHYS_PAGES);
  case 0x0063: /* _SC_AVPHYS_PAGES */
    return sysconf(_SC_AVPHYS_PAGES);
  default:
    ab_log("[bionic] sysconf(%d) desconhecido", name);
    return -1;
  }
}

static long ab_pathconf(const char *path, int name) {
  (void)path;
  if (name == 4) /* _PC_NAME_MAX no bionic */
    return 255;
  return 255;
}

/* ==================== setjmp / longjmp próprios ====================
 * O jmp_buf do bionic no ARM32 tem 64 palavras (256 B). O do glibc tem 388 B
 * e estouraria o slot que o jogo (libpng, principalmente) aloca inline.
 * Salvamos r4-r11, sp, lr e d8-d15 à mão: 13*4 + 8*8 = 116 B, cabe folgado.
 */
__asm__(".text\n"
        ".align 2\n"
        ".globl ab_setjmp\n"
        ".type ab_setjmp, %function\n"
        "ab_setjmp:\n"
        "  stmia r0!, {r4-r11}\n"
        "  str sp, [r0], #4\n"
        "  str lr, [r0], #4\n"
        "  vstmia r0!, {d8-d15}\n"
        "  mov r0, #0\n"
        "  bx lr\n"
        ".size ab_setjmp, .-ab_setjmp\n"
        ".align 2\n"
        ".globl ab_longjmp\n"
        ".type ab_longjmp, %function\n"
        "ab_longjmp:\n"
        "  ldmia r0!, {r4-r11}\n"
        "  ldr sp, [r0], #4\n"
        "  ldr lr, [r0], #4\n"
        "  vldmia r0!, {d8-d15}\n"
        "  movs r0, r1\n"
        "  it eq\n"
        "  moveq r0, #1\n"
        "  bx lr\n"
        ".size ab_longjmp, .-ab_longjmp\n");
extern int ab_setjmp(void *buf);
extern void ab_longjmp(void *buf, int value);

/* ==================== sinais: nunca instalar ====================
 * sigset_t no bionic tem 4 bytes e no glibc 128; além disso o único uso aqui
 * é o breakpad do HockeyApp, que não queremos capturando SIGSEGV nosso.
 */
static int ab_sigaction(int signum, const void *act, void *old) {
  (void)signum;
  (void)act;
  if (old)
    memset(old, 0, 16);
  return 0;
}
static int ab_sigprocmask(int how, const void *set, void *old) {
  (void)how;
  (void)set;
  if (old)
    memset(old, 0, 4);
  return 0;
}

/* ==================== tabelas de ctype no estilo BSD ==================== */

static char g_ctype_table[257];
static short g_tolower_table[257];
static short g_toupper_table[257];
const char *_ab_ctype_ptr;
const short *_ab_tolower_ptr;
const short *_ab_toupper_ptr;
uintptr_t _ab_stack_chk_guard = 0x5a5ac3c3u;

static void init_ctype_tables(void) {
  g_ctype_table[0] = 0;
  g_tolower_table[0] = -1;
  g_toupper_table[0] = -1;
  for (int c = 0; c < 256; c++) {
    char flags = 0;
    if (isupper(c))
      flags |= 0x01; /* _U */
    if (islower(c))
      flags |= 0x02; /* _L */
    if (isdigit(c))
      flags |= 0x04; /* _N */
    if (isspace(c))
      flags |= 0x08; /* _S */
    if (ispunct(c))
      flags |= 0x10; /* _P */
    if (iscntrl(c))
      flags |= 0x20; /* _C */
    if (isxdigit(c))
      flags |= 0x40; /* _X */
    if (c == ' ')
      flags |= 0x80; /* _B */
    g_ctype_table[c + 1] = flags;
    g_tolower_table[c + 1] = (short)tolower(c);
    g_toupper_table[c + 1] = (short)toupper(c);
  }
  _ab_ctype_ptr = g_ctype_table;
  _ab_tolower_ptr = g_tolower_table;
  _ab_toupper_ptr = g_toupper_table;
}

/* ==================== rotinas __aeabi_ ==================== */

static void ab_aeabi_memcpy(void *dst, const void *src, size_t n) {
  memcpy(dst, src, n);
}
static void ab_aeabi_memmove(void *dst, const void *src, size_t n) {
  memmove(dst, src, n);
}
/* ATENÇÃO: a ordem do EABI é (dest, n, c), não (dest, c, n). */
static void ab_aeabi_memset(void *dst, size_t n, int c) { memset(dst, c, n); }
static void ab_aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }

/* ==================== diversos ==================== */

static int *ab_errno(void) { return __errno_location(); }

static void ab_stack_chk_fail(void) {
  ab_log("[bionic] __stack_chk_fail no convidado");
  abort();
}

static void ab_noop(void) {}

/* O convidado registra destrutores globais que nunca queremos executar no
 * teardown (a lição do Chrono: o SIGSEGV mora no desmonte, não no jogo). */
static int ab_cxa_atexit(void (*fn)(void *), void *arg, void *handle) {
  (void)fn;
  (void)arg;
  (void)handle;
  return 0;
}
static void ab_cxa_finalize(void *handle) { (void)handle; }

static void *ab_dlopen(const char *name, int flags) {
  (void)flags;
  /* libjs.so (SpiderMonkey) e libadcolony.so são de anúncio: recusar é o
   * comportamento desejado e o jogo já trata falha de dlopen. */
  ab_log("[bionic] dlopen(%s) recusado (periferia de anuncio)",
         name ? name : "?");
  return NULL;
}
static void *ab_dlsym(void *handle, const char *name) {
  (void)handle;
  (void)name;
  return NULL;
}
static int ab_dlclose(void *handle) {
  (void)handle;
  return 0;
}
static const char *ab_dlerror(void) { return "unavailable"; }

static void ab_exit(int code) {
  ab_log("[bionic] convidado chamou exit(%d)", code);
  ab_log_close();
  _exit(code);
}
static void ab_abort(void) {
  ab_log("[bionic] convidado chamou abort()");
  ab_log_close();
  _exit(134);
}

/* lrint/lrintf recebem double/float por valor: precisam da borda softfp. */
#define AB_SOFTFP __attribute__((pcs("aapcs")))
AB_SOFTFP static long ab_lrint(double x) { return lrint(x); }
AB_SOFTFP static long ab_lrintf(float x) { return lrintf(x); }

/* ==================== EHABI ==================== */

typedef struct {
  uintptr_t table;
  int count;
} ExidxResult;

static void *ab_find_exidx(uintptr_t pc, int *count) {
  uintptr_t table = 0;
  size_t entries = 0;
  if (ab_guest &&
      nxloader_module_find_arm_exidx(ab_guest, pc, &table, &entries) ==
          NXLOADER_OK) {
    if (count)
      *count = (int)entries;
    return (void *)table;
  }
  if (count)
    *count = 0;
  return NULL;
}

/* ==================== AAssetManager ==================== */

typedef struct AbAsset {
  const void *data;
  size_t size;
  int owned;
} AbAsset;

static int g_asset_manager_token = 0xA55E7;

static void *ab_asset_manager_from_java(void *env, void *obj) {
  (void)env;
  (void)obj;
  return &g_asset_manager_token;
}

static void *ab_asset_open(void *manager, const char *name, int mode) {
  size_t size = 0;
  int owned = 0;
  const void *data;
  (void)manager;
  (void)mode;
  data = ab_apk_asset(name, &size, &owned);
  if (!data) {
    if (ab_env_int("AB_ASSET_LOG", 0))
      ab_log("[asset] MISS %s", name ? name : "?");
    return NULL;
  }
  {
    AbAsset *asset = malloc(sizeof(*asset));
    if (!asset) {
      if (owned)
        free((void *)data);
      return NULL;
    }
    asset->data = data;
    asset->size = size;
    asset->owned = owned;
    if (ab_env_int("AB_ASSET_LOG", 0))
      ab_log("[asset] OK %s (%zu B%s)", name, size, owned ? ", inflado" : "");
    return asset;
  }
}

static const void *ab_asset_get_buffer(void *handle) {
  AbAsset *asset = handle;
  return asset ? asset->data : NULL;
}

static int64_t ab_asset_get_length64(void *handle) {
  AbAsset *asset = handle;
  return asset ? (int64_t)asset->size : 0;
}

static int ab_asset_get_length(void *handle) {
  AbAsset *asset = handle;
  return asset ? (int)asset->size : 0;
}

static void ab_asset_close(void *handle) {
  AbAsset *asset = handle;
  if (!asset)
    return;
  if (asset->owned)
    free((void *)asset->data);
  free(asset);
}

/* ==================== tabela ==================== */

extern void *ab_bstdio_sF(void); /* de ab_stdio.c */

#define S(name, address) {name, (uintptr_t)(address), 0}
#define P(name) {#name, (uintptr_t)&name, 0}

nxloader_result ab_add_bionic_provider(nxloader_registry *registry) {
  static nxloader_symbol symbols[400];
  size_t count = 0;
  nxloader_provider provider;
  init_ctype_tables();

#define ADD(sym_name, sym_addr)                          \
  do {                                                   \
    symbols[count].name = (sym_name);                    \
    symbols[count].address = (uintptr_t)(sym_addr);      \
    symbols[count].flags = 0;                            \
    count++;                                             \
  } while (0)
#define ADD_DIRECT(fn) ADD(#fn, &fn)

  /* --- específicos do Android --- */
  ADD("__android_log_print", ab_android_log_print);
  ADD("__android_log_write", ab_android_log_write);
  ADD("__system_property_get", ab_system_property_get);
  ADD("AAssetManager_fromJava", ab_asset_manager_from_java);
  ADD("AAssetManager_open", ab_asset_open);
  ADD("AAsset_getBuffer", ab_asset_get_buffer);
  ADD("AAsset_getLength", ab_asset_get_length);
  ADD("AAsset_getLength64", ab_asset_get_length64);
  ADD("AAsset_close", ab_asset_close);
  ADD("__gnu_Unwind_Find_exidx", ab_find_exidx);
  ADD("__google_potentially_blocking_region_begin", ab_noop);
  ADD("__google_potentially_blocking_region_end", ab_noop);

  /* --- estruturas divergentes --- */
  ADD("stat", ab_stat);
  ADD("fstat", ab_fstat);
  ADD("lstat", ab_lstat);
  ADD("statfs", ab_statfs);
  ADD("readdir_r", ab_readdir_r);
  ADD("getaddrinfo", ab_getaddrinfo);
  ADD("freeaddrinfo", ab_freeaddrinfo);
  ADD("getpwuid", ab_getpwuid);
  ADD("sysconf", ab_sysconf);
  ADD("pathconf", ab_pathconf);
  ADD("setjmp", ab_setjmp);
  ADD("_setjmp", ab_setjmp);
  ADD("longjmp", ab_longjmp);
  ADD("_longjmp", ab_longjmp);
  ADD("sigaction", ab_sigaction);
  ADD("sigprocmask", ab_sigprocmask);

  /* --- dados --- */
  ADD("_ctype_", &_ab_ctype_ptr);
  ADD("_tolower_tab_", &_ab_tolower_ptr);
  ADD("_toupper_tab_", &_ab_toupper_ptr);
  ADD("__stack_chk_guard", &_ab_stack_chk_guard);
  ADD("__stack_chk_fail", ab_stack_chk_fail);
  ADD("__sF", ab_bstdio_sF());

  /* --- EABI --- */
  ADD("__aeabi_memcpy", ab_aeabi_memcpy);
  ADD("__aeabi_memcpy4", ab_aeabi_memcpy);
  ADD("__aeabi_memcpy8", ab_aeabi_memcpy);
  ADD("__aeabi_memmove", ab_aeabi_memmove);
  ADD("__aeabi_memset", ab_aeabi_memset);
  ADD("__aeabi_memset4", ab_aeabi_memset);
  ADD("__aeabi_memset8", ab_aeabi_memset);
  ADD("__aeabi_memclr", ab_aeabi_memclr);
  ADD("__aeabi_memclr4", ab_aeabi_memclr);
  ADD("__aeabi_memclr8", ab_aeabi_memclr);

  /* --- comportamento nosso --- */
  ADD("dlopen", ab_dlopen);
  ADD("dlsym", ab_dlsym);
  ADD("dlclose", ab_dlclose);
  ADD("dlerror", ab_dlerror);
  ADD("exit", ab_exit);
  ADD("_exit", ab_exit);
  ADD("abort", ab_abort);
  ADD("__errno", ab_errno);
  ADD("lrint", ab_lrint);
  ADD("lrintf", ab_lrintf);

  /* --- passagem direta (ABI idêntica no ARM32) --- */
  ADD_DIRECT(access);
  ADD_DIRECT(atoi);
  ADD_DIRECT(atol);
  ADD_DIRECT(basename);
  ADD_DIRECT(bind);
  ADD_DIRECT(bsearch);
  ADD_DIRECT(btowc);
  ADD_DIRECT(calloc);
  ADD_DIRECT(chmod);
  ADD_DIRECT(clock);
  ADD_DIRECT(clock_gettime);
  ADD_DIRECT(close);
  ADD_DIRECT(closedir);
  ADD_DIRECT(connect);
  ADD("__cxa_atexit", ab_cxa_atexit);
  ADD("__cxa_finalize", ab_cxa_finalize);
  ADD_DIRECT(difftime);
  ADD_DIRECT(dup);
  ADD_DIRECT(fcntl);
  ADD_DIRECT(fnmatch);
  ADD_DIRECT(free);
  ADD_DIRECT(fsync);
  ADD_DIRECT(ftruncate);
  ADD_DIRECT(gai_strerror);
  ADD_DIRECT(getauxval);
  ADD_DIRECT(getcwd);
  ADD_DIRECT(getenv);
  ADD_DIRECT(geteuid);
  ADD_DIRECT(gethostname);
  ADD_DIRECT(getpeername);
  ADD_DIRECT(getpid);
  ADD_DIRECT(getservbyport);
  ADD_DIRECT(getsockname);
  ADD_DIRECT(getsockopt);
  ADD_DIRECT(gettimeofday);
  ADD_DIRECT(gmtime);
  ADD_DIRECT(gmtime_r);
  ADD_DIRECT(if_indextoname);
  ADD_DIRECT(if_nametoindex);
  ADD_DIRECT(inet_addr);
  ADD_DIRECT(inet_ntop);
  ADD_DIRECT(inet_pton);
  ADD_DIRECT(ioctl);
  ADD_DIRECT(isdigit);
  ADD_DIRECT(isspace);
  ADD_DIRECT(isupper);
  ADD_DIRECT(isxdigit);
  ADD_DIRECT(iswctype);
  ADD_DIRECT(localtime);
  ADD_DIRECT(localtime_r);
  ADD_DIRECT(lrand48);
  ADD_DIRECT(lseek);
  ADD_DIRECT(malloc);
  ADD_DIRECT(mbrtowc);
  ADD_DIRECT(memchr);
  ADD_DIRECT(memcmp);
  ADD_DIRECT(memcpy);
  ADD_DIRECT(memmem);
  ADD_DIRECT(memmove);
  ADD_DIRECT(memrchr);
  ADD_DIRECT(memset);
  ADD_DIRECT(mkdir);
  ADD_DIRECT(mkstemp);
  ADD_DIRECT(mktime);
  ADD_DIRECT(mmap);
  ADD_DIRECT(munmap);
  ADD_DIRECT(nanosleep);
  ADD_DIRECT(open);
  ADD_DIRECT(opendir);
  ADD_DIRECT(pipe);
  ADD_DIRECT(poll);
  ADD_DIRECT(qsort);
  ADD_DIRECT(raise);
  ADD_DIRECT(read);
  ADD_DIRECT(realloc);
  ADD_DIRECT(recv);
  ADD_DIRECT(recvfrom);
  ADD_DIRECT(rename);
  ADD_DIRECT(rmdir);
  ADD_DIRECT(sched_yield);
  ADD_DIRECT(send);
  ADD_DIRECT(setlocale);
  ADD_DIRECT(setsockopt);
  ADD_DIRECT(snprintf);
  ADD_DIRECT(socket);
  ADD_DIRECT(sprintf);
  ADD_DIRECT(srand48);
  ADD_DIRECT(sscanf);
  ADD_DIRECT(strcasecmp);
  ADD_DIRECT(strcat);
  ADD_DIRECT(strchr);
  ADD_DIRECT(strcmp);
  ADD_DIRECT(strcoll);
  ADD_DIRECT(strcpy);
  ADD_DIRECT(strcspn);
  ADD_DIRECT(strdup);
  ADD_DIRECT(strerror);
  ADD_DIRECT(strerror_r);
  ADD_DIRECT(strftime);
  ADD_DIRECT(strlen);
  ADD_DIRECT(strncasecmp);
  ADD_DIRECT(strncat);
  ADD_DIRECT(strncmp);
  ADD_DIRECT(strncpy);
  ADD_DIRECT(strpbrk);
  ADD_DIRECT(strrchr);
  ADD_DIRECT(strspn);
  ADD_DIRECT(strstr);
  ADD_DIRECT(strtok_r);
  ADD_DIRECT(strtol);
  ADD_DIRECT(strtoll);
  ADD_DIRECT(strtoul);
  ADD_DIRECT(strtoull);
  ADD_DIRECT(strxfrm);
  ADD_DIRECT(syscall);
  ADD_DIRECT(time);
  ADD_DIRECT(tolower);
  ADD_DIRECT(towlower);
  ADD_DIRECT(towupper);
  ADD_DIRECT(uname);
  ADD_DIRECT(unlink);
  ADD_DIRECT(utime);
  ADD_DIRECT(vsnprintf);
  ADD_DIRECT(vsprintf);
  ADD_DIRECT(wcrtomb);
  ADD_DIRECT(wcscoll);
  ADD_DIRECT(wcsftime);
  ADD_DIRECT(wcslen);
  ADD_DIRECT(wcsxfrm);
  ADD_DIRECT(wctob);
  ADD_DIRECT(wctype);
  ADD_DIRECT(wmemchr);
  ADD_DIRECT(wmemcmp);
  ADD_DIRECT(wmemcpy);
  ADD_DIRECT(wmemmove);
  ADD_DIRECT(wmemset);
  ADD_DIRECT(write);
  ADD_DIRECT(writev);
#undef ADD_DIRECT
#undef ADD

  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "angrybirds-bionic";
  provider.symbols = symbols;
  provider.symbol_count = count;
  provider.priority = 10;
  return nxloader_registry_add_provider(registry, &provider, NULL);
}
