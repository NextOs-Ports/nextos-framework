/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_nxcompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static unsigned int fake_reported_count;
static nxinput_pad_state fake_slots[NXINPUT_MAX_PADS];
static int fake_slot_failure = -1;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

unsigned int nxinput_connected_count(const nxinput_context *input) {
  CHECK(input != NULL);
  return fake_reported_count;
}

int nxinput_get_pad(const nxinput_context *input, unsigned int slot,
                    nxinput_pad_state *state) {
  CHECK(input != NULL);
  if (!state || slot >= NXINPUT_MAX_PADS || (int)slot == fake_slot_failure)
    return 0;
  *state = fake_slots[slot];
  return 1;
}

static nxcompat_capability_evidence get_evidence(nxcompat_registry *registry,
                                                 uint32_t id) {
  nxcompat_capability_evidence evidence;
  memset(&evidence, 0, sizeof(evidence));
  CHECK(nxcompat_registry_get(registry, id, &evidence) == NXCOMPAT_OK);
  return evidence;
}

static void reset_fake_slots(void) {
  unsigned int slot;
  memset(fake_slots, 0, sizeof(fake_slots));
  for (slot = 0u; slot < NXINPUT_MAX_PADS; ++slot)
    fake_slots[slot].slot = slot;
  fake_reported_count = 0u;
  fake_slot_failure = -1;
}

int main(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_input_receipt receipt;
  nxcompat_capability_evidence evidence;
  const nxinput_context *fake_context =
      (const nxinput_context *)(const void *)&fake_slots[0];

  reset_fake_slots();
  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.source == NXCOMPAT_SOURCE_NXINPUT);
  CHECK(receipt.topology_generation == 1u);
  CHECK(receipt.connected_count == 0u);
  CHECK((receipt.proof_flags &
         NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE) != 0u);
  CHECK((receipt.proof_flags & NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE) != 0u);
  CHECK((receipt.proof_flags & NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE) != 0u);
  CHECK((receipt.proof_flags & NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED) == 0u);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_HOTPLUG).state ==
        NXCOMPAT_EVIDENCE_ABSENT);

  memset(&receipt, 0x5a, sizeof(receipt));
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, &receipt) ==
        NXCOMPAT_FAILED);
  CHECK(receipt.api_version == 0u);

  fake_slots[0].connected = 1;
  fake_slots[0].generation = 1u;
  fake_slots[0].instance_id = 77;
  (void)snprintf(fake_slots[0].name, sizeof(fake_slots[0].name), "%s",
                 "controller /home/private-user/private");
  (void)snprintf(fake_slots[0].guid, sizeof(fake_slots[0].guid), "%s",
                 "03000000deadbeef0000000000000000");
  fake_reported_count = 1u;
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.topology_generation == 2u);
  CHECK(receipt.connected_count == 1u);
  CHECK(receipt.mapping_source == NXCOMPAT_INPUT_MAPPING_RUNTIME);
  CHECK((receipt.proof_flags & NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED) != 0u);
  CHECK(get_evidence(registry,
                     NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED)
            .state == NXCOMPAT_EVIDENCE_ACTIVE);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING)
            .state == NXCOMPAT_EVIDENCE_OPENED);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING)
            .source == NXCOMPAT_SOURCE_NXINPUT);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING)
            .reason ==
        NXCOMPAT_REASON_INPUT_CONTROLLER_MAPPING_RESOLVED);

  fake_slots[0].connected = 0;
  fake_slots[0].generation = 2u;
  fake_reported_count = 0u;
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.topology_generation == 3u);
  CHECK(get_evidence(registry,
                     NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED)
            .state == NXCOMPAT_EVIDENCE_LOST);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);
  CHECK(get_evidence(registry, NXCOMPAT_CAPABILITY_INPUT_HOTPLUG).state ==
        NXCOMPAT_EVIDENCE_ABSENT);

  fake_reported_count = 1u;
  memset(&receipt, 0x5a, sizeof(receipt));
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, &receipt) ==
        NXCOMPAT_FAILED);
  CHECK(receipt.api_version == 0u);
  evidence = get_evidence(
      registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED);
  CHECK(evidence.state == NXCOMPAT_EVIDENCE_LOST && evidence.generation == 3u);

  fake_reported_count = 0u;
  fake_slot_failure = 2;
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, NULL) ==
        NXCOMPAT_FAILED);
  CHECK(nxinput_nxcompat_publish_context(NULL, fake_context, NULL) ==
        NXCOMPAT_INVALID);
  CHECK(nxinput_nxcompat_publish_context(registry, NULL, NULL) ==
        NXCOMPAT_INVALID);

  reset_fake_slots();
  for (unsigned int slot = 0u; slot < NXINPUT_MAX_PADS; ++slot)
    fake_slots[slot].generation = UINT32_MAX;
  CHECK(nxinput_nxcompat_publish_context(registry, fake_context, &receipt) ==
        NXCOMPAT_OK);
  CHECK(receipt.topology_generation ==
        UINT64_C(1) + (uint64_t)NXINPUT_MAX_PADS * UINT32_MAX);

  nxcompat_registry_destroy(registry);
  if (failures != 0)
    return 1;
  (void)fprintf(stdout, "nxinput nxcompat bridge tests passed\n");
  return 0;
}
