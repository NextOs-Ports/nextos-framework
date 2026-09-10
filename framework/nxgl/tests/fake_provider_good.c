/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdlib.h>

static unsigned int initialize_calls;
static unsigned int terminate_calls;

void *eglGetDisplay(void *native_display) {
  (void)native_display;
  return (void *)0x1;
}

unsigned int eglInitialize(void *display, int *major, int *minor) {
  (void)display;
  ++initialize_calls;
  if (getenv("NXGL_TEST_EGL_INITIALIZE_FAIL"))
    return 0u;
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  return 1u;
}

unsigned int eglTerminate(void *display) {
  (void)display;
  ++terminate_calls;
  if (getenv("NXGL_TEST_EGL_TERMINATE_FAIL"))
    return 0u;
  return 1u;
}

unsigned int nxglTestGetInitializeCalls(void) { return initialize_calls; }

unsigned int nxglTestGetTerminateCalls(void) { return terminate_calls; }

unsigned int glCreateShader(unsigned int type) { return type + 1u; }

const unsigned char *glGetString(unsigned int name) {
  (void)name;
  return (const unsigned char *)"nxgl-test-provider";
}
