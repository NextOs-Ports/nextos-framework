/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_SDL3_PORTMASTER_H
#define NXINPUT_SDL3_PORTMASTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_SDL3_PM_CACHE_MAX 16u
#define NXINPUT_SDL3_PM_MAPPING_MAX 2048u
#define NXINPUT_SDL3_PM_CONSUMER_BUTTON_MAX 32u

enum {
  NXINPUT_SDL3_PM_ERROR = -1,
  NXINPUT_SDL3_PM_NOT_APPLICABLE = 0,
  NXINPUT_SDL3_PM_REWRITTEN = 1,
};

typedef enum nxinput_sdl3_pm_reason {
  NXINPUT_SDL3_PM_REASON_NONE = 0,
  NXINPUT_SDL3_PM_REASON_REWRITTEN,
  NXINPUT_SDL3_PM_REASON_NO_MAPPING,
  NXINPUT_SDL3_PM_REASON_NO_DEVICE_PATH,
  NXINPUT_SDL3_PM_REASON_DEVICE_OPEN_FAILED,
  NXINPUT_SDL3_PM_REASON_DEVICE_STAT_FAILED,
  NXINPUT_SDL3_PM_REASON_NOT_CHARACTER_DEVICE,
  NXINPUT_SDL3_PM_REASON_CAPABILITY_QUERY_FAILED,
  NXINPUT_SDL3_PM_REASON_NATIVE_OR_UNSUPPORTED,
  NXINPUT_SDL3_PM_REASON_CONVERSION_FAILED,
  NXINPUT_SDL3_PM_REASON_REGISTRATION_FAILED,
  NXINPUT_SDL3_PM_REASON_EFFECTIVE_MAPPING_MISMATCH,
  NXINPUT_SDL3_PM_REASON_CACHE_FULL,
  NXINPUT_SDL3_PM_REASON_GUID_COLLISION,
  NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT,
} nxinput_sdl3_pm_reason;

typedef enum nxinput_sdl3_pm_stage {
  NXINPUT_SDL3_PM_STAGE_ENUMERATION = 0,
  NXINPUT_SDL3_PM_STAGE_CLASSIFICATION,
  NXINPUT_SDL3_PM_STAGE_OPEN,
} nxinput_sdl3_pm_stage;

typedef struct nxinput_sdl3_pm_evidence {
  unsigned int key_buttons;
  unsigned int gamepad_buttons;
  unsigned int lower_key_buttons;
  unsigned int button_bindings;
  unsigned int rewritten_bindings;
  unsigned int legacy_volume_markers;
} nxinput_sdl3_pm_evidence;

typedef struct nxinput_sdl3_pm_receipt {
  uint32_t instance_id;
  nxinput_sdl3_pm_stage stage;
  int result;
  nxinput_sdl3_pm_reason reason;
  int error_number;
  unsigned int cache_hit;
  unsigned int classification_observed;
  unsigned int classification_result;
  unsigned int open_observed;
  unsigned int open_result;
  unsigned int effective_mapping_verified;
  /* How many entries the staged SDL_GAMECONTROLLERCONFIG really carried. */
  unsigned int staged_entry_count;
  uint64_t consumer_delivery_count;
  uint32_t consumer_pressed_mask;
  uint32_t consumer_released_mask;
  nxinput_sdl3_pm_evidence evidence;
} nxinput_sdl3_pm_receipt;

/* Pure conversion API. key_bits is the EV_KEY capability bitmap for the
 * exact event node represented by target_guid. A legacy PortMaster/joydev
 * ordinal walks keys in ascending code order; the private SDL3 Linux backend
 * walks gamepad-class keys first and lower keyboard/media keys second.
 *
 * The converter is capability-driven. It does not inspect a CFW, board,
 * controller name, VID or PID. Native/already converted mappings remain a
 * closed no-op. Axes, hats, semantic destinations and optional hints are
 * preserved byte-for-byte. */
/* Heterogeneous multi-entry selection (V4-CONTROLLERS-02).
 *
 * A real SDL_GAMECONTROLLERCONFIG is a newline-separated list. PortMaster and
 * the CFWs ship one entry per known device, in several dialects, and nothing
 * guarantees that the entry a given device needs is the first one.
 *
 * The rule is deliberately conservative, because picking the wrong entry is
 * worse than picking none:
 *
 *   * exactly one entry            -> that entry is used, and its GUID is
 *                                     rewritten to the target as before;
 *   * several entries, one matches -> the entry whose GUID equals the target
 *                                     GUID is used;
 *   * several entries, none match  -> NOT_APPLICABLE. Guessing between
 *                                     heterogeneous entries is never done;
 *   * several identical entries for the target GUID -> accepted;
 *   * several DIVERGENT entries for the target GUID -> fails closed with
 *                                     EPROTO. That collision is explicit,
 *                                     not silently resolved by order.
 *
 * Blank lines and '#' comments are skipped. A line without a 32 hex-digit
 * GUID first field, or longer than the bounded storage, fails closed.
 * Returns NXINPUT_SDL3_PM_REWRITTEN when an entry was selected,
 * NXINPUT_SDL3_PM_NOT_APPLICABLE when none applies, and
 * NXINPUT_SDL3_PM_ERROR on a malformed list or collision. */
