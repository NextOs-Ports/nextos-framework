#define _GNU_SOURCE
#include "huntdown_build.h"

#include <stdio.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct HdBuildContract {
  HdBuild id;
  long long unity_bytes;
  long long il2cpp_bytes;
  long long main_bytes;
  const char *version_name;
  int version_code;
  const char *unity_version;
  const char *label;
} HdBuildContract;

static const HdBuildContract g_contracts[] = {
    {HD_BUILD_200023, 16349176, 49557792, 6728, "0.1.23", 200023,
     "2022.3.47f1", "playstore-200023"},
    {HD_BUILD_200036, 19734416, 63452984, 6696, "0.1", 200036,
     "6000.2.6f2", "playstore-200036"},
};

static const HdBuildContract *g_build;

static long long hd_file_bytes(const char *game_dir, const char *name) {
  char path[PATH_MAX];
  int n = snprintf(path, sizeof path, "%s/%s", game_dir, name);
  if (n <= 0 || n >= (int)sizeof path) return -1;
  FILE *file = fopen(path, "rb");
  if (!file) return -1;
  long long result = -1;
  if (fseeko(file, 0, SEEK_END) == 0) result = (long long)ftello(file);
  fclose(file);
  return result;
}

int hd_build_detect(const char *game_dir) {
  if (!game_dir || !*game_dir) return 0;
  long long unity = hd_file_bytes(game_dir, "libunity.so");
  long long il2cpp = hd_file_bytes(game_dir, "libil2cpp.so");
  long long main = hd_file_bytes(game_dir, "libmain.so");
  for (size_t i = 0; i < sizeof g_contracts / sizeof g_contracts[0]; ++i) {
    const HdBuildContract *candidate = &g_contracts[i];
    if (candidate->unity_bytes == unity && candidate->il2cpp_bytes == il2cpp &&
        candidate->main_bytes == main) {
      g_build = candidate;
      fprintf(stderr,
              "[HD-BUILD] perfil %s: Huntdown %s (%d), Unity %s\n",
              candidate->label, candidate->version_name,
              candidate->version_code, candidate->unity_version);
      return 1;
    }
  }
  fprintf(stderr,
          "[HD-BUILD] conjunto de engine não reconhecido: unity=%lld "
          "il2cpp=%lld main=%lld\n",
          unity, il2cpp, main);
  return 0;
}

HdBuild hd_build_current(void) {
  return g_build ? g_build->id : HD_BUILD_UNKNOWN;
}

int hd_build_is_unity6(void) {
  return hd_build_current() == HD_BUILD_200036;
}

const char *hd_build_version_name(void) {
  return g_build ? g_build->version_name : "unknown";
}

int hd_build_version_code(void) {
  return g_build ? g_build->version_code : 0;
}

const char *hd_build_unity_version(void) {
  return g_build ? g_build->unity_version : "unknown";
}

const char *hd_build_label(void) {
  return g_build ? g_build->label : "unknown";
}
