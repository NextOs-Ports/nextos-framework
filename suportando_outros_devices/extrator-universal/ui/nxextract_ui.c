/*
 * SPDX-License-Identifier: MIT
 *
 * nxextract-ui: backend-neutral first-run screen for NXEXTRACT_V1.
 *
 * SDL2 is loaded at runtime so the same binary follows the firmware's own video
 * backend. If SDL cannot open a visible window, the same SDL software renderer
 * and draw_screen() layout are used directly on a validated Linux framebuffer.
 * The text-console renderer is diagnostic-only and never proves public UI
 * readiness. Public runners require a graphical renderer readiness proof before
 * extraction may begin.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Rect {
  int x, y, w, h;
} SDL_Rect;
typedef union SDL_Event {
  uint32_t type;
  unsigned char padding[256];
} SDL_Event;

enum {
  SDL_INIT_VIDEO = 0x00000020u,
  SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
  SDL_RENDERER_SOFTWARE = 0x00000001u,
  SDL_RENDERER_ACCELERATED = 0x00000002u,
  SDL_RENDERER_PRESENTVSYNC = 0x00000004u,
  SDL_QUIT_EVENT = 0x00000100u
};

static int (*pSDL_Init)(uint32_t);
static void (*pSDL_Quit)(void);
static const char *(*pSDL_GetError)(void);
static const char *(*pSDL_GetCurrentVideoDriver)(void);
static int (*pSDL_GetNumVideoDrivers)(void);
static const char *(*pSDL_GetVideoDriver)(int);
static SDL_Window *(*pSDL_CreateWindow)(const char *, int, int, int, int,
                                        uint32_t);
static void (*pSDL_DestroyWindow)(SDL_Window *);
static SDL_Renderer *(*pSDL_CreateRenderer)(SDL_Window *, int, uint32_t);
static SDL_Surface *(*pSDL_CreateRGBSurfaceFrom)(void *, int, int, int, int,
                                                 uint32_t, uint32_t, uint32_t,
                                                 uint32_t);
static void (*pSDL_FreeSurface)(SDL_Surface *);
static SDL_Renderer *(*pSDL_CreateSoftwareRenderer)(SDL_Surface *);
static void (*pSDL_DestroyRenderer)(SDL_Renderer *);
static int (*pSDL_GetRendererOutputSize)(SDL_Renderer *, int *, int *);
static int (*pSDL_SetRenderDrawColor)(SDL_Renderer *, uint8_t, uint8_t, uint8_t,
                                      uint8_t);
static int (*pSDL_RenderClear)(SDL_Renderer *);
static int (*pSDL_RenderFillRect)(SDL_Renderer *, const SDL_Rect *);
static int (*pSDL_RenderDrawRect)(SDL_Renderer *, const SDL_Rect *);
static void (*pSDL_RenderPresent)(SDL_Renderer *);
static int (*pSDL_PollEvent)(SDL_Event *);
static void (*pSDL_Delay)(uint32_t);

static void *sdl_handle;

#define SDL_LOAD(symbol)                                                       \
  do {                                                                         \
    *(void **)(&p##symbol) = dlsym(sdl_handle, #symbol);                       \
    if (!p##symbol) {                                                          \
      fprintf(stderr, "nxextract-ui: missing %s\n", #symbol);                  \
      return 0;                                                                \
    }                                                                          \
  } while (0)

static int load_sdl(void) {
  static const char *names[] = {
      "libSDL2-2.0.so.0", "libSDL2.so.0", "libSDL2.so", NULL,
  };
  int index;
  for (index = 0; names[index] && !sdl_handle; index++)
    sdl_handle = dlopen(names[index], RTLD_NOW | RTLD_LOCAL);
  if (!sdl_handle) {
    fprintf(stderr, "nxextract-ui: SDL2 unavailable: %s\n", dlerror());
    return 0;
  }
  SDL_LOAD(SDL_Init);
  SDL_LOAD(SDL_Quit);
  SDL_LOAD(SDL_GetError);
  SDL_LOAD(SDL_GetCurrentVideoDriver);
  SDL_LOAD(SDL_GetNumVideoDrivers);
  SDL_LOAD(SDL_GetVideoDriver);
  SDL_LOAD(SDL_CreateWindow);
  SDL_LOAD(SDL_DestroyWindow);
  SDL_LOAD(SDL_CreateRenderer);
  SDL_LOAD(SDL_CreateRGBSurfaceFrom);
  SDL_LOAD(SDL_FreeSurface);
  SDL_LOAD(SDL_CreateSoftwareRenderer);
  SDL_LOAD(SDL_DestroyRenderer);
  SDL_LOAD(SDL_GetRendererOutputSize);
  SDL_LOAD(SDL_SetRenderDrawColor);
  SDL_LOAD(SDL_RenderClear);
  SDL_LOAD(SDL_RenderFillRect);
  SDL_LOAD(SDL_RenderDrawRect);
  SDL_LOAD(SDL_RenderPresent);
  SDL_LOAD(SDL_PollEvent);
  SDL_LOAD(SDL_Delay);
  return 1;
}

#undef SDL_LOAD

typedef struct ProgressState {
  int state;
  int active;
  int phase;
  int overall;
  int phase_progress;
  uint64_t done_bytes;
  uint64_t total_bytes;
  char message[256];
  char detail[256];
} ProgressState;

typedef struct Color {
  uint8_t r, g, b, a;
} Color;

static const char *phase_labels[] = {
    "PREPARING",       "SCANNING FILES",  "VALIDATING PACKAGES",
    "SELECTING DATA",  "EXTRACTING DATA", "PROCESSING DATA",
    "VALIDATING DATA", "INSTALLING DATA", "READY",
};

static int path_exists(const char *path) {
  struct stat value;
  return path && stat(path, &value) == 0;
}

/* P1: canal privado de sessao por descritores herdados ("fd:N" no argv).
 * O motor cria os pipes, valida dono/tipo/identidade e entrega somente os
 * descritores; nenhum pathname de sessao existe para um firmware reciclar
 * (XDG_RUNTIME_DIR com Linger=no) ou um FAT sem chmod enfraquecer. Um token
 * "fd:" invalido e' falha fechada; um pathname absoluto segue no contrato
 * historico para compatibilidade com motores antigos. */
static int session_stop_fd = -1;
static int session_ready_fd = -1;

static int parse_session_fd_token(const char *token) {
  char *end;
  long value;
  struct stat info;
  if (!token || strncmp(token, "fd:", 3) != 0)
    return -1;
  errno = 0;
  value = strtol(token + 3, &end, 10);
  if (errno != 0 || end == token + 3 || *end != 0 || value < 3 ||
      value > 65535)
    return -2;
  if (fstat((int)value, &info) != 0 || !S_ISFIFO(info.st_mode))
    return -2;
  return (int)value;
}

static int stop_requested(const char *stop_path) {
  if (session_stop_fd >= 0) {
    struct pollfd probe;
    int result;
    probe.fd = session_stop_fd;
    probe.events = POLLIN;
    probe.revents = 0;
    result = poll(&probe, 1, 0);
    if (result < 0)
      return errno != EINTR; /* canal quebrado = parar */
    return result > 0 &&
           (probe.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
  }
  return path_exists(stop_path);
}

static int directory_usable(const char *path) {
  struct stat value;
  if (!path || !*path || stat(path, &value) != 0 || !S_ISDIR(value.st_mode))
    return 0;
  return access(path, R_OK | W_OK | X_OK) == 0;
}

static int wayland_socket_exists(void) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  const char *display = getenv("WAYLAND_DISPLAY");
  char path[512];
  struct stat value;
  if (!runtime || !*runtime || !display || !*display || strchr(display, '/'))
    return 0;
  if (snprintf(path, sizeof(path), "%s/%s", runtime, display) >=
      (int)sizeof(path))
    return 0;
  return stat(path, &value) == 0 && S_ISSOCK(value.st_mode);
}

/*
 * Reuse the session directories already created by the firmware. This is the
 * proven Sonic/Dysmantle fallback: it never starts a compositor and never
 * chooses a video backend. It only makes an existing Wayland socket visible.
 */
