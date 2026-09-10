/* SPDX-License-Identifier: GPL-3.0-only */
/* nxcompat_video -- see include/nxcompat_video.h. Pure integer geometry. */
#include "nxcompat_video.h"
#include <stdio.h>
#include <string.h>

int nxcompat_video_aspect_from_string(const char *t, nxcompat_video_aspect *out) {
  if (t == NULL || out == NULL) return -1;
  if (strcmp(t, "engine") == 0) *out = NXCOMPAT_VIDEO_ASPECT_ENGINE;
  else if (strcmp(t, "preserve") == 0) *out = NXCOMPAT_VIDEO_ASPECT_PRESERVE;
  else if (strcmp(t, "stretch") == 0) *out = NXCOMPAT_VIDEO_ASPECT_STRETCH;
  else if (strcmp(t, "crop") == 0) *out = NXCOMPAT_VIDEO_ASPECT_CROP;
  else if (strcmp(t, "integer") == 0) *out = NXCOMPAT_VIDEO_ASPECT_INTEGER;
  else if (strcmp(t, "auto") == 0) *out = NXCOMPAT_VIDEO_ASPECT_AUTO;
  else return -1;
  return 0;
}

const char *nxcompat_video_aspect_name(nxcompat_video_aspect a) {
  switch (a) {
    case NXCOMPAT_VIDEO_ASPECT_ENGINE: return "engine";
    case NXCOMPAT_VIDEO_ASPECT_PRESERVE: return "preserve";
    case NXCOMPAT_VIDEO_ASPECT_STRETCH: return "stretch";
    case NXCOMPAT_VIDEO_ASPECT_CROP: return "crop";
    case NXCOMPAT_VIDEO_ASPECT_INTEGER: return "integer";
    case NXCOMPAT_VIDEO_ASPECT_AUTO: return "auto";
    default: return "?";
  }
}

static void bars_of(nxcompat_video_decision *d) {
  d->bar_left = d->content.x > 0 ? d->content.x : 0;
  d->bar_top = d->content.y > 0 ? d->content.y : 0;
  d->bar_right = d->drawable_w - (d->content.x + d->content.w);
  d->bar_bottom = d->drawable_h - (d->content.y + d->content.h);
  if (d->bar_right < 0) d->bar_right = 0;
  if (d->bar_bottom < 0) d->bar_bottom = 0;
}

