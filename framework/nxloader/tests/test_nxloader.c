/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader.h"

#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#endif
#ifndef R_AARCH64_PREL64
#define R_AARCH64_PREL64 260
#endif
#ifndef R_AARCH64_PREL32
#define R_AARCH64_PREL32 261
#endif
#ifndef R_AARCH64_PREL16
#define R_AARCH64_PREL16 262
#endif
#ifndef R_AARCH64_ADR_PREL_LO21
#define R_AARCH64_ADR_PREL_LO21 274
#endif
#ifndef R_AARCH64_ADR_PREL_PG_HI21
#define R_AARCH64_ADR_PREL_PG_HI21 275
#endif
#ifndef R_AARCH64_ADD_ABS_LO12_NC
#define R_AARCH64_ADD_ABS_LO12_NC 277
#endif
#ifndef R_AARCH64_LDST64_ABS_LO12_NC
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#endif
#ifndef R_AARCH64_JUMP26
#define R_AARCH64_JUMP26 282
#endif
#ifndef R_AARCH64_CALL26
#define R_AARCH64_CALL26 283
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif
#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif
#ifndef R_ARM_NONE
#define R_ARM_NONE 0
#endif
#ifndef R_ARM_REL32
#define R_ARM_REL32 3
#endif
#ifndef R_ARM_THM_CALL
#define R_ARM_THM_CALL 10
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
#ifndef R_ARM_CALL
#define R_ARM_CALL 28
#endif
#ifndef R_ARM_JUMP24
#define R_ARM_JUMP24 29
#endif
#ifndef R_ARM_TLS_GOTDESC
#define R_ARM_TLS_GOTDESC 90
#endif
#ifndef R_ARM_TLS_CALL
#define R_ARM_TLS_CALL 91
#endif
#ifndef R_ARM_TLS_DESCSEQ
#define R_ARM_TLS_DESCSEQ 92
#endif
#ifndef R_ARM_THM_TLS_DESCSEQ16
#define R_ARM_THM_TLS_DESCSEQ16 129
#endif
#ifndef R_ARM_THM_TLS_DESCSEQ32
#define R_ARM_THM_TLS_DESCSEQ32 130
#endif
#ifndef R_ARM_THM_TLS_CALL
#define R_ARM_THM_TLS_CALL 93
#endif
#ifndef R_ARM_IRELATIVE
#define R_ARM_IRELATIVE 160
#endif
#ifndef EF_ARM_ABI_FLOAT_SOFT
#define EF_ARM_ABI_FLOAT_SOFT 0x00000200
#endif
#ifndef EF_ARM_ABI_FLOAT_HARD
#define EF_ARM_ABI_FLOAT_HARD 0x00000400
#endif
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif
#ifndef STT_TLS
#define STT_TLS 6
#endif
#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA 0x60000011
#endif
#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL 0x6000000f
#endif
#ifndef DT_ANDROID_RELSZ
#define DT_ANDROID_RELSZ 0x60000010
#endif
#ifndef DT_ANDROID_RELASZ
#define DT_ANDROID_RELASZ 0x60000012
#endif
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef DT_RELR
#define DT_RELR 36
#endif
#ifndef DT_TLSDESC_PLT
#define DT_TLSDESC_PLT 0x6ffffef6
#endif
#ifndef DF_TEXTREL
#define DF_TEXTREL 0x00000004
#endif

#define FIXTURE_SIZE 0x4000u
#define DYNAMIC_VMA 0x2100u
#define SLOT_RELATIVE 0x2200u
#define SLOT_ABSOLUTE64 0x2208u
#define SLOT_IMPORT64 0x2210u
#define SLOT_WEAK64 0x2218u
#define SLOT_ABSOLUTE32 0x2204u
#define SLOT_IMPORT32 0x2208u
#define SLOT_WEAK32 0x220cu
#define INIT_ARRAY_VMA 0x2280u
#define STRING_VMA 0x2400u
#define SYMBOL_VMA 0x2500u
#define HASH_VMA 0x2600u
#define RELOCATION_VMA 0x2700u
#define LOCAL_VMA 0x1000u
#define HOOK_VMA 0x1100u
#define ARM_BRANCH_VMA 0x1200u
#define ARM_BRANCH2_VMA 0x1210u

/* Deliberately large enough that a quadratic DT_NEEDED duplicate check would
 * perform roughly two million name comparisons per ABI. */
#define MANY_NEEDED_COUNT 2048u
#define MANY_NEEDED_FIXTURE_SIZE 0x20000u
#define MANY_NEEDED_DYNAMIC_VMA 0x2000u
#define MANY_NEEDED_NAME_STRIDE 16u

/* Mirrors the internal parser contract at its two exact boundary values. */
#define TEST_DYNAMIC_NAME_LIMIT 4096u
#define LONG_NEEDED_FIXTURE_SIZE 0x10000u
#define LONG_NEEDED_DYNAMIC_VMA 0x2000u
#define LONG_NEEDED_STRING_VMA 0x5000u
#define LONG_NEEDED_SYMBOL_VMA 0x6100u
#define LONG_NEEDED_HASH_VMA 0x6200u
#define OVERLAPPING_NEEDED_COUNT 512u

/* Thousands of tiny, ordered PT_LOAD entries make a linear lookup per
 * relocation quadratic.  Each occupies a distinct page so the same fixture
 * also exercises the single-sweep PT_GNU_RELRO coverage algorithm. */
#define MANY_SEGMENT_COUNT 4096u
#define MANY_SEGMENT_FIXTURE_SIZE 0x100000u
#define MANY_SEGMENT_DYNAMIC_VMA 0x40000u
#define MANY_SEGMENT_STRING_VMA 0x41000u
#define MANY_SEGMENT_SYMBOL_VMA 0x42000u
#define MANY_SEGMENT_HASH_VMA 0x42100u
#define MANY_SEGMENT_RELOCATION_VMA 0x50000u
#define MANY_SEGMENT_TARGET_VMA 0x100000u
#define MANY_SEGMENT_STRIDE 0x1000u

#if UINTPTR_MAX > UINT32_MAX
#define TEST_HOST64 ((uintptr_t)UINT64_C(0x123456789abc))
#else
#define TEST_HOST64 ((uintptr_t)UINT32_C(0x12345000))
#endif

static int failures;
static size_t unknown_relocation_hook_calls;

typedef struct test_log_capture {
  char text[4096];
  size_t used;
} test_log_capture;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      failures++;                                                            \
    }                                                                        \
  } while (0)

static void capture_log(void *userdata, nxloader_log_level level,
                        const char *message) {
  test_log_capture *capture = (test_log_capture *)userdata;
  int count;
  (void)level;
  if (!capture || !message || capture->used >= sizeof(capture->text))
    return;
  count = snprintf(capture->text + capture->used,
                   sizeof(capture->text) - capture->used, "%s\n", message);
  if (count < 0)
    return;
  if ((size_t)count >= sizeof(capture->text) - capture->used)
    capture->used = sizeof(capture->text) - 1;
  else
    capture->used += (size_t)count;
}

