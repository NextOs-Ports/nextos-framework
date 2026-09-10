/* ab_pthread.c — ponte pthread bionic→glibc (ARM32).
 *
 * No bionic pthread_mutex_t e pthread_cond_t têm 4 BYTES e pthread_rwlock_t 40;
 * no glibc são 24, 48 e 32. O jogo aloca esses objetos INLINE dentro das
 * próprias structs, então escrever o objeto do glibc lá esmaga o vizinho.
 * Tratamos o slot do convidado como guardando um PONTEIRO pro objeto real.
 *
 * Registro POR ENDEREÇO e sem free: re-init no mesmo endereço REUSA o objeto,
 * então waiters antigos continuam alcançáveis por signals futuros — igual ao
 * futex-por-endereço do bionic.
 *
 * As conds nascem com CLOCK_MONOTONIC porque a única espera temporizada que a
 * .so importa é `pthread_cond_timedwait_monotonic`.
 *
 * 🚨 pthread_key: o bionic NUNCA devolve a chave 0, e código que assume isso
 * trata 0 como "não inicializado". Queimamos a chave 0 na inicialização.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ab_port.h"

#define MIN_PTR ((uintptr_t)0x10000)

static pthread_mutex_t g_gate = PTHREAD_MUTEX_INITIALIZER;

#define REG_SIZE 4096
typedef struct {
  void *slot;
  void *object;
} RegEnt;
static RegEnt g_mutex_reg[REG_SIZE];
static RegEnt g_cond_reg[REG_SIZE];
static RegEnt g_rwlock_reg[REG_SIZE];

/* sempre chamado com g_gate travado */
static RegEnt *reg_find(RegEnt *registry, void *slot) {
  uint32_t h = ((uint32_t)(uintptr_t)slot >> 2) * 2654435761u;
  for (unsigned i = 0; i < REG_SIZE; i++) {
    RegEnt *e = &registry[(h + i) & (REG_SIZE - 1)];
    if (e->slot == slot || e->slot == NULL)
      return e;
  }
  return NULL;
}

/* ---------------- mutex ---------------- */

static pthread_mutex_t *new_mutex(int type) {
  pthread_mutex_t *m = malloc(sizeof(*m));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, type ? type : PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  return m;
}

static pthread_mutex_t *bridge_mutex_locked(void **slot, int type_hint) {
  RegEnt *e = reg_find(g_mutex_reg, (void *)slot);
  pthread_mutex_t *m;
  if (e && e->slot) {
    m = e->object;
    /* re-init num mutex ainda travado: só aí trocamos o objeto */
    int rc = pthread_mutex_trylock(m);
    if (rc == 0)
      pthread_mutex_unlock(m);
    else if (rc == EBUSY) {
      m = new_mutex(type_hint);
      e->object = m;
    }
  } else {
    m = new_mutex(type_hint);
    if (e) {
      e->slot = (void *)slot;
      e->object = m;
    }
  }
  __atomic_store_n((uintptr_t *)slot, (uintptr_t)m, __ATOMIC_RELEASE);
  return m;
}

static pthread_mutex_t *real_mutex(void **slot) {
  uintptr_t v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
  pthread_mutex_t *m;
  if (v >= MIN_PTR)
    return (pthread_mutex_t *)v;
  pthread_mutex_lock(&g_gate);
  v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
  m = (v >= MIN_PTR) ? (pthread_mutex_t *)v : bridge_mutex_locked(slot, 0);
  pthread_mutex_unlock(&g_gate);
  return m;
}

static int b_mutex_init(void **slot, const void *attr) {
  int type = PTHREAD_MUTEX_RECURSIVE;
  if (attr) {
    int declared = *(const int *)attr;
    if (declared == 2)
      type = PTHREAD_MUTEX_ERRORCHECK;
  }
  pthread_mutex_lock(&g_gate);
  *slot = 0;
  bridge_mutex_locked(slot, type);
  pthread_mutex_unlock(&g_gate);
  return 0;
}
static int b_mutex_destroy(void **slot) {
  (void)slot;
  return 0;
}
static int b_mutex_lock(void **slot) { return pthread_mutex_lock(real_mutex(slot)); }
static int b_mutex_unlock(void **slot) { return pthread_mutex_unlock(real_mutex(slot)); }
static int b_mutex_trylock(void **slot) { return pthread_mutex_trylock(real_mutex(slot)); }

/* ---------------- cond ---------------- */

