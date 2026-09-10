/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_sdl_seam -- see include/nxinput_sdl_seam.h.
 *
 * Runs INSIDE the SDL process, on SDL's own joystick thread, before the
 * device is announced. It opens no device, guesses no domain and announces
 * nothing itself: the caller announces only on ADMIT. */
#include "nxinput_sdl_seam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ plumbing */

static int ops_valid(const nxinput_sdl_seam_ops *ops) {
  return ops != 0 && ops->api_version == NXINPUT_SDL_SEAM_API_VERSION &&
         (ops->struct_size == NXINPUT_SDL_SEAM_OPS_SIZE_0_8_1 ||
          ops->struct_size == NXINPUT_SDL_SEAM_OPS_SIZE_0_9_0 ||
          ops->struct_size == sizeof(*ops)) &&
         ops->getenv_fn != 0 &&
         ops->read_text_fn != 0 && ops->add_mapping_fn != 0 &&
         ops->mapping_for_guid_fn != 0 && ops->monotonic_ns != 0 &&
         ops->pid != 0 && ops->tid != 0 && ops->receipt_fn != 0 &&
         (ops->api == (uint8_t)NXINPUT_SDL_API_2 ||
          ops->api == (uint8_t)NXINPUT_SDL_API_3);
}

static int ops_has_normalizer(const nxinput_sdl_seam_ops *ops) {
  return ops != 0 && ops->struct_size >= NXINPUT_SDL_SEAM_OPS_SIZE_0_9_0 &&
         ops->normalize_source_fn != 0;
}

/* The 0.10.0 tail members are read ONLY from a full-size table. */
static int ops_has_livedb(const nxinput_sdl_seam_ops *ops) {
  return ops != 0 && ops->struct_size == sizeof(*ops) &&
         ops->livedb_acquire_fn != 0;
}

static uint8_t ops_face_layout(const nxinput_sdl_seam_ops *ops) {
  return ops != 0 && ops->struct_size == sizeof(*ops) ? ops->face_layout
                                                      : (uint8_t)0u;
}

static const char *face_layout_word(uint8_t value) {
  switch (value) {
    case 1u:
      return "modern";
    case 2u:
      return "retro";
    default:
      return "auto";
  }
}

/* Bounded FNV-1a 64 over the effective mapping: a stable receipt checksum
 * (NOT a cryptographic seal; the durable pins stay SHA-256 elsewhere). */
static uint64_t fnv1a64(const char *text) {
  uint64_t hash = 14695981039346656037ull;
  while (text != 0 && *text != '\0') {
    hash ^= (uint64_t)(unsigned char)*text++;
    hash *= 1099511628211ull;
  }
  return hash;
}

/* Escape the device name for a receipt: printable ASCII only, spaces and
 * separators collapsed to '_', hard cap. Evidence, never a selector. */
static void sanitize_name(const char *name, char *out, size_t cap) {
  size_t used = 0u;

  if (cap == 0u) {
    return;
  }
  while (name != 0 && *name != '\0' && used + 1u < cap && used < 48u) {
    unsigned char byte = (unsigned char)*name++;
    out[used++] = (byte > 0x20u && byte < 0x7fu && byte != '=' &&
                   byte != '"')
                      ? (char)byte
                      : '_';
  }
  out[used] = '\0';
  if (used == 0u) {
    (void)snprintf(out, cap, "-");
  }
}

static int device_valid(const nxinput_sdl_seam_device *device) {
  size_t n;

  if (device == 0 || device->api_version != NXINPUT_SDL_SEAM_API_VERSION ||
      (device->struct_size != sizeof(*device) &&
       device->struct_size != NXINPUT_SDL_SEAM_DEVICE_SIZE_0_9_0)) {
    return 0;
  }
  /* A GUID SDL could not compute, or an all-zero one, identifies nothing and
   * must never select a mapping. */
  n = strlen(device->guid);
  if (n != 32u || strspn(device->guid, "0123456789abcdef") != 32u ||
      strcmp(device->guid, "00000000000000000000000000000000") == 0) {
    return 0;
  }
  /* Capabilities are MEASURED. Zero buttons and zero axes is not a pad. */
  if (device->buttons < 0 || device->axes < 0 || device->hats < 0 ||
      (device->buttons == 0 && device->axes == 0)) {
    return 0;
  }
  return 1;
}

