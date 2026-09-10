/*
 * so_util.c -- utils to load and hook .so modules  (ELF32 / ARM EABI variant)
 *
 * Portado do so_util aarch64 (Dysmantle, verde) para ELF32-ARM seguindo a ARM
 * ELF ABI: classe ELFCLASS32, e_machine EM_ARM, relocs REL (.rel.dyn/.rel.plt)
 * com ADDEND IMPLÍCITO (o valor já presente em *ptr), tipos R_ARM_*, entradas
 * de init_array de 4 bytes, e preservação do bit Thumb (st_value bit0) nos
 * endereços de função.  Estrutura/contrato idênticos ao original aarch64.
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 */

#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "error.h"
#include "so_util.h"
#include "util.h"

#ifndef EM_ARM
#define EM_ARM 40
#endif

/* tipos de reloc ARM (caso <elf.h> não defina todos) */
#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE 23
#endif

void *text_base, *text_virtbase;
size_t text_size;

void *data_base, *data_virtbase;
size_t data_size;

static void *load_base, *load_virtbase;
static size_t load_size;

static void *so_base;

static Elf32_Ehdr *elf_hdr;
static Elf32_Phdr *prog_hdr;
static Elf32_Shdr *sec_hdr;
static Elf32_Sym *syms;
static int num_syms;

static char *shstrtab;
static char *dynstrtab;

/* Pool de trampolins na CAUDA do heap do módulo (RWX). Cada hook escreve 4
 * bytes no entry (um B p/ o slot). Trampolim ARM: LDR PC,[PC,#-4] ; .word dst.
 * (LDR em PC faz interworking automático no ARMv7 — bit Thumb preservado.) */
static uint8_t *g_tramp_pool = NULL;
static int g_tramp_used = 0;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  /* nome mantido por compat com a API; aqui é ARM32. */
  if (addr == 0)
    return;
  if (g_tramp_pool == NULL) {
    g_tramp_pool = (uint8_t *)(((uintptr_t)data_base + data_size + 15) & ~(uintptr_t)15);
  }
  uint32_t *slot = (uint32_t *)(g_tramp_pool + (size_t)g_tramp_used * 16);
  g_tramp_used++;
  slot[0] = 0xe51ff004u;          /* LDR PC, [PC, #-4]  */
  slot[1] = (uint32_t)dst;        /* alvo (32-bit, bit Thumb preservado) */
  __builtin___clear_cache((char *)slot, (char *)slot + 8);

  /* entry (assumido ARM): B slot  (alcance ±32MB) */
  uint32_t *hook = (uint32_t *)(addr & ~1u);
  intptr_t off = (intptr_t)((uintptr_t)slot - (uintptr_t)hook) - 8; /* PC=insn+8 */
  hook[0] = 0xea000000u | (((uint32_t)(off >> 2)) & 0x00FFFFFFu);
  __builtin___clear_cache((char *)hook, (char *)hook + 4);
}

