/* SPDX-License-Identifier: GPL-3.0-only */
/* nxcompat 0.5.1 / V5 FV3 + 7A.2 + matrix 9.4: nxcompat_video is the SINGLE
 * authority for aspect/resolution. Proven here:
 *   - the election nextos|engine|synchronized, with OVERLAP outside the
 *     election failing BEFORE init (never last-writer-wins);
 *   - the precedence port-env > settings > owner-EDITED native config >
 *     declared total `auto` > package default;
 *   - the Tearscape ratio rule reproduced THROUGH the framework on the four
 *     normative drawables plus portrait, so no port needs its own copy;
 *   - the readback, which cannot be satisfied by "the file was read".
 * Mutants killed are marked MUTANT: each asserts that the wrong rule would
 * produce a DIFFERENT observable, not merely a different label. */
#include "nxcompat_video.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

static int rect_is(const nxcompat_video_decision *d, int x, int y, int w, int h) {
  return d->content.x == x && d->content.y == y && d->content.w == w && d->content.h == h;
}

/* The Tearscape total algorithm (rule 2.2C), now applied by the CALLER and
 * owned by the framework: a port passes the result of the ONE algorithm it
 * declares, it never re-derives a ratio threshold of its own. */
static void tearscape_input(nxcompat_video_owner_input *in, int dw, int dh) {
  memset(in, 0, sizeof *in);
  in->source_w = 1280; in->source_h = 720;
  in->drawable_w = dw; in->drawable_h = dh;
  in->auto_algorithm_declared = 1;
  in->auto_algorithm_result = nxcompat_video_auto_ratio_threshold(
      dw, dh, NXCOMPAT_VIDEO_AUTO_RATIO_THRESHOLD_DEFAULT);
  in->package_default = NXCOMPAT_VIDEO_ASPECT_STRETCH; /* the shipped "ignore" */
}

