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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "nxc6_glue.h"

#include "nxinput_livedb.h"
#include "nxinput_portmaster.h"
#include "nxinput_provider.h"
#include "nxinput_provider_linux.h"
#include "nxinput_sdl_seam.h"
#include "nxinput_translate.h"
#include "nxinput_decision.h"

#include <dlfcn.h>
#include <stdint.h>

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
/* V5: the provider descriptor of the SDL this process really mapped,
 * resolved ONCE per process generation, before any admission decides. */
static nxinput_provider_descriptor g_provider;
static int g_provider_resolved;
/* 0.11.4 (N2): the staging boundary left the env line for the stock import. */
static int g_env_left_for_stock;
static uint32_t g_provider_generation;
static uint8_t g_last_source_domain; /* what the normalizer proved last */
/* 0.11.1: the probe's table API (ById function pointers) survives the
 * resolution so MEASURED_INPROCESS can run per device, post-open. */
static nxinput_provider_probe g_probe;

typedef struct nxc6_admission_context {
  const char *receipt_path;
  unsigned long key_bits[NXC6_NBITS(NXINPUT_GODOT_KEY_BITS)];
  unsigned long abs_bits[NXC6_NBITS(NXINPUT_GODOT_ABS_BITS)];
  /* 0.11.1: per-device evidence for the integral equivalence. */
  nxinput_provider_device_probe device_probe;
  uint64_t corpus_digest;
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

/* V5: resolve the provider the process REALLY maps. `entry` is the address
 * of SDL_Init as the dynamic linker resolved it for this process (never the
 * first ldconfig hit). Emits one NXC6-PROVIDER receipt per generation. */
static void nxc6_resolve_provider(void *userdata) {
  nxinput_provider_probe probe;
  const void *entry;
  char line[600];

  if (g_provider_resolved) {
    return;
  }
  /* The definition the dynamic linker bound for THIS process. A port that
   * resolves every SDL entry point through dlsym (no link-time SDL, e.g.
   * the MonoGame adapter, whose release gate forbids any SDL_* import) has
   * no static fallback: a NULL entry leaves the provider UNKNOWN (never
   * rewrite), which is the safe state. No SDL symbol is referenced here. */
  entry = dlsym(RTLD_DEFAULT, "SDL_Init");
  g_provider_generation++;
  /* 0.11.1: a runtime pin file (harness / CFW integration) extends the
   * compiled-in manifest; malformed files install nothing. */
  {
    const char *pins = getenv("NXINPUT_PROVIDER_PINS");
    if (pins != NULL && pins[0] != '\0') {
      int n = nxinput_provider_load_pin_file(pins);
      (void)snprintf(line, sizeof line, "NXC6-PROVIDER-PINS source=file installed=%d",
                     n < 0 ? 0 : n);
      nxc6_receipt(userdata, line);
    }
  }
  if (nxinput_provider_probe_sdl(entry, (uint8_t)NXC6_API, &probe) != 0) {
    memset(&probe, 0, sizeof probe);
    probe.fd = -1;
    probe.evidence.api_version = NXINPUT_PROVIDER_API_VERSION;
    probe.evidence.struct_size = sizeof probe.evidence;
    probe.evidence.api = (uint8_t)NXC6_API;
  }
  (void)nxinput_provider_resolve(&probe.evidence, g_provider_generation,
                                 &g_provider);
  /* The FD stays open across the decision; the decision is now taken. */
  nxinput_provider_probe_close(&probe);
  g_probe = probe; /* fd already closed; the ById pointers stay valid */
  g_provider_resolved = 1;
  if (nxinput_provider_receipt_line(&g_provider, line, sizeof line) > 0) {
    nxc6_receipt(userdata, line);
  }
}

/* 0.11.1: MEASURED_INPROCESS. When the provider exports the ById table API
 * and this device is an evdev node the provider has ENUMERATED (the padset
 * layout: admission runs after SDL_Init, before the port opens the pad), the
 * glue opens the joystick itself, reads the table the provider really
 * built for it, closes it again and matches the table against the
 * transcribed plans. Inside a libSDL (MaybeAddDevice layout) the device is
 * not enumerable yet, so nothing is measured and the descriptor stays
 * EXPORTED_API/UNDECLARED: stock mode, never a guess by symbol presence. */
static void nxc6_measure_provider_inprocess(void *userdata, int instance_id,
                                            const nxc6_admission_context *context) {
#ifdef NXC6_IS_SDL3
  (void)userdata; (void)instance_id; (void)context;
#else
  nxinput_provider_measurement m;
  nxinput_godot_caps caps;
  uint8_t matched = NXINPUT_SDL_DOMAIN_UNDECLARED, ambiguous = 0u;
  uint32_t plans = 0u;
  char line[600];
  int index = -1, i, n;
  SDL_Joystick *joy;
  SDL_JoystickID (*instance_for_index)(int);

  /* 0.11.4 (review 2, N1): EVERY admitted evdev device is measured. A pad
   * numbered identically by several plans decides nothing; a decided
   * provider (pin / earlier measurement) is confirmed or contradicted, never
   * replaced by "the first plan in order". Measuring once per process let the
   * first (ambiguous) pad fix a wrong domain and silence the next pad's line. */
  if (!g_provider_resolved ||
      !g_probe.evidence.has_exported_bytable ||
      context->device_probe.driver != NXINPUT_PROVIDER_DRIVER_EVDEV) {
    return;
  }
  /* SDL_JoystickGetDeviceInstanceID was born in SDL 2.0.6, above the
   * universal floor (2.0.4, nx-sdl-symbol-floor/1): it is reached through
   * the dynamic linker, never by direct import -- an SDL without it simply
   * cannot enumerate the instance from here and nothing is measured. */
  instance_for_index = (SDL_JoystickID (*)(int))(uintptr_t)dlsym(
      RTLD_DEFAULT, "SDL_JoystickGetDeviceInstanceID");
  if (instance_for_index == NULL) {
    return;
  }
  n = SDL_NumJoysticks();
  for (i = 0; i < n; i++) {
    if (instance_for_index(i) == (SDL_JoystickID)instance_id) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    return; /* not enumerable from here (in-SDL layout): nothing measured */
  }
  joy = SDL_JoystickOpen(index);
  if (joy == NULL) {
    return;
  }
  if (nxinput_provider_measure_by_id(&g_probe, instance_id, &m) == 0 &&
      nxinput_godot_caps_init(&caps, context->key_bits, NXINPUT_GODOT_KEY_BITS,
                              context->abs_bits, NXINPUT_GODOT_ABS_BITS) == 0 &&
      nxinput_provider_measurement_match(&m, &caps, (uint8_t)NXC6_API,
                                         &matched, &ambiguous) == 0 &&
      nxinput_provider_measurement_plans(&m, &caps, (uint8_t)NXC6_API, &plans) == 0) {
    (void)nxinput_provider_apply_measurement_set(&g_provider, &m, plans);
    (void)snprintf(line, sizeof line,
                   "NXC6-MEASURE instance=%d buttons=%u axes=%u hats=%u "
                   "b0=0x%x b1=0x%x a0=0x%x matched=%s ambiguous=%u plans=0x%x "
                   "decided=%s conflict=%u method=%s",
                   instance_id, m.buttons, m.axes, m.hats,
                   m.buttons > 0u ? (unsigned)m.button_code[0] : 0u,
                   m.buttons > 1u ? (unsigned)m.button_code[1] : 0u,
                   m.axes > 0u ? (unsigned)m.axis_code[0] : 0u,
                   nxinput_sdl_domain_name((nxinput_sdl_domain)matched), ambiguous,
                   (unsigned)plans,
                   nxinput_sdl_domain_name((nxinput_sdl_domain)g_provider.domain),
                   (unsigned)g_provider.measurement_conflict,
                   nxinput_provider_method_name((nxinput_provider_method)g_provider.method));
    nxc6_receipt(userdata, line);
    if (nxinput_provider_receipt_line(&g_provider, line, sizeof line) > 0) {
      nxc6_receipt(userdata, line);
    }
  }
  SDL_JoystickClose(joy);
#endif
}

int nxc6_declare_static_provider(const void *sdl_entry, const char *pin_id,
                                 int domain) {
  nxinput_provider_probe probe;
  nxinput_provider_descriptor d;
  nxc6_admission_context context;
  char line[512];
  if (g_provider_resolved || sdl_entry == NULL) {
    return -1;
  }
  memset(&context, 0, sizeof context);
  context.receipt_path = getenv("NXC6_RECEIPT");
  if (nxinput_provider_probe_sdl(sdl_entry, (uint8_t)NXC6_API, &probe) != 0) {
    return -1;
  }
  g_provider_generation++;
  if (nxinput_provider_declare_static(&probe.evidence, pin_id, (uint8_t)domain,
                                      g_provider_generation, &d) != 0) {
    nxinput_provider_probe_close(&probe);
    return -1;
  }
  nxinput_provider_probe_close(&probe);
  g_provider = d;
  g_provider_resolved = 1;
  if (nxinput_provider_receipt_line(&g_provider, line, sizeof line) > 0) {
    nxc6_receipt(&context, line);
  }
  return 0;
}

/* 0.11.1: staging that knows the provider (see nxinput_sdl_seam.h). The
 * port calls this ONCE before SDL_Init instead of staging blind. Returns
 * 0 when staged (or nothing to stage), 1 when the CFW line was LEFT for the
 * provider's stock import (UNKNOWN provider), -1 on error (too late,
 * overflow, unsetenv failure). `out` receives the staged bytes and
 * NXC6_STAGED_MAPPING is exported for the admission, exactly as the ports
 * did by hand. */
static const char *nxc6_env_get(void *u, const char *n) { (void)u; return getenv(n); }
static int nxc6_env_unset(void *u, const char *n) { (void)u; return unsetenv(n); }
/* "Too late to stage" means the subsystem that IMPORTS SDL_GAMECONTROLLERCONFIG
 * is already up -- the JOYSTICK/GAMECONTROLLER (SDL3: GAMEPAD) subsystem, NOT
 * any subsystem. Engines (Unity/Godot/MonoGame) bring SDL VIDEO up for their
 * GL context long before the port opens the pad; keying on SDL_WasInit(0) here
 * made staging refuse (rc=-1) for every such engine, and the port then found
 * no controller. The mask below is exactly what SDL reads the env at. */
static int nxc6_env_was_init(void *u) {
  (void)u;
#ifdef NXC6_IS_SDL3
  return SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD) != 0;
#else
  return SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0u;
#endif
}

