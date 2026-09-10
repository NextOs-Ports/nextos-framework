#!/usr/bin/env python3
"""Exercise the actual tutorial-return adapter using host-only IL2CPP objects."""
from pathlib import Path
import os
import subprocess
import tempfile

port = Path(__file__).resolve().parents[1]
source = (port / "src/tutorial_return.c").read_text()
# Keep every function from the real adapter, replacing only its engine headers.
for header in ("il2.h", "nx_elf.h", "tutorial_return.h"):
    source = source.replace(f'#include "{header}"', "")

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *ns, *name; } Il2CppClass;
typedef struct { const char *name; unsigned argc; } MethodInfo;
typedef struct { const char *name; int owner, kind; } FieldInfo;
typedef int nx_mod;
enum {
    GUI, BOARD, WORKSHOP, MANAGER, MENU, VISUAL, CONTROLLER, SAVE, LEVEL,
    BUTTON, LOADER, ENTRY, ITEMS, SHADOW, ACTION, LIST, ARRAY, REFLECTION, TYPE,
    CLASS_COUNT
};
static Il2CppClass classes[] = {
    {"App.View", "Gui"}, {"App.View.Interface", "BoardSceeen"},
    {"Assets.Scripts.View.Menu", "BoardWorkshopMenu"},
    {"Assets.Scripts.Controllers.Game", "MenuInputManager"},
    {"App.View.Interface", "MenuContext"}, {"App.View", "VisualComponent"},
    {"Assets.Scripts.Controllers.Game.SaveGame", "SaveGameController"},
    {"Assets.Scripts.Controllers.Game.SaveGame", "SaveGameData"},
    {"Assets.Scripts.Controllers.Game.SaveGame", "SaveGameLevelData"},
    {"App.Core.ButtonsManager", "GestureButton"},
    {"", "AppLoaderMainMenu"}, {"", "EnterPointMainMenu"},
    {"Assets.Scripts.View.Menu", "BoardItemsMenu"},
    {"App.View.Interface", "GameShadow"}, {"System", "Action"},
    {"System.Collections.Generic", "List"}, {"System", "Array"},
    {"System.Reflection", "MethodInfo"}, {"System", "Type"}
};
enum { REF, INT, BOOL };
enum {
    F_BOARD, F_LEVELS, F_ITEMS, F_WORKSHOP, F_WORKSHOP_ITEM,
    F_MANAGER_INSTANCE, F_MENU, F_SHOWN, F_BUTTONS, F_SAVE_INSTANCE, F_SAVE,
    F_CURRENT, F_PLAYED, F_POOL, F_COMPLETED_LIST, F_NUMBER, F_LEVEL,
    F_COMPLETED, F_ACTIVE, F_VISIBLE, F_CLICK, F_START, F_ENTRY, F_ENTRY_LEVEL,
    F_SHADOW, F_SHADOW_HIDE, F_SHADOW_COMPLETE, F_BUTTON_OBJECTS,
    F_SIZE, F_LIST_ITEMS, FIELD_COUNT
};
static FieldInfo fields[] = {
    {"BoardScrn", GUI, REF}, {"boardLevelsMenu", BOARD, REF},
    {"boardItemsMenu", BOARD, REF}, {"boardWorkshopMenu", BOARD, REF},
    {"currentItem", WORKSHOP, REF}, {"instance", MANAGER, REF},
    {"menu", MANAGER, REF}, {"_isShow", VISUAL, BOOL},
    {"_menuButtons", MENU, REF}, {"instance", CONTROLLER, REF},
    {"saveGame", CONTROLLER, REF}, {"currentLevelNum", SAVE, INT},
    {"playedLevelNum", SAVE, INT}, {"levelPool", SAVE, REF},
    {"completedLevels", SAVE, REF}, {"levelNumber", LEVEL, INT},
    {"level", LEVEL, INT}, {"completed", LEVEL, BOOL},
    {"Active", BUTTON, BOOL}, {"visible", BUTTON, BOOL},
    {"_onClick", BUTTON, REF}, {"_onStart", BUTTON, REF},
    {"enterPoint", LOADER, REF}, {"Level", ENTRY, INT},
    {"Shadow", GUI, REF}, {"_onHide", SHADOW, REF},
    {"_onComplete", SHADOW, REF}, {"_menuButtonsGO", MENU, REF},
    {"_size", LIST, INT}, {"_items", LIST, REF}
};
typedef union { void *ref; int32_t integer; uint8_t flag; } Value;
typedef struct Object {
    Il2CppClass *klass;
    Value value[FIELD_COUNT];
    uint32_t length;
    struct Object *element[65];
} Object;
typedef struct {
    Object save, pool, pool_array, completed, completed_array, levels[21];
} SaveState;
static struct {
    SaveState data;
    Object board, levels_menu, items, workshop, manager, controller, entry;
    Object buttons, buttons_array, button[21], original[21];
    Object shadow, item_objects, item_objects_array;
    Object action, reflection, action_type, stranger;
} world;
static Object *static_board, *static_manager, *static_controller, *static_entry;
static Object *static_shadow;
static nx_mod module;
static const MethodInfo methods[] = {
    {"SetActivate", 1}, {"SelectFirstActive", 0}, {"OnClickStart", 0},
    {"CreateDelegate", 2}
};
enum { ACTIVATE, SELECT_FIRST, NATIVE_START, CREATE_DELEGATE };
static const char *missing_class, *missing_field, *missing_method, *missing_symbol;
static int loaded, module_present, reflection_present, type_present;
static int action_present, action_valid, writes, enables, disables, selects;
static int delegates, native_starts, new_calls, freed, fail_new_at;
static int user_activation;
static const MethodInfo *reflected_method, *delegate_method;
static Object *delegate_target;
static Object *handles[16];
static int live_handles;
/* The target runtime uses pointer-sized GC handles, including their high bits. */
#define HANDLE_BASE ((uintptr_t)UINT64_C(0x712345670000))
static uintptr_t handle_index(uintptr_t handle) {
    assert(sizeof(uintptr_t) > sizeof(uint32_t));
    assert((handle & ~(uintptr_t)15) == HANDLE_BASE);
    uintptr_t index = handle - HANDLE_BASE;
    assert(index > 0 && index < 16);
    return index;
}

