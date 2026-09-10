/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * V4-CONTROLLERS-03 / C6 -- the seam's fail-closed ladder, hermetically.
 *
 * CLASS: FIXTURE_HOST. This is the SAME function the three real SDL
 * libraries have linked in, driven here with a scripted runtime so every
 * refusal branch can be reached on purpose -- including the ones a healthy
 * pad never takes. It proves the ladder; it proves nothing about SDL. What
 * SDL actually answered is the matrix gate's business.
 */
#include "nxinput_sdl_seam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(int ok, const char *label) {
  checks++;
  if (ok) {
    printf("ok   %s\n", label);
  } else {
    failures++;
    printf("FAIL %s\n", label);
  }
}

/* ------------------------------------------------------- scripted runtime */
struct fake {
  const char *env_mapping;
  const char *env_database;
  const char *env_bundle;
  const char *file_body;
  char held[NXINPUT_SOVEREIGN_LINE_MAX];  /* what "SDL" now holds */
  int refuse_setter;
  int hostile_readback;   /* accept the setter, report something else */
  int receipts;
  char last[900];
};

static const char *f_getenv(void *u, const char *name) {
  struct fake *f = (struct fake *)u;
  if (strcmp(name, NXINPUT_AUTHORITY_ENV_MAPPING) == 0) return f->env_mapping;
  if (strcmp(name, NXINPUT_AUTHORITY_ENV_DATABASE) == 0) return f->env_database;
  if (strcmp(name, NXINPUT_AUTHORITY_ENV_BUNDLE) == 0) return f->env_bundle;
  return 0;
}

static int f_read_text(void *u, const char *path, char *out, size_t cap) {
  struct fake *f = (struct fake *)u;
  (void)path;
  if (f->file_body == 0) return -1;
  if (strlen(f->file_body) >= cap) return -1;
  strcpy(out, f->file_body);
  return 0;
}

static int f_add(void *u, const char *line) {
  struct fake *f = (struct fake *)u;
  if (f->refuse_setter) return -1;
  (void)snprintf(f->held, sizeof f->held, "%s", line);
  return 0;
}

static int f_for_guid(void *u, const char *guid, char *out, size_t cap) {
  struct fake *f = (struct fake *)u;
  (void)guid;
  if (f->held[0] == '\0') return -1;
  if (f->hostile_readback) {
    /* The setter said yes and the runtime kept something else. This is the
     * real SDL failure mode: a USER-priority mapping imported from the
     * environment outranks the API one, and the setter still returns
     * success. A decision that trusted the setter would ship the wrong pad. */
    (void)snprintf(out, cap, "%.32s,Hostile,a:b9,platform:Linux,", f->held);
    return 0;
  }
  (void)snprintf(out, cap, "%s", f->held);
  return 0;
}

static uint64_t f_now(void *u) { (void)u; return 1u; }
static long f_pid(void *u) { (void)u; return 4242; }
static long f_tid(void *u) { (void)u; return 4243; }
static void f_receipt(void *u, const char *line) {
  struct fake *f = (struct fake *)u;
  f->receipts++;
  (void)snprintf(f->last, sizeof f->last, "%s", line);
}

static int f_normalize_fail(void *u, uint8_t api, const char *guid,
                            const char *source, char *out, size_t cap,
                            unsigned int *lines,
                            unsigned int *bindings) {
  (void)u; (void)api; (void)guid; (void)source; (void)out; (void)cap;
  (void)lines; (void)bindings;
  return -1;
}

/* ------------------------------------------ 0.10.0 fake live database */
static int g_livedb_calls;

static int fake_livedb_ok(void *u, char *out, size_t cap,
                          nxinput_livedb_receipt *receipt) {
  (void)u;
  g_livedb_calls++;
  receipt->api_version = NXINPUT_LIVEDB_API_VERSION;
  receipt->struct_size = sizeof *receipt;
  receipt->path_class = (uint8_t)NXINPUT_LIVEDB_PATH_CANONICAL;
  (void)snprintf(receipt->target, sizeof receipt->target, "retro");
  receipt->attempts = 3u;
  receipt->elapsed_ns = 50000000ull;
  receipt->acquired = 1;
  (void)snprintf(out, cap, "%s\n",
                 "0300439e12090000a1c5000010010000,Pad A,"
                 "a:b0,b:b1,start:b9,platform:Linux,");
  return 0;
}