static void emit(nxinput_sdl_seam *seam, const nxinput_sdl_seam_ops *ops,
                 const nxinput_sdl_seam_device *device, const char *stage,
                 const char *detail) {
  char line[1400];

  (void)snprintf(line, sizeof line,
                 "NXC6-SEAM seq=%u t_ns=%llu pid=%ld tid=%ld sdl=%s "
                 "instance=%ld guid=%s stage=%s %s",
                 ++seam->sequence,
                 (unsigned long long)ops->monotonic_ns(ops->userdata),
                 ops->pid(ops->userdata), ops->tid(ops->userdata),
                 nxinput_sdl_api_name((nxinput_sdl_api)ops->api),
                 device != 0 ? (long)device->instance_id : -1L,
                 device != 0 && device->guid[0] != '\0' ? device->guid : "-",
                 stage, detail);
  ops->receipt_fn(ops->userdata, line);
}

/* --------------------------------------------- the C3 runtime bindings */
/*
 * These are the vtable nxinput_authority (C3) asks for. Every one of them is
 * a straight forward to the real SDL, or to the device record SDL itself
 * built. Nothing here interprets a mapping: that is C3's job.
 *
 * The authority addresses devices by a `device_index`; the seam is called per
 * device, so the index is a slot into a single-entry bridge rather than an
 * SDL enumeration index. Binding it any other way would reintroduce exactly
 * the "fd of the first pad instead of the right device" defect the 116B audit
 * called out for MMW.
 */
/* The bridge is the seam itself: `current_ops` and `current_device` say what
 * this admission is about. Passing a stack struct would have forced the
 * authority to be re-initialised per call, and that would have erased its
 * entry table -- the exact "fd of the first pad instead of the right device"
 * shape the 116B audit called out. */
struct bridge {
  const nxinput_sdl_seam_ops *ops;
  const nxinput_sdl_seam_device *device;
};

/* Bus-form GUIDs reserve bytes 2-3 for a CRC16 of the device name -- SDL3
 * always, SDL2 since 2.26 (both pinned SDL2 majors, 2.28.5 and 2.32.10, write
 * it in SDL_CreateJoystickGUID and their SDL_PrivateGetControllerMappingForGUID
 * falls back to a zero-CRC database entry after the exact one misses).
 * SDL_GetJoystickGUIDInfo() recognizes both its standard VID/PID form and its
 * unknown-VID/PID name form from this bus word, and SDL's mapping lookup then
 * clears the CRC word before comparing a database GUID. PortMaster databases
 * therefore legitimately carry the same identity with 0000 there. Reproduce
 * only that equality, on both majors: no name lookup, no VID/PID inference,
 * no wildcard and no difference in any other byte. A pre-2.26 SDL2 writes a
 * zero word into the live GUID, so the rule is inert there by construction. */
