/* Android/bionic, EGL/GLES and system import bridge for Dead Trigger. */
#define _GNU_SOURCE

#include "dt.h"
#include "egl_sdl.h"
#include "framework_bridge.h"

#include "opensles_dt.h"

#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* pthread/sem bridges proven by the working Terraria ARM64 port. */
extern int pthread_mutex_init_fake(void **, const void *);
extern int pthread_mutex_destroy_fake(void **);
extern int pthread_mutex_lock_fake(void **);
extern int pthread_mutex_unlock_fake(void **);
extern int pthread_mutex_trylock_fake(void **);
extern int pthread_cond_init_fake(void **, const void *);
extern int pthread_cond_destroy_fake(void **);
extern int pthread_cond_wait_fake(void **, void **);
extern int pthread_cond_timedwait_fake(void **, void **, const struct timespec *);
extern int pthread_cond_signal_fake(void **);
extern int pthread_cond_broadcast_fake(void **);
extern int pthread_condattr_init_fake(void *);
extern int pthread_condattr_destroy_fake(void *);
extern int pthread_condattr_setclock_fake(void *, int);
extern int pthread_mutexattr_init_fake(void *);
extern int pthread_mutexattr_destroy_fake(void *);
extern int pthread_mutexattr_settype_fake(void *, int);
extern int pthread_rwlock_init_fake(void **, const void *);
extern int pthread_rwlock_destroy_fake(void **);
extern int pthread_rwlock_rdlock_fake(void **);
extern int pthread_rwlock_wrlock_fake(void **);
extern int pthread_rwlock_tryrdlock_fake(void **);
extern int pthread_rwlock_trywrlock_fake(void **);
extern int pthread_rwlock_unlock_fake(void **);
extern int pthread_create_fake(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_attr_init_fake(void *);
extern int pthread_attr_destroy_fake(void *);
extern int pthread_attr_setdetachstate_fake(void *, int);
extern int pthread_attr_setstacksize_fake(void *, size_t);
extern int pthread_attr_setschedparam_fake(void *, const void *);
extern int pthread_sigmask_fake(int, const void *, void *);
extern int pthread_once_fake(int *, void (*)(void));
extern int pthread_key_create_fake(unsigned *, void (*)(void *));
extern int pthread_key_delete_fake(unsigned);
extern void *pthread_getspecific_fake(unsigned);
extern int pthread_setspecific_fake(unsigned, const void *);
extern int pthread_detach_fake(pthread_t);
extern int pthread_join_fake(pthread_t, void **);
extern pthread_t pthread_self_fake(void);
extern int pthread_setname_np_fake(pthread_t, const char *);

extern int sh_sem_init(void *, int, unsigned);
extern int sh_sem_wait(void *);
extern int sh_sem_trywait(void *);
extern int sh_sem_timedwait(void *, const struct timespec *);
extern int sh_sem_post(void *);
extern int sh_sem_getvalue(void *, int *);
extern int sh_sem_destroy(void *);

static DynLibFunction g_imports[256];
static int g_import_count;
static int g_width = 1280;
static int g_height = 720;
static atomic_int g_cursor_x = 640;
static atomic_int g_cursor_y = 360;
static atomic_int g_cursor_visible;
static const int g_sl_record_tag = 0x44545243;
static const void *g_sl_iid_record = &g_sl_record_tag;

void dt_cursor_update(float x, float y, int visible) {
    int integer_x = (int)(x + 0.5f);
    int integer_y = (int)(y + 0.5f);
    if (integer_x < 0) integer_x = 0;
    if (integer_y < 0) integer_y = 0;
    if (integer_x >= g_width) integer_x = g_width - 1;
    if (integer_y >= g_height) integer_y = g_height - 1;
    atomic_store(&g_cursor_x, integer_x);
    atomic_store(&g_cursor_y, integer_y);
    atomic_store(&g_cursor_visible, visible != 0);
}

static void cursor_clear_rectangle(int x, int y, int width, int height,
                                   float red, float green, float blue) {
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > g_width)
        width = g_width - x;
    if (y + height > g_height)
        height = g_height - y;
    if (width <= 0 || height <= 0)
        return;
    glScissor(x, y, width, height);
    glClearColor(red, green, blue, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/*
 * Menu pointer: classic arrow with a dark outline and an amber fill that
 * matches the game's orange UI. Drawn as scanline runs of scissored clears
 * (the same primitive the old crosshair used — no shader/state risk inside
 * the game's context). 'X' = outline, 'o' = fill; hotspot = bitmap (0,0);
 * each cell is scaled 2x2 so the arrow reads well on a TV.
 */
static const char *const g_cursor_shape[] = {
    "X           ",
    "XX          ",
    "XoX         ",
    "XooX        ",
    "XoooX       ",
    "XooooX      ",
    "XoooooX     ",
    "XooooooX    ",
    "XoooooooX   ",
    "XooooooooX  ",
    "XoooooooooX ",
    "XooooooXXXXX",
    "XoooXooX    ",
    "XooX XooX   ",
    "XoX  XooX   ",
    "XX    XooX  ",
    "X     XooX  ",
    "       XX   ",
};

static void draw_gamepad_cursor(void) {
    if (!atomic_load(&g_cursor_visible))
        return;

    GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean color_mask[4];
    GLint old_scissor[4];
    GLfloat old_clear_color[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);

    int x = atomic_load(&g_cursor_x);
    int top_y = atomic_load(&g_cursor_y);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    const int scale = 2;
    const int rows = (int)(sizeof g_cursor_shape /
                           sizeof g_cursor_shape[0]);
    for (int row = 0; row < rows; ++row) {
        const char *cells = g_cursor_shape[row];
        int gl_y = g_height - (top_y + row * scale) - scale;
        for (int column = 0; cells[column];) {
            char cell = cells[column];
            if (cell == ' ') {
                ++column;
                continue;
            }
            int run_start = column;
            while (cells[column] == cell)
                ++column;
            int run_length = column - run_start;
            if (cell == 'X')
                cursor_clear_rectangle(x + run_start * scale, gl_y,
                                       run_length * scale, scale,
                                       0.06f, 0.05f, 0.04f);
            else
                cursor_clear_rectangle(x + run_start * scale, gl_y,
                                       run_length * scale, scale,
                                       1.0f, 0.68f, 0.13f);
        }
    }

    glColorMask(color_mask[0], color_mask[1],
                color_mask[2], color_mask[3]);
    glClearColor(old_clear_color[0], old_clear_color[1],
                 old_clear_color[2], old_clear_color[3]);
    glScissor(old_scissor[0], old_scissor[1],
              old_scissor[2], old_scissor[3]);
    if (!scissor_was_enabled)
        glDisable(GL_SCISSOR_TEST);
}

static EGLBoolean dt_egl_swap_buffers(EGLDisplay display, EGLSurface surface) {
    static int framework_graphics_ready;
    static int framework_graphics_failure_logged;

    if (!dt_sdl_video_active() && !framework_graphics_ready) {
        if (dt_framework_publish_current_graphics() == 0) {
            framework_graphics_ready = 1;
        } else if (!framework_graphics_failure_logged) {
            framework_graphics_failure_logged = 1;
            fprintf(stderr,
                    "[deadtrigger/framework] graphics receipt pending\n");
        }
    }
    draw_gamepad_cursor();
    if (dt_sdl_video_active())
        return dt_sdl_swap_buffers(display, surface);
    return eglSwapBuffers(display, surface);
}

static void add_import(const char *name, void *function) {
    if (g_import_count >= (int)(sizeof g_imports / sizeof g_imports[0])) {
        fprintf(stderr, "[imports] tabela cheia em %s\n", name);
        abort();
    }
    g_imports[g_import_count].symbol = (char *)name;
    g_imports[g_import_count].func = (uintptr_t)function;
    ++g_import_count;
}

DynLibFunction *dt_imports(void) {
    return g_imports;
}

int dt_import_count(void) {
    return g_import_count;
}

/* ---------------- paths / Android application storage ---------------- */

static int path_has(const char *path, const char *needle) {
    return path && strstr(path, needle) != NULL;
}

const char *dt_map_path(const char *path, char *buffer, size_t buffer_size) {
    if (!path || !*path)
        return path;
    const char *root = dt_game_root();
    size_t root_length = strlen(root);
    if (!strncmp(path, root, root_length) &&
        (path[root_length] == '/' || path[root_length] == '\0'))
        return path;

    if (!strncmp(path, "/data/local/tmp", 15)) {
        snprintf(buffer, buffer_size, "/tmp%s", path + 15);
        return buffer;
    }

    if (path_has(path, "com.madfingergames.deadtrigger") &&
        (path_has(path, "/files") || path_has(path, "/cache") ||
         path_has(path, "/shared_prefs") ||
         path_has(path, "/Android/data/"))) {
        const char *leaf = strrchr(path, '/');
        snprintf(buffer, buffer_size, "%s/userdata/%s", root,
                 leaf && leaf[1] ? leaf + 1 : "data");
        return buffer;
    }

    size_t length = strlen(path);
    if ((length >= 4 && !strcmp(path + length - 4, ".apk")) ||
        path_has(path, "/base.apk")) {
        snprintf(buffer, buffer_size, "%s/game.apk", root);
        return buffer;
    }

    const char *data = strstr(path, "assets/bin/Data/");
    if (data) {
        snprintf(buffer, buffer_size, "%s/%s", root, data);
        return buffer;
    }
    data = strstr(path, "bin/Data/");
    if (data) {
        snprintf(buffer, buffer_size, "%s/assets/%s", root, data);
        return buffer;
    }
    if (!strncmp(path, "assets/", 7)) {
        snprintf(buffer, buffer_size, "%s/%s", root, path);
        return buffer;
    }

    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    if (!strcmp(leaf, "global-metadata.dat")) {
        snprintf(buffer, buffer_size,
                 "%s/assets/bin/Data/Managed/Metadata/global-metadata.dat",
                 root);
        return buffer;
    }
    if (!strncmp(leaf, "globalgamemanagers", 18) ||
        !strncmp(leaf, "level", 5) ||
        !strncmp(leaf, "sharedassets", 12) ||
        !strcmp(leaf, "boot.config") ||
        !strcmp(leaf, "unity default resources") ||
        !strcmp(leaf, "unity_builtin_extra") ||
        strstr(leaf, ".assets") || strstr(leaf, ".resS") ||
        strstr(leaf, ".resource")) {
        snprintf(buffer, buffer_size, "%s/assets/bin/Data/%s", root, leaf);
        if (access(buffer, F_OK) == 0)
            return buffer;
    }
    return path;
}

static int io_trace(void) {
    static int value = -1;
    if (value < 0)
        value = getenv("DT_IO_TRACE") ? 1 : 0;
    return value;
}

static int dt_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    int fd = open(real, flags, mode);
    if (io_trace() && path &&
        (fd < 0 || real != path || strstr(path, "Data") ||
         strstr(path, ".apk")))
        fprintf(stderr, "[io] open %s%s%s -> %d (%s)\n", path,
                real != path ? " => " : "", real != path ? real : "",
                fd, fd < 0 ? strerror(errno) : "ok");
    return fd;
}

static FILE *dt_fopen(const char *path, const char *mode) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    FILE *file = fopen(real, mode);
    if (io_trace() && path && (file == NULL || real != path))
        fprintf(stderr, "[io] fopen %s%s%s -> %p\n", path,
                real != path ? " => " : "", real != path ? real : "",
                (void *)file);
    return file;
}

