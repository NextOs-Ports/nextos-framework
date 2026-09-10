#!/usr/bin/env python3
"""Exercise the actual hover ownership repair, without game data or SDL."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/input.c").read_text()
start = source.index("static void update_menu_focus(")
end = source.index("/* ===== Tutorial glyph visibility", start)
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef const char Il2CppClass;
typedef const char FieldInfo;
#define ST_GPTK_CONTEXT_MENU 0
static int context, loaded = 1, present = 1, writes, input_diag;
static char instance;
static uint8_t mouse = 1;
static int st_gptk_context(void) { return context; }
static int il2_load(void) { return loaded; }
static Il2CppClass *il2_class(const char *ns, const char *name) {
    assert(!strcmp(ns, "Assets.Scripts.Controllers.Game"));
    assert(!strcmp(name, "MenuInputManager")); return name;
}
static FieldInfo *il2_field(Il2CppClass *klass, const char *name) {
    assert(klass); return name;
}
static void il2_static_get(FieldInfo *field, void *out) {
    assert(!strcmp(field, "instance")); *(void **)out = present ? &instance : NULL;
}
static void il2_field_get(void *obj, FieldInfo *field, void *out) {
    assert(obj == &instance && !strcmp(field, "mouseControl"));
    *(uint8_t *)out = mouse;
}
static void il2_field_set(void *obj, FieldInfo *field, void *value) {
    assert(obj == &instance && !strcmp(field, "mouseControl"));
    mouse = *(uint8_t *)value; ++writes;
}
'''
checks = r'''
int main(void) {
    update_menu_focus(0, 0); assert(mouse == 1 && writes == 0);
    update_menu_focus(1, 0); assert(mouse == 0 && writes == 1);
    update_menu_focus(0, 0); assert(mouse == 0 && writes == 1);
    /* Unity may re-enable hover from its remembered touch coordinate. */
    mouse = 1; update_menu_focus(0, 0); assert(mouse == 0 && writes == 2);
    update_menu_focus(0, 1); assert(mouse == 1 && writes == 3);
    update_menu_focus(1, 1); assert(mouse == 1); /* Real pointer wins. */
    context = 1; update_menu_focus(1, 0); assert(mouse == 1);
    context = 0; update_menu_focus(0, 0); assert(mouse == 1);
    present = 0; update_menu_focus(1, 0); assert(mouse == 1);
    present = 1; update_menu_focus(0, 0); assert(mouse == 0);
    context = -1; mouse = 1; update_menu_focus(1, 0); assert(mouse == 1);
    context = 0; loaded = 0; update_menu_focus(1, 0); assert(mouse == 1);
    loaded = 1; update_menu_focus(0, 0); assert(mouse == 0);
    puts("PASS: native menu focus survives release/stale hover; real pointer restores hover; gameplay/unknown/missing engine untouched");
}
'''
with tempfile.TemporaryDirectory(prefix="partyhard-focus-", dir=os.getenv("TMPDIR")) as temp:
    c_file, binary = Path(temp) / "test.c", Path(temp) / "test"
    c_file.write_text(fixture + source[start:end] + checks)
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
