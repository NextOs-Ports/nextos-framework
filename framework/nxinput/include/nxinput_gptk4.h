/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK4_H
#define NXINPUT_GPTK4_H
/*
 * nxinput_gptk4 -- NEXTOS_CONTROLLERS/4 (V5, mission 6).
 *
 * The owner file `<port>/NEXTOSCONTROLLERS.gptk` in schema 4:
 *
 *   format = NEXTOS_CONTROLLERS/4
 *   port = <id>
 *   CONTROL_STANDARD = xbox        (the only value; positional aliases)
 *   GLYPH_STYLE = xbox | <opt-in theme>   (drawing only, never position)
 *   AUTHORITY = nextos | engine | synchronized
 *   CONTEXT_POLICY = unified
 *
 *   [base]            exclusive owner of the DISCRETE controls:
 *                     A B X Y L1 R1 L3 R3 START SELECT GUIDE UP DOWN LEFT RIGHT
 *   [stick.left]      exclusive owner of LEFT_STICK (+ 4 derived directions)
 *   [stick.right]     exclusive owner of RIGHT_STICK
 *   [trigger.left]    exclusive owner of L2
 *   [trigger.right]   exclusive owner of R2
 *   [override.<ctx>]  SPARSE per-context exceptions (menu, pause, dialog,
 *                     cursor, coop, ...), same keys as the section they touch
 *                     (discrete keys, or stick.left.up etc. spelled as keys)
 *   [keyboard.base]   physical keyboard SOURCE: KEY_CHORD = digital_binding
 *   [keyboard.override.<ctx>]
 *
 * Grammar (6.4):
 *   digital_binding := native | null | action:<id> | action:<id>@key:<chord>
 *   analog_binding  := native | null | action:<id>
 *   vector2_binding := native | null | action:<id>
 * Every action id must exist in the adapter contract with a value_kind
 * (digital | scalar | vector2); a binding of the wrong kind is an error.
 *
 * Positional aliases: A=FACE_SOUTH B=FACE_EAST X=FACE_WEST Y=FACE_NORTH.
 *
 * COMPLETENESS: every core control, both sticks (mode + vector + four
 * directions) and both triggers (mode + analog + digital) must be present in
 * a complete file. Omission is an error (NXI4001), never a silent null.
 *
 * Errors carry line and column; the owner text is never rewritten by the
 * loader. Pure: no I/O; the caller hands the bytes.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK4_MAGIC "format = NEXTOS_CONTROLLERS/4"
#define NXINPUT_GPTK4_MAX_BYTES 65536u
#define NXINPUT_GPTK4_ACTION_MAX 64u
#define NXINPUT_GPTK4_KEY_MAX 48u
#define NXINPUT_GPTK4_MAX_OVERRIDES 8u
#define NXINPUT_GPTK4_MAX_KEYBOARD 64u
#define NXINPUT_GPTK4_MAX_ACTIONS 96u

/* Error codes (NXI4xxx). */
enum {
  NXINPUT_GPTK4_ERR_MAGIC = 4000,
  NXINPUT_GPTK4_ERR_OMITTED = 4001,      /* core control missing */
  NXINPUT_GPTK4_ERR_MALFORMED = 4002,
  NXINPUT_GPTK4_ERR_DUPLICATE = 4003,    /* same control twice / two sections */
  NXINPUT_GPTK4_ERR_UNKNOWN = 4004,      /* unknown key/section/action */
  NXINPUT_GPTK4_ERR_KIND = 4005,         /* binding kind vs action value_kind */
  NXINPUT_GPTK4_ERR_NATIVE_DERIVED = 4006, /* native on a derived direction */
  NXINPUT_GPTK4_ERR_MODE = 4007,         /* stick/trigger mode contract */
  NXINPUT_GPTK4_ERR_THRESHOLD = 4008,
  NXINPUT_GPTK4_ERR_KEYBOARD = 4009,     /* @key without keyboard backend,
                                            key outside allowlist, loop */
  NXINPUT_GPTK4_ERR_TOO_LARGE = 4010,
  NXINPUT_GPTK4_ERR_HEADER = 4011,
  NXINPUT_GPTK4_ERR_MIXED_OWNER = 4012   /* native + action in one stick */
};

