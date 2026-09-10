#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "env.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *) -1)
#endif

#define EXPORT __attribute__((visibility("default")))
#ifdef BUILD_DYNAMIC
#define EXPORTDYN __attribute__((visibility("default")))
#else
#define EXPORTDYN
#endif

#ifndef MAP_32BIT
#define MAP_32BIT   0x40
#endif

typedef void x64emu_t;
extern void* mapallmem;
extern int box64_is32bits;
void setProtection(uintptr_t addr, size_t size, uint32_t prot);
void freeProtection(uintptr_t addr, size_t size);
void* InternalMmap(void* addr, unsigned long length, int prot, int flags, int fd, ssize_t offset);
int InternalMunmap(void* addr, unsigned long length);
void* box_mmap(void *addr, unsigned long length, int prot, int flags, int fd, ssize_t offset);

void* my_mmap64(x64emu_t* emu, void *addr, unsigned long length, int prot, int flags, int fd, ssize_t offset);

extern int running32bits;
extern box64env_t box64env;

static int is_native_gpu_fd(int fd)
{
    struct stat st;
    if(fd < 0 || fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
        return 0;
    const unsigned int device_major = major(st.st_rdev);
    const unsigned int device_minor = minor(st.st_rdev);
    return (device_major == 10 && device_minor == 117) || device_major == 226;
}

static void* mmap_native_gpu_high(unsigned long length, int prot, int flags,
                                  int fd, ssize_t offset)
{
    /*
     * Box32 reserves native VAs above 4 GiB so ordinary host pointers stay
     * representable by the guest. Valhall r44p0 is the exception: its private
     * /dev/mali0 and DRM mappings must live in the native high address space.
     * Release only the exact GPU mapping slots, retaining low addresses for
     * libraries, strings, objects and every guest-visible pointer.
     */
    const uintptr_t alignment = 0x200000;
    const uintptr_t allocation = (length + alignment - 1) & ~(alignment - 1);
    static uintptr_t user_cursor = UINT64_C(0x7f00000000);
    static uintptr_t gpu_cursor = UINT64_C(0x6000000000);
    uintptr_t *cursor = offset == 0x2f000 ? &user_cursor : &gpu_cursor;
    const uintptr_t end =
        __atomic_fetch_sub(cursor, allocation + alignment, __ATOMIC_RELAXED);
    const uintptr_t start = (end - allocation) & ~(alignment - 1);

    InternalMunmap((void*)start, allocation);
    if(mapallmem)
        freeProtection(start, allocation);

    int native_flags = flags & ~MAP_32BIT;
    /*
     * The kbase driver rejects MAP_FIXED for the USER register page. With
     * every other high VA still reserved, a normal NULL-address mmap can
     * select only the bounded hole released above.
     */
    return InternalMmap(NULL, length, prot, native_flags, fd, offset);
}

EXPORT void* mmap64(void *addr, unsigned long length, int prot, int flags, int fd, ssize_t offset)
{
    void* ret;
    if(box64_is32bits && !addr && is_native_gpu_fd(fd))
        ret = mmap_native_gpu_high(length, prot, flags, fd, offset);
    else if(!addr && ((running32bits && BOX64ENV(mmap32)) || (flags&MAP_32BIT) || box64_is32bits))
        ret = box_mmap(addr, length, prot, flags | MAP_32BIT, fd, offset);
    else
        ret = InternalMmap(addr, length, prot, flags, fd, offset);
    if(ret!=MAP_FAILED && mapallmem)
        setProtection((uintptr_t)ret, length, prot);
    return ret;
}
EXPORT void* mmap(void *addr, unsigned long length, int prot, int flags, int fd, ssize_t offset) __attribute__((alias("mmap64")));

EXPORT int munmap(void* addr, unsigned long length)
{
    int ret = InternalMunmap(addr, length);
    if(!ret && mapallmem) {
        freeProtection((uintptr_t)addr, length);
    }
    return ret;
}
