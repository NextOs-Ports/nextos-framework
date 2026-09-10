/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_provider -- see include/nxinput_provider.h. Pure. */
#include "nxinput_provider.h"
#include "nxinput_godot.h"

#include <stdio.h>
#include <string.h>

static const nxinput_provider_pin *runtime_pins;
static size_t runtime_pin_count;

void nxinput_provider_set_runtime_pins(const nxinput_provider_pin *pins,
                                       size_t count) {
  runtime_pins = pins;
  runtime_pin_count = pins != NULL ? count : 0u;
}

/*
 * The pin table. Every row is REPRODUCED by tests/providers: the sha256 is
 * the mapped object, the domain is what an independent measurement (source
 * patch transcription AND a uinput ordinal probe against the exact library)
 * established. tests/v5/test_v5_provider_pins.py fails when this table and
 * tests/providers/provider-manifest-v5.json disagree.
 *
 * A row may pin a sha256 to UNDECLARED on purpose: "these bytes are known,
 * their table is NOT measured yet". That is still UNKNOWN for rewrite.
 */
static const nxinput_provider_pin pins[] = {
    /* the pinned CFW 2.30.12 (CFW image 20250813): RetroArch-derived patch set. */
    {"8c4dc956e278546c96b06a6a7b0a2b8a1991e877e81e172fa1bed26a9ada453a",
     "pin-8c4dc956-sdl2-2.30.12", NXINPUT_SDL_API_2,
     NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED},
    /* the second pinned CFW image (2601.1): SDL 2.28.5 with the same ById
     * patch family; exact DSO MEASURED on the authorized aarch64 device
     * (tests/providers/receipts/probe-cfw2601-sdl2-2.28.5-on-aarch64.json). */
    {"40d0616f6b97d447f47e1e77c116d84bc4d3d7a5e33a50e2f4f472f9b250f46d",
     "pin-40d0616f-sdl2-2.28.5-cfw2601", NXINPUT_SDL_API_2,
     NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED},
    /* its 32-bit sibling: bytes known, table NOT measured => UNKNOWN. */
    {"4e95cde522cc7d355e9e93dc3763b831f962ac37f6967a77500991e998606504",
     "pin-4e95cde5-sdl2-2.28.5-cfw2601-arm32", NXINPUT_SDL_API_2,
     NXINPUT_SDL_DOMAIN_UNDECLARED},
    /* upstream-release device: upstream release-2.32.10 (SDL-release-2.32.10-0-g5d2495703). */
    {"4fd539cd0e2b2dc883beb8dedffa1f85212276b34e51b2a8bf98b4f8fd4f5cb4",
     "pin-4fd539cd-sdl2-2.32.10", NXINPUT_SDL_API_2,
     NXINPUT_SDL_DOMAIN_SDL2_EVDEV},
    /* Host (Arch) upstream release-2.32.70: the host harness provider. */
    {"5da9fd36889d02cb7271ff525c329189d390a8da489a25cb58e219fcbb3bfb54",
     "pin-5da9fd36-sdl2-2.32.70", NXINPUT_SDL_API_2, NXINPUT_SDL_DOMAIN_SDL2_EVDEV},
    /* private SDL2 fork (revision string pinned in the manifest):
     * no source pin; table MEASURED in-process on the authorized device
     * (tests/providers/receipts/probe-pin-1ac99b5c-sdl2-fork.json):
     * upstream high-first. */
    {"1ac99b5cea2844c7faa20d043811e8409ccfb30724a7314222a555b45a1917b6",
     "pin-1ac99b5c-sdl2-fork", NXINPUT_SDL_API_2,
     NXINPUT_SDL_DOMAIN_SDL2_EVDEV},
    /* private SDL3 (SDL-3.5.0-mali-350-improvements-v1-691). */
    {"eceaf5f97ca778ccb86e0389153c9a0a333557c338183123dc05f63a73912fbf",
     "pin-eceaf5f9-sdl3-3.5.0", NXINPUT_SDL_API_3,
     NXINPUT_SDL_DOMAIN_UNDECLARED},
};

const nxinput_provider_pin *nxinput_provider_pins(size_t *count) {
  if (count != NULL) {
    *count = sizeof pins / sizeof pins[0];
  }
  return pins;
}

static int hex64_valid(const char *s) {
  size_t i;
  if (s == NULL) {
    return 0;
  }
  for (i = 0; i < 64u; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return 0;
    }
  }
  return s[64] == '\0';
}

