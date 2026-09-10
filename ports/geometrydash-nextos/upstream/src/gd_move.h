#ifndef GD_MOVE_H
#define GD_MOVE_H

/* Andar para os lados nos niveis de plataforma (2.2): ver gd_move.c. */
void gd_move_install(void);
int gd_move_live(void);
void gd_move_set(int left, int right);
void gd_move_jump(int down);

#endif /* GD_MOVE_H */