int nxc6_stage_before_init(char *out, size_t cap, size_t *staged_len) {
  nxinput_sdl_seam_env_ops env;
  nxc6_admission_context context;
  int left = 0, rc;
  size_t len = 0u;
  char line[200];
  if (out == NULL || cap == 0u) {
    return -1;
  }
  memset(&context, 0, sizeof context);
  context.receipt_path = getenv("NXC6_RECEIPT");
  nxc6_resolve_provider(&context);
  memset(&env, 0, sizeof env);
  env.api_version = NXINPUT_SDL_SEAM_API_VERSION;
  env.struct_size = sizeof env;
  env.getenv_fn = nxc6_env_get;
  env.unsetenv_fn = nxc6_env_unset;
  env.sdl_was_init_fn = nxc6_env_was_init;
  rc = nxinput_sdl_seam_stage_with_provider(&env, &g_provider, out, cap, &len, &left);
  g_env_left_for_stock = rc == 0 && left ? 1 : 0;
  if (staged_len != NULL) {
    *staged_len = len;
  }
  if (rc == 0 && len > 0u && setenv("NXC6_STAGED_MAPPING", out, 1) != 0) {
    rc = -1;
  }
  (void)snprintf(line, sizeof line,
                 "NXC6-STAGE result=%s staged_bytes=%lu provider_method=%s "
                 "provider_domain=%s",
                 rc != 0 ? "error" : left ? "left-for-stock" : len > 0u ? "staged" : "absent",
                 (unsigned long)len,
                 nxinput_provider_method_name((nxinput_provider_method)g_provider.method),
                 nxinput_sdl_domain_name((nxinput_sdl_domain)g_provider.domain));
  nxc6_receipt(&context, line);
  return rc != 0 ? -1 : left ? 1 : 0;
}

