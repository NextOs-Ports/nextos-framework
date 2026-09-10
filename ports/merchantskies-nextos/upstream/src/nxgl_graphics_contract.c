/* SPDX-License-Identifier: GPL-3.0-only */
/* nxgl_graphics_contract -- see include/nxgl_graphics_contract.h. Pure. */
#include "nxgl_graphics_contract.h"

#include <stdio.h>
#include <string.h>

int nxgl_graphics_contract_default(nxgl_graphics_contract *contract) {
  if (contract == NULL) {
    return -1;
  }
  contract->api_version = NXGL_GRAPHICS_CONTRACT_API_VERSION;
  contract->struct_size = sizeof(*contract);
  contract->api = NXGL_GRAPHICS_API_GLES;
  contract->profile = NXGL_GRAPHICS_PROFILE_ES;
  contract->version_major = 2;
  contract->version_minor = 0;
  contract->version_policy = NXGL_GRAPHICS_POLICY_EXACT;
  contract->version_max_major = 2;
  contract->version_max_minor = 0;
  contract->shader_dialect = NXGL_SHADER_DIALECT_ESSL100;
  contract->drawable_ready_timeout_ms = 5000;
  return 0;
}

static int nxgl_version_cmp(int amaj, int amin, int bmaj, int bmin) {
  if (amaj != bmaj) {
    return amaj < bmaj ? -1 : 1;
  }
  if (amin != bmin) {
    return amin < bmin ? -1 : 1;
  }
  return 0;
}

int nxgl_graphics_contract_is_valid(const nxgl_graphics_contract *c) {
  if (c == NULL || c->struct_size != sizeof(*c) ||
      c->api_version != NXGL_GRAPHICS_CONTRACT_API_VERSION) {
    return 0;
  }
  /* Every enum bounded. */
  if (c->api != NXGL_GRAPHICS_API_GLES && c->api != NXGL_GRAPHICS_API_GL) {
    return 0;
  }
  if (c->profile != NXGL_GRAPHICS_PROFILE_ES &&
      c->profile != NXGL_GRAPHICS_PROFILE_CORE &&
      c->profile != NXGL_GRAPHICS_PROFILE_COMPAT) {
    return 0;
  }
  if (c->version_policy != NXGL_GRAPHICS_POLICY_EXACT &&
      c->version_policy != NXGL_GRAPHICS_POLICY_MINIMUM &&
      c->version_policy != NXGL_GRAPHICS_POLICY_RANGE) {
    return 0;
  }
  if (c->shader_dialect != NXGL_SHADER_DIALECT_ESSL100 &&
      c->shader_dialect != NXGL_SHADER_DIALECT_ESSL300 &&
      c->shader_dialect != NXGL_SHADER_DIALECT_ESSL310 &&
      c->shader_dialect != NXGL_SHADER_DIALECT_GLSL_ANY) {
    return 0;
  }
  /* API <-> profile coherence: GLES is profile ES; desktop GL is core/compat. */
  if (c->api == NXGL_GRAPHICS_API_GLES &&
      c->profile != NXGL_GRAPHICS_PROFILE_ES) {
    return 0;
  }
  if (c->api == NXGL_GRAPHICS_API_GL &&
      c->profile == NXGL_GRAPHICS_PROFILE_ES) {
    return 0;
  }
  /* The desktop core profile was introduced by OpenGL 3.2. Accepting a lower
   * version would make the adapter fabricate a shader dialect that the
   * declaration itself cannot represent. */
  if (c->profile == NXGL_GRAPHICS_PROFILE_CORE &&
      nxgl_version_cmp(c->version_major, c->version_minor, 3, 2) < 0) {
    return 0;
  }
  /* API <-> shader dialect coherence: a GLES contract carries an ESSL dialect;
   * a desktop GL contract is not pinned to ESSL (GLSL_ANY). */
  if (c->api == NXGL_GRAPHICS_API_GLES &&
      c->shader_dialect == NXGL_SHADER_DIALECT_GLSL_ANY) {
    return 0;
  }
  if (c->api == NXGL_GRAPHICS_API_GL &&
      c->shader_dialect != NXGL_SHADER_DIALECT_GLSL_ANY) {
    return 0;
  }
  /* Versions are non-negative; a RANGE policy needs max >= min. */
  if (c->version_major < 0 || c->version_minor < 0) {
    return 0;
  }
  if (c->version_policy == NXGL_GRAPHICS_POLICY_RANGE) {
    if (c->version_max_major < 0 || c->version_max_minor < 0) {
      return 0;
    }
    if (nxgl_version_cmp(c->version_max_major, c->version_max_minor,
                         c->version_major, c->version_minor) < 0) {
      return 0;
    }
  }
  /* Timeout is a non-negative, bounded number of milliseconds. */
  if (c->drawable_ready_timeout_ms < 0 ||
      c->drawable_ready_timeout_ms > 60000) {
    return 0;
  }
  return 1;
}

