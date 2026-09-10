#define _GNU_SOURCE
#include "save_migration.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define SB_SAVE_PATH_MAX 4096
#define SB_SAVE_MAX_BYTES (64LL * 1024LL * 1024LL)

static int sb_path(char *out, size_t capacity, const char *left,
                   const char *right)
{
    int length;

    if (!out || !capacity || !left || !left[0] || !right || !right[0])
        return 0;
    length = snprintf(out, capacity, "%s/%s", left, right);
    return length > 0 && (size_t)length < capacity;
}

static int sb_ensure_real_directory(const char *path)
{
    struct stat info;

    if (lstat(path, &info) == 0)
        return S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode);
    if (errno != ENOENT)
        return 0;
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return 0;
    if (lstat(path, &info) != 0)
        return 0;
    return S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode);
}

int sb_prepare_data_dirs(const char *data_dir)
{
    char config[SB_SAVE_PATH_MAX];
    char isolated[SB_SAVE_PATH_MAX];
    char cache[SB_SAVE_PATH_MAX];

    if (!data_dir || !data_dir[0] ||
        !sb_path(cache, sizeof cache, data_dir, "cache") ||
        !sb_path(config, sizeof config, data_dir, ".config") ||
        !sb_path(isolated, sizeof isolated, config, ".isolated-storage"))
        return 0;

    if (!sb_ensure_real_directory(data_dir) ||
        !sb_ensure_real_directory(cache) ||
        !sb_ensure_real_directory(config) ||
        !sb_ensure_real_directory(isolated)) {
        fprintf(stderr, "[save-path] diretorio gravavel invalido: %s\n",
                data_dir);
        return 0;
    }
    return 1;
}

static int sb_copy_regular_file_if_missing(const char *source,
                                           const char *destination)
{
    struct stat source_info;
    struct stat destination_info;
    char temporary[SB_SAVE_PATH_MAX] = {0};
    unsigned char buffer[32768];
    int source_fd = -1;
    int destination_fd = -1;
    int result = 0;
    int length;

    if (lstat(destination, &destination_info) == 0)
        return 1;
    if (errno != ENOENT)
        return 0;

    source_fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0)
        return errno == ENOENT ? 1 : 0;
    if (fstat(source_fd, &source_info) != 0 ||
        !S_ISREG(source_info.st_mode) || source_info.st_size < 0 ||
        source_info.st_size > SB_SAVE_MAX_BYTES)
        goto done;

    length = snprintf(temporary, sizeof temporary, "%s.migrate.%ld",
                      destination, (long)getpid());
    if (length <= 0 || (size_t)length >= sizeof temporary)
        goto done;
    destination_fd = open(temporary,
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (destination_fd < 0)
        goto done;

    for (;;) {
        ssize_t received = read(source_fd, buffer, sizeof buffer);
        size_t offset = 0;

        if (received < 0) {
            if (errno == EINTR) continue;
            goto done;
        }
        if (received == 0) break;
        while (offset < (size_t)received) {
            ssize_t written = write(destination_fd, buffer + offset,
                                    (size_t)received - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                goto done;
            }
            offset += (size_t)written;
        }
    }
    if (fsync(destination_fd) != 0 || close(destination_fd) != 0) {
        destination_fd = -1;
        goto done;
    }
    destination_fd = -1;

    /* A single-instance lock prevents an in-process race. Recheck anyway so
     * an existing user save can never be overwritten by migration. */
    if (lstat(destination, &destination_info) == 0) {
        result = 1;
        goto done;
    }
    if (errno != ENOENT || rename(temporary, destination) != 0)
        goto done;
    temporary[0] = '\0';
    result = 1;

done:
    if (destination_fd >= 0) close(destination_fd);
    if (source_fd >= 0) close(source_fd);
    if (temporary[0]) unlink(temporary);
    return result;
}

int sb_migrate_legacy_saves(const char *game_dir, const char *data_dir)
{
    static const char *const names[] = {
        "settings.ini", "gamepad.map", "0.sav"
    };
    char legacy_root[SB_SAVE_PATH_MAX];
    char data_config[SB_SAVE_PATH_MAX];
    char destination_root[SB_SAVE_PATH_MAX];
    int copied = 0;

    if (!game_dir || !game_dir[0] || !data_dir || !data_dir[0] ||
        !sb_path(legacy_root, sizeof legacy_root, game_dir,
                 "libs/.config/.isolated-storage") ||
        !sb_path(data_config, sizeof data_config, data_dir, ".config") ||
        !sb_path(destination_root, sizeof destination_root, data_config,
                 ".isolated-storage"))
        return 0;
    if (!sb_prepare_data_dirs(data_dir))
        return 0;

    for (size_t index = 0; index < sizeof names / sizeof names[0]; ++index) {
        char source[SB_SAVE_PATH_MAX];
        char destination[SB_SAVE_PATH_MAX];
        struct stat before;

        if (!sb_path(source, sizeof source, legacy_root, names[index]) ||
            !sb_path(destination, sizeof destination, destination_root,
                     names[index]))
            return 0;
        if (lstat(destination, &before) != 0 && errno == ENOENT) {
            struct stat legacy;
            if (lstat(source, &legacy) == 0 && S_ISREG(legacy.st_mode) &&
                !S_ISLNK(legacy.st_mode)) {
                if (!sb_copy_regular_file_if_missing(source, destination)) {
                    fprintf(stderr,
                            "[save-migration] falha ao preservar %s\n",
                            names[index]);
                    return 0;
                }
                ++copied;
                fprintf(stderr,
                        "[save-migration] %s copiado para data; legado mantido\n",
                        names[index]);
            }
        }
    }
    fprintf(stderr, "[save-path] data=%s migrados=%d\n", data_dir, copied);
    return 1;
}