static int dt_stat(const char *path, void *status) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    int result = stat(real, (struct stat *)status);
    if (io_trace() && path && (result < 0 || real != path))
        fprintf(stderr, "[io] stat %s%s%s -> %d\n", path,
                real != path ? " => " : "", real != path ? real : "",
                result);
    return result;
}

static int dt_lstat(const char *path, void *status) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    return lstat(real, (struct stat *)status);
}

static int dt_fstat(int descriptor, void *status) {
    return fstat(descriptor, (struct stat *)status);
}

static int dt_access(const char *path, int mode) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    return access(real, mode);
}

static int dt_mkdir(const char *path, mode_t mode) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    return mkdir(real, mode);
}

static int dt_remove(const char *path) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    return remove(real);
}

static int dt_unlink(const char *path) {
    char mapped[PATH_MAX];
    const char *real = dt_map_path(path, mapped, sizeof mapped);
    return unlink(real);
}

static int dt_rename(const char *from, const char *to) {
    char mapped_from[PATH_MAX], mapped_to[PATH_MAX];
    const char *real_from = dt_map_path(from, mapped_from, sizeof mapped_from);
    const char *real_to = dt_map_path(to, mapped_to, sizeof mapped_to);
    return rename(real_from, real_to);
}

/* ---------------- bionic stdio LP64 ---------------- */

