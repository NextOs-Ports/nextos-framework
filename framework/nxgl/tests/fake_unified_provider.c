/* SPDX-License-Identifier: GPL-3.0-only */
/* Blob unificado falso e VIVO: exporta EGL e GLES1 no MESMO objeto (o que o
 * adapter exige por dlsym) e responde a prova viva como o blob certo da
 * firmware responde -- eglGetDisplay devolve um display e eglInitialize
 * inicializa. As assinaturas reais importam porque o adapter CHAMA essas
 * funcoes na prova pre-contexto. */
void *eglGetDisplay(void *display_id) {
  (void)display_id;
  return (void *)0x1;
}
unsigned eglInitialize(void *display, int *major, int *minor) {
  (void)display;
  if (major) {
    *major = 1;
  }
  if (minor) {
    *minor = 4;
  }
  return 1u;
}
unsigned eglTerminate(void *display) {
  (void)display;
  return 1u;
}
void glOrthof(void) {}
void glDrawArrays(void) {}
void glClear(void) {}
