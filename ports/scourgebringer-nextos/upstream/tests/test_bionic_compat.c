#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int sdv_stat(const char *path, struct stat *buf);
extern int sdv_lstat(const char *path, struct stat *buf);
extern int sdv_fstat(int fd, struct stat *buf);
extern int sdv_fstatat(int dirfd, const char *path, struct stat *buf, int flags);
extern int sdv_mknod(const char *path, mode_t mode, dev_t dev);
extern size_t sdv_strlcpy(char *dst, const char *src, size_t size);
extern size_t sdv_strlcat(char *dst, const char *src, size_t size);
extern void sdv_arc4random_buf(void *buf, size_t size);
extern const char *sb_bionic_ctype;

typedef uint64_t bionic_sigset_t;
struct bionic_sigaction {
    int bsa_flags;
    void *bsa_handler;
    bionic_sigset_t bsa_mask;
    void *bsa_restorer;
};

extern int my_sigaction(int sig, const struct bionic_sigaction *act,
                        struct bionic_sigaction *oldact);
extern int my_sigemptyset(bionic_sigset_t *set);
extern int my_sigfillset(bionic_sigset_t *set);
extern int my_sigaddset(bionic_sigset_t *set, int sig);
extern int my_sigdelset(bionic_sigset_t *set, int sig);
extern int my_sigismember(const bionic_sigset_t *set, int sig);
extern int my_sigprocmask(int how, const bionic_sigset_t *set,
                          bionic_sigset_t *oldset);
extern int my_pthread_sigmask(int how, const bionic_sigset_t *set,
                              bionic_sigset_t *oldset);
extern int my_sigsuspend(const bionic_sigset_t *set);
extern int my_sigpending(bionic_sigset_t *set);

#define GUARD_BEFORE UINT64_C(0x1122334455667788)
#define GUARD_AFTER UINT64_C(0x8877665544332211)

struct guarded_sigset {
    uint64_t before;
    bionic_sigset_t set;
    uint64_t after;
};

struct guarded_sigaction {
    uint64_t before;
    struct bionic_sigaction action;
    uint64_t after;
};

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int sigset_guards_ok(const struct guarded_sigset *guarded) {
    return guarded->before == GUARD_BEFORE && guarded->after == GUARD_AFTER;
}

static void harmless_signal_handler(int signal_number) {
    (void)signal_number;
}

static int test_bionic_signal_bridge(void) {
    struct guarded_sigset guarded = {
        .before = GUARD_BEFORE,
        .set = UINT64_C(0xa5a5a5a5a5a5a5a5),
        .after = GUARD_AFTER,
    };

    if (sizeof(bionic_sigset_t) != 8 ||
        sizeof(struct bionic_sigaction) != 32)
        return fail("Bionic signal ABI sizes");

    if (my_sigemptyset(&guarded.set) != 0 || guarded.set != 0 ||
        !sigset_guards_ok(&guarded))
        return fail("sigemptyset writes outside Bionic sigset_t");
    if (my_sigfillset(&guarded.set) != 0 || guarded.set != UINT64_MAX ||
        !sigset_guards_ok(&guarded))
        return fail("sigfillset writes outside Bionic sigset_t");
    if (my_sigdelset(&guarded.set, 64) != 0 ||
        my_sigismember(&guarded.set, 64) != 0 ||
        my_sigaddset(&guarded.set, 64) != 0 ||
        my_sigismember(&guarded.set, 64) != 1 ||
        !sigset_guards_ok(&guarded))
        return fail("upper Bionic signal bit");

    bionic_sigset_t unchanged = guarded.set;
    errno = 0;
    if (my_sigaddset(&guarded.set, 65) != -1 || errno != EINVAL ||
        guarded.set != unchanged || !sigset_guards_ok(&guarded))
        return fail("invalid Bionic signal number");
    errno = 0;
    if (my_sigemptyset(NULL) != -1 || errno != EINVAL)
        return fail("sigemptyset NULL semantics");

    bionic_sigset_t original_mask;
    if (my_sigprocmask(SIG_SETMASK, NULL, &original_mask) != 0)
        return fail("read original process signal mask");

    bionic_sigset_t one_signal = 0;
    if (my_sigaddset(&one_signal, SIGUSR2) != 0)
        return fail("construct Bionic signal mask");
    if (my_sigprocmask(SIG_BLOCK, &one_signal, NULL) != 0) {
        my_sigprocmask(SIG_SETMASK, &original_mask, NULL);
        return fail("block signal through Bionic sigprocmask");
    }

    guarded.set = 0;
    if (my_sigprocmask(SIG_SETMASK, NULL, &guarded.set) != 0 ||
        my_sigismember(&guarded.set, SIGUSR2) != 1 ||
        !sigset_guards_ok(&guarded)) {
        my_sigprocmask(SIG_SETMASK, &original_mask, NULL);
        return fail("sigprocmask output ABI");
    }
    if (my_sigprocmask(SIG_SETMASK, &original_mask, NULL) != 0)
        return fail("restore original process signal mask");

    guarded.set = 0;
    errno = ERANGE;
    if (my_pthread_sigmask(SIG_SETMASK, NULL, &guarded.set) != 0 ||
        errno != ERANGE || !sigset_guards_ok(&guarded))
        return fail("pthread_sigmask return/errno/output ABI");

    guarded.set = UINT64_MAX;
    if (my_sigpending(&guarded.set) != 0 || !sigset_guards_ok(&guarded))
        return fail("sigpending output ABI");
    errno = 0;
    if (my_sigsuspend(NULL) != -1 || errno != EFAULT)
        return fail("sigsuspend NULL semantics");

    struct bionic_sigaction install;
    memset(&install, 0, sizeof install);
    install.bsa_flags = SA_RESTART;
    install.bsa_handler = (void *)harmless_signal_handler;
    if (my_sigemptyset(&install.bsa_mask) != 0 ||
        my_sigaddset(&install.bsa_mask, SIGUSR2) != 0)
        return fail("construct Bionic sigaction");

    struct guarded_sigaction previous = {
        .before = GUARD_BEFORE,
        .after = GUARD_AFTER,
    };
    if (my_sigaction(SIGUSR2, &install, &previous.action) != 0)
        return fail("install translated Bionic sigaction");
    if (previous.before != GUARD_BEFORE || previous.after != GUARD_AFTER) {
        my_sigaction(SIGUSR2, &previous.action, NULL);
        return fail("sigaction oldact writes outside Bionic layout");
    }
    if (my_sigaction(SIGUSR2, &previous.action, NULL) != 0)
        return fail("restore translated Bionic sigaction");

    return 0;
}

