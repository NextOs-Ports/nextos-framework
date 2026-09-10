/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "present_policy.h"
#include "nxcompat_settings.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int resolved;
static nxcompat_video_aspect requested = NXCOMPAT_VIDEO_ASPECT_STRETCH;
static const char *source_name = "package-default";
static nxcompat_video_decision last;
static int have_last;
static int last_key[4];

static int read_settings(const char *gamedir, char *buf, size_t cap, size_t *len)
{
    char path[1024]; int fd; struct stat st; ssize_t got; size_t used = 0;
    if (snprintf(path, sizeof path, "%s/NEXTOSSETTINGS.txt", gamedir) >= (int)sizeof path) return -1;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size >= cap) { close(fd); return -2; }
    while (used < cap - 1) {
        got = read(fd, buf + used, cap - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
    }
    close(fd); buf[used] = '\0'; *len = used;
    return 0;
}

static void resolve(void)
{
    const char *env = getenv("NX_VIDEO_ASPECT");
    const char *gamedir = getenv("GAMEDIR");
    char gamedir_cwd[1024];
    static char text[NXCOMPAT_SETTINGS_MAX_BYTES + 1];
    size_t len = 0;
    nxcompat_video_aspect a;
    resolved = 1;
    /* nxbootstrap keeps GAMEDIR as a protected shell variable rather than
     * exporting it.  The sealed adapter still chdirs into the physical game
     * directory before exec, so use the same cwd fallback as input preinit. */
    if ((!gamedir || !*gamedir) && getcwd(gamedir_cwd, sizeof gamedir_cwd))
        gamedir = gamedir_cwd;
    if (env && *env) {
        if (nxcompat_video_aspect_from_string(env, &a) == 0 && a != NXCOMPAT_VIDEO_ASPECT_CROP && a != NXCOMPAT_VIDEO_ASPECT_INTEGER) {
            requested = a; source_name = "port-env";
            fprintf(stderr, "[sb/video] owner hook NX_VIDEO_ASPECT=%s\n", env);
            return;
        }
        fprintf(stderr, "[sb/video] NX_VIDEO_ASPECT=%s is not a policy this port implements (auto|engine|preserve|stretch); ignored\n", env);
    }
    if (gamedir && *gamedir && read_settings(gamedir, text, sizeof text, &len) == 0) {
        nxcompat_settings s; nxcompat_settings_error e;
        if (nxcompat_settings_parse2(text, len, &s, &e) == 0) {
            if (s.schema == 2u && s.video_aspect[0]) {
                if (nxcompat_video_aspect_from_string(s.video_aspect, &a) == 0 && a != NXCOMPAT_VIDEO_ASPECT_CROP && a != NXCOMPAT_VIDEO_ASPECT_INTEGER) {
                    requested = a; source_name = "settings";
                    fprintf(stderr, "[sb/video] NEXTOSSETTINGS.txt video.aspect=%s\n", s.video_aspect);
                    return;
                }
                fprintf(stderr, "[sb/video] NEXTOSSETTINGS.txt: video.aspect=%s is not implemented by this port; package default applied (invalid_policy=package_default)\n", s.video_aspect);
            }
        } else {
            /* invalid_policy=package_default: bytes untouched, diagnostic with line, default applied */
            fprintf(stderr, "[sb/video] NEXTOSSETTINGS.txt:%u: %s (E%d); package default applied, file NOT rewritten\n", e.line, e.what, e.code);
        }
    }
    requested = NXCOMPAT_VIDEO_ASPECT_STRETCH; source_name = "package-default";
}

nxcompat_video_aspect sb_present_policy_requested(void)
{
    if (!resolved) resolve();
    return requested;
}

const char *sb_present_policy_source(void)
{
    if (!resolved) resolve();
    return source_name;
}

const nxcompat_video_decision *sb_present_policy_decide(int sw, int sh, int dw, int dh)
{
    nxcompat_video_aspect req, eff; nxcompat_video_decision d; char line[400];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return have_last ? &last : NULL;
    if (have_last && last_key[0] == sw && last_key[1] == sh && last_key[2] == dw && last_key[3] == dh) return &last;
    req = sb_present_policy_requested();
    eff = req == NXCOMPAT_VIDEO_ASPECT_AUTO ? nxcompat_video_auto_epsilon(sw, sh, dw, dh, NXCOMPAT_VIDEO_AUTO_EPSILON_DEFAULT) : req;
    if (nxcompat_video_content_rect(eff, sw, sh, dw, dh, &d) != 0) return have_last ? &last : NULL;
    d.requested = req;
    last = d; have_last = 1;
    last_key[0] = sw; last_key[1] = sh; last_key[2] = dw; last_key[3] = dh;
    if (nxcompat_video_receipt(&d, "nextos", source_name, line, sizeof line) > 0) {
        const char *path = getenv("NXVIDEO_RECEIPT");
        fprintf(stderr, "%s\n", line);
        if (path && *path) {
            FILE *f = fopen(path, "a");
            if (f) { fputs(line, f); fputc('\n', f); fclose(f); }
        }
    }
    return &last;
}

const nxcompat_video_decision *sb_present_policy_last(void)
{
    return have_last ? &last : NULL;
}

int sb_present_policy_touch(float dx, float dy, float *sx, float *sy)
{
    int ix, iy, inside;
    if (!have_last || last.effective == NXCOMPAT_VIDEO_ASPECT_ENGINE || last.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH) {
        *sx = dx; *sy = dy; return 1;
    }
    inside = nxcompat_video_drawable_to_source(&last, (int)dx, (int)dy, &ix, &iy);
    /* the game believes its surface is the drawable: scale source -> surface */
    *sx = (float)ix * (float)last.drawable_w / (float)last.source_w;
    *sy = (float)iy * (float)last.drawable_h / (float)last.source_h;
    return inside;
}
