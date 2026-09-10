#include "startup_compat.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "so_util.h"
#include "util.h"

#if defined(__i386__)

enum {
  ASM2_FIRST_CHECK_CALL_VMA = 0x005a9529u,
  ASM2_FIRST_CHECK_CALL_SIZE = 5u,
  ASM2_FIRST_CHECK_PENDING_VMA = 0x01eaf0a9u,
  ASM2_CONFIRM_BOX_SYS_SLOT_VMA = 0x01eabb54u,
  ASM2_CONFIRM_BOX_SYS_ACTIVE_OFFSET = 0x01ceu,
  ASM2_CONFIRM_BOX_SYS_CLOSING_OFFSET = 0x01d0u,
  ASM2_FIRST_PROFILE_GUARD_VMA = 0x0057144du,
  ASM2_FIRST_PROFILE_GUARD_SIZE = 21u,
};

static const unsigned char asm2_first_check_original[] = {
    0xe8, 0x52, 0xb9, 0xcf, 0xff,
};
static const unsigned char asm2_x86_nop5[] = {
    0x90, 0x90, 0x90, 0x90, 0x90,
};
static const unsigned char asm2_first_profile_original[] = {
    0x89, 0x3c, 0x24, 0x89, 0x07, 0xe8, 0x89,
    0x95, 0x00, 0x00, 0x8b, 0x77, 0x64, 0x85,
    0xf6, 0x0f, 0x84, 0xad, 0x00, 0x00, 0x00,
};
static const unsigned char asm2_first_profile_guarded[] = {
    0x89, 0x3c, 0x24, 0x89, 0x07, 0x8b, 0x77,
    0x64, 0x85, 0xf6, 0x0f, 0x84, 0xb2, 0x00,
    0x00, 0x00, 0xe8, 0x7e, 0x95, 0x00, 0x00,
};

static unsigned char *first_check_instruction;
static unsigned char *first_check_pending;
static uint32_t *confirm_box_sys_slot;
static int first_check_deferred;

static void write_instruction_bytes(unsigned char *instruction,
                                    const unsigned char *value,
                                    size_t size) {
  memcpy(instruction, value, size);
  __builtin___clear_cache((char *)instruction,
                          (char *)instruction + size);
}

static int write_runtime_instruction_bytes(unsigned char *instruction,
                                           const unsigned char *value,
                                           size_t size) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    page_size = 4096;

  const uintptr_t page_mask = (uintptr_t)page_size - 1u;
  uintptr_t first_page = (uintptr_t)instruction & ~page_mask;
  uintptr_t last_page =
      ((uintptr_t)instruction + size - 1u) & ~page_mask;
  size_t span = (size_t)(last_page - first_page) + (size_t)page_size;
  if (mprotect((void *)first_page, span,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR writable: %s\n",
                strerror(errno));
    return -1;
  }

  write_instruction_bytes(instruction, value, size);

  if (mprotect((void *)first_page, span, PROT_READ | PROT_EXEC) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR executable: %s\n",
                strerror(errno));
    return -1;
  }
  return 0;
}

int asm2_startup_compat_apply(void) {
  first_check_instruction =
      so_guest_vma(ASM2_FIRST_CHECK_CALL_VMA,
                   ASM2_FIRST_CHECK_CALL_SIZE);
  first_check_pending = so_guest_vma(ASM2_FIRST_CHECK_PENDING_VMA,
                                     sizeof(*first_check_pending));
  confirm_box_sys_slot = so_guest_vma(ASM2_CONFIRM_BOX_SYS_SLOT_VMA,
                                      sizeof(*confirm_box_sys_slot));
  unsigned char *first_profile_guard =
      so_guest_vma(ASM2_FIRST_PROFILE_GUARD_VMA,
                   ASM2_FIRST_PROFILE_GUARD_SIZE);
  if (!first_check_instruction || !first_check_pending ||
      !confirm_box_sys_slot || !first_profile_guard ||
      memcmp(first_check_instruction, asm2_first_check_original,
             sizeof(asm2_first_check_original)) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR x86 first-check\n");
    errno = EINVAL;
    return -1;
  }
  if (memcmp(first_profile_guard, asm2_first_profile_original,
             sizeof(asm2_first_profile_original)) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR x86 first-profile\n");
    errno = EINVAL;
    return -1;
  }

  /*
   * This is the same one-frame deferral as the ARM build.  Suppress only the
   * direct ConfirmBoxSYS builder call while the earlier offline notice owns
   * the singleton, then restore the exact versioned call bytes.
   */
  write_instruction_bytes(first_check_instruction, asm2_x86_nop5,
                          sizeof(asm2_x86_nop5));
  first_check_deferred = 1;
  debugPrintf("ASM2_STARTUP_PATCH_OK x86 first-check deferred\n");

  /*
   * Move the existing +0x64 NULL guard ahead of the unsafe helper.  Branch
   * and call targets are preserved byte-for-byte for the non-NULL path.
   */
  write_instruction_bytes(first_profile_guard,
                          asm2_first_profile_guarded,
                          sizeof(asm2_first_profile_guarded));
  debugPrintf("ASM2_STARTUP_PATCH_OK x86 first-profile registry guarded\n");
  return 0;
}