int main(void) {
    char root[] = "/tmp/scourge-bionic-test.XXXXXX";
    char file_path[512];
    char link_path[512];
    char fifo_path[512];
    char copy[5];
    char append[8] = "ab";
    unsigned char random_a[64] = {0};
    unsigned char random_b[64] = {0};
    struct stat info;
    int fd;

    if (!mkdtemp(root)) return fail("mkdtemp");
    snprintf(file_path, sizeof file_path, "%s/payload", root);
    snprintf(link_path, sizeof link_path, "%s/link", root);
    snprintf(fifo_path, sizeof fifo_path, "%s/fifo", root);
    fd = open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "data", 4) != 4)
        return fail("fixture file");

    if (sdv_stat(file_path, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size != 4)
        return fail("stat wrapper");
    if (sdv_fstat(fd, &info) != 0 || info.st_size != 4)
        return fail("fstat wrapper");
    if (sdv_fstatat(AT_FDCWD, file_path, &info, 0) != 0 ||
        !S_ISREG(info.st_mode))
        return fail("fstatat wrapper");
    if (symlink(file_path, link_path) != 0 ||
        sdv_lstat(link_path, &info) != 0 || !S_ISLNK(info.st_mode))
        return fail("lstat wrapper");
    if (sdv_mknod(fifo_path, S_IFIFO | 0600, 0) != 0 ||
        sdv_lstat(fifo_path, &info) != 0 || !S_ISFIFO(info.st_mode))
        return fail("mknod wrapper");

    if (sdv_strlcpy(copy, "abcdef", sizeof copy) != 6 ||
        strcmp(copy, "abcd") != 0)
        return fail("strlcpy semantics");
    if (sdv_strlcat(append, "cdefgh", sizeof append) != 8 ||
        strcmp(append, "abcdefg") != 0)
        return fail("strlcat semantics");

    if (!(sb_bionic_ctype['A' + 1] & 0x01) ||
        !(sb_bionic_ctype['a' + 1] & 0x02) ||
        !(sb_bionic_ctype['9' + 1] & 0x04) ||
        !(sb_bionic_ctype[' ' + 1] & 0x88) ||
        sb_bionic_ctype[0] != 0)
        return fail("bionic ctype table");

    sdv_arc4random_buf(random_a, sizeof random_a);
    sdv_arc4random_buf(random_b, sizeof random_b);
    if (memcmp(random_a, random_b, sizeof random_a) == 0)
        return fail("arc4random_buf did not produce independent seeds");

    if (test_bionic_signal_bridge() != 0)
        return 1;

    close(fd);
    unlink(fifo_path);
    unlink(link_path);
    unlink(file_path);
    rmdir(root);
    puts("bionic compat: old-glibc aliases and complete arm64 signal ABI OK");
    return 0;
}
