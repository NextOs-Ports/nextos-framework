/*
 * so_util.c -- utils to load and hook .so modules
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

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif

void *text_base, *text_virtbase;
size_t text_size;

void *data_base, *data_virtbase;
size_t data_size;

static void *load_base, *load_virtbase;
static size_t load_size;

static void *so_base;

static Elf64_Ehdr *elf_hdr;
static Elf64_Phdr *prog_hdr;
static Elf64_Shdr *sec_hdr;
static Elf64_Sym *syms;
static int num_syms;

static char *shstrtab;
static char *dynstrtab;

/* Pool de trampolins na CAUDA do heap do libGame (dentro de ±128MB de todo o
 * .text). Cada hook escreve só 4 bytes no entry (um B p/ o slot do pool) -> não
 * overflowa funções pequenas (NvAPKSize/NvAPKClose têm 4 bytes e ficam 4 bytes
 * de NvAPKRead/NvAPKGetc; o trampolim antigo de 16 bytes corrompia o vizinho). */
static uint8_t *g_tramp_pool = NULL;
static int g_tramp_used = 0;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  if (g_tramp_pool == NULL) {
    /* cauda do módulo carregado (data_base+data_size), RWX (mmap original) */
    g_tramp_pool = (uint8_t *)(((uintptr_t)data_base + data_size + 15) & ~(uintptr_t)15);
  }
  uint32_t *slot = (uint32_t *)(g_tramp_pool + (size_t)g_tramp_used * 16);
  g_tramp_used++;
  slot[0] = 0x58000051u;          // LDR X17, #0x8
  slot[1] = 0xd61f0220u;          // BR X17
  *(uint64_t *)(slot + 2) = dst;  // alvo (64-bit)
  __builtin___clear_cache((char *)slot, (char *)slot + 16);

  /* entry: B slot (4 bytes) — alcance ±128MB; pool está a poucos MB do .text */
  uint32_t *hook = (uint32_t *)addr;
  intptr_t off = (intptr_t)((uintptr_t)slot - addr);
  hook[0] = 0x14000000u | (((uint32_t)(off >> 2)) & 0x03FFFFFFu);
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
  // Só endurece o segmento executável (RX). O resto da região (rodata, data,
  // bss, GOT) fica RWX (o heap já foi mmap'd RWX) — multi-segmento PT_LOAD não
  // permite o esquema simples text-RX / data-RW sem clobber.
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

  elf_hdr = (Elf64_Ehdr *)so_base;

  if (elf_hdr->e_ident[EI_CLASS] != ELFCLASS64) {
    debugPrintf("so_load: Not a 64-bit ELF file\n");
    res = -1;
    goto err_free_so;
  }

  if (elf_hdr->e_machine != EM_AARCH64) {
    debugPrintf("so_load: Not an AArch64 ELF (machine=%d)\n",
                elf_hdr->e_machine);
    res = -1;
    goto err_free_so;
  }

  debugPrintf("so_load: ELF64 AArch64, %d program headers, %d section headers\n",
              elf_hdr->e_phnum, elf_hdr->e_shnum);

  if (elf_hdr->e_phoff + (elf_hdr->e_phnum * sizeof(Elf64_Phdr)) > so_size) {
    debugPrintf("so_load: Program headers extend beyond file\n");
    res = -1;
    goto err_free_so;
  }

  prog_hdr = (Elf64_Phdr *)((uintptr_t)so_base + elf_hdr->e_phoff);

  if (elf_hdr->e_shoff + (elf_hdr->e_shnum * sizeof(Elf64_Shdr)) > so_size) {
    debugPrintf("so_load: Section headers extend beyond file\n");
    res = -1;
    goto err_free_so;
  }

  sec_hdr = (Elf64_Shdr *)((uintptr_t)so_base + elf_hdr->e_shoff);

  if (elf_hdr->e_shstrndx >= elf_hdr->e_shnum) {
    debugPrintf("so_load: Invalid string table index\n");
    res = -1;
    goto err_free_so;
  }

  shstrtab =
      (char *)((uintptr_t)so_base + sec_hdr[elf_hdr->e_shstrndx].sh_offset);

  // Span total = maior (p_vaddr + p_memsz) entre os PT_LOAD; acha o seg. exec.
  // (NDK r27 gera vários PT_LOAD: R / R+X / RW / RW+RELRO — mapear TODOS.)
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

  // Mapeia cada PT_LOAD no seu p_vaddr (relativo a load_base = vaddr 0).
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

  // Bookkeeping do segmento executável (para hooks/crash/finalize).
  text_size = prog_hdr[exec_seg].p_memsz;
  text_base = (void *)((uintptr_t)load_base + prog_hdr[exec_seg].p_vaddr);
  text_virtbase = text_base;
  // data_base/size = região inteira; so_finalize só endurece o text.
  data_base = load_base;
  data_size = load_size;
  data_virtbase = load_base;

  syms = NULL;
  dynstrtab = NULL;

  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      syms = (Elf64_Sym *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      num_syms = sec_hdr[i].sh_size / sizeof(Elf64_Sym);
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

int so_relocate(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        uintptr_t *ptr =
            (uintptr_t *)((uintptr_t)load_base + rels[j].r_offset);
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_AARCH64_ABS64:
          // ABS64 a símbolo EXTERNO (UNDEF) deve ser resolvido por NOME
          // (igual GLOB_DAT) em so_resolve — NÃO base+0. Aqui só os definidos.
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = (uintptr_t)load_base + sym->st_value + rels[j].r_addend;
          break;
        case R_AARCH64_RELATIVE:
          *ptr = (uintptr_t)load_base + rels[j].r_addend;
          break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
          if (sym->st_shndx != SHN_UNDEF)
            *ptr =
                (uintptr_t)load_base + sym->st_value + rels[j].r_addend;
          break;
        default:
          // TLS (TPREL/TLSDESC) e outros: bring-up loga e segue (não fatal).
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
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        uintptr_t *ptr =
            (uintptr_t *)((uintptr_t)load_base + rels[j].r_offset);
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
        case R_AARCH64_ABS64: {
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
              // Fallback: escopo global do processo (libs do device
              // pré-carregadas RTLD_GLOBAL + glibc/libm/libdl).
              resolved = (uintptr_t)dlsym(RTLD_DEFAULT, name);
            }
            if (resolved) {
              *ptr = resolved + rels[j].r_addend;
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
      int n = (int)(sec_hdr[i].sh_size / 8);
      int (**init_array)() =
          (void *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      debugPrintf("so_execute_init_array: %d construtores\n", n);
      for (int j = 0; j < n; j++) {
        if (init_array[j] != 0) {
          debugPrintf("  init[%d] @ %p\n", j, (void *)init_array[j]);
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
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)load_base + syms[i].st_value;
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_rel_addr(const char *symbol) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);
        if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
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
    if (strcmp(sh_name, ".rela.dyn") == 0 ||
        strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels =
          (Elf64_Rela *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela)); j++) {
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);
        if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
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
  // 1ª passada: contar símbolos definidos exportáveis (GLOBAL/WEAK, func/obj)
  int n = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (syms[i].st_value == 0)
      continue;
    int bind = ELF64_ST_BIND(syms[i].st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      continue;
    if (syms[i].st_name == 0)
      continue;
    n++;
  }

  DynLibFunction *tbl = malloc(sizeof(DynLibFunction) * (n > 0 ? n : 1));
  if (!tbl) {
    if (out_count)
      *out_count = 0;
    return NULL;
  }

  int k = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (syms[i].st_value == 0)
      continue;
    int bind = ELF64_ST_BIND(syms[i].st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      continue;
    if (syms[i].st_name == 0)
      continue;
    tbl[k].symbol = dynstrtab + syms[i].st_name; // .dynstr permanece mapeado
    tbl[k].func = (uintptr_t)load_base + syms[i].st_value;
    k++;
  }

  if (out_count)
    *out_count = k;
  return tbl;
}
