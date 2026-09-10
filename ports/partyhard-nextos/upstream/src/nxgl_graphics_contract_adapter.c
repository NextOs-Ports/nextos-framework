/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "nxgl_graphics_contract_adapter.h"

#include <dlfcn.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* GL enum for glGetString(GL_VERSION). */
#define NXGL_GC_GL_VERSION 0x1F02u
/* SDL_GLattr + profile flags (stable ABI values). */
#define NXGL_GC_SDL_CONTEXT_MAJOR 17
#define NXGL_GC_SDL_CONTEXT_MINOR 18
#define NXGL_GC_SDL_CONTEXT_PROFILE_MASK 21
#define NXGL_GC_SDL_PROFILE_CORE 0x0001
#define NXGL_GC_SDL_PROFILE_COMPAT 0x0002
#define NXGL_GC_SDL_PROFILE_ES 0x0004

static void *(*g_resolver)(const char *);
static nxgl_graphics_resolver_fn g_resolver_ex;
static void *g_resolver_userdata;

void nxgl_graphics_contract_adapter_set_resolver(void *(*resolver)(const char *)) {
  g_resolver = resolver;
  g_resolver_ex = NULL;
  g_resolver_userdata = NULL;
}

void nxgl_graphics_contract_adapter_set_resolver_ex(
    nxgl_graphics_resolver_fn resolver, void *userdata) {
  g_resolver = NULL;
  g_resolver_ex = resolver;
  g_resolver_userdata = resolver != NULL ? userdata : NULL;
}

static void *nxgl_gc_resolve(const char *name) {
  void *found;
  if (g_resolver_ex) {
    return g_resolver_ex(g_resolver_userdata, name);
  }
  if (g_resolver) {
    found = g_resolver(name);
    if (found) {
      return found;
    }
  }
  found = dlsym(RTLD_DEFAULT, name);
  if (found) {
    return found;
  }
  {
    void *(*sdl_get_proc)(const char *) =
        (void *(*)(const char *))dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
    if (sdl_get_proc) {
      return sdl_get_proc(name);
    }
  }
  return NULL;
}

/* Parse the first "MAJOR.MINOR" found in `s` (digits after any prefix). Returns
 * 1 on success. */
static int nxgl_gc_parse_version(const char *s, int *major, int *minor) {
  int m = 0, n = 0, seen = 0;
  if (s == NULL || major == NULL || minor == NULL) {
    return 0;
  }
  while (*s && (*s < '0' || *s > '9')) {
    s++;
  }
  if (*s < '0' || *s > '9') {
    return 0;
  }
  while (*s >= '0' && *s <= '9') {
    if (m > (INT_MAX - (*s - '0')) / 10) {
      return 0;
    }
    m = m * 10 + (*s - '0');
    s++;
    seen = 1;
  }
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') {
      if (n > (INT_MAX - (*s - '0')) / 10) {
        return 0;
      }
      n = n * 10 + (*s - '0');
      s++;
    }
  }
  if (!seen) {
    return 0;
  }
  *major = m;
  *minor = n;
  return 1;
}

