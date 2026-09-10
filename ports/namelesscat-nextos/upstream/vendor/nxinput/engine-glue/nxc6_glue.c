/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxc6_glue -- V4-CONTROLLERS-03 / C6.
 *
 * The per-major glue that is vendored INTO an SDL checkout and compiled as
 * part of libSDL. It binds nxinput_sdl_seam to the REAL API of the SDL it is
 * linked into, and it is the only file in C6 that knows an SDL header.
 *
 * One file serves both majors: NXC6_SDL3 selects the SDL3 spelling of the
 * same four operations. Keeping it single-sourced is deliberate -- two
 * copies would be two chances for the SDL2 and SDL3 evidence to stop meaning
 * the same thing.
 *
 * Call site: src/joystick/linux/SDL_sysjoystick.c, in MaybeAddDevice(),
 * immediately before SDL_PrivateJoystickAdded(). See the patches.
 */
#include "nxc6_glue.h"

#include "nxinput_livedb.h"
#include "nxinput_portmaster.h"
#include "nxinput_sdl_seam.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * Which major we are inside is decided by the TREE we were vendored into,
 * not by a build flag someone can forget to pass.
 *
 * The discriminator is the SDL2 umbrella header: an SDL2 checkout puts
 * `SDL.h` at the root of its include path and an SDL3 checkout does not
 * (SDL3 ships `SDL3/SDL.h` only). Asking for <SDL3/SDL.h> instead would be
 * wrong and was: on a host that merely has SDL3 development headers
 * installed, that question is answered `yes` while building SDL2, and the
 * SDL2 library then fails to link against SDL3 entry points that are not
 * there. Ask about the tree, not about the host.
 *
 * NXC6_SDL3 stays honoured as an explicit override.
 */
#if !defined(NXC6_SDL3) && defined(__has_include)
#if __has_include("SDL.h")
#define NXC6_IS_SDL2 1
#endif
#endif

#ifdef NXC6_IS_SDL2
#include "SDL.h"
#define NXC6_API NXINPUT_SDL_API_2
#else
#include <SDL3/SDL.h>
#define NXC6_API NXINPUT_SDL_API_3
#define NXC6_IS_SDL3 1
#endif

#define NXC6_BITS_PER_LONG (8u * (unsigned int)sizeof(unsigned long))
#define NXC6_NBITS(x) ((((x) - 1u) / NXC6_BITS_PER_LONG) + 1u)

static nxinput_sdl_seam g_seam;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* The FACE_LAYOUT the port declared before SDL_Init. Evidence for the
 * receipts and the bundle-name selection; never a mapping decision. */
static int g_face_layout = NXC6_FACE_LAYOUT_AUTO;

typedef struct nxc6_admission_context {
  const char *receipt_path;
  unsigned long key_bits[NXC6_NBITS(NXINPUT_GODOT_KEY_BITS)];
} nxc6_admission_context;

/* ------------------------------------------------------------- receipts */
/*
 * Every decision line lands in TWO sinks (0.10.0, contract 5.9):
 *
 *   1. the private durable receipt named by NXC6_RECEIPT, opened, appended
 *      and flushed per line -- deliberately NOT a scratch handle a battery
 *      can rewrite later;
 *   2. the process stderr, so the port's normal log (and therefore the
 *      support bundle) carries the same sanitized evidence. The lines never
 *      contain a personal path, IP, hostname or raw hostile content.
 */
static void nxc6_receipt(void *userdata, const char *line) {
  const nxc6_admission_context *context =
      (const nxc6_admission_context *)userdata;
  const char *path = context != NULL ? context->receipt_path : NULL;
  FILE *stream;

  (void)fprintf(stderr, "%s\n", line);
  (void)fflush(stderr);
  if (path == NULL || *path == '\0') {
    return;
  }
  stream = fopen(path, "a");
  if (stream == NULL) {
    return;
  }
  (void)fprintf(stream, "%s\n", line);
  (void)fflush(stream);
  (void)fclose(stream);
}