int main(void) {
  nxcompat_video_owner_input in;
  nxcompat_video_owner_decision d, e;
  nxcompat_video_readback rb;
  char line[512];

  /* ---------------- election ---------------- */
  tearscape_input(&in, 720, 720);
  in.settings_authority = "engine";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.authority == NXCOMPAT_VIDEO_AUTHORITY_ENGINE &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_ENGINE &&
        rect_is(&d.geometry, 0, 0, 720, 720),
        "authority=engine: inherit, the drawable is the rect, nothing asserted");
  in.settings_aspect = "engine";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_ENGINE,
        "authority=engine + video.aspect=engine: inherit is not an overlap");
  in.settings_aspect = "preserve";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 && d.failed &&
        strstr(d.reason, "overlap outside the election") != NULL,
        "MUTANT killed: authority=engine with an asserted video.aspect FAILS before init (no last-writer-wins)");
  tearscape_input(&in, 720, 720);
  in.settings_authority = "synchronized";
  in.native_config_present = 1; in.native_config_aspect = "stretch";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 && d.failed &&
        strstr(d.reason, "CAS") != NULL,
        "authority=synchronized without the video_config_generation it read: refused (CAS)");
  in.cas_generation = 7u;
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 && d.cas_generation == 7u &&
        d.native_config_apply == 1,
        "authority=synchronized with a CAS generation: resolves and projects onto the native config");
  in.native_config_present = 0;
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 &&
        strstr(d.reason, "native config present") != NULL,
        "authority=synchronized without the declared native config: refused");
  tearscape_input(&in, 720, 720);
  in.settings_authority = "whatever";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 &&
        strstr(d.reason, "video.authority") != NULL,
        "unknown authority token: refused, named");
  tearscape_input(&in, 720, 720);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.authority == NXCOMPAT_VIDEO_AUTHORITY_NEXTOS,
        "absent video.authority is the documented default nextos (never a hidden value)");

  /* ---------------- precedence ---------------- */
  /* nothing asserted, no owner edit -> the declared auto algorithm decides */
  tearscape_input(&in, 720, 720);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 && d.via_auto &&
        d.source == NXCOMPAT_VIDEO_SOURCE_AUTO &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE &&
        rect_is(&d.geometry, 0, 157, 720, 405),
        "1:1 panel, nothing asserted: the declared auto algorithm gives preserve (0,157,720,405)");
  /* an UNEDITED native config is a package default: it does NOT outrank auto */
  in.native_config_present = 1; in.native_config_edited = 0;
  in.native_config_aspect = "stretch";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.source == NXCOMPAT_VIDEO_SOURCE_AUTO &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE,
        "an UNTOUCHED native config is a package default: auto still decides");
  /* an owner EDIT of the native config outranks auto */
  in.native_config_edited = 1;
  CHECK(nxcompat_video_resolve_owner(&in, &e) == 0 &&
        e.source == NXCOMPAT_VIDEO_SOURCE_NATIVE_CONFIG && !e.via_auto &&
        e.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
        rect_is(&e.geometry, 0, 0, 720, 720),
        "an owner EDIT of the native config outranks auto (Tearscape rule, now universal)");
  CHECK(memcmp(&d.geometry.content, &e.geometry.content, sizeof d.geometry.content) != 0,
        "MUTANT killed: ignoring the owner's native-config edit changes the CONTENT RECT, not just a label");
  /* the typed settings file outranks the native config */
  in.settings_aspect = "integer";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.source == NXCOMPAT_VIDEO_SOURCE_SETTINGS &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_INTEGER,
        "NEXTOSSETTINGS.txt outranks the owner-edited native config");
  /* the allowlisted port-env export is the owner's last word */
  in.port_env_aspect = "crop";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.source == NXCOMPAT_VIDEO_SOURCE_PORT_ENV &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_CROP,
        "the allowlisted port-env.sh export outranks the typed file (sourced last: the owner's last word)");
  CHECK(nxcompat_video_owner_receipt(&d, line, sizeof line) > 0 &&
        strstr(line, "NX-VIDEO/1 authority=nextos source=port-env requested=crop effective=crop") != NULL &&
        strstr(line, "via_auto=0") != NULL,
        "receipt NX-VIDEO/1 names the elected authority AND the winning source of the decision");
  /* a bad token anywhere is refused, named, never silently downgraded */
  tearscape_input(&in, 720, 720);
  in.settings_aspect = "keep";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 &&
        strstr(d.reason, "video.aspect") != NULL,
        "an engine-native token (Godot `keep`) in the typed file is refused: the schema names only");
  tearscape_input(&in, 720, 720);
  in.native_config_present = 1; in.native_config_edited = 1;
  in.native_config_aspect = "ignore";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 &&
        strstr(d.reason, "native config") != NULL,
        "an untranslated native-config value is refused: the port's 5-line translation is mandatory");
  /* `auto` with no declared algorithm is rejected, never quietly preserved */
  tearscape_input(&in, 720, 720);
  in.auto_algorithm_declared = 0; in.settings_aspect = "auto";
  CHECK(nxcompat_video_resolve_owner(&in, &d) == -1 &&
        strstr(d.reason, "no total auto algorithm") != NULL,
        "MUTANT killed: `auto` requested with no declared algorithm is REJECTED (never a quiet preserve)");
  in.settings_aspect = NULL;
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.source == NXCOMPAT_VIDEO_SOURCE_PACKAGE_DEFAULT && !d.via_auto &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH,
        "no algorithm and nobody asked: the declared package default, not a heuristic");

  /* ---------------- matrix 9.4: the four normative drawables + portrait ----
   * The Tearscape ratio rule now lives HERE. A Godot port only translates
   * the effective policy (preserve->keep, stretch->ignore). */
  tearscape_input(&in, 640, 480);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
        rect_is(&d.geometry, 0, 0, 640, 480) && d.geometry.distorted,
        "9.4 drawable 640x480 under auto: stretch (the R2-proved fill), reported distorted");
  tearscape_input(&in, 720, 720);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE &&
        rect_is(&d.geometry, 0, 157, 720, 405) &&
        d.geometry.bar_top == 157 && d.geometry.bar_bottom == 158,
        "9.4 drawable 720x720 under auto: preserve, 720x405 centred, bars 157/158");
  tearscape_input(&in, 1280, 720);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
        rect_is(&d.geometry, 0, 0, 1280, 720) && !d.geometry.distorted,
        "9.4 drawable 1280x720 under auto: stretch is a geometric no-op (not distorted)");
  tearscape_input(&in, 1920, 1080);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
        rect_is(&d.geometry, 0, 0, 1920, 1080) && !d.geometry.distorted,
        "9.4 drawable 1920x1080 under auto: stretch is a geometric no-op");
  tearscape_input(&in, 720, 1280);
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
        d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE &&
        rect_is(&d.geometry, 0, 437, 720, 405),
        "9.4 portrait/rotated surface 720x1280 under auto: preserve, centred 720x405");
  /* forcing preserve on a wide panel is a no-op; forcing stretch on 1:1 is not */
  tearscape_input(&in, 720, 720);
  in.settings_aspect = "stretch";
  nxcompat_video_resolve_owner(&in, &e);
  tearscape_input(&in, 720, 720);
  in.settings_aspect = "preserve";
  nxcompat_video_resolve_owner(&in, &d);
  CHECK(memcmp(&d.geometry.content, &e.geometry.content, sizeof d.geometry.content) != 0 &&
        rect_is(&d.geometry, 0, 157, 720, 405) && rect_is(&e.geometry, 0, 0, 720, 720),
        "MUTANT killed: swapping preserve/stretch on 720x720 moves the content rect");

  /* ---------------- readback (FV3) ---------------- */
  tearscape_input(&in, 720, 720);
  in.cas_generation = 3u;
  CHECK(nxcompat_video_resolve_owner(&in, &d) == 0, "decision for readback built");
  memset(&rb, 0, sizeof rb);
  CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == -1 &&
        strstr(line, "mismatch=api_version") != NULL,
        "MUTANT killed: an EMPTY readback is a mismatch -- 'the config file was read' never approves");
  rb.api_version = NXCOMPAT_VIDEO_OWNER_API_VERSION;
  rb.drawable_w = 720; rb.drawable_h = 720;
  rb.effective = NXCOMPAT_VIDEO_ASPECT_PRESERVE;
  rb.content = d.geometry.content;
  rb.cas_generation = 3u;
  CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == 0 &&
        strstr(line, "NX-VIDEO-READBACK/1 authority=nextos source=auto effective=preserve") != NULL &&
        strstr(line, "match=1 mismatch=none") != NULL,
        "readback matching the decision passes and names authority/source/effective/content");
  /* 0.5.2 (review 2): a decision that REFUSED to launch (failed=1) never
   * matches a readback -- even an all-zero readback of an all-zero geometry. */
  { nxcompat_video_owner_decision bad = d; nxcompat_video_readback zero; memset(&zero, 0, sizeof zero);
    bad.failed = 1; memset(&bad.geometry, 0, sizeof bad.geometry); zero.api_version = NXCOMPAT_VIDEO_OWNER_API_VERSION;
    CHECK(nxcompat_video_readback_check(&bad, &zero, line, sizeof line) == -1 && strstr(line, "mismatch=decision"), "MUTANT killed: a failed election matched by a zeroed readback (readback must refuse a refused decision)"); }
  rb.effective = NXCOMPAT_VIDEO_ASPECT_STRETCH;
  CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == -1 &&
        strstr(line, "mismatch=effective") != NULL,
        "MUTANT killed: the engine applying stretch where preserve was elected is reported, not approved");
  rb.effective = NXCOMPAT_VIDEO_ASPECT_PRESERVE;
  rb.content.h = 720;
  CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == -1 &&
        strstr(line, "mismatch=content") != NULL,
        "MUTANT killed: a content rect that fills the panel while preserve was elected is a mismatch");
  rb.content = d.geometry.content;
  rb.drawable_w = 1280;
  CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == -1 &&
        strstr(line, "mismatch=drawable") != NULL,
        "MUTANT killed: measuring the REQUESTED size instead of the real drawable is a mismatch");
  rb.drawable_w = 720; rb.cas_generation = 4u;
  CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == -1 &&
        strstr(line, "mismatch=cas_generation") != NULL,
        "MUTANT killed: a stale video_config_generation is a mismatch (no approval across generations)");

  /* the inverse transform for touch/cursor comes from the SAME decision */
  {
    int sx = -1, sy = -1;
    CHECK(nxcompat_video_drawable_to_source(&d.geometry, 360, 359, &sx, &sy) == 1 &&
          sx == 640 && sy == 359,
          "touch/cursor uses the inverse of the ELECTED rect (one decision, one transform)");
  }
  printf(fails ? "nxcompat-video-owner: FAIL\n" : "nxcompat-video-owner: OK\n");
  return fails ? 1 : 0;
}
