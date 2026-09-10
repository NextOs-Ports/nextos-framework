/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXCOMPAT_REGISTRY_INTERNAL_H
#define NXCOMPAT_REGISTRY_INTERNAL_H

#include "nxcompat.h"

#define NXCOMPAT_REGISTRY_MAGIC UINT32_C(0x4e584352)

struct nxcompat_registry {
  uint32_t magic;
  uint64_t generation;
  nxcompat_capability_evidence evidence[NXCOMPAT_CAPABILITY_COUNT];
  int probe_controller_mapping;
  uint64_t probe_controller_mapping_generation;
  int has_graphics;
  int has_audio;
  int has_input;
  nxcompat_graphics_receipt graphics;
  nxcompat_audio_receipt audio;
  nxcompat_input_receipt input;
};

int nxcompat_registry_instance_valid(const nxcompat_registry *registry);
int nxcompat_registry_bounded_string(const char *value, size_t size);
int nxcompat_registry_evidence_satisfies(
    nxcompat_evidence_state actual, nxcompat_evidence_state minimum);
void nxcompat_registry_stage_evidence(
    nxcompat_registry *registry, nxcompat_capability_id capability_id,
    nxcompat_evidence_state state, nxcompat_source source,
    nxcompat_reason_code reason, uint64_t generation);
void nxcompat_registry_stage_boolean(
    nxcompat_registry *registry, nxcompat_capability_id capability_id,
    int present, nxcompat_evidence_state present_state, nxcompat_source source,
    nxcompat_reason_code present_reason, uint64_t generation);
void nxcompat_runtime_report_sanitize(nxcompat_runtime_report *report);

#endif /* NXCOMPAT_REGISTRY_INTERNAL_H */