#define BIONIC_FILE_SIZE 152
static unsigned char g_bionic_sF[3 * BIONIC_FILE_SIZE]
    __attribute__((aligned(16)));

static FILE *host_file(void *opaque) {
    uintptr_t pointer = (uintptr_t)opaque;
    uintptr_t base = (uintptr_t)g_bionic_sF;
    if (pointer >= base && pointer < base + sizeof g_bionic_sF) {
        size_t index = (pointer - base) / BIONIC_FILE_SIZE;
        return index == 0 ? stdin : index == 1 ? stdout : stderr;
    }
    return (FILE *)opaque;
}

static int std_fclose(void *file) {
    FILE *host = host_file(file);
    if (host == stdin || host == stdout || host == stderr)
        return fflush(host);
    return fclose(host);
}
static int std_feof(void *file) { return feof(host_file(file)); }
static int std_ferror(void *file) { return ferror(host_file(file)); }
static void std_clearerr(void *file) { clearerr(host_file(file)); }
static int std_fflush(void *file) {
    return file ? fflush(host_file(file)) : fflush(NULL);
}
static char *std_fgets(char *text, int size, void *file) {
    return fgets(text, size, host_file(file));
}
static int std_fputc(int character, void *file) {
    return fputc(character, host_file(file));
}
static int std_fputs(const char *text, void *file) {
    return fputs(text, host_file(file));
}
static size_t std_fread(void *data, size_t size, size_t count, void *file) {
    return fread(data, size, count, host_file(file));
}
static size_t std_fwrite(const void *data, size_t size, size_t count,
                         void *file) {
    return fwrite(data, size, count, host_file(file));
}
static int std_fseek(void *file, long offset, int origin) {
    return fseek(host_file(file), offset, origin);
}
static int std_fseeko(void *file, off_t offset, int origin) {
    return fseeko(host_file(file), offset, origin);
}
static long std_ftell(void *file) { return ftell(host_file(file)); }
static off_t std_ftello(void *file) { return ftello(host_file(file)); }
static int std_getc(void *file) { return getc(host_file(file)); }
static int std_setvbuf(void *file, char *buffer, int mode, size_t size) {
    return setvbuf(host_file(file), buffer, mode, size);
}
static int std_fprintf(void *file, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(host_file(file), format, arguments);
    va_end(arguments);
    return result;
}
static int std_vfprintf(void *file, const char *format, va_list arguments) {
    return vfprintf(host_file(file), format, arguments);
}
static int std_fscanf(void *file, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(host_file(file), format, arguments);
    va_end(arguments);
    return result;
}

/* ---------------- libc ABI differences ---------------- */

static int *dt_errno(void) {
    return __errno_location();
}

static size_t dt_strlcpy(char *destination, const char *source, size_t size) {
    size_t length = strlen(source);
    if (size) {
        size_t copy = length < size - 1 ? length : size - 1;
        memcpy(destination, source, copy);
        destination[copy] = '\0';
    }
    return length;
}

static int dt_strerror_r(int error, char *buffer, size_t size) {
    if (!buffer || !size)
        return EINVAL;
    const char *text = strerror(error);
    if (!text)
        text = "Unknown error";
    dt_strlcpy(buffer, text, size);
    return 0;
}

static void dt_fd_set_chk(int fd, fd_set *set, size_t set_size) {
    if (fd >= 0 && (size_t)fd < set_size * 8)
        FD_SET(fd, set);
}

static int dt_fd_isset_chk(int fd, fd_set *set, size_t set_size) {
    return fd >= 0 && (size_t)fd < set_size * 8 ? FD_ISSET(fd, set) : 0;
}

