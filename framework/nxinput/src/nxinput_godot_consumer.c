/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_godot_consumer -- see include/nxinput_godot_consumer.h. */
#include "nxinput_godot_consumer.h"

#include <stdio.h>
#include <string.h>

const char *nxinput_godot_admit_result_name(nxinput_godot_admit_result r) {
  switch (r) {
    case NXINPUT_GODOT_ADMIT_OK:
      return "admitted";
    case NXINPUT_GODOT_ADMIT_BLOCKED_MAP:
      return "blocked-mapping";
    case NXINPUT_GODOT_ADMIT_BLOCKED_SETTER:
      return "blocked-setter";
    case NXINPUT_GODOT_ADMIT_BLOCKED_READBACK:
      return "blocked-readback";
    case NXINPUT_GODOT_ADMIT_BLOCKED_ANNOUNCE:
      return "blocked-announce";
    case NXINPUT_GODOT_ADMIT_INVALID:
    default:
      return "invalid";
  }
}

static int ops_valid(const nxinput_godot_engine_ops *ops) {
  return ops != 0 &&
         ops->api_version == NXINPUT_GODOT_CONSUMER_API_VERSION &&
         ops->struct_size == sizeof(*ops) && ops->set_mapping != 0 &&
         ops->read_identity != 0 && ops->read_logical_for_code != 0 &&
         ops->announce != 0 && ops->doctor != 0;
}

static void receipt_reason(nxinput_godot_consumer_receipt *receipt,
                           const char *text) {
  (void)snprintf(receipt->reason, sizeof receipt->reason, "%s", text);
}

static nxinput_godot_admit_result finish(
    const nxinput_godot_engine_ops *ops,
    nxinput_godot_consumer_receipt *local,
    nxinput_godot_consumer_receipt *out, nxinput_godot_admit_result result,
    const char *reason) {
  local->result = (uint8_t)result;
  receipt_reason(local, reason);
  if (result != NXINPUT_GODOT_ADMIT_OK && ops != 0 && ops->doctor != 0) {
    ops->doctor(ops->userdata, reason);
  }
  if (out != 0) {
    *out = *local;
  }
  return result;
}

nxinput_godot_admit_result nxinput_godot_consumer_admit(
    nxinput_godot_engine engine, const nxinput_godot_engine_ops *ops,
    const nxinput_godot_origin *origin, const nxinput_godot_caps *caps,
    const char *mapping, const nxinput_godot_probe *probes,
    unsigned int probe_count, nxinput_godot_consumer_receipt *receipt) {
  nxinput_godot_consumer_receipt local;
  char served[NXINPUT_GODOT_LINE_MAX];
  nxinput_godot_result mapping_result;
  unsigned int i;

  memset(&local, 0, sizeof local);
  local.api_version = NXINPUT_GODOT_CONSUMER_API_VERSION;
  local.struct_size = sizeof local;
  local.result = (uint8_t)NXINPUT_GODOT_ADMIT_INVALID;
  (void)nxinput_godot_evidence_init(&local.mapping_evidence);

  if (!ops_valid(ops) || probe_count > NXINPUT_GODOT_CONSUMER_MAX_PROBES ||
      (probe_count > 0u && probes == 0)) {
    return finish(ops_valid(ops) ? ops : 0, &local, receipt,
                  NXINPUT_GODOT_ADMIT_INVALID,
                  "the engine ops or probe list are structurally invalid");
  }

  /* 1-2. measure + resolve with the DECLARED origin. */
  mapping_result = nxinput_godot_serve(engine, origin, caps, mapping, served,
                                       sizeof served,
                                       &local.mapping_evidence);
  if (mapping_result != NXINPUT_GODOT_BYTE_INTACT &&
      mapping_result != NXINPUT_GODOT_CONVERTED) {
    return finish(ops, &local, receipt, NXINPUT_GODOT_ADMIT_BLOCKED_MAP,
                  local.mapping_evidence.reason[0]
                      ? local.mapping_evidence.reason
                      : "the mapping could not be resolved");
  }

  /* 3. the ENGINE's real setter. */
  if (ops->set_mapping(ops->userdata, served) != 0) {
    return finish(ops, &local, receipt, NXINPUT_GODOT_ADMIT_BLOCKED_SETTER,
                  "the engine refused the mapping");
  }

  /* 4. the ENGINE's real readback. This is the only readback that counts. */
  if (ops->read_identity(ops->userdata, local.engine_guid,
                         sizeof local.engine_guid, local.engine_name,
                         sizeof local.engine_name) != 0) {
    return finish(ops, &local, receipt, NXINPUT_GODOT_ADMIT_BLOCKED_READBACK,
                  "the engine did not report the pad identity back");
  }
  local.engine_readback = 1u;
  for (i = 0u; i < probe_count; i++) {
    int engine_logical =
        ops->read_logical_for_code(ops->userdata, probes[i].evdev_code);

    local.probes_checked++;
    if (engine_logical != probes[i].expected_logical) {
      char detail[160];

      (void)snprintf(detail, sizeof detail,
                     "engine disagreed on evdev 0x%x: engine=%d served=%d",
                     probes[i].evdev_code, engine_logical,
                     probes[i].expected_logical);
      return finish(ops, &local, receipt,
                    NXINPUT_GODOT_ADMIT_BLOCKED_READBACK, detail);
    }
    local.probes_agreed++;
  }
  if (probe_count > 0u && local.probes_agreed != probe_count) {
    return finish(ops, &local, receipt, NXINPUT_GODOT_ADMIT_BLOCKED_READBACK,
                  "the engine readback did not cover every probe");
  }

  /* 5. only now may the joypad exist for the game. */
  if (ops->announce(ops->userdata) != 0) {
    return finish(ops, &local, receipt, NXINPUT_GODOT_ADMIT_BLOCKED_ANNOUNCE,
                  "the engine refused to announce the joypad");
  }
  local.announced = 1u;
  return finish(ops, &local, receipt, NXINPUT_GODOT_ADMIT_OK,
                "engine accepted the mapping and confirmed it on readback");
}

int nxinput_godot_consumer_receipt_line(
    const nxinput_godot_consumer_receipt *receipt, char *out,
    size_t out_size) {
  int written;

  if (receipt == 0 || out == 0 || out_size == 0u ||
      receipt->api_version != NXINPUT_GODOT_CONSUMER_API_VERSION) {
    return -1;
  }
  written = snprintf(
      out, out_size,
      "NXINPUT-GODOT-CONSUMER-RECEIPT: engine=%s result=%s announced=%u "
      "engine_readback=%u probes=%u/%u engine_guid=%s mapping_result=%s "
      "declared_origin=%s provider=%s reason=%s",
      nxinput_godot_engine_name(
          (nxinput_godot_engine)receipt->mapping_evidence.engine),
      nxinput_godot_admit_result_name(
          (nxinput_godot_admit_result)receipt->result),
      (unsigned int)receipt->announced,
      (unsigned int)receipt->engine_readback, receipt->probes_agreed,
      receipt->probes_checked,
      receipt->engine_guid[0] ? receipt->engine_guid : "-",
      nxinput_godot_result_name(
          (nxinput_godot_result)receipt->mapping_evidence.result),
      nxinput_godot_domain_name(
          (nxinput_godot_domain)receipt->mapping_evidence.source_domain),
      receipt->mapping_evidence.provider[0]
          ? receipt->mapping_evidence.provider : "-",
      receipt->reason);
  return written > 0 && (size_t)written < out_size ? written : -1;
}
