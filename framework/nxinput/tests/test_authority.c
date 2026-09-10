/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C3 (mission 114A): gates for the PRODUCTION mapping
 * adapter -- the code path nxinput.c executes for every pad.
 *
 * The functions under test are the real ones (nxinput_authority_admit /
 * nxinput_authority_forget driving nxinput_sovereign_resolve). Only the SDL
 * boundary is scripted, so a disconnection, a reconnection, a hostile setter
 * and a missing database can be produced without a device. Nothing here
 * re-implements the resolver: the fake runtime only stores and returns what
 * the adapter actually pushed into it. */
#include "nxinput_authority.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, name)                                    \
  do {                                                       \
    if (cond) {                                              \
      printf("ok %s\n", name);                               \
    } else {                                                 \
      printf("FAIL %s (line %d)\n", name, __LINE__);         \
      g_failures++;                                          \
    }                                                        \
  } while (0)

static const char GOSUPER[] =
    "190000004b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,"
    "dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,"
    "leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,"
    "rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,"
    "platform:Linux,";
static const char GUID_GOSUPER[] = "190000004b4800000011000000010000";

static const char X360[] =
    "030000005e0400008e02000014010000,X360 Controller,a:b0,b:b1,x:b2,y:b3,"
    "back:b6,start:b7,leftx:a0,lefty:a1,platform:Linux,";
static const char GUID_X360[] = "030000005e0400008e02000014010000";

/* ------------------------------------------------------------ fake runtime */

#define FAKE_MAX 4

struct fake_device {
  int present;
  char guid[NXINPUT_SOVEREIGN_GUID_MAX];
  int buttons, axes, hats;
};

struct fake_env {
  char name[64];
  char value[1024];
};

struct fake_file {
  char path[256];
  const char *content;
};

struct fake_runtime {
  struct fake_device devices[FAKE_MAX];
  struct fake_env env[4];
  struct fake_file files[4];
  /* What the runtime effectively holds, keyed by GUID. */
  char installed_guid[FAKE_MAX][NXINPUT_SOVEREIGN_GUID_MAX];
  char installed_line[FAKE_MAX][NXINPUT_SOVEREIGN_LINE_MAX];
  int installed_count;
  /* Built-in database the runtime would use with no mapping applied. */
  const char *builtin_line;
  /* Hostile setter: the runtime silently stores a different line. */
  const char *hostile_line;
  /* Setter that refuses everything. */
  int setter_fails;
  unsigned int applies;
  unsigned int readbacks;
};

static const char *fake_getenv(void *ud, const char *name) {
  struct fake_runtime *f = (struct fake_runtime *)ud;
  int i;
  for (i = 0; i < 4; i++) {
    if (f->env[i].name[0] != '\0' && strcmp(f->env[i].name, name) == 0) {
      return f->env[i].value;
    }
  }
  return NULL;
}

static int fake_read_text(void *ud, const char *path, char *out, size_t cap) {
  struct fake_runtime *f = (struct fake_runtime *)ud;
  int i;
  for (i = 0; i < 4; i++) {
    if (f->files[i].path[0] != '\0' && strcmp(f->files[i].path, path) == 0) {
      if (strlen(f->files[i].content) + 1u > cap) {
        return -1;
      }
      (void)snprintf(out, cap, "%s", f->files[i].content);
      return 0;
    }
  }
  return -1;
}

static int fake_device_guid(void *ud, int index, char *out, size_t cap) {
  struct fake_runtime *f = (struct fake_runtime *)ud;
  if (index < 0 || index >= FAKE_MAX || !f->devices[index].present) {
    return -1;
  }
  (void)snprintf(out, cap, "%s", f->devices[index].guid);
  return 0;
}

static int fake_device_caps(void *ud, int index, int *buttons, int *axes,
                            int *hats) {
  struct fake_runtime *f = (struct fake_runtime *)ud;
  if (index < 0 || index >= FAKE_MAX || !f->devices[index].present) {
    return -1;
  }
  *buttons = f->devices[index].buttons;
  *axes = f->devices[index].axes;
  *hats = f->devices[index].hats;
  return 0;
}

