/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_SDL_SEAM_H
#define NXINPUT_SDL_SEAM_H

/*
 * nxinput_sdl_seam -- V4-CONTROLLERS-03 / C6: the PRODUCTION seam that is
 * compiled and linked INTO a real SDL2 and a real SDL3.
 *
 * WHY IT LIVES INSIDE SDL
 * -----------------------
 * C5B learned the lesson the hard way for Godot: a separate program that
 * queries an engine which is merely present proves nothing about that
 * engine's own setter, readback and announce. C6 does not repeat it. This
 * file is called from inside the SDL Linux joystick backend, in
 * MaybeAddDevice(), on SDL's own thread, holding SDL's own joystick lock,
 * IMMEDIATELY BEFORE SDL_PrivateJoystickAdded(). At that point:
 *
 *   - nothing has been announced: no SDL_JOYDEVICEADDED / SDL_EVENT_JOYSTICK_ADDED
 *     and no SDL_EVENT_GAMEPAD_ADDED has been pushed;
 *   - nothing has been classified: SDL_IsGameController() / SDL_IsGamepad()
 *     has not been called for this device, because SDL2 calls it INSIDE
 *     SDL_PrivateJoystickAdded and SDL3 answers it from the announced list;
 *   - nothing has been opened: SDL_GameControllerOpen() / SDL_OpenGamepad()
 *     cannot have run for a device the application has not been told about.
 *
 * A pad the seam does not admit is never announced. That is the only way a
 * refusal can be honest: the game does not get a half-configured pad, it
 * gets no pad, and the port fails before gameplay exactly as C3 requires.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * It does NOT decide the mapping. The decision is `nxinput_authority_admit`,
 * which is `nxinput_sovereign_resolve`, which is the C3 order, LITERALLY:
 *
 *   live get_controls -> official CFW GUID -> pinned bundle -> built-in ->
 *   declared raw -> explicit failure
 *
 * There is no second, local order here. The seam supplies the real sources,
 * the real measured capabilities and the real SDL setter/readback, and then
 * obeys the answer. `env`, file and bundle do not create a parallel ranking:
 * they are steps 1, 2 and 3 of that one order, resolved by C3 code.
 *
 * The seam also projects one identity detail that belongs to SDL itself,
 * before handing a source to the otherwise exact-GUID C3 resolver. SDL3, and
 * SDL2 since 2.26, put a CRC16 of the device name in bytes 2-3 of a live
 * bus-form GUID, but clear that word when matching mapping databases.
 * PortMaster databases commonly carry the same GUID with that word zero. On
 * both majors, and only when every other GUID byte is identical, the seam
 * fills that zero word from the live GUID. It never derives bindings, matches
 * a name, VID/PID or model; a pre-2.26 SDL2 hands over a zero word and is
 * therefore untouched.
 *
 * One domain projection is also explicit at this boundary. Some official
 * PortMaster mappings enumerate every EV_KEY in ascending joydev order, while
 * current SDL2/SDL3 enumerate gamepad keys first and lower media keys second.
 * The glue may project those bN values only with positive capability proof
 * from the exact event node plus the mapping's volume-key markers. The CFW's
 * semantic choices remain sovereign; only their ordinal representation
 * changes. Native/current mappings remain byte-identical. The resulting line
 * still passes C3 syntax, measured reachability, the real setter and effective
 * readback.
 *
 * The remaining boundary rule the seam adds on top of C3 is imposed by SDL
 * itself: SDL stores mappings BY GUID, while a pad is admitted BY INSTANCE.
 * Two live instances that share a GUID cannot hold divergent mappings in one
 * SDL. C3 keeps their entries independent, correctly; SDL cannot. So the
 * seam arbitrates that collision explicitly and fails closed instead of
 * letting whichever pad arrived last silently redefine the other.
 *
 * PORTABLE: no SDL headers. Every SDL call arrives through the ops table that
 * the per-major glue fills with the real API of the SDL it is linked into.
 * That keeps this file C, testable and identical in both binaries.
 */

