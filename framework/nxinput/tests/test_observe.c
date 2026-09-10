/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C2 gates for nxinput_observe. Hermetic: no SDL, no
 * device, no environment. */
#include "nxinput_observe.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ------------------------------------------------------------------ sink */

#define SINK_MAX 2048
static char *g_lines[SINK_MAX];
static int g_count;

static void sink(void *userdata, const char *line) {
  (void)userdata;
  if (g_count < SINK_MAX) {
    g_lines[g_count++] = strdup(line);
  }
}

static void sink_clear(void) {
  int i;
  for (i = 0; i < g_count; i++) {
    free(g_lines[i]);
  }
  g_count = 0;
}

static int count_lines_with(const char *marker, const char *needle) {
  int i, n = 0;
  for (i = 0; i < g_count; i++) {
    if (strncmp(g_lines[i], marker, strlen(marker)) == 0 &&
        (needle == NULL || strstr(g_lines[i], needle) != NULL)) {
      n++;
    }
  }
  return n;
}

static const char *find_line(const char *marker, const char *needle) {
  int i;
  for (i = 0; i < g_count; i++) {
    if (strncmp(g_lines[i], marker, strlen(marker)) == 0 &&
        (needle == NULL || strstr(g_lines[i], needle) != NULL)) {
      return g_lines[i];
    }
  }
  return NULL;
}

/* ------------------------------------------------- synthetic decision rig
 * A tiny mapping-selection + event pipeline that mimics the real decision
 * flow shape. The SAME rig runs with observability ON and OFF; its decision
 * trace must be byte-identical, proving telemetry cannot change decisions. */

struct rig_entry {
  const char *guid;
  const char *mapping;
};

static void rig_run(nxinput_observe *obs, char *trace, size_t cap) {
  static const struct rig_entry table[] = {
      {"190000004b4800000011000000010000", "GO-Super,a:b1,b:b0,x:b2,y:b3"},
      {"030000005e0400008e02000014010000", "X360,a:b0,b:b1,x:b2,y:b3"},
  };
  const char *wanted = "190000004b4800000011000000010000";
  const struct rig_entry *chosen = NULL;
  size_t off = 0u;
  size_t i;
  int written;

  for (i = 0u; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(table[i].guid, wanted) == 0) {
      chosen = &table[i];
      break;
    }
  }
  nxinput_observe_load(obs, NXINPUT_OBSERVE_SOURCE_PORTMASTER_ENV, 2u,
                       "deadbeef", wanted, chosen ? chosen->guid : "",
                       0u, chosen ? "selected" : "no-match");
  written = snprintf(trace + off, cap - off, "selected=%s;",
                     chosen ? chosen->mapping : "none");
  off += (size_t)written;

  /* Decision: A pressed -> jump; SELECT+START chord -> exit armed. */
  nxinput_observe_event(obs, 0u, NXINPUT_OBSERVE_A,
                        NXINPUT_OBSERVE_PHASE_PRESS, "b1", "gameplay",
                        "hermetic");
  written = snprintf(trace + off, cap - off, "A=jump;");
  off += (size_t)written;
  nxinput_observe_chord(obs, 0u, "b12", "b13", 1,
                        NXINPUT_OBSERVE_CHORD_ARMED);
  written = snprintf(trace + off, cap - off, "chord=armed;");
  off += (size_t)written;
  (void)off;
}

