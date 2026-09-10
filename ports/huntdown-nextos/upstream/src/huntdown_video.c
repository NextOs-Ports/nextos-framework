#define _GNU_SOURCE
#include "huntdown_video.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "egl_shim.h"
#include "huntdown_build.h"
#include "huntdown_display.h"
#include "huntdown_video_gl.h"
#include "so_util.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * Two presentation routes, chosen by the backend SDL actually opened — never
 * by a device or firmware name:
 *
 *   fbdev/Mali  -> the decoder writes BGRA straight into /dev/fb0 (the route
 *                  validated on NextOS Amlogic; byte-for-byte unchanged).
 *   SDL-owned   -> the decoder streams RGBA over a pipe and the loader draws
 *                  it as a full-screen quad in the same context Unity swaps
 *                  from.  Writing to /dev/fb0 under KMSDRM would land on a
 *                  buffer that never becomes a page flip.
 *
 * Audio is negotiated independently of video: a real PulseAudio socket keeps
 * the proven `pacat` sink, otherwise the decoder feeds `aplay`.  A movie whose
 * audio sink is missing still plays, with the loss logged once.
 */
enum {
  HD_ROUTE_FBDEV = 0,
  HD_ROUTE_GL = 1,
};

typedef uintptr_t (*HdVideoMethod)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                   uintptr_t, uintptr_t, uintptr_t, uintptr_t);

typedef enum HdVideoState {
  HD_VIDEO_IDLE,
  HD_VIDEO_READY,
  HD_VIDEO_ACTIVE,
  HD_VIDEO_FINISHED,
} HdVideoState;

typedef struct HdVideoSession {
  HdVideoState state;
  void *player;
  char url[PATH_MAX];
  char path[PATH_MAX];
  pid_t video_pid;
  pid_t audio_pid;
  pid_t pacat_pid;
  int completion_emitted;
  int owns_screen;
  int frame_fd;          /* GL route: read end of the raw RGBA stream */
  unsigned char *frame;  /* GL route: one full frame being assembled */
  size_t frame_bytes;    /* GL route: width * height * 4 */
  size_t frame_used;     /* GL route: bytes of the current frame received */
  int frame_ready;       /* GL route: a complete frame is waiting upload */
  int frame_width;
  int frame_height;
  int gl_ready;
} HdVideoSession;

static HdVideoSession g_video;
static int g_video_installed;
static int g_video_trace;
static int g_video_route;
static const char *g_ffmpeg;
static const char *g_pacat;
static const char *g_aplay;
static const char *g_audio_sink;   /* "pacat", "aplay" or NULL */
static int g_no_decoder;           /* firmware has no ffmpeg: skip movies */
static uintptr_t g_il2cpp_base;

static HdVideoMethod g_set_url_original;
static HdVideoMethod g_play_original;
static HdVideoMethod g_stop_original;
static HdVideoMethod g_is_playing_original;
static void (*g_invoke_loop_point)(void *, void *);
static void (*g_invoke_started)(void *, void *);

typedef struct HdVideoTarget {
  uintptr_t rva;
  unsigned char signature[16];
  const char *name;
  HdVideoMethod hook;
  HdVideoMethod *original;
} HdVideoTarget;

static int hd_env_off(const char *name) {
  const char *value = getenv(name);
  return value && (!strcmp(value, "0") || !strcasecmp(value, "false") ||
                   !strcasecmp(value, "no") || !strcasecmp(value, "off"));
}

static int hd_env_on(const char *name) {
  const char *value = getenv(name);
  return value && *value && !hd_env_off(name);
}

static const char *hd_find_program(const char *name) {
  static char slots[3][PATH_MAX];
  static int used;
  char *output = slots[used < 3 ? used : 2];
  if (used < 3) ++used;
  static const char *directories[] = {"/usr/bin", "/bin", "/usr/local/bin"};
  for (size_t i = 0; i < sizeof directories / sizeof directories[0]; ++i) {
    int written = snprintf(output, PATH_MAX, "%s/%s", directories[i], name);
    if (written > 0 && written < PATH_MAX && access(output, X_OK) == 0)
      return output;
  }
  output[0] = 0;
  return NULL;
}

/* A PulseAudio socket only counts when it really exists and is usable.  An
 * inherited PULSE_SERVER pointing at a dead daemon must not decide the sink. */
static int hd_pulse_available(void) {
  const char *server = getenv("PULSE_SERVER");
  if (server && !strncmp(server, "unix:", 5) &&
      access(server + 5, R_OK | W_OK) == 0)
    return 1;
  return access("/run/pulse/native", R_OK | W_OK) == 0 ||
         access("/var/run/pulse/native", R_OK | W_OK) == 0;
}