static const char *nxc6_getenv(void *userdata, const char *name) {
  (void)userdata;
  return getenv(name);
}

static int nxc6_normalize_source(void *userdata, uint8_t api,
                                 const char *target_guid,
                                 const char *source, char *out, size_t cap,
                                 unsigned int *rewritten_lines,
                                 unsigned int *rewritten_bindings) {
  const nxc6_admission_context *context =
      (const nxc6_admission_context *)userdata;
  nxinput_pm_source_evidence evidence;
  nxinput_sdl_domain target = nxinput_sdl_api_domain((nxinput_sdl_api)api);
  int result;

  if (rewritten_lines != NULL) {
    *rewritten_lines = 0u;
  }
  if (rewritten_bindings != NULL) {
    *rewritten_bindings = 0u;
  }
  if (context == NULL) {
    errno = EINVAL;
    return -1;
  }
  result = nxinput_pm_normalize_source(
      source, context->key_bits, NXINPUT_GODOT_KEY_BITS, target, target_guid,
      out, cap, &evidence);
  /* The explicit domain classification of THIS source, in both sinks. */
  if (evidence.matching_lines > 0u) {
    char line[300];
    (void)snprintf(line, sizeof line,
                   "NXC6-DOMAIN guid=%.32s matching=%u rewritten=%u "
                   "rewritten_bindings=%u native=%u identical=%u "
                   "ambiguous=%u invalid=%u volume_markers=%u result=%s",
                   target_guid, evidence.matching_lines,
                   evidence.rewritten_lines, evidence.rewritten_bindings,
                   evidence.native_lines, evidence.identical_lines,
                   evidence.ambiguous_lines, evidence.invalid_lines,
                   evidence.legacy_volume_markers,
                   result == NXINPUT_PM_SOURCE_YIELDS ? "source-yields"
                   : result == NXINPUT_PM_ERROR       ? "error"
                   : result == NXINPUT_PM_REWRITTEN   ? "rewritten"
                                                      : "byte-intact");
    nxc6_receipt(userdata, line);
  }
  if (result == NXINPUT_PM_ERROR || result == NXINPUT_PM_SOURCE_YIELDS) {
    /* SOURCE_YIELDS (0.10.0): a target line was AMBIGUOUS or INVALID under
     * the semantic domain proof. The whole source yields to the next
     * authority; nothing of it may be admitted in an unproved domain. */
    return -1;
  }
  if (rewritten_lines != NULL) {
    *rewritten_lines = evidence.rewritten_lines;
  }
  if (rewritten_bindings != NULL) {
    *rewritten_bindings = evidence.rewritten_bindings;
  }
  return 0;
}

static int nxc6_read_text(void *userdata, const char *path, char *out,
                          size_t cap) {
  FILE *stream;
  size_t got;

  (void)userdata;
  if (path == NULL || *path == '\0' || out == NULL || cap == 0u) {
    return -1;
  }
  stream = fopen(path, "rb");
  if (stream == NULL) {
    return -1;
  }
  got = fread(out, 1u, cap - 1u, stream);
  if (ferror(stream) != 0 || !feof(stream)) {
    /* Bigger than the cap, or unreadable. Never truncate a database into a
     * decision: yield to the next authority instead. */
    (void)fclose(stream);
    return -1;
  }
  (void)fclose(stream);
  out[got] = '\0';
  return 0;
}