static void configure_session_runtime(void) {
  char run_user[64], var_run_user[64], run_legacy[64];
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  const char *candidates[7];
  int index;

  snprintf(run_user, sizeof(run_user), "/run/user/%u", (unsigned)getuid());
  snprintf(var_run_user, sizeof(var_run_user), "/var/run/user/%u",
           (unsigned)getuid());
  snprintf(run_legacy, sizeof(run_legacy), "/run/%u-runtime-dir",
           (unsigned)getuid());
  candidates[0] = run_legacy;
  candidates[1] = run_user;
  candidates[2] = var_run_user;
  candidates[3] = "/run/0-runtime-dir";
  candidates[4] = "/var/run/0-runtime-dir";
  candidates[5] = NULL;

  if (!directory_usable(runtime)) {
    for (index = 0; candidates[index]; index++) {
      if (!directory_usable(candidates[index]))
        continue;
      if (setenv("XDG_RUNTIME_DIR", candidates[index], 1) == 0) {
        runtime = getenv("XDG_RUNTIME_DIR");
        fprintf(stderr, "nxextract-ui: session runtime=%s\n", runtime);
      }
      break;
    }
  }

  if (!wayland_socket_exists() && runtime && *runtime) {
    DIR *directory = opendir(runtime);
    struct dirent *entry;
    if (!directory)
      return;
    while ((entry = readdir(directory)) != NULL) {
      char path[512];
      struct stat value;
      if (strncmp(entry->d_name, "wayland-", 8) != 0 ||
          strstr(entry->d_name, ".lock") != NULL)
        continue;
      if (snprintf(path, sizeof(path), "%s/%s", runtime, entry->d_name) >=
          (int)sizeof(path))
        continue;
      if (stat(path, &value) != 0 || !S_ISSOCK(value.st_mode))
        continue;
      if (setenv("WAYLAND_DISPLAY", entry->d_name, 1) == 0)
        fprintf(stderr, "nxextract-ui: wayland socket=%s\n", entry->d_name);
      break;
    }
    closedir(directory);
  }
}

static int clamp_permille(int value) {
  if (value < 0)
    return 0;
  if (value > 1000)
    return 1000;
  return value;
}

static int read_progress(const char *path, ProgressState *state) {
  FILE *stream;
  ProgressState next = *state;
  char line[512];
  unsigned long long done = 0, total = 0;
  int legacy_total = 1000;

  stream = fopen(path, "r");
  if (!stream)
    return 0;
  if (!fgets(line, sizeof(line), stream) ||
      sscanf(line, "%d %d %d", &next.state, &next.overall, &legacy_total) != 3) {
    fclose(stream);
    return 0;
  }
  if (!fgets(next.message, sizeof(next.message), stream)) {
    fclose(stream);
    return 0;
  }
  next.message[strcspn(next.message, "\r\n")] = 0;
  if (!fgets(line, sizeof(line), stream) ||
      sscanf(line, "NXEXTRACT_V1 %d %d %d %llu %llu", &next.phase,
             &next.overall, &next.phase_progress, &done, &total) != 5) {
    next.phase = next.state == 3 ? 8 : 0;
    next.phase_progress =
        legacy_total > 0 ? next.overall * 1000 / legacy_total : 0;
    next.overall = next.phase_progress;
  }
  if (fgets(next.detail, sizeof(next.detail), stream))
    next.detail[strcspn(next.detail, "\r\n")] = 0;
  else
    next.detail[0] = 0;
  fclose(stream);

  next.state = next.state < 1 || next.state > 3 ? 1 : next.state;
  next.phase = next.phase < 0 || next.phase > 8 ? 0 : next.phase;
  next.overall = clamp_permille(next.overall);
  next.phase_progress = clamp_permille(next.phase_progress);
  next.done_bytes = (uint64_t)done;
  next.total_bytes = (uint64_t)total;
  next.active = next.state == 1 && next.phase_progress < 1000;
  *state = next;
  return 1;
}

static int open_console_path(const char *path) {
  struct stat value;
  if (!path || path[0] != '/' || lstat(path, &value) != 0 ||
      !S_ISCHR(value.st_mode) || S_ISLNK(value.st_mode))
    return -1;
  return open(path, O_WRONLY | O_NOCTTY | O_CLOEXEC);
}