int nxgl_graphics_contract_adapter_measure(nxgl_graphics_obtained *obtained) {
  const unsigned char *(*gl_get_string)(unsigned);
  const char *version;
  const char *es;
  int major = 0, minor = 0;

  if (obtained == NULL) {
    return -1;
  }
  gl_get_string = (const unsigned char *(*)(unsigned))nxgl_gc_resolve("glGetString");
  if (gl_get_string == NULL) {
    return -1;
  }
  version = (const char *)gl_get_string(NXGL_GC_GL_VERSION);
  if (version == NULL) {
    return -1;
  }

  obtained->api_version = NXGL_GRAPHICS_CONTRACT_API_VERSION;
  obtained->struct_size = sizeof(*obtained);

  /* Authoritative: a version string WITHOUT "OpenGL ES" is a desktop context,
   * whatever SDL_GL_SetAttribute requested (the "3.1 Mesa" field case). */
  es = strstr(version, "OpenGL ES");
  if (es != NULL) {
    if (!nxgl_gc_parse_version(es + 9, &major, &minor)) {
      return -1;
    }
    obtained->api = NXGL_GRAPHICS_API_GLES;
    obtained->profile = NXGL_GRAPHICS_PROFILE_ES;
  } else {
    int mask = 0;
    int (*get_attr)(int, int *) =
        (int (*)(int, int *))nxgl_gc_resolve("SDL_GL_GetAttribute");
    if (!nxgl_gc_parse_version(version, &major, &minor)) {
      return -1;
    }
    obtained->api = NXGL_GRAPHICS_API_GL;
    if (get_attr != NULL &&
        get_attr(NXGL_GC_SDL_CONTEXT_PROFILE_MASK, &mask) == 0 &&
        (mask & NXGL_GC_SDL_PROFILE_CORE) != 0) {
      obtained->profile = NXGL_GRAPHICS_PROFILE_CORE;
    } else {
      obtained->profile = NXGL_GRAPHICS_PROFILE_COMPAT;
    }
  }
  obtained->version_major = major;
  obtained->version_minor = minor;
  return 0;
}

int nxgl_graphics_contract_adapter_drawable(int *width, int *height) {
  void *(*get_current_window)(void);
  int w = 0, h = 0;

  if (width != NULL) {
    *width = 0;
  }
  if (height != NULL) {
    *height = 0;
  }
  /* SDL2 spells it SDL_GL_GetDrawableSize; SDL3 renamed it to
   * SDL_GetWindowSizeInPixels (same (window, int*, int*) shape). Resolve the
   * one this runtime actually provides. */
  get_current_window =
      (void *(*)(void))nxgl_gc_resolve("SDL_GL_GetCurrentWindow");
  {
    void *window = get_current_window != NULL ? get_current_window() : NULL;
    void (*sdl2_get_size)(void *, int *, int *) =
        (void (*)(void *, int *, int *))nxgl_gc_resolve(
            "SDL_GL_GetDrawableSize");
    if (sdl2_get_size != NULL) {
      sdl2_get_size(window, &w, &h);
    } else {
      _Bool (*sdl3_get_size)(void *, int *, int *) =
          (_Bool (*)(void *, int *, int *))nxgl_gc_resolve(
              "SDL_GetWindowSizeInPixels");
      if (sdl3_get_size == NULL || !sdl3_get_size(window, &w, &h)) {
        return -1;
      }
    }
  }
  if (width != NULL) {
    *width = w;
  }
  if (height != NULL) {
    *height = h;
  }
  return 0;
}

/* ---------------------- item 4: monotonic wait, SDL major, real shader
 * probe, and the structured evidence pass. These need <time.h> (monotonic
 * clock + nanosleep), dladdr/dl_iterate_phdr (provider build-id) and getenv
 * (provenance). Still measures, never decides by name. */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <link.h>
#include <elf.h>

/* GL scalar types (avoid pulling a GL header into the framework). */
typedef unsigned nxgl_glenum;
typedef unsigned nxgl_gluint;
typedef int nxgl_glint;
typedef int nxgl_glsizei;

#define NXGL_GC_GL_VERTEX_SHADER 0x8B31u
#define NXGL_GC_GL_FRAGMENT_SHADER 0x8B30u
#define NXGL_GC_GL_COMPILE_STATUS 0x8B81u
#define NXGL_GC_GL_LINK_STATUS 0x8B82u
#define NXGL_GC_GL_TRUE 1

/* Read CLOCK_MONOTONIC into milliseconds. Returns 1 on success and writes
 * `out_ms`; returns 0 if the clock read fails OR the value would overflow
 * int64 (a caller that cannot read a trustworthy clock must fail immediately
 * rather than treat a broken clock as "time 0"). */
