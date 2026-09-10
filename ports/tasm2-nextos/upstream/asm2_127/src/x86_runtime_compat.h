#ifndef ASM2_X86_RUNTIME_COMPAT_H
#define ASM2_X86_RUNTIME_COMPAT_H

#include <link.h>
#include <stddef.h>
#include <stdint.h>

int asm2_x86_cxa_atexit(void (*function)(void *), void *argument, void *dso);
int asm2_x86_dl_iterate_phdr(
    int (*callback)(struct dl_phdr_info *, size_t, void *), void *data);
uintptr_t asm2_x86_gl_lazy_stub(const char *name);
void *asm2_x86_gl_resolve_now(const char *name);
size_t asm2_x86_recorded_destructor_count(void);

#endif