static uint64_t nxc6_now(void *userdata) {
  struct timespec ts;
  (void)userdata;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0u;
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long nxc6_pid(void *userdata) {
  (void)userdata;
  return (long)getpid();
}

static long nxc6_tid(void *userdata) {
  (void)userdata;
  return (long)syscall(SYS_gettid);
}

/* ------------------------------------------------- the real SDL setter */

static int nxc6_add_mapping(void *userdata, const char *line) {
  (void)userdata;
#ifdef NXC6_IS_SDL3
  return SDL_AddGamepadMapping(line) < 0 ? -1 : 0;
#else
  return SDL_GameControllerAddMapping(line) < 0 ? -1 : 0;
#endif
}

/* The real readback. What SDL EFFECTIVELY holds for this GUID right now --
 * which is not necessarily what the setter was handed. */
static int nxc6_mapping_for_guid(void *userdata, const char *guid, char *out,
                                 size_t cap) {
  char *held;
  size_t n;
#ifdef NXC6_IS_SDL3
  SDL_GUID id = SDL_StringToGUID(guid);
  held = SDL_GetGamepadMappingForGUID(id);
#else
  SDL_JoystickGUID id = SDL_JoystickGetGUIDFromString(guid);
  held = SDL_GameControllerMappingForGUID(id);
#endif
  (void)userdata;
  if (held == NULL) {
    return -1;
  }
  n = strlen(held);
  if (n >= cap) {
    SDL_free(held);
    return -1;
  }
  memcpy(out, held, n + 1u);
  SDL_free(held);
  return 0;
}

/* -------------------------------------------- measuring the real device */
/*
 * Counts are MEASURED from the very node SDL chose, with the same
 * enumeration the SDL Linux backend performs -- nxinput_sdl carries that
 * domain and tests/c6_domain_gate.py holds it against the pinned upstream
 * sources. Deriving the counts from the mapping's own text, or from a name
 * or VID/PID, is exactly the circularity the C3 audit rejected.
 */
static int nxc6_measure(const char *devpath, int *buttons, int *axes,
                        int *hats, unsigned long *measured_key_bits,
                        size_t measured_key_words) {
  unsigned long keybit[NXC6_NBITS(NXINPUT_GODOT_KEY_BITS)];
  unsigned long absbit[NXC6_NBITS(NXINPUT_GODOT_ABS_BITS)];
  nxinput_godot_caps caps;
  struct stat sb;
  int fd;
  int n;

  if (devpath == NULL || *devpath == '\0') {
    return -1;
  }
  memset(keybit, 0, sizeof keybit);
  memset(absbit, 0, sizeof absbit);
  fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &sb) != 0 || !S_ISCHR(sb.st_mode) ||
      ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit) < 0 ||
      ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbit), absbit) < 0) {
    (void)close(fd);
    return -1;
  }
  (void)close(fd);

  if (measured_key_bits == NULL ||
      measured_key_words < sizeof keybit / sizeof keybit[0]) {
    return -1;
  }
  memcpy(measured_key_bits, keybit, sizeof keybit);

  if (nxinput_godot_caps_init(&caps, keybit, NXINPUT_GODOT_KEY_BITS, absbit,
                              NXINPUT_GODOT_ABS_BITS) != 0) {
    return -1;
  }
  for (n = 0; nxinput_sdl_button_code(nxinput_sdl_api_domain(NXC6_API), &caps,
                                      (unsigned int)n) >= 0;
       n++) {
  }
  *buttons = n;
  for (n = 0; nxinput_sdl_axis_code(nxinput_sdl_api_domain(NXC6_API), &caps,
                                    (unsigned int)n) >= 0;
       n++) {
  }
  *axes = n;
  /* A hat exists only when BOTH halves of the pair do, which is exactly when
   * the backend removes it from the axis numbering. */
  *hats = 0;
  for (n = 0; n < (int)NXINPUT_GODOT_HAT_COUNT; n++) {
    unsigned int base = (unsigned int)NXINPUT_GODOT_ABS_HAT0X +
                        (unsigned int)n * 2u;
    unsigned int x = base / NXC6_BITS_PER_LONG;
    unsigned int y = (base + 1u) / NXC6_BITS_PER_LONG;
    if (((absbit[x] >> (base % NXC6_BITS_PER_LONG)) & 1ul) != 0ul &&
        ((absbit[y] >> ((base + 1u) % NXC6_BITS_PER_LONG)) & 1ul) != 0ul) {
      (*hats)++;
    }
  }
  return 0;
}