int nxgl_graphics_obtained_is_valid(const nxgl_graphics_obtained *o) {
  if (o == NULL || o->struct_size != sizeof(*o) ||
      o->api_version != NXGL_GRAPHICS_CONTRACT_API_VERSION) {
    return 0;
  }
  if (o->api != NXGL_GRAPHICS_API_GLES && o->api != NXGL_GRAPHICS_API_GL) {
    return 0;
  }
  if (o->profile != NXGL_GRAPHICS_PROFILE_ES &&
      o->profile != NXGL_GRAPHICS_PROFILE_CORE &&
      o->profile != NXGL_GRAPHICS_PROFILE_COMPAT) {
    return 0;
  }
  if ((o->api == NXGL_GRAPHICS_API_GLES &&
       o->profile != NXGL_GRAPHICS_PROFILE_ES) ||
      (o->api == NXGL_GRAPHICS_API_GL &&
       o->profile == NXGL_GRAPHICS_PROFILE_ES) ||
      o->version_major < 0 || o->version_minor < 0) {
    return 0;
  }
  return 1;
}

nxgl_graphics_reason nxgl_graphics_contract_validate(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_obtained *obtained) {
  int cmp;

  if (!nxgl_graphics_contract_is_valid(contract) ||
      !nxgl_graphics_obtained_is_valid(obtained)) {
    return NXGL_GRAPHICS_CONTRACT_INVALID;
  }
  /* API FIRST: a GLES contract that received a desktop GL context is the Beach
   * Buggy failure and must be caught before any shader is offered. */
  if (contract->api == NXGL_GRAPHICS_API_GLES &&
      obtained->api == NXGL_GRAPHICS_API_GL) {
    return NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT;
  }
  if (contract->api == NXGL_GRAPHICS_API_GL &&
      obtained->api == NXGL_GRAPHICS_API_GLES) {
    return NXGL_GRAPHICS_GLES_FOR_GL_CONTRACT;
  }
  if (contract->profile != obtained->profile) {
    return NXGL_GRAPHICS_PROFILE_MISMATCH;
  }
  cmp = nxgl_version_cmp(obtained->version_major, obtained->version_minor,
                         contract->version_major, contract->version_minor);
  switch (contract->version_policy) {
    case NXGL_GRAPHICS_POLICY_EXACT:
      if (cmp != 0) {
        return NXGL_GRAPHICS_VERSION_NOT_EXACT;
      }
      break;
    case NXGL_GRAPHICS_POLICY_MINIMUM:
      if (cmp < 0) {
        return NXGL_GRAPHICS_VERSION_BELOW_MINIMUM;
      }
      break;
    case NXGL_GRAPHICS_POLICY_RANGE:
      if (cmp < 0 ||
          nxgl_version_cmp(obtained->version_major, obtained->version_minor,
                           contract->version_max_major,
                           contract->version_max_minor) > 0) {
        return NXGL_GRAPHICS_VERSION_OUT_OF_RANGE;
      }
      break;
    default:
      return NXGL_GRAPHICS_CONTRACT_INVALID;
  }
  return NXGL_GRAPHICS_OK;
}

int nxgl_graphics_drawable_usable(int width, int height) {
  /* 1x1 is the SDL placeholder before the surface is mapped; it is never proof
   * of video. Require both dimensions positive and larger than the placeholder. */
  if (width <= 1 || height <= 1) {
    return 0;
  }
  return 1;
}

const char *nxgl_shader_dialect_version_line(nxgl_shader_dialect dialect) {
  switch (dialect) {
    case NXGL_SHADER_DIALECT_ESSL100:
      return "#version 100";
    case NXGL_SHADER_DIALECT_ESSL300:
      return "#version 300 es";
    case NXGL_SHADER_DIALECT_ESSL310:
      return "#version 310 es";
    case NXGL_SHADER_DIALECT_GLSL_ANY:
    default:
      return "";
  }
}

