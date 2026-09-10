/*
 * so_util.c (armhf / ARM 32-bit) -- carrega e faz hook de .so Android armeabi-v7a
 *
 * Port do so_util arm64 do framework nextos_ports_android p/ ARM 32-bit.
 * Diferenças-chave vs arm64:
 *   - Elf32_* em vez de Elf64_*, ELFCLASS32, EM_ARM
 *   - Relocações REL (.rel.dyn/.rel.plt, Elf32_Rel SEM r_addend) — o addend é
 *     o valor IMPLÍCITO já gravado no alvo (*ptr). Android armeabi-v7a usa REL.
 *   - Tipos R_ARM_* (ABS32=2, GLOB_DAT=21, JUMP_SLOT=22, RELATIVE=23)
 *   - hook_arm (ARM + Thumb) em vez de hook_arm64
 */
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
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

static void *mapping_base, *load_base, *load_virtbase;
static size_t load_size;
static uintptr_t load_min_vaddr;

static void *so_base;

static Elf32_Ehdr *elf_hdr;
static Elf32_Phdr *prog_hdr;
static Elf32_Shdr *sec_hdr;
static Elf32_Sym *syms;
static int num_syms;

static char *shstrtab;
static char *dynstrtab;

/* Hook ARM 32-bit. Detecta Thumb pelo bit0 do endereço (ponteiros Thumb têm LSB=1). */
void hook_arm(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  if (addr & 1) {
    /* Thumb-2: LDR.W PC, [PC] ; .word dst  (alinhar em 2 bytes) */
    uint16_t *hook = (uint16_t *)(addr & ~1u);
    hook[0] = 0xf8df; /* LDR.W PC, [PC, #0] */
    hook[1] = 0xf000;
    *(uint32_t *)(hook + 2) = dst;
  } else {
    /* ARM: LDR PC, [PC, #-4] ; .word dst */
    uint32_t *hook = (uint32_t *)addr;
    hook[0] = 0xe51ff004u; /* LDR PC, [PC, #-4] */
    hook[1] = (uint32_t)dst;
  }
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
  __builtin___clear_cache((char *)mapping_base,
                          (char *)mapping_base + load_size);
}

void so_free_temp(void) {
  /* Section headers and their names are intentionally retained until unload:
   * relocation lookup, exported-symbol lookup and incremental init-array
   * execution all use that metadata. */
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
    debugPrintf("mprotect(%p, %zu, 0x%x) failed: %s\n", (void *)page_base, plen,
                prot, strerror(errno));
    return -1;
  }
  return 0;
}

void so_finalize(void) {
  for (int index = 0; index < elf_hdr->e_phnum; ++index) {
    if (prog_hdr[index].p_type != PT_LOAD || prog_hdr[index].p_memsz == 0)
      continue;
    int protection = 0;
    if (prog_hdr[index].p_flags & PF_R) protection |= PROT_READ;
    if (prog_hdr[index].p_flags & PF_W) protection |= PROT_WRITE;
    if (prog_hdr[index].p_flags & PF_X) protection |= PROT_EXEC;
    void *segment =
        (void *)((uintptr_t)load_virtbase + prog_hdr[index].p_vaddr);
    if (protect_range(segment, prog_hdr[index].p_memsz, protection) != 0)
      fatal_error("Error: could not protect PT_LOAD %d at %p (size %u)",
                  index, segment, (unsigned)prog_hdr[index].p_memsz);
  }
}