/* ------------------------------------------------- the live database */
/* Authority 2 when the PortMaster environment did not hand it over: the
 * bounded, snapshot-stable acquisition of the CFW's live database. The
 * declared-path branch stays with the seam (it reads the environment);
 * this callback is only ever invoked for the canonical runtime path. */
static int nxc6_livedb_acquire(void *userdata, char *out, size_t cap,
                               nxinput_livedb_receipt *receipt) {
  (void)userdata;
  return nxinput_livedb_acquire(nxinput_livedb_default_ops(), NULL,
                                (int)(sizeof(void *) * 8u), out, cap,
                                receipt);
}

/* --------------------------------------------------------- the boundary */

static void nxc6_fill_ops(nxinput_sdl_seam_ops *ops,
                          nxc6_admission_context *context) {
  memset(ops, 0, sizeof *ops);
  ops->api_version = NXINPUT_SDL_SEAM_API_VERSION;
  ops->struct_size = sizeof *ops;
  context->receipt_path = getenv("NXC6_RECEIPT");
  ops->userdata = context;
  ops->api = (uint8_t)NXC6_API;
  ops->getenv_fn = nxc6_getenv;
  ops->read_text_fn = nxc6_read_text;
  ops->normalize_source_fn = nxc6_normalize_source;
  ops->add_mapping_fn = nxc6_add_mapping;
  ops->mapping_for_guid_fn = nxc6_mapping_for_guid;
  ops->monotonic_ns = nxc6_now;
  ops->pid = nxc6_pid;
  ops->tid = nxc6_tid;
  ops->receipt_fn = nxc6_receipt;
  /* SDL always ships a mapping database of its own, so authority 4 exists. */
  ops->runtime_has_builtin = 1;
  /* Authority 5 only when the port declared its consumer understands a raw
   * pad. Declared, never inferred. */
  ops->consumer_accepts_raw =
      getenv("NXINPUT_RAW_CONSUMER_DECLARED") != NULL ? 1 : 0;
  /* The bytes the port staged out of SDL_GAMECONTROLLERCONFIG before
   * SDL_Init, so SDL could not import them at USER priority. Still
   * authority 1, still the same bytes. */
  ops->staged_mapping = getenv("NXC6_STAGED_MAPPING");
  /* 0.10.0 tail: the live-database acquisition and the declared layout. */
  ops->livedb_acquire_fn = nxc6_livedb_acquire;
  ops->face_layout = (uint8_t)g_face_layout;
}

int nxc6_admit_before_announce_named(int instance_id, const char *guid_string,
                                     const char *devpath, const char *name) {
  nxc6_admission_context context;
  nxinput_sdl_seam_ops ops;
  nxinput_sdl_seam_device device;
  nxinput_sdl_seam_result result;

  /* Not adopted: the seam is inert and SDL behaves exactly as upstream. */
  if (getenv("NXC6_SEAM") == NULL) {
    return 1;
  }
  memset(&context, 0, sizeof context);
  memset(&device, 0, sizeof device);
  device.api_version = NXINPUT_SDL_SEAM_API_VERSION;
  device.struct_size = sizeof device;
  device.instance_id = (int32_t)instance_id;
  if (guid_string != NULL) {
    (void)snprintf(device.guid, sizeof device.guid, "%s", guid_string);
  }
  if (devpath != NULL) {
    (void)snprintf(device.devpath, sizeof device.devpath, "%s", devpath);
  }
  if (name != NULL) {
    /* Evidence only. The seam sanitizes and bounds it before any receipt. */
    (void)snprintf(device.name, sizeof device.name, "%s", name);
  }
  if (nxc6_measure(devpath, &device.buttons, &device.axes, &device.hats,
                   context.key_bits,
                   sizeof context.key_bits / sizeof context.key_bits[0]) !=
      0) {
    /* A pad whose capabilities cannot be measured cannot have a mapping
     * validated against them. Fail closed: do not announce. */
    device.buttons = -1;
  }

  nxc6_fill_ops(&ops, &context);
  (void)pthread_mutex_lock(&g_lock);
  result = nxinput_sdl_seam_admit(&g_seam, &ops, &device);
  (void)pthread_mutex_unlock(&g_lock);

  return (result == NXINPUT_SDL_SEAM_ADMIT ||
          result == NXINPUT_SDL_SEAM_NO_DECLARATION)
             ? 1
             : 0;
}