static int fake_apply(void *ud, const char *line) {
  struct fake_runtime *f = (struct fake_runtime *)ud;
  const char *stored = f->hostile_line ? f->hostile_line : line;
  int i;
  f->applies++;
  if (f->setter_fails) {
    return -1;
  }
  for (i = 0; i < f->installed_count; i++) {
    if (strncmp(f->installed_guid[i], line, 32u) == 0) {
      (void)snprintf(f->installed_line[i], sizeof f->installed_line[i], "%s",
                     stored);
      return 0;
    }
  }
  if (f->installed_count >= FAKE_MAX) {
    return -1;
  }
  (void)snprintf(f->installed_guid[f->installed_count],
                 sizeof f->installed_guid[0], "%.32s", line);
  (void)snprintf(f->installed_line[f->installed_count],
                 sizeof f->installed_line[0], "%s", stored);
  f->installed_count++;
  return 0;
}

static int fake_mapping_for_guid(void *ud, const char *guid, char *out,
                                 size_t cap) {
  struct fake_runtime *f = (struct fake_runtime *)ud;
  int i;
  f->readbacks++;
  for (i = 0; i < f->installed_count; i++) {
    if (strcmp(f->installed_guid[i], guid) == 0) {
      (void)snprintf(out, cap, "%s", f->installed_line[i]);
      return 0;
    }
  }
  if (f->builtin_line != NULL && strncmp(f->builtin_line, guid, 32u) == 0) {
    (void)snprintf(out, cap, "%s", f->builtin_line);
    return 0;
  }
  return -1;
}

static void fake_runtime_vtable(nxinput_authority_runtime *rt,
                                struct fake_runtime *f, int has_builtin,
                                int accepts_raw) {
  memset(rt, 0, sizeof(*rt));
  rt->api_version = NXINPUT_AUTHORITY_API_VERSION;
  rt->struct_size = sizeof(*rt);
  rt->userdata = f;
  rt->getenv_fn = fake_getenv;
  rt->read_text_fn = fake_read_text;
  rt->device_guid_fn = fake_device_guid;
  rt->device_caps_fn = fake_device_caps;
  rt->apply_mapping_fn = fake_apply;
  rt->mapping_for_guid_fn = fake_mapping_for_guid;
  rt->runtime_has_builtin = has_builtin;
  rt->consumer_accepts_raw = accepts_raw;
}

static void set_env(struct fake_runtime *f, int slot, const char *name,
                    const char *value) {
  (void)snprintf(f->env[slot].name, sizeof f->env[slot].name, "%s", name);
  (void)snprintf(f->env[slot].value, sizeof f->env[slot].value, "%s", value);
}

static void set_device(struct fake_runtime *f, int index, const char *guid,
                       int buttons, int axes, int hats) {
  f->devices[index].present = 1;
  (void)snprintf(f->devices[index].guid, sizeof f->devices[index].guid, "%s",
                 guid);
  f->devices[index].buttons = buttons;
  f->devices[index].axes = axes;
  f->devices[index].hats = hats;
}

