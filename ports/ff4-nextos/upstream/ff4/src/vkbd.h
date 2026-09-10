// vkbd — teclado virtual on-screen p/ a tela de nomear (o jogo usa o teclado do
// Android, que nao temos). Overlay GLES1: d-pad navega, A/R3 confirma.
#ifndef VKBD_H
#define VKBD_H

void vkbd_init(const char *ttf_path);        // bake do atlas (com contexto GL ativo)
void vkbd_activate(const char *def, int maxlen);
void vkbd_deactivate(void);
int  vkbd_is_active(void);
// input por EDGE (1 = acabou de apertar). Retorna 1 quando o usuario confirma OK.
int  vkbd_handle(int left, int right, int up, int down, int confirm, int cancel);
void vkbd_render(int win_w, int win_h);      // desenha o teclado por cima
const char *vkbd_text(void);                 // texto atual (getEditText devolve isto)

#endif
