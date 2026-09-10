/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C5B: the seam's fail-closed ladder.
 *
 * CLASS: FIXTURE_HOST. The engine table here is scripted, so this is NOT an
 * engine execution and must never be called REAL_API_HOST. What it does give
 * is the exact PRODUCTION function -- nxinput_godot_seam_admit(), the same
 * one compiled into the two real binaries -- driven through the failure
 * classes that cannot be injected from outside a running engine: a setter
 * that refuses, a readback that disagrees, and a probe set that is empty.
 *
 * The origin and identity classes are proved against the REAL engines by
 * tests/c5b/negatives_drive.py, where the pad must never be announced.
 */
#include "nxinput_godot_seam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, name)                                    \
  do {                                                       \
    if (cond) {                                              \
      printf("ok   %s\n", name);                             \
    } else {                                                 \
      printf("FAIL %s (line %d)\n", name, __LINE__);         \
      g_failures++;                                          \
    }                                                        \
  } while (0)

#define BPL (8u * (unsigned int)sizeof(unsigned long))
#define KEY_WORDS ((0x300u + BPL - 1u) / BPL)
#define ABS_WORDS ((0x40u + BPL - 1u) / BPL)

static const char GUID[] = "0300000009120000a1c5000010010000";
/* Bindings only on buttons: no axis, so no absinfo is required to admit. */
static const char MAPPING[] =
    "0300000009120000a1c5000010010000,NXC5 Seam Pad,a:b1,b:b0,x:b3,y:b2,"
    "leftshoulder:b4,rightshoulder:b5,back:b8,start:b9,platform:Linux,";
/* sha256 of MAPPING, computed by the test's own harness script. */
static const char MAPPING_SHA[] =
    "87970ebdbf6bcf7c29e9d39e83a190642a21c31a1b3ea19012d5020a4e39c0f8";

struct fake_engine {
  int setter_ok;
  int setter_calls;
  int readback_shift;   /* 0 = honest; non-zero = the engine disagrees */
  int readback_fail;    /* every readback answers "unknown" */
  int has_mapping;
  unsigned int seq;
  char last[4096];
};

static int op_add(void *userdata, const char *mapping) {
  struct fake_engine *e = (struct fake_engine *)userdata;
  (void)mapping;
  e->setter_calls++;
  if (!e->setter_ok) {
    return -1;
  }
  e->has_mapping = 1;
  return 0;
}

static int op_has(void *userdata, int joy_id) {
  struct fake_engine *e = (struct fake_engine *)userdata;
  (void)joy_id;
  return e->has_mapping;
}

/* The honest answer is the one MAPPING really encodes: a:b1 means physical
 * button 1 is logical 0, b:b0 means physical 0 is logical 1, and so on. */
static int op_readback(void *userdata, int joy_id, int physical) {
  struct fake_engine *e = (struct fake_engine *)userdata;
  /* What MAPPING really encodes, in the engine's own logical order
   * (Godot: a=0 b=1 x=2 y=3 L=4 R=5 L2=6 R2=7 L3=8 R3=9 SELECT=10
   * START=11). Physical ordinal N is the index. */
  static const int LOGICAL[16] = {1, 0, 3, 2, 4, 5, -1, -1,
                                  10, 11, -1, -1, -1, -1, -1, -1};
  int logical;

  (void)joy_id;
  if (e->readback_fail) {
    return -1;
  }
  if (physical < 0 || physical >= 16) {
    return -1;
  }
  logical = LOGICAL[physical];
  if (logical >= 0 && e->readback_shift != 0) {
    logical = logical == 0 ? 1 : 0; /* silently swap A and B */
  }
  return logical;
}

static uint64_t op_now(void *userdata) {
  struct fake_engine *e = (struct fake_engine *)userdata;
  return (uint64_t)(++e->seq) * 1000u;
}
static long op_pid(void *userdata) { (void)userdata; return 4242; }
static long op_tid(void *userdata) { (void)userdata; return 4243; }
static void op_receipt(void *userdata, const char *line) {
  struct fake_engine *e = (struct fake_engine *)userdata;
  (void)snprintf(e->last, sizeof e->last, "%s", line);
}

