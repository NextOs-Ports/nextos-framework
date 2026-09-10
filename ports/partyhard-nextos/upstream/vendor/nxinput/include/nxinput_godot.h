/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GODOT_H
#define NXINPUT_GODOT_H

/*
 * nxinput_godot -- V4-CONTROLLERS-03 / C5 (corrected by audit 116A):
 * serving the sovereign mapping to Godot 3 and Godot 4.
 *
 * WHAT THE 116A AUDIT CHANGED, AND WHY
 * ------------------------------------
 * The first version decided the mapping's ordinal domain by looking at the
 * SEMANTIC NAME of each binding and assuming a canonical evdev code for it
 * (`a` must be BTN_SOUTH, `b` must be BTN_EAST, ...). That premise is wrong:
 * `a`, `b`, `x`, `y` are LOGICAL functions, and a mapping exists precisely to
 * say that this pad's `a` sits on a different physical button. A real Godot
 * 3.5.3 run in this repository's gate proves it: with `a:b1,b:b0` applied
 * through `Input.add_joy_mapping`, pressing physical BTN_A makes the engine
 * report logical index 1, and BTN_B reports index 0. Any inference from the
 * name would have "corrected" that correct mapping.
 *
 * So the source domain is no longer guessed. It must be DECLARED, with the
 * provenance that authorizes it (provider + receipt). An undeclared, unknown
 * or mismatched origin BLOCKS; it is never inferred.
 *
 * THE DOMAINS, transcribed from the pinned upstream sources and re-verified
 * by tests/godot_domain_gate.py against those exact commits:
 *
 *   buttons
 *     SDL2  : [BTN_JOYSTICK, KEY_MAX) then [0, BTN_JOYSTICK)
 *     Godot : [BTN_JOYSTICK, KEY_MAX) then [BTN_MISC, BTN_JOYSTICK)
 *   axes
 *     SDL2  : [0, ABS_MAX)  skipping the pairs it classified as digital hats
 *     Godot : [0, ABS_MISC) skipping ABS_HAT0X..ABS_HAT3Y unconditionally
 *
 * Both scan the button high range first and identically, so a binding whose
 * code is >= BTN_JOYSTICK has the same ordinal in both: most pads do not
 * diverge and their mapping must stay BYTE-INTACT.
 *
 * PURE: no SDL, no Godot headers, no I/O, no environment. Conversion is
 * driven by MEASURED EV_KEY/EV_ABS capability plus the declared domain.
 *
 * WHAT THE 116B AUDIT CHANGED, AND WHY
 * ------------------------------------
 * The 116A parser classified anything it could not parse as BIND_OTHER and
 * then IGNORED it. `a:b`, `a:bx`, `a:b3x`, `a:h0`, `a:h0.3` and `a:z9` all
 * reached the setter untouched. A hat's MASK was never read at all, so a hat
 * direction that does not exist could not be refused, and no `absinfo` was
 * carried, so SDL's half-axis and trigger decisions could not be reproduced.
 *
 * The grammar is now closed and total. Every field past the GUID and the name
 * is either a declared METADATA key (`platform`, `crc`, `type`, `hint`, `sdk`,
 * `hidapi`) or a binding that must parse COMPLETELY:
 *
 *   button := 'b' <1..4 digits>
 *   axis   := ['+'|'-'] 'a' <1..4 digits> ['~']
 *   hat    := 'h' <1..4 digits> '.' <1..4 digits>   mask in {1,2,4,8}
 *
 * Anything else BLOCKS. A hat needs both ABS_HAT<N>X and ABS_HAT<N>Y present
 * in the measured capability. An axis binding needs `absinfo` for the code it
 * resolves to, and a half-range binding needs that half to exist in the range
 * the kernel reported. `~` is an AXIS inversion and is refused on `bN`/`hN.M`.
 *
 * `absinfo` is measured (EVIOCGABS on the fd that belongs to this joy id), it
 * is never derived from the mapping's own text.
 *
 * CLAIM BOUNDARY: what this file computes is a proposal. It does NOT
 * constitute a readback. The readback that authorizes announcing a joypad
 * must come from the engine's own API -- see nxinput_godot_consumer.h.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GODOT_API_VERSION 3u
#define NXINPUT_GODOT_LINE_MAX 1024u
#define NXINPUT_GODOT_MAX_BINDINGS 48u
#define NXINPUT_GODOT_PROVIDER_MAX 64u
#define NXINPUT_GODOT_RECEIPT_MAX 80u

/* Linux input codes, declared here so the module needs no <linux/input.h>. */
#define NXINPUT_GODOT_BTN_MISC 0x100
#define NXINPUT_GODOT_BTN_JOYSTICK 0x120
#define NXINPUT_GODOT_KEY_MAX 0x2ff
#define NXINPUT_GODOT_KEY_BITS (NXINPUT_GODOT_KEY_MAX + 1)
#define NXINPUT_GODOT_ABS_HAT0X 0x10
#define NXINPUT_GODOT_ABS_HAT3Y 0x17
#define NXINPUT_GODOT_ABS_MISC 0x28
#define NXINPUT_GODOT_ABS_MAX 0x40
#define NXINPUT_GODOT_ABS_BITS (NXINPUT_GODOT_ABS_MAX + 1)
#define NXINPUT_GODOT_HAT_COUNT 4u