static uint32_t read32(const void *pointer) {
  uint32_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static uint16_t read16(const void *pointer) {
  uint16_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static uint64_t read64(const void *pointer) {
  uint64_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static void write32(void *pointer, uint32_t value) {
  memcpy(pointer, &value, sizeof(value));
}

static void write64(void *pointer, uint64_t value) {
  memcpy(pointer, &value, sizeof(value));
}

static void write16(void *pointer, uint16_t value) {
  memcpy(pointer, &value, sizeof(value));
}

static int bytes_are_zero(const void *pointer, size_t size) {
  const unsigned char *bytes = (const unsigned char *)pointer;
  size_t index;
  for (index = 0; index < size; ++index) {
    if (bytes[index] != 0)
      return 0;
  }
  return 1;
}

static int mapping_permissions(const void *pointer, char permissions[5]) {
  FILE *maps = fopen("/proc/self/maps", "r");
  char line[512];
  uintptr_t address = (uintptr_t)pointer;
  if (!maps)
    return 0;
  while (fgets(line, sizeof(line), maps)) {
    unsigned long long start;
    unsigned long long end;
    char found[5] = {0};
    if (sscanf(line, "%llx-%llx %4s", &start, &end, found) == 3 &&
        address >= (uintptr_t)start && address < (uintptr_t)end) {
      memcpy(permissions, found, sizeof(found));
      fclose(maps);
      return 1;
    }
  }
  fclose(maps);
  return 0;
}

static void append_dynamic64(unsigned char *data, int64_t tag,
                             uint64_t value) {
  Elf64_Dyn *dynamic = (Elf64_Dyn *)(data + DYNAMIC_VMA);
  size_t index = 0;
  while (dynamic[index].d_tag != DT_NULL)
    index++;
  dynamic[index].d_tag = tag;
  dynamic[index].d_un.d_val = value;
  dynamic[index + 1].d_tag = DT_NULL;
}

static void append_dynamic32(unsigned char *data, int32_t tag,
                             uint32_t value) {
  Elf32_Dyn *dynamic = (Elf32_Dyn *)(data + DYNAMIC_VMA);
  size_t index = 0;
  while (dynamic[index].d_tag != DT_NULL)
    index++;
  dynamic[index].d_tag = tag;
  dynamic[index].d_un.d_val = value;
  dynamic[index + 1].d_tag = DT_NULL;
}

static Elf64_Dyn *find_dynamic64(unsigned char *data, int64_t tag) {
  Elf64_Dyn *dynamic = (Elf64_Dyn *)(data + DYNAMIC_VMA);
  size_t index;
  for (index = 0; dynamic[index].d_tag != DT_NULL; ++index) {
    if (dynamic[index].d_tag == tag)
      return &dynamic[index];
  }
  return NULL;
}

static Elf32_Dyn *find_dynamic32(unsigned char *data, int32_t tag) {
  Elf32_Dyn *dynamic = (Elf32_Dyn *)(data + DYNAMIC_VMA);
  size_t index;
  for (index = 0; dynamic[index].d_tag != DT_NULL; ++index) {
    if (dynamic[index].d_tag == tag)
      return &dynamic[index];
  }
  return NULL;
}

static size_t build_strings(unsigned char *data) {
  static const char strings[] =
      "\0local\0host\0optional\0hook_target\0libaux.so\0libsynthetic.so\0";
  memcpy(data + STRING_VMA, strings, sizeof(strings));
  return sizeof(strings);
}

enum {
  STR_LOCAL = 1,
  STR_HOST = 7,
  STR_OPTIONAL = 12,
  STR_HOOK = 21,
  STR_NEEDED = 33,
  STR_SONAME = 43
};

static unsigned char *build_elf64(uint32_t first_relocation_type) {
  unsigned char *data = (unsigned char *)calloc(1, FIXTURE_SIZE);
  Elf64_Ehdr *header;
  Elf64_Phdr *programs;
  Elf64_Dyn *dynamic;
  Elf64_Sym *symbols;
  Elf64_Rela *relocations;
  uint32_t *hash;
  size_t dynamic_index = 0;
  size_t string_size;
  if (!data)
    return NULL;
  header = (Elf64_Ehdr *)data;
  memcpy(header->e_ident, ELFMAG, SELFMAG);
  header->e_ident[EI_CLASS] = ELFCLASS64;
  header->e_ident[EI_DATA] = ELFDATA2LSB;
  header->e_ident[EI_VERSION] = EV_CURRENT;
  header->e_type = ET_DYN;
  header->e_machine = EM_AARCH64;
  header->e_version = EV_CURRENT;
  header->e_ehsize = sizeof(*header);
  header->e_phoff = sizeof(*header);
  header->e_phentsize = sizeof(Elf64_Phdr);
  header->e_phnum = 4;
  programs = (Elf64_Phdr *)(data + header->e_phoff);
  programs[0].p_type = PT_LOAD;
  programs[0].p_offset = 0;
  programs[0].p_vaddr = 0;
  programs[0].p_filesz = 0x2000;
  programs[0].p_memsz = 0x2000;
  programs[0].p_flags = PF_R | PF_X;
  programs[0].p_align = 0x1000;
  programs[1].p_type = PT_LOAD;
  programs[1].p_offset = 0x2000;
  programs[1].p_vaddr = 0x2000;
  programs[1].p_filesz = 0x2000;
  programs[1].p_memsz = 0x3000;
  programs[1].p_flags = PF_R | PF_W;
  programs[1].p_align = 0x1000;
  programs[2].p_type = PT_DYNAMIC;
  programs[2].p_offset = DYNAMIC_VMA;
  programs[2].p_vaddr = DYNAMIC_VMA;
  programs[2].p_filesz = 0x100;
  programs[2].p_memsz = 0x100;
  programs[2].p_flags = PF_R | PF_W;
  programs[2].p_align = 8;
  programs[3].p_type = PT_GNU_RELRO;
  programs[3].p_offset = 0x2000;
  programs[3].p_vaddr = 0x2000;
  programs[3].p_filesz = 0x100;
  programs[3].p_memsz = 0x100;
  programs[3].p_flags = PF_R;
  programs[3].p_align = 0x1000;
  /* AArch64 RET and a separate hookable entry. */
  write32(data + LOCAL_VMA, UINT32_C(0xd65f03c0));
  write32(data + HOOK_VMA, UINT32_C(0xd503201f));
  string_size = build_strings(data);
  symbols = (Elf64_Sym *)(data + SYMBOL_VMA);
  symbols[1].st_name = STR_LOCAL;
  symbols[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[1].st_shndx = 1;
  symbols[1].st_value = LOCAL_VMA;
  symbols[1].st_size = 4;
  symbols[2].st_name = STR_HOST;
  symbols[2].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[2].st_shndx = SHN_UNDEF;
  symbols[3].st_name = STR_OPTIONAL;
  symbols[3].st_info = ELF64_ST_INFO(STB_WEAK, STT_FUNC);
  symbols[3].st_shndx = SHN_UNDEF;
  symbols[4].st_name = STR_HOOK;
  symbols[4].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[4].st_shndx = 1;
  symbols[4].st_value = HOOK_VMA;
  symbols[4].st_size = 16;
  hash = (uint32_t *)(data + HASH_VMA);
  hash[0] = 1;
  hash[1] = 5;
  hash[2] = 1;
  hash[3] = 0;
  hash[4] = 0;
  hash[5] = 0;
  hash[6] = 0;
  hash[7] = 0;
  relocations = (Elf64_Rela *)(data + RELOCATION_VMA);
  relocations[0].r_offset = SLOT_RELATIVE;
  relocations[0].r_info = ELF64_R_INFO(0, first_relocation_type);
  relocations[0].r_addend = 0x1200;
  relocations[1].r_offset = SLOT_ABSOLUTE64;
  relocations[1].r_info = ELF64_R_INFO(1, R_AARCH64_ABS64);
  relocations[1].r_addend = 7;
  relocations[2].r_offset = SLOT_IMPORT64;
  relocations[2].r_info = ELF64_R_INFO(2, R_AARCH64_GLOB_DAT);
  relocations[3].r_offset = SLOT_WEAK64;
  relocations[3].r_info = ELF64_R_INFO(3, R_AARCH64_JUMP_SLOT);
  relocations[4].r_offset = INIT_ARRAY_VMA;
  relocations[4].r_info = ELF64_R_INFO(0, R_AARCH64_RELATIVE);
  relocations[4].r_addend = LOCAL_VMA;

  dynamic = (Elf64_Dyn *)(data + DYNAMIC_VMA);
#define DYN64(tag_value, dynamic_value)                                     \
  do {                                                                      \
    dynamic[dynamic_index].d_tag = (tag_value);                             \
    dynamic[dynamic_index].d_un.d_val = (dynamic_value);                    \
    dynamic_index++;                                                        \
  } while (0)
  DYN64(DT_STRTAB, STRING_VMA);
  DYN64(DT_STRSZ, string_size);
  DYN64(DT_SYMTAB, SYMBOL_VMA);
  DYN64(DT_SYMENT, sizeof(Elf64_Sym));
  DYN64(DT_HASH, HASH_VMA);
  DYN64(DT_RELA, RELOCATION_VMA);
  DYN64(DT_RELASZ, 5 * sizeof(Elf64_Rela));
  DYN64(DT_RELAENT, sizeof(Elf64_Rela));
  DYN64(DT_INIT_ARRAY, INIT_ARRAY_VMA);
  DYN64(DT_INIT_ARRAYSZ, sizeof(uint64_t));
  DYN64(DT_NEEDED, STR_NEEDED);
  DYN64(DT_SONAME, STR_SONAME);
  DYN64(DT_NULL, 0);
#undef DYN64
  return data;
}

static unsigned char *build_elf32(uint32_t first_relocation_type) {
  unsigned char *data = (unsigned char *)calloc(1, FIXTURE_SIZE);
  Elf32_Ehdr *header;
  Elf32_Phdr *programs;
  Elf32_Dyn *dynamic;
  Elf32_Sym *symbols;
  Elf32_Rel *relocations;
  uint32_t *hash;
  size_t dynamic_index = 0;
  size_t string_size;
  if (!data)
    return NULL;
  header = (Elf32_Ehdr *)data;
  memcpy(header->e_ident, ELFMAG, SELFMAG);
  header->e_ident[EI_CLASS] = ELFCLASS32;
  header->e_ident[EI_DATA] = ELFDATA2LSB;
  header->e_ident[EI_VERSION] = EV_CURRENT;
  header->e_type = ET_DYN;
  header->e_machine = EM_ARM;
  header->e_version = EV_CURRENT;
  header->e_flags = UINT32_C(0x05000000);
  header->e_ehsize = sizeof(*header);
  header->e_phoff = sizeof(*header);
  header->e_phentsize = sizeof(Elf32_Phdr);
  header->e_phnum = 5;
  programs = (Elf32_Phdr *)(data + header->e_phoff);
  programs[0].p_type = PT_LOAD;
  programs[0].p_offset = 0;
  programs[0].p_vaddr = 0;
  programs[0].p_filesz = 0x2000;
  programs[0].p_memsz = 0x2000;
  programs[0].p_flags = PF_R | PF_X;
  programs[0].p_align = 0x1000;
  programs[1].p_type = PT_LOAD;
  programs[1].p_offset = 0x2000;
  programs[1].p_vaddr = 0x2000;
  programs[1].p_filesz = 0x2000;
  programs[1].p_memsz = 0x3000;
  programs[1].p_flags = PF_R | PF_W;
  programs[1].p_align = 0x1000;
  programs[2].p_type = PT_DYNAMIC;
  programs[2].p_offset = DYNAMIC_VMA;
  programs[2].p_vaddr = DYNAMIC_VMA;
  programs[2].p_filesz = 0x100;
  programs[2].p_memsz = 0x100;
  programs[2].p_flags = PF_R | PF_W;
  programs[2].p_align = 4;
  programs[3].p_type = PT_GNU_RELRO;
  programs[3].p_offset = 0x2000;
  programs[3].p_vaddr = 0x2000;
  programs[3].p_filesz = 0x100;
  programs[3].p_memsz = 0x100;
  programs[3].p_flags = PF_R;
  programs[3].p_align = 0x1000;
  programs[4].p_type = PT_ARM_EXIDX;
  programs[4].p_offset = 0x1800;
  programs[4].p_vaddr = 0x1800;
  programs[4].p_filesz = 16;
  programs[4].p_memsz = 16;
  programs[4].p_flags = PF_R;
  programs[4].p_align = 4;

  /* ARM BX LR and a separate hookable entry. */
  write32(data + LOCAL_VMA, UINT32_C(0xe12fff1e));
  write32(data + HOOK_VMA, UINT32_C(0xe1a00000));
  write32(data + HOOK_VMA + 4, UINT32_C(0xe1a00000));
  string_size = build_strings(data);
  symbols = (Elf32_Sym *)(data + SYMBOL_VMA);
  symbols[1].st_name = STR_LOCAL;
  symbols[1].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[1].st_shndx = 1;
  symbols[1].st_value = LOCAL_VMA;
  symbols[1].st_size = 4;
  symbols[2].st_name = STR_HOST;
  symbols[2].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[2].st_shndx = SHN_UNDEF;
  symbols[3].st_name = STR_OPTIONAL;
  symbols[3].st_info = ELF32_ST_INFO(STB_WEAK, STT_FUNC);
  symbols[3].st_shndx = SHN_UNDEF;
  symbols[4].st_name = STR_HOOK;
  symbols[4].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[4].st_shndx = 1;
  symbols[4].st_value = HOOK_VMA;
  symbols[4].st_size = 8;
  hash = (uint32_t *)(data + HASH_VMA);
  hash[0] = 1;
  hash[1] = 5;
  hash[2] = 1;
  hash[3] = 0;
  hash[4] = 0;
  hash[5] = 0;
  hash[6] = 0;
  hash[7] = 0;
  write32(data + SLOT_RELATIVE, 0x1200);
  write32(data + SLOT_ABSOLUTE32, 7);
  write32(data + INIT_ARRAY_VMA, LOCAL_VMA);
  relocations = (Elf32_Rel *)(data + RELOCATION_VMA);
  relocations[0].r_offset = SLOT_RELATIVE;
  relocations[0].r_info = ELF32_R_INFO(0, first_relocation_type);
  relocations[1].r_offset = SLOT_ABSOLUTE32;
  relocations[1].r_info = ELF32_R_INFO(1, R_ARM_ABS32);
  relocations[2].r_offset = SLOT_IMPORT32;
  relocations[2].r_info = ELF32_R_INFO(2, R_ARM_GLOB_DAT);
  relocations[3].r_offset = SLOT_WEAK32;
  relocations[3].r_info = ELF32_R_INFO(3, R_ARM_JUMP_SLOT);
  relocations[4].r_offset = INIT_ARRAY_VMA;
  relocations[4].r_info = ELF32_R_INFO(0, R_ARM_RELATIVE);

  dynamic = (Elf32_Dyn *)(data + DYNAMIC_VMA);
#define DYN32(tag_value, dynamic_value)                                     \
  do {                                                                      \
    dynamic[dynamic_index].d_tag = (tag_value);                             \
    dynamic[dynamic_index].d_un.d_val = (dynamic_value);                    \
    dynamic_index++;                                                        \
  } while (0)
  DYN32(DT_STRTAB, STRING_VMA);
  DYN32(DT_STRSZ, string_size);
  DYN32(DT_SYMTAB, SYMBOL_VMA);
  DYN32(DT_SYMENT, sizeof(Elf32_Sym));
  DYN32(DT_HASH, HASH_VMA);
  DYN32(DT_REL, RELOCATION_VMA);
  DYN32(DT_RELSZ, 5 * sizeof(Elf32_Rel));
  DYN32(DT_RELENT, sizeof(Elf32_Rel));
  DYN32(DT_INIT_ARRAY, INIT_ARRAY_VMA);
  DYN32(DT_INIT_ARRAYSZ, sizeof(uint32_t));
  DYN32(DT_NEEDED, STR_NEEDED);
  DYN32(DT_SONAME, STR_SONAME);
  DYN32(DT_NULL, 0);
#undef DYN32
  return data;
}

static size_t test_align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

/* Builds a sectionless load-only ELF with a large dependency list. The
 * dependency order is intentionally the reverse of the string-table order so
 * validation may sort a private index without changing the public order. */
static unsigned char *build_many_needed_fixture(nxloader_arch arch,
                                                size_t needed_count,
                                                int duplicate_last) {
  unsigned char *data;
  size_t dynamic_entry_size;
  size_t dynamic_count;
  size_t dynamic_size;
  size_t string_vma;
  size_t string_size;
  size_t symbol_vma;
  size_t symbol_size;
  size_t hash_vma;
  size_t index;
  uint32_t *hash;
  if ((arch != NXLOADER_ARCH_AARCH64 && arch != NXLOADER_ARCH_ARMV7) ||
      needed_count == 0 || needed_count > UINT32_MAX ||
      needed_count > (SIZE_MAX - 6))
    return NULL;
  dynamic_entry_size = arch == NXLOADER_ARCH_AARCH64 ? sizeof(Elf64_Dyn)
                                                      : sizeof(Elf32_Dyn);
  dynamic_count = needed_count + 6;
  if (dynamic_count > SIZE_MAX / dynamic_entry_size)
    return NULL;
  dynamic_size = dynamic_count * dynamic_entry_size;
  string_vma = test_align_up(MANY_NEEDED_DYNAMIC_VMA + dynamic_size, 0x100);
  if (needed_count > (SIZE_MAX - 1) / MANY_NEEDED_NAME_STRIDE)
    return NULL;
  string_size = 1 + needed_count * MANY_NEEDED_NAME_STRIDE;
  symbol_size = arch == NXLOADER_ARCH_AARCH64 ? sizeof(Elf64_Sym)
                                               : sizeof(Elf32_Sym);
  symbol_vma = test_align_up(string_vma + string_size, 8);
  hash_vma = test_align_up(symbol_vma + symbol_size, sizeof(uint32_t));
  if (hash_vma > MANY_NEEDED_FIXTURE_SIZE - 16)
    return NULL;
  data = (unsigned char *)calloc(1, MANY_NEEDED_FIXTURE_SIZE);
  if (!data)
    return NULL;

  for (index = 0; index < needed_count; ++index) {
    char *name = (char *)(data + string_vma + 1 +
                          index * MANY_NEEDED_NAME_STRIDE);
    int count = snprintf(name, MANY_NEEDED_NAME_STRIDE, "lib%08zu.so", index);
    if (count < 0 || (size_t)count >= MANY_NEEDED_NAME_STRIDE) {
      free(data);
      return NULL;
    }
  }
  hash = (uint32_t *)(data + hash_vma);
  hash[0] = 1;
  hash[1] = 1;
  hash[2] = 0;
  hash[3] = 0;

  if (arch == NXLOADER_ARCH_AARCH64) {
    Elf64_Ehdr *header = (Elf64_Ehdr *)data;
    Elf64_Phdr *programs;
    Elf64_Dyn *dynamic;
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_AARCH64;
    header->e_version = EV_CURRENT;
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = 3;
    programs = (Elf64_Phdr *)(data + header->e_phoff);
    programs[0].p_type = PT_LOAD;
    programs[0].p_filesz = 0x1000;
    programs[0].p_memsz = 0x1000;
    programs[0].p_flags = PF_R | PF_X;
    programs[0].p_align = 0x1000;
    programs[1].p_type = PT_LOAD;
    programs[1].p_offset = 0x1000;
    programs[1].p_vaddr = 0x1000;
    programs[1].p_filesz = MANY_NEEDED_FIXTURE_SIZE - 0x1000;
    programs[1].p_memsz = programs[1].p_filesz;
    programs[1].p_flags = PF_R | PF_W;
    programs[1].p_align = 0x1000;
    programs[2].p_type = PT_DYNAMIC;
    programs[2].p_offset = MANY_NEEDED_DYNAMIC_VMA;
    programs[2].p_vaddr = MANY_NEEDED_DYNAMIC_VMA;
    programs[2].p_filesz = dynamic_size;
    programs[2].p_memsz = dynamic_size;
    programs[2].p_flags = PF_R | PF_W;
    programs[2].p_align = 8;
    write32(data + 0x800, UINT32_C(0xd65f03c0));
    dynamic = (Elf64_Dyn *)(data + MANY_NEEDED_DYNAMIC_VMA);
    dynamic[0].d_tag = DT_STRTAB;
    dynamic[0].d_un.d_val = string_vma;
    dynamic[1].d_tag = DT_STRSZ;
    dynamic[1].d_un.d_val = string_size;
    dynamic[2].d_tag = DT_SYMTAB;
    dynamic[2].d_un.d_val = symbol_vma;
    dynamic[3].d_tag = DT_SYMENT;
    dynamic[3].d_un.d_val = sizeof(Elf64_Sym);
    dynamic[4].d_tag = DT_HASH;
    dynamic[4].d_un.d_val = hash_vma;
    for (index = 0; index < needed_count; ++index) {
      size_t name_index = needed_count - 1 - index;
      if (duplicate_last && index == needed_count - 1)
        name_index = needed_count - 1;
      dynamic[5 + index].d_tag = DT_NEEDED;
      dynamic[5 + index].d_un.d_val =
          1 + name_index * MANY_NEEDED_NAME_STRIDE;
    }
    dynamic[5 + needed_count].d_tag = DT_NULL;
  } else {
    Elf32_Ehdr *header = (Elf32_Ehdr *)data;
    Elf32_Phdr *programs;
    Elf32_Dyn *dynamic;
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS32;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_ARM;
    header->e_version = EV_CURRENT;
    header->e_flags = UINT32_C(0x05000000);
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(Elf32_Phdr);
    header->e_phnum = 3;
    programs = (Elf32_Phdr *)(data + header->e_phoff);
    programs[0].p_type = PT_LOAD;
    programs[0].p_filesz = 0x1000;
    programs[0].p_memsz = 0x1000;
    programs[0].p_flags = PF_R | PF_X;
    programs[0].p_align = 0x1000;
    programs[1].p_type = PT_LOAD;
    programs[1].p_offset = 0x1000;
    programs[1].p_vaddr = 0x1000;
    programs[1].p_filesz = MANY_NEEDED_FIXTURE_SIZE - 0x1000;
    programs[1].p_memsz = programs[1].p_filesz;
    programs[1].p_flags = PF_R | PF_W;
    programs[1].p_align = 0x1000;
    programs[2].p_type = PT_DYNAMIC;
    programs[2].p_offset = MANY_NEEDED_DYNAMIC_VMA;
    programs[2].p_vaddr = MANY_NEEDED_DYNAMIC_VMA;
    programs[2].p_filesz = (Elf32_Word)dynamic_size;
    programs[2].p_memsz = (Elf32_Word)dynamic_size;
    programs[2].p_flags = PF_R | PF_W;
    programs[2].p_align = 4;
    write32(data + 0x800, UINT32_C(0xe12fff1e));
    dynamic = (Elf32_Dyn *)(data + MANY_NEEDED_DYNAMIC_VMA);
    dynamic[0].d_tag = DT_STRTAB;
    dynamic[0].d_un.d_val = (Elf32_Word)string_vma;
    dynamic[1].d_tag = DT_STRSZ;
    dynamic[1].d_un.d_val = (Elf32_Word)string_size;
    dynamic[2].d_tag = DT_SYMTAB;
    dynamic[2].d_un.d_val = (Elf32_Word)symbol_vma;
    dynamic[3].d_tag = DT_SYMENT;
    dynamic[3].d_un.d_val = sizeof(Elf32_Sym);
    dynamic[4].d_tag = DT_HASH;
    dynamic[4].d_un.d_val = (Elf32_Word)hash_vma;
    for (index = 0; index < needed_count; ++index) {
      size_t name_index = needed_count - 1 - index;
      if (duplicate_last && index == needed_count - 1)
        name_index = needed_count - 1;
      dynamic[5 + index].d_tag = DT_NEEDED;
      dynamic[5 + index].d_un.d_val =
          (Elf32_Word)(1 + name_index * MANY_NEEDED_NAME_STRIDE);
    }
    dynamic[5 + needed_count].d_tag = DT_NULL;
  }
  return data;
}

/* A long dynstr suffix can be referenced from many different offsets without
 * duplicating bytes in the file.  This is the adversarial shape that made an
 * unbounded checked_string() perform quadratic aggregate scans. */
static unsigned char *build_long_needed_fixture(nxloader_arch arch,
                                                size_t name_length,
                                                size_t needed_count,
                                                int duplicate_last) {
  unsigned char *data;
  size_t dynamic_entry_size;
  size_t dynamic_count;
  size_t dynamic_size;
  size_t index;
  uint32_t *hash;
  if ((arch != NXLOADER_ARCH_AARCH64 && arch != NXLOADER_ARCH_ARMV7) ||
      name_length == 0 || needed_count == 0 ||
      needed_count > OVERLAPPING_NEEDED_COUNT || needed_count > name_length)
    return NULL;
  dynamic_entry_size = arch == NXLOADER_ARCH_AARCH64 ? sizeof(Elf64_Dyn)
                                                      : sizeof(Elf32_Dyn);
  dynamic_count = needed_count + 6;
  if (dynamic_count > SIZE_MAX / dynamic_entry_size)
    return NULL;
  dynamic_size = dynamic_count * dynamic_entry_size;
  if (LONG_NEEDED_DYNAMIC_VMA + dynamic_size > LONG_NEEDED_STRING_VMA ||
      name_length + 2 > LONG_NEEDED_SYMBOL_VMA - LONG_NEEDED_STRING_VMA)
    return NULL;
  data = (unsigned char *)calloc(1, LONG_NEEDED_FIXTURE_SIZE);
  if (!data)
    return NULL;
  memset(data + LONG_NEEDED_STRING_VMA + 1, 'a', name_length);
  data[LONG_NEEDED_STRING_VMA + 1 + name_length] = '\0';
  hash = (uint32_t *)(data + LONG_NEEDED_HASH_VMA);
  hash[0] = 1;
  hash[1] = 1;
  hash[2] = 0;
  hash[3] = 0;

  if (arch == NXLOADER_ARCH_AARCH64) {
    Elf64_Ehdr *header = (Elf64_Ehdr *)data;
    Elf64_Phdr *programs;
    Elf64_Dyn *dynamic;
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_AARCH64;
    header->e_version = EV_CURRENT;
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = 3;
    programs = (Elf64_Phdr *)(data + header->e_phoff);
    programs[0].p_type = PT_LOAD;
    programs[0].p_filesz = 0x1000;
    programs[0].p_memsz = 0x1000;
    programs[0].p_flags = PF_R | PF_X;
    programs[0].p_align = 0x1000;
    programs[1].p_type = PT_LOAD;
    programs[1].p_offset = 0x1000;
    programs[1].p_vaddr = 0x1000;
    programs[1].p_filesz = LONG_NEEDED_FIXTURE_SIZE - 0x1000;
    programs[1].p_memsz = programs[1].p_filesz;
    programs[1].p_flags = PF_R | PF_W;
    programs[1].p_align = 0x1000;
    programs[2].p_type = PT_DYNAMIC;
    programs[2].p_offset = LONG_NEEDED_DYNAMIC_VMA;
    programs[2].p_vaddr = LONG_NEEDED_DYNAMIC_VMA;
    programs[2].p_filesz = dynamic_size;
    programs[2].p_memsz = dynamic_size;
    programs[2].p_flags = PF_R | PF_W;
    programs[2].p_align = 8;
    write32(data + 0x800, UINT32_C(0xd65f03c0));
    dynamic = (Elf64_Dyn *)(data + LONG_NEEDED_DYNAMIC_VMA);
    dynamic[0].d_tag = DT_STRTAB;
    dynamic[0].d_un.d_val = LONG_NEEDED_STRING_VMA;
    dynamic[1].d_tag = DT_STRSZ;
    dynamic[1].d_un.d_val = name_length + 2;
    dynamic[2].d_tag = DT_SYMTAB;
    dynamic[2].d_un.d_val = LONG_NEEDED_SYMBOL_VMA;
    dynamic[3].d_tag = DT_SYMENT;
    dynamic[3].d_un.d_val = sizeof(Elf64_Sym);
    dynamic[4].d_tag = DT_HASH;
    dynamic[4].d_un.d_val = LONG_NEEDED_HASH_VMA;
    for (index = 0; index < needed_count; ++index) {
      size_t offset = 1 + index;
      if (duplicate_last && index == needed_count - 1)
        offset = 1;
      dynamic[5 + index].d_tag = DT_NEEDED;
      dynamic[5 + index].d_un.d_val = offset;
    }
    dynamic[5 + needed_count].d_tag = DT_NULL;
  } else {
    Elf32_Ehdr *header = (Elf32_Ehdr *)data;
    Elf32_Phdr *programs;
    Elf32_Dyn *dynamic;
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS32;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_ARM;
    header->e_version = EV_CURRENT;
    header->e_flags = UINT32_C(0x05000000);
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(Elf32_Phdr);
    header->e_phnum = 3;
    programs = (Elf32_Phdr *)(data + header->e_phoff);
    programs[0].p_type = PT_LOAD;
    programs[0].p_filesz = 0x1000;
    programs[0].p_memsz = 0x1000;
    programs[0].p_flags = PF_R | PF_X;
    programs[0].p_align = 0x1000;
    programs[1].p_type = PT_LOAD;
    programs[1].p_offset = 0x1000;
    programs[1].p_vaddr = 0x1000;
    programs[1].p_filesz = LONG_NEEDED_FIXTURE_SIZE - 0x1000;
    programs[1].p_memsz = programs[1].p_filesz;
    programs[1].p_flags = PF_R | PF_W;
    programs[1].p_align = 0x1000;
    programs[2].p_type = PT_DYNAMIC;
    programs[2].p_offset = LONG_NEEDED_DYNAMIC_VMA;
    programs[2].p_vaddr = LONG_NEEDED_DYNAMIC_VMA;
    programs[2].p_filesz = (Elf32_Word)dynamic_size;
    programs[2].p_memsz = (Elf32_Word)dynamic_size;
    programs[2].p_flags = PF_R | PF_W;
    programs[2].p_align = 4;
    write32(data + 0x800, UINT32_C(0xe12fff1e));
    dynamic = (Elf32_Dyn *)(data + LONG_NEEDED_DYNAMIC_VMA);
    dynamic[0].d_tag = DT_STRTAB;
    dynamic[0].d_un.d_val = LONG_NEEDED_STRING_VMA;
    dynamic[1].d_tag = DT_STRSZ;
    dynamic[1].d_un.d_val = (Elf32_Word)(name_length + 2);
    dynamic[2].d_tag = DT_SYMTAB;
    dynamic[2].d_un.d_val = LONG_NEEDED_SYMBOL_VMA;
    dynamic[3].d_tag = DT_SYMENT;
    dynamic[3].d_un.d_val = sizeof(Elf32_Sym);
    dynamic[4].d_tag = DT_HASH;
    dynamic[4].d_un.d_val = LONG_NEEDED_HASH_VMA;
    for (index = 0; index < needed_count; ++index) {
      size_t offset = 1 + index;
      if (duplicate_last && index == needed_count - 1)
        offset = 1;
      dynamic[5 + index].d_tag = DT_NEEDED;
      dynamic[5 + index].d_un.d_val = (Elf32_Word)offset;
    }
    dynamic[5 + needed_count].d_tag = DT_NULL;
  }
  return data;
}

static uint64_t many_segment_target_vma(size_t index, int relro_gap) {
  uint64_t vma = MANY_SEGMENT_TARGET_VMA +
                 (uint64_t)index * MANY_SEGMENT_STRIDE;
  if (relro_gap && index >= MANY_SEGMENT_COUNT / 2)
    vma += MANY_SEGMENT_STRIDE;
  return vma;
}

static unsigned char *build_many_segment_fixture(nxloader_arch arch,
                                                 int relro_gap) {
  unsigned char *data;
  size_t load_index;
  size_t program_count = MANY_SEGMENT_COUNT + 3;
  uint32_t *hash;
  uint64_t final_target;
  if (arch != NXLOADER_ARCH_AARCH64 && arch != NXLOADER_ARCH_ARMV7)
    return NULL;
  if (program_count > UINT16_MAX)
    return NULL;
  data = (unsigned char *)calloc(1, MANY_SEGMENT_FIXTURE_SIZE);
  if (!data)
    return NULL;
  hash = (uint32_t *)(data + MANY_SEGMENT_HASH_VMA);
  hash[0] = 1;
  hash[1] = 1;
  hash[2] = 0;
  hash[3] = 0;
  data[MANY_SEGMENT_STRING_VMA] = '\0';
  final_target = many_segment_target_vma(MANY_SEGMENT_COUNT - 1, relro_gap);

  if (arch == NXLOADER_ARCH_AARCH64) {
    Elf64_Ehdr *header = (Elf64_Ehdr *)data;
    Elf64_Phdr *programs;
    Elf64_Dyn *dynamic;
    Elf64_Rela *relocations;
    size_t dynamic_index = 0;
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_AARCH64;
    header->e_version = EV_CURRENT;
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = (Elf64_Half)program_count;
    programs = (Elf64_Phdr *)(data + header->e_phoff);
    programs[0].p_type = PT_LOAD;
    programs[0].p_filesz = MANY_SEGMENT_FIXTURE_SIZE;
    programs[0].p_memsz = MANY_SEGMENT_FIXTURE_SIZE;
    programs[0].p_flags = PF_R | PF_X;
    programs[0].p_align = 0x1000;
    for (load_index = 0; load_index < MANY_SEGMENT_COUNT; ++load_index) {
      Elf64_Phdr *program = &programs[1 + load_index];
      uint64_t target_vma = many_segment_target_vma(load_index, relro_gap);
      program->p_type = PT_LOAD;
      program->p_offset = target_vma & UINT64_C(0xfff);
      program->p_vaddr = target_vma;
      program->p_memsz = 8;
      program->p_flags = PF_R | PF_W;
      program->p_align = 1;
    }
    programs[1 + MANY_SEGMENT_COUNT].p_type = PT_DYNAMIC;
    programs[1 + MANY_SEGMENT_COUNT].p_offset = MANY_SEGMENT_DYNAMIC_VMA;
    programs[1 + MANY_SEGMENT_COUNT].p_vaddr = MANY_SEGMENT_DYNAMIC_VMA;
    programs[1 + MANY_SEGMENT_COUNT].p_filesz = 9 * sizeof(Elf64_Dyn);
    programs[1 + MANY_SEGMENT_COUNT].p_memsz = 9 * sizeof(Elf64_Dyn);
    programs[1 + MANY_SEGMENT_COUNT].p_flags = PF_R;
    programs[1 + MANY_SEGMENT_COUNT].p_align = 8;
    programs[2 + MANY_SEGMENT_COUNT].p_type = PT_GNU_RELRO;
    programs[2 + MANY_SEGMENT_COUNT].p_vaddr = MANY_SEGMENT_TARGET_VMA;
    programs[2 + MANY_SEGMENT_COUNT].p_memsz =
        final_target + 8 - MANY_SEGMENT_TARGET_VMA;
    programs[2 + MANY_SEGMENT_COUNT].p_flags = PF_R;
    programs[2 + MANY_SEGMENT_COUNT].p_align = 0x1000;
    write32(data + 0x800, UINT32_C(0xd65f03c0));
    relocations = (Elf64_Rela *)(data + MANY_SEGMENT_RELOCATION_VMA);
    for (load_index = 0; load_index < MANY_SEGMENT_COUNT; ++load_index) {
      relocations[load_index].r_offset =
          many_segment_target_vma(load_index, relro_gap);
      relocations[load_index].r_info =
          ELF64_R_INFO(0, R_AARCH64_RELATIVE);
      relocations[load_index].r_addend = 0;
    }
    dynamic = (Elf64_Dyn *)(data + MANY_SEGMENT_DYNAMIC_VMA);
#define MANY_SEGMENT_DYN64(tag_value, dynamic_value)                        \
    do {                                                                     \
      dynamic[dynamic_index].d_tag = (tag_value);                            \
      dynamic[dynamic_index].d_un.d_val = (dynamic_value);                   \
      dynamic_index++;                                                       \
    } while (0)
    MANY_SEGMENT_DYN64(DT_STRTAB, MANY_SEGMENT_STRING_VMA);
    MANY_SEGMENT_DYN64(DT_STRSZ, 1);
    MANY_SEGMENT_DYN64(DT_SYMTAB, MANY_SEGMENT_SYMBOL_VMA);
    MANY_SEGMENT_DYN64(DT_SYMENT, sizeof(Elf64_Sym));
    MANY_SEGMENT_DYN64(DT_HASH, MANY_SEGMENT_HASH_VMA);
    MANY_SEGMENT_DYN64(DT_RELA, MANY_SEGMENT_RELOCATION_VMA);
    MANY_SEGMENT_DYN64(DT_RELASZ,
                       MANY_SEGMENT_COUNT * sizeof(Elf64_Rela));
    MANY_SEGMENT_DYN64(DT_RELAENT, sizeof(Elf64_Rela));
    MANY_SEGMENT_DYN64(DT_NULL, 0);
#undef MANY_SEGMENT_DYN64
  } else {
    Elf32_Ehdr *header = (Elf32_Ehdr *)data;
    Elf32_Phdr *programs;
    Elf32_Dyn *dynamic;
    Elf32_Rel *relocations;
    size_t dynamic_index = 0;
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS32;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_ARM;
    header->e_version = EV_CURRENT;
    header->e_flags = UINT32_C(0x05000000);
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(Elf32_Phdr);
    header->e_phnum = (Elf32_Half)program_count;
    programs = (Elf32_Phdr *)(data + header->e_phoff);
    programs[0].p_type = PT_LOAD;
    programs[0].p_filesz = MANY_SEGMENT_FIXTURE_SIZE;
    programs[0].p_memsz = MANY_SEGMENT_FIXTURE_SIZE;
    programs[0].p_flags = PF_R | PF_X;
    programs[0].p_align = 0x1000;
    for (load_index = 0; load_index < MANY_SEGMENT_COUNT; ++load_index) {
      Elf32_Phdr *program = &programs[1 + load_index];
      uint64_t target_vma = many_segment_target_vma(load_index, relro_gap);
      program->p_type = PT_LOAD;
      program->p_offset = (Elf32_Off)(target_vma & UINT64_C(0xfff));
      program->p_vaddr = (Elf32_Addr)target_vma;
      program->p_memsz = 8;
      program->p_flags = PF_R | PF_W;
      program->p_align = 1;
    }
    programs[1 + MANY_SEGMENT_COUNT].p_type = PT_DYNAMIC;
    programs[1 + MANY_SEGMENT_COUNT].p_offset = MANY_SEGMENT_DYNAMIC_VMA;
    programs[1 + MANY_SEGMENT_COUNT].p_vaddr = MANY_SEGMENT_DYNAMIC_VMA;
    programs[1 + MANY_SEGMENT_COUNT].p_filesz = 9 * sizeof(Elf32_Dyn);
    programs[1 + MANY_SEGMENT_COUNT].p_memsz = 9 * sizeof(Elf32_Dyn);
    programs[1 + MANY_SEGMENT_COUNT].p_flags = PF_R;
    programs[1 + MANY_SEGMENT_COUNT].p_align = 4;
    programs[2 + MANY_SEGMENT_COUNT].p_type = PT_GNU_RELRO;
    programs[2 + MANY_SEGMENT_COUNT].p_vaddr = MANY_SEGMENT_TARGET_VMA;
    programs[2 + MANY_SEGMENT_COUNT].p_memsz =
        (Elf32_Word)(final_target + 8 - MANY_SEGMENT_TARGET_VMA);
    programs[2 + MANY_SEGMENT_COUNT].p_flags = PF_R;
    programs[2 + MANY_SEGMENT_COUNT].p_align = 0x1000;
    write32(data + 0x800, UINT32_C(0xe12fff1e));
    relocations = (Elf32_Rel *)(data + MANY_SEGMENT_RELOCATION_VMA);
    for (load_index = 0; load_index < MANY_SEGMENT_COUNT; ++load_index) {
      relocations[load_index].r_offset =
          (Elf32_Addr)many_segment_target_vma(load_index, relro_gap);
      relocations[load_index].r_info = ELF32_R_INFO(0, R_ARM_RELATIVE);
    }
    dynamic = (Elf32_Dyn *)(data + MANY_SEGMENT_DYNAMIC_VMA);
#define MANY_SEGMENT_DYN32(tag_value, dynamic_value)                        \
    do {                                                                     \
      dynamic[dynamic_index].d_tag = (tag_value);                            \
      dynamic[dynamic_index].d_un.d_val = (dynamic_value);                   \
      dynamic_index++;                                                       \
    } while (0)
    MANY_SEGMENT_DYN32(DT_STRTAB, MANY_SEGMENT_STRING_VMA);
    MANY_SEGMENT_DYN32(DT_STRSZ, 1);
    MANY_SEGMENT_DYN32(DT_SYMTAB, MANY_SEGMENT_SYMBOL_VMA);
    MANY_SEGMENT_DYN32(DT_SYMENT, sizeof(Elf32_Sym));
    MANY_SEGMENT_DYN32(DT_HASH, MANY_SEGMENT_HASH_VMA);
    MANY_SEGMENT_DYN32(DT_REL, MANY_SEGMENT_RELOCATION_VMA);
    MANY_SEGMENT_DYN32(DT_RELSZ,
                       MANY_SEGMENT_COUNT * sizeof(Elf32_Rel));
    MANY_SEGMENT_DYN32(DT_RELENT, sizeof(Elf32_Rel));
    MANY_SEGMENT_DYN32(DT_NULL, 0);
#undef MANY_SEGMENT_DYN32
  }
  return data;
}

static unsigned char *build_arm_branch_fixture(uint32_t type,
                                               uint32_t symbol_index,
                                               uint32_t target_vma,
                                               uint32_t instruction) {
  unsigned char *data = build_elf32(R_ARM_RELATIVE);
  Elf32_Rel *relocations;
  Elf32_Dyn *rel_size;
  if (!data)
    return NULL;
  relocations = (Elf32_Rel *)(data + RELOCATION_VMA);
  relocations[0].r_offset = target_vma;
  relocations[0].r_info = ELF32_R_INFO(symbol_index, type);
  rel_size = find_dynamic32(data, DT_RELSZ);
  if (!rel_size) {
    free(data);
    return NULL;
  }
  rel_size->d_un.d_val = sizeof(*relocations);
  if (type == R_ARM_THM_CALL) {
    write16(data + target_vma, (uint16_t)instruction);
    write16(data + target_vma + 2, (uint16_t)(instruction >> 16));
  } else {
    write32(data + target_vma, instruction);
  }
  append_dynamic32(data, DT_TEXTREL, 0);
  return data;
}

static uint32_t arm_far_address(uint32_t place, int thumb) {
  uint32_t address = place < UINT32_C(0x80000000)
                         ? place + UINT32_C(0x08000000)
                         : place - UINT32_C(0x08000000);
  address &= ~UINT32_C(3);
  return address | (thumb ? 1u : 0u);
}

static const char *test_alias(void *userdata, const nxloader_module *module,
                              const char *name) {
  (void)userdata;
  (void)module;
  return strcmp(name, "host") == 0 ? "host_alias" : NULL;
}

static nxloader_reloc_action skip_unknown(void *userdata,
                                          const nxloader_module *module,
                                          const nxloader_reloc_info *info,
                                          uint64_t *value) {
  (void)userdata;
  (void)module;
  (void)value;
  unknown_relocation_hook_calls++;
  return info->type == 999 ? NXLOADER_RELOC_SKIP
                           : NXLOADER_RELOC_USE_DEFAULT;
}

static nxloader_reloc_action skip_every_relocation(
    void *userdata, const nxloader_module *module,
    const nxloader_reloc_info *info, uint64_t *value) {
  (void)userdata;
  (void)module;
  (void)info;
  (void)value;
  return NXLOADER_RELOC_SKIP;
}

static nxloader_reloc_action count_and_skip_relocation(
    void *userdata, const nxloader_module *module,
    const nxloader_reloc_info *info, uint64_t *value) {
  (void)userdata;
  (void)module;
  (void)info;
  (void)value;
  unknown_relocation_hook_calls++;
  return NXLOADER_RELOC_SKIP;
}

static nxloader_reloc_action skip_host_import(
    void *userdata, const nxloader_module *module,
    const nxloader_reloc_info *info, uint64_t *value) {
  (void)userdata;
  (void)module;
  (void)value;
  if (info->phase == NXLOADER_RELOC_PHASE_IMPORT && info->symbol &&
      strcmp(info->symbol, "host") == 0)
    return NXLOADER_RELOC_SKIP;
  return NXLOADER_RELOC_USE_DEFAULT;
}

static nxloader_reloc_action write_host_import(
    void *userdata, const nxloader_module *module,
    const nxloader_reloc_info *info, uint64_t *value) {
  (void)userdata;
  (void)module;
  if (info->phase == NXLOADER_RELOC_PHASE_IMPORT && info->symbol &&
      strcmp(info->symbol, "host") == 0) {
    *value = UINT64_C(0x51a7c0de);
    return NXLOADER_RELOC_WRITE;
  }
  return NXLOADER_RELOC_USE_DEFAULT;
}

enum m11_initializer_mode {
  M11_INITIALIZER_SKIP_ALL = 0,
  M11_INITIALIZER_RUN_THEN_REJECT = 1
};

typedef struct m11_callback_context {
  nxloader_module *self;
  nxloader_module *other;
  size_t callback_calls;
  size_t log_reenter_after;
  size_t probe_failures;
  nxloader_state observed_state;
  nxloader_arch observed_arch;
  nxloader_result observed_info_result;
  nxloader_result observed_other_info_result;
  size_t initializer_count;
  nxloader_initializer_kind initializer_kinds[4];
  size_t initializer_indexes[4];
  uintptr_t initializer_addresses[4];
  enum m11_initializer_mode initializer_mode;
  size_t jni_alias_calls;
} m11_callback_context;

static void m11_probe_same_module_reentrancy(m11_callback_context *context) {
  nxloader_module_info info;
  nxloader_registry *registry = NULL;
  uintptr_t address = 0;
  uintptr_t table = 0;
  size_t count = 0;
  if (!context || !context->self) {
    if (context)
      context->probe_failures++;
    return;
  }
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  context->observed_info_result =
      nxloader_module_get_info(context->self, &info);
  if (context->observed_info_result != NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_get_info(context->self, NULL) != NXLOADER_EREENTRANT)
    context->probe_failures++;
  context->observed_state = nxloader_module_get_state(context->self);
  if (context->observed_state != NXLOADER_STATE_ERROR)
    context->probe_failures++;
  context->observed_arch = nxloader_module_get_arch(context->self);
  if (context->observed_arch != NXLOADER_ARCH_AUTO)
    context->probe_failures++;
  if (nxloader_module_vma_to_pointer(context->self, 0, 0) != NULL)
    context->probe_failures++;
  if (nxloader_module_load_memory(context->self, NULL, 0, NULL) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_load_file(context->self, NULL) != NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_relocate(context->self) != NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_resolve(context->self, NULL, 0, NULL) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_finalize(context->self) != NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_begin_patch(context->self) != NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_call_initializers(context->self) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_call_jni_onload(context->self, NULL, NULL) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_find_export(context->self, NULL, NULL) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_find_relocation(context->self, NULL, NULL) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_needed_count(context->self) != 0)
    context->probe_failures++;
  if (nxloader_module_needed(context->self, SIZE_MAX) != NULL)
    context->probe_failures++;
  if (nxloader_module_soname(context->self) != NULL)
    context->probe_failures++;
  if (nxloader_module_find_arm_exidx(context->self, 0, &table, &count) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_module_install_hook(context->self, 0, 0, 0) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
  if (nxloader_registry_create(&registry) != NXLOADER_OK || !registry)
    context->probe_failures++;
  else if (nxloader_registry_add_module(registry, context->self,
                                        "m11-reentrant", 0, NULL) !=
           NXLOADER_EREENTRANT)
    context->probe_failures++;
  nxloader_registry_destroy(registry);
  if (context->other) {
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    context->observed_other_info_result =
        nxloader_module_get_info(context->other, &info);
    if (context->observed_other_info_result != NXLOADER_OK ||
        info.state != NXLOADER_STATE_EMPTY)
      context->probe_failures++;
  }
  nxloader_module_destroy(context->self);
  if (nxloader_module_find_export(context->self, "local", &address) !=
      NXLOADER_EREENTRANT)
    context->probe_failures++;
}

static void m11_reentrant_log(void *userdata, nxloader_log_level level,
                              const char *message) {
  m11_callback_context *context = (m11_callback_context *)userdata;
  (void)level;
  (void)message;
  context->callback_calls++;
  if (context->callback_calls > context->log_reenter_after)
    m11_probe_same_module_reentrancy(context);
}

static const char *m11_reentrant_alias(void *userdata,
                                       const nxloader_module *module,
                                       const char *name) {
  m11_callback_context *context = (m11_callback_context *)userdata;
  if (module != context->self)
    context->probe_failures++;
  context->callback_calls++;
  m11_probe_same_module_reentrancy(context);
  return strcmp(name, "host") == 0 ? "host_alias" : NULL;
}

static nxloader_reloc_action m11_reentrant_relocation(
    void *userdata, const nxloader_module *module,
    const nxloader_reloc_info *info, uint64_t *value) {
  m11_callback_context *context = (m11_callback_context *)userdata;
  (void)info;
  (void)value;
  if (module != context->self)
    context->probe_failures++;
  context->callback_calls++;
  m11_probe_same_module_reentrancy(context);
  return NXLOADER_RELOC_USE_DEFAULT;
}

static nxloader_initializer_action m11_reentrant_initializer(
    void *userdata, const nxloader_module *module,
    const nxloader_initializer_info *initializer) {
  m11_callback_context *context = (m11_callback_context *)userdata;
  (void)initializer;
  if (module != context->self)
    context->probe_failures++;
  context->callback_calls++;
  m11_probe_same_module_reentrancy(context);
  return NXLOADER_INITIALIZER_SKIP;
}

static nxloader_initializer_action m11_record_initializer(
    void *userdata, const nxloader_module *module,
    const nxloader_initializer_info *initializer) {
  m11_callback_context *context = (m11_callback_context *)userdata;
  size_t record = context->initializer_count;
  if (!initializer) {
    context->probe_failures++;
    return NXLOADER_INITIALIZER_REJECT;
  }
  if (module != context->self ||
      initializer->struct_size < sizeof(*initializer))
    context->probe_failures++;
  if (record < sizeof(context->initializer_kinds) /
                   sizeof(context->initializer_kinds[0])) {
    context->initializer_kinds[record] = initializer->kind;
    context->initializer_indexes[record] = initializer->index;
    context->initializer_addresses[record] = initializer->address;
  }
  context->initializer_count++;
  if (context->initializer_mode == M11_INITIALIZER_RUN_THEN_REJECT)
    return record == 0 ? NXLOADER_INITIALIZER_RUN
                       : NXLOADER_INITIALIZER_REJECT;
  return NXLOADER_INITIALIZER_SKIP;
}

static const char *m11_jni_alias_observer(void *userdata,
                                          const nxloader_module *module,
                                          const char *name) {
  m11_callback_context *context = (m11_callback_context *)userdata;
  if (module != context->self)
    context->probe_failures++;
  if (strcmp(name, "JNI_OnLoad") == 0) {
    context->jni_alias_calls++;
    return "local";
  }
  return NULL;
}

static nxloader_module *new_m11_module(
    nxloader_arch arch, m11_callback_context *context, nxloader_log_fn log,
    nxloader_alias_fn alias, nxloader_reloc_hook_fn relocation_hook,
    nxloader_initializer_filter_fn initializer_filter) {
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_config_init(&config);
  config.expected_arch = arch;
  config.flags = NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH;
  config.max_file_size = 8u * 1024u * 1024u;
  config.max_image_size = 8u * 1024u * 1024u;
  config.trampoline_pool_size = 4096;
  config.log = log;
  config.alias = alias;
  config.relocation_hook = relocation_hook;
  config.initializer_filter = initializer_filter;
  config.userdata = context;
  CHECK(nxloader_module_create(&config, &module) == NXLOADER_OK);
  if (context)
    context->self = module;
  return module;
}

static nxloader_module *new_module_with_limits(
    nxloader_arch arch, nxloader_reloc_hook_fn hook, uint32_t extra_flags,
    size_t trampoline_pool_size, size_t max_image_size) {
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_config_init(&config);
  config.expected_arch = arch;
  config.flags = NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH | extra_flags;
  config.max_file_size = 8u * 1024u * 1024u;
  config.max_image_size = max_image_size;
  config.trampoline_pool_size = trampoline_pool_size;
  config.alias = test_alias;
  config.relocation_hook = hook;
  CHECK(nxloader_module_create(&config, &module) == NXLOADER_OK);
  return module;
}

static nxloader_module *new_module_with_options(
    nxloader_arch arch, nxloader_reloc_hook_fn hook, uint32_t extra_flags,
    size_t trampoline_pool_size) {
  return new_module_with_limits(arch, hook, extra_flags, trampoline_pool_size,
                                8u * 1024u * 1024u);
}

static nxloader_module *new_module(nxloader_arch arch,
                                   nxloader_reloc_hook_fn hook) {
  return new_module_with_options(arch, hook, 0, 4096);
}

static nxloader_result try_load(const unsigned char *fixture,
                                nxloader_arch arch, const char *name) {
  nxloader_module *module = new_module(arch, NULL);
  nxloader_result result = nxloader_module_load_memory(
      module, fixture, FIXTURE_SIZE, name);
  nxloader_module_destroy(module);
  return result;
}

static void expect_load64(unsigned char *fixture, nxloader_result expected,
                          const char *name) {
  CHECK(fixture != NULL);
  if (fixture)
    CHECK(try_load(fixture, NXLOADER_ARCH_AARCH64, name) == expected);
  free(fixture);
}

static void expect_load32(unsigned char *fixture, nxloader_result expected,
                          const char *name) {
  CHECK(fixture != NULL);
  if (fixture)
    CHECK(try_load(fixture, NXLOADER_ARCH_ARMV7, name) == expected);
  free(fixture);
}

static nxloader_registry *new_registry_with_symbol(const char *name,
                                                   uintptr_t address) {
  nxloader_registry *registry = NULL;
  nxloader_symbol symbol;
  nxloader_provider provider;
  memset(&symbol, 0, sizeof(symbol));
  memset(&provider, 0, sizeof(provider));
  symbol.name = name;
  symbol.address = address;
  provider.struct_size = sizeof(provider);
  provider.name = "host-shims";
  provider.symbols = &symbol;
  provider.symbol_count = 1;
  provider.priority = 100;
  CHECK(nxloader_registry_create(&registry) == NXLOADER_OK);
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  return registry;
}

static nxloader_registry *new_registry_with_host(uintptr_t address) {
  return new_registry_with_symbol("host_alias", address);
}

static unsigned char *build_jump_slot_sentinel_fixture(nxloader_arch arch) {
  unsigned char *fixture;
  if (arch == NXLOADER_ARCH_AARCH64) {
    Elf64_Rela *relocations;
    fixture = build_elf64(R_AARCH64_RELATIVE);
    if (!fixture)
      return NULL;
    relocations = (Elf64_Rela *)(fixture + RELOCATION_VMA);
    relocations[2].r_info = ELF64_R_INFO(2, R_AARCH64_JUMP_SLOT);
    /* Undefined weak RELA resolves to its signed addend (A). */
    relocations[3].r_addend = 42;
    write64(fixture + SLOT_IMPORT64, UINT64_C(0x15250));
    write64(fixture + SLOT_WEAK64, UINT64_C(0x15250));
    return fixture;
  }
  fixture = build_elf32(R_ARM_RELATIVE);
  if (fixture) {
    Elf32_Rel *relocations = (Elf32_Rel *)(fixture + RELOCATION_VMA);
    relocations[2].r_info = ELF32_R_INFO(2, R_ARM_JUMP_SLOT);
    write32(fixture + SLOT_IMPORT32, UINT32_C(0x15250));
    write32(fixture + SLOT_WEAK32, UINT32_C(0x15250));
  }
  return fixture;
}

static uint64_t read_jump_slot(const nxloader_module *module,
                               nxloader_arch arch, uint64_t vma) {
  size_t width = arch == NXLOADER_ARCH_AARCH64 ? sizeof(uint64_t)
                                               : sizeof(uint32_t);
  const void *slot = nxloader_module_vma_to_pointer(module, vma, width);
  CHECK(slot != NULL);
  if (!slot)
    return UINT64_MAX;
  return arch == NXLOADER_ARCH_AARCH64 ? read64(slot) : read32(slot);
}

static void test_missing_strong_jump_slot_contract_for_arch(
    nxloader_arch arch) {
  const uint64_t import_vma = arch == NXLOADER_ARCH_AARCH64
                                  ? SLOT_IMPORT64
                                  : SLOT_IMPORT32;
  const uint64_t weak_vma = arch == NXLOADER_ARCH_AARCH64 ? SLOT_WEAK64
                                                          : SLOT_WEAK32;
  const uint64_t host_address = arch == NXLOADER_ARCH_AARCH64
                                    ? (uint64_t)TEST_HOST64
                                    : UINT64_C(0x12345000);
  const uint64_t weak_default = arch == NXLOADER_ARCH_AARCH64 ? 42u : 0u;
  unsigned char *fixture;
  nxloader_module *module;
  nxloader_registry *empty;
  nxloader_registry *provider;
  nxloader_resolution_report report;

  /* A hook cannot turn an absent strong JUMP_SLOT into success with SKIP.
   * The whole phase remains atomic, then a provider-backed retry succeeds;
   * because that retry has a checked default, SKIP is still permitted. */
  fixture = build_jump_slot_sentinel_fixture(arch);
  module = new_module(arch, skip_host_import);
  empty = NULL;
  provider = NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "jump-slot-skip-retry") == NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    CHECK(nxloader_module_resolve(module, empty, 0, &report) ==
          NXLOADER_EUNRESOLVED);
    CHECK(report.unresolved_strong == 1 && report.imports_resolved == 0 &&
          report.weak_imports_zeroed == 1);
    CHECK(report.first_unresolved != NULL &&
          strcmp(report.first_unresolved, "host") == 0);
    CHECK(read_jump_slot(module, arch, import_vma) == UINT64_C(0x15250));
    CHECK(read_jump_slot(module, arch, weak_vma) == UINT64_C(0x15250));

    provider = new_registry_with_host((uintptr_t)host_address);
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    CHECK(nxloader_module_resolve(module, provider, 0, &report) ==
          NXLOADER_OK);
    CHECK(report.unresolved_strong == 0 && report.imports_resolved == 0 &&
          report.weak_imports_zeroed == 1);
    CHECK(read_jump_slot(module, arch, import_vma) == UINT64_C(0x15250));
    CHECK(read_jump_slot(module, arch, weak_vma) == weak_default);
  }
  nxloader_registry_destroy(provider);
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(module);
  free(fixture);

  /* WRITE is an explicit adapter resolution and therefore replaces PLT0 even
   * when the registry has no provider. Weak keeps its ABI default (A/zero). */
  fixture = build_jump_slot_sentinel_fixture(arch);
  module = new_module(arch, write_host_import);
  empty = NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "jump-slot-explicit-write") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    CHECK(nxloader_module_resolve(module, empty, 0, &report) == NXLOADER_OK);
    CHECK(report.unresolved_strong == 0 && report.imports_resolved == 1 &&
          report.weak_imports_zeroed == 1);
    CHECK(read_jump_slot(module, arch, import_vma) ==
          UINT64_C(0x51a7c0de));
    CHECK(read_jump_slot(module, arch, weak_vma) == weak_default);
  }
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(module);
  free(fixture);

  /* The diagnostic opt-in remains explicit: it publishes the weak default
   * but deliberately leaves the unresolved strong PLT0 sentinel untouched. */
  fixture = build_jump_slot_sentinel_fixture(arch);
  module = new_module(arch, skip_host_import);
  empty = NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "jump-slot-allow-unresolved") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    CHECK(nxloader_module_resolve(module, empty,
                                  NXLOADER_RESOLVE_ALLOW_UNRESOLVED,
                                  &report) == NXLOADER_OK);
    CHECK(report.unresolved_strong == 1 && report.imports_resolved == 0 &&
          report.weak_imports_zeroed == 1);
    CHECK(read_jump_slot(module, arch, import_vma) == UINT64_C(0x15250));
    CHECK(read_jump_slot(module, arch, weak_vma) == weak_default);
    CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
    /* V4-GRAPHICS-03 fail-closed: the diagnostic opt-in inspects, it does not
     * authorize execution. A constructor must never run over a GOT/PLT slot
     * that still holds its raw link-time value -- that is precisely how the
     * OTR 1.0.3 guest reached eglGetCurrentContext@plt and died. */
    CHECK(nxloader_module_call_initializers(module) == NXLOADER_EUNRESOLVED);
    CHECK(read_jump_slot(module, arch, import_vma) == UINT64_C(0x15250));
  }
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(module);
  free(fixture);

  /* The ordinary provider/default path still overwrites the sentinel. */
  fixture = build_jump_slot_sentinel_fixture(arch);
  module = new_module(arch, NULL);
  provider = NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "jump-slot-provider-default") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    provider = new_registry_with_host((uintptr_t)host_address);
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    CHECK(nxloader_module_resolve(module, provider, 0, &report) ==
          NXLOADER_OK);
    CHECK(report.unresolved_strong == 0 && report.imports_resolved == 1 &&
          report.weak_imports_zeroed == 1);
    CHECK(read_jump_slot(module, arch, import_vma) == host_address);
    CHECK(read_jump_slot(module, arch, weak_vma) == weak_default);
  }
  nxloader_registry_destroy(provider);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_missing_strong_jump_slot_contract(void) {
  test_missing_strong_jump_slot_contract_for_arch(NXLOADER_ARCH_ARMV7);
  test_missing_strong_jump_slot_contract_for_arch(NXLOADER_ARCH_AARCH64);
}