static int nxgl_gc_monotonic_ms(int64_t *out_ms) {
  struct timespec ts;
  int64_t sec, ms;
  if (out_ms == NULL) {
    return 0;
  }
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  sec = (int64_t)ts.tv_sec;
  /* sec * 1000 must not overflow int64. */
  if (sec > (INT64_MAX - 1000) / 1000) {
    return 0;
  }
  ms = sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
  *out_ms = ms;
  return 1;
}

static void nxgl_gc_sleep_ms(long ms) {
  struct timespec req;
  req.tv_sec = ms / 1000L;
  req.tv_nsec = (ms % 1000L) * 1000000L;
  (void)nanosleep(&req, NULL);
}

int nxgl_graphics_contract_adapter_sdl_major(void) {
  if (nxgl_gc_resolve("SDL_GL_GetDrawableSize") != NULL) {
    return 2; /* SDL2 name */
  }
  if (nxgl_gc_resolve("SDL_GetWindowSizeInPixels") != NULL) {
    return 3; /* SDL3 renamed it */
  }
  return 0;
}

int nxgl_graphics_contract_adapter_drawable_wait(int *width, int *height,
                                                 int timeout_ms) {
  int w = 0, h = 0;
  int64_t start = 0, now = 0, deadline;

  if (width != NULL) {
    *width = 0;
  }
  if (height != NULL) {
    *height = 0;
  }
  /* One read, no wait. */
  if (nxgl_graphics_contract_adapter_drawable(&w, &h) != 0) {
    return -1;
  }
  if (width != NULL) {
    *width = w;
  }
  if (height != NULL) {
    *height = h;
  }
  if (nxgl_graphics_drawable_usable(w, h)) {
    return 0;
  }
  if (timeout_ms <= 0) {
    return -1;
  }
  /* A trustworthy monotonic clock is required to bound the wait; if we cannot
   * read it, fail immediately rather than spin on a broken clock. */
  if (!nxgl_gc_monotonic_ms(&start)) {
    return -1;
  }
  if (start > INT64_MAX - (int64_t)timeout_ms) {
    return -1; /* deadline would overflow */
  }
  deadline = start + (int64_t)timeout_ms;
  {
    void (*pump)(void) = (void (*)(void))nxgl_gc_resolve("SDL_PumpEvents");
    while (nxgl_gc_monotonic_ms(&now) && now < deadline) {
      if (pump != NULL) {
        pump();
      }
      nxgl_gc_sleep_ms(4);
      (void)nxgl_graphics_contract_adapter_drawable(&w, &h);
      if (width != NULL) {
        *width = w;
      }
      if (height != NULL) {
        *height = h;
      }
      if (nxgl_graphics_drawable_usable(w, h)) {
        return 0;
      }
    }
  }
  return -1;
}

static nxgl_gluint nxgl_gc_compile_stage(
    nxgl_gluint (*create_shader)(nxgl_glenum),
    void (*shader_source)(nxgl_gluint, nxgl_glsizei, const char *const *,
                          const nxgl_glint *),
    void (*compile_shader)(nxgl_gluint),
    void (*get_shader_iv)(nxgl_gluint, nxgl_glenum, nxgl_glint *),
    void (*delete_shader)(nxgl_gluint),
    nxgl_glenum type, const char *src) {
  nxgl_gluint shader;
  const char *sources[1];
  nxgl_glint status = 0;

  shader = create_shader(type);
  if (shader == 0u) {
    return 0u;
  }
  sources[0] = src;
  shader_source(shader, 1, sources, NULL);
  compile_shader(shader);
  get_shader_iv(shader, NXGL_GC_GL_COMPILE_STATUS, &status);
  if (status != NXGL_GC_GL_TRUE) {
    delete_shader(shader);
    return 0u;
  }
  return shader;
}

