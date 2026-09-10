#define _GNU_SOURCE 1

#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>

#include "x86_runtime_compat.h"

int so_guest_phdr_view(const void **program_headers,
                       uint16_t *program_header_count,
                       uintptr_t *load_bias,
                       const char **path,
                       uintptr_t *mapped_size) {
  (void)program_headers;
  (void)program_header_count;
  (void)load_bias;
  (void)path;
  (void)mapped_size;
  return -1;
}

__attribute__((visibility("default")))
uint32_t glAsm2LazyProbe(uint32_t a, uint32_t b, uint32_t c,
                         uint32_t d) {
  return (a * 3u) ^ (b * 5u) ^ (c * 7u) ^ (d * 11u);
}

int main(void) {
  typedef uint32_t (*probe_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
  uintptr_t address = asm2_x86_gl_lazy_stub("glAsm2LazyProbe");
  if (!address) {
    fprintf(stderr, "x86 lazy GL stub allocation failed\n");
    return 1;
  }
  probe_fn probe = (probe_fn)address;
  uint32_t first = probe(0x12345678u, 0x01020304u, 0x11223344u,
                         0xa5a5a5a5u);
  uint32_t second = probe(3u, 5u, 7u, 11u);
  if (first != glAsm2LazyProbe(0x12345678u, 0x01020304u, 0x11223344u,
                              0xa5a5a5a5u) ||
      second != glAsm2LazyProbe(3u, 5u, 7u, 11u)) {
    fprintf(stderr, "x86 lazy GL cdecl preservation failed\n");
    return 2;
  }
  printf("ASM2_X86_GL_LAZY_OK address=%p values=%08x/%08x\n",
         (void *)address, first, second);
  return 0;
}
