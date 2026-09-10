/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_internal.h"

/* All nxgl-owned SDL, provider, environment and hint transitions are
 * serialized by one non-blocking process-global arbiter. Cross-component
 * ordering remains the bootstrap/adapter's responsibility; nxgl never reaches
 * into nxcompat's private arbiter. */
static volatile int nxgl_global_arbiter;

int nxgl_arbiter_try_acquire(void) {
#if defined(__GNUC__) || defined(__clang__)
  return __sync_lock_test_and_set(&nxgl_global_arbiter, 1) == 0;
#else
#error "nxgl requires GCC/Clang atomic builtins for its global arbiter"
#endif
}

void nxgl_arbiter_release(void) {
#if defined(__GNUC__) || defined(__clang__)
  __sync_lock_release(&nxgl_global_arbiter);
#endif
}
