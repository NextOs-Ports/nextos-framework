#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "cpuinfo_compat.h"

static void test_aarch64_aliases(void) {
  char buffer[512] =
      "processor\t: 0\n"
      "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
      "CPU architecture: 8\n";
  size_t length = strlen(buffer);

  assert(ts_cpuinfo_add_armv7_aliases(buffer, &length, sizeof(buffer) - 1u) ==
         1);
  buffer[length] = '\0';
  assert(strstr(buffer, "vfp vfpv3 vfpv3d16 neon") != NULL);
  assert(strstr(buffer, "CPU architecture: 8") != NULL);
}

static void test_native_armv7_is_untouched(void) {
  char buffer[256] =
      "Processor\t: ARMv7 Processor rev 5 (v7l)\n"
      "Features\t: swp half thumb fastmult vfp edsp neon vfpv3\n"
      "CPU architecture: 7\n";
  char original[sizeof(buffer)];
  size_t length = strlen(buffer);
  size_t original_length = length;

  memcpy(original, buffer, sizeof(buffer));
  assert(ts_cpuinfo_add_armv7_aliases(buffer, &length, sizeof(buffer)) == 0);
  assert(length == original_length);
  assert(memcmp(buffer, original, sizeof(buffer)) == 0);
}

static void test_full_read_discards_only_the_tail(void) {
  char buffer[256] =
      "processor: 0\n"
      "Features: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n"
      "CPU architecture: 8\n"
      "processor: 1\n"
      "Features: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n"
      "CPU architecture: 8\n"
      "padding-padding-padding-padding-padding-padding-padding-padding";
  size_t capacity = strlen(buffer);
  size_t length = capacity;

  assert(capacity < sizeof(buffer));
  assert(ts_cpuinfo_add_armv7_aliases(buffer, &length, capacity) == 1);
  assert(length == capacity);
  buffer[length] = '\0';
  assert(strstr(buffer, "vfp vfpv3 vfpv3d16 neon") != NULL);
  assert(strstr(buffer, "CPU architecture: 8") != NULL);
}

static void test_unproven_features_are_not_invented(void) {
  char buffer[128] =
      "Features: aes pmull sha1 sha2 crc32\n"
      "CPU architecture: 8\n";
  size_t length = strlen(buffer);

  assert(ts_cpuinfo_add_armv7_aliases(buffer, &length, sizeof(buffer)) == 0);
  assert(strstr(buffer, "vfp") == NULL);
  assert(strstr(buffer, "neon") == NULL);
}

int main(void) {
  test_aarch64_aliases();
  test_native_armv7_is_untouched();
  test_full_read_discards_only_the_tail();
  test_unproven_features_are_not_invented();
  return 0;
}
