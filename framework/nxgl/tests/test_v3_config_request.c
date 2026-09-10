/* SPDX-License-Identifier: GPL-3.0-only */
/* V3-GRAPHICS-01: EGLConfig requirement API. The first check is the one the
 * header swears by: the DEFAULT request is all-don't-care -- RGBA8888 is the
 * Huntdown adapter's declaration and must never leak into the default. */
#include "nxgl_config_request.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      g_failures++;                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                    \
  } while (0)

int main(void) {
  char line[256];

  /* 1. STATIC GUARANTEE: the default request is all-don't-care. */
  {
    nxgl_config_request request = nxgl_config_request_default();

    CHECK(request.api_version == NXGL_CONFIG_REQUEST_API_VERSION);
    CHECK(request.struct_size == sizeof(request));
    CHECK(request.red == NXGL_CONFIG_DONT_CARE);
    CHECK(request.green == NXGL_CONFIG_DONT_CARE);
    CHECK(request.blue == NXGL_CONFIG_DONT_CARE);
    CHECK(request.alpha == NXGL_CONFIG_DONT_CARE);
    CHECK(request.depth == NXGL_CONFIG_DONT_CARE);
    CHECK(request.stencil == NXGL_CONFIG_DONT_CARE);
    CHECK(request.samples == NXGL_CONFIG_DONT_CARE);
    CHECK(request.require_native_visual == 0);
  }

  /* 2. The default request is satisfied by ANY observation, including the
   * poorest config a firmware can hand out. */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    observed.red = 5;
    observed.green = 6;
    observed.blue = 5;
    observed.alpha = 0;
    observed.depth = 0;
    observed.stencil = 0;
    observed.samples = 0;
    observed.native_visual_id = 0;
    CHECK(nxgl_config_satisfies(&request, &observed) == 1);
  }

  /* 3. Adapter-declared RGBA8888 (the Huntdown case): violated by 565,
   * satisfied by 8888, satisfied by MORE than requested (EGL at-least). */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    request.red = 8;
    request.green = 8;
    request.blue = 8;
    request.alpha = 8;

    observed.red = 5;
    observed.green = 6;
    observed.blue = 5;
    observed.alpha = 0;
    CHECK(nxgl_config_satisfies(&request, &observed) == 0);

    observed.red = 8;
    observed.green = 8;
    observed.blue = 8;
    observed.alpha = 8;
    CHECK(nxgl_config_satisfies(&request, &observed) == 1);

    observed.red = 10;
    observed.green = 10;
    observed.blue = 10;
    observed.alpha = 8;
    CHECK(nxgl_config_satisfies(&request, &observed) == 1);
  }

  /* 4. depth / stencil / samples honor the same at-least rule. */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    request.depth = 24;
    request.stencil = 8;
    request.samples = 4;
    observed.depth = 24;
    observed.stencil = 8;
    observed.samples = 4;
    CHECK(nxgl_config_satisfies(&request, &observed) == 1);
    observed.samples = 0;
    CHECK(nxgl_config_satisfies(&request, &observed) == 0);
  }

  /* 5. require_native_visual: violated by visual 0 only. */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    request.require_native_visual = 1;
    observed.native_visual_id = 0;
    CHECK(nxgl_config_satisfies(&request, &observed) == 0);
    observed.native_visual_id = 0x21;
    CHECK(nxgl_config_satisfies(&request, &observed) == 1);
  }

  /* 6. Malformed calls fail closed with -1. */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();
    nxgl_config_request bad = request;

    CHECK(nxgl_config_satisfies(NULL, &observed) == -1);
    CHECK(nxgl_config_satisfies(&request, NULL) == -1);
    bad.api_version = 99;
    CHECK(nxgl_config_satisfies(&bad, &observed) == -1);
    bad = request;
    bad.struct_size = 1;
    CHECK(nxgl_config_satisfies(&bad, &observed) == -1);
    CHECK(nxgl_format_config_receipt(NULL, &observed, line,
                                     sizeof(line)) == -1);
    CHECK(nxgl_format_config_receipt(&request, &observed, NULL, 8) == -1);
  }

  /* 7. Receipt line: satisfied RGBA8888. */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    request.red = 8;
    request.green = 8;
    request.blue = 8;
    request.alpha = 8;
    observed.red = 8;
    observed.green = 8;
    observed.blue = 8;
    observed.alpha = 8;
    observed.depth = 24;
    observed.stencil = 8;
    observed.native_visual_id = 0x21;
    CHECK(nxgl_format_config_receipt(&request, &observed, line,
                                     sizeof(line)) > 0);
    CHECK(strcmp(line,
                 "EGLCONFIG: requested=r8g8b8a8-d*-s*-m*-nv0 "
                 "observed=r8g8b8a8-d24-s8-m0-nv0x21 verdict=satisfied") == 0);
  }

  /* 8. Receipt line: violation names the FIRST failing attribute. */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    request.alpha = 8;
    observed.red = 5;
    observed.green = 6;
    observed.blue = 5;
    observed.alpha = 0;
    CHECK(nxgl_format_config_receipt(&request, &observed, line,
                                     sizeof(line)) > 0);
    CHECK(strcmp(line,
                 "EGLCONFIG: requested=r*g*b*a8-d*-s*-m*-nv0 "
                 "observed=r5g6b5a0-d0-s0-m0-nv0x0 "
                 "verdict=violated:alpha") == 0);
  }

  /* 9. Receipt line for the DEFAULT request against anything: satisfied,
   * every requested attribute a "*". */
  {
    nxgl_config_request request = nxgl_config_request_default();
    nxgl_config_observed observed = nxgl_config_observed_init();

    CHECK(nxgl_format_config_receipt(&request, &observed, line,
                                     sizeof(line)) > 0);
    CHECK(strstr(line, "requested=r*g*b*a*-d*-s*-m*-nv0") != NULL);
    CHECK(strstr(line, "verdict=satisfied") != NULL);
  }

  if (g_failures != 0) {
    fprintf(stderr, "nxgl_v3_config_request=FAIL failures=%d\n", g_failures);
    return 1;
  }
  printf("nxgl_v3_config_request=PASS cases=9\n");
  return 0;
}
