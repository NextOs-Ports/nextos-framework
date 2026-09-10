/* pthread_fake.c — bridge pthread bionic(armhf) -> glibc, p/ o so-loader.
 *
 * Problema de ABI (ARM 32-bit): o bionic guarda mutex/cond/sem em 4 bytes,
 * mas a glibc usa 24/48/16 bytes. Se a glibc escrever no slot de 4 bytes do
 * jogo, corrompe memória. Solução: o slot de 4 bytes do jogo guarda um
 * PONTEIRO p/ um objeto glibc que a gente aloca (lazy-init cobre os
 * PTHREAD_*_INITIALIZER estáticos, que vêm zerados).
 *
 * pthread_t / pthread_key_t / pthread_once_t são 4 bytes em ambos no armhf,
 * então pthread_create/key/once mapeiam ~direto (attr ignorado).
 *
 * Baseado na lógica do gtactw_vita (que usa primitivas Vita); aqui = glibc.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------- mutex ---------- */
static pthread_mutex_t *mtx_get(void **slot) {
  if (!*slot) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    *slot = m;
  }
  return (pthread_mutex_t *)*slot;
}
int pthread_mutex_init_fake(void **slot, const void *attr) {
  (void)attr; *slot = NULL; mtx_get(slot); return 0;
}
int pthread_mutex_destroy_fake(void **slot) {
  if (*slot) { pthread_mutex_destroy((pthread_mutex_t *)*slot); free(*slot); *slot = NULL; }
  return 0;
}
int pthread_mutex_lock_fake(void **slot) { return pthread_mutex_lock(mtx_get(slot)); }
int pthread_mutex_unlock_fake(void **slot) {
  if (!*slot) return 0; return pthread_mutex_unlock((pthread_mutex_t *)*slot);
}
int pthread_mutex_trylock_fake(void **slot) { return pthread_mutex_trylock(mtx_get(slot)); }

/* ---------- cond ---------- */
static pthread_cond_t *cond_get(void **slot) {
  if (!*slot) {
    pthread_cond_t *c = malloc(sizeof(pthread_cond_t));
    pthread_cond_init(c, NULL);
    *slot = c;
  }
  return (pthread_cond_t *)*slot;
}
int pthread_cond_init_fake(void **slot, const void *attr) {
  (void)attr; *slot = NULL; cond_get(slot); return 0;
}
int pthread_cond_destroy_fake(void **slot) {
  if (*slot) { pthread_cond_destroy((pthread_cond_t *)*slot); free(*slot); *slot = NULL; }
  return 0;
}
int pthread_cond_signal_fake(void **slot) { return pthread_cond_signal(cond_get(slot)); }
int pthread_cond_broadcast_fake(void **slot) { return pthread_cond_broadcast(cond_get(slot)); }
/* CUP_CONDPOLL=ms: defesa contra lost-wakeup em condition variables (mesma classe
   do fix CUP_SEMPOLL p/ semáforos). Em vez de esperar p/ sempre, faz timedwait
   curto e RETORNA (acordada espúria, permitida pelo POSIX) → o chamador re-adquire
   o mutex e re-checa seu predicado no while(); se não pronto, volta a esperar.
   Isso acorda periodicamente a UnityPreload/Workers que ficam presas no futex. */
#include <time.h>
static long g_cond_poll_ms = 0;   /* 0 = wait infinito normal */
void cond_set_poll(int ms) { g_cond_poll_ms = ms; }
int pthread_cond_wait_fake(void **cslot, void **mslot) {
  if (g_cond_poll_ms <= 0)
    return pthread_cond_wait(cond_get(cslot), mtx_get(mslot));
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += g_cond_poll_ms * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  int r = pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
  return (r == ETIMEDOUT) ? 0 : r;   /* timeout vira wakeup espúrio */
}
int pthread_cond_timedwait_fake(void **cslot, void **mslot, const struct timespec *ts) {
  return pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), ts);
}

/* ---------- sem ---------- */
static sem_t *sem_get(void **slot) {
  if (!*slot) { sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, 0); *slot = s; }
  return (sem_t *)*slot;
}
int sem_init_fake(void **slot, int pshared, unsigned value) {
  (void)pshared; sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, value); *slot = s; return 0;
}
int sem_destroy_fake(void **slot) {
  if (*slot) { sem_destroy((sem_t *)*slot); free(*slot); *slot = NULL; } return 0;
}
int sem_post_fake(void **slot) { return sem_post(sem_get(slot)); }
int sem_wait_fake(void **slot) { return sem_wait(sem_get(slot)); }
int sem_trywait_fake(void **slot) { return sem_trywait(sem_get(slot)); }

/* ---------- create / attr (mapeiam ~direto; attr bionic ignorado) ---------- */
/* trampolim: instala um sigaltstack PRÓPRIO nesta thread antes de chamar o start
 * real. Sem isso, um SIGSEGV numa thread do Unity roda o on_crash na pilha da
 * própria thread (que pode estar corrompida/cheia) → re-fault → morte calada.
 * Com altstack por-thread, o on_crash sempre consegue dumpar. */
