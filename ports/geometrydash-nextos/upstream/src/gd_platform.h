#ifndef GD_PLATFORM_H
#define GD_PLATFORM_H

/* Instancia unica travada no PROPRIO BINARIO (ver gd_platform.c). SIGTERM ja'
 * entra pelo caminho de pausa/save do main.c, entao nao mora aqui. */
int gd_single_instance_lock(void);   /* 0 = adquirida, -1 = ja ha outra */
void gd_single_instance_unlock(void);

#endif /* GD_PLATFORM_H */