/* Collapse runs of spaces/tabs to a single space and trim; returns dst. */
static const char *nxgl_collapse_ws(const char *src, char *dst, size_t cap) {
  size_t o = 0;
  int in_space = 1; /* skip leading */
  if (cap == 0) {
    return dst;
  }
  for (; *src && o + 1 < cap; src++) {
    char ch = *src;
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      if (!in_space) {
        dst[o++] = ' ';
        in_space = 1;
      }
    } else {
      dst[o++] = ch;
      in_space = 0;
    }
  }
  while (o > 0 && dst[o - 1] == ' ') {
    o--;
  }
  dst[o] = '\0';
  return dst;
}

int nxgl_shader_source_matches_dialect(const char *source,
                                       nxgl_shader_dialect dialect) {
  const char *hash;
  const char *eol;
  char line[64];
  char norm[64];
  size_t len;

  if (dialect == NXGL_SHADER_DIALECT_GLSL_ANY) {
    return 1; /* desktop GLSL: dialect not pinned */
  }
  if (source == NULL) {
    return 0;
  }
  hash = strstr(source, "#version");
  if (hash == NULL) {
    return 0; /* an ESSL dialect REQUIRES an explicit #version */
  }
  eol = strchr(hash, '\n');
  len = eol ? (size_t)(eol - hash) : strlen(hash);
  if (len >= sizeof(line)) {
    len = sizeof(line) - 1;
  }
  memcpy(line, hash, len);
  line[len] = '\0';
  nxgl_collapse_ws(line, norm, sizeof(norm));
  return strcmp(norm, nxgl_shader_dialect_version_line(dialect)) == 0;
}

const char *nxgl_graphics_reason_name(nxgl_graphics_reason reason) {
  switch (reason) {
    case NXGL_GRAPHICS_OK:
      return "ok";
    case NXGL_GRAPHICS_DESKTOP_GL_FOR_GLES_CONTRACT:
      return "desktop-gl-for-gles-contract";
    case NXGL_GRAPHICS_GLES_FOR_GL_CONTRACT:
      return "gles-for-gl-contract";
    case NXGL_GRAPHICS_PROFILE_MISMATCH:
      return "profile-mismatch";
    case NXGL_GRAPHICS_VERSION_NOT_EXACT:
      return "version-not-exact";
    case NXGL_GRAPHICS_VERSION_BELOW_MINIMUM:
      return "version-below-minimum";
    case NXGL_GRAPHICS_VERSION_OUT_OF_RANGE:
      return "version-out-of-range";
    case NXGL_GRAPHICS_DRAWABLE_STUCK_1X1:
      return "drawable-stuck-1x1";
    case NXGL_GRAPHICS_SHADER_PROBE_FAILED:
      return "shader-probe-failed";
    case NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY:
      return "provider-nominal-only";
    case NXGL_GRAPHICS_EVIDENCE_INCOMPLETE:
      return "evidence-incomplete";
    case NXGL_GRAPHICS_RECEIPT_REJECTED:
      return "receipt-rejected";
    case NXGL_GRAPHICS_CONTRACT_INVALID:
    default:
      return "contract-invalid";
  }
}

const char *nxgl_graphics_api_name(nxgl_graphics_api api) {
  switch (api) {
    case NXGL_GRAPHICS_API_GLES:
      return "gles";
    case NXGL_GRAPHICS_API_GL:
      return "gl";
    default:
      return "invalid";
  }
}

const char *nxgl_graphics_profile_name(nxgl_graphics_profile profile) {
  switch (profile) {
    case NXGL_GRAPHICS_PROFILE_CORE:
      return "core";
    case NXGL_GRAPHICS_PROFILE_COMPAT:
      return "compat";
    case NXGL_GRAPHICS_PROFILE_ES:
      return "es";
    default:
      return "invalid";
  }
}

const char *nxgl_graphics_policy_name(nxgl_graphics_version_policy policy) {
  switch (policy) {
    case NXGL_GRAPHICS_POLICY_MINIMUM:
      return "minimum";
    case NXGL_GRAPHICS_POLICY_RANGE:
      return "range";
    case NXGL_GRAPHICS_POLICY_EXACT:
      return "exact";
    default:
      return "invalid";
  }
}