int nxcompat_video_content_rect(nxcompat_video_aspect policy, int sw, int sh, int dw, int dh, nxcompat_video_decision *o) {
  if (o == NULL) return -1;
  memset(o, 0, sizeof *o);
  o->api_version = NXCOMPAT_VIDEO_API_VERSION;
  o->requested = policy; o->effective = policy;
  o->source_w = sw; o->source_h = sh; o->drawable_w = dw; o->drawable_h = dh;
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || sw > 16384 || sh > 16384 || dw > 16384 || dh > 16384) {
    snprintf(o->reason, sizeof o->reason, "invalid geometry"); return -1;
  }
  switch (policy) {
    case NXCOMPAT_VIDEO_ASPECT_AUTO:
      snprintf(o->reason, sizeof o->reason, "auto is not a policy: resolve through the declared algorithm");
      return -1;
    case NXCOMPAT_VIDEO_ASPECT_ENGINE:
      o->content.x = 0; o->content.y = 0; o->content.w = dw; o->content.h = dh;
      snprintf(o->reason, sizeof o->reason, "engine inherit: nothing asserted");
      break;
    case NXCOMPAT_VIDEO_ASPECT_STRETCH:
      o->content.x = 0; o->content.y = 0; o->content.w = dw; o->content.h = dh;
      o->distorted = (long)sw * dh != (long)sh * dw;
      snprintf(o->reason, sizeof o->reason, o->distorted ? "stretch: deliberate distortion" : "stretch: same ratio, no distortion");
      break;
    case NXCOMPAT_VIDEO_ASPECT_PRESERVE: {
      /* scale = min(dw/sw, dh/sh) in exact integer arithmetic */
      long cw, ch;
      if ((long)dw * sh <= (long)dh * sw) { cw = dw; ch = (long)dw * sh / sw; }   /* width-bound */
      else { ch = dh; cw = (long)dh * sw / sh; }                                     /* height-bound */
      o->content.w = (int)cw; o->content.h = (int)ch;
      o->content.x = (dw - (int)cw) / 2; o->content.y = (dh - (int)ch) / 2;
      snprintf(o->reason, sizeof o->reason, "preserve: largest centered rect");
      break; }
    case NXCOMPAT_VIDEO_ASPECT_CROP: {
      long cw, ch;
      if ((long)dw * sh >= (long)dh * sw) { cw = dw; ch = (long)dw * sh / sw; }
      else { ch = dh; cw = (long)dh * sw / sh; }
      o->content.w = (int)cw; o->content.h = (int)ch;
      o->content.x = (dw - (int)cw) / 2; o->content.y = (dh - (int)ch) / 2; /* <= 0: clipped */
      snprintf(o->reason, sizeof o->reason, "crop: centered, clipped by the drawable");
      break; }
    case NXCOMPAT_VIDEO_ASPECT_INTEGER: {
      int kx = dw / sw, ky = dh / sh, k = kx < ky ? kx : ky;
      if (k < 1) {
        o->unsupported = 1; o->integer_factor = 0;
        o->content.x = 0; o->content.y = 0; o->content.w = 0; o->content.h = 0;
        snprintf(o->reason, sizeof o->reason, "integer: no factor >= 1 fits (unsupported)");
        return 0;
      }
      o->integer_factor = k;
      o->content.w = sw * k; o->content.h = sh * k;
      o->content.x = (dw - o->content.w) / 2; o->content.y = (dh - o->content.h) / 2;
      snprintf(o->reason, sizeof o->reason, "integer: factor %d", k);
      break; }
    default:
      snprintf(o->reason, sizeof o->reason, "unknown policy"); return -1;
  }
  bars_of(o);
  return 0;
}

nxcompat_video_aspect nxcompat_video_auto_stretch(void) {
  return NXCOMPAT_VIDEO_ASPECT_STRETCH;
}

nxcompat_video_aspect nxcompat_video_auto_ratio_threshold(int dw, int dh, double threshold) {
  double rd;
  if (dw <= 0 || dh <= 0) return NXCOMPAT_VIDEO_ASPECT_PRESERVE; /* safe side */
  if (threshold <= 0.0) threshold = NXCOMPAT_VIDEO_AUTO_RATIO_THRESHOLD_DEFAULT;
  rd = (double)dw / (double)dh;
  return rd <= threshold ? NXCOMPAT_VIDEO_ASPECT_PRESERVE : NXCOMPAT_VIDEO_ASPECT_STRETCH;
}

nxcompat_video_aspect nxcompat_video_auto_epsilon(int sw, int sh, int dw, int dh, double epsilon) {
  double rs, rd, diff;
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NXCOMPAT_VIDEO_ASPECT_PRESERVE;
  if (epsilon < 0.0) epsilon = NXCOMPAT_VIDEO_AUTO_EPSILON_DEFAULT;
  rs = (double)sw / (double)sh; rd = (double)dw / (double)dh;
  diff = rd > rs ? rd - rs : rs - rd;
  return diff <= epsilon ? NXCOMPAT_VIDEO_ASPECT_ENGINE : NXCOMPAT_VIDEO_ASPECT_PRESERVE;
}

int nxcompat_video_drawable_to_source(const nxcompat_video_decision *d, int dx, int dy, int *sx, int *sy) {
  long rx, ry; int inside = 1;
  if (d == NULL || sx == NULL || sy == NULL || d->content.w <= 0 || d->content.h <= 0) return 0;
  rx = dx - d->content.x; ry = dy - d->content.y;
  if (rx < 0) { rx = 0; inside = 0; }
  if (ry < 0) { ry = 0; inside = 0; }
  if (rx >= d->content.w) { rx = d->content.w - 1; inside = 0; }
  if (ry >= d->content.h) { ry = d->content.h - 1; inside = 0; }
  *sx = (int)(rx * d->source_w / d->content.w);
  *sy = (int)(ry * d->source_h / d->content.h);
  return inside;
}

