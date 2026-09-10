#include "audio_compat.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "so_util.h"
#include "util.h"

#if defined(__i386__)

enum {
  ASM2_AUDIO_COMPLETE_DTOR_CLEANUP_CALL_VMA = 0x01136733u,
  ASM2_AUDIO_DELETING_DTOR_CLEANUP_CALL_VMA = 0x011367bbu,
  ASM2_AUDIO_CLEANUP_LOCK_CALL_VMA = 0x011371afu,
  ASM2_AUDIO_CLEANUP_UNLOCK_CALL_VMA = 0x01137249u,
  ASM2_AUDIO_CALLBACK_VMA = 0x011380e0u,
  ASM2_AUDIO_CALLBACK_LOCK_CALL_VMA = 0x01138103u,
  ASM2_AUDIO_CALLBACK_UNLOCK_CALL_VMA = 0x0113811au,
};

struct asm2_audio_x86_call_check {
  uintptr_t vma;
  unsigned char expected[5];
};

static const unsigned char asm2_audio_x86_callback_prefix[] = {
    0x56, 0x53, 0xe8, 0x7c, 0x21, 0xfb, 0xfe, 0x81,
    0xc3, 0xf5, 0xb8, 0xd5, 0x00, 0x8d, 0x64, 0x24,
    0xec, 0x8b, 0x74, 0x24, 0x24, 0x85, 0xf6, 0x74,
    0x26, 0x8b, 0x46, 0x04, 0x85, 0xc0, 0x74, 0x08,
};

static void *asm2_audio_callback;

int asm2_audio_compat_apply(void) {
  /*
   * GCC i386 factors the cleanup body shared by the complete and deleting
   * destructors.  Validate both wrapper edges as well as the single lock /
   * unlock pair so this bridge is enabled only for the exact 1.2.7d layout.
   */
  static const struct asm2_audio_x86_call_check checks[] = {
      {ASM2_AUDIO_COMPLETE_DTOR_CLEANUP_CALL_VMA,
       {0xe8, 0x58, 0x0a, 0x00, 0x00}},
      {ASM2_AUDIO_DELETING_DTOR_CLEANUP_CALL_VMA,
       {0xe8, 0xd0, 0x09, 0x00, 0x00}},
      {ASM2_AUDIO_CLEANUP_LOCK_CALL_VMA,
       {0xe8, 0x7c, 0x01, 0xf7, 0xfe}},
      {ASM2_AUDIO_CLEANUP_UNLOCK_CALL_VMA,
       {0xe8, 0xf2, 0x00, 0xf7, 0xfe}},
      {ASM2_AUDIO_CALLBACK_LOCK_CALL_VMA,
       {0xe8, 0x28, 0xf2, 0xf6, 0xfe}},
      {ASM2_AUDIO_CALLBACK_UNLOCK_CALL_VMA,
       {0xe8, 0x21, 0xf2, 0xf6, 0xfe}},
  };

  for (size_t index = 0; index < sizeof(checks) / sizeof(checks[0]);
       ++index) {
    const unsigned char *instruction =
        so_guest_vma(checks[index].vma, sizeof(checks[index].expected));
    if (!instruction ||
        memcmp(instruction, checks[index].expected,
               sizeof(checks[index].expected)) != 0) {
      debugPrintf("ASM2_AUDIO_PATCH_ERROR x86 vma=%08lx\n",
                  (unsigned long)checks[index].vma);
      errno = EINVAL;
      return -1;
    }
  }

  unsigned char *callback =
      so_guest_vma(ASM2_AUDIO_CALLBACK_VMA,
                   sizeof(asm2_audio_x86_callback_prefix));
  if (!callback ||
      memcmp(callback, asm2_audio_x86_callback_prefix,
             sizeof(asm2_audio_x86_callback_prefix)) != 0) {
    debugPrintf("ASM2_AUDIO_PATCH_ERROR x86 callback\n");
    errno = EINVAL;
    return -1;
  }
  asm2_audio_callback = callback;
  debugPrintf("ASM2_AUDIO_PATCH_OK x86 teardown mutex handoff enabled\n");
  return 0;
}

