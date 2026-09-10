/* Incomplete tutorial -> Quit -> Board has no open level in the Android
 * game. Offer re-entry on the first poster through the EXISTING native
 * BoardItemsMenu.OnClickStart action. This is not a level unlock: never
 * write the save, level dictionary, selected level or completion flags.
 *
 * Authenticated flow: OnClickStart (non-workshop) -> ShowScreenSpecial
 * (GameScreen) -> GameScreen.OnShow -> AppLoaderMainMenu.StartGame, which
 * obtains the tutorial from the unchanged SaveGameData.GetLevel(). Native
 * gestures/A activate the delegate; the port never fabricates a click.
 * All bindings are metadata-based. No AOT patch, raw RVA, or new engine
 * entry sequence. The temporary button binding is restored on menu exit.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "il2.h"
#include "nx_elf.h"
#include "tutorial_return.h"

static int resolved;
static FieldInfo *board_f, *levels_f, *items_f, *workshop_f, *workshop_item_f;
static FieldInfo *shadow_f, *shadow_hide_f, *shadow_complete_f;
static FieldInfo *manager_f, *menu_f, *shown_f, *buttons_f, *button_objects_f;
static FieldInfo *save_controller_f, *save_f, *current_f, *played_f, *pool_f;
static FieldInfo *completed_list_f, *number_f, *level_f, *completed_f;
static FieldInfo *active_f, *visible_f, *click_f, *start_f;
static FieldInfo *entry_f, *entry_level_f;
static const MethodInfo *activate_m, *select_first_m, *start_m;
static Il2CppClass *items_class, *action_class;
static void *(*method_object)(const MethodInfo *, Il2CppClass *);
/* Unity 6 GC handles are pointer-sized, not legacy 32-bit indices. */
static uintptr_t (*handle_new)(void *, int);
static void *(*handle_target)(uintptr_t);
static void (*handle_free)(uintptr_t);
static void (*set_reference)(void *, FieldInfo *, void *);
static uintptr_t button_handle, original_handle, action_handle;

static void *ref(void *obj, FieldInfo *field)
{
    void *value = NULL;
    if (obj) il2_field_get(obj, field, &value);
    return value;
}

static int32_t integer(void *obj, FieldInfo *field)
{
    int32_t value = -1;
    if (obj) il2_field_get(obj, field, &value);
    return value;
}

static int flag(void *obj, FieldInfo *field)
{
    uint8_t value = 0;
    if (obj) il2_field_get(obj, field, &value);
    return value != 0;
}

/* Lists are checked against the actual backing array, not raw offsets. */
static int list_array(void *list, void **array)
{
    *array = NULL;
    if (!list) return -1;
    Il2CppClass *klass = il2_class_of(list);
    FieldInfo *size = il2_field(klass, "_size");
    FieldInfo *items = il2_field(klass, "_items");
    if (!size || !items) return -1;
    int count = integer(list, size);
    *array = ref(list, items);
    if (count < 0 || count > 64 || !*array ||
        (uint32_t)count > il2_arr_len(*array)) return -1;
    return count;
}