void asm2_startup_compat_tick(void) {
  if (!first_check_deferred || !first_check_pending ||
      !*first_check_pending)
    return;

  uintptr_t confirm_box = (uintptr_t)*confirm_box_sys_slot;
  if (confirm_box) {
    const unsigned char *state = (const unsigned char *)confirm_box;
    if (state[ASM2_CONFIRM_BOX_SYS_ACTIVE_OFFSET] ||
        state[ASM2_CONFIRM_BOX_SYS_CLOSING_OFFSET])
      return;
  }

  if (memcmp(first_check_instruction, asm2_x86_nop5,
             sizeof(asm2_x86_nop5)) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR x86 deferred instruction\n");
    first_check_deferred = 0;
    return;
  }
  if (write_runtime_instruction_bytes(first_check_instruction,
                                      asm2_first_check_original,
                                      sizeof(asm2_first_check_original)) != 0)
    return;
  *first_check_pending = 0;
  first_check_deferred = 0;
  debugPrintf("ASM2_STARTUP_PATCH_OK x86 first-check resumed\n");
}

#else

enum {
  ASM2_FIRST_CHECK_CALL_VMA = 0x00435a80u,
  ASM2_FIRST_CHECK_ORIGINAL_BL = 0xebf76281u,
  ASM2_ARM_NOP = 0xe1a00000u,
  ASM2_FIRST_CHECK_PENDING_VMA = 0x01352321u,
  ASM2_CONFIRM_BOX_SYS_SLOT_VMA = 0x0134f724u,
  ASM2_CONFIRM_BOX_SYS_ACTIVE_OFFSET = 0x01ceu,
  ASM2_CONFIRM_BOX_SYS_CLOSING_OFFSET = 0x01d0u,
  ASM2_FIRST_PROFILE_GUARD_VMA = 0x00419a60u,
  ASM2_FIRST_PROFILE_ORIGINAL_BL = 0xebffff9eu,
  ASM2_FIRST_PROFILE_ORIGINAL_LOAD = 0xe5967064u,
  ASM2_FIRST_PROFILE_ORIGINAL_COMPARE = 0xe3570000u,
  ASM2_FIRST_PROFILE_ORIGINAL_BRANCH = 0x0a000030u,
  ASM2_FIRST_PROFILE_GUARDED_LOAD = 0xe5967064u,
  ASM2_FIRST_PROFILE_GUARDED_COMPARE = 0xe3570000u,
  ASM2_FIRST_PROFILE_GUARDED_BRANCH = 0x0a000031u,
  ASM2_FIRST_PROFILE_GUARDED_BL = 0xebffff9bu,
};

static uint32_t *first_check_instruction;
static unsigned char *first_check_pending;
static uint32_t *confirm_box_sys_slot;
static int first_check_deferred;

static void write_instruction(uint32_t *instruction, uint32_t value) {
  *instruction = value;
  __builtin___clear_cache((char *)instruction, (char *)(instruction + 1));
}

static int write_runtime_instruction(uint32_t *instruction, uint32_t value) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    page_size = 4096;

  const uintptr_t page_mask = (uintptr_t)page_size - 1u;
  void *page = (void *)((uintptr_t)instruction & ~page_mask);
  if (mprotect(page, (size_t)page_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR writable: %s\n",
                strerror(errno));
    return -1;
  }

  write_instruction(instruction, value);

  if (mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR executable: %s\n",
                strerror(errno));
    return -1;
  }
  return 0;
}

