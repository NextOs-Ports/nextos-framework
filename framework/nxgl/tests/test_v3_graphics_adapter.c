/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for nxgl_graphics_contract_adapter with a fake SDL/GL. */
#include "nxgl_graphics_contract_adapter.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void check(int cond, const char *msg) {
  if (!cond) {
    (void)fprintf(stderr, "nxgl_graphics_adapter: %s\n", msg);
    exit(1);
  }
}

/* Fake driver state driven by the scenario. */
static const char *g_gl_version;
static int g_profile_mask;
static int g_have_attr;
static int g_draw_w;
static int g_draw_h;

static const unsigned char *fake_glGetString(unsigned name) {
  (void)name;
  return (const unsigned char *)g_gl_version;
}
static int fake_SDL_GL_GetAttribute(int attr, int *value) {
  if (!g_have_attr) {
    return -1;
  }
  if (attr == 21 /* PROFILE_MASK */) {
    *value = g_profile_mask;
    return 0;
  }
  *value = 0;
  return 0;
}
static void fake_SDL_GL_GetDrawableSize(void *win, int *w, int *h) {
  (void)win;
  *w = g_draw_w;
  *h = g_draw_h;
}

/* Fake GL shader pipeline. g_compile_ok / g_link_ok drive the outcome; the
 * probe must return PASS only when both are true. */
static int g_shader_funcs_present = 1;
static int g_compile_ok = 1;
static int g_link_ok = 1;
static unsigned g_next_id = 1;
static int g_deleted_shaders;
static int g_deleted_programs;

static unsigned fake_glCreateShader(unsigned type) { (void)type; return g_next_id++; }
static void fake_glShaderSource(unsigned s, int n, const char *const *src,
                                const int *len) {
  (void)s; (void)n; (void)src; (void)len;
}
static void fake_glCompileShader(unsigned s) { (void)s; }
static void fake_glGetShaderiv(unsigned s, unsigned pname, int *out) {
  (void)s; (void)pname; *out = g_compile_ok ? 1 : 0;
}
static unsigned fake_glCreateProgram(void) { return g_next_id++; }
static void fake_glAttachShader(unsigned p, unsigned s) { (void)p; (void)s; }
static void fake_glLinkProgram(unsigned p) { (void)p; }
static void fake_glGetProgramiv(unsigned p, unsigned pname, int *out) {
  (void)p; (void)pname; *out = g_link_ok ? 1 : 0;
}
static void fake_glDeleteShader(unsigned s) { (void)s; g_deleted_shaders++; }
static void fake_glDeleteProgram(unsigned p) { (void)p; g_deleted_programs++; }

/* SDL3 mode: the runtime exposes SDL_GetWindowSizeInPixels (SDL3) and NOT
 * SDL_GL_GetDrawableSize (SDL2). The adapter must resolve the drawable through
 * the SDL3 name and report sdl_major == 3. */
static int g_sdl3_mode = 0;
static int g_sdl3_size_ok = 1;

static _Bool fake_SDL_GetWindowSizeInPixels(void *win, int *w, int *h) {
  fake_SDL_GL_GetDrawableSize(win, w, h);
  return g_sdl3_size_ok != 0;
}

static void *resolver(const char *name) {
  if (g_gl_version != NULL && strcmp(name, "glGetString") == 0) {
    return (void *)fake_glGetString;
  }
  if (strcmp(name, "SDL_GL_GetAttribute") == 0) {
    return (void *)fake_SDL_GL_GetAttribute;
  }
  if (!g_sdl3_mode && strcmp(name, "SDL_GL_GetDrawableSize") == 0) {
    return (void *)fake_SDL_GL_GetDrawableSize;
  }
  if (g_sdl3_mode && strcmp(name, "SDL_GetWindowSizeInPixels") == 0) {
    return (void *)fake_SDL_GetWindowSizeInPixels;
  }
  if (g_shader_funcs_present) {
    if (strcmp(name, "glCreateShader") == 0) return (void *)fake_glCreateShader;
    if (strcmp(name, "glShaderSource") == 0) return (void *)fake_glShaderSource;
    if (strcmp(name, "glCompileShader") == 0) return (void *)fake_glCompileShader;
    if (strcmp(name, "glGetShaderiv") == 0) return (void *)fake_glGetShaderiv;
    if (strcmp(name, "glCreateProgram") == 0) return (void *)fake_glCreateProgram;
    if (strcmp(name, "glAttachShader") == 0) return (void *)fake_glAttachShader;
    if (strcmp(name, "glLinkProgram") == 0) return (void *)fake_glLinkProgram;
    if (strcmp(name, "glGetProgramiv") == 0) return (void *)fake_glGetProgramiv;
    if (strcmp(name, "glDeleteShader") == 0) return (void *)fake_glDeleteShader;
    if (strcmp(name, "glDeleteProgram") == 0) return (void *)fake_glDeleteProgram;
  }
  return NULL; /* SDL_GL_GetCurrentWindow etc.: absent is fine */
}

