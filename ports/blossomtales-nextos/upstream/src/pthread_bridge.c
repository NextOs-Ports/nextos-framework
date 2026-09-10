/*
 * pthread_bridge.c — ponte de ABI pthread bionic -> glibc.
 *
 * O reVC e o libc++ foram compilados contra o bionic, cujos objetos opacos
 * (pthread_mutex_t=40B, cond=48B, rwlock=56B no LP64) são MENORES que os do
 * glibc (mutex=48B, cond=48B, rwlock=56B). Passar um mutex bionic pro
 * pthread_mutex_lock do glibc faz o glibc ler/escrever além do storage ->
 * corrupção/deadlock (foi o que travou a cutscene).
 *
 * Solução: tratamos o storage bionic como guardando um PONTEIRO (8B) para o
 * objeto glibc REAL, alocado no heap (lazy na 1ª uso p/ inicializadores
 * estáticos = zero). Só interceptamos os tipos com tamanho divergente; keys,
 * create/join/self/detach (pthread_t compatível 8B) vão direto pro glibc.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "so_util.h"

struct bionic_attr {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
  char reserved[16];
};

#define BIONIC_ATTR_DETACHED 0x00000001u


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

// ---------------- cond (clock MONOTONIC p/ casar o default do bionic) ----------------
static pthread_cond_t *cnd_real(void *c) {
  pthread_cond_t **slot = (pthread_cond_t **)c;
  if (IS_HEAP_PTR(*slot))
    return *slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_cond_t *r = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(r, &a);
    pthread_condattr_destroy(&a);
    *slot = r;
  }
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_cond_init(void *c, const void *a) {
  (void)a;
  pthread_cond_t **slot = (pthread_cond_t **)c;
  pthread_mutex_lock(&g_lock);
  *slot = NULL; // força lazy (com clock monotonic) no 1º uso
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

/* Wrapper de pthread_create so p/ TRACE (SB_THREAD_TRACE): loga o entry point
 * de cada thread criada (mono runtime, thread pool, game loop LongRunning).
 * Chama o pthread_create real do glibc. */
static int (*g_real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *);
static int b_pthread_create(pthread_t *t, const pthread_attr_t *a,
                            void *(*fn)(void *), void *arg) {
    if (!g_real_pthread_create)
        g_real_pthread_create =
            (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *),
                     void *))dlsym(RTLD_NEXT, "pthread_create");
    if (getenv("SB_THREAD_TRACE")) {
        void *sym = NULL; Dl_info di;
        if (dladdr((void *)fn, &di) && di.dli_sname) sym = (void *)di.dli_sname;
        fprintf(stderr, "[pthread] create fn=%p (%s) arg=%p\n",
                (void *)fn, sym ? (char *)sym : "?", arg);
    }
    if (!a) return g_real_pthread_create(t, NULL, fn, arg);
    const struct bionic_attr *b = (const struct bionic_attr *)a;
    pthread_attr_t real;
    pthread_attr_init(&real);
    if (b->flags & BIONIC_ATTR_DETACHED)
        pthread_attr_setdetachstate(&real, PTHREAD_CREATE_DETACHED);
    size_t stack_size = b->stack_size;
    if (stack_size && stack_size < (size_t)PTHREAD_STACK_MIN)
        stack_size = (size_t)PTHREAD_STACK_MIN;
    if (b->stack_base && stack_size)
        pthread_attr_setstack(&real, b->stack_base, stack_size);
    else if (stack_size)
        pthread_attr_setstacksize(&real, stack_size);
    if (b->guard_size) pthread_attr_setguardsize(&real, b->guard_size);
    int rc = g_real_pthread_create(t, &real, fn, arg);
    pthread_attr_destroy(&real);
    return rc;
}


/* ---- pthread_attr_t: 56 B no bionic, 64 B no glibc/aarch64 ----
 *
 * Esta e a diferenca de tamanho mais traicoeira da familia: o chamador reserva
 * 56 bytes na PROPRIA PILHA e o pthread_attr_init do glibc escreve 64,
 * apagando 8 bytes de registrador salvo logo acima. No FMOD isso zerava o x24
 * salvo do frame que criava as threads de audio; ao voltar, `ldr x0,[x24]`
 * caia em SIGSEGV com o x24 = 0 (crash sem relacao aparente com threads).
 *
 * Tratamos o storage como a struct do bionic e so montamos um attr glibc de
 * verdade dentro do pthread_create.
 */
static int b_attr_init(void *a) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b) return EINVAL;
  memset(b, 0, sizeof *b);
  b->stack_size = 1024 * 1024;      /* default do bionic em 64 bits */
  b->guard_size = (size_t)sysconf(_SC_PAGESIZE);
  return 0;
}
static int b_attr_destroy(void *a) { (void)a; return 0; }
static int b_attr_setdetachstate(void *a, int state) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b) return EINVAL;
  if (state == PTHREAD_CREATE_DETACHED) b->flags |= BIONIC_ATTR_DETACHED;
  else if (state == PTHREAD_CREATE_JOINABLE) b->flags &= ~BIONIC_ATTR_DETACHED;
  else return EINVAL;
  return 0;
}
static int b_attr_getdetachstate(void *a, int *state) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !state) return EINVAL;
  *state = (b->flags & BIONIC_ATTR_DETACHED) ? PTHREAD_CREATE_DETACHED
                                             : PTHREAD_CREATE_JOINABLE;
  return 0;
}
/* O bionic aceita pilhas bem menores que o PTHREAD_STACK_MIN da glibc
 * (128 KiB no aarch64). O FMOD pede pilhas pequenas para as threads de mixer e
 * de stream e trata QUALQUER erro aqui como falha fatal: devolver EINVAL fazia
 * System::init responder ERR_INTERNAL e o jogo morrer no SoundHelper. Aceitamos
 * o valor pedido e so elevamos ao minimo da glibc na hora de criar a thread. */
