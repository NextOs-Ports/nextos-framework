/* SPDX-License-Identifier: GPL-3.0-or-later */
/* M11 cross-only lifecycle gate. The sectionless ELF is built entirely in
 * memory by this test; no external guest, device or network resource exists. */
#include "nxloader.h"

#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE 23
#endif
#ifndef PT_GNU_RELRO
#define PT_GNU_RELRO 0x6474e552
#endif

#if defined(__aarch64__)
#define M11_ARCH NXLOADER_ARCH_AARCH64
#define M11_LABEL "m11-aarch64-lifecycle-cross"
#define M11_JNI_VERSION INT32_C(0x00010006)
#define M11_STACK_ALIGNMENT 16u
#elif defined(__arm__) && UINTPTR_MAX == UINT32_MAX
#define M11_ARCH NXLOADER_ARCH_ARMV7
#define M11_LABEL "m11-armv7-lifecycle-cross"
#define M11_JNI_VERSION INT32_C(0x00010004)
#define M11_STACK_ALIGNMENT 8u
#else
#error "M11 lifecycle cross gate requires ARMv7 or AArch64"
#endif

enum {
  M11_FIXTURE_SIZE = 0x4000,
  M11_DT_INIT_VMA = 0x1000,
  M11_INIT_ARRAY_FN1_VMA = 0x1040,
  M11_INIT_ARRAY_FN2_VMA = 0x1080,
  M11_JNI_ONLOAD_VMA = 0x10c0,
  M11_STATE_VMA = 0x2000,
  M11_INIT_ARRAY_VMA = 0x2080,
  M11_DYNAMIC_VMA = 0x3100,
  M11_STRING_VMA = 0x3300,
  M11_SYMBOL_VMA = 0x3400,
  M11_HASH_VMA = 0x3500,
  M11_RELOCATION_VMA = 0x3600
};

typedef struct m11_guest_state {
  uint32_t call_count;
  uint32_t dt_init_order;
  uint32_t init_array1_order;
  uint32_t init_array2_order;
  uint32_t jni_order;
  uint32_t jni_calls;
  uint64_t java_vm;
  uint64_t reserved;
  uint64_t dt_init_sp;
  uint64_t init_array1_sp;
  uint64_t init_array2_sp;
  uint64_t jni_sp;
} m11_guest_state;

static size_t alias_calls;

#if defined(__aarch64__)
static void write64(void *pointer, uint64_t value) {
  memcpy(pointer, &value, sizeof(value));
}
#else
static void write32(void *pointer, uint32_t value) {
  memcpy(pointer, &value, sizeof(value));
}
#endif

static const char *block_public_jni_alias(void *userdata,
                                          const nxloader_module *module,
                                          const char *name) {
  (void)userdata;
  (void)module;
  if (strcmp(name, "JNI_OnLoad") == 0) {
    alias_calls++;
    return "m11_alias_must_not_reach_jni_dispatch";
  }
  return NULL;
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

static int mapping_range_has_wx(const void *base, size_t size) {
  FILE *maps = fopen("/proc/self/maps", "r");
  char line[512];
  uintptr_t range_start = (uintptr_t)base;
  uintptr_t range_end;
  if (!maps || size > UINTPTR_MAX - range_start) {
    if (maps)
      fclose(maps);
    return 1;
  }
  range_end = range_start + size;
  while (fgets(line, sizeof(line), maps)) {
    unsigned long long start;
    unsigned long long end;
    char permissions[5] = {0};
    if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3 &&
        (uintptr_t)start < range_end && range_start < (uintptr_t)end &&
        permissions[1] == 'w' && permissions[2] == 'x') {
      fclose(maps);
      return 1;
    }
  }
  fclose(maps);
  return 0;
}

