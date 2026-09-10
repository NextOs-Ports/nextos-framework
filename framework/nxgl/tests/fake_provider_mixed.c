/* SPDX-License-Identifier: GPL-3.0-only */
extern unsigned int glCreateShader(unsigned int type);

void *eglGetDisplay(void *native_display) {
  (void)native_display;
  return (void *)0x1;
}

unsigned int eglInitialize(void *display, int *major, int *minor) {
  (void)display;
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  return 1u;
}

unsigned int eglTerminate(void *display) {
  (void)display;
  return 1u;
}

/* Keep the dependency in DT_NEEDED. dlsym(handle, "glCreateShader") then
 * succeeds, but the common helper must reject it because another DSO owns it. */
unsigned int nxgl_test_force_dependency(void) { return glCreateShader(1u); }