static uintptr_t m11_host_address(nxloader_arch arch) {
  return arch == NXLOADER_ARCH_ARMV7 ? (uintptr_t)UINT32_C(0x12345000)
                                     : TEST_HOST64;
}

static nxloader_registry *m11_finalize_module(nxloader_module *module,
                                              unsigned char *fixture,
                                              nxloader_arch arch,
                                              const char *name) {
  nxloader_registry *registry =
      new_registry_with_symbol("host", m11_host_address(arch));
  CHECK(module != NULL && fixture != NULL && registry != NULL);
  if (!module || !fixture || !registry)
    return registry;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) == NXLOADER_OK);
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  return registry;
}

static void test_public_api_contract(void) {
  nxloader_config config;
  nxloader_module *module = (nxloader_module *)(uintptr_t)1;
  CHECK(NXLOADER_API_VERSION_MAJOR == 1u);
  CHECK(NXLOADER_API_VERSION_MINOR == 3u);
  CHECK(strcmp(NXLOADER_VERSION_STRING, "0.9.0") == 0);
  CHECK(NXLOADER_EREENTRANT == -15);
  CHECK(NXLOADER_STATE_ERROR == 6);
  CHECK(NXLOADER_STATE_INITIALIZING == 7);
  CHECK(NXLOADER_STATE_JNI_LOADING == 8);
  CHECK(NXLOADER_STATE_READY == 9);
  CHECK(NXLOADER_STATE_PATCHING == 10);
  CHECK(strcmp(nxloader_state_string(NXLOADER_STATE_PATCHING), "patching") ==
        0);
  CHECK(nxloader_module_begin_patch(NULL) == NXLOADER_EINVAL);
  CHECK(NXLOADER_JNI_ONLOAD_MAX_ACCEPTED_VERSIONS == 16u);
  CHECK(strcmp(nxloader_arm_float_abi_string(
                   NXLOADER_ARM_FLOAT_ABI_NOT_APPLICABLE),
               "not-applicable") == 0);
  CHECK(strcmp(nxloader_arm_float_abi_string(
                   NXLOADER_ARM_FLOAT_ABI_UNSPECIFIED),
               "unspecified") == 0);
  CHECK(strcmp(nxloader_arm_float_abi_string(NXLOADER_ARM_FLOAT_ABI_SOFT),
               "soft") == 0);
  CHECK(strcmp(nxloader_arm_float_abi_string(NXLOADER_ARM_FLOAT_ABI_HARD),
               "hard") == 0);
  CHECK(strcmp(nxloader_result_string(NXLOADER_ECALLBACK),
               "callback rejected operation") == 0);
  CHECK(strcmp(nxloader_result_string(NXLOADER_EREENTRANT),
               "reentrant nxloader callback operation") == 0);
  CHECK(strcmp(nxloader_state_string(NXLOADER_STATE_INITIALIZING),
               "initializing") == 0);
  CHECK(strcmp(nxloader_state_string(NXLOADER_STATE_JNI_LOADING),
               "jni-loading") == 0);
  CHECK(strcmp(nxloader_state_string(NXLOADER_STATE_READY), "ready") == 0);
  nxloader_config_init(&config);
  config.flags = UINT32_C(0x80000000);
  CHECK(nxloader_module_create(&config, &module) == NXLOADER_EINVAL);
  CHECK(module == NULL);
  nxloader_config_init(&config);
  config.api_version++;
  module = (nxloader_module *)(uintptr_t)1;
  CHECK(nxloader_module_create(&config, &module) == NXLOADER_EINVAL);
  CHECK(module == NULL);
  nxloader_config_init(&config);
  config.struct_size--;
  module = (nxloader_module *)(uintptr_t)1;
  CHECK(nxloader_module_create(&config, &module) == NXLOADER_EINVAL);
  CHECK(module == NULL);
}

static void test_m11_log_reentrancy_guard(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  m11_callback_context context;
  nxloader_module *module;
  nxloader_module *other = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module_info info;
  memset(&context, 0, sizeof(context));
  context.other = other;
  module = new_m11_module(NXLOADER_ARCH_AARCH64, &context,
                          m11_reentrant_log, NULL, NULL, NULL);
  CHECK(fixture != NULL && module != NULL && other != NULL);
  if (fixture && module) {
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "m11-log-reentrant") ==
          NXLOADER_EREENTRANT);
    CHECK(context.callback_calls == 1);
    CHECK(context.probe_failures == 0);
    CHECK(context.observed_info_result == NXLOADER_EREENTRANT);
    CHECK(context.observed_other_info_result == NXLOADER_OK);
    CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_ERROR);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
    CHECK(info.mapping_base == NULL && info.mapping_size == 0);
  }
  nxloader_module_destroy(module);
  nxloader_module_destroy(other);
  free(fixture);

  fixture = build_elf64(R_AARCH64_RELATIVE);
  memset(&context, 0, sizeof(context));
  other = new_module(NXLOADER_ARCH_AARCH64, NULL);
  context.other = other;
  context.log_reenter_after = 1;
  module = new_m11_module(NXLOADER_ARCH_AARCH64, &context,
                          m11_reentrant_log, NULL, NULL, NULL);
  CHECK(fixture != NULL && module != NULL && other != NULL);
  if (fixture && module) {
    nxloader_registry *empty = NULL;
    nxloader_resolution_report report;
    nxloader_resolution_report before;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "m11-error-log-reentrant") ==
          NXLOADER_OK);
    CHECK(context.callback_calls == 1);
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
    memset(&report, 0x5a, sizeof(report));
    report.struct_size = sizeof(report);
    before = report;
    CHECK(nxloader_module_resolve(module, empty, 0, &report) ==
          NXLOADER_EREENTRANT);
    CHECK(memcmp(&report, &before, sizeof(report)) == 0);
    CHECK(context.callback_calls == 2);
    CHECK(context.probe_failures == 0);
    CHECK(context.observed_other_info_result == NXLOADER_OK);
    CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_RELOCATED);
    nxloader_registry_destroy(empty);
  }
  nxloader_module_destroy(module);
  nxloader_module_destroy(other);
  free(fixture);
}

static void test_m11_relocation_and_alias_reentrancy(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  size_t pass;
  for (pass = 0; pass < sizeof(arches) / sizeof(arches[0]); ++pass) {
    nxloader_arch arch = arches[pass];
    unsigned char *fixture =
        arch == NXLOADER_ARCH_AARCH64
            ? build_elf64(R_AARCH64_RELATIVE)
            : build_elf32(R_ARM_RELATIVE);
    m11_callback_context context;
    nxloader_module *other = new_module(arch, NULL);
    nxloader_module *module;
    void *target;
    unsigned char before[8];
    size_t width = arch == NXLOADER_ARCH_AARCH64 ? 8u : 4u;
    memset(&context, 0, sizeof(context));
    context.other = other;
    module = new_m11_module(arch, &context, NULL, NULL,
                            m11_reentrant_relocation, NULL);
    CHECK(fixture != NULL && module != NULL && other != NULL);
    if (fixture && module) {
      CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                        "m11-reloc-reentrant") == NXLOADER_OK);
      target = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, width);
      CHECK(target != NULL);
      if (target)
        memcpy(before, target, width);
      CHECK(nxloader_module_relocate(module) == NXLOADER_EREENTRANT);
      CHECK(context.callback_calls == 1);
      CHECK(context.probe_failures == 0);
      CHECK(context.observed_other_info_result == NXLOADER_OK);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_LOADED);
      if (target)
        CHECK(memcmp(before, target, width) == 0);
    }
    nxloader_module_destroy(module);
    nxloader_module_destroy(other);
    free(fixture);

    fixture = arch == NXLOADER_ARCH_AARCH64
                  ? build_elf64(R_AARCH64_RELATIVE)
                  : build_elf32(R_ARM_RELATIVE);
    memset(&context, 0, sizeof(context));
    other = new_module(arch, NULL);
    context.other = other;
    module = new_m11_module(arch, &context, NULL, m11_reentrant_alias, NULL,
                            NULL);
    CHECK(fixture != NULL && module != NULL && other != NULL);
    if (fixture && module) {
      nxloader_registry *registry =
          new_registry_with_host(m11_host_address(arch));
      uint64_t before_slot = 0;
      target = NULL;
      CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                        "m11-alias-reentrant") == NXLOADER_OK);
      CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
      target = nxloader_module_vma_to_pointer(
          module, arch == NXLOADER_ARCH_AARCH64 ? SLOT_IMPORT64 : SLOT_IMPORT32,
          width);
      CHECK(target != NULL);
      if (target)
        memcpy(&before_slot, target, width);
      CHECK(nxloader_module_resolve(module, registry, 0, NULL) ==
            NXLOADER_EREENTRANT);
      CHECK(context.callback_calls == 1);
      CHECK(context.probe_failures == 0);
      CHECK(context.observed_other_info_result == NXLOADER_OK);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_RELOCATED);
      if (target)
        CHECK(memcmp(&before_slot, target, width) == 0);
      nxloader_registry_destroy(registry);
    }
    nxloader_module_destroy(module);
    nxloader_module_destroy(other);
    free(fixture);
  }
}

