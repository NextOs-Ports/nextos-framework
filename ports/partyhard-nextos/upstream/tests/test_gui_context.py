#!/usr/bin/env python3
"""Compile the actual adapter GUI/context code against a static-Gui fixture."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/input.c").read_text()
start = source.index("/* ===== Current screen:")
end = source.index("/* ===== Injeção Android", start)
adapter = source[start:end]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void Il2CppClass;
typedef void Il2CppObject;
typedef void MethodInfo;
typedef struct { const char *name; int flags, value; } FieldInfo;
typedef void nx_mod;
static int gui_class, object_class, enum_class, module, method;
static int visual_class, popup_object, popup_shown;
static int live = 1, paused, context = -1, field_lookups;
static const char *context_source;
static FieldInfo screen_field = {"_screenType", 16, 0};
static FieldInfo container_field = {"_screenContainer", 16, 0};
static FieldInfo popup_field = {"ControlsSelectorPopUp", 16, 0};
static FieldInfo visible_field = {"_isShow", 0, 0};
/* Deliberately NOT 2/3: the adapter must derive the enum values. */
static FieldInfo enum_fields[] = {{"GameScreen", 16, 41},
                                  {"GamePauseScreen", 16, 73}};
static void *get_fields(void *klass, void **iter) {
    assert(klass == &enum_class);
    uintptr_t index = (uintptr_t)*iter;
    if (index == 2) return NULL;
    *iter = (void *)(index + 1);
    return &enum_fields[index];
}
static const char *get_name(void *field) { return ((FieldInfo *)field)->name; }
static int get_flags(void *field) { return ((FieldInfo *)field)->flags; }
static const void *get_type(void *field) {
    assert(field == &screen_field); return &enum_class;
}
static void *class_from_type(const void *type) { return (void *)type; }
static int il2_load(void) { return 1; }
static nx_mod *nx_find_mod(const char *name) {
    assert(!strcmp(name, "libil2cpp.so")); return &module;
}
static void *nx_lookup_in(nx_mod *mod, const char *name) {
    assert(mod == &module);
    if (!strcmp(name, "il2cpp_class_get_fields")) return get_fields;
    if (!strcmp(name, "il2cpp_field_get_name")) return get_name;
    if (!strcmp(name, "il2cpp_field_get_flags")) return get_flags;
    if (!strcmp(name, "il2cpp_field_get_type")) return get_type;
    if (!strcmp(name, "il2cpp_class_from_il2cpp_type")) return class_from_type;
    assert(0); return NULL;
}
static Il2CppClass *il2_class(const char *ns, const char *name) {
    if (!strcmp(ns, "App.View") && !strcmp(name, "Gui")) return &gui_class;
    if (!strcmp(ns, "App.View") && !strcmp(name, "VisualComponent")) return &visual_class;
    assert(!strcmp(ns, "UnityEngine") && !strcmp(name, "Object"));
    return &object_class;
}
static const MethodInfo *il2_method(Il2CppClass *klass, const char *name, int argc) {
    assert(klass == &object_class && !strcmp(name, "op_Implicit") && argc == 1);
    return &method;
}
static FieldInfo *il2_field(Il2CppClass *klass, const char *name) {
    ++field_lookups;
    if (klass == &visual_class) {
        assert(!strcmp(name, "_isShow")); return &visible_field;
    }
    assert(klass == &gui_class);
    if (!strcmp(name, "_screenType")) return &screen_field;
    if (!strcmp(name, "ControlsSelectorPopUp")) return &popup_field;
    assert(!strcmp(name, "_screenContainer")); return &container_field;
}
static void il2_static_get(FieldInfo *field, void *out) {
    assert(field->flags & 16);
    if (field == &container_field) *(void **)out = live ? &live : NULL;
    else if (field == &popup_field) *(void **)out = &popup_object;
    else *(int32_t *)out = field->value;
}
static void il2_field_get(void *obj, FieldInfo *field, void *out) {
    assert(obj == &popup_object && field == &visible_field);
    *(uint8_t *)out = (uint8_t)popup_shown;
}
static Il2CppObject *il2_call(const MethodInfo *m, void *self, void **args,
                             const char *what) {
    (void)what;
    assert(m == &method && !self && args[0] == &live); return &live;
}
static void *il2_unbox(Il2CppObject *boxed) { return boxed; }
static int scene_api_state = 1, scene_is_loading, scene_is_menu = 1;
static char scene_name[96] = "MainMenu";
static void sample_scene(void) { }
static int scene_timescale_zero(void) { return paused; }
#define ST_SCENE_SAMPLE_FRAMES 30
#define ST_GPTK_CONTEXT_MENU 0
#define ST_GPTK_CONTEXT_GAMEPLAY 1
static void st_gptk_clear_context(const char *why) { context = -1; context_source = why; }
static void st_gptk_set_context(int value, const char *why) { context = value; context_source = why; }
'''

checks = r'''
int main(void) {
    assert(gui_api_resolve());
    assert(field_lookups == 4); /* No singleton/instance search at all. */
    assert(gui_game_screen == 41 && gui_pause_screen == 73);
    screen_field.value = 41;
    update_engine_context(1);
    assert(context == ST_GPTK_CONTEXT_GAMEPLAY);
    assert(!strcmp(context_source, "gui:game-screen"));
    popup_shown = 1; update_engine_context(2);
    assert(context == ST_GPTK_CONTEXT_MENU);
    assert(!strcmp(context_source, "gui:controls-selector"));
    popup_shown = 0; update_engine_context(3);
    assert(context == ST_GPTK_CONTEXT_GAMEPLAY);
    screen_field.value = 73;
    update_engine_context(2); /* Must not wait for frame 31. */
    assert(context == ST_GPTK_CONTEXT_MENU);
    assert(!strcmp(context_source, "gui:pause-screen"));
    screen_field.value = 13; /* First-run control selector window. */
    update_engine_context(3);
    assert(context == ST_GPTK_CONTEXT_MENU);
    screen_field.value = 41; paused = 1;
    update_engine_context(4);
    assert(context == ST_GPTK_CONTEXT_MENU);
    paused = 0; update_engine_context(5);
    assert(context == ST_GPTK_CONTEXT_GAMEPLAY);
    live = 0; update_engine_context(6);
    assert(context == -1 && gui_screen_value == -1);
    assert(!strcmp(context_source, "gui:not-ready"));
    strcpy(scene_name, "cutscene"); update_engine_context(7);
    assert(context == ST_GPTK_CONTEXT_MENU);
    assert(!strcmp(context_source, "scene:standalone-menu"));
    strcpy(scene_name, "AndroidLicensePermissionResolver"); update_engine_context(8);
    assert(context == ST_GPTK_CONTEXT_MENU);
    strcpy(scene_name, "MainMenu"); update_engine_context(9);
    assert(context == -1);
    live = 1; update_engine_context(7);
    assert(context == ST_GPTK_CONTEXT_GAMEPLAY);
    update_engine_context(0); assert(context == -1);
    gui_api_state = 0; screen_field.flags = 0;
    assert(!gui_api_resolve()); /* Never read an instance field as static. */
    puts("PASS: static Gui, live-container gate, popup/pause, same-MainMenu gameplay, immediate transition, stale-state reset, metadata enum, invalid storage");
}
'''

with tempfile.TemporaryDirectory(prefix="partyhard-gui-test-", dir=os.getenv("TMPDIR")) as temp:
    c_file = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    c_file.write_text(fixture + adapter + checks)
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
