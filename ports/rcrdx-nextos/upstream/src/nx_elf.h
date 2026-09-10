/*
 * nx_elf -- multi-module Android ELF loader for the Tightrope Theatre port.
 *
 * The kit's so_util keeps a single module in global state.  Tightrope needs
 * three arm64 objects resident at once (libApplicationMain, liblime and, for
 * the one-shot recovery run only, libpairipcore) and they import from each
 * other, so this loader keeps a list of modules and resolves across it.
 *
 * Segments are mapped straight from the file (MAP_PRIVATE) instead of being
 * copied into an anonymous block: libApplicationMain alone is 34 MB and the
 * device has 916 MB, so keeping .text file-backed and clean matters.
 */

#ifndef NX_ELF_H
#define NX_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

typedef struct {
    const char *name;
    void *addr;
} nx_import;

typedef struct nx_mod {
    char name[64];              /* soname as the game refers to it */
    char path[512];
    uint8_t *base;
    size_t span;                /* bytes reserved from base */
    Elf64_Dyn *dyn;
    Elf64_Sym *dynsym;
    const char *dynstr;
    size_t dynstr_sz;
    size_t nsym;                /* from DT_HASH nchain when present */
    Elf64_Rela *rela;
    size_t rela_n;
    Elf64_Rela *jmprel;
    size_t jmprel_n;
    uintptr_t *pltgot;
    size_t pltgot_n;            /* derived from the .got.plt span when known */
    void (**init_array)(void);
    size_t init_n;
    void (*init_func)(void);
    Elf64_Phdr *phdr;
    size_t phnum;
    const uint32_t *hash;       /* DT_HASH, if present */
    const uint32_t *gnu_hash;   /* DT_GNU_HASH, when an object only ships it */
    int relocated;
    int inited;
    struct nx_mod *next;
} nx_mod;

/* Resolution order for an undefined symbol: the host import table first (so
 * our bionic shims win over anything the game ships), then module exports. */
void nx_set_imports(const nx_import *tab, size_t n);

/* Last-resort resolver, consulted after the host table and every module.  The
 * GL entry points come from here: naming all 180 of them in a static table
 * would duplicate the driver's own export list. */
void nx_set_fallback(void *(*fn)(const char *sym));

nx_mod *nx_load(const char *path, const char *soname);
int nx_relocate(nx_mod *m);
/* Restore each segment's real ELF protections.  Relocation needs the whole
 * image writable, but leaving it that way puts rwxp segments in
 * /proc/self/maps, which is the classic signature of an injected process --
 * and PairIP reads that file before it decides to decrypt anything. */
int nx_protect(nx_mod *m);
void nx_run_init(nx_mod *m);
nx_mod *nx_find_mod(const char *soname);
void *nx_lookup(const char *sym);              /* across all modules */
void *nx_lookup_in(nx_mod *m, const char *sym);
void *nx_resolve_import(const char *sym);      /* host table only */

/* Count of relocations that stayed unresolved on the last nx_relocate(). */
extern int nx_unresolved;

/* Verbose tracing, gated by RCR_VERBOSE at startup. */
extern int nx_verbose;
void nx_log(const char *fmt, ...);
void nx_die(const char *fmt, ...) __attribute__((noreturn));

/* Patch a single .got.plt slot of a module (used to fill the PLT entries that
 * PairIP hides from the dynamic tables). */
int nx_patch_pltgot(nx_mod *m, unsigned slot, void *fn);
uintptr_t nx_read_pltgot(nx_mod *m, unsigned slot);

/* Build a stub that tail-calls handler(id) with the original argument
 * registers untouched; used to identify PLT slots we could not name. */
void *nx_make_probe(unsigned id, void *handler);
extern unsigned nx_probe_slot;   /* slot number of the probe that fired last */

#endif /* NX_ELF_H */