static void test_m11_initializer_preflight_and_reentrancy(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  size_t pass;
  for (pass = 0; pass < sizeof(arches) / sizeof(arches[0]); ++pass) {
    nxloader_arch arch = arches[pass];
    unsigned char *fixture =
        arch == NXLOADER_ARCH_AARCH64
            ? build_elf64(R_AARCH64_RELATIVE)
            : build_elf32(R_ARM_RELATIVE);
    m11_callback_context context;
    nxloader_module *module;
    nxloader_registry *registry;
    memset(&context, 0, sizeof(context));
    if (fixture) {
      if (arch == NXLOADER_ARCH_AARCH64)
        append_dynamic64(fixture, DT_INIT, LOCAL_VMA);
      else
        append_dynamic32(fixture, DT_INIT, LOCAL_VMA);
    }
    module = new_m11_module(arch, &context, NULL, NULL, NULL,
                            m11_record_initializer);
    registry = m11_finalize_module(module, fixture, arch,
                                   "m11-initializer-order");
    if (module && fixture && registry) {
      CHECK(nxloader_module_call_initializers(module) == NXLOADER_OK);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_INITIALIZED);
      CHECK(context.probe_failures == 0);
      CHECK(context.initializer_count == 2);
      CHECK(context.initializer_kinds[0] == NXLOADER_INITIALIZER_DT_INIT);
      CHECK(context.initializer_indexes[0] == SIZE_MAX);
      CHECK(context.initializer_kinds[1] == NXLOADER_INITIALIZER_INIT_ARRAY);
      CHECK(context.initializer_indexes[1] == 0);
      CHECK(context.initializer_addresses[0] != 0);
      CHECK(context.initializer_addresses[1] != 0);
      CHECK(nxloader_module_call_initializers(module) == NXLOADER_ESTATE);
      CHECK(context.initializer_count == 2);
    }
    nxloader_registry_destroy(registry);
    nxloader_module_destroy(module);
    free(fixture);

    fixture = arch == NXLOADER_ARCH_AARCH64
                  ? build_elf64(R_AARCH64_RELATIVE)
                  : build_elf32(R_ARM_RELATIVE);
    memset(&context, 0, sizeof(context));
    context.initializer_mode = M11_INITIALIZER_RUN_THEN_REJECT;
    if (fixture) {
      if (arch == NXLOADER_ARCH_AARCH64)
        append_dynamic64(fixture, DT_INIT, LOCAL_VMA);
      else
        append_dynamic32(fixture, DT_INIT, LOCAL_VMA);
    }
    module = new_m11_module(arch, &context, NULL, NULL, NULL,
                            m11_record_initializer);
    registry = m11_finalize_module(module, fixture, arch,
                                   "m11-initializer-late-reject");
    if (module && fixture && registry) {
      CHECK(nxloader_module_call_initializers(module) == NXLOADER_ECALLBACK);
      CHECK(context.initializer_count == 2);
      CHECK(context.initializer_kinds[0] == NXLOADER_INITIALIZER_DT_INIT);
      CHECK(context.initializer_kinds[1] == NXLOADER_INITIALIZER_INIT_ARRAY);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_ERROR);
    }
    nxloader_registry_destroy(registry);
    nxloader_module_destroy(module);
    free(fixture);

    fixture = arch == NXLOADER_ARCH_AARCH64
                  ? build_elf64(R_AARCH64_RELATIVE)
                  : build_elf32(R_ARM_RELATIVE);
    memset(&context, 0, sizeof(context));
    context.other = new_module(arch, NULL);
    if (fixture) {
      if (arch == NXLOADER_ARCH_AARCH64)
        append_dynamic64(fixture, DT_INIT, LOCAL_VMA);
      else
        append_dynamic32(fixture, DT_INIT, LOCAL_VMA);
    }
    module = new_m11_module(arch, &context, NULL, NULL, NULL,
                            m11_reentrant_initializer);
    registry = m11_finalize_module(module, fixture, arch,
                                   "m11-initializer-reentrant");
    if (module && fixture && registry) {
      CHECK(nxloader_module_call_initializers(module) ==
            NXLOADER_EREENTRANT);
      CHECK(context.callback_calls == 1);
      CHECK(context.probe_failures == 0);
      CHECK(context.observed_other_info_result == NXLOADER_OK);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_ERROR);
    }
    nxloader_registry_destroy(registry);
    nxloader_module_destroy(module);
    nxloader_module_destroy(context.other);
    free(fixture);
  }
}

static void test_m11_jni_onload_contract(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  static const int32_t accepted_versions[] = {INT32_C(0x00010006),
                                               INT32_C(0x00010008)};
  size_t pass;
  for (pass = 0; pass < sizeof(arches) / sizeof(arches[0]); ++pass) {
    nxloader_arch arch = arches[pass];
    unsigned char *fixture =
        arch == NXLOADER_ARCH_AARCH64
            ? build_elf64(R_AARCH64_RELATIVE)
            : build_elf32(R_ARM_RELATIVE);
    m11_callback_context context;
    nxloader_module *module;
    nxloader_registry *registry;
    nxloader_jni_onload_options options;
    int32_t out_version = -1;
    memset(&context, 0, sizeof(context));
    module = new_m11_module(arch, &context, NULL, m11_jni_alias_observer, NULL,
                            m11_record_initializer);
    registry = m11_finalize_module(module, fixture, arch,
                                   "m11-jni-optional");
    if (module && fixture && registry) {
      CHECK(nxloader_module_call_initializers(module) == NXLOADER_OK);
      memset(&options, 0, sizeof(options));
      options.struct_size = sizeof(options);
      options.java_vm = (void *)(uintptr_t)1;
      options.accepted_versions = accepted_versions;
      options.accepted_version_count =
          sizeof(accepted_versions) / sizeof(accepted_versions[0]);
      options.flags = NXLOADER_JNI_ONLOAD_OPTIONAL;
      CHECK(nxloader_module_call_jni_onload(module, &options, &out_version) ==
            NXLOADER_OK);
      CHECK(out_version == 0);
      CHECK(context.jni_alias_calls == 0);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_READY);
      CHECK(nxloader_module_call_jni_onload(module, &options, &out_version) ==
            NXLOADER_ESTATE);
    }
    nxloader_registry_destroy(registry);
    nxloader_module_destroy(module);
    free(fixture);

    fixture = arch == NXLOADER_ARCH_AARCH64
                  ? build_elf64(R_AARCH64_RELATIVE)
                  : build_elf32(R_ARM_RELATIVE);
    memset(&context, 0, sizeof(context));
    module = new_m11_module(arch, &context, NULL, m11_jni_alias_observer, NULL,
                            m11_record_initializer);
    registry = m11_finalize_module(module, fixture, arch,
                                   "m11-jni-required-missing");
    if (module && fixture && registry) {
      CHECK(nxloader_module_call_initializers(module) == NXLOADER_OK);
      memset(&options, 0, sizeof(options));
      options.struct_size = sizeof(options);
      options.java_vm = (void *)(uintptr_t)1;
      options.accepted_versions = accepted_versions;
      options.accepted_version_count =
          sizeof(accepted_versions) / sizeof(accepted_versions[0]);
      out_version = -2;
      CHECK(nxloader_module_call_jni_onload(module, &options, &out_version) ==
            NXLOADER_EUNRESOLVED);
      CHECK(out_version == -2);
      CHECK(context.jni_alias_calls == 0);
      CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_ERROR);
    }
    nxloader_registry_destroy(registry);
    nxloader_module_destroy(module);
    free(fixture);
  }
}

static void test_m11_jni_onload_option_validation(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  static const int32_t valid_versions[] = {INT32_C(0x00010006),
                                           INT32_C(0x00010008)};
  int32_t many_versions[NXLOADER_JNI_ONLOAD_MAX_ACCEPTED_VERSIONS + 1u];
  int32_t invalid_versions[2];
  m11_callback_context context;
  nxloader_module *module;
  nxloader_registry *registry;
  nxloader_jni_onload_options options;
  int32_t output = -77;
  size_t index;
  memset(&context, 0, sizeof(context));
  module = new_m11_module(NXLOADER_ARCH_AARCH64, &context, NULL,
                          m11_jni_alias_observer, NULL,
                          m11_record_initializer);
  registry = m11_finalize_module(module, fixture, NXLOADER_ARCH_AARCH64,
                                 "m11-jni-options");
  CHECK(module != NULL && fixture != NULL && registry != NULL);
  if (!module || !fixture || !registry)
    goto cleanup;
  CHECK(nxloader_module_call_initializers(module) == NXLOADER_OK);
  memset(&options, 0, sizeof(options));
  options.struct_size = sizeof(options);
  options.java_vm = (void *)(uintptr_t)1;
  options.accepted_versions = valid_versions;
  options.accepted_version_count =
      sizeof(valid_versions) / sizeof(valid_versions[0]);
  options.flags = NXLOADER_JNI_ONLOAD_OPTIONAL;

  options.struct_size--;
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  options.struct_size = sizeof(options);
  options.java_vm = NULL;
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  options.java_vm = (void *)(uintptr_t)1;
  options.reserved = (void *)(uintptr_t)1;
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  options.reserved = NULL;
  options.accepted_versions = NULL;
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  options.accepted_versions = valid_versions;
  options.accepted_version_count = 0;
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  for (index = 0;
       index < sizeof(many_versions) / sizeof(many_versions[0]); ++index)
    many_versions[index] = (int32_t)(index + 1);
  options.accepted_versions = many_versions;
  options.accepted_version_count =
      sizeof(many_versions) / sizeof(many_versions[0]);
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  invalid_versions[0] = 0;
  invalid_versions[1] = INT32_C(0x00010006);
  options.accepted_versions = invalid_versions;
  options.accepted_version_count = 2;
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  invalid_versions[0] = INT32_C(0x00010006);
  invalid_versions[1] = INT32_C(0x00010006);
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  options.accepted_versions = valid_versions;
  options.flags = UINT32_C(0x80000000);
  CHECK(nxloader_module_call_jni_onload(module, &options, &output) ==
        NXLOADER_EINVAL);
  CHECK(output == -77);
  CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_INITIALIZED);
  CHECK(context.jni_alias_calls == 0);

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static void expect_arm_float_abi(uint32_t float_flags,
                                 nxloader_result expected_result,
                                 nxloader_arm_float_abi expected_abi,
                                 const char *name) {
  unsigned char *fixture = build_elf32(R_ARM_RELATIVE);
  Elf32_Ehdr *header = (Elf32_Ehdr *)fixture;
  nxloader_module *module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  nxloader_module_info info;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module) {
    nxloader_module_destroy(module);
    free(fixture);
    return;
  }
  header->e_flags = UINT32_C(0x05000000) | float_flags;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        expected_result);
  if (expected_result == NXLOADER_OK) {
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
    CHECK(info.elf_flags == header->e_flags);
    CHECK(info.arm_float_abi == expected_abi);
  }
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_arm_float_abi_contract(void) {
  expect_arm_float_abi(0, NXLOADER_OK,
                       NXLOADER_ARM_FLOAT_ABI_UNSPECIFIED,
                       "arm-float-unspecified");
  expect_arm_float_abi(EF_ARM_ABI_FLOAT_SOFT, NXLOADER_OK,
                       NXLOADER_ARM_FLOAT_ABI_SOFT, "arm-float-soft");
  expect_arm_float_abi(EF_ARM_ABI_FLOAT_HARD, NXLOADER_OK,
                       NXLOADER_ARM_FLOAT_ABI_HARD, "arm-float-hard");
  expect_arm_float_abi(EF_ARM_ABI_FLOAT_SOFT | EF_ARM_ABI_FLOAT_HARD,
                       NXLOADER_EFORMAT,
                       NXLOADER_ARM_FLOAT_ABI_NOT_APPLICABLE,
                       "arm-float-conflicting");
}

static void test_arm_data_relocations(void) {
  unsigned char *abs_fixture = build_elf32(R_ARM_RELATIVE);
  unsigned char *rel_fixture = build_elf32(R_ARM_RELATIVE);
  unsigned char *rel_negative_fixture = build_elf32(R_ARM_RELATIVE);
  nxloader_module *abs_module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  nxloader_module *rel_module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  nxloader_module *rel_negative_module =
      new_module(NXLOADER_ARCH_ARMV7, NULL);
  Elf32_Rel *relocations;
  nxloader_module_info info;
  uint64_t bias;
  uint32_t expected;

  CHECK(abs_fixture != NULL && rel_fixture != NULL &&
        rel_negative_fixture != NULL && abs_module != NULL &&
        rel_module != NULL && rel_negative_module != NULL);
  if (!abs_fixture || !rel_fixture || !rel_negative_fixture || !abs_module ||
      !rel_module || !rel_negative_module)
    goto cleanup;

  write32(abs_fixture + SLOT_ABSOLUTE32, UINT32_C(0xfffffffc));
  CHECK(nxloader_module_load_memory(abs_module, abs_fixture, FIXTURE_SIZE,
                                    "arm-abs32-negative-addend") ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(abs_module, &info) == NXLOADER_OK);
  bias = (uintptr_t)info.mapping_base - info.minimum_vma;
  CHECK(nxloader_module_relocate(abs_module) == NXLOADER_OK);
  expected = (uint32_t)(bias + LOCAL_VMA - 4u);
  CHECK(read32(nxloader_module_vma_to_pointer(abs_module, SLOT_ABSOLUTE32, 4)) ==
        expected);

  relocations = (Elf32_Rel *)(rel_fixture + RELOCATION_VMA);
  relocations[1].r_info = ELF32_R_INFO(1, R_ARM_REL32);
  write32(rel_fixture + SLOT_ABSOLUTE32, 12);
  CHECK(nxloader_module_load_memory(rel_module, rel_fixture, FIXTURE_SIZE,
                                    "arm-rel32-s-plus-a-minus-p") ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(rel_module, &info) == NXLOADER_OK);
  bias = (uintptr_t)info.mapping_base - info.minimum_vma;
  CHECK(nxloader_module_relocate(rel_module) == NXLOADER_OK);
  expected = (uint32_t)((bias + LOCAL_VMA) + 12u -
                        (bias + SLOT_ABSOLUTE32));
  CHECK(read32(nxloader_module_vma_to_pointer(rel_module, SLOT_ABSOLUTE32, 4)) ==
        expected);
  CHECK(expected == UINT32_C(0xffffee08));

  relocations = (Elf32_Rel *)(rel_negative_fixture + RELOCATION_VMA);
  relocations[1].r_info = ELF32_R_INFO(1, R_ARM_REL32);
  write32(rel_negative_fixture + SLOT_ABSOLUTE32, UINT32_C(0xfffffffc));
  CHECK(nxloader_module_load_memory(rel_negative_module, rel_negative_fixture,
                                    FIXTURE_SIZE,
                                    "arm-rel32-negative-addend") ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(rel_negative_module, &info) == NXLOADER_OK);
  bias = (uintptr_t)info.mapping_base - info.minimum_vma;
  CHECK(nxloader_module_relocate(rel_negative_module) == NXLOADER_OK);
  expected = (uint32_t)((bias + LOCAL_VMA) - 4u -
                        (bias + SLOT_ABSOLUTE32));
  CHECK(expected == UINT32_C(0xffffedf8));
  CHECK(read32(nxloader_module_vma_to_pointer(
                   rel_negative_module, SLOT_ABSOLUTE32, 4)) == expected);

cleanup:
  nxloader_module_destroy(rel_negative_module);
  nxloader_module_destroy(rel_module);
  nxloader_module_destroy(abs_module);
  free(rel_negative_fixture);
  free(rel_fixture);
  free(abs_fixture);
}

static void test_arm_weak_rel32_uses_addend(void) {
  unsigned char *fixture = build_elf32(R_ARM_RELATIVE);
  unsigned char *atomic_fixture = build_elf32(R_ARM_RELATIVE);
  nxloader_module *module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  nxloader_module *atomic_module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  nxloader_registry *empty = NULL;
  Elf32_Rel *relocations;
  Elf32_Dyn *rel_size;
  nxloader_resolution_report report;
  void *target;
  void *atomic_rel32;
  void *atomic_weak;
  void *atomic_strong;
  uint32_t legacy_zero_symbol_value;

  CHECK(fixture != NULL && atomic_fixture != NULL && module != NULL &&
        atomic_module != NULL);
  if (!fixture || !atomic_fixture || !module || !atomic_module)
    goto cleanup;
  CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);

  relocations = (Elf32_Rel *)(fixture + RELOCATION_VMA);
  rel_size = find_dynamic32(fixture, DT_RELSZ);
  CHECK(rel_size != NULL);
  if (!rel_size)
    goto cleanup;
  relocations[0].r_offset = SLOT_ABSOLUTE32;
  relocations[0].r_info = ELF32_R_INFO(3, R_ARM_REL32);
  rel_size->d_un.d_val = sizeof(*relocations);
  write32(fixture + SLOT_ABSOLUTE32, UINT32_C(0xfffffffc));
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "arm-weak-rel32-addend") == NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE32, 4);
  CHECK(target != NULL && read32(target) == UINT32_C(0xfffffffc));
  if (!target)
    goto cleanup;
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(module, empty, 0, &report) == NXLOADER_OK);
  CHECK(report.weak_imports_zeroed == 1);
  CHECK(report.imports_resolved == 0 && report.unresolved_strong == 0);
  CHECK(read32(target) == UINT32_C(0xfffffffc));
  legacy_zero_symbol_value =
      UINT32_C(0xfffffffc) - (uint32_t)(uintptr_t)target;
  CHECK(read32(target) != legacy_zero_symbol_value);

  relocations = (Elf32_Rel *)(atomic_fixture + RELOCATION_VMA);
  rel_size = find_dynamic32(atomic_fixture, DT_RELSZ);
  CHECK(rel_size != NULL);
  if (!rel_size)
    goto cleanup;
  relocations[0].r_offset = SLOT_ABSOLUTE32;
  relocations[0].r_info = ELF32_R_INFO(3, R_ARM_REL32);
  relocations[1].r_offset = SLOT_IMPORT32;
  relocations[1].r_info = ELF32_R_INFO(3, R_ARM_GLOB_DAT);
  relocations[2].r_offset = SLOT_WEAK32;
  relocations[2].r_info = ELF32_R_INFO(2, R_ARM_GLOB_DAT);
  rel_size->d_un.d_val = 3 * sizeof(*relocations);
  write32(atomic_fixture + SLOT_ABSOLUTE32, UINT32_C(0xfffffffc));
  write32(atomic_fixture + SLOT_IMPORT32, UINT32_C(0xa5a5a5a5));
  write32(atomic_fixture + SLOT_WEAK32, UINT32_C(0x5a5a5a5a));
  CHECK(nxloader_module_load_memory(atomic_module, atomic_fixture, FIXTURE_SIZE,
                                    "arm-weak-rel32-atomic") == NXLOADER_OK);
  atomic_rel32 =
      nxloader_module_vma_to_pointer(atomic_module, SLOT_ABSOLUTE32, 4);
  atomic_weak =
      nxloader_module_vma_to_pointer(atomic_module, SLOT_IMPORT32, 4);
  atomic_strong =
      nxloader_module_vma_to_pointer(atomic_module, SLOT_WEAK32, 4);
  CHECK(atomic_rel32 != NULL && atomic_weak != NULL && atomic_strong != NULL);
  if (!atomic_rel32 || !atomic_weak || !atomic_strong)
    goto cleanup;
  CHECK(nxloader_module_relocate(atomic_module) == NXLOADER_OK);
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(atomic_module, empty, 0, &report) ==
        NXLOADER_EUNRESOLVED);
  CHECK(report.weak_imports_zeroed == 2 && report.unresolved_strong == 1);
  CHECK(read32(atomic_rel32) == UINT32_C(0xfffffffc));
  CHECK(read32(atomic_weak) == UINT32_C(0xa5a5a5a5));
  CHECK(read32(atomic_strong) == UINT32_C(0x5a5a5a5a));

cleanup:
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(atomic_module);
  nxloader_module_destroy(module);
  free(atomic_fixture);
  free(fixture);
}

static void expect_local_arm_branch(uint32_t type, uint32_t target_vma,
                                    uint32_t original, int target_thumb,
                                    uint32_t expected, const char *name) {
  unsigned char *fixture =
      build_arm_branch_fixture(type, 1, target_vma, original);
  Elf32_Sym *symbols = fixture ? (Elf32_Sym *)(fixture + SYMBOL_VMA) : NULL;
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 16);
  void *target;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module) {
    nxloader_module_destroy(module);
    free(fixture);
    return;
  }
  symbols[1].st_value = LOCAL_VMA | (target_thumb ? 1u : 0u);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, target_vma, sizeof(uint32_t));
  CHECK(target != NULL && read32(target) == original);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  CHECK(read32(target) == expected);
  nxloader_module_destroy(module);
  free(fixture);
}

static void expect_invalid_local_arm_branch(uint32_t type,
                                            uint32_t target_vma,
                                            uint32_t instruction,
                                            nxloader_result expected,
                                            const char *name) {
  unsigned char *fixture =
      build_arm_branch_fixture(type, 1, target_vma, instruction);
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 0);
  void *target;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module) {
    nxloader_module_destroy(module);
    free(fixture);
    return;
  }
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, target_vma, sizeof(uint32_t));
  CHECK(target != NULL && read32(target) == instruction);
  CHECK(nxloader_module_relocate(module) == expected);
  CHECK(read32(target) == instruction);
  nxloader_module_destroy(module);
  free(fixture);
}

static int64_t decode_arm_branch_displacement(uint32_t instruction) {
  uint32_t encoded = (instruction & UINT32_C(0x00ffffff)) << 2;
  return (encoded & UINT32_C(0x02000000))
             ? (int64_t)encoded - INT64_C(0x04000000)
             : (int64_t)encoded;
}

static int64_t decode_aarch64_branch_displacement(uint32_t instruction) {
  uint32_t encoded = (instruction & UINT32_C(0x03ffffff)) << 2;
  return (encoded & UINT32_C(0x08000000))
             ? (int64_t)encoded - INT64_C(0x10000000)
             : (int64_t)encoded;
}

static int64_t decode_thumb_branch_displacement(uint32_t instruction) {
  uint16_t high = (uint16_t)(instruction & UINT32_C(0xffff));
  uint16_t low = (uint16_t)(instruction >> 16);
  uint32_t sign = (high >> 10) & 1u;
  uint32_t j1 = (low >> 13) & 1u;
  uint32_t j2 = (low >> 11) & 1u;
  uint32_t i1 = (~(j1 ^ sign)) & 1u;
  uint32_t i2 = (~(j2 ^ sign)) & 1u;
  uint32_t encoded = (sign << 24) | (i1 << 23) | (i2 << 22) |
                     ((uint32_t)(high & UINT16_C(0x03ff)) << 12) |
                     ((uint32_t)(low & UINT16_C(0x07ff)) << 1);
  return (encoded & UINT32_C(0x01000000))
             ? (int64_t)encoded - INT64_C(0x02000000)
             : (int64_t)encoded;
}

static void test_arm_branch_textrel_and_codecs(void) {
  unsigned char *denied = build_arm_branch_fixture(
      R_ARM_CALL, 1, ARM_BRANCH_VMA, UINT32_C(0xebfffffe));
  nxloader_module *denied_module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  unsigned char *data_target = build_arm_branch_fixture(
      R_ARM_CALL, 1, SLOT_RELATIVE, UINT32_C(0xebfffffe));
  nxloader_module *data_target_module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 16);
  CHECK(denied != NULL && denied_module != NULL && data_target != NULL &&
        data_target_module != NULL);
  if (denied && denied_module)
    CHECK(nxloader_module_load_memory(denied_module, denied, FIXTURE_SIZE,
                                      "arm-textrel-default-denied") ==
          NXLOADER_EUNSUPPORTED);
  if (data_target && data_target_module) {
    CHECK(nxloader_module_load_memory(data_target_module, data_target,
                                      FIXTURE_SIZE,
                                      "arm-textrel-data-target") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(data_target_module) == NXLOADER_EFORMAT);
  }
  nxloader_module_destroy(data_target_module);
  nxloader_module_destroy(denied_module);
  free(data_target);
  free(denied);

  expect_local_arm_branch(R_ARM_CALL, ARM_BRANCH_VMA,
                          UINT32_C(0xebfffffe), 0,
                          UINT32_C(0xebffff7e), "arm-call-arm");
  expect_local_arm_branch(R_ARM_JUMP24, ARM_BRANCH_VMA,
                          UINT32_C(0xeafffffe), 0,
                          UINT32_C(0xeaffff7e), "arm-jump24-arm");
  expect_local_arm_branch(R_ARM_JUMP24, ARM_BRANCH_VMA,
                          UINT32_C(0x1bfffffe), 0,
                          UINT32_C(0x1bffff7e),
                          "arm-jump24-conditional-link");
  expect_local_arm_branch(R_ARM_CALL, ARM_BRANCH_VMA,
                          UINT32_C(0xebfffffe), 1,
                          UINT32_C(0xfaffff7e), "arm-call-thumb-interwork");
  expect_local_arm_branch(R_ARM_THM_CALL, ARM_BRANCH_VMA,
                          UINT32_C(0xfffef7ff), 1,
                          UINT32_C(0xfefef7ff), "thumb-call-thumb");
  expect_local_arm_branch(R_ARM_THM_CALL, ARM_BRANCH_VMA,
                          UINT32_C(0xfffef7ff), 0,
                          UINT32_C(0xeefef7ff), "thumb-call-arm-interwork");
  expect_local_arm_branch(R_ARM_THM_CALL, ARM_BRANCH_VMA + 2,
                          UINT32_C(0xfffef7ff), 1,
                          UINT32_C(0xfefdf7ff), "thumb-call-halfword-aligned");

  expect_invalid_local_arm_branch(R_ARM_CALL, ARM_BRANCH_VMA,
                                  UINT32_C(0xe1a00000), NXLOADER_EFORMAT,
                                  "arm-call-invalid-opcode");
  expect_invalid_local_arm_branch(R_ARM_JUMP24, ARM_BRANCH_VMA,
                                  UINT32_C(0xebfffffe), NXLOADER_EFORMAT,
                                  "arm-jump24-link-opcode");
  expect_invalid_local_arm_branch(R_ARM_THM_CALL, ARM_BRANCH_VMA,
                                  UINT32_C(0xbf00bf00), NXLOADER_EFORMAT,
                                  "thumb-call-invalid-opcode");
  expect_invalid_local_arm_branch(R_ARM_CALL, ARM_BRANCH_VMA + 2,
                                  UINT32_C(0xebfffffe), NXLOADER_EFORMAT,
                                  "arm-call-misaligned");
  expect_invalid_local_arm_branch(R_ARM_THM_CALL, ARM_BRANCH_VMA + 1,
                                  UINT32_C(0xfffef7ff), NXLOADER_EFORMAT,
                                  "thumb-call-misaligned");
}