const char *nxgl_shader_dialect_name(nxgl_shader_dialect dialect) {
  switch (dialect) {
    case NXGL_SHADER_DIALECT_ESSL300:
      return "essl300";
    case NXGL_SHADER_DIALECT_ESSL310:
      return "essl310";
    case NXGL_SHADER_DIALECT_GLSL_ANY:
      return "glsl-any";
    case NXGL_SHADER_DIALECT_ESSL100:
      return "essl100";
    default:
      return "invalid";
  }
}

size_t nxgl_graphics_contract_receipt(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_obtained *obtained,
    int drawable_w, int drawable_h,
    nxgl_graphics_reason reason,
    char *buf, size_t cap) {
  int written;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (contract == NULL || obtained == NULL) {
    return 0u;
  }
  written = snprintf(
      buf, cap,
      "GRAPHICS: requested=%s/%s/%d.%d/%s obtained=%s/%s/%d.%d "
      "drawable=%dx%d shader=%s verdict=%s reason=%s",
      nxgl_graphics_api_name(contract->api),
      nxgl_graphics_profile_name(contract->profile),
      contract->version_major, contract->version_minor,
      nxgl_graphics_policy_name(contract->version_policy),
      nxgl_graphics_api_name(obtained->api),
      nxgl_graphics_profile_name(obtained->profile),
      obtained->version_major, obtained->version_minor,
      drawable_w, drawable_h,
      nxgl_shader_dialect_name(contract->shader_dialect),
      reason == NXGL_GRAPHICS_OK ? "OK" : "FAIL",
      nxgl_graphics_reason_name(reason));
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}

/* --- V3-GRAPHICS-02 item 4: shader probe sources + structured evidence --- */

size_t nxgl_shader_probe_source(nxgl_shader_dialect dialect,
                                nxgl_shader_stage stage,
                                char *buf, size_t cap) {
  const char *src = NULL;
  int written;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  switch (dialect) {
    case NXGL_SHADER_DIALECT_ESSL100:
      src = (stage == NXGL_SHADER_STAGE_VERTEX)
                ? "#version 100\n"
                  "attribute vec4 nxgl_p;\n"
                  "void main() { gl_Position = nxgl_p; }\n"
                : "#version 100\n"
                  "precision mediump float;\n"
                  "void main() { gl_FragColor = vec4(1.0); }\n";
      break;
    case NXGL_SHADER_DIALECT_ESSL300:
      src = (stage == NXGL_SHADER_STAGE_VERTEX)
                ? "#version 300 es\n"
                  "in vec4 nxgl_p;\n"
                  "void main() { gl_Position = nxgl_p; }\n"
                : "#version 300 es\n"
                  "precision mediump float;\n"
                  "out vec4 nxgl_c;\n"
                  "void main() { nxgl_c = vec4(1.0); }\n";
      break;
    case NXGL_SHADER_DIALECT_ESSL310:
      src = (stage == NXGL_SHADER_STAGE_VERTEX)
                ? "#version 310 es\n"
                  "in vec4 nxgl_p;\n"
                  "void main() { gl_Position = nxgl_p; }\n"
                : "#version 310 es\n"
                  "precision mediump float;\n"
                  "out vec4 nxgl_c;\n"
                  "void main() { nxgl_c = vec4(1.0); }\n";
      break;
    case NXGL_SHADER_DIALECT_GLSL_ANY:
    default:
      /* Desktop dialect not pinned: a legacy 120 shader compiles on a
       * compatibility context, which is what a GLSL_ANY contract targets. */
      src = (stage == NXGL_SHADER_STAGE_VERTEX)
                ? "#version 120\n"
                  "attribute vec4 nxgl_p;\n"
                  "void main() { gl_Position = nxgl_p; }\n"
                : "#version 120\n"
                  "void main() { gl_FragColor = vec4(1.0); }\n";
      break;
  }
  written = snprintf(buf, cap, "%s", src);
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}

