/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input.h>

#include "nxinput_sdl3_portmaster.h"

#define TEST_BITS_PER_LONG (8u * sizeof(unsigned long))
#define TEST_KEY_WORDS \
  ((KEY_MAX + TEST_BITS_PER_LONG - 1u) / TEST_BITS_PER_LONG)

static const char fixture_mapping[] =
    "19000000010000000100000000010000,Deeplay-keys,"
    "a:b4,b:b3,x:b5,y:b6,leftshoulder:b7,rightshoulder:b8,"
    "lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,"
    "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
    "volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,"
    "rightx:a2,righty:a3,rightstick:b15,platform:Linux,";

static const char expected_mapping[] =
    "19004ca6010000000100000000010000,Deeplay-keys,"
    "a:b1,b:b0,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
    "lefttrigger:b10,righttrigger:b11,guide:b8,start:b7,back:b6,"
    "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,"
    "volumedown:b14,volumeup:b15,leftx:a0,lefty:a1,leftstick:b9,"
    "rightx:a2,righty:a3,rightstick:b12,platform:Linux,";

static void set_key(unsigned long *bits, unsigned int code) {
  bits[code / TEST_BITS_PER_LONG] |=
      1ul << (code % TEST_BITS_PER_LONG);
}

static int binding(const char *mapping, const char *semantic) {
  char needle[64];
  const char *found;
  int length = snprintf(needle, sizeof needle, ",%s:b", semantic);
  assert(length > 0 && (size_t)length < sizeof needle);
  found = strstr(mapping, needle);
  assert(found != NULL);
  return atoi(found + length);
}

static void populate_fixture_keys(unsigned long *bits) {
  static const unsigned int keys[] = {
      0x01, 0x72, 0x73,
      0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136,
      0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c,
  };
  size_t index;
  for (index = 0; index < sizeof keys / sizeof keys[0]; ++index)
    set_key(bits, keys[index]);
}