static void expect_weak_arm_branch_fallthrough(
    uint32_t type, uint32_t original, uint32_t expected,
    int64_t expected_displacement, const char *name) {
  unsigned char *fixture = build_arm_branch_fixture(
      type, 3, ARM_BRANCH_VMA, original);
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 8);
  nxloader_registry *empty = NULL;
  nxloader_resolution_report report;
  void *target;
  int64_t displacement;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, ARM_BRANCH_VMA, 4);
  CHECK(target != NULL && read32(target) == original);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(module, empty, 0, &report) == NXLOADER_OK);
  CHECK(report.weak_imports_zeroed == 1);
  CHECK(report.imports_resolved == 0 && report.unresolved_strong == 0);
  CHECK(read32(target) == expected);
  displacement = type == R_ARM_THM_CALL
                     ? decode_thumb_branch_displacement(read32(target))
                     : decode_arm_branch_displacement(read32(target));
  CHECK(displacement == expected_displacement);
  CHECK((uintptr_t)((int64_t)(uintptr_t)target +
                    (type == R_ARM_THM_CALL ? 4 : 8) + displacement) ==
        (uintptr_t)target + 4);

cleanup:
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_arm_weak_branch_fallthrough(void) {
  expect_weak_arm_branch_fallthrough(
      R_ARM_CALL, UINT32_C(0xebfffffe), UINT32_C(0xebffffff), -4,
      "arm-call-weak-fallthrough");
  expect_weak_arm_branch_fallthrough(
      R_ARM_THM_CALL, UINT32_C(0xfffef7ff), UINT32_C(0xf800f000), 0,
      "thumb-call-weak-fallthrough");
}

static void expect_far_arm_branch_without_pool(uint32_t type,
                                               uint32_t instruction,
                                               int target_thumb,
                                               const char *name) {
  unsigned char *fixture = build_arm_branch_fixture(
      type, 2, ARM_BRANCH_VMA, instruction);
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 0);
  nxloader_registry *registry = NULL;
  void *target;
  uint32_t far_address;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, ARM_BRANCH_VMA, 4);
  CHECK(target != NULL && read32(target) == instruction);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  far_address = arm_far_address((uint32_t)(uintptr_t)target, target_thumb);
  registry = new_registry_with_host(far_address);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) ==
        NXLOADER_EOVERFLOW);
  CHECK(read32(target) == instruction);

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_arm_branch_range_and_veneers(void) {
  unsigned char *arm_fixture = build_arm_branch_fixture(
      R_ARM_CALL, 2, ARM_BRANCH_VMA, UINT32_C(0xebfffffe));
  unsigned char *thumb_fixture = build_arm_branch_fixture(
      R_ARM_THM_CALL, 2, ARM_BRANCH_VMA, UINT32_C(0xfffef7ff));
  nxloader_module *arm_module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 8);
  nxloader_module *thumb_module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 8);
  nxloader_registry *arm_registry = NULL;
  nxloader_registry *thumb_registry = NULL;
  nxloader_module_info info;
  void *target;
  uint8_t *veneer;
  uint32_t far_address;
  uint32_t instruction;
  int64_t displacement;
  char permissions[5] = {0};

  expect_far_arm_branch_without_pool(R_ARM_CALL, UINT32_C(0xebfffffe), 0,
                                     "arm-call-range-no-pool");
  expect_far_arm_branch_without_pool(R_ARM_THM_CALL, UINT32_C(0xfffef7ff), 1,
                                     "thumb-call-range-no-pool");

  CHECK(arm_fixture != NULL && arm_module != NULL);
  if (arm_fixture && arm_module) {
    CHECK(nxloader_module_load_memory(arm_module, arm_fixture, FIXTURE_SIZE,
                                      "arm-call-far-veneer") == NXLOADER_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK(nxloader_module_get_info(arm_module, &info) == NXLOADER_OK);
    target = nxloader_module_vma_to_pointer(arm_module, ARM_BRANCH_VMA, 4);
    veneer = (uint8_t *)info.mapping_base + info.image_size;
    far_address = arm_far_address((uint32_t)(uintptr_t)target, 0);
    arm_registry = new_registry_with_host(far_address);
    CHECK(nxloader_module_relocate(arm_module) == NXLOADER_OK);
    CHECK(nxloader_module_resolve(arm_module, arm_registry, 0, NULL) ==
          NXLOADER_OK);
    CHECK(read32(veneer) == UINT32_C(0xe51ff004));
    CHECK(read32(veneer + 4) == far_address);
    instruction = read32(target);
    CHECK((instruction & UINT32_C(0xff000000)) == UINT32_C(0xeb000000));
    displacement = decode_arm_branch_displacement(instruction);
    CHECK((uintptr_t)((int64_t)(uintptr_t)target + 8 + displacement) ==
          (uintptr_t)veneer);
    CHECK(mapping_permissions(veneer, permissions) &&
          strcmp(permissions, "rw-p") == 0);
    CHECK(nxloader_module_finalize(arm_module) == NXLOADER_OK);
    memset(permissions, 0, sizeof(permissions));
    CHECK(mapping_permissions(veneer, permissions) &&
          strcmp(permissions, "r-xp") == 0);
    CHECK(strchr(permissions, 'w') == NULL);
  }

  CHECK(thumb_fixture != NULL && thumb_module != NULL);
  if (thumb_fixture && thumb_module) {
    CHECK(nxloader_module_load_memory(thumb_module, thumb_fixture, FIXTURE_SIZE,
                                      "thumb-call-far-veneer") ==
          NXLOADER_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK(nxloader_module_get_info(thumb_module, &info) == NXLOADER_OK);
    target = nxloader_module_vma_to_pointer(thumb_module, ARM_BRANCH_VMA, 4);
    veneer = (uint8_t *)info.mapping_base + info.image_size;
    far_address = arm_far_address((uint32_t)(uintptr_t)target, 1);
    thumb_registry = new_registry_with_host(far_address);
    CHECK(nxloader_module_relocate(thumb_module) == NXLOADER_OK);
    CHECK(nxloader_module_resolve(thumb_module, thumb_registry, 0, NULL) ==
          NXLOADER_OK);
    CHECK(read16(veneer) == UINT16_C(0xf8df));
    CHECK(read16(veneer + 2) == UINT16_C(0xf000));
    CHECK(read32(veneer + 4) == far_address);
    instruction = read32(target);
    CHECK((instruction & UINT32_C(0x0000f800)) == UINT32_C(0x0000f000));
    CHECK((instruction & UINT32_C(0xc0000000)) == UINT32_C(0xc0000000));
    displacement = decode_thumb_branch_displacement(instruction);
    CHECK((uintptr_t)((int64_t)(uintptr_t)target + 4 + displacement) ==
          (uintptr_t)veneer);
    memset(permissions, 0, sizeof(permissions));
    CHECK(mapping_permissions(veneer, permissions) &&
          strcmp(permissions, "rw-p") == 0);
    CHECK(nxloader_module_finalize(thumb_module) == NXLOADER_OK);
    memset(permissions, 0, sizeof(permissions));
    CHECK(mapping_permissions(veneer, permissions) &&
          strcmp(permissions, "r-xp") == 0);
    CHECK(strchr(permissions, 'w') == NULL);
  }

  nxloader_registry_destroy(thumb_registry);
  nxloader_registry_destroy(arm_registry);
  nxloader_module_destroy(thumb_module);
  nxloader_module_destroy(arm_module);
  free(thumb_fixture);
  free(arm_fixture);
}

static void test_arm_veneer_capacity_is_atomic(void) {
  unsigned char *fixture = build_arm_branch_fixture(
      R_ARM_JUMP24, 1, ARM_BRANCH_VMA, UINT32_C(0xeafffffe));
  Elf32_Rel *relocations =
      fixture ? (Elf32_Rel *)(fixture + RELOCATION_VMA) : NULL;
  Elf32_Sym *symbols = fixture ? (Elf32_Sym *)(fixture + SYMBOL_VMA) : NULL;
  Elf32_Dyn *rel_size = fixture ? find_dynamic32(fixture, DT_RELSZ) : NULL;
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 8);
  nxloader_module_info info;
  void *first;
  void *second;
  uint8_t *pool;
  CHECK(fixture != NULL && rel_size != NULL && module != NULL);
  if (!fixture || !rel_size || !module)
    goto cleanup;
  symbols[1].st_value = LOCAL_VMA | 1u;
  relocations[1].r_offset = ARM_BRANCH2_VMA;
  relocations[1].r_info = ELF32_R_INFO(1, R_ARM_JUMP24);
  write32(fixture + ARM_BRANCH2_VMA, UINT32_C(0xeafffffe));
  rel_size->d_un.d_val = 2 * sizeof(*relocations);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "arm-veneer-capacity-atomic") ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  first = nxloader_module_vma_to_pointer(module, ARM_BRANCH_VMA, 4);
  second = nxloader_module_vma_to_pointer(module, ARM_BRANCH2_VMA, 4);
  pool = (uint8_t *)info.mapping_base + info.image_size;
  CHECK(bytes_are_zero(pool, 8));
  CHECK(nxloader_module_relocate(module) == NXLOADER_EOVERFLOW);
  CHECK(read32(first) == UINT32_C(0xeafffffe));
  CHECK(read32(second) == UINT32_C(0xeafffffe));
  CHECK(bytes_are_zero(pool, 8));

cleanup:
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_thumb_veneer_pool_must_be_reachable(void) {
  unsigned char *fixture = build_arm_branch_fixture(
      R_ARM_THM_CALL, 2, ARM_BRANCH_VMA, UINT32_C(0xfffef7ff));
  Elf32_Ehdr *header = fixture ? (Elf32_Ehdr *)fixture : NULL;
  Elf32_Phdr *programs =
      fixture ? (Elf32_Phdr *)(fixture + header->e_phoff) : NULL;
  nxloader_module *module = new_module_with_limits(
      NXLOADER_ARCH_ARMV7, NULL, NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS, 8,
      64u * 1024u * 1024u);
  nxloader_registry *registry = NULL;
  nxloader_module_info info;
  void *target;
  uint8_t *pool;
  uint32_t far_address;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  /* A large final BSS models the TASM-class image where a single suffix pool
   * is real and allocated, but outside THM_CALL's signed 25-bit reach. */
  programs[1].p_memsz = UINT32_C(0x02000000);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "thumb-unreachable-existing-pool") ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, ARM_BRANCH_VMA, 4);
  pool = (uint8_t *)info.mapping_base + info.image_size;
  CHECK((uintptr_t)pool > (uintptr_t)target);
  CHECK((uintptr_t)pool - (uintptr_t)target > UINT32_C(0x01000000));
  CHECK(bytes_are_zero(pool, 8));
  CHECK(read32(target) == UINT32_C(0xfffef7ff));
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  far_address = arm_far_address((uint32_t)(uintptr_t)target, 1);
  registry = new_registry_with_host(far_address);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) ==
        NXLOADER_EOVERFLOW);
  CHECK(read32(target) == UINT32_C(0xfffef7ff));
  CHECK(bytes_are_zero(pool, 8));

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_diagnostics_do_not_leak_mapping(void) {
  unsigned char *fixture = build_elf64(999);
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_module_info info;
  test_log_capture capture;
  char mapping_address[64];
  memset(&capture, 0, sizeof(capture));
  nxloader_config_init(&config);
  config.expected_arch = NXLOADER_ARCH_AARCH64;
  config.flags = NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH;
  config.max_file_size = 8u * 1024u * 1024u;
  config.max_image_size = 8u * 1024u * 1024u;
  config.log = capture_log;
  config.userdata = &capture;
  CHECK(nxloader_module_create(&config, &module) == NXLOADER_OK);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "diagnostic-fixture") == NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  CHECK(nxloader_module_relocate(module) == NXLOADER_ERELOC);
  CHECK(strstr(capture.text, "diagnostic-fixture") != NULL);
  CHECK(strstr(capture.text, "relocation 999") != NULL);
  (void)snprintf(mapping_address, sizeof(mapping_address), "%p",
                 info.mapping_base);
  CHECK(strstr(capture.text, mapping_address) == NULL);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_registry_collisions(void) {
  nxloader_registry *registry = NULL;
  nxloader_symbol symbol;
  nxloader_symbol batch[2];
  nxloader_provider provider;
  nxloader_registry_match match;
  nxloader_registry_report report;
  nxloader_registry_report report_before;
  const char *borrowed_provider;
  CHECK(nxloader_registry_create(&registry) == NXLOADER_OK);
  memset(&symbol, 0, sizeof(symbol));
  memset(&provider, 0, sizeof(provider));
  symbol.name = "same";
  symbol.address = 0x1000;
  provider.struct_size = sizeof(provider);
  provider.name = "first";
  provider.symbols = &symbol;
  provider.symbol_count = 1;
  provider.priority = 10;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_OK);
  CHECK(report.added == 1);
  symbol.address = 0x2000;
  provider.name = "lower";
  provider.priority = 5;
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  provider.name = "collision";
  provider.priority = 10;
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_ECOLLISION);
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "same", &match) == NXLOADER_OK);
  CHECK(match.address == 0x1000);
  provider.name = "higher";
  provider.priority = 20;
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "same", &match) == NXLOADER_OK);
  CHECK(match.address == 0x2000);
  CHECK(strcmp(match.provider, "higher") == 0);
  symbol.flags = NXLOADER_SYMBOL_WEAK;
  symbol.address = 0x3000;
  provider.name = "same-priority-weak";
  provider.priority = 20;
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "same", &match) == NXLOADER_OK);
  CHECK(match.address == 0x2000 &&
        (match.flags & NXLOADER_SYMBOL_WEAK) == 0);
  provider.name = "higher-priority-weak";
  provider.priority = 30;
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "same", &match) == NXLOADER_OK);
  CHECK(match.address == 0x3000 &&
        (match.flags & NXLOADER_SYMBOL_WEAK) != 0);
  symbol.flags = 0;
  symbol.address = 0x4000;
  provider.name = "same-priority-strong";
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "same", &match) == NXLOADER_OK);
  borrowed_provider = match.provider;
  memset(batch, 0, sizeof(batch));
  batch[0].name = "fresh";
  batch[0].address = 0x5000;
  batch[1].name = "same";
  batch[1].address = 0x6000;
  provider.name = "atomic-collision";
  provider.symbols = batch;
  provider.symbol_count = 2;
  memset(&report, 0xa5, sizeof(report));
  report.struct_size = sizeof(report);
  report_before = report;
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_ECOLLISION);
  CHECK(memcmp(&report, &report_before, sizeof(report)) == 0);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "fresh", &match) ==
        NXLOADER_EUNRESOLVED);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "same", &match) == NXLOADER_OK);
  CHECK(match.address == 0x4000 &&
        strcmp(match.provider, "same-priority-strong") == 0 &&
        match.provider == borrowed_provider);
  nxloader_registry_destroy(registry);
}

#define REGISTRY_FNV_COLLISION_COUNT 2048u
#define REGISTRY_FNV_COLLISION_MASK UINT64_C(0x0fff)
#define REGISTRY_PUBLIC_NAME_LIMIT 4096u

static uint64_t test_registry_fnv1a(const char *name) {
  uint64_t hash = UINT64_C(14695981039346656037);
  const unsigned char *cursor = (const unsigned char *)name;
  while (*cursor) {
    hash ^= *cursor++;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static char *test_registry_fnv_collision_name(size_t index) {
  uint32_t nonce;
  for (nonce = 0; nonce < UINT32_C(1048576); ++nonce) {
    char candidate[64];
    int length = snprintf(candidate, sizeof(candidate),
                          "fnv-collision-%04zu-%08u", index, nonce);
    if (length > 0 && (size_t)length < sizeof(candidate) &&
        (test_registry_fnv1a(candidate) & REGISTRY_FNV_COLLISION_MASK) == 0) {
      char *copy = (char *)malloc((size_t)length + 1u);
      if (copy)
        memcpy(copy, candidate, (size_t)length + 1u);
      return copy;
    }
  }
  return NULL;
}

static void test_registry_fnv_collision_scaling(void) {
  nxloader_registry *registry = NULL;
  nxloader_symbol *symbols = (nxloader_symbol *)calloc(
      REGISTRY_FNV_COLLISION_COUNT, sizeof(*symbols));
  char **names =
      (char **)calloc(REGISTRY_FNV_COLLISION_COUNT, sizeof(*names));
  nxloader_provider provider;
  nxloader_registry_report report;
  nxloader_registry_match match;
  nxloader_symbol duplicates[4];
  size_t index;
  CHECK(symbols != NULL && names != NULL);
  if (!symbols || !names)
    goto cleanup;
  for (index = 0; index < REGISTRY_FNV_COLLISION_COUNT; ++index) {
    size_t reverse = REGISTRY_FNV_COLLISION_COUNT - index - 1u;
    names[index] = test_registry_fnv_collision_name(index);
    CHECK(names[index] != NULL);
    if (!names[index])
      goto cleanup;
    CHECK((test_registry_fnv1a(names[index]) &
           REGISTRY_FNV_COLLISION_MASK) == 0);
    symbols[reverse].name = names[index];
    symbols[reverse].address = (uintptr_t)UINT32_C(0x100000) + index;
  }
  CHECK(nxloader_registry_create(&registry) == NXLOADER_OK);
  if (!registry)
    goto cleanup;
  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "fnv-collision-corpus";
  provider.symbols = symbols;
  provider.symbol_count = REGISTRY_FNV_COLLISION_COUNT;
  provider.priority = 40;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_OK);
  CHECK(report.added == REGISTRY_FNV_COLLISION_COUNT &&
        report.replaced_lower_priority == 0 &&
        report.ignored_lower_priority == 0 && report.equivalent == 0);
  for (index = REGISTRY_FNV_COLLISION_COUNT; index > 0; --index) {
    size_t selected = index - 1;
    memset(&match, 0, sizeof(match));
    match.struct_size = sizeof(match);
    CHECK(nxloader_registry_lookup(registry, names[selected], &match) ==
          NXLOADER_OK);
    CHECK(match.address == (uintptr_t)UINT32_C(0x100000) + selected &&
          strcmp(match.provider, "fnv-collision-corpus") == 0);
  }
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "fnv-collision-absent", &match) ==
        NXLOADER_EUNRESOLVED);

  memset(duplicates, 0, sizeof(duplicates));
  duplicates[0].name = "batch-duplicate";
  duplicates[0].address = 0x10;
  duplicates[0].flags = NXLOADER_SYMBOL_WEAK;
  duplicates[1].name = "batch-duplicate";
  duplicates[1].address = 0x20;
  duplicates[2].name = "batch-duplicate";
  duplicates[2].address = 0x30;
  duplicates[2].flags = NXLOADER_SYMBOL_WEAK;
  duplicates[3].name = "batch-duplicate";
  duplicates[3].address = 0x20;
  provider.name = "ordered-duplicate-batch";
  provider.symbols = duplicates;
  provider.symbol_count = sizeof(duplicates) / sizeof(duplicates[0]);
  provider.priority = 20;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_OK);
  CHECK(report.added == 1 && report.replaced_lower_priority == 1 &&
        report.ignored_lower_priority == 1 && report.equivalent == 1);
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, "batch-duplicate", &match) ==
        NXLOADER_OK);
  CHECK(match.address == 0x20 && match.flags == 0 &&
        strcmp(match.provider, "ordered-duplicate-batch") == 0);

cleanup:
  nxloader_registry_destroy(registry);
  if (names) {
    for (index = 0; index < REGISTRY_FNV_COLLISION_COUNT; ++index)
      free(names[index]);
  }
  free(names);
  free(symbols);
}

static void test_registry_public_name_bounds(void) {
  nxloader_registry *registry = NULL;
  nxloader_module *module = NULL;
  nxloader_symbol symbol;
  nxloader_provider provider;
  nxloader_registry_match match;
  nxloader_registry_report report;
  nxloader_registry_report report_before;
  char *boundary =
      (char *)malloc(REGISTRY_PUBLIC_NAME_LIMIT + 1u);
  char *overlong =
      (char *)malloc(REGISTRY_PUBLIC_NAME_LIMIT + 2u);
  CHECK(boundary != NULL && overlong != NULL);
  if (!boundary || !overlong)
    goto cleanup;
  memset(boundary, 'b', REGISTRY_PUBLIC_NAME_LIMIT);
  boundary[REGISTRY_PUBLIC_NAME_LIMIT] = '\0';
  memset(overlong, 'o', REGISTRY_PUBLIC_NAME_LIMIT + 1u);
  overlong[REGISTRY_PUBLIC_NAME_LIMIT + 1u] = '\0';
  CHECK(nxloader_registry_create(&registry) == NXLOADER_OK);
  if (!registry)
    goto cleanup;
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  CHECK(module != NULL);
  memset(&symbol, 0, sizeof(symbol));
  memset(&provider, 0, sizeof(provider));
  symbol.name = boundary;
  symbol.address = 0x1234;
  provider.struct_size = sizeof(provider);
  provider.name = "public-name-boundary";
  provider.symbols = &symbol;
  provider.symbol_count = 1;
  CHECK(nxloader_registry_add_provider(registry, &provider, NULL) ==
        NXLOADER_OK);
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, boundary, &match) == NXLOADER_OK);
  CHECK(match.address == 0x1234);

  memset(&report, 0x5a, sizeof(report));
  report.struct_size = sizeof(report);
  report_before = report;
  symbol.name = overlong;
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_EINVAL);
  CHECK(memcmp(&report, &report_before, sizeof(report)) == 0);
  provider.name = overlong;
  symbol.name = "valid-after-overlong";
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_EINVAL);
  CHECK(memcmp(&report, &report_before, sizeof(report)) == 0);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, overlong, &match) ==
        NXLOADER_EINVAL);

  provider.name = "overflow-before-dereference";
  provider.symbols = &symbol;
  provider.symbol_count = SIZE_MAX;
  CHECK(nxloader_registry_add_provider(registry, &provider, &report) ==
        NXLOADER_EOVERFLOW);
  CHECK(memcmp(&report, &report_before, sizeof(report)) == 0);
  if (module)
    CHECK(nxloader_registry_add_module(registry, module, "phase-invalid", 10,
                                       &report) == NXLOADER_ESTATE);
  CHECK(memcmp(&report, &report_before, sizeof(report)) == 0);
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(registry, boundary, &match) == NXLOADER_OK &&
        match.address == 0x1234);

cleanup:
  nxloader_module_destroy(module);
  nxloader_registry_destroy(registry);
  free(overlong);
  free(boundary);
}

static void test_elf64(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  nxloader_module *module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_registry *empty = NULL;
  nxloader_registry *registry;
  nxloader_resolution_report report;
  nxloader_module_info info;
  uintptr_t local = 0;
  uintptr_t hook = 0;
  uintptr_t slot = 0;
  uint64_t bias;
  char permissions[5] = {0};
  CHECK(fixture != NULL && module != NULL);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "synthetic-aarch64.so") == NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  CHECK(info.arch == NXLOADER_ARCH_AARCH64 && info.relocation_count == 5);
  CHECK(info.needed_count == 1);
  CHECK(strcmp(nxloader_module_needed(module, 0), "libaux.so") == 0);
  CHECK(strcmp(nxloader_module_soname(module), "libsynthetic.so") == 0);
  CHECK(bytes_are_zero(nxloader_module_vma_to_pointer(module, 0x4000, 256),
                       256));
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, LOCAL_VMA, 1), permissions) &&
        strcmp(permissions, "rw-p") == 0);
  bias = (uintptr_t)info.mapping_base - info.minimum_vma;
  CHECK(nxloader_module_resolve(module, NULL, 0, NULL) == NXLOADER_EINVAL);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8)) ==
        bias + 0x1200);
  CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8)) ==
        bias + LOCAL_VMA + 7);
  CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(module, empty, 0, &report) ==
        NXLOADER_EUNRESOLVED);
  CHECK(report.unresolved_strong == 1);
  CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_IMPORT64, 8)) == 0);
  registry = new_registry_with_host(TEST_HOST64);
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(module, registry, 0, &report) == NXLOADER_OK);
  CHECK(report.imports_resolved == 1 && report.weak_imports_zeroed == 1);
  CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_IMPORT64, 8)) ==
        (uint64_t)TEST_HOST64);
  CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_WEAK64, 8)) == 0);
  CHECK(nxloader_module_find_export(module, "local", &local) == NXLOADER_OK);
  CHECK(local == bias + LOCAL_VMA);
  CHECK(nxloader_module_find_relocation(module, "host", &slot) == NXLOADER_OK);
  CHECK(slot == (uintptr_t)nxloader_module_vma_to_pointer(module,
                                                          SLOT_IMPORT64, 8));
  CHECK(nxloader_registry_add_module(registry, module, "guest-aux", 50,
                                     NULL) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "hook_target", &hook) ==
        NXLOADER_OK);
  CHECK(nxloader_module_install_hook(module, hook, local, 4) == NXLOADER_OK);
  CHECK((read32((const void *)hook) & UINT32_C(0xfc000000)) ==
        UINT32_C(0x14000000));
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, LOCAL_VMA, 1), permissions) &&
        strcmp(permissions, "r-xp") == 0);
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, 0x2000, 1), permissions) &&
        strcmp(permissions, "r--p") == 0);
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, 0x3000, 1), permissions) &&
        strcmp(permissions, "rw-p") == 0);
  CHECK(mapping_permissions((const unsigned char *)info.mapping_base +
                                info.image_size, permissions) &&
        strcmp(permissions, "r-xp") == 0);
  if (nxloader_process_arch() == NXLOADER_ARCH_AARCH64) {
    CHECK(nxloader_module_call_initializers(module) == NXLOADER_OK);
    CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_INITIALIZED);
  } else {
    CHECK(nxloader_module_call_initializers(module) == NXLOADER_EARCH);
  }
  nxloader_registry_destroy(registry);
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(module);
  free(fixture);
}