static long dt_sysconf(int name) {
    switch (name) {
        case 6: return 100;          /* _SC_CLK_TCK */
        case 39:
        case 40: return 4096;        /* bionic PAGE_SIZE/PAGESIZE */
        case 96:
        case 97: {
            long processors = sysconf(_SC_NPROCESSORS_ONLN);
            return processors > 0 ? processors : 4;
        }
        case 98: {
            long pages = sysconf(_SC_PHYS_PAGES);
            return pages > 0 ? pages : (1024L * 1024 * 1024) / 4096;
        }
        case 99: {
            long pages = sysconf(_SC_AVPHYS_PAGES);
            return pages > 0 ? pages : (512L * 1024 * 1024) / 4096;
        }
        default: return sysconf(name);
    }
}

/*
 * Bionic exports pthread_atfork while modern glibc only exposes its internal
 * registration entry point. The game never forks; accepting the handlers is
 * therefore equivalent for its lifetime and avoids leaving a poisoned GOT
 * slot in IL2CPP.
 */
static int dt_pthread_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void)) {
    (void)prepare;
    (void)parent;
    (void)child;
    return 0;
}

/*
 * Unity's Boehm GC asks pthread_getattr_np + pthread_attr_getstack for the
 * real bounds of every thread it scans. pthread_attr_t is ABI-private and
 * differs between bionic and glibc; passing it through (or inventing a range
 * around the current SP) can hide live managed roots from the collector.
 *
 * Android ARM64's guest object is 56 bytes. Query the target glibc thread
 * with a real host pthread_attr_t, then serialize only the fields consumed by
 * bionic's pthread_attr_getstack into the guest layout.
 */
#define DT_BIONIC_PTHREAD_ATTR64_SIZE 56
#define DT_BIONIC_PTHREAD_ATTR64_STACK_BASE 8
#define DT_BIONIC_PTHREAD_ATTR64_STACK_SIZE 16
#define DT_BIONIC_PTHREAD_ATTR64_GUARD_SIZE 24

static void dt_attr64_store(void *attribute, size_t offset,
                            const void *value, size_t bytes) {
    memcpy((unsigned char *)attribute + offset, value, bytes);
}

static void dt_attr64_load(const void *attribute, size_t offset,
                           void *value, size_t bytes) {
    memcpy(value, (const unsigned char *)attribute + offset, bytes);
}

static int dt_pthread_getattr_np(pthread_t thread, void *guest_attribute) {
    if (!guest_attribute)
        return EINVAL;

    pthread_attr_t host_attribute;
    void *stack_base = NULL;
    size_t stack_size = 0;
    size_t guard_size = 0;
    int result = pthread_getattr_np(thread, &host_attribute);
    if (!result) {
        result = pthread_attr_getstack(
            &host_attribute, &stack_base, &stack_size);
        if (!result)
            (void)pthread_attr_getguardsize(
                &host_attribute, &guard_size);
        pthread_attr_destroy(&host_attribute);
    }

    memset(guest_attribute, 0, DT_BIONIC_PTHREAD_ATTR64_SIZE);
    if (!result) {
        dt_attr64_store(
            guest_attribute, DT_BIONIC_PTHREAD_ATTR64_STACK_BASE,
            &stack_base, sizeof stack_base);
        dt_attr64_store(
            guest_attribute, DT_BIONIC_PTHREAD_ATTR64_STACK_SIZE,
            &stack_size, sizeof stack_size);
        dt_attr64_store(
            guest_attribute, DT_BIONIC_PTHREAD_ATTR64_GUARD_SIZE,
            &guard_size, sizeof guard_size);
    }

    if (getenv("DT_STACK_TRACE") || result) {
        fprintf(stderr,
                "[stack] getattr target=%lx current=%lx base=%p "
                "size=%zu top=%p guard=%zu result=%d\n",
                (unsigned long)thread,
                (unsigned long)pthread_self(), stack_base, stack_size,
                stack_base
                    ? (void *)((unsigned char *)stack_base + stack_size)
                    : NULL,
                guard_size, result);
    }
    return result;
}

static int dt_pthread_attr_getstack(
    const void *guest_attribute, void **stack_address, size_t *stack_size) {
    if (!guest_attribute)
        return EINVAL;

    void *base = NULL;
    size_t size = 0;
    dt_attr64_load(
        guest_attribute, DT_BIONIC_PTHREAD_ATTR64_STACK_BASE,
        &base, sizeof base);
    dt_attr64_load(
        guest_attribute, DT_BIONIC_PTHREAD_ATTR64_STACK_SIZE,
        &size, sizeof size);
    if (stack_address)
        *stack_address = base;
    if (stack_size)
        *stack_size = size;

    int result = base && size ? 0 : EINVAL;
    if (getenv("DT_STACK_TRACE") || result)
        fprintf(stderr,
                "[stack] attr_getstack base=%p size=%zu result=%d\n",
                base, size, result);
    return result;
}

/* _setjmp does not save the signal mask and fits in bionic's LP64 buffer. */
extern int _setjmp(void *);
extern void _longjmp(void *, int) __attribute__((noreturn));
static int dt_setjmp(void *environment) { return _setjmp(environment); }
static void dt_longjmp(void *environment, int value) {
    _longjmp(environment, value);
}

struct bionic_sigaction {
    int flags;
    void *handler;
    unsigned long mask;
    void *restorer;
};