const nxinput_provider_pin *nxinput_provider_pin_lookup(const char *sha256) {
  size_t i;
  if (!hex64_valid(sha256)) {
    return NULL;
  }
  for (i = 0; i < sizeof pins / sizeof pins[0]; i++) {
    if (strcmp(pins[i].sha256, sha256) == 0) {
      return &pins[i];
    }
  }
  for (i = 0; i < runtime_pin_count; i++) {
    if (runtime_pins[i].sha256 != NULL &&
        strcmp(runtime_pins[i].sha256, sha256) == 0) {
      return &runtime_pins[i];
    }
  }
  return NULL;
}

static int evidence_valid(const nxinput_provider_evidence *e) {
  return e != NULL && e->api_version == NXINPUT_PROVIDER_API_VERSION &&
         e->struct_size == sizeof *e && e->api < NXINPUT_SDL_API_COUNT;
}

int nxinput_provider_resolve(const nxinput_provider_evidence *evidence,
                             uint32_t provider_generation,
                             nxinput_provider_descriptor *out) {
  const nxinput_provider_pin *pin;

  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->api_version = NXINPUT_PROVIDER_API_VERSION;
  out->struct_size = sizeof *out;
  out->provider_generation = provider_generation;
  out->method = NXINPUT_PROVIDER_METHOD_UNKNOWN;
  out->domain = NXINPUT_SDL_DOMAIN_UNDECLARED;
  out->driver = NXINPUT_PROVIDER_DRIVER_UNKNOWN;
  if (!evidence_valid(evidence)) {
    return -1;
  }
  out->evidence = *evidence;

  /* 0.11.1: the exported ById API is a MEASUREMENT INSTRUMENT, not a
   * table. Its presence (SDL2 only; the probe guarantees the four symbols
   * resolved in the SAME object as SDL_Init) says the table CAN be read
   * back per device; nxinput_provider_apply_measurement() decides the
   * domain from what was actually read. Until then the descriptor is
   * EXPORTED_API with an UNDECLARED domain: no rewrite, stock behaviour. */
  if (evidence->api == NXINPUT_SDL_API_2 && evidence->has_exported_bytable) {
    out->table_api_available = 1u;
  }
  /* 2. Bytes pinned to a reproduced table. The sha must be bound to the
   * object the process mapped, otherwise the file could have been replaced
   * after mmap and the pin would describe other bytes. A pin still decides
   * a provider whose ById API exists but has not been measured yet. */
  if (evidence->sha_bound_to_mapping) {
    pin = nxinput_provider_pin_lookup(evidence->sha256);
    if (pin != NULL && pin->api == evidence->api &&
        pin->domain != NXINPUT_SDL_DOMAIN_UNDECLARED) {
      out->method = NXINPUT_PROVIDER_METHOD_PINNED_ELF;
      out->domain = pin->domain;
      out->confidence = evidence->sha_bound_to_mapping == 1u ? 90u : 75u;
      out->pin_id = pin->id;
      return 0;
    }
    if (pin != NULL) {
      out->pin_id = pin->id; /* known bytes, unmeasured table */
    }
  }
  if (out->table_api_available) {
    out->method = NXINPUT_PROVIDER_METHOD_EXPORTED_API;
    out->domain = NXINPUT_SDL_DOMAIN_UNDECLARED; /* until measured */
    out->confidence = 0u;
    return 0;
  }
  /* 3. UNKNOWN. Nothing is rewritten; the provider keeps its stock path. */
  out->confidence = 0u;
  return 0;
}

static int domain_of_api(uint8_t api, nxinput_sdl_domain d) {
  switch (d) {
    case NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV:
    case NXINPUT_SDL_DOMAIN_SDL2_EVDEV:
    case NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED:
      return api == NXINPUT_SDL_API_2;
    case NXINPUT_SDL_DOMAIN_SDL3_EVDEV:
      return api == NXINPUT_SDL_API_3;
    default:
      return 0;
  }
}

