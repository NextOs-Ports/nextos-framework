/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C3 gates for the sovereign mapping authority order.
 * Hermetic: no SDL, no device, no environment. */
#include "nxinput_sovereign.h"

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

/* The REAL GO-Super mapping captured physically in C1 (byte-exact). */
static const char GOSUPER[] =
    "190000004b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,"
    "dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,"
    "leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,"
    "rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,"
    "platform:Linux,";
static const char GUID_GOSUPER[] = "190000004b4800000011000000010000";

static const char OTHER_LINE[] =
    "030000005e0400008e02000014010000,X360 Controller,a:b0,b:b1,x:b2,y:b3,"
    "back:b6,start:b7,leftx:a0,lefty:a1,platform:Linux,";

/* Faithful readback: echoes the applied line (a well-behaved runtime). */
static int readback_ok(void *userdata, const char *line, char *out,
                       size_t cap) {
  (void)userdata;
  if (line[0] == '\0') {
    return -1; /* no built-in database */
  }
  (void)snprintf(out, cap, "%s", line);
  return 0;
}

/* Hostile readback: silently swaps A/B and X/Y (button reorder), the exact
 * class of drift a setter-without-readback used to hide. */
static int readback_swapped(void *userdata, const char *line, char *out,
                            size_t cap) {
  (void)userdata;
  (void)line;
  (void)snprintf(out, cap,
                 "190000004b4800000011000000010000,GO-Super Gamepad,a:b0,"
                 "b:b1,x:b3,y:b2,start:b13,back:b12,platform:Linux,");
  return 0;
}

/* Readbacks that answer with an AMBIGUOUS duplicated key. The divergent
 * occurrence is placed first in one and last in the other, so the order of
 * the duplicates can never be what saves the comparison. */
static int readback_duplicate_first(void *userdata, const char *line,
                                    char *out, size_t cap) {
  (void)userdata;
  (void)line;
  (void)snprintf(out, cap,
                 "190000004b4800000011000000010000,GO-Super Gamepad,a:b0,"
                 "a:b1,b:b0,x:b2,y:b3,platform:Linux,");
  return 0;
}

static int readback_duplicate_last(void *userdata, const char *line,
                                   char *out, size_t cap) {
  (void)userdata;
  (void)line;
  (void)snprintf(out, cap,
                 "190000004b4800000011000000010000,GO-Super Gamepad,a:b1,"
                 "b:b0,x:b2,y:b3,a:b0,platform:Linux,");
  return 0;
}

/* Built-in-database runtime: answers only the empty-line query. */
static int readback_builtin(void *userdata, const char *line, char *out,
                            size_t cap) {
  (void)userdata;
  if (line[0] != '\0') {
    (void)snprintf(out, cap, "%s", line);
    return 0;
  }
  (void)snprintf(out, cap, "%s", GOSUPER);
  return 0;
}

static void make_request(nxinput_sovereign_request *request) {
  (void)nxinput_sovereign_request_init(request);
  memcpy(request->guid, GUID_GOSUPER, sizeof(GUID_GOSUPER));
  request->caps.buttons = 17;
  request->caps.axes = 4;
  request->caps.hats = 0;
}

