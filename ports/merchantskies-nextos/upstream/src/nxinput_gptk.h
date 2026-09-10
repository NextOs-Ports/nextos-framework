/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_H
#define NXINPUT_GPTK_H

/* NEXTOSCONTROLLERS.gptk -- the NextOS-own semantic controller mapping.
 *
 * This is NOT gptokeyb and never converts controller input into keyboard
 * keys. A mapping file binds SYMBOLIC physical controls (A, B, L2, ...) to
 * semantic ACTIONS (ui.confirm, player.jump, ...) per context. Numeric evdev
 * codes are rejected on purpose: raw codes are firmware-specific and already
 * caused the Chrono Trigger regression where evdev numbers turned L2/R2 into
 * START/SELECT on another pad.
 *
 * The parser is deliberately dumb and closed: it reads only the memory
 * buffer it is given (the caller performs the symlink-safe file read), it
 * performs no shell expansion, no includes and no evaluation of any kind,
 * and every violation fails closed with a stable NXI#### code while the
 * offending entry stays out of the result.
 *
 * This header is self-contained (no SDL, no nxinput.h) so host tools and
 * tests can build it standalone. nxinput_gptk_control_button() bridges the
 * digital controls onto the nxinput_button values from nxinput.h; a
 * consistency static-assert fires for the shared A/B anchor values when
 * nxinput.h is also included in the same translation unit.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK_API_VERSION 1u

/* First non-comment, non-blank line of a mapping file, byte for byte. */
#define NXINPUT_GPTK_MAGIC "format = NEXTOS_CONTROLLERS/1"

/* Strict fail-closed limits. */
#define NXINPUT_GPTK_MAX_BYTES 65536u
#define NXINPUT_GPTK_MAX_LINES 512u
#define NXINPUT_GPTK_ACTION_MAX 64u   /* characters, excluding NUL */
#define NXINPUT_GPTK_PORT_MAX 64u
#define NXINPUT_GPTK_MAX_SINKS 64u

/* Stable error codes (returned as positive ints, e.g. 1004 = NXI1004). */
#define NXINPUT_GPTK_ERR_UNKNOWN_NAME 1001  /* NXI1001 control/section/action */
#define NXINPUT_GPTK_ERR_MALFORMED 1002     /* NXI1002 malformed line/action */
#define NXINPUT_GPTK_ERR_DUPLICATE 1003     /* NXI1003 duplicate in section */
#define NXINPUT_GPTK_ERR_TOO_LARGE 1004     /* NXI1004 size/line-count limit */
#define NXINPUT_GPTK_ERR_BAD_BYTES 1005     /* NXI1005 invalid UTF-8/control */
#define NXINPUT_GPTK_ERR_BAD_MAGIC 1006     /* NXI1006 missing/wrong magic */

/* ------------------------------------------------------------------ */
/* V3 tuning: [cursor] / [camera] numeric keys (documented bounds).    */
/* All keys are OPT-IN: a file without tuning keys parses to exactly   */
/* the defaults below, which reproduce approved port behavior.         */
/* ------------------------------------------------------------------ */

/* Bounds (inclusive). Out-of-bounds or malformed values are NXI1002. */
#define NXINPUT_GPTK_SPEED_MIN 0.05f       /* screen-heights per second */
#define NXINPUT_GPTK_SPEED_MAX 8.0f
#define NXINPUT_GPTK_DEADZONE_MIN 0.0f     /* fraction of full deflection */
#define NXINPUT_GPTK_DEADZONE_MAX 0.9f
#define NXINPUT_GPTK_CURVE_MIN 0.25f       /* pow() exponent */
#define NXINPUT_GPTK_CURVE_MAX 4.0f
#define NXINPUT_GPTK_ACCEL_MIN 0.0f        /* extra gain at full deflection */
#define NXINPUT_GPTK_ACCEL_MAX 4.0f
#define NXINPUT_GPTK_SMOOTHING_MIN 0.0f    /* milliseconds (time constant) */
#define NXINPUT_GPTK_SMOOTHING_MAX 500.0f
#define NXINPUT_GPTK_SENSITIVITY_MIN 0.05f /* unitless multiplier */
#define NXINPUT_GPTK_SENSITIVITY_MAX 8.0f

/* Camera authority: who applies deadzone/curve/sensitivity. NEVER both --
 * multiplying framework and native sensitivity double-applies tuning. */