static void expect_aarch64_relocation_not_hookable(uint32_t type,
                                                   nxloader_result expected,
                                                   const char *name) {
  unsigned char *fixture = build_elf64(type);
  nxloader_module *module = new_module(NXLOADER_ARCH_AARCH64, skip_unknown);
  void *target;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module) {
    nxloader_module_destroy(module);
    free(fixture);
    return;
  }
  unknown_relocation_hook_calls = 0;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE,
                                           sizeof(uint64_t));
  CHECK(target != NULL && read64(target) == 0);
  CHECK(nxloader_module_relocate(module) == expected);
  CHECK(unknown_relocation_hook_calls == 0);
  CHECK(read64(target) == 0);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_aarch64_hook_veneer_contract(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_AARCH64, NULL, 0, 32);
  nxloader_registry *registry = NULL;
  nxloader_module_info info;
  uintptr_t local = 0;
  uintptr_t hook = 0;
  uint8_t *pool;
  void *relro = NULL;
  void *mapping_base = NULL;
  uint8_t saved_pool[16];
  uint32_t branch;
  int64_t displacement;
  char permissions[5] = {0};
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "aarch64-hook-veneer") == NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  pool = (uint8_t *)info.mapping_base + info.image_size;
  relro = nxloader_module_vma_to_pointer(module, 0x2000, 1);
  mapping_base = info.mapping_base;
  CHECK(relro != NULL);
  CHECK(bytes_are_zero(pool, 16));
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  registry = new_registry_with_host(TEST_HOST64);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "local", &local) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "hook_target", &hook) ==
        NXLOADER_OK);
  CHECK(nxloader_module_install_hook(module, hook, local + 2, 4) ==
        NXLOADER_EINVAL);
  CHECK(read32((const void *)hook) == UINT32_C(0xd503201f));
  CHECK(bytes_are_zero(pool, 16));
  CHECK(nxloader_module_install_hook(module, hook, local, 4) == NXLOADER_OK);
  CHECK(read32(pool) == UINT32_C(0x58000051));
  CHECK(read32(pool + 4) == UINT32_C(0xd61f0220));
  CHECK(read64(pool + 8) == (uint64_t)local);
  branch = read32((const void *)hook);
  CHECK((branch & UINT32_C(0xfc000000)) == UINT32_C(0x14000000));
  displacement = decode_aarch64_branch_displacement(branch);
  CHECK((uintptr_t)((int64_t)hook + displacement) == (uintptr_t)pool);
  memcpy(saved_pool, pool, sizeof(saved_pool));
  CHECK(nxloader_module_install_hook(
            module, (uintptr_t)info.mapping_base + info.mapping_size,
            hook, 4) ==
        NXLOADER_EBOUNDS);
  CHECK(read32((const void *)local) == UINT32_C(0xd65f03c0));
  CHECK(memcmp(pool, saved_pool, sizeof(saved_pool)) == 0);
  CHECK(mapping_permissions(pool, permissions) &&
        strcmp(permissions, "rw-p") == 0);
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(pool, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(relro, permissions) &&
        strcmp(permissions, "r--p") == 0);
  CHECK(nxloader_module_begin_patch(module) == NXLOADER_OK);
  CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_PATCHING);
  CHECK(nxloader_module_begin_patch(module) == NXLOADER_ESTATE);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  CHECK(info.mapping_base == mapping_base);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "rw-p") == 0 && strchr(permissions, 'x') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(pool, permissions) &&
        strcmp(permissions, "rw-p") == 0 && strchr(permissions, 'x') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(relro, permissions) &&
        strcmp(permissions, "r--p") == 0 && strchr(permissions, 'w') == NULL &&
        strchr(permissions, 'x') == NULL);
  /* NXL090 protection invariant: image code/main pool are RW-not-X while
   * PATCHING, and GNU RELRO remains R-- across the same-mapping window. */
  CHECK(nxloader_module_install_hook(module, hook, local, 4) == NXLOADER_OK);
  CHECK(read32(pool + 16) == UINT32_C(0x58000051));
  CHECK(read32(pool + 20) == UINT32_C(0xd61f0220));
  CHECK(read64(pool + 24) == (uint64_t)local);
  branch = read32((const void *)hook);
  displacement = decode_aarch64_branch_displacement(branch);
  CHECK((uintptr_t)((int64_t)hook + displacement) == (uintptr_t)(pool + 16));
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_FINALIZED);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(pool, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_patch_preserves_executable_relro(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  Elf64_Ehdr *header = fixture ? (Elf64_Ehdr *)fixture : NULL;
  Elf64_Phdr *programs =
      fixture ? (Elf64_Phdr *)(fixture + header->e_phoff) : NULL;
  nxloader_module *module = new_module_with_options(
      NXLOADER_ARCH_AARCH64, NULL, 0, 16);
  nxloader_registry *registry = NULL;
  nxloader_module_info info;
  uintptr_t local = 0;
  uintptr_t hook = 0;
  uint8_t *pool = NULL;
  char permissions[5] = {0};
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  programs[3].p_offset = 0x1000;
  programs[3].p_vaddr = 0x1000;
  programs[3].p_filesz = 0x100;
  programs[3].p_memsz = 0x100;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "aarch64-executable-relro") ==
        NXLOADER_OK);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  registry = new_registry_with_host(TEST_HOST64);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "local", &local) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "hook_target", &hook) ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  pool = (uint8_t *)info.mapping_base + info.image_size;
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  CHECK(nxloader_module_begin_patch(module) == NXLOADER_OK);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  CHECK(nxloader_module_install_hook(module, hook, local, 4) ==
        NXLOADER_EPROTECT);
  CHECK(read32((const void *)hook) == UINT32_C(0xd503201f));
  CHECK(bytes_are_zero(pool, 16));
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_aarch64_hook_pool_range_is_atomic(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  Elf64_Ehdr *header = fixture ? (Elf64_Ehdr *)fixture : NULL;
  Elf64_Phdr *programs =
      fixture ? (Elf64_Phdr *)(fixture + header->e_phoff) : NULL;
  nxloader_module *module = new_module_with_limits(
      NXLOADER_ARCH_AARCH64, NULL, 0, 16, 160u * 1024u * 1024u);
  nxloader_registry *registry = NULL;
  nxloader_module_info info;
  uintptr_t local = 0;
  uintptr_t hook = 0;
  uint8_t *pool;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  programs[1].p_memsz = UINT64_C(0x09000000);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "aarch64-unreachable-hook-pool") ==
        NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  pool = (uint8_t *)info.mapping_base + info.image_size;
  CHECK((uintptr_t)pool > (uintptr_t)info.mapping_base + HOOK_VMA);
  CHECK((uintptr_t)pool - ((uintptr_t)info.mapping_base + HOOK_VMA) >
        (uintptr_t)(128u * 1024u * 1024u));
  CHECK(bytes_are_zero(pool, 16));
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  registry = new_registry_with_host(TEST_HOST64);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "local", &local) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "hook_target", &hook) ==
        NXLOADER_OK);
  CHECK(nxloader_module_install_hook(module, hook, local, 4) ==
        NXLOADER_EOVERFLOW);
  CHECK(read32((const void *)hook) == UINT32_C(0xd503201f));
  CHECK(bytes_are_zero(pool, 16));

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_aarch64_relocation_contract(void) {
  static const uint32_t unsupported_instruction_types[] = {
      R_AARCH64_PREL64, R_AARCH64_PREL32, R_AARCH64_PREL16,
      R_AARCH64_ADR_PREL_LO21, R_AARCH64_ADR_PREL_PG_HI21,
      R_AARCH64_ADD_ABS_LO12_NC, R_AARCH64_LDST64_ABS_LO12_NC,
      R_AARCH64_JUMP26, R_AARCH64_CALL26};
  size_t index;
  unsigned char *fixture;
  Elf64_Rela *relocations;
  Elf64_Sym *symbols;
  nxloader_module *module;
  nxloader_module_info info;
  void *relative_slot;
  void *absolute_slot;
  void *initializer_slot;
  uint64_t bias;
  static const uint64_t protected_metadata_targets[] = {
      DYNAMIC_VMA, STRING_VMA, SYMBOL_VMA, HASH_VMA, RELOCATION_VMA};

  for (index = 0;
       index < sizeof(unsupported_instruction_types) /
                   sizeof(unsupported_instruction_types[0]);
       ++index)
    expect_aarch64_relocation_not_hookable(
        unsupported_instruction_types[index], NXLOADER_ERELOC,
        "aarch64-unproven-dynamic-relocation");
  expect_aarch64_relocation_not_hookable(512, NXLOADER_EUNSUPPORTED,
                                         "aarch64-tls-relocation");
  expect_aarch64_relocation_not_hookable(R_AARCH64_IRELATIVE,
                                         NXLOADER_EUNSUPPORTED,
                                         "aarch64-irelative");
  expect_aarch64_relocation_not_hookable(999, NXLOADER_ERELOC,
                                         "aarch64-unknown-relocation");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, skip_unknown);
  symbols = fixture ? (Elf64_Sym *)(fixture + SYMBOL_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    symbols[4].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_GNU_IFUNC);
    unknown_relocation_hook_calls = 0;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-ifunc-global") ==
          NXLOADER_EUNSUPPORTED);
    CHECK(unknown_relocation_hook_calls == 0);
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* A malformed late RELATIVE must discard every earlier pending write. */
  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[4].r_info = ELF64_R_INFO(1, R_AARCH64_RELATIVE);
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-relative-symbol-index") ==
          NXLOADER_OK);
    relative_slot = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8);
    absolute_slot = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8);
    initializer_slot =
        nxloader_module_vma_to_pointer(module, INIT_ARRAY_VMA, 8);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
    CHECK(read64(relative_slot) == 0 && read64(absolute_slot) == 0 &&
          read64(initializer_slot) == 0);
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* RELA addends are signed: an in-range negative value succeeds. */
  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[1].r_addend = -7;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-negative-rela-addend") ==
          NXLOADER_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
    bias = (uintptr_t)info.mapping_base - info.minimum_vma;
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    absolute_slot = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8);
    CHECK(read64(absolute_slot) == bias + LOCAL_VMA - 7);
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* Underflow in a late absolute addend is rejected before any commit. */
  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  symbols = fixture ? (Elf64_Sym *)(fixture + SYMBOL_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    symbols[1].st_shndx = SHN_ABS;
    symbols[1].st_value = 0;
    relocations[4].r_info = ELF64_R_INFO(1, R_AARCH64_ABS64);
    relocations[4].r_addend = -1;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-addend-underflow-atomic") ==
          NXLOADER_OK);
    relative_slot = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8);
    absolute_slot = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8);
    initializer_slot =
        nxloader_module_vma_to_pointer(module, INIT_ARRAY_VMA, 8);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EOVERFLOW);
    CHECK(read64(relative_slot) == 0 && read64(absolute_slot) == 0 &&
          read64(initializer_slot) == 0);
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* Duplicate destinations are detected after O(n) collection, still before
   * any write is made. */
  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[4].r_offset = SLOT_RELATIVE;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-duplicate-target-atomic") ==
          NXLOADER_OK);
    relative_slot = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8);
    absolute_slot = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
    CHECK(read64(relative_slot) == 0 && read64(absolute_slot) == 0);
  }
  nxloader_module_destroy(module);
  free(fixture);

  for (index = 0;
       index < sizeof(protected_metadata_targets) /
                   sizeof(protected_metadata_targets[0]);
       ++index) {
    fixture = build_elf64(R_AARCH64_RELATIVE);
    module = new_module(NXLOADER_ARCH_AARCH64, NULL);
    relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module) {
      relocations[0].r_offset = protected_metadata_targets[index];
      CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                        "aarch64-metadata-target") ==
            NXLOADER_OK);
      CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
      CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8)) ==
            0);
      CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8)) ==
            0);
    }
    nxloader_module_destroy(module);
    free(fixture);
  }

  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[4].r_offset = RELOCATION_VMA;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-late-metadata-atomic") ==
          NXLOADER_OK);
    relative_slot = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8);
    absolute_slot = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64, 8);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
    CHECK(read64(relative_slot) == 0 && read64(absolute_slot) == 0);
  }
  nxloader_module_destroy(module);
  free(fixture);

  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[0].r_offset = LOCAL_VMA;
    relocations[0].r_info = ELF64_R_INFO(1, R_AARCH64_ABS64);
    relocations[0].r_addend = 0;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-data-reloc-in-code") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EUNSUPPORTED);
    CHECK(read32(nxloader_module_vma_to_pointer(module, LOCAL_VMA, 4)) ==
          UINT32_C(0xd65f03c0));
  }
  nxloader_module_destroy(module);
  free(fixture);

  fixture = build_elf64(R_AARCH64_RELATIVE);
  module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  relocations = fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[0].r_offset = SLOT_RELATIVE + 1;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "aarch64-misaligned-relocation") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
    CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8)) == 0);
  }
  nxloader_module_destroy(module);
  free(fixture);
}

static void expect_arm32_metadata_target_rejected(uint32_t target_vma,
                                                  const char *debug_name) {
  unsigned char *fixture = build_elf32(R_ARM_RELATIVE);
  Elf32_Rel *relocations =
      fixture ? (Elf32_Rel *)(fixture + RELOCATION_VMA) : NULL;
  nxloader_module *module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  void *relative_slot;
  void *absolute_slot;
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[0].r_offset = target_vma;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      debug_name) == NXLOADER_OK);
    relative_slot =
        nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, sizeof(uint32_t));
    absolute_slot = nxloader_module_vma_to_pointer(
        module, SLOT_ABSOLUTE32, sizeof(uint32_t));
    CHECK(relative_slot != NULL && absolute_slot != NULL);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
    if (relative_slot && absolute_slot)
      CHECK(read32(relative_slot) == UINT32_C(0x1200) &&
            read32(absolute_slot) == 7);
  }
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_arm32_metadata_target_contract(void) {
  static const uint32_t protected_metadata_targets[] = {
      DYNAMIC_VMA, STRING_VMA, SYMBOL_VMA, HASH_VMA, RELOCATION_VMA};
  size_t index;
  unsigned char *fixture;
  Elf32_Rel *relocations;
  Elf32_Dyn *hash_entry;
  uint32_t *gnu_hash;
  nxloader_module *module;
  nxloader_module_info info;
  void *relative_slot;
  void *absolute_slot;
  void *initializer_slot;
  uint64_t bias;

  for (index = 0;
       index < sizeof(protected_metadata_targets) /
                   sizeof(protected_metadata_targets[0]);
       ++index)
    expect_arm32_metadata_target_rejected(protected_metadata_targets[index],
                                          "armv7-metadata-target");

  /* Exercise a partial range overlap, not only an exact metadata address. */
  expect_arm32_metadata_target_rejected(DYNAMIC_VMA - 2,
                                        "armv7-metadata-partial-overlap");

  /* A late metadata target must leave every earlier staged relocation
   * untouched. */
  fixture = build_elf32(R_ARM_RELATIVE);
  relocations = fixture ? (Elf32_Rel *)(fixture + RELOCATION_VMA) : NULL;
  module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[4].r_offset = RELOCATION_VMA;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "armv7-late-metadata-atomic") ==
          NXLOADER_OK);
    relative_slot =
        nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, sizeof(uint32_t));
    absolute_slot = nxloader_module_vma_to_pointer(
        module, SLOT_ABSOLUTE32, sizeof(uint32_t));
    initializer_slot = nxloader_module_vma_to_pointer(
        module, INIT_ARRAY_VMA, sizeof(uint32_t));
    CHECK(relative_slot != NULL && absolute_slot != NULL &&
          initializer_slot != NULL);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
    if (relative_slot && absolute_slot && initializer_slot)
      CHECK(read32(relative_slot) == UINT32_C(0x1200) &&
            read32(absolute_slot) == 7 &&
            read32(initializer_slot) == LOCAL_VMA);
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* DT_INIT_ARRAY is relocation data, not loader metadata: the normal
   * Android initializer pointer relocation remains valid, but it is never
   * executed by this test. */
  fixture = build_elf32(R_ARM_RELATIVE);
  module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "armv7-init-array-relocation") ==
          NXLOADER_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
    bias = (uintptr_t)info.mapping_base - info.minimum_vma;
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    initializer_slot = nxloader_module_vma_to_pointer(
        module, INIT_ARRAY_VMA, sizeof(uint32_t));
    CHECK(initializer_slot != NULL);
    if (initializer_slot)
      CHECK(read32(initializer_slot) == (uint32_t)(bias + LOCAL_VMA));
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* R_ARM_NONE performs no write and may therefore name metadata safely. */
  fixture = build_elf32(R_ARM_RELATIVE);
  relocations = fixture ? (Elf32_Rel *)(fixture + RELOCATION_VMA) : NULL;
  module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  CHECK(fixture != NULL && module != NULL);
  if (fixture && module) {
    relocations[0].r_offset = DYNAMIC_VMA;
    relocations[0].r_info = ELF32_R_INFO(0, R_ARM_NONE);
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "armv7-none-metadata-target") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  }
  nxloader_module_destroy(module);
  free(fixture);

  /* A valid GNU hash table receives the same structural protection as the
   * SysV table used by the base fixture. */
  fixture = build_elf32(R_ARM_RELATIVE);
  relocations = fixture ? (Elf32_Rel *)(fixture + RELOCATION_VMA) : NULL;
  hash_entry = fixture ? find_dynamic32(fixture, DT_HASH) : NULL;
  gnu_hash = fixture ? (uint32_t *)(fixture + HASH_VMA) : NULL;
  module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  CHECK(fixture != NULL && relocations != NULL && hash_entry != NULL &&
        gnu_hash != NULL && module != NULL);
  if (fixture && relocations && hash_entry && gnu_hash && module) {
    hash_entry->d_tag = DT_GNU_HASH;
    memset(gnu_hash, 0, 10 * sizeof(*gnu_hash));
    gnu_hash[0] = 1; /* bucket count */
    gnu_hash[1] = 1; /* first hashed symbol */
    gnu_hash[2] = 1; /* bloom word count */
    gnu_hash[5] = 1; /* bucket starts at symbol 1 */
    gnu_hash[9] = 1; /* symbol 4 terminates the chain */
    relocations[0].r_offset = HASH_VMA;
    CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                      "armv7-gnu-hash-target") ==
          NXLOADER_OK);
    CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
  }
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_elf32(void) {
  unsigned char *fixture = build_elf32(R_ARM_RELATIVE);
  nxloader_module *module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  nxloader_registry *empty = NULL;
  nxloader_registry *registry;
  nxloader_resolution_report report;
  nxloader_module_info info;
  uintptr_t local = 0;
  uintptr_t hook = 0;
  uintptr_t exidx = 0;
  size_t exidx_count = 0;
  uint64_t bias;
  char permissions[5] = {0};
  CHECK(fixture != NULL && module != NULL);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "synthetic-armv7.so") == NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  CHECK(strcmp(nxloader_module_soname(module), "libsynthetic.so") == 0);
  CHECK(bytes_are_zero(nxloader_module_vma_to_pointer(module, 0x4000, 256),
                       256));
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, LOCAL_VMA, 1), permissions) &&
        strcmp(permissions, "rw-p") == 0);
  bias = (uintptr_t)info.mapping_base - info.minimum_vma;
  CHECK(bias + info.maximum_vma <= UINT32_MAX);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  CHECK(read32(nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 4)) ==
        (uint32_t)(bias + 0x1200));
  CHECK(read32(nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE32, 4)) ==
        (uint32_t)(bias + LOCAL_VMA + 7));
  CHECK(nxloader_registry_create(&empty) == NXLOADER_OK);
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(module, empty, 0, &report) ==
        NXLOADER_EUNRESOLVED);
  CHECK(read32(nxloader_module_vma_to_pointer(module, SLOT_IMPORT32, 4)) == 0);
  registry = new_registry_with_host(UINT32_C(0x12345000));
  report.struct_size = sizeof(report);
  CHECK(nxloader_module_resolve(module, registry, 0, &report) == NXLOADER_OK);
  CHECK(report.imports_resolved == 1 && report.weak_imports_zeroed == 1);
  CHECK(read32(nxloader_module_vma_to_pointer(module, SLOT_IMPORT32, 4)) ==
        UINT32_C(0x12345000));
  CHECK(read32(nxloader_module_vma_to_pointer(module, SLOT_WEAK32, 4)) == 0);
  CHECK(nxloader_module_find_export(module, "local", &local) == NXLOADER_OK);
  CHECK(local == (uintptr_t)(bias + LOCAL_VMA));
  CHECK(nxloader_module_find_arm_exidx(module, local, &exidx, &exidx_count) ==
        NXLOADER_OK);
  CHECK(exidx == (uintptr_t)nxloader_module_vma_to_pointer(module, 0x1800, 16));
  CHECK(exidx_count == 2);
  CHECK(nxloader_module_find_export(module, "hook_target", &hook) ==
        NXLOADER_OK);
  CHECK(nxloader_module_install_hook(module, hook, local, 8) == NXLOADER_OK);
  CHECK(read32((const void *)hook) == UINT32_C(0xe51ff004));
  {
    uintptr_t thumb = (uintptr_t)(bias + 0x1122) | (uintptr_t)1;
    uintptr_t clean_thumb = thumb & ~(uintptr_t)1;
    CHECK(nxloader_module_install_hook(module, thumb, local, 8) == NXLOADER_OK);
    CHECK(read16((const void *)clean_thumb) == UINT16_C(0xf8df));
    CHECK(read16((const void *)(clean_thumb + 2)) == UINT16_C(0xf002));
    CHECK(read32((const void *)(clean_thumb + 4)) == (uint32_t)local);
  }
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, LOCAL_VMA, 1), permissions) &&
        strcmp(permissions, "r-xp") == 0);
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, 0x2000, 1), permissions) &&
        strcmp(permissions, "r--p") == 0);
  CHECK(mapping_permissions(nxloader_module_vma_to_pointer(
                                module, 0x3000, 1), permissions) &&
        strcmp(permissions, "rw-p") == 0);
  if (nxloader_process_arch() == NXLOADER_ARCH_ARMV7)
    CHECK(nxloader_module_call_initializers(module) == NXLOADER_OK);
  else
    CHECK(nxloader_module_call_initializers(module) == NXLOADER_EARCH);
  nxloader_registry_destroy(registry);
  nxloader_registry_destroy(empty);
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_bounds_and_atomic_relocation(void) {
  unsigned char *bad_bounds = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *packed = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *writable_code = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *unknown = build_elf64(999);
  unsigned char *accepted = build_elf64(999);
  unsigned char *gnu_bounds = build_elf32(R_ARM_RELATIVE);
  nxloader_module *module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module *packed_module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module *wx_module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module *unknown_module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module *accepted_module =
      new_module(NXLOADER_ARCH_AARCH64, skip_unknown);
  nxloader_module *gnu_bounds_module =
      new_module(NXLOADER_ARCH_ARMV7, NULL);
  Elf64_Ehdr *header = (Elf64_Ehdr *)bad_bounds;
  Elf64_Phdr *programs = (Elf64_Phdr *)(bad_bounds + header->e_phoff);
  programs[1].p_filesz = programs[1].p_memsz + 1;
  CHECK(nxloader_module_load_memory(module, bad_bounds, FIXTURE_SIZE,
                                    "bad-bounds.so") == NXLOADER_EBOUNDS);
  {
    Elf64_Dyn *dynamic = (Elf64_Dyn *)(packed + DYNAMIC_VMA);
    size_t index = 0;
    while (dynamic[index].d_tag != DT_NULL)
      index++;
    dynamic[index].d_tag = DT_ANDROID_RELA;
    dynamic[index].d_un.d_val = RELOCATION_VMA;
    CHECK(nxloader_module_load_memory(packed_module, packed, FIXTURE_SIZE,
                                      "packed-relocations.so") ==
          NXLOADER_EUNSUPPORTED);
  }
  {
    Elf64_Ehdr *wx_header = (Elf64_Ehdr *)writable_code;
    Elf64_Phdr *wx_programs =
        (Elf64_Phdr *)(writable_code + wx_header->e_phoff);
    wx_programs[0].p_flags |= PF_W;
    CHECK(nxloader_module_load_memory(wx_module, writable_code, FIXTURE_SIZE,
                                      "writable-code.so") ==
          NXLOADER_EPROTECT);
  }
  {
    Elf32_Dyn *dynamic = (Elf32_Dyn *)(gnu_bounds + DYNAMIC_VMA);
    uint32_t *gnu_hash = (uint32_t *)(gnu_bounds + HASH_VMA);
    size_t index;
    for (index = 0; dynamic[index].d_tag != DT_NULL; ++index) {
      if (dynamic[index].d_tag == DT_HASH) {
        dynamic[index].d_tag = DT_GNU_HASH;
        break;
      }
    }
    CHECK(dynamic[index].d_tag == DT_GNU_HASH);
    gnu_hash[0] = UINT32_MAX;
    gnu_hash[1] = 1;
    gnu_hash[2] = 1;
    gnu_hash[3] = 0;
    gnu_hash[4] = 0;
    CHECK(nxloader_module_load_memory(gnu_bounds_module, gnu_bounds,
                                      FIXTURE_SIZE, "gnu-hash-bounds.so") ==
          NXLOADER_EBOUNDS);
  }
  CHECK(nxloader_module_load_memory(unknown_module, unknown, FIXTURE_SIZE,
                                    "unknown-reloc.so") == NXLOADER_OK);
  CHECK(read64(nxloader_module_vma_to_pointer(unknown_module, SLOT_RELATIVE,
                                              8)) == 0);
  CHECK(nxloader_module_relocate(unknown_module) == NXLOADER_ERELOC);
  CHECK(read64(nxloader_module_vma_to_pointer(unknown_module, SLOT_RELATIVE,
                                              8)) == 0);
  unknown_relocation_hook_calls = 0;
  CHECK(nxloader_module_load_memory(accepted_module, accepted, FIXTURE_SIZE,
                                    "hooked-reloc.so") == NXLOADER_OK);
  CHECK(nxloader_module_relocate(accepted_module) == NXLOADER_ERELOC);
  CHECK(unknown_relocation_hook_calls == 0);
  CHECK(read64(nxloader_module_vma_to_pointer(accepted_module, SLOT_RELATIVE,
                                              8)) == 0);
  nxloader_module_destroy(accepted_module);
  nxloader_module_destroy(gnu_bounds_module);
  nxloader_module_destroy(unknown_module);
  nxloader_module_destroy(wx_module);
  nxloader_module_destroy(packed_module);
  nxloader_module_destroy(module);
  free(accepted);
  free(gnu_bounds);
  free(unknown);
  free(writable_code);
  free(packed);
  free(bad_bounds);
}

