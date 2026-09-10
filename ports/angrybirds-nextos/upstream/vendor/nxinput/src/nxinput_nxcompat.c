/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_nxcompat.h"

#include <string.h>

nxcompat_result_code nxinput_nxcompat_publish_context(
    nxcompat_registry *registry, const nxinput_context *input,
    nxcompat_input_receipt *published_receipt) {
  nxcompat_input_receipt receipt;
  nxinput_pad_state state;
  unsigned int reported_count;
  unsigned int observed_count = 0u;
  unsigned int slot;
  uint64_t topology_generation = 1u;
  nxcompat_result_code result;
  if (published_receipt)
    memset(published_receipt, 0, sizeof(*published_receipt));
  if (!registry || !input)
    return NXCOMPAT_INVALID;
  reported_count = nxinput_connected_count(input);
  if (reported_count > NXINPUT_MAX_PADS)
    return NXCOMPAT_FAILED;
  for (slot = 0u; slot < NXINPUT_MAX_PADS; ++slot) {
    memset(&state, 0, sizeof(state));
    if (!nxinput_get_pad(input, slot, &state) || state.slot != slot ||
        (state.connected != 0 && state.connected != 1) ||
        (state.connected && (state.instance_id < 0 || state.generation == 0u)))
      return NXCOMPAT_FAILED;
    if (UINT64_MAX - topology_generation < (uint64_t)state.generation)
      return NXCOMPAT_FAILED;
    topology_generation += (uint64_t)state.generation;
    if (state.connected)
      ++observed_count;
  }
  if (observed_count != reported_count || topology_generation == 0u)
    return NXCOMPAT_FAILED;
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.source = NXCOMPAT_SOURCE_NXINPUT;
  receipt.topology_generation = topology_generation;
  receipt.connected_count = observed_count;
  receipt.proof_flags =
      NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE |
      NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE |
      NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE;
  if (observed_count != 0u) {
    receipt.proof_flags |= NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE |
                           NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED;
    receipt.mapping_source = NXCOMPAT_INPUT_MAPPING_RUNTIME;
  }
  result = nxcompat_registry_publish_input(registry, &receipt);
  if (result == NXCOMPAT_OK && published_receipt)
    *published_receipt = receipt;
  return result;
}
