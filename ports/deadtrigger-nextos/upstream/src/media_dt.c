/*
 * Dead Trigger 2.1.0 intro-video backend for NextOS.
 *
 * Unity's Android VideoPlayer reaches AMedia NDK/MediaPlayer through Java.
 * Those services do not exist in the native loader environment. We keep the
 * game's own MoviePlayer -> IntroVideoPlayer.StartVideo -> StopIntroVideo
 * sequence and replace only the unavailable presentation backend. The
 * original embedded MP4 is decoded to PulseAudio and /dev/fb0 only when the
 * process owns the raw framebuffer path. SDL/KMSDRM keeps exclusive display
 * ownership and completes through the game's native StopVideo seam.
 */
#define _GNU_SOURCE

#include "dt.h"
#include "egl_sdl.h"
#include "media_dt.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DT_INTRO_RESOURCE_OFFSET 1404128
#define DT_INTRO_RESOURCE_SIZE 9085287

typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef const void *(*il2cpp_class_get_method_from_name_fn)(
    void *, const char *, int);

typedef void (*intro_start_fn)(void *, const void *);
typedef void (*intro_stop_fn)(void *, const void *);

static _Atomic int g_skip_requested;
static int g_install_state;
static unsigned g_install_attempts;
static intro_start_fn g_original_start_video;
static intro_stop_fn g_stop_video;
static const void *g_stop_video_method;

void dt_media_set_skip_requested(int requested) {
    atomic_store_explicit(&g_skip_requested, requested != 0,
                          memory_order_release);
}

static void *find_method(dt_module *il2cpp, const char *class_name,
                         const char *method_name, int parameter_count) {
#define API(type, name)                                                        \
    type name = (type)dt_module_symbol(il2cpp, #name)
    API(il2cpp_domain_get_fn, il2cpp_domain_get);
    API(il2cpp_domain_get_assemblies_fn, il2cpp_domain_get_assemblies);
    API(il2cpp_assembly_get_image_fn, il2cpp_assembly_get_image);
    API(il2cpp_class_from_name_fn, il2cpp_class_from_name);
    API(il2cpp_class_get_method_from_name_fn,
        il2cpp_class_get_method_from_name);
#undef API

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name)
        return NULL;

    void *domain = il2cpp_domain_get();
    if (!domain)
        return NULL;
    size_t assembly_count = 0;
    const void **assemblies =
        il2cpp_domain_get_assemblies(domain, &assembly_count);
    if (!assemblies)
        return NULL;

    for (size_t index = 0; index < assembly_count; ++index) {
        void *image = il2cpp_assembly_get_image(assemblies[index]);
        void *klass = image
            ? il2cpp_class_from_name(image, "", class_name) : NULL;
        if (!klass)
            continue;
        return (void *)il2cpp_class_get_method_from_name(
            klass, method_name, parameter_count);
    }
    return NULL;
}

static int intro_file_valid(const char *path) {
    struct stat status;
    if (stat(path, &status) != 0 ||
        status.st_size != DT_INTRO_RESOURCE_SIZE)
        return 0;
    int file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return 0;
    unsigned char header[12];
    ssize_t got = read(file, header, sizeof header);
    close(file);
    return got == (ssize_t)sizeof header &&
           !memcmp(header + 4, "ftyp", 4);
}

