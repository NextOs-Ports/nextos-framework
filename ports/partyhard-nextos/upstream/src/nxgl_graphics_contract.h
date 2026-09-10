/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_graphics_contract -- the declared graphics context contract (V3),
 * adapter-owned, validated by the framework, never decided by device name.
 *
 * WHY THIS EXISTS
 * ---------------
 * Beach Buggy Racing 1/2 (field case) on an ES3-capable driver completed
 * extract+splash, found SDL_main, created "a context", reported provider
 * liveness=ok -- and then got
 * GL_VERSION="3.1 Mesa" (a DESKTOP GL context) with a 1x1 drawable and a shader
 * "syntax error", crashing (SIGSEGV / return 255). liveness=ok only proves the
 * functions exist and glGetString answers; it does NOT prove the API is GLES,
 * the ESSL dialect the port needs, a usable drawable or a compilable shader.
 *
 * A port DECLARES what it needs (api/profile/version/policy/shader dialect);
 * the framework MEASURES what was obtained and refuses a divergent context
 * BEFORE the engine feeds shaders. The decision is by measured capability,
 * never by the GPU/CFW/game name. Everything here is pure: no EGL, no GL, no
 * I/O; the adapter measures with SDL/EGL/glGetString and feeds the numbers in.
 */
#ifndef NXGL_GRAPHICS_CONTRACT_H
#define NXGL_GRAPHICS_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_GRAPHICS_CONTRACT_API_VERSION 2u

typedef enum nxgl_graphics_api {
  NXGL_GRAPHICS_API_GLES = 0, /* SDL_GL_CONTEXT_PROFILE_ES */
  NXGL_GRAPHICS_API_GL        /* desktop OpenGL (core or compat) */
} nxgl_graphics_api;

typedef enum nxgl_graphics_profile {
  NXGL_GRAPHICS_PROFILE_ES = 0,
  NXGL_GRAPHICS_PROFILE_CORE,
  NXGL_GRAPHICS_PROFILE_COMPAT
} nxgl_graphics_profile;

typedef enum nxgl_graphics_version_policy {
  NXGL_GRAPHICS_POLICY_EXACT = 0, /* obtained == requested */
  NXGL_GRAPHICS_POLICY_MINIMUM,   /* obtained >= requested */
  NXGL_GRAPHICS_POLICY_RANGE      /* requested <= obtained <= max */
} nxgl_graphics_version_policy;

typedef enum nxgl_shader_dialect {
  NXGL_SHADER_DIALECT_ESSL100 = 0, /* #version 100 */
  NXGL_SHADER_DIALECT_ESSL300,     /* #version 300 es */
  NXGL_SHADER_DIALECT_ESSL310,     /* #version 310 es */
  NXGL_SHADER_DIALECT_GLSL_ANY     /* desktop GLSL: dialect not pinned */
} nxgl_shader_dialect;

/* What the adapter DECLARES it requires. Fixed storage, no pointers. */
typedef struct nxgl_graphics_contract {
  uint32_t api_version; /* NXGL_GRAPHICS_CONTRACT_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_graphics_contract) */
  nxgl_graphics_api api;
  nxgl_graphics_profile profile;
  int version_major;
  int version_minor;
  nxgl_graphics_version_policy version_policy;
  int version_max_major; /* only for RANGE; ignored otherwise */
  int version_max_minor;
  nxgl_shader_dialect shader_dialect;
  int drawable_ready_timeout_ms; /* how long a 1x1 drawable may persist */
} nxgl_graphics_contract;

/* What was actually MEASURED after context creation (SDL_GL_GetAttribute +
 * glGetString + EGL). The adapter fills this; nothing here queries GL. */
typedef struct nxgl_graphics_obtained {
  uint32_t api_version;
  size_t struct_size;
  nxgl_graphics_api api;
  nxgl_graphics_profile profile;
  int version_major;
  int version_minor;
} nxgl_graphics_obtained;

/* Every way a context can betray the contract, one stable code per cause. */
typedef enum nxgl_graphics_reason {
  NXGL_GRAPHICS_OK = 0,
  NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT, /* NXG asked ES, got desktop GL */
  NXGL_GRAPHICS_GLES_FOR_GL_CONTRACT,
  NXGL_GRAPHICS_PROFILE_MISMATCH,
  NXGL_GRAPHICS_VERSION_NOT_EXACT,
  NXGL_GRAPHICS_VERSION_BELOW_MINIMUM,
  NXGL_GRAPHICS_VERSION_OUT_OF_RANGE,
  NXGL_GRAPHICS_DRAWABLE_STUCK_1X1,       /* still 1x1 at the deadline */
  NXGL_GRAPHICS_SHADER_PROBE_FAILED,      /* declared dialect will not compile */
  NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY,    /* symbols exist, no live renderer */
  NXGL_GRAPHICS_EVIDENCE_INCOMPLETE,      /* live proof lacks release binding */
  NXGL_GRAPHICS_RECEIPT_REJECTED,         /* evidence could not be persisted */
  NXGL_GRAPHICS_CONTRACT_INVALID,         /* the declaration itself is malformed */
  /* V4-GRAPHICS-04 additive codes (post-first-present gate). Appended so every
   * pre-existing value keeps its exact number; the old one-shot API never
   * produces them. */
  NXGL_GRAPHICS_DRAWABLE_UNREADABLE,      /* size symbol absent / read failed / 0x0 */
  NXGL_GRAPHICS_DRAWABLE_ABSURD,          /* dimension beyond any sane panel */
  NXGL_GRAPHICS_GATE_IDENTITY_CHANGED,    /* window/context/provider changed between phases */
  NXGL_GRAPHICS_GATE_MISUSE,              /* phase order violated / gate uninitialized */
  NXGL_GRAPHICS_CLOCK_UNAVAILABLE         /* monotonic clock could not bound the deadline */
} nxgl_graphics_reason;