int nxinput_provider_measurement_match(const nxinput_provider_measurement *m,
                                       const struct nxinput_godot_caps *caps,
                                       uint8_t api, uint8_t *matched,
                                       uint8_t *ambiguous) {
  int d;
  unsigned found = 0u;
  if (m == NULL || caps == NULL || matched == NULL || ambiguous == NULL ||
      m->buttons > NXINPUT_PROVIDER_MEASURE_BUTTONS ||
      m->axes > NXINPUT_PROVIDER_MEASURE_AXES ||
      m->hats > NXINPUT_PROVIDER_MEASURE_HATS) {
    return -1;
  }
  /* Preference among plans that number THIS pad identically (translation
   * between them is the identity): the major's upstream plan first, the
   * patched plan next, the legacy plan last. */
  static const nxinput_sdl_domain order[] = {
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NXINPUT_SDL_DOMAIN_SDL3_EVDEV,
      NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV,
      NXINPUT_SDL_DOMAIN_JOYDEV_LEGACY};
  size_t oi;
  *matched = NXINPUT_SDL_DOMAIN_UNDECLARED;
  *ambiguous = 0u;
  if (m->buttons == 0u && m->axes == 0u) {
    return 0; /* nothing measured: nothing matched */
  }
  for (oi = 0; oi < sizeof order / sizeof order[0]; oi++) {
    unsigned i; int ok = 1;
    d = (int)order[oi];
    if (!domain_of_api(api, (nxinput_sdl_domain)d)) {
      continue;
    }
    for (i = 0; ok && i < m->buttons; i++) {
      if (nxinput_sdl_button_code((nxinput_sdl_domain)d, caps, i) != m->button_code[i]) ok = 0;
    }
    if (ok && nxinput_sdl_button_code((nxinput_sdl_domain)d, caps, m->buttons) >= 0) ok = 0; /* plan names MORE buttons */
    for (i = 0; ok && i < m->axes; i++) {
      if (nxinput_sdl_axis_code((nxinput_sdl_domain)d, caps, i) != m->axis_code[i]) ok = 0;
    }
    if (ok && nxinput_sdl_axis_code((nxinput_sdl_domain)d, caps, m->axes) >= 0) ok = 0;
    for (i = 0; ok && i < NXINPUT_PROVIDER_MEASURE_HATS; i++) {
      int present = nxinput_sdl_hat_present((nxinput_sdl_domain)d, caps, i);
      int measured = i < m->hats;
      if (present != measured) ok = 0;
      if (ok && measured && m->hat_code[i] != (int)(NXINPUT_GODOT_ABS_HAT0X + i * 2u)) ok = 0;
    }
    if (ok) {
      if (found == 0u) *matched = (uint8_t)d;
      found++;
    }
  }
  *ambiguous = (uint8_t)found;
  return 0;
}

static int provider_is_decided(const nxinput_provider_descriptor *d) {
  return d->method == NXINPUT_PROVIDER_METHOD_PINNED_ELF ||
         d->method == NXINPUT_PROVIDER_METHOD_DECLARED_STATIC_SOURCE ||
         d->method == NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS;
}

int nxinput_provider_measurement_plans(const nxinput_provider_measurement *m,
                                       const struct nxinput_godot_caps *caps,
                                       uint8_t api, uint32_t *mask) {
  uint8_t matched = NXINPUT_SDL_DOMAIN_UNDECLARED, ambiguous = 0u;
  static const nxinput_sdl_domain order[] = {
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NXINPUT_SDL_DOMAIN_SDL3_EVDEV,
      NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV,
      NXINPUT_SDL_DOMAIN_JOYDEV_LEGACY};
  size_t oi;
  if (mask == NULL) {
    return -1;
  }
  *mask = 0u;
  if (nxinput_provider_measurement_match(m, caps, api, &matched, &ambiguous) != 0) {
    return -1;
  }
  if (ambiguous == 0u) {
    return 0;
  }
  /* Re-run the same predicate per plan to publish the SET, not just the
   * first hit: the caller decides against the set (a pin inside the set is
   * confirmed; a pin outside it is contradicted). */
  for (oi = 0; oi < sizeof order / sizeof order[0]; oi++) {
    int d = (int)order[oi];
    unsigned i; int ok = 1;
    if (!domain_of_api(api, (nxinput_sdl_domain)d)) {
      continue;
    }
    for (i = 0; ok && i < m->buttons; i++) {
      if (nxinput_sdl_button_code((nxinput_sdl_domain)d, caps, i) != m->button_code[i]) ok = 0;
    }
    if (ok && nxinput_sdl_button_code((nxinput_sdl_domain)d, caps, m->buttons) >= 0) ok = 0;
    for (i = 0; ok && i < m->axes; i++) {
      if (nxinput_sdl_axis_code((nxinput_sdl_domain)d, caps, i) != m->axis_code[i]) ok = 0;
    }
    if (ok && nxinput_sdl_axis_code((nxinput_sdl_domain)d, caps, m->axes) >= 0) ok = 0;
    for (i = 0; ok && i < NXINPUT_PROVIDER_MEASURE_HATS; i++) {
      int present = nxinput_sdl_hat_present((nxinput_sdl_domain)d, caps, i);
      int measured = i < m->hats;
      if (present != measured) ok = 0;
      if (ok && measured && m->hat_code[i] != (int)(NXINPUT_GODOT_ABS_HAT0X + i * 2u)) ok = 0;
    }
    if (ok) {
      *mask |= 1u << (unsigned)d;
    }
  }
  return 0;
}