/* SDL's hat mask. Exactly one direction per binding; 0 and any combination
 * are refused, because a mapping cannot bind a key to two directions. */
#define NXINPUT_GODOT_HAT_UP 1u
#define NXINPUT_GODOT_HAT_RIGHT 2u
#define NXINPUT_GODOT_HAT_DOWN 4u
#define NXINPUT_GODOT_HAT_LEFT 8u

typedef enum nxinput_godot_engine {
  NXINPUT_GODOT_ENGINE_3 = 0,
  NXINPUT_GODOT_ENGINE_4,
  NXINPUT_GODOT_ENGINE_COUNT
} nxinput_godot_engine;

typedef enum nxinput_godot_domain {
  NXINPUT_GODOT_DOMAIN_UNDECLARED = 0,
  NXINPUT_GODOT_DOMAIN_GODOT,
  NXINPUT_GODOT_DOMAIN_SDL2_EVDEV,
  NXINPUT_GODOT_DOMAIN_COUNT
} nxinput_godot_domain;

typedef enum nxinput_godot_result {
  NXINPUT_GODOT_BYTE_INTACT = 0, /* origin already is the engine's domain */
  NXINPUT_GODOT_CONVERTED,       /* domains differ; converted exactly once */
  NXINPUT_GODOT_ORIGIN_UNDECLARED, /* blocked: nothing authorizes a domain */
  NXINPUT_GODOT_UNREACHABLE,     /* blocked: engine cannot name a binding */
  NXINPUT_GODOT_DUPLICATE,       /* blocked: same key bound twice */
  NXINPUT_GODOT_INVALID,
  NXINPUT_GODOT_SYNTAX,   /* blocked: a field is neither declared metadata
                           * nor a binding that parses completely */
  NXINPUT_GODOT_HAT,      /* blocked: hat ordinal, mask or hat pair absent */
  NXINPUT_GODOT_ABSINFO   /* blocked: an axis binding has no measured
                           * absinfo, or names a half its range has not */
} nxinput_godot_result;

/* One EVIOCGABS answer, as the kernel reported it for one ABS code. Indexed
 * by the evdev ABS code itself, so entry `i` describes ABS code `i`. */
typedef struct nxinput_godot_absinfo {
  int32_t minimum;
  int32_t maximum;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
  uint8_t present;   /* 0 when the kernel reported no range for this code */
  uint8_t reserved[3];
} nxinput_godot_absinfo;

/* WHERE the mapping came from, and therefore which domain it is written in.
 * This is a declaration by the caller, backed by provenance -- never a guess
 * made from the mapping's contents. */
typedef struct nxinput_godot_origin {
  uint32_t api_version;
  size_t struct_size;
  uint8_t domain;                              /* nxinput_godot_domain */
  char provider[NXINPUT_GODOT_PROVIDER_MAX];   /* e.g. "portmaster-gui" */
  char receipt[NXINPUT_GODOT_RECEIPT_MAX];     /* content address / receipt */
} nxinput_godot_origin;

/* MEASURED capability of the pad. Both bitmaps are required: a mapping that
 * binds axes cannot be validated from EV_KEY alone. */