typedef enum nxinput_gptk_authority {
  NXINPUT_GPTK_AUTHORITY_NEXTOS = 0, /* NextOS shapes the axes (default) */
  NXINPUT_GPTK_AUTHORITY_NATIVE = 1  /* raw pass-through; native menu governs */
} nxinput_gptk_authority;

/* Fully-populated cursor tuning. The *_set flags are 1 only when the key
 * appeared explicitly in the file; the value fields are ALWAYS valid
 * (defaults where unset). */
typedef struct nxinput_gptk_cursor_tuning {
  float speed;          /* screen-heights/s at full deflection (default 1.0) */
  float deadzone;       /* radial, 0..0.9 (default 0.15) */
  float response_curve; /* pow exponent on normalized magnitude (default 1.0) */
  float acceleration;   /* extra gain term, 0..4 (default 0.0) */
  float smoothing_ms;   /* exponential smoothing time constant (default 0.0) */
  uint8_t speed_set;
  uint8_t deadzone_set;
  uint8_t response_curve_set;
  uint8_t acceleration_set;
  uint8_t smoothing_ms_set;
} nxinput_gptk_cursor_tuning;

/* Fully-populated camera tuning (same *_set discipline). */
typedef struct nxinput_gptk_camera_tuning {
  float sensitivity_x;  /* multiplier, 0.05..8 (default 1.0) */
  float sensitivity_y;  /* multiplier, 0.05..8 (default 1.0) */
  float deadzone;       /* radial, 0..0.9 (default 0.15) */
  float response_curve; /* pow exponent (default 1.0) */
  uint8_t invert_x;     /* 0/1 (default 0) */
  uint8_t invert_y;     /* 0/1 (default 0) */
  uint8_t authority;    /* nxinput_gptk_authority (default NEXTOS) */
  uint8_t sensitivity_x_set;
  uint8_t sensitivity_y_set;
  uint8_t deadzone_set;
  uint8_t response_curve_set;
  uint8_t invert_x_set;
  uint8_t invert_y_set;
  uint8_t authority_set;
} nxinput_gptk_camera_tuning;

/* Fill a tuning struct with the documented defaults, all *_set flags 0. */
void nxinput_gptk_cursor_tuning_defaults(nxinput_gptk_cursor_tuning *t);
void nxinput_gptk_camera_tuning_defaults(nxinput_gptk_camera_tuning *t);

/* Symbolic physical vocabulary. This is the complete accepted set; anything
 * else in the control position of a mapping line -- including any token made
 * of digits -- is NXI1001. */
typedef enum nxinput_gptk_control {
  NXINPUT_GPTK_A = 0,
  NXINPUT_GPTK_B,
  NXINPUT_GPTK_X,
  NXINPUT_GPTK_Y,
  NXINPUT_GPTK_L1,
  NXINPUT_GPTK_R1,
  NXINPUT_GPTK_L2,          /* analog-capable trigger */
  NXINPUT_GPTK_R2,          /* analog-capable trigger */
  NXINPUT_GPTK_L3,
  NXINPUT_GPTK_R3,
  NXINPUT_GPTK_START,
  NXINPUT_GPTK_SELECT,
  NXINPUT_GPTK_UP,
  NXINPUT_GPTK_DOWN,
  NXINPUT_GPTK_LEFT,
  NXINPUT_GPTK_RIGHT,
  NXINPUT_GPTK_LEFT_STICK,  /* analog-capable stick vector */
  NXINPUT_GPTK_RIGHT_STICK, /* analog-capable stick vector */
  NXINPUT_GPTK_CONTROL_COUNT
} nxinput_gptk_control;

typedef enum nxinput_gptk_context {
  NXINPUT_GPTK_CONTEXT_MENU = 0,
  NXINPUT_GPTK_CONTEXT_GAMEPLAY,
  NXINPUT_GPTK_CONTEXT_CURSOR,
  NXINPUT_GPTK_CONTEXT_COUNT
} nxinput_gptk_context;

/* Parsed mapping. Fixed storage only: no allocation, safe to memcpy. */
typedef struct nxinput_gptk {
  uint32_t api_version;
  /* action[c][k][0] == '\0' means "control k unmapped in context c". */
  char action[NXINPUT_GPTK_CONTEXT_COUNT][NXINPUT_GPTK_CONTROL_COUNT]
             [NXINPUT_GPTK_ACTION_MAX + 1u];
  /* 1 when the section header appeared in the file. */
  int context_present[NXINPUT_GPTK_CONTEXT_COUNT];
  char port[NXINPUT_GPTK_PORT_MAX + 1u]; /* "" when no port line */
  /* V3 tuning. Populated with defaults on every successful parse; the
   * *_set flags mark the keys the file set explicitly. */
  nxinput_gptk_cursor_tuning cursor_tuning;
  nxinput_gptk_camera_tuning camera_tuning;
  int camera_present; /* 1 when a [camera] section header appeared */
} nxinput_gptk;