static int b_attr_setstacksize(void *a, size_t size) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || size == 0) return EINVAL;
  b->stack_size = size;
  return 0;
}
static int b_attr_getstacksize(void *a, size_t *size) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !size) return EINVAL;
  *size = b->stack_size;
  return 0;
}
static int b_attr_setstack(void *a, void *base, size_t size) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || size == 0) return EINVAL;
  b->stack_base = base;
  b->stack_size = size;
  return 0;
}
static int b_attr_getstack(void *a, void **base, size_t *size) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !base || !size) return EINVAL;
  *base = b->stack_base;
  *size = b->stack_size;
  return 0;
}
static int b_attr_setguardsize(void *a, size_t size) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b) return EINVAL;
  b->guard_size = size;
  return 0;
}
static int b_attr_getguardsize(void *a, size_t *size) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !size) return EINVAL;
  *size = b->guard_size;
  return 0;
}
static int b_attr_setschedpolicy(void *a, int policy) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b) return EINVAL;
  b->sched_policy = policy;
  return 0;
}
static int b_attr_getschedpolicy(void *a, int *policy) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !policy) return EINVAL;
  *policy = b->sched_policy;
  return 0;
}
static int b_attr_setschedparam(void *a, const struct sched_param *param) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !param) return EINVAL;
  b->sched_priority = param->sched_priority;
  return 0;
}
static int b_attr_getschedparam(void *a, struct sched_param *param) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b || !param) return EINVAL;
  param->sched_priority = b->sched_priority;
  return 0;
}
/* O bionic aceita e ignora estes dois; manter a mesma semantica. */
static int b_attr_setinheritsched(void *a, int inherit) { (void)a; (void)inherit; return 0; }
static int b_attr_getinheritsched(void *a, int *inherit) {
  (void)a;
  if (!inherit) return EINVAL;
  *inherit = PTHREAD_INHERIT_SCHED;
  return 0;
}
static int b_attr_setscope(void *a, int scope) {
  (void)a;
  return scope == PTHREAD_SCOPE_SYSTEM ? 0 : ENOTSUP;
}
static int b_attr_getscope(void *a, int *scope) {
  (void)a;
  if (!scope) return EINVAL;
  *scope = PTHREAD_SCOPE_SYSTEM;
  return 0;
}

/* pthread_getattr_np devolveria 64 bytes no storage de 56 do chamador. */
static int b_getattr_np(pthread_t t, void *a) {
  struct bionic_attr *b = (struct bionic_attr *)a;
  if (!b) return EINVAL;
  pthread_attr_t real;
  int rc = pthread_getattr_np(t, &real);
  if (rc != 0) return rc;
  b_attr_init(b);
  void *base = NULL; size_t size = 0;
  if (pthread_attr_getstack(&real, &base, &size) == 0) {
    b->stack_base = base;
    b->stack_size = size;
  }
  size_t guard = 0;
  if (pthread_attr_getguardsize(&real, &guard) == 0) b->guard_size = guard;
  int detach = PTHREAD_CREATE_JOINABLE;
  if (pthread_attr_getdetachstate(&real, &detach) == 0 &&
      detach == PTHREAD_CREATE_DETACHED)
    b->flags |= BIONIC_ATTR_DETACHED;
  pthread_attr_destroy(&real);
  return 0;
}

DynLibFunction revc_pthread_table[] = {
    {"pthread_create", (uintptr_t)&b_pthread_create},
    {"pthread_attr_init", (uintptr_t)&b_attr_init},
    {"pthread_attr_destroy", (uintptr_t)&b_attr_destroy},
    {"pthread_attr_setdetachstate", (uintptr_t)&b_attr_setdetachstate},
    {"pthread_attr_getdetachstate", (uintptr_t)&b_attr_getdetachstate},
    {"pthread_attr_setstacksize", (uintptr_t)&b_attr_setstacksize},
    {"pthread_attr_getstacksize", (uintptr_t)&b_attr_getstacksize},
    {"pthread_attr_setstack", (uintptr_t)&b_attr_setstack},
    {"pthread_attr_getstack", (uintptr_t)&b_attr_getstack},
    {"pthread_attr_setguardsize", (uintptr_t)&b_attr_setguardsize},
    {"pthread_attr_getguardsize", (uintptr_t)&b_attr_getguardsize},
    {"pthread_attr_setschedpolicy", (uintptr_t)&b_attr_setschedpolicy},
    {"pthread_attr_getschedpolicy", (uintptr_t)&b_attr_getschedpolicy},
    {"pthread_attr_setschedparam", (uintptr_t)&b_attr_setschedparam},
    {"pthread_attr_getschedparam", (uintptr_t)&b_attr_getschedparam},
    {"pthread_attr_setinheritsched", (uintptr_t)&b_attr_setinheritsched},
    {"pthread_attr_getinheritsched", (uintptr_t)&b_attr_getinheritsched},
    {"pthread_attr_setscope", (uintptr_t)&b_attr_setscope},
    {"pthread_attr_getscope", (uintptr_t)&b_attr_getscope},
    {"pthread_getattr_np", (uintptr_t)&b_getattr_np},
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