int main(void) {
  unsigned long bits[TEST_KEY_WORDS] = {0};
  nxinput_sdl3_pm_evidence evidence;
  char converted[1024];
  char renamed[sizeof fixture_mapping + 32u];
  const char *name_end;
  int result;

  /* Literal field capture from muOS 2601.1/RG40XX-H. Its exact byte count is
   * deliberate: shortening this fixture would stop guarding the field bug. */
  assert(strlen(fixture_mapping) == 315u);
  populate_fixture_keys(bits);
  /* The pinned SDL3 and the proven BB1 both iterate code < KEY_MAX. The
   * backing word has room for the sentinel bit, but bit_count excludes it;
   * accepting it would shift every lower-key ordinal away from SDL. */
  set_key(bits, KEY_MAX);
  result = nxinput_sdl3_pm_convert_mapping(
      fixture_mapping, bits, KEY_MAX,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_SDL3_PM_REWRITTEN);
  assert(strcmp(converted, expected_mapping) == 0);
  assert(evidence.key_buttons == 16u);
  assert(evidence.gamepad_buttons == 13u);
  assert(evidence.lower_key_buttons == 3u);
  assert(evidence.button_bindings == 15u);
  assert(evidence.rewritten_bindings == 15u);
  assert(evidence.legacy_volume_markers == 2u);

  assert(binding(converted, "a") == 1);
  assert(binding(converted, "b") == 0);
  assert(binding(converted, "start") == 7);
  assert(binding(converted, "back") == 6);
  /* Hats, axes, inversion/qualifiers and platform fields are not ordinals and
   * must remain byte-identical. The full strcmp above is the primary gate;
   * these checks make a failure local and readable. */
  assert(strstr(converted,
                "dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,") !=
         NULL);
  assert(strstr(converted,
                "leftx:a0,lefty:a1,leftstick:b9,rightx:a2,righty:a3,") !=
         NULL);
  assert(strstr(converted, "platform:Linux,") != NULL);

  /* Classification is capability-driven, never controller-name-driven. */
  name_end = strchr(strchr(fixture_mapping, ',') + 1, ',');
  assert(name_end != NULL);
  snprintf(renamed, sizeof renamed,
           "19000000010000000100000000010000,Arbitrary Pad%s", name_end);
  result = nxinput_sdl3_pm_convert_mapping(
      renamed, bits, KEY_MAX, "19004ca6010000000100000000010000",
      converted, sizeof converted, NULL);
  assert(result == NXINPUT_SDL3_PM_REWRITTEN);
  assert(binding(converted, "a") == 1);
  assert(binding(converted, "b") == 0);
  assert(binding(converted, "start") == 7);
  assert(binding(converted, "back") == 6);

  /* A native/already converted mapping is a closed no-op, never a second
   * translation over the same physical buttons. */
  memset(converted, 0xa5, sizeof converted);
  memset(&evidence, 0, sizeof evidence);
  result = nxinput_sdl3_pm_convert_mapping(
      expected_mapping, bits, KEY_MAX,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_SDL3_PM_NOT_APPLICABLE);
  assert(converted[0] == '\0');
  assert(evidence.legacy_volume_markers == 0u);


  /* --------------------------------------------------------------------
   * V4-CONTROLLERS-02: heterogeneous multi-entry SDL_GAMECONTROLLERCONFIG.
   * Real CFW lists carry one entry per known device, in several dialects,
   * with blank lines and comments. Picking by position is exactly the false
   * success this front exists to stop.
   * ------------------------------------------------------------------ */
  {
    static const char kOther[] =
        "030000005e0400008e02000010010000,Xbox 360 Controller,"
        "a:b0,b:b1,x:b2,y:b3,start:b7,back:b6,platform:Linux,";
    static const char kTargetGuid[] = "19004ca6010000000100000000010000";
    char list[4096];
    char selected[NXINPUT_SDL3_PM_MAPPING_MAX];
    unsigned int entries = 0u;
    int status;

    /* One entry only: the historical behaviour is preserved exactly. */
    status = nxinput_sdl3_pm_select_mapping(fixture_mapping, kTargetGuid,
                                            selected, sizeof selected,
                                            &entries);
    assert(status == NXINPUT_SDL3_PM_REWRITTEN);
    assert(entries == 1u);
    assert(strcmp(selected, fixture_mapping) == 0);

    /* Several entries and the target GUID present: that entry is selected,
     * regardless of its position, and blank lines/comments are skipped. */
    snprintf(list, sizeof list, "# a CFW comment\n%s\n\n%s\n", kOther,
             expected_mapping);
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_REWRITTEN);
    assert(entries == 2u);
    assert(strcmp(selected, expected_mapping) == 0);

    /* CRLF and trailing whitespace are transport noise, not content. */
    snprintf(list, sizeof list, "%s  \r\n%s\t\r\n", kOther, expected_mapping);
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_REWRITTEN);
    assert(strcmp(selected, expected_mapping) == 0);

    /* Several entries and NONE for this device: no guess, no rewrite. */
    snprintf(list, sizeof list, "%s\n%s\n", kOther, kOther);
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_NOT_APPLICABLE);
    assert(selected[0] == '\0');

    /* The same GUID twice with identical bytes is harmless. */
    snprintf(list, sizeof list, "%s\n%s\n", expected_mapping,
             expected_mapping);
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_REWRITTEN);
    assert(entries == 2u);

    /* The same GUID twice with DIVERGENT bytes is an explicit collision: the
     * SDL3 store is keyed by GUID, so order must never decide the winner. */
    {
      char divergent[NXINPUT_SDL3_PM_MAPPING_MAX];
      snprintf(divergent, sizeof divergent, "%s", expected_mapping);
      divergent[strlen(divergent) - 2u] = 'X';
      snprintf(list, sizeof list, "%s\n%s\n", expected_mapping, divergent);
      status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                              sizeof selected, &entries);
      assert(status == NXINPUT_SDL3_PM_ERROR);
      assert(selected[0] == '\0');
    }

    /* Malformed entries fail closed instead of being skipped: a line without
     * a GUID first field could be a truncated entry for this very device. */
    snprintf(list, sizeof list, "not-a-guid,Pad,a:b0,\n%s\n",
             expected_mapping);
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_ERROR);
    snprintf(list, sizeof list, "19004ca601000000010000000001000g,Pad,a:b0,\n");
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_ERROR);
    snprintf(list, sizeof list, "%s\n", kTargetGuid);
    status = nxinput_sdl3_pm_select_mapping(list, kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_ERROR);

    /* An empty or comment-only list applies to nothing. */
    status = nxinput_sdl3_pm_select_mapping("\n# only a comment\n\n",
                                            kTargetGuid, selected,
                                            sizeof selected, &entries);
    assert(status == NXINPUT_SDL3_PM_NOT_APPLICABLE);
    assert(entries == 0u);

    /* Bad arguments never write anything. */
    assert(nxinput_sdl3_pm_select_mapping(NULL, kTargetGuid, selected,
                                          sizeof selected, NULL) ==
           NXINPUT_SDL3_PM_ERROR);
    assert(nxinput_sdl3_pm_select_mapping(list, "short", selected,
                                          sizeof selected, NULL) ==
           NXINPUT_SDL3_PM_ERROR);
    assert(nxinput_sdl3_pm_select_mapping(expected_mapping, kTargetGuid,
                                          selected, 4u, NULL) ==
           NXINPUT_SDL3_PM_ERROR);
  }

  puts("nxinput_sdl3_portmaster_mapping: PASS");
  return 0;
}