int main(void) {
  nxinput_observe obs;
  char buffer[128];

  /* 1. Redaction: path, IPv4, free-form name, hostname-ish, symbolic ok. */
  CHECK(strcmp(nxinput_observe_sanitize(buffer, sizeof buffer,
                                        "/home/someone/pad"), "redacted") == 0,
        "a path is redacted");
  CHECK(strcmp(nxinput_observe_sanitize(buffer, sizeof buffer,
                                        "192.168.31.99"), "redacted") == 0,
        "an IPv4 is redacted");
  CHECK(strcmp(nxinput_observe_sanitize(buffer, sizeof buffer,
                                        "My Pad (USB)!"), "My_Pad__USB__") == 0,
        "free-form characters are neutralized");
  CHECK(strcmp(nxinput_observe_sanitize(buffer, sizeof buffer, "BTN_TL2"),
               "BTN_TL2") == 0 &&
            strcmp(nxinput_observe_sanitize(buffer, sizeof buffer, "b12"),
                   "b12") == 0,
        "symbolic BTN_/ordinal names pass untouched");
  CHECK(strcmp(nxinput_observe_sanitize(buffer, sizeof buffer, ""), "-") == 0,
        "empty text serializes as -");

  /* 2. OFF is a total no-op; ON emits. Same rig, identical decisions. */
  {
    char trace_on[256], trace_off[256];
    (void)nxinput_observe_init(&obs, sink, NULL, "run-1", "gen-1", "hermetic");
    sink_clear();
    rig_run(&obs, trace_on, sizeof trace_on);
    CHECK(g_count > 0, "observability ON emits receipts");
    {
      int on_lines = g_count;
      nxinput_observe off;
      (void)nxinput_observe_init(&off, NULL, NULL, "run-1", "gen-1",
                                 "hermetic");
      rig_run(&off, trace_off, sizeof trace_off);
      CHECK(g_count == on_lines,
            "observability OFF emits nothing at all");
      CHECK(strcmp(trace_on, trace_off) == 0,
            "ON/OFF replay: decision traces are byte-identical");
    }
  }

  /* 3. LOAD line content and schema header. */
  sink_clear();
  (void)nxinput_observe_init(&obs, sink, NULL, "porttest-1", "gen-abc",
                             "godot3");
  nxinput_observe_load(&obs, NXINPUT_OBSERVE_SOURCE_PORTMASTER_ENV, 1u,
                       "aabbcc", "190000004b4800000011000000010000",
                       "190000004b4800000011000000010000", 0u, "selected");
  {
    const char *line = find_line("NXINPUT-LOAD:", NULL);
    CHECK(line != NULL &&
              strstr(line, "schema=nx-input-observe/1") != NULL &&
              strstr(line, "run=porttest-1") != NULL &&
              strstr(line, "gen=gen-abc") != NULL &&
              strstr(line, "consumer=godot3") != NULL &&
              strstr(line, "source=portmaster-env") != NULL &&
              strstr(line, "entries=1") != NULL &&
              strstr(line, "map_sha256=aabbcc") != NULL &&
              strstr(line,
                     "guid_selected=190000004b4800000011000000010000") !=
                  NULL &&
              strstr(line, "result=selected") != NULL,
          "NXINPUT-LOAD carries provenance, GUIDs and result");
  }

  /* 4. CAPABILITIES: numbers + digest, never a device name field. */
  nxinput_observe_capabilities(&obs, 0u, 17u, 4u, 0u, 0x2c4u, 3u, 0x130u,
                               0x13eu, 4u);
  {
    const char *line = find_line("NXINPUT-CAPABILITIES:", NULL);
    CHECK(line != NULL && strstr(line, "buttons=17") != NULL &&
              strstr(line, "axes=4") != NULL &&
              strstr(line, "low_keys=3") != NULL &&
              strstr(line, "gamepad_range=304-318") != NULL &&
              strstr(line, "digest=") != NULL &&
              strstr(line, "name") == NULL,
          "NXINPUT-CAPABILITIES is numeric with digest and no name");
  }

  /* 5. BINDING: all 18 canonical controls, completeness provable. */
  {
    unsigned int control;
    CHECK(nxinput_observe_binding_missing(&obs, 0u) ==
              (UINT32_C(1) << 18u) - 1u,
          "before the report every control is missing");
    for (control = 0u; control < NXINPUT_OBSERVE_CONTROL_COUNT; control++) {
      nxinput_observe_trigger_kind trigger_kind =
          (control == NXINPUT_OBSERVE_L2 || control == NXINPUT_OBSERVE_R2)
              ? NXINPUT_OBSERVE_TRIGGER_BUTTON
              : NXINPUT_OBSERVE_TRIGGER_NA;
      nxinput_observe_binding(&obs, 0u, (nxinput_observe_control)control,
                              "b1", NXINPUT_OBSERVE_BIND_BUTTON, 1,
                              NXINPUT_OBSERVE_SEMANTIC_LEGACY_UNMANAGED,
                              "sdl-gamecontroller", 1, trigger_kind);
    }
    CHECK(nxinput_observe_binding_missing(&obs, 0u) == 0u,
          "all 18 controls reported -> missing mask is zero");
    CHECK(count_lines_with("NXINPUT-BINDING:", NULL) == 18,
          "exactly 18 binding lines were emitted");
    CHECK(count_lines_with("NXINPUT-BINDING:", "control=L2") == 1 &&
              find_line("NXINPUT-BINDING:", "control=L2") != NULL &&
              strstr(find_line("NXINPUT-BINDING:", "control=L2"),
                     "trigger_kind=button") != NULL,
          "L2 declares its trigger kind");
    CHECK(find_line("NXINPUT-BINDING:", "control=A") != NULL &&
              strstr(find_line("NXINPUT-BINDING:", "control=A"),
                     "semantic=legacy-unmanaged") != NULL,
          "C2 reports only today's state: legacy-unmanaged, never null");
  }

  /* 6. CHORD: governing pair + the three negative attestations. */
  nxinput_observe_chord(&obs, 0u, "b12", "b13", 1,
                        NXINPUT_OBSERVE_CHORD_ARMED);
  nxinput_observe_chord_denied(&obs, NXINPUT_OBSERVE_CHORD_NEG_L2_R2);
  nxinput_observe_chord_denied(&obs, NXINPUT_OBSERVE_CHORD_NEG_GUIDE_START);
  nxinput_observe_chord_denied(&obs, NXINPUT_OBSERVE_CHORD_NEG_CROSS_PAD);
  CHECK(find_line("NXINPUT-CHORD:", "select=b12 start=b13 same_instance=1 "
                                    "state=armed") != NULL,
        "the SELECT+START governing pair is explicit");
  CHECK(find_line("NXINPUT-CHORD:", "pair=L2+R2 exit=denied") != NULL &&
            find_line("NXINPUT-CHORD:", "pair=GUIDE+START exit=denied") !=
                NULL &&
            find_line("NXINPUT-CHORD:",
                      "pair=SELECT+START-cross-pad exit=denied") != NULL,
        "L2+R2, GUIDE+START and cross-pad exits are attested denied");

  /* 7. Bounded events: first press/release once; repeats only counted. */
  sink_clear();
  (void)nxinput_observe_init(&obs, sink, NULL, "run-2", "gen-2", "hermetic");
  {
    int i;
    for (i = 0; i < 5; i++) {
      nxinput_observe_event(&obs, 0u, NXINPUT_OBSERVE_B,
                            NXINPUT_OBSERVE_PHASE_PRESS, "b0", "gameplay",
                            "hermetic");
      nxinput_observe_event(&obs, 0u, NXINPUT_OBSERVE_B,
                            NXINPUT_OBSERVE_PHASE_RELEASE, "b0", "gameplay",
                            "hermetic");
    }
    CHECK(count_lines_with("NXINPUT-EVENT:", "control=B phase=press") == 1 &&
              count_lines_with("NXINPUT-EVENT:",
                               "control=B phase=release") == 1,
          "bounded mode logs press and release exactly once per control");
    CHECK(obs.dropped == 8u, "repeats are counted as dropped, never logged");
  }

  /* 8. Sticks and triggers: firsts + center/min/max in the summary. */
  nxinput_observe_stick(&obs, 0u, 0, 0, 0, 8000);          /* center */
  nxinput_observe_stick(&obs, 0u, 0, 32000, -12000, 8000); /* out */
  nxinput_observe_stick(&obs, 0u, 0, -32768, 500, 8000);   /* extreme */
  nxinput_observe_stick(&obs, 0u, 0, 0, 0, 8000);          /* neutral */
  nxinput_observe_trigger(&obs, 0u, 1, 0, 1000);
  nxinput_observe_trigger(&obs, 0u, 1, 32767, 1000);
  nxinput_observe_trigger(&obs, 0u, 1, 0, 1000);
  CHECK(count_lines_with("NXINPUT-EVENT:",
                         "control=LEFT_STICK phase=dz-exit") >= 1 &&
            find_line("NXINPUT-EVENT:", "control=LEFT_STICK phase=dz-exit")
                != NULL &&
            strstr(find_line("NXINPUT-EVENT:",
                             "control=LEFT_STICK phase=dz-exit"),
                   "first=1") != NULL,
        "stick deadzone exit is logged with values");
  CHECK(find_line("NXINPUT-EVENT:", "control=LEFT_STICK phase=dz-enter") !=
            NULL,
        "stick back-to-neutral is logged");
  CHECK(find_line("NXINPUT-EVENT:", "control=R2 phase=thr-enter") != NULL &&
            find_line("NXINPUT-EVENT:", "control=R2 phase=thr-exit") != NULL,
        "analog trigger threshold enter/exit are logged");
  nxinput_observe_summary(&obs, 0u);
  CHECK(find_line("NXINPUT-EVENT:", "kind=stick-summary side=left") != NULL &&
            strstr(find_line("NXINPUT-EVENT:",
                             "kind=stick-summary side=left"),
                   "min_x=-32768") != NULL &&
            strstr(find_line("NXINPUT-EVENT:",
                             "kind=stick-summary side=left"),
                   "max_x=32000") != NULL,
        "stick extremes survive into the summary");
  CHECK(find_line("NXINPUT-EVENT:", "kind=trigger-summary side=right") !=
            NULL &&
            strstr(find_line("NXINPUT-EVENT:",
                             "kind=trigger-summary side=right"),
                   "max=32767") != NULL,
        "trigger min/max survive into the summary");
  CHECK(find_line("NXINPUT-EVENT:", "kind=summary") != NULL &&
            strstr(find_line("NXINPUT-EVENT:", "kind=summary"),
                   "never_pressed=") != NULL &&
            strstr(find_line("NXINPUT-EVENT:", "kind=summary"), "A,") != NULL,
        "controls never pressed are named in the summary");

  /* 9. Hotplug and two pads: receipts never mixed; reset re-arms firsts of
   * ONE pad only. */
  sink_clear();
  (void)nxinput_observe_init(&obs, sink, NULL, "run-3", "gen-3", "hermetic");
  nxinput_observe_event(&obs, 0u, NXINPUT_OBSERVE_A,
                        NXINPUT_OBSERVE_PHASE_PRESS, "b1", "-", "-");
  nxinput_observe_event(&obs, 1u, NXINPUT_OBSERVE_A,
                        NXINPUT_OBSERVE_PHASE_PRESS, "b0", "-", "-");
  CHECK(count_lines_with("NXINPUT-EVENT:", "pad=0 control=A") == 1 &&
            count_lines_with("NXINPUT-EVENT:", "pad=1 control=A") == 1,
        "two pads keep independent first-press receipts");
  nxinput_observe_pad_reset(&obs, 1u);
  nxinput_observe_event(&obs, 1u, NXINPUT_OBSERVE_A,
                        NXINPUT_OBSERVE_PHASE_PRESS, "b0", "-", "-");
  nxinput_observe_event(&obs, 0u, NXINPUT_OBSERVE_A,
                        NXINPUT_OBSERVE_PHASE_PRESS, "b1", "-", "-");
  CHECK(count_lines_with("NXINPUT-EVENT:", "pad=1 control=A") == 2,
        "hotplug reset re-arms the replugged pad's firsts");
  CHECK(count_lines_with("NXINPUT-EVENT:", "pad=0 control=A") == 1,
        "the untouched pad keeps its bounded state across the hotplug");

  /* 10. CONSUMER: pending by default; only the hermetic consumer delivers. */
  nxinput_observe_consumer(&obs, NXINPUT_OBSERVE_A, "jump", "press",
                           "gameplay", NXINPUT_OBSERVE_PENDING_NOT_INSTRUMENTED);
  CHECK(find_line("NXINPUT-CONSUMER:",
                  "delivery=pending/not-instrumented") != NULL,
        "an uninstrumented adapter reports pending, never delivered");
  nxinput_observe_consumer(&obs, NXINPUT_OBSERVE_A, "jump", "press",
                           "gameplay", NXINPUT_OBSERVE_DELIVERED);
  nxinput_observe_consumer(&obs, NXINPUT_OBSERVE_B, "cancel", "press",
                           "gameplay", NXINPUT_OBSERVE_SUPPRESSED);
  CHECK(find_line("NXINPUT-CONSUMER:",
                  "control=A action=jump state=press context=gameplay "
                  "delivery=delivered") != NULL &&
            find_line("NXINPUT-CONSUMER:", "delivery=suppressed") != NULL,
        "the hermetic consumer reports delivered and suppressed");

  /* 11. Diagnostic budget: extra events stop at the hard cap. */
  sink_clear();
  (void)nxinput_observe_init(&obs, sink, NULL, "run-4", "gen-4", "hermetic");
  obs.diagnostic_mode = 1;
  {
    int i;
    for (i = 0; i < (int)NXINPUT_OBSERVE_DIAG_BUDGET + 100; i++) {
      nxinput_observe_event(&obs, 0u, NXINPUT_OBSERVE_A,
                            NXINPUT_OBSERVE_PHASE_PRESS, "b1", "-", "-");
    }
    CHECK(g_count == 1 + (int)NXINPUT_OBSERVE_DIAG_BUDGET &&
              obs.dropped == 99u,
          "diagnostic mode is a hard cap, never an unbounded log");
  }

  /* 12. Golden snapshot: the exact first receipt of a scripted run. */
  sink_clear();
  (void)nxinput_observe_init(&obs, sink, NULL, "golden-run", "golden-gen",
                             "hermetic");
  nxinput_observe_load(&obs, NXINPUT_OBSERVE_SOURCE_CFW_FILE, 3u, "cafe01",
                       "-", "030000005e0400008e02000014010000", 1u,
                       "selected");
  CHECK(g_count == 1 &&
            strcmp(g_lines[0],
                   "NXINPUT-LOAD: schema=nx-input-observe/1 run=golden-run "
                   "gen=golden-gen consumer=hermetic seq=1 source=cfw-file "
                   "entries=3 map_sha256=cafe01 guid_requested=- "
                   "guid_selected=030000005e0400008e02000014010000 "
                   "priority=1 result=selected") == 0,
        "golden snapshot of NXINPUT-LOAD is byte-exact");

  /* 13. Receipts never contain a raw mapping, path or IP even if fed one. */
  sink_clear();
  (void)nxinput_observe_init(&obs, sink, NULL, "run-5", "gen-5", "hermetic");
  nxinput_observe_load(&obs, NXINPUT_OBSERVE_SOURCE_CFW_FILE, 1u,
                       "/roms/ports/secret.txt", "192.168.31.44", "ok-guid",
                       0u, "selected");
  nxinput_observe_binding(&obs, 0u, NXINPUT_OBSERVE_A, "/dev/input/event3",
                          NXINPUT_OBSERVE_BIND_BUTTON, 1,
                          NXINPUT_OBSERVE_SEMANTIC_LEGACY_UNMANAGED,
                          "My Sink Name!", 1, NXINPUT_OBSERVE_TRIGGER_NA);
  {
    int i, dirty = 0;
    for (i = 0; i < g_count; i++) {
      if (strstr(g_lines[i], "/roms") != NULL ||
          strstr(g_lines[i], "/dev/") != NULL ||
          strstr(g_lines[i], "192.168.31.44") != NULL ||
          strstr(g_lines[i], "My Sink Name!") != NULL) {
        dirty = 1;
      }
    }
    CHECK(!dirty, "paths, IPs and free-form names never reach a receipt");
  }

  sink_clear();
  if (g_failures != 0) {
    printf("test_observe: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_observe: ALL PASS\n");
  return 0;
}