static int lower_hex(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

static int hex_byte(const char *text) {
  int high;
  int low;
  if (!lower_hex(text[0]) || !lower_hex(text[1])) {
    return -1;
  }
  high = text[0] <= '9' ? text[0] - '0' : text[0] - 'a' + 10;
  low = text[1] <= '9' ? text[1] - '0' : text[1] - 'a' + 10;
  return high * 16 + low;
}

static int crc_alias(const char *candidate, const char *live) {
  int bus_low;
  int bus_high;
  unsigned int i;

  if (candidate == 0 || live == 0 || candidate[32] != ',' ||
      memcmp(candidate + 4, "0000", 4u) != 0 ||
      memcmp(live + 4, "0000", 4u) == 0 ||
      memcmp(candidate, live, 4u) != 0 ||
      memcmp(candidate + 8, live + 8, 24u) != 0) {
    return 0;
  }
  for (i = 0u; i < 32u; i++) {
    if (!lower_hex(candidate[i]) || !lower_hex(live[i])) {
      return 0;
    }
  }
  bus_low = hex_byte(live);
  bus_high = hex_byte(live + 2);
  if (bus_low < 0 || bus_high < 0) {
    return 0;
  }
  /* The exact bus-form predicate used by SDL_GetJoystickGUIDInfo(): a bus
   * value below ASCII space, or SDL_HARDWARE_BUS_VIRTUAL (0xff). */
  return ((bus_low | (bus_high << 8)) < 0x20 ||
          (bus_low | (bus_high << 8)) == 0xff);
}

static unsigned int project_crc_aliases(char *text, const char *live) {
  char *cursor = text;
  unsigned int count = 0u;
  if (text == 0 || live == 0) {
    return 0u;
  }
  /* SDL's own lookup consults the EXACT GUID first and falls back to the
   * zero-CRC entry only when the exact one misses. When this source already
   * carries an exact entry for the live GUID, the aliases are unreachable in
   * SDL and must stay untouched here too -- projecting them used to create
   * an artificial exact-vs-alias duplicate that failed a working source
   * closed (the divergent case), which is not what the runtime executes. */
  while (*cursor != '\0') {
    char *eol = strchr(cursor, '\n');
    size_t length = eol != 0 ? (size_t)(eol - cursor) : strlen(cursor);
    if (length > 33u && memcmp(cursor, live, 32u) == 0 && cursor[32] == ',') {
      return 0u;
    }
    if (eol == 0) {
      break;
    }
    cursor = eol + 1;
  }
  cursor = text;
  while (*cursor != '\0') {
    char *eol = strchr(cursor, '\n');
    size_t length = eol != 0 ? (size_t)(eol - cursor) : strlen(cursor);
    if (length > 33u && crc_alias(cursor, live)) {
      memcpy(cursor + 4, live + 4, 4u);
      count++;
    }
    if (eol == 0) {
      break;
    }
    cursor = eol + 1;
  }
  return count;
}

typedef struct admission_context {
  const nxinput_sdl_seam_ops *ops;
  const nxinput_sdl_seam_device *device;
  unsigned int crc_aliases;
  unsigned int domain_lines;
  unsigned int domain_bindings;
  int mapping_view_failed;
} admission_context;

static admission_context *admission_of(nxinput_sdl_seam *seam) {
  return seam != 0 ? (admission_context *)seam->current_ops : 0;
}

/* The authority order owns WHICH source wins. This helper changes only the
 * ordinal representation inside a source after GUID identity has already
 * been projected. A NULL adapter is the backwards-compatible byte-intact
 * path. */
static int normalize_source(nxinput_sdl_seam *seam,
                            const nxinput_sdl_seam_ops *ops,
                            const nxinput_sdl_seam_device *device,
                            const char *source, char *out, size_t cap) {
  unsigned int lines = 0u;
  unsigned int bindings = 0u;
  size_t length;
  admission_context *context = admission_of(seam);

  if (source == 0 || out == 0 || cap == 0u || out == source) {
    return -1;
  }
  if (!ops_has_normalizer(ops)) {
    length = strlen(source);
    if (length >= cap) {
      return -1;
    }
    memcpy(out, source, length + 1u);
    return 0;
  }
  if (ops->normalize_source_fn(
          ops->userdata, ops->api, device->guid, source, out, cap, &lines,
          &bindings) != 0) {
    return -1;
  }
  if (context == 0) {
    return -1;
  }
  context->domain_lines += lines;
  context->domain_bindings += bindings;
  return 0;
}

static struct bridge bridge_of(void *userdata) {
  const nxinput_sdl_seam *seam = (const nxinput_sdl_seam *)userdata;
  const admission_context *context =
      seam != 0 ? (const admission_context *)seam->current_ops : 0;
  struct bridge b;
  b.ops = context != 0 ? context->ops : 0;
  b.device = context != 0 ? context->device : 0;
  return b;
}

/* Authority 2 acquired by nxinput_livedb is handed to C3 as if it were the
 * declared database file, through a sentinel path only bridge_read_text
 * understands. Same rank, same order -- only the acquisition is new. */
#define SEAM_LIVEDB_SENTINEL "\x01nx-livedb-snapshot"

static const char *bridge_getenv(void *userdata, const char *name) {
  struct bridge bridge = bridge_of(userdata);
  struct bridge *b = &bridge;

  if (strcmp(name, NXINPUT_AUTHORITY_ENV_DATABASE) == 0) {
    nxinput_sdl_seam *seam = (nxinput_sdl_seam *)userdata;
    const char *declared = b->ops->getenv_fn(b->ops->userdata, name);
    if ((declared == 0 || declared[0] == '\0') &&
        seam->current_db_snapshot != 0) {
      return SEAM_LIVEDB_SENTINEL;
    }
    return declared;
  }
  /* Authority 1 comes from the staged copy when the port staged one. Same
   * source, same rank, same bytes -- just not left where SDL_Init would
   * import it at USER priority and outrank the decision. */
  if (strcmp(name, NXINPUT_AUTHORITY_ENV_MAPPING) == 0) {
    admission_context *context =
        admission_of((nxinput_sdl_seam *)userdata);
    if (((nxinput_sdl_seam *)userdata)->current_mapping_view != 0) {
      return ((nxinput_sdl_seam *)userdata)->current_mapping_view;
    }
    if (context != 0 && context->mapping_view_failed) {
      return "";
    }
    if (b->ops->staged_mapping != 0 &&
        b->ops->staged_mapping[0] != '\0') {
      return b->ops->staged_mapping;
    }
  }
  return b->ops->getenv_fn(b->ops->userdata, name);
}

static int bridge_read_text(void *userdata, const char *path, char *out,
                            size_t cap) {
  struct bridge bridge = bridge_of(userdata);
  struct bridge *b = &bridge;
  int result;

  if (path != 0 && strcmp(path, SEAM_LIVEDB_SENTINEL) == 0) {
    nxinput_sdl_seam *seam = (nxinput_sdl_seam *)userdata;
    size_t length;
    if (seam->current_db_snapshot == 0) {
      return -1;
    }
    length = strlen(seam->current_db_snapshot);
    if (length >= cap) {
      return -1;
    }
    memcpy(out, seam->current_db_snapshot, length + 1u);
    result = 0;
  } else {
    result = b->ops->read_text_fn(b->ops->userdata, path, out, cap);
  }
  if (result == 0) {
    nxinput_sdl_seam *seam = (nxinput_sdl_seam *)userdata;
    admission_context *context = admission_of(seam);
    char *normalized;
    if (context == 0) {
      out[0] = '\0';
      return -1;
    }
    context->crc_aliases += project_crc_aliases(out, b->device->guid);
    normalized = (char *)malloc(cap);
    if (normalized == 0 ||
        normalize_source(seam, b->ops, b->device, out, normalized, cap) != 0) {
      free(normalized);
      out[0] = '\0';
      return -1;
    }
    memcpy(out, normalized, strlen(normalized) + 1u);
    free(normalized);
  }
  return result;
}

static int bridge_device_guid(void *userdata, int device_index, char *out,
                              size_t cap) {
  struct bridge bridge = bridge_of(userdata);
  struct bridge *b = &bridge;
  size_t n;

  if (device_index != 0) {
    return -1;
  }
  n = strlen(b->device->guid);
  if (cap <= n) {
    return -1;
  }
  memcpy(out, b->device->guid, n + 1u);
  return 0;
}

static int bridge_device_caps(void *userdata, int device_index, int *buttons,
                              int *axes, int *hats) {
  struct bridge bridge = bridge_of(userdata);
  struct bridge *b = &bridge;

  if (device_index != 0) {
    return -1;
  }
  *buttons = b->device->buttons;
  *axes = b->device->axes;
  *hats = b->device->hats;
  return 0;
}

static int bridge_apply_mapping(void *userdata, const char *line) {
  struct bridge bridge = bridge_of(userdata);
  struct bridge *b = &bridge;
  return b->ops->add_mapping_fn(b->ops->userdata, line);
}

static int bridge_mapping_for_guid(void *userdata, const char *guid, char *out,
                                   size_t cap) {
  struct bridge bridge = bridge_of(userdata);
  struct bridge *b = &bridge;
  return b->ops->mapping_for_guid_fn(b->ops->userdata, guid, out, cap);
}

/* --------------------------------------------------------------- slots */

static nxinput_sdl_seam_slot *slot_for(nxinput_sdl_seam *seam,
                                       int32_t instance_id) {
  unsigned int i;
  for (i = 0u; i < NXINPUT_SDL_SEAM_MAX_DEVICES; i++) {
    if (seam->slots[i].in_use && seam->slots[i].instance_id == instance_id) {
      return &seam->slots[i];
    }
  }
  return 0;
}

static nxinput_sdl_seam_slot *slot_free(nxinput_sdl_seam *seam) {
  unsigned int i;
  for (i = 0u; i < NXINPUT_SDL_SEAM_MAX_DEVICES; i++) {
    if (!seam->slots[i].in_use) {
      return &seam->slots[i];
    }
  }
  return 0;
}

/* SDL keys its mapping store by GUID. Another LIVE instance with the same
 * GUID and a DIFFERENT line cannot coexist: installing the second would
 * silently redefine the first. Order must never decide that, so it fails
 * closed. A byte-identical line is not a collision -- two identical pads are
 * the normal case, not an error. */
static const nxinput_sdl_seam_slot *colliding_slot(
    const nxinput_sdl_seam *seam, int32_t instance_id, const char *guid,
    const char *line) {
  unsigned int i;
  for (i = 0u; i < NXINPUT_SDL_SEAM_MAX_DEVICES; i++) {
    const nxinput_sdl_seam_slot *s = &seam->slots[i];
    if (!s->in_use || s->instance_id == instance_id) {
      continue;
    }
    if (strcmp(s->guid, guid) != 0) {
      continue;
    }
    if (!nxinput_sovereign_semantically_identical(s->line, line)) {
      return s;
    }
  }
  return 0;
}

/* ----------------------------------------------------------------- API */

int nxinput_sdl_seam_init(nxinput_sdl_seam *seam,
                          const nxinput_sdl_seam_ops *ops) {
  nxinput_authority_runtime runtime;

  if (seam == 0 || !ops_valid(ops)) {
    return -1;
  }
  if (seam->initialised) {
    return 0;
  }
  memset(seam, 0, sizeof *seam);
  seam->api_version = NXINPUT_SDL_SEAM_API_VERSION;
  seam->struct_size = sizeof *seam;

  memset(&runtime, 0, sizeof runtime);
  runtime.api_version = NXINPUT_AUTHORITY_API_VERSION;
  runtime.struct_size = sizeof runtime;
  runtime.userdata = seam;
  runtime.getenv_fn = bridge_getenv;
  runtime.read_text_fn = bridge_read_text;
  runtime.device_guid_fn = bridge_device_guid;
  runtime.device_caps_fn = bridge_device_caps;
  runtime.apply_mapping_fn = bridge_apply_mapping;
  runtime.mapping_for_guid_fn = bridge_mapping_for_guid;
  runtime.runtime_has_builtin = ops->runtime_has_builtin;
  runtime.consumer_accepts_raw = ops->consumer_accepts_raw;
  if (nxinput_authority_init(&seam->authority, &runtime) != 0) {
    return -1;
  }
  seam->initialised = 1;
  return 0;
}

const nxinput_sdl_seam_slot *nxinput_sdl_seam_find(
    const nxinput_sdl_seam *seam, int32_t instance_id) {
  unsigned int i;
  if (seam == 0) {
    return 0;
  }
  for (i = 0u; i < NXINPUT_SDL_SEAM_MAX_DEVICES; i++) {
    if (seam->slots[i].in_use && seam->slots[i].instance_id == instance_id) {
      return &seam->slots[i];
    }
  }
  return 0;
}

const char *nxinput_sdl_seam_result_name(nxinput_sdl_seam_result result) {
  switch (result) {
    case NXINPUT_SDL_SEAM_ADMIT: return "admit";
    case NXINPUT_SDL_SEAM_NO_DECLARATION: return "no-declaration";
    case NXINPUT_SDL_SEAM_BLOCK_OPS: return "block-ops";
    case NXINPUT_SDL_SEAM_BLOCK_IDENTITY: return "block-identity";
    case NXINPUT_SDL_SEAM_BLOCK_AUTHORITY: return "block-authority";
    case NXINPUT_SDL_SEAM_BLOCK_COLLISION:
    default: return "block-collision";
  }
}

void nxinput_sdl_seam_forget(nxinput_sdl_seam *seam,
                             const nxinput_sdl_seam_ops *ops,
                             int32_t instance_id) {
  nxinput_sdl_seam_slot *slot;

  if (seam == 0 || !seam->initialised) {
    return;
  }
  slot = slot_for(seam, instance_id);
  if (slot == 0) {
    return;
  }
  /* ONLY this instance. The other slots keep their decisions: a hotplug of
   * pad B must not invalidate pad A, and the next device that takes this
   * instance id must not inherit a byte of what stood here. */
  memset(slot, 0, sizeof *slot);
  nxinput_authority_forget(&seam->authority, instance_id);
  seam->forgotten++;
  if (ops_valid(ops)) {
    char detail[160];
    nxinput_sdl_seam_device shim;
    memset(&shim, 0, sizeof shim);
    shim.api_version = NXINPUT_SDL_SEAM_API_VERSION;
    shim.struct_size = sizeof shim;
    shim.instance_id = instance_id;
    (void)snprintf(detail, sizeof detail,
                   "result=ok forgotten=%u still_admitted=%u",
                   seam->forgotten,
                   seam->admitted > 0u ? seam->admitted - 1u : 0u);
    if (seam->admitted > 0u) {
      seam->admitted--;
    }
    emit(seam, ops, &shim, "forget", detail);
  }
}

nxinput_sdl_seam_result nxinput_sdl_seam_admit(
    nxinput_sdl_seam *seam, const nxinput_sdl_seam_ops *ops,
    const nxinput_sdl_seam_device *device) {
  nxinput_sovereign_decision decision;
  nxinput_sdl_seam_slot *slot;
  const nxinput_sdl_seam_slot *clash;
  const char *env_mapping;
  unsigned int source_crc_aliases;
  unsigned int source_domain_lines;
  unsigned int source_domain_bindings;
  int authority_status;
  char detail[900];
  char evidence_tail[240];
  char name_evidence[64];
  admission_context context;

  if (seam == 0 || !ops_valid(ops)) {
    return NXINPUT_SDL_SEAM_BLOCK_OPS;
  }
  if (nxinput_sdl_seam_init(seam, ops) != 0) {
    return NXINPUT_SDL_SEAM_BLOCK_OPS;
  }
  if (!device_valid(device)) {
    emit(seam, ops, device, "identity",
         "result=block reason=device-record-not-usable");
    seam->blocked++;
    return NXINPUT_SDL_SEAM_BLOCK_IDENTITY;
  }

  /* NO DECLARATION: the port adopted nothing for this run. SDL keeps its
   * native behaviour untouched -- the seam must not turn an unadopted game
   * into a blocked one. "Declared" means at least one of the three C3
   * sources is present; a port that declares none is simply not adopting. */
  env_mapping = ops->staged_mapping != 0 && ops->staged_mapping[0] != '\0'
                    ? ops->staged_mapping
                    : ops->getenv_fn(ops->userdata,
                                     NXINPUT_AUTHORITY_ENV_MAPPING);
  if ((env_mapping == 0 || env_mapping[0] == '\0') &&
      ops->getenv_fn(ops->userdata, NXINPUT_AUTHORITY_ENV_DATABASE) == 0 &&
      ops->getenv_fn(ops->userdata, NXINPUT_AUTHORITY_ENV_BUNDLE) == 0) {
    emit(seam, ops, device, "declaration",
         "result=no-declaration reason=port-declared-no-source "
         "native_behaviour=preserved");
    return NXINPUT_SDL_SEAM_NO_DECLARATION;
  }

  /* THE C3 ORDER, unchanged. The seam points the already-initialised
   * authority at THIS device and obeys the answer. The authority itself is
   * not rebuilt here: its per-instance entries have to outlive one call. */
  memset(&context, 0, sizeof context);
  context.ops = ops;
  context.device = device;
  seam->current_ops = &context;
  seam->current_device = device;
  seam->current_mapping_view = 0;
  seam->current_crc_aliases = 0u;
  if (env_mapping != 0 && env_mapping[0] != '\0') {
    size_t mapping_size = strlen(env_mapping) + 1u;
    if (mapping_size <= NXINPUT_AUTHORITY_SOURCE_MAX) {
      char *identity_view = (char *)malloc(mapping_size);
      seam->current_mapping_view =
          (char *)malloc(NXINPUT_AUTHORITY_SOURCE_MAX);
      if (identity_view != 0 && seam->current_mapping_view != 0) {
        memcpy(identity_view, env_mapping, mapping_size);
        context.crc_aliases += project_crc_aliases(
            identity_view, device->guid);
        if (normalize_source(seam, ops, device, identity_view,
                             seam->current_mapping_view,
                             NXINPUT_AUTHORITY_SOURCE_MAX) != 0) {
          seam->current_mapping_view[0] = '\0';
          context.mapping_view_failed = 1;
        }
      } else {
        free(seam->current_mapping_view);
        seam->current_mapping_view = 0;
        context.mapping_view_failed = 1;
      }
      free(identity_view);
    } else {
      context.mapping_view_failed = 1;
    }
  }
  /* 0.10.0: with authority 1 empty and no declared database path, acquire
   * the LIVE canonical database once for THIS admission (bounded wait,
   * stable snapshot -- nxinput_livedb). The snapshot enters the order as
   * authority 2, nothing more: an env mapping still outranks it and the
   * bundle still ranks below it. */
  seam->current_db_snapshot = 0;
  memset(&seam->current_db_receipt, 0, sizeof seam->current_db_receipt);
  if ((env_mapping == 0 || env_mapping[0] == '\0') &&
      ops_has_livedb(ops)) {
    const char *declared_db =
        ops->getenv_fn(ops->userdata, NXINPUT_AUTHORITY_ENV_DATABASE);
    if (declared_db == 0 || declared_db[0] == '\0') {
      char *snapshot = (char *)malloc(NXINPUT_AUTHORITY_SOURCE_MAX);
      if (snapshot != 0) {
        if (ops->livedb_acquire_fn(ops->userdata, snapshot,
                                   NXINPUT_AUTHORITY_SOURCE_MAX,
                                   &seam->current_db_receipt) == 0 &&
            snapshot[0] != '\0') {
          seam->current_db_snapshot = snapshot;
        } else {
          free(snapshot);
        }
      }
    }
  }

  /* These two are declarations the caller makes per run, not properties of
   * the authority, so they are refreshed here rather than frozen at init.
   * A port that declares a raw consumer must not depend on which admission
   * happened to be first. */
  seam->authority.runtime.runtime_has_builtin = ops->runtime_has_builtin;
  seam->authority.runtime.consumer_accepts_raw = ops->consumer_accepts_raw;
  memset(&decision, 0, sizeof decision);
  authority_status = nxinput_authority_admit(
      &seam->authority, 0, device->instance_id, &decision);
  source_crc_aliases = context.crc_aliases;
  source_domain_lines = context.domain_lines;
  source_domain_bindings = context.domain_bindings;
  free(seam->current_mapping_view);
  seam->current_mapping_view = 0;
  free(seam->current_db_snapshot);
  seam->current_db_snapshot = 0;
  seam->current_crc_aliases = 0u;
  seam->current_ops = 0;
  seam->current_device = 0;
  sanitize_name(device->struct_size == sizeof(*device) ? device->name : 0,
                name_evidence, sizeof name_evidence);
  (void)snprintf(evidence_tail, sizeof evidence_tail,
                 "name=%s db_class=%s db_target=%s db_retries=%u "
                 "db_elapsed_ms=%llu face_layout=%s",
                 name_evidence,
                 nxinput_livedb_path_class_name(
                     (int)seam->current_db_receipt.path_class),
                 seam->current_db_receipt.target[0] != '\0'
                     ? seam->current_db_receipt.target
                     : "-",
                 seam->current_db_receipt.attempts,
                 (unsigned long long)(seam->current_db_receipt.elapsed_ns /
                                      1000000ull),
                 face_layout_word(ops_face_layout(ops)));
  if (authority_status != 0) {
    (void)snprintf(detail, sizeof detail,
                   "result=block reason=%s source=%s "
                   "step_env=%s step_cfw=%s step_bundle=%s step_builtin=%s "
                   "step_raw=%s readback_checked=%d source_crc_aliases=%u "
                   "dup_lastwins=%d domain_lines=%u domain_bindings=%u "
                   "source_domain=%s target_domain=%s %s",
                   nxinput_sovereign_reason_name(decision.reason),
                   nxinput_sovereign_source_name(decision.source),
                   nxinput_sovereign_reason_name(decision.step_reason[0]),
                   nxinput_sovereign_reason_name(decision.step_reason[1]),
                   nxinput_sovereign_reason_name(decision.step_reason[2]),
                   nxinput_sovereign_reason_name(decision.step_reason[3]),
                   nxinput_sovereign_reason_name(decision.step_reason[4]),
                   decision.readback_checked, source_crc_aliases,
                   decision.duplicate_lastwins, source_domain_lines,
                   source_domain_bindings,
                   source_domain_lines > 0u ? "joydev-legacy" : "unchanged",
                   nxinput_sdl_domain_name(nxinput_sdl_api_domain(
                       (nxinput_sdl_api)ops->api)),
                   evidence_tail);
    emit(seam, ops, device, "authority", detail);
    seam->blocked++;
    return NXINPUT_SDL_SEAM_BLOCK_AUTHORITY;
  }

  /*
   * A decision that INSTALLED a mapping and was not read back never
   * authorizes gameplay. C3 already refuses to win on a setter alone; the
   * seam repeats the invariant because this is the boundary that announces,
   * so this is where an unproved mapping would actually reach the player.
   *
   * Authority 5 is the one honest exception, and it is an exception about
   * what exists rather than a relaxation: raw passthrough installs NO
   * mapping, so there is nothing for the runtime to report back. Demanding a
   * readback there does not make anything safer -- it simply makes step 5
   * unreachable, which would quietly delete a step of the C3 order the
   * moment a port legitimately needed it. What guards step 5 instead is the
   * thing that is actually specific to it: the consumer must have DECLARED
   * that it understands a raw pad, which C3 enforces before returning it and
   * which is asserted again here rather than trusted.
   */
  if (decision.source == NXINPUT_SOVEREIGN_RAW_PASSTHROUGH) {
    if (!ops->consumer_accepts_raw) {
      emit(seam, ops, device, "raw",
           "result=block reason=raw-passthrough-without-declared-consumer");
      seam->blocked++;
      return NXINPUT_SDL_SEAM_BLOCK_AUTHORITY;
    }
  } else if (!decision.readback_checked) {
    emit(seam, ops, device, "readback",
         "result=block reason=no-live-readback-behind-this-decision");
    seam->blocked++;
    return NXINPUT_SDL_SEAM_BLOCK_AUTHORITY;
  }

  /* SDL's store is keyed by GUID; C3's entries are keyed by instance. When
   * those two disagree, refuse rather than let arrival order decide. */
  clash = colliding_slot(seam, device->instance_id, device->guid,
                         decision.line);
  if (clash != 0) {
    (void)snprintf(detail, sizeof detail,
                   "result=block reason=guid-store-collision "
                   "other_instance=%ld same_guid=1 divergent_mapping=1",
                   (long)clash->instance_id);
    emit(seam, ops, device, "collision", detail);
    nxinput_authority_forget(&seam->authority, device->instance_id);
    seam->collisions++;
    seam->blocked++;
    return NXINPUT_SDL_SEAM_BLOCK_COLLISION;
  }

  slot = slot_for(seam, device->instance_id);
  if (slot == 0) {
    slot = slot_free(seam);
  }
  if (slot == 0) {
    emit(seam, ops, device, "slots",
         "result=block reason=no-free-slot (refusing rather than evicting a "
         "live pad)");
    nxinput_authority_forget(&seam->authority, device->instance_id);
    seam->blocked++;
    return NXINPUT_SDL_SEAM_BLOCK_IDENTITY;
  }
  memset(slot, 0, sizeof *slot);
  slot->in_use = 1;
  slot->instance_id = device->instance_id;
  /* device_valid() already proved the GUID is exactly 32 hex digits, so this
   * copy is bounded by construction rather than by a truncating printf. */
  memcpy(slot->guid, device->guid, 33u);
  (void)snprintf(slot->line, sizeof slot->line, "%s", decision.line);
  slot->source = decision.source;
  seam->admitted++;

  (void)snprintf(detail, sizeof detail,
                 "result=admit source=%s "
                 "step_env=%s step_cfw=%s step_bundle=%s step_builtin=%s "
                 "step_raw=%s readback_checked=%d buttons=%d "
                 "axes=%d hats=%d source_crc_aliases=%u resolutions=%u "
                 "admitted=%u dup_lastwins=%d domain_lines=%u "
                 "domain_bindings=%u source_domain=%s target_domain=%s "
                 "%s effective_guid=%.32s map_fnv1a64=%016llx map_bytes=%u",
                 nxinput_sovereign_source_name(decision.source),
                 nxinput_sovereign_reason_name(decision.step_reason[0]),
                 nxinput_sovereign_reason_name(decision.step_reason[1]),
                 nxinput_sovereign_reason_name(decision.step_reason[2]),
                 nxinput_sovereign_reason_name(decision.step_reason[3]),
                 nxinput_sovereign_reason_name(decision.step_reason[4]),
                 decision.readback_checked, device->buttons, device->axes,
                 device->hats, source_crc_aliases,
                 seam->authority.resolutions, seam->admitted,
                 decision.duplicate_lastwins, source_domain_lines,
                 source_domain_bindings,
                 source_domain_lines > 0u ? "joydev-legacy" : "unchanged",
                 nxinput_sdl_domain_name(nxinput_sdl_api_domain(
                     (nxinput_sdl_api)ops->api)),
                 evidence_tail,
                 decision.line[0] != '\0' ? decision.line : "-",
                 (unsigned long long)fnv1a64(decision.line),
                 (unsigned int)strlen(decision.line));
  emit(seam, ops, device, "announce", detail);
  return NXINPUT_SDL_SEAM_ADMIT;
}

/* --------------------------------------------------- pre-init staging */

static int env_ops_valid(const nxinput_sdl_seam_env_ops *ops) {
  return ops != 0 && ops->api_version == NXINPUT_SDL_SEAM_API_VERSION &&
         ops->struct_size == sizeof(*ops) && ops->getenv_fn != 0 &&
         ops->unsetenv_fn != 0 && ops->sdl_was_init_fn != 0;
}

int nxinput_sdl_seam_stage_before_init(const nxinput_sdl_seam_env_ops *ops,
                                       char *out, size_t cap,
                                       size_t *staged_len) {
  const char *value;
  size_t n;

  if (!env_ops_valid(ops) || out == 0 || cap == 0u || staged_len == 0) {
    return -1;
  }
  *staged_len = 0u;
  out[0] = '\0';
  /* Too late is not "nearly on time". Once any subsystem is up we cannot
   * prove the variable was not already imported at USER priority, so the
   * honest answer is failure, not a best effort. */
  if (ops->sdl_was_init_fn(ops->userdata) != 0) {
    return -1;
  }
  value = ops->getenv_fn(ops->userdata, NXINPUT_AUTHORITY_ENV_MAPPING);
  if (value == 0) {
    return 0; /* absent: legitimate pass-through */
  }
  n = strlen(value);
  if (n >= cap) {
    return -1; /* never truncate a mapping into a decision */
  }
  memcpy(out, value, n + 1u);
  if (ops->unsetenv_fn(ops->userdata, NXINPUT_AUTHORITY_ENV_MAPPING) != 0) {
    out[0] = '\0';
    return -1;
  }
  /* Proof, not assumption: the variable must really be gone now. */
  if (ops->getenv_fn(ops->userdata, NXINPUT_AUTHORITY_ENV_MAPPING) != 0) {
    out[0] = '\0';
    return -1;
  }
  *staged_len = n;
  return 0;
}