static int hd_utf16_to_utf8(const void *managed_string, char *output,
                            size_t capacity) {
  if (!output || capacity == 0) return 0;
  output[0] = 0;
  if (!managed_string) return 0;
  const unsigned char *object = (const unsigned char *)managed_string;
  int length = *(const int *)(object + 0x10);
  if (length < 0 || length > 8192) return 0;
  const uint16_t *input = (const uint16_t *)(object + 0x14);
  size_t used = 0;
  for (int i = 0; i < length; ++i) {
    uint32_t codepoint = input[i];
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 1 < length) {
      uint32_t low = input[i + 1];
      if (low >= 0xDC00 && low <= 0xDFFF) {
        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                    (low - 0xDC00);
        ++i;
      }
    }
    unsigned char bytes[4];
    size_t count;
    if (codepoint < 0x80) {
      bytes[0] = (unsigned char)codepoint;
      count = 1;
    } else if (codepoint < 0x800) {
      bytes[0] = 0xC0 | (unsigned char)(codepoint >> 6);
      bytes[1] = 0x80 | (unsigned char)(codepoint & 0x3F);
      count = 2;
    } else if (codepoint < 0x10000) {
      bytes[0] = 0xE0 | (unsigned char)(codepoint >> 12);
      bytes[1] = 0x80 | (unsigned char)((codepoint >> 6) & 0x3F);
      bytes[2] = 0x80 | (unsigned char)(codepoint & 0x3F);
      count = 3;
    } else {
      bytes[0] = 0xF0 | (unsigned char)(codepoint >> 18);
      bytes[1] = 0x80 | (unsigned char)((codepoint >> 12) & 0x3F);
      bytes[2] = 0x80 | (unsigned char)((codepoint >> 6) & 0x3F);
      bytes[3] = 0x80 | (unsigned char)(codepoint & 0x3F);
      count = 4;
    }
    if (used + count >= capacity) return 0;
    memcpy(output + used, bytes, count);
    used += count;
  }
  output[used] = 0;
  return 1;
}

static int hd_hex_value(unsigned char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static int hd_percent_decode(const char *input, char *output, size_t capacity) {
  size_t used = 0;
  if (!input || !output || capacity == 0) return 0;
  while (*input) {
    unsigned char value = (unsigned char)*input++;
    if (value == '%' && input[0] && input[1]) {
      int high = hd_hex_value((unsigned char)input[0]);
      int low = hd_hex_value((unsigned char)input[1]);
      if (high >= 0 && low >= 0) {
        value = (unsigned char)((high << 4) | low);
        input += 2;
      }
    }
    if (value == 0 || used + 1 >= capacity) return 0;
    output[used++] = (char)value;
  }
  output[used] = 0;
  return 1;
}

static const char *hd_video_basename(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  return slash ? slash + 1 : path;
}

static int hd_is_authored_movie(const char *path) {
  const char *name = hd_video_basename(path);
  return name && (!strcmp(name, "CoffeeIntro.mp4") ||
                  !strcmp(name, "EasyTriggerVignette.mp4"));
}

static int hd_resolve_movie(const char *url, char output[PATH_MAX]) {
  if (!url) return 0;
  const char *encoded_path;
  if (strncmp(url, "file://", 7) == 0)
    encoded_path = url + 7;
  else if (url[0] == '/')
    encoded_path = url;
  else
    return 0;
  char requested[PATH_MAX];
  if (!hd_percent_decode(encoded_path, requested, sizeof requested) ||
      !hd_is_authored_movie(requested))
    return 0;
  if (access(requested, R_OK) == 0) {
    snprintf(output, PATH_MAX, "%s", requested);
    return 1;
  }

  /* Android reports <install>/assets as StreamingAssets, while the BYO-data
   * recipe extracts the contents of assets directly into the game directory. */
  const char *assets = strstr(requested, "/assets/");
  if (assets) {
    size_t prefix = (size_t)(assets - requested);
    const char *suffix = assets + strlen("/assets");
    if (prefix + strlen(suffix) + 1 < PATH_MAX) {
      memcpy(output, requested, prefix);
      strcpy(output + prefix, suffix);
      if (access(output, R_OK) == 0) return 1;
    }
  }

  const char *game_dir = getenv("HD_GAMEDIR");
  const char *name = hd_video_basename(requested);
  if (game_dir && *game_dir && name) {
    int written = snprintf(output, PATH_MAX, "%s/video/%s", game_dir, name);
    if (written > 0 && written < PATH_MAX && access(output, R_OK) == 0)
      return 1;
  }
  output[0] = 0;
  return 0;
}

static void hd_fb_size(int *width, int *height, int *bits_per_pixel) {
  int w = 0, h = 0, bpp = 0;
  int fd = open("/dev/fb0", O_RDONLY);
  if (fd >= 0) {
    struct fb_var_screeninfo variable;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &variable) == 0) {
      w = (int)variable.xres;
      h = (int)variable.yres;
      bpp = (int)variable.bits_per_pixel;
    }
    close(fd);
  }
  if (w <= 0 || h <= 0)
    (void)hd_display_size_detect(&w, &h, NULL);
  if (w <= 0) w = 1280;
  if (h <= 0) h = 720;
  if (bpp <= 0) bpp = 32;
  if (width) *width = w;
  if (height) *height = h;
  if (bits_per_pixel) *bits_per_pixel = bpp;
}