static nxgl_graphics_contract gles2(void) {
  nxgl_graphics_contract c;
  nxgl_graphics_contract_default(&c); /* gles/es/2.0/exact/essl100 */
  return c;
}

static nxgl_graphics_reason verify_full(const nxgl_graphics_contract *contract,
                                        int *width, int *height,
                                        char *receipt, size_t receipt_cap) {
  nxgl_graphics_evidence evidence;
  nxgl_graphics_reason reason = nxgl_graphics_contract_adapter_evidence(
      contract, &evidence, receipt, receipt_cap);
  if (width != NULL) {
    *width = evidence.drawable_w;
  }
  if (height != NULL) {
    *height = evidence.drawable_h;
  }
  return reason;
}

int main(void) {
  nxgl_graphics_contract c = gles2();
  nxgl_graphics_obtained o;
  nxgl_graphics_reason reason;
  char rcpt[4096];
  int w = -1, h = -1;

  nxgl_graphics_contract_adapter_set_resolver(resolver);
  c.drawable_ready_timeout_ms = 30;

  /* 1. A real GLES2 driver: usable drawable -> OK. */
  g_gl_version = "OpenGL ES 2.0 fake-driver";
  g_have_attr = 1;
  g_profile_mask = 0x0004; /* ES */
  g_draw_w = 640; g_draw_h = 480;
  check(nxgl_graphics_contract_adapter_measure(&o) == 0, "measure gles2");
  check(o.api == NXGL_GRAPHICS_API_GLES && o.version_major == 2 &&
        o.version_minor == 0 && o.profile == NXGL_GRAPHICS_PROFILE_ES,
        "measured gles 2.0");
  reason = verify_full(&c, &w, &h, rcpt, sizeof rcpt);
  check(reason == NXGL_GRAPHICS_OK, "gles2 usable drawable -> OK");
  check(w == 640 && h == 480, "drawable recorded");
  check(strstr(rcpt, "\"verdict\":\"OK\"") != NULL, "receipt says OK");

  /* 2. THE Beach Buggy case: glGetString reports a DESKTOP context even though
   * the port asked for ES. Must fail as desktop-gl-for-gles-contract, BEFORE
   * any shader, regardless of the drawable. */
  g_gl_version = "3.1 Mesa 26.1.2";
  g_have_attr = 1;
  g_profile_mask = 0x0002; /* compat */
  g_draw_w = 640; g_draw_h = 480;
  check(nxgl_graphics_contract_adapter_measure(&o) == 0, "measure desktop");
  check(o.api == NXGL_GRAPHICS_API_GL && o.version_major == 3 &&
        o.version_minor == 1, "measured desktop gl 3.1");
  reason = verify_full(&c, &w, &h, rcpt, sizeof rcpt);
  check(reason == NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
        "desktop GL for a GLES contract is caught at runtime");
  check(strstr(rcpt, "\"obtained\":{\"api\":\"gl\",\"profile\":\"compat\","
                     "\"version\":\"3.1\"}") != NULL &&
        strstr(rcpt, "\"reason\":\"desktop-gl-for-gles-contract\"") != NULL,
        "receipt names the real desktop context");

  /* 3. Correct GLES2 context but the drawable is still the 1x1 placeholder. */
  g_gl_version = "OpenGL ES 2.0 fake-driver";
  g_draw_w = 1; g_draw_h = 1;
  reason = verify_full(&c, &w, &h, rcpt, sizeof rcpt);
  check(reason == NXGL_GRAPHICS_DRAWABLE_STUCK_1X1, "1x1 drawable is rejected");
  check(strstr(rcpt, "\"drawable\":{\"w\":1,\"h\":1}") != NULL,
        "receipt shows the 1x1 drawable");

  /* 4. No measurable context (glGetString unresolvable) -> nominal only. */
  g_gl_version = NULL;
  reason = verify_full(&c, &w, &h, rcpt, sizeof rcpt);
  check(reason == NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY,
        "an unmeasurable context is provider-nominal-only");

  /* 5. A genuine GLES3 port under a MINIMUM policy accepts ES 3.2. */
  {
    nxgl_graphics_contract mn = gles2();
    mn.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    mn.shader_dialect = NXGL_SHADER_DIALECT_ESSL300;
    g_gl_version = "OpenGL ES 3.2 v1.r13p0"; g_draw_w = 800; g_draw_h = 600;
    mn.drawable_ready_timeout_ms = 30;
    reason = verify_full(&mn, &w, &h, rcpt, sizeof rcpt);
    check(reason == NXGL_GRAPHICS_OK, "gles3 minimum policy accepts ES 3.2");
  }

  /* 6. SDL major is detected by which drawable symbol resolves (SDL2 here). */
  check(nxgl_graphics_contract_adapter_sdl_major() == 2,
        "SDL_GL_GetDrawableSize present -> SDL2");

  /* 7. Real shader probe: PASS only when compile AND link succeed. */
  g_gl_version = "OpenGL ES 2.0 fake-driver";
  g_shader_funcs_present = 1;
  g_compile_ok = 1; g_link_ok = 1;
  check(nxgl_graphics_contract_adapter_shader_probe(&c) == NXGL_SHADER_PROBE_PASS,
        "compile+link ok -> probe pass");
  g_compile_ok = 0; g_link_ok = 1;
  g_deleted_shaders = 0; g_deleted_programs = 0;
  check(nxgl_graphics_contract_adapter_shader_probe(&c) ==
            NXGL_SHADER_PROBE_COMPILE_FAILED,
        "compile fail -> probe compile-failed");
  check(g_deleted_shaders == 2 && g_deleted_programs == 0,
        "compile failure deletes both independently-created shaders");
  g_compile_ok = 1; g_link_ok = 0;
  g_deleted_shaders = 0; g_deleted_programs = 0;
  check(nxgl_graphics_contract_adapter_shader_probe(&c) ==
            NXGL_SHADER_PROBE_LINK_FAILED,
        "link fail -> probe link-failed");
  check(g_deleted_shaders == 2 && g_deleted_programs == 1,
        "link failure deletes both shaders and the program");
  g_shader_funcs_present = 0;
  check(nxgl_graphics_contract_adapter_shader_probe(&c) ==
            NXGL_SHADER_PROBE_SKIPPED,
        "no GL entry points -> probe skipped (never a pass)");
  g_shader_funcs_present = 1;

  /* 8. drawable_wait: a usable size returns 0 immediately; a stuck 1x1 times
   * out on the real monotonic clock (short timeout keeps the test fast). */
  {
    int dw = 0, dh = 0;
    g_draw_w = 640; g_draw_h = 480;
    check(nxgl_graphics_contract_adapter_drawable_wait(&dw, &dh, 50) == 0 &&
              dw == 640 && dh == 480,
          "usable drawable returns immediately");
    g_draw_w = 1; g_draw_h = 1;
    check(nxgl_graphics_contract_adapter_drawable_wait(&dw, &dh, 30) == -1 &&
              dw == 1 && dh == 1,
          "stuck 1x1 drawable times out");
  }

  /* 9. Full evidence pass: GLES2 context, usable drawable, compile+link ok,
   * provenance from the environment. Structured GRAPHICS-EVIDENCE line. */
  {
    nxgl_graphics_evidence ev;
    char big[4096];
    nxgl_graphics_reason r;
    setenv("NXOBS_RUN_ID", "porttest-1700000000-77-9", 1);
    setenv("NX_CFW", "darkos", 1);
    setenv("NX_GENERATION",
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
           1);
    setenv("NX_ARTIFACT_SHA256",
           "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
           1);
    g_gl_version = "OpenGL ES 2.0 fake-driver";
    g_draw_w = 1280; g_draw_h = 720;
    g_compile_ok = 1; g_link_ok = 1;
    r = nxgl_graphics_contract_adapter_evidence(&c, &ev, big, sizeof big);
    check(r == NXGL_GRAPHICS_OK, "evidence pass -> OK");
    check(ev.shader_probe == NXGL_SHADER_PROBE_PASS, "evidence probe pass");
    check(ev.drawable_w == 1280 && ev.drawable_h == 720, "evidence drawable");
    check(ev.sdl_major == 2, "evidence sdl major 2");
    check(big[0] == '{', "evidence receipt is JSON");
    check(strstr(big, "\"run_id\":\"porttest-1700000000-77-9\"") != NULL,
          "evidence carries run_id from env");
    check(strstr(big, "\"cfw\":\"darkos\"") != NULL,
          "evidence carries cfw from env");
    check(strstr(big, "\"shader_probe\":\"pass\"") != NULL,
          "evidence probe pass document");
    check(strstr(big, "\"verdict\":\"OK\"") != NULL,
          "evidence verdict OK");

    /* A GLES context that matches but whose shader will not link is a
     * SHADER_PROBE_FAILED verdict, not a silent OK. */
    g_link_ok = 0;
    r = nxgl_graphics_contract_adapter_evidence(&c, &ev, big, sizeof big);
    check(r == NXGL_GRAPHICS_SHADER_PROBE_FAILED,
          "matching context + failing shader -> shader-probe-failed");
    check(strstr(big, "\"reason\":\"shader-probe-failed\"") != NULL,
          "evidence names the shader failure");
    g_link_ok = 1;
    unsetenv("NXOBS_RUN_ID"); unsetenv("NX_CFW"); unsetenv("NX_GENERATION");
    unsetenv("NX_ARTIFACT_SHA256");
  }

  /* 10. SDL3-only runtime: SDL_GL_GetDrawableSize is ABSENT and the drawable
   * comes from SDL_GetWindowSizeInPixels; sdl_major must be 3 and the evidence
   * still resolves a usable drawable. */
  {
    nxgl_graphics_evidence ev;
    char big[4096];
    nxgl_graphics_reason r;
    g_sdl3_mode = 1;
    g_gl_version = "OpenGL ES 2.0 fake-driver";
    g_draw_w = 1920; g_draw_h = 1080;
    g_compile_ok = 1; g_link_ok = 1; g_shader_funcs_present = 1;
    check(nxgl_graphics_contract_adapter_sdl_major() == 3,
          "SDL3-only runtime -> sdl_major 3");
    {
      int dw = 0, dh = 0;
      check(nxgl_graphics_contract_adapter_drawable(&dw, &dh) == 0 &&
                dw == 1920 && dh == 1080,
            "SDL3 drawable read via SDL_GetWindowSizeInPixels");
    }
    r = nxgl_graphics_contract_adapter_evidence(&c, &ev, big, sizeof big);
    check(r == NXGL_GRAPHICS_OK && ev.sdl_major == 3,
          "SDL3-only evidence pass with sdl_major 3");
    check(strstr(big, "\"sdl_major\":3") != NULL,
          "evidence document shows sdl_major 3");
    g_sdl3_size_ok = 0;
    check(nxgl_graphics_contract_adapter_drawable(NULL, NULL) == -1,
          "SDL3 false return is a drawable failure");
    g_sdl3_size_ok = 1;
    g_sdl3_mode = 0;
  }

  /* 11. A matched context whose GL entry points cannot be resolved: the probe
   * is SKIPPED, and SKIPPED is NOT a success -- the verdict must be
   * shader-probe-failed, never OK. */
  {
    nxgl_graphics_evidence ev;
    char big[4096];
    nxgl_graphics_reason r;
    g_gl_version = "OpenGL ES 2.0 fake-driver";
    g_draw_w = 1280; g_draw_h = 720;
    g_shader_funcs_present = 0; /* context exists but no shader entry points */
    r = nxgl_graphics_contract_adapter_evidence(&c, &ev, big, sizeof big);
    check(ev.shader_probe == NXGL_SHADER_PROBE_SKIPPED, "probe skipped");
    check(r == NXGL_GRAPHICS_SHADER_PROBE_FAILED,
          "SKIPPED probe on a matched context is NOT a success");
    check(strstr(big, "\"verdict\":\"FAIL\"") != NULL,
          "evidence verdict FAIL");
    g_shader_funcs_present = 1;
  }

  /* 12. ROCKNIX / Vector Unit field fixtures. These names are intentional:
   * they are the host mirror required before BBR1/BBR2 may opt into nxgl.
   * BBR1 declares GLES2 exact + ESSL100. */
  {
    nxgl_graphics_contract bbr1 = gles2();
    bbr1.drawable_ready_timeout_ms = 30;
    g_shader_funcs_present = 1;
    g_compile_ok = 1; g_link_ok = 1;
    g_gl_version = "OpenGL ES 2.0 ROCKNIX Vector Unit fixture";
    g_draw_w = 1280; g_draw_h = 720;
    check(verify_full(&bbr1, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_OK,
          "ROCKNIX/Vector Unit BBR1: real GLES2 context accepted");
    g_gl_version = "3.1 Mesa ROCKNIX Vector Unit fixture";
    g_profile_mask = 0x0002;
    check(verify_full(&bbr1, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
          "ROCKNIX/Vector Unit BBR1: desktop GL rejected");
    g_gl_version = "OpenGL ES 2.0 ROCKNIX Vector Unit fixture";
    g_draw_w = 1; g_draw_h = 1;
    check(verify_full(&bbr1, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_DRAWABLE_STUCK_1X1,
          "ROCKNIX/Vector Unit BBR1: 1x1 rejected");
    g_draw_w = 1280; g_draw_h = 720; g_link_ok = 0;
    check(verify_full(&bbr1, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_SHADER_PROBE_FAILED,
          "ROCKNIX/Vector Unit BBR1: shader failure rejected");
    g_link_ok = 1;
  }

  /* BBR2 declares GLES3 profile ES, minimum 3.0, ESSL300. */
  {
    nxgl_graphics_contract bbr2 = gles2();
    bbr2.version_major = 3;
    bbr2.version_minor = 0;
    bbr2.version_max_major = 3;
    bbr2.version_max_minor = 0;
    bbr2.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    bbr2.shader_dialect = NXGL_SHADER_DIALECT_ESSL300;
    bbr2.drawable_ready_timeout_ms = 30;
    g_shader_funcs_present = 1;
    g_compile_ok = 1; g_link_ok = 1;
    g_gl_version = "OpenGL ES 3.2 ROCKNIX Vector Unit fixture";
    g_draw_w = 1280; g_draw_h = 720;
    check(verify_full(&bbr2, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_OK,
          "ROCKNIX/Vector Unit BBR2: real GLES3 context accepted");
    g_gl_version = "4.6 Mesa ROCKNIX Vector Unit fixture";
    g_profile_mask = 0x0002;
    check(verify_full(&bbr2, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
          "ROCKNIX/Vector Unit BBR2: desktop GL rejected");
    g_gl_version = "OpenGL ES 3.2 ROCKNIX Vector Unit fixture";
    g_draw_w = 1; g_draw_h = 1;
    check(verify_full(&bbr2, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_DRAWABLE_STUCK_1X1,
          "ROCKNIX/Vector Unit BBR2: 1x1 rejected");
    g_draw_w = 1280; g_draw_h = 720; g_compile_ok = 0;
    check(verify_full(&bbr2, &w, &h, rcpt, sizeof rcpt) ==
              NXGL_GRAPHICS_SHADER_PROBE_FAILED,
          "ROCKNIX/Vector Unit BBR2: shader failure rejected");
    g_compile_ok = 1;
  }

  (void)puts("nxgl_graphics_contract_adapter tests: ok "
             "(measure/full-evidence/desktop-gl/1x1/nominal/"
             "sdl-major/shader-probe/drawable-wait/evidence/sdl3/skipped-fail/"
             "rocknix-vector-unit-bbr1-bbr2)");
  return 0;
}