/* Initialise a contract to a defined default: GLES 2.0, profile ES, EXACT,
 * ESSL100, 5000 ms. Returns 0, or -1 on NULL. A default is a starting point,
 * not a global policy: the adapter overrides what it measured it needs. */
int nxgl_graphics_contract_default(nxgl_graphics_contract *contract);

/* Structural validation without touching an obtained context. Adapters call
 * this before reading a timeout or making any SDL/GL call, so a malformed
 * declaration cannot turn into an unbounded/effectful probe. */
int nxgl_graphics_contract_is_valid(const nxgl_graphics_contract *contract);

/* Structural validation for measured data. This includes api_version,
 * struct_size, bounded enums, API/profile coherence and non-negative version. */
int nxgl_graphics_obtained_is_valid(const nxgl_graphics_obtained *obtained);

/* The core decision (pure): does the obtained context satisfy the contract?
 * Checks, in order: contract well-formed; API (a GLES contract that got desktop
 * GL fails FIRST, before any shader); profile; version under the policy.
 * Returns the first violated reason, or NXGL_GRAPHICS_OK. */
nxgl_graphics_reason nxgl_graphics_contract_validate(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_obtained *obtained);

/* A drawable is usable only when it has left the 1x1 placeholder AND both
 * dimensions are positive. 1x1 is NEVER proof of video. Returns 1 usable. */
int nxgl_graphics_drawable_usable(int width, int height);

/* The #version line a shader of `dialect` must carry, e.g. "#version 100" or
 * "#version 300 es". Returns "" for GLSL_ANY (desktop dialect not pinned). */
const char *nxgl_shader_dialect_version_line(nxgl_shader_dialect dialect);

/* Does `source`'s first non-empty, non-comment `#version` line match the
 * declared dialect? A shader probe that disagrees fails the contract before
 * the engine's real shaders run. Returns 1 on match (or GLSL_ANY), 0 otherwise.
 * Pure string check; never compiles. */
int nxgl_shader_source_matches_dialect(const char *source,
                                       nxgl_shader_dialect dialect);

/* The outcome of the REAL shader probe the adapter runs (compile + link of a
 * minimal shader in the declared dialect against the LIVE context). SKIPPED
 * means the adapter could not resolve the GL entry points (e.g. host build with
 * no context) -- it is NOT a pass, and a release proof must show PASS. */
typedef enum nxgl_shader_probe_result {
  NXGL_SHADER_PROBE_SKIPPED = 0,
  NXGL_SHADER_PROBE_PASS,
  NXGL_SHADER_PROBE_COMPILE_FAILED,
  NXGL_SHADER_PROBE_LINK_FAILED
} nxgl_shader_probe_result;

typedef enum nxgl_shader_stage {
  NXGL_SHADER_STAGE_VERTEX = 0,
  NXGL_SHADER_STAGE_FRAGMENT
} nxgl_shader_stage;

/* Emit a MINIMAL, valid shader source for `dialect`/`stage` into `buf` (the
 * text the adapter actually compiles against the live context). Carries the
 * correct `#version` line, a trivial gl_Position / fragment write, and the
 * precision qualifier the dialect requires. Returns bytes written (excluding
 * NUL), or 0 on bad args / short buffer. Pure: no GL, no I/O. */
size_t nxgl_shader_probe_source(nxgl_shader_dialect dialect,
                                nxgl_shader_stage stage,
                                char *buf, size_t cap);

/* Contract-aware source used by the live adapter. Unlike the dialect-only
 * compatibility helper, this emits a desktop core shader matching the
 * requested GL version/profile instead of assuming GLSL 1.20 compatibility. */
size_t nxgl_shader_probe_source_for_contract(
    const nxgl_graphics_contract *contract,
    nxgl_shader_stage stage, char *buf, size_t cap);

const char *nxgl_shader_probe_result_name(nxgl_shader_probe_result result);

/* Structured, machine-parsable EVIDENCE for one context verdict. The pure
 * fields (obtained/drawable/verdict/shader_probe) are the framework's measured
 * decision; the provenance fields (run_id/generation/commit/cfw/providers/
 * build_id/sdl_major) are gathered by the adapter and are OPAQUE text to the
 * pure module -- it only formats them. This is the receipt nxrelease validates
 * and links a physical proof to; it is NOT hand-writable past the gate. */
