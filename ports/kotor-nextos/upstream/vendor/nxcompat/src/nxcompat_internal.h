/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXCOMPAT_INTERNAL_H
#define NXCOMPAT_INTERNAL_H

#include "nxcompat.h"

#include <stddef.h>

typedef struct nxcompat_context {
  char root[NXCOMPAT_PATH_MAX];
} nxcompat_context;

/* One non-blocking process-global arbiter protects getenv/setenv and runtime
 * ownership transitions shared by probe, plan/apply and backend negotiation. */
nxcompat_result_code nxcompat_global_arbiter_try_acquire(void);
void nxcompat_global_arbiter_release(void);
int nxcompat_api_version_supported(uint32_t api_version);

void nxcompat_copy_string(char *destination, size_t destination_size,
                          const char *source);
int nxcompat_join_path(const char *directory, const char *suffix, char *output,
                       size_t output_size);
int nxcompat_join_root(const nxcompat_context *context, const char *device_path,
                       char *host_path, size_t host_path_size);
int nxcompat_path_exists(const nxcompat_context *context,
                         const char *device_path);
int nxcompat_directory_exists(const nxcompat_context *context,
                              const char *device_path);
int nxcompat_directory_usable(const nxcompat_context *context,
                              const char *device_path);
int nxcompat_socket_exists(const nxcompat_context *context,
                           const char *device_path);
int nxcompat_socket_connectable(const nxcompat_context *context,
                                const char *device_path);
int nxcompat_regular_file_exists(const nxcompat_context *context,
                                 const char *device_path);
int nxcompat_read_first_line(const nxcompat_context *context,
                             const char *device_path, char *output,
                             size_t output_size);
int nxcompat_parse_display_mode(const char *text, int *width, int *height);
int nxcompat_host_instance_valid(const nxcompat_host *host);
int nxcompat_plan_instance_valid(const nxcompat_plan *plan);

#endif