int nxc6_provider_allows_rewrite(void) {
  return g_provider_resolved && nxinput_provider_allows_rewrite(&g_provider);
}

/* V5 (1.4) decision for ONE line, through the single pure machine:
 * source trust from the translate evidence, consumer table from the
 * provider descriptor, integral equivalence over the complete ordinal
 * tables of BOTH domains for the measured caps (same corpus and physical
 * identity: same process, same device). The glue never decides by label. */
static void nxc6_decide_line(const nxc6_admission_context *context,
                             const nxinput_translate_evidence *ev,
                             nxinput_decision *out) {
  nxinput_decision_input in;
  memset(&in, 0, sizeof in);
  in.source_trust = ev->source_domain != NXINPUT_SDL_DOMAIN_UNDECLARED
                        ? NXINPUT_TRUST_PROVED : NXINPUT_TRUST_UNKNOWN;
  in.consumer_kind = g_provider.evidence.api == NXINPUT_SDL_API_3
                         ? NXINPUT_CONSUMER_SDL3 : NXINPUT_CONSUMER_SDL2;
  in.consumer_table = g_provider.domain != NXINPUT_SDL_DOMAIN_UNDECLARED &&
                              g_provider.method != NXINPUT_PROVIDER_METHOD_UNKNOWN
                          ? NXINPUT_CTABLE_PROVED : NXINPUT_CTABLE_UNKNOWN;
  /* 0.11.1: every field of the identity comes from EVIDENCE, on both
   * sides (review finding 4): the source line was authored for evdev
   * ordinals (bN/aN/hN are evdev sweeps) and carries its own half-axis /
   * inversion / trigger-as-button flags; the consumer is the provider's
   * measured driver for THIS device with its own hat detection; the corpus
   * digest is the inventory of every line this admission saw; the physical
   * digest is the node's bus/vid/pid/phys/uniq/caps. */
  {
    nxinput_godot_caps caps;
    int caps_ok = nxinput_godot_caps_init(&caps, context->key_bits, NXINPUT_GODOT_KEY_BITS,
                                          context->abs_bits, NXINPUT_GODOT_ABS_BITS) == 0;
    in.source.domain = ev->source_domain;
    in.source.backend = NXINPUT_PROVIDER_DRIVER_EVDEV;
    in.source.hat_complete = caps_ok && ev->hat_bindings > 0u
        ? (uint8_t)nxinput_sdl_hat_present((nxinput_sdl_domain)ev->source_domain, &caps, 0u) : 0u;
    in.source.half_axis = ev->half_axis;
    in.source.inversion = ev->inversion;
    in.source.trigger_as_button = ev->trigger_as_button;
    in.source.table_digest = nxinput_decision_table_digest(
        (nxinput_sdl_domain)ev->source_domain, context->key_bits,
        NXINPUT_GODOT_KEY_BITS, context->abs_bits, NXINPUT_GODOT_ABS_BITS);
    in.source.corpus_digest = context->corpus_digest;
    in.source.physical_digest = context->device_probe.physical_digest;
    in.consumer.domain = g_provider.domain;
    in.consumer.backend = context->device_probe.driver;
    in.consumer.hat_complete = caps_ok && ev->hat_bindings > 0u
        ? (uint8_t)nxinput_sdl_hat_present((nxinput_sdl_domain)g_provider.domain, &caps, 0u) : 0u;
    in.consumer.half_axis = ev->half_axis;           /* every pinned provider honours +aN/-aN */
    in.consumer.inversion = ev->inversion;           /* and aN~ */
    in.consumer.trigger_as_button = ev->trigger_as_button;
    in.consumer.table_digest = nxinput_decision_table_digest(
        (nxinput_sdl_domain)g_provider.domain, context->key_bits,
        NXINPUT_GODOT_KEY_BITS, context->abs_bits, NXINPUT_GODOT_ABS_BITS);
    in.consumer.corpus_digest = context->corpus_digest;
    in.consumer.physical_digest = context->device_probe.physical_digest;
  }
  if (nxinput_decision_decide(&in, out) != 0) {
    memset(out, 0, sizeof *out);
    out->decision = NXINPUT_DECIDE_DO_NOT_MUTATE_STORE;
    out->blocked_degraded = 1u;
  }
}