void so_make_text_writable(void) {
  const size_t text_asize = ALIGN_MEM(text_size, 0x1000);
  if (mprotect(text_virtbase, text_asize,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    debugPrintf("Warning: Could not make text segment writable\n");
  }
}

void so_make_text_executable(void) {
  const size_t text_asize = ALIGN_MEM(text_size, 0x1000);
  if (mprotect(text_virtbase, text_asize, PROT_READ | PROT_EXEC) != 0) {
    debugPrintf("Warning: Could not restore text segment permissions\n");
  }
}

void so_flush_caches(void) {
  __builtin___clear_cache((char *)load_virtbase,
                          (char *)load_virtbase + load_size);
}

void so_free_temp(void) {
  free(so_base);
  so_base = NULL;
}

static inline size_t round_up(size_t x, size_t a) {
  return (x + a - 1) & ~(a - 1);
}

static int protect_range(void *start, size_t len, int prot) {
  int ps = getpagesize();
  if (ps <= 0)
    ps = 4096;
  uintptr_t addr = (uintptr_t)start;
  uintptr_t page_base = addr & ~((uintptr_t)ps - 1);
  size_t head = addr - page_base;
  size_t plen = round_up(len + head, (size_t)ps);
  if (mprotect((void *)page_base, plen, prot) != 0) {
    debugPrintf("mprotect(%p, %zu, 0x%x) failed: %s\n", (void *)page_base,
                plen, prot, strerror(errno));
    return -1;
  }
  return 0;
}

void so_finalize(void) {
  /* Só endurece o segmento executável (RX); resto fica RWX (heap mmap RWX). */
  if (protect_range(text_virtbase, text_size, PROT_READ | PROT_EXEC) != 0) {
    debugPrintf("Warning: could not set RX on text at %p (size %zu)\n",
                text_virtbase, text_size);
  }
}

int so_load(const char *filename, void *base, size_t max_size) {
  int res = 0;
  size_t so_size = 0;

  debugPrintf("so_load: Opening %s\n", filename);
  FILE *fd = fopen(filename, "rb");
  if (fd == NULL) {
    debugPrintf("so_load: Failed to open file\n");
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);
  debugPrintf("so_load: File size: %zu bytes\n", so_size);

  so_base = malloc(so_size);
  if (!so_base) {
    fclose(fd);
    return -2;
  }

  if (fread(so_base, so_size, 1, fd) != 1) {
    fclose(fd);
    free(so_base);
    return -3;
  }
  fclose(fd);

  if (memcmp(so_base, ELFMAG, SELFMAG) != 0) {
    debugPrintf("so_load: Not a valid ELF file\n");
    res = -1;
    goto err_free_so;
  }

  elf_hdr = (Elf32_Ehdr *)so_base;

  if (elf_hdr->e_ident[EI_CLASS] != ELFCLASS32) {
    debugPrintf("so_load: Not a 32-bit ELF file\n");
    res = -1;
    goto err_free_so;
  }

  if (elf_hdr->e_machine != EM_ARM) {
    debugPrintf("so_load: Not an ARM ELF (machine=%d)\n", elf_hdr->e_machine);
    res = -1;
    goto err_free_so;
  }

  debugPrintf("so_load: ELF32 ARM, %d program headers, %d section headers\n",
              elf_hdr->e_phnum, elf_hdr->e_shnum);

  if (elf_hdr->e_phoff + (elf_hdr->e_phnum * sizeof(Elf32_Phdr)) > so_size) {
    debugPrintf("so_load: Program headers extend beyond file\n");
    res = -1;
    goto err_free_so;
  }

  prog_hdr = (Elf32_Phdr *)((uintptr_t)so_base + elf_hdr->e_phoff);

  if (elf_hdr->e_shoff + (elf_hdr->e_shnum * sizeof(Elf32_Shdr)) > so_size) {
    debugPrintf("so_load: Section headers extend beyond file\n");
    res = -1;
    goto err_free_so;
  }

  sec_hdr = (Elf32_Shdr *)((uintptr_t)so_base + elf_hdr->e_shoff);

  if (elf_hdr->e_shstrndx >= elf_hdr->e_shnum) {
    debugPrintf("so_load: Invalid string table index\n");
    res = -1;
    goto err_free_so;
  }

  shstrtab =
      (char *)((uintptr_t)so_base + sec_hdr[elf_hdr->e_shstrndx].sh_offset);

  /* Span total = maior (p_vaddr + p_memsz) entre os PT_LOAD; acha o exec. */
  size_t max_end = 0;
  int exec_seg = -1;
  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type == PT_LOAD) {
      size_t end = prog_hdr[i].p_vaddr + prog_hdr[i].p_memsz;
      if (end > max_end)
        max_end = end;
      if ((prog_hdr[i].p_flags & PF_X) == PF_X)
        exec_seg = i;
    }
  }
  if (exec_seg < 0) {
    res = -1;
    goto err_free_so;
  }

  load_size = ALIGN_MEM(max_end, 0x1000);
  debugPrintf("so_load: Total load size: %zu bytes (max: %zu)\n", load_size,
              max_size);
  if (load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  load_base = base;
  if (!load_base)
    goto err_free_so;

  memset(load_base, 0, load_size);
  load_virtbase = load_base;
  debugPrintf("so_load: load base = %p\n", load_virtbase);

  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type != PT_LOAD)
      continue;
    memcpy((void *)((uintptr_t)load_base + prog_hdr[i].p_vaddr),
           (void *)((uintptr_t)so_base + prog_hdr[i].p_offset),
           prog_hdr[i].p_filesz);
    debugPrintf("so_load:  PT_LOAD vaddr=0x%lx file=0x%lx mem=0x%lx %c%c%c\n",
                (unsigned long)prog_hdr[i].p_vaddr,
                (unsigned long)prog_hdr[i].p_filesz,
                (unsigned long)prog_hdr[i].p_memsz,
                (prog_hdr[i].p_flags & PF_R) ? 'R' : '-',
                (prog_hdr[i].p_flags & PF_W) ? 'W' : '-',
                (prog_hdr[i].p_flags & PF_X) ? 'X' : '-');
  }

  text_size = prog_hdr[exec_seg].p_memsz;
  text_base = (void *)((uintptr_t)load_base + prog_hdr[exec_seg].p_vaddr);
  text_virtbase = text_base;
  data_base = load_base;
  data_size = load_size;
  data_virtbase = load_base;

  syms = NULL;
  dynstrtab = NULL;

  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      syms = (Elf32_Sym *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      num_syms = sec_hdr[i].sh_size / sizeof(Elf32_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      dynstrtab = (char *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
    }
  }

  if (syms == NULL || dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  debugPrintf("so_load: %d dynamic symbols found\n", num_syms);
  return 0;

err_free_load:
  free(load_base);
err_free_so:
  free(so_base);
  return res;
}

/* ARM usa REL (addend implícito em *ptr). */
int so_relocate(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 ||
        strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        uint32_t *ptr =
            (uint32_t *)((uintptr_t)load_base + rels[j].r_offset);
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_ARM_ABS32:
          /* S + A (A = *ptr). Símbolo EXTERNO (UNDEF) é resolvido por NOME em
           * so_resolve; aqui só os definidos no módulo. */
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = (uint32_t)((uintptr_t)load_base + sym->st_value) + *ptr;
          break;
        case R_ARM_RELATIVE:
          /* B + A (A = *ptr, o vaddr base). */
          *ptr = (uint32_t)((uintptr_t)load_base) + *ptr;
          break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
          /* S (addend = 0 por convenção). O valor in-place de um JUMP_SLOT é o
           * stub do PLT (lazy-bind), NÃO um addend -> NÃO somar *ptr (senão
           * S+stub_PLT = lixo). Definidos aqui; UNDEF resolvido em so_resolve. */
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = (uint32_t)((uintptr_t)load_base + sym->st_value);
          break;
        default:
          debugPrintf("so_relocate: reloc type %x ignorada (off 0x%lx sym '%s')\n",
                      type, (unsigned long)rels[j].r_offset,
                      sym->st_name ? dynstrtab + sym->st_name : "?");
          break;
        }
      }
    }
  }
  return 0;
}

