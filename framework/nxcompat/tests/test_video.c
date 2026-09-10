/* SPDX-License-Identifier: GPL-3.0-only */
/* nxcompat 0.5.0 / V5 7A.3 + 9.4: content-rect geometry, auto algorithms,
 * inverse touch transform, receipt. Mutants of mission 8.3 killed here:
 * preserve<->stretch swap, rect from the REQUESTED size instead of the real
 * drawable, touch/cursor outside the same transform, letterbox as BLACK. */
#include "nxcompat_video.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static int rect_is(const nxcompat_video_decision *d, int x, int y, int w, int h) { return d->content.x == x && d->content.y == y && d->content.w == w && d->content.h == h; }
int main(void) {
  nxcompat_video_decision d, s; nxcompat_video_aspect a; char line[400]; int sx, sy, dx, dy;
  /* normative examples (7A.3) */
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 720, 720, &d) == 0 && rect_is(&d, 0, 157, 720, 405) && d.bar_top == 157 && d.bar_bottom == 158 && !d.distorted, "16:9 on 720x720 preserve = (0,157,720,405), bars 157/158");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 640, 480, &d) == 0 && rect_is(&d, 0, 60, 640, 360) && d.bar_top == 60 && d.bar_bottom == 60, "16:9 on 640x480 preserve = (0,60,640,360)");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 1280, 720, &d) == 0 && rect_is(&d, 0, 0, 1280, 720), "16:9 on 1280x720 preserve = full (no-op)");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 1920, 1080, &d) == 0 && rect_is(&d, 0, 0, 1920, 1080), "16:9 on 1920x1080 preserve = full");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 640, 360, 720, 1280, &d) == 0 && rect_is(&d, 0, 437, 720, 405), "portrait 720x1280 (rotated surface) preserve = centered 720x405");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_STRETCH, 1280, 720, 720, 720, &s) == 0 && rect_is(&s, 0, 0, 720, 720) && s.distorted && s.bar_top == 0, "stretch on 720x720 = whole drawable, reported distorted");
  CHECK(memcmp(&d.content, &s.content, sizeof d.content) != 0, "MUTANT killed: preserve and stretch give DISTINCT content rects on the same bytes");
  /* MUTANT: rect computed from the requested size instead of the real drawable */
  nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 1280, 720, &d);
  nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 720, 720, &s);
  CHECK(!rect_is(&s, 0, 0, 1280, 720) && rect_is(&d, 0, 0, 1280, 720), "MUTANT killed: requested 1280x720 vs real drawable 720x720 yield different rects (the drawable decides)");
  /* crop / integer */
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_CROP, 1280, 720, 720, 720, &d) == 0 && d.content.w == 1280 && d.content.h == 720 && d.content.x == -280 && d.content.y == 0 && d.bar_left == 0, "crop on 720x720: 1280x720 centered, clipped (x=-280), no bars");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_INTEGER, 640, 360, 1920, 1080, &d) == 0 && d.integer_factor == 3 && rect_is(&d, 0, 0, 1920, 1080), "integer 640x360 on 1920x1080 = factor 3");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_INTEGER, 640, 360, 720, 720, &d) == 0 && d.integer_factor == 1 && rect_is(&d, 40, 180, 640, 360), "integer 640x360 on 720x720 = factor 1 centered");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_INTEGER, 1280, 720, 720, 720, &d) == 0 && d.unsupported && d.integer_factor == 0, "integer 1280x720 on 720x720: unsupported, reported (never faked)");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_ENGINE, 1280, 720, 720, 720, &d) == 0 && rect_is(&d, 0, 0, 720, 720) && !d.distorted, "engine: inherit, nothing asserted");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_AUTO, 1280, 720, 720, 720, &d) == -1, "auto is not a concrete policy: refused until resolved by a declared algorithm");
  CHECK(nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 0, 720, 720, 720, &d) == -1, "invalid geometry refused");
  /* auto algorithms */
  CHECK(nxcompat_video_auto_stretch() == NXCOMPAT_VIDEO_ASPECT_STRETCH, "auto stretch: every drawable uses the complete panel; preserve remains an explicit owner choice");
  CHECK(nxcompat_video_auto_ratio_threshold(720, 720, 1.20) == NXCOMPAT_VIDEO_ASPECT_PRESERVE, "auto ratio (Tearscape): 1:1 -> preserve");
  CHECK(nxcompat_video_auto_ratio_threshold(640, 480, 1.20) == NXCOMPAT_VIDEO_ASPECT_STRETCH, "auto ratio: 4:3 (1.33) -> stretch (R2-proved fill kept)");
  CHECK(nxcompat_video_auto_ratio_threshold(1280, 720, 1.20) == NXCOMPAT_VIDEO_ASPECT_STRETCH, "auto ratio: 16:9 -> stretch (geometric no-op)");
  CHECK(nxcompat_video_auto_ratio_threshold(1280, 1024, 1.20) == NXCOMPAT_VIDEO_ASPECT_STRETCH && nxcompat_video_auto_ratio_threshold(720, 640, 1.20) == NXCOMPAT_VIDEO_ASPECT_PRESERVE, "auto ratio: 5:4 -> stretch, 9:8 -> preserve (deterministic)");
  CHECK(nxcompat_video_auto_epsilon(1280, 720, 720, 720, 0.01) == NXCOMPAT_VIDEO_ASPECT_PRESERVE, "auto epsilon (Blossom): 16:9 on 1:1 -> preserve");
  CHECK(nxcompat_video_auto_epsilon(1280, 720, 1920, 1080, 0.01) == NXCOMPAT_VIDEO_ASPECT_ENGINE, "auto epsilon: same ratio -> engine no-op");
  CHECK(nxcompat_video_auto_epsilon(1280, 720, 640, 480, 0.01) == NXCOMPAT_VIDEO_ASPECT_PRESERVE, "auto epsilon: 4:3 -> preserve");
  /* touch/cursor inverse on the SAME rect */
  nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_PRESERVE, 1280, 720, 720, 720, &d);
  CHECK(nxcompat_video_drawable_to_source(&d, 360, 359, &sx, &sy) == 1 && sx == 640 && sy == 359, "drawable centre (360,359) -> source (640,359) inside the content rect");
  CHECK(nxcompat_video_drawable_to_source(&d, 0, 157, &sx, &sy) == 1 && sx == 0 && sy == 0, "top-left of the content rect -> source (0,0)");
  CHECK(nxcompat_video_drawable_to_source(&d, 100, 10, &sx, &sy) == 0 && sy == 0, "a tap in the top bar is OUTSIDE (clamped to the edge)");
  nxcompat_video_source_to_drawable(&d, 640, 360, &dx, &dy);
  CHECK(dx == 360 && dy == 157 + 202, "source centre -> drawable (360,359): forward = inverse of the same rect");
  nxcompat_video_content_rect(NXCOMPAT_VIDEO_ASPECT_STRETCH, 1280, 720, 720, 720, &s);
  nxcompat_video_drawable_to_source(&s, 360, 360, &dx, &dy);
  CHECK(dx == 640 && dy == 360 && sy != dy, "MUTANT killed: touch transformed by a DIFFERENT policy than the present lands on a different source pixel");
  /* letterbox is not BLACK: the bars are declared, the content rect is separate */
  CHECK(d.bar_top + d.content.h + d.bar_bottom == d.drawable_h, "MUTANT killed: frame proof can sample the content rect and the opaque bars separately (bars + content = drawable)");
  /* receipt */
  CHECK(nxcompat_video_receipt(&d, "nextos", "settings", line, sizeof line) > 0 && strstr(line, "NX-VIDEO/1 authority=nextos source=settings requested=preserve effective=preserve src=1280x720 drawable=720x720 content=0,157,720,405 bars=0,0,157,158"), "receipt names requested/effective/drawable/content rect/bars");
  CHECK(nxcompat_video_aspect_from_string("preserve", &a) == 0 && a == NXCOMPAT_VIDEO_ASPECT_PRESERVE && nxcompat_video_aspect_from_string("keep", &a) == -1, "token parsing: schema names only");
  printf(fails ? "nxcompat-video: FAIL\n" : "nxcompat-video: OK\n"); return fails ? 1 : 0;
}