size_t nxgl_shader_probe_source_for_contract(
    const nxgl_graphics_contract *contract,
    nxgl_shader_stage stage, char *buf, size_t cap) {
  int written;
  int glsl_version;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!nxgl_graphics_contract_is_valid(contract) ||
      (stage != NXGL_SHADER_STAGE_VERTEX &&
       stage != NXGL_SHADER_STAGE_FRAGMENT)) {
    return 0u;
  }
  if (contract->api == NXGL_GRAPHICS_API_GLES) {
    return nxgl_shader_probe_source(contract->shader_dialect, stage, buf, cap);
  }

  if (contract->profile == NXGL_GRAPHICS_PROFILE_COMPAT) {
    /* A compatibility profile deliberately accepts the oldest shader syntax
     * guaranteed by its declared GL generation. */
    glsl_version = nxgl_version_cmp(contract->version_major,
                                    contract->version_minor, 2, 1) >= 0
                       ? 120
                       : 110;
    written = snprintf(
        buf, cap,
        stage == NXGL_SHADER_STAGE_VERTEX
            ? "#version %d\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n"
            : "#version %d\nvoid main() { gl_FragColor = vec4(1.0); }\n",
        glsl_version);
  } else {
    /* Core profile: select the GLSL generation paired with the minimum GL
     * version in the contract. Never emit removed `attribute`/gl_FragColor. */
    if (contract->version_major == 3 && contract->version_minor == 2) {
      glsl_version = 150;
    } else if (contract->version_major == 3) {
      glsl_version = 330;
    } else {
      glsl_version = contract->version_major * 100 +
                     contract->version_minor * 10;
    }
    written = snprintf(
        buf, cap,
        stage == NXGL_SHADER_STAGE_VERTEX
            ? "#version %d core\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n"
            : "#version %d core\nout vec4 nxgl_c;\nvoid main() { nxgl_c = vec4(1.0); }\n",
        glsl_version);
  }
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}

const char *nxgl_shader_probe_result_name(nxgl_shader_probe_result result) {
  switch (result) {
    case NXGL_SHADER_PROBE_PASS:
      return "pass";
    case NXGL_SHADER_PROBE_COMPILE_FAILED:
      return "compile-failed";
    case NXGL_SHADER_PROBE_LINK_FAILED:
      return "link-failed";
    case NXGL_SHADER_PROBE_SKIPPED:
    default:
      return "skipped";
  }
}

int nxgl_graphics_evidence_init(nxgl_graphics_evidence *ev) {
  if (ev == NULL) {
    return -1;
  }
  memset(ev, 0, sizeof(*ev));
  ev->api_version = NXGL_GRAPHICS_CONTRACT_API_VERSION;
  ev->struct_size = sizeof(*ev);
  ev->obtained.api_version = NXGL_GRAPHICS_CONTRACT_API_VERSION;
  ev->obtained.struct_size = sizeof(ev->obtained);
  ev->sdl_major = 0;
  ev->verdict = NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY;
  ev->shader_probe = NXGL_SHADER_PROBE_SKIPPED;
  return 0;
}

static const char *nxgl_gc_field(const char *s) {
  return (s != NULL && s[0] != '\0') ? s : "-";
}

size_t nxgl_graphics_contract_evidence_receipt(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *ev,
    char *buf, size_t cap) {
  int written;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (contract == NULL || ev == NULL) {
    return 0u;
  }
  written = snprintf(
      buf, cap,
      "GRAPHICS-EVIDENCE: run_id=%s generation=%s commit=%s cfw=%s sdl=%d "
      "provider_egl=%s provider_gles=%s build_id=%s "
      "requested=%s/%s/%d.%d/%s obtained=%s/%s/%d.%d drawable=%dx%d "
      "shader_probe=%s verdict=%s reason=%s",
      nxgl_gc_field(ev->run_id), nxgl_gc_field(ev->generation),
      nxgl_gc_field(ev->commit), nxgl_gc_field(ev->cfw), ev->sdl_major,
      nxgl_gc_field(ev->egl_provider), nxgl_gc_field(ev->gles_provider),
      nxgl_gc_field(ev->dso_build_id),
      nxgl_graphics_api_name(contract->api),
      nxgl_graphics_profile_name(contract->profile),
      contract->version_major, contract->version_minor,
      nxgl_graphics_policy_name(contract->version_policy),
      nxgl_graphics_api_name(ev->obtained.api),
      nxgl_graphics_profile_name(ev->obtained.profile),
      ev->obtained.version_major, ev->obtained.version_minor,
      ev->drawable_w, ev->drawable_h,
      nxgl_shader_probe_result_name(ev->shader_probe),
      ev->verdict == NXGL_GRAPHICS_OK ? "OK" : "FAIL",
      nxgl_graphics_reason_name(ev->verdict));
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}

/* --- Section 1: the full evidence as versioned JSON (pure) ---------------- */

/* Append a raw NUL-terminated fragment; returns 1 or 0 on overflow. Always
 * keeps `buf` NUL-terminated. */
