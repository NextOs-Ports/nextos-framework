#!/usr/bin/env python3
"""Compile real guest open wrappers; no ALSA device or game data is required."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/bionic.c").read_text()
start = source.index("/* Guest-only PCM guard:")
end = source.index("static int my_stat(", start)
assert "M(open), M(open64), E(openat)" in source
assert "E(open)," not in source and "E(open64)," not in source

fixture = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
static const char *seen_path;
static int host_calls, seen_wide, seen_flags, seen_has_mode, host_result = 47;
static mode_t seen_mode;
static int record_open(int wide, const char *path, int flags, int has_mode, mode_t mode) {
    ++host_calls; seen_wide = wide; seen_path = path; seen_flags = flags;
    seen_has_mode = has_mode; seen_mode = mode;
    errno = ENOENT; return host_result;
}
static int host_open2(const char *path, int flags) {
    return record_open(0, path, flags, 0, 0);
}
static int host_open3(const char *path, int flags, mode_t mode) {
    return record_open(0, path, flags, 1, mode);
}
static int host_open64_2(const char *path, int flags) {
    return record_open(1, path, flags, 0, 0);
}
static int host_open64_3(const char *path, int flags, mode_t mode) {
    return record_open(1, path, flags, 1, mode);
}
/* Count actual host arguments: O_DIRECTORY alone must not consume a mode. */
#define OPEN_ARITY(_1, _2, _3, FUNCTION, ...) FUNCTION
#define open(...) OPEN_ARITY(__VA_ARGS__, host_open3, host_open2)(__VA_ARGS__)
#define open64(...) OPEN_ARITY(__VA_ARGS__, host_open64_3, host_open64_2)(__VA_ARGS__)
'''

checks = r'''
typedef int (*GuestOpen)(const char *, int, ...);
static void check_forward(GuestOpen guest, int wide, const char *path,
                          int flags, int has_mode, mode_t mode) {
    int before = host_calls;
    int result = has_mode ? guest(path, flags, mode) : guest(path, flags);
    assert(result == host_result && errno == ENOENT && host_calls == before + 1);
    assert(seen_path == path && seen_flags == flags && seen_wide == wide);
    assert(seen_has_mode == has_mode && seen_mode == mode);
}
int main(void) {
    GuestOpen guests[] = {my_open, my_open64};
    const char *pcm[] = {"/dev/snd/pcmC0D0p", "/dev/snd/pcmC0D0c",
        "/dev/snd/pcmC123D456p", "/dev/snd/pcmC00D01c"};
    const char *other[] = {NULL, "", "/dev/urandom", "/dev/input/event0",
        "save.dat", "/dev/snd/controlC0", "/dev/snd/pcm", "/dev/snd/pcmC",
        "/dev/snd/pcmCD0p", "/dev/snd/pcmC0Dp", "/dev/snd/pcmC0D",
        "/dev/snd/pcmC0D0", "/dev/snd/pcmC0D0P", "/dev/snd/pcmC0D0x",
        "/dev/snd/pcmC0D0pp", "/dev/snd/pcmC0D0p/", "/dev/snd/pcmC0D0p.so",
        "/dev/snd/pcmC-1D0p", "/dev/snd/pcmC0D-1p", "/dev/snd/pcmC+1D0p",
        "/dev/snd/pcmC0D+1p", "/dev/snd/pcmC0aD0p", "dev/snd/pcmC0D0p",
        "/other/dev/snd/pcmC0D0p", "/dev/snd//pcmC0D0p"};
    for (int wide = 0; wide < 2; ++wide) {
        GuestOpen guest = guests[wide];
        for (unsigned i = 0; i < sizeof pcm / sizeof *pcm; ++i) {
            int read_flags[] = {O_RDONLY, O_RDONLY | O_CLOEXEC,
                O_RDONLY | O_NONBLOCK, O_RDONLY | O_DIRECTORY};
            for (unsigned j = 0; j < sizeof read_flags / sizeof *read_flags; ++j) {
                int before = host_calls; errno = 0;
                assert(guest(pcm[i], read_flags[j]) == -1);
                assert(errno == EACCES && host_calls == before);
            }
            check_forward(guest, wide, pcm[i], O_WRONLY | O_CLOEXEC, 0, 0);
            check_forward(guest, wide, pcm[i], O_RDWR | O_NONBLOCK, 0, 0);
            check_forward(guest, wide, pcm[i], O_WRONLY | O_CREAT, 1, 0600);
        }
        for (unsigned i = 0; i < sizeof other / sizeof *other; ++i)
            check_forward(guest, wide, other[i], O_RDONLY | O_CLOEXEC, 0, 0);
        check_forward(guest, wide, "/tmp", O_RDONLY | O_DIRECTORY, 0, 0);
        check_forward(guest, wide, "save.dat", O_CREAT | O_WRONLY | O_EXCL, 1, 0640);
        check_forward(guest, wide, "save.dat", O_CREAT | O_RDONLY, 1, 0600);
        check_forward(guest, wide, "/tmp", O_TMPFILE | O_RDWR | O_CLOEXEC, 1, 0644);
        check_forward(guest, wide, "/tmp", O_TMPFILE | O_WRONLY, 1, 0600);
        host_result = -1;
        check_forward(guest, wide, "missing.dat", O_RDONLY, 0, 0);
        host_result = 47;
    }
    puts("PASS: guest-only exact PCM read guard; open/open64 dispatch, host flags/errno/results and create/tmpfile modes preserved");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="partyhard-pcm-open-", dir=os.getenv("TMPDIR")) as temp:
    c_file, binary = Path(temp) / "test.c", Path(temp) / "test"
    c_file.write_text(fixture + source[start:end] + checks)
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