typedef enum nxinput_gptk4_control {
  NXINPUT_GPTK4_A = 0, NXINPUT_GPTK4_B, NXINPUT_GPTK4_X, NXINPUT_GPTK4_Y,
  NXINPUT_GPTK4_L1, NXINPUT_GPTK4_R1, NXINPUT_GPTK4_L3, NXINPUT_GPTK4_R3,
  NXINPUT_GPTK4_START, NXINPUT_GPTK4_SELECT, NXINPUT_GPTK4_GUIDE,
  NXINPUT_GPTK4_UP, NXINPUT_GPTK4_DOWN, NXINPUT_GPTK4_LEFT, NXINPUT_GPTK4_RIGHT,
  NXINPUT_GPTK4_LS_UP, NXINPUT_GPTK4_LS_DOWN, NXINPUT_GPTK4_LS_LEFT, NXINPUT_GPTK4_LS_RIGHT,
  NXINPUT_GPTK4_RS_UP, NXINPUT_GPTK4_RS_DOWN, NXINPUT_GPTK4_RS_LEFT, NXINPUT_GPTK4_RS_RIGHT,
  NXINPUT_GPTK4_L2_DIGITAL, NXINPUT_GPTK4_R2_DIGITAL,
  NXINPUT_GPTK4_DIGITAL_COUNT,
  /* non-digital slots */
  NXINPUT_GPTK4_LS_VECTOR = NXINPUT_GPTK4_DIGITAL_COUNT, NXINPUT_GPTK4_RS_VECTOR,
  NXINPUT_GPTK4_L2_ANALOG, NXINPUT_GPTK4_R2_ANALOG,
  NXINPUT_GPTK4_SLOT_COUNT
} nxinput_gptk4_control;

typedef enum nxinput_gptk4_kind { NXINPUT_GPTK4_UNSET = 0, NXINPUT_GPTK4_NATIVE, NXINPUT_GPTK4_NULL, NXINPUT_GPTK4_ACTION } nxinput_gptk4_kind;
typedef enum nxinput_gptk4_value_kind { NXINPUT_GPTK4_V_DIGITAL = 0, NXINPUT_GPTK4_V_SCALAR, NXINPUT_GPTK4_V_VECTOR2 } nxinput_gptk4_value_kind;
typedef enum nxinput_gptk4_authority { NXINPUT_GPTK4_AUTH_NEXTOS = 0, NXINPUT_GPTK4_AUTH_ENGINE, NXINPUT_GPTK4_AUTH_SYNCHRONIZED } nxinput_gptk4_authority;
typedef enum nxinput_gptk4_stick_mode { NXINPUT_GPTK4_STICK_VECTOR = 0, NXINPUT_GPTK4_STICK_DIGITAL, NXINPUT_GPTK4_STICK_SPLIT } nxinput_gptk4_stick_mode;
typedef enum nxinput_gptk4_trigger_mode { NXINPUT_GPTK4_TRIGGER_ANALOG = 0, NXINPUT_GPTK4_TRIGGER_DIGITAL } nxinput_gptk4_trigger_mode;

typedef struct nxinput_gptk4_binding {
  uint8_t kind;                       /* nxinput_gptk4_kind */
  char action[NXINPUT_GPTK4_ACTION_MAX + 1];
  char key[NXINPUT_GPTK4_KEY_MAX + 1]; /* canonical "LCTRL+S", "" if none */
  uint16_t line;
} nxinput_gptk4_binding;

typedef struct nxinput_gptk4_stick {
  uint8_t mode; float enter, exit; uint8_t eight_way; uint8_t tie_horizontal;
  float dir_enter, dir_exit, dig_enter, dig_exit; /* split only */
} nxinput_gptk4_stick;
typedef struct nxinput_gptk4_trigger { uint8_t mode; float enter, exit; } nxinput_gptk4_trigger;

typedef struct nxinput_gptk4_layer {
  char context[24];                   /* "" = base */
  nxinput_gptk4_binding slot[NXINPUT_GPTK4_SLOT_COUNT];
} nxinput_gptk4_layer;