#include "nxinput_authority.h"
#include "nxinput_livedb.h"
#include "nxinput_provider.h"
#include "nxinput_sdl.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_SDL_SEAM_API_VERSION 1u
#define NXINPUT_SDL_SEAM_GUID_MAX 40u
#define NXINPUT_SDL_SEAM_PATH_MAX 256u
#define NXINPUT_SDL_SEAM_MAX_DEVICES NXINPUT_AUTHORITY_MAX_DEVICES

/* The real SDL operations of the major this seam is linked into. Every
 * member is mandatory; a partial table is refused rather than worked around,
 * because a missing readback is exactly the hole that would let an unproved
 * mapping through. */
typedef struct nxinput_sdl_seam_ops {
  uint32_t api_version;
  size_t struct_size;
  void *userdata;
  uint8_t api;                 /* nxinput_sdl_api: which major is linked */

  /* The C3 sources. Read-only; none of them decides anything alone. */
  const char *(*getenv_fn)(void *userdata, const char *name);
  int (*read_text_fn)(void *userdata, const char *path, char *out, size_t cap);

  /* The REAL setter of this SDL: SDL_GameControllerAddMapping (SDL2) or
   * SDL_AddGamepadMapping (SDL3). 0 on success. */
  int (*add_mapping_fn)(void *userdata, const char *line);
  /* The REAL readback of this SDL: SDL_GameControllerMappingForGUID (SDL2)
   * or SDL_GetGamepadMappingForGUID (SDL3). 0 on success, and `out` holds
   * what SDL EFFECTIVELY has for that GUID right now -- which is not
   * necessarily what the setter was handed, because a USER-priority mapping
   * imported from the environment outranks an API-priority one. */
  int (*mapping_for_guid_fn)(void *userdata, const char *guid, char *out,
                             size_t cap);

  /* Process facts, from the SDL process itself. */
  uint64_t (*monotonic_ns)(void *userdata);
  long (*pid)(void *userdata);
  long (*tid)(void *userdata);
  /* Append one receipt line durably. Must not go through a scratch $WORK
   * that the battery can rewrite afterwards. */
  void (*receipt_fn)(void *userdata, const char *line);

  /* SDL always ships a built-in mapping database, so authority 4 exists. */
  int runtime_has_builtin;
  /* Authority 5 (raw passthrough) is legal ONLY when the port declared that
   * its consumer understands a raw pad. Never inferred. */
  int consumer_accepts_raw;

  /*
   * The STAGED live mapping, when the port removed SDL_GAMECONTROLLERCONFIG
   * from the environment before SDL_Init (see stage_before_init below).
   *
   * This is not a fourth source and not a second ranking. It is authority 1 --
   * the live get_controls mapping -- held in private storage instead of in an
   * environment variable, for one reason: left in the environment, SDL
   * imports those same bytes at USER priority during initialisation, which
   * outranks the API priority of anything the seam installs afterwards. The
   * mapping would then win WITHOUT passing the order, which is the opposite
   * of sovereign.
   *
   * NULL means nothing was staged and authority 1 is read from the
   * environment as usual.
   */
  const char *staged_mapping;

  /* Optional trailing domain adapter. `source` is already projected to the
   * live GUID, so this callback may change only ordinal representation,
   * never identity or authority order. It writes a complete source into
   * `out` and reports how many mapping lines/bindings changed. NULL preserves
   * all source bytes. Return 0 on success, -1 on an unprovable conversion
   * failure. Kept at the tail so all previously validated member offsets
   * remain unchanged. */
  int (*normalize_source_fn)(void *userdata, uint8_t api,
                             const char *target_guid, const char *source,
                             char *out, size_t cap,
                             unsigned int *rewritten_lines,
                             unsigned int *rewritten_bindings);

  /* 0.10.0 tail. Acquire the LIVE controller database (authority 2) when no
   * SDL_GAMECONTROLLERCONFIG_FILE is declared and authority 1 is empty --
   * the affected CFW recreates that database as a background boot symlink, so the
   * acquisition is a bounded, snapshot-stable wait (nxinput_livedb). NULL
   * keeps the 0.9.0 behaviour (no canonical-path consultation). Runs at
   * most once per admission, never in a frame loop. */
  int (*livedb_acquire_fn)(void *userdata, char *out, size_t cap,
                           nxinput_livedb_receipt *receipt);
  /* The FACE_LAYOUT the port selected before SDL_Init (evidence for the
   * receipt only; the bundle selection already happened in the declare
   * boundary). Values follow nxinput_gptk_face_layout: 0 auto, 1 modern,
   * 2 retro. */
  uint8_t face_layout;

  /*
   * V5 (0.11.0) tail: the PROVIDER DESCRIPTOR of the SDL this process really
   * mapped (nxinput_provider.h). `provider_domain` is the ordinal domain the
   * glue resolved (exported ById API, pinned bytes, or UNDECLARED = UNKNOWN).
   * The seam prints it as target_domain and NEVER substitutes the upstream
   * presumption of the major for it. `source_domain_slot`, when non-NULL, is
   * where the glue's normalizer records the source domain it proved for the
   * last normalized source (nxinput_sdl_domain), so the receipt reports the
   * real pair instead of the V4 "joydev-legacy" label.
   */
  uint8_t provider_domain;
  uint8_t provider_method;      /* nxinput_provider_method, evidence only */
  const uint8_t *source_domain_slot;
  /* 0.11.4 tail (review 2, N2): 1 when the staging boundary LEFT the CFW's
   * SDL_GAMECONTROLLERCONFIG line in the environment because the provider
   * was not decided before init. SDL imported it at USER priority; the
   * seam then stays in stock mode for the whole run even if the provider
   * is decided later (in-process measurement) -- API priority can never
   * outrank the imported line, so "rewritten" would be a lie. */
  uint8_t env_left_for_stock;
} nxinput_sdl_seam_ops;

