/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C5B: the GPTK V2 half of the runtime chain.
 *
 * The 116A audit refused the owner-swap evidence because there was no chain
 * from the launcher's configuration to the mapping the engine received: the
 * swapped mapping was simply written by hand. This tool closes that gap on
 * the V2 side. It parses the port owner's real NEXTOSCONTROLLERS file with
 * the FRAMEWORK's own parser and dispatcher, and prints the live decision
 * for every one of the eighteen V2 controls -- action, `null` or `native`.
 *
 * It decides nothing itself and it never sees a mapping: c5b_v2_to_mapping.py
 * projects these decisions onto the sovereign SDL line.
 *
 * CLAIM CLASS: FIXTURE_HOST. Pure C over a file; no device, no engine.
 */
#include "nxinput_gptk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CONTROLS[NXINPUT_GPTK_CONTROL_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK"};

static const char *decision_name(nxinput_gptk_decision d) {
  switch (d) {
    case NXINPUT_GPTK_DECIDE_ACTION:
      return "action";
    case NXINPUT_GPTK_DECIDE_SUPPRESS:
      return "null";
    case NXINPUT_GPTK_DECIDE_NATIVE:
      return "native";
    case NXINPUT_GPTK_DECIDE_NONE:
    default:
      return "none";
  }
}

int main(int argc, char **argv) {
  static char buffer[NXINPUT_GPTK_MAX_BYTES + 1u];
  nxinput_gptk map;
  nxinput_gptk_dispatcher dispatcher;
  char error[192] = "";
  FILE *stream;
  size_t length;
  int code;
  int control;
  int context = NXINPUT_GPTK_CONTEXT_GAMEPLAY;

  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: %s <NEXTOSCONTROLLERS file> [menu|gameplay]\n",
            argv[0]);
    return 2;
  }
  if (argc == 3 && strcmp(argv[2], "menu") == 0) {
    context = NXINPUT_GPTK_CONTEXT_MENU;
  }
  stream = fopen(argv[1], "rb");
  if (stream == NULL) {
    fprintf(stderr, "unreadable file\n");
    return 2;
  }
  length = fread(buffer, 1u, sizeof buffer, stream);
  (void)fclose(stream);
  code = nxinput_gptk_parse(buffer, length, &map, error, sizeof error);
  if (code != 0) {
    printf("V2 parse=FAIL code=%d error=%s\n", code,
           error[0] != '\0' ? error : "-");
    return 1;
  }
  nxinput_gptk_dispatcher_init(&dispatcher, &map);
  (void)nxinput_gptk_dispatcher_set_context(&dispatcher,
                                            (nxinput_gptk_context)context);
  printf("V2 parse=OK schema=%u port=%s context=%s\n",
         (unsigned)map.schema_version, map.port[0] ? map.port : "-",
         context == NXINPUT_GPTK_CONTEXT_MENU ? "menu" : "gameplay");
  for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
    const char *action = NULL;
    nxinput_gptk_decision decision =
        nxinput_gptk_dispatcher_decision(&dispatcher, control, &action);

    printf("V2 control=%s decision=%s action=%s\n", CONTROLS[control],
           decision_name(decision),
           action != NULL && action[0] != '\0' ? action : "-");
  }
  printf("V2 null_mask=0x%08x native_mask=0x%08x\n",
         (unsigned)nxinput_gptk_dispatcher_null_mask(&dispatcher),
         (unsigned)nxinput_gptk_dispatcher_native_mask(&dispatcher));
  return 0;
}
