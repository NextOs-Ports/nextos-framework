#ifndef ASM2_SHOP_COMPAT_H
#define ASM2_SHOP_COMPAT_H

#include <stdint.h>

/* Installs the original 1.2.7d Google catalog as an offline billing reply. */
int asm2_shop_compat_initialize(const char *game_directory,
                                uintptr_t billing_native_send_data,
                                void *billing_class);

/* Delivers the original local CRM catalog once its native manager exists. */
void asm2_shop_compat_tick(void);

#endif /* ASM2_SHOP_COMPAT_H */
