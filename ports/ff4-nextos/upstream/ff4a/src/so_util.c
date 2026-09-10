/*
 * so_util.c -- loader ELF64 aarch64 mínimo p/ o FF4. Módulo único.
 *
 * Mecânica de relocação universal (correta p/ NDK r25b: PT_LOAD layout-agnóstico
 * ancorado em vaddr 0, ABS64/RELATIVE/GLOB_DAT/JUMP_SLOT + RELR compacto).
 * Exceções C++: dl_iterate_phdr custom (o unwinder do libgcc só enxerga libs do
 * linker; nosso módulo é mapeado à mão -> invisível sem isso).
 */
#define _GNU_SOURCE
#include "nxgl_gles1.h"

#include "so_util.h"
#include "error.h"
#include "util.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>

/* Buster (glibc 2.28) ainda nao tinha Elf64_Relr/SHT_RELR: entraram na 2.36.
 * O build universal roda nesse toolchain, entao o tipo vem daqui quando o
 * elf.h disponivel nao o define. Layout identico ao da especificacao. */
#ifndef SHT_RELR
#define SHT_RELR 19
#endif
#if !defined(__GLIBC__) || __GLIBC__ < 2 || \
    (__GLIBC__ == 2 && __GLIBC_MINOR__ < 36)
typedef Elf64_Xword Elf64_Relr;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif

uintptr_t so_text_base;
size_t so_load_size;

static void *so_file;             /* buffer temporário do arquivo */
static uintptr_t load_base;       /* base absoluta (== so_text_base) */
static Elf64_Ehdr *ehdr;
static Elf64_Phdr *phdr;
static Elf64_Shdr *shdr;
static Elf64_Sym *dynsym;
static int n_dynsym;
static char *shstr;
static char *dynstr;

static inline size_t round_up(size_t x, size_t a) {
  return (x + a - 1) & ~(a - 1);
}

int so_load(const char *filename, void *base, size_t max_size) {
  debugPrintf("so_load: abrindo %s\n", filename);
  FILE *f = fopen(filename, "rb");
  if (!f) {
    debugPrintf("so_load: fopen falhou (%s)\n", strerror(errno));
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  so_file = malloc(sz);
  if (!so_file || fread(so_file, sz, 1, f) != 1) {
    fclose(f);
    return -2;
  }
  fclose(f);

  if (memcmp(so_file, ELFMAG, SELFMAG) != 0) {
    debugPrintf("so_load: não é ELF\n");
    return -3;
  }
  ehdr = (Elf64_Ehdr *)so_file;
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_machine != EM_AARCH64) {
    debugPrintf("so_load: não é ELF64 aarch64 (machine=%d)\n", ehdr->e_machine);
    return -3;
  }
  phdr = (Elf64_Phdr *)((uintptr_t)so_file + ehdr->e_phoff);
  shdr = (Elf64_Shdr *)((uintptr_t)so_file + ehdr->e_shoff);
  shstr = (char *)((uintptr_t)so_file + shdr[ehdr->e_shstrndx].sh_offset);

  /* extensão total = maior (p_vaddr + p_memsz) de todos PT_LOAD, ancorado em 0. */
  Elf64_Addr max_vaddr = 0;
  int n_load = 0;
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    n_load++;
    Elf64_Addr end = phdr[i].p_vaddr + phdr[i].p_memsz;
    if (end > max_vaddr)
      max_vaddr = end;
  }
  if (n_load == 0)
    return -4;
  so_load_size = ALIGN_MEM(max_vaddr, 0x1000);
  if (so_load_size > max_size) {
    debugPrintf("so_load: módulo (%zu) > buffer (%zu)\n", so_load_size, max_size);
    return -5;
  }

  load_base = (uintptr_t)base;
  so_text_base = load_base;
  memset(base, 0, so_load_size);

  /* copia cada PT_LOAD p/ base + p_vaddr */
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    memcpy((void *)(load_base + phdr[i].p_vaddr),
           (void *)((uintptr_t)so_file + phdr[i].p_offset), phdr[i].p_filesz);
    debugPrintf("so_load: PT_LOAD vaddr=0x%lx filesz=0x%lx %c%c%c\n",
                (unsigned long)phdr[i].p_vaddr,
                (unsigned long)phdr[i].p_filesz,
                (phdr[i].p_flags & PF_R) ? 'R' : '-',
                (phdr[i].p_flags & PF_W) ? 'W' : '-',
                (phdr[i].p_flags & PF_X) ? 'X' : '-');
  }

  /* localiza .dynsym / .dynstr */
  for (int i = 0; i < ehdr->e_shnum; i++) {
    const char *nm = shstr + shdr[i].sh_name;
    if (!strcmp(nm, ".dynsym")) {
      dynsym = (Elf64_Sym *)(load_base + shdr[i].sh_addr);
      n_dynsym = shdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (!strcmp(nm, ".dynstr")) {
      dynstr = (char *)(load_base + shdr[i].sh_addr);
    }
  }
  if (!dynsym || !dynstr)
    return -6;
  debugPrintf("so_load: %d dynsyms, load_size=0x%zx base=%p\n", n_dynsym,
              so_load_size, base);
  return 0;
}

