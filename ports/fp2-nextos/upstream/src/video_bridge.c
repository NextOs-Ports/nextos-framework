/*
 * video_bridge.c -- deliver the transition the intro's PlayVideo waits for.
 *
 * Freedom Planet 2 boots into the scene "Intro", whose NodeCanvas state machine sits in
 * an ActionState containing a PlayVideo task.  That task ends when Unity's
 * video player reports the clip finished.  On this device it never can: the
 * clip is H.264 inside the APK and there is no Android media stack to decode
 * it, so Unity's own log ends at "AndroidVideoMedia surface creation stalled"
 * and the FSM waits forever.  The whole game is behind that one transition --
 * the boot scene has zero cameras until the FSM leaves this state, which is
 * exactly the "cameras=0" wall the first attempt at this port spent four
 * sessions on, misread as a job-system deadlock.
 *
 * The rule for a peripheral (DRM, ads, social, VIDEO) is a stub that answers
 * "ok" so the game's own flow continues.  This is that stub, placed at the one
 * point where the missing answer belongs: when the decoder has reported -- as a
 * measured fact, not a timeout -- that it cannot produce a single frame, the
 * running PlayVideo task is ended with success, the same call the video player
 * would make at the clip's end.  The FSM then takes its own transition, loads
 * the menu and the game continues along its normal path.
 *
 * Deliberately NOT done here: skipping the state, forcing a scene load, or
 * touching any other task.  Only the video task, only after the decoder said
 * no, and only once per task instance.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il2.h"
#include "mediandk_shim.h"
#include "nx_elf.h"

#define SEEN_MAX 16
static void *ended[SEEN_MAX];
static unsigned ended_n;

static int already_ended(void *task)
{
    for (unsigned i = 0; i < ended_n; i++)
        if (ended[i] == task)
            return 1;
    if (ended_n < SEEN_MAX)
        ended[ended_n++] = task;
    return 0;
}

/* NodeCanvas's ActionTask.EndAction(bool) is protected; il2cpp_runtime_invoke
 * does not enforce C# accessibility, and this is the same method the task's own
 * video callback would call. */
static int end_task(Il2CppObject *task, Il2CppClass *k)
{
    const MethodInfo *end = il2_method_deep(k, "EndAction", 1);
    if (!end)
        end = il2_method_deep(k, "EndAction", 0);
    if (!end)
        return 0;
    uint8_t ok = 1;
    void *args[1] = { &ok };
    il2_call(end, task, args, "PlayVideo.EndAction");
    return 1;
}

void fp2_video_bridge_tick(unsigned long frame)
{
    static int disabled;
    if (disabled || !fp2_media_video_gave_up())
        return;
    /* Cheap: only look while the video is known to be undecodable, and only a
     * few times a second. */
    if (frame % 30)
        return;
    if (!il2_load()) {
        disabled = 1;
        return;
    }
    il2_attach();

    Il2CppClass *object = il2_class("UnityEngine", "Object");
    Il2CppClass *owner = il2_class("NodeCanvas.StateMachines", "FSMOwner");
    if (!object || !owner) {
        disabled = 1;
        return;
    }
    const MethodInfo *find = il2_method_p(object, "FindObjectsOfType", 1,
                                          "Type");
    if (!find) {
        disabled = 1;
        return;
    }
    void *args[1] = { il2_type(owner) };
    Il2CppObject *all = il2_call(find, NULL, args, "FindObjectsOfType(FSMOwner)");
    uint32_t n = il2_arr_len(all);
    for (uint32_t i = 0; i < n; i++) {
        Il2CppObject *fsm_owner = il2_arr_at(all, i);
        if (!fsm_owner)
            continue;
        const MethodInfo *get_graph = il2_method_deep(il2_class_of(fsm_owner),
                                                      "get_graph", 0);
        if (!get_graph)
            get_graph = il2_method_deep(il2_class_of(fsm_owner),
                                        "get_behaviour", 0);
        Il2CppObject *graph = get_graph
            ? il2_call(get_graph, fsm_owner, NULL, "get_graph") : NULL;
        if (!graph)
            continue;
        Il2CppClass *gk = il2_class_of(graph);
        const MethodInfo *cur = il2_method_deep(gk, "get_currentState", 0);
        Il2CppObject *node = cur ? il2_call(cur, graph, NULL,
                                            "get_currentState") : NULL;
        if (!node)
            continue;
        Il2CppClass *nk = il2_class_of(node);
        const MethodInfo *get_list = il2_method_deep(nk, "get_actionList", 0);
        Il2CppObject *action_list = get_list
            ? il2_call(get_list, node, NULL, "get_actionList") : NULL;
        if (!action_list)
            continue;
        FieldInfo *f = il2_field(il2_class_of(action_list), "actions");
        Il2CppObject *list = NULL;
        if (f)
            il2_field_get(action_list, f, &list);
        if (!list)
            continue;
        Il2CppClass *lk = il2_class_of(list);
        const MethodInfo *count = il2_method_deep(lk, "get_Count", 0);
        const MethodInfo *item = il2_method_deep(lk, "get_Item", 1);
        if (!count || !item)
            continue;
        int32_t total = 0;
        Il2CppObject *boxed = il2_call(count, list, NULL, "get_Count");
        void *value = il2_unbox(boxed);
        total = value ? *(int32_t *)value : 0;
        for (int32_t a = 0; a < total; a++) {
            void *index[1] = { &a };
            Il2CppObject *task = il2_call(item, list, index, "get_Item");
            if (!task)
                continue;
            Il2CppClass *tk = il2_class_of(task);
            const char *name = il2_class_name(tk);
            if (!name || strcmp(name, "PlayVideo") != 0)
                continue;
            const MethodInfo *active = il2_method_deep(tk, "get_isActive", 0);
            int running = 1;
            if (active) {
                Il2CppObject *r = il2_call(active, task, NULL, "get_isActive");
                void *v = il2_unbox(r);
                running = v ? *(uint8_t *)v : 0;
            }
            if (!running || already_ended(task))
                continue;
            if (end_task(task, tk))
                fprintf(stderr,
                        "[fp2/video] intro clip is not decodable on this "
                        "device; PlayVideo ended so the boot flow continues\n");
        }
    }
}