static int fake_livedb_yield(void *u, char *out, size_t cap,
                             nxinput_livedb_receipt *receipt) {
  (void)u;
  g_livedb_calls++;
  receipt->api_version = NXINPUT_LIVEDB_API_VERSION;
  receipt->struct_size = sizeof *receipt;
  receipt->path_class = (uint8_t)NXINPUT_LIVEDB_PATH_CANONICAL;
  (void)snprintf(receipt->target, sizeof receipt->target, "other");
  receipt->attempts = 20u;
  receipt->elapsed_ns = 475000000ull;
  receipt->acquired = 0;
  if (cap > 0u) {
    out[0] = '\0';
  }
  return -1;
}

#define GUID_A "0300439e12090000a1c5000010010000"
#define GUID_B "030068ec13090000a2c5000010010000"
#define GUID_GO_LIVE "1900bb3e4b4800000011000000010000"
#define GUID_GO_ZERO "190000004b4800000011000000010000"
#define GUID_NONBUS_LIVE "4100bb3e4b4800000011000000010000"
#define GUID_NONBUS_ZERO "410000004b4800000011000000010000"
#define LINE_A GUID_A ",Pad A,a:b0,b:b1,start:b9,platform:Linux,"
#define LINE_A2 GUID_A ",Pad A,a:b1,b:b0,start:b9,platform:Linux,"
#define LINE_GO_ZERO \
  GUID_GO_ZERO ",GO-Super Gamepad,a:b1,b:b0,start:b13,back:b12,platform:Linux,"
#define LINE_GO_LIVE \
  GUID_GO_LIVE ",GO-Super Gamepad,a:b1,b:b0,start:b13,back:b12,platform:Linux,"
/* The real dArkOS line for the GO-Super, as /opt/system/Tools/PortMaster/
 * gamecontrollerdb.txt carries it: zero CRC word, L3/R3 on b14/b15. */
#define LINE_GO_DARKOS \
  GUID_GO_ZERO ",GO-Super Gamepad,a:b1,b:b0,x:b3,y:b2,leftshoulder:b4," \
  "rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b12,start:b13," \
  "leftstick:b14,rightstick:b15,leftx:a0,lefty:a1,rightx:a2,righty:a3," \
  "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,"

static void ops_init(nxinput_sdl_seam_ops *ops, struct fake *f) {
  memset(ops, 0, sizeof *ops);
  ops->api_version = NXINPUT_SDL_SEAM_API_VERSION;
  ops->struct_size = sizeof *ops;
  ops->userdata = f;
  ops->api = (uint8_t)NXINPUT_SDL_API_2;
  ops->getenv_fn = f_getenv;
  ops->read_text_fn = f_read_text;
  ops->add_mapping_fn = f_add;
  ops->mapping_for_guid_fn = f_for_guid;
  ops->monotonic_ns = f_now;
  ops->pid = f_pid;
  ops->tid = f_tid;
  ops->receipt_fn = f_receipt;
  ops->runtime_has_builtin = 0;
  /* V5 (0.11.0): a full-size ops table carries the provider descriptor. These
   * ladder fixtures model the upstream provider explicitly; with the tail
   * left UNDECLARED (= UNKNOWN provider) no external ordinal line may reach
   * the setter at all -- that rule has its own test (tests/v5/test_v5_seam.c). */
  ops->provider_domain = (uint8_t)NXINPUT_SDL_DOMAIN_SDL2_EVDEV;
}

static void dev_init(nxinput_sdl_seam_device *d, int32_t id, const char *guid) {
  memset(d, 0, sizeof *d);
  d->api_version = NXINPUT_SDL_SEAM_API_VERSION;
  d->struct_size = sizeof *d;
  d->instance_id = id;
  (void)snprintf(d->guid, sizeof d->guid, "%s", guid);
  (void)snprintf(d->devpath, sizeof d->devpath, "/dev/input/event%d", (int)id);
  d->buttons = 13;
  d->axes = 6;
  d->hats = 1;
}

/* ------------------------------------------------------- env staging shims */
static const char *g_env_value;
static int g_unset_fails;
static int g_already_init;
static int g_unset_called;

static const char *e_get(void *u, const char *name) {
  (void)u; (void)name;
  return g_env_value;
}
static int e_unset(void *u, const char *name) {
  (void)u; (void)name;
  g_unset_called++;
  if (g_unset_fails) return -1;
  g_env_value = 0;
  return 0;
}
static int e_init(void *u) { (void)u; return g_already_init; }