#if defined(__aarch64__)
static void emit_guest_code(unsigned char *data) {
  static const uint32_t dt_init_code[] = {
      UINT32_C(0x10008000), /* adr x0, state */
      UINT32_C(0xb9400002), /* ldr w2, [x0] */
      UINT32_C(0x11000442), /* add w2, w2, #1 */
      UINT32_C(0xb9000002), /* str w2, [x0] */
      UINT32_C(0xb9000402), /* str w2, [x0, #4] */
      UINT32_C(0x910003e3), /* mov x3, sp */
      UINT32_C(0xf9001403), /* str x3, [x0, #40] */
      UINT32_C(0xd65f03c0)  /* ret */
  };
  static const uint32_t init_array1_code[] = {
      UINT32_C(0x10007e00), /* adr x0, state */
      UINT32_C(0xb9400002), /* ldr w2, [x0] */
      UINT32_C(0x11000442), /* add w2, w2, #1 */
      UINT32_C(0xb9000002), /* str w2, [x0] */
      UINT32_C(0xb9000802), /* str w2, [x0, #8] */
      UINT32_C(0x910003e3), /* mov x3, sp */
      UINT32_C(0xf9001803), /* str x3, [x0, #48] */
      UINT32_C(0xd65f03c0)  /* ret */
  };
  static const uint32_t init_array2_code[] = {
      UINT32_C(0x10007c00), /* adr x0, state */
      UINT32_C(0xb9400002), /* ldr w2, [x0] */
      UINT32_C(0x11000442), /* add w2, w2, #1 */
      UINT32_C(0xb9000002), /* str w2, [x0] */
      UINT32_C(0xb9000c02), /* str w2, [x0, #12] */
      UINT32_C(0x910003e3), /* mov x3, sp */
      UINT32_C(0xf9001c03), /* str x3, [x0, #56] */
      UINT32_C(0xd65f03c0)  /* ret */
  };
  static const uint32_t jni_code[] = {
      UINT32_C(0xaa0003e3), /* mov x3, x0 */
      UINT32_C(0xaa0103e4), /* mov x4, x1 */
      UINT32_C(0x100079c0), /* adr x0, state */
      UINT32_C(0xb9400002), /* ldr w2, [x0] */
      UINT32_C(0x11000442), /* add w2, w2, #1 */
      UINT32_C(0xb9000002), /* str w2, [x0] */
      UINT32_C(0xb9001002), /* str w2, [x0, #16] */
      UINT32_C(0xb9401405), /* ldr w5, [x0, #20] */
      UINT32_C(0x110004a5), /* add w5, w5, #1 */
      UINT32_C(0xb9001405), /* str w5, [x0, #20] */
      UINT32_C(0xf9000c03), /* str x3, [x0, #24] */
      UINT32_C(0xf9001004), /* str x4, [x0, #32] */
      UINT32_C(0x910003e5), /* mov x5, sp */
      UINT32_C(0xf9002005), /* str x5, [x0, #64] */
      UINT32_C(0x528000c0), /* mov w0, #6 */
      UINT32_C(0x72a00020), /* movk w0, #1, lsl #16 */
      UINT32_C(0xd65f03c0)  /* ret */
  };
  memcpy(data + M11_DT_INIT_VMA, dt_init_code, sizeof(dt_init_code));
  memcpy(data + M11_INIT_ARRAY_FN1_VMA, init_array1_code,
         sizeof(init_array1_code));
  memcpy(data + M11_INIT_ARRAY_FN2_VMA, init_array2_code,
         sizeof(init_array2_code));
  memcpy(data + M11_JNI_ONLOAD_VMA, jni_code, sizeof(jni_code));
}

