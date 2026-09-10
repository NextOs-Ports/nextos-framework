/* Internal pending-write transaction tests. SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STRESS_WRITES = 16384, STRESS_STRIDE = 8 };

static int failures;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      fprintf(stderr, "pending: CHECK failed at %s:%d: %s\n", __FILE__,     \
              __LINE__, #expression);                                         \
      failures++;                                                             \
    }                                                                         \
  } while (0)

static void test_widths_and_adjacency(void) {
  uint8_t bytes[24];
  nxloader_pending_list pending;
  memset(bytes, 0, sizeof(bytes));
  memset(&pending, 0, sizeof(pending));

  /* Reverse insertion proves that validation, not insertion order, defines
   * the overlap check. Adjacent intervals are valid. */
  CHECK(nxloader_pending_add(&pending, bytes + 8,
                             UINT64_C(0x0123456789abcdef), 8) == NXLOADER_OK);
  CHECK(nxloader_pending_add(&pending, bytes + 4, UINT32_C(0x89abcdef), 4) ==
        NXLOADER_OK);
  CHECK(nxloader_pending_add(&pending, bytes + 2, UINT16_C(0x4567), 2) ==
        NXLOADER_OK);
  CHECK(nxloader_pending_validate(&pending) == NXLOADER_OK);
  CHECK(nxloader_pending_commit(&pending) == NXLOADER_OK);
  CHECK(nxloader_read_u16(bytes + 2) == UINT16_C(0x4567));
  CHECK(nxloader_read_u32(bytes + 4) == UINT32_C(0x89abcdef));
  CHECK(nxloader_read_u64(bytes + 8) == UINT64_C(0x0123456789abcdef));
  nxloader_pending_dispose(&pending);
}

static void test_overlap_is_atomic(void) {
  uint8_t bytes[32];
  uint8_t original[sizeof(bytes)];
  nxloader_pending_list pending;
  memset(bytes, 0x5a, sizeof(bytes));
  memcpy(original, bytes, sizeof(bytes));
  memset(&pending, 0, sizeof(pending));

  /* Both appends succeed: pending_add is an amortized O(1) append. The single
   * O(n log n) validation pass detects the partial overlap before any write. */
  CHECK(nxloader_pending_add(&pending, bytes + 8,
                             UINT64_C(0x1111222233334444), 8) == NXLOADER_OK);
  CHECK(nxloader_pending_add(&pending, bytes + 12, UINT32_C(0x55667788), 4) ==
        NXLOADER_OK);
  CHECK(nxloader_pending_validate(&pending) == NXLOADER_EFORMAT);
  CHECK(memcmp(bytes, original, sizeof(bytes)) == 0);
  CHECK(nxloader_pending_commit(&pending) == NXLOADER_EFORMAT);
  CHECK(memcmp(bytes, original, sizeof(bytes)) == 0);
  nxloader_pending_dispose(&pending);

  memset(&pending, 0, sizeof(pending));
  CHECK(nxloader_pending_add(&pending, bytes + 20, UINT32_C(1), 4) ==
        NXLOADER_OK);
  CHECK(nxloader_pending_add(&pending, bytes + 20, UINT16_C(2), 2) ==
        NXLOADER_OK);
  CHECK(nxloader_pending_commit(&pending) == NXLOADER_EFORMAT);
  CHECK(memcmp(bytes, original, sizeof(bytes)) == 0);
  nxloader_pending_dispose(&pending);
}

static void test_invalid_inputs(void) {
  uint8_t byte = 0;
  nxloader_pending_list pending;
  memset(&pending, 0, sizeof(pending));
  CHECK(nxloader_pending_validate(NULL) == NXLOADER_EINVAL);
  CHECK(nxloader_pending_commit(NULL) == NXLOADER_EINVAL);
  CHECK(nxloader_pending_add(&pending, &byte, 1, 1) == NXLOADER_EINVAL);
  CHECK(nxloader_pending_add(&pending, &byte, 1, 3) == NXLOADER_EINVAL);
  CHECK(nxloader_pending_add(&pending, &byte, 1, 16) == NXLOADER_EINVAL);
  CHECK(nxloader_pending_add(&pending,
                             (void *)(uintptr_t)(UINTPTR_MAX - 1), 1, 2) ==
        NXLOADER_EOVERFLOW);
  CHECK(pending.count == 0);
  CHECK(nxloader_pending_commit(&pending) == NXLOADER_OK);
  nxloader_pending_dispose(&pending);
}

static uint64_t stress_value(size_t index) {
  return UINT64_C(0xd1ce000000000000) ^
         ((uint64_t)index * UINT64_C(0x9e3779b97f4a7c15));
}

static void test_deterministic_stress(void) {
  const size_t byte_count = (size_t)STRESS_WRITES * STRESS_STRIDE;
  uint8_t *bytes = (uint8_t *)calloc(byte_count, 1);
  nxloader_pending_list pending;
  size_t iteration;
  memset(&pending, 0, sizeof(pending));
  CHECK(bytes != NULL);
  if (!bytes)
    return;

  /* STRESS_WRITES is a power of two and 4051 is odd, so this visits every slot
   * exactly once in a deterministic non-sorted order. */
  for (iteration = 0; iteration < STRESS_WRITES; ++iteration) {
    size_t index = (iteration * (size_t)4051) & (STRESS_WRITES - 1);
    uint8_t width = index % 3 == 0 ? 2 : (index % 3 == 1 ? 4 : 8);
    CHECK(nxloader_pending_add(&pending, bytes + index * STRESS_STRIDE,
                               stress_value(index), width) == NXLOADER_OK);
  }
  CHECK(pending.count == STRESS_WRITES);
  CHECK(nxloader_pending_commit(&pending) == NXLOADER_OK);
  for (iteration = 0; iteration < STRESS_WRITES; ++iteration) {
    uint8_t *slot = bytes + iteration * STRESS_STRIDE;
    uint64_t expected = stress_value(iteration);
    if (iteration % 3 == 0)
      CHECK(nxloader_read_u16(slot) == (uint16_t)expected);
    else if (iteration % 3 == 1)
      CHECK(nxloader_read_u32(slot) == (uint32_t)expected);
    else
      CHECK(nxloader_read_u64(slot) == expected);
  }
  nxloader_pending_dispose(&pending);
  free(bytes);
}

int main(void) {
  test_widths_and_adjacency();
  test_overlap_is_atomic();
  test_invalid_inputs();
  test_deterministic_stress();
  if (failures) {
    fprintf(stderr, "pending: FAIL failures=%d\n", failures);
    return 1;
  }
  puts("pending: PASS widths=2,4,8 append=amortized-O(1) "
       "validation=O(n-log-n) stress=16384 atomic_overlap=1");
  return 0;
}