static void hd_fb_pan_zero(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) return;
  struct fb_var_screeninfo variable;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &variable) == 0 &&
      variable.yoffset != 0) {
    variable.yoffset = 0;
    (void)ioctl(fd, FBIOPAN_DISPLAY, &variable);
  }
  close(fd);
}

static void hd_fb_clear(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) return;
  struct fb_var_screeninfo variable;
  struct fb_fix_screeninfo fixed;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &variable) != 0 ||
      ioctl(fd, FBIOGET_FSCREENINFO, &fixed) != 0 ||
      variable.bits_per_pixel != 32) {
    close(fd);
    return;
  }
  variable.yoffset = 0;
  (void)ioctl(fd, FBIOPAN_DISPLAY, &variable);
  size_t height = variable.yres_virtual > variable.yres
                      ? (size_t)variable.yres_virtual
                      : (size_t)variable.yres;
  size_t map_size = fixed.smem_len ? (size_t)fixed.smem_len
                                   : (size_t)fixed.line_length * height;
  void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map != MAP_FAILED) {
    for (size_t y = 0; y < height; ++y) {
      uint32_t *row = (uint32_t *)((unsigned char *)map +
                                   y * (size_t)fixed.line_length);
      for (size_t x = 0; x < (size_t)fixed.line_length / 4; ++x)
        row[x] = 0xFF000000u;
    }
    munmap(map, map_size);
  }
  close(fd);
}

static void hd_child_setup(void) {
  (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() == 1) _exit(125);
}

/* Some firmware FFmpeg builds link libplacebo even when decoding H.264 fully
 * in software.  If that optional dependency was installed beside a broken
 * libvulkan.so.1 symlink, the dynamic loader aborts before FFmpeg can parse the
 * movie.  The port ships a no-libc Vulkan resolver stub for this one child
 * role; it returns NULL for every optional Vulkan entry point.  Never expose
 * it to Unity, SDL or the launcher-wide library closure. */
static void hd_child_enable_ffmpeg_compat(void) {
  const char *game_dir = getenv("HD_GAMEDIR");
  const char *inherited = getenv("LD_LIBRARY_PATH");
  char directory[PATH_MAX];
  char stub[PATH_MAX];
  char library_path[PATH_MAX * 2];
  struct stat info;
  int written;

  if (!game_dir || !*game_dir) return;
  written = snprintf(directory, sizeof directory, "%s/video-compat", game_dir);
  if (written <= 0 || written >= (int)sizeof directory) return;
  written = snprintf(stub, sizeof stub, "%s/libvulkan.so.1", directory);
  if (written <= 0 || written >= (int)sizeof stub || lstat(stub, &info) != 0 ||
      !S_ISREG(info.st_mode) || S_ISLNK(info.st_mode))
    return;

  if (inherited && *inherited) {
    written = snprintf(library_path, sizeof library_path, "%s:%s", directory,
                       inherited);
    if (written <= 0 || written >= (int)sizeof library_path) return;
  } else {
    written = snprintf(library_path, sizeof library_path, "%s", directory);
    if (written <= 0 || written >= (int)sizeof library_path) return;
  }
  (void)setenv("LD_LIBRARY_PATH", library_path, 1);
}

static void hd_wait_briefly(pid_t *pid) {
  if (!pid || *pid <= 0) return;
  int status = 0;
  pid_t child = *pid;
  if (waitpid(child, &status, WNOHANG) == child) {
    *pid = 0;
    return;
  }
  (void)kill(child, SIGTERM);
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int attempt = 0; attempt < 20; ++attempt) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child || (result < 0 && errno == ECHILD)) {
      *pid = 0;
      return;
    }
    nanosleep(&pause, NULL);
  }
  (void)kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  *pid = 0;
}

static void hd_stop_children(void) {
  hd_wait_briefly(&g_video.video_pid);
  hd_wait_briefly(&g_video.audio_pid);
  hd_wait_briefly(&g_video.pacat_pid);
  hd_audio_movie_end();
}

/* Decoder -> sink pair for the movie soundtrack.  Failure is never fatal: the
 * movie keeps playing and the loss is reported once. */