int nxinput_sdl3_pm_select_mapping(const char *config, const char *target_guid,
                                   char *output, size_t output_size,
                                   unsigned int *entry_count);

int nxinput_sdl3_pm_convert_mapping(
    const char *mapping, const unsigned long *key_bits, size_t key_bit_count,
    const char *target_guid, char *output, size_t output_size,
    nxinput_sdl3_pm_evidence *evidence);

#ifndef NXINPUT_SDL3_PM_CORE_ONLY

#include <SDL3/SDL.h>

typedef struct nxinput_sdl3_pm_context nxinput_sdl3_pm_context;

/* Runtime ownership contract: enumeration, prepare, classification, open,
 * remove and reset are serialized on the port's single SDL owner thread.
 * The internal mutex protects cache/receipt snapshots; it does not authorize
 * concurrent SDL calls. A hotplug remove must not race a wrapper for the same
 * instance ID. */

/* Mandatory pre-SDL_Init staging boundary. Call this on the SDL owner thread
 * before any SDL subsystem is initialized. If SDL_GAMECONTROLLERCONFIG is
 * present, the manager first copies it into private bounded storage and then
 * removes it from the process environment, preventing SDL_Init from loading
 * the legacy PortMaster bytes at USER priority. A copy/unset failure returns
 * NULL and leaves the caller responsible for aborting before SDL_Init.
 * Missing/empty input creates a pure passthrough manager. There is deliberately
 * no public constructor that accepts an already-loaded mapping. */
nxinput_sdl3_pm_context *nxinput_sdl3_pm_stage_before_sdl_init(void);
void nxinput_sdl3_pm_destroy(nxinput_sdl3_pm_context *context);
void nxinput_sdl3_pm_reset(nxinput_sdl3_pm_context *context);

/* Remove all state for a disconnected instance. Call for
 * SDL_EVENT_GAMEPAD_REMOVED/SDL_EVENT_JOYSTICK_REMOVED before an instance ID
 * can be reused. */
void nxinput_sdl3_pm_remove(nxinput_sdl3_pm_context *context,
                            SDL_JoystickID instance_id);

/* Prepare a capability-proved rewrite at an explicit native-flow boundary.
 * The staged original mapping is registered when no ordinal rewrite applies;
 * a converted mapping is registered when the BB1 signature matches. Neither
 * result becomes stable until SDL_GetGamepadMappingForID confirms the same
 * semantic input bindings. Path, open, fstat, node-type, ioctl, conversion,
 * registration and effective-readback failures remain retryable. A second
 * live instance with the same GUID but divergent wanted mapping fails closed
 * before registration. */
int nxinput_sdl3_pm_prepare(nxinput_sdl3_pm_context *context,
                            SDL_JoystickID instance_id,
                            nxinput_sdl3_pm_stage stage,
                            nxinput_sdl3_pm_receipt *receipt);

/* Guest-boundary wrappers. They preserve the native sequence while ensuring
 * preparation occurs during enumeration and immediately before the guest's
 * first classification/open. Preparation is best-effort, exactly like the
 * approved reference: a transient translation or registration error is
 * reported and remains retryable, but never suppresses the guest's
 * SDL_IsGamepad or SDL_OpenGamepad call. */
SDL_JoystickID *
nxinput_sdl3_pm_get_joysticks(nxinput_sdl3_pm_context *context, int *count);
bool nxinput_sdl3_pm_is_gamepad(nxinput_sdl3_pm_context *context,
                                SDL_JoystickID instance_id);
SDL_Gamepad *nxinput_sdl3_pm_open_gamepad(nxinput_sdl3_pm_context *context,
                                          SDL_JoystickID instance_id);

/* A framework counter is not proof that a game consumed input. The adapter
 * must call this function only after the real guest/engine consumer has
 * received the semantic button state. button_index is an adapter-defined
 * normalized semantic in 0..31. The returned receipt binds that delivery to
 * the mapping, classification and open observations for the same instance. */
int nxinput_sdl3_pm_record_consumer_delivery(
    nxinput_sdl3_pm_context *context, SDL_JoystickID instance_id,
    unsigned int button_index, bool pressed,
    nxinput_sdl3_pm_receipt *receipt);
int nxinput_sdl3_pm_get_receipt(nxinput_sdl3_pm_context *context,
                                SDL_JoystickID instance_id,
                                nxinput_sdl3_pm_receipt *receipt);

#endif /* NXINPUT_SDL3_PM_CORE_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_SDL3_PORTMASTER_H */
