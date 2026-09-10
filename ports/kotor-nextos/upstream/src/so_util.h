/*
 * so_util.h -- multi-module ARMHF .so loader (matches template-arm/so_util.c).
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen.
 */
#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

/* globals reflect the CURRENTLY-loaded module (last so_load). */
extern void *text_base, *text_virtbase;
extern size_t text_size;
extern void *data_base, *data_virtbase;
extern size_t data_size;

void hook_arm(uintptr_t addr, uintptr_t dst);

void so_make_text_writable(void);
void so_make_text_executable(void);
void so_flush_caches(void);
void so_free_temp(void);
int so_load(const char *filename, void *base, size_t max_size);
int so_relocate(void);
int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
DynLibFunction *so_snapshot_symbols(int *out_count);
void so_execute_init_array(void);
uintptr_t so_find_addr(const char *symbol);
uintptr_t so_find_addr_safe(const char *symbol);
uintptr_t so_find_addr_rx(const char *symbol);
uintptr_t so_find_rel_addr(const char *symbol);
uintptr_t so_find_rel_addr_safe(const char *symbol);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name);
void so_finalize(void);
int so_unload(void);

/* exidx of the CURRENTLY-loaded module (for the C++ unwinder registry). */
int so_current_exidx(uintptr_t *out_text_lo, uintptr_t *out_text_hi,
                     uintptr_t *out_exidx, int *out_count);

#endif