static int resolve(void)
{
    if (resolved) return resolved > 0;
    if (!il2_load()) return 0;
    resolved = -1;
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    if (!mod) return 0;
    *(void **)&method_object = nx_lookup_in(mod, "il2cpp_method_get_object");
    *(void **)&handle_new = nx_lookup_in(mod, "il2cpp_gchandle_new");
    *(void **)&handle_target = nx_lookup_in(mod, "il2cpp_gchandle_get_target");
    *(void **)&handle_free = nx_lookup_in(mod, "il2cpp_gchandle_free");
    *(void **)&set_reference = nx_lookup_in(mod, "il2cpp_field_set_value_object");
    if (!method_object || !handle_new || !handle_target || !handle_free ||
        !set_reference)
        return 0;
    Il2CppClass *gui = il2_class("App.View", "Gui");
    Il2CppClass *board = il2_class("App.View.Interface", "BoardSceeen");
    Il2CppClass *workshop = il2_class("Assets.Scripts.View.Menu", "BoardWorkshopMenu");
    Il2CppClass *manager = il2_class("Assets.Scripts.Controllers.Game", "MenuInputManager");
    Il2CppClass *menu = il2_class("App.View.Interface", "MenuContext");
    Il2CppClass *visual = il2_class("App.View", "VisualComponent");
    Il2CppClass *shadow = il2_class("App.View.Interface", "GameShadow");
    Il2CppClass *controller = il2_class("Assets.Scripts.Controllers.Game.SaveGame", "SaveGameController");
    Il2CppClass *save = il2_class("Assets.Scripts.Controllers.Game.SaveGame", "SaveGameData");
    Il2CppClass *level = il2_class("Assets.Scripts.Controllers.Game.SaveGame", "SaveGameLevelData");
    Il2CppClass *button = il2_class("App.Core.ButtonsManager", "GestureButton");
    Il2CppClass *loader = il2_class("", "AppLoaderMainMenu");
    Il2CppClass *entry = il2_class("", "EnterPointMainMenu");
    items_class = il2_class("Assets.Scripts.View.Menu", "BoardItemsMenu");
    action_class = il2_class("System", "Action");
    if (!gui || !board || !workshop || !manager || !menu || !visual || !shadow ||
        !controller || !save || !level || !button || !loader || !entry ||
        !items_class || !action_class) return 0;
#define BIND(slot, klass, name) do { slot = il2_field(klass, name); if (!slot) return 0; } while (0)
    BIND(board_f, gui, "BoardScrn");
    BIND(shadow_f, gui, "Shadow");
    BIND(shadow_hide_f, shadow, "_onHide");
    BIND(shadow_complete_f, shadow, "_onComplete");
    BIND(levels_f, board, "boardLevelsMenu");
    BIND(items_f, board, "boardItemsMenu");
    BIND(workshop_f, board, "boardWorkshopMenu");
    BIND(workshop_item_f, workshop, "currentItem");
    BIND(manager_f, manager, "instance");
    BIND(menu_f, manager, "menu");
    BIND(shown_f, visual, "_isShow");
    BIND(buttons_f, menu, "_menuButtons");
    BIND(button_objects_f, menu, "_menuButtonsGO");
    BIND(save_controller_f, controller, "instance");
    BIND(save_f, controller, "saveGame");
    BIND(current_f, save, "currentLevelNum");
    BIND(played_f, save, "playedLevelNum");
    BIND(pool_f, save, "levelPool");
    BIND(completed_list_f, save, "completedLevels");
    BIND(number_f, level, "levelNumber");
    BIND(level_f, level, "level");
    BIND(completed_f, level, "completed");
    BIND(active_f, button, "Active");
    BIND(visible_f, button, "visible");
    BIND(click_f, button, "_onClick");
    BIND(start_f, button, "_onStart");
    BIND(entry_f, loader, "enterPoint");
    BIND(entry_level_f, entry, "Level");
#undef BIND
    activate_m = il2_method(button, "SetActivate", 1);
    select_first_m = il2_method(menu, "SelectFirstActive", 0);
    start_m = il2_method(items_class, "OnClickStart", 0);
    if (!activate_m || !select_first_m || !start_m) return 0;
    resolved = 1;
    return 1;
}

static void restore(void)
{
    if (!button_handle) return;
    void *button = handle_target(button_handle);
    void *action = handle_target(action_handle);
    if (button && action && ref(button, click_f) == action) {
        set_reference(button, click_f, handle_target(original_handle));
        uint8_t off = 0;
        void *args[] = { &off };
        il2_call(activate_m, button, args, "tutorial-return: restore button");
    }
    handle_free(button_handle);
    handle_free(original_handle);
    handle_free(action_handle);
    button_handle = original_handle = action_handle = 0;
}

/* Strict initial-tutorial contract. Read only; inconsistent/advanced saves
 * keep the stock menu, including all its native level locks. */
