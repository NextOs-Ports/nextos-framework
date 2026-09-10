/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_graphics_contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "test_graphics_receipt: %s\n", message);
    exit(1);
  }
}

int main(void) {
  nxgl_graphics_contract contract;
  nxgl_graphics_evidence evidence;
  char line[2048];
  char copy[2048];
  char *save = NULL;
  char *token;
  unsigned fields = 0u;

  require(nxgl_graphics_contract_default(&contract) == 0,
          "contract initialization failed");
  contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
  require(nxgl_graphics_evidence_init(&evidence) == 0,
          "evidence initialization failed");
  (void)snprintf(evidence.run_id, sizeof(evidence.run_id), "otr-run-1");
  (void)snprintf(evidence.generation, sizeof(evidence.generation),
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  (void)snprintf(evidence.commit, sizeof(evidence.commit), "port-commit");
  (void)snprintf(evidence.cfw, sizeof(evidence.cfw), "test-cfw");
  (void)snprintf(evidence.egl_provider, sizeof(evidence.egl_provider),
                 "libEGL.so");
  (void)snprintf(evidence.gles_provider, sizeof(evidence.gles_provider),
                 "libGLESv2.so.2");
  (void)snprintf(evidence.dso_build_id, sizeof(evidence.dso_build_id),
                 "deadbeef");
  evidence.sdl_major = 2;
  evidence.obtained.api_version = NXGL_GRAPHICS_CONTRACT_API_VERSION;
  evidence.obtained.struct_size = sizeof(evidence.obtained);
  evidence.obtained.api = NXGL_GRAPHICS_API_GLES;
  evidence.obtained.profile = NXGL_GRAPHICS_PROFILE_ES;
  evidence.obtained.version_major = 3;
  evidence.obtained.version_minor = 2;
  evidence.drawable_w = 640;
  evidence.drawable_h = 480;
  evidence.shader_probe = NXGL_SHADER_PROBE_PASS;
  evidence.verdict = NXGL_GRAPHICS_OK;

  require(nxgl_graphics_contract_evidence_receipt(
              &contract, &evidence, line, sizeof(line)) != 0u,
          "canonical receipt formatting failed");
  require(strncmp(line, "GRAPHICS-EVIDENCE: ", 19u) == 0,
          "receipt prefix changed");
  (void)snprintf(copy, sizeof(copy), "%s", line + 19);
  for (token = strtok_r(copy, " ", &save); token != NULL;
       token = strtok_r(NULL, " ", &save)) {
    char *equals = strchr(token, '=');
    require(equals != NULL && equals != token && equals[1] != '\0' &&
                strchr(equals + 1, '=') == NULL,
            "receipt field is not exactly key=value");
    fields++;
  }
  require(fields == 14u, "receipt field count changed");
  require(strstr(line, "requested=gles/es/2.0/minimum") != NULL &&
              strstr(line, "obtained=gles/es/3.2") != NULL &&
              strstr(line, "drawable=640x480") != NULL &&
              strstr(line, "shader_probe=pass") != NULL &&
              strstr(line, "verdict=OK") != NULL,
          "OTR contract values are absent from the receipt");
  puts("test_graphics_receipt: PASS");
  return 0;
}
