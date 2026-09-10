#ifndef SB_TITLE_MENU_GUARD_H
#define SB_TITLE_MENU_GUARD_H

#define SB_TITLE_STATE_MAIN_MENU 1
#define SB_TITLE_MENU_NEW_GAME 0
#define SB_TITLE_MENU_EXIT 3
#define SB_TITLE_MENU_DISCORD 4

int sb_title_menu_visible_selection(int selected);
int sb_title_menu_repair_target(int state, int menu_flags, int selected,
                                int have_last_visible, int last_visible,
                                int *replacement);

#endif /* SB_TITLE_MENU_GUARD_H */
