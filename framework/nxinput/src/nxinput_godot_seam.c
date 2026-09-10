/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_godot_seam -- see include/nxinput_godot_seam.h.
 *
 * Runs INSIDE the engine process, on the engine's joypad thread, before the
 * pad is announced. Reads its declaration from disk once per admission; it
 * never opens a device, never guesses a domain and never announces anything
 * itself -- the caller announces only on ADMIT. */
#include "nxinput_godot_seam.h"
#include "nxinput_translate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- SHA-256 */
/* Self-contained so the seam adds no dependency to the engine build. */
typedef struct { uint32_t s[8]; uint64_t n; size_t u; unsigned char b[64]; } sha_t;

static uint32_t ror(uint32_t v, unsigned int k) { return (v >> k) | (v << (32u - k)); }

static void sha_block(sha_t *c, const unsigned char *p) {
  static const uint32_t K[64] = {
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
      0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
      0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
      0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
      0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
      0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
      0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
      0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
      0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
  uint32_t w[64], a, b, c2, d, e, f, g, h; unsigned int i;
  for (i = 0u; i < 16u; i++)
    w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
  for (i = 16u; i < 64u; i++) {
    uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
    uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
    w[i] = w[i-16]+s0+w[i-7]+s1;
  }
  a=c->s[0];b=c->s[1];c2=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
  for (i = 0u; i < 64u; i++) {
    uint32_t t1 = h + (ror(e,6)^ror(e,11)^ror(e,25)) + ((e&f)^(~e&g)) + K[i] + w[i];
    uint32_t t2 = (ror(a,2)^ror(a,13)^ror(a,22)) + ((a&b)^(a&c2)^(b&c2));
    h=g;g=f;f=e;e=d+t1;d=c2;c2=b;b=a;a=t1+t2;
  }
  c->s[0]+=a;c->s[1]+=b;c->s[2]+=c2;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}

static void sha_hex(const char *data, size_t len, char out[65]) {
  static const char hx[] = "0123456789abcdef";
  sha_t c; unsigned char dg[32]; uint64_t bits; size_t i;
  memset(&c, 0, sizeof c);
  c.s[0]=0x6a09e667u;c.s[1]=0xbb67ae85u;c.s[2]=0x3c6ef372u;c.s[3]=0xa54ff53au;
  c.s[4]=0x510e527fu;c.s[5]=0x9b05688cu;c.s[6]=0x1f83d9abu;c.s[7]=0x5be0cd19u;
  for (i = 0u; i < len; i++) {
    c.b[c.u++] = (unsigned char)data[i];
    if (c.u == 64u) { sha_block(&c, c.b); c.u = 0u; }
  }
  bits = (uint64_t)len * 8u;
  c.b[c.u++] = 0x80u;
  if (c.u > 56u) { while (c.u < 64u) c.b[c.u++] = 0u; sha_block(&c, c.b); c.u = 0u; }
  while (c.u < 56u) c.b[c.u++] = 0u;
  for (i = 0u; i < 8u; i++) c.b[63u - i] = (unsigned char)(bits >> (i * 8u));
  sha_block(&c, c.b);
  for (i = 0u; i < 8u; i++) {
    dg[i*4]   = (unsigned char)(c.s[i] >> 24);
    dg[i*4+1] = (unsigned char)(c.s[i] >> 16);
    dg[i*4+2] = (unsigned char)(c.s[i] >> 8);
    dg[i*4+3] = (unsigned char)c.s[i];
  }
  for (i = 0u; i < 32u; i++) { out[i*2] = hx[dg[i]>>4]; out[i*2+1] = hx[dg[i]&15]; }
  out[64] = '\0';
}

/* ------------------------------------------------- the C3 declaration */
/* An authenticated declaration, not a loose environment string. Every field
 * must be present and must AGREE with the others and with the device; a typo,
 * an empty value or an unknown provider blocks. */
#define SEAM_LINE_MAX NXINPUT_GODOT_LINE_MAX

struct declaration {
  char domain[24];
  char provider[NXINPUT_GODOT_PROVIDER_MAX];
  char receipt[NXINPUT_GODOT_RECEIPT_MAX];
  char generation[80];
  char guid[40];
  char mapping_sha256[65];
  char mapping[SEAM_LINE_MAX];
};

/* The frozen C3 provider allowlist. A provider outside it never authorizes a
 * domain, however well-formed the rest of the declaration is. */
static int provider_allowed(const char *provider) {
  static const char *const allow[] = {"portmaster-gui", "nxinput-authority-c3"};
  size_t i;
  for (i = 0u; i < sizeof(allow) / sizeof(allow[0]); i++) {
    if (strcmp(allow[i], provider) == 0) {
      return 1;
    }
  }
  return 0;
}

static int copy_field(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n == 0u || n >= cap) {
    return -1;
  }
  memcpy(dst, src, n + 1u);
  return 0;
}