nxgl_shader_probe_result nxgl_graphics_contract_adapter_shader_probe(
    const nxgl_graphics_contract *contract) {
  nxgl_gluint (*create_shader)(nxgl_glenum);
  void (*shader_source)(nxgl_gluint, nxgl_glsizei, const char *const *,
                        const nxgl_glint *);
  void (*compile_shader)(nxgl_gluint);
  void (*get_shader_iv)(nxgl_gluint, nxgl_glenum, nxgl_glint *);
  nxgl_gluint (*create_program)(void);
  void (*attach_shader)(nxgl_gluint, nxgl_gluint);
  void (*link_program)(nxgl_gluint);
  void (*get_program_iv)(nxgl_gluint, nxgl_glenum, nxgl_glint *);
  void (*delete_shader)(nxgl_gluint);
  void (*delete_program)(nxgl_gluint);
  char vs_src[512], fs_src[512];
  nxgl_gluint vs, fs, program;
  nxgl_glint link_status = 0;

  if (!nxgl_graphics_contract_is_valid(contract)) {
    return NXGL_SHADER_PROBE_SKIPPED;
  }
  create_shader = (nxgl_gluint (*)(nxgl_glenum))nxgl_gc_resolve("glCreateShader");
  shader_source = (void (*)(nxgl_gluint, nxgl_glsizei, const char *const *,
                            const nxgl_glint *))nxgl_gc_resolve("glShaderSource");
  compile_shader = (void (*)(nxgl_gluint))nxgl_gc_resolve("glCompileShader");
  get_shader_iv = (void (*)(nxgl_gluint, nxgl_glenum, nxgl_glint *))
      nxgl_gc_resolve("glGetShaderiv");
  create_program = (nxgl_gluint (*)(void))nxgl_gc_resolve("glCreateProgram");
  attach_shader =
      (void (*)(nxgl_gluint, nxgl_gluint))nxgl_gc_resolve("glAttachShader");
  link_program = (void (*)(nxgl_gluint))nxgl_gc_resolve("glLinkProgram");
  get_program_iv = (void (*)(nxgl_gluint, nxgl_glenum, nxgl_glint *))
      nxgl_gc_resolve("glGetProgramiv");
  delete_shader = (void (*)(nxgl_gluint))nxgl_gc_resolve("glDeleteShader");
  delete_program = (void (*)(nxgl_gluint))nxgl_gc_resolve("glDeleteProgram");

  if (create_shader == NULL || shader_source == NULL || compile_shader == NULL ||
      get_shader_iv == NULL || create_program == NULL || attach_shader == NULL ||
      link_program == NULL || get_program_iv == NULL || delete_shader == NULL ||
      delete_program == NULL) {
    return NXGL_SHADER_PROBE_SKIPPED; /* no live context to probe */
  }

  if (nxgl_shader_probe_source_for_contract(contract,
                                            NXGL_SHADER_STAGE_VERTEX, vs_src,
                                            sizeof vs_src) == 0u ||
      nxgl_shader_probe_source_for_contract(contract,
                                            NXGL_SHADER_STAGE_FRAGMENT, fs_src,
                                            sizeof fs_src) == 0u) {
    return NXGL_SHADER_PROBE_SKIPPED;
  }

  vs = nxgl_gc_compile_stage(create_shader, shader_source, compile_shader,
                             get_shader_iv, delete_shader,
                             NXGL_GC_GL_VERTEX_SHADER, vs_src);
  fs = nxgl_gc_compile_stage(create_shader, shader_source, compile_shader,
                             get_shader_iv, delete_shader,
                             NXGL_GC_GL_FRAGMENT_SHADER, fs_src);
  if (vs == 0u || fs == 0u) {
    if (vs != 0u) {
      delete_shader(vs);
    }
    if (fs != 0u) {
      delete_shader(fs);
    }
    return NXGL_SHADER_PROBE_COMPILE_FAILED;
  }

  program = create_program();
  if (program == 0u) {
    delete_shader(vs);
    delete_shader(fs);
    return NXGL_SHADER_PROBE_LINK_FAILED;
  }
  attach_shader(program, vs);
  attach_shader(program, fs);
  link_program(program);
  get_program_iv(program, NXGL_GC_GL_LINK_STATUS, &link_status);
  delete_shader(vs);
  delete_shader(fs);
  delete_program(program);
  return (link_status == NXGL_GC_GL_TRUE) ? NXGL_SHADER_PROBE_PASS
                                          : NXGL_SHADER_PROBE_LINK_FAILED;
}

