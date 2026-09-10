/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <linux/input.h>

#include "nxinput_portmaster.h"

#define TEST_BITS_PER_LONG (8u * sizeof(unsigned long))
#define TEST_KEY_WORDS \
  ((KEY_MAX + 1u + TEST_BITS_PER_LONG - 1u) / TEST_BITS_PER_LONG)

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

static void clear_key(unsigned long *bits, unsigned int code) {
  bits[code / TEST_BITS_PER_LONG] &=
      ~(1ul << (code % TEST_BITS_PER_LONG));
}

static void populate_fixture_keys(unsigned long *bits) {
  static const unsigned int keys[] = {
      0x01, 0x72, 0x73, 0x130, 0x131, 0x132, 0x133, 0x134,
      0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c,
  };
  size_t index;
  for (index = 0u; index < sizeof keys / sizeof keys[0]; ++index) {
    set_key(bits, keys[index]);
  }
}

int main(void) {
  unsigned long bits[TEST_KEY_WORDS] = {0};
  nxinput_pm_evidence evidence;
  nxinput_pm_source_evidence source_evidence;
  char converted[2048];
  char live_mapping[2048];
  char source[4096];
  char normalized[4096];
  char partial_mapping[2048];
  char invalid_mapping[2048];
  char duplicate_mapping[2048];
  char uppercase_mapping[2048];
  char suffixed_source[4096];
  char suffixed_expected[4096];
  char tiny[32];
  unsigned long reduced_bits[TEST_KEY_WORDS];
  const char *binding;
  char *target_line;
  int result;

  assert(strlen(fixture_mapping) == 315u);
  populate_fixture_keys(bits);
  snprintf(live_mapping, sizeof live_mapping, "%s", fixture_mapping);
  memcpy(live_mapping, "19004ca6010000000100000000010000", 32u);
  result = nxinput_pm_convert_joydev_mapping(
      live_mapping, bits, KEY_MAX + 1u, NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_PM_REWRITTEN);
  assert(strcmp(converted, expected_mapping) == 0);
  assert(evidence.key_buttons == 16u);
  assert(evidence.gamepad_buttons == 13u);
  assert(evidence.lower_key_buttons == 3u);
  assert(evidence.button_bindings == 15u);
  assert(evidence.rewritten_bindings == 15u);
  assert(evidence.legacy_volume_markers == 2u);

  result = nxinput_pm_convert_joydev_mapping(
      live_mapping, bits, KEY_MAX + 1u, NXINPUT_SDL_DOMAIN_SDL3_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      NULL);
  assert(result == NXINPUT_PM_REWRITTEN);
  assert(strcmp(converted, expected_mapping) == 0);

  snprintf(source, sizeof source,
           "NXCONTROLLER_PROFILES/1\n# retained\n"
           "030000005e0400008e02000010010000,Decoy,a:b0,platform:Linux,\n"
           "%s\n",
           fixture_mapping);
  target_line = strstr(source, fixture_mapping);
  assert(target_line != NULL);
  memcpy(target_line, "19004ca6010000000100000000010000", 32u);
  result = nxinput_pm_normalize_source(
      source, bits, KEY_MAX + 1u, NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", normalized, sizeof normalized,
      &source_evidence);
  assert(result == NXINPUT_PM_REWRITTEN);
  assert(strstr(normalized, "NXCONTROLLER_PROFILES/1\n# retained\n") ==
         normalized);
  assert(strstr(normalized,
                "030000005e0400008e02000010010000,Decoy,a:b0,") != NULL);
  assert(strstr(normalized, expected_mapping) != NULL);
  assert(source_evidence.matching_lines == 1u);
  assert(source_evidence.rewritten_lines == 1u);
  assert(source_evidence.rewritten_bindings == 15u);

  result = nxinput_pm_convert_joydev_mapping(
      expected_mapping, bits, KEY_MAX + 1u, NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_PM_NOT_APPLICABLE);
  assert(converted[0] == '\0');

  result = nxinput_pm_normalize_source(
      expected_mapping, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", normalized, sizeof normalized,
      &source_evidence);
  assert(result == NXINPUT_PM_NOT_APPLICABLE);
  assert(strcmp(normalized, expected_mapping) == 0);
  assert(source_evidence.matching_lines == 1u);
  assert(source_evidence.rewritten_lines == 0u);
  assert(source_evidence.rewritten_bindings == 0u);

  /* 0.10.0 (contract 5.6): one corrupted volume marker no longer blinds the
   * domain gate. The semantic proof still shows only the joydev reading is
   * coherent (the intact volumedown:b1 lands on KEY_VOLUMEDOWN there and on
   * a BTN_* code in evdev), so the line converts -- the field incident that
   * motivated this ("TrimUI-class line without both markers passed in the
   * wrong domain silently") is closed by proof, not by markers. */
  snprintf(partial_mapping, sizeof partial_mapping, "%s", live_mapping);
  target_line = strstr(partial_mapping, "volumeup");
  assert(target_line != NULL);
  memcpy(target_line, "volumeux", 8u);
  result = nxinput_pm_normalize_source(
      partial_mapping, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", normalized, sizeof normalized,
      &source_evidence);
  assert(result == NXINPUT_PM_REWRITTEN);
  {
    char partial_expected[2048];
    snprintf(partial_expected, sizeof partial_expected, "%s",
             expected_mapping);
    target_line = strstr(partial_expected, "volumeup");
    assert(target_line != NULL);
    memcpy(target_line, "volumeux", 8u);
    assert(strcmp(normalized, partial_expected) == 0);
  }
  assert(source_evidence.rewritten_lines == 1u);

  /* A mapping that binds volume keys on a device that measures none is
   * coherent in NO domain: INVALID, and the whole source yields instead of
   * passing the original bytes in an unproved domain. */
  memcpy(reduced_bits, bits, sizeof reduced_bits);
  clear_key(reduced_bits, KEY_VOLUMEDOWN);
  clear_key(reduced_bits, KEY_VOLUMEUP);
  result = nxinput_pm_convert_joydev_mapping(
      live_mapping, reduced_bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_PM_SOURCE_YIELDS);
  assert(evidence.domain_class == (unsigned int)NXINPUT_PM_CLASS_INVALID);
  result = nxinput_pm_normalize_source(
      live_mapping, reduced_bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", normalized, sizeof normalized,
      &source_evidence);
  assert(result == NXINPUT_PM_SOURCE_YIELDS);
  assert(normalized[0] == '\0');
  assert(source_evidence.invalid_lines == 1u);

  /* An ordinal no domain can reach makes the line coherent nowhere:
   * INVALID, source yields (0.10.0; the 0.9.0 ERROR had the same ladder
   * outcome -- the source never decided). */
  binding = strstr(live_mapping, "a:b4,");
  assert(binding != NULL);
  assert(snprintf(invalid_mapping, sizeof invalid_mapping, "%.*s99%s",
                  (int)(binding + 3 - live_mapping), live_mapping,
                  binding + 4) > 0);
  result = nxinput_pm_convert_joydev_mapping(
      invalid_mapping, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_PM_SOURCE_YIELDS);
  assert(evidence.domain_class == (unsigned int)NXINPUT_PM_CLASS_INVALID);

  errno = 0;
  result = nxinput_pm_convert_joydev_mapping(
      live_mapping, bits, KEY_MAX + 1u, NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", tiny, sizeof tiny, &evidence);
  assert(result == NXINPUT_PM_ERROR);
  assert(errno == ENOSPC);

  snprintf(uppercase_mapping, sizeof uppercase_mapping, "%s", live_mapping);
  target_line = strstr(uppercase_mapping, "4ca6");
  assert(target_line != NULL);
  target_line[0] = 'C';
  result = nxinput_pm_normalize_source(
      uppercase_mapping, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", normalized, sizeof normalized,
      &source_evidence);
  assert(result == NXINPUT_PM_NOT_APPLICABLE);
  assert(strcmp(normalized, uppercase_mapping) == 0);
  errno = 0;
  result = nxinput_pm_convert_joydev_mapping(
      uppercase_mapping, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_PM_ERROR);
  assert(errno == EINVAL);

  /* Contradictory duplicate volume bindings cannot be coherent in either
   * domain: INVALID, source yields. */
  binding = strstr(live_mapping, "leftx:a0");
  assert(binding != NULL);
  assert(snprintf(duplicate_mapping, sizeof duplicate_mapping,
                  "%.*svolumedown:b14,volumeup:b15,%s",
                  (int)(binding - live_mapping), live_mapping, binding) > 0);
  result = nxinput_pm_convert_joydev_mapping(
      duplicate_mapping, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", converted, sizeof converted,
      &evidence);
  assert(result == NXINPUT_PM_SOURCE_YIELDS);
  assert(evidence.domain_class == (unsigned int)NXINPUT_PM_CLASS_INVALID);

  assert(snprintf(suffixed_source, sizeof suffixed_source, "%s \t\r\n"
                  "# retained\r\n", live_mapping) > 0);
  assert(snprintf(suffixed_expected, sizeof suffixed_expected, "%s \t\r\n"
                  "# retained\r\n", expected_mapping) > 0);
  result = nxinput_pm_normalize_source(
      suffixed_source, bits, KEY_MAX + 1u,
      NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
      "19004ca6010000000100000000010000", normalized, sizeof normalized,
      &source_evidence);
  assert(result == NXINPUT_PM_REWRITTEN);
  assert(strcmp(normalized, suffixed_expected) == 0);

  /* ---- 0.10.0 classification (contract 5.6) ---------------------- */
  {
    unsigned long trimui_bits[TEST_KEY_WORDS] = {0};
    unsigned long pure_bits[TEST_KEY_WORDS] = {0};
    unsigned int code;
    static const char volumeless_legacy[] =
        "19000000010000000100000000010000,Legacy-NoVolume,"
        "a:b1,b:b2,x:b3,y:b4,leftshoulder:b5,rightshoulder:b6,"
        "back:b7,start:b8,guide:b9,leftstick:b10,rightstick:b14,"
        "leftx:a0,lefty:a1,platform:Linux,";
    static const char ambiguous_line[] =
        "19000000010000000100000000010000,Ambiguous,"
        "a:b1,b:b2,x:b3,y:b4,platform:Linux,";
    static const char identical_line[] =
        "19000000010000000100000000010000,PureGamepad,"
        "a:b0,b:b1,x:b2,y:b3,start:b7,back:b6,platform:Linux,";
    nxinput_pm_domain_class verdict;

    /* KEY_ESC + 14 gamepad buttons: the general no-volume-keys incident. */
    set_key(trimui_bits, KEY_ESC);
    for (code = 0x130u; code <= 0x13du; ++code) {
      set_key(trimui_bits, code);
    }
    verdict = nxinput_pm_classify_mapping(
        volumeless_legacy, trimui_bits, KEY_MAX + 1u,
        NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &evidence);
    assert(verdict == NXINPUT_PM_CLASS_LEGACY_JOYDEV_REWRITE);
    assert(evidence.legacy_volume_markers == 0u);
    result = nxinput_pm_convert_joydev_mapping(
        volumeless_legacy, trimui_bits, KEY_MAX + 1u,
        NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
        "19000000010000000100000000010000", converted, sizeof converted,
        &evidence);
    assert(result == NXINPUT_PM_REWRITTEN);
    /* joydev b1 ranks the second capability (0x130); the evdev ordinal of
     * 0x130 is 0, so `a` must now read b0. */
    assert(strstr(converted, "a:b0,") != NULL);
    assert(strstr(converted, "rightstick:b13,") != NULL);

    /* Both domains coherent but divergent: AMBIGUOUS, the source yields --
     * never a silent pass-through of the original bytes. */
    verdict = nxinput_pm_classify_mapping(
        ambiguous_line, trimui_bits, KEY_MAX + 1u,
        NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &evidence);
    assert(verdict == NXINPUT_PM_CLASS_AMBIGUOUS);
    result = nxinput_pm_normalize_source(
        ambiguous_line, trimui_bits, KEY_MAX + 1u,
        NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
        "19000000010000000100000000010000", normalized, sizeof normalized,
        &source_evidence);
    assert(result == NXINPUT_PM_SOURCE_YIELDS);
    assert(normalized[0] == '\0');
    assert(source_evidence.ambiguous_lines == 1u);

    /* No lower keys: the two enumerations coincide, bytes are preserved. */
    for (code = 0x130u; code <= 0x13du; ++code) {
      set_key(pure_bits, code);
    }
    verdict = nxinput_pm_classify_mapping(
        identical_line, pure_bits, KEY_MAX + 1u,
        NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &evidence);
    assert(verdict == NXINPUT_PM_CLASS_IDENTICAL_IN_BOTH);
    result = nxinput_pm_normalize_source(
        identical_line, pure_bits, KEY_MAX + 1u,
        NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
        "19000000010000000100000000010000", normalized, sizeof normalized,
        &source_evidence);
    assert(result == NXINPUT_PM_NOT_APPLICABLE);
    assert(strcmp(normalized, identical_line) == 0);
    assert(source_evidence.identical_lines == 1u);

    assert(strcmp(nxinput_pm_domain_class_name(
                      NXINPUT_PM_CLASS_LEGACY_JOYDEV_REWRITE),
                  "legacy-joydev-rewrite") == 0);
    assert(strcmp(nxinput_pm_domain_class_name(NXINPUT_PM_CLASS_AMBIGUOUS),
                  "ambiguous") == 0);
  }

  puts("nxinput_portmaster_domain: PASS");
  return 0;
}