static int read_declaration(const char *path, struct declaration *out) {
  char line[SEAM_LINE_MAX + 128];
  FILE *stream;
  int got = 0;

  memset(out, 0, sizeof *out);
  stream = fopen(path, "r");
  if (stream == 0) {
    return -1;
  }
  while (fgets(line, (int)sizeof line, stream) != 0) {
    size_t n = strlen(line);
    char *value;
    while (n > 0u && (line[n-1u] == '\n' || line[n-1u] == '\r')) line[--n] = '\0';
    value = strchr(line, '=');
    if (value == 0) continue;
    *value++ = '\0';
    if (strcmp(line, "domain") == 0 && copy_field(out->domain, sizeof out->domain, value) == 0) got |= 1;
    else if (strcmp(line, "provider") == 0 && copy_field(out->provider, sizeof out->provider, value) == 0) got |= 2;
    else if (strcmp(line, "receipt") == 0 && copy_field(out->receipt, sizeof out->receipt, value) == 0) got |= 4;
    else if (strcmp(line, "generation") == 0 && copy_field(out->generation, sizeof out->generation, value) == 0) got |= 8;
    else if (strcmp(line, "guid") == 0 && copy_field(out->guid, sizeof out->guid, value) == 0) got |= 16;
    else if (strcmp(line, "mapping_sha256") == 0 && copy_field(out->mapping_sha256, sizeof out->mapping_sha256, value) == 0) got |= 32;
    else if (strcmp(line, "mapping") == 0 && copy_field(out->mapping, sizeof out->mapping, value) == 0) got |= 64;
  }
  fclose(stream);
  return got == 127 ? 0 : -1;
}

const char *nxinput_godot_seam_result_name(nxinput_godot_seam_result r) {
  switch (r) {
    case NXINPUT_GODOT_SEAM_ADMIT: return "admit";
    case NXINPUT_GODOT_SEAM_NO_DECLARATION: return "no-declaration";
    case NXINPUT_GODOT_SEAM_BLOCK_ORIGIN: return "block-origin";
    case NXINPUT_GODOT_SEAM_BLOCK_IDENTITY: return "block-identity";
    case NXINPUT_GODOT_SEAM_BLOCK_MAPPING: return "block-mapping";
    case NXINPUT_GODOT_SEAM_BLOCK_SETTER: return "block-setter";
    case NXINPUT_GODOT_SEAM_BLOCK_READBACK: return "block-readback";
    case NXINPUT_GODOT_SEAM_BLOCK_PROBES: return "block-probes";
    case NXINPUT_GODOT_SEAM_BLOCK_ENGINE_TABLE:
    default: return "block-engine-table";
  }
}

/* Godot's logical button enums differ between the majors -- measured on both
 * running engines. The seam needs them to know what the engine SHOULD answer
 * for a given SDL binding name. */
static const char *const godot3_order[] = {
    "a","b","x","y","leftshoulder","rightshoulder","lefttrigger","righttrigger",
    "leftstick","rightstick","back","start","dpup","dpdown","dpleft","dpright",
    "guide"};
static const char *const godot4_order[] = {
    "a","b","x","y","back","guide","start","leftstick","rightstick",
    "leftshoulder","rightshoulder","dpup","dpdown","dpleft","dpright"};

/* Which logical index must this engine report for the binding whose physical
 * ordinal is `ordinal`, according to the SERVED line? */
