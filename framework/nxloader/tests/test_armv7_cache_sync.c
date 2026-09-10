/*
 * Manual physical ARMv7 cache-coherency probe for M09-012.
 *
 * It owns one anonymous page, alternates only RW -> RX -> RW -> RX, clears
 * the exact instruction range after each write and executes two tiny
 * test-owned functions. It never loads a guest ELF or calls an initializer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__arm__)
#error "test_armv7_cache_sync.c must run on a real 32-bit ARM process"
#endif

typedef uint32_t (*probe_fn)(void);

static probe_fn function_at(uintptr_t address) {
  probe_fn function = NULL;
  if (sizeof(function) == sizeof(address))
    memcpy(&function, &address, sizeof(function));
  return function;
}

static void write_programs(uint8_t *page, uint8_t arm_value,
                           uint8_t thumb_value) {
  const uint32_t arm_program[] = {
      UINT32_C(0xe3a00000) | arm_value, /* mov r0,#value */
      UINT32_C(0xe12fff1e),             /* bx lr */
  };
  const uint16_t thumb_program[] = {
      (uint16_t)(UINT16_C(0x2000) | thumb_value), /* movs r0,#value */
      UINT16_C(0x4770),                          /* bx lr */
  };
  memcpy(page, arm_program, sizeof(arm_program));
  memcpy(page + 16, thumb_program, sizeof(thumb_program));
}

int main(void) {
  long page_size_value = sysconf(_SC_PAGESIZE);
  size_t page_size;
  uint8_t *page;
  probe_fn arm;
  probe_fn thumb;
  uint32_t first_arm;
  uint32_t first_thumb;
  uint32_t second_arm;
  uint32_t second_thumb;
  if (page_size_value <= 0) {
    fprintf(stderr, "armv7-cache-sync: invalid page size\n");
    return 1;
  }
  page_size = (size_t)page_size_value;
  page = (uint8_t *)mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    fprintf(stderr, "armv7-cache-sync: mmap failed: %s\n", strerror(errno));
    return 1;
  }

  arm = function_at((uintptr_t)page);
  thumb = function_at((uintptr_t)(page + 16) | (uintptr_t)1);
  if (!arm || !thumb) {
    fprintf(stderr, "armv7-cache-sync: function pointer ABI unsupported\n");
    (void)munmap(page, page_size);
    return 1;
  }

  write_programs(page, 42, 43);
  __builtin___clear_cache((char *)page, (char *)page + 20);
  if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {
    fprintf(stderr, "armv7-cache-sync: first RX transition failed: %s\n",
            strerror(errno));
    (void)munmap(page, page_size);
    return 1;
  }
  first_arm = arm();
  first_thumb = thumb();

  if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0) {
    fprintf(stderr, "armv7-cache-sync: RW transition failed: %s\n",
            strerror(errno));
    (void)munmap(page, page_size);
    return 1;
  }
  write_programs(page, 99, 100);
  __builtin___clear_cache((char *)page, (char *)page + 20);
  if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {
    fprintf(stderr, "armv7-cache-sync: second RX transition failed: %s\n",
            strerror(errno));
    (void)munmap(page, page_size);
    return 1;
  }
  second_arm = arm();
  second_thumb = thumb();

  if (munmap(page, page_size) != 0) {
    fprintf(stderr, "armv7-cache-sync: munmap failed: %s\n", strerror(errno));
    return 1;
  }
  if (first_arm != 42 || first_thumb != 43 || second_arm != 99 ||
      second_thumb != 100) {
    fprintf(stderr,
            "armv7-cache-sync: stale code arm=%u/%u thumb=%u/%u\n",
            first_arm, second_arm, first_thumb, second_thumb);
    return 1;
  }
  puts("armv7-cache-sync: PASS arm_rewrite=1 thumb_rewrite=1 "
       "wx_mapping=0 guest_elf_loaded=0 guest_initializers_executed=0");
  return 0;
}
