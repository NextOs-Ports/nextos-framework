#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define LOG_NAME "debug.log"

static char g_shader_cache_identity[96];

int configure_shader_cache_identity(const char *identity) {
  char directory[256];
  size_t i, length;
  if (identity == NULL || identity[0] == '\0') return -1;
  length = strlen(identity);
  if (length >= sizeof(g_shader_cache_identity)) return -1;
  for (i = 0u; i < length; i++) {
    unsigned char c = (unsigned char)identity[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return -1;
  }
  if (mkdir("./userdata", 0755) != 0 && access("./userdata", F_OK) != 0)
    return -1;
  if (mkdir("./userdata/shader-cache", 0755) != 0 &&
      access("./userdata/shader-cache", F_OK) != 0)
    return -1;
  if (snprintf(directory, sizeof(directory), "./userdata/shader-cache/%s",
               identity) >= (int)sizeof(directory))
    return -1;
  if (mkdir(directory, 0755) != 0 && access(directory, F_OK) != 0)
    return -1;
  (void)snprintf(g_shader_cache_identity, sizeof(g_shader_cache_identity),
                 "%s", identity);
  return 0;
}

int debugPrintf(const char *text, ...) {
  va_list list;

  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);

  return 0;
}

uintptr_t read_tls_stack_guard(void) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    return *(uintptr_t *)(tls + 0x28);
#endif
  return 0;
}

const char *resolve_android_path(const char *path) {
  static _Thread_local char alt_path[2048];
  static _Thread_local char asset_path[2048];

  static const char *app_data_prefix = "/data/data/com.dogbytegames.offtheroad/";
  static const char *app_data_prefix_2 = "/data/user/0/com.dogbytegames.offtheroad/";
  static const char *sdcard_prefix =
      "/storage/emulated/0/Android/data/com.dogbytegames.offtheroad/";

  if (!path || path[0] == '\0')
    return path;

  /* Cache binario de shader do bgfx: o nome `sh_<hash>` identifica a entrada
   * do jogo, mas nao o renderer/provider que produziu o binario. Reutilizar o
   * mesmo userdata/ entre Mali-450/G31, providers ou updates pode alimentar ao
   * bgfx um cache incompatível e deixar a engine viva apresentando RGB=0. O
   * cache e descartavel; saves/config continuam fora deste namespace. */
  if (strncmp(path, "sh_", 3) == 0 && !strchr(path, '/')) {
    if (g_shader_cache_identity[0] != '\0')
      snprintf(alt_path, sizeof(alt_path),
               "./userdata/shader-cache/%s/%s", g_shader_cache_identity,
               path);
    else
      snprintf(alt_path, sizeof(alt_path),
               "./userdata/shader-cache/unconfigured/%s", path);
    return alt_path;
  }

  /* Dados privados do app (UserDefault.xml, saves) vivem em ./userdata/,
   * que e o diretorio gravavel do port (o launcher garante que existe). */
  if (strncmp(path, app_data_prefix, strlen(app_data_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./userdata/%s",
             path + strlen(app_data_prefix));
    return alt_path;
  }
  if (strncmp(path, app_data_prefix_2, strlen(app_data_prefix_2)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./userdata/%s",
             path + strlen(app_data_prefix_2));
    return alt_path;
  }
  if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
    return alt_path;
  }

  if (access(path, F_OK) == 0)
    return path;

  if (path[0] == '/') {
    if (snprintf(alt_path, sizeof(alt_path), ".%s", path) <
            (int)sizeof(alt_path) &&
        access(alt_path, F_OK) == 0) {
      return alt_path;
    }
  }

  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (basename[0] != '\0') {
    if (snprintf(asset_path, sizeof(asset_path), "./assets/%s", basename) <
            (int)sizeof(asset_path) &&
        access(asset_path, F_OK) == 0) {
      return asset_path;
    }
  }

  return path;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
