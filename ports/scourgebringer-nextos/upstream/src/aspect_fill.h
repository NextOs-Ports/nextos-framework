#ifndef SB_ASPECT_FILL_H
#define SB_ASPECT_FILL_H

/* Expande o quadro 16:9 ja composto para painéis mais estreitos, somente
 * quando SB_ASPECT_FILL esta habilitado. Em 16:9 nao altera o backbuffer. */
void sb_present_aspect_fill(int backbuffer_width, int backbuffer_height);

#endif
