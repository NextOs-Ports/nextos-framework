#!/usr/bin/env python3
"""Compile the real glyph repair with a visibility/lifecycle fixture."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/input.c").read_text()
start = source.index("static int glyph_bool(")
end = source.index("/* ===== Poll por quadro", start)
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef const char Il2CppClass;
typedef const char FieldInfo;
typedef const char MethodInfo;
static void *controller = (void *)1;
static int input_diag, context = 1;
static uint8_t mobile = 1, finished, enabled = 1, active, parent_visible = 1;
static int current_clip_present = 1, writes;
static uint32_t count = 1;
static char sign, input, tutorial_object, animator, glyph, transform, parent, parent_go, array, clip;
static const char *input_class = "XboxGamepadButtons";
#define ST_GPTK_CONTEXT_GAMEPLAY 1
static int st_gptk_context(void) { return context; }
static int il2_load(void) { return 1; }
static Il2CppClass *il2_class(const char *ns, const char *name) { (void)ns; return name; }
static FieldInfo *il2_field(Il2CppClass *klass, const char *name) { (void)klass; return name; }
static const MethodInfo *il2_method(Il2CppClass *klass, const char *name, unsigned argc) {
    (void)klass; (void)argc; return name;
}
static const MethodInfo *il2_method_p(Il2CppClass *klass, const char *name, unsigned argc, const char *param) {
    assert(!strcmp(param, "Type")); return il2_method(klass, name, argc);
}
static void il2_static_get(FieldInfo *field, void *out) {
    if (!strcmp(field, "<TouchMobileMode>k__BackingField")) *(uint8_t *)out = mobile;
    else { assert(!strcmp(field, "_instance")); *(void **)out = &sign; }
}
static void il2_field_get(void *obj, FieldInfo *field, void *out) {
    if (!strcmp(field, "currentInput")) { assert(obj == &sign); *(void **)out = &input; }
    else if (!strcmp(field, "finishGame")) { assert(obj == &tutorial_object); *(uint8_t *)out = finished; }
    else { assert(obj == &tutorial_object && !strcmp(field, "infoSignSelectAnimation")); *(void **)out = &animator; }
}
static Il2CppClass *il2_class_of(void *obj) { assert(obj == &input); return input_class; }
static const char *il2_class_name(Il2CppClass *klass) { return klass; }
static const char *il2_str_utf8(void *obj, char *out, size_t size) {
    (void)obj; snprintf(out, size, "test"); return out;
}
static void *il2_type(Il2CppClass *klass) { assert(!strcmp(klass, "TutorialArrowButtonSign")); return (void *)klass; }
static void *il2_unbox(void *obj) { return obj; }
static uint32_t il2_arr_len(void *obj) { assert(obj == &array); return count; }
static void *il2_arr_at(void *obj, uint32_t i) { assert(obj == &array && !i); return &tutorial_object; }
static void *il2_call(const MethodInfo *method, void *obj, void **args, const char *why) {
    (void)why;
    if (!strcmp(method, "FindObjectsOfType")) { assert(!obj && args); return &array; }
    if (!strcmp(method, "get_isActiveAndEnabled")) { assert(obj == &tutorial_object); return &enabled; }
    if (!strcmp(method, "get_CurrentClip")) { assert(obj == &animator); return current_clip_present ? &clip : NULL; }
    if (!strcmp(method, "get_gameObject")) { assert(obj == &animator || obj == &parent); return obj == &animator ? &glyph : &parent_go; }
    if (!strcmp(method, "get_activeSelf")) { assert(obj == &glyph); return &active; }
    if (!strcmp(method, "get_transform")) { assert(obj == &glyph); return &transform; }
    if (!strcmp(method, "get_parent")) { assert(obj == &transform); return &parent; }
    if (!strcmp(method, "get_activeInHierarchy")) { assert(obj == &parent_go); return &parent_visible; }
    assert(!strcmp(method, "SetActive") && obj == &glyph && *(uint8_t *)args[0] == 1);
    active = 1; ++writes; return NULL;
}
'''
checks = r'''
int main(void) {
    restore_tutorial_gamepad_glyphs(30); assert(active && writes == 1);
    restore_tutorial_gamepad_glyphs(60); assert(writes == 1);
    active = 0; context = 0; restore_tutorial_gamepad_glyphs(90); assert(!active);
    context = 1; controller = NULL; restore_tutorial_gamepad_glyphs(90); assert(!active);
    controller = &input; mobile = 0; restore_tutorial_gamepad_glyphs(90); assert(!active);
    mobile = 1; input_class = "KeyboardButtons"; restore_tutorial_gamepad_glyphs(90); assert(!active);
    input_class = "XboxGamepadButtons"; parent_visible = 0; restore_tutorial_gamepad_glyphs(90); assert(!active);
    parent_visible = 1; finished = 1; restore_tutorial_gamepad_glyphs(90); assert(!active);
    finished = 0; enabled = 0; restore_tutorial_gamepad_glyphs(90); assert(!active);
    enabled = 1; current_clip_present = 0; restore_tutorial_gamepad_glyphs(90); assert(!active);
    current_clip_present = 1; count = 17; restore_tutorial_gamepad_glyphs(90); assert(!active);
    count = 1; restore_tutorial_gamepad_glyphs(91); assert(!active);
    restore_tutorial_gamepad_glyphs(120); assert(active && writes == 2);
    assert(mobile == 1 && input_class && !finished); /* No gameplay state writes. */
    puts("PASS: existing glyph only; menu, hidden parent, finished tutorial, non-gamepad, missing animation and duplicate writes guarded");
}
'''
with tempfile.TemporaryDirectory(prefix="partyhard-glyph-test-", dir=os.getenv("TMPDIR")) as temp:
    c_file = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    c_file.write_text(fixture + source[start:end] + checks)
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