static int active_console_path(char *path, size_t capacity) {
  static const char active_path[] = "/sys/class/tty/tty0/active";
  char name[64];
  ssize_t length;
  size_t used, index;
  int descriptor, written;

  if (!path || capacity < sizeof("/dev/tty1"))
    return 0;
  descriptor = open(active_path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    return 0;
  length = read(descriptor, name, sizeof(name) - 1u);
  close(descriptor);
  if (length <= 0)
    return 0;
  used = (size_t)length;
  name[used] = 0;
  while (used > 0u && isspace((unsigned char)name[used - 1u]))
    name[--used] = 0;
  if (used < 4u || strncmp(name, "tty", 3u) != 0)
    return 0;
  for (index = 3u; index < used; index++) {
    if (!isdigit((unsigned char)name[index]))
      return 0;
  }
  if (name[3] == '0' && name[4] == 0)
    return 0;
  written = snprintf(path, capacity, "/dev/%s", name);
  return written > 0 && (size_t)written < capacity;
}

static int open_console(char *selected, size_t selected_capacity) {
  const char *configured = getenv("NXEXTRACT_UI_TTY");
  char active[96];
  int descriptor;

  if (configured && *configured) {
    descriptor = open_console_path(configured);
    if (descriptor >= 0) {
      (void)snprintf(selected, selected_capacity, "%s", configured);
      return descriptor;
    }
  }
  if (!active_console_path(active, sizeof(active)))
    return -1;
  descriptor = open_console_path(active);
  if (descriptor >= 0)
    (void)snprintf(selected, selected_capacity, "%s", active);
  return descriptor;
}

static void sanitize_console_text(const char *input, char *output,
                                  size_t capacity) {
  size_t used = 0;
  const unsigned char *cursor =
      (const unsigned char *)(input ? input : "");
  if (!capacity)
    return;
  while (*cursor && used + 1u < capacity) {
    unsigned char value = *cursor++;
    if (value >= 0x20u && value < 0x7fu)
      output[used++] = (char)value;
    else if (value >= 0x80u) {
      while ((*cursor & 0xc0u) == 0x80u)
        cursor++;
      output[used++] = '?';
    } else if (value == '\t') {
      output[used++] = ' ';
    }
  }
  output[used] = 0;
}

static void make_console_bar(char *bar, size_t capacity, int permille) {
  size_t width = capacity > 0u ? capacity - 1u : 0u;
  size_t filled, index;
  permille = clamp_permille(permille);
  filled = width * (size_t)permille / 1000u;
  for (index = 0; index < width; index++)
    bar[index] = index < filled ? '#' : '-';
  if (capacity)
    bar[width] = 0;
}

static int publish_ready(const char *ready_path, const char *renderer) {
  int descriptor, success = 1;
  if (session_ready_fd >= 0) {
    if (dprintf(session_ready_fd, "visible=%s\n",
                renderer ? renderer : "unknown") < 0)
      success = 0;
    if (close(session_ready_fd) != 0)
      success = 0;
    session_ready_fd = -1;
    if (!success)
      fprintf(stderr, "nxextract-ui: cannot seal visible readiness\n");
    return success;
  }
  if (!ready_path || ready_path[0] != '/')
    return 0;
  descriptor = open(ready_path,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
  if (descriptor < 0) {
    fprintf(stderr, "nxextract-ui: cannot publish visible readiness: %s\n",
            strerror(errno));
    return 0;
  }
  if (dprintf(descriptor, "visible=%s\n", renderer ? renderer : "unknown") < 0)
    success = 0;
  if (fsync(descriptor) != 0)
    success = 0;
  if (close(descriptor) != 0)
    success = 0;
  if (!success) {
    fprintf(stderr, "nxextract-ui: cannot seal visible readiness\n");
    return 0;
  }
  return 1;
}

static void run_console_ui(int descriptor, const char *progress_path,
                           const char *stop_path, const char *title,
                           const char *version, ProgressState *progress) {
  char clean_title[64], clean_version[64], clean_message[64], clean_detail[64];
  char phase_bar[31], overall_bar[31];
  int first = 1;

  sanitize_console_text(title, clean_title, sizeof(clean_title));
  sanitize_console_text(version, clean_version, sizeof(clean_version));
  while (!stop_requested(stop_path)) {
    (void)read_progress(progress_path, progress);
    sanitize_console_text(progress->message, clean_message,
                          sizeof(clean_message));
    sanitize_console_text(progress->detail, clean_detail, sizeof(clean_detail));
    make_console_bar(phase_bar, sizeof(phase_bar), progress->phase_progress);
    make_console_bar(overall_bar, sizeof(overall_bar), progress->overall);
    (void)dprintf(
        descriptor,
        "%s\033[H"
        "+--------------------------------------------------------------+\r\n"
        "| NEXTOS NXEXTRACT UNIVERSAL DATA SETUP                       |\r\n"
        "+--------------------------------------------------------------+\r\n"
        "| GAME / JOGO: %-47.47s |\r\n"
        "| RECIPE: %-51.51s |\r\n"
        "|                                                              |\r\n"
        "| PHASE / ETAPA: %-44.44s |\r\n"
        "| STATUS: %-51.51s |\r\n"
        "| FILE / ARQUIVO: %-43.43s |\r\n"
        "|                                                              |\r\n"
        "| PROGRESS / PROGRESSO [%-30s] %3d%% |\r\n"
        "| OVERALL / GERAL     [%-30s] %3d%% |\r\n"
        "|                                                              |\r\n"
        "| LOADING / CARREGANDO - PLEASE WAIT / AGUARDE                 |\r\n"
        "|                         RETRO ELITE                          |\r\n"
        "+--------------------------------------------------------------+\r\n",
        first ? "\033[?25l\033[2J" : "", clean_title, clean_version,
        phase_labels[progress->phase], clean_message, clean_detail, phase_bar,
        progress->phase_progress / 10, overall_bar, progress->overall / 10);
    first = 0;
    usleep(100000);
  }
  (void)dprintf(descriptor, "\033[2J\033[H");
}

static const char **glyph5x7(unsigned codepoint) {
  static const char *blank[] = {"00000", "00000", "00000", "00000",
                                "00000", "00000", "00000"};
  static const char *question[] = {"01110", "10001", "00001", "00010",
                                   "00100", "00000", "00100"};
  static const char *dot[] = {"00000", "00000", "00000", "00000",
                              "00000", "01100", "01100"};
  static const char *comma[] = {"00000", "00000", "00000", "00000",
                                "01100", "01100", "01000"};
  static const char *dash[] = {"00000", "00000", "00000", "11111",
                               "00000", "00000", "00000"};
  static const char *underscore[] = {"00000", "00000", "00000", "00000",
                                     "00000", "00000", "11111"};
  static const char *slash[] = {"00001", "00010", "00010", "00100",
                                "01000", "01000", "10000"};
  static const char *colon[] = {"00000", "01100", "01100", "00000",
                                "01100", "01100", "00000"};
  static const char *pct[] = {"11001", "11010", "00100", "01000",
                              "10110", "00110", "00000"};
  static const char *plus[] = {"00000", "00100", "00100", "11111",
                               "00100", "00100", "00000"};
  static const char *left[] = {"00010", "00100", "01000", "01000",
                               "01000", "00100", "00010"};
  static const char *right[] = {"01000", "00100", "00010", "00010",
                                "00010", "00100", "01000"};
  static const char *pipe[] = {"00100", "00100", "00100", "00100",
                               "00100", "00100", "00100"};
  static const char *n0[] = {"01110", "10001", "10011", "10101",
                             "11001", "10001", "01110"};
  static const char *n1[] = {"00100", "01100", "00100", "00100",
                             "00100", "00100", "01110"};
  static const char *n2[] = {"01110", "10001", "00001", "00010",
                             "00100", "01000", "11111"};
  static const char *n3[] = {"11110", "00001", "00001", "01110",
                             "00001", "00001", "11110"};
  static const char *n4[] = {"00010", "00110", "01010", "10010",
                             "11111", "00010", "00010"};
  static const char *n5[] = {"11111", "10000", "10000", "11110",
                             "00001", "00001", "11110"};
  static const char *n6[] = {"00110", "01000", "10000", "11110",
                             "10001", "10001", "01110"};
  static const char *n7[] = {"11111", "00001", "00010", "00100",
                             "01000", "01000", "01000"};
  static const char *n8[] = {"01110", "10001", "10001", "01110",
                             "10001", "10001", "01110"};
  static const char *n9[] = {"01110", "10001", "10001", "01111",
                             "00001", "00010", "01100"};
  static const char *letters[][7] = {
      {"01110", "10001", "10001", "11111", "10001", "10001", "10001"},
      {"11110", "10001", "10001", "11110", "10001", "10001", "11110"},
      {"01110", "10001", "10000", "10000", "10000", "10001", "01110"},
      {"11110", "10001", "10001", "10001", "10001", "10001", "11110"},
      {"11111", "10000", "10000", "11110", "10000", "10000", "11111"},
      {"11111", "10000", "10000", "11110", "10000", "10000", "10000"},
      {"01110", "10001", "10000", "10111", "10001", "10001", "01110"},
      {"10001", "10001", "10001", "11111", "10001", "10001", "10001"},
      {"01110", "00100", "00100", "00100", "00100", "00100", "01110"},
      {"00001", "00001", "00001", "00001", "10001", "10001", "01110"},
      {"10001", "10010", "10100", "11000", "10100", "10010", "10001"},
      {"10000", "10000", "10000", "10000", "10000", "10000", "11111"},
      {"10001", "11011", "10101", "10101", "10001", "10001", "10001"},
      {"10001", "11001", "10101", "10011", "10001", "10001", "10001"},
      {"01110", "10001", "10001", "10001", "10001", "10001", "01110"},
      {"11110", "10001", "10001", "11110", "10000", "10000", "10000"},
      {"01110", "10001", "10001", "10001", "10101", "10010", "01101"},
      {"11110", "10001", "10001", "11110", "10100", "10010", "10001"},
      {"01111", "10000", "10000", "01110", "00001", "00001", "11110"},
      {"11111", "00100", "00100", "00100", "00100", "00100", "00100"},
      {"10001", "10001", "10001", "10001", "10001", "10001", "01110"},
      {"10001", "10001", "10001", "10001", "10001", "01010", "00100"},
      {"10001", "10001", "10001", "10101", "10101", "10101", "01010"},
      {"10001", "10001", "01010", "00100", "01010", "10001", "10001"},
      {"10001", "10001", "01010", "00100", "00100", "00100", "00100"},
      {"11111", "00001", "00010", "00100", "01000", "10000", "11111"},
  };
  static const char **numbers[] = {n0, n1, n2, n3, n4, n5, n6, n7, n8, n9};

  if (codepoint >= 'a' && codepoint <= 'z')
    codepoint = (unsigned)toupper((int)codepoint);
  if (codepoint >= 'A' && codepoint <= 'Z')
    return letters[codepoint - 'A'];
  if (codepoint >= '0' && codepoint <= '9')
    return numbers[codepoint - '0'];
  switch (codepoint) {
  case ' ': return blank;
  case '.': return dot;
  case ',': return comma;
  case '-': return dash;
  case '_': return underscore;
  case '/': return slash;
  case ':': return colon;
  case '%': return pct;
  case '+': return plus;
  case '(': return left;
  case ')': return right;
  case '|': return pipe;
  default: return question;
  }
}

static unsigned next_codepoint(const unsigned char **cursor) {
  const unsigned char *value = *cursor;
  unsigned codepoint;
  if (*value < 0x80) {
    *cursor = value + 1;
    return *value;
  }
  if ((*value & 0xE0) == 0xC0 && value[1]) {
    codepoint = ((unsigned)(value[0] & 0x1F) << 6) | (value[1] & 0x3F);
    *cursor = value + 2;
  } else if ((*value & 0xF0) == 0xE0 && value[1] && value[2]) {
    codepoint = ((unsigned)(value[0] & 0x0F) << 12) |
                ((unsigned)(value[1] & 0x3F) << 6) | (value[2] & 0x3F);
    *cursor = value + 3;
  } else {
    *cursor = value + 1;
    return '?';
  }
  switch (codepoint) {
  case 0x00C1: case 0x00C0: case 0x00C2: case 0x00C3: case 0x00C4:
  case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E3: case 0x00E4:
    return 'A';
  case 0x00C9: case 0x00C8: case 0x00CA: case 0x00CB:
  case 0x00E9: case 0x00E8: case 0x00EA: case 0x00EB:
    return 'E';
  case 0x00CD: case 0x00CC: case 0x00CE: case 0x00CF:
  case 0x00ED: case 0x00EC: case 0x00EE: case 0x00EF:
    return 'I';
  case 0x00D3: case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6:
  case 0x00F3: case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6:
    return 'O';
  case 0x00DA: case 0x00D9: case 0x00DB: case 0x00DC:
  case 0x00FA: case 0x00F9: case 0x00FB: case 0x00FC:
    return 'U';
  case 0x00C7: case 0x00E7:
    return 'C';
  default:
    return codepoint < 128 ? codepoint : '?';
  }
}

static int text_width(const char *text, int scale) {
  const unsigned char *cursor = (const unsigned char *)text;
  int count = 0;
  while (cursor && *cursor) {
    (void)next_codepoint(&cursor);
    count++;
  }
  return count ? count * 6 * scale - scale : 0;
}

static void set_color(SDL_Renderer *renderer, Color color) {
  pSDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

static void fill(SDL_Renderer *renderer, int x, int y, int width, int height,
                 Color color) {
  SDL_Rect rect = {x, y, width, height};
  if (width <= 0 || height <= 0)
    return;
  set_color(renderer, color);
  pSDL_RenderFillRect(renderer, &rect);
}

static void outline(SDL_Renderer *renderer, int x, int y, int width, int height,
                    Color color) {
  SDL_Rect rect = {x, y, width, height};
  set_color(renderer, color);
  pSDL_RenderDrawRect(renderer, &rect);
}

static void draw_text(SDL_Renderer *renderer, int x, int y, int scale,
                      const char *text, Color color) {
  SDL_Rect pixel = {0, 0, scale, scale};
  const unsigned char *cursor = (const unsigned char *)text;
  set_color(renderer, color);
  while (cursor && *cursor) {
    unsigned codepoint = next_codepoint(&cursor);
    const char **glyph = glyph5x7(codepoint);
    int yy, xx;
    for (yy = 0; yy < 7; yy++) {
      for (xx = 0; xx < 5; xx++) {
        if (glyph[yy][xx] == '1') {
          pixel.x = x + xx * scale;
          pixel.y = y + yy * scale;
          pSDL_RenderFillRect(renderer, &pixel);
        }
      }
    }
    x += 6 * scale;
  }
}

static void draw_text_centered(SDL_Renderer *renderer, int center, int y,
                               int scale, const char *text, Color color) {
  draw_text(renderer, center - text_width(text, scale) / 2, y, scale, text,
            color);
}

static void ellipsize(char *text, size_t capacity, int max_chars) {
  size_t length = strlen(text);
  if (max_chars < 1) {
    text[0] = 0;
    return;
  }
  if ((size_t)max_chars >= length)
    return;
  if ((size_t)max_chars >= capacity)
    max_chars = (int)capacity - 1;
  if (max_chars <= 3) {
    memset(text, '.', (size_t)max_chars);
    text[max_chars] = 0;
    return;
  }
  memcpy(text + max_chars - 3, "...", 4);
}

static int wrap_text(const char *input, char lines[][128], int max_lines,
                     int max_chars) {
  const char *cursor = input ? input : "";
  int count = 0;
  if (max_chars < 8)
    max_chars = 8;
  if (max_chars > 127)
    max_chars = 127;
  while (*cursor && count < max_lines) {
    int used = 0;
    while (*cursor == ' ' || *cursor == '\t')
      cursor++;
    while (*cursor && used < max_chars) {
      const char *word = cursor;
      int length = 0;
      while (word[length] && word[length] != ' ' && word[length] != '\t')
        length++;
      if (used && used + 1 + length > max_chars)
        break;
      if (!used && length > max_chars)
        length = max_chars;
      if (used)
        lines[count][used++] = ' ';
      memcpy(lines[count] + used, word, (size_t)length);
      used += length;
      lines[count][used] = 0;
      cursor += length;
      while (*cursor == ' ' || *cursor == '\t')
        cursor++;
      if (length == max_chars)
        break;
    }
    count++;
  }
  if (*cursor && count) {
    int used = (int)strlen(lines[count - 1]);
    if (used > max_chars - 3)
      used = max_chars - 3;
    memcpy(lines[count - 1] + used, "...", 4);
  }
  if (!count) {
    lines[0][0] = 0;
    count = 1;
  }
  return count;
}

static void draw_bar(SDL_Renderer *renderer, int x, int y, int width,
                     int height, int value, Color accent, int active,
                     unsigned tick) {
  Color track = {29, 35, 49, 255};
  Color border = {77, 88, 110, 255};
  int inset = height < 14 ? 2 : 3;
  int inner = width - inset * 2;
  int fill_width;
  value = clamp_permille(value);
  fill(renderer, x, y, width, height, track);
  outline(renderer, x, y, width, height, border);
  fill_width = inner * value / 1000;
  fill(renderer, x + inset, y + inset, fill_width, height - inset * 2, accent);
  if (active && value < 1000) {
    int remaining = inner - fill_width;
    int segment = remaining / 5;
    int travel, offset, left, right;
    Color light = {(uint8_t)(accent.r + (255 - accent.r) / 2),
                   (uint8_t)(accent.g + (255 - accent.g) / 2),
                   (uint8_t)(accent.b + (255 - accent.b) / 2), 255};
    if (segment < 8)
      segment = remaining < 8 ? remaining : 8;
    if (segment > 48)
      segment = 48;
    travel = remaining + segment;
    offset = (int)((tick / 4u) % (unsigned)(travel ? travel : 1)) - segment;
    left = x + inset + fill_width + offset;
    right = left + segment;
    if (left < x + inset + fill_width)
      left = x + inset + fill_width;
    if (right > x + inset + inner)
      right = x + inset + inner;
    fill(renderer, left, y + inset, right - left, height - inset * 2, light);
  }
}

static void draw_screen(SDL_Renderer *renderer, int width, int height,
                        const ProgressState *progress, const char *title,
                        const char *version, unsigned tick) {
  Color background_top = {11, 16, 29, 255};
  Color background_bottom = {24, 31, 48, 255};
  Color panel = {19, 25, 40, 255};
  Color panel_border = {55, 66, 90, 255};
  Color primary = {232, 238, 247, 255};
  Color secondary = {145, 158, 181, 255};
  Color accent = {55, 210, 154, 255};
  Color blue = {69, 156, 235, 255};
  Color error = {232, 83, 91, 255};
  Color success = {87, 218, 137, 255};
  Color dim = {45, 55, 74, 255};
  int stripe, margin, panel_x, panel_y, panel_w, panel_h;
  int scale_base, title_scale, version_scale, small_scale, message_scale;
  int content_x, content_w, title_y, step_y, phase_y, message_y;
  int bar_y, bar_height, overall_y, max_chars, line_count, line;
  int header_gap, title_limit, version_limit, version_width;
  int segment_gap, segment_width, index;
  char shown_title[128], version_text[80], phase[64];
  char message_lines[2][128], shown_detail[256];
  Color state_accent;

  for (stripe = 0; stripe < 32; stripe++) {
    int y0 = height * stripe / 32;
    int y1 = height * (stripe + 1) / 32;
    Color color = {
        (uint8_t)(background_top.r +
                  (background_bottom.r - background_top.r) * stripe / 31),
        (uint8_t)(background_top.g +
                  (background_bottom.g - background_top.g) * stripe / 31),
        (uint8_t)(background_top.b +
                  (background_bottom.b - background_top.b) * stripe / 31),
        255};
    fill(renderer, 0, y0, width, y1 - y0, color);
  }
  fill(renderer, 0, 0, width, height / 90 + 2,
       progress->state == 2 ? error : accent);

  margin = width / 18;
  if (margin < 10)
    margin = 10;
  if (margin > 56)
    margin = 56;
  panel_x = margin;
  panel_y = height / 14;
  panel_w = width - margin * 2;
  panel_h = height - panel_y - height / 16;
  fill(renderer, panel_x + 4, panel_y + 5, panel_w, panel_h,
       (Color){6, 9, 17, 120});
  fill(renderer, panel_x, panel_y, panel_w, panel_h, panel);
  outline(renderer, panel_x, panel_y, panel_w, panel_h, panel_border);

  content_x = panel_x + panel_w / 16;
  content_w = panel_w - panel_w / 8;
  scale_base = height < 300 ? 1 : (height < 700 ? 2 : 3);
  title_scale = scale_base + (height >= 440 ? 1 : 0);
  small_scale = scale_base;
  message_scale = scale_base;

  version_text[0] = 0;
  version_scale = small_scale;
  version_width = 0;
  if (version && *version) {
    snprintf(version_text, sizeof(version_text), "RECIPE %s", version);
    version_limit = content_w * 38 / 100;
    while (version_scale > 1 &&
           text_width(version_text, version_scale) > version_limit)
      version_scale--;
    if (text_width(version_text, version_scale) > version_limit)
      ellipsize(version_text, sizeof(version_text),
                version_limit / (6 * version_scale));
    version_width = text_width(version_text, version_scale);
  }

  header_gap = version_width ? small_scale * 8 : 0;
  title_limit = content_w - version_width - header_gap;
  if (title_limit < content_w / 3)
    title_limit = content_w / 3;
  snprintf(shown_title, sizeof(shown_title), "%s", title);
  while (title_scale > 1 &&
         text_width(shown_title, title_scale) > title_limit)
    title_scale--;
  if (text_width(shown_title, title_scale) > title_limit)
    ellipsize(shown_title, sizeof(shown_title),
              (title_limit + title_scale) / (6 * title_scale));

  title_y = panel_y + panel_h / 12;
  draw_text(renderer, content_x, title_y, title_scale, shown_title, primary);
  if (version_text[0]) {
    draw_text(renderer,
              content_x + content_w - version_width,
              title_y + (title_scale - version_scale) * 3, version_scale,
              version_text, secondary);
  }

  step_y = title_y + title_scale * 7 + panel_h / 13;
  segment_gap = content_w / 160;
  if (segment_gap < 3)
    segment_gap = 3;
  segment_width = (content_w - segment_gap * 8) / 9;
  for (index = 0; index < 9; index++) {
    Color color = index < progress->phase
                      ? accent
                      : index == progress->phase
                            ? (progress->state == 2 ? error : blue)
                            : dim;
    fill(renderer, content_x + index * (segment_width + segment_gap), step_y,
         segment_width, height < 300 ? 4 : 7, color);
  }

  snprintf(phase, sizeof(phase), "%s",
           progress->state == 2 ? "SETUP FAILED"
                                : phase_labels[progress->phase]);
  phase_y = step_y + (height < 300 ? 13 : 22);
  state_accent =
      progress->state == 2 ? error : (progress->state == 3 ? success : blue);
  draw_text(renderer, content_x, phase_y, small_scale, phase, state_accent);

  max_chars = content_w / (6 * message_scale);
  line_count =
      wrap_text(progress->message, message_lines, height < 260 ? 1 : 2, max_chars);
  message_y = phase_y + small_scale * 7 + panel_h / 18;
  for (line = 0; line < line_count; line++)
    draw_text(renderer, content_x, message_y + line * message_scale * 10,
              message_scale, message_lines[line], primary);

  bar_height = height < 300 ? 13 : (height < 700 ? 20 : 28);
  bar_y = panel_y + panel_h * 65 / 100;
  draw_bar(renderer, content_x, bar_y, content_w, bar_height,
           progress->phase_progress, state_accent, progress->active, tick);

  if (progress->detail[0]) {
    snprintf(shown_detail, sizeof(shown_detail), "%s", progress->detail);
    ellipsize(shown_detail, sizeof(shown_detail),
              content_w / (6 * small_scale));
    draw_text(renderer, content_x, bar_y + bar_height + small_scale * 5,
              small_scale, shown_detail, secondary);
  }

  overall_y = panel_y + panel_h * 86 / 100;
  draw_text(renderer, content_x, overall_y - small_scale * 10, small_scale,
            "OVERALL", secondary);
  draw_bar(renderer, content_x, overall_y, content_w, height < 300 ? 7 : 10,
           progress->overall, accent, 0, tick);
  {
    char percent[32];
    snprintf(percent, sizeof(percent), "%d%%", progress->overall / 10);
    draw_text(renderer, content_x + content_w - text_width(percent, small_scale),
              overall_y - small_scale * 10, small_scale, percent, secondary);
  }
  draw_text_centered(renderer, width / 2,
                     panel_y + panel_h - small_scale * 12, small_scale,
                     "NXEXTRACT UNIVERSAL DATA SETUP", secondary);
}

typedef struct FramebufferRenderer {
  int descriptor;
  void *mapping;
  size_t mapping_length;
  unsigned char *shadow;
  size_t shadow_length;
  uint64_t pixel_offset;
  size_t row_bytes;
  unsigned pitch;
  int writeback;
  SDL_Surface *surface;
  SDL_Renderer *renderer;
  int width;
  int height;
} FramebufferRenderer;

static void framebuffer_renderer_reset(FramebufferRenderer *framebuffer) {
  memset(framebuffer, 0, sizeof(*framebuffer));
  framebuffer->descriptor = -1;
  framebuffer->mapping = MAP_FAILED;
}

static void framebuffer_renderer_close(FramebufferRenderer *framebuffer) {
  if (!framebuffer)
    return;
  if (framebuffer->renderer)
    pSDL_DestroyRenderer(framebuffer->renderer);
  if (framebuffer->surface)
    pSDL_FreeSurface(framebuffer->surface);
  if (framebuffer->mapping != MAP_FAILED)
    munmap(framebuffer->mapping, framebuffer->mapping_length);
  free(framebuffer->shadow);
  if (framebuffer->descriptor >= 0)
    close(framebuffer->descriptor);
  framebuffer_renderer_reset(framebuffer);
}

static int framebuffer_channel_mask(const struct fb_bitfield *field,
                                    unsigned bits_per_pixel, int required,
                                    uint32_t *mask) {
  uint32_t value;
  if (!field || !mask)
    return 0;
  if (!field->length) {
    *mask = 0;
    return !required;
  }
  if (field->msb_right || field->offset >= 32u || field->length > 32u ||
      field->offset + field->length > bits_per_pixel ||
      field->offset + field->length > 32u)
    return 0;
  value = field->length == 32u ? UINT32_MAX
                               : ((UINT32_C(1) << field->length) - 1u);
  *mask = value << field->offset;
  return 1;
}

static int open_framebuffer_device(const char *path) {
  struct stat before, after;
  int descriptor;
  if (!path || path[0] != '/' || lstat(path, &before) != 0 ||
      !S_ISCHR(before.st_mode) || S_ISLNK(before.st_mode))
    return -1;
  descriptor = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    return -1;
  if (fstat(descriptor, &after) != 0 || !S_ISCHR(after.st_mode) ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      before.st_rdev != after.st_rdev) {
    close(descriptor);
    return -1;
  }
  return descriptor;
}

static int framebuffer_write_at(int descriptor, const unsigned char *buffer,
                                size_t length, uint64_t offset) {
  size_t done = 0;
  if (offset > (uint64_t)INT64_MAX ||
      lseek(descriptor, (off_t)offset, SEEK_SET) < 0)
    return 0;
  while (done < length) {
    ssize_t written = write(descriptor, buffer + done, length - done);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }
    if (!written)
      return 0;
    done += (size_t)written;
  }
  return 1;
}

static int framebuffer_present(FramebufferRenderer *framebuffer) {
  int row;
  if (!framebuffer || !framebuffer->renderer)
    return 0;
  pSDL_RenderPresent(framebuffer->renderer);
  if (!framebuffer->writeback) {
    __sync_synchronize();
    return 1;
  }
  if (framebuffer->row_bytes == framebuffer->pitch) {
    if (!framebuffer_write_at(framebuffer->descriptor, framebuffer->shadow,
                              framebuffer->shadow_length,
                              framebuffer->pixel_offset)) {
      fprintf(stderr, "nxextract-ui: fbdev writeback failed: %s\n",
              strerror(errno));
      return 0;
    }
    (void)fsync(framebuffer->descriptor);
    __sync_synchronize();
    return 1;
  }
  for (row = 0; row < framebuffer->height; row++) {
    uint64_t offset = framebuffer->pixel_offset +
                      (uint64_t)(unsigned)row * framebuffer->pitch;
    const unsigned char *source =
        framebuffer->shadow + (size_t)row * framebuffer->pitch;
    if (!framebuffer_write_at(framebuffer->descriptor, source,
                              framebuffer->row_bytes, offset)) {
      fprintf(stderr, "nxextract-ui: fbdev writeback failed: %s\n",
              strerror(errno));
      return 0;
    }
  }
  (void)fsync(framebuffer->descriptor);
  __sync_synchronize();
  return 1;
}

static int create_framebuffer_renderer(FramebufferRenderer *framebuffer) {
  static const char framebuffer_path[] = "/dev/fb0";
  struct fb_fix_screeninfo fixed;
  struct fb_var_screeninfo variable;
  uint32_t red_mask, green_mask, blue_mask, alpha_mask;
  uint64_t pixel_offset, visible_end;
  unsigned bytes_per_pixel;
  void *pixels;
  int output_width, output_height;

  if (!framebuffer)
    return 0;
  framebuffer_renderer_reset(framebuffer);
  framebuffer->descriptor = open_framebuffer_device(framebuffer_path);
  if (framebuffer->descriptor < 0) {
    fprintf(stderr, "nxextract-ui: graphical fbdev unavailable: %s\n",
            strerror(errno));
    return 0;
  }
  memset(&fixed, 0, sizeof(fixed));
  memset(&variable, 0, sizeof(variable));
  if (ioctl(framebuffer->descriptor, FBIOGET_FSCREENINFO, &fixed) != 0 ||
      ioctl(framebuffer->descriptor, FBIOGET_VSCREENINFO, &variable) != 0) {
    fprintf(stderr, "nxextract-ui: cannot inspect /dev/fb0: %s\n",
            strerror(errno));
    goto failed;
  }
  if (fixed.type != FB_TYPE_PACKED_PIXELS ||
      (fixed.visual != FB_VISUAL_TRUECOLOR &&
       fixed.visual != FB_VISUAL_DIRECTCOLOR)) {
    fprintf(stderr,
            "nxextract-ui: unsupported fbdev type=%u visual=%u\n",
            fixed.type, fixed.visual);
    goto failed;
  }
  if ((variable.bits_per_pixel != 16u && variable.bits_per_pixel != 24u &&
       variable.bits_per_pixel != 32u) ||
      variable.xres < 160u || variable.yres < 120u ||
      variable.xres > (unsigned)INT_MAX ||
      variable.yres > (unsigned)INT_MAX || fixed.line_length == 0u ||
      fixed.line_length > (unsigned)INT_MAX || fixed.smem_len == 0u) {
    fprintf(stderr,
            "nxextract-ui: unsupported fbdev geometry=%ux%u bpp=%u pitch=%u\n",
            variable.xres, variable.yres, variable.bits_per_pixel,
            fixed.line_length);
    goto failed;
  }
  if (variable.xoffset > variable.xres_virtual ||
      variable.yoffset > variable.yres_virtual ||
      variable.xres > variable.xres_virtual - variable.xoffset ||
      variable.yres > variable.yres_virtual - variable.yoffset) {
    fprintf(stderr, "nxextract-ui: invalid fbdev visible viewport\n");
    goto failed;
  }
  bytes_per_pixel = variable.bits_per_pixel / 8u;
  if ((uint64_t)(variable.xoffset + variable.xres) * bytes_per_pixel >
      fixed.line_length) {
    fprintf(stderr, "nxextract-ui: fbdev scanline is shorter than viewport\n");
    goto failed;
  }
  pixel_offset = (uint64_t)variable.yoffset * fixed.line_length +
                 (uint64_t)variable.xoffset * bytes_per_pixel;
  visible_end = pixel_offset +
                (uint64_t)(variable.yres - 1u) * fixed.line_length +
                (uint64_t)variable.xres * bytes_per_pixel;
  if (visible_end > fixed.smem_len) {
    fprintf(stderr, "nxextract-ui: fbdev mapping does not cover viewport\n");
    goto failed;
  }
  if (!framebuffer_channel_mask(&variable.red, variable.bits_per_pixel, 1,
                                &red_mask) ||
      !framebuffer_channel_mask(&variable.green, variable.bits_per_pixel, 1,
                                &green_mask) ||
      !framebuffer_channel_mask(&variable.blue, variable.bits_per_pixel, 1,
                                &blue_mask) ||
      !framebuffer_channel_mask(&variable.transp, variable.bits_per_pixel, 0,
                                &alpha_mask) ||
      (red_mask & green_mask) || (red_mask & blue_mask) ||
      (green_mask & blue_mask) ||
      (alpha_mask & (red_mask | green_mask | blue_mask))) {
    fprintf(stderr, "nxextract-ui: unsupported fbdev channel layout\n");
    goto failed;
  }

  framebuffer->mapping_length = (size_t)fixed.smem_len;
  framebuffer->pixel_offset = pixel_offset;
  framebuffer->row_bytes = (size_t)variable.xres * bytes_per_pixel;
  framebuffer->pitch = fixed.line_length;
  framebuffer->mapping = mmap(NULL, framebuffer->mapping_length,
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              framebuffer->descriptor, 0);
  if (framebuffer->mapping == MAP_FAILED) {
    framebuffer->shadow_length =
        (size_t)(variable.yres - 1u) * fixed.line_length +
        framebuffer->row_bytes;
    framebuffer->shadow = calloc(1u, framebuffer->shadow_length);
    if (!framebuffer->shadow) {
      fprintf(stderr, "nxextract-ui: cannot allocate fbdev writeback: %s\n",
              strerror(errno));
      goto failed;
    }
    framebuffer->writeback = 1;
    pixels = framebuffer->shadow;
    fprintf(stderr,
            "nxextract-ui: fbdev mmap unavailable (%s); using graphical "
            "writeback\n",
            strerror(errno));
  } else {
    pixels = (unsigned char *)framebuffer->mapping + (size_t)pixel_offset;
  }
  framebuffer->surface = pSDL_CreateRGBSurfaceFrom(
      pixels, (int)variable.xres, (int)variable.yres,
      (int)variable.bits_per_pixel, (int)fixed.line_length, red_mask,
      green_mask, blue_mask, alpha_mask);
  if (!framebuffer->surface) {
    fprintf(stderr, "nxextract-ui: cannot create fbdev SDL surface: %s\n",
            pSDL_GetError());
    goto failed;
  }
  framebuffer->renderer =
      pSDL_CreateSoftwareRenderer(framebuffer->surface);
  if (!framebuffer->renderer) {
    fprintf(stderr, "nxextract-ui: cannot create fbdev software renderer: %s\n",
            pSDL_GetError());
    goto failed;
  }
  output_width = output_height = 0;
  if (pSDL_GetRendererOutputSize(framebuffer->renderer, &output_width,
                                 &output_height) != 0 ||
      output_width != (int)variable.xres ||
      output_height != (int)variable.yres) {
    fprintf(stderr, "nxextract-ui: fbdev renderer size mismatch\n");
    goto failed;
  }
  framebuffer->width = output_width;
  framebuffer->height = output_height;
  fprintf(stderr,
          "nxextract-ui: graphical fbdev ready %dx%d bpp=%u pitch=%u\n",
          framebuffer->width, framebuffer->height, variable.bits_per_pixel,
          fixed.line_length);
  return 1;

failed:
  framebuffer_renderer_close(framebuffer);
  return 0;
}

static int write_all(int descriptor, const void *buffer, size_t length) {
  const unsigned char *cursor = (const unsigned char *)buffer;
  while (length) {
    ssize_t written = write(descriptor, cursor, length);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }
    if (!written)
      return 0;
    cursor += (size_t)written;
    length -= (size_t)written;
  }
  return 1;
}

static int capture_dimensions(const char *value, int *width, int *height) {
  if (!value || !*value || !strcmp(value, "640x480")) {
    *width = 640;
    *height = 480;
    return 1;
  }
  if (!strcmp(value, "1280x720")) {
    *width = 1280;
    *height = 720;
    return 1;
  }
  return 0;
}

static int run_test_capture(const char *output_path, const char *size,
                            const char *progress_path, const char *title,
                            const char *version, ProgressState *progress) {
  static const uint32_t red_mask = UINT32_C(0x00ff0000);
  static const uint32_t green_mask = UINT32_C(0x0000ff00);
  static const uint32_t blue_mask = UINT32_C(0x000000ff);
  static const uint32_t alpha_mask = UINT32_C(0xff000000);
  SDL_Surface *surface = NULL;
  SDL_Renderer *renderer = NULL;
  uint32_t *pixels = NULL;
  unsigned char *row = NULL;
  size_t pixel_count, row_length;
  int descriptor = -1, width, height, x, y, result = 1;
  char header[64];
  int header_length;

  if (!output_path || output_path[0] != '/' ||
      !capture_dimensions(size, &width, &height)) {
    fprintf(stderr,
            "nxextract-ui: test capture requires an absolute output and "
            "size 640x480 or 1280x720\n");
    return 2;
  }
  pixel_count = (size_t)width * (size_t)height;
  row_length = (size_t)width * 3u;
  pixels = calloc(pixel_count, sizeof(*pixels));
  row = malloc(row_length);
  if (!pixels || !row) {
    fprintf(stderr, "nxextract-ui: test capture allocation failed\n");
    result = 1;
    goto out;
  }
  surface = pSDL_CreateRGBSurfaceFrom(
      pixels, width, height, 32, width * (int)sizeof(*pixels), red_mask,
      green_mask, blue_mask, alpha_mask);
  if (!surface) {
    fprintf(stderr, "nxextract-ui: test capture surface failed: %s\n",
            pSDL_GetError());
    result = 1;
    goto out;
  }
  renderer = pSDL_CreateSoftwareRenderer(surface);
  if (!renderer) {
    fprintf(stderr, "nxextract-ui: test capture renderer failed: %s\n",
            pSDL_GetError());
    result = 1;
    goto out;
  }
  (void)read_progress(progress_path, progress);
  draw_screen(renderer, width, height, progress, title, version, 0u);
  pSDL_RenderPresent(renderer);

  descriptor = open(output_path,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
  if (descriptor < 0) {
    fprintf(stderr, "nxextract-ui: cannot create test capture: %s\n",
            strerror(errno));
    result = 1;
    goto out;
  }
  header_length = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width,
                           height);
  if (header_length <= 0 || (size_t)header_length >= sizeof(header) ||
      !write_all(descriptor, header, (size_t)header_length)) {
    result = 1;
    goto out;
  }
  for (y = 0; y < height; y++) {
    for (x = 0; x < width; x++) {
      uint32_t pixel = pixels[(size_t)y * (size_t)width + (size_t)x];
      row[(size_t)x * 3u] = (unsigned char)((pixel & red_mask) >> 16);
      row[(size_t)x * 3u + 1u] =
          (unsigned char)((pixel & green_mask) >> 8);
      row[(size_t)x * 3u + 2u] = (unsigned char)(pixel & blue_mask);
    }
    if (!write_all(descriptor, row, row_length)) {
      result = 1;
      goto out;
    }
  }
  if (fsync(descriptor) != 0) {
    result = 1;
    goto out;
  }
  result = 0;
  fprintf(stderr, "nxextract-ui: deterministic test capture=%s size=%dx%d\n",
          output_path, width, height);

out:
  if (descriptor >= 0 && close(descriptor) != 0)
    result = 1;
  if (renderer)
    pSDL_DestroyRenderer(renderer);
  if (surface)
    pSDL_FreeSurface(surface);
  free(row);
  free(pixels);
  return result;
}

enum {
  MAX_VIDEO_CANDIDATES = 24,
  MAX_VIDEO_DRIVER_NAME = 64,
  FALLBACK_RETRIES_PER_DRIVER = 6
};

typedef struct VideoCandidates {
  char names[MAX_VIDEO_CANDIDATES][MAX_VIDEO_DRIVER_NAME];
  int count;
  int next;
  int exhausted;
} VideoCandidates;

static int driver_is_invisible(const char *name) {
  return name &&
         (!strcasecmp(name, "dummy") || !strcasecmp(name, "offscreen") ||
          !strcasecmp(name, "evdev"));
}

static int driver_is_plausible(const char *name) {
  if (!name || !*name || driver_is_invisible(name))
    return 0;
  if (!strcasecmp(name, "wayland"))
    return wayland_socket_exists();
  if (!strcasecmp(name, "x11"))
    return getenv("DISPLAY") && *getenv("DISPLAY");
  if (!strcasecmp(name, "kmsdrm"))
    return path_exists("/dev/dri/card0");
  if (!strcasecmp(name, "mali") || !strcasecmp(name, "fbcon") ||
      !strcasecmp(name, "directfb"))
    return path_exists("/dev/fb0");
  return 1;
}

static void collect_video_candidates(VideoCandidates *candidates) {
  int count = pSDL_GetNumVideoDrivers();
  int index;
  memset(candidates, 0, sizeof(*candidates));
  for (index = 0; index < count &&
                  candidates->count < MAX_VIDEO_CANDIDATES;
       index++) {
    const char *name = pSDL_GetVideoDriver(index);
    int duplicate = 0;
    int previous;
    if (!driver_is_plausible(name))
      continue;
    for (previous = 0; previous < candidates->count; previous++) {
      if (!strcasecmp(candidates->names[previous], name)) {
        duplicate = 1;
        break;
      }
    }
    if (duplicate)
      continue;
    snprintf(candidates->names[candidates->count],
             sizeof(candidates->names[candidates->count]), "%s", name);
    candidates->count++;
  }
}

static int select_next_video_candidate(VideoCandidates *candidates,
                                       int *fallback_attempts) {
  while (candidates->next < candidates->count) {
    const char *name = candidates->names[candidates->next++];
    if (setenv("SDL_VIDEODRIVER", name, 1) != 0)
      continue;
    *fallback_attempts = 0;
    fprintf(stderr,
            "nxextract-ui: automatic SDL selection was invisible; "
            "trying advertised backend=%s\n",
            name);
    return 1;
  }
  candidates->exhausted = 1;
  unsetenv("SDL_VIDEODRIVER");
  return 0;
}

static void note_video_failure(int inherited_mode, int *fallback_mode,
                               int *fallback_attempts,
                               VideoCandidates *candidates) {
  if (inherited_mode || candidates->exhausted)
    return;
  if (!*fallback_mode) {
    *fallback_mode =
        select_next_video_candidate(candidates, fallback_attempts);
    return;
  }
  (*fallback_attempts)++;
  if (*fallback_attempts >= FALLBACK_RETRIES_PER_DRIVER)
    *fallback_mode =
        select_next_video_candidate(candidates, fallback_attempts);
}

static int try_renderer_once(const char *title, int attempt,
                             SDL_Window **window_out,
                             SDL_Renderer **renderer_out) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  const char *driver;

  if (pSDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "nxextract-ui: SDL_Init try %d: %s\n", attempt,
            pSDL_GetError());
    pSDL_Quit();
    return 0;
  }
  driver = pSDL_GetCurrentVideoDriver();
  if (driver_is_invisible(driver)) {
    fprintf(stderr, "nxextract-ui: invisible SDL driver rejected: %s\n",
            driver);
    pSDL_Quit();
    return 0;
  }
  window = pSDL_CreateWindow(title, 0, 0, 640, 480,
                             SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (window)
    renderer = pSDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer && window)
    renderer = pSDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) {
    fprintf(stderr, "nxextract-ui: window/renderer try %d: %s\n", attempt,
            pSDL_GetError());
    if (window)
      pSDL_DestroyWindow(window);
    pSDL_Quit();
    return 0;
  }
  *window_out = window;
  *renderer_out = renderer;
  return 1;
}