/* 0.8.1 callers end immediately before the additive normalizer callback.
 * The implementation accepts both layouts under API 1 and never reads the
 * callback from the shorter layout. */
#define NXINPUT_SDL_SEAM_OPS_SIZE_0_8_1 \
  offsetof(nxinput_sdl_seam_ops, normalize_source_fn)
/* 0.9.0 callers end immediately before the 0.10.0 tail. */
#define NXINPUT_SDL_SEAM_OPS_SIZE_0_10_0 \
  (offsetof(nxinput_sdl_seam_ops, provider_domain))
#define NXINPUT_SDL_SEAM_OPS_SIZE_0_9_0 \
  offsetof(nxinput_sdl_seam_ops, livedb_acquire_fn)

/* One device, as SDL itself found it, with its capabilities MEASURED from
 * the very event node SDL chose. */
typedef struct nxinput_sdl_seam_device {
  uint32_t api_version;
  size_t struct_size;
  int32_t instance_id;                       /* SDL's own instance id */
  char guid[NXINPUT_SDL_SEAM_GUID_MAX];      /* as SDL computed it */
  char devpath[NXINPUT_SDL_SEAM_PATH_MAX];   /* the node SDL chose */
  /* Measured counts, from EVIOCGBIT on that node -- never from the mapping's
   * own text and never from a name or VID/PID. */
  int buttons;
  int axes;
  int hats;
  /* 0.10.0 tail: the device name, sanitized and bounded, as EVIDENCE for
   * the receipt only. It never selects a mapping, a domain or a layout. */
  char name[64];
  /* 0.11.1 tail: the joystick DRIVER the provider uses for THIS device
   * (nxinput_provider_driver): evdev nodes carry measured caps; a HIDAPI
   * (hidraw) device has no evdev caps and no ordinal table -- the provider
   * synthesizes its own mapping, so the seam admits it in stock mode
   * instead of blocking it as "device-record-not-usable". */
  uint8_t driver;
} nxinput_sdl_seam_device;