static void mask_to_host(unsigned long mask, sigset_t *host) {
    sigemptyset(host);
    for (int signal_number = 1; signal_number <= 64; ++signal_number)
        if (mask & (1UL << (signal_number - 1)))
            sigaddset(host, signal_number);
}

static unsigned long mask_from_host(const sigset_t *host) {
    unsigned long mask = 0;
    for (int signal_number = 1; signal_number <= 64; ++signal_number)
        if (sigismember(host, signal_number) == 1)
            mask |= 1UL << (signal_number - 1);
    return mask;
}

static int dt_sigaction(int signal_number,
                        const struct bionic_sigaction *action,
                        struct bionic_sigaction *old_action) {
    struct sigaction host_action, host_old;
    struct sigaction *host_action_pointer = NULL;
    struct sigaction *host_old_pointer = NULL;
    if (action) {
        memset(&host_action, 0, sizeof host_action);
        host_action.sa_flags = action->flags;
        mask_to_host(action->mask, &host_action.sa_mask);
        if (action->flags & SA_SIGINFO)
            host_action.sa_sigaction =
                (void (*)(int, siginfo_t *, void *))action->handler;
        else
            host_action.sa_handler = (void (*)(int))action->handler;
        host_action_pointer = &host_action;
    }
    if (old_action) {
        memset(&host_old, 0, sizeof host_old);
        host_old_pointer = &host_old;
    }
    int result = sigaction(signal_number, host_action_pointer,
                           host_old_pointer);
    if (result == 0 && old_action) {
        old_action->flags = host_old.sa_flags;
        old_action->handler = host_old.sa_flags & SA_SIGINFO
            ? (void *)host_old.sa_sigaction : (void *)host_old.sa_handler;
        old_action->mask = mask_from_host(&host_old.sa_mask);
        old_action->restorer = NULL;
    }
    return result;
}

static int dt_sigemptyset(void *set) {
    *(unsigned long *)set = 0;
    return 0;
}
static int dt_sigfillset(void *set) {
    *(unsigned long *)set = ~0UL;
    return 0;
}
static int dt_sigaddset(void *set, int signal_number) {
    if (signal_number <= 0 || signal_number > 64) {
        errno = EINVAL;
        return -1;
    }
    *(unsigned long *)set |= 1UL << (signal_number - 1);
    return 0;
}
static int dt_sigdelset(void *set, int signal_number) {
    if (signal_number <= 0 || signal_number > 64) {
        errno = EINVAL;
        return -1;
    }
    *(unsigned long *)set &= ~(1UL << (signal_number - 1));
    return 0;
}
static int dt_sigsuspend(const void *set) {
    sigset_t host;
    mask_to_host(*(const unsigned long *)set, &host);
    return sigsuspend(&host);
}

/* ---------------- Android system properties ---------------- */

struct dt_property {
    char name[96];
    char value[96];
};
static struct dt_property g_property;

static const char *property_value(const char *name) {
    if (!name) return "";
    if (!strcmp(name, "ro.build.version.sdk")) return "25";
    if (!strcmp(name, "ro.build.version.release")) return "7.1.2";
    if (!strcmp(name, "ro.product.manufacturer")) return "Amlogic";
    if (!strcmp(name, "ro.product.model")) return "NextOS";
    if (!strcmp(name, "ro.product.brand")) return "NextOS";
    if (!strcmp(name, "ro.product.device")) return "NextOS";
    if (!strcmp(name, "ro.hardware")) return "amlogic";
    if (!strcmp(name, "ro.opengles.version")) return "131072";
    return "";
}

static int dt_property_get(const char *name, char *value) {
    const char *source = property_value(name);
    if (value)
        strcpy(value, source);
    return (int)strlen(source);
}

static const void *dt_property_find(const char *name) {
    snprintf(g_property.name, sizeof g_property.name, "%s", name ? name : "");
    snprintf(g_property.value, sizeof g_property.value, "%s",
             property_value(name));
    return &g_property;
}

static int dt_property_read(const void *opaque, char *name, char *value) {
    const struct dt_property *property =
        opaque ? (const struct dt_property *)opaque : &g_property;
    if (name)
        strcpy(name, property->name);
    if (value)
        strcpy(value, property->value);
    return (int)strlen(property->value);
}

/* ---------------- Android logging / native window / looper / sensors ----- */

static int android_log_vprint(int priority, const char *tag,
                              const char *format, va_list arguments) {
    (void)priority;
    fprintf(stderr, "[%s] ", tag ? tag : "android");
    int result = vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    return result;
}

static int android_log_print(int priority, const char *tag,
                             const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = android_log_vprint(priority, tag, format, arguments);
    va_end(arguments);
    return result;
}

static int android_log_write(int priority, const char *tag,
                             const char *message) {
    (void)priority;
    fprintf(stderr, "[%s] %s\n", tag ? tag : "android",
            message ? message : "");
    return 0;
}

static void android_abort_message(const char *message) {
    fprintf(stderr, "[android-abort] %s\n", message ? message : "");
}

struct dt_native_window {
    uint16_t width;
    uint16_t height;
};
static struct dt_native_window g_window = {1280, 720};
static int g_looper;
static int g_sensor_manager;
static int g_sensor_queue;

void dt_android_set_size(int width, int height) {
    if (width > 0) g_width = width;
    if (height > 0) g_height = height;
    g_window.width = (uint16_t)g_width;
    g_window.height = (uint16_t)g_height;
}

int dt_android_width(void) {
    return g_width;
}

