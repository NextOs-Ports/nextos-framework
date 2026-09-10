/*
 * mono_trace.c -- sondas de diagnostico M2 (muro: Task LongRunning do
 * Game.Run() nunca spawna a game thread; 0 pthread_create apos onCreate).
 *
 * SB_JIT_TRACE=1   loga cada metodo managed JIT-ado (JIT puro: AOT recusado,
 *                    entao 1a execucao de QUALQUER metodo passa aqui) + falhas
 *                    de JIT + excecoes managed lancadas + threads mono.
 * SB_JIT_FILTER=a,b substrings (case-sensitive) p/ filtrar o jit-done trace.
 * SB_THREAD_TEST=1 apos Runtime_init: mono_thread_create() isolado — testa o
 *                    create_thread interno do mono ate pthread_create/fn rodar.
 *
 * NAO usar _Thread_local aqui (desloca g_bionic_guard_pad -> stack smashing).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "so_util.h"

extern DynLibFunction *g_resolv_tbl;
extern int g_resolv_n;

static uintptr_t trace_find(DynLibFunction *t, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (t[i].symbol && strcmp(t[i].symbol, name) == 0) return t[i].func;
    return 0;
}

/* ---- ponteiros mono resolvidos no install ---- */
static char *(*p_method_full_name)(void *method, int signature);
static void *(*p_object_get_class)(void *obj);
static const char *(*p_class_get_name)(void *klass);
static const char *(*p_class_get_namespace)(void *klass);
static void (*p_stack_walk)(void *, void *);

static char *g_jit_filter;   /* copia mutavel de SB_JIT_FILTER */

static int jit_filter_match(const char *full) {
    if (!g_jit_filter || !*g_jit_filter) return 1;
    /* lista separada por virgula; match = contem qualquer substring */
    char buf[256];
    snprintf(buf, sizeof buf, "%s", g_jit_filter);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
        if (*tok && strstr(full, tok)) return 1;
    return 0;
}

static void cb_jit_done(void *prof, void *method, void *jinfo) {
    (void)prof; (void)jinfo;
    if (!p_method_full_name) return;
    char *full = p_method_full_name(method, 1);
    if (full) {
        if (jit_filter_match(full))
            fprintf(stderr, "[jit] %ld %s\n",
                    (long)syscall(SYS_gettid), full);
        free(full);
    }
}

static void cb_jit_failed(void *prof, void *method) {
    (void)prof;
    char *full = p_method_full_name ? p_method_full_name(method, 1) : NULL;
    fprintf(stderr, "[jit-FAILED] %ld %s\n",
            (long)syscall(SYS_gettid), full ? full : "?");
    if (full) free(full);
}

static int cb_stack_frame(void *method, int32_t native_offset,
                          int32_t il_offset, int managed, void *data) {
    int *frames = (int *)data;
    if (!managed || !method || !p_method_full_name || *frames >= 24)
        return *frames < 24;
    char *full = p_method_full_name(method, 1);
    fprintf(stderr, "[exc-frame] #%d il=0x%x native=0x%x %s\n",
            *frames, (unsigned)il_offset, (unsigned)native_offset,
            full ? full : "?");
    if (full) free(full);
    (*frames)++;
    return *frames < 24;
}

static void cb_exc_throw(void *prof, void *exc) {
    (void)prof;
    const char *ns = "?", *nm = "?";
    if (exc && p_object_get_class) {
        void *klass = p_object_get_class(exc);
        if (klass) {
            if (p_class_get_namespace) ns = p_class_get_namespace(klass);
            if (p_class_get_name) nm = p_class_get_name(klass);
        }
    }
    fprintf(stderr, "[exc-THROW] %ld %s.%s\n",
            (long)syscall(SYS_gettid), ns ? ns : "?", nm ? nm : "?");
    if (p_stack_walk && nm && strcmp(nm, "NullReferenceException") == 0) {
        int frames = 0;
        p_stack_walk((void *)cb_stack_frame, &frames);
    }
}

static void cb_thread_started(void *prof, uintptr_t tid) {
    (void)prof;
    fprintf(stderr, "[mono-thread-started] tid=%lu ltid=%ld\n",
            (unsigned long)tid, (long)syscall(SYS_gettid));
}

static void cb_thread_name(void *prof, uintptr_t tid, const char *name) {
    (void)prof;
    fprintf(stderr, "[mono-thread-name] tid=%lu '%s'\n",
            (unsigned long)tid, name ? name : "(null)");
}

