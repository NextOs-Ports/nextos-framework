/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
  fprintf(stderr,
          "usage: %s --game-dir DIR [--port-id ID] [--portmaster-dir DIR] "
          "[--runtime-arch ARCH] [--root DIR] [--apply] [--json]\n",
          program);
}

static nxcompat_arch parse_arch(const char *value) {
  if (!value)
    return NXCOMPAT_ARCH_UNKNOWN;
  if (strcmp(value, "armv7") == 0 || strcmp(value, "armhf") == 0)
    return NXCOMPAT_ARCH_ARMV7;
  if (strcmp(value, "aarch64") == 0 || strcmp(value, "arm64") == 0)
    return NXCOMPAT_ARCH_AARCH64;
  if (strcmp(value, "i386") == 0 || strcmp(value, "x86") == 0)
    return NXCOMPAT_ARCH_I386;
  if (strcmp(value, "x86_64") == 0 || strcmp(value, "amd64") == 0)
    return NXCOMPAT_ARCH_X86_64;
  return NXCOMPAT_ARCH_UNKNOWN;
}

int main(int argc, char **argv) {
  nxcompat_probe_options probe;
  nxcompat_plan_options plan_options;
  nxcompat_host host;
  nxcompat_plan plan;
  char device_line[1024];
  char fix_line[1024];
  char json[8192];
  int apply = 0;
  int print_json = 0;
  int index;

  memset(&probe, 0, sizeof(probe));
  probe.api_version = NXCOMPAT_API_VERSION;
  probe.struct_size = sizeof(probe);
  probe.port_id = "unknown-port";
  memset(&plan_options, 0, sizeof(plan_options));
  plan_options.api_version = NXCOMPAT_API_VERSION;
  plan_options.struct_size = sizeof(plan_options);
  plan_options.runtime_arch = NXCOMPAT_ARCH_UNKNOWN;
  plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE;
  plan_options.low_memory_arena_max = 2;

  for (index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--apply") == 0)
      apply = 1;
    else if (strcmp(argv[index], "--json") == 0)
      print_json = 1;
    else if (index + 1 < argc && strcmp(argv[index], "--game-dir") == 0)
      probe.game_dir = argv[++index];
    else if (index + 1 < argc && strcmp(argv[index], "--port-id") == 0)
      probe.port_id = argv[++index];
    else if (index + 1 < argc && strcmp(argv[index], "--portmaster-dir") == 0)
      probe.portmaster_dir = argv[++index];
    else if (index + 1 < argc && strcmp(argv[index], "--runtime-arch") == 0) {
      plan_options.runtime_arch = parse_arch(argv[++index]);
      if (plan_options.runtime_arch == NXCOMPAT_ARCH_UNKNOWN) {
        fprintf(stderr, "nxcompat: unsupported runtime architecture: %s\n",
                argv[index]);
        return 64;
      }
    } else if (index + 1 < argc && strcmp(argv[index], "--root") == 0)
      probe.probe_root = argv[++index];
    else {
      usage(argv[0]);
      return 64;
    }
  }
  if (!probe.game_dir) {
    usage(argv[0]);
    return 64;
  }
  if (nxcompat_probe(&probe, &host) != 0 ||
      nxcompat_plan_environment(&host, &plan_options, &plan) != 0) {
    fprintf(stderr, "nxcompat: capability probe failed\n");
    return 1;
  }
  if (apply && nxcompat_apply_environment(&plan) != 0)
    fprintf(stderr, "nxcompat: one or more safe environment actions failed\n");
  if (nxcompat_format_device_line(&host, device_line, sizeof(device_line)) < 0 ||
      nxcompat_format_fix_line(&plan, fix_line, sizeof(fix_line)) < 0) {
    fprintf(stderr, "nxcompat: status report overflow\n");
    return 1;
  }
  if (print_json) {
    if (nxcompat_format_json(&host, &plan, json, sizeof(json)) < 0) {
      fprintf(stderr, "nxcompat: JSON report overflow\n");
      return 1;
    }
    printf("%s\n", json);
  } else
    printf("[nxcompat] %s\n[nxcompat] fixes: %s\n", device_line, fix_line);
  return 0;
}