/* dl_iterate_phdr context: find the GNU build-id note of the object whose load
 * base matches `target_base` (the DSO that exports glGetString). */
struct nxgl_gc_buildid_ctx {
  void *target_base;
  char *out;
  size_t out_cap;
  int found;
};

static int nxgl_gc_buildid_cb(struct dl_phdr_info *info, size_t size,
                              void *data) {
  struct nxgl_gc_buildid_ctx *ctx = (struct nxgl_gc_buildid_ctx *)data;
  ElfW(Half) i;
  (void)size;
  if ((void *)info->dlpi_addr != ctx->target_base) {
    return 0;
  }
  for (i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    const unsigned char *p;
    const unsigned char *end;
    if (ph->p_type != PT_NOTE) {
      continue;
    }
    p = (const unsigned char *)(info->dlpi_addr + ph->p_vaddr);
    end = p + ph->p_memsz;
    while (p + sizeof(ElfW(Nhdr)) <= end) {
      const ElfW(Nhdr) *nh = (const ElfW(Nhdr) *)p;
      const unsigned char *desc =
          p + sizeof(ElfW(Nhdr)) + ((nh->n_namesz + 3u) & ~3u);
      if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4u &&
          desc + nh->n_descsz <= end) {
        size_t j;
        size_t max = nh->n_descsz;
        if (max * 2u + 1u > ctx->out_cap) {
          max = (ctx->out_cap - 1u) / 2u;
        }
        for (j = 0; j < max; j++) {
          (void)snprintf(ctx->out + j * 2u, 3u, "%02x", desc[j]);
        }
        ctx->out[max * 2u] = '\0';
        ctx->found = 1;
        return 1;
      }
      p = desc + ((nh->n_descsz + 3u) & ~3u);
    }
  }
  return 1; /* matched base, stop */
}

static void nxgl_gc_copy_env(char *dst, size_t cap, const char *name) {
  const char *v = getenv(name);
  if (dst == NULL || cap == 0u) {
    return;
  }
  dst[0] = '\0';
  if (v != NULL && v[0] != '\0') {
    (void)snprintf(dst, cap, "%s", v);
  }
}

/* Provenance from the environment the launcher/observability established.
 * RECORDED only -- never a decision input. Shared verbatim by the V3 one-shot
 * pass and the V4 present gate so both record identical identities. */
static void nxgl_gc_gather_provenance(nxgl_graphics_evidence *ev) {
  nxgl_gc_copy_env(ev->run_id, sizeof ev->run_id, "NXOBS_RUN_ID");
  nxgl_gc_copy_env(ev->generation, sizeof ev->generation, "NX_GENERATION");
  nxgl_gc_copy_env(ev->commit, sizeof ev->commit, "NX_FRAMEWORK_COMMIT");
  nxgl_gc_copy_env(ev->cfw, sizeof ev->cfw, "NX_CFW");
  nxgl_gc_copy_env(ev->device, sizeof ev->device, "NX_DEVICE");
  nxgl_gc_copy_env(ev->port_id, sizeof ev->port_id, "NX_PORT_ID");
  nxgl_gc_copy_env(ev->port_version, sizeof ev->port_version, "NX_PORT_VERSION");
  nxgl_gc_copy_env(ev->artifact_sha256, sizeof ev->artifact_sha256,
                   "NX_ARTIFACT_SHA256");
  ev->sdl_major = nxgl_graphics_contract_adapter_sdl_major();
}

/* Measured GL strings + EGL/GLES provider paths and build-ids of the live
 * context. Shared verbatim by the V3 one-shot pass and the V4 present gate. */