/* Copy the tuning out of a parsed map. Always writes a fully-populated
 * struct: defaults when g is NULL, was cleared by a failed parse, or the
 * file simply did not set the key. */
void nxinput_gptk_cursor_tuning_get(const nxinput_gptk *g,
                                    nxinput_gptk_cursor_tuning *out);
void nxinput_gptk_camera_tuning_get(const nxinput_gptk *g,
                                    nxinput_gptk_camera_tuning *out);

/* Parse a complete mapping buffer. Returns 0 on success, else the positive
 * NXI code (e.g. 1004); on failure *out is fully cleared and, when error is
 * non-NULL, a stable "NXI####: reason" message is written (truncated to
 * error_size, always NUL-terminated when error_size > 0). A file missing the
 * required [menu] or [gameplay] sections is rejected (NXI1002). */
int nxinput_gptk_parse(const char *text, size_t length, nxinput_gptk *out,
                       char *error, size_t error_size);

/* Action bound to a control in a context, or NULL when unmapped/out of
 * range. The returned pointer aliases storage inside *g. */
const char *nxinput_gptk_action(const nxinput_gptk *g,
                                nxinput_gptk_context c, int control);

/* Check every mapped action against the adapter's allowlist. Returns 0 when
 * every action is known, else NXI1001 with the first offender named in
 * error. A NULL/empty allowlist rejects any mapped action. */
int nxinput_gptk_validate_actions(const nxinput_gptk *g,
                                  const char *const *allowed,
                                  size_t allowed_count, char *error,
                                  size_t error_size);

/* nxinput_button value (from nxinput.h) behind a digital control, or -1 for
 * the analog-only controls (L2, R2, LEFT_STICK, RIGHT_STICK). Kept as plain
 * ints so this header stays standalone. */
int nxinput_gptk_control_button(int control);

/* ------------------------------------------------------------------ */
/* Dispatcher: physical edges in, semantic actions out.                */
/* ------------------------------------------------------------------ */

typedef void (*nxinput_gptk_sink_fn)(void *user, const char *action,
                                     int pressed, float value);

typedef struct nxinput_gptk_sink {
  char action[NXINPUT_GPTK_ACTION_MAX + 1u];
  nxinput_gptk_sink_fn fn;
  void *user;
} nxinput_gptk_sink;

/* Vector sink for the analog cursor/camera path (V3 blocker 7). Delivers the
 * transformed pair: for a cursor.* action, (ax,ay) is the ABSOLUTE cursor
 * position in pixels; for a camera.* action, (ax,ay) is the shaped camera
 * axis pair. The dispatcher runs the stick vector through the kinematics
 * (nxinput_gptk_motion) using the map's [cursor]/[camera] tuning. */
typedef void (*nxinput_gptk_vector_sink_fn)(void *user, const char *action,
                                            float ax, float ay);

typedef struct nxinput_gptk_vector_sink {
  char action[NXINPUT_GPTK_ACTION_MAX + 1u];
  nxinput_gptk_vector_sink_fn fn;
  void *user;
} nxinput_gptk_vector_sink;

typedef struct nxinput_gptk_dispatcher {
  const nxinput_gptk *map; /* not owned; must outlive the dispatcher */
  nxinput_gptk_context context;
  nxinput_gptk_sink sinks[NXINPUT_GPTK_MAX_SINKS];
  size_t sink_count;
  /* One latch bit per control: set while the control is logically pressed
   * in the current context. Edge discipline lives here, not in the sinks. */
  uint32_t latched;
  /* V3 (blocker 7): the stick-vector path is INTEGRATED into the dispatcher,
   * not a loose helper. These mirror an nxinput_gptk_cursor_state (kept as
   * plain floats so this header stays free of nxinput_gptk_motion.h, which
   * itself depends on this one). */
  nxinput_gptk_vector_sink vector_sinks[NXINPUT_GPTK_MAX_SINKS];
  size_t vector_sink_count;
  float cursor_x;
  float cursor_y;
  float cursor_vel_x;
  float cursor_vel_y;
  int drawable_w;
  int drawable_h;
  int motion_configured;
} nxinput_gptk_dispatcher;