static int extract_intro(char *destination, size_t destination_size) {
    const char *root = dt_game_root();
    if (!root || !*root)
        return -1;
    if (snprintf(destination, destination_size,
                 "%s/userdata/Intro_Deca.mp4", root) >=
        (int)destination_size)
        return -1;
    if (intro_file_valid(destination))
        return 0;

    char source[1024], temporary[1024];
    if (snprintf(source, sizeof source,
                 "%s/assets/bin/Data/sharedassets0.resource", root) >=
            (int)sizeof source ||
        snprintf(temporary, sizeof temporary, "%s.tmp", destination) >=
            (int)sizeof temporary)
        return -1;

    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        fprintf(stderr, "[video] abrir %s: %s\n", source, strerror(errno));
        return -1;
    }
    struct stat source_status;
    if (fstat(input, &source_status) != 0 ||
        source_status.st_size <
            DT_INTRO_RESOURCE_OFFSET + DT_INTRO_RESOURCE_SIZE) {
        fprintf(stderr, "[video] recurso da introducao incompleto\n");
        close(input);
        return -1;
    }

    int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      0644);
    if (output < 0) {
        fprintf(stderr, "[video] criar %s: %s\n",
                temporary, strerror(errno));
        close(input);
        return -1;
    }

    char buffer[65536];
    off_t offset = DT_INTRO_RESOURCE_OFFSET;
    size_t remaining = DT_INTRO_RESOURCE_SIZE;
    int okay = 1;
    while (remaining) {
        size_t wanted = remaining < sizeof buffer ? remaining : sizeof buffer;
        ssize_t got = pread(input, buffer, wanted, offset);
        if (got <= 0) {
            okay = 0;
            break;
        }
        size_t written = 0;
        while (written < (size_t)got) {
            ssize_t result =
                write(output, buffer + written, (size_t)got - written);
            if (result <= 0) {
                okay = 0;
                break;
            }
            written += (size_t)result;
        }
        if (!okay)
            break;
        offset += got;
        remaining -= (size_t)got;
    }
    if (okay && fsync(output) != 0)
        okay = 0;
    if (close(output) != 0)
        okay = 0;
    close(input);

    if (!okay || !intro_file_valid(temporary) ||
        rename(temporary, destination) != 0) {
        fprintf(stderr, "[video] extrair introducao: %s\n",
                strerror(errno));
        unlink(temporary);
        return -1;
    }
    fprintf(stderr,
            "[video] Intro_Deca.mp4 extraido dos dados originais (%d bytes)\n",
            DT_INTRO_RESOURCE_SIZE);
    return 0;
}

static void child_stderr_to_null(void) {
    int null_file = open("/dev/null", O_WRONLY);
    if (null_file >= 0) {
        dup2(null_file, STDERR_FILENO);
        if (null_file != STDERR_FILENO)
            close(null_file);
    }
}

static pid_t spawn_audio_decoder(const char *video, int pipe_write) {
    pid_t child = fork();
    if (child != 0)
        return child;
    setpgid(0, 0);
    dup2(pipe_write, STDOUT_FILENO);
    close(pipe_write);
    child_stderr_to_null();
    execl("/usr/bin/ffmpeg", "ffmpeg",
          "-hide_banner", "-loglevel", "error", "-nostdin", "-re",
          "-i", video, "-vn", "-f", "s16le", "-ar", "48000",
          "-ac", "2", "-", (char *)NULL);
    _exit(127);
}

static pid_t spawn_pacat(int pipe_read) {
    pid_t child = fork();
    if (child != 0)
        return child;
    setpgid(0, 0);
    dup2(pipe_read, STDIN_FILENO);
    close(pipe_read);
    child_stderr_to_null();
    execl("/usr/bin/pacat", "pacat",
          "--rate=48000", "--channels=2", "--format=s16le",
          (char *)NULL);
    _exit(127);
}

static pid_t spawn_video(const char *video) {
    int frame_width = dt_android_width();
    int frame_height = dt_android_height();
    if (frame_width < 2 || frame_height < 2)
        return -1;

    /* Preserve the proven 4:3 intro composition inside the real surface. */
    int content_width = (frame_height * 4) / 3;
    int content_height = frame_height;
    if (content_width > frame_width) {
        content_width = frame_width;
        content_height = (frame_width * 3) / 4;
    }
    content_width &= ~1;
    content_height &= ~1;
    int offset_x = (frame_width - content_width) / 2;
    int offset_y = (frame_height - content_height) / 2;
    char filter[192];
    if (snprintf(filter, sizeof filter,
                 "scale=%d:%d,format=bgra,pad=%d:%d:%d:%d:black",
                 content_width, content_height, frame_width, frame_height,
                 offset_x, offset_y) >= (int)sizeof filter)
        return -1;
    fprintf(stderr,
            "[video] fbdev %dx%d: conteudo %dx%d em %d,%d\n",
            frame_width, frame_height, content_width, content_height,
            offset_x, offset_y);

    pid_t child = fork();
    if (child != 0)
        return child;
    setpgid(0, 0);
    child_stderr_to_null();
    execl("/usr/bin/ffmpeg", "ffmpeg",
          "-hide_banner", "-loglevel", "error", "-nostdin", "-re",
          "-i", video, "-an",
          "-vf", filter,
          "-pix_fmt", "bgra", "-f", "fbdev", "/dev/fb0",
          (char *)NULL);
    _exit(127);
}