static void nxgl_gc_gather_gl_identity(nxgl_graphics_evidence *ev) {
  {
    const unsigned char *(*gl_get_string)(unsigned) =
        (const unsigned char *(*)(unsigned))nxgl_gc_resolve("glGetString");
    if (gl_get_string != NULL) {
      const char *renderer = (const char *)gl_get_string(0x1F01u); /* RENDERER */
      const char *glver = (const char *)gl_get_string(0x1F02u);    /* VERSION */
      const char *glsl =
          (const char *)gl_get_string(0x8B8Cu); /* SHADING_LANGUAGE_VERSION */
      if (renderer != NULL) {
        (void)snprintf(ev->renderer, sizeof ev->renderer, "%s", renderer);
      }
      if (glver != NULL) {
        (void)snprintf(ev->gl_version_str, sizeof ev->gl_version_str, "%s",
                       glver);
      }
      if (glsl != NULL) {
        (void)snprintf(ev->glsl_version, sizeof ev->glsl_version, "%s", glsl);
      }
    }
  }

  /* EGL version + EGL provider build-id, when an EGL is present. */
  {
    const char *(*egl_query)(void *, int) =
        (const char *(*)(void *, int))nxgl_gc_resolve("eglQueryString");
    void *(*egl_get_display)(void) =
        (void *(*)(void))nxgl_gc_resolve("eglGetCurrentDisplay");
    void *egl_make_current = nxgl_gc_resolve("eglMakeCurrent");
    if (egl_query != NULL && egl_get_display != NULL) {
      void *display = egl_get_display();
      const char *ver = egl_query(display, 0x3054); /* EGL_VERSION */
      if (ver != NULL) {
        (void)snprintf(ev->egl_version, sizeof ev->egl_version, "%s", ver);
      }
    }
    if (egl_make_current != NULL) {
      Dl_info edi;
      if (dladdr(egl_make_current, &edi) != 0 && edi.dli_fname != NULL) {
        (void)snprintf(ev->egl_provider, sizeof ev->egl_provider, "%s",
                       edi.dli_fname);
        {
          struct nxgl_gc_buildid_ctx ectx;
          ectx.target_base = edi.dli_fbase;
          ectx.out = ev->egl_build_id;
          ectx.out_cap = sizeof ev->egl_build_id;
          ectx.found = 0;
          (void)dl_iterate_phdr(nxgl_gc_buildid_cb, &ectx);
        }
      }
    }
  }

  /* GLES provider path + build-id from the DSO that actually exports
   * glGetString (dladdr -> dl_iterate_phdr on its load base). */
  {
    void *gl_get_string = nxgl_gc_resolve("glGetString");
    Dl_info di;
    if (gl_get_string != NULL && dladdr(gl_get_string, &di) != 0 &&
        di.dli_fname != NULL) {
      (void)snprintf(ev->gles_provider, sizeof ev->gles_provider, "%s",
                     di.dli_fname);
      {
        struct nxgl_gc_buildid_ctx ctx;
        ctx.target_base = di.dli_fbase;
        ctx.out = ev->dso_build_id;
        ctx.out_cap = sizeof ev->dso_build_id;
        ctx.found = 0;
        (void)dl_iterate_phdr(nxgl_gc_buildid_cb, &ctx);
      }
    }
  }
}