int so_relocate(void) {
  int so_relocated_any = 0;
  for (int i = 0; i < ehdr->e_shnum; i++) {
    const char *nm = shstr + shdr[i].sh_name;
    if (!strcmp(nm, ".rela.dyn") || !strcmp(nm, ".rela.plt")) {
      Elf64_Rela *rela = (Elf64_Rela *)(load_base + shdr[i].sh_addr);
      int n = shdr[i].sh_size / sizeof(Elf64_Rela);
      so_relocated_any = 1;
      for (int j = 0; j < n; j++) {
        uintptr_t *ptr;
        Elf64_Sym *sym;
        int type;
        /* Onda v2 (AUD-20): um .so malformado nao pode virar escrita fora do
         * mapa nem leitura de simbolo fora do .dynsym -- os 2 checks minimos
         * que o nxloader ja' faz e as copias de so_util nunca fizeram. */
        if (rela[j].r_offset + sizeof(uintptr_t) > so_load_size) {
          debugPrintf("so_relocate: r_offset fora do mapa (0x%llx)\n",
                      (unsigned long long)rela[j].r_offset);
          return -7;
        }
        if ((int)ELF64_R_SYM(rela[j].r_info) >= n_dynsym) {
          debugPrintf("so_relocate: indice de simbolo fora do .dynsym (%u)\n",
                      (unsigned)ELF64_R_SYM(rela[j].r_info));
          return -8;
        }
        ptr = (uintptr_t *)(load_base + rela[j].r_offset);
        sym = &dynsym[ELF64_R_SYM(rela[j].r_info)];
        type = ELF64_R_TYPE(rela[j].r_info);
        switch (type) {
        case R_AARCH64_ABS64:
          /* import externo: deixa p/ so_resolve (senão vira base+0). */
          if (sym->st_shndx == SHN_UNDEF)
            break;
          *ptr = load_base + sym->st_value + rela[j].r_addend;
          break;
        case R_AARCH64_RELATIVE:
          *ptr = load_base + rela[j].r_addend;
          break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = load_base + sym->st_value + rela[j].r_addend;
          break;
        default:
          break; /* resto (TLS etc) — reporta em so_resolve se sobrar */
        }
      }
    } else if (!strcmp(nm, ".relr.dyn")) {
      /* RELR compacto (NDK r25b usa p/ vtables/ponteiros internos). */
      Elf64_Relr *relr = (Elf64_Relr *)(load_base + shdr[i].sh_addr);
      size_t n = shdr[i].sh_size / sizeof(Elf64_Relr);
      uintptr_t *where = NULL;
      for (size_t k = 0; k < n; k++) {
        Elf64_Relr e = relr[k];
        if ((e & 1) == 0) {
          where = (uintptr_t *)(load_base + e);
          *where++ += load_base;
        } else {
          for (int b = 0; (e >>= 1) != 0; b++)
            if (e & 1)
              where[b] += load_base;
          where += 63;
        }
      }
    }
  }
  /* Secoes de relocacao ausentes = strip agressivo dos section headers.
   * Relocar ZERO entradas e reportar sucesso deixava o crash para muito
   * depois, numa chamada indireta com ponteiro cru. */
  if (!so_relocated_any) {
    debugPrintf("so_relocate: nenhuma secao .rela.dyn/.rela.plt encontrada "
                "(section headers stripped?); use um loader por PT_DYNAMIC\n");
    return -9;
  }
  return 0;
}

