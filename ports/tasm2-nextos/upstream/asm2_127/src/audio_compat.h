#ifndef ASM2_AUDIO_COMPAT_H
#define ASM2_AUDIO_COMPAT_H

/* Applies version-locked audio teardown fixes for the 1.2.7d guest binary. */
int asm2_audio_compat_apply(void);

/* Returns the guest pthread_mutex_t slot used by the exact 1.2.7d OpenSL
 * completion callback, or NULL for any other callback/context pair. */
void *asm2_audio_compat_callback_mutex(void *callback, void *context);

#endif
