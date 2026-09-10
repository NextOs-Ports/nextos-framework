/*
 * ot_bionic.c -- o punhado de simbolos onde a ABI do Bionic e a da glibc
 * discordam de verdade e o resto do so-loader nao cobre.
 *
 * Medido em 23/08/2026 contra as 395 UND de libgame.so (arm64-v8a):
 * o resolvedor cai em dlsym(RTLD_DEFAULT) e a glibc 2.43 do device fecha
 * quase tudo.  Sobram tres familias:
 *   - sem_t: 4 bytes no Bionic, 32 na glibc -> escrever a versao alheia
 *     estoura a struct vizinha do jogo;
 *   - setjmp/longjmp: jmp_buf de 256 bytes no Bionic vs __jmp_buf_tag de 312
 *     na glibc (o corpo mora em setjmp_bridge.S);
 *   - __emutls_get_address: o NDK emite TLS emulado; a glibc nao exporta.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int futex_wait(volatile int *p, int val) {
  return (int)syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}
static int futex_wake(volatile int *p, int n) {
  return (int)syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0);
}

int ot_sem_init(void *s, int pshared, unsigned value) {
  (void)pshared;
  __atomic_store_n((int *)s, (int)value, __ATOMIC_SEQ_CST);
  return 0;
}
int ot_sem_destroy(void *s) { (void)s; return 0; }
int ot_sem_trywait(void *s) {
  volatile int *p = (volatile int *)s;
  for (;;) {
    int old = __atomic_load_n((int *)p, __ATOMIC_SEQ_CST);
    if (old <= 0) { errno = EAGAIN; return -1; }
    if (__atomic_compare_exchange_n((int *)p, &old, old - 1, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return 0;
  }
}
int ot_sem_wait(void *s) {
  volatile int *p = (volatile int *)s;
  for (;;) {
    int old = __atomic_load_n((int *)p, __ATOMIC_SEQ_CST);
    if (old > 0) {
      if (__atomic_compare_exchange_n((int *)p, &old, old - 1, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
      continue;
    }
    futex_wait(p, old);
  }
}
int ot_sem_post(void *s) {
  volatile int *p = (volatile int *)s;
  __atomic_add_fetch((int *)p, 1, __ATOMIC_SEQ_CST);
  futex_wake(p, 1);
  return 0;
}
int ot_sem_getvalue(void *s, int *out) {
  if (out) *out = __atomic_load_n((int *)s, __ATOMIC_SEQ_CST);
  return 0;
}

int ot_gettid(void) { return (int)syscall(SYS_gettid); }

/* ------------------------------------------------------------- emutls --- */
/* Cada objeto emutls do guest e {size, align, ptr_or_index, template}.  Uma
 * chave pthread por objeto e o suficiente: o jogo so precisa que cada thread
 * veja a propria copia, inicializada pelo template. */
typedef struct {
  uintptr_t size;
  uintptr_t align;
  void *index;
  const void *templ;
} ot_emutls_obj;

static pthread_mutex_t g_emutls_lock = PTHREAD_MUTEX_INITIALIZER;

void *__emutls_get_address(void *ptr) {
  ot_emutls_obj *o = (ot_emutls_obj *)ptr;
  pthread_key_t *k = (pthread_key_t *)__atomic_load_n(&o->index, __ATOMIC_ACQUIRE);
  if (!k) {
    pthread_mutex_lock(&g_emutls_lock);
    k = (pthread_key_t *)o->index;
    if (!k) {
      k = (pthread_key_t *)malloc(sizeof(pthread_key_t));
      pthread_key_create(k, free);
      __atomic_store_n(&o->index, (void *)k, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_emutls_lock);
  }
  void *v = pthread_getspecific(*k);
  if (!v) {
    size_t sz = o->size ? o->size : 1;
    size_t al = o->align ? o->align : 16;
    if (posix_memalign(&v, al < sizeof(void *) ? sizeof(void *) : al, sz) != 0)
      return NULL;
    if (o->templ) memcpy(v, o->templ, sz);
    else memset(v, 0, sz);
    pthread_setspecific(*k, v);
  }
  return v;
}
