#ifndef ASM2_OPENSL_BRIDGE_H
#define ASM2_OPENSL_BRIDGE_H

#include <stdint.h>

#include "bionic_compat.h"

/* OpenSL ES exports interface IDs as pointer-valued data objects.  The import
 * table therefore exports the address of each variable below, just like
 * libOpenSLES.so does on Android. */
extern const void *asm2_sl_iid_engine;
extern const void *asm2_sl_iid_bufferqueue;
extern const void *asm2_sl_iid_play;

struct asm2_opensl_stats {
  uint64_t players_created;
  uint64_t players_released;
  uint64_t workers_started;
  uint64_t workers_exited;
  uint64_t self_deferred_destroys;
};

void asm2_opensl_get_stats(struct asm2_opensl_stats *stats);

uint32_t ASM2_GUEST_PCS asm2_slCreateEngine(
    void **engine, uint32_t option_count, const void *options,
    uint32_t interface_count, const void *interface_ids,
    const uint32_t *interface_required);

#endif