static int il2_load(void) { return loaded; }
static Il2CppClass *il2_class(const char *ns, const char *name) {
    if (missing_class && !strcmp(missing_class, name)) return NULL;
    for (int i = 0; i < CLASS_COUNT; ++i)
        if (!strcmp(classes[i].ns, ns) && !strcmp(classes[i].name, name))
            return &classes[i];
    assert(!"unexpected class lookup"); return NULL;
}
static Il2CppClass *il2_class_of(void *obj) {
    assert(obj); return ((Object *)obj)->klass;
}
static FieldInfo *il2_field(Il2CppClass *klass, const char *name) {
    assert(klass);
    if (missing_field && !strcmp(missing_field, name)) return NULL;
    for (int i = 0; i < FIELD_COUNT; ++i)
        if (klass == &classes[fields[i].owner] && !strcmp(name, fields[i].name))
            return &fields[i];
    assert(!"unexpected field lookup"); return NULL;
}
static void il2_static_get(FieldInfo *field, void *out) {
    if (field == &fields[F_BOARD]) *(void **)out = static_board;
    else if (field == &fields[F_MANAGER_INSTANCE]) *(void **)out = static_manager;
    else if (field == &fields[F_SAVE_INSTANCE]) *(void **)out = static_controller;
    else if (field == &fields[F_ENTRY]) *(void **)out = static_entry;
    else if (field == &fields[F_SHADOW]) *(void **)out = static_shadow;
    else assert(!"unexpected static access");
}
static void il2_field_get(void *obj, FieldInfo *field, void *out) {
    assert(obj && field);
    Value *value = &((Object *)obj)->value[field - fields];
    if (field->kind == REF) *(void **)out = value->ref;
    else if (field->kind == INT) *(int32_t *)out = value->integer;
    else *(uint8_t *)out = value->flag;
}
static void mock_set_reference(void *obj, FieldInfo *field, void *value) {
    /* A write to any save/progression/selection field fails immediately. */
    assert(obj && field == &fields[F_CLICK]);
    assert(((Object *)obj)->klass == &classes[BUTTON]);
    ((Object *)obj)->value[F_CLICK].ref = value;
    ++writes;
}
static uint32_t il2_arr_len(void *obj) {
    assert(obj && ((Object *)obj)->klass == &classes[ARRAY]);
    return ((Object *)obj)->length;
}
static void *il2_arr_at(void *obj, uint32_t index) {
    assert(index < il2_arr_len(obj) && index < 65);
    return ((Object *)obj)->element[index];
}
static const MethodInfo *il2_method(Il2CppClass *klass, const char *name,
                                    unsigned argc) {
    if (missing_method && !strcmp(missing_method, name)) return NULL;
    for (int i = 0; i < 4; ++i) {
        if (strcmp(methods[i].name, name) || methods[i].argc != argc) continue;
        int expected[] = {BUTTON, MENU, ITEMS, REFLECTION};
        assert(klass == &classes[expected[i]]); return &methods[i];
    }
    assert(!"unexpected method lookup"); return NULL;
}
static void *il2_type(Il2CppClass *klass) {
    assert(klass == &classes[ACTION]);
    return type_present ? &world.action_type : NULL;
}
static void *il2_call(const MethodInfo *method, void *self, void **args,
                      const char *why) {
    (void)why;
    if (method == &methods[CREATE_DELEGATE]) {
        assert(self == &world.reflection && args);
        assert(args[0] == &world.action_type && args[1] == &world.items);
        ++delegates;
        delegate_method = reflected_method; delegate_target = args[1];
        world.action.klass = &classes[action_valid ? ACTION : ITEMS];
        return action_present ? &world.action : NULL;
    }
    if (method == &methods[ACTIVATE]) {
        assert(self && ((Object *)self)->klass == &classes[BUTTON] && args);
        uint8_t active = *(uint8_t *)args[0];
        assert(active <= 1);
        ((Object *)self)->value[F_ACTIVE].flag = active;
        if (active) ++enables; else ++disables;
        return NULL;
    }
    if (method == &methods[SELECT_FIRST]) {
        assert(self == &world.levels_menu && !args);
        assert(world.button[0].value[F_ACTIVE].flag); ++selects; return NULL;
    }
    assert(user_activation && method == &methods[NATIVE_START]);
    assert(self == &world.items && !args);
    ++native_starts; return NULL;
}
static void *mock_method_object(const MethodInfo *method, Il2CppClass *klass) {
    assert(method == &methods[NATIVE_START] && klass == &classes[ITEMS]);
    reflected_method = method;
    return reflection_present ? &world.reflection : NULL;
}
static uintptr_t mock_handle_new(void *obj, int pinned) {
    assert(obj && !pinned);
    if (++new_calls == fail_new_at) return 0;
    for (uintptr_t i = 1; i < 16; ++i) {
        if (!handles[i]) { handles[i] = obj; ++live_handles; return HANDLE_BASE + i; }
    }
    assert(!"too many live handles"); return 0;
}
static void *mock_handle_target(uintptr_t handle) {
    return handles[handle_index(handle)];
}
static void mock_handle_free(uintptr_t handle) {
    uintptr_t index = handle_index(handle);
    assert(handles[index]);
    handles[index] = NULL; ++freed; --live_handles;
}
static nx_mod *nx_find_mod(const char *name) {
    assert(!strcmp(name, "libil2cpp.so")); return module_present ? &module : NULL;
}
static void *nx_lookup_in(nx_mod *mod, const char *name) {
    assert(mod == &module);
    if (missing_symbol && !strcmp(missing_symbol, name)) return NULL;
    if (!strcmp(name, "il2cpp_method_get_object")) return mock_method_object;
    if (!strcmp(name, "il2cpp_gchandle_new")) return mock_handle_new;
    if (!strcmp(name, "il2cpp_gchandle_get_target")) return mock_handle_target;
    if (!strcmp(name, "il2cpp_gchandle_free")) return mock_handle_free;
    if (!strcmp(name, "il2cpp_field_set_value_object")) return mock_set_reference;
    assert(!"unexpected symbol lookup"); return NULL;
}
'''

checks = r'''
static void init_object(Object *obj, int klass) {
    memset(obj, 0, sizeof *obj); obj->klass = &classes[klass];
}
static void init_list(Object *list, Object *array, int count, int capacity) {
    init_object(list, LIST); init_object(array, ARRAY);
    list->value[F_SIZE].integer = count; list->value[F_LIST_ITEMS].ref = array;
    array->length = (uint32_t)capacity;
}
static void fresh(void) {
    assert(!live_handles);
    memset(&world, 0, sizeof world); memset(handles, 0, sizeof handles);
    resolved = 0; button_handle = original_handle = action_handle = 0;
    missing_class = missing_field = missing_method = missing_symbol = NULL;
    loaded = module_present = reflection_present = type_present = 1;
    action_present = action_valid = 1;
    writes = enables = disables = selects = delegates = native_starts = 0;
    new_calls = freed = fail_new_at = 0;
    user_activation = 0; reflected_method = delegate_method = NULL;
    delegate_target = NULL;
    SaveState *data = &world.data;
    init_object(&data->save, SAVE);
    init_list(&data->pool, &data->pool_array, 21, 21);
    init_list(&data->completed, &data->completed_array, 0, 0);
    data->save.value[F_POOL].ref = &data->pool;
    data->save.value[F_COMPLETED_LIST].ref = &data->completed;
    for (int i = 0; i < 21; ++i) {
        init_object(&data->levels[i], LEVEL);
        data->levels[i].value[F_NUMBER].integer = i;
        data->levels[i].value[F_LEVEL].integer = i == 0 ? 80 : i;
        data->pool_array.element[i] = &data->levels[i];
        init_object(&world.button[i], BUTTON);
        init_object(&world.original[i], ACTION);
        world.button[i].value[F_VISIBLE].flag = 1;
        world.button[i].value[F_ACTIVE].flag = (i == 20);
        world.button[i].value[F_CLICK].ref = &world.original[i];
    }
    init_object(&world.board, BOARD); init_object(&world.levels_menu, MENU);
    init_object(&world.items, ITEMS); init_object(&world.workshop, WORKSHOP);
    init_object(&world.manager, MANAGER); init_object(&world.controller, CONTROLLER);
    init_object(&world.entry, ENTRY); init_object(&world.action, ACTION);
    init_object(&world.reflection, REFLECTION); init_object(&world.action_type, TYPE);
    init_object(&world.stranger, ACTION);
    init_object(&world.shadow, SHADOW);
    init_list(&world.item_objects, &world.item_objects_array, 6, 6);
    world.item_objects_array.element[5] = &world.stranger;
    world.items.value[F_BUTTON_OBJECTS].ref = &world.item_objects;
    init_list(&world.buttons, &world.buttons_array, 21, 21);
    for (int i = 0; i < 21; ++i) world.buttons_array.element[i] = &world.button[i];
    world.board.value[F_LEVELS].ref = &world.levels_menu;
    world.board.value[F_ITEMS].ref = &world.items;
    world.board.value[F_WORKSHOP].ref = &world.workshop;
    world.board.value[F_SHOWN].flag = world.levels_menu.value[F_SHOWN].flag = 1;
    world.manager.value[F_MENU].ref = &world.levels_menu;
    world.controller.value[F_SAVE].ref = &data->save;
    world.levels_menu.value[F_BUTTONS].ref = &world.buttons;
    static_board = &world.board; static_manager = &world.manager;
    static_controller = &world.controller; static_entry = &world.entry;
    static_shadow = &world.shadow;
}
static void poll_unchanged_save(int context) {
    SaveState before = world.data;
    int starts = native_starts;
    st_tutorial_return_poll(context);
    assert(!memcmp(&before, &world.data, sizeof before));
    assert(native_starts == starts); /* Polling never fabricates a click. */
}
static void assert_stock(void) {
    assert(!writes && !enables && !disables && !selects && !native_starts);
    assert(!live_handles && !button_handle && !original_handle && !action_handle);
    for (int i = 0; i < 21; ++i) {
        assert(world.button[i].value[F_CLICK].ref == &world.original[i]);
        assert(world.button[i].value[F_ACTIVE].flag == (i == 20));
    }
}
static void reject_save(void) {
    SaveState before = world.data;
    assert(resolve()); assert(!incomplete_tutorial(&world.data.save));
    assert(!memcmp(&before, &world.data, sizeof before));
    poll_unchanged_save(1); assert_stock();
}
static void test_progress_contract(void) {
    fresh(); assert(resolve()); assert(incomplete_tutorial(&world.data.save));
    assert(!incomplete_tutorial(NULL));
    int bad_numbers[] = {-1, 1, 80};
    for (unsigned i = 0; i < sizeof bad_numbers / sizeof *bad_numbers; ++i) {
        fresh(); world.data.save.value[F_CURRENT].integer = bad_numbers[i]; reject_save();
        fresh(); world.data.save.value[F_PLAYED].integer = bad_numbers[i]; reject_save();
    }
    int bad_counts[] = {-1, 0, 20, 22, 65};
    for (unsigned i = 0; i < sizeof bad_counts / sizeof *bad_counts; ++i) {
        fresh(); world.data.pool.value[F_SIZE].integer = bad_counts[i]; reject_save();
    }
    for (int i = 0; i < 21; ++i) {
        fresh(); world.data.levels[i].value[F_NUMBER].integer = i + 1; reject_save();
        fresh(); world.data.levels[i].value[F_COMPLETED].flag = 1; reject_save();
        fresh(); world.data.pool_array.element[i] = NULL; reject_save();
    }
    fresh(); world.data.levels[0].value[F_LEVEL].integer = 0; reject_save();
    fresh(); world.data.pool_array.length = 20; reject_save();
    fresh(); world.data.pool.value[F_LIST_ITEMS].ref = NULL; reject_save();
    fresh(); world.data.save.value[F_POOL].ref = NULL; reject_save();
    fresh(); world.data.save.value[F_COMPLETED_LIST].ref = NULL; reject_save();
    fresh(); world.data.completed.value[F_LIST_ITEMS].ref = NULL; reject_save();
    fresh(); world.data.completed.value[F_SIZE].integer = -1; reject_save();
    fresh(); world.data.completed.value[F_SIZE].integer = 1;
    world.data.completed_array.length = 1; reject_save();
    fresh(); world.data.completed.value[F_SIZE].integer = 1; reject_save();
    fresh(); missing_field = "_size"; reject_save();
    fresh(); missing_field = "_items"; reject_save();
    /* Capacity is not Count: unused backing slots are legitimate. */
    fresh(); world.data.pool_array.length = 64; world.data.completed_array.length = 8;
    assert(resolve()); assert(incomplete_tutorial(&world.data.save));
    puts("PASS: strict tutorial identity, all 21 ordered/uncompleted levels, initial progress, malformed list bounds, no save writes");
}
static void test_delegate_lifecycle(void) {
    fresh();
    Object other_buttons[20]; memcpy(other_buttons, world.button + 1, sizeof other_buttons);
    poll_unchanged_save(1);
    assert(writes == 1 && enables == 1 && selects == 1 && delegates == 1);
    assert(live_handles == 3 && !freed && !native_starts);
    assert(world.button[0].value[F_ACTIVE].flag);
    assert(world.button[0].value[F_CLICK].ref == &world.action);
    assert(!memcmp(other_buttons, world.button + 1, sizeof other_buttons));
    poll_unchanged_save(1); poll_unchanged_save(1);
    assert(writes == 1 && delegates == 1 && selects == 1 && new_calls == 3);
    /* Only a real user activation runs the already-native start action. */
    assert(world.button[0].value[F_CLICK].ref == &world.action);
    assert(delegate_method && delegate_target == &world.items);
    user_activation = 1;
    il2_call(delegate_method, delegate_target, NULL, "user activation");
    user_activation = 0;
    assert(native_starts == 1);
    poll_unchanged_save(0);
    assert(writes == 2 && disables == 1 && freed == 3 && !live_handles);
    assert(world.button[0].value[F_CLICK].ref == &world.original[0]);
    assert(!world.button[0].value[F_ACTIVE].flag);
    assert(!memcmp(other_buttons, world.button + 1, sizeof other_buttons));
    poll_unchanged_save(0); assert(freed == 3 && writes == 2);
    poll_unchanged_save(1); assert(live_handles == 3 && delegates == 2);
    poll_unchanged_save(0); assert(freed == 6 && !live_handles);
    fresh(); world.entry.value[F_ENTRY_LEVEL].integer = 80;
    poll_unchanged_save(1); assert(enables == 1); poll_unchanged_save(0);
    puts("PASS: only first poster binds native delegate; no synthetic click; stable polling; exact restore and pointer-sized GC handles on exit/re-entry");
}
static void test_menu_guards(void) {
    for (int guard = 0; guard < 27; ++guard) {
        fresh();
        switch (guard) {
        case 0: static_board = NULL; break;
        case 1: static_manager = NULL; break;
        case 2: static_controller = NULL; break;
        case 3: static_entry = NULL; break;
        case 4: world.board.value[F_LEVELS].ref = NULL; break;
        case 5: world.board.value[F_ITEMS].ref = NULL; break;
        case 6: world.board.value[F_WORKSHOP].ref = NULL; break;
        case 7: world.manager.value[F_MENU].ref = &world.items; break;
        case 8: world.board.value[F_SHOWN].flag = 0; break;
        case 9: world.levels_menu.value[F_SHOWN].flag = 0; break;
        case 10: world.workshop.value[F_WORKSHOP_ITEM].ref = &world.stranger; break;
        case 11: world.entry.value[F_ENTRY_LEVEL].integer = 1; break;
        case 12: world.entry.value[F_ENTRY_LEVEL].integer = -1; break;
        case 13: world.buttons.value[F_SIZE].integer = 20; break;
        case 14: world.buttons_array.length = 20; break;
        case 15: world.buttons.value[F_LIST_ITEMS].ref = NULL; break;
        case 16: world.levels_menu.value[F_BUTTONS].ref = NULL; break;
        case 17: world.controller.value[F_SAVE].ref = NULL; break;
        case 18: static_shadow = NULL; break;
        case 19: world.shadow.value[F_SHOWN].flag = 1; break;
        case 20: world.shadow.value[F_SHADOW_HIDE].ref = &world.stranger; break;
        case 21: world.shadow.value[F_SHADOW_COMPLETE].ref = &world.stranger; break;
        case 22: world.items.value[F_BUTTON_OBJECTS].ref = NULL; break;
        case 23: world.item_objects.value[F_SIZE].integer = 5; break;
        case 24: world.item_objects_array.length = 5; break;
        case 25: world.item_objects.value[F_LIST_ITEMS].ref = NULL; break;
        case 26: world.item_objects_array.element[5] = NULL; break;
        }
        poll_unchanged_save(1); assert_stock();
    }
    fresh(); poll_unchanged_save(0); assert_stock(); assert(!resolved);
    for (int i = 0; i < 20; ++i) {
        fresh(); world.buttons_array.element[i] = NULL;
        poll_unchanged_save(1); assert_stock();
        fresh(); world.button[i].value[F_VISIBLE].flag = 0;
        poll_unchanged_save(1); assert_stock();
        fresh(); world.button[i].value[F_ACTIVE].flag = 1;
        poll_unchanged_save(1);
        assert(!writes && !enables && !delegates && !live_handles);
    }
    fresh(); world.button[0].value[F_CLICK].ref = NULL;
    poll_unchanged_save(1); assert(!writes && !delegates && !live_handles);
    fresh(); world.button[0].value[F_START].ref = &world.stranger;
    poll_unchanged_save(1); assert_stock(); assert(!delegates);
    /* A native menu replacement owns its new callback; never overwrite it. */
    fresh(); poll_unchanged_save(1);
    world.button[0].value[F_CLICK].ref = &world.stranger;
    poll_unchanged_save(1);
    assert(writes == 1 && !disables && !live_handles && freed == 3);
    assert(world.button[0].value[F_CLICK].ref == &world.stranger);
    /* Restore even if progress becomes ineligible or the active menu changes. */
    fresh(); poll_unchanged_save(1); world.data.levels[0].value[F_COMPLETED].flag = 1;
    poll_unchanged_save(1); assert(disables == 1 && !live_handles && freed == 3);
    fresh(); poll_unchanged_save(1); world.manager.value[F_MENU].ref = &world.items;
    poll_unchanged_save(1); assert(disables == 1 && !live_handles && freed == 3);
    fresh(); poll_unchanged_save(1); world.buttons_array.element[0] = &world.button[1];
    poll_unchanged_save(1); assert(disables == 1 && !live_handles && freed == 3);
    /* A synchronous transition callback blocks rearming before shadow fades in. */
    for (int transition = 0; transition < 3; ++transition) {
        fresh(); poll_unchanged_save(1);
        if (transition == 0) world.shadow.value[F_SHOWN].flag = 1;
        if (transition == 1) world.shadow.value[F_SHADOW_HIDE].ref = &world.stranger;
        if (transition == 2) world.shadow.value[F_SHADOW_COMPLETE].ref = &world.stranger;
        poll_unchanged_save(1);
        assert(disables == 1 && !live_handles && freed == 3);
        poll_unchanged_save(1); assert(enables == 1 && delegates == 1);
    }
    puts("PASS: workshop/entry/context/menu/list/stock-lock guards; shadow visibility and synchronous transition callbacks; callback ownership respected");
}
static void test_metadata_and_handle_failures(void) {
    for (int i = 0; i <= ACTION; ++i) {
        fresh(); missing_class = classes[i].name;
        poll_unchanged_save(1); assert_stock();
    }
    for (int i = 0; i < F_SIZE; ++i) {
        fresh(); missing_field = fields[i].name;
        poll_unchanged_save(1); assert_stock();
    }
    const char *symbols[] = {"il2cpp_method_get_object", "il2cpp_gchandle_new",
        "il2cpp_gchandle_get_target", "il2cpp_gchandle_free",
        "il2cpp_field_set_value_object"};
    for (unsigned i = 0; i < sizeof symbols / sizeof *symbols; ++i) {
        fresh(); missing_symbol = symbols[i];
        poll_unchanged_save(1); assert_stock();
    }
    for (int i = 0; i < 4; ++i) {
        fresh(); missing_method = methods[i].name;
        poll_unchanged_save(1); assert_stock();
    }
    fresh(); loaded = 0; poll_unchanged_save(1); assert_stock();
    loaded = 1; poll_unchanged_save(1); assert(enables == 1); poll_unchanged_save(0);
    fresh(); module_present = 0; poll_unchanged_save(1); assert_stock();
    fresh(); reflection_present = 0; poll_unchanged_save(1); assert_stock();
    fresh(); type_present = 0; poll_unchanged_save(1); assert_stock();
    fresh(); action_present = 0; poll_unchanged_save(1); assert_stock();
    fresh(); action_valid = 0; poll_unchanged_save(1); assert_stock();
    for (int i = 1; i <= 3; ++i) {
        fresh(); fail_new_at = i; poll_unchanged_save(1); assert_stock();
        assert(new_calls == 3 && freed == 2);
        fail_new_at = 0; poll_unchanged_save(1);
        assert(enables == 1 && live_handles == 3);
        poll_unchanged_save(0); assert(!live_handles && freed == 5);
    }
    puts("PASS: missing metadata/runtime/delegate gates; every partial GC-handle failure cleans up and retries safely");
}
int main(void) {
    test_progress_contract(); test_delegate_lifecycle(); test_menu_guards();
    test_metadata_and_handle_failures();
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="partyhard-tutorial-return-", dir=os.getenv("TMPDIR")) as temp:
    c_file, binary = Path(temp) / "test.c", Path(temp) / "test"
    c_file.write_text(fixture + source + checks)
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