/* Instala o profiler. Chamar DEPOIS do snapshot do libmonosgen e ANTES do
 * Runtime_init (timing padrao de profiler do mono). */
void sb_mono_trace_install(DynLibFunction *tbl, int n) {
    if (!getenv("SB_JIT_TRACE")) return;

    void *(*prof_create)(void *) =
        (void *(*)(void *))trace_find(tbl, n, "mono_profiler_create");
    void (*set_jit_done)(void *, void *) =
        (void (*)(void *, void *))trace_find(tbl, n, "mono_profiler_set_jit_done_callback");
    void (*set_jit_failed)(void *, void *) =
        (void (*)(void *, void *))trace_find(tbl, n, "mono_profiler_set_jit_failed_callback");
    void (*set_exc_throw)(void *, void *) =
        (void (*)(void *, void *))trace_find(tbl, n, "mono_profiler_set_exception_throw_callback");
    void (*set_thr_started)(void *, void *) =
        (void (*)(void *, void *))trace_find(tbl, n, "mono_profiler_set_thread_started_callback");
    void (*set_thr_name)(void *, void *) =
        (void (*)(void *, void *))trace_find(tbl, n, "mono_profiler_set_thread_name_callback");

    p_method_full_name = (char *(*)(void *, int))
        trace_find(tbl, n, "mono_method_full_name");
    p_object_get_class = (void *(*)(void *))
        trace_find(tbl, n, "mono_object_get_class");
    p_class_get_name = (const char *(*)(void *))
        trace_find(tbl, n, "mono_class_get_name");
    p_class_get_namespace = (const char *(*)(void *))
        trace_find(tbl, n, "mono_class_get_namespace");
    p_stack_walk = (void (*)(void *, void *))
        trace_find(tbl, n, "mono_stack_walk");

    const char *filter = getenv("SB_JIT_FILTER");
    if (filter && *filter) g_jit_filter = strdup(filter);

    if (!prof_create || !set_jit_done) {
        fprintf(stderr, "[mono-trace] API de profiler incompleta (create=%p jit_done=%p)\n",
                (void *)prof_create, (void *)set_jit_done);
        return;
    }
    void *handle = prof_create(NULL);
    if (!handle) {
        fprintf(stderr, "[mono-trace] mono_profiler_create devolveu NULL\n");
        return;
    }
    set_jit_done(handle, (void *)cb_jit_done);
    if (set_jit_failed) set_jit_failed(handle, (void *)cb_jit_failed);
    if (set_exc_throw) set_exc_throw(handle, (void *)cb_exc_throw);
    if (set_thr_started) set_thr_started(handle, (void *)cb_thread_started);
    if (set_thr_name) set_thr_name(handle, (void *)cb_thread_name);
    fprintf(stderr, "[mono-trace] profiler instalado (handle=%p filtro='%s')\n",
            handle, g_jit_filter ? g_jit_filter : "");
}

/* ---- teste isolado de criacao de thread mono ---- */
static volatile int g_thread_test_ran;

static uint32_t sb_thread_test_fn(void *arg) {
    (void)arg;
    g_thread_test_ran = 1;
    fprintf(stderr, "[thread-test] FN managed-thread RODANDO tid=%ld\n",
            (long)syscall(SYS_gettid));
    return 0;
}

void sb_thread_test(void) {
    if (!getenv("SB_THREAD_TEST")) return;

    void *(*get_root_domain)(void) =
        (void *(*)(void))trace_find(g_resolv_tbl, g_resolv_n, "mono_get_root_domain");
    void (*thread_create)(void *, void *, void *) =
        (void (*)(void *, void *, void *))trace_find(g_resolv_tbl, g_resolv_n, "mono_thread_create");
    if (!get_root_domain || !thread_create) {
        fprintf(stderr, "[thread-test] API indisponivel (root=%p create=%p)\n",
                (void *)get_root_domain, (void *)thread_create);
        return;
    }
    void *domain = get_root_domain();
    fprintf(stderr, "[thread-test] mono_thread_create(domain=%p fn=%p)...\n",
            domain, (void *)sb_thread_test_fn);
    thread_create(domain, (void *)sb_thread_test_fn, NULL);
    fprintf(stderr, "[thread-test] mono_thread_create RETORNOU; aguardando fn...\n");
    for (int i = 0; i < 20 && !g_thread_test_ran; i++)
        usleep(100000);
    fprintf(stderr, "[thread-test] RESULTADO: fn %s\n",
            g_thread_test_ran ? "RODOU (create_thread do mono OK)"
                              : "NAO rodou em 2s (create_thread quebrado)");
}