static unsigned char *build_sectionless_fixture(void) {
  unsigned char *data = (unsigned char *)calloc(1, M11_FIXTURE_SIZE);
  Elf64_Ehdr *header;
  Elf64_Phdr *programs;
  Elf64_Dyn *dynamic;
  Elf64_Sym *symbols;
  Elf64_Rela *relocations;
  uint32_t *hash;
  size_t index = 0;
  static const char strings[] = "\0JNI_OnLoad\0";
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
  header->e_shoff = 0;
  header->e_shentsize = 0;
  header->e_shnum = 0;
  header->e_shstrndx = SHN_UNDEF;
  programs = (Elf64_Phdr *)(data + header->e_phoff);
  programs[0].p_type = PT_LOAD;
  programs[0].p_filesz = 0x2000;
  programs[0].p_memsz = 0x2000;
  programs[0].p_flags = PF_R | PF_X;
  programs[0].p_align = 0x1000;
  programs[1].p_type = PT_LOAD;
  programs[1].p_offset = 0x2000;
  programs[1].p_vaddr = 0x2000;
  programs[1].p_filesz = 0x2000;
  programs[1].p_memsz = 0x2000;
  programs[1].p_flags = PF_R | PF_W;
  programs[1].p_align = 0x1000;
  programs[2].p_type = PT_DYNAMIC;
  programs[2].p_offset = M11_DYNAMIC_VMA;
  programs[2].p_vaddr = M11_DYNAMIC_VMA;
  programs[2].p_filesz = 12 * sizeof(Elf64_Dyn);
  programs[2].p_memsz = programs[2].p_filesz;
  programs[2].p_flags = PF_R | PF_W;
  programs[2].p_align = 8;
  programs[3].p_type = PT_GNU_RELRO;
  programs[3].p_offset = 0x3000;
  programs[3].p_vaddr = 0x3000;
  programs[3].p_filesz = 0x1000;
  programs[3].p_memsz = 0x1000;
  programs[3].p_flags = PF_R;
  programs[3].p_align = 0x1000;
  emit_guest_code(data);
  memcpy(data + M11_STRING_VMA, strings, sizeof(strings));
  symbols = (Elf64_Sym *)(data + M11_SYMBOL_VMA);
  symbols[1].st_name = 1;
  symbols[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[1].st_other = STV_DEFAULT;
  symbols[1].st_shndx = 1;
  symbols[1].st_value = M11_JNI_ONLOAD_VMA;
  symbols[1].st_size = 17 * sizeof(uint32_t);
  hash = (uint32_t *)(data + M11_HASH_VMA);
  hash[0] = 1;
  hash[1] = 2;
  hash[2] = 1;
  hash[3] = 0;
  hash[4] = 0;
  relocations = (Elf64_Rela *)(data + M11_RELOCATION_VMA);
  relocations[0].r_offset = M11_INIT_ARRAY_VMA + 2 * sizeof(uint64_t);
  relocations[0].r_info = ELF64_R_INFO(0, R_AARCH64_RELATIVE);
  relocations[0].r_addend = M11_INIT_ARRAY_FN1_VMA;
  relocations[1].r_offset = M11_INIT_ARRAY_VMA + 3 * sizeof(uint64_t);
  relocations[1].r_info = ELF64_R_INFO(0, R_AARCH64_RELATIVE);
  relocations[1].r_addend = M11_INIT_ARRAY_FN2_VMA;
  dynamic = (Elf64_Dyn *)(data + M11_DYNAMIC_VMA);
#define M11_DYN64(tag_value, value)                                           \
  do {                                                                        \
    dynamic[index].d_tag = (tag_value);                                       \
    dynamic[index].d_un.d_val = (value);                                      \
    index++;                                                                  \
  } while (0)
  M11_DYN64(DT_STRTAB, M11_STRING_VMA);
  M11_DYN64(DT_STRSZ, sizeof(strings));
  M11_DYN64(DT_SYMTAB, M11_SYMBOL_VMA);
  M11_DYN64(DT_SYMENT, sizeof(Elf64_Sym));
  M11_DYN64(DT_HASH, M11_HASH_VMA);
  M11_DYN64(DT_RELA, M11_RELOCATION_VMA);
  M11_DYN64(DT_RELASZ, 2 * sizeof(Elf64_Rela));
  M11_DYN64(DT_RELAENT, sizeof(Elf64_Rela));
  M11_DYN64(DT_INIT, M11_DT_INIT_VMA);
  M11_DYN64(DT_INIT_ARRAY, M11_INIT_ARRAY_VMA);
  M11_DYN64(DT_INIT_ARRAYSZ, 4 * sizeof(uint64_t));
  M11_DYN64(DT_NULL, 0);
#undef M11_DYN64
  write64(data + M11_INIT_ARRAY_VMA, 0);
  write64(data + M11_INIT_ARRAY_VMA + sizeof(uint64_t), UINT64_MAX);
  return data;
}
#else
static void emit_guest_code(unsigned char *data) {
  static const uint32_t dt_init_code[] = {
      UINT32_C(0xe24f0008), /* sub r0, pc, #8 */
      UINT32_C(0xe2800a01), /* add r0, r0, #0x1000 */
      UINT32_C(0xe5902000), /* ldr r2, [r0] */
      UINT32_C(0xe2822001), /* add r2, r2, #1 */
      UINT32_C(0xe5802000), /* str r2, [r0] */
      UINT32_C(0xe5802004), /* str r2, [r0, #4] */
      UINT32_C(0xe1a0300d), /* mov r3, sp */
      UINT32_C(0xe5803028), /* str r3, [r0, #40] */
      UINT32_C(0xe12fff1e)  /* bx lr */
  };
  static const uint32_t init_array1_code[] = {
      UINT32_C(0xe24f0048), /* sub r0, pc, #0x48 */
      UINT32_C(0xe2800a01), /* add r0, r0, #0x1000 */
      UINT32_C(0xe5902000), /* ldr r2, [r0] */
      UINT32_C(0xe2822001), /* add r2, r2, #1 */
      UINT32_C(0xe5802000), /* str r2, [r0] */
      UINT32_C(0xe5802008), /* str r2, [r0, #8] */
      UINT32_C(0xe1a0300d), /* mov r3, sp */
      UINT32_C(0xe5803030), /* str r3, [r0, #48] */
      UINT32_C(0xe12fff1e)  /* bx lr */
  };
  static const uint32_t init_array2_code[] = {
      UINT32_C(0xe24f0088), /* sub r0, pc, #0x88 */
      UINT32_C(0xe2800a01), /* add r0, r0, #0x1000 */
      UINT32_C(0xe5902000), /* ldr r2, [r0] */
      UINT32_C(0xe2822001), /* add r2, r2, #1 */
      UINT32_C(0xe5802000), /* str r2, [r0] */
      UINT32_C(0xe580200c), /* str r2, [r0, #12] */
      UINT32_C(0xe1a0300d), /* mov r3, sp */
      UINT32_C(0xe5803038), /* str r3, [r0, #56] */
      UINT32_C(0xe12fff1e)  /* bx lr */
  };
  static const uint32_t jni_code[] = {
      UINT32_C(0xe1a03000), /* mov r3, r0 */
      UINT32_C(0xe1a0c001), /* mov r12, r1 */
      UINT32_C(0xe24f00d0), /* sub r0, pc, #0xd0 */
      UINT32_C(0xe2800a01), /* add r0, r0, #0x1000 */
      UINT32_C(0xe5902000), /* ldr r2, [r0] */
      UINT32_C(0xe2822001), /* add r2, r2, #1 */
      UINT32_C(0xe5802000), /* str r2, [r0] */
      UINT32_C(0xe5802010), /* str r2, [r0, #16] */
      UINT32_C(0xe5902014), /* ldr r2, [r0, #20] */
      UINT32_C(0xe2822001), /* add r2, r2, #1 */
      UINT32_C(0xe5802014), /* str r2, [r0, #20] */
      UINT32_C(0xe5803018), /* str r3, [r0, #24] */
      UINT32_C(0xe580c020), /* str r12, [r0, #32] */
      UINT32_C(0xe1a0300d), /* mov r3, sp */
      UINT32_C(0xe5803040), /* str r3, [r0, #64] */
      UINT32_C(0xe3000004), /* movw r0, #4 */
      UINT32_C(0xe3400001), /* movt r0, #1 */
      UINT32_C(0xe12fff1e)  /* bx lr */
  };
  memcpy(data + M11_DT_INIT_VMA, dt_init_code, sizeof(dt_init_code));
  memcpy(data + M11_INIT_ARRAY_FN1_VMA, init_array1_code,
         sizeof(init_array1_code));
  memcpy(data + M11_INIT_ARRAY_FN2_VMA, init_array2_code,
         sizeof(init_array2_code));
  memcpy(data + M11_JNI_ONLOAD_VMA, jni_code, sizeof(jni_code));
}

static unsigned char *build_sectionless_fixture(void) {
  unsigned char *data = (unsigned char *)calloc(1, M11_FIXTURE_SIZE);
  Elf32_Ehdr *header;
  Elf32_Phdr *programs;
  Elf32_Dyn *dynamic;
  Elf32_Sym *symbols;
  Elf32_Rel *relocations;
  uint32_t *hash;
  size_t index = 0;
  static const char strings[] = "\0JNI_OnLoad\0";
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
  header->e_flags = UINT32_C(0x05000400);
  header->e_ehsize = sizeof(*header);
  header->e_phoff = sizeof(*header);
  header->e_phentsize = sizeof(Elf32_Phdr);
  header->e_phnum = 4;
  header->e_shoff = 0;
  header->e_shentsize = 0;
  header->e_shnum = 0;
  header->e_shstrndx = SHN_UNDEF;
  programs = (Elf32_Phdr *)(data + header->e_phoff);
  programs[0].p_type = PT_LOAD;
  programs[0].p_filesz = 0x2000;
  programs[0].p_memsz = 0x2000;
  programs[0].p_flags = PF_R | PF_X;
  programs[0].p_align = 0x1000;
  programs[1].p_type = PT_LOAD;
  programs[1].p_offset = 0x2000;
  programs[1].p_vaddr = 0x2000;
  programs[1].p_filesz = 0x2000;
  programs[1].p_memsz = 0x2000;
  programs[1].p_flags = PF_R | PF_W;
  programs[1].p_align = 0x1000;
  programs[2].p_type = PT_DYNAMIC;
  programs[2].p_offset = M11_DYNAMIC_VMA;
  programs[2].p_vaddr = M11_DYNAMIC_VMA;
  programs[2].p_filesz = 12 * sizeof(Elf32_Dyn);
  programs[2].p_memsz = programs[2].p_filesz;
  programs[2].p_flags = PF_R | PF_W;
  programs[2].p_align = 4;
  programs[3].p_type = PT_GNU_RELRO;
  programs[3].p_offset = 0x3000;
  programs[3].p_vaddr = 0x3000;
  programs[3].p_filesz = 0x1000;
  programs[3].p_memsz = 0x1000;
  programs[3].p_flags = PF_R;
  programs[3].p_align = 0x1000;
  emit_guest_code(data);
  memcpy(data + M11_STRING_VMA, strings, sizeof(strings));
  symbols = (Elf32_Sym *)(data + M11_SYMBOL_VMA);
  symbols[1].st_name = 1;
  symbols[1].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
  symbols[1].st_other = STV_DEFAULT;
  symbols[1].st_shndx = 1;
  symbols[1].st_value = M11_JNI_ONLOAD_VMA;
  symbols[1].st_size = 18 * sizeof(uint32_t);
  hash = (uint32_t *)(data + M11_HASH_VMA);
  hash[0] = 1;
  hash[1] = 2;
  hash[2] = 1;
  hash[3] = 0;
  hash[4] = 0;
  write32(data + M11_INIT_ARRAY_VMA, 0);
  write32(data + M11_INIT_ARRAY_VMA + sizeof(uint32_t), UINT32_MAX);
  write32(data + M11_INIT_ARRAY_VMA + 2 * sizeof(uint32_t),
          M11_INIT_ARRAY_FN1_VMA);
  write32(data + M11_INIT_ARRAY_VMA + 3 * sizeof(uint32_t),
          M11_INIT_ARRAY_FN2_VMA);
  relocations = (Elf32_Rel *)(data + M11_RELOCATION_VMA);
  relocations[0].r_offset = M11_INIT_ARRAY_VMA + 2 * sizeof(uint32_t);
  relocations[0].r_info = ELF32_R_INFO(0, R_ARM_RELATIVE);
  relocations[1].r_offset = M11_INIT_ARRAY_VMA + 3 * sizeof(uint32_t);
  relocations[1].r_info = ELF32_R_INFO(0, R_ARM_RELATIVE);
  dynamic = (Elf32_Dyn *)(data + M11_DYNAMIC_VMA);
#define M11_DYN32(tag_value, value)                                           \
  do {                                                                        \
    dynamic[index].d_tag = (tag_value);                                       \
    dynamic[index].d_un.d_val = (value);                                      \
    index++;                                                                  \
  } while (0)
  M11_DYN32(DT_STRTAB, M11_STRING_VMA);
  M11_DYN32(DT_STRSZ, sizeof(strings));
  M11_DYN32(DT_SYMTAB, M11_SYMBOL_VMA);
  M11_DYN32(DT_SYMENT, sizeof(Elf32_Sym));
  M11_DYN32(DT_HASH, M11_HASH_VMA);
  M11_DYN32(DT_REL, M11_RELOCATION_VMA);
  M11_DYN32(DT_RELSZ, 2 * sizeof(Elf32_Rel));
  M11_DYN32(DT_RELENT, sizeof(Elf32_Rel));
  M11_DYN32(DT_INIT, M11_DT_INIT_VMA);
  M11_DYN32(DT_INIT_ARRAY, M11_INIT_ARRAY_VMA);
  M11_DYN32(DT_INIT_ARRAYSZ, 4 * sizeof(uint32_t));
  M11_DYN32(DT_NULL, 0);
#undef M11_DYN32
  return data;
}
#endif

static int run_lifecycle_gate(void) {
  unsigned char *fixture = build_sectionless_fixture();
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_registry *registry = NULL;
  nxloader_module_info info;
  nxloader_jni_onload_options jni;
  m11_guest_state *state = NULL;
  uintptr_t aliased_address = 0;
  int32_t returned_version = -1;
  uint32_t java_vm_token = UINT32_C(0x4d313156);
  char code_permissions[5] = {0};
  char data_permissions[5] = {0};
  char relro_permissions[5] = {0};
  int status = 1;
#if defined(__arm__)
  static const int32_t accepted_versions[] = {INT32_C(0x00010004)};
#else
  static const int32_t accepted_versions[] = {INT32_C(0x00010006)};
#endif
#define M11_REQUIRE(condition, message)                                       \
  do {                                                                        \
    if (!(condition)) {                                                       \
      fprintf(stderr, "%s: FAIL %s\n", M11_LABEL, (message));               \
      goto cleanup;                                                           \
    }                                                                         \
  } while (0)
  M11_REQUIRE(fixture != NULL, "fixture allocation");
  nxloader_config_init(&config);
  config.expected_arch = M11_ARCH;
  config.max_file_size = M11_FIXTURE_SIZE;
  config.max_image_size = M11_FIXTURE_SIZE;
  config.trampoline_pool_size = 0;
  config.alias = block_public_jni_alias;
  M11_REQUIRE(nxloader_module_create(&config, &module) == NXLOADER_OK,
              "module create");
  M11_REQUIRE(nxloader_registry_create(&registry) == NXLOADER_OK,
              "registry create");
  M11_REQUIRE(nxloader_module_load_memory(module, fixture, M11_FIXTURE_SIZE,
                                          M11_LABEL) == NXLOADER_OK,
              "load sectionless ELF");
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  M11_REQUIRE(nxloader_module_get_info(module, &info) == NXLOADER_OK,
              "module info");
  M11_REQUIRE(info.arch == M11_ARCH &&
                  info.state == NXLOADER_STATE_LOADED &&
                  info.segment_count == 2 && info.relocation_count == 2,
              "loaded metadata");
  M11_REQUIRE(!mapping_range_has_wx(info.mapping_base, info.mapping_size),
              "load mapping W+X");
  M11_REQUIRE(nxloader_module_relocate(module) == NXLOADER_OK,
              "relative relocation");
  M11_REQUIRE(nxloader_module_get_state(module) == NXLOADER_STATE_RELOCATED,
              "relocated state");
  M11_REQUIRE(nxloader_module_resolve(module, registry, 0, NULL) == NXLOADER_OK,
              "resolve empty registry");
  M11_REQUIRE(nxloader_module_get_state(module) == NXLOADER_STATE_RESOLVED,
              "resolved state");
  M11_REQUIRE(nxloader_module_finalize(module) == NXLOADER_OK,
              "finalize image");
  M11_REQUIRE(nxloader_module_get_state(module) == NXLOADER_STATE_FINALIZED,
              "finalized state");
  state = (m11_guest_state *)nxloader_module_vma_to_pointer(
      module, M11_STATE_VMA, sizeof(*state));
  M11_REQUIRE(state != NULL, "guest state pointer");
  M11_REQUIRE(mapping_permissions(nxloader_module_vma_to_pointer(
                                      module, M11_DT_INIT_VMA, 1),
                                  code_permissions) &&
                  strcmp(code_permissions, "r-xp") == 0,
              "code is not RX");
  M11_REQUIRE(mapping_permissions(state, data_permissions) &&
                  strcmp(data_permissions, "rw-p") == 0,
              "state is not RW");
  M11_REQUIRE(mapping_permissions(nxloader_module_vma_to_pointer(
                                      module, M11_DYNAMIC_VMA, 1),
                                  relro_permissions) &&
                  strcmp(relro_permissions, "r--p") == 0,
              "metadata is not RELRO");
  M11_REQUIRE(!mapping_range_has_wx(info.mapping_base, info.mapping_size),
              "final mapping W+X");

  M11_REQUIRE(nxloader_module_call_initializers(module) == NXLOADER_OK,
              "initializer dispatch");
  M11_REQUIRE(nxloader_module_get_state(module) == NXLOADER_STATE_INITIALIZED,
              "initialized state");
  M11_REQUIRE(state->call_count == 3 && state->dt_init_order == 1 &&
                  state->init_array1_order == 2 &&
                  state->init_array2_order == 3 && state->jni_order == 0,
              "DT_INIT/init_array order");
  M11_REQUIRE((state->dt_init_sp & (M11_STACK_ALIGNMENT - 1u)) == 0 &&
                  (state->init_array1_sp & (M11_STACK_ALIGNMENT - 1u)) == 0 &&
                  (state->init_array2_sp & (M11_STACK_ALIGNMENT - 1u)) == 0,
              "initializer entry stack alignment");
  M11_REQUIRE(nxloader_module_call_initializers(module) == NXLOADER_ESTATE,
              "second initializer call must fail");
  M11_REQUIRE(state->call_count == 3 && state->dt_init_order == 1 &&
                  state->init_array1_order == 2 &&
                  state->init_array2_order == 3,
              "initializers executed more than once");

  alias_calls = 0;
  M11_REQUIRE(nxloader_module_find_export(module, "JNI_OnLoad",
                                          &aliased_address) ==
                  NXLOADER_EUNRESOLVED &&
                  alias_calls == 1,
              "public alias control");
  memset(&jni, 0, sizeof(jni));
  jni.struct_size = sizeof(jni);
  jni.java_vm = &java_vm_token;
  jni.reserved = NULL;
  jni.accepted_versions = accepted_versions;
  jni.accepted_version_count =
      sizeof(accepted_versions) / sizeof(accepted_versions[0]);
  jni.flags = 0;
  M11_REQUIRE(nxloader_module_call_jni_onload(module, &jni,
                                              &returned_version) == NXLOADER_OK,
              "JNI_OnLoad dispatch");
  M11_REQUIRE(returned_version == M11_JNI_VERSION && alias_calls == 1,
              "literal JNI lookup/version allowlist");
  M11_REQUIRE(nxloader_module_get_state(module) == NXLOADER_STATE_READY,
              "READY state");
  M11_REQUIRE(state->call_count == 4 && state->jni_order == 4 &&
                  state->jni_calls == 1,
              "JNI order/count");
  M11_REQUIRE((uintptr_t)state->java_vm == (uintptr_t)&java_vm_token &&
                  state->reserved == 0,
              "JNI arguments");
  M11_REQUIRE((state->jni_sp & (M11_STACK_ALIGNMENT - 1u)) == 0,
              "JNI entry stack alignment");
  M11_REQUIRE(nxloader_module_call_jni_onload(module, &jni,
                                              &returned_version) ==
                  NXLOADER_ESTATE,
              "second JNI call must fail");
  M11_REQUIRE(state->call_count == 4 && state->jni_calls == 1,
              "JNI executed more than once");
  M11_REQUIRE(!mapping_range_has_wx(info.mapping_base, info.mapping_size),
              "post-lifecycle mapping W+X");
  status = 0;

cleanup:
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  free(fixture);
#undef M11_REQUIRE
  return status;
}

int main(void) {
  if (sizeof(uintptr_t) != (M11_ARCH == NXLOADER_ARCH_ARMV7 ? 4u : 8u) ||
      nxloader_process_arch() != M11_ARCH)
    return 2;
  if (run_lifecycle_gate() != 0)
    return 1;
  printf("%s: PASS sectionless=1 lifecycle=1 dt_init_order=1 "
         "init_array1_order=2 init_array2_order=3 init_array_entries=4 "
         "init_array_sentinels_ignored=2 initializers_exactly_once=1 "
         "jni_order=4 "
         "jni_version=0x%08x jni_literal_lookup=1 jni_exactly_once=1 "
         "ready=1 stack_align=%u wx_mapping=0 relro=1 "
         "test_owned_guest=1 external_guest=0 device_access=0 "
         "network_access=0 hardware_ran=0\n",
         M11_LABEL, (unsigned)M11_JNI_VERSION, M11_STACK_ALIGNMENT);
  return 0;
}
