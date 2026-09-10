#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_compat.h"
#include "so_util.h"

enum {
  DTOR_LOCK_VMA = 0x00bb21fcu,
  DTOR_UNLOCK_VMA = 0x00bb2284u,
  DELETING_DTOR_LOCK_VMA = 0x00bb23c4u,
  DELETING_DTOR_UNLOCK_VMA = 0x00bb244cu,
  CALLBACK_VMA = 0x00bb7c98u,
};

static uint32_t dtor_lock = 0xebff6f09u;
static uint32_t dtor_unlock = 0xebff6eebu;
static uint32_t deleting_dtor_lock = 0xebff6e97u;
static uint32_t deleting_dtor_unlock = 0xebff6e79u;
static uint32_t callback_prologue = 0xe92d4070u;

void debugPrintf(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
}

void *so_guest_vma(uintptr_t vma, size_t size) {
  if (size != sizeof(uint32_t))
    return NULL;
  switch (vma) {
  case DTOR_LOCK_VMA:
    return &dtor_lock;
  case DTOR_UNLOCK_VMA:
    return &dtor_unlock;
  case DELETING_DTOR_LOCK_VMA:
    return &deleting_dtor_lock;
  case DELETING_DTOR_UNLOCK_VMA:
    return &deleting_dtor_unlock;
  case CALLBACK_VMA:
    return &callback_prologue;
  default:
    return NULL;
  }
}

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    exit(1);
  }
}

int main(void) {
  require(asm2_audio_compat_apply() == 0,
          "accept exact 1.2.7d destructor and callback instructions");

  unsigned char context[4 + sizeof(void *)];
  memset(context, 0, sizeof(context));
  void *expected_mutex = (void *)(uintptr_t)0x12345678u;
  memcpy(context + 4, &expected_mutex, sizeof(expected_mutex));
  require(asm2_audio_compat_callback_mutex(&callback_prologue, context) ==
              expected_mutex,
          "dereference the guest callback context mutex wrapper at +4");
  require(asm2_audio_compat_callback_mutex(&dtor_lock, context) == NULL,
          "reject an unrecognized callback address");
  require(asm2_audio_compat_callback_mutex(&callback_prologue, NULL) == NULL,
          "reject a null callback context");

  puts("audio compat: exact layout and callback mutex resolver OK");
  return 0;
}
