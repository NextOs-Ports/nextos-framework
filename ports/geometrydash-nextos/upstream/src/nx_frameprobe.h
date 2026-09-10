/* nx_frameprobe — sonda de frame dentro do processo (ver nx_frameprobe.c).
 *
 * Chamar imediatamente ANTES do swap:
 *     nx_frameprobe_before_swap();
 *     eglSwapBuffers(dpy, surf);
 *
 * Desligada por padrão. Liga com NX_FRAMEPROBE=1 (e NX_FRAMEPROBE_EVERY=<n>).
 * Com ela ligada o fps cai (glReadPixels no Mali é stall) — não meça
 * performance com a sonda ativa.
 */
#ifndef NX_FRAMEPROBE_H
#define NX_FRAMEPROBE_H

void nx_frameprobe_before_swap(void);

/* A mesma chamada tambem grava uma captura de tela quando NX_FRAMESHOT aponta
 * para um arquivo (PPM cru, uma vez so'; NX_FRAMESHOT_AFTER=<n> espera n
 * frames). Desligada por padrao. */

#endif /* NX_FRAMEPROBE_H */
