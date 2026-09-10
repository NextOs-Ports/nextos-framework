// so_util — carregador ELF64 aarch64 para .so do Android em Linux/glibc.
//
// Por que um loader proprio em vez de dlopen(): o .so do Android referencia
// libc.so/liblog.so (nomes bionic) e simbolos bionic (__errno, __memcpy_chk...),
// e precisamos INTERPOR nossas implementacoes de GL/OpenSLES/JNI. O dlopen do
// glibc nao permite nada disso.
#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *name;
    uintptr_t addr;
} so_import_t;

typedef struct {
    void *base;         // base de carga (mmap)
    size_t size;        // bytes mapeados
    uint8_t *dynstr;
    void *dynsym;       // Elf64_Sym*
    size_t num_dynsym;
    void *rela;         // Elf64_Rela* (.rela.dyn)
    size_t rela_count;
    void *plt_rela;     // Elf64_Rela* (.rela.plt)
    size_t plt_rela_count;
    void (**init_array)(void);
    size_t init_array_count;
} so_module_t;

// Carrega o .so, aplica relocacoes e resolve imports pela tabela fornecida.
// Retorna 0 em sucesso. Simbolo ausente na tabela vira stub que loga e aborta.
int so_load(so_module_t *mod, const char *path, const so_import_t *imports, size_t num_imports);

// Roda os construtores (.init_array). Separado de so_load porque alguns
// construtores ja chamam de volta pro nosso shim, que precisa estar pronto.
void so_run_init(so_module_t *mod);

// Resolve um simbolo exportado pelo modulo.
uintptr_t so_symbol(so_module_t *mod, const char *name);

#endif