static void hd_spawn_movie_audio(const char *path) {
  if (!g_audio_sink) return;
  int audio_pipe[2];
  if (pipe(audio_pipe) != 0) {
    fprintf(stderr, "[HD-VIDEO] pipe de audio falhou: %s\n", strerror(errno));
    return;
  }

  pid_t audio = fork();
  if (audio == 0) {
    hd_child_setup();
    close(audio_pipe[0]);
    if (dup2(audio_pipe[1], STDOUT_FILENO) < 0) _exit(126);
    close(audio_pipe[1]);
    hd_child_enable_ffmpeg_compat();
    execl(g_ffmpeg, "ffmpeg", "-nostdin", "-hide_banner", "-loglevel",
          "error", "-i", path, "-vn", "-f", "s16le", "-ar", "48000",
          "-ac", "2", "-", (char *)NULL);
    _exit(127);
  }
  if (audio < 0) {
    close(audio_pipe[0]);
    close(audio_pipe[1]);
    return;
  }
  g_video.audio_pid = audio;

  pid_t sink = fork();
  if (sink == 0) {
    hd_child_setup();
    close(audio_pipe[1]);
    if (dup2(audio_pipe[0], STDIN_FILENO) < 0) _exit(126);
    close(audio_pipe[0]);
    if (!strcmp(g_audio_sink, "pacat"))
      execl(g_pacat, "pacat", "--rate=48000", "--channels=2",
            "--format=s16le", (char *)NULL);
    else
      execl(g_aplay, "aplay", "-q", "-f", "S16_LE", "-r", "48000",
            "-c", "2", "-", (char *)NULL);
    _exit(127);
  }
  close(audio_pipe[0]);
  close(audio_pipe[1]);
  if (sink < 0) {
    hd_wait_briefly(&g_video.audio_pid);
    return;
  }
  g_video.pacat_pid = sink;
}

static int hd_spawn_movie_fbdev(const char *path) {
  int width, height, bits_per_pixel;
  hd_fb_size(&width, &height, &bits_per_pixel);
  if (width <= 0 || height <= 0 || bits_per_pixel != 32) {
    fprintf(stderr, "[HD-VIDEO] framebuffer incompativel: %dx%d %dbpp\n",
            width, height, bits_per_pixel);
    return 0;
  }

  char filter[192];
  int filter_length = snprintf(
      filter, sizeof filter,
      "scale=%d:%d:force_original_aspect_ratio=decrease:flags=fast_bilinear,"
      "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,format=bgra",
      width, height, width, height);
  if (filter_length <= 0 || filter_length >= (int)sizeof filter) return 0;

  hd_fb_clear();
  hd_fb_pan_zero();
  g_video.owns_screen = 1;

  if (g_audio_sink) hd_audio_movie_begin();
  hd_spawn_movie_audio(path);

  pid_t video = fork();
  if (video == 0) {
    hd_child_setup();
    hd_child_enable_ffmpeg_compat();
    execl(g_ffmpeg, "ffmpeg", "-nostdin", "-hide_banner", "-loglevel",
          "error", "-re", "-an", "-i", path, "-vf", filter,
          "-pix_fmt", "bgra", "-f", "fbdev", "/dev/fb0",
          (char *)NULL);
    _exit(127);
  }
  if (video < 0) {
    hd_stop_children();
    g_video.owns_screen = 0;
    return 0;
  }
  g_video.video_pid = video;
  fprintf(stderr,
          "[HD-VIDEO] Play %s -> fb0 %dx%d, audio %s "
          "(video=%d audio=%d sink=%d)\n",
          hd_video_basename(path), width, height,
          g_audio_sink ? g_audio_sink : "nenhum", (int)video,
          (int)g_video.audio_pid, (int)g_video.pacat_pid);
  return 1;
}

static int hd_spawn_movie_gl(const char *path) {
  int width = 0, height = 0;
  egl_shim_get_size(&width, &height);
  if (width <= 0 || height <= 0) {
    fprintf(stderr, "[HD-VIDEO] drawable indisponivel para a rota GL\n");
    return 0;
  }
  /* The decoder scales to the real drawable, so the movie never depends on a
   * hard-coded panel size. */
  size_t frame_bytes = (size_t)width * (size_t)height * 4u;
  unsigned char *frame = malloc(frame_bytes);
  if (!frame) {
    fprintf(stderr, "[HD-VIDEO] sem memoria para o quadro %dx%d\n",
            width, height);
    return 0;
  }

  char filter[192];
  int filter_length = snprintf(
      filter, sizeof filter,
      "scale=%d:%d:force_original_aspect_ratio=decrease:flags=fast_bilinear,"
      "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,format=rgba",
      width, height, width, height);
  if (filter_length <= 0 || filter_length >= (int)sizeof filter) {
    free(frame);
    return 0;
  }

  int frame_pipe[2];
  if (pipe(frame_pipe) != 0) {
    fprintf(stderr, "[HD-VIDEO] pipe de video falhou: %s\n", strerror(errno));
    free(frame);
    return 0;
  }

  if (g_audio_sink) hd_audio_movie_begin();
  hd_spawn_movie_audio(path);

  pid_t video = fork();
  if (video == 0) {
    hd_child_setup();
    close(frame_pipe[0]);
    if (dup2(frame_pipe[1], STDOUT_FILENO) < 0) _exit(126);
    close(frame_pipe[1]);
    hd_child_enable_ffmpeg_compat();
    execl(g_ffmpeg, "ffmpeg", "-nostdin", "-hide_banner", "-loglevel",
          "error", "-re", "-an", "-i", path, "-vf", filter,
          "-pix_fmt", "rgba", "-f", "rawvideo", "-", (char *)NULL);
    _exit(127);
  }
  close(frame_pipe[1]);
  if (video < 0) {
    close(frame_pipe[0]);
    free(frame);
    hd_stop_children();
    return 0;
  }

  /* Non-blocking drain from the presentation thread keeps the decoder paced by
   * its own `-re` clock instead of by Unity's swap rate. */
  int flags = fcntl(frame_pipe[0], F_GETFL, 0);
  if (flags >= 0) (void)fcntl(frame_pipe[0], F_SETFL, flags | O_NONBLOCK);
#ifdef F_SETPIPE_SZ
  (void)fcntl(frame_pipe[0], F_SETPIPE_SZ, 1 << 20);
#endif

  g_video.video_pid = video;
  g_video.frame_fd = frame_pipe[0];
  g_video.frame = frame;
  g_video.frame_bytes = frame_bytes;
  g_video.frame_used = 0;
  g_video.frame_ready = 0;
  g_video.frame_width = width;
  g_video.frame_height = height;
  g_video.gl_ready = 0;
  g_video.owns_screen = 1;
  fprintf(stderr,
          "[HD-VIDEO] Play %s -> quad GL %dx%d, audio %s "
          "(video=%d audio=%d sink=%d)\n",
          hd_video_basename(path), width, height,
          g_audio_sink ? g_audio_sink : "nenhum", (int)video,
          (int)g_video.audio_pid, (int)g_video.pacat_pid);
  return 1;
}