int nxinput_provider_apply_measurement_set(nxinput_provider_descriptor *d,
                                           const nxinput_provider_measurement *m,
                                           uint32_t mask) {
  unsigned count = 0u, bit;
  uint8_t only = NXINPUT_SDL_DOMAIN_UNDECLARED;
  if (d == NULL || m == NULL) {
    return -1;
  }
  for (bit = 0u; bit < 32u; bit++) {
    if (mask & (1u << bit)) {
      if (nxinput_sdl_domain_plan((nxinput_sdl_domain)bit) == NULL ||
          !domain_of_api(d->evidence.api, (nxinput_sdl_domain)bit)) {
        mask &= ~(1u << bit); /* a plan of another major never counts */
        continue;
      }
      count++;
      only = (uint8_t)bit;
    }
  }
  if (count == 0u) {
    /* The provider's own table matches no transcribed plan: whatever the
     * symbols or the pin claimed, the table is UNKNOWN. Never rewrite. */
    d->method = NXINPUT_PROVIDER_METHOD_UNKNOWN;
    d->domain = NXINPUT_SDL_DOMAIN_UNDECLARED;
    d->measured = 1u;
    d->measurement_conflict = 1u;
    d->measured_ambiguous = 0u;
    d->confidence = 0u;
    return 0;
  }
  if (provider_is_decided(d)) {
    /* 0.11.4 (review 2, N1): a decided provider (pin, declared static source
     * or an earlier unambiguous measurement) is CONFIRMED when its domain is
     * inside the set this pad reproduces, and CONTRADICTED only by a table
     * that excludes it. It is never replaced by "the first plan in order". */
    if (mask & (1u << (unsigned)d->domain)) {
      d->measured = 1u;
      d->measurement_conflict = 0u;
      d->measured_ambiguous = (uint8_t)count;
      return 0;
    }
    d->method = NXINPUT_PROVIDER_METHOD_UNKNOWN;
    d->domain = NXINPUT_SDL_DOMAIN_UNDECLARED;
    d->measured = 1u;
    d->measurement_conflict = 1u;
    d->measured_ambiguous = (uint8_t)count;
    d->confidence = 0u;
    return 0;
  }
  if (count > 1u) {
    /* This pad is numbered identically by several plans (only BTN_* codes,
     * no discriminating low key): it cannot decide the provider. The
     * descriptor stays UNDECIDED -- stock mode, no rewrite -- and the next
     * admitted pad gets its own measurement. (N1: deciding here by plan
     * order silenced the CFW line of the next, discriminating pad.) */
    d->measured = 0u;
    d->measurement_conflict = 0u;
    d->measured_ambiguous = (uint8_t)count;
    return 0;
  }
  d->method = NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS;
  d->domain = only;
  d->measured = 1u;
  d->measurement_conflict = 0u;
  d->measured_ambiguous = 1u;
  d->confidence = 99u;
  return 0;
}

int nxinput_provider_apply_measurement(nxinput_provider_descriptor *d,
                                       const nxinput_provider_measurement *m,
                                       uint8_t matched, uint8_t ambiguous) {
  if (d == NULL || m == NULL) {
    return -1;
  }
  /* Compatibility entry (0.11.1 signature): the caller names only the FIRST
   * matching plan and the count. Several plans => the set is unknown here,
   * so nothing is decided and nothing decided is touched (N1: ambiguity
   * never decides). One plan => a set of one. None => conflict. */
  if (ambiguous > 1u) {
    d->measured_ambiguous = ambiguous;
    if (!provider_is_decided(d)) {
      d->measured = 0u;
      d->measurement_conflict = 0u;
    }
    return 0;
  }
  return nxinput_provider_apply_measurement_set(
      d, m, matched != NXINPUT_SDL_DOMAIN_UNDECLARED && ambiguous == 1u
                ? (1u << (unsigned)matched) : 0u);
}

int nxinput_provider_shared_core(const nxinput_provider_descriptor *sdl2,
                                 const nxinput_provider_descriptor *sdl3) {
  if (sdl2 == NULL || sdl3 == NULL) return 0;
  if (sdl2->evidence.api != NXINPUT_SDL_API_2 || sdl3->evidence.api != NXINPUT_SDL_API_3) return 0;
  return sdl2->evidence.compat_over_sdl3 ? 1 : 0;
}