int so_load(const char *filename, void *base, size_t max_size) {
  int res = 0;
  size_t so_size = 0;
  int text_segno = -1;
  int data_segno = -1;
  uintptr_t maximum_vaddr = 0;

  mapping_base = NULL;
  load_base = NULL;
  load_virtbase = NULL;
  load_size = 0;
  load_min_vaddr = UINTPTR_MAX;

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

  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type == PT_LOAD) {
      if (prog_hdr[i].p_filesz > prog_hdr[i].p_memsz ||
          (uint64_t)prog_hdr[i].p_offset + prog_hdr[i].p_filesz > so_size) {
        debugPrintf("so_load: invalid PT_LOAD %d bounds\n", i);
        res = -1;
        goto err_free_so;
      }
      uintptr_t start = prog_hdr[i].p_vaddr & ~(uintptr_t)0xfff;
      uintptr_t end = ALIGN_MEM((uintptr_t)prog_hdr[i].p_vaddr +
                                    prog_hdr[i].p_memsz,
                                0x1000);
      if (end < start) {
        res = -1;
        goto err_free_so;
      }
      if (start < load_min_vaddr)
        load_min_vaddr = start;
      if (end > maximum_vaddr)
        maximum_vaddr = end;
      if ((prog_hdr[i].p_flags & PF_X) && text_segno < 0)
        text_segno = i;
      if ((prog_hdr[i].p_flags & PF_W) && data_segno < 0)
        data_segno = i;
    }
  }

  if (load_min_vaddr == UINTPTR_MAX || maximum_vaddr <= load_min_vaddr ||
      text_segno < 0 || data_segno < 0) {
    debugPrintf("so_load: missing usable PT_LOAD segments\n");
    res = -1;
    goto err_free_so;
  }
  load_size = maximum_vaddr - load_min_vaddr;
  debugPrintf("so_load: Total load size: %zu bytes (max: %zu)\n", load_size,
              max_size);
  if (load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  mapping_base = base;
  if (!mapping_base) {
    res = -1;
    goto err_free_so;
  }

  memset(mapping_base, 0, load_size);
  load_base = (void *)((uintptr_t)mapping_base - load_min_vaddr);
  load_virtbase = load_base;

  debugPrintf("so_load: mapping=%p bias=%p min_vaddr=0x%lx\n", mapping_base,
              load_virtbase, (unsigned long)load_min_vaddr);

  for (int i = 0; i < elf_hdr->e_phnum; ++i) {
    if (prog_hdr[i].p_type != PT_LOAD)
      continue;
    void *destination =
        (void *)((uintptr_t)load_virtbase + prog_hdr[i].p_vaddr);
    memcpy(destination, (const char *)so_base + prog_hdr[i].p_offset,
           prog_hdr[i].p_filesz);
  }

  /* Compatibility views used by hooks and diagnostics. */
  text_size = prog_hdr[text_segno].p_memsz;
  text_virtbase =
      (void *)(prog_hdr[text_segno].p_vaddr + (Elf32_Addr)(uintptr_t)load_virtbase);
  text_base = text_virtbase;

  data_size = prog_hdr[data_segno].p_memsz;
  data_virtbase =
      (void *)(prog_hdr[data_segno].p_vaddr + (Elf32_Addr)(uintptr_t)load_virtbase);
  data_base = data_virtbase;

  syms = NULL;
  dynstrtab = NULL;

  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      syms = (Elf32_Sym *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
      num_syms = sec_hdr[i].sh_size / sizeof(Elf32_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      dynstrtab = (char *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
    }
  }

  if (syms == NULL || dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  debugPrintf("so_load: %d dynamic symbols found\n", num_syms);
  return 0;

err_free_load:
err_free_so:
  free(so_base);
  so_base = NULL;
  return res;
}

/* REL: o addend é o valor implícito gravado em *ptr (não há r_addend). */
int so_relocate(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 || strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        uintptr_t *ptr =
            (uintptr_t *)((uintptr_t)load_virtbase + rels[j].r_offset);
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_ARM_ABS32:
          /* S + A(implícito) */
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = (uintptr_t)load_virtbase + sym->st_value + *ptr;
          break;
        case R_ARM_RELATIVE:
          /* B + A(implícito) */
          *ptr = (uintptr_t)load_virtbase + *ptr;
          break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
          /* só resolve os DEFINIDOS aqui; UNDEF (imports) vão em so_resolve */
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = (uintptr_t)load_virtbase + sym->st_value;
          break;
        default:
          fatal_error("Error: unknown relocation type: %x\n", type);
          break;
        }
      }
    }
  }
  return 0;
}

/* captura os símbolos EXPORTADOS (definidos, GLOBAL/WEAK) do módulo carregado
 * AGORA numa tabela DynLibFunction (nome→endereço resolvido). Usado p/ resolver
 * módulos posteriores contra este (ex: libapp resolve std::__ndk1 da libc++).
 * Cada módulo deve estar no seu próprio heap (não sobrescrever) p/ os ponteiros
 * de nome (na .dynstr do módulo) seguirem válidos. */
DynLibFunction *so_snapshot_symbols(int *out_count) {
  int n = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF || syms[i].st_value == 0 ||
        syms[i].st_name == 0)
      continue;
    int bind = ELF32_ST_BIND(syms[i].st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      continue;
    n++;
  }
  DynLibFunction *tbl = malloc(sizeof(DynLibFunction) * (n > 0 ? n : 1));
  if (!tbl) { if (out_count) *out_count = 0; return NULL; }
  int k = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF || syms[i].st_value == 0 ||
        syms[i].st_name == 0)
      continue;
    int bind = ELF32_ST_BIND(syms[i].st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      continue;
    tbl[k].symbol = dynstrtab + syms[i].st_name;
    tbl[k].func = (uintptr_t)load_virtbase + syms[i].st_value;
    k++;
  }
  if (out_count) *out_count = k;
  return tbl;
}