int main(void) {
  nxinput_sdl_seam seam;
  nxinput_sdl_seam_ops ops;
  nxinput_sdl_seam_device dev;
  struct fake f;
  nxinput_sdl_seam_result r;

  /* 1. A partial ops table is refused rather than worked around. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.mapping_for_guid_fn = 0;   /* no readback == no decision */
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_OPS,
        "an ops table without a readback cannot admit anything");

  /* 2. A device record that identifies nothing. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, "00000000000000000000000000000000");
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_IDENTITY,
        "an all-zero GUID identifies nothing and never selects a mapping");
  dev_init(&dev, 1, "nothex");
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_IDENTITY,
        "a malformed GUID is refused, not normalised");
  dev_init(&dev, 1, GUID_A);
  dev.buttons = 0;
  dev.axes = 0;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_IDENTITY,
        "a pad with no measurable buttons and no axes is not a pad");

  /* 3. Nothing declared: SDL keeps its own behaviour. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_NO_DECLARATION,
        "an unadopted port is neither admitted nor blocked");

  /* 4. The happy path, and the receipt that must stand behind it. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, GUID_A);
  r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
  check(r == NXINPUT_SDL_SEAM_ADMIT, "a complete declaration is admitted");
  check(strstr(f.last, "result=admit") != 0 &&
            strstr(f.last, "source=env-get-controls") != 0,
        "the receipt names the authority that won");
  check(strstr(f.last, "pid=4242") != 0 && strstr(f.last, "tid=4243") != 0,
        "the receipt carries the process and thread that decided");
  check(nxinput_sdl_seam_find(&seam, 1) != 0,
        "the admitted instance is recorded");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.struct_size = NXINPUT_SDL_SEAM_OPS_SIZE_0_8_1;
  ops.normalize_source_fn = f_normalize_fail;
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_ADMIT,
        "the 0.8.1 ops layout remains valid and never reads the additive "
        "normalizer field");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.normalize_source_fn = f_normalize_fail;
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY && f.held[0] == '\0',
        "a failed domain adapter blocks the authority instead of silently "
        "passing its unnormalised mapping");

  {
    size_t oversize_size = (size_t)NXINPUT_AUTHORITY_SOURCE_MAX + 1u;
    char *oversize = (char *)malloc(oversize_size);
    check(oversize != 0, "the oversize-source fixture can be allocated");
    if (oversize != 0) {
      memset(oversize, 'x', oversize_size);
      memcpy(oversize, GUID_A, 32u);
      oversize[32] = ',';
      oversize[oversize_size - 1u] = '\0';
      memset(&seam, 0, sizeof seam);
      memset(&f, 0, sizeof f);
      ops_init(&ops, &f);
      f.env_mapping = oversize;
      dev_init(&dev, 1, GUID_A);
      check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
                NXINPUT_SDL_SEAM_BLOCK_AUTHORITY && f.held[0] == '\0',
            "an oversized authority cannot fall through as raw staged "
            "mapping after the normalization buffer is refused");
      free(oversize);
    }
  }

  /* 5. The setter refuses. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  f.refuse_setter = 1;
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY,
        "a setter that refuses blocks the pad before the announce");

  /* 6. The setter accepts and the runtime keeps something else. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  f.hostile_readback = 1;
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY,
        "a successful setter with a divergent readback is NOT a decision");

  /* 7. Two live instances of one GUID. SDL's store is keyed by GUID, so
   * identical is fine and divergent must fail closed. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT,
        "the first instance of a shared GUID is admitted");
  dev_init(&dev, 2, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT,
        "a second instance wanting the SAME mapping is admitted too");
  check(seam.admitted == 2u, "both instances are live");
  /* The C3 authority must ACCUMULATE across admissions. An earlier version
   * of this file re-initialised it on every call, which silently erased the
   * per-instance table -- so a second pad, a reconnection and a hotplug were
   * all indistinguishable from a first admission, and `resolutions` never
   * left 1. This check is here so that cannot come back. */
  check(seam.authority.resolutions == 2u,
        "the C3 authority ran twice and kept BOTH entries, rather than being "
        "rebuilt and losing the first");
  check(nxinput_authority_find(&seam.authority, 1) != 0 &&
            nxinput_authority_find(&seam.authority, 2) != 0,
        "both instances are live in the AUTHORITY's own table too");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, GUID_A);
  (void)nxinput_sdl_seam_admit(&seam, &ops, &dev);
  f.env_mapping = LINE_A2;      /* same GUID, divergent body */
  dev_init(&dev, 2, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_COLLISION,
        "a second instance wanting a DIVERGENT mapping for the same GUID "
        "fails closed instead of silently redefining the first");
  check(nxinput_sdl_seam_find(&seam, 1) != 0,
        "the collision does not disturb the instance that was already there");

  /* 8. Hotplug invalidates only the instance named. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_A;
  dev_init(&dev, 1, GUID_A);
  (void)nxinput_sdl_seam_admit(&seam, &ops, &dev);
  dev_init(&dev, 2, GUID_A);
  (void)nxinput_sdl_seam_admit(&seam, &ops, &dev);
  nxinput_sdl_seam_forget(&seam, &ops, 1);
  check(nxinput_sdl_seam_find(&seam, 1) == 0,
        "the disconnected instance is gone");
  check(nxinput_sdl_seam_find(&seam, 2) != 0,
        "the OTHER pad keeps its decision through the hotplug");
  check(seam.forgotten == 1u, "exactly one instance was forgotten");

  /* 9. Raw passthrough: declared or nothing. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = GUID_B ",Other,a:b0,platform:Linux,";  /* not this pad */
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY,
        "with no entry for this pad and no raw declaration, the order ends "
        "in an explicit failure before gameplay");
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.consumer_accepts_raw = 1;
  f.env_mapping = GUID_B ",Other,a:b0,platform:Linux,";
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT,
        "the SAME inputs are admitted once the consumer DECLARES it "
        "understands a raw pad");
  check(strstr(f.last, "source=raw-passthrough") != 0 &&
            strstr(f.last, "readback_checked=0") != 0,
        "raw passthrough reports honestly that it has no readback");

  /* 10. The staged mapping is authority 1, not a fourth source. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.staged_mapping = LINE_A;   /* env is EMPTY; the bytes were staged */
  dev_init(&dev, 1, GUID_A);
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT,
        "a staged mapping still wins as authority 1");
  check(strstr(f.last, "source=env-get-controls") != 0,
        "and it is reported as authority 1, not as a new rank");

  /* 11. The live GUID carries a name CRC16 that PortMaster's SDL2-format
   * database omits. This is SDL's own mapping identity rule on both executed
   * majors (SDL3; SDL2 since 2.26 -- both pinned SDL2 trees write the CRC and
   * fall back to the zero-CRC entry): the zero word may be filled only when
   * every other GUID byte is identical. Unrelated identities remain blocked.
   *
   * 0.7.2 directed regression, the dArkOS GO-Super physical case: the SDL2
   * route must admit the official zero-CRC line for the live CRC GUID with
   * every binding intact -- rightstick (R3) and leftstick (L3) exist, back/
   * start are untouched, the owner's a/b swap survives, nothing is rebound
   * and nothing synthetic is added. */
  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_GO_DARKOS;
  dev_init(&dev, 1, GUID_GO_LIVE);
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT,
        "SDL2 admits the official zero-CRC GO-Super line for the live CRC GUID");
  check(strncmp(f.held, GUID_GO_LIVE, 32u) == 0 &&
            strstr(f.last, "source_crc_aliases=1") != 0,
        "SDL2: only the CRC word is rebound to the live GUID and receipted");
  check(strcmp(&f.held[32], &LINE_GO_DARKOS[32]) == 0,
        "SDL2: every byte after the GUID is the sovereign line, byte-intact");
  check(strstr(f.held, ",rightstick:b15,") != 0 &&
            strstr(f.held, ",leftstick:b14,") != 0,
        "SDL2: R3 (rightstick:b15) and L3 (leftstick:b14) exist after admission");
  check(strstr(f.held, ",back:b12,") != 0 && strstr(f.held, ",start:b13,") != 0,
        "SDL2: SELECT/START keep b12/b13");
  check(strstr(f.held, ",a:b1,b:b0,") != 0,
        "SDL2: the owner's a/b assignment is not 'corrected'");
  check(strstr(f.held, "key") == 0 && strstr(f.last, "key") == 0 &&
            f.receipts == 1,
        "SDL2: no synthetic key, no extra binding, one admission receipt");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping =
      GUID_GO_ZERO ",Wrong product,a:b1,b:b0,start:b13,back:b12,platform:Linux,";
  dev_init(&dev, 1, "1900bb3e4b4800003412000000010000");
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY,
        "SDL2: a non-CRC GUID byte difference is never treated as an alias");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  f.env_mapping = LINE_GO_LIVE "\n"
      GUID_GO_ZERO ",GO-Super Gamepad,a:b0,b:b1,start:b13,back:b12,platform:Linux,";
  dev_init(&dev, 1, GUID_GO_LIVE);
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT &&
            strstr(f.held, ",a:b1,b:b0,") != 0 &&
            strstr(f.last, "source_crc_aliases=0") != 0,
        "SDL2: with an exact entry present the alias is unreachable in SDL "
        "and the EXACT entry wins, unprojected");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.api = (uint8_t)NXINPUT_SDL_API_3;
  f.env_mapping = LINE_GO_ZERO;
  dev_init(&dev, 1, GUID_GO_LIVE);
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT,
        "SDL3 admits the otherwise identical PortMaster zero-CRC identity");
  check(strncmp(f.held, GUID_GO_LIVE, 32u) == 0 &&
            strstr(f.last, "source_crc_aliases=1") != 0,
        "only the CRC word is rebound to the live GUID and receipted");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.api = (uint8_t)NXINPUT_SDL_API_3;
  f.env_database = "/official/gamecontrollerdb.txt";
  f.file_body = LINE_GO_ZERO;
  dev_init(&dev, 1, GUID_GO_LIVE);
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT &&
            strstr(f.last, "source=cfw-db-guid") != 0,
        "the same bounded projection reaches official CFW database authority");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.api = (uint8_t)NXINPUT_SDL_API_3;
  f.env_mapping =
      GUID_GO_ZERO ",Wrong product,a:b1,b:b0,start:b13,back:b12,platform:Linux,";
  dev_init(&dev, 1, "1900bb3e4b4800003412000000010000");
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY,
        "a non-CRC GUID byte difference is never treated as an alias");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.api = (uint8_t)NXINPUT_SDL_API_3;
  f.env_mapping = GUID_NONBUS_ZERO
      ",Non-bus identity,a:b1,b:b0,start:b13,back:b12,platform:Linux,";
  dev_init(&dev, 1, GUID_NONBUS_LIVE);
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) ==
            NXINPUT_SDL_SEAM_BLOCK_AUTHORITY,
        "a non-bus GUID never gains the SDL3 CRC projection");

  memset(&seam, 0, sizeof seam);
  memset(&f, 0, sizeof f);
  ops_init(&ops, &f);
  ops.api = (uint8_t)NXINPUT_SDL_API_3;
  f.env_mapping = LINE_GO_LIVE "\n"
      GUID_GO_ZERO ",GO-Super Gamepad,a:b0,b:b1,start:b13,back:b12,platform:Linux,";
  dev_init(&dev, 1, GUID_GO_LIVE);
  dev.buttons = 17;
  check(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT &&
            strstr(f.held, ",a:b1,b:b0,") != 0 &&
            strstr(f.last, "source_crc_aliases=0") != 0,
        "with an exact entry present the alias is unreachable in SDL and the "
        "EXACT entry wins, unprojected");

  /* ------------------------------------------------------- pre-init staging */
  {
    nxinput_sdl_seam_env_ops env;
    char buf[256];
    size_t len;

    memset(&env, 0, sizeof env);
    env.api_version = NXINPUT_SDL_SEAM_API_VERSION;
    env.struct_size = sizeof env;
    env.getenv_fn = e_get;
    env.unsetenv_fn = e_unset;
    env.sdl_was_init_fn = e_init;

    g_env_value = LINE_A;
    g_already_init = 0;
    g_unset_fails = 0;
    g_unset_called = 0;
    check(nxinput_sdl_seam_stage_before_init(&env, buf, sizeof buf, &len) == 0
              && len == strlen(LINE_A) && strcmp(buf, LINE_A) == 0,
          "staging copies the mapping out before SDL can import it");
    check(g_unset_called == 1 && g_env_value == 0,
          "and REMOVES it from the environment, so SDL_Init cannot register "
          "it at USER priority above the decision");

    g_env_value = LINE_A;
    g_already_init = 1;
    check(nxinput_sdl_seam_stage_before_init(&env, buf, sizeof buf, &len) != 0,
          "staging after SDL is up fails instead of pretending: once a "
          "subsystem exists we cannot prove the variable was not imported");

    g_env_value = 0;
    g_already_init = 0;
    check(nxinput_sdl_seam_stage_before_init(&env, buf, sizeof buf, &len) == 0
              && len == 0u,
          "an absent variable is a legitimate pass-through, not an error");

    g_env_value = LINE_A;
    check(nxinput_sdl_seam_stage_before_init(&env, buf, 8u, &len) != 0,
          "a mapping too long for the buffer fails rather than being "
          "truncated into a decision");

    g_env_value = LINE_A;
    g_unset_fails = 1;
    check(nxinput_sdl_seam_stage_before_init(&env, buf, sizeof buf, &len) != 0,
          "if the removal fails, staging fails: a copy that leaves the "
          "original in place has changed nothing");
  }

  /* --------------------- 0.10.0: the live-database acquisition ---------- */
  {
    nxinput_sdl_seam *lseam = calloc(1, sizeof *lseam);
    nxinput_sdl_seam_ops lops;
    nxinput_sdl_seam_device ldev;
    struct fake lf;

    memset(&lf, 0, sizeof lf);
    ops_init(&lops, &lf);
    /* A port that declares only a (missing) bundle, on a CFW whose live
     * database arrives late: the injected acquisition hands the snapshot to
     * the order as authority 2 -- nothing more, nothing less. */
    lf.env_bundle = "/nonexistent/controllers.nxb";
    lf.file_body = 0;
    lops.livedb_acquire_fn = fake_livedb_ok;
    lops.face_layout = 2u; /* retro, receipt evidence */
    g_livedb_calls = 0;
    dev_init(&ldev, 60, GUID_A);
    check(nxinput_sdl_seam_admit(lseam, &lops, &ldev) ==
              NXINPUT_SDL_SEAM_ADMIT,
          "an acquired live database admits the pad");
    check(g_livedb_calls == 1,
          "the acquisition ran exactly once for the admission");
    check(strstr(lf.last, "source=cfw-db-guid") != 0,
          "the snapshot entered the order as authority 2");
    check(strstr(lf.last, "db_class=canonical") != 0 &&
              strstr(lf.last, "db_target=retro") != 0 &&
              strstr(lf.last, "db_retries=3") != 0,
          "the receipt carries the sanitized acquisition evidence");
    check(strstr(lf.last, "face_layout=retro") != 0,
          "the receipt names the selected FACE_LAYOUT");

    /* With authority 1 present the acquisition is not even attempted. */
    memset(lseam, 0, sizeof *lseam);
    memset(&lf, 0, sizeof lf);
    ops_init(&lops, &lf);
    lf.env_mapping = LINE_A;
    lops.livedb_acquire_fn = fake_livedb_ok;
    g_livedb_calls = 0;
    dev_init(&ldev, 61, GUID_A);
    check(nxinput_sdl_seam_admit(lseam, &lops, &ldev) ==
              NXINPUT_SDL_SEAM_ADMIT &&
              g_livedb_calls == 0 &&
              strstr(lf.last, "source=env-get-controls") != 0,
          "a live env mapping outranks and skips the acquisition");

    /* A failed acquisition simply leaves the ladder without authority 2. */
    memset(lseam, 0, sizeof *lseam);
    memset(&lf, 0, sizeof lf);
    ops_init(&lops, &lf);
    lf.env_bundle = "/nonexistent/controllers.nxb";
    lops.livedb_acquire_fn = fake_livedb_yield;
    dev_init(&ldev, 62, GUID_A);
    check(nxinput_sdl_seam_admit(lseam, &lops, &ldev) ==
              NXINPUT_SDL_SEAM_BLOCK_AUTHORITY &&
              strstr(lf.last, "result=block") != 0 &&
              strstr(lf.last, "db_retries=20") != 0,
          "an empty ladder after a yielded acquisition blocks with the "
          "acquisition evidence in the receipt");
    free(lseam);
  }

  printf("\ntest_sdl_seam: %d checks, %d failures\n", checks, failures);
  if (failures != 0) {
    printf("test_sdl_seam: FAIL\n");
    return 1;
  }
  printf("test_sdl_seam: ALL PASS (FIXTURE_HOST; the real SDL answers are "
         "the matrix gate's)\n");
  return 0;
}
