/*
 * pairip_capture.c -- analysis-only recovery of the decrypted windows.
 *
 * Never entered by the shipped game path.  With TR_VM_CAPTURE=1 the loader
 * stops right after PairIP's startup program has run, lets each object's real
 * DT_INIT decrypt it in place, and writes the resulting plaintext of the three
 * measured windows next to the libraries.  Those files can later be
 * version-pinned so a release does not have to load libpairipcore at all.
 *
 * The acceptance rule is the one the PF2 work established: every byte outside
 * the window has to still match the file on disk.  A capture that also moved
 * bytes outside its window has caught the running process's own pointers, not
 * the object's static content, and is useless -- so it fails loudly here
 * instead of being pinned and debugged months later.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <setjmp.h>
#include <signal.h>

#include "nx_elf.h"
#include "tr.h"

/* Running the real DT_INIT is the point of this mode, and it is expected to
 * fault as soon as the decrypted code reaches a PLT slot the bootstrap did not
 * fill.  That fault is data, not a failure: everything decrypted up to that
 * instant is already in memory, so catch it and go on to the comparison. */
static sigjmp_buf init_escape;
static volatile sig_atomic_t init_faulted;

static void on_init_fault(int sig)
{
    init_faulted = sig;
    siglongjmp(init_escape, 1);
}

static void run_init_tolerating_faults(nx_mod *m)
{
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_init_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    init_faulted = 0;
    if (sigsetjmp(init_escape, 1) == 0)
        nx_run_init(m);
    if (init_faulted)
        fprintf(stderr, "[tr] capture: %s DT_INIT stopped on signal %d\n",
                m->name, (int)init_faulted);

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
}

/* Measured on Tightrope Theatre 1.0.5 arm64-v8a: 12 contiguous 4 KB pages
 * carrying the AArch64 cipher signature in each object, plus the writable
 * window in the application image. */
static const struct {
    const char *soname;
    const char *out;
    uint64_t vaddr;
    size_t size;
} WINDOWS[] = {
    { "libApplicationMain.so", "libApplicationMain.text", 0x6e08b0, 0xc000 },
    { "liblime.so",            "liblime.text",            0x19c800, 0xc000 },
};

static int write_file(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[tr] capture: cannot write %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    size_t done = 0;
    while (done < n) {
        ssize_t put = write(fd, (const unsigned char *)data + done, n - done);
        if (put < 0 && errno == EINTR)
            continue;
        if (put <= 0)
            break;
        done += (size_t)put;
    }
    close(fd);
    return done == n ? 0 : -1;
}

/* Read the same range straight from the file, for the outside-the-window
 * comparison and to prove the window really did change. */
static unsigned char *read_from_file(nx_mod *m, uint64_t vaddr, size_t size)
{
    int fd = open(m->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    unsigned char *buf = malloc(size);
    /* The objects here map every PT_LOAD at p_vaddr == p_offset, so the file
     * offset of a virtual address inside .text is the address itself. */
    if (!buf || pread(fd, buf, size, (off_t)vaddr) != (ssize_t)size) {
        free(buf);
        buf = NULL;
    }
    close(fd);
    return buf;
}

int tr_pairip_capture(void)
{
    int failures = 0;

    for (size_t i = 0; i < sizeof WINDOWS / sizeof *WINDOWS; i++) {
        nx_mod *m = nx_find_mod(WINDOWS[i].soname);
        if (!m) {
            fprintf(stderr, "[tr] capture: %s is not loaded\n",
                    WINDOWS[i].soname);
            failures++;
            continue;
        }
        /* Run this object's real DT_INIT now, so the decryption happens with
         * the interpreter already initialised. */
        run_init_tolerating_faults(m);

        const unsigned char *live = m->base + WINDOWS[i].vaddr;
        unsigned char *onfile = read_from_file(m, WINDOWS[i].vaddr,
                                               WINDOWS[i].size);
        if (!onfile) {
            fprintf(stderr, "[tr] capture: cannot re-read %s from %s\n",
                    WINDOWS[i].out, m->path);
            failures++;
            continue;
        }
        size_t changed = 0;
        for (size_t k = 0; k < WINDOWS[i].size; k++)
            if (live[k] != onfile[k])
                changed++;
        free(onfile);

        if (changed == 0) {
            fprintf(stderr, "[tr] capture: %s is byte-identical to the file -- "
                            "the window was NOT decrypted\n", WINDOWS[i].out);
            failures++;
            continue;
        }

        char path[1400];
        snprintf(path, sizeof path, "%s/lib/%s", tr_gamedir, WINDOWS[i].out);
        if (write_file(path, live, WINDOWS[i].size) != 0) {
            failures++;
            continue;
        }
        fprintf(stderr, "[tr] capture: %s (%zu bytes, %zu changed) -> %s\n",
                WINDOWS[i].soname, WINDOWS[i].size, changed, path);
    }
    return failures ? -1 : 0;
}