int so_resolve(DynLibFunction *funcs, int num_funcs) {
  int unresolved = 0;
  for (int i = 0; i < ehdr->e_shnum; i++) {
    const char *nm = shstr + shdr[i].sh_name;
    if (strcmp(nm, ".rela.dyn") && strcmp(nm, ".rela.plt"))
      continue;
    Elf64_Rela *rela = (Elf64_Rela *)(load_base + shdr[i].sh_addr);
    int n = shdr[i].sh_size / sizeof(Elf64_Rela);
    for (int j = 0; j < n; j++) {
      int type = ELF64_R_TYPE(rela[j].r_info);
      if (type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT &&
          type != R_AARCH64_ABS64)
        continue;
      Elf64_Sym *sym = &dynsym[ELF64_R_SYM(rela[j].r_info)];
      if (sym->st_shndx != SHN_UNDEF)
        continue;
      const char *name = dynstr + sym->st_name;
      uintptr_t add = (type == R_AARCH64_ABS64) ? rela[j].r_addend : 0;
      uintptr_t *ptr = (uintptr_t *)(load_base + rela[j].r_offset);
      uintptr_t val = 0;
      /* 1) tabela de shims explícitos */
      for (int k = 0; k < num_funcs; k++) {
        if (!strcmp(name, funcs[k].symbol)) {
          val = funcs[k].func;
          break;
        }
      }
      /* 2) GLES1 resolvido pelo nxgl (o binario nao linka libGLESv1_CM) */
      if (!val) {
        void *gles1 = nxgl_gles1_lookup(name);
        if (gles1)
          val = (uintptr_t)gles1;
      }
      /* 3) fallback: libc/libm/dl reais do glibc do device */
      if (!val) {
        void *real = dlsym(RTLD_DEFAULT, name);
        if (real)
          val = (uintptr_t)real;
      }
      if (val) {
        *ptr = val + add;
      } else {
        if (unresolved++ < 40)
          debugPrintf("UNRESOLVED: %s (off 0x%lx)\n", name,
                      (unsigned long)rela[j].r_offset);
      }
    }
  }
  debugPrintf("so_resolve: %d imports não resolvidos\n", unresolved);
  return unresolved;
}

void so_execute_init_array(void) {
  for (int i = 0; i < ehdr->e_shnum; i++) {
    if (strcmp(shstr + shdr[i].sh_name, ".init_array"))
      continue;
    void (**arr)(void) = (void (**)(void))(load_base + shdr[i].sh_addr);
    int cnt = shdr[i].sh_size / 8;
    debugPrintf("init_array: %d entradas\n", cnt);
    for (int j = 0; j < cnt; j++) {
      uintptr_t fn = (uintptr_t)arr[j];
      if (fn == 0 || fn == load_base || fn == (uintptr_t)-1)
        continue;
      debugPrintf("  ctor[%d/%d] libff4a+0x%lx\n", j, cnt,
                  (unsigned long)(fn - load_base));
      arr[j]();
    }
  }
}

uintptr_t so_find_addr(const char *symbol) {
  uintptr_t a = so_find_addr_safe(symbol);
  if (!a)
    fatal_error("so_find_addr: símbolo não encontrado: %s", symbol);
  return a;
}

uintptr_t so_find_addr_safe(const char *symbol) {
  for (int i = 0; i < n_dynsym; i++) {
    if (dynsym[i].st_shndx == SHN_UNDEF)
      continue;
    if (!strcmp(dynstr + dynsym[i].st_name, symbol))
      return load_base + dynsym[i].st_value;
  }
  return 0;
}

void so_finalize(void) {
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    int prot = 0;
    if (phdr[i].p_flags & PF_R)
      prot |= PROT_READ;
    if (phdr[i].p_flags & PF_W)
      prot |= PROT_WRITE;
    if (phdr[i].p_flags & PF_X)
      prot |= PROT_EXEC;
    uintptr_t a = load_base + phdr[i].p_vaddr;
    uintptr_t pg = a & ~0xfffull;
    size_t len = round_up((a - pg) + phdr[i].p_memsz, 0x1000);
    if (mprotect((void *)pg, len, prot) != 0)
      debugPrintf("so_finalize: mprotect(%p,0x%zx,%d) falhou: %s\n", (void *)pg,
                  len, prot, strerror(errno));
  }
  __builtin___clear_cache((char *)load_base, (char *)(load_base + so_load_size));
}

/* ---- dl_iterate_phdr custom: torna o módulo visível ao unwinder C++ ---- */
static struct {
  uintptr_t base;
  Elf64_Phdr ph[24];
  int phnum;
  char name[32];
} g_mod;
static int g_have_mod = 0;

void so_record_phdr(const char *name) {
  g_mod.base = load_base;
  int n = ehdr->e_phnum;
  if (n > 24)
    n = 24;
  g_mod.phnum = n;
  for (int i = 0; i < n; i++)
    g_mod.ph[i] = phdr[i]; /* p_vaddr já é offset relativo (não mexemos) */
  snprintf(g_mod.name, sizeof(g_mod.name), "%s", name ? name : "libff4a.so");
  g_have_mod = 1;
}

/* intercepta o dl_iterate_phdr do glibc: primeiro os módulos reais, depois o nosso. */
extern int __real_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *),
                                  void *data) __attribute__((weak));

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
  /* módulos reais via a implementação da libc (resolvida por symbol interposition) */
  int r = 0;
  static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
  if (!real)
    real = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
  if (real)
    r = real(cb, data);
  if (r != 0)
    return r;
  if (g_have_mod) {
    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr = g_mod.base;
    info.dlpi_name = g_mod.name;
    info.dlpi_phdr = g_mod.ph;
    info.dlpi_phnum = g_mod.phnum;
    r = cb(&info, sizeof(info), data);
  }
  return r;
}

void so_free_temp(void) {
  free(so_file);
  so_file = NULL;
}