static void test_arm_relocation_rejection_is_atomic(void) {
  unsigned char *late_unknown = build_elf32(R_ARM_RELATIVE);
  Elf32_Rel *relocations =
      late_unknown ? (Elf32_Rel *)(late_unknown + RELOCATION_VMA) : NULL;
  nxloader_module *module = new_module(NXLOADER_ARCH_ARMV7, NULL);
  void *relative_slot;
  void *absolute_slot;
  CHECK(late_unknown != NULL && module != NULL);
  if (!late_unknown || !module)
    goto cleanup;
  relocations[4].r_info = ELF32_R_INFO(0, 999);
  CHECK(nxloader_module_load_memory(module, late_unknown, FIXTURE_SIZE,
                                    "arm-late-unknown-atomic") == NXLOADER_OK);
  relative_slot =
      nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, sizeof(uint32_t));
  absolute_slot = nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE32,
                                                  sizeof(uint32_t));
  CHECK(relative_slot != NULL && absolute_slot != NULL);
  CHECK(read32(relative_slot) == UINT32_C(0x1200));
  CHECK(read32(absolute_slot) == 7);
  CHECK(nxloader_module_relocate(module) == NXLOADER_ERELOC);
  CHECK(read32(relative_slot) == UINT32_C(0x1200));
  CHECK(read32(absolute_slot) == 7);

cleanup:
  nxloader_module_destroy(module);
  free(late_unknown);
}

static void test_arm_symbol_type_rejection_is_pre_hook(void) {
  unsigned char *local_fixture = build_elf32(R_ARM_RELATIVE);
  unsigned char *import_fixture = build_elf32(R_ARM_RELATIVE);
  nxloader_module *local_module =
      new_module(NXLOADER_ARCH_ARMV7, count_and_skip_relocation);
  nxloader_module *import_module =
      new_module(NXLOADER_ARCH_ARMV7, count_and_skip_relocation);
  nxloader_registry *registry = NULL;
  Elf32_Rel *relocations;
  Elf32_Sym *symbols;
  Elf32_Dyn *rel_size;
  void *target;

  CHECK(local_fixture != NULL && local_module != NULL);
  if (local_fixture && local_module) {
    relocations = (Elf32_Rel *)(local_fixture + RELOCATION_VMA);
    symbols = (Elf32_Sym *)(local_fixture + SYMBOL_VMA);
    rel_size = find_dynamic32(local_fixture, DT_RELSZ);
    CHECK(rel_size != NULL);
    relocations[0].r_offset = SLOT_ABSOLUTE32;
    relocations[0].r_info = ELF32_R_INFO(1, R_ARM_ABS32);
    symbols[1].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_TLS);
    if (rel_size)
      rel_size->d_un.d_val = sizeof(*relocations);
    CHECK(nxloader_module_load_memory(local_module, local_fixture,
                                      FIXTURE_SIZE,
                                      "arm-local-tls-symbol-pre-hook") ==
          NXLOADER_OK);
    target = nxloader_module_vma_to_pointer(
        local_module, SLOT_ABSOLUTE32, sizeof(uint32_t));
    CHECK(target != NULL);
    if (target)
      CHECK(read32(target) == 7);
    unknown_relocation_hook_calls = 0;
    CHECK(nxloader_module_relocate(local_module) == NXLOADER_EUNSUPPORTED);
    CHECK(unknown_relocation_hook_calls == 0);
    if (target)
      CHECK(read32(target) == 7);
  }

  CHECK(import_fixture != NULL && import_module != NULL);
  if (import_fixture && import_module) {
    relocations = (Elf32_Rel *)(import_fixture + RELOCATION_VMA);
    rel_size = find_dynamic32(import_fixture, DT_RELSZ);
    CHECK(rel_size != NULL);
    relocations[0].r_offset = SLOT_IMPORT32;
    relocations[0].r_info = ELF32_R_INFO(2, R_ARM_GLOB_DAT);
    if (rel_size)
      rel_size->d_un.d_val = sizeof(*relocations);
    CHECK(nxloader_module_load_memory(import_module, import_fixture,
                                      FIXTURE_SIZE,
                                      "arm-import-tls-symbol-pre-hook") ==
          NXLOADER_OK);
    target = nxloader_module_vma_to_pointer(
        import_module, SLOT_IMPORT32, sizeof(uint32_t));
    CHECK(target != NULL);
    if (target)
      CHECK(read32(target) == 0);
    CHECK(nxloader_module_relocate(import_module) == NXLOADER_OK);
    symbols = (Elf32_Sym *)nxloader_module_vma_to_pointer(
        import_module, SYMBOL_VMA, 5 * sizeof(Elf32_Sym));
    CHECK(symbols != NULL);
    if (symbols)
      symbols[2].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_TLS);
    CHECK(nxloader_registry_create(&registry) == NXLOADER_OK);
    unknown_relocation_hook_calls = 0;
    if (symbols && registry)
      CHECK(nxloader_module_resolve(import_module, registry, 0, NULL) ==
            NXLOADER_EUNSUPPORTED);
    CHECK(unknown_relocation_hook_calls == 0);
    if (target)
      CHECK(read32(target) == 0);
  }

  nxloader_registry_destroy(registry);
  nxloader_module_destroy(import_module);
  nxloader_module_destroy(local_module);
  free(import_fixture);
  free(local_fixture);
}

static void test_header_and_segment_contract(void) {
  unsigned char *fixture;
  Elf64_Ehdr *header64;
  Elf64_Phdr *programs64;
  Elf32_Ehdr *header32;
  Elf32_Phdr *programs32;
  Elf64_Phdr temporary64;

  fixture = build_elf64(R_AARCH64_RELATIVE);
  fixture[EI_OSABI] = ELFOSABI_LINUX;
  expect_load64(fixture, NXLOADER_EFORMAT, "osabi64");

  fixture = build_elf32(R_ARM_RELATIVE);
  fixture[EI_ABIVERSION] = 1;
  expect_load32(fixture, NXLOADER_EFORMAT, "abi-version32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  fixture[EI_PAD] = 1;
  expect_load64(fixture, NXLOADER_EFORMAT, "ident-padding64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  header64->e_machine = EM_ARM;
  expect_load64(fixture, NXLOADER_EFORMAT, "machine64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  header64->e_flags = 1;
  expect_load64(fixture, NXLOADER_EFORMAT, "aarch64-nonzero-eflags");

  fixture = build_elf32(R_ARM_RELATIVE);
  header32 = (Elf32_Ehdr *)fixture;
  header32->e_flags = 0;
  expect_load32(fixture, NXLOADER_EFORMAT, "eabi32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  header64->e_phoff = UINT64_MAX - 7u;
  expect_load64(fixture, NXLOADER_EBOUNDS, "phoff-overflow64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[0].p_align = 24;
  expect_load64(fixture, NXLOADER_EFORMAT, "non-power-align64");

  fixture = build_elf32(R_ARM_RELATIVE);
  header32 = (Elf32_Ehdr *)fixture;
  programs32 = (Elf32_Phdr *)(fixture + header32->e_phoff);
  programs32[1].p_vaddr++;
  expect_load32(fixture, NXLOADER_EFORMAT, "page-incongruent32");

  fixture = build_elf32(R_ARM_RELATIVE);
  header32 = (Elf32_Ehdr *)fixture;
  programs32 = (Elf32_Phdr *)(fixture + header32->e_phoff);
  programs32[0].p_flags |= UINT32_C(0x80000000);
  expect_load32(fixture, NXLOADER_EFORMAT, "unknown-segment-flags32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[1].p_vaddr = 0x1000;
  expect_load64(fixture, NXLOADER_EFORMAT, "overlap64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  temporary64 = programs64[0];
  programs64[0] = programs64[1];
  programs64[1] = temporary64;
  expect_load64(fixture, NXLOADER_EFORMAT, "unordered64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[0].p_filesz = 0x1800;
  programs64[0].p_memsz = 0x1800;
  programs64[1].p_offset = 0x1800;
  programs64[1].p_vaddr = 0x1800;
  programs64[1].p_filesz = 0x2800;
  programs64[1].p_memsz = 0x3800;
  expect_load64(fixture, NXLOADER_EPROTECT, "shared-page-wx64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[4] = programs64[2];
  header64->e_phnum = 5;
  expect_load64(fixture, NXLOADER_EFORMAT, "duplicate-dynamic64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[2].p_align = 3;
  expect_load64(fixture, NXLOADER_EFORMAT, "dynamic-align64");

  fixture = build_elf32(R_ARM_RELATIVE);
  header32 = (Elf32_Ehdr *)fixture;
  programs32 = (Elf32_Phdr *)(fixture + header32->e_phoff);
  programs32[2].p_offset += 8;
  expect_load32(fixture, NXLOADER_EFORMAT, "dynamic-file-map32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[3].p_vaddr = 0x8000;
  expect_load64(fixture, NXLOADER_EBOUNDS, "relro-bounds64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  programs64[4] = programs64[3];
  header64->e_phnum = 5;
  expect_load64(fixture, NXLOADER_EFORMAT, "duplicate-relro64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  memset(&programs64[4], 0, sizeof(programs64[4]));
  programs64[4].p_type = PT_LOAD;
  programs64[4].p_offset = 0x3000;
  programs64[4].p_vaddr = 0x8000;
  programs64[4].p_filesz = 0x1000;
  programs64[4].p_memsz = 0x1000;
  programs64[4].p_flags = PF_R;
  programs64[4].p_align = 0x1000;
  programs64[3].p_vaddr = 0x6000;
  programs64[3].p_memsz = 0x1000;
  header64->e_phnum = 5;
  expect_load64(fixture, NXLOADER_EFORMAT, "relro-gap64");

  fixture = build_elf32(R_ARM_RELATIVE);
  header32 = (Elf32_Ehdr *)fixture;
  programs32 = (Elf32_Phdr *)(fixture + header32->e_phoff);
  memset(&programs32[5], 0, sizeof(programs32[5]));
  programs32[5].p_type = PT_TLS;
  programs32[5].p_memsz = 8;
  header32->e_phnum = 6;
  expect_load32(fixture, NXLOADER_EUNSUPPORTED, "tls-segment32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  memset(&programs64[4], 0, sizeof(programs64[4]));
  programs64[4].p_type = PT_TLS;
  programs64[4].p_memsz = 16;
  header64->e_phnum = 5;
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "tls-segment64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  programs64 = (Elf64_Phdr *)(fixture + header64->e_phoff);
  memset(&programs64[4], 0, sizeof(programs64[4]));
  programs64[4].p_type = PT_INTERP;
  programs64[4].p_offset = 0x300;
  programs64[4].p_filesz = 8;
  programs64[4].p_memsz = 8;
  programs64[4].p_flags = PF_R;
  programs64[4].p_align = 1;
  memcpy(fixture + 0x300, "/linker", 8);
  header64->e_phnum = 5;
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "guest-interpreter64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  header64 = (Elf64_Ehdr *)fixture;
  header64->e_shoff = UINT64_MAX;
  header64->e_shnum = UINT16_MAX;
  header64->e_shentsize = 1;
  expect_load64(fixture, NXLOADER_OK, "sectionless64");

  fixture = build_elf32(R_ARM_RELATIVE);
  header32 = (Elf32_Ehdr *)fixture;
  header32->e_shoff = UINT32_MAX;
  header32->e_shnum = UINT16_MAX;
  header32->e_shentsize = 1;
  expect_load32(fixture, NXLOADER_OK, "sectionless32");
}

static void test_dynamic_contract(void) {
  unsigned char *fixture;
  Elf64_Dyn *dynamic64;
  Elf32_Dyn *dynamic32;

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_STRTAB, STRING_VMA);
  expect_load64(fixture, NXLOADER_EFORMAT, "duplicate-strtab64");

  fixture = build_elf32(R_ARM_RELATIVE);
  dynamic32 = find_dynamic32(fixture, DT_RELSZ);
  CHECK(dynamic32 != NULL);
  if (dynamic32)
    dynamic32->d_tag = DT_DEBUG;
  expect_load32(fixture, NXLOADER_EFORMAT, "partial-rel32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  dynamic64 = find_dynamic64(fixture, DT_HASH);
  CHECK(dynamic64 != NULL);
  if (dynamic64)
    dynamic64->d_tag = DT_DEBUG;
  expect_load64(fixture, NXLOADER_EFORMAT, "missing-hash64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  memcpy(fixture + STRING_VMA + STR_SONAME, "../evil", 8);
  expect_load64(fixture, NXLOADER_EFORMAT, "invalid-soname64");

  fixture = build_elf32(R_ARM_RELATIVE);
  dynamic32 = find_dynamic32(fixture, DT_NEEDED);
  CHECK(dynamic32 != NULL);
  if (dynamic32)
    dynamic32->d_un.d_val = 0;
  expect_load32(fixture, NXLOADER_EFORMAT, "empty-needed32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_NEEDED, STR_NEEDED);
  expect_load64(fixture, NXLOADER_EFORMAT, "duplicate-needed64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_RPATH, STR_NEEDED);
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "rpath64");

  fixture = build_elf32(R_ARM_RELATIVE);
  append_dynamic32(fixture, DT_RUNPATH, STR_NEEDED);
  expect_load32(fixture, NXLOADER_EUNSUPPORTED, "runpath32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_TEXTREL, 0);
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "textrel64");

  fixture = build_elf32(R_ARM_RELATIVE);
  append_dynamic32(fixture, DT_FLAGS, DF_TEXTREL);
  expect_load32(fixture, NXLOADER_EUNSUPPORTED, "flags-textrel32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_TLSDESC_PLT, 0);
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "tlsdesc64");

  fixture = build_elf32(R_ARM_RELATIVE);
  append_dynamic32(fixture, DT_ANDROID_REL, 0);
  expect_load32(fixture, NXLOADER_EUNSUPPORTED, "packed-rel32");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_RELR, 0);
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "relr64");

  fixture = build_elf64(R_AARCH64_RELATIVE);
  append_dynamic64(fixture, DT_REL, 0);
  expect_load64(fixture, NXLOADER_EUNSUPPORTED, "wrong-rel-model64");

  fixture = build_elf32(R_ARM_RELATIVE);
  append_dynamic32(fixture, DT_RELA, 0);
  expect_load32(fixture, NXLOADER_EUNSUPPORTED, "wrong-rela-model32");
}

static unsigned char *build_many_bucket_gnu_fixture(nxloader_arch arch) {
  unsigned char *fixture =
      arch == NXLOADER_ARCH_AARCH64 ? build_elf64(R_AARCH64_RELATIVE)
                                    : build_elf32(R_ARM_RELATIVE);
  uint32_t *gnu_hash;
  size_t bucket_base;
  size_t chain_base;
  size_t index;
  if (!fixture)
    return NULL;
  if (arch == NXLOADER_ARCH_AARCH64) {
    Elf64_Dyn *entry = find_dynamic64(fixture, DT_HASH);
    if (!entry) {
      free(fixture);
      return NULL;
    }
    entry->d_tag = DT_GNU_HASH;
    bucket_base = 6; /* Four header words plus one 64-bit bloom word. */
  } else {
    Elf32_Dyn *entry = find_dynamic32(fixture, DT_HASH);
    if (!entry) {
      free(fixture);
      return NULL;
    }
    entry->d_tag = DT_GNU_HASH;
    bucket_base = 5; /* Four header words plus one 32-bit bloom word. */
  }
  gnu_hash = (uint32_t *)(fixture + HASH_VMA);
  memset(gnu_hash, 0, 0x100);
  gnu_hash[0] = 32; /* Every bucket deliberately shares the same chain. */
  gnu_hash[1] = 1;
  gnu_hash[2] = 1;
  for (index = 0; index < 32; ++index)
    gnu_hash[bucket_base + index] = 1;
  chain_base = bucket_base + 32;
  for (index = 0; index < 8; ++index)
    gnu_hash[chain_base + index] = index == 7 ? 1u : 0u;
  return fixture;
}

static void test_linear_gnu_hash_scan(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  size_t arch_index;
  for (arch_index = 0; arch_index < sizeof(arches) / sizeof(arches[0]);
       ++arch_index) {
    nxloader_arch arch = arches[arch_index];
    unsigned char *fixture = build_many_bucket_gnu_fixture(arch);
    nxloader_config config;
    nxloader_module *module = NULL;
    nxloader_module_info info;
    test_log_capture capture;
    const char *expected_log = arch == NXLOADER_ARCH_AARCH64
                                   ? "AArch64 GNU hash scan: buckets=32 "
                                     "chain_entries=8 work_budget=5120"
                                   : "ARMv7 GNU hash scan: buckets=32 "
                                     "chain_entries=8 work_budget=5120";
    CHECK(fixture != NULL);
    memset(&capture, 0, sizeof(capture));
    nxloader_config_init(&config);
    config.expected_arch = arch;
    config.flags = NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH;
    config.max_file_size = 8u * 1024u * 1024u;
    config.max_image_size = 8u * 1024u * 1024u;
    config.log = capture_log;
    config.userdata = &capture;
    CHECK(nxloader_module_create(&config, &module) == NXLOADER_OK);
    if (fixture && module) {
      CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                        "many-bucket-gnu-hash") ==
            NXLOADER_OK);
      memset(&info, 0, sizeof(info));
      info.struct_size = sizeof(info);
      CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
      CHECK(info.symbol_count == 9);
      /* The old nested walk examined 32 * 8 chain entries. The loader's
       * diagnostic proves that the shared chain is now examined once. */
      CHECK(strstr(capture.text, expected_log) != NULL);
      CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
    }
    nxloader_module_destroy(module);
    free(fixture);
  }

  /* GNU hash metadata remains protected on ELF64 after the scan rewrite, and
   * the late validation failure must not commit an earlier relocation. */
  {
    unsigned char *fixture =
        build_many_bucket_gnu_fixture(NXLOADER_ARCH_AARCH64);
    Elf64_Rela *relocations =
        fixture ? (Elf64_Rela *)(fixture + RELOCATION_VMA) : NULL;
    nxloader_module *module = new_module(NXLOADER_ARCH_AARCH64, NULL);
    CHECK(fixture != NULL && relocations != NULL && module != NULL);
    if (fixture && relocations && module) {
      relocations[4].r_offset = HASH_VMA;
      CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                        "gnu64-metadata-atomic") ==
            NXLOADER_OK);
      CHECK(nxloader_module_relocate(module) == NXLOADER_EFORMAT);
      CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_RELATIVE, 8)) ==
            0);
      CHECK(read64(nxloader_module_vma_to_pointer(module, SLOT_ABSOLUTE64,
                                                   8)) == 0);
    }
    nxloader_module_destroy(module);
    free(fixture);
  }
}

static void test_many_needed_scaling(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  size_t arch_index;
  for (arch_index = 0; arch_index < sizeof(arches) / sizeof(arches[0]);
       ++arch_index) {
    nxloader_arch arch = arches[arch_index];
    unsigned char *fixture =
        build_many_needed_fixture(arch, MANY_NEEDED_COUNT, 0);
    nxloader_module *module = new_module(arch, NULL);
    nxloader_module_info info;
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module) {
      CHECK(nxloader_module_load_memory(module, fixture,
                                        MANY_NEEDED_FIXTURE_SIZE,
                                        "many-needed-unique") == NXLOADER_OK);
      memset(&info, 0, sizeof(info));
      info.struct_size = sizeof(info);
      CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
      CHECK(info.needed_count == MANY_NEEDED_COUNT);
      CHECK(nxloader_module_needed_count(module) == MANY_NEEDED_COUNT);
      CHECK(strcmp(nxloader_module_needed(module, 0), "lib00002047.so") == 0);
      CHECK(strcmp(nxloader_module_needed(module, MANY_NEEDED_COUNT / 2),
                   "lib00001023.so") == 0);
      CHECK(strcmp(nxloader_module_needed(module, MANY_NEEDED_COUNT - 1),
                   "lib00000000.so") == 0);
    }
    nxloader_module_destroy(module);
    free(fixture);

    fixture = build_many_needed_fixture(arch, MANY_NEEDED_COUNT, 1);
    module = new_module(arch, NULL);
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module)
      CHECK(nxloader_module_load_memory(module, fixture,
                                        MANY_NEEDED_FIXTURE_SIZE,
                                        "many-needed-duplicate") ==
            NXLOADER_EFORMAT);
    nxloader_module_destroy(module);
    free(fixture);
  }
}

static void test_dynamic_name_bounds_and_overlapping_offsets(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  size_t arch_index;
  for (arch_index = 0; arch_index < sizeof(arches) / sizeof(arches[0]);
       ++arch_index) {
    nxloader_arch arch = arches[arch_index];
    unsigned char *fixture = build_long_needed_fixture(
        arch, TEST_DYNAMIC_NAME_LIMIT, 1, 0);
    nxloader_module *module = new_module(arch, NULL);
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module) {
      CHECK(nxloader_module_load_memory(module, fixture,
                                        LONG_NEEDED_FIXTURE_SIZE,
                                        "dynamic-name-boundary-4096") ==
            NXLOADER_OK);
      CHECK(nxloader_module_needed_count(module) == 1);
      CHECK(nxloader_module_needed(module, 0) != NULL);
      if (nxloader_module_needed(module, 0))
        CHECK(strlen(nxloader_module_needed(module, 0)) ==
              TEST_DYNAMIC_NAME_LIMIT);
    }
    nxloader_module_destroy(module);
    free(fixture);

    fixture = build_long_needed_fixture(
        arch, TEST_DYNAMIC_NAME_LIMIT + 1, 1, 0);
    module = new_module(arch, NULL);
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module)
      CHECK(nxloader_module_load_memory(module, fixture,
                                        LONG_NEEDED_FIXTURE_SIZE,
                                        "dynamic-name-boundary-4097") ==
            NXLOADER_EFORMAT);
    nxloader_module_destroy(module);
    free(fixture);

    fixture = build_long_needed_fixture(
        arch, TEST_DYNAMIC_NAME_LIMIT, OVERLAPPING_NEEDED_COUNT, 0);
    module = new_module(arch, NULL);
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module) {
      const char *first;
      const char *middle;
      const char *last;
      CHECK(nxloader_module_load_memory(module, fixture,
                                        LONG_NEEDED_FIXTURE_SIZE,
                                        "overlapping-needed-offsets") ==
            NXLOADER_OK);
      CHECK(nxloader_module_needed_count(module) ==
            OVERLAPPING_NEEDED_COUNT);
      first = nxloader_module_needed(module, 0);
      middle = nxloader_module_needed(module, OVERLAPPING_NEEDED_COUNT / 2);
      last = nxloader_module_needed(module, OVERLAPPING_NEEDED_COUNT - 1);
      CHECK(first != NULL && middle != NULL && last != NULL);
      if (first && middle && last) {
        CHECK(strlen(first) == TEST_DYNAMIC_NAME_LIMIT);
        CHECK(strlen(middle) ==
              TEST_DYNAMIC_NAME_LIMIT - OVERLAPPING_NEEDED_COUNT / 2);
        CHECK(strlen(last) ==
              TEST_DYNAMIC_NAME_LIMIT - (OVERLAPPING_NEEDED_COUNT - 1));
      }
    }
    nxloader_module_destroy(module);
    free(fixture);

    fixture = build_long_needed_fixture(
        arch, TEST_DYNAMIC_NAME_LIMIT, OVERLAPPING_NEEDED_COUNT, 1);
    module = new_module(arch, NULL);
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module)
      CHECK(nxloader_module_load_memory(module, fixture,
                                        LONG_NEEDED_FIXTURE_SIZE,
                                        "overlapping-needed-duplicate") ==
            NXLOADER_EFORMAT);
    nxloader_module_destroy(module);
    free(fixture);
  }
}

static void test_many_segment_lookup_and_relro_scaling(void) {
  static const nxloader_arch arches[] = {NXLOADER_ARCH_AARCH64,
                                         NXLOADER_ARCH_ARMV7};
  size_t arch_index;
  for (arch_index = 0; arch_index < sizeof(arches) / sizeof(arches[0]);
       ++arch_index) {
    nxloader_arch arch = arches[arch_index];
    unsigned char *fixture = build_many_segment_fixture(arch, 0);
    nxloader_module *module = new_module_with_limits(
        arch, NULL, 0, 4096, 32u * 1024u * 1024u);
    nxloader_module_info info;
    size_t samples[] = {0, MANY_SEGMENT_COUNT / 2,
                        MANY_SEGMENT_COUNT - 1};
    size_t sample_index;
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module) {
      CHECK(nxloader_module_load_memory(module, fixture,
                                        MANY_SEGMENT_FIXTURE_SIZE,
                                        "many-segment-relro") == NXLOADER_OK);
      memset(&info, 0, sizeof(info));
      info.struct_size = sizeof(info);
      CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
      CHECK(info.segment_count == MANY_SEGMENT_COUNT + 1);
      CHECK(info.relocation_count == MANY_SEGMENT_COUNT);
      for (sample_index = 0;
           sample_index < sizeof(samples) / sizeof(samples[0]);
           ++sample_index) {
        uint64_t target_vma = many_segment_target_vma(samples[sample_index], 0);
        size_t width = arch == NXLOADER_ARCH_AARCH64 ? sizeof(uint64_t)
                                                     : sizeof(uint32_t);
        CHECK(nxloader_module_vma_to_pointer(module, target_vma, width) !=
              NULL);
        CHECK(nxloader_module_vma_to_pointer(module, target_vma + 8, 1) ==
              NULL);
      }
      CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
      for (sample_index = 0;
           sample_index < sizeof(samples) / sizeof(samples[0]);
           ++sample_index) {
        uint64_t target_vma = many_segment_target_vma(samples[sample_index], 0);
        const void *target = nxloader_module_vma_to_pointer(
            module, target_vma,
            arch == NXLOADER_ARCH_AARCH64 ? sizeof(uint64_t)
                                           : sizeof(uint32_t));
        CHECK(target != NULL);
        if (target && arch == NXLOADER_ARCH_AARCH64)
          CHECK(read64(target) == (uint64_t)(uintptr_t)info.mapping_base);
        else if (target)
          CHECK(read32(target) == (uint32_t)(uintptr_t)info.mapping_base);
      }
    }
    nxloader_module_destroy(module);
    free(fixture);

    fixture = build_many_segment_fixture(arch, 1);
    module = new_module_with_limits(arch, NULL, 0, 4096,
                                    32u * 1024u * 1024u);
    CHECK(fixture != NULL && module != NULL);
    if (fixture && module)
      CHECK(nxloader_module_load_memory(module, fixture,
                                        MANY_SEGMENT_FIXTURE_SIZE,
                                        "many-segment-relro-gap") ==
            NXLOADER_EFORMAT);
    nxloader_module_destroy(module);
    free(fixture);
  }
}

