/* SPDX-License-Identifier: GPL-3.0-only */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "test_util_cache: %s\n", message);
    exit(1);
  }
}

int main(int argc, char **argv) {
  char first[2048], second[2048], after_reject[2048];
  const char *save;
  FILE *marker;

  require(argc == 2, "temporary directory argument missing");
  require(chdir(argv[1]) == 0, "cannot enter temporary directory");
  require(configure_shader_cache_identity("gles2-provider-a") == 0,
          "first safe graphics identity rejected");
  (void)snprintf(first, sizeof(first), "%s", resolve_android_path("sh_deadbeef"));
  require(strcmp(first,
                 "./userdata/shader-cache/gles2-provider-a/sh_deadbeef") == 0,
          "first shader cache was not namespaced");

  marker = fopen("userdata/save.marker", "wb");
  require(marker != NULL, "cannot create owner save marker");
  require(fputs("owner-data\n", marker) >= 0 && fclose(marker) == 0,
          "cannot persist owner save marker");

  require(configure_shader_cache_identity("gles2-provider-b") == 0,
          "second safe graphics identity rejected");
  (void)snprintf(second, sizeof(second), "%s", resolve_android_path("sh_deadbeef"));
  require(strcmp(second,
                 "./userdata/shader-cache/gles2-provider-b/sh_deadbeef") == 0,
          "second shader cache was not isolated");
  require(strcmp(first, second) != 0, "providers shared a shader-cache path");
  require(access("userdata/save.marker", F_OK) == 0,
          "provider rotation deleted owner data");

  save = resolve_android_path(
      "/data/data/com.dogbytegames.offtheroad/UserDefault.xml");
  require(strcmp(save, "./userdata/UserDefault.xml") == 0,
          "save/config path entered the disposable shader namespace");
  require(configure_shader_cache_identity("../escape") == -1,
          "unsafe graphics identity was accepted");
  (void)snprintf(after_reject, sizeof(after_reject), "%s",
                 resolve_android_path("sh_cafebabe"));
  require(strcmp(after_reject,
                 "./userdata/shader-cache/gles2-provider-b/sh_cafebabe") == 0,
          "rejected identity changed the active namespace");
  puts("test_util_cache: PASS");
  return 0;
}