nxgl_graphics_reason nxgl_graphics_contract_adapter_evidence(
    const nxgl_graphics_contract *contract,
    nxgl_graphics_evidence *ev,
    char *receipt, size_t receipt_cap) {
  nxgl_graphics_reason reason;
  int w = 0, h = 0;
  int timeout;

  if (ev == NULL) {
    return NXGL_GRAPHICS_CONTRACT_INVALID;
  }
  (void)nxgl_graphics_evidence_init(ev);
  if (receipt != NULL && receipt_cap > 0u) {
    receipt[0] = '\0';
  }
  /* Validate the complete declaration before reading its timeout or touching
   * SDL/GL. A malformed timeout must never control an effectful wait. */
  if (!nxgl_graphics_contract_is_valid(contract)) {
    return NXGL_GRAPHICS_CONTRACT_INVALID;
  }

  nxgl_gc_gather_provenance(ev);

  if (nxgl_graphics_contract_adapter_measure(&ev->obtained) != 0) {
    ev->verdict = NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY;
    (void)nxgl_graphics_contract_evidence_json(contract, ev, receipt,
                                               receipt_cap);
    return ev->verdict;
  }

  nxgl_gc_gather_gl_identity(ev);

  timeout = contract->drawable_ready_timeout_ms;
  (void)nxgl_graphics_contract_adapter_drawable_wait(&w, &h, timeout);
  ev->drawable_w = w;
  ev->drawable_h = h;

  /* Context contract FIRST. */
  reason = nxgl_graphics_contract_validate(contract, &ev->obtained);
  if (reason == NXGL_GRAPHICS_OK && !nxgl_graphics_drawable_usable(w, h)) {
    reason = NXGL_GRAPHICS_DRAWABLE_STUCK_1X1;
  }
  /* Only probe shaders once the context and drawable are sound. A SKIPPED
   * probe is NOT a success: once a context has been measured and matched, the
   * GL entry points must exist -- if the probe could not even run, the proof is
   * incomplete and the verdict cannot stay OK. Only compile+link PASS keeps OK. */
  if (reason == NXGL_GRAPHICS_OK) {
    ev->shader_probe = nxgl_graphics_contract_adapter_shader_probe(contract);
    if (ev->shader_probe != NXGL_SHADER_PROBE_PASS) {
      reason = NXGL_GRAPHICS_SHADER_PROBE_FAILED;
    }
  }
  ev->verdict = reason;
  (void)nxgl_graphics_contract_evidence_json(contract, ev, receipt,
                                             receipt_cap);
  return reason;
}

/* =====================================================================
 * V4-GRAPHICS-04: the post-first-present boundary. The wrapper calls the
 * guest's REAL present first; only afterwards does it hand the observation to
 * the pure gate. The adapter never clears, draws or swaps -- it measures.
 * ===================================================================== */
#include "nxgl_graphics_present_gate.h"

#include <pthread.h>

/* Serialises gate transitions only. Never held across an SDL/EGL/GL call:
 * every measurement happens before the lock is taken. */
static pthread_mutex_t nxgl_gc_gate_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Re-derive the provider identities of the live context exactly the way the
 * preflight recorded them, so an identity comparison compares like with like. */
static void nxgl_gc_current_provider_paths(char *gles, size_t gles_cap,
                                           char *egl, size_t egl_cap) {
  Dl_info di;
  void *gl_get_string;
  void *egl_make_current;

  if (gles != NULL && gles_cap > 0u) {
    gles[0] = '\0';
    gl_get_string = nxgl_gc_resolve("glGetString");
    if (gl_get_string != NULL && dladdr(gl_get_string, &di) != 0 &&
        di.dli_fname != NULL) {
      (void)snprintf(gles, gles_cap, "%s", di.dli_fname);
    }
  }
  if (egl != NULL && egl_cap > 0u) {
    egl[0] = '\0';
    egl_make_current = nxgl_gc_resolve("eglMakeCurrent");
    if (egl_make_current != NULL && dladdr(egl_make_current, &di) != 0 &&
        di.dli_fname != NULL) {
      (void)snprintf(egl, egl_cap, "%s", di.dli_fname);
    }
  }
}