static pthread_cond_t *new_cond(void) {
  pthread_cond_t *c = malloc(sizeof(*c));
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(c, &attr);
  pthread_condattr_destroy(&attr);
  return c;
}

static pthread_cond_t *bridge_cond_locked(void **slot) {
  RegEnt *e = reg_find(g_cond_reg, (void *)slot);
  pthread_cond_t *c;
  if (e && e->slot) {
    c = e->object; /* reusa SEMPRE: waiters antigos precisam de signals novos */
  } else {
    c = new_cond();
    if (e) {
      e->slot = (void *)slot;
      e->object = c;
    }
  }
  __atomic_store_n((uintptr_t *)slot, (uintptr_t)c, __ATOMIC_RELEASE);
  return c;
}

static pthread_cond_t *real_cond(void **slot) {
  uintptr_t v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
  pthread_cond_t *c;
  if (v >= MIN_PTR)
    return (pthread_cond_t *)v;
  pthread_mutex_lock(&g_gate);
  v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
  c = (v >= MIN_PTR) ? (pthread_cond_t *)v : bridge_cond_locked(slot);
  pthread_mutex_unlock(&g_gate);
  return c;
}

static int b_cond_init(void **slot, const void *attr) {
  (void)attr;
  pthread_mutex_lock(&g_gate);
  *slot = 0;
  bridge_cond_locked(slot);
  pthread_mutex_unlock(&g_gate);
  return 0;
}
static int b_cond_destroy(void **slot) {
  (void)slot;
  return 0;
}
static int b_cond_signal(void **slot) { return pthread_cond_signal(real_cond(slot)); }
static int b_cond_broadcast(void **slot) { return pthread_cond_broadcast(real_cond(slot)); }
static int b_cond_wait(void **cond_slot, void **mutex_slot) {
  return pthread_cond_wait(real_cond(cond_slot), real_mutex(mutex_slot));
}
static int b_cond_timedwait_monotonic(void **cond_slot, void **mutex_slot,
                                      const struct timespec *abstime) {
  if (!abstime)
    return pthread_cond_wait(real_cond(cond_slot), real_mutex(mutex_slot));
  return pthread_cond_timedwait(real_cond(cond_slot), real_mutex(mutex_slot),
                                abstime);
}
/* A variante relativa do bionic recebe um intervalo, não um instante. */
static int b_cond_timedwait_relative(void **cond_slot, void **mutex_slot,
                                     const struct timespec *reltime) {
  struct timespec deadline;
  if (!reltime)
    return pthread_cond_wait(real_cond(cond_slot), real_mutex(mutex_slot));
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += reltime->tv_sec;
  deadline.tv_nsec += reltime->tv_nsec;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_nsec -= 1000000000L;
    deadline.tv_sec++;
  }
  return pthread_cond_timedwait(real_cond(cond_slot), real_mutex(mutex_slot),
                                &deadline);
}
static int b_condattr_init(void *attr) {
  if (attr)
    *(int *)attr = 0;
  return 0;
}
static int b_condattr_destroy(void *attr) {
  (void)attr;
  return 0;
}

/* ---------------- rwlock (40 B no bionic, ponteiro na 1ª palavra) -------- */

static pthread_rwlock_t *real_rwlock(void **slot) {
  uintptr_t v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
  pthread_rwlock_t *l;
  RegEnt *e;
  if (v >= MIN_PTR)
    return (pthread_rwlock_t *)v;
  pthread_mutex_lock(&g_gate);
  v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
  if (v >= MIN_PTR) {
    pthread_mutex_unlock(&g_gate);
    return (pthread_rwlock_t *)v;
  }
  e = reg_find(g_rwlock_reg, (void *)slot);
  if (e && e->slot) {
    l = e->object;
  } else {
    l = malloc(sizeof(*l));
    pthread_rwlock_init(l, NULL);
    if (e) {
      e->slot = (void *)slot;
      e->object = l;
    }
  }
  __atomic_store_n((uintptr_t *)slot, (uintptr_t)l, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_gate);
  return l;
}

static int b_rwlock_init(void **slot, const void *attr) {
  (void)attr;
  pthread_mutex_lock(&g_gate);
  *slot = 0;
  pthread_mutex_unlock(&g_gate);
  real_rwlock(slot);
  return 0;
}
static int b_rwlock_destroy(void **slot) {
  (void)slot;
  return 0;
}
static int b_rwlock_rdlock(void **slot) { return pthread_rwlock_rdlock(real_rwlock(slot)); }
static int b_rwlock_wrlock(void **slot) { return pthread_rwlock_wrlock(real_rwlock(slot)); }
static int b_rwlock_unlock(void **slot) { return pthread_rwlock_unlock(real_rwlock(slot)); }