void *asm2_audio_compat_callback_mutex(void *callback, void *context) {
  if (!asm2_audio_callback || callback != asm2_audio_callback || !context)
    return NULL;
  void *guest_mutex = NULL;
  memcpy(&guest_mutex, (const unsigned char *)context + 4,
         sizeof(guest_mutex));
  return guest_mutex;
}

#else

enum {
  /* Complete and deleting variants of the 1.2.7d audio destructor. */
  ASM2_AUDIO_DTOR_LOCK_VMA = 0x00bb21fcu,
  ASM2_AUDIO_DTOR_LOCK_BL = 0xebff6f09u,
  ASM2_AUDIO_DTOR_UNLOCK_VMA = 0x00bb2284u,
  ASM2_AUDIO_DTOR_UNLOCK_BL = 0xebff6eebu,
  ASM2_AUDIO_DELETING_DTOR_LOCK_VMA = 0x00bb23c4u,
  ASM2_AUDIO_DELETING_DTOR_LOCK_BL = 0xebff6e97u,
  ASM2_AUDIO_DELETING_DTOR_UNLOCK_VMA = 0x00bb244cu,
  ASM2_AUDIO_DELETING_DTOR_UNLOCK_BL = 0xebff6e79u,
  ASM2_AUDIO_CALLBACK_VMA = 0x00bb7c98u,
  ASM2_AUDIO_CALLBACK_PROLOGUE = 0xe92d4070u,
};

struct asm2_audio_instruction_patch {
  uintptr_t vma;
  uint32_t expected;
  uint32_t *instruction;
};

static void *asm2_audio_callback;

int asm2_audio_compat_apply(void) {
  struct asm2_audio_instruction_patch patches[] = {
      {ASM2_AUDIO_DTOR_LOCK_VMA, ASM2_AUDIO_DTOR_LOCK_BL, NULL},
      {ASM2_AUDIO_DTOR_UNLOCK_VMA, ASM2_AUDIO_DTOR_UNLOCK_BL, NULL},
      {ASM2_AUDIO_DELETING_DTOR_LOCK_VMA,
       ASM2_AUDIO_DELETING_DTOR_LOCK_BL, NULL},
      {ASM2_AUDIO_DELETING_DTOR_UNLOCK_VMA,
       ASM2_AUDIO_DELETING_DTOR_UNLOCK_BL, NULL},
  };

  for (size_t index = 0; index < sizeof(patches) / sizeof(patches[0]);
       ++index) {
    patches[index].instruction =
        so_guest_vma(patches[index].vma, sizeof(uint32_t));
    if (!patches[index].instruction ||
        *patches[index].instruction != patches[index].expected) {
      debugPrintf("ASM2_AUDIO_PATCH_ERROR vma=%08lx instruction=%08x\n",
                  (unsigned long)patches[index].vma,
                  patches[index].instruction
                      ? (unsigned)*patches[index].instruction
                      : 0u);
      errno = EINVAL;
      return -1;
    }
  }

  uint32_t *callback =
      so_guest_vma(ASM2_AUDIO_CALLBACK_VMA, sizeof(uint32_t));
  if (!callback || *callback != ASM2_AUDIO_CALLBACK_PROLOGUE) {
    debugPrintf("ASM2_AUDIO_PATCH_ERROR callback=%08x\n",
                callback ? (unsigned)*callback : 0u);
    errno = EINVAL;
    return -1;
  }
  asm2_audio_callback = callback;

  /*
   * Both guest destructors hold the mixer mutex across OpenSL Stop/Destroy.
   * The completion callback begins by taking that same mutex.  A conforming
   * bridge must not return from Destroy while such a callback can still touch
   * the context, but joining it with the mutex held deadlocks.  The OpenSL
   * bridge uses this verified layout to hand that mutex temporarily to the
   * callback, join it, and reacquire it before Destroy returns.  The guest
   * instructions remain untouched, preserving exclusion against every other
   * mixer method.
   */
  debugPrintf("ASM2_AUDIO_PATCH_OK teardown mutex handoff enabled\n");
  return 0;
}

void *asm2_audio_compat_callback_mutex(void *callback, void *context) {
  if (!asm2_audio_callback || callback != asm2_audio_callback || !context)
    return NULL;
  void *guest_mutex = NULL;
  memcpy(&guest_mutex, (const unsigned char *)context + 4,
         sizeof(guest_mutex));
  return guest_mutex;
}

#endif