/* 0.9.0 device records end immediately before the evidence name. */
#define NXINPUT_SDL_SEAM_DEVICE_SIZE_0_9_0 \
  offsetof(nxinput_sdl_seam_device, name)
/* 0.10.0 device records end immediately before the driver. */
#define NXINPUT_SDL_SEAM_DEVICE_SIZE_0_10_0 \
  offsetof(nxinput_sdl_seam_device, driver)

typedef enum nxinput_sdl_seam_result {
  NXINPUT_SDL_SEAM_ADMIT = 0,       /* SDL may announce this device */
  NXINPUT_SDL_SEAM_NO_DECLARATION,  /* nothing declared: native behaviour */
  NXINPUT_SDL_SEAM_BLOCK_OPS,       /* the ops table is not usable */
  NXINPUT_SDL_SEAM_BLOCK_IDENTITY,  /* the device record is not usable */
  NXINPUT_SDL_SEAM_BLOCK_AUTHORITY, /* C3 ended in FAIL_EXPLICIT */
  NXINPUT_SDL_SEAM_BLOCK_COLLISION, /* same GUID, divergent mappings */
  /* 0.11.1: STOCK MODE. The provider is UNKNOWN (no measured/pinned
   * table) or the device is driven by HIDAPI: the seam translates
   * NOTHING, mutates the store with NOTHING external, and lets the
   * provider behave exactly as stock (its own environment import, its
   * built-in database, its HIDAPI mapping). The pad is announced; the
   * receipt says DO_NOT_MUTATE_STORE + passthrough. The ONE exception is
   * the CFW's own SDL_GAMECONTROLLERCONFIG line that a LEGACY staging call
   * removed before the descriptor existed: it is handed back to the setter
   * untranslated (EXISTING_NATIVE_PASSTHROUGH: native to that provider by
   * provenance), never a file/bundle line. A pad is never muted because
   * the provider is unknown (review finding 1, 2026-09-03). */
  NXINPUT_SDL_SEAM_ADMIT_STOCK
} nxinput_sdl_seam_result;

/* One admitted instance, so a later collision or hotplug can be judged. */
typedef struct nxinput_sdl_seam_slot {
  int in_use;
  int32_t instance_id;
  char guid[NXINPUT_SOVEREIGN_GUID_MAX];
  char line[NXINPUT_SOVEREIGN_LINE_MAX];
  nxinput_sovereign_source source;
} nxinput_sdl_seam_slot;

typedef struct nxinput_sdl_seam {
  uint32_t api_version;
  size_t struct_size;
  /* The C3 authority, initialised ONCE per process. It owns the per-instance
   * entries, so it must survive between admissions: re-initialising it on
   * every call would wipe the very table that makes a hotplug, a second pad
   * or a reconnection distinguishable. */
  nxinput_authority authority;
  /* What the current admission is about. The authority addresses devices by
   * index through a bridge; these two are what that bridge reads, and they
   * are set immediately before each call and never cached. */
  const void *current_ops;
  const void *current_device;
  /* Per-admission SDL view of authority 1. It may change the bus-form zero
   * CRC16 word and a positively proved ordinal domain, as described above,
   * and is freed before admit returns. */
  char *current_mapping_view;
  unsigned int current_crc_aliases;
  /* 0.10.0: the live-database snapshot of the CURRENT admission and its
   * receipt. The snapshot is freed before admit returns; the receipt only
   * feeds the evidence lines. */
  char *current_db_snapshot;
  nxinput_livedb_receipt current_db_receipt;
  nxinput_sdl_seam_slot slots[NXINPUT_SDL_SEAM_MAX_DEVICES];
  unsigned int sequence;        /* one counter for the whole process */
  unsigned int admitted;
  unsigned int blocked;
  unsigned int collisions;
  unsigned int forgotten;
  int initialised;
  /* V5 (C3): a same-GUID divergent line was refused AT THE SETTER, before
   * the store could change. Read by admit() to report the collision. */
  int pending_collision;
  int32_t pending_collision_instance;
} nxinput_sdl_seam;

