/*
 * so_util.h -- loader ELF64 aarch64 mínimo p/ o FF4 (módulo único, STL estática).
 * Escrito do zero: mecânica de relocação universal (não é bagagem de outro jogo),
 * mas sem o multi-módulo/aux-libc++ do ff7 (o libff4a não NEEDA libc++_shared).
 */
#ifndef FF4A_SO_UTIL_H
#define FF4A_SO_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct {
  const char *symbol;
  uintptr_t func;
} DynLibFunction;

/* base absoluta do módulo carregado (ancorado em vaddr 0). */
extern uintptr_t so_text_base;
extern size_t so_load_size;

/* carrega o ELF do arquivo p/ 'base' (mmap RWX do chamador, max_size bytes). */
int so_load(const char *filename, void *base, size_t max_size);
/* aplica relocations internas (ABS64 local, RELATIVE, RELR). */
int so_relocate(void);
/* resolve imports UNDEF: 1º a tabela funcs, senão dlsym(RTLD_DEFAULT). */
int so_resolve(DynLibFunction *funcs, int num_funcs);
/* roda .init_array. */
void so_execute_init_array(void);
/* endereço absoluto de um símbolo exportado (fatal se não achar). */
uintptr_t so_find_addr(const char *symbol);
/* idem, mas devolve 0 se não achar (não aborta). */
uintptr_t so_find_addr_safe(const char *symbol);
/* ajusta permissões finais (RX no text, RW no data) via phdr flags. */
void so_finalize(void);
/* registra os phdr p/ o dl_iterate_phdr custom (unwind de exceções C++). */
void so_record_phdr(const char *name);
/* libera o buffer temporário do arquivo. */
void so_free_temp(void);

#endif