int main(void) {
  nxinput_sovereign_request request;
  nxinput_sovereign_decision decision;
  char bundle[2048];

  (void)snprintf(bundle, sizeof bundle,
                 "NXCONTROLLER_PROFILES/1\n# portmaster_commit=test\n%s\n%s\n",
                 GOSUPER, OTHER_LINE);

  /* 1. Authority 1 wins with the PortMaster env mapping, BYTE-INTACT. The
   * removed rewrite would have produced x:b3,y:b2; sovereignty keeps
   * x:b2,y:b3 exactly as the CFW delivered. */
  make_request(&request);
  CHECK(nxinput_sovereign_resolve(&request, GOSUPER, OTHER_LINE, bundle,
                                  readback_ok, NULL, &decision) == 0 &&
            decision.source == NXINPUT_SOVEREIGN_ENV_GET_CONTROLS &&
            decision.reason == NXINPUT_SOVEREIGN_OK &&
            decision.readback_checked == 1 &&
            strcmp(decision.line, GOSUPER) == 0,
        "the get_controls mapping is sovereign and byte-intact");
  CHECK(strstr(decision.line, "x:b2,y:b3") != NULL &&
            strstr(decision.line, "a:b1,b:b0") != NULL,
        "no post-load A/B/X/Y rewrite is ever applied");

  /* 2. Exact GUID only: a list without this GUID yields GUID_NOT_FOUND and
   * NEVER the first line; the next authority (CFW db) wins. */
  {
    char db[2048];
    (void)snprintf(db, sizeof db, "%s\n%s\n", OTHER_LINE, GOSUPER);
    CHECK(nxinput_sovereign_resolve(&request, OTHER_LINE, db, "",
                                    readback_ok, NULL, &decision) == 0 &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_GUID_NOT_FOUND &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              strcmp(decision.line, GOSUPER) == 0,
          "a non-matching list never yields its first line");
  }

  /* 3. Same GUID twice with divergent bodies: real CFW stores carry this
   * legally (the muOS official database does), and the runtime that will
   * execute the mapping is SDL, whose AddMapping REPLACES an existing entry.
   * The LAST line wins, and the tolerance is COUNTED as evidence. */
  {
    char db[2048];
    (void)snprintf(db, sizeof db,
                   "%s\n190000004b4800000011000000010000,Divergent,a:b0,"
                   "b:b1,platform:Linux,\n", GOSUPER);
    CHECK(nxinput_sovereign_resolve(&request, "", db, bundle, readback_ok,
                                    NULL, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              decision.reason == NXINPUT_SOVEREIGN_OK &&
              decision.duplicate_lastwins == 1 &&
              strstr(decision.line, "Divergent") != NULL,
          "duplicate divergent GUID resolves last-wins and is counted");
  }

  /* 4. Byte-identical duplicate is fine and counts no divergence. */
  {
    char db[2048];
    (void)snprintf(db, sizeof db, "%s\n%s\n", GOSUPER, GOSUPER);
    CHECK(nxinput_sovereign_resolve(&request, "", db, "", readback_ok, NULL,
                                    &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              decision.duplicate_lastwins == 0,
          "a byte-identical duplicate entry is accepted");
  }

  /* 5. Truncated / malformed lines fail syntax, never win. A line whose
   * only fields are unknown keys binds nothing and is not a mapping; an
   * unknown key NEXT TO a real binding is metadata (SDL ignores it). */
  CHECK(nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Trunc,a:b") ==
            NXINPUT_SOVEREIGN_SYNTAX_INVALID &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Bad,a=b1,") ==
            NXINPUT_SOVEREIGN_SYNTAX_INVALID &&
        nxinput_sovereign_line_syntax("not-a-guid,Name,a:b1,") ==
            NXINPUT_SOVEREIGN_SYNTAX_INVALID &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Bad,zz:b1,") ==
            NXINPUT_SOVEREIGN_SYNTAX_INVALID,
        "truncated, malformed and zero-binding lines fail syntax");
  CHECK(nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Real CFW,zz:b1,sdk>=:21,"
            "hint:SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,a:b1,") ==
            NXINPUT_SOVEREIGN_OK,
        "unknown key:value fields are metadata beside a real binding");

  /* 6. Unreachable mapping (ordinal beyond measured caps) yields the step. */
  {
    nxinput_sovereign_request small;
    make_request(&small);
    small.caps.buttons = 10; /* GOSUPER references up to b16 */
    CHECK(nxinput_sovereign_resolve(&small, GOSUPER, "", bundle, readback_ok,
                                    NULL, &decision) == 0 &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_UNREACHABLE &&
              decision.step_reason[NXINPUT_SOVEREIGN_PORT_BUNDLE] ==
                  NXINPUT_SOVEREIGN_UNREACHABLE &&
              decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT,
          "an unreachable mapping never wins any step");
  }

  /* 7. Readback that swaps A/B/X/Y is refused: a setter without an
   * identical semantic readback never wins. */
  CHECK(nxinput_sovereign_resolve(&request, GOSUPER, "", "",
                                  readback_swapped, NULL, &decision) == 0 &&
            decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                NXINPUT_SOVEREIGN_READBACK_MISMATCH &&
            decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
            decision.reason != NXINPUT_SOVEREIGN_OK,
        "a swapped readback fails closed instead of silently drifting");

  /* 8. Semantic identity: axis-as-button, button-as-axis and inverted hats
   * are never identical; field order and name are cosmetic. */
  CHECK(nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,N1,a:b1,x:b2,platform:Linux,",
            "190000004b4800000011000000010000,Other Name,x:b2,a:b1,") == 1,
        "field order and name are cosmetic for semantic identity");
  CHECK(nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,N,a:b1,",
            "190000004b4800000011000000010000,N,a:a1,") == 0 &&
        nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,N,leftx:a0,",
            "190000004b4800000011000000010000,N,leftx:b0,") == 0 &&
        nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,N,dpup:h0.1,",
            "190000004b4800000011000000010000,N,dpup:h0.4,") == 0,
        "axis-vs-button and inverted hats are never identical");

  /* 9. Bundle: header is mandatory; a valid bundle serves its GUID. */
  CHECK(nxinput_sovereign_bundle_lookup("junk\n" , GUID_GOSUPER, NULL, 0u) ==
            NXINPUT_SOVEREIGN_BUNDLE_HEADER_INVALID,
        "a bundle without the NXCONTROLLER_PROFILES/1 header is refused");
  {
    char out[NXINPUT_SOVEREIGN_LINE_MAX];
    CHECK(nxinput_sovereign_bundle_lookup(bundle, GUID_GOSUPER, out,
                                          sizeof out) ==
              NXINPUT_SOVEREIGN_OK &&
              strcmp(out, GOSUPER) == 0,
          "the bundle serves its GUID entry byte-intact");
  }
  CHECK(nxinput_sovereign_resolve(&request, "", "", bundle, readback_ok,
                                  NULL, &decision) == 0 &&
            decision.source == NXINPUT_SOVEREIGN_PORT_BUNDLE &&
            strcmp(decision.line, GOSUPER) == 0,
        "authority 3: the pinned bundle wins when 1 and 2 are empty");

  /* 10. Authority 4: the runtime's built-in database, validated by the same
   * ladder through the injected effect. */
  {
    nxinput_sovereign_request builtin;
    make_request(&builtin);
    builtin.runtime_has_builtin = 1;
    CHECK(nxinput_sovereign_resolve(&builtin, "", "", "", readback_builtin,
                                    NULL, &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_RUNTIME_BUILTIN &&
              decision.readback_checked == 1 &&
              strcmp(decision.line, GOSUPER) == 0,
          "authority 4: a validated built-in database can win");
  }

  /* 11. Authority 5: raw passthrough only with the consumer's declaration. */
  CHECK(nxinput_sovereign_resolve(&request, "", "", "", readback_ok, NULL,
                                  &decision) == 0 &&
            decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
            decision.step_reason[NXINPUT_SOVEREIGN_RAW_PASSTHROUGH] ==
                NXINPUT_SOVEREIGN_CONSUMER_REFUSES_RAW,
        "raw passthrough is refused without the consumer's declaration");
  {
    nxinput_sovereign_request raw;
    make_request(&raw);
    raw.consumer_accepts_raw = 1;
    CHECK(nxinput_sovereign_resolve(&raw, "", "", "", readback_ok, NULL,
                                    &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_RAW_PASSTHROUGH,
          "raw passthrough wins only by the consumer's declaration");
  }

  /* 12. Explicit failure before gameplay when nothing is reachable, with
   * every step's reason recorded. */
  CHECK(nxinput_sovereign_resolve(&request, "", "", "", readback_ok, NULL,
                                  &decision) == 0 &&
            decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
            decision.line[0] == '\0' &&
            decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                NXINPUT_SOVEREIGN_SOURCE_EMPTY &&
            decision.step_reason[NXINPUT_SOVEREIGN_CFW_DB_GUID] ==
                NXINPUT_SOVEREIGN_SOURCE_EMPTY,
        "nothing reachable is an explicit failure, never silent gameplay");

  /* 13. Two devices with the same GUID: resolution is deterministic and
   * identical for both (order and instance never decide). */
  {
    nxinput_sovereign_decision second;
    (void)nxinput_sovereign_resolve(&request, GOSUPER, "", bundle,
                                    readback_ok, NULL, &decision);
    (void)nxinput_sovereign_resolve(&request, GOSUPER, "", bundle,
                                    readback_ok, NULL, &second);
    CHECK(decision.source == second.source &&
              strcmp(decision.line, second.line) == 0,
          "two pads with the same GUID get the identical decision");
  }

  /* 14. Malformed request fails closed. */
  {
    nxinput_sovereign_request bad;
    make_request(&bad);
    bad.guid[5] = 'Z';
    CHECK(nxinput_sovereign_resolve(&bad, GOSUPER, "", "", readback_ok, NULL,
                                    &decision) == -1 &&
              decision.reason == NXINPUT_SOVEREIGN_REQUEST_INVALID,
          "an invalid GUID in the request is refused");
  }

  /* 15. Mission 114A / blocker 4, re-grounded on the muOS regression: a
   * duplicated binding key is not ambiguous to the runtime that executes
   * the line -- SDL parses fields in order and the LAST assignment wins.
   * The parser resolves the duplicate exactly that way (dedup, last wins),
   * so the semantic comparison reads EFFECTIVE pairs and a divergent
   * duplicate can never hide behind a satisfied first occurrence. */
  CHECK(nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Dup,a:b1,x:b2,a:b0,") ==
            NXINPUT_SOVEREIGN_OK &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Dup,a:b0,x:b2,a:b1,") ==
            NXINPUT_SOVEREIGN_OK &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Dup,a:b1,x:b2,a:b1,") ==
            NXINPUT_SOVEREIGN_OK,
        "a duplicated binding key parses the way SDL executes it");
  /* The EFFECTIVE binding is the last occurrence, and nothing else. */
  CHECK(nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,Dup,a:b1,x:b2,a:b0,",
            "190000004b4800000011000000010000,N,a:b0,x:b2,") == 1 &&
        nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,Dup,a:b1,x:b2,a:b0,",
            "190000004b4800000011000000010000,N,a:b1,x:b2,") == 0,
        "the last occurrence is the effective binding");
  /* The comparison can no longer be satisfied by reusing one occurrence. */
  CHECK(nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,N,a:b1,a:b1,x:b2,",
            "190000004b4800000011000000010000,N,a:b1,a:b0,x:b2,") == 0 &&
        nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,N,a:b1,a:b0,x:b2,",
            "190000004b4800000011000000010000,N,a:b1,a:b1,x:b2,") == 0,
        "a duplicated key can never be satisfied twice by one occurrence");
  /* A readback that answers with a duplicated, divergent key is refused in
   * both orders -- the divergent occurrence is never allowed to hide. */
  CHECK(nxinput_sovereign_resolve(&request, GOSUPER, "", "",
                                  readback_duplicate_first, NULL,
                                  &decision) == 0 &&
            decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
            decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                NXINPUT_SOVEREIGN_READBACK_MISMATCH,
        "a readback with a duplicated key fails closed (divergent first)");
  CHECK(nxinput_sovereign_resolve(&request, GOSUPER, "", "",
                                  readback_duplicate_last, NULL,
                                  &decision) == 0 &&
            decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT &&
            decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                NXINPUT_SOVEREIGN_READBACK_MISMATCH,
        "a readback with a duplicated key fails closed (divergent last)");
  /* A database entry with a duplicated key wins with its EFFECTIVE (last)
   * bindings -- refusing it would reject the CFW's own working data. */
  {
    char db[2048];
    (void)snprintf(db, sizeof db,
                   "190000004b4800000011000000010000,Dup,a:b1,b:b0,a:b0,"
                   "platform:Linux,\n");
    CHECK(nxinput_sovereign_resolve(&request, "", db, "", readback_ok, NULL,
                                    &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              nxinput_sovereign_semantically_identical(
                  decision.line,
                  "190000004b4800000011000000010000,N,a:b0,b:b0,") == 1,
          "a database entry with a duplicated key wins by its last value");
  }

  /* 16. Mission 114A / blocker 4: a mapping with NO effective binding is not
   * a mapping. It parses, but it would compare identical to any other empty
   * line and would authorize gameplay with nothing reachable. */
  CHECK(nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Empty,platform:Linux,") ==
            NXINPUT_SOVEREIGN_SYNTAX_INVALID &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Empty,") ==
            NXINPUT_SOVEREIGN_SYNTAX_INVALID &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Empty,platform:Linux,crc:aa,")
            == NXINPUT_SOVEREIGN_SYNTAX_INVALID,
        "a syntactically valid mapping with zero bindings fails closed");
  CHECK(nxinput_sovereign_semantically_identical(
            "190000004b4800000011000000010000,A,platform:Linux,",
            "190000004b4800000011000000010000,B,platform:Linux,") == 0,
        "two empty mappings are never 'identical' to each other");
  {
    char db[2048];
    (void)snprintf(db, sizeof db,
                   "190000004b4800000011000000010000,Empty,platform:Linux,"
                   "\n");
    CHECK(nxinput_sovereign_resolve(&request, db, "", "", readback_ok, NULL,
                                    &decision) == 0 &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_SYNTAX_INVALID &&
              decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT,
          "an empty mapping never wins an authority step");
  }
  /* And the rules stay narrow: one real binding is enough, and the SDL2
   * half-axis keys are distinct keys, not duplicates. */
  CHECK(nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,One,a:b1,") ==
            NXINPUT_SOVEREIGN_OK &&
        nxinput_sovereign_line_syntax(
            "190000004b4800000011000000010000,Half,+leftx:a0,-leftx:a1,"
            "leftx:a0,platform:Linux,") == NXINPUT_SOVEREIGN_OK,
        "one real binding is enough and +key/-key/key are distinct keys");

  /* 17. muOS regression (Nameless Cat 1.2.3, 2026-08-31): the CFW's own
   * data must be admissible. The live failure had every step refusing --
   * env syntax-invalid, cfw duplicate-divergent, bundle source-empty -- and
   * the port died with "controller initialization failed closed" although
   * the muOS database HELD a working entry for the pad. The same shape must
   * now resolve. GUID is the live one from the device log. */
  {
    static const char MUOS_GUID[] = "19000000010000000100000000010000";
    static const char MUOS_A[] =
        "19000000010000000100000000010000,muOS Gamepad,a:b0,b:b1,x:b2,y:b3,"
        "back:b8,start:b9,leftx:a0,lefty:a1,volumeup:b6,platform:Linux,";
    static const char MUOS_B[] =
        "19000000010000000100000000010000,muOS Gamepad,a:b1,b:b0,x:b2,y:b3,"
        "back:b8,start:b9,leftx:a0,lefty:a1,platform:Linux,";
    nxinput_sovereign_request muos;
    char db[2048];
    (void)nxinput_sovereign_request_init(&muos);
    memcpy(muos.guid, MUOS_GUID, sizeof(MUOS_GUID));
    muos.caps.buttons = 10;
    muos.caps.axes = 2;
    muos.caps.hats = 0;
    /* The env mapping carries a key this parser never met plus the real
     * bindings -- real get_controls output, not lab bytes. It must WIN. */
    {
      char env[2048];
      (void)snprintf(env, sizeof env, "%s\n", MUOS_A);
      /* An unknown metadata key inside the env line. */
      CHECK(nxinput_sovereign_resolve(&muos, env, "", "", readback_ok, NULL,
                                      &decision) == 0 &&
                decision.source == NXINPUT_SOVEREIGN_ENV_GET_CONTROLS &&
                strcmp(decision.line, MUOS_A) == 0,
            "muOS: the delivered get_controls mapping is admissible");
    }
    /* The official database carries the SAME GUID twice with divergent
     * bodies (the live case). SDL semantics: the last wins, counted. */
    (void)snprintf(db, sizeof db, "%s\n%s\n", MUOS_A, MUOS_B);
    CHECK(nxinput_sovereign_resolve(&muos, "", db, "", readback_ok, NULL,
                                    &decision) == 0 &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID &&
              decision.duplicate_lastwins == 1 &&
              strcmp(decision.line, MUOS_B) == 0,
          "muOS: a divergent duplicate in the CFW database resolves");
    /* And when the env really is garbage, the db still saves the run. */
    CHECK(nxinput_sovereign_resolve(&muos, "not a mapping at all", db,
                                    "", readback_ok, NULL, &decision) == 0 &&
              decision.step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] ==
                  NXINPUT_SOVEREIGN_GUID_NOT_FOUND &&
              decision.source == NXINPUT_SOVEREIGN_CFW_DB_GUID,
          "muOS: a broken env yields and the CFW database wins");
  }

  if (g_failures != 0) {
    printf("test_sovereign: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_sovereign: ALL PASS\n");
  return 0;
}