static void expect_arm_relocation_not_hookable(uint32_t type,
                                               nxloader_result expected,
                                               const char *name) {
  unsigned char *fixture = build_elf32(type);
  nxloader_module *module =
      new_module(NXLOADER_ARCH_ARMV7, skip_every_relocation);
  void *target;
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module) {
    nxloader_module_destroy(module);
    free(fixture);
    return;
  }
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE, name) ==
        NXLOADER_OK);
  target = nxloader_module_vma_to_pointer(module, SLOT_RELATIVE,
                                           sizeof(uint32_t));
  CHECK(target != NULL);
  CHECK(read32(target) == UINT32_C(0x1200));
  CHECK(nxloader_module_relocate(module) == expected);
  CHECK(read32(target) == UINT32_C(0x1200));
  nxloader_module_destroy(module);
  free(fixture);
}

static void test_unhookable_relocations_and_ifunc(void) {
  static const uint32_t arm_tls_types[] = {
      17, R_ARM_THM_TLS_CALL, R_ARM_THM_TLS_DESCSEQ16,
      R_ARM_THM_TLS_DESCSEQ32};
  static const char *const arm_tls_names[] = {
      "arm-tls-17", "arm-tls-93", "arm-tls-129", "arm-tls-130"};
  unsigned char *fixture64 = build_elf64(512);
  nxloader_module *module64 =
      new_module(NXLOADER_ARCH_AARCH64, skip_every_relocation);
  unsigned char *ifunc = build_elf32(R_ARM_RELATIVE);
  nxloader_module *ifunc_module =
      new_module(NXLOADER_ARCH_ARMV7, skip_every_relocation);
  Elf32_Rel *relocations;
  Elf32_Sym *symbols;
  Elf32_Dyn *rel_size;
  size_t index;

  CHECK(fixture64 != NULL && module64 != NULL);
  if (fixture64 && module64) {
    CHECK(nxloader_module_load_memory(module64, fixture64, FIXTURE_SIZE,
                                      "tls-reloc64") == NXLOADER_OK);
    CHECK(nxloader_module_relocate(module64) == NXLOADER_EUNSUPPORTED);
  }
  nxloader_module_destroy(module64);
  free(fixture64);

  for (index = 0; index < sizeof(arm_tls_types) / sizeof(arm_tls_types[0]);
       ++index) {
    expect_arm_relocation_not_hookable(arm_tls_types[index],
                                       NXLOADER_EUNSUPPORTED,
                                       arm_tls_names[index]);
  }
  expect_arm_relocation_not_hookable(R_ARM_IRELATIVE, NXLOADER_EUNSUPPORTED,
                                     "arm-irelative");
  expect_arm_relocation_not_hookable(999, NXLOADER_ERELOC,
                                     "arm-unknown-hook-skip");

  CHECK(ifunc != NULL && ifunc_module != NULL);
  if (ifunc && ifunc_module) {
    relocations = (Elf32_Rel *)(ifunc + RELOCATION_VMA);
    symbols = (Elf32_Sym *)(ifunc + SYMBOL_VMA);
    rel_size = find_dynamic32(ifunc, DT_RELSZ);
    CHECK(rel_size != NULL);
    relocations[0].r_offset = SLOT_ABSOLUTE32;
    relocations[0].r_info = ELF32_R_INFO(1, R_ARM_ABS32);
    symbols[1].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_GNU_IFUNC);
    write32(ifunc + SLOT_ABSOLUTE32, 4);
    if (rel_size)
      rel_size->d_un.d_val = sizeof(*relocations);
    CHECK(nxloader_module_load_memory(ifunc_module, ifunc, FIXTURE_SIZE,
                                      "arm-ifunc-hook-skip") ==
          NXLOADER_EUNSUPPORTED);
  }
  nxloader_module_destroy(ifunc_module);
  free(ifunc);
}

static void test_partial_cleanup_and_ownership(void) {
  unsigned char *bad = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *owned = build_elf64(R_AARCH64_RELATIVE);
  nxloader_module *bad_module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module *owned_module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module_info info;
  nxloader_registry *registry;
  nxloader_registry *ownership_registry = NULL;
  nxloader_registry_match match;
  nxloader_symbol symbol;
  nxloader_provider provider;
  char symbol_name[] = "owned-symbol";
  char provider_name[] = "owned-provider";

  memcpy(bad + STRING_VMA + STR_SONAME, "../bad", 7);
  CHECK(nxloader_module_load_memory(bad_module, bad, FIXTURE_SIZE,
                                    "cleanup-after-map") == NXLOADER_EFORMAT);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(bad_module, &info) == NXLOADER_OK);
  CHECK(info.state == NXLOADER_STATE_ERROR);
  CHECK(info.mapping_base == NULL && info.mapping_size == 0);
  CHECK(info.image_size == 0 && info.segment_count == 0);

  CHECK(nxloader_module_load_memory(owned_module, owned, FIXTURE_SIZE,
                                    "copied-input") == NXLOADER_OK);
  memset(owned, 0xa5, FIXTURE_SIZE);
  free(owned);
  owned = NULL;
  CHECK(strcmp(nxloader_module_soname(owned_module), "libsynthetic.so") == 0);
  CHECK(nxloader_module_relocate(owned_module) == NXLOADER_OK);
  registry = new_registry_with_host(TEST_HOST64);
  CHECK(nxloader_module_resolve(owned_module, registry, 0, NULL) ==
        NXLOADER_OK);
  CHECK(nxloader_module_finalize(owned_module) == NXLOADER_OK);

  memset(&symbol, 0, sizeof(symbol));
  memset(&provider, 0, sizeof(provider));
  symbol.name = symbol_name;
  symbol.address = 0x1234;
  provider.struct_size = sizeof(provider);
  provider.name = provider_name;
  provider.symbols = &symbol;
  provider.symbol_count = 1;
  CHECK(nxloader_registry_create(&ownership_registry) == NXLOADER_OK);
  CHECK(nxloader_registry_add_provider(ownership_registry, &provider, NULL) ==
        NXLOADER_OK);
  symbol_name[0] = 'X';
  provider_name[0] = 'X';
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  CHECK(nxloader_registry_lookup(ownership_registry, "owned-symbol", &match) ==
        NXLOADER_OK);
  CHECK(strcmp(match.provider, "owned-provider") == 0);

  nxloader_registry_destroy(ownership_registry);
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(owned_module);
  nxloader_module_destroy(bad_module);
  free(owned);
  free(bad);
}

static void exercise_mutated_fixture(const unsigned char *fixture,
                                     nxloader_arch arch) {
  nxloader_module *module = new_module(arch, NULL);
  nxloader_registry *registry = NULL;
  nxloader_result result = nxloader_module_load_memory(
      module, fixture, FIXTURE_SIZE, "deterministic-mutation");
  CHECK(result <= NXLOADER_OK && result >= NXLOADER_ECALLBACK);
  if (result == NXLOADER_OK) {
    result = nxloader_module_relocate(module);
    CHECK(result <= NXLOADER_OK && result >= NXLOADER_ECALLBACK);
    if (result == NXLOADER_OK) {
      CHECK(nxloader_registry_create(&registry) == NXLOADER_OK);
      result = nxloader_module_resolve(
          module, registry, NXLOADER_RESOLVE_ALLOW_UNRESOLVED, NULL);
      CHECK(result <= NXLOADER_OK && result >= NXLOADER_ECALLBACK);
      if (result == NXLOADER_OK)
        CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
    }
  }
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
}

static uint32_t mutation_random(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static void test_deterministic_mutation_sweep(void) {
  unsigned char *base64 = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *base32 = build_elf32(R_ARM_RELATIVE);
  unsigned char *scratch = (unsigned char *)malloc(FIXTURE_SIZE);
  uint32_t state = UINT32_C(0x4e584c44);
  size_t abi;
  size_t iteration;
  CHECK(base64 != NULL && base32 != NULL && scratch != NULL);
  if (!base64 || !base32 || !scratch) {
    free(scratch);
    free(base32);
    free(base64);
    return;
  }
  for (abi = 0; abi < 2; ++abi) {
    const unsigned char *base = abi == 0 ? base64 : base32;
    nxloader_arch arch = abi == 0 ? NXLOADER_ARCH_AARCH64
                                  : NXLOADER_ARCH_ARMV7;
    for (iteration = 0; iteration < 1024; ++iteration) {
      size_t mutation_count;
      size_t mutation;
      memcpy(scratch, base, FIXTURE_SIZE);
      mutation_count = 1u + mutation_random(&state) % 3u;
      for (mutation = 0; mutation < mutation_count; ++mutation) {
        size_t offset = mutation_random(&state) % FIXTURE_SIZE;
        unsigned char mask =
            (unsigned char)(1u << (mutation_random(&state) & 7u));
        scratch[offset] ^= mask;
      }
      exercise_mutated_fixture(scratch, arch);
    }
  }
  free(scratch);
  free(base32);
  free(base64);
}

static void test_truncated_inputs(void) {
  unsigned char *fixture64 = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *fixture32 = build_elf32(R_ARM_RELATIVE);
  size_t size;
  CHECK(fixture64 != NULL && fixture32 != NULL);
  for (size = 0; size < FIXTURE_SIZE; size += 113) {
    nxloader_module *module64 = new_module(NXLOADER_ARCH_AARCH64, NULL);
    nxloader_module *module32 = new_module(NXLOADER_ARCH_ARMV7, NULL);
    CHECK(nxloader_module_load_memory(module64, fixture64, size,
                                      "truncated64") != NXLOADER_OK);
    CHECK(nxloader_module_load_memory(module32, fixture32, size,
                                      "truncated32") != NXLOADER_OK);
    nxloader_module_destroy(module64);
    nxloader_module_destroy(module32);
  }
  free(fixture32);
  free(fixture64);
}

static void test_unaligned_memory_input(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *unaligned = (unsigned char *)malloc(FIXTURE_SIZE + 1);
  nxloader_module *module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  CHECK(fixture != NULL && unaligned != NULL && module != NULL);
  if (fixture && unaligned && module) {
    memcpy(unaligned + 1, fixture, FIXTURE_SIZE);
    CHECK(nxloader_module_load_memory(module, unaligned + 1, FIXTURE_SIZE,
                                      "unaligned-input") == NXLOADER_OK);
  }
  nxloader_module_destroy(module);
  free(unaligned);
  free(fixture);
}

static void test_large_load_alignment(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  Elf64_Ehdr *header = (Elf64_Ehdr *)fixture;
  Elf64_Phdr *programs = (Elf64_Phdr *)(fixture + header->e_phoff);
  nxloader_module *module = new_module(NXLOADER_ARCH_AARCH64, NULL);
  nxloader_module_info info;
  programs[0].p_align = 0x10000;
  programs[1].p_align = 0x10000;
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "aligned-64k") == NXLOADER_OK);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  CHECK(nxloader_module_get_info(module, &info) == NXLOADER_OK);
  CHECK(info.load_alignment == 0x10000);
  CHECK((((uintptr_t)info.mapping_base - info.minimum_vma) & 0xffffu) == 0);
  nxloader_module_destroy(module);
  free(fixture);
}


/* O mapeamento file-backed opcional e' otimizacao de residencia: a imagem tem
 * de sair BYTE A BYTE igual a' da copia anonima, e as paginas de texto tem de
 * ficar respaldadas pelo arquivo de verdade -- conferido em /proc/self/maps,
 * que nomeia o arquivo de um mapeamento file-backed e nao nomeia nada num
 * anonimo.  Um teste que so' olhasse o valor de retorno nao provaria nada:
 * quando o remapeamento e' recusado, a copia fica e tudo "passa" do mesmo
 * jeito. */
static int mapping_line_names_a_file(uintptr_t address) {
  FILE *maps = fopen("/proc/self/maps", "r");
  char line[512];
  int named = 0;
  if (!maps)
    return -1; /* sem /proc: o teste se declara inconclusivo, nao aprovado */
  while (fgets(line, sizeof(line), maps)) {
    unsigned long low = 0;
    unsigned long high = 0;
    if (sscanf(line, "%lx-%lx", &low, &high) != 2)
      continue;
    if ((unsigned long)address < low || (unsigned long)address >= high)
      continue;
    named = strchr(line, '/') != NULL;
    break;
  }
  fclose(maps);
  return named;
}

static void test_file_backed_text_is_opt_in_and_identical(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  char path[] = "/tmp/nxloader-file-backed-XXXXXX";
  int fd = mkstemp(path);
  nxloader_module *plain;
  nxloader_module *backed;
  nxloader_module_info plain_info;
  nxloader_module_info backed_info;
  int named;
  CHECK(fixture != NULL);
  CHECK(fd >= 0);
  CHECK(write(fd, fixture, FIXTURE_SIZE) == (ssize_t)FIXTURE_SIZE);
  CHECK(close(fd) == 0);

  /* a bandeira e' aceita pela validacao de configuracao */
  plain = new_module_with_options(NXLOADER_ARCH_AARCH64, NULL, 0, 4096);
  CHECK(nxloader_module_load_file(plain, path) == NXLOADER_OK);
  memset(&plain_info, 0, sizeof(plain_info));
  plain_info.struct_size = sizeof(plain_info);
  CHECK(nxloader_module_get_info(plain, &plain_info) == NXLOADER_OK);

  backed = new_module_with_options(NXLOADER_ARCH_AARCH64, NULL,
                                   NXLOADER_CONFIG_FILE_BACKED_TEXT, 4096);
  CHECK(nxloader_module_load_file(backed, path) == NXLOADER_OK);
  memset(&backed_info, 0, sizeof(backed_info));
  backed_info.struct_size = sizeof(backed_info);
  CHECK(nxloader_module_get_info(backed, &backed_info) == NXLOADER_OK);

  /* mesmo tamanho de imagem e mesmo conteudo */
  CHECK(plain_info.image_size == backed_info.image_size);
  CHECK(memcmp((const void *)plain_info.mapping_base,
               (const void *)backed_info.mapping_base,
               plain_info.image_size) == 0);

  /* o segmento R+X do fixture comeca no VMA 0 e tem 0x2000 bytes: paginas
   * inteiras, congruentes, dentro do arquivo -- tem de ter sido remapeado */
  named = mapping_line_names_a_file((uintptr_t)backed_info.mapping_base);
  if (named >= 0) {
    CHECK(named == 1);
    /* e o carregamento comum continua anonimo, senao o teste acima estaria
     * medindo outra coisa */
    CHECK(mapping_line_names_a_file((uintptr_t)plain_info.mapping_base) == 0);
  }

  nxloader_module_destroy(backed);
  nxloader_module_destroy(plain);

  /* inerte, e nao um erro, quando nao ha arquivo nenhum para mapear */
  {
    nxloader_module *from_memory = new_module_with_options(
        NXLOADER_ARCH_AARCH64, NULL, NXLOADER_CONFIG_FILE_BACKED_TEXT, 4096);
    CHECK(nxloader_module_load_memory(from_memory, fixture, FIXTURE_SIZE,
                                      "no-file") == NXLOADER_OK);
    nxloader_module_destroy(from_memory);
  }

  CHECK(unlink(path) == 0);
  free(fixture);
}

/* Com NXLOADER_CONFIG_FAR_HOOKS o mesmo alvo inalcancavel do teste de contrato
 * passa a ser hookavel: o emissor mapeia um pool auxiliar perto dele. O que NAO
 * pode mudar e' a honestidade do desvio — conferimos que a instrucao escrita no
 * alvo e' um `B` cujo deslocamento realmente alcanca o slot, em vez de confiar
 * no codigo de retorno. Sem essa checagem o teste passaria com um desvio que
 * aponta para o lugar errado. */
static void test_far_hooks_opt_in(void) {
  unsigned char *fixture = build_elf64(R_AARCH64_RELATIVE);
  Elf64_Ehdr *header = (Elf64_Ehdr *)fixture;
  Elf64_Phdr *programs = (Elf64_Phdr *)(fixture + header->e_phoff);
  nxloader_module *module = new_module_with_limits(
      NXLOADER_ARCH_AARCH64, NULL, NXLOADER_CONFIG_FAR_HOOKS, 16,
      160u * 1024u * 1024u);
  nxloader_registry *registry = NULL;
  uintptr_t local = 0;
  uintptr_t hook = 0;
  uintptr_t first_slot = 0;
  void *relro = NULL;
  long page_size = sysconf(_SC_PAGESIZE);
  char permissions[5] = {0};
  CHECK(fixture != NULL && module != NULL);
  if (!fixture || !module)
    goto cleanup;
  programs[1].p_memsz = UINT64_C(0x09000000);
  CHECK(nxloader_module_load_memory(module, fixture, FIXTURE_SIZE,
                                    "aarch64-far-hook") == NXLOADER_OK);
  CHECK(nxloader_module_relocate(module) == NXLOADER_OK);
  registry = new_registry_with_host(TEST_HOST64);
  CHECK(nxloader_module_resolve(module, registry, 0, NULL) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "local", &local) == NXLOADER_OK);
  CHECK(nxloader_module_find_export(module, "hook_target", &hook) ==
        NXLOADER_OK);
  relro = nxloader_module_vma_to_pointer(module, 0x2000, 1);
  CHECK(relro != NULL);
  /* sem a bandeira este mesmo caso devolve NXLOADER_EOVERFLOW */
  CHECK(nxloader_module_install_hook(module, hook, local, 4) == NXLOADER_OK);
  {
    uint32_t branch = read32((const void *)hook);
    CHECK((branch & UINT32_C(0xfc000000)) == UINT32_C(0x14000000));
    int32_t imm26 = (int32_t)(branch & UINT32_C(0x03ffffff));
    if (imm26 & (1 << 25))
      imm26 -= (1 << 26);
    uintptr_t slot = hook + (uintptr_t)((intptr_t)imm26 * 4);
    first_slot = slot;
    /* o slot tem de conter o par LDR X17,literal / BR X17 e o destino */
    CHECK(read32((const void *)slot) == UINT32_C(0x58000051));
    CHECK(read32((const void *)(slot + 4)) == UINT32_C(0xd61f0220));
    uintptr_t destination = 0;
    memcpy(&destination, (const void *)(slot + 8), sizeof(destination));
    CHECK(destination == local);
    CHECK(mapping_permissions((const void *)slot, permissions) &&
          strcmp(permissions, "r-xp") == 0 &&
          strchr(permissions, 'w') == NULL);
  }
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(relro, permissions) &&
        strcmp(permissions, "r--p") == 0 && strchr(permissions, 'w') == NULL &&
        strchr(permissions, 'x') == NULL);
  CHECK(nxloader_module_begin_patch(module) == NXLOADER_OK);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "rw-p") == 0 && strchr(permissions, 'x') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)first_slot, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  /* NXL090 auxiliary-pool invariant: far veneers remain RX at the public
   * patch boundary and return to RX after each bounded internal slot write. */
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions(relro, permissions) &&
        strcmp(permissions, "r--p") == 0 && strchr(permissions, 'w') == NULL &&
        strchr(permissions, 'x') == NULL);
  CHECK(nxloader_module_install_hook(module, hook, local, 4) == NXLOADER_OK);
  {
    uint32_t branch = read32((const void *)hook);
    int32_t imm26 = (int32_t)(branch & UINT32_C(0x03ffffff));
    uintptr_t second_slot;
    if (imm26 & (1 << 25))
      imm26 -= (1 << 26);
    second_slot = hook + (uintptr_t)((intptr_t)imm26 * 4);
    CHECK(second_slot == first_slot + 16);
    CHECK(read32((const void *)second_slot) == UINT32_C(0x58000051));
    CHECK(read32((const void *)(second_slot + 4)) == UINT32_C(0xd61f0220));
    memset(permissions, 0, sizeof(permissions));
    CHECK(mapping_permissions((const void *)second_slot, permissions) &&
          strcmp(permissions, "r-xp") == 0 &&
          strchr(permissions, 'w') == NULL);
  }
  CHECK(nxloader_module_finalize(module) == NXLOADER_OK);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)hook, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  memset(permissions, 0, sizeof(permissions));
  CHECK(mapping_permissions((const void *)first_slot, permissions) &&
        strcmp(permissions, "r-xp") == 0 && strchr(permissions, 'w') == NULL);
  CHECK(page_size > 0 && ((unsigned long)page_size &
                          ((unsigned long)page_size - 1ul)) == 0);
  CHECK(nxloader_module_begin_patch(module) == NXLOADER_OK);
  if (page_size > 0) {
    uintptr_t aux_page = first_slot & ~((uintptr_t)page_size - 1u);
    CHECK(munmap((void *)aux_page, (size_t)page_size) == 0);
    CHECK(nxloader_module_finalize(module) == NXLOADER_EPROTECT);
    CHECK(nxloader_module_get_state(module) == NXLOADER_STATE_ERROR);
    memset(permissions, 0, sizeof(permissions));
    CHECK(mapping_permissions((const void *)hook, permissions) &&
          strcmp(permissions, "rw-p") == 0 &&
          strchr(permissions, 'x') == NULL);
    CHECK(!mapping_permissions((const void *)first_slot, permissions));
  }

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
}

static int write_fuzz_seed(const char *directory, const char *name,
                           const unsigned char *data) {
  char path[1024];
  FILE *output;
  size_t written;
  int close_status;
  int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
  if (count < 0 || (size_t)count >= sizeof(path))
    return 0;
  output = fopen(path, "wb");
  if (!output)
    return 0;
  written = fwrite(data, 1, FIXTURE_SIZE, output);
  close_status = fclose(output);
  return written == FIXTURE_SIZE && close_status == 0;
}

static int write_fuzz_corpus(const char *directory) {
  unsigned char *fixture64 = build_elf64(R_AARCH64_RELATIVE);
  unsigned char *fixture32 = build_elf32(R_ARM_RELATIVE);
  int success = fixture64 && fixture32 &&
                write_fuzz_seed(directory, "synthetic-aarch64.so", fixture64) &&
                write_fuzz_seed(directory, "synthetic-armv7.so", fixture32);
  free(fixture32);
  free(fixture64);
  return success ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--write-fuzz-corpus") == 0)
    return write_fuzz_corpus(argv[2]);
  if (argc != 1) {
    fprintf(stderr, "usage: %s [--write-fuzz-corpus DIRECTORY]\n", argv[0]);
    return 2;
  }
  test_public_api_contract();
  test_missing_strong_jump_slot_contract();
  test_m11_log_reentrancy_guard();
  test_m11_relocation_and_alias_reentrancy();
  test_m11_initializer_preflight_and_reentrancy();
  test_m11_jni_onload_contract();
  test_m11_jni_onload_option_validation();
  test_arm_float_abi_contract();
  test_arm_data_relocations();
  test_arm_weak_rel32_uses_addend();
  test_arm_branch_textrel_and_codecs();
  test_arm_weak_branch_fallthrough();
  test_arm_branch_range_and_veneers();
  test_arm_veneer_capacity_is_atomic();
  test_thumb_veneer_pool_must_be_reachable();
  test_diagnostics_do_not_leak_mapping();
  test_registry_collisions();
  test_registry_fnv_collision_scaling();
  test_registry_public_name_bounds();
  test_elf64();
  test_aarch64_hook_veneer_contract();
  test_patch_preserves_executable_relro();
  test_aarch64_hook_pool_range_is_atomic();
  test_aarch64_relocation_contract();
  test_arm32_metadata_target_contract();
  test_elf32();
  test_bounds_and_atomic_relocation();
  test_arm_relocation_rejection_is_atomic();
  test_arm_symbol_type_rejection_is_pre_hook();
  test_header_and_segment_contract();
  test_dynamic_contract();
  test_linear_gnu_hash_scan();
  test_many_needed_scaling();
  test_dynamic_name_bounds_and_overlapping_offsets();
  test_many_segment_lookup_and_relro_scaling();
  test_unhookable_relocations_and_ifunc();
  test_partial_cleanup_and_ownership();
  test_truncated_inputs();
  test_unaligned_memory_input();
  test_large_load_alignment();
  test_file_backed_text_is_opt_in_and_identical();
  test_far_hooks_opt_in();
  test_deterministic_mutation_sweep();
  if (failures) {
    fprintf(stderr, "%d nxloader test(s) failed\n", failures);
    return 1;
  }
  puts("nxloader: all synthetic ELF32/ELF64 tests passed");
  return 0;
}