void nxcompat_video_source_to_drawable(const nxcompat_video_decision *d, int sx, int sy, int *dx, int *dy) {
  if (d == NULL || dx == NULL || dy == NULL || d->source_w <= 0 || d->source_h <= 0) return;
  *dx = d->content.x + (int)((long)sx * d->content.w / d->source_w);
  *dy = d->content.y + (int)((long)sy * d->content.h / d->source_h);
}

int nxcompat_video_receipt(const nxcompat_video_decision *d, const char *authority, const char *source, char *out, size_t cap) {
  int n;
  if (d == NULL || out == NULL || cap == 0u) return -1;
  n = snprintf(out, cap, "NX-VIDEO/1 authority=%s source=%s requested=%s effective=%s src=%dx%d drawable=%dx%d content=%d,%d,%d,%d bars=%d,%d,%d,%d integer=%d unsupported=%d distorted=%d reason=%s",
               authority ? authority : "?", source ? source : "?",
               nxcompat_video_aspect_name(d->requested), nxcompat_video_aspect_name(d->effective),
               d->source_w, d->source_h, d->drawable_w, d->drawable_h,
               d->content.x, d->content.y, d->content.w, d->content.h,
               d->bar_left, d->bar_right, d->bar_top, d->bar_bottom, d->integer_factor, d->unsupported, d->distorted, d->reason);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* ===================================================================== *
 * nxcompat 0.5.1 / V5 FV3 + 7A.2: owner election + resolution + readback.
 * Still pure: tokens and numbers in, a decision out. No I/O, no getenv,
 * no device names, no engine vocabulary.
 * ===================================================================== */

int nxcompat_video_authority_from_string(const char *t, nxcompat_video_authority *out) {
  if (out == NULL) return -1;
  if (t == NULL || t[0] == '\0') { *out = NXCOMPAT_VIDEO_AUTHORITY_NEXTOS; return 0; }
  if (strcmp(t, "nextos") == 0) *out = NXCOMPAT_VIDEO_AUTHORITY_NEXTOS;
  else if (strcmp(t, "engine") == 0) *out = NXCOMPAT_VIDEO_AUTHORITY_ENGINE;
  else if (strcmp(t, "synchronized") == 0) *out = NXCOMPAT_VIDEO_AUTHORITY_SYNCHRONIZED;
  else return -1;
  return 0;
}

const char *nxcompat_video_authority_name(nxcompat_video_authority a) {
  switch (a) {
    case NXCOMPAT_VIDEO_AUTHORITY_NEXTOS: return "nextos";
    case NXCOMPAT_VIDEO_AUTHORITY_ENGINE: return "engine";
    case NXCOMPAT_VIDEO_AUTHORITY_SYNCHRONIZED: return "synchronized";
    default: return "?";
  }
}

const char *nxcompat_video_source_name(nxcompat_video_source s) {
  switch (s) {
    case NXCOMPAT_VIDEO_SOURCE_NONE: return "none";
    case NXCOMPAT_VIDEO_SOURCE_PORT_ENV: return "port-env";
    case NXCOMPAT_VIDEO_SOURCE_SETTINGS: return "settings";
    case NXCOMPAT_VIDEO_SOURCE_NATIVE_CONFIG: return "native-config";
    case NXCOMPAT_VIDEO_SOURCE_AUTO: return "auto";
    case NXCOMPAT_VIDEO_SOURCE_PACKAGE_DEFAULT: return "package-default";
    default: return "?";
  }
}

static int nxcv_present(const char *t) { return t != NULL && t[0] != '\0'; }

static int nxcv_refuse(nxcompat_video_owner_decision *o, const char *why) {
  o->failed = 1;
  snprintf(o->reason, sizeof o->reason, "%s", why);
  return -1;
}

int nxcompat_video_resolve_owner(const nxcompat_video_owner_input *in,
                                 nxcompat_video_owner_decision *o) {
  nxcompat_video_aspect token = NXCOMPAT_VIDEO_ASPECT_ENGINE, effective;
  int asserted_by_settings_tier = 0;
  if (o == NULL) return -1;
  memset(o, 0, sizeof *o);
  o->api_version = NXCOMPAT_VIDEO_OWNER_API_VERSION;
  o->source = NXCOMPAT_VIDEO_SOURCE_NONE;
  if (in == NULL) return nxcv_refuse(o, "no owner input");
  if (nxcompat_video_authority_from_string(in->settings_authority, &o->authority) != 0)
    return nxcv_refuse(o, "video.authority token is unknown");
  o->cas_generation = in->cas_generation;

  /* Which tier speaks, in precedence order (port-env is the owner's last
   * word inside the settings tier: the hook is sourced after the file). */
  if (nxcv_present(in->port_env_aspect)) {
    if (nxcompat_video_aspect_from_string(in->port_env_aspect, &token) != 0)
      return nxcv_refuse(o, "port-env.sh video aspect token is unknown");
    o->source = NXCOMPAT_VIDEO_SOURCE_PORT_ENV;
    asserted_by_settings_tier = 1;
  } else if (nxcv_present(in->settings_aspect)) {
    if (nxcompat_video_aspect_from_string(in->settings_aspect, &token) != 0)
      return nxcv_refuse(o, "video.aspect token is unknown");
    o->source = NXCOMPAT_VIDEO_SOURCE_SETTINGS;
    asserted_by_settings_tier = 1;
  }

  /* 7A.2 election: under `engine` the native config owns the key and the
   * settings tier must stay in inherit. An assertion there is an OVERLAP
   * outside the election and fails BEFORE init -- no last-writer-wins. */
  if (o->authority == NXCOMPAT_VIDEO_AUTHORITY_ENGINE) {
    if (asserted_by_settings_tier && token != NXCOMPAT_VIDEO_ASPECT_ENGINE)
      return nxcv_refuse(o, "authority=engine but the settings tier asserts video.aspect (overlap outside the election)");
    o->requested = NXCOMPAT_VIDEO_ASPECT_ENGINE;
    o->source = in->native_config_present ? NXCOMPAT_VIDEO_SOURCE_NATIVE_CONFIG
                                          : NXCOMPAT_VIDEO_SOURCE_PACKAGE_DEFAULT;
    o->native_config_apply = 0; /* the engine already owns it */
    if (nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_ENGINE, in->source_w, in->source_h,
                                    in->drawable_w, in->drawable_h, &o->geometry) != 0)
      return nxcv_refuse(o, "invalid geometry");
    snprintf(o->reason, sizeof o->reason, "authority=engine: inherit, nothing asserted");
    return 0;
  }
  if (o->authority == NXCOMPAT_VIDEO_AUTHORITY_SYNCHRONIZED) {
    if (in->cas_generation == 0u)
      return nxcv_refuse(o, "authority=synchronized needs the video_config_generation it read (CAS)");
    if (!in->native_config_present)
      return nxcv_refuse(o, "authority=synchronized needs the declared native config present");
  }

  /* nextos / synchronized: one resolver, the tiers in order. */
  if (o->source == NXCOMPAT_VIDEO_SOURCE_NONE) {
    if (in->native_config_present && in->native_config_edited) {
      if (nxcompat_video_aspect_from_string(in->native_config_aspect, &token) != 0)
        return nxcv_refuse(o, "native config aspect token is unknown (translate it to the schema first)");
      o->source = NXCOMPAT_VIDEO_SOURCE_NATIVE_CONFIG;
    } else {
      token = NXCOMPAT_VIDEO_ASPECT_AUTO;
      o->source = NXCOMPAT_VIDEO_SOURCE_AUTO;
    }
  }
  o->requested = token;

  if (token == NXCOMPAT_VIDEO_ASPECT_AUTO) {
    if (!in->auto_algorithm_declared) {
      /* No total algorithm: reject the token instead of faking a fallback.
       * When nobody asked for `auto` explicitly we fall back to the package
       * default (that is a declared value, not a heuristic). */
      if (o->source == NXCOMPAT_VIDEO_SOURCE_AUTO) {
        effective = in->package_default;
        if (effective == NXCOMPAT_VIDEO_ASPECT_AUTO)
          return nxcv_refuse(o, "package default cannot be `auto`");
        o->source = NXCOMPAT_VIDEO_SOURCE_PACKAGE_DEFAULT;
        o->requested = effective;
      } else {
        return nxcv_refuse(o, "`auto` requested but this port declares no total auto algorithm");
      }
    } else {
      effective = in->auto_algorithm_result;
      if (effective == NXCOMPAT_VIDEO_ASPECT_AUTO)
        return nxcv_refuse(o, "the declared auto algorithm returned `auto`");
      o->via_auto = 1;
    }
  } else {
    effective = token;
  }

  if (nxcompat_video_content_rect(effective, in->source_w, in->source_h,
                                  in->drawable_w, in->drawable_h, &o->geometry) != 0)
    return nxcv_refuse(o, o->geometry.reason[0] ? o->geometry.reason : "invalid geometry");
  o->geometry.requested = o->requested;
  /* The native config is a projected sink under nextos, a CAS participant
   * under synchronized: either way the adapter must write the decision
   * there when the engine reads its own config. */
  o->native_config_apply = in->native_config_present ? 1 : 0;
  snprintf(o->reason, sizeof o->reason, "%s%s",
           o->geometry.unsupported ? "unsupported: " : "",
           o->via_auto ? "resolved by the declared auto algorithm"
                       : nxcompat_video_source_name(o->source));
  return 0;
}

