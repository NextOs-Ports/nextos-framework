#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>

// Keep libbsd's arc4random path independent of the glibc 2.25 getentropy
// wrapper. Linux getrandom is a kernel ABI; /dev/urandom covers old kernels.
extern "C" int getentropy(void *buffer, size_t length)
{
    if (length > 256) {
        errno = EIO;
        return -1;
    }

    unsigned char *cursor = static_cast<unsigned char *>(buffer);
    size_t remaining = length;
#ifdef SYS_getrandom
    while (remaining > 0) {
        long result = syscall(SYS_getrandom, cursor, remaining, 0);
        if (result > 0) {
            cursor += result;
            remaining -= static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        break;
    }
    if (remaining == 0)
        return 0;
#endif

    int descriptor;
    do {
        descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0)
        return -1;

    while (remaining > 0) {
        ssize_t result = read(descriptor, cursor, remaining);
        if (result > 0) {
            cursor += result;
            remaining -= static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        int saved_errno = result == 0 ? EIO : errno;
        close(descriptor);
        errno = saved_errno;
        return -1;
    }

    while (close(descriptor) < 0 && errno == EINTR) {
    }
    return 0;
}