/* Configure the cursor drawable for the vector path. Places the cursor at the
 * drawable centre with zero velocity. Returns 0, or -1 on a non-positive
 * drawable. Required before a cursor.* stick delivers anything; camera.* does
 * not need it. */
int nxinput_gptk_dispatcher_configure_motion(nxinput_gptk_dispatcher *d,
                                             int drawable_w, int drawable_h);

/* Register a vector sink for a cursor.* or camera.* action. Same fan-out rule as
 * the digital sinks. Returns 0, or -1 on bad input / full table. */
int nxinput_gptk_dispatcher_register_vector(nxinput_gptk_dispatcher *d,
                                            const char *action,
                                            nxinput_gptk_vector_sink_fn fn,
                                            void *user);

/* Feed a stick vector (x,y in -1..1) for LEFT_STICK or RIGHT_STICK. If the
 * stick is mapped to a cursor.* action in the current context, the vector is
 * run through the cursor kinematics and the sink receives the absolute cursor
 * position; if mapped to a camera.* action, through the camera transform. Any
 * other mapping (or unmapped) delivers nothing on this path. */
void nxinput_gptk_dispatcher_feed_stick(nxinput_gptk_dispatcher *d, int control,
                                        float x, float y, float dt_seconds);

/* Double-read guard (blocker 7), PER CONTROL. Returns a bitmask whose bit
 * `1u << control` is set for each physical stick the live context hands to a
 * cursor.* or camera.* action -- i.e. the sticks the framework owns and the
 * guest MUST NOT also read raw. Only NXINPUT_GPTK_LEFT_STICK / RIGHT_STICK can
 * appear. The adapter suppresses EXACTLY these bits, so a game that maps only
 * the right stick keeps its native left stick (no blanket steal). Derived from
 * the live mapping, never a standalone flag; 0 means read both raw. */
uint32_t nxinput_gptk_dispatcher_suppressed_mask(
    const nxinput_gptk_dispatcher *d);

/* Convenience per-control query over the mask above: 1 when THIS control is
 * owned by the framework in the live context, 0 otherwise (out-of-range 0). */
int nxinput_gptk_dispatcher_control_suppressed(
    const nxinput_gptk_dispatcher *d, int control);

/* Whole-pad convenience: 1 when ANY stick is owned (mask != 0). Prefer the
 * mask / per-control query so a single owned stick never suppresses the other. */
int nxinput_gptk_dispatcher_physical_suppressed(
    const nxinput_gptk_dispatcher *d);

/* Bind a dispatcher to a parsed map. Starts in MENU with nothing latched. */
void nxinput_gptk_dispatcher_init(nxinput_gptk_dispatcher *d,
                                  const nxinput_gptk *g);

/* Register one sink for one action. Several sinks may share an action; a
 * single physical press is ONE logical action delivered once to EACH of its
 * sinks. Returns 0 on success, -1 on bad input or a full table. */
int nxinput_gptk_dispatcher_register(nxinput_gptk_dispatcher *d,
                                     const char *action,
                                     nxinput_gptk_sink_fn fn, void *user);

/* Switch context. Every latched control first receives its release
 * (pressed=0) in the OLD context, so no sink is ever left holding a phantom
 * press across a context change. */
void nxinput_gptk_dispatcher_set_context(nxinput_gptk_dispatcher *d,
                                         nxinput_gptk_context c);

/* Feed one physical edge. Edge-triggered: a pressed=1 for an already
 * latched control emits nothing, a pressed=0 for a control that is not
 * latched emits nothing. value carries the analog magnitude (0..1, clamped);
 * digital controls use 0/1. Unmapped controls latch silently so a later
 * context switch stays consistent, but emit nothing. */
void nxinput_gptk_dispatcher_feed(nxinput_gptk_dispatcher *d, int control,
                                  int pressed, float value);

/* Compile-time consistency with nxinput.h when both headers are visible. */
#ifdef NXINPUT_H
typedef char nxinput_gptk_assert_a[(int)NXINPUT_GPTK_A ==
                                   (int)NXINPUT_BUTTON_A ? 1 : -1];
typedef char nxinput_gptk_assert_b[(int)NXINPUT_GPTK_B ==
                                   (int)NXINPUT_BUTTON_B ? 1 : -1];
#endif

#ifdef __cplusplus
}
#endif

#endif