static int nxgl_gc_json_put(char *buf, size_t cap, size_t *off, const char *s) {
  size_t i;
  for (i = 0u; s[i] != '\0'; i++) {
    if (*off + 1u >= cap) {
      return 0;
    }
    buf[*off] = s[i];
    (*off)++;
  }
  buf[*off] = '\0';
  return 1;
}

/* Append a JSON-escaped string literal (with surrounding quotes). A NULL string
 * serialises as "". */
static int nxgl_gc_json_str(char *buf, size_t cap, size_t *off,
                            const char *s) {
  size_t i;
  if (!nxgl_gc_json_put(buf, cap, off, "\"")) {
    return 0;
  }
  for (i = 0u; s != NULL && s[i] != '\0'; i++) {
    unsigned char c = (unsigned char)s[i];
    const char *frag = NULL;
    char esc[8];
    switch (c) {
      case '"':  frag = "\\\""; break;
      case '\\': frag = "\\\\"; break;
      case '\n': frag = "\\n"; break;
      case '\r': frag = "\\r"; break;
      case '\t': frag = "\\t"; break;
      default: break;
    }
    if (frag == NULL && c < 0x20u) {
      int n = snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
      if (n < 0 || (size_t)n >= sizeof esc) {
        return 0;
      }
      frag = esc;
    }
    if (frag != NULL) {
      if (!nxgl_gc_json_put(buf, cap, off, frag)) {
        return 0;
      }
    } else {
      if (*off + 1u >= cap) {
        return 0;
      }
      buf[*off] = (char)c;
      (*off)++;
      buf[*off] = '\0';
    }
  }
  return nxgl_gc_json_put(buf, cap, off, "\"");
}

static int nxgl_gc_json_int(char *buf, size_t cap, size_t *off, int value) {
  char tmp[16];
  int n = snprintf(tmp, sizeof tmp, "%d", value);
  if (n < 0 || (size_t)n >= sizeof tmp) {
    return 0;
  }
  return nxgl_gc_json_put(buf, cap, off, tmp);
}

/* "key":"value" pair. */
static int nxgl_gc_json_kv(char *buf, size_t cap, size_t *off,
                           const char *key, const char *value) {
  return nxgl_gc_json_str(buf, cap, off, key) &&
         nxgl_gc_json_put(buf, cap, off, ":") &&
         nxgl_gc_json_str(buf, cap, off, value);
}

static int nxgl_gc_bounded_string(const char *value, size_t capacity) {
  return value != NULL && capacity > 0u &&
         memchr(value, '\0', capacity) != NULL;
}

#define NXGL_GC_STRING_FIELD_VALID(ev, field) \
  nxgl_gc_bounded_string((ev)->field, sizeof((ev)->field))

static int nxgl_gc_evidence_structural_valid(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *ev) {
  nxgl_graphics_reason recomputed;
  if (!nxgl_graphics_contract_is_valid(contract) || ev == NULL ||
      ev->api_version != NXGL_GRAPHICS_CONTRACT_API_VERSION ||
      ev->struct_size != sizeof(*ev) ||
      ev->sdl_major < 0 || ev->sdl_major > 3 || ev->sdl_major == 1 ||
      ev->drawable_w < 0 || ev->drawable_h < 0 ||
      ev->shader_probe < NXGL_SHADER_PROBE_SKIPPED ||
      ev->shader_probe > NXGL_SHADER_PROBE_LINK_FAILED ||
      ev->verdict < NXGL_GRAPHICS_OK ||
      ev->verdict > NXGL_GRAPHICS_CONTRACT_INVALID) {
    return 0;
  }
  /* A failed measurement has no honest obtained tuple. Evidence init leaves
   * it as the explicit all-zero/0.0 sentinel, which is serialised as
   * invalid/invalid/0.0 for diagnosis. Any other incoherent tuple is rejected. */
  if (!nxgl_graphics_obtained_is_valid(&ev->obtained) &&
      !(ev->verdict != NXGL_GRAPHICS_OK && ev->obtained.api == 0 &&
        ev->obtained.profile == 0 && ev->obtained.version_major == 0 &&
        ev->obtained.version_minor == 0)) {
    return 0;
  }
  if (!NXGL_GC_STRING_FIELD_VALID(ev, run_id) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, generation) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, commit) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, cfw) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, device) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, port_id) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, port_version) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, artifact_sha256) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, egl_provider) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, gles_provider) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, dso_build_id) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, egl_build_id) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, renderer) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, gl_version_str) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, glsl_version) ||
      !NXGL_GC_STRING_FIELD_VALID(ev, egl_version)) {
    return 0;
  }
  /* A success document is self-consistent; nxrelease independently repeats
   * this computation when binding a physical proof. Failure documents remain
   * serialisable so the device leaves a diagnosis. */
  if (ev->verdict == NXGL_GRAPHICS_OK) {
    recomputed = nxgl_graphics_contract_validate(contract, &ev->obtained);
    if (recomputed != NXGL_GRAPHICS_OK ||
        !nxgl_graphics_drawable_usable(ev->drawable_w, ev->drawable_h) ||
        ev->shader_probe != NXGL_SHADER_PROBE_PASS) {
      return 0;
    }
  }
  return 1;
}

