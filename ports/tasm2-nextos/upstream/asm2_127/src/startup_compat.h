#ifndef ASM2_STARTUP_COMPAT_H
#define ASM2_STARTUP_COMPAT_H

/* Applies version-locked startup fixes for the 1.2.7d guest binary. */
int asm2_startup_compat_apply(void);

/* Advances the temporary first-check deferral after each guest frame. */
void asm2_startup_compat_tick(void);

#endif
