#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bionic_compat.h"

void debugPrintf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s (errno=%d)\n", message, errno);
    exit(1);
  }
}

int main(void) {
  char path[] = "/tmp/asm2-file-registry-XXXXXX";
  int descriptor = mkstemp(path);
  require(descriptor >= 0, "mkstemp");
  require(write(descriptor, "ASM2", 4) == 4, "seed file");
  require(close(descriptor) == 0, "close seed descriptor");

  asm2_bionic_init(".");
  void *long_lived = asm2_fopen(path, "rb");
  require(long_lived != NULL, "open long-lived stream");

  void *first_stale = NULL;
  for (unsigned int index = 0; index < 50000; ++index) {
    void *stream = asm2_fopen(path, "rb");
    require(stream != NULL, "open churn stream");
    if (!first_stale)
      first_stale = stream;
    char byte = 0;
    require(asm2_fread(&byte, 1, 1, stream) == 1 && byte == 'A',
            "read churn stream");
    require(asm2_fclose(stream) == 0, "close churn stream");
  }

  char text[5] = {0};
  require(asm2_fseek(long_lived, 0, SEEK_SET) == 0,
          "seek long-lived stream after churn");
  require(asm2_fread(text, 1, 4, long_lived) == 4 &&
              strcmp(text, "ASM2") == 0,
          "read long-lived stream after churn");

  errno = 0;
  require(asm2_fclose(first_stale) == EOF && errno == EBADF,
          "reject stale stream without scanning retired history");

  struct asm2_bionic_file_stats stats;
  asm2_bionic_get_file_stats(&stats);
  require(stats.created == 50001 && stats.open == 1 &&
              stats.closed == 50000,
          "registry counters before final close");
  require(stats.longest_active_bucket <= 1,
          "only the long-lived stream remains active");
  require(asm2_fclose(long_lived) == 0, "close long-lived stream");
  asm2_bionic_get_file_stats(&stats);
  require(stats.open == 0 && stats.closed == 50001,
          "registry counters after final close");

  require(unlink(path) == 0, "remove temporary file");
  puts("bionic FILE registry: 50,001 opens, stale rejection and O(1) active lookup OK");
  return 0;
}
