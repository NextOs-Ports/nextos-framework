/* SPDX-License-Identifier: GPL-3.0-only */
/* Pure EGLConfig requirement matching and receipt. See nxgl_config_request.h
 * for the adapter-only rule: no attribute here ever becomes a global default. */
#include "nxgl_config_request.h"

#include <stdio.h>
#include <string.h>

static int request_valid(const nxgl_config_request *request) {
  return request != NULL &&
         request->api_version == NXGL_CONFIG_REQUEST_API_VERSION &&
         request->struct_size == sizeof(*request);
}

static int observed_valid(const nxgl_config_observed *observed) {
  return observed != NULL &&
         observed->api_version == NXGL_CONFIG_REQUEST_API_VERSION &&
         observed->struct_size == sizeof(*observed);
}

nxgl_config_request nxgl_config_request_default(void) {
  nxgl_config_request request;

  memset(&request, 0, sizeof(request));
  request.api_version = NXGL_CONFIG_REQUEST_API_VERSION;
  request.struct_size = sizeof(request);
  request.red = NXGL_CONFIG_DONT_CARE;
  request.green = NXGL_CONFIG_DONT_CARE;
  request.blue = NXGL_CONFIG_DONT_CARE;
  request.alpha = NXGL_CONFIG_DONT_CARE;
  request.depth = NXGL_CONFIG_DONT_CARE;
  request.stencil = NXGL_CONFIG_DONT_CARE;
  request.samples = NXGL_CONFIG_DONT_CARE;
  request.require_native_visual = 0;
  return request;
}

nxgl_config_observed nxgl_config_observed_init(void) {
  nxgl_config_observed observed;

  memset(&observed, 0, sizeof(observed));
  observed.api_version = NXGL_CONFIG_REQUEST_API_VERSION;
  observed.struct_size = sizeof(observed);
  return observed;
}

/* EGL "at least" semantics per size attribute. */
static int attribute_satisfied(int requested, int observed) {
  if (requested <= NXGL_CONFIG_DONT_CARE) {
    return 1;
  }
  return observed >= requested;
}

/* Name of the first violated attribute, or NULL when satisfied. */
static const char *first_violation(const nxgl_config_request *request,
                                   const nxgl_config_observed *observed) {
  if (!attribute_satisfied(request->red, observed->red)) {
    return "red";
  }
  if (!attribute_satisfied(request->green, observed->green)) {
    return "green";
  }
  if (!attribute_satisfied(request->blue, observed->blue)) {
    return "blue";
  }
  if (!attribute_satisfied(request->alpha, observed->alpha)) {
    return "alpha";
  }
  if (!attribute_satisfied(request->depth, observed->depth)) {
    return "depth";
  }
  if (!attribute_satisfied(request->stencil, observed->stencil)) {
    return "stencil";
  }
  if (!attribute_satisfied(request->samples, observed->samples)) {
    return "samples";
  }
  if (request->require_native_visual != 0 && observed->native_visual_id == 0) {
    return "native-visual";
  }
  return NULL;
}

int nxgl_config_satisfies(const nxgl_config_request *request,
                          const nxgl_config_observed *observed) {
  if (!request_valid(request) || !observed_valid(observed)) {
    return -1;
  }
  return first_violation(request, observed) == NULL ? 1 : 0;
}

/* "*" for don't care, the number otherwise. */
static void format_attr(char *out, size_t capacity, int value) {
  if (value <= NXGL_CONFIG_DONT_CARE) {
    snprintf(out, capacity, "*");
  } else {
    snprintf(out, capacity, "%d", value);
  }
}

int nxgl_format_config_receipt(const nxgl_config_request *request,
                               const nxgl_config_observed *observed,
                               char *buffer, size_t capacity) {
  char red[12], green[12], blue[12], alpha[12], depth[12], stencil[12];
  char samples[12];
  const char *violation;
  int written;

  if (!request_valid(request) || !observed_valid(observed) ||
      buffer == NULL || capacity == 0) {
    return -1;
  }

  violation = first_violation(request, observed);

  format_attr(red, sizeof(red), request->red);
  format_attr(green, sizeof(green), request->green);
  format_attr(blue, sizeof(blue), request->blue);
  format_attr(alpha, sizeof(alpha), request->alpha);
  format_attr(depth, sizeof(depth), request->depth);
  format_attr(stencil, sizeof(stencil), request->stencil);
  format_attr(samples, sizeof(samples), request->samples);

  written = snprintf(
      buffer, capacity,
      "EGLCONFIG: requested=r%sg%sb%sa%s-d%s-s%s-m%s-nv%d "
      "observed=r%dg%db%da%d-d%d-s%d-m%d-nv0x%X verdict=%s%s",
      red, green, blue, alpha, depth, stencil, samples,
      request->require_native_visual != 0 ? 1 : 0,
      observed->red, observed->green, observed->blue, observed->alpha,
      observed->depth, observed->stencil, observed->samples,
      (unsigned)(observed->native_visual_id < 0 ? 0
                                                : observed->native_visual_id),
      violation == NULL ? "satisfied" : "violated:",
      violation == NULL ? "" : violation);
  return written;
}
