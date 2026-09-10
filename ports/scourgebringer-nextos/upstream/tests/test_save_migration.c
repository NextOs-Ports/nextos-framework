#define _GNU_SOURCE
#include "save_migration.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t length = strlen(text);
    ssize_t written = descriptor >= 0 ? write(descriptor, text, length) : -1;
    if (descriptor >= 0) close(descriptor);
    return written == (ssize_t)length;
}

static int read_equals(const char *path, const char *expected)
{
    char buffer[128];
    int descriptor = open(path, O_RDONLY);
    ssize_t received = descriptor >= 0 ? read(descriptor, buffer,
                                              sizeof buffer - 1) : -1;
    if (descriptor >= 0) close(descriptor);
    if (received < 0) return 0;
    buffer[received] = '\0';
    return strcmp(buffer, expected) == 0;
}

int main(void)
{
    char root[] = "/tmp/scourge-save-test.XXXXXX";
    char path[1024];
    char data[1024];
    char destination[2048];

    if (!mkdtemp(root)) return 1;
    snprintf(path, sizeof path, "%s/libs", root);
    if (mkdir(path, 0700) != 0) return 1;
    strncat(path, "/.config", sizeof path - strlen(path) - 1);
    if (mkdir(path, 0700) != 0) return 1;
    strncat(path, "/.isolated-storage", sizeof path - strlen(path) - 1);
    if (mkdir(path, 0700) != 0) return 1;
    strncat(path, "/settings.ini", sizeof path - strlen(path) - 1);
    if (!write_file(path, "Language=PB\n")) return 1;

    snprintf(data, sizeof data, "%s/data", root);
    if (!sb_migrate_legacy_saves(root, data)) return 1;
    snprintf(destination, sizeof destination,
             "%s/.config/.isolated-storage/settings.ini", data);
    if (!read_equals(destination, "Language=PB\n") ||
        !read_equals(path, "Language=PB\n"))
        return 1;

    /* Existing destination wins on every later launch. */
    if (!write_file(destination, "Language=FR\n") ||
        !sb_migrate_legacy_saves(root, data) ||
        !read_equals(destination, "Language=FR\n"))
        return 1;

    unlink(destination);
    snprintf(destination, sizeof destination,
             "%s/.config/.isolated-storage", data);
    rmdir(destination);
    snprintf(destination, sizeof destination, "%s/.config", data);
    rmdir(destination);
    snprintf(destination, sizeof destination, "%s/cache", data);
    rmdir(destination);
    rmdir(data);
    unlink(path);
    snprintf(path, sizeof path, "%s/libs/.config/.isolated-storage", root);
    rmdir(path);
    snprintf(path, sizeof path, "%s/libs/.config", root);
    rmdir(path);
    snprintf(path, sizeof path, "%s/libs", root);
    rmdir(path);
    rmdir(root);

    puts("save migration: legacy copied once, source preserved, destination wins");
    return 0;
}