static void terminate_child(pid_t child) {
    if (child <= 0)
        return;
    if (waitpid(child, NULL, WNOHANG) == child)
        return;
    kill(child, SIGTERM);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (waitpid(child, NULL, WNOHANG) == child)
            return;
        usleep(10000);
    }
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int play_intro_native(const char *video) {
    int audio_pipe[2];
    if (pipe2(audio_pipe, O_CLOEXEC) != 0) {
        fprintf(stderr, "[video] pipe de audio: %s\n", strerror(errno));
        return -1;
    }

    pid_t decoder = spawn_audio_decoder(video, audio_pipe[1]);
    close(audio_pipe[1]);
    if (decoder < 0) {
        close(audio_pipe[0]);
        return -1;
    }
    pid_t player = spawn_pacat(audio_pipe[0]);
    close(audio_pipe[0]);
    if (player < 0) {
        terminate_child(decoder);
        return -1;
    }
    pid_t video_player = spawn_video(video);
    if (video_player < 0) {
        terminate_child(decoder);
        terminate_child(player);
        return -1;
    }

    fprintf(stderr,
            "[video] introducao original: 40,7 s (A/B/Start pula)\n");
    int status = 0;
    int skipped = 0;
    for (;;) {
        pid_t result = waitpid(video_player, &status, WNOHANG);
        if (result == video_player)
            break;
        if (result < 0 && errno != EINTR)
            break;
        if (atomic_load_explicit(&g_skip_requested,
                                 memory_order_acquire)) {
            skipped = 1;
            terminate_child(video_player);
            break;
        }
        usleep(20000);
    }
    terminate_child(video_player);
    terminate_child(decoder);
    terminate_child(player);
    fprintf(stderr, "[video] introducao %s\n",
            skipped ? "pulada pelo controle" : "concluida");
    return 0;
}

static void hooked_start_video(void *instance, const void *method) {
    /*
     * Preserve every managed side effect first: gamepad-sensitive skip text,
     * intro GUI visibility and VideoPlayer.Play. The Android backend reports
     * its normal error, then this native substitute owns presentation until
     * the original MoviePlayer coroutine resumes and calls StopIntroVideo.
     */
    const int sdl_active = dt_sdl_video_active();
    const int have_ffmpeg = access("/usr/bin/ffmpeg", X_OK) == 0;
    const int have_pacat = access("/usr/bin/pacat", X_OK) == 0;
    const int have_fbdev = access("/dev/fb0", W_OK) == 0;
    const int can_present = !sdl_active && have_ffmpeg && have_pacat &&
                            have_fbdev;
    fprintf(stderr,
            "[video] StartVideo nativo: entrando "
            "(ffmpeg=%d pacat=%d fbdev=%d sdl=%d)\n",
            have_ffmpeg, have_pacat, have_fbdev, sdl_active);
    fflush(stderr);
    g_original_start_video(instance, method);
    fprintf(stderr, "[video] StartVideo nativo: retornou\n");
    fflush(stderr);

    char video[1024];
    if (!can_present || extract_intro(video, sizeof video) != 0) {
        fprintf(stderr,
                "[video] apresentador externo incompativel; "
                "concluindo pelo IntroVideoPlayer nativo\n");
    } else {
        (void)play_intro_native(video);
    }

    /*
     * Android normally deactivates the VideoPlayer object at end-of-stream.
     * The failed JNI backend leaves isPlaying stuck at true. StopVideo is the
     * game's own backend-completion seam: it deactivates the intro object, so
     * the untouched MoviePlayer coroutine observes isPlaying == false and
     * executes StopIntroVideo itself on its next MoveNext.
     */
    g_stop_video(instance, g_stop_video_method);
    fprintf(stderr,
            "[video] fim sinalizado por IntroVideoPlayer.StopVideo; "
            "coroutine nativo retomado\n");
}

static int64_t sign_extend_21(uint64_t value) {
    return (int64_t)(value << (64 - 21)) >> (64 - 21);
}