/* V5 normalizer: every line of `source` that matches the GUID is translated
 * from its PROVED source domain into the PROVIDER's domain by physical
 * code (nxinput_translate), and admitted to the setter ONLY when the 1.4
 * machine says so. Source or provider UNKNOWN => DO_NOT_MUTATE_STORE: the
 * line is preserved for the receipt but the whole source YIELDS (never
 * reaches AddMapping, not even byte-identical). */
static int nxc6_normalize_source(void *userdata, uint8_t api,
                                 const char *target_guid,
                                 const char *source, char *out, size_t cap,
                                 unsigned int *rewritten_lines,
                                 unsigned int *rewritten_bindings) {
  const nxc6_admission_context *context =
      (const nxc6_admission_context *)userdata;
  const char *cursor;
  size_t used = 0u;
  unsigned int matching = 0u, rewritten = 0u, native = 0u, unproven = 0u,
               rejected = 0u, bindings = 0u;
  uint8_t source_domain = NXINPUT_SDL_DOMAIN_UNDECLARED;
  uint8_t last_decision = NXINPUT_DECIDE_DO_NOT_MUTATE_STORE;
  (void)api;

  if (rewritten_lines != NULL) {
    *rewritten_lines = 0u;
  }
  if (rewritten_bindings != NULL) {
    *rewritten_bindings = 0u;
  }
  if (context == NULL || source == NULL || out == NULL || cap == 0u) {
    errno = EINVAL;
    return -1;
  }
  nxc6_resolve_provider(userdata);
  out[0] = '\0';
  cursor = source;
  while (*cursor != '\0') {
    const char *eol = strchr(cursor, '\n');
    size_t length = eol != NULL ? (size_t)(eol - cursor) : strlen(cursor);
    size_t trimmed = length;
    char in[NXINPUT_PM_MAPPING_MAX];
    char tr[NXINPUT_PM_MAPPING_MAX];
    const char *emit_ptr = cursor;
    size_t emit_len = length;

    while (trimmed > 0u && (cursor[trimmed - 1u] == '\r' ||
                            cursor[trimmed - 1u] == ' ' ||
                            cursor[trimmed - 1u] == '\t')) {
      --trimmed;
    }
    if (trimmed > 33u && trimmed >= sizeof in &&
        strncmp(cursor, target_guid, 32u) == 0 && cursor[32] == ',') {
      /* 0.11.1 (review finding 10): a line of the target GUID too long to
       * translate never bypasses the machine byte-intact: the source
       * yields, with a receipt. */
      matching++;
      rejected++;
    } else if (trimmed > 33u && trimmed < sizeof in &&
        strncmp(cursor, target_guid, 32u) == 0 && cursor[32] == ',') {
      nxinput_translate_evidence ev;
      nxinput_translate_result r;
      memcpy(in, cursor, trimmed);
      in[trimmed] = '\0';
      matching++;
      ((nxc6_admission_context *)context)->corpus_digest = nxinput_decision_fnv1a64(
          in, trimmed, ((nxc6_admission_context *)context)->corpus_digest);
      r = nxinput_translate_line(in, context->key_bits, NXINPUT_GODOT_KEY_BITS,
                                 context->abs_bits, NXINPUT_GODOT_ABS_BITS,
                                 (nxinput_sdl_domain)g_provider.domain, NULL,
                                 tr, sizeof tr, &ev);
      bindings += ev.button_bindings + ev.axis_bindings + ev.hat_bindings;
      if (ev.source_domain != NXINPUT_SDL_DOMAIN_UNDECLARED) {
        source_domain = ev.source_domain;
      }
      if (r == NXINPUT_TRANSLATE_REWRITTEN ||
          r == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE ||
          r == NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN) {
        nxinput_decision d;
        nxc6_decide_line(context, &ev, &d);
        last_decision = d.decision;
        if (!d.ordinal_setter_allowed) {
          /* 1.4: any side UNKNOWN -> DO_NOT_MUTATE_STORE. */
          r = NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN;
        } else if (r == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE &&
                   !d.byte_intact_claim) {
          /* label equal, integral equivalence failed: never BYTE_INTACT.
           * The physical translation already produced the typed line. */
          r = NXINPUT_TRANSLATE_REWRITTEN;
        }
      }
      switch (r) {
        case NXINPUT_TRANSLATE_REWRITTEN:
          rewritten++;
          if (rewritten_bindings != NULL) {
            *rewritten_bindings += ev.rewritten_bindings;
          }
          emit_ptr = tr;
          emit_len = strlen(tr);
          break;
        case NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE: native++; break;
        case NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN: unproven++; break;
        default:
          /* REJECTED / ERROR: no coherent physical reading. The whole
           * source yields to the next authority (as 0.10.0 did). */
          rejected++;
          break;
      }
    }
    if (rejected == 0u) {
      if (used + emit_len + 2u >= cap) {
        errno = ENOSPC;
        return -1;
      }
      memcpy(out + used, emit_ptr, emit_len);
      used += emit_len;
      if (emit_ptr == tr && trimmed < length) {
        memcpy(out + used, cursor + trimmed, length - trimmed);
        used += length - trimmed;
      }
      if (eol != NULL) {
        out[used++] = '\n';
      }
      out[used] = '\0';
    }
    cursor = eol != NULL ? eol + 1 : cursor + length;
  }
  g_last_source_domain = source_domain;
  if (matching > 0u) {
    char line[360];
    (void)snprintf(line, sizeof line,
                   "NXC6-DOMAIN guid=%.32s matching=%u rewritten=%u "
                   "rewritten_bindings=%u native=%u unproven=%u rejected=%u "
                   "bindings=%u source_domain=%s provider_domain=%s "
                   "provider_method=%s decision=%s result=%s",
                   target_guid, matching, rewritten,
                   rewritten_bindings != NULL ? *rewritten_bindings : 0u,
                   native, unproven, rejected, bindings,
                   nxinput_sdl_domain_name((nxinput_sdl_domain)source_domain),
                   nxinput_sdl_domain_name((nxinput_sdl_domain)g_provider.domain),
                   nxinput_provider_method_name(
                       (nxinput_provider_method)g_provider.method),
                   nxinput_decision_name((nxinput_mapping_decision)last_decision),
                   rejected > 0u    ? "source-yields"
                   : unproven > 0u  ? "source-unproven-yields"
                   : rewritten > 0u ? ((rewritten_bindings != NULL && *rewritten_bindings == 0u)
                                          ? "typed-identical" : "rewritten")
                                    : "byte-intact");
    nxc6_receipt(userdata, line);
  }
  if (rejected > 0u || unproven > 0u) {
    /* 1.4: an unproven line never enters the setter, not even unchanged.
     * The source yields; the provider keeps only what already belongs to it. */
    return -1;
  }
  if (rewritten_lines != NULL) {
    *rewritten_lines = rewritten;
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
                        size_t measured_key_words,
                        unsigned long *measured_abs_bits,
                        size_t measured_abs_words,
                        nxinput_provider_device_probe *device_probe) {
  nxinput_godot_caps caps;
  int n;

  if (measured_key_bits == NULL ||
      measured_key_words < NXC6_NBITS(NXINPUT_GODOT_KEY_BITS) ||
      measured_abs_bits == NULL ||
      measured_abs_words < NXC6_NBITS(NXINPUT_GODOT_ABS_BITS) ||
      device_probe == NULL) {
    return -1;
  }
  /* 0.11.1: the DEVICE probe classifies the driver (evdev / hidapi /
   * virtual / unknown) and measures the bitmaps of an evdev node. A HIDAPI
   * device has no evdev caps: the caller admits it in stock mode. */
  (void)nxinput_provider_probe_device(devpath, measured_key_bits,
                                      measured_key_words, measured_abs_bits,
                                      measured_abs_words, device_probe);
  if (device_probe->driver != NXINPUT_PROVIDER_DRIVER_EVDEV ||
      !device_probe->caps_measured) {
    return -1;
  }
  if (nxinput_godot_caps_init(&caps, measured_key_bits, NXINPUT_GODOT_KEY_BITS,
                              measured_abs_bits, NXINPUT_GODOT_ABS_BITS) != 0) {
    return -1;
  }
  /* V5: counts follow the PROVIDER's enumeration, never the major's
   * upstream presumption. With an UNKNOWN provider the counts are not
   * claimable: the pad is admitted in stock mode and the counts are only
   * reachability evidence (no ORDER is claimed, nothing is rewritten). */
  if (nxinput_sdl_domain_plan((nxinput_sdl_domain)g_provider.domain) == NULL) {
    unsigned int i, count = 0u;
    for (i = 0u; i < NXINPUT_GODOT_KEY_BITS; i++) {
      if (((measured_key_bits[i / NXC6_BITS_PER_LONG] >> (i % NXC6_BITS_PER_LONG)) & 1ul) != 0ul) {
        count++;
      }
    }
    *buttons = (int)count;
    for (n = 0; nxinput_sdl_axis_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps,
                                      (unsigned int)n) >= 0;
         n++) {
    }
    *axes = n;
  } else {
    for (n = 0; nxinput_sdl_button_code((nxinput_sdl_domain)g_provider.domain,
                                        &caps, (unsigned int)n) >= 0;
         n++) {
    }
    *buttons = n;
    for (n = 0; nxinput_sdl_axis_code((nxinput_sdl_domain)g_provider.domain,
                                      &caps, (unsigned int)n) >= 0;
         n++) {
    }
    *axes = n;
  }
  /* Hats: DETECTED per the provider's rule (0.11.1: p009 makes a lone half
   * a hat on the ascending provider; upstream needs the pair). With an
   * unknown provider the pair rule counts. */
  *hats = 0;
  for (n = 0; n < (int)NXINPUT_GODOT_HAT_COUNT; n++) {
    if (nxinput_sdl_hat_present(
            nxinput_sdl_domain_plan((nxinput_sdl_domain)g_provider.domain) != NULL
                ? (nxinput_sdl_domain)g_provider.domain : NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
            &caps, (unsigned int)n)) {
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
  /* V5 tail: the provider descriptor and the proved source domain slot. */
  ops->provider_domain = g_provider.domain;
  ops->provider_method = g_provider.method;
  ops->env_left_for_stock = (uint8_t)g_env_left_for_stock;
  ops->source_domain_slot = &g_last_source_domain;
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
  /* 0.11.1 (review finding 3): the durable receipt path is known BEFORE
   * the provider is resolved, so NXC6-PROVIDER lands in NXC6_RECEIPT. */
  context.receipt_path = getenv("NXC6_RECEIPT");
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
  nxc6_resolve_provider(&context);
  if (nxc6_measure(devpath, &device.buttons, &device.axes, &device.hats,
                   context.key_bits,
                   sizeof context.key_bits / sizeof context.key_bits[0],
                   context.abs_bits,
                   sizeof context.abs_bits / sizeof context.abs_bits[0],
                   &context.device_probe) != 0) {
    /* No evdev capabilities: a HIDAPI/virtual/unknown device. The seam
     * decides (HIDAPI => stock mode, admitted; unknown => not usable). */
    device.buttons = -1;
  }
  device.driver = context.device_probe.driver;
  /* 0.11.1: measure the provider's real table for this device (ById). */
  nxc6_measure_provider_inprocess(&context, instance_id, &context);
  {
    char line[200];
    (void)snprintf(line, sizeof line,
                   "NXC6-DEVICE instance=%d driver=%s caps_measured=%u bus=0x%x "
                   "physical=%016llx",
                   instance_id,
                   nxinput_provider_driver_name((nxinput_provider_driver)context.device_probe.driver),
                   (unsigned)context.device_probe.caps_measured,
                   (unsigned)context.device_probe.bus,
                   (unsigned long long)context.device_probe.physical_digest);
    nxc6_receipt(&context, line);
  }

  nxc6_fill_ops(&ops, &context);
  (void)pthread_mutex_lock(&g_lock);
  result = nxinput_sdl_seam_admit(&g_seam, &ops, &device);
  (void)pthread_mutex_unlock(&g_lock);

  return (result == NXINPUT_SDL_SEAM_ADMIT ||
          result == NXINPUT_SDL_SEAM_ADMIT_STOCK ||
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
