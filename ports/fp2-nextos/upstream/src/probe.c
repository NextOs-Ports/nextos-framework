/*
 * probe.c -- C#-level truth about the boot flow, read through IL2CPP.
 *
 * Native instruments say the process is healthy: the render loop runs, the GL
 * driver draws, the loading threads did work and then parked.  None of that
 * says WHICH managed state the game is sitting in, and guessing at it is what
 * cost the first Freedom Planet 2 attempt four sessions.  This probe asks the runtime
 * directly -- active scene, camera count, timescale, loaded-scene list -- so a
 * stall is described instead of theorised.
 *
 * It is diagnostic only: nothing here changes the game's behaviour, and the
 * whole thing stays asleep unless FP2_IL2PROBE is set for a test launch.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il2.h"
#include "nx_elf.h"

static int probe_enabled = -1;
static unsigned long probe_period = 300;

static int probe_on(void)
{
    if (probe_enabled < 0) {
        const char *v = getenv("FP2_IL2PROBE");
        probe_enabled = v && *v && strcmp(v, "0") != 0;
        if (probe_enabled) {
            long p = strtol(v, NULL, 10);
            if (p > 1)
                probe_period = (unsigned long)p;
        }
    }
    return probe_enabled;
}

static int32_t call_int(Il2CppClass *k, const char *name, unsigned argc,
                        int32_t fallback)
{
    const MethodInfo *m = il2_method(k, name, argc);
    if (!m)
        return fallback;
    Il2CppObject *r = il2_call(m, NULL, NULL, name);
    void *v = il2_unbox(r);
    return v ? *(int32_t *)v : fallback;
}

static float call_float(Il2CppClass *k, const char *name, float fallback)
{
    const MethodInfo *m = il2_method(k, name, 0);
    if (!m)
        return fallback;
    Il2CppObject *r = il2_call(m, NULL, NULL, name);
    void *v = il2_unbox(r);
    return v ? *(float *)v : fallback;
}

/* SceneManager.GetActiveScene() returns a value type; its name comes from
 * Scene.get_name(), which takes the struct by reference. */
static void report_scenes(void)
{
    Il2CppClass *manager = il2_class("UnityEngine.SceneManagement",
                                     "SceneManager");
    if (!manager)
        return;
    int32_t loaded = call_int(manager, "get_sceneCount", 0, -1);
    fprintf(stderr, "[fp2/probe] scenes loaded=%d\n", loaded);

    const MethodInfo *active = il2_method(manager, "GetActiveScene", 0);
    Il2CppClass *scene = il2_class("UnityEngine.SceneManagement", "Scene");
    if (!active || !scene)
        return;
    /* A returned struct arrives boxed; unbox it and call get_name on the raw
     * storage, which is what the instance method expects as `this`. */
    Il2CppObject *boxed = il2_call(active, NULL, NULL, "GetActiveScene");
    void *raw = il2_unbox(boxed);
    const MethodInfo *get_name = il2_method(scene, "get_name", 0);
    const MethodInfo *is_loaded = il2_method(scene, "get_isLoaded", 0);
    if (raw && get_name) {
        char text[128];
        Il2CppObject *name = il2_call(get_name, raw, NULL, "Scene.get_name");
        int32_t ready = 0;
        if (is_loaded) {
            Il2CppObject *r = il2_call(is_loaded, raw, NULL, "isLoaded");
            void *v = il2_unbox(r);
            ready = v ? *(uint8_t *)v : 0;
        }
        fprintf(stderr, "[fp2/probe] active scene=\"%s\" loaded=%d\n",
                il2_str_utf8(name, text, sizeof text), ready);
    }
}


/* The boot flow is a NodeCanvas state machine.  Naming the state it is sitting
 * in turns "the game is stuck" into "the game is waiting inside <state>", and
 * that is the difference between fixing the cause and guessing at it. */