static int install_start_video_hook(dt_module *il2cpp,
                                    const void *method,
                                    const void *stop_method) {
#if defined(__aarch64__)
    if (!method || !stop_method)
        return 0;
    uintptr_t target = *(const uintptr_t *)method;
    uintptr_t stop_target = *(const uintptr_t *)stop_method;
    uintptr_t base = (uintptr_t)dt_module_base(il2cpp);
    size_t module_size = dt_module_size(il2cpp);
    if (target < base || target + 16 > base + module_size ||
        stop_target < base || stop_target >= base + module_size)
        return 0;

    const uint32_t expected[3] = {
        0xa9be4ff4u, /* stp x20, x19, [sp, #-32]! */
        0xa9017bfdu, /* stp x29, x30, [sp, #16] */
        0x910043fdu, /* add x29, sp, #16 */
    };
    uint32_t *original = (uint32_t *)target;
    if (memcmp(original, expected, sizeof expected) != 0 ||
        (original[3] & 0x9f000000u) != 0x90000000u) {
        fprintf(stderr,
                "[video] prologo StartVideo inesperado em il2cpp+0x%lx; "
                "hook recusado\n",
                (unsigned long)(target - base));
        return 0;
    }

    uint32_t adrp = original[3];
    unsigned destination_register = adrp & 31u;
    uint64_t immediate =
        (((uint64_t)(adrp >> 5) & 0x7ffffu) << 2) |
        ((adrp >> 29) & 3u);
    intptr_t page_delta =
        (intptr_t)(sign_extend_21(immediate) * INT64_C(4096));
    uintptr_t adrp_value = (uintptr_t)(
        (intptr_t)((target + 12u) & ~(uintptr_t)0xfffu) + page_delta);

    uint32_t *gateway = mmap(NULL, 48,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (gateway == MAP_FAILED) {
        fprintf(stderr, "[video] mmap gateway: %s\n", strerror(errno));
        return 0;
    }
    memcpy(gateway, original, 12);
    gateway[3] = 0x58000000u | (5u << 5) | destination_register;
    gateway[4] = 0x58000000u | (6u << 5) | 16u;
    gateway[5] = 0xd61f0200u; /* br x16 */
    gateway[6] = gateway[7] = 0xd503201fu;
    memcpy(gateway + 8, &adrp_value, sizeof adrp_value);
    uintptr_t continuation = target + 16u;
    memcpy(gateway + 10, &continuation, sizeof continuation);
    __builtin___clear_cache((char *)gateway, (char *)gateway + 48);
    g_original_start_video = (intro_start_fn)gateway;
    g_stop_video = (intro_stop_fn)stop_target;
    g_stop_video_method = stop_method;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        munmap(gateway, 48);
        g_original_start_video = NULL;
        g_stop_video = NULL;
        g_stop_video_method = NULL;
        return 0;
    }
    uintptr_t page = target & ~((uintptr_t)page_size - 1u);
    uintptr_t patch_end = target + 16u;
    uintptr_t end_page =
        (patch_end + (uintptr_t)page_size - 1u) &
        ~((uintptr_t)page_size - 1u);
    size_t patch_span = (size_t)(end_page - page);
    if (mprotect((void *)page, patch_span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[video] mprotect StartVideo: %s\n",
                strerror(errno));
        munmap(gateway, 48);
        g_original_start_video = NULL;
        g_stop_video = NULL;
        g_stop_video_method = NULL;
        return 0;
    }
    original[0] = 0x58000050u; /* ldr x16, #8 */
    original[1] = 0xd61f0200u; /* br x16 */
    void *replacement = (void *)&hooked_start_video;
    memcpy(original + 2, &replacement, sizeof replacement);
    __builtin___clear_cache((char *)target, (char *)target + 16);
    if (mprotect((void *)page, patch_span,
                 PROT_READ | PROT_EXEC) != 0)
        fprintf(stderr,
                "[video] aviso: restaurar RX StartVideo: %s\n",
                strerror(errno));
    fprintf(stderr,
            "[video] backend instalado em IntroVideoPlayer.StartVideo "
            "(il2cpp+0x%lx)\n",
            (unsigned long)(target - base));
    return 1;
#else
    (void)il2cpp;
    (void)method;
    (void)stop_method;
    return 0;
#endif
}

void dt_media_try_install(void) {
    if (g_install_state)
        return;
    dt_module *il2cpp = dt_module_find("libil2cpp.so");
    if (!il2cpp)
        return;
    const void *method =
        find_method(il2cpp, "IntroVideoPlayer", "StartVideo", 0);
    const void *stop_method =
        find_method(il2cpp, "IntroVideoPlayer", "StopVideo", 0);
    if (!method || !stop_method) {
        if (++g_install_attempts == 1 || g_install_attempts % 600 == 0)
            fprintf(stderr,
                    "[video] aguardando metadata IntroVideoPlayer "
                    "(tentativa %u)\n",
                    g_install_attempts);
        return;
    }
    if (!install_start_video_hook(il2cpp, method, stop_method)) {
        g_install_state = -1;
        return;
    }
    g_install_state = 1;
}