int dt_android_height(void) {
    return g_height;
}

static void *window_from_surface(void *environment, void *surface) {
    (void)environment;
    (void)surface;
    return &g_window;
}
static void window_void(void *window) { (void)window; }
static int window_width(void *window) {
    (void)window;
    return g_width;
}
static int window_height(void *window) {
    (void)window;
    return g_height;
}
static int window_geometry(void *window, int width, int height, int format) {
    (void)window;
    (void)format;
    if (width > 0 && height > 0)
        dt_android_set_size(width, height);
    return 0;
}

static void *looper_for_thread(void) { return &g_looper; }
static void *looper_prepare(int options) {
    (void)options;
    return &g_looper;
}
static void looper_void(void *looper) { (void)looper; }
static int looper_poll(int timeout, int *fd, int *events, void **data) {
    (void)fd;
    (void)events;
    (void)data;
    dt_audio_pump();
    if (timeout > 0)
        usleep((useconds_t)(timeout > 5 ? 5 : timeout) * 1000);
    return -3;
}

static void *sensor_manager(void) { return &g_sensor_manager; }
static void *sensor_default(void *manager, int type) {
    (void)manager; (void)type; return NULL;
}
static int sensor_list(void *manager, const void ***list) {
    (void)manager;
    if (list) *list = NULL;
    return 0;
}
static void *sensor_create_queue(void *manager, void *looper, int ident,
                                 void *callback, void *data) {
    (void)manager; (void)looper; (void)ident; (void)callback; (void)data;
    return &g_sensor_queue;
}
static int sensor_zero(void *a, ...) { (void)a; return 0; }
static const char *sensor_name(void *sensor) {
    (void)sensor; return "none";
}
static const char *sensor_vendor(void *sensor) {
    (void)sensor; return "NextOS";
}
static float sensor_resolution(void *sensor) {
    (void)sensor; return 0.0f;
}

/* ---------------- AAsset manager backed by extracted APK assets ---------- */

struct dt_asset {
    FILE *file;
    off_t length;
    void *buffer;
    char path[PATH_MAX];
};
static int g_asset_manager;

static void *asset_manager_from_java(void *environment, void *object) {
    (void)environment; (void)object; return &g_asset_manager;
}

static struct dt_asset *asset_open(void *manager, const char *name, int mode) {
    (void)manager; (void)mode;
    if (!name)
        return NULL;
    struct dt_asset *asset = calloc(1, sizeof *asset);
    if (!asset)
        return NULL;
    const char *relative = name;
    while (*relative == '/') ++relative;
    snprintf(asset->path, sizeof asset->path, "%s/assets/%s",
             dt_game_root(), relative);
    asset->file = fopen(asset->path, "rb");
    if (!asset->file) {
        free(asset);
        return NULL;
    }
    fseeko(asset->file, 0, SEEK_END);
    asset->length = ftello(asset->file);
    fseeko(asset->file, 0, SEEK_SET);
    if (io_trace())
        fprintf(stderr, "[asset] %s (%lld bytes)\n", name,
                (long long)asset->length);
    return asset;
}

static int asset_read(struct dt_asset *asset, void *buffer, size_t size) {
    return asset && asset->file
        ? (int)fread(buffer, 1, size, asset->file) : -1;
}
static off_t asset_seek(struct dt_asset *asset, off_t offset, int origin) {
    if (!asset || !asset->file || fseeko(asset->file, offset, origin) != 0)
        return -1;
    return ftello(asset->file);
}
static off_t asset_length(struct dt_asset *asset) {
    return asset ? asset->length : 0;
}
static off_t asset_remaining(struct dt_asset *asset) {
    return asset && asset->file ? asset->length - ftello(asset->file) : 0;
}
static const void *asset_buffer(struct dt_asset *asset) {
    if (!asset || !asset->file)
        return NULL;
    if (!asset->buffer && asset->length > 0) {
        asset->buffer = malloc((size_t)asset->length);
        if (asset->buffer) {
            off_t position = ftello(asset->file);
            fseeko(asset->file, 0, SEEK_SET);
            if (fread(asset->buffer, 1, (size_t)asset->length, asset->file)
                != (size_t)asset->length) {
                free(asset->buffer);
                asset->buffer = NULL;
            }
            fseeko(asset->file, position, SEEK_SET);
        }
    }
    return asset->buffer;
}
static int asset_open_fd(struct dt_asset *asset, off_t *start, off_t *length) {
    if (!asset || !asset->file)
        return -1;
    int descriptor = open(asset->path, O_RDONLY);
    if (start) *start = 0;
    if (length) *length = asset->length;
    return descriptor;
}
static void asset_close(struct dt_asset *asset) {
    if (!asset) return;
    if (asset->file) fclose(asset->file);
    free(asset->buffer);
    free(asset);
}

/* ---------------- audio ------------------------------------------------- */

void dt_audio_pump(void) {
    opensles_shim_pump_callbacks();
}

/* ---------------- import table ----------------------------------------- */

#define ADD(name, function) add_import((name), (void *)(function))