static void report_state_machines(void)
{
    Il2CppClass *object = il2_class("UnityEngine", "Object");
    Il2CppClass *owner = il2_class("NodeCanvas.StateMachines", "FSMOwner");
    if (!object || !owner)
        return;
    const MethodInfo *find =
        il2_method_p(object, "FindObjectsOfType", 1, "Type");
    if (!find)
        return;
    Il2CppObject *type = il2_type(owner);
    void *args[1] = { type };
    Il2CppObject *all = il2_call(find, NULL, args, "FindObjectsOfType(FSMOwner)");
    uint32_t n = il2_arr_len(all);
    fprintf(stderr, "[fp2/probe] FSMOwner instances=%u\n", n);
    if (!n)
        return;
    const MethodInfo *get_graph = il2_method_deep(owner, "graph", 0);
    if (!get_graph)
        get_graph = il2_method_deep(owner, "get_graph", 0);
    if (!get_graph)
        get_graph = il2_method_deep(owner, "get_behaviour", 0);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        Il2CppObject *o = il2_arr_at(all, i);
        if (!o || !get_graph)
            continue;
        Il2CppObject *graph = il2_call(get_graph, o, NULL, "get_graph");
        if (!graph)
            continue;
        Il2CppClass *k = il2_class_of(graph);
        const MethodInfo *state = il2_method_deep(k, "get_currentStateName", 0);
        const MethodInfo *running = il2_method_deep(k, "get_isRunning", 0);
        char text[128] = "(none)";
        if (state) {
            Il2CppObject *name =
                il2_call(state, graph, NULL, "get_currentStateName");
            il2_str_utf8(name, text, sizeof text);
        }
        int on = 0;
        if (running) {
            Il2CppObject *r = il2_call(running, graph, NULL, "get_isRunning");
            void *v = il2_unbox(r);
            on = v ? *(uint8_t *)v : 0;
        }
        fprintf(stderr, "[fp2/probe]   fsm[%u] %s state=\"%s\" running=%d\n",
                i, il2_class_name(k), text, on);

        /* The state's own name is generic ("Action State"); what identifies
         * the stall is the task inside it that never reports Success. */
        const MethodInfo *cur = il2_method_deep(k, "get_currentState", 0);
        Il2CppObject *node = cur ? il2_call(cur, graph, NULL,
                                            "get_currentState") : NULL;
        if (!node)
            continue;
        Il2CppClass *nk = il2_class_of(node);
        fprintf(stderr, "[fp2/probe]     node class=%s\n", il2_class_name(nk));
        static const char *const props[] = { "get_name", "get_summaryInfo",
                                             "get_actionList" };
        for (size_t p = 0; p < sizeof props / sizeof *props; p++) {
            const MethodInfo *m = il2_method_deep(nk, props[p], 0);
            if (!m)
                continue;
            Il2CppObject *r = il2_call(m, node, NULL, props[p]);
            if (!r)
                continue;
            if (strcmp(props[p], "get_actionList") == 0) {
                Il2CppClass *lk = il2_class_of(r);
                /* NodeCanvas keeps the task list in a public FIELD, not a
                 * property: asking for get_actions finds nothing. */
                Il2CppObject *list = NULL;
                FieldInfo *f = il2_field(lk, "actions");
                if (f)
                    il2_field_get(r, f, &list);
                Il2CppClass *listk = list ? il2_class_of(list) : NULL;
                const MethodInfo *count = listk
                    ? il2_method_deep(listk, "get_Count", 0) : NULL;
                const MethodInfo *item = listk
                    ? il2_method_deep(listk, "get_Item", 1) : NULL;
                int32_t total = 0;
                if (count) {
                    Il2CppObject *c = il2_call(count, list, NULL, "get_Count");
                    void *v = il2_unbox(c);
                    total = v ? *(int32_t *)v : 0;
                }
                for (int32_t a = 0; a < total && a < 8 && item; a++) {
                    void *idx[1] = { &a };
                    Il2CppObject *task = il2_call(item, list, idx, "get_Item");
                    if (!task)
                        continue;
                    Il2CppClass *tk = il2_class_of(task);
                    fprintf(stderr, "[fp2/probe]     action[%d] %s\n", a,
                            il2_class_name(tk));
                }
                continue;
            }
            char buffer[160];
            fprintf(stderr, "[fp2/probe]     %s=\"%s\"\n", props[p],
                    il2_str_utf8(r, buffer, sizeof buffer));
        }
    }
}

void fp2_il2_probe(unsigned long frame)
{
    /* Never on frame 0: the managed domain is still being built there and a
     * runtime_invoke against a half-initialised domain faults. */
    if (!probe_on() || frame < probe_period || (frame % probe_period) != 0)
        return;
    if (!il2_load()) {
        fprintf(stderr, "[fp2/probe] IL2CPP API unavailable\n");
        probe_enabled = 0;
        return;
    }
    il2_attach();

    Il2CppClass *camera = il2_class("UnityEngine", "Camera");
    Il2CppClass *time = il2_class("UnityEngine", "Time");
    int32_t cameras = camera ? call_int(camera, "get_allCamerasCount", 0, -1)
                             : -1;
    float scale = time ? call_float(time, "get_timeScale", -1.0f) : -1.0f;
    float since = time ? call_float(time, "get_realtimeSinceStartup", -1.0f)
                       : -1.0f;
    int32_t frames = time ? call_int(time, "get_frameCount", 0, -1) : -1;

    fprintf(stderr,
            "[fp2/probe] frame=%lu cameras=%d timeScale=%.2f "
            "realtime=%.1fs Time.frameCount=%d\n",
            frame, cameras, scale, since, frames);
    report_scenes();
    report_state_machines();
    fflush(stderr);
}