static int expected_logical(const char *served, unsigned int ordinal,
                            uint8_t major) {
  const char *const *order = major == (uint8_t)NXINPUT_GODOT_ENGINE_4
                                 ? godot4_order : godot3_order;
  size_t count = major == (uint8_t)NXINPUT_GODOT_ENGINE_4
                     ? sizeof(godot4_order)/sizeof(godot4_order[0])
                     : sizeof(godot3_order)/sizeof(godot3_order[0]);
  size_t i;
  for (i = 0u; i < count; i++) {
    char needle[40];
    const char *hit;
    unsigned int bound;
    int written = snprintf(needle, sizeof needle, ",%s:b", order[i]);
    if (written <= 0 || (size_t)written >= sizeof needle) continue;
    hit = strstr(served, needle);
    if (hit != 0 && sscanf(hit + (size_t)written, "%u", &bound) == 1 &&
        bound == ordinal) {
      return (int)i;
    }
  }
  return -1;
}

static int engine_table_valid(const nxinput_godot_seam_engine *e) {
  return e != 0 && e->api_version == NXINPUT_GODOT_SEAM_API_VERSION &&
         e->struct_size == sizeof(*e) && e->add_joy_mapping != 0 &&
         e->has_mapping != 0 && e->readback_logical_button != 0 &&
         e->monotonic_ns != 0 && e->pid != 0 && e->tid != 0 &&
         e->receipt != 0 &&
         (e->major == (uint8_t)NXINPUT_GODOT_ENGINE_3 ||
          e->major == (uint8_t)NXINPUT_GODOT_ENGINE_4);
}

static void emit(const nxinput_godot_seam_engine *e,
                 const nxinput_godot_seam_device *d, unsigned int seq,
                 const char *stage, const char *detail) {
  char line[900];
  (void)snprintf(line, sizeof line,
                 "NXC5B-SEAM seq=%u t_ns=%llu pid=%ld tid=%ld engine=%s "
                 "joy_id=%d fd=%d guid=%s devpath=%s stage=%s %s",
                 seq, (unsigned long long)e->monotonic_ns(e->userdata),
                 e->pid(e->userdata), e->tid(e->userdata),
                 e->major == (uint8_t)NXINPUT_GODOT_ENGINE_4 ? "godot4" : "godot3",
                 d->joy_id, d->fd, d->guid[0] ? d->guid : "-",
                 d->devpath[0] ? d->devpath : "-", stage, detail);
  e->receipt(e->userdata, line);
}