int asm2_startup_compat_apply(void) {
  first_check_instruction =
      so_guest_vma(ASM2_FIRST_CHECK_CALL_VMA,
                   sizeof(*first_check_instruction));
  first_check_pending = so_guest_vma(ASM2_FIRST_CHECK_PENDING_VMA,
                                     sizeof(*first_check_pending));
  confirm_box_sys_slot = so_guest_vma(ASM2_CONFIRM_BOX_SYS_SLOT_VMA,
                                      sizeof(*confirm_box_sys_slot));
  uint32_t *first_profile_guard =
      so_guest_vma(ASM2_FIRST_PROFILE_GUARD_VMA, 4u * sizeof(uint32_t));
  if (!first_check_instruction || !first_check_pending ||
      !confirm_box_sys_slot || !first_profile_guard ||
      *first_check_instruction != ASM2_FIRST_CHECK_ORIGINAL_BL) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR first-check instruction=%08x\n",
                first_check_instruction
                    ? (unsigned)*first_check_instruction
                    : 0u);
    errno = EINVAL;
    return -1;
  }
  if (first_profile_guard[0] != ASM2_FIRST_PROFILE_ORIGINAL_BL ||
      first_profile_guard[1] != ASM2_FIRST_PROFILE_ORIGINAL_LOAD ||
      first_profile_guard[2] != ASM2_FIRST_PROFILE_ORIGINAL_COMPARE ||
      first_profile_guard[3] != ASM2_FIRST_PROFILE_ORIGINAL_BRANCH) {
    debugPrintf(
        "ASM2_STARTUP_PATCH_ERROR first-profile instructions=%08x/%08x/"
        "%08x/%08x\n",
        (unsigned)first_profile_guard[0], (unsigned)first_profile_guard[1],
        (unsigned)first_profile_guard[2], (unsigned)first_profile_guard[3]);
    errno = EINVAL;
    return -1;
  }

  /*
   * Offline startup can open the game-data notice and UI_FIRST_CHECK in the
   * same frame.  Both use the single ConfirmBoxSYS instance: the later check
   * overwrites the callback while the earlier notice stays visible, leaving
   * an OK button which can never dismiss its own message.  Suppress the later
   * builder only while the existing SYS box is active.  The per-frame tick
   * restores the guest's original BL as soon as that box has closed, so
   * UI_FIRST_CHECK is then created and handled by the original game code.
   */
  write_instruction(first_check_instruction, ASM2_ARM_NOP);
  first_check_deferred = 1;
  debugPrintf("ASM2_STARTUP_PATCH_OK first-check deferred\n");

  /*
   * A completely empty profile takes a one-time registry reset while the
   * object at +0x64 is legitimately NULL.  The 1.2.7d destructor already has
   * a NULL branch, but calls the helper which dereferences that object one
   * instruction too early.  Move the existing load/compare/branch before the
   * helper call.  The non-NULL path is unchanged; the NULL path now reaches
   * the guest's own cleanup target instead of reading address 0x0c.
   */
  first_profile_guard[0] = ASM2_FIRST_PROFILE_GUARDED_LOAD;
  first_profile_guard[1] = ASM2_FIRST_PROFILE_GUARDED_COMPARE;
  first_profile_guard[2] = ASM2_FIRST_PROFILE_GUARDED_BRANCH;
  first_profile_guard[3] = ASM2_FIRST_PROFILE_GUARDED_BL;
  __builtin___clear_cache((char *)first_profile_guard,
                          (char *)(first_profile_guard + 4));
  debugPrintf("ASM2_STARTUP_PATCH_OK first-profile registry guarded\n");
  return 0;
}

void asm2_startup_compat_tick(void) {
  if (!first_check_deferred || !first_check_pending ||
      !*first_check_pending)
    return;

  uintptr_t confirm_box = (uintptr_t)*confirm_box_sys_slot;
  if (confirm_box) {
    const unsigned char *state = (const unsigned char *)confirm_box;
    if (state[ASM2_CONFIRM_BOX_SYS_ACTIVE_OFFSET] ||
        state[ASM2_CONFIRM_BOX_SYS_CLOSING_OFFSET])
      return;
  }

  if (*first_check_instruction != ASM2_ARM_NOP) {
    debugPrintf("ASM2_STARTUP_PATCH_ERROR deferred instruction=%08x\n",
                (unsigned)*first_check_instruction);
    first_check_deferred = 0;
    return;
  }

  if (write_runtime_instruction(first_check_instruction,
                                ASM2_FIRST_CHECK_ORIGINAL_BL) != 0)
    return;
  /* The guest set this latch immediately before the call we suppressed.
   * Roll back that one incomplete attempt so the original update revisits the
   * callsite on the next frame and builds the dialog with its native context. */
  *first_check_pending = 0;
  first_check_deferred = 0;
  debugPrintf("ASM2_STARTUP_PATCH_OK first-check resumed\n");
}

#endif