static int incomplete_tutorial(void *save)
{
    if (!save || integer(save, current_f) != 0 || integer(save, played_f) != 0)
        return 0;
    void *array = NULL;
    if (list_array(ref(save, completed_list_f), &array) != 0) return 0;
    if (list_array(ref(save, pool_f), &array) != 21) return 0;
    for (int i = 0; i < 21; ++i) {
        void *level = il2_arr_at(array, (uint32_t)i);
        if (!level || integer(level, number_f) != i || flag(level, completed_f))
            return 0;
        /* Levels.Tutorial = 80 in the authenticated game contract. */
        if (i == 0 && integer(level, level_f) != 80) return 0;
    }
    return 1;
}

void st_tutorial_return_poll(int board_context)
{
    if (!board_context) { restore(); return; }
    if (!resolve()) return;
    void *board = NULL, *manager = NULL, *controller = NULL, *entry = NULL;
    void *shadow = NULL;
    il2_static_get(board_f, &board);
    il2_static_get(manager_f, &manager);
    il2_static_get(save_controller_f, &controller);
    il2_static_get(entry_f, &entry);
    il2_static_get(shadow_f, &shadow);
    void *levels = ref(board, levels_f), *items = ref(board, items_f);
    void *workshop = ref(board, workshop_f);
    int entry_level = integer(entry, entry_level_f);
    if (!board || !levels || !items || !workshop || !entry || !shadow ||
        flag(shadow, shown_f) || ref(shadow, shadow_hide_f) ||
        ref(shadow, shadow_complete_f) ||
        ref(manager, menu_f) != levels || !flag(board, shown_f) ||
        !flag(levels, shown_f) || ref(workshop, workshop_item_f) ||
        (entry_level != 0 && entry_level != 80) ||
        !incomplete_tutorial(ref(controller, save_f))) {
        restore(); return;
    }
    void *buttons = NULL;
    void *item_objects = NULL;
    if (list_array(ref(items, button_objects_f), &item_objects) < 6 ||
        !il2_arr_at(item_objects, 5)) { restore(); return; }
    if (list_array(ref(levels, buttons_f), &buttons) != 21) {
        restore(); return;
    }
    void *first = il2_arr_at(buttons, 0);
    /* Last button is the stock Back, not a phase. Every phase except our
     * owned resume button must still be disabled before this can apply. */
    for (uint32_t i = 0; i < 20; ++i) {
        void *button = il2_arr_at(buttons, i);
        int owned = i == 0 && button_handle &&
                    button == handle_target(button_handle);
        if (!button || !flag(button, visible_f) ||
            (flag(button, active_f) && !owned)) { restore(); return; }
    }
    if (button_handle) {
        if (first != handle_target(button_handle) ||
            ref(first, click_f) != handle_target(action_handle)) restore();
        return;
    }
    void *original = ref(first, click_f);
    if (!original || ref(first, start_f)) return;
    void *reflection = method_object(start_m, items_class);
    const MethodInfo *create = reflection ? il2_method(
        il2_class_of(reflection), "CreateDelegate", 2) : NULL;
    void *type = il2_type(action_class);
    if (!create || !type) return;
    void *args[] = { type, items };
    void *action = il2_call(create, reflection, args,
                            "tutorial-return: native resume delegate");
    if (!action || il2_class_of(action) != action_class) return;
    button_handle = handle_new(first, 0);
    original_handle = handle_new(original, 0);
    action_handle = handle_new(action, 0);
    if (!button_handle || !original_handle || !action_handle) {
        if (button_handle) handle_free(button_handle);
        if (original_handle) handle_free(original_handle);
        if (action_handle) handle_free(action_handle);
        button_handle = original_handle = action_handle = 0;
        return;
    }
    set_reference(first, click_f, action);
    uint8_t on = 1;
    void *enable[] = { &on };
    il2_call(activate_m, first, enable, "tutorial-return: enable resume");
    il2_call(select_first_m, levels, NULL, "tutorial-return: select resume");
    fprintf(stderr, "[st/input] tutorial return: first poster resumes existing tutorial; save and phase locks unchanged\n");
}