int so_resolve(DynLibFunction *funcs, int num_funcs,
               int taint_missing_imports) {
  int unresolved = 0;
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 || strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        uintptr_t *ptr =
            (uintptr_t *)((uintptr_t)load_virtbase + rels[j].r_offset);
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        switch (type) {
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
        case R_ARM_ABS32: {
          if (sym->st_shndx == SHN_UNDEF) {
            uintptr_t addend = type == R_ARM_ABS32 ? *ptr : 0;
            if (taint_missing_imports)
              *ptr = rels[j].r_offset;
            char *name = dynstrtab + sym->st_name;
            int found = 0;
            for (int k = 0; k < num_funcs; k++) {
              if (strcmp(name, funcs[k].symbol) == 0) {
                *ptr = funcs[k].func + addend;
                found = 1;
                break;
              }
            }
            if (!found) {
              /* fallback: resolve da glibc/libs linkadas no loader (libc, m,
               * dl, pthread, SDL2, EGL, GLESv2) — cobre os ~188 libc/GLES
               * triviais sem listar cada um. A TABELA tem prioridade (shims
               * nossos vencem); só cai aqui o que a tabela não tem. */
              void *p = dlsym(RTLD_DEFAULT, name);
              if (p) {
                *ptr = (uintptr_t)p + addend;
                found = 1;
              }
            }
            if (!found) {
              if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                *ptr = addend;
                found = 1;
              }
            }
            if (!found) {
              unresolved++;
              fprintf(stderr,
                      "*** UNRESOLVED import: \"%s\" (GOT offset 0x%x) ***\n",
                      name, (unsigned int)rels[j].r_offset);
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
  return unresolved;
}

size_t so_init_array_count(void) {
  for (int i = 0; i < elf_hdr->e_shnum; ++i) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0)
      return sec_hdr[i].sh_size / sizeof(uint32_t);
  }
  return 0;
}

size_t so_execute_init_array_limit(size_t limit) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      void (**init_array)(void) =
          (void *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
      size_t count = sec_hdr[i].sh_size / sizeof(uint32_t);
      if (limit > count)
        limit = count;
      for (size_t j = 0; j < limit; ++j) {
        uintptr_t function = (uintptr_t)init_array[j];
        if (function != 0 && function != UINTPTR_MAX) {
          debugPrintf("ASM2_INIT_ARRAY index=%zu/%zu function=%p\n", j,
                      count, (void *)function);
          init_array[j]();
        }
      }
      return limit;
    }
  }
  return 0;
}

void so_execute_init_array(void) {
  (void)so_execute_init_array_limit(so_init_array_count());
}

uintptr_t so_find_exidx(uintptr_t pc, int *count) {
  uintptr_t image_start = (uintptr_t)mapping_base;
  uintptr_t image_end = image_start + load_size;
  if (pc < image_start || pc >= image_end)
    return 0;
  for (int index = 0; index < elf_hdr->e_phnum; ++index) {
    if (prog_hdr[index].p_type == PT_ARM_EXIDX) {
      if (count)
        *count = prog_hdr[index].p_memsz / 8;
      return (uintptr_t)load_virtbase + prog_hdr[index].p_vaddr;
    }
  }
  if (count)
    *count = 0;
  return 0;
}

uintptr_t so_find_addr(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)load_virtbase + syms[i].st_value;
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_addr_safe(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (syms[i].st_shndx != SHN_UNDEF && strcmp(name, symbol) == 0)
      return (uintptr_t)load_virtbase + syms[i].st_value;
  }
  return 0;
}

uintptr_t so_find_addr_rx(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)load_virtbase + syms[i].st_value;
  }
  fatal_error("Error: could not find symbol: %s\n", symbol);
  return 0;
}

uintptr_t so_find_rel_addr(const char *symbol) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 || strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT ||
            type == R_ARM_ABS32) {
          char *name = dynstrtab + sym->st_name;
          if (strcmp(name, symbol) == 0)
            return (uintptr_t)load_virtbase + rels[j].r_offset;
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
    if (strcmp(sh_name, ".rel.dyn") == 0 || strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels =
          (Elf32_Rel *)((uintptr_t)load_virtbase + sec_hdr[i].sh_addr);
      for (int j = 0; j < (int)(sec_hdr[i].sh_size / sizeof(Elf32_Rel)); j++) {
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);
        if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT ||
            type == R_ARM_ABS32) {
          char *name = dynstrtab + sym->st_name;
          if (strcmp(name, symbol) == 0)
            return (uintptr_t)load_virtbase + rels[j].r_offset;
        }
      }
    }
  }
  return 0;
}

void *so_guest_vma(uintptr_t vma, size_t size) {
  if (load_virtbase == NULL || vma < load_min_vaddr)
    return NULL;
  uintptr_t offset = vma - load_min_vaddr;
  if (offset > load_size || size > load_size - offset)
    return NULL;
  return (void *)((uintptr_t)load_virtbase + vma);
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(void) {
  if (mapping_base == NULL)
    return -1;
  free(so_base);
  so_base = NULL;
  if (munmap(mapping_base, load_size) != 0)
    fatal_error("Error: could not unmap library memory");
  mapping_base = NULL;
  load_base = NULL;
  load_virtbase = NULL;
  return 0;
}
