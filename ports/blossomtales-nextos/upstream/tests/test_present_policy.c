/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "present_policy.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *why)
{
    fprintf(stderr, "present-policy: FAIL: %s\n", why);
    exit(1);
}

static void write_settings(void)
{
    static const char body[] =
        "# NEXTOS_SETTINGS/2\n"
        "language=auto\n"
        "video.authority=nextos\n"
        "video.output_size=display\n"
        "video.aspect=stretch\n"
        "video.filter=engine\n"
        "video.invalid_policy=package_default\n";
    int fd = open("NEXTOSSETTINGS.txt", O_WRONLY | O_CREAT | O_EXCL, 0600);
    ssize_t written;
    if (fd < 0) fail("cannot create cwd fixture");
    written = write(fd, body, sizeof body - 1u);
    if (written != (ssize_t)(sizeof body - 1u) || close(fd) != 0)
        fail("cannot write cwd fixture");
}

int main(int argc, char **argv)
{
    const nxcompat_video_decision *d;
    int with_settings = argc == 2 && strcmp(argv[1], "settings") == 0;
    if (argc != 2 || (!with_settings && strcmp(argv[1], "fallback") != 0))
        fail("usage: settings|fallback");
    unsetenv("GAMEDIR");
    unsetenv("NX_VIDEO_ASPECT");
    if (with_settings) write_settings();
    if (sb_present_policy_requested() != NXCOMPAT_VIDEO_ASPECT_STRETCH)
        fail("stretch was not selected");
    if (strcmp(sb_present_policy_source(),
               with_settings ? "settings" : "package-default") != 0)
        fail("wrong policy source");
    d = sb_present_policy_decide(1280, 720, 640, 480);
    if (!d || d->effective != NXCOMPAT_VIDEO_ASPECT_STRETCH ||
        d->content.x != 0 || d->content.y != 0 ||
        d->content.w != 640 || d->content.h != 480 ||
        d->bar_left != 0 || d->bar_right != 0 ||
        d->bar_top != 0 || d->bar_bottom != 0)
        fail("stretch did not fill the drawable");
    printf("present-policy: PASS source=%s content=640x480 bars=0\n",
           sb_present_policy_source());
    return 0;
}
