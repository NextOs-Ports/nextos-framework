#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# E7: matriz de CONTROLES por device. Cada caso de fixtures/controls/*.json
# carrega a tabela evdev REAL do device + o mapping SDL que o frontend exporta
# e prova qual autoridade assume o SELECT+START (SDL por estado ou fallback
# evdev) e por quais codigos. Test-first das duas regressoes de campo:
#   - gosuper-arkos-frontend reprovava no nxinput 0.4.0 (mute cego do evdev);
#   - h700-bindless reprovava no 0.4.1 (fallback escolhia L2/R2).
# Joysticks virtuais do SDL; sem hardware, sem rede, sem uinput.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO=$(cd -- "$HERE/../.." && pwd -P)
FIXTURE="$HERE/fixtures/controls/controls-v1.json"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nx-device-controls.XXXXXX")
trap 'rm -rf "$TEST_ROOT"' EXIT

fail() { printf 'device-controls: FAIL %s\n' "$1" >&2; exit 1; }

[ -f "$FIXTURE" ] || fail "fixture ausente: $FIXTURE"
python3 - "$FIXTURE" "$TEST_ROOT/cases.inc" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
assert doc["schema"] == "nxframework-device-controls-v1"
out = open(sys.argv[2], "w")
out.write("static const nx_ctl_case NX_CTL_CASES[] = {\n")
for case in doc["cases"]:
    keys = ",".join(str(int(k, 16)) for k in case["evdev_keys"])
    mapping = case["sdl_mapping_suffix"]
    out.write("  {\"%s\", (const int[]){%s}, %d, %s, %d, %d, %d, %d, %d, %d, \"%s\"},\n" % (
        case["id"], keys, len(case["evdev_keys"]),
        '"%s"' % mapping if mapping else "0",
        case["sdl_buttons"], case["sdl_axes"], case["sdl_hats"],
        1 if case["expect_authority"] == "sdl" else 0,
        int(case["expect_select"], 16) if case["expect_select"] else -1,
        int(case["expect_start"], 16) if case["expect_start"] else -1,
        case["expect_source"]))
out.write("};\n#define NX_CTL_CASE_COUNT %d\n" % len(doc["cases"]))
out.close()
PY

cat > "$TEST_ROOT/harness.c" <<'C'
#include <stdio.h>
#include <string.h>
#define NXINPUT_EVDEV_CHORD_IMPLEMENTATION
#include "nxinput_evdev_chord.h"

typedef struct {
  const char *id;
  const int *keys;
  int key_count;
  const char *mapping;      /* sufixo apos o GUID; NULL = sem pad SDL */
  int buttons, axes, hats;
  int expect_sdl_authority; /* 1 = SDL assume (evdev muta) */
  int expect_select, expect_start; /* -1 = nao aplicavel */
  const char *expect_source;
} nx_ctl_case;

#include "cases.inc"

static int fails;
#define CHECK(cond, id, msg) do { \
  if (!(cond)) { printf("FAIL [%s] %s\n", id, msg); fails++; } } while (0)

int main(void) {
  int i;
  if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
    puts("device-controls: SKIP (SDL indisponivel no host)");
    return 0;
  }
  for (i = 0; i < NX_CTL_CASE_COUNT; ++i) {
    const nx_ctl_case *c = &NX_CTL_CASES[i];
    /* 1. fallback CRU sobre a tabela evdev real do device */
    nx_evc_pad pad;
    int k;
    memset(&pad, 0, sizeof pad);
    for (k = 0; k < c->key_count; ++k)
      pad.keybits[c->keys[k] / (8 * (int)sizeof(unsigned long))] |=
          1UL << (c->keys[k] % (8 * (int)sizeof(unsigned long)));
    nx_evc_apply_raw_fallback(&pad);
    CHECK(strcmp(pad.source, c->expect_source) == 0, c->id,
          "fonte do fallback cru divergiu");
    if (c->expect_select >= 0) {
      CHECK(pad.code_select == c->expect_select, c->id,
            "codigo SELECT do fallback divergiu");
      CHECK(pad.code_start == c->expect_start, c->id,
            "codigo START do fallback divergiu");
    }
    /* 2. autoridade com o mapping REAL que o frontend exporta */
    if (c->mapping) {
      char guid[64], mapping[768];
      int device_index = SDL_JoystickAttachVirtual(
          SDL_JOYSTICK_TYPE_GAMECONTROLLER, c->axes, c->buttons, c->hats);
      SDL_GameController *sdl_pad;
      CHECK(device_index >= 0, c->id, "joystick virtual nao abriu");
      if (device_index < 0)
        continue;
      SDL_JoystickGetGUIDString(
          SDL_JoystickGetDeviceGUID(device_index), guid, sizeof guid);
      snprintf(mapping, sizeof mapping, "%s,%s", guid, c->mapping);
      SDL_GameControllerAddMapping(mapping);
      sdl_pad = SDL_GameControllerOpen(device_index);
      CHECK(sdl_pad != NULL, c->id, "pad virtual nao abriu como controller");
      if (sdl_pad) {
        g_nx_evc_count = 0;
        g_nx_evc_sdl_bound = !c->expect_sdl_authority; /* forca transicao */
        nx_evdev_chord_bind_sdl(sdl_pad);
        CHECK(g_nx_evc_sdl_bound == c->expect_sdl_authority, c->id,
              "autoridade do chord divergiu do contrato do device");
        g_nx_evc_count = -1;
        SDL_GameControllerClose(sdl_pad);
      }
      SDL_JoystickDetachVirtual(device_index);
    }
  }
  SDL_Quit();
  if (fails) { printf("device-controls: FAIL cases=%d\n", fails); return 1; }
  printf("device-controls: PASS cases=%d authorities=sdl+evdev "
         "hardware_ran=0 device_access=0\n", NX_CTL_CASE_COUNT);
  return 0;
}
C

cc -Wall -Wextra -O1 -I"$REPO/framework/nxinput/include" -I"$TEST_ROOT" \
  $(pkg-config --cflags sdl2) "$TEST_ROOT/harness.c" \
  $(pkg-config --libs sdl2) -o "$TEST_ROOT/harness" ||
  fail "harness nao compilou"
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} "$TEST_ROOT/harness" 2>/dev/null