/* Reuse the provider recovery physically validated by NXSplash 0.1.2. Some
 * KMSDRM firmwares expose the matching GPU provider through the unversioned
 * names while their versioned EGL soname resolves to an unusable dispatcher.
 * Retry only after normal discovery fails and never replace an explicit
 * firmware/user provider choice. */
static int enable_portable_provider_retry(void) {
  if (getenv("SDL_VIDEO_EGL_DRIVER") || getenv("SDL_VIDEO_GL_DRIVER"))
    return 0;
  if (setenv("SDL_VIDEO_EGL_DRIVER", "libEGL.so", 1) != 0)
    return 0;
  if (setenv("SDL_VIDEO_GL_DRIVER", "libGLESv2.so", 1) != 0) {
    unsetenv("SDL_VIDEO_EGL_DRIVER");
    return 0;
  }
  fprintf(stderr,
          "nxextract-ui: retrying portable EGL/GLES provider names\n");
  return 1;
}

static void drop_portable_provider_retry(void) {
  unsetenv("SDL_VIDEO_EGL_DRIVER");
  unsetenv("SDL_VIDEO_GL_DRIVER");
}

static int create_renderer(const char *stop_path, const char *title,
                           SDL_Window **window_out,
                           SDL_Renderer **renderer_out) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  VideoCandidates candidates;
  int inherited_mode =
      getenv("SDL_VIDEODRIVER") && *getenv("SDL_VIDEODRIVER");
  int fallback_mode = 0;
  int fallback_attempts = 0;
  int portable_provider_attempted = 0;
  int attempt;

  collect_video_candidates(&candidates);
  for (attempt = 0; attempt < 30 && !renderer; attempt++) {
    if (stop_requested(stop_path))
      return 0;
    if (attempt == 6 && inherited_mode) {
      fprintf(stderr,
              "nxextract-ui: inherited SDL_VIDEODRIVER=%s failed; retrying auto\n",
              getenv("SDL_VIDEODRIVER"));
      unsetenv("SDL_VIDEODRIVER");
      inherited_mode = 0;
      fallback_mode = 0;
    }
    if (try_renderer_once(title, attempt, &window, &renderer))
      break;

    if (!portable_provider_attempted) {
      portable_provider_attempted = 1;
      if (enable_portable_provider_retry()) {
        if (try_renderer_once(title, attempt, &window, &renderer)) {
          fprintf(stderr,
                  "nxextract-ui: recovered with portable EGL/GLES provider "
                  "names\n");
          break;
        }
        drop_portable_provider_retry();
      }
    }
    note_video_failure(inherited_mode, &fallback_mode, &fallback_attempts,
                       &candidates);
    usleep(500000);
  }
  if (!renderer)
    return 0;
  *window_out = window;
  *renderer_out = renderer;
  return 1;
}

