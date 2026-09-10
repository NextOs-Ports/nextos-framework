/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "crash_diag.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *message) {
  (void)fprintf(stderr, "crash diagnostic test: %s\n", message);
  return 1;
}

int main(void) {
  int output[2];
  char buffer[1024];
  ssize_t length;
  int status = 0;

  if (pipe(output) != 0)
    return fail("pipe failed");
  pid_t child = fork();
  if (child < 0)
    return fail("fork failed");
  if (child == 0) {
    (void)close(output[0]);
    if (dup2(output[1], STDERR_FILENO) < 0)
      _exit(2);
    (void)close(output[1]);
    crash_diag_install((uintptr_t)0x1000u, 0x1000u);
    crash_diag_set_phase(CRASH_PHASE_DRAW);
    crash_diag_set_frame(7);
    (void)raise(SIGSEGV);
    _exit(3);
  }

  (void)close(output[1]);
  length = read(output[0], buffer, sizeof(buffer) - 1u);
  (void)close(output[0]);
  if (length < 0)
    return fail("read failed");
  buffer[length] = '\0';
  if (waitpid(child, &status, 0) != child)
    return fail("waitpid failed");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 128 + SIGSEGV)
    return fail("handler did not preserve 128+signal status");
  if (!strstr(buffer, "Swordigo crash sig=11") ||
      !strstr(buffer, "phase=draw") || !strstr(buffer, "frame=7") ||
      !strstr(buffer, "fault="))
    return fail("bounded report lacks required fields");

  (void)fprintf(stdout, "Swordigo crash diagnostic tests passed\n");
  return 0;
}