/* ---------------- threads ---------------- */

static int b_pthread_create(pthread_t *thread, void **attr_slot,
                            void *(*start)(void *), void *arg) {
  pthread_attr_t attr;
  int rc;
  (void)attr_slot; /* a .so não importa pthread_attr_*: sempre NULL */
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  rc = pthread_create(thread, &attr, start, arg);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    ab_log("[pthread] create falhou rc=%d", rc);
    if (thread)
      *thread = 0;
  }
  return rc;
}

static int b_pthread_join(pthread_t thread, void **retval) {
  if (!thread) {
    if (retval)
      *retval = NULL;
    return ESRCH;
  }
  return pthread_join(thread, retval);
}

static int b_pthread_setname_np(pthread_t thread, const char *name) {
  char shortname[16];
  if (!name)
    return 0;
  snprintf(shortname, sizeof(shortname), "%s", name);
  return pthread_setname_np(thread, shortname);
}

/* ---------------- tabela ---------------- */

nxloader_result ab_add_pthread_provider(nxloader_registry *registry) {
  static nxloader_symbol symbols[48];
  size_t count = 0;
  nxloader_provider provider;
  pthread_key_t burned;

  /* 🚨 o bionic nunca devolve a chave 0; queimamos a primeira do glibc */
  if (pthread_key_create(&burned, NULL) == 0 && burned != 0)
    pthread_key_delete(burned);

#define ADD(sym_name, sym_addr)                     \
  do {                                              \
    symbols[count].name = (sym_name);               \
    symbols[count].address = (uintptr_t)(sym_addr); \
    symbols[count].flags = 0;                       \
    count++;                                        \
  } while (0)

  ADD("pthread_mutex_init", b_mutex_init);
  ADD("pthread_mutex_destroy", b_mutex_destroy);
  ADD("pthread_mutex_lock", b_mutex_lock);
  ADD("pthread_mutex_unlock", b_mutex_unlock);
  ADD("pthread_mutex_trylock", b_mutex_trylock);
  ADD("pthread_cond_init", b_cond_init);
  ADD("pthread_cond_destroy", b_cond_destroy);
  ADD("pthread_cond_signal", b_cond_signal);
  ADD("pthread_cond_broadcast", b_cond_broadcast);
  ADD("pthread_cond_wait", b_cond_wait);
  ADD("pthread_cond_timedwait", b_cond_timedwait_monotonic);
  ADD("pthread_cond_timedwait_monotonic", b_cond_timedwait_monotonic);
  ADD("pthread_cond_timedwait_monotonic_np", b_cond_timedwait_monotonic);
  ADD("pthread_cond_timedwait_relative_np", b_cond_timedwait_relative);
  ADD("pthread_condattr_init", b_condattr_init);
  ADD("pthread_condattr_destroy", b_condattr_destroy);
  ADD("pthread_rwlock_init", b_rwlock_init);
  ADD("pthread_rwlock_destroy", b_rwlock_destroy);
  ADD("pthread_rwlock_rdlock", b_rwlock_rdlock);
  ADD("pthread_rwlock_wrlock", b_rwlock_wrlock);
  ADD("pthread_rwlock_unlock", b_rwlock_unlock);
  ADD("pthread_create", b_pthread_create);
  ADD("pthread_join", b_pthread_join);
  ADD("pthread_setname_np", b_pthread_setname_np);
  /* tipos idênticos entre bionic e glibc no ARM32 */
  ADD("pthread_detach", pthread_detach);
  ADD("pthread_equal", pthread_equal);
  ADD("pthread_self", pthread_self);
  ADD("pthread_once", pthread_once);
  ADD("pthread_key_create", pthread_key_create);
  ADD("pthread_key_delete", pthread_key_delete);
  ADD("pthread_getspecific", pthread_getspecific);
  ADD("pthread_setspecific", pthread_setspecific);
  ADD("pthread_getschedparam", pthread_getschedparam);
  ADD("pthread_setschedparam", pthread_setschedparam);
#undef ADD

  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "angrybirds-pthread";
  provider.symbols = symbols;
  provider.symbol_count = count;
  provider.priority = 20;
  return nxloader_registry_add_provider(registry, &provider, NULL);
}
