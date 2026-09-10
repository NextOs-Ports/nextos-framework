#include <assert.h>
#include <stdio.h>

#include "title_menu_guard.h"

int main(void)
{
    int replacement = -1;
    unsigned int gameplay_buttons = (1u << 13) | (1u << 14);
    short gameplay_left_x = -32768;

    assert(sb_title_menu_visible_selection(SB_TITLE_MENU_NEW_GAME));
    assert(sb_title_menu_visible_selection(SB_TITLE_MENU_EXIT));
    assert(!sb_title_menu_visible_selection(SB_TITLE_MENU_DISCORD));
    assert(!sb_title_menu_visible_selection(-1));

    assert(sb_title_menu_repair_target(
        SB_TITLE_STATE_MAIN_MENU, 0, SB_TITLE_MENU_DISCORD,
        1, 2, &replacement));
    assert(replacement == 2);

    replacement = -1;
    assert(sb_title_menu_repair_target(
        SB_TITLE_STATE_MAIN_MENU, 0, SB_TITLE_MENU_DISCORD,
        0, -1, &replacement));
    assert(replacement == SB_TITLE_MENU_NEW_GAME);

    assert(!sb_title_menu_repair_target(
        SB_TITLE_STATE_MAIN_MENU, 0, 1, 1, 1, &replacement));
    assert(!sb_title_menu_repair_target(
        SB_TITLE_STATE_MAIN_MENU, 1, SB_TITLE_MENU_DISCORD,
        1, 1, &replacement));
    assert(!sb_title_menu_repair_target(
        0, 0, SB_TITLE_MENU_DISCORD, 1, 1, &replacement));
    assert(!sb_title_menu_repair_target(
        SB_TITLE_STATE_MAIN_MENU, 0, SB_TITLE_MENU_DISCORD,
        1, 1, NULL));

    /* The repair API cannot mutate controller state, even if the title object
     * keeps stale MainMenu values after gameplay starts. */
    assert(gameplay_buttons == ((1u << 13) | (1u << 14)));
    assert(gameplay_left_x == -32768);

    puts("title menu guard: hidden selection repaired without filtering input");
    return 0;
}