nxgl_graphics_gate_status nxgl_graphics_contract_adapter_pre_present(
    const nxgl_graphics_contract *contract,
    uintptr_t window_token,
    uintptr_t context_token,
    nxgl_graphics_present_gate *gate,
    nxgl_graphics_gate_result *result) {
  nxgl_graphics_evidence measured;
  nxgl_graphics_gate_status status;
  int w = 0, h = 0;

  (void)nxgl_graphics_evidence_init(&measured);
  nxgl_gc_gather_provenance(&measured);

  /* Measure the live context on the calling (owner) thread. Failure leaves
   * the all-zero obtained sentinel; the pure gate turns that into
   * provider-nominal-only. */
  if (nxgl_graphics_contract_adapter_measure(&measured.obtained) == 0) {
    nxgl_gc_gather_gl_identity(&measured);
    /* Only probe shaders on a context that matches the declaration; the pure
     * gate re-validates and stays the single authority. */
    if (nxgl_graphics_contract_is_valid(contract) &&
        nxgl_graphics_contract_validate(contract, &measured.obtained) ==
            NXGL_GRAPHICS_OK) {
      measured.shader_probe =
          nxgl_graphics_contract_adapter_shader_probe(contract);
    }
  } else {
    /* Dead provider: hand the pure gate the fully zeroed sentinel, which can
     * never be confused with a real measurement. */
    memset(&measured.obtained, 0, sizeof(measured.obtained));
  }

  /* ONE diagnostic drawable read. No wait, no event pumping: pre-present size
   * is diagnosis, never proof, and the guest must get control back. */
  if (nxgl_graphics_contract_adapter_drawable(&w, &h) == 0) {
    measured.drawable_w = w;
    measured.drawable_h = h;
  } else {
    measured.drawable_w = 0;
    measured.drawable_h = 0;
  }

  (void)pthread_mutex_lock(&nxgl_gc_gate_mutex);
  status = nxgl_graphics_present_gate_preflight(
      gate, contract, &measured, window_token, context_token, result);
  (void)pthread_mutex_unlock(&nxgl_gc_gate_mutex);
  return status;
}

nxgl_graphics_gate_status nxgl_graphics_contract_adapter_after_present(
    const nxgl_graphics_contract *contract,
    uintptr_t window_token,
    uintptr_t context_token,
    nxgl_graphics_present_gate *gate,
    nxgl_graphics_gate_result *result,
    char *final_receipt, size_t final_receipt_cap) {
  nxgl_graphics_gate_status status;
  char gles_path[160];
  char egl_path[160];
  int64_t now_ms = -1;
  int w = 0, h = 0;
  int read_ok;
  int terminal = 0;

  /* Fast path after conclusion: no SDL/EGL/dl call per frame once the gate is
   * terminal. The pure observe returns the stable verdict untouched. */
  (void)pthread_mutex_lock(&nxgl_gc_gate_mutex);
  if (gate != NULL &&
      gate->api_version == NXGL_GRAPHICS_PRESENT_GATE_API_VERSION &&
      gate->struct_size == sizeof(*gate) &&
      (gate->phase == NXGL_GRAPHICS_GATE_PHASE_PROVED ||
       gate->phase == NXGL_GRAPHICS_GATE_PHASE_REJECTED)) {
    terminal = 1;
    status = nxgl_graphics_present_gate_observe(
        gate, contract, window_token, context_token, 0, 0, 0, NULL, NULL, -1,
        result, final_receipt, final_receipt_cap);
  }
  (void)pthread_mutex_unlock(&nxgl_gc_gate_mutex);
  if (terminal) {
    return status;
  }

  /* Measure BEFORE locking: the mutex is never held across SDL/EGL/GL. */
  {
    int64_t ms;
    if (nxgl_gc_monotonic_ms(&ms)) {
      now_ms = ms;
    }
  }
  read_ok = nxgl_graphics_contract_adapter_drawable(&w, &h) == 0;
  nxgl_gc_current_provider_paths(gles_path, sizeof gles_path, egl_path,
                                 sizeof egl_path);

  (void)pthread_mutex_lock(&nxgl_gc_gate_mutex);
  status = nxgl_graphics_present_gate_observe(
      gate, contract, window_token, context_token, w, h, read_ok, gles_path,
      egl_path, now_ms, result, final_receipt, final_receipt_cap);
  (void)pthread_mutex_unlock(&nxgl_gc_gate_mutex);
  return status;
}
