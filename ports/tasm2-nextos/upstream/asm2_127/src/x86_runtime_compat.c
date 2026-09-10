#define _GNU_SOURCE 1

#include "x86_runtime_compat.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "so_util.h"

enum {
  ASM2_X86_GL_STUB_BYTES = 10,
  ASM2_X86_GL_MAX_STUBS = 160,
};

struct asm2_dtor_record {
  void (*function)(void *);
  void *argument;
  void *dso;
};

struct asm2_gl_slot {
  char *name;
  void *resolved;
};

static struct asm2_dtor_record *asm2_dtors;
static size_t asm2_dtor_count;
static size_t asm2_dtor_capacity;
static struct asm2_gl_slot asm2_gl_slots[ASM2_X86_GL_MAX_STUBS];
static size_t asm2_gl_slot_count;
static unsigned char *asm2_gl_stub_memory;
static size_t asm2_gl_stub_memory_size;
static pthread_mutex_t asm2_gl_lock = PTHREAD_MUTEX_INITIALIZER;

extern void asm2_x86_gl_lazy_common(void);

int asm2_x86_cxa_atexit(void (*function)(void *), void *argument, void *dso) {
  if (asm2_dtor_count == asm2_dtor_capacity) {
    size_t capacity = asm2_dtor_capacity ? asm2_dtor_capacity * 2u : 256u;
    void *records = realloc(asm2_dtors, capacity * sizeof(*asm2_dtors));
    if (!records)
      return -1;
    asm2_dtors = records;
    asm2_dtor_capacity = capacity;
  }
  asm2_dtors[asm2_dtor_count++] =
      (struct asm2_dtor_record){function, argument, dso};
  return 0;
}

size_t asm2_x86_recorded_destructor_count(void) {
  return asm2_dtor_count;
}

int asm2_x86_dl_iterate_phdr(
    int (*callback)(struct dl_phdr_info *, size_t, void *), void *data) {
  const void *program_headers = NULL;
  uint16_t program_header_count = 0;
  uintptr_t load_bias = 0;
  if (so_guest_phdr_view(&program_headers, &program_header_count, &load_bias,
                         NULL, NULL) == 0) {
    struct dl_phdr_info info = {0};
    info.dlpi_addr = (ElfW(Addr))load_bias;
    info.dlpi_name = "libtasm2.so";
    info.dlpi_phdr = (const ElfW(Phdr) *)program_headers;
    info.dlpi_phnum = program_header_count;
    int result = callback(&info, sizeof(info), data);
    if (result)
      return result;
  }
  return dl_iterate_phdr(callback, data);
}

void *asm2_x86_gl_resolve_now(const char *name) {
  if (!name || strncmp(name, "gl", 2) != 0)
    return NULL;
  void *function = SDL_GL_GetProcAddress(name);
  if (!function)
    function = dlsym(RTLD_DEFAULT, name);
  return function;
}

void *asm2_x86_gl_resolve_index(unsigned int index) {
  if (index >= asm2_gl_slot_count)
    _exit(87);
  void *function =
      __atomic_load_n(&asm2_gl_slots[index].resolved, __ATOMIC_ACQUIRE);
  if (function)
    return function;

  pthread_mutex_lock(&asm2_gl_lock);
  function = asm2_gl_slots[index].resolved;
  if (!function) {
    function = asm2_x86_gl_resolve_now(asm2_gl_slots[index].name);
    if (!function) {
      fprintf(stderr,
              "ASM2_X86_GL_RESOLVE_ERROR name=%s error=%s\n",
              asm2_gl_slots[index].name,
              SDL_GetError() ? SDL_GetError() : "?");
      fflush(stderr);
      _exit(88);
    }
    __atomic_store_n(&asm2_gl_slots[index].resolved, function,
                     __ATOMIC_RELEASE);
    fprintf(stderr, "ASM2_X86_GL_RESOLVE_OK name=%s function=%p\n",
            asm2_gl_slots[index].name, function);
  }
  pthread_mutex_unlock(&asm2_gl_lock);
  return function;
}

static int asm2_allocate_gl_stubs(void) {
  if (asm2_gl_stub_memory)
    return 0;
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    page_size = 4096;
  size_t required = ASM2_X86_GL_STUB_BYTES * ASM2_X86_GL_MAX_STUBS;
  asm2_gl_stub_memory_size =
      (required + (size_t)page_size - 1u) & ~((size_t)page_size - 1u);
  asm2_gl_stub_memory =
      mmap(NULL, asm2_gl_stub_memory_size,
           PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (asm2_gl_stub_memory == MAP_FAILED) {
    asm2_gl_stub_memory = NULL;
    return -1;
  }
  return 0;
}

uintptr_t asm2_x86_gl_lazy_stub(const char *name) {
  if (!name || strncmp(name, "gl", 2) != 0) {
    errno = EINVAL;
    return 0;
  }
  for (size_t index = 0; index < asm2_gl_slot_count; ++index)
    if (strcmp(asm2_gl_slots[index].name, name) == 0)
      return (uintptr_t)(asm2_gl_stub_memory +
                         index * ASM2_X86_GL_STUB_BYTES);
  if (asm2_gl_slot_count >= ASM2_X86_GL_MAX_STUBS ||
      asm2_allocate_gl_stubs() != 0) {
    errno = ENOSPC;
    return 0;
  }

  const size_t index = asm2_gl_slot_count;
  asm2_gl_slots[index].name = strdup(name);
  if (!asm2_gl_slots[index].name)
    return 0;
  unsigned char *stub =
      asm2_gl_stub_memory + index * ASM2_X86_GL_STUB_BYTES;
  stub[0] = 0x68; /* push imm32 */
  uint32_t encoded_index = (uint32_t)index;
  memcpy(stub + 1, &encoded_index, sizeof(encoded_index));
  stub[5] = 0xe9; /* jmp rel32 */
  intptr_t displacement =
      (intptr_t)asm2_x86_gl_lazy_common - ((intptr_t)stub + 10);
  int32_t encoded_displacement = (int32_t)displacement;
  if ((intptr_t)encoded_displacement != displacement) {
    free(asm2_gl_slots[index].name);
    asm2_gl_slots[index].name = NULL;
    errno = ERANGE;
    return 0;
  }
  memcpy(stub + 6, &encoded_displacement, sizeof(encoded_displacement));
  __builtin___clear_cache((char *)stub,
                          (char *)stub + ASM2_X86_GL_STUB_BYTES);
  ++asm2_gl_slot_count;
  return (uintptr_t)stub;
}