static int hd_spawn_movie(const char *path) {
  return g_video_route == HD_ROUTE_GL ? hd_spawn_movie_gl(path)
                                      : hd_spawn_movie_fbdev(path);
}

static void hd_reset_session(int keep_player) {
  void *player = keep_player ? g_video.player : NULL;
  hd_stop_children();
  if (g_video.frame_fd > 0) close(g_video.frame_fd);
  free(g_video.frame);
  memset(&g_video, 0, sizeof g_video);
  g_video.player = player;
  g_video.state = HD_VIDEO_IDLE;
}

static void hd_set_url(void *player, const char *url) {
  /* Always tear the previous session down first: a finished GL session still
   * owns the decoder pipe and its frame buffer. */
  hd_reset_session(0);
  g_video.player = player;
  if (url) snprintf(g_video.url, sizeof g_video.url, "%s", url);
  if (hd_resolve_movie(g_video.url, g_video.path)) {
    g_video.state = HD_VIDEO_READY;
    fprintf(stderr, "[HD-VIDEO] URL %s -> %s\n", g_video.url, g_video.path);
  } else {
    g_video.state = HD_VIDEO_IDLE;
    if (g_video_trace)
      fprintf(stderr, "[HD-VIDEO] URL fora do backend: %s\n", g_video.url);
  }
}

static int hd_try_play(void *player) {
  if (player != g_video.player || g_video.state == HD_VIDEO_IDLE) return 0;
  if (g_video.state == HD_VIDEO_ACTIVE) return 1;
  if (g_video.state != HD_VIDEO_READY || !g_video.path[0]) return 0;
  if (g_no_decoder) {
    /* Without a firmware decoder the authored movie completes immediately:
     * the same FINISHED state of the natural end delivers loopPointReached on
     * the next isPlaying poll, so the authored flow advances while the
     * failing MediaNDK path is never entered.  The game retries Play() every
     * frame while isPlaying is false and only advances on loopPointReached
     * (Vignettes registers no error callback), so leaving the native player
     * to fail keeps the first vignette black forever. */
    g_video.state = HD_VIDEO_FINISHED;
    g_video.completion_emitted = 0;
    if (g_invoke_started) g_invoke_started(player, NULL);
    fprintf(stderr, "[HD-VIDEO] sem decodificador: %s pulado\n",
            hd_video_basename(g_video.path));
    return 1;
  }
  if (!hd_spawn_movie(g_video.path)) return 0;
  g_video.state = HD_VIDEO_ACTIVE;
  g_video.completion_emitted = 0;
  if (g_invoke_started) g_invoke_started(player, NULL);
  return 1;
}