int nxcompat_video_owner_receipt(const nxcompat_video_owner_decision *d, char *out, size_t cap) {
  char geometry[320];
  int n;
  if (d == NULL || out == NULL || cap == 0u) return -1;
  if (nxcompat_video_receipt(&d->geometry, nxcompat_video_authority_name(d->authority),
                             nxcompat_video_source_name(d->source), geometry, sizeof geometry) < 0)
    return -1;
  n = snprintf(out, cap, "%s via_auto=%d native_config_apply=%d cas_generation=%u failed=%d",
               geometry, d->via_auto, d->native_config_apply, d->cas_generation, d->failed);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

int nxcompat_video_readback_check(const nxcompat_video_owner_decision *want,
                                  const nxcompat_video_readback *got,
                                  char *out, size_t cap) {
  const char *mismatch = NULL;
  if (want == NULL || got == NULL) return -1;
  if (want->failed)
    mismatch = "decision";              /* 0.5.2 (review 2): a refused election never matches */
  else if (got->api_version != NXCOMPAT_VIDEO_OWNER_API_VERSION)
    mismatch = "api_version";           /* nothing was measured */
  else if (got->drawable_w != want->geometry.drawable_w ||
           got->drawable_h != want->geometry.drawable_h)
    mismatch = "drawable";
  else if (got->effective != want->geometry.effective)
    mismatch = "effective";
  else if (got->content.x != want->geometry.content.x ||
           got->content.y != want->geometry.content.y ||
           got->content.w != want->geometry.content.w ||
           got->content.h != want->geometry.content.h)
    mismatch = "content";
  else if (got->cas_generation != want->cas_generation)
    mismatch = "cas_generation";
  if (out != NULL && cap > 0u) {
    int n = snprintf(out, cap,
                     "NX-VIDEO-READBACK/1 authority=%s source=%s effective=%s "
                     "drawable=%dx%d content=%d,%d,%d,%d generation=%u match=%d mismatch=%s",
                     nxcompat_video_authority_name(want->authority),
                     nxcompat_video_source_name(want->source),
                     nxcompat_video_aspect_name(got->effective),
                     got->drawable_w, got->drawable_h,
                     got->content.x, got->content.y, got->content.w, got->content.h,
                     got->cas_generation, mismatch == NULL ? 1 : 0,
                     mismatch == NULL ? "none" : mismatch);
    if (n < 0 || (size_t)n >= cap) return -1;
  }
  return mismatch == NULL ? 0 : -1;
}