static void engine_table(nxinput_godot_seam_engine *engine,
                         struct fake_engine *fake) {
  memset(engine, 0, sizeof *engine);
  engine->api_version = NXINPUT_GODOT_SEAM_API_VERSION;
  engine->struct_size = sizeof *engine;
  engine->userdata = fake;
  engine->major = 0u;
  engine->add_joy_mapping = op_add;
  engine->has_mapping = op_has;
  engine->readback_logical_button = op_readback;
  engine->monotonic_ns = op_now;
  engine->pid = op_pid;
  engine->tid = op_tid;
  engine->receipt = op_receipt;
}

static unsigned long g_keys[KEY_WORDS];
static unsigned long g_abs[ABS_WORDS];
/* The kernel's EVIOCGABS answers for THIS fd. The seam refuses to validate an
 * axis without them, so the table is always supplied -- here every entry is
 * absent, which is the truth for a pad that declares no axis at all. */
static nxinput_godot_absinfo g_absinfo[0x40];

static void set_bit(unsigned long *bits, unsigned int code) {
  bits[code / BPL] |= 1UL << (code % BPL);
}

static void device_table(nxinput_godot_seam_device *device, const char *guid) {
  memset(device, 0, sizeof *device);
  device->api_version = NXINPUT_GODOT_SEAM_API_VERSION;
  device->struct_size = sizeof *device;
  device->joy_id = 0;
  device->fd = 7;
  (void)snprintf(device->guid, sizeof device->guid, "%s", guid);
  (void)snprintf(device->name, sizeof device->name, "NXC5 Seam Pad");
  (void)snprintf(device->devpath, sizeof device->devpath,
                 "/dev/input/event99");
  device->key_bits = g_keys;
  device->key_bit_count = 0x300u;
  device->abs_bits = g_abs;
  device->abs_bit_count = 0x40u;
  device->abs_info = g_absinfo;
  device->abs_info_count = 0x40u;
}

static const char *write_declaration(const char *name, const char *domain,
                                     const char *provider, const char *guid,
                                     const char *sha, const char *mapping) {
  static char path[256];
  FILE *stream;

  (void)snprintf(path, sizeof path, "/tmp/nxc5b-seam-%s-%ld.txt", name,
                 (long)getpid());
  stream = fopen(path, "w");
  if (stream == NULL) {
    return NULL;
  }
  fprintf(stream,
          "domain=%s\nprovider=%s\nreceipt=a578a7d82d47e681ad7a1cbe48bb4932"
          "7dad6dbe1c808e46ca3485dbab0dae43\n"
          "generation=c3-nxinput-authority-v1\nguid=%s\nmapping_sha256=%s\n"
          "mapping=%s\n",
          domain, provider, guid, sha, mapping);
  fclose(stream);
  return path;
}