#undef NXGL_GC_STRING_FIELD_VALID

size_t nxgl_graphics_contract_evidence_json(
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *ev,
    char *buf, size_t cap) {
  size_t off = 0u;
  char ver[24];
  char ver_max[24];
  int ok = 1;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!nxgl_gc_evidence_structural_valid(contract, ev)) {
    return 0u;
  }

  ok = ok && nxgl_gc_json_put(buf, cap, &off, "{");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "schema",
                             NXGL_GRAPHICS_EVIDENCE_JSON_SCHEMA);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",\"schema_version\":");
  ok = ok && nxgl_gc_json_int(buf, cap, &off,
                              NXGL_GRAPHICS_EVIDENCE_JSON_VERSION);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "run_id", ev->run_id);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "generation", ev->generation);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "commit", ev->commit);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "cfw", ev->cfw);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "device", ev->device);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",\"port\":{");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "id", ev->port_id);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "version", ev->port_version);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "},");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "artifact_sha256",
                             ev->artifact_sha256);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",\"sdl_major\":");
  ok = ok && nxgl_gc_json_int(buf, cap, &off, ev->sdl_major);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",\"provider\":{");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "egl", ev->egl_provider);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "egl_build_id", ev->egl_build_id);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "gles", ev->gles_provider);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "gles_build_id", ev->dso_build_id);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "},\"gl\":{");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "renderer", ev->renderer);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "version", ev->gl_version_str);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "glsl", ev->glsl_version);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "egl_version", ev->egl_version);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "},\"requested\":{");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "api",
                             nxgl_graphics_api_name(contract->api));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "profile",
                             nxgl_graphics_profile_name(contract->profile));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  (void)snprintf(ver, sizeof ver, "%d.%d", contract->version_major,
                 contract->version_minor);
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "version", ver);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  if (contract->version_policy == NXGL_GRAPHICS_POLICY_RANGE) {
    (void)snprintf(ver_max, sizeof ver_max, "%d.%d",
                   contract->version_max_major, contract->version_max_minor);
    ok = ok && nxgl_gc_json_kv(buf, cap, &off, "version_max", ver_max);
  } else {
    ok = ok && nxgl_gc_json_put(buf, cap, &off, "\"version_max\":null");
  }
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "policy",
                             nxgl_graphics_policy_name(contract->version_policy));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "dialect",
                             nxgl_shader_dialect_name(contract->shader_dialect));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "},\"obtained\":{");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "api",
                             nxgl_graphics_api_name(ev->obtained.api));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "profile",
                             nxgl_graphics_profile_name(ev->obtained.profile));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  (void)snprintf(ver, sizeof ver, "%d.%d", ev->obtained.version_major,
                 ev->obtained.version_minor);
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "version", ver);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "},\"drawable\":{\"w\":");
  ok = ok && nxgl_gc_json_int(buf, cap, &off, ev->drawable_w);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",\"h\":");
  ok = ok && nxgl_gc_json_int(buf, cap, &off, ev->drawable_h);
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "},");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "shader_probe",
                             nxgl_shader_probe_result_name(ev->shader_probe));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "verdict",
                             ev->verdict == NXGL_GRAPHICS_OK ? "OK" : "FAIL");
  ok = ok && nxgl_gc_json_put(buf, cap, &off, ",");
  ok = ok && nxgl_gc_json_kv(buf, cap, &off, "reason",
                             nxgl_graphics_reason_name(ev->verdict));
  ok = ok && nxgl_gc_json_put(buf, cap, &off, "}");

  if (!ok) {
    buf[0] = '\0';
    return 0u;
  }
  return off;
}
