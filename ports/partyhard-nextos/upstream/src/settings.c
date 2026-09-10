/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "gb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static nxcompat_settings snapshot;
static int ready;
static const char *source_name = "package-default";

static void package_defaults(nxcompat_settings *out)
{
    memset(out, 0, sizeof *out);
    out->api_version = NXCOMPAT_SETTINGS_API_VERSION;
    out->schema = 2u;
    strcpy(out->language, "auto");
    strcpy(out->quality, "auto");
    strcpy(out->video_authority, "nextos");
    strcpy(out->video_output_size, "display");
    strcpy(out->video_aspect, "auto");
    strcpy(out->video_filter, "engine");
    strcpy(out->video_invalid_policy, "package_default");
}

const nxcompat_settings *st_settings_get(void)
{
    if (ready)
        return &snapshot;
    ready = 1;
    package_defaults(&snapshot);

    char path[sizeof st_gamedir + 32u];
    int n = snprintf(path, sizeof path, "%s/NEXTOSSETTINGS.txt", st_gamedir);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "[st/settings] path too long; source=package-default\n");
        return &snapshot;
    }

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[st/settings] owner file absent (%s); source=package-default\n",
                strerror(errno));
        return &snapshot;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0 ||
        (uintmax_t)sb.st_size > NXCOMPAT_SETTINGS_MAX_BYTES) {
        fprintf(stderr, "[st/settings] owner file unsafe/oversize; source=package-default\n");
        close(fd);
        return &snapshot;
    }

    char bytes[NXCOMPAT_SETTINGS_MAX_BYTES];
    size_t used = 0;
    while (used < (size_t)sb.st_size) {
        ssize_t got = read(fd, bytes + used, (size_t)sb.st_size - used);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    close(fd);

    nxcompat_settings parsed;
    nxcompat_settings_error error;
    memset(&error, 0, sizeof error);
    if (used == (size_t)sb.st_size &&
        nxcompat_settings_parse2(bytes, used, &parsed, &error) == 0) {
        snapshot = parsed;
        source_name = "settings";
        fprintf(stderr, "[st/settings] NEXTOS_SETTINGS/%u source=settings\n",
                snapshot.schema);
        return &snapshot;
    }

    nxcompat_settings_recovery recovery;
    char receipt[256];
    if (error.code == 0) {
        error.code = NXCOMPAT_SETTINGS_E_SYNTAX;
        snprintf(error.what, sizeof error.what, "short read");
    }
    if (nxcompat_settings_recover(NXCOMPAT_SETTINGS_INVALID_PACKAGE_DEFAULT,
                                  &error, 0, 0, &recovery) == 0 &&
        nxcompat_settings_recovery_receipt(&recovery, receipt,
                                           sizeof receipt) > 0)
        fprintf(stderr, "[st/settings] %s\n", receipt);
    fprintf(stderr,
            "[st/settings] NEXTOSSETTINGS.txt:%u: %s; owner bytes preserved; source=package-default\n",
            error.line, error.what[0] ? error.what : "invalid settings");
    return &snapshot;
}

const char *st_settings_source(void)
{
    (void)st_settings_get();
    return source_name;
}
