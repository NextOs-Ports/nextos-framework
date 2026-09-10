#define _GNU_SOURCE
#include "huntdown_paths.h"
#include "huntdown_build.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "so_util.h"
#include "util.h"

static void *(*g_il2cpp_string_new)(const char *);

static void *hd_streaming_assets_path(void) {
  const char *path = hd_game_dir();
  void *result = g_il2cpp_string_new ? g_il2cpp_string_new(path) : NULL;
  static int logged;
  if (!logged++)
    fprintf(stderr, "[HD-PATH] StreamingAssets -> %s\n", path);
  return result;
}

int hd_paths_install(uintptr_t il2cpp_base) {
  static const unsigned char signature_200023[16] = {
      0xfe,0x4f,0xbf,0xa9,0xf3,0x2e,0x00,0xf0,
      0x60,0x32,0x47,0xf9,0xa0,0x00,0x00,0xb5,
  };
  static const unsigned char signature_200036[16] = {
      0xff,0x43,0x01,0xd1,0xfe,0x57,0x03,0xa9,
      0xf4,0x4f,0x04,0xa9,0xd4,0x42,0x00,0xd0,
  };
  uintptr_t getter_rva = 0, string_new_rva = 0;
  const unsigned char *signature = NULL;
  if (hd_build_current() == HD_BUILD_200023) {
    getter_rva = 0x296EED8;
    string_new_rva = 0x11DFC00;
    signature = signature_200023;
  } else if (hd_build_current() == HD_BUILD_200036) {
    getter_rva = 0x343DFA0;
    string_new_rva = 0x17310C4;
    signature = signature_200036;
  }
  const uintptr_t getter = il2cpp_base + getter_rva;
  if (!il2cpp_base || !getter_rva ||
      memcmp((const void *)getter, signature, 16) != 0) {
    fprintf(stderr,
            "[HD-PATH] assinatura de Application.streamingAssetsPath divergente\n");
    return 0;
  }
  g_il2cpp_string_new =
      (void *(*)(const char *))(il2cpp_base + string_new_rva);
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) page_size = 4096;
  uintptr_t page = getter & ~((uintptr_t)page_size - 1);
  if (mprotect((void *)page, (size_t)page_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    return 0;
  hook_arm64(getter, (uintptr_t)hd_streaming_assets_path);
  __builtin___clear_cache((char *)page, (char *)page + page_size);
  mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_EXEC);
  fprintf(stderr,
          "[HD-PATH] getter StreamingAssets ligado a raiz BYO extraida\n");
  return 1;
}