int main(int argc, char **argv) {
  const char *progress_path, *stop_path, *ready_path, *title, *version;
  const char *capture_path, *capture_size, *allow_tty;
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  FramebufferRenderer framebuffer;
  char console_path[96] = "unknown";
  int console, result, sdl_loaded;
  ProgressState progress = {
      .state = 1,
      .active = 1,
      .phase = 0,
      .overall = 0,
      .phase_progress = 0,
      .done_bytes = 0,
      .total_bytes = 0,
      .message = "PREPARING GAME DATA",
      .detail = "",
  };
  unsigned tick = 0;

  framebuffer_renderer_reset(&framebuffer);

  if (argc != 6) {
    fprintf(stderr,
            "usage: nxextract-ui PROGRESS_FILE STOP_FILE READY_FILE TITLE VERSION\n");
    return 2;
  }
  progress_path = argv[1];
  stop_path = argv[2];
  ready_path = argv[3];
  title = argv[4];
  version = argv[5];
  signal(SIGPIPE, SIG_IGN);
  session_stop_fd = parse_session_fd_token(stop_path);
  if (session_stop_fd == -2) {
    fprintf(stderr, "nxextract-ui: invalid session stop descriptor\n");
    return 2;
  }
  session_ready_fd = parse_session_fd_token(ready_path);
  if (session_ready_fd == -2) {
    fprintf(stderr, "nxextract-ui: invalid session ready descriptor\n");
    return 2;
  }
  if ((session_stop_fd >= 0) != (session_ready_fd >= 0)) {
    fprintf(stderr, "nxextract-ui: session channel descriptors must arrive "
                    "together\n");
    return 2;
  }
  configure_session_runtime();

  capture_path = getenv("NXEXTRACT_TEST_CAPTURE_PPM");
  capture_size = getenv("NXEXTRACT_TEST_CAPTURE_SIZE");
  sdl_loaded = load_sdl();
  if (capture_path && *capture_path) {
    if (!sdl_loaded)
      return 1;
    result = run_test_capture(capture_path, capture_size, progress_path, title,
                              version, &progress);
    if (pSDL_Quit)
      pSDL_Quit();
    if (sdl_handle)
      dlclose(sdl_handle);
    return result;
  }

  if (sdl_loaded && create_renderer(stop_path, title, &window, &renderer)) {
    const char *driver = pSDL_GetCurrentVideoDriver();
    int width = 640, height = 480;
    SDL_Event event;
    while (pSDL_PollEvent(&event))
      ;
    (void)read_progress(progress_path, &progress);
    if (pSDL_GetRendererOutputSize(renderer, &width, &height) != 0 ||
        width < 160 || height < 120) {
      width = 640;
      height = 480;
    }
    draw_screen(renderer, width, height, &progress, title, version, tick++);
    pSDL_RenderPresent(renderer);
    fprintf(stderr, "nxextract-ui: renderer=sdl driver=%s\n",
            driver ? driver : "unknown");
    if (!publish_ready(ready_path, "sdl"))
      goto failed;

    while (!stop_requested(stop_path)) {
      while (pSDL_PollEvent(&event))
        ;
      (void)read_progress(progress_path, &progress);
      draw_screen(renderer, width, height, &progress, title, version, tick++);
      pSDL_RenderPresent(renderer);
      pSDL_Delay(50);
    }
    goto out;
  }

  if (sdl_loaded && create_framebuffer_renderer(&framebuffer)) {
    (void)read_progress(progress_path, &progress);
    draw_screen(framebuffer.renderer, framebuffer.width, framebuffer.height,
                &progress, title, version, tick++);
    if (!framebuffer_present(&framebuffer))
      goto failed;
    fprintf(stderr, "nxextract-ui: renderer=fbdev path=/dev/fb0\n");
    if (!publish_ready(ready_path, "fbdev"))
      goto failed;

    while (!stop_requested(stop_path)) {
      (void)read_progress(progress_path, &progress);
      draw_screen(framebuffer.renderer, framebuffer.width, framebuffer.height,
                  &progress, title, version, tick++);
      if (!framebuffer_present(&framebuffer))
        goto failed;
      pSDL_Delay(50);
    }
    goto out;
  }

  allow_tty = getenv("NXEXTRACT_ALLOW_TTY_UI");
  if (allow_tty && !strcmp(allow_tty, "1")) {
    console = open_console(console_path, sizeof(console_path));
    if (console >= 0) {
      fprintf(stderr,
              "nxextract-ui: diagnostic renderer=tty path=%s; graphical "
              "readiness intentionally withheld\n",
              console_path);
      run_console_ui(console, progress_path, stop_path, title, version,
                     &progress);
      close(console);
    }
  }

  fprintf(stderr,
          "nxextract-ui: no graphical SDL or fbdev renderer; refusing "
          "headless extraction\n");
  goto failed;

failed:
  framebuffer_renderer_close(&framebuffer);
  if (renderer)
    pSDL_DestroyRenderer(renderer);
  if (window)
    pSDL_DestroyWindow(window);
  if (pSDL_Quit)
    pSDL_Quit();
  if (sdl_handle)
    dlclose(sdl_handle);
  return 1;

out:
  framebuffer_renderer_close(&framebuffer);
  if (renderer)
    pSDL_DestroyRenderer(renderer);
  if (window)
    pSDL_DestroyWindow(window);
  pSDL_Quit();
  if (sdl_handle)
    dlclose(sdl_handle);
  return 0;
}
