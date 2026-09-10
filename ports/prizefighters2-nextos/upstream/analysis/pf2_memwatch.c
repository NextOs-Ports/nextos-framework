/*
 * Observe PF2 from outside its process and freeze it when libil2cpp's PairIP
 * constructor has filled every hidden .got.plt slot.
 *
 * This static x86_64 Linux helper runs as root inside Waydroid.  It does not
 * ptrace, inject, patch, or execute code in the game process.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define PACKAGE "com.koalitygame.prizefighters2"
#define IL2CPP_GOT_VADDR 0x02eb98c0UL
#define IL2CPP_GOT_SIZE  0x000011e0UL
#define IL2CPP_TEXT_PAGE 0x0118b000UL
#define IL2CPP_TEXT_SIZE 0x0000f000UL
#define UNITY_TEXT_PAGE  0x00397000UL
#define UNITY_TEXT_SIZE  0x0000d000UL
#define UNITY_GOT_VADDR  0x011b8100UL
#define UNITY_GOT_SIZE   0x00000a98UL

/* PairIP encrypts a window of writable data as well, with the same 0xc7b0
 * budget it spends on .text.  In libil2cpp that window is the IL2CPP
 * metadata-usage table (0x2f0e7a0..0x2f1af50); libunity's is at
 * 0x11cdbf8..0x11d2408.  Both are page-aligned outwards here so the dump also
 * carries clear bytes on each side -- that surrounding data is what lets the
 * offline splice prove its alignment instead of assuming it. */
#define IL2CPP_DATA_PAGE 0x02f0e000UL
#define IL2CPP_DATA_SIZE 0x0000d000UL
#define UNITY_DATA_PAGE  0x011cd000UL
#define UNITY_DATA_SIZE  0x00006000UL

/* A decrypted metadata-usage table is 55% zeros and 97.6% valid
 * `(kind << 29) | (index << 1) | 1` tokens; while still encrypted it has
 * neither.  Waiting on this instead of only on the GOT means the capture cannot
 * land between the two things DT_INIT does. */
static int usage_table_is_plaintext(const uint64_t *w, size_t n)
{
    size_t good = 0;
    for (size_t i = 0; i < n; i++) {
        if (w[i] == 0) {
            good++;
            continue;
        }
        if (w[i] >> 32)
            continue;
        unsigned kind = (unsigned)((w[i] >> 29) & 7);
        if ((w[i] & 1) && kind >= 1 && kind <= 7)
            good++;
    }
    return n && good * 100 / n >= 80;
}

static int numeric_name(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

static pid_t find_game(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    pid_t found = 0;
    while ((e = readdir(d)) != NULL) {
        if (!numeric_name(e->d_name))
            continue;
        char path[512], buf[256];
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = 0;
        if (strcmp(buf, PACKAGE) == 0) {
            found = (pid_t)strtol(e->d_name, NULL, 10);
            break;
        }
    }
    closedir(d);
    return found;
}

static uintptr_t module_base(pid_t pid, const char *soname)
{
    char path[64], line[8192];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "re");
    if (!f)
        return 0;
    uintptr_t best = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long start, end, off;
        char perms[8], name[4096] = "";
        int fields = sscanf(line, "%lx-%lx %7s %lx %*s %*s %4095s",
                            &start, &end, perms, &off, name);
        if (fields < 5 || off != 0 || !strstr(name, soname))
            continue;
        if (start < UINT64_C(0x400000000000) ||
            start >= UINT64_C(0x500000000000))
            continue;
        /*
         * Houdini maps the guest AArch64 ELF around 0x4000_0000_0000 and also
         * has host-side bridge mappings around 0x7f00_0000_0000.  The lower
         * matching address is the guest base whose vaddrs match the ELF.
         */
        if (!best || start < best)
            best = start;
    }
    fclose(f);
    return best;
}

static ssize_t read_remote(pid_t pid, uintptr_t address, void *buf, size_t len)
{
    struct iovec local = { buf, len };
    struct iovec remote = { (void *)address, len };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0);
}