int main(void) {
  nxinput_godot_seam_engine engine;
  nxinput_godot_seam_device device;
  struct fake_engine fake;
  const char *good;
  unsigned int i;

  /* Exactly the buttons MAPPING names, and nothing else. */
  for (i = 0x130u; i <= 0x13eu; i++) {
    set_bit(g_keys, i);
  }

  good = write_declaration("good", "godot", "portmaster-gui", GUID,
                           MAPPING_SHA, MAPPING);
  if (good == NULL) {
    printf("FAIL could not write the declaration\n");
    return 1;
  }

  /* 1. The happy path: setter accepted, readback agreed, pad admitted. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  engine_table(&engine, &fake);
  device_table(&device, GUID);
  {
    nxinput_godot_seam_result r =
        nxinput_godot_seam_admit(&engine, &device, good);
    if (r != NXINPUT_GODOT_SEAM_ADMIT) {
      printf("     debug: r=%s last=%s\n",
             nxinput_godot_seam_result_name(r), fake.last);
    }
    CHECK(r == NXINPUT_GODOT_SEAM_ADMIT,
          "a real setter and an agreeing readback admit the pad");
  }
  CHECK(fake.setter_calls == 1,
        "the setter is called exactly once");

  /* 2. SETTER refuses: no admission, and the failure is named. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 0;
  engine_table(&engine, &fake);
  device_table(&device, GUID);
  CHECK(nxinput_godot_seam_admit(&engine, &device, good) ==
            NXINPUT_GODOT_SEAM_BLOCK_SETTER,
        "a setter that refuses blocks the announce");
  CHECK(strstr(fake.last, "result=block") != NULL,
        "the refusal leaves a deterministic receipt");

  /* 3. READBACK disagrees: the engine stored something else. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  fake.readback_shift = 1;
  engine_table(&engine, &fake);
  device_table(&device, GUID);
  CHECK(nxinput_godot_seam_admit(&engine, &device, good) ==
            NXINPUT_GODOT_SEAM_BLOCK_READBACK,
        "a readback that silently swaps A/B blocks the announce");

  /* 4. READBACK answers nothing at all: zero agreed probes. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  fake.readback_fail = 1;
  engine_table(&engine, &fake);
  device_table(&device, GUID);
  {
    nxinput_godot_seam_result r =
        nxinput_godot_seam_admit(&engine, &device, good);
    CHECK(r == NXINPUT_GODOT_SEAM_BLOCK_READBACK ||
              r == NXINPUT_GODOT_SEAM_BLOCK_PROBES,
          "a readback that knows nothing never admits with zero probes");
  }

  /* 5. IDENTITY: the declaration is for another pad. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  engine_table(&engine, &fake);
  device_table(&device, "deadbeefdeadbeefdeadbeefdeadbeef");
  CHECK(nxinput_godot_seam_admit(&engine, &device, good) ==
            NXINPUT_GODOT_SEAM_BLOCK_IDENTITY,
        "a declaration for another GUID never admits this pad");
  CHECK(fake.setter_calls == 0,
        "identity is checked BEFORE the setter is ever called");

  /* 6. ORIGIN: unknown domain, unknown provider, wrong digest. Each blocks,
   * and none of them reaches the setter. */
  {
    static const struct {
      const char *name;
      const char *domain;
      const char *provider;
      const char *sha;
    } BAD[] = {
        {"domain", "godotX", "portmaster-gui", MAPPING_SHA},
        {"empty-domain", "", "portmaster-gui", MAPPING_SHA},
        {"provider", "godot", "some-random-provider", MAPPING_SHA},
        {"digest", "godot", "portmaster-gui",
         "0000000000000000000000000000000000000000000000000000000000000000"},
    };
    size_t k;

    for (k = 0u; k < sizeof(BAD) / sizeof(BAD[0]); k++) {
      const char *path = write_declaration(BAD[k].name, BAD[k].domain,
                                           BAD[k].provider, GUID,
                                           BAD[k].sha, MAPPING);
      char label[96];

      memset(&fake, 0, sizeof fake);
      fake.setter_ok = 1;
      engine_table(&engine, &fake);
      device_table(&device, GUID);
      (void)snprintf(label, sizeof label,
                     "a bad %s blocks before the setter", BAD[k].name);
      CHECK(nxinput_godot_seam_admit(&engine, &device, path) ==
                    NXINPUT_GODOT_SEAM_BLOCK_ORIGIN &&
                fake.setter_calls == 0,
            label);
      (void)remove(path);
    }
  }

  /* 7. A declaration path that is SET but unreadable is fail-closed: only a
   * genuinely absent declaration is the documented pass-through. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  engine_table(&engine, &fake);
  device_table(&device, GUID);
  CHECK(nxinput_godot_seam_admit(&engine, &device,
                                 "/tmp/nxc5b-does-not-exist-at-all") ==
            NXINPUT_GODOT_SEAM_BLOCK_ORIGIN,
        "a declaration that was asked for but is missing fails closed");
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  engine_table(&engine, &fake);
  device_table(&device, GUID);
  CHECK(nxinput_godot_seam_admit(&engine, &device, "") ==
            NXINPUT_GODOT_SEAM_NO_DECLARATION,
        "no declaration at all is the documented opt-in pass-through");

  /* 8. A partial engine table is refused outright. */
  memset(&fake, 0, sizeof fake);
  fake.setter_ok = 1;
  engine_table(&engine, &fake);
  engine.readback_logical_button = NULL;
  device_table(&device, GUID);
  CHECK(nxinput_godot_seam_admit(&engine, &device, good) ==
            NXINPUT_GODOT_SEAM_BLOCK_ENGINE_TABLE,
        "an engine table without a readback can never admit anything");

  (void)remove(good);
  if (g_failures != 0) {
    printf("test_godot_seam: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_godot_seam: ALL PASS (FIXTURE_HOST; engines proved "
         "separately)\n");
  return 0;
}