typedef struct nxinput_godot_caps {
  uint32_t api_version;
  size_t struct_size;
  const unsigned long *key_bits;
  size_t key_bit_count;
  const unsigned long *abs_bits;
  size_t abs_bit_count;
  /* Added by 116B. A caller built against API 2 stays valid: `struct_size`
   * distinguishes the two layouts and the older one simply carries no
   * absinfo, which blocks axis bindings instead of silently accepting
   * them. Nothing already published is broken by these members. */
  const nxinput_godot_absinfo *abs_info;
  size_t abs_info_count;
} nxinput_godot_caps;

/* The API-2 layout, kept so an older caller is still accepted. */
#define NXINPUT_GODOT_CAPS_SIZE_V2 \
  (offsetof(nxinput_godot_caps, abs_info))

typedef struct nxinput_godot_evidence {
  uint32_t api_version;
  size_t struct_size;
  uint8_t engine;
  uint8_t source_domain;
  uint8_t result;
  uint8_t internal_consistency; /* 1 when the CONVERTER re-derived its own
                                 * output successfully. This is INTERNAL
                                 * CONSISTENCY, never an engine readback. */
  unsigned int keys;
  unsigned int godot_buttons;
  unsigned int ignored_low;
  unsigned int axes;
  unsigned int godot_axes;
  unsigned int button_bindings;
  unsigned int axis_bindings;
  unsigned int hat_bindings;
  unsigned int metadata_fields;
  unsigned int half_axis_bindings;
  unsigned int inverted_axis_bindings;
  unsigned int rewritten_bindings;
  unsigned int unreachable_bindings;
  unsigned int dropped_bindings;
  char provider[NXINPUT_GODOT_PROVIDER_MAX];
  char receipt[NXINPUT_GODOT_RECEIPT_MAX];
  char reason[128];
} nxinput_godot_evidence;

int nxinput_godot_origin_init(nxinput_godot_origin *origin);
int nxinput_godot_origin_declare(nxinput_godot_origin *origin,
                                 nxinput_godot_domain domain,
                                 const char *provider, const char *receipt);
int nxinput_godot_caps_init(nxinput_godot_caps *caps,
                            const unsigned long *key_bits,
                            size_t key_bit_count,
                            const unsigned long *abs_bits,
                            size_t abs_bit_count);
/* Attach the MEASURED absinfo. `info` is indexed by ABS code. Required
 * before an axis binding can be served. */
int nxinput_godot_caps_set_absinfo(nxinput_godot_caps *caps,
                                   const nxinput_godot_absinfo *info,
                                   size_t info_count);
int nxinput_godot_evidence_init(nxinput_godot_evidence *evidence);

/* Ordinal <-> code, per domain, for the three binding classes. Pure. */
int nxinput_godot_button_code(nxinput_godot_domain domain,
                              const nxinput_godot_caps *caps,
                              unsigned int ordinal);
int nxinput_godot_button_ordinal(nxinput_godot_domain domain,
                                 const nxinput_godot_caps *caps,
                                 unsigned int code);
int nxinput_godot_axis_code(nxinput_godot_domain domain,
                            const nxinput_godot_caps *caps,
                            unsigned int ordinal);
int nxinput_godot_axis_ordinal(nxinput_godot_domain domain,
                               const nxinput_godot_caps *caps,
                               unsigned int code);

/* Serve `mapping`, written in `origin`'s DECLARED domain, to `engine`.
 *
 * Blocks (empty output, joypad must NOT be announced) when the origin is
 * undeclared, when a binding names something the engine cannot enumerate, or
 * when a key is bound twice. Otherwise the mapping is either byte-intact or
 * converted exactly once. Serving an already-served mapping is a no-op. */
nxinput_godot_result nxinput_godot_serve(nxinput_godot_engine engine,
                                         const nxinput_godot_origin *origin,
                                         const nxinput_godot_caps *caps,
                                         const char *mapping, char *out,
                                         size_t out_size,
                                         nxinput_godot_evidence *evidence);

const char *nxinput_godot_domain_name(nxinput_godot_domain domain);
const char *nxinput_godot_result_name(nxinput_godot_result result);
const char *nxinput_godot_engine_name(nxinput_godot_engine engine);
int nxinput_godot_evidence_line(const nxinput_godot_evidence *evidence,
                                char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_GODOT_H */