void dt_imports_init(void) {
    if (g_import_count)
        return;

    ADD("dlopen", dt_dlopen);
    ADD("dlsym", dt_dlsym);
    ADD("dlclose", dt_dlclose);
    ADD("dlerror", dt_dlerror);
    ADD("dladdr", dt_dladdr);
    ADD("dl_iterate_phdr", dl_iterate_phdr);
    ADD("eglSwapBuffers", dt_egl_swap_buffers);
    if (dt_sdl_video_active()) {
        static const char *const egl_symbols[] = {
            "eglGetDisplay", "eglGetProcAddress", "eglInitialize",
            "eglTerminate", "eglGetConfigs", "eglChooseConfig",
            "eglGetConfigAttrib", "eglCreateWindowSurface",
            "eglCreatePbufferSurface", "eglDestroySurface",
            "eglQuerySurface", "eglBindAPI", "eglQueryAPI",
            "eglCreateContext", "eglDestroyContext", "eglMakeCurrent",
            "eglGetCurrentContext", "eglGetCurrentSurface",
            "eglGetCurrentDisplay", "eglQueryContext", "eglGetError",
            "eglQueryString", "eglSurfaceAttrib", "eglSwapInterval",
            "eglReleaseThread", "eglWaitClient", "eglWaitGL",
            "eglWaitNative",
        };
        for (size_t index = 0;
             index < sizeof(egl_symbols) / sizeof(egl_symbols[0]);
             ++index) {
            void *function = dt_sdl_egl_proc(egl_symbols[index]);
            if (function)
                add_import(egl_symbols[index], function);
        }
    }

    ADD("open", dt_open);
    ADD("open64", dt_open);
    ADD("fopen", dt_fopen);
    ADD("fopen64", dt_fopen);
    ADD("stat", dt_stat);
    ADD("stat64", dt_stat);
    ADD("lstat", dt_lstat);
    ADD("lstat64", dt_lstat);
    ADD("fstat", dt_fstat);
    ADD("fstat64", dt_fstat);
    ADD("access", dt_access);
    ADD("mkdir", dt_mkdir);
    ADD("remove", dt_remove);
    ADD("unlink", dt_unlink);
    ADD("rename", dt_rename);

    ADD("__sF", g_bionic_sF);
    ADD("fclose", std_fclose);
    ADD("feof", std_feof);
    ADD("ferror", std_ferror);
    ADD("clearerr", std_clearerr);
    ADD("fflush", std_fflush);
    ADD("fgets", std_fgets);
    ADD("fputc", std_fputc);
    ADD("fputs", std_fputs);
    ADD("fread", std_fread);
    ADD("fwrite", std_fwrite);
    ADD("fseek", std_fseek);
    ADD("fseeko", std_fseeko);
    ADD("ftell", std_ftell);
    ADD("ftello", std_ftello);
    ADD("getc", std_getc);
    ADD("setvbuf", std_setvbuf);
    ADD("fprintf", std_fprintf);
    ADD("vfprintf", std_vfprintf);
    ADD("fscanf", std_fscanf);

    ADD("__errno", dt_errno);
    ADD("strlcpy", dt_strlcpy);
    ADD("strerror_r", dt_strerror_r);
    ADD("__FD_SET_chk", dt_fd_set_chk);
    ADD("__FD_ISSET_chk", dt_fd_isset_chk);
    ADD("sysconf", dt_sysconf);
    ADD("setjmp", dt_setjmp);
    ADD("_setjmp", dt_setjmp);
    ADD("longjmp", dt_longjmp);
    ADD("_longjmp", dt_longjmp);
    ADD("sigaction", dt_sigaction);
    ADD("sigemptyset", dt_sigemptyset);
    ADD("sigfillset", dt_sigfillset);
    ADD("sigaddset", dt_sigaddset);
    ADD("sigdelset", dt_sigdelset);
    ADD("sigsuspend", dt_sigsuspend);

    ADD("__system_property_get", dt_property_get);
    ADD("__system_property_find", dt_property_find);
    ADD("__system_property_read", dt_property_read);

    ADD("__android_log_print", android_log_print);
    ADD("__android_log_vprint", android_log_vprint);
    ADD("__android_log_write", android_log_write);
    ADD("android_set_abort_message", android_abort_message);

    ADD("ANativeWindow_fromSurface", window_from_surface);
    ADD("ANativeWindow_acquire", window_void);
    ADD("ANativeWindow_release", window_void);
    ADD("ANativeWindow_getWidth", window_width);
    ADD("ANativeWindow_getHeight", window_height);
    ADD("ANativeWindow_setBuffersGeometry", window_geometry);
    ADD("ALooper_forThread", looper_for_thread);
    ADD("ALooper_prepare", looper_prepare);
    ADD("ALooper_acquire", looper_void);
    ADD("ALooper_release", looper_void);
    ADD("ALooper_wake", looper_void);
    ADD("ALooper_pollAll", looper_poll);

    ADD("ASensorManager_getInstance", sensor_manager);
    ADD("ASensorManager_getDefaultSensor", sensor_default);
    ADD("ASensorManager_getSensorList", sensor_list);
    ADD("ASensorManager_createEventQueue", sensor_create_queue);
    ADD("ASensorManager_destroyEventQueue", sensor_zero);
    ADD("ASensorEventQueue_enableSensor", sensor_zero);
    ADD("ASensorEventQueue_disableSensor", sensor_zero);
    ADD("ASensorEventQueue_setEventRate", sensor_zero);
    ADD("ASensorEventQueue_hasEvents", sensor_zero);
    ADD("ASensorEventQueue_getEvents", sensor_zero);
    ADD("ASensor_getType", sensor_zero);
    ADD("ASensor_getName", sensor_name);
    ADD("ASensor_getVendor", sensor_vendor);
    ADD("ASensor_getResolution", sensor_resolution);
    ADD("ASensor_getMinDelay", sensor_zero);

    ADD("AAssetManager_fromJava", asset_manager_from_java);
    ADD("AAssetManager_open", asset_open);
    ADD("AAsset_read", asset_read);
    ADD("AAsset_seek", asset_seek);
    ADD("AAsset_seek64", asset_seek);
    ADD("AAsset_getLength", asset_length);
    ADD("AAsset_getLength64", asset_length);
    ADD("AAsset_getRemainingLength", asset_remaining);
    ADD("AAsset_getRemainingLength64", asset_remaining);
    ADD("AAsset_getBuffer", asset_buffer);
    ADD("AAsset_openFileDescriptor", asset_open_fd);
    ADD("AAsset_openFileDescriptor64", asset_open_fd);
    ADD("AAsset_close", asset_close);

    ADD("pthread_mutex_init", pthread_mutex_init_fake);
    ADD("pthread_mutex_destroy", pthread_mutex_destroy_fake);
    ADD("pthread_mutex_lock", pthread_mutex_lock_fake);
    ADD("pthread_mutex_unlock", pthread_mutex_unlock_fake);
    ADD("pthread_mutex_trylock", pthread_mutex_trylock_fake);
    ADD("pthread_cond_init", pthread_cond_init_fake);
    ADD("pthread_cond_destroy", pthread_cond_destroy_fake);
    ADD("pthread_cond_wait", pthread_cond_wait_fake);
    ADD("pthread_cond_timedwait", pthread_cond_timedwait_fake);
    ADD("pthread_cond_signal", pthread_cond_signal_fake);
    ADD("pthread_cond_broadcast", pthread_cond_broadcast_fake);
    ADD("pthread_condattr_init", pthread_condattr_init_fake);
    ADD("pthread_condattr_destroy", pthread_condattr_destroy_fake);
    ADD("pthread_condattr_setclock", pthread_condattr_setclock_fake);
    ADD("pthread_mutexattr_init", pthread_mutexattr_init_fake);
    ADD("pthread_mutexattr_destroy", pthread_mutexattr_destroy_fake);
    ADD("pthread_mutexattr_settype", pthread_mutexattr_settype_fake);
    ADD("pthread_rwlock_init", pthread_rwlock_init_fake);
    ADD("pthread_rwlock_destroy", pthread_rwlock_destroy_fake);
    ADD("pthread_rwlock_rdlock", pthread_rwlock_rdlock_fake);
    ADD("pthread_rwlock_wrlock", pthread_rwlock_wrlock_fake);
    ADD("pthread_rwlock_tryrdlock", pthread_rwlock_tryrdlock_fake);
    ADD("pthread_rwlock_trywrlock", pthread_rwlock_trywrlock_fake);
    ADD("pthread_rwlock_unlock", pthread_rwlock_unlock_fake);
    ADD("pthread_create", pthread_create_fake);
    ADD("pthread_attr_init", pthread_attr_init_fake);
    ADD("pthread_attr_destroy", pthread_attr_destroy_fake);
    ADD("pthread_attr_setdetachstate", pthread_attr_setdetachstate_fake);
    ADD("pthread_attr_setstacksize", pthread_attr_setstacksize_fake);
    ADD("pthread_attr_setschedparam", pthread_attr_setschedparam_fake);
    ADD("pthread_getattr_np", dt_pthread_getattr_np);
    ADD("pthread_attr_getstack", dt_pthread_attr_getstack);
    ADD("pthread_sigmask", pthread_sigmask_fake);
    ADD("pthread_once", pthread_once_fake);
    ADD("pthread_key_create", pthread_key_create_fake);
    ADD("pthread_key_delete", pthread_key_delete_fake);
    ADD("pthread_getspecific", pthread_getspecific_fake);
    ADD("pthread_setspecific", pthread_setspecific_fake);
    ADD("pthread_detach", pthread_detach_fake);
    ADD("pthread_join", pthread_join_fake);
    ADD("pthread_self", pthread_self_fake);
    ADD("pthread_setname_np", pthread_setname_np_fake);
    ADD("pthread_atfork", dt_pthread_atfork);

    ADD("sem_init", sh_sem_init);
    ADD("sem_wait", sh_sem_wait);
    ADD("sem_trywait", sh_sem_trywait);
    ADD("sem_timedwait", sh_sem_timedwait);
    ADD("sem_post", sh_sem_post);
    ADD("sem_getvalue", sh_sem_getvalue);
    ADD("sem_destroy", sh_sem_destroy);

    ADD("slCreateEngine", slCreateEngine_shim);
    ADD("SL_IID_ENGINE", &sl_IID_ENGINE);
    ADD("SL_IID_PLAY", &sl_IID_PLAY);
    ADD("SL_IID_VOLUME", &sl_IID_VOLUME);
    ADD("SL_IID_BUFFERQUEUE", &sl_IID_BUFFERQUEUE);
    ADD("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &sl_IID_BUFFERQUEUE);
    ADD("SL_IID_EFFECTSEND", &sl_IID_EFFECTSEND);
    ADD("SL_IID_ENGINECAPABILITIES", &sl_IID_ENGINECAPABILITIES);
    ADD("SL_IID_ANDROIDCONFIGURATION", &sl_IID_ANDROIDCONFIGURATION);
    ADD("SL_IID_ENVIRONMENTALREVERB", &sl_IID_ENVIRONMENTALREVERB);
    ADD("SL_IID_RECORD", &g_sl_iid_record);

    dt_loader_set_imports(g_imports, g_import_count);
    fprintf(stderr, "[imports] %d pontes ABI registradas\n", g_import_count);
}
