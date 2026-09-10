/* SPDX-License-Identifier: GPL-3.0-only */
/* Probe for the C4 corpus gate. Reads files prepared by the orchestrator;
 * never touches SDL, devices or the network. Prints only parse outcomes --
 * it states no verdict of its own.
 *
 *   parse FILE        parse the file as-is; print the NXI code.
 *   control TOKEN     put TOKEN in the control position of an otherwise
 *                     valid NEXTOS_CONTROLLERS/2 skeleton; print the code.
 */
#include "nxinput_gptk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CONTROLS[NXINPUT_GPTK_CONTROL_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK"};

static int cmd_parse(const char *path) {
  static char buffer[NXINPUT_GPTK_MAX_BYTES + 1u];
  nxinput_gptk map;
  char error[192] = "";
  FILE *stream = fopen(path, "rb");
  size_t length;
  int code;

  if (stream == NULL) {
    fprintf(stderr, "unreadable file\n");
    return 2;
  }
  length = fread(buffer, 1u, sizeof buffer, stream);
  fclose(stream);
  code = nxinput_gptk_parse(buffer, length, &map, error, sizeof error);
  printf("PARSE code=%d schema=%u error=%s\n", code,
         (unsigned)map.schema_version, error[0] != '\0' ? error : "-");
  return 0;
}

static int cmd_control(const char *token) {
  static char text[8192];
  nxinput_gptk map;
  char error[192] = "";
  size_t used = 0u;
  int i;
  int code;

  used += (size_t)snprintf(text + used, sizeof text - used, "%s\n[menu]\n",
                           NXINPUT_GPTK_MAGIC_V2);
  /* The token takes the place of the control it names (when it names one of
   * ours); every OTHER control is written normally, so the only thing under
   * test is the token itself and never a manufactured duplicate. */
  used += (size_t)snprintf(text + used, sizeof text - used, "%s = null\n",
                           token);
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    if (strcmp(CONTROLS[i], token) == 0) {
      continue;
    }
    used += (size_t)snprintf(text + used, sizeof text - used, "%s = null\n",
                             CONTROLS[i]);
  }
  used += (size_t)snprintf(text + used, sizeof text - used, "[gameplay]\n");
  for (i = 0; i < (int)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    used += (size_t)snprintf(text + used, sizeof text - used, "%s = null\n",
                             CONTROLS[i]);
  }
  code = nxinput_gptk_parse(text, used, &map, error, sizeof error);
  printf("CONTROL token=%s code=%d error=%s\n", token, code,
         error[0] != '\0' ? error : "-");
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "parse") == 0) {
    return cmd_parse(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "control") == 0) {
    return cmd_control(argv[2]);
  }
  fprintf(stderr, "usage: gptk_v2_probe parse FILE | control TOKEN\n");
  return 2;
}