static int write_remote_dump(pid_t pid, uintptr_t address, size_t len,
                             const char *path)
{
    unsigned char *buf = malloc(len);
    if (!buf)
        return -1;
    ssize_t got = read_remote(pid, address, buf, len);
    if (got != (ssize_t)len) {
        fprintf(stderr, "dump read %s: %zd/%zu: %s\n", path, got, len,
                got < 0 ? strerror(errno) : "short read");
        free(buf);
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "dump open %s: %s\n", path, strerror(errno));
        free(buf);
        return -1;
    }
    ssize_t put = write(fd, buf, len);
    close(fd);
    free(buf);
    if (put != (ssize_t)len) {
        fprintf(stderr, "dump write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "dumped %s (%zu bytes)\n", path, len);
    return 0;
}

static void copy_maps(pid_t pid)
{
    char src[64];
    snprintf(src, sizeof src, "/proc/%d/maps", (int)pid);
    int in = open(src, O_RDONLY | O_CLOEXEC);
    int out = open("/data/local/tmp/pf2/watch_maps.txt",
                   O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (in < 0 || out < 0) {
        if (in >= 0)
            close(in);
        if (out >= 0)
            close(out);
        return;
    }
    char buf[16384];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n)
            break;
    close(in);
    close(out);
}

int main(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "waiting for %s\n", PACKAGE);

    pid_t pid;
    struct timespec wait = { 0, 1000000 };
    while ((pid = find_game()) == 0)
        nanosleep(&wait, NULL);
    fprintf(stderr, "pid=%d\n", (int)pid);

    uintptr_t il2cpp = 0;
    while (!il2cpp) {
        if (kill(pid, 0) < 0) {
            fprintf(stderr, "process died before libil2cpp mapped\n");
            return 2;
        }
        il2cpp = module_base(pid, "/lib/arm64/libil2cpp.so");
    }
    fprintf(stderr, "libil2cpp=%#lx\n", (unsigned long)il2cpp);

    uint64_t got[IL2CPP_GOT_SIZE / sizeof(uint64_t)];
    int previous = -1;
    int saw_hidden = 0;
    int data_wait_reported = 0;
    for (;;) {
        ssize_t n = read_remote(pid, il2cpp + IL2CPP_GOT_VADDR,
                                got, sizeof got);
        if (n == (ssize_t)sizeof got) {
            int hidden = 0;
            for (size_t i = 3; i < sizeof got / sizeof got[0]; i++)
                if (got[i] != 0 && got[i] < UINT64_C(0x100000000))
                    hidden++;
            if (hidden != previous) {
                fprintf(stderr, "hidden-got=%d\n", hidden);
                previous = hidden;
            }
            if (hidden >= 100)
                saw_hidden = 1;
            if (saw_hidden && hidden == 0) {
                /* The GOT is done; make sure the data window has been
                 * decrypted too before freezing the process. */
                uint64_t probe[256];
                ssize_t p = read_remote(pid, il2cpp + 0x02f0e7a0,
                                        probe, sizeof probe);
                if (p == (ssize_t)sizeof probe &&
                    usage_table_is_plaintext(probe,
                                             sizeof probe / sizeof probe[0]))
                    break;
                if (!data_wait_reported) {
                    fprintf(stderr, "GOT complete; waiting for the encrypted "
                                    "data window to be decrypted\n");
                    data_wait_reported = 1;
                }
            }
        } else if (kill(pid, 0) < 0) {
            fprintf(stderr, "process died with hidden-got=%d\n", previous);
            return 3;
        }
    }

    if (kill(pid, SIGSTOP) < 0) {
        fprintf(stderr, "SIGSTOP: %s\n", strerror(errno));
        return 4;
    }
    fprintf(stderr, "STOPPED pid=%d after final hidden GOT slot\n", (int)pid);

    struct timespec settle = { 0, 1000000 };
    nanosleep(&settle, NULL);
    uintptr_t unity = module_base(pid, "/lib/arm64/libunity.so");
    copy_maps(pid);
    write_remote_dump(pid, il2cpp + IL2CPP_TEXT_PAGE, IL2CPP_TEXT_SIZE,
                      "/data/local/tmp/pf2/watch_il2cpp_text.bin");
    write_remote_dump(pid, il2cpp + IL2CPP_GOT_VADDR, IL2CPP_GOT_SIZE,
                      "/data/local/tmp/pf2/watch_il2cpp_got.bin");
    write_remote_dump(pid, il2cpp + IL2CPP_DATA_PAGE, IL2CPP_DATA_SIZE,
                      "/data/local/tmp/pf2/watch_il2cpp_data.bin");
    if (unity) {
        fprintf(stderr, "libunity=%#lx\n", (unsigned long)unity);
        write_remote_dump(pid, unity + UNITY_TEXT_PAGE, UNITY_TEXT_SIZE,
                          "/data/local/tmp/pf2/watch_unity_text.bin");
        write_remote_dump(pid, unity + UNITY_GOT_VADDR, UNITY_GOT_SIZE,
                          "/data/local/tmp/pf2/watch_unity_got.bin");
        write_remote_dump(pid, unity + UNITY_DATA_PAGE, UNITY_DATA_SIZE,
                          "/data/local/tmp/pf2/watch_unity_data.bin");
    }
    return 0;
}