int so_resolve(DynLibFunction *funcs, int num_funcs,
               int taint_missing_imports) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 ||
        strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        uint32_t *ptr =
            (uint32_t *)((uintptr_t)load_base + rels[j].r_offset);
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
        case R_ARM_ABS32: {
          if (sym->st_shndx == SHN_UNDEF) {
            char *name = dynstrtab + sym->st_name;
            uintptr_t resolved = 0;
            for (int k = 0; k < num_funcs; k++) {
              if (strcmp(name, funcs[k].symbol) == 0) {
                resolved = funcs[k].func;
                break;
              }
            }
            if (!resolved) {
              /* ponte softfp->hardfp p/ funções float (jogo é softfp, device é
               * hardfp): ANTES do dlsym, rota libm/glClearColor p/ wrappers. */
              extern void *softfp_resolve(const char *);
              resolved = (uintptr_t)softfp_resolve(name);
            }
            if (!resolved) {
              resolved = (uintptr_t)dlsym(RTLD_DEFAULT, name);
            }
            if (resolved) {
              /* ABS32 mantém o addend implícito; GLOB_DAT/JUMP_SLOT addend=0 */
              if (type == R_ARM_ABS32)
                *ptr = (uint32_t)resolved + *ptr;
              else
                *ptr = (uint32_t)resolved;
            } else {
              if (taint_missing_imports)
                *ptr = rels[j].r_offset;
              fprintf(stderr,
                      "*** UNRESOLVED import: \"%s\" (off 0x%lx, type %d) ***\n",
                      name, (unsigned long)rels[j].r_offset, type);
            }
          }
          break;
        }
        default:
          break;
        }
      }
    }
  }
  return 0;
}

void so_execute_init_array(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int n = (int)(sec_hdr[i].sh_size / 4);
      int (**init_array)() =
          (void *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      debugPrintf("so_execute_init_array: %d construtores\n", n);
      for (int j = 0; j < n; j++) {
        uintptr_t fn = (uintptr_t)init_array[j];
        if (fn != 0 && fn != (uintptr_t)-1) {
          debugPrintf("  init[%d] @ %p\n", j, (void *)fn);
          init_array[j]();
        }
      }
      debugPrintf("so_execute_init_array: concluído (%d)\n", n);
    }
  }
}

uintptr_t so_find_addr(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)load_base + syms[i].st_value;
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_addr_safe(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)load_base + syms[i].st_value;
  }
  return 0;
}

uintptr_t so_find_addr_rx(const char *symbol) {
  return so_find_addr(symbol);
}

uintptr_t so_find_rel_addr(const char *symbol) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 ||
        strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT) {
          char *name = dynstrtab + sym->st_name;
          if (strcmp(name, symbol) == 0)
            return (uintptr_t)load_base + rels[j].r_offset;
        }
      }
    }
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_rel_addr_safe(const char *symbol) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 ||
        strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT) {
          char *name = dynstrtab + sym->st_name;
          if (strcmp(name, symbol) == 0)
            return (uintptr_t)load_base + rels[j].r_offset;
        }
      }
    }
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(void) {
  if (load_base == NULL)
    return -1;
  if (so_base)
    so_free_temp();
  if (munmap(load_base, load_size) != 0)
    fatal_error("Error: could not unmap library memory");
  return 0;
}

DynLibFunction *so_snapshot_symbols(int *out_count) {
  int n = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF) continue;
    if (syms[i].st_value == 0) continue;
    int bind = ELF32_ST_BIND(syms[i].st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
    if (syms[i].st_name == 0) continue;
    n++;
  }

  DynLibFunction *tbl = malloc(sizeof(DynLibFunction) * (n > 0 ? n : 1));
  if (!tbl) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  int k = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF) continue;
    if (syms[i].st_value == 0) continue;
    int bind = ELF32_ST_BIND(syms[i].st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
    if (syms[i].st_name == 0) continue;
    tbl[k].symbol = dynstrtab + syms[i].st_name;
    tbl[k].func = (uintptr_t)load_base + syms[i].st_value;
    k++;
  }

  if (out_count) *out_count = k;
  return tbl;
}
