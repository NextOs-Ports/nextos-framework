/*
 * pthread_bridge.c — ponte de ABI pthread bionic -> glibc.
 *
 * O reVC e o libc++ foram compilados contra o bionic, cujos objetos opacos
 * (pthread_mutex_t=40B, cond=48B, rwlock=56B no LP64) são MENORES que os do
 * glibc (mutex=48B, cond=48B, rwlock=56B). Passar um mutex bionic pro
 * pthread_mutex_lock do glibc faz o glibc ler/escrever além do storage ->
 * corrupção/deadlock (foi o que travou a cutscene). pthread_attr_t também
 * diverge: Bionic AArch64 reserva 56B e a glibc deste sysroot usa 64B. Passar
 * o attr direto fez pthread_attr_init zerar o registrador x22 salvo logo após
 * o objeto na pilha de ANativeActivity_onCreate.
 *
 * Solução: tratamos o storage bionic como guardando um PONTEIRO (8B) para o
 * objeto glibc REAL, alocado no heap (lazy na 1ª uso p/ inicializadores
 * estáticos = zero). Só interceptamos os tipos com tamanho divergente; keys,
 * create/join/self/detach (pthread_t compatível 8B) vão direto pro glibc.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "so_util.h"

// lock global (recursivo) p/ serializar lazy-init sem corrida
static pthread_mutex_t g_lock;
__attribute__((constructor)) static void init_glock(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_lock, &a);
  pthread_mutexattr_destroy(&a);
}

// ---------------- mutexattr (bionic = int) ----------------
int b_mutexattr_init(void *a) {
  if (a)
    *(int *)a = 0;
  return 0;
}
int b_mutexattr_destroy(void *a) {
  (void)a;
  return 0;
}
int b_mutexattr_settype(void *a, int type) {
  if (a)
    *(int *)a = type;
  return 0;
}

// ponteiro do heap (grande) vs valor mágico de inicializador estático bionic
// (pequeno: 0, 0x4000 recursivo, 0x8000 errorcheck, etc — todos < 0x10000).
#define IS_HEAP_PTR(v) ((uintptr_t)(v) > 0x10000u)

// ---------------- pthread_attr (bionic 56B -> glibc 64B) ----------------
static pthread_attr_t *attr_real(void *a) {
  if (!a)
    return NULL;
  pthread_attr_t **slot = (pthread_attr_t **)a;
  if (IS_HEAP_PTR(*slot))
    return *slot;
  pthread_attr_t *r = calloc(1, sizeof(*r));
  if (!r)
    return NULL;
  if (pthread_attr_init(r) != 0) {
    free(r);
    return NULL;
  }
  *slot = r;
  return r;
}

static int b_attr_init(void *a) {
  if (!a)
    return EINVAL;
  *(pthread_attr_t **)a = NULL;
  return attr_real(a) ? 0 : ENOMEM;
}

static int b_attr_destroy(void *a) {
  if (!a)
    return EINVAL;
  pthread_attr_t **slot = (pthread_attr_t **)a;
  if (!IS_HEAP_PTR(*slot))
    return 0;
  int r = pthread_attr_destroy(*slot);
  free(*slot);
  *slot = NULL;
  return r;
}

static int b_attr_setdetachstate(void *a, int state) {
  pthread_attr_t *r = attr_real(a);
  return r ? pthread_attr_setdetachstate(r, state) : ENOMEM;
}

static int b_attr_setstacksize(void *a, size_t size) {
  pthread_attr_t *r = attr_real(a);
  return r ? pthread_attr_setstacksize(r, size) : ENOMEM;
}

static int b_pthread_create(pthread_t *thread, const void *guest_attr,
                            void *(*start)(void *), void *arg) {
  pthread_attr_t *real_attr =
      guest_attr ? attr_real((void *)guest_attr) : NULL;
  if (guest_attr && !real_attr)
    return ENOMEM;
  return pthread_create(thread, real_attr, start, arg);
}

// cria um glibc mutex RECURSIVO (seguro p/ qualquer uso; evita self-deadlock
// quando o bionic queria recursivo via inicializador estático ou attr).
static pthread_mutex_t *new_recursive_mutex(void) {
  pthread_mutex_t *r = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(r, &a);
  pthread_mutexattr_destroy(&a);
  return r;
}

// ---------------- mutex ----------------
static pthread_mutex_t *mtx_real(void *m) {
  pthread_mutex_t **slot = (pthread_mutex_t **)m;
  if (IS_HEAP_PTR(*slot))
    return *slot; // já é nosso glibc mutex
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) // re-check sob lock (descarta mágica bionic estática)
    *slot = new_recursive_mutex();
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_mutex_init(void *m, const void *attr) {
  (void)attr; // sempre recursivo (superset seguro)
  pthread_mutex_t **slot = (pthread_mutex_t **)m;
  pthread_mutex_t *r = new_recursive_mutex();
  pthread_mutex_lock(&g_lock);
  *slot = r;
  pthread_mutex_unlock(&g_lock);
  return 0;
}
int b_mutex_lock(void *m) { return pthread_mutex_lock(mtx_real(m)); }
int b_mutex_unlock(void *m) { return pthread_mutex_unlock(mtx_real(m)); }
int b_mutex_trylock(void *m) { return pthread_mutex_trylock(mtx_real(m)); }
int b_mutex_destroy(void *m) {
  pthread_mutex_t **slot = (pthread_mutex_t **)m;
  pthread_mutex_lock(&g_lock);
  if (*slot) {
    pthread_mutex_destroy(*slot);
    free(*slot);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

// ---------------- condition variables ----------------
// Action Squad's three pthread_cond_timedwait call sites all build absolute
// deadlines from CLOCK_REALTIME (including libc++ system_clock and OpenAL).
// A monotonic glibc cond interprets those epoch timestamps as monotonic uptime
// and can sleep for decades.  The default POSIX realtime clock preserves the
// guest's measured contract.
static pthread_cond_t *cnd_real(void *c) {
  pthread_cond_t **slot = (pthread_cond_t **)c;
  if (IS_HEAP_PTR(*slot))
    return *slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_cond_t *r = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
    pthread_cond_init(r, NULL);
    *slot = r;
  }
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_cond_init(void *c, const void *a) {
  (void)a;
  pthread_cond_t **slot = (pthread_cond_t **)c;
  pthread_mutex_lock(&g_lock);
  *slot = NULL; // force lazy allocation on first use
  pthread_mutex_unlock(&g_lock);
  cnd_real(c);
  return 0;
}
int b_cond_wait(void *c, void *m) {
  return pthread_cond_wait(cnd_real(c), mtx_real(m));
}
int b_cond_timedwait(void *c, void *m, const struct timespec *t) {
  return pthread_cond_timedwait(cnd_real(c), mtx_real(m), t);
}
int b_cond_signal(void *c) { return pthread_cond_signal(cnd_real(c)); }
int b_cond_broadcast(void *c) { return pthread_cond_broadcast(cnd_real(c)); }
int b_cond_destroy(void *c) {
  pthread_cond_t **slot = (pthread_cond_t **)c;
  pthread_mutex_lock(&g_lock);
  if (*slot) {
    pthread_cond_destroy(*slot);
    free(*slot);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

// ---------------- rwlock ----------------
static pthread_rwlock_t *rw_real(void *r) {
  pthread_rwlock_t **slot = (pthread_rwlock_t **)r;
  if (IS_HEAP_PTR(*slot))
    return *slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_rwlock_t *rr =
        (pthread_rwlock_t *)calloc(1, sizeof(pthread_rwlock_t));
    pthread_rwlock_init(rr, NULL);
    *slot = rr;
  }
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_rwlock_rdlock(void *r) { return pthread_rwlock_rdlock(rw_real(r)); }
int b_rwlock_wrlock(void *r) { return pthread_rwlock_wrlock(rw_real(r)); }
int b_rwlock_unlock(void *r) { return pthread_rwlock_unlock(rw_real(r)); }

// ---------------- once (bionic = int) ----------------
int b_once(void *once_ctl, void (*init)(void)) {
  // bionic once_control = int. 0=não feito, 1=em progresso, 2=feito.
  // NÃO segura g_lock durante init() (init pode esperar outra thread).
  volatile int *st = (volatile int *)once_ctl;
  pthread_mutex_lock(&g_lock);
  while (*st == 1) { // outra thread inicializando: espera
    pthread_mutex_unlock(&g_lock);
    usleep(200);
    pthread_mutex_lock(&g_lock);
  }
  if (*st == 0) {
    *st = 1;
    pthread_mutex_unlock(&g_lock);
    init();
    pthread_mutex_lock(&g_lock);
    *st = 2;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

DynLibFunction revc_pthread_table[] = {
    {"pthread_attr_init", (uintptr_t)&b_attr_init},
    {"pthread_attr_destroy", (uintptr_t)&b_attr_destroy},
    {"pthread_attr_setdetachstate", (uintptr_t)&b_attr_setdetachstate},
    {"pthread_attr_setstacksize", (uintptr_t)&b_attr_setstacksize},
    {"pthread_create", (uintptr_t)&b_pthread_create},
    {"pthread_mutexattr_init", (uintptr_t)&b_mutexattr_init},
    {"pthread_mutexattr_destroy", (uintptr_t)&b_mutexattr_destroy},
    {"pthread_mutexattr_settype", (uintptr_t)&b_mutexattr_settype},
    {"pthread_mutex_init", (uintptr_t)&b_mutex_init},
    {"pthread_mutex_lock", (uintptr_t)&b_mutex_lock},
    {"pthread_mutex_unlock", (uintptr_t)&b_mutex_unlock},
    {"pthread_mutex_trylock", (uintptr_t)&b_mutex_trylock},
    {"pthread_mutex_destroy", (uintptr_t)&b_mutex_destroy},
    {"pthread_cond_init", (uintptr_t)&b_cond_init},
    {"pthread_cond_wait", (uintptr_t)&b_cond_wait},
    {"pthread_cond_timedwait", (uintptr_t)&b_cond_timedwait},
    {"pthread_cond_signal", (uintptr_t)&b_cond_signal},
    {"pthread_cond_broadcast", (uintptr_t)&b_cond_broadcast},
    {"pthread_cond_destroy", (uintptr_t)&b_cond_destroy},
    {"pthread_rwlock_rdlock", (uintptr_t)&b_rwlock_rdlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&b_rwlock_wrlock},
    {"pthread_rwlock_unlock", (uintptr_t)&b_rwlock_unlock},
    {"pthread_once", (uintptr_t)&b_once},
};
const int revc_pthread_count =
    sizeof(revc_pthread_table) / sizeof(revc_pthread_table[0]);
