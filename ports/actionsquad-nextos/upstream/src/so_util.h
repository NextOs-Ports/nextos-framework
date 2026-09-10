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

void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_make_text_writable(void);
void so_make_text_executable(void);
void so_flush_caches(void);
void so_free_temp(void);
int so_load(const char *filename, void *base, size_t max_size);
int so_relocate(void);
int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
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

// Harvest de todos os símbolos DEFINIDOS (GLOBAL/WEAK) do módulo atualmente
// carregado, num array {nome, addr}. Usado para expor um módulo (ex.
// libc++_shared.so) como provedor de símbolos para outro carregado depois.
// O array é malloc'd; os nomes apontam para o .dynstr do módulo (que permanece
// mapeado). Devolve o array e escreve a contagem em *out_count.
DynLibFunction *so_snapshot_symbols(int *out_count);

#endif