typedef struct nxgl_graphics_evidence {
  uint32_t api_version; /* NXGL_GRAPHICS_CONTRACT_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_graphics_evidence) */
  char run_id[96];      /* porttest-<epoch>-<pid>-<n>, opaque here */
  char generation[72];  /* installed generation id (full SHA-256), opaque */
  char commit[64];      /* framework commit, opaque */
  char cfw[64];         /* CFW tag, opaque */
  char device[64];      /* device model/id, opaque */
  char port_id[64];     /* port id, opaque */
  char port_version[32];/* port version, opaque */
  /* SHA-256 of the selected immutable runtime-generation manifest. The outer
   * ZIP hash cannot be embedded in that ZIP without a circular identity. */
  char artifact_sha256[72];
  char egl_provider[160];  /* resolved EGL DSO path, opaque */
  char gles_provider[160]; /* resolved GLES DSO path, opaque */
  char dso_build_id[129];  /* complete GLES provider build-id hex, opaque */
  char egl_build_id[129];  /* complete EGL provider build-id hex, opaque */
  char renderer[128];      /* glGetString(GL_RENDERER), opaque */
  char gl_version_str[96]; /* glGetString(GL_VERSION), opaque */
  char glsl_version[96];   /* glGetString(GL_SHADING_LANGUAGE_VERSION), opaque */
  char egl_version[64];    /* eglQueryString(EGL_VERSION), opaque */
  int sdl_major;           /* 2 or 3; 0 = unknown */
  nxgl_graphics_obtained obtained;
  int drawable_w;
  int drawable_h;
  nxgl_graphics_reason verdict;
  nxgl_shader_probe_result shader_probe;
} nxgl_graphics_evidence;

/* Version of the JSON evidence document (nxgl_graphics_contract_evidence_json). */
#define NXGL_GRAPHICS_EVIDENCE_JSON_SCHEMA "nx-graphics-evidence"
#define NXGL_GRAPHICS_EVIDENCE_JSON_VERSION 2

/* Initialise evidence to empty/unknown (all provenance "", sdl_major 0,
 * shader_probe SKIPPED). Returns 0, or -1 on NULL. */
int nxgl_graphics_evidence_init(nxgl_graphics_evidence *ev);

/* One structured line, e.g.
 *   GRAPHICS-EVIDENCE: run_id=... generation=... commit=... cfw=... sdl=2
 *     provider_egl=... provider_gles=... build_id=... requested=gles/es/2.0/minimum
 *     obtained=gles/es/3.2 drawable=640x480 shader_probe=pass verdict=OK reason=ok
 * Empty provenance fields print as "-". Returns bytes written (excluding NUL),
 * or 0 on bad args / short buffer. Pure: no I/O. */
size_t nxgl_graphics_contract_evidence_receipt(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *ev,
    char *buf, size_t cap);

/* The FULL evidence as a versioned JSON object (schema
 * "nx-graphics-evidence"/2): run_id, generation, commit, cfw, device,
 * port{id,version}, artifact_sha256, sdl_major, provider{egl,egl_build_id,
 * gles,gles_build_id}, gl{renderer,version,glsl,egl_version},
 * requested{api,profile,version,policy,dialect}, obtained{api,profile,version},
 * drawable{w,h}, shader_probe, verdict, reason. String values are JSON-escaped;
 * empty opaque fields serialise as "". This is the artifact nxrelease reparses,
 * RECOMPUTES the verdict from, and binds to the immutable ZIP/tree hash --
 * never hand-writable. Returns bytes written (excluding NUL), or 0 on bad args /
 * short buffer. Pure: no I/O. */
size_t nxgl_graphics_contract_evidence_json(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *ev,
    char *buf, size_t cap);

/* Stable lowercase name for a reason code (for logs/receipts). Never NULL. */
const char *nxgl_graphics_reason_name(nxgl_graphics_reason reason);
const char *nxgl_graphics_api_name(nxgl_graphics_api api);
const char *nxgl_graphics_profile_name(nxgl_graphics_profile profile);
const char *nxgl_graphics_policy_name(nxgl_graphics_version_policy policy);
const char *nxgl_shader_dialect_name(nxgl_shader_dialect dialect);

/* One-line machine-parsable verdict receipt, e.g.
 *   GRAPHICS: requested=gles/es/2.0/exact obtained=gl/compat/3.1
 *             drawable=1x1 shader=essl100 verdict=FAIL
 *             reason=desktop-gl-for-gles-contract
 * Returns bytes written (excluding NUL), or 0 on bad args / short buffer.
 * Pure: no I/O. The wider receipt (SDL driver, EGL vendor, provider build-id,
 * EGLConfig, frame proof) is assembled by the adapter around this verdict. */
size_t nxgl_graphics_contract_receipt(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_obtained *obtained,
    int drawable_w, int drawable_h,
    nxgl_graphics_reason reason,
    char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_GRAPHICS_CONTRACT_H */