#include <signal.h>
#include <sys/mman.h>
struct thr_boot { void *(*start)(void *); void *arg; };
static void *thr_trampoline(void *p) {
  struct thr_boot b = *(struct thr_boot *)p;
  free(p);
  size_t asz = 256 * 1024;   /* fixo e generoso (SIGSTKSZ pode ser minúsculo/dinâmico
                                e o on_crash dumpa bastante → estouraria) */
  void *mem = mmap(NULL, asz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem != MAP_FAILED) {
    stack_t ss; ss.ss_sp = mem; ss.ss_size = asz; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
  }
  return b.start(b.arg);
}
int pthread_create_fake(pthread_t *t, const void *attr, void *(*start)(void *), void *arg) {
  (void)attr;
  struct thr_boot *b = malloc(sizeof *b);
  if (!b) return pthread_create(t, NULL, start, arg);
  b->start = start; b->arg = arg;
  int rc = pthread_create(t, NULL, thr_trampoline, b);
  if (rc) { free(b); rc = pthread_create(t, NULL, start, arg); }
  return rc;
}
int pthread_attr_init_fake(void *a) { (void)a; return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { (void)a; (void)s; return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { (void)a; (void)s; return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }
int pthread_setschedparam_fake(pthread_t t, int p, const void *s) { (void)t; (void)p; (void)s; return 0; }
int pthread_setname_np_fake(pthread_t t, const char *n) { (void)t; (void)n; return 0; }

/* bionic: pthread_cond_timeout_np(cond, mutex, ms) — espera relativa em ms */
#include <time.h>
int pthread_cond_timeout_np_fake(void **cslot, void **mslot, unsigned ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  return pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
}

/* ======== GAP arm64: rwlock/once/key/thread-misc/sem-extras ======== */
#include <signal.h>
#include <sched.h>
#include <time.h>
/* ---------- rwlock (slot->ptr glibc, igual mutex) ---------- */
static pthread_rwlock_t *rwl_get(void **slot) {
  if (!*slot) { pthread_rwlock_t *r = malloc(sizeof(pthread_rwlock_t)); pthread_rwlock_init(r, NULL); *slot = r; }
  return (pthread_rwlock_t *)*slot;
}
int pthread_rwlock_init_fake(void **slot, const void *a) { (void)a; *slot = NULL; rwl_get(slot); return 0; }
int pthread_rwlock_destroy_fake(void **slot) { if (*slot) { pthread_rwlock_destroy((pthread_rwlock_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int pthread_rwlock_rdlock_fake(void **slot) { return pthread_rwlock_rdlock(rwl_get(slot)); }
int pthread_rwlock_wrlock_fake(void **slot) { return pthread_rwlock_wrlock(rwl_get(slot)); }
int pthread_rwlock_tryrdlock_fake(void **slot) { return pthread_rwlock_tryrdlock(rwl_get(slot)); }
int pthread_rwlock_trywrlock_fake(void **slot) { return pthread_rwlock_trywrlock(rwl_get(slot)); }
int pthread_rwlock_unlock_fake(void **slot) { if (!*slot) return 0; return pthread_rwlock_unlock((pthread_rwlock_t *)*slot); }
/* ---------- thread misc (forward direto; pthread_t = 8B arm64 compat) ---------- */
int pthread_detach_fake(pthread_t t) { return pthread_detach(t); }
int pthread_join_fake(pthread_t t, void **r) { return pthread_join(t, r); }
pthread_t pthread_self_fake(void) { return pthread_self(); }
int pthread_getschedparam_fake(pthread_t t, int *pol, void *par) { return pthread_getschedparam(t, pol, (struct sched_param *)par); }
int pthread_sigmask_fake(int how, const void *set, void *old) { return pthread_sigmask(how, (const sigset_t *)set, (sigset_t *)old); }
/* ---------- key / specific (pthread_key_t compat) ---------- */
int pthread_key_create_fake(unsigned *k, void (*d)(void *)) { return pthread_key_create((pthread_key_t *)k, d); }
int pthread_key_delete_fake(unsigned k) { return pthread_key_delete((pthread_key_t)k); }
void *pthread_getspecific_fake(unsigned k) { return pthread_getspecific((pthread_key_t)k); }
int pthread_setspecific_fake(unsigned k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }
/* ---------- once (bionic once_t = int) ---------- */
int pthread_once_fake(int *o, void (*f)(void)) { return pthread_once((pthread_once_t *)o, f); }
/* ---------- mutexattr (no-op; mtx_get usa RECURSIVE) ---------- */
int pthread_mutexattr_init_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_settype_fake(void *a, int t) { (void)a; (void)t; return 0; }
/* ---------- sem extras ---------- */
int sem_getvalue_fake(void **slot, int *v) { return sem_getvalue(sem_get(slot), v); }
int sem_timedwait_fake(void **slot, const struct timespec *ts) { return sem_timedwait(sem_get(slot), ts); }

/* ======== GAP Cuphead (Unity IL2CPP): condattr/attr_getstack/getattr_np ======== */
int pthread_condattr_init_fake(void *a){ (void)a; return 0; }
int pthread_condattr_destroy_fake(void *a){ (void)a; return 0; }
int pthread_condattr_setclock_fake(void *a, int clk){ (void)a; (void)clk; return 0; }
/* Unity usa getattr_np+attr_getstack p/ achar os limites da stack (GC scan).
 * F0: devolve uma faixa plausível (8MB abaixo do SP atual). */
int pthread_getattr_np_fake(pthread_t t, void *attr){ (void)t; (void)attr; return 0; }
int pthread_attr_getstack_fake(void *attr, void **addr, size_t *size){ (void)attr;
  unsigned long sp; __asm__ volatile("mov %0, sp":"=r"(sp));
  size_t sz = 8*1024*1024; unsigned long base = (sp & ~0xfffUL) - sz;
  if(addr) *addr = (void*)base; if(size) *size = sz; return 0; }