nxinput_godot_seam_result nxinput_godot_seam_admit(
    const nxinput_godot_seam_engine *engine,
    const nxinput_godot_seam_device *device, const char *declaration_path) {
  struct declaration decl;
  nxinput_godot_origin origin;
  nxinput_godot_caps caps;
  nxinput_godot_evidence evidence;
  char served[SEAM_LINE_MAX];
  char digest[65];
  char detail[600];
  /* ONE sequence for the whole process. It used to restart at each call, so
   * a second admission -- a second pad, or an engine that opens the same
   * node twice -- produced 1..5 twice and the ordering could not be read
   * from the receipt at all. */
  static unsigned int g_seq = 0u;
  unsigned int probes = 0u;
  unsigned int agreed = 0u;
  unsigned int ordinal;
  nxinput_godot_result mapped;

  if (!engine_table_valid(engine)) {
    return NXINPUT_GODOT_SEAM_BLOCK_ENGINE_TABLE;
  }
  if (device == 0 || device->api_version != NXINPUT_GODOT_SEAM_API_VERSION ||
      device->struct_size != sizeof(*device) || device->fd < 0 ||
      device->key_bits == 0 || device->abs_bits == 0) {
    emit(engine, device ? device : &(nxinput_godot_seam_device){0}, ++g_seq,
         "device", "result=block reason=incomplete-device-record");
    return NXINPUT_GODOT_SEAM_BLOCK_IDENTITY;
  }
  if (declaration_path == 0 || declaration_path[0] == '\0') {
    return NXINPUT_GODOT_SEAM_NO_DECLARATION;
  }
  if (read_declaration(declaration_path, &decl) != 0) {
    emit(engine, device, ++g_seq, "declaration",
         "result=block reason=declaration-missing-or-incomplete");
    return NXINPUT_GODOT_SEAM_BLOCK_ORIGIN;
  }

  /* 1. ORIGIN: strict enum, allowlisted provider, digest bound to the bytes,
   * and GUID bound to THIS device. No default, no inference.
   * 0.11.1 (C6): a line the CFW authored for its RetroArch-derived
   * ascending SDL2 (`sdl2-ascending-patched`, declared by the producer) is
   * accepted too: it is TRANSLATED through the physical codes into the
   * high-first numbering the engine's own joypad backend uses, and only the
   * translated line reaches the engine. Importing it as if it were
   * high-first is exactly the P0 defect, now impossible here. */
  if (strcmp(decl.domain, "godot") != 0 &&
      strcmp(decl.domain, "sdl2-evdev") != 0 &&
      strcmp(decl.domain, "sdl2-ascending-patched") != 0) {
    emit(engine, device, ++g_seq, "origin",
         "result=block reason=unknown-domain (no default is applied)");
    return NXINPUT_GODOT_SEAM_BLOCK_ORIGIN;
  }
  if (!provider_allowed(decl.provider)) {
    emit(engine, device, ++g_seq, "origin",
         "result=block reason=provider-not-in-c3-allowlist");
    return NXINPUT_GODOT_SEAM_BLOCK_ORIGIN;
  }
  sha_hex(decl.mapping, strlen(decl.mapping), digest);
  if (strcmp(digest, decl.mapping_sha256) != 0) {
    (void)snprintf(detail, sizeof detail,
                   "result=block reason=mapping-digest-mismatch declared=%s "
                   "recomputed=%s", decl.mapping_sha256, digest);
    emit(engine, device, ++g_seq, "origin", detail);
    return NXINPUT_GODOT_SEAM_BLOCK_ORIGIN;
  }
  if (strcmp(decl.guid, device->guid) != 0) {
    (void)snprintf(detail, sizeof detail,
                   "result=block reason=guid-bound-to-other-device "
                   "declared=%s device=%s", decl.guid, device->guid);
    emit(engine, device, ++g_seq, "identity", detail);
    return NXINPUT_GODOT_SEAM_BLOCK_IDENTITY;
  }
  if (strncmp(decl.mapping, device->guid, 32u) != 0) {
    emit(engine, device, ++g_seq, "identity",
         "result=block reason=mapping-guid-differs-from-device");
    return NXINPUT_GODOT_SEAM_BLOCK_IDENTITY;
  }
  (void)snprintf(detail, sizeof detail,
                 "result=ok domain=%s provider=%s receipt=%s generation=%s "
                 "mapping_sha256=%s", decl.domain, decl.provider, decl.receipt,
                 decl.generation, digest);
  emit(engine, device, ++g_seq, "origin", detail);
  if (strcmp(decl.domain, "sdl2-ascending-patched") == 0) {
    /* 0.11.1 (C6): translate the ascending line into the engine's
     * high-first numbering by physical code (the declaration IS the source
     * descriptor: declared by the producer). Anything but a proved
     * translation blocks before the engine sees a byte. */
    nxinput_source_descriptor src;
    nxinput_translate_evidence tev;
    char translated[SEAM_LINE_MAX];
    nxinput_translate_result tr;
    src.provenance = NXINPUT_SOURCE_DECLARED_BY_PRODUCER;
    src.domain = NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED;
    tr = nxinput_translate_line(decl.mapping, device->key_bits, device->key_bit_count,
                                device->abs_bits, device->abs_bit_count,
                                NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &src, translated,
                                sizeof translated, &tev);
    if (tr != NXINPUT_TRANSLATE_REWRITTEN && tr != NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE) {
      (void)snprintf(detail, sizeof detail,
                     "result=block reason=ascending-line-not-translatable "
                     "translate=%s coherent_domains=%u",
                     nxinput_translate_result_name(tr), tev.coherent_domains);
      emit(engine, device, ++g_seq, "translate", detail);
      return NXINPUT_GODOT_SEAM_BLOCK_MAPPING;
    }
    (void)snprintf(decl.mapping, sizeof decl.mapping, "%s", translated);
    (void)snprintf(decl.domain, sizeof decl.domain, "%s", "sdl2-evdev");
    (void)snprintf(detail, sizeof detail,
                   "result=ok translated_from=sdl2-ascending-patched to=sdl2-evdev "
                   "translate=%s rewritten_bindings=%u source_proved_by=%u",
                   nxinput_translate_result_name(tr), tev.rewritten_bindings,
                   (unsigned)tev.source_proved_by);
    emit(engine, device, ++g_seq, "translate", detail);
  }

  /* 2. Resolve against the capabilities MEASURED from this engine's own fd. */
  if (nxinput_godot_origin_declare(
          &origin,
          strcmp(decl.domain, "godot") == 0 ? NXINPUT_GODOT_DOMAIN_GODOT
                                            : NXINPUT_GODOT_DOMAIN_SDL2_EVDEV,
          decl.provider, decl.receipt) != 0 ||
      nxinput_godot_caps_init(&caps, device->key_bits, device->key_bit_count,
                              device->abs_bits, device->abs_bit_count) != 0 ||
      device->abs_info == 0 || device->abs_info_count == 0u ||
      nxinput_godot_caps_set_absinfo(&caps, device->abs_info,
                                     device->abs_info_count) != 0) {
    emit(engine, device, ++g_seq, "resolve",
         "result=block reason=origin-or-caps-invalid");
    return NXINPUT_GODOT_SEAM_BLOCK_ORIGIN;
  }
  mapped = nxinput_godot_serve((nxinput_godot_engine)engine->major, &origin,
                               &caps, decl.mapping, served, sizeof served,
                               &evidence);
  if (mapped != NXINPUT_GODOT_BYTE_INTACT &&
      mapped != NXINPUT_GODOT_CONVERTED) {
    (void)snprintf(detail, sizeof detail, "result=block reason=%s detail=%s",
                   nxinput_godot_result_name(mapped), evidence.reason);
    emit(engine, device, ++g_seq, "resolve", detail);
    return NXINPUT_GODOT_SEAM_BLOCK_MAPPING;
  }
  (void)snprintf(detail, sizeof detail,
                 "result=%s rewritten=%u buttons=%u axis=%u hats=%u",
                 nxinput_godot_result_name(mapped),
                 evidence.rewritten_bindings, evidence.button_bindings,
                 evidence.axis_bindings, evidence.hat_bindings);
  emit(engine, device, ++g_seq, "resolve", detail);

  /* 3. The ENGINE's real setter, on the engine's own thread. */
  if (engine->add_joy_mapping(engine->userdata, served) != 0 ||
      engine->has_mapping(engine->userdata, device->joy_id) == 0) {
    emit(engine, device, ++g_seq, "setter",
         "result=block reason=engine-refused-or-did-not-store-mapping");
    return NXINPUT_GODOT_SEAM_BLOCK_SETTER;
  }
  emit(engine, device, ++g_seq, "setter", "result=ok engine_stored_mapping=1");

  /* 4. The ENGINE's own readback, probed over every physical button it can
   * name. probe_count == 0 can never admit a mapping that binds anything. */
  for (ordinal = 0u; ordinal < NXINPUT_GODOT_SEAM_MAX_PROBES; ordinal++) {
    int code = nxinput_godot_button_code(NXINPUT_GODOT_DOMAIN_GODOT, &caps,
                                         ordinal);
    int want;
    int got;

    if (code < 0) {
      break;
    }
    want = expected_logical(served, ordinal, engine->major);
    got = engine->readback_logical_button(engine->userdata, device->joy_id,
                                          (int)ordinal);
    probes++;
    if (want != got) {
      (void)snprintf(detail, sizeof detail,
                     "result=block reason=engine-readback-disagrees "
                     "ordinal=%u evdev=0x%x served=%d engine=%d",
                     ordinal, (unsigned int)code, want, got);
      emit(engine, device, ++g_seq, "readback", detail);
      return NXINPUT_GODOT_SEAM_BLOCK_READBACK;
    }
    agreed++;
  }
  if (probes == 0u || agreed != probes) {
    (void)snprintf(detail, sizeof detail,
                   "result=block reason=no-usable-probe probes=%u agreed=%u",
                   probes, agreed);
    emit(engine, device, ++g_seq, "probes", detail);
    return NXINPUT_GODOT_SEAM_BLOCK_PROBES;
  }
  (void)snprintf(detail, sizeof detail, "result=ok probes=%u agreed=%u",
                 probes, agreed);
  emit(engine, device, ++g_seq, "readback", detail);

  emit(engine, device, ++g_seq, "announce",
       "result=admit reason=engine-accepted-and-confirmed-before-announce");
  return NXINPUT_GODOT_SEAM_ADMIT;
}