/* Prepare the seam for a process. Safe to call more than once: the first
 * call wins and the rest are no-ops, because SDL may re-enter detection. */
int nxinput_sdl_seam_init(nxinput_sdl_seam *seam,
                          const nxinput_sdl_seam_ops *ops);

/* THE boundary. Returns ADMIT only after the C3 order chose a line, the real
 * SDL setter accepted it and the real SDL readback returned the same
 * semantics. Any other value means the caller MUST NOT announce the device.
 *
 * NO_DECLARATION means the port declared nothing for this run: SDL keeps its
 * own behaviour untouched, which is what an unadopted game must get. The
 * seam adds nothing and takes nothing away in that case. */
nxinput_sdl_seam_result nxinput_sdl_seam_admit(
    nxinput_sdl_seam *seam, const nxinput_sdl_seam_ops *ops,
    const nxinput_sdl_seam_device *device);

/* Hotplug. Invalidates ONLY the instance named, so a second pad that is
 * still connected keeps its decision and its state. */
void nxinput_sdl_seam_forget(nxinput_sdl_seam *seam,
                             const nxinput_sdl_seam_ops *ops,
                             int32_t instance_id);

/* The mapping line this instance was admitted with, or NULL. */
const nxinput_sdl_seam_slot *nxinput_sdl_seam_find(
    const nxinput_sdl_seam *seam, int32_t instance_id);

const char *nxinput_sdl_seam_result_name(nxinput_sdl_seam_result result);

/* ------------------------------------------------------ pre-init staging */
/*
 * A separate, EARLIER boundary, and the reason it exists is concrete: SDL
 * imports SDL_GAMECONTROLLERCONFIG during initialisation at USER priority,
 * which outranks the API priority of any mapping the seam later installs.
 * Left in place, the environment silently wins and the sovereign decision
 * becomes decoration.
 *
 * This call copies the variable into caller-owned storage and then removes it
 * from the environment, BEFORE SDL is initialised. The bytes are not thrown
 * away: they are still authority 1 (live get_controls) when the seam resolves
 * -- they just no longer bypass the decision.
 *
 * Returns 0 on success, -1 when SDL is already initialised (too late to
 * prove anything: the caller must abort rather than pretend) or when the copy
 * or the removal failed. `staged_len` receives the length copied, 0 when the
 * variable was absent, which is a legitimate pass-through.
 */
typedef struct nxinput_sdl_seam_env_ops {
  uint32_t api_version;
  size_t struct_size;
  void *userdata;
  const char *(*getenv_fn)(void *userdata, const char *name);
  int (*unsetenv_fn)(void *userdata, const char *name);
  /* Non-zero when any SDL subsystem is already up (SDL_WasInit(0) != 0). */
  int (*sdl_was_init_fn)(void *userdata);
} nxinput_sdl_seam_env_ops;

int nxinput_sdl_seam_stage_before_init(const nxinput_sdl_seam_env_ops *ops,
                                       char *out, size_t cap,
                                       size_t *staged_len);

/* 0.11.1: staging that KNOWS the provider. The destructive step (removing
 * SDL_GAMECONTROLLERCONFIG so SDL cannot import it at USER priority) is
 * only justified when the seam is going to decide a mapping -- i.e. when
 * the provider descriptor allows a rewrite. With an UNKNOWN provider the
 * variable is LEFT IN PLACE (`*left_for_stock = 1`, staged_len 0): SDL
 * imports its own CFW line exactly as stock, and the seam later admits the
 * pad in stock mode. The legacy call above, which stages blind, is what
 * used to mute every unpinned CFW (review finding 1). */
int nxinput_sdl_seam_stage_with_provider(
    const nxinput_sdl_seam_env_ops *ops,
    const nxinput_provider_descriptor *provider, char *out, size_t cap,
    size_t *staged_len, int *left_for_stock);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_SDL_SEAM_H */
