#include "title_menu_guard.h"

int sb_title_menu_visible_selection(int selected)
{
    return selected >= SB_TITLE_MENU_NEW_GAME &&
        selected <= SB_TITLE_MENU_EXIT;
}

int sb_title_menu_repair_target(int state, int menu_flags, int selected,
                                int have_last_visible, int last_visible,
                                int *replacement)
{
    if (!replacement || state != SB_TITLE_STATE_MAIN_MENU || menu_flags != 0 ||
        selected != SB_TITLE_MENU_DISCORD)
        return 0;
    *replacement = have_last_visible &&
        sb_title_menu_visible_selection(last_visible)
        ? last_visible : SB_TITLE_MENU_NEW_GAME;
    return 1;
}