int nxc6_admit_before_announce(int instance_id, const char *guid_string,
                               const char *devpath) {
  return nxc6_admit_before_announce_named(instance_id, guid_string, devpath,
                                          NULL);
}

int nxc6_declare_port_bundle_for_layout(const char *gamedir,
                                        int face_layout) {
  /* The canonical in-package names for the pinned NXCONTROLLER_PROFILES/1
   * bundles. nxgenerator pins them (controls.controller_profiles) and
   * nxrelease verifies them byte-exact inside the ZIP; this is the runtime
   * end of that same contract. A 1.2.3 field failure shipped no bundle at
   * all and the authority order lost its own step 3 -- the port declares it
   * here so the safety net exists wherever the ZIP lands, whatever launcher
   * ran it.
   *
   * FACE_LAYOUT selects only WHICH variant may serve as step 3. It never
   * outranks a live env mapping or the CFW's current database, because the
   * order above this declaration is untouched. `auto` declares only the
   * invariant base bundle, which must never freeze a user-preference
   * modern/retro line. */
  const char *bundle_name;
  char path[512];
  struct stat sb;
  int written;

  if (gamedir == NULL || gamedir[0] == '\0') {
    return -1;
  }
  switch (face_layout) {
    case NXC6_FACE_LAYOUT_AUTO:
      bundle_name = "controllers.nxb";
      break;
    case NXC6_FACE_LAYOUT_MODERN:
      bundle_name = "controllers-modern.nxb";
      break;
    case NXC6_FACE_LAYOUT_RETRO:
      bundle_name = "controllers-retro.nxb";
      break;
    default:
      return -1;
  }
  g_face_layout = face_layout;
  {
    const char *existing = getenv("NXCONTROLLER_PROFILES");
    if (existing != NULL && existing[0] != '\0') {
      return 1; /* the launcher or the owner already declared one */
    }
  }
  written = snprintf(path, sizeof path, "%s/%s", gamedir, bundle_name);
  if (written <= 0 || (size_t)written >= sizeof path) {
    return -1;
  }
  /* Regular, non-symlink, by contract. lstat: a symlinked bundle is not a
   * pinned bundle and silently leaves the ladder without step 3. */
  if (lstat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) {
    return 0; /* the port ships no bundle; the ladder simply lacks step 3 */
  }
  if (setenv("NXCONTROLLER_PROFILES", path, 0) != 0) {
    return -1;
  }
  return 1;
}

int nxc6_declare_port_bundle(const char *gamedir) {
  return nxc6_declare_port_bundle_for_layout(gamedir, NXC6_FACE_LAYOUT_AUTO);
}

void nxc6_forget(int instance_id) {
  nxc6_admission_context context;
  nxinput_sdl_seam_ops ops;

  if (getenv("NXC6_SEAM") == NULL) {
    return;
  }
  memset(&context, 0, sizeof context);
  nxc6_fill_ops(&ops, &context);
  (void)pthread_mutex_lock(&g_lock);
  nxinput_sdl_seam_forget(&g_seam, &ops, instance_id);
  (void)pthread_mutex_unlock(&g_lock);
}