int main(void) {
  /* 1. PRODUCTION: the sovereign order decides, and the winning bytes are
   * what the runtime actually ends up holding. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_MAPPING, GOSUPER);
    fake_runtime_vtable(&rt, &f, 0, 0);
    CHECK(nxinput_authority_init(&authority, &rt) == 0,
          "the production adapter initializes over the runtime seam");
    CHECK(nxinput_authority_admit(&authority, 0, 11, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_ENV_GET_CONTROLS &&
              decision.readback_checked == 1 &&
              strcmp(decision.line, GOSUPER) == 0,
          "authority 1 decides in production, byte-intact");
    CHECK(f.applies == 1u && f.readbacks == 1u &&
              strcmp(f.installed_line[0], GOSUPER) == 0,
          "the real setter ran and the runtime holds the decided bytes");
    CHECK(nxinput_authority_find(&authority, 11) != NULL &&
              strcmp(nxinput_authority_find(&authority, 11)->line,
                     GOSUPER) == 0 &&
              nxinput_authority_find(&authority, 11)->buttons == 17,
          "the decision is recorded against the MEASURED capabilities");
  }

  /* 2. The port bundle is REACHABLE from the production adapter: with no
   * get_controls mapping and no CFW database, authority 3 wins from the file
   * named by NXCONTROLLER_PROFILES. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    static char bundle[2048];
    memset(&f, 0, sizeof f);
    (void)snprintf(bundle, sizeof bundle,
                   "NXCONTROLLER_PROFILES/1\n# supplier=test\n%s\n%s\n",
                   X360, GOSUPER);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_BUNDLE, "/port/controllers.nxb");
    (void)snprintf(f.files[0].path, sizeof f.files[0].path,
                   "/port/controllers.nxb");
    f.files[0].content = bundle;
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 7, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_PORT_BUNDLE &&
              strcmp(decision.line, GOSUPER) == 0 &&
              decision.readback_checked == 1,
          "authority 3: the pinned port bundle is reachable in production");
  }

  /* 3. The CFW database is read from SDL_GAMECONTROLLERCONFIG_FILE and the
   * exact GUID wins -- never the first line of the file. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    static char db[2048];
    memset(&f, 0, sizeof f);
    (void)snprintf(db, sizeof db, "%s\n%s\n", X360, GOSUPER);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_DATABASE, "/cfw/db.txt");
    (void)snprintf(f.files[0].path, sizeof f.files[0].path, "/cfw/db.txt");
    f.files[0].content = db;
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 3, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              strcmp(decision.line, GOSUPER) == 0,
          "authority 2 reads the CFW database and matches the exact GUID");
  }

  /* 4. A setter whose READBACK drifts is refused, in production: the pad is
   * not admitted, so gameplay never starts with a silently rewritten pad. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_MAPPING, GOSUPER);
    f.hostile_line =
        "190000004b4800000011000000010000,GO-Super Gamepad,a:b0,b:b1,x:b3,"
        "y:b2,start:b13,back:b12,platform:Linux,";
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 5, &decision) == -1 &&
              decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_READBACK_MISMATCH &&
              authority.admitted == 0u && authority.refused == 1u &&
              nxinput_authority_find(&authority, 5) == NULL,
          "a drifting readback blocks the pad before gameplay");
  }

  /* 5. Nothing reachable at all: explicit failure, no admission, no state. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 1, &decision) == -1 &&
              decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
              decision.step_reason[NXINPUT_SOVEREIGN_RAW_PASSTHROUGH] ==
                  NXINPUT_SOVEREIGN_CONSUMER_REFUSES_RAW &&
              authority.admitted == 0u && f.applies == 0u,
          "no authority reachable is an explicit refusal, not a silent pad");
  }

  /* 6. Authority 4: the runtime's own database, validated through the same
   * readback. Nothing is applied -- the runtime is only asked. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    f.builtin_line = GOSUPER;
    fake_runtime_vtable(&rt, &f, 1, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 2, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_RUNTIME_BUILTIN &&
              f.applies == 0u,
          "authority 4 is validated by readback and applies nothing");
  }

  /* 7. Authority 5 exists only by the consumer's declaration. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    fake_runtime_vtable(&rt, &f, 0, 1);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 4, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_RAW_PASSTHROUGH,
          "raw passthrough is admitted only when the consumer declared it");
  }

  /* 8. HOTPLUG. A pad is admitted, disconnected, and a DIFFERENT pad comes
   * back on the same instance id. The decision must be taken again from the
   * CURRENT GUID and CURRENT capabilities, with no byte of the previous
   * device surviving. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision first, second;
    static char db[2048];
    const nxinput_authority_entry *entry;
    uint32_t first_generation;
    memset(&f, 0, sizeof f);
    (void)snprintf(db, sizeof db, "%s\n%s\n", GOSUPER, X360);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_DATABASE, "/cfw/db.txt");
    (void)snprintf(f.files[0].path, sizeof f.files[0].path, "/cfw/db.txt");
    f.files[0].content = db;
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);

    CHECK(nxinput_authority_admit(&authority, 0, 9, &first) == 0 &&
              first.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              strcmp(first.line, GOSUPER) == 0 && authority.admitted == 1u,
          "hotplug: the first pad is admitted through the sovereign order");
    entry = nxinput_authority_find(&authority, 9);
    first_generation = entry->resolved_generation;

    /* Disconnection: the entry is invalidated immediately. */
    nxinput_authority_forget(&authority, 9);
    CHECK(nxinput_authority_find(&authority, 9) == NULL &&
              authority.admitted == 0u && authority.forgotten == 1u,
          "hotplug: disconnection invalidates the stale decision at once");

    /* A different physical pad reappears on the same instance id. */
    f.devices[0].present = 0;
    set_device(&f, 0, GUID_X360, 11, 2, 0);
    CHECK(nxinput_authority_admit(&authority, 0, 9, &second) == 0 &&
              second.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              strcmp(second.line, X360) == 0,
          "hotplug: reconnection is re-resolved by the CURRENT GUID");
    entry = nxinput_authority_find(&authority, 9);
    CHECK(entry != NULL && strcmp(entry->guid, GUID_X360) == 0 &&
              entry->buttons == 11 && entry->axes == 2 && entry->hats == 0 &&
              strcmp(entry->line, X360) == 0 &&
              strstr(entry->line, "GO-Super") == NULL &&
              entry->resolved_generation != first_generation,
          "hotplug: nothing of the previous device is inherited");
    CHECK(authority.resolutions == 2u && authority.admitted == 1u,
          "hotplug: exactly one fresh resolution per connection");
  }

  /* 9. HOTPLUG with a shrunken pad: the mapping that served the old device
   * is unreachable on the new one, so the reconnection FAILS instead of
   * inheriting a mapping the hardware cannot honor. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_MAPPING, GOSUPER);
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 12, &decision) == 0,
          "hotplug/caps: the full pad is admitted first");
    nxinput_authority_forget(&authority, 12);
    f.devices[0].present = 0;
    set_device(&f, 0, GUID_GOSUPER, 10, 4, 1); /* GOSUPER needs b16 */
    CHECK(nxinput_authority_admit(&authority, 0, 12, &decision) == -1 &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_UNREACHABLE &&
              nxinput_authority_find(&authority, 12) == NULL,
          "hotplug/caps: a mapping the new pad cannot reach is refused");
  }

  /* 10. Two pads with the SAME GUID: independent entries, identical
   * decision, and forgetting one never touches the other. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision a, b;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_device(&f, 1, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_MAPPING, GOSUPER);
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 20, &a) == 0 &&
              nxinput_authority_admit(&authority, 1, 21, &b) == 0 &&
              strcmp(a.line, b.line) == 0 && authority.admitted == 2u,
          "two pads with the same GUID get the identical decision");
    nxinput_authority_forget(&authority, 20);
    CHECK(nxinput_authority_find(&authority, 20) == NULL &&
              nxinput_authority_find(&authority, 21) != NULL &&
              strcmp(nxinput_authority_find(&authority, 21)->line,
                     GOSUPER) == 0 &&
              authority.admitted == 1u,
          "unplugging one of two identical pads leaves no crossed state");
  }

  /* 11. A setter that refuses is not a success: the step yields and, with no
   * other authority, the pad is refused. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    nxinput_sovereign_decision decision;
    memset(&f, 0, sizeof f);
    set_device(&f, 0, GUID_GOSUPER, 17, 4, 1);
    set_env(&f, 0, NXINPUT_AUTHORITY_ENV_MAPPING, GOSUPER);
    f.setter_fails = 1;
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 30, &decision) == -1 &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_READBACK_MISMATCH,
          "a refusing setter never counts as an applied mapping");
  }

  /* 12. An absent device is refused without touching the runtime. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    memset(&f, 0, sizeof f);
    fake_runtime_vtable(&rt, &f, 0, 0);
    (void)nxinput_authority_init(&authority, &rt);
    CHECK(nxinput_authority_admit(&authority, 0, 40, NULL) == -1 &&
              f.applies == 0u && authority.refused == 1u,
          "a device that cannot be measured is refused, never guessed");
  }

  /* 13. An incomplete runtime seam is refused: production cannot run with a
   * missing setter or readback. */
  {
    struct fake_runtime f;
    nxinput_authority_runtime rt;
    nxinput_authority authority;
    memset(&f, 0, sizeof f);
    fake_runtime_vtable(&rt, &f, 0, 0);
    rt.mapping_for_guid_fn = NULL;
    CHECK(nxinput_authority_init(&authority, &rt) == -1,
          "a runtime without a readback cannot back the authority");
  }

  if (g_failures != 0) {
    printf("test_authority: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_authority: ALL PASS\n");
  return 0;
}