typedef struct nxinput_gptk4_keybind { char chord[NXINPUT_GPTK4_KEY_MAX + 1]; nxinput_gptk4_binding binding; char context[24]; } nxinput_gptk4_keybind;

/* The adapter contract the parser validates against. */
typedef struct nxinput_gptk4_action_decl { const char *id; uint8_t value_kind; } nxinput_gptk4_action_decl;
#define NXINPUT_GPTK4_MAX_EXT 8u
#define NXINPUT_GPTK4_EXT_NAME_MAX 24u
/* E4a: capability-gated EXTENSION controls (MISC, paddles, touchpad click,
 * ...). The adapter declares them from the provider/capability descriptor
 * with a stable name in the `EXT.` namespace (`EXT.MISC1`, `EXT.PADDLE1`,
 * `EXT.TOUCHPAD`); the owner binds them in [base] like any digital control.
 * A declared extension omitted from the owner is NXI4001 (never silent
 * null); an `EXT.*` key the adapter did not declare is NXI4004 (no
 * improvised public names). */
typedef struct nxinput_gptk4_contract {
  const nxinput_gptk4_action_decl *actions; size_t count;
  uint8_t keyboard_backend;   /* 1 when a proved keyboard output route exists */
  const char *const *contexts; size_t context_count; /* declared overlays */
  const char *const *extensions; size_t extension_count; /* E4a, `EXT.NAME` */
} nxinput_gptk4_contract;

typedef struct nxinput_gptk4 {
  char port[65];
  uint8_t authority, glyph_style_opt_in;
  char glyph_style[24];
  nxinput_gptk4_layer base;
  nxinput_gptk4_layer override[NXINPUT_GPTK4_MAX_OVERRIDES]; unsigned overrides;
  nxinput_gptk4_stick left, right; nxinput_gptk4_trigger l2, r2;
  nxinput_gptk4_keybind keyboard[NXINPUT_GPTK4_MAX_KEYBOARD]; unsigned keybinds;
  struct { char name[NXINPUT_GPTK4_EXT_NAME_MAX + 1]; nxinput_gptk4_binding binding; } ext[NXINPUT_GPTK4_MAX_EXT]; unsigned exts;
  uint64_t digest;                    /* fnv1a64 of the accepted bytes */
} nxinput_gptk4;

typedef struct nxinput_gptk4_error { int code; unsigned line, column; char what[96]; } nxinput_gptk4_error;

/* Parse a COMPLETE owner file. Returns 0 or -1 with `err` filled. */
int nxinput_gptk4_parse(const char *text, size_t len, const nxinput_gptk4_contract *contract, nxinput_gptk4 *out, nxinput_gptk4_error *err);

/* Resolution step 1 (6.2): merge base + the ACTIVE overlays in declared
 * total order (later wins) for one digital slot. Returns the binding; unknown
 * context => base passthrough. */
const nxinput_gptk4_binding *nxinput_gptk4_resolve(const nxinput_gptk4 *g, const char *context, nxinput_gptk4_control slot);

/* Serialize the canonical COMPLETE default for a contract: the single
 * generator source (E10). Writes into `out`; returns length or -1. */
int nxinput_gptk4_default(const char *port, const nxinput_gptk4_contract *contract, char *out, size_t cap);

const char *nxinput_gptk4_control_name(nxinput_gptk4_control c);
/* Canonical key chord: accepts modifiers in any order, serializes
 * LCTRL/RCTRL, LALT/RALT, LSHIFT/RSHIFT, LGUI/RGUI + KEYSYM. Returns 0 or -1. */
int nxinput_gptk4_canonical_key(const char *in, char *out, size_t cap);
/* E4a: the binding of a declared extension, NULL when not declared. */
const nxinput_gptk4_binding *nxinput_gptk4_ext(const nxinput_gptk4 *g, const char *name);
/* A valid extension name: `EXT.` + [A-Z0-9_]{1,19}. */
int nxinput_gptk4_ext_name_valid(const char *name);
#ifdef __cplusplus
}
#endif
#endif
