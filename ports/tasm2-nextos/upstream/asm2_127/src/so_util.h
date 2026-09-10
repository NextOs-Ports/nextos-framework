/*
 * so_util.h -- utils to load and hook .so modules
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
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

extern void *text_base, *data_base;
extern size_t text_size, data_size;

void hook_arm(uintptr_t addr, uintptr_t dst);

void so_make_text_writable(void);
void so_make_text_executable(void);
void so_flush_caches(void);
void so_free_temp(void);
int so_load(const char *filename, void *base, size_t max_size);
int so_relocate(void);
void so_set_missing_import_target(uintptr_t target);
int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
DynLibFunction *so_snapshot_symbols(int *out_count);
void so_execute_init_array(void);
size_t so_init_array_count(void);
size_t so_execute_init_array_limit(size_t limit);
uintptr_t so_find_exidx(uintptr_t pc, int *count);
int so_guest_phdr_view(const void **phdr, uint16_t *phnum,
                       uintptr_t *load_bias, uintptr_t *mapping,
                       size_t *mapping_size);
uintptr_t so_find_addr(const char *symbol);
uintptr_t so_find_addr_safe(const char *symbol);
uintptr_t so_find_addr_rx(const char *symbol);
uintptr_t so_find_rel_addr(const char *symbol);
uintptr_t so_find_rel_addr_safe(const char *symbol);
void *so_guest_vma(uintptr_t vma, size_t size);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name);
void so_finalize(void);
int so_unload(void);

#endif