static int hd_query_playing(void *player, int *playing, int *completed) {
  if (playing) *playing = 0;
  if (completed) *completed = 0;
  if (player != g_video.player || g_video.state == HD_VIDEO_IDLE) return 0;
  if (g_video.state == HD_VIDEO_ACTIVE) {
    int status = 0;
    pid_t result = waitpid(g_video.video_pid, &status, WNOHANG);
    if (result == 0) {
      if (playing) *playing = 1;
      return 1;
    }
    if (result == g_video.video_pid || (result < 0 && errno == ECHILD)) {
      if (result == g_video.video_pid &&
          (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
        fprintf(stderr, "[HD-VIDEO] decoder terminou com status=0x%x\n", status);
      g_video.video_pid = 0;
      hd_wait_briefly(&g_video.audio_pid);
      hd_wait_briefly(&g_video.pacat_pid);
      hd_audio_movie_end();
      g_video.owns_screen = 0;
      g_video.state = HD_VIDEO_FINISHED;
      fprintf(stderr, "[HD-VIDEO] fim natural %s\n",
              hd_video_basename(g_video.path));
    } else {
      fprintf(stderr, "[HD-VIDEO] waitpid falhou: %s\n", strerror(errno));
      hd_reset_session(0);
      return 0;
    }
  }
  if (g_video.state == HD_VIDEO_FINISHED && !g_video.completion_emitted) {
    g_video.completion_emitted = 1;
    if (completed) *completed = 1;
    /* Report true for the callback frame. The managed callback updates the URL
     * and the following frame starts the next authored movie exactly once. */
    if (playing) *playing = 1;
  }
  return 1;
}

static void hd_stop(void *player) {
  if (player != g_video.player || g_video.state == HD_VIDEO_IDLE) return;
  if (g_video.state == HD_VIDEO_ACTIVE)
    fprintf(stderr, "[HD-VIDEO] Stop %s\n", hd_video_basename(g_video.path));
  hd_reset_session(0);
}

static uintptr_t hd_set_url_hook(uintptr_t a, uintptr_t b, uintptr_t c,
                                 uintptr_t d, uintptr_t e, uintptr_t f,
                                 uintptr_t g, uintptr_t h) {
  uintptr_t result = g_set_url_original(a, b, c, d, e, f, g, h);
  char url[PATH_MAX];
  if (hd_utf16_to_utf8((void *)b, url, sizeof url))
    hd_set_url((void *)a, url);
  return result;
}

static uintptr_t hd_play_hook(uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f,
                              uintptr_t g, uintptr_t h) {
  if (hd_try_play((void *)a)) return 0;
  return g_play_original(a, b, c, d, e, f, g, h);
}

static uintptr_t hd_stop_hook(uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f,
                              uintptr_t g, uintptr_t h) {
  hd_stop((void *)a);
  return g_stop_original(a, b, c, d, e, f, g, h);
}

static uintptr_t hd_is_playing_hook(uintptr_t a, uintptr_t b, uintptr_t c,
                                    uintptr_t d, uintptr_t e, uintptr_t f,
                                    uintptr_t g, uintptr_t h) {
  int playing = 0, completed = 0;
  if (!hd_query_playing((void *)a, &playing, &completed))
    return g_is_playing_original(a, b, c, d, e, f, g, h);
  if (completed && g_invoke_loop_point) {
    fprintf(stderr, "[HD-VIDEO] loopPointReached %s\n",
            hd_video_basename(g_video.path));
    g_invoke_loop_point((void *)a, NULL);
  }
  return (uintptr_t)(playing != 0);
}

static void *hd_make_trampoline(uintptr_t target, uint32_t **cursor,
                                uint32_t *limit, const char *name) {
  uint32_t *start = *cursor;
  uint32_t *output = start;
  const uint32_t *source = (const uint32_t *)target;
  for (int i = 0; i < 4; ++i) {
    uint32_t instruction = source[i];
    if ((instruction & 0x9F000000u) == 0x90000000u) {
      int destination = instruction & 31;
      long low = (instruction >> 29) & 3;
      long high = (instruction >> 5) & 0x7FFFF;
      long immediate = (high << 2) | low;
      if (immediate & (1L << 20)) immediate -= 1L << 21;
      uint64_t page = ((target + (uintptr_t)i * 4) & ~0xFFFUL) +
                      ((uint64_t)immediate << 12);
      if (output + 4 >= limit) return NULL;
      *output++ = 0x58000040u | (uint32_t)destination;
      *output++ = 0x14000003u;
      *(uint64_t *)output = page;
      output += 2;
    } else if ((instruction & 0x7C000000u) == 0x14000000u ||
               (instruction & 0xFF000000u) == 0x58000000u ||
               (instruction & 0x7C000000u) == 0x94000000u ||
               (instruction & 0xFE000000u) == 0x54000000u) {
      fprintf(stderr, "[HD-VIDEO] %s: instrucao nao relocavel %08x\n",
              name, instruction);
      return NULL;
    } else {
      if (output >= limit) return NULL;
      *output++ = instruction;
    }
  }
  if (output + 4 >= limit) return NULL;
  *output++ = 0x58000051u;
  *output++ = 0xD61F0220u;
  *(uint64_t *)output = target + 16;
  output += 2;
  while (((uintptr_t)output & 15) != 0) *output++ = 0xD503201Fu;
  *cursor = output;
  return start;
}

int hd_video_bridge_install(uintptr_t il2cpp_base, int fbdev_backend) {
  if (g_video_installed) return 1;
  if (!il2cpp_base || hd_env_off("HD_VIDEO_BRIDGE")) return 0;
  g_video_trace = getenv("HD_VIGNETTE_TRACE") != NULL ||
                  getenv("HD_VIDEO_TRACE") != NULL;
  g_video_route = fbdev_backend ? HD_ROUTE_FBDEV : HD_ROUTE_GL;
  /* The authored intro movies are skipped on every firmware: hd_try_play
   * answers each one with an immediate FINISHED, so loopPointReached advances
   * the game through the same native callback of a movie that ended.  The
   * external-decoder route stalls Unity's loop for the whole movie, and the
   * second movie's decoder can outlive the vignette and keep painting behind
   * the menu.  Firmwares without any decoder always needed the skip anyway.
   * HD_PLAY_MOVIES=1 keeps the old route as a diagnostic escape hatch. */
  if (!hd_env_on("HD_PLAY_MOVIES")) {
    g_no_decoder = 1;
    fprintf(stderr,
            "[HD-VIDEO] filmes autorais desativados; o fluxo segue pelo "
            "callback nativo de fim de filme\n");
    g_ffmpeg = NULL;
    g_pacat = NULL;
    g_aplay = NULL;
    g_audio_sink = NULL;
  } else {
    g_ffmpeg = hd_find_program("ffmpeg");
    g_pacat = hd_find_program("pacat");
    g_aplay = hd_find_program("aplay");
    if (g_pacat && hd_pulse_available())
      g_audio_sink = "pacat";
    else if (g_aplay)
      g_audio_sink = "aplay";
    else
      g_audio_sink = NULL;
  }

  if (g_no_decoder) {
    /* nothing else to negotiate: the skip path owns the authored movies */
  } else if (!g_ffmpeg) {
    g_no_decoder = 1;
    fprintf(stderr,
            "[HD-VIDEO] sem decodificador no firmware; os filmes autorais "
            "sao pulados pelo callback nativo de fim de filme\n");
  } else {
    if (g_video_route == HD_ROUTE_FBDEV &&
        access("/dev/fb0", R_OK | W_OK) != 0) {
      fprintf(stderr, "[HD-VIDEO] rota fbdev sem /dev/fb0 gravavel\n");
      return 0;
    }
    fprintf(stderr, "[HD-VIDEO] rota=%s audio=%s\n",
            g_video_route == HD_ROUTE_GL ? "quad GL (SDL possui o scanout)"
                                         : "fbdev",
            g_audio_sink ? g_audio_sink : "nenhum (filme fica mudo)");
  }

  static const HdVideoTarget targets_200023[] = {
      {0x2BE7628,
       {0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,
        0x55,0x1b,0x00,0xd0,0xa2,0xc6,0x41,0xf9},
       "VideoPlayer.set_url", hd_set_url_hook, &g_set_url_original},
      {0x2BE777C,
       {0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,
        0x54,0x1b,0x00,0xd0,0x81,0xda,0x41,0xf9},
       "VideoPlayer.Play", hd_play_hook, &g_play_original},
      {0x2BE77B8,
       {0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,
        0x54,0x1b,0x00,0xd0,0x81,0xde,0x41,0xf9},
       "VideoPlayer.Stop", hd_stop_hook, &g_stop_original},
      {0x2BE77F4,
       {0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,
        0x54,0x1b,0x00,0xd0,0x81,0xe2,0x41,0xf9},
       "VideoPlayer.get_isPlaying", hd_is_playing_hook,
       &g_is_playing_original},
  };
  static const HdVideoTarget targets_200036[] = {
      {0x3790CA8,
       {0xff,0x43,0x01,0xd1,0xfe,0x57,0x03,0xa9,
        0xf4,0x4f,0x04,0xa9,0x53,0x28,0x00,0xf0},
       "VideoPlayer.set_url", hd_set_url_hook, &g_set_url_original},
      {0x37911CC,
       {0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,
        0x53,0x28,0x00,0xd0,0xf4,0x03,0x00,0xaa},
       "VideoPlayer.Play", hd_play_hook, &g_play_original},
      {0x3791280,
       {0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,
        0x53,0x28,0x00,0xd0,0xf4,0x03,0x00,0xaa},
       "VideoPlayer.Stop", hd_stop_hook, &g_stop_original},
      {0x3791334,
       {0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,
        0x53,0x28,0x00,0xd0,0xf4,0x03,0x00,0xaa},
       "VideoPlayer.get_isPlaying", hd_is_playing_hook,
       &g_is_playing_original},
  };
  const HdVideoTarget *targets = targets_200023;
  size_t target_count = sizeof targets_200023 / sizeof targets_200023[0];
  uintptr_t loop_rva = 0x2BE7AC0, started_rva = 0x2BE7AF4;
  if (hd_build_current() == HD_BUILD_200036) {
    targets = targets_200036;
    target_count = sizeof targets_200036 / sizeof targets_200036[0];
    loop_rva = 0x3791838;
    started_rva = 0x3791864;
  } else if (hd_build_current() != HD_BUILD_200023) {
    fprintf(stderr, "[HD-VIDEO] perfil de build desconhecido\n");
    return 0;
  }
  for (size_t i = 0; i < target_count; ++i) {
    if (memcmp((const void *)(il2cpp_base + targets[i].rva),
               targets[i].signature, sizeof targets[i].signature) != 0) {
      fprintf(stderr, "[HD-VIDEO] assinatura divergente em %s\n",
              targets[i].name);
      return 0;
    }
  }

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) page_size = 4096;
  uint32_t *trampoline_page = mmap(NULL, (size_t)page_size,
                                   PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (trampoline_page == MAP_FAILED) return 0;
  uint32_t *cursor = trampoline_page;
  uint32_t *limit = trampoline_page + page_size / (long)sizeof(uint32_t);
  void *trampolines[4];
  for (size_t i = 0; i < target_count; ++i) {
    trampolines[i] = hd_make_trampoline(il2cpp_base + targets[i].rva,
                                        &cursor, limit, targets[i].name);
    if (!trampolines[i]) {
      munmap(trampoline_page, (size_t)page_size);
      return 0;
    }
  }

  uintptr_t target_page =
      (il2cpp_base + targets[0].rva) & ~((uintptr_t)page_size - 1);
  uintptr_t target_end =
      ((il2cpp_base + targets[target_count - 1].rva + 16 + page_size - 1) &
       ~((uintptr_t)page_size - 1));
  size_t target_span = (size_t)(target_end - target_page);
  if (mprotect((void *)target_page, target_span,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    munmap(trampoline_page, (size_t)page_size);
    return 0;
  }
  for (size_t i = 0; i < target_count; ++i) {
    *targets[i].original = (HdVideoMethod)trampolines[i];
    hook_arm64(il2cpp_base + targets[i].rva, (uintptr_t)targets[i].hook);
  }
  __builtin___clear_cache((char *)target_page,
                          (char *)target_page + target_span);
  mprotect((void *)target_page, target_span, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)trampoline_page, (char *)cursor);
  mprotect(trampoline_page, (size_t)page_size, PROT_READ | PROT_EXEC);

  g_il2cpp_base = il2cpp_base;
  g_invoke_loop_point =
      (void (*)(void *, void *))(il2cpp_base + loop_rva);
  g_invoke_started =
      (void (*)(void *, void *))(il2cpp_base + started_rva);
  g_video_installed = 1;
  fprintf(stderr,
          "[HD-VIDEO] bridge VideoPlayer instalado (callbacks Unity nativos)\n");
  return 1;
}

int hd_video_bridge_owns_screen(void) {
  return g_video_installed && g_video.owns_screen;
}

/* Drain everything the decoder has produced so far, keeping only the newest
 * complete frame.  Returns 1 when a fresh frame is ready to upload. */
static int hd_video_drain_frames(void) {
  if (g_video.frame_fd <= 0 || !g_video.frame) return g_video.frame_ready;
  for (;;) {
    ssize_t got = read(g_video.frame_fd, g_video.frame + g_video.frame_used,
                       g_video.frame_bytes - g_video.frame_used);
    if (got > 0) {
      g_video.frame_used += (size_t)got;
      if (g_video.frame_used == g_video.frame_bytes) {
        g_video.frame_used = 0;
        g_video.frame_ready = 1;
      }
      continue;
    }
    if (got == 0) break;                        /* decoder closed the stream */
    if (errno == EINTR) continue;
    break;                                      /* EAGAIN: nothing more yet */
  }
  return g_video.frame_ready;
}

int hd_video_bridge_present_tick(void) {
  static unsigned ticks;
  if (!hd_video_bridge_owns_screen()) return 0;

  if (g_video_route != HD_ROUTE_GL) {
    if (++ticks == 1 || ticks % 15 == 0) hd_fb_pan_zero();
    return 0;  /* the decoder owns the scanout; Unity must not swap */
  }

  if (!g_video.gl_ready) {
    if (!hd_video_gl_begin(g_video.frame_width, g_video.frame_height)) {
      /* Narrow, logged fallback: without a presenter the movie cannot be
       * shown, so hand the screen back and let the managed callbacks advance
       * the boot flow instead of stalling on a black frame. */
      static int reported;
      if (!reported++)
        fprintf(stderr,
                "[HD-VIDEO] apresentador GL indisponivel; filme %s sem "
                "imagem (fluxo do jogo preservado)\n",
                hd_video_basename(g_video.path));
      g_video.owns_screen = 0;
      return 0;
    }
    g_video.gl_ready = 1;
  }

  int fresh = hd_video_drain_frames();
  hd_video_gl_present(fresh ? g_video.frame : NULL);
  g_video.frame_ready = 0;
  return 1;  /* the loader drew this frame; the caller performs the swap */
}

void hd_video_bridge_shutdown(void) {
  if (!g_video_installed) return;
  if (g_video.state == HD_VIDEO_ACTIVE)
    fprintf(stderr, "[HD-VIDEO] encerrando player externo\n");
  hd_reset_session(0);
  /* The GL objects belong to the render thread's context; only release them
   * when shutdown really runs there. */
  if (g_video_route == HD_ROUTE_GL) hd_video_gl_end();
}
