#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jni_bridge.h"

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    exit(1);
  }
}

int main(void) {
  void *long_lived = asm2_jni_string("long-lived");
  require(long_lived != NULL, "create long-lived JNI string");

  const asm2_jint value = 127;
  for (unsigned int index = 0; index < 50000; ++index)
    require(asm2_jni_int_array(&value, 1) != NULL,
            "create churn JNI array");

  require(strcmp(asm2_jni_string_chars(long_lived), "long-lived") == 0,
          "lookup long-lived JNI handle after churn");
  require(asm2_jni_string("long-lived") == long_lived,
          "interned string remains stable after array churn");
  struct asm2_jni_stats stats;
  asm2_jni_get_stats(&stats);
  fprintf(stderr, "JNI registry handles=%llu arrays=%llu bucket_max=%u\n",
          (unsigned long long)stats.handles,
          (unsigned long long)stats.arrays, stats.longest_handle_bucket);
  require(stats.handles >= 50001 && stats.arrays == 50000,
          "JNI registry counters");
  require(stats.longest_handle_bucket < 128,
          "pointer hash keeps JNI lookup buckets bounded");
  require(stats.longest_intern_bucket < 16,
          "array churn does not lengthen content-intern lookup");

  puts("JNI handle registry: 50,001 handles and hashed pointer lookup OK");
  return 0;
}