int nxinput_provider_same_instance(const nxinput_provider_descriptor *a,
                                   const nxinput_provider_descriptor *b) {
  if (a == NULL || b == NULL) {
    return 0;
  }
  if (a->evidence.api != b->evidence.api) {
    return 0;
  }
  if (a->provider_generation != b->provider_generation) {
    return 0;
  }
  return strcmp(a->evidence.sha256, b->evidence.sha256) == 0 &&
         a->evidence.sha256[0] != '\0';
}

int nxinput_provider_allows_rewrite(const nxinput_provider_descriptor *d) {
  return d != NULL && d->method != NXINPUT_PROVIDER_METHOD_UNKNOWN &&
         d->domain != NXINPUT_SDL_DOMAIN_UNDECLARED;
}

int nxinput_provider_declare_static(const nxinput_provider_evidence *evidence,
                                    const char *pin_id, uint8_t domain,
                                    uint32_t provider_generation,
                                    nxinput_provider_descriptor *out) {
  if (evidence == NULL || out == NULL || pin_id == NULL || pin_id[0] == '\0' ||
      !evidence_valid(evidence) || !evidence->statically_linked ||
      nxinput_sdl_domain_plan((nxinput_sdl_domain)domain) == NULL) {
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->api_version = NXINPUT_PROVIDER_API_VERSION;
  out->struct_size = sizeof *out;
  out->evidence = *evidence;
  out->method = NXINPUT_PROVIDER_METHOD_DECLARED_STATIC_SOURCE;
  out->domain = domain;
  out->driver = NXINPUT_PROVIDER_DRIVER_STATIC_BUILTIN;
  out->confidence = 70u;
  out->provider_generation = provider_generation;
  out->pin_id = pin_id;
  return 0;
}

const char *nxinput_provider_method_name(nxinput_provider_method m) {
  switch (m) {
    case NXINPUT_PROVIDER_METHOD_EXPORTED_API: return "exported-api";
    case NXINPUT_PROVIDER_METHOD_PINNED_ELF: return "pinned-elf";
    case NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS: return "measured-inprocess";
    case NXINPUT_PROVIDER_METHOD_DECLARED_STATIC_SOURCE: return "declared-static-source";
    case NXINPUT_PROVIDER_METHOD_UNKNOWN:
    default: return "unknown";
  }
}

const char *nxinput_provider_driver_name(nxinput_provider_driver d) {
  switch (d) {
    case NXINPUT_PROVIDER_DRIVER_EVDEV: return "evdev";
    case NXINPUT_PROVIDER_DRIVER_HIDAPI: return "hidapi";
    case NXINPUT_PROVIDER_DRIVER_VIRTUAL: return "virtual";
    case NXINPUT_PROVIDER_DRIVER_STATIC_BUILTIN: return "static-builtin";
    case NXINPUT_PROVIDER_DRIVER_UNKNOWN:
    default: return "unknown";
  }
}

int nxinput_provider_receipt_line(const nxinput_provider_descriptor *d,
                                  char *out, size_t cap) {
  int n;
  if (d == NULL || out == NULL || cap == 0u) {
    return -1;
  }
  n = snprintf(out, cap,
               "NXC6-PROVIDER api=%s version=%.40s soname=%.40s "
               "path_class=%.24s sha256=%.64s build_id=%.40s "
               "sha_bound=%u static=%u bytable=%u method=%s domain=%s "
               "pin=%s driver=%s confidence=%u provider_generation=%u "
               "measured=%u measurement_conflict=%u measured_ambiguous=%u "
               "compat_over_sdl3=%u",
               nxinput_sdl_api_name((nxinput_sdl_api)d->evidence.api),
               d->evidence.version[0] ? d->evidence.version : "-",
               d->evidence.soname[0] ? d->evidence.soname : "-",
               d->evidence.path_class[0] ? d->evidence.path_class : "-",
               d->evidence.sha256[0] ? d->evidence.sha256 : "-",
               d->evidence.build_id[0] ? d->evidence.build_id : "none",
               (unsigned)d->evidence.sha_bound_to_mapping,
               (unsigned)d->evidence.statically_linked,
               (unsigned)d->evidence.has_exported_bytable,
               nxinput_provider_method_name((nxinput_provider_method)d->method),
               nxinput_sdl_domain_name((nxinput_sdl_domain)d->domain),
               d->pin_id != NULL ? d->pin_id : "-",
               nxinput_provider_driver_name((nxinput_provider_driver)d->driver),
               (unsigned)d->confidence, (unsigned)d->provider_generation,
               (unsigned)d->measured, (unsigned)d->measurement_conflict,
               (unsigned)d->measured_ambiguous,
               (unsigned)d->evidence.compat_over_sdl3);
  if (n < 0 || (size_t)n >= cap) {
    return -1;
  }
  return n;
}
