/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for nxgl_graphics_contract (V3 graphics context contract). */
#include "nxgl_graphics_contract.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void check(int cond, const char *msg) {
  if (!cond) {
    (void)fprintf(stderr, "nxgl_graphics_contract: %s\n", msg);
    exit(1);
  }
}

static nxgl_graphics_obtained obtained(nxgl_graphics_api api,
                                       nxgl_graphics_profile profile,
                                       int major, int minor) {
  nxgl_graphics_obtained o;
  o.api_version = NXGL_GRAPHICS_CONTRACT_API_VERSION;
  o.struct_size = sizeof(o);
  o.api = api;
  o.profile = profile;
  o.version_major = major;
  o.version_minor = minor;
  return o;
}

int main(void) {
  nxgl_graphics_contract c;
  nxgl_graphics_obtained o;
  char buf[256];

  check(nxgl_graphics_contract_default(&c) == 0, "default ok");
  check(c.api == NXGL_GRAPHICS_API_GLES && c.profile == NXGL_GRAPHICS_PROFILE_ES
        && c.version_major == 2 && c.version_minor == 0
        && c.version_policy == NXGL_GRAPHICS_POLICY_EXACT
        && c.shader_dialect == NXGL_SHADER_DIALECT_ESSL100,
        "default is gles/es/2.0/exact/essl100");
  check(nxgl_graphics_contract_default(NULL) == -1, "default null rejected");

  /* Happy path: GLES2 ES 2.0 exact, obtained the same. */
  o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 2, 0);
  check(nxgl_graphics_contract_validate(&c, &o) == NXGL_GRAPHICS_OK,
        "gles2 exact matches");

  /* THE Beach Buggy case: a GLES contract that received a desktop GL context
   * (GL_VERSION "3.1 Mesa") must fail FIRST, before any shader. */
  o = obtained(NXGL_GRAPHICS_API_GL, NXGL_GRAPHICS_PROFILE_COMPAT, 3, 1);
  check(nxgl_graphics_contract_validate(&c, &o) ==
            NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
        "desktop GL for a GLES contract is caught");

  /* Reverse: a desktop-GL contract that got GLES. */
  {
    nxgl_graphics_contract gl = c;
    gl.api = NXGL_GRAPHICS_API_GL;
    gl.profile = NXGL_GRAPHICS_PROFILE_CORE;
    gl.version_major = 3;
    gl.version_minor = 2;
    gl.version_max_major = 3;
    gl.version_max_minor = 2;
    gl.shader_dialect = NXGL_SHADER_DIALECT_GLSL_ANY; /* GL contract coherence */
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 2, 0);
    check(nxgl_graphics_contract_validate(&gl, &o) ==
              NXGL_GRAPHICS_GLES_FOR_GL_CONTRACT,
          "gles for a gl contract is caught");
  }

  /* An incoherent obtained tuple is malformed, not a profile mismatch. */
  o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_CORE, 2, 0);
  check(nxgl_graphics_contract_validate(&c, &o) ==
            NXGL_GRAPHICS_CONTRACT_INVALID,
        "incoherent obtained profile caught");
  {
    nxgl_graphics_contract gl = c;
    gl.api = NXGL_GRAPHICS_API_GL;
    gl.profile = NXGL_GRAPHICS_PROFILE_CORE;
    gl.version_major = 3;
    gl.version_minor = 2;
    gl.version_max_major = 3;
    gl.version_max_minor = 2;
    gl.shader_dialect = NXGL_SHADER_DIALECT_GLSL_ANY;
    o = obtained(NXGL_GRAPHICS_API_GL, NXGL_GRAPHICS_PROFILE_COMPAT, 3, 2);
    check(nxgl_graphics_contract_validate(&gl, &o) ==
              NXGL_GRAPHICS_PROFILE_MISMATCH,
          "valid desktop profile mismatch caught");
  }

  /* Version policies. */
  o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 3, 0);
  check(nxgl_graphics_contract_validate(&c, &o) ==
            NXGL_GRAPHICS_VERSION_NOT_EXACT,
        "exact policy rejects a higher version");
  {
    nxgl_graphics_contract mn = c;
    mn.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 3, 0);
    check(nxgl_graphics_contract_validate(&mn, &o) == NXGL_GRAPHICS_OK,
          "minimum policy accepts a higher version");
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 1, 1);
    check(nxgl_graphics_contract_validate(&mn, &o) ==
              NXGL_GRAPHICS_VERSION_BELOW_MINIMUM,
          "minimum policy rejects a lower version");
  }
  {
    nxgl_graphics_contract rg = c;
    rg.version_policy = NXGL_GRAPHICS_POLICY_RANGE;
    rg.version_major = 3; rg.version_minor = 0;
    rg.version_max_major = 3; rg.version_max_minor = 1;
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 3, 1);
    check(nxgl_graphics_contract_validate(&rg, &o) == NXGL_GRAPHICS_OK,
          "range accepts the upper bound");
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 3, 2);
    check(nxgl_graphics_contract_validate(&rg, &o) ==
              NXGL_GRAPHICS_VERSION_OUT_OF_RANGE,
          "range rejects above the upper bound");
  }

  /* Malformed contract / obtained. */
  {
    nxgl_graphics_contract bad = c;
    bad.struct_size = 3;
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 2, 0);
    check(nxgl_graphics_contract_validate(&bad, &o) ==
              NXGL_GRAPHICS_CONTRACT_INVALID,
          "malformed contract rejected");
    check(nxgl_graphics_contract_validate(&c, NULL) ==
              NXGL_GRAPHICS_CONTRACT_INVALID,
          "null obtained rejected");
  }

  /* Drawable: 1x1 is never proof of video. */
  check(nxgl_graphics_drawable_usable(1, 1) == 0, "1x1 not usable");
  check(nxgl_graphics_drawable_usable(640, 480) == 1, "640x480 usable");
  check(nxgl_graphics_drawable_usable(0, 480) == 0, "zero width not usable");
  check(nxgl_graphics_drawable_usable(2, 2) == 1, "2x2 usable");

  /* Shader dialect probe. */
  check(nxgl_shader_source_matches_dialect("#version 100\nvoid main(){}",
                                           NXGL_SHADER_DIALECT_ESSL100),
        "essl100 source matches");
  check(!nxgl_shader_source_matches_dialect("#version 300 es\nvoid main(){}",
                                            NXGL_SHADER_DIALECT_ESSL100),
        "essl300 source does not match essl100 contract");
  check(nxgl_shader_source_matches_dialect("#version 300 es\n",
                                           NXGL_SHADER_DIALECT_ESSL300),
        "essl300 source matches essl300");
  check(!nxgl_shader_source_matches_dialect("void main(){}",
                                            NXGL_SHADER_DIALECT_ESSL100),
        "missing #version fails an ESSL dialect");
  check(nxgl_shader_source_matches_dialect("anything",
                                           NXGL_SHADER_DIALECT_GLSL_ANY),
        "glsl-any never pins the dialect");
  /* whitespace tolerance */
  check(nxgl_shader_source_matches_dialect("#version   300   es \n",
                                           NXGL_SHADER_DIALECT_ESSL300),
        "essl300 with extra spaces matches");

  /* Receipt line for the Beach Buggy failure. */
  o = obtained(NXGL_GRAPHICS_API_GL, NXGL_GRAPHICS_PROFILE_COMPAT, 3, 1);
  check(nxgl_graphics_contract_receipt(
            &c, &o, 1, 1, NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
            buf, sizeof buf) > 0,
        "receipt formats");
  check(strcmp(buf,
               "GRAPHICS: requested=gles/es/2.0/exact obtained=gl/compat/3.1 "
               "drawable=1x1 shader=essl100 verdict=FAIL "
               "reason=desktop-gl-for-gles-contract") == 0,
        "receipt text is the machine-parsable verdict line");
  check(nxgl_graphics_contract_receipt(&c, &o, 1, 1,
                                       NXGL_GRAPHICS_OK, buf, 8) == 0 &&
            buf[0] == '\0',
        "short buffer -> empty");

  /* ---- Field fixtures (step 4): Beach Buggy 1/2 negative, FF4/FF4A positive.
   * Real device: an ES3-capable Wayland driver whose desktop-GL dispatch
   * answered "3.1 Mesa" for a port that asked for GLES2. FF4/FF4A are the
   * approved GLES2 ports that keep passing. ---- */
  {
    nxgl_graphics_contract gles2c;
    nxgl_graphics_contract_default(&gles2c);
    /* Beach Buggy Racing 1: obtained a desktop GL 3.1 context. */
    o = obtained(NXGL_GRAPHICS_API_GL, NXGL_GRAPHICS_PROFILE_COMPAT, 3, 1);
    check(nxgl_graphics_contract_validate(&gles2c, &o) ==
              NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
          "field: Beach Buggy 1 desktop-GL is rejected");
    /* Beach Buggy Racing 2: same desktop-GL context, same verdict. */
    check(nxgl_graphics_contract_validate(&gles2c, &o) ==
              NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT,
          "field: Beach Buggy 2 desktop-GL is rejected");
    /* FF4 / FF4A: a real GLES2 context with a usable drawable -> OK. */
    o = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 2, 0);
    check(nxgl_graphics_contract_validate(&gles2c, &o) == NXGL_GRAPHICS_OK &&
              nxgl_graphics_drawable_usable(1280, 720),
          "field: FF4/FF4A GLES2 context is accepted");
  }

  /* ---- Section-1 hardening: the validator rejects a malformed contract as
   * CONTRACT_INVALID (enums, api/profile & api/dialect coherence, RANGE max>=min,
   * bounded timeout) before it ever compares an obtained context. */
  {
    nxgl_graphics_obtained good =
        obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES, 2, 0);
    nxgl_graphics_contract bad;

    nxgl_graphics_contract_default(&bad);
    bad.profile = (nxgl_graphics_profile)99;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "out-of-range profile rejected");

    nxgl_graphics_contract_default(&bad);
    bad.version_policy = (nxgl_graphics_version_policy)42;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "out-of-range policy rejected");

    nxgl_graphics_contract_default(&bad);
    bad.shader_dialect = (nxgl_shader_dialect)7;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "out-of-range dialect rejected");

    nxgl_graphics_contract_default(&bad); /* GLES api but non-ES profile */
    bad.profile = NXGL_GRAPHICS_PROFILE_CORE;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "gles api + core profile rejected");

    nxgl_graphics_contract_default(&bad); /* GLES api but desktop dialect */
    bad.shader_dialect = NXGL_SHADER_DIALECT_GLSL_ANY;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "gles api + glsl_any rejected");

    nxgl_graphics_contract_default(&bad); /* GL api but ESSL dialect */
    bad.api = NXGL_GRAPHICS_API_GL;
    bad.profile = NXGL_GRAPHICS_PROFILE_COMPAT;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "gl api + essl dialect rejected");

    nxgl_graphics_contract_default(&bad); /* RANGE with max < min */
    bad.version_policy = NXGL_GRAPHICS_POLICY_RANGE;
    bad.version_major = 3;
    bad.version_minor = 0;
    bad.version_max_major = 2;
    bad.version_max_minor = 0;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "range max < min rejected");

    nxgl_graphics_contract_default(&bad); /* timeout out of bounds */
    bad.drawable_ready_timeout_ms = -1;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "negative timeout rejected");
    nxgl_graphics_contract_default(&bad);
    bad.drawable_ready_timeout_ms = 60001;
    check(nxgl_graphics_contract_validate(&bad, &good) ==
              NXGL_GRAPHICS_CONTRACT_INVALID, "over-max timeout rejected");
  }

  /* ---- item 4: shader probe sources carry the right #version + qualifier. */
  {
    char vs[256];
    char fs[256];
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL100,
                                   NXGL_SHADER_STAGE_VERTEX, vs, sizeof vs) > 0,
          "essl100 vs source");
    check(strstr(vs, "#version 100") == vs, "essl100 vs #version 100 first");
    check(strstr(vs, "attribute vec4") != NULL, "essl100 vs uses attribute");
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL100,
                                   NXGL_SHADER_STAGE_FRAGMENT, fs, sizeof fs) > 0,
          "essl100 fs source");
    check(strstr(fs, "precision mediump float") != NULL,
          "essl100 fs has precision");
    check(strstr(fs, "gl_FragColor") != NULL, "essl100 fs writes gl_FragColor");

    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL300,
                                   NXGL_SHADER_STAGE_VERTEX, vs, sizeof vs) > 0 &&
              strstr(vs, "#version 300 es") == vs && strstr(vs, "in vec4"),
          "essl300 vs #version 300 es + in");
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL300,
                                   NXGL_SHADER_STAGE_FRAGMENT, fs, sizeof fs) > 0 &&
              strstr(fs, "out vec4") != NULL,
          "essl300 fs uses out");
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL310,
                                   NXGL_SHADER_STAGE_VERTEX, vs, sizeof vs) > 0 &&
              strstr(vs, "#version 310 es") == vs,
          "essl310 vs #version 310 es");
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_GLSL_ANY,
                                   NXGL_SHADER_STAGE_VERTEX, vs, sizeof vs) > 0 &&
              strstr(vs, "#version 120") == vs,
          "glsl_any vs #version 120");
    /* Each generated source agrees with the dialect matcher (self-consistent). */
    check(nxgl_shader_source_matches_dialect(vs, NXGL_SHADER_DIALECT_GLSL_ANY),
          "generated glsl_any source matches its dialect");
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL100,
                                   NXGL_SHADER_STAGE_VERTEX, vs, 4) == 0 &&
              vs[0] == '\0',
          "short buffer -> empty");
    check(nxgl_shader_probe_source(NXGL_SHADER_DIALECT_ESSL100,
                                   NXGL_SHADER_STAGE_VERTEX, NULL, 16) == 0,
          "null buffer -> 0");

    {
      nxgl_graphics_contract core = c;
      core.api = NXGL_GRAPHICS_API_GL;
      core.profile = NXGL_GRAPHICS_PROFILE_CORE;
      core.version_major = 3;
      core.version_minor = 2;
      core.version_max_major = 3;
      core.version_max_minor = 2;
      core.shader_dialect = NXGL_SHADER_DIALECT_GLSL_ANY;
      check(nxgl_shader_probe_source_for_contract(
                &core, NXGL_SHADER_STAGE_VERTEX, vs, sizeof vs) > 0 &&
                strstr(vs, "#version 150 core") == vs &&
                strstr(vs, "attribute") == NULL,
            "desktop core probe uses GLSL 150 core without removed syntax");
      check(nxgl_shader_probe_source_for_contract(
                &core, NXGL_SHADER_STAGE_FRAGMENT, fs, sizeof fs) > 0 &&
                strstr(fs, "out vec4") != NULL &&
                strstr(fs, "gl_FragColor") == NULL,
            "desktop core fragment probe uses an explicit output");
    }
  }

  /* ---- item 4: probe result names are stable. */
  check(strcmp(nxgl_shader_probe_result_name(NXGL_SHADER_PROBE_PASS),
               "pass") == 0, "probe pass name");
  check(strcmp(nxgl_shader_probe_result_name(NXGL_SHADER_PROBE_COMPILE_FAILED),
               "compile-failed") == 0, "probe compile-failed name");
  check(strcmp(nxgl_shader_probe_result_name(NXGL_SHADER_PROBE_LINK_FAILED),
               "link-failed") == 0, "probe link-failed name");
  check(strcmp(nxgl_shader_probe_result_name(NXGL_SHADER_PROBE_SKIPPED),
               "skipped") == 0, "probe skipped name");

  /* ---- item 4: structured evidence receipt. */
  {
    nxgl_graphics_evidence ev;
    char big[512];
    check(nxgl_graphics_evidence_init(&ev) == 0, "evidence init");
    /* Empty provenance prints as '-', probe SKIPPED, verdict not-OK. */
    check(nxgl_graphics_contract_evidence_receipt(&c, &ev, big, sizeof big) > 0,
          "evidence receipt formats empty");
    check(strstr(big, "GRAPHICS-EVIDENCE:") == big, "evidence prefix");
    check(strstr(big, "run_id=-") != NULL, "empty run_id -> dash");
    check(strstr(big, "shader_probe=skipped") != NULL, "probe skipped in line");
    check(strstr(big, "sdl=0") != NULL, "unknown sdl -> 0");

    /* A full, passing evidence line. */
    (void)snprintf(ev.run_id, sizeof ev.run_id, "porttest-1700000000-77-3");
    (void)snprintf(ev.generation, sizeof ev.generation,
                   "0123456789abcdef0123456789abcdef01234567");
    (void)snprintf(ev.commit, sizeof ev.commit, "cfe2ad9");
    (void)snprintf(ev.cfw, sizeof ev.cfw, "darkos");
    (void)snprintf(ev.egl_provider, sizeof ev.egl_provider,
                   "/usr/lib/libmali.so");
    (void)snprintf(ev.gles_provider, sizeof ev.gles_provider,
                   "/usr/lib/libmali.so");
    (void)snprintf(ev.dso_build_id, sizeof ev.dso_build_id, "deadbeef1234");
    ev.sdl_major = 2;
    ev.obtained = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES,
                           3, 2);
    ev.drawable_w = 640;
    ev.drawable_h = 480;
    ev.verdict = NXGL_GRAPHICS_OK;
    ev.shader_probe = NXGL_SHADER_PROBE_PASS;
    check(nxgl_graphics_contract_evidence_receipt(&c, &ev, big, sizeof big) > 0,
          "evidence receipt formats full");
    check(strstr(big, "run_id=porttest-1700000000-77-3") != NULL, "run_id");
    check(strstr(big, "sdl=2") != NULL, "sdl major 2");
    check(strstr(big, "obtained=gles/es/3.2") != NULL, "obtained gles 3.2");
    check(strstr(big, "drawable=640x480") != NULL, "drawable");
    check(strstr(big, "shader_probe=pass") != NULL, "probe pass");
    check(strstr(big, "verdict=OK") != NULL, "verdict ok");
    /* Short buffer / null args fail closed. */
    check(nxgl_graphics_contract_evidence_receipt(&c, &ev, big, 8) == 0 &&
              big[0] == '\0',
          "evidence short buffer -> empty");
    check(nxgl_graphics_contract_evidence_receipt(NULL, &ev, big,
                                                  sizeof big) == 0,
          "evidence null contract -> 0");
  }

  /* ---- Section 1: the versioned JSON evidence document. */
  {
    nxgl_graphics_evidence ev;
    nxgl_graphics_contract json_contract = c;
    char json[1024];
    json_contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    nxgl_graphics_evidence_init(&ev);
    (void)snprintf(ev.run_id, sizeof ev.run_id, "porttest-1700000000-77-9");
    (void)snprintf(ev.generation, sizeof ev.generation,
                   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    (void)snprintf(ev.commit, sizeof ev.commit, "98ebced");
    (void)snprintf(ev.cfw, sizeof ev.cfw, "darkos");
    (void)snprintf(ev.device, sizeof ev.device, "mali-g31");
    (void)snprintf(ev.port_id, sizeof ev.port_id, "merchantskies");
    (void)snprintf(ev.port_version, sizeof ev.port_version, "2.0");
    (void)snprintf(ev.artifact_sha256, sizeof ev.artifact_sha256, "%s",
                   "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    (void)snprintf(ev.egl_provider, sizeof ev.egl_provider, "/usr/lib/libEGL.so");
    (void)snprintf(ev.egl_build_id, sizeof ev.egl_build_id, "aaaa1111");
    (void)snprintf(ev.gles_provider, sizeof ev.gles_provider,
                   "/usr/lib/\"weird\"/libmali.so"); /* forces JSON escaping */
    (void)snprintf(ev.dso_build_id, sizeof ev.dso_build_id, "bbbb2222");
    (void)snprintf(ev.renderer, sizeof ev.renderer, "Mali-G31");
    (void)snprintf(ev.gl_version_str, sizeof ev.gl_version_str,
                   "OpenGL ES 3.2 v1.r26p0");
    (void)snprintf(ev.glsl_version, sizeof ev.glsl_version,
                   "OpenGL ES GLSL ES 3.20");
    (void)snprintf(ev.egl_version, sizeof ev.egl_version, "1.4");
    ev.sdl_major = 2;
    ev.obtained = obtained(NXGL_GRAPHICS_API_GLES, NXGL_GRAPHICS_PROFILE_ES,
                           3, 2);
    ev.drawable_w = 640;
    ev.drawable_h = 480;
    ev.verdict = NXGL_GRAPHICS_OK;
    ev.shader_probe = NXGL_SHADER_PROBE_PASS;

    check(nxgl_graphics_contract_evidence_json(&json_contract, &ev, json,
                                               sizeof json) > 0,
          "json formats");
    check(json[0] == '{', "json starts with object");
    check(strstr(json, "\"schema\":\"nx-graphics-evidence\"") != NULL,
          "json schema");
    check(strstr(json, "\"schema_version\":2") != NULL, "json schema version");
    check(strstr(json, "\"run_id\":\"porttest-1700000000-77-9\"") != NULL,
          "json run_id");
    check(strstr(json, "\"artifact_sha256\":\"deadbeef") != NULL,
          "json artifact hash");
    check(strstr(json, "\"port\":{\"id\":\"merchantskies\"") != NULL,
          "json port block");
    check(strstr(json, "\"renderer\":\"Mali-G31\"") != NULL, "json renderer");
    check(strstr(json, "\"glsl\":\"OpenGL ES GLSL ES 3.20\"") != NULL,
          "json glsl");
    check(strstr(json, "\"gles_build_id\":\"bbbb2222\"") != NULL,
          "json separate gles build id");
    check(strstr(json, "\"egl_build_id\":\"aaaa1111\"") != NULL,
          "json separate egl build id");
    check(strstr(json, "\"obtained\":{\"api\":\"gles\",\"profile\":\"es\","
                       "\"version\":\"3.2\"}") != NULL, "json obtained");
    check(strstr(json, "\"version_max\":null") != NULL,
          "non-range request has a null upper bound");
    check(strstr(json, "\"drawable\":{\"w\":640,\"h\":480}") != NULL,
          "json drawable");
    check(strstr(json, "\"verdict\":\"OK\"") != NULL, "json verdict");
    /* The embedded quote in the gles provider path must be escaped. */
    check(strstr(json, "\\\"weird\\\"") != NULL, "json escapes embedded quote");
    /* Short buffer / null fail closed. */
    check(nxgl_graphics_contract_evidence_json(&json_contract, &ev, json, 8) == 0 &&
              json[0] == '\0', "json short buffer -> empty");
    check(nxgl_graphics_contract_evidence_json(NULL, &ev, json,
                                               sizeof json) == 0,
          "json null contract -> 0");
  }

  (void)puts("nxgl_graphics_contract tests: ok "
             "(validate/drawable/shader-dialect/receipt/field-fixtures/"
             "probe-source/evidence/json)");
  return 0;
}
