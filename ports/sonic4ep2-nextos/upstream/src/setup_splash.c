/* Sonic 4 EP2 first-run setup UI. This module never loads libfox. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SONIC_PORT_VERSION
#define SONIC_PORT_VERSION "V6"
#endif

static int sp_file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

static const char **glyph5x7(int c) {
  static const char *sp[] = {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
  static const char *q[] = {"01110", "10001", "00001", "00010", "00100", "00000", "00100"};
  static const char *dot[] = {"00000", "00000", "00000", "00000", "00000", "01100", "01100"};
  static const char *dash[] = {"00000", "00000", "00000", "11110", "00000", "00000", "00000"};
  static const char *slash[] = {"00001", "00010", "00010", "00100", "01000", "01000", "10000"};
  static const char *colon[] = {"00000", "01100", "01100", "00000", "01100", "01100", "00000"};
  static const char *pct[] = {"11001", "11010", "00100", "01000", "10110", "00110", "00000"};
  static const char *n0[] = {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
  static const char *n1[] = {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
  static const char *n2[] = {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
  static const char *n3[] = {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
  static const char *n4[] = {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
  static const char *n5[] = {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
  static const char *n6[] = {"00110", "01000", "10000", "11110", "10001", "10001", "01110"};
  static const char *n7[] = {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
  static const char *n8[] = {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
  static const char *n9[] = {"01110", "10001", "10001", "01111", "00001", "00010", "01100"};
  static const char *A[] = {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
  static const char *B[] = {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
  static const char *C[] = {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
  static const char *D[] = {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
  static const char *E[] = {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
  static const char *F[] = {"11111", "10000", "10000", "11110", "10000", "10000", "10000"};
  static const char *G[] = {"01110", "10001", "10000", "10111", "10001", "10001", "01110"};
  static const char *H[] = {"10001", "10001", "10001", "11111", "10001", "10001", "10001"};
  static const char *I[] = {"01110", "00100", "00100", "00100", "00100", "00100", "01110"};
  static const char *J[] = {"00001", "00001", "00001", "00001", "10001", "10001", "01110"};
  static const char *K[] = {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
  static const char *L[] = {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
  static const char *M[] = {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
  static const char *N[] = {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
  static const char *O[] = {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
  static const char *P[] = {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
  static const char *Q[] = {"01110", "10001", "10001", "10001", "10101", "10010", "01101"};
  static const char *R[] = {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
  static const char *S[] = {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
  static const char *T[] = {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
  static const char *U[] = {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
  static const char *V[] = {"10001", "10001", "10001", "10001", "10001", "01010", "00100"};
  static const char *W[] = {"10001", "10001", "10001", "10101", "10101", "10101", "01010"};
  static const char *X[] = {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
  static const char *Y[] = {"10001", "10001", "01010", "00100", "00100", "00100", "00100"};
  static const char *Z[] = {"11111", "00001", "00010", "00100", "01000", "10000", "11111"};
  if (c >= 'a' && c <= 'z')
    c = toupper(c);
  switch (c) {
  case '0': return n0; case '1': return n1; case '2': return n2; case '3': return n3; case '4': return n4;
  case '5': return n5; case '6': return n6; case '7': return n7; case '8': return n8; case '9': return n9;
  case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E;
  case 'F': return F; case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J;
  case 'K': return K; case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
  case 'P': return P; case 'Q': return Q; case 'R': return R; case 'S': return S; case 'T': return T;
  case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X; case 'Y': return Y; case 'Z': return Z;
  case '.': return dot; case '-': return dash; case '/': return slash; case ':': return colon; case '%': return pct;
  case ' ': return sp; default: return q;
  }
}

static void draw_text(SDL_Renderer *r, int x, int y, int scale, const char *s) {
  SDL_Rect px = {0, 0, scale, scale};
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
    const char **g = glyph5x7(*p);
    for (int yy = 0; yy < 7; yy++) {
      for (int xx = 0; xx < 5; xx++) {
        if (g[yy][xx] == '1') {
          px.x = x + xx * scale;
          px.y = y + yy * scale;
          SDL_RenderFillRect(r, &px);
        }
      }
    }
    x += 6 * scale;
  }
}

static int text_width(const char *s, int scale) {
  size_t length = s ? strlen(s) : 0;
  return length ? (int)(length * 6 * (size_t)scale) - scale : 0;
}

static void draw_text_centered(SDL_Renderer *r, int center_x, int y, int scale,
                               const char *s) {
  draw_text(r, center_x - text_width(s, scale) / 2, y, scale, s);
}

static void ellipsize_text(char *text, size_t capacity, int max_chars) {
  size_t length;
  if (!text || capacity == 0)
    return;
  length = strlen(text);
  if (max_chars < 1) {
    text[0] = 0;
    return;
  }
  if ((size_t)max_chars >= length)
    return;
  if ((size_t)max_chars >= capacity)
    max_chars = (int)capacity - 1;
  if (max_chars <= 3) {
    int i;
    for (i = 0; i < max_chars; i++)
      text[i] = '.';
    text[max_chars] = 0;
    return;
  }
  text[max_chars - 3] = '.';
  text[max_chars - 2] = '.';
  text[max_chars - 1] = '.';
  text[max_chars] = 0;
}

static int wrap_text(const char *input, char lines[][96], int max_lines,
                     int max_chars) {
  const char *cursor = input ? input : "";
  int count = 0;
  if (max_chars < 8)
    max_chars = 8;
  if (max_chars > 95)
    max_chars = 95;

  while (*cursor && count < max_lines) {
    int used = 0;
    while (*cursor == ' ' || *cursor == '\t')
      cursor++;
    if (!*cursor)
      break;
    lines[count][0] = 0;
    while (*cursor && used < max_chars) {
      const char *word = cursor;
      int word_len = 0;
      while (word[word_len] && word[word_len] != ' ' && word[word_len] != '\t')
        word_len++;
      if (used && used + 1 + word_len > max_chars)
        break;
      if (!used && word_len > max_chars)
        word_len = max_chars;
      if (used)
        lines[count][used++] = ' ';
      memcpy(lines[count] + used, word, (size_t)word_len);
      used += word_len;
      lines[count][used] = 0;
      cursor += word_len;
      while (*cursor == ' ' || *cursor == '\t')
        cursor++;
      if (word_len == max_chars)
        break;
    }
    count++;
  }
  if (*cursor && count > 0) {
    int last = count - 1;
    int used = (int)strlen(lines[last]);
    if (used > max_chars - 3)
      used = max_chars - 3;
    memcpy(lines[last] + used, "...", 4);
  }
  if (count == 0) {
    lines[0][0] = 0;
    count = 1;
  }
  return count;
}

typedef struct SetupProgress {
  int state;
  int active_done;
  int active_total;
  int phase;
  int validation_permille;
  int extraction_permille;
  int has_v2;
  char message[180];
} SetupProgress;

typedef struct ChildWatch {
  pid_t pid;
  int status;
  int reaped;
  int wait_error;
} ChildWatch;

static int clamp_permille(int value) {
  if (value < 0)
    return 0;
  if (value > 1000)
    return 1000;
  return value;
}

static const char *setup_phase_label(int phase) {
  static const char *labels[] = {
      "STARTING",          "SCANNING SOURCES", "VALIDATING PACKAGE",
      "SELECTING PAYLOAD", "EXTRACTING DATA",  "VALIDATING DATA",
      "PREPARING INSTALL", "INSTALLING DATA",  "READY",
  };
  if (phase < 0 || phase >= (int)(sizeof(labels) / sizeof(labels[0])))
    return "SETTING UP";
  return labels[phase];
}

static int setup_active_bar(const SetupProgress *progress) {
  if (!progress || progress->state != 1)
    return 0;
  switch (progress->phase) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 5:
    return 1;
  case 4:
    return 2;
  default:
    return 0;
  }
}

static int read_setup_state(const char *path, SetupProgress *progress) {
  SetupProgress next;
  char line[256];
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;

  next = *progress;
  if (!fgets(line, sizeof(line), f) ||
      sscanf(line, "%d %d %d", &next.state, &next.active_done,
             &next.active_total) != 3 ||
      next.state < 1 || next.state > 3) {
    fclose(f);
    return 0;
  }
  if (!fgets(next.message, sizeof(next.message), f)) {
    fclose(f);
    return 0;
  }
  next.message[sizeof(next.message) - 1] = 0;
  next.message[strcspn(next.message, "\r\n")] = 0;

  next.has_v2 = 0;
  if (fgets(line, sizeof(line), f)) {
    int phase = 0, validation = 0, extraction = 0;
    if (sscanf(line, "SONIC_SETUP_V2 %d %d %d", &phase, &validation,
               &extraction) == 3) {
      next.phase = phase;
      next.validation_permille = clamp_permille(validation);
      next.extraction_permille = clamp_permille(extraction);
      next.has_v2 = 1;
    }
  }
  fclose(f);

  if (!next.has_v2) {
    int permille = 0;
    if (next.active_total > 0 && next.active_done > 0) {
      int64_t scaled = (int64_t)next.active_done * 1000;
      permille = (int)(scaled / next.active_total);
    }
    next.phase = next.state == 3 ? 8 : 4;
    next.validation_permille = 0;
    next.extraction_permille = clamp_permille(permille);
  }

  *progress = next;
  return 1;
}

static void draw_setup_bar(SDL_Renderer *r, int x, int y, int width,
                           int height, int permille, int active, int state,
                           int validation_bar, unsigned activity_tick) {
  SDL_Rect border = {x, y, width, height};
  int inset, inner_w, inner_h, fill_w;
  SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
  SDL_RenderDrawRect(r, &border);

  inset = height < 14 ? 2 : 3;
  inner_w = width - inset * 2;
  inner_h = height - inset * 2;
  if (inner_w < 1 || inner_h < 1)
    return;

  permille = clamp_permille(permille);
  fill_w = (int)((int64_t)inner_w * permille / 1000);
  if (state == 2)
    SDL_SetRenderDrawColor(r, 180, 68, 68, 255);
  else if (validation_bar)
    SDL_SetRenderDrawColor(r, 74, 151, 198, 255);
  else
    SDL_SetRenderDrawColor(r, 86, 174, 118, 255);
  if (fill_w > 0) {
    SDL_Rect fill = {x + inset, y + inset, fill_w, inner_h};
    SDL_RenderFillRect(r, &fill);
  }

  if (active && state == 1 && permille < 1000) {
    int remaining_x = x + inset + fill_w;
    int remaining_w = inner_w - fill_w;
    int segment_w, travel, offset, segment_x, left, right, limit;
    SDL_Rect activity;
    if (remaining_w < 1)
      return;
    segment_w = remaining_w / 4;
    if (segment_w < 10)
      segment_w = remaining_w < 10 ? remaining_w : 10;
    if (segment_w > 48)
      segment_w = 48;
    travel = remaining_w + segment_w;
    offset = (int)((activity_tick / 8u) % (unsigned)travel) - segment_w;
    segment_x = remaining_x + offset;
    left = segment_x < remaining_x ? remaining_x : segment_x;
    right = segment_x + segment_w;
    limit = remaining_x + remaining_w;
    if (right > limit)
      right = limit;
    if (right <= left)
      return;
    if (validation_bar)
      SDL_SetRenderDrawColor(r, 158, 205, 229, 255);
    else
      SDL_SetRenderDrawColor(r, 166, 220, 183, 255);
    activity.x = left;
    activity.y = y + inset;
    activity.w = right - left;
    activity.h = inner_h;
    SDL_RenderFillRect(r, &activity);
  }
}

static int child_poll(ChildWatch *child) {
  pid_t result;
  if (!child || child->pid <= 0)
    return 0;
  if (child->reaped)
    return 1;
  do {
    result = waitpid(child->pid, &child->status, WNOHANG);
  } while (result < 0 && errno == EINTR);
  if (result == child->pid) {
    child->reaped = 1;
    return 1;
  }
  if (result < 0) {
    child->wait_error = errno;
    child->reaped = 1;
    return 1;
  }
  return 0;
}

static int child_wait(ChildWatch *child) {
  pid_t result;
  if (!child || child->pid <= 0)
    return -1;
  if (!child->reaped) {
    do {
      result = waitpid(child->pid, &child->status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != child->pid) {
      child->wait_error = errno;
      child->reaped = 1;
      return -1;
    }
    child->reaped = 1;
  }
  if (child->wait_error)
    return -1;
  if (WIFEXITED(child->status))
    return WEXITSTATUS(child->status);
  if (WIFSIGNALED(child->status))
    return 128 + WTERMSIG(child->status);
  return -1;
}

static int splash_loop(ChildWatch *child) {
  const char *setup = getenv("SONIC_SETUP_FILE");
  const char *stop = getenv("SONIC_SETUP_STOP");
  SDL_Window *w = NULL;
  SDL_Renderer *r = NULL;
  int setup_w = 640, setup_h = 480;
  unsigned activity_tick = 0;
  int try;
  if (!setup || !*setup)
    setup = "/tmp/sonic_setup.txt";
  if (!stop || !*stop)
    stop = "/tmp/sonic_setup_stop";

  {
    const char *size_env = getenv("SONIC_SETUP_SIZE");
    if (size_env)
      sscanf(size_env, "%dx%d", &setup_w, &setup_h);
    if (setup_w < 160 || setup_w > 4096)
      setup_w = 640;
    if (setup_h < 120 || setup_h > 4096)
      setup_h = 480;
  }

  /* The display may remain busy briefly after EmulationStation exits. */
  for (try = 0; try < 30 && !r; try++) {
    Uint32 window_flags;
    const char *windowed;
    if (sp_file_exists(stop) || child_poll(child))
      return 0;
    if (try == 6 && getenv("SDL_VIDEODRIVER")) {
      fprintf(stderr,
              "[setup] inherited driver '%s' unavailable; retrying automatic selection\n",
              getenv("SDL_VIDEODRIVER"));
      unsetenv("SDL_VIDEODRIVER");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      fprintf(stderr, "[setup] SDL_Init failed (try %d): %s\n", try,
              SDL_GetError());
      SDL_Quit();
      usleep(500 * 1000);
      continue;
    }
    {
      const char *driver = SDL_GetCurrentVideoDriver();
      if (driver && (strcmp(driver, "offscreen") == 0 ||
                     strcmp(driver, "dummy") == 0)) {
        fprintf(stderr, "[setup] invisible driver '%s' rejected (try %d)\n",
                driver, try);
        SDL_Quit();
        usleep(500 * 1000);
        continue;
      }
    }
    windowed = getenv("SONIC_SETUP_WINDOWED");
    window_flags = windowed && strcmp(windowed, "0") != 0
                       ? 0
                       : SDL_WINDOW_FULLSCREEN_DESKTOP;
    w = SDL_CreateWindow("Sonic 4 EP2 setup", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, setup_w, setup_h,
                         window_flags);
    if (!w) {
      fprintf(stderr, "[setup] window failed (try %d): %s\n", try,
              SDL_GetError());
      SDL_Quit();
      usleep(500 * 1000);
      continue;
    }
    r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
    if (!r)
      r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) {
      fprintf(stderr, "[setup] renderer failed (try %d): %s\n", try,
              SDL_GetError());
      SDL_DestroyWindow(w);
      w = NULL;
      SDL_Quit();
      usleep(500 * 1000);
    }
  }
  if (!r) {
    fprintf(stderr, "[setup] no visible renderer; extraction continues headless\n");
    return 1;
  }
  {
    SDL_RendererInfo ri;
    int ww = 0, wh = 0;
    SDL_GetRendererOutputSize(r, &ww, &wh);
    fprintf(stderr, "[setup] UI ready: videodriver=%s renderer=%s output=%dx%d\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
            SDL_GetRendererInfo(r, &ri) == 0 ? ri.name : "?", ww, wh);
  }

  {
    SetupProgress progress = {
        .state = 1,
        .active_done = 0,
        .active_total = 1,
        .phase = 0,
        .validation_permille = 0,
        .extraction_permille = 0,
        .has_v2 = 0,
        .message = "PREPARING GAME DATA",
    };

    while (!sp_file_exists(stop) && !child_poll(child)) {
      SDL_Event event;
      int ww = 640, wh = 480;
      int margin, content_w, title_scale, phase_scale, message_scale;
      int max_chars, max_message_lines, line_count;
      int tiny, compact, title_y, phase_y, message_y, line_step;
      int validation_label_y, label_scale, bar_h, bar_w;
      int validation_bar_y, extraction_label_y, extraction_bar_y;
      int active_bar, line;
      char title[96], message_lines[2][96];
      char validation_label[64], extraction_label[64];
      const char *phase, *version;

      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
          goto out;
      }
      read_setup_state(setup, &progress);
      SDL_GetRendererOutputSize(r, &ww, &wh);
      if (ww < 1)
        ww = setup_w;
      if (wh < 1)
        wh = setup_h;

      margin = ww / 20;
      if (margin < 8)
        margin = 8;
      if (margin > 32)
        margin = 32;
      content_w = ww - margin * 2;
      version = getenv("SONIC_VERSION");
      if (!version || !*version)
        version = SONIC_PORT_VERSION;
      snprintf(title, sizeof(title), "SONIC 4 EP2 %s", version);

      title_scale = wh < 300 ? 3 : 4;
      while (title_scale > 1 && text_width(title, title_scale) > content_w)
        title_scale--;
      if (text_width(title, title_scale) > content_w)
        ellipsize_text(title, sizeof(title),
                       (content_w + title_scale) / (6 * title_scale));
      phase_scale = wh < 200 ? 1 : 2;
      phase = progress.state == 2 ? "SETUP FAILED"
                                  : setup_phase_label(progress.phase);
      while (phase_scale > 1 && text_width(phase, phase_scale) > content_w)
        phase_scale--;

      message_scale = wh < 300 ? 1 : 2;
      max_chars = content_w / (6 * message_scale);
      max_message_lines = wh < 200 ? 1 : 2;
      line_count = wrap_text(progress.message, message_lines,
                             max_message_lines, max_chars);

      tiny = wh < 200;
      compact = wh < 300;
      title_y = tiny ? 6 : (compact ? 12 : wh / 10);
      phase_y = title_y + 7 * title_scale + (tiny ? 4 : (compact ? 7 : 12));
      message_y = phase_y + 7 * phase_scale +
                  (tiny ? 4 : (compact ? 7 : 12));
      line_step = 9 * message_scale;
      validation_label_y = message_y + max_message_lines * line_step +
                           (tiny ? 3 : (compact ? 6 : 14));
      label_scale = tiny ? 1 : (compact ? 1 : 2);
      while (label_scale > 1 &&
             text_width("VALIDATION  WORKING", label_scale) > content_w)
        label_scale--;
      bar_h = tiny ? 10 : (compact ? 14 : 22);
      bar_w = content_w > 480 ? 480 : content_w;
      validation_bar_y = validation_label_y + 7 * label_scale +
                         (tiny ? 2 : 4);
      extraction_label_y = validation_bar_y + bar_h +
                           (tiny ? 5 : (compact ? 8 : 14));
      extraction_bar_y = extraction_label_y + 7 * label_scale +
                         (tiny ? 2 : 4);

      SDL_SetRenderDrawColor(r, 4, 7, 12, 255);
      SDL_RenderClear(r);
      SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
      draw_text_centered(r, ww / 2, title_y, title_scale, title);
      SDL_SetRenderDrawColor(r, progress.state == 2 ? 224 : 176,
                             progress.state == 2 ? 92 : 190,
                             progress.state == 2 ? 92 : 198, 255);
      draw_text_centered(r, ww / 2, phase_y, phase_scale, phase);
      SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
      for (line = 0; line < line_count; line++)
        draw_text_centered(r, ww / 2, message_y + line * line_step,
                           message_scale, message_lines[line]);

      active_bar = setup_active_bar(&progress);
      if (active_bar == 1 && progress.validation_permille == 0)
        snprintf(validation_label, sizeof(validation_label),
                 "VALIDATION  WORKING");
      else
        snprintf(validation_label, sizeof(validation_label), "VALIDATION  %d%%",
                 progress.validation_permille / 10);
      if (active_bar == 2 && progress.extraction_permille == 0)
        snprintf(extraction_label, sizeof(extraction_label),
                 "EXTRACTION  WORKING");
      else
        snprintf(extraction_label, sizeof(extraction_label), "EXTRACTION  %d%%",
                 progress.extraction_permille / 10);

      SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
      draw_text_centered(r, ww / 2, validation_label_y, label_scale,
                         validation_label);
      draw_setup_bar(r, ww / 2 - bar_w / 2, validation_bar_y, bar_w, bar_h,
                     progress.validation_permille, active_bar == 1,
                     progress.state, 1, activity_tick);
      SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
      draw_text_centered(r, ww / 2, extraction_label_y, label_scale,
                         extraction_label);
      draw_setup_bar(r, ww / 2 - bar_w / 2, extraction_bar_y, bar_w, bar_h,
                     progress.extraction_permille, active_bar == 2,
                     progress.state, 0, activity_tick);

      if (!tiny) {
        int signature_scale = compact ? 1 : 2;
        SDL_SetRenderDrawColor(r, 150, 160, 145, 255);
        draw_text(r, ww - margin - text_width("NEXTOS", signature_scale),
                  wh - margin - 7 * signature_scale, signature_scale, "NEXTOS");
      }

      SDL_RenderPresent(r);
      SDL_Delay(100);
      activity_tick += 100;
    }
  }

out:
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return 0;
}

int sonic_run_setup_splash(void) { return splash_loop(NULL); }

int sonic_run_firstrun_bake(void) {
  char setup_path[160], stop_path[160];
  const char *setup = getenv("SONIC_SETUP_FILE");
  const char *stop = getenv("SONIC_SETUP_STOP");
  const char *extractor = getenv("SONIC_EXTRACTOR");
  int own_setup = !setup || !*setup;
  int own_stop = !stop || !*stop;
  int splash_rc, extract_rc;
  ChildWatch child;
  long token_time = (long)time(NULL);

  if (own_setup) {
    snprintf(setup_path, sizeof(setup_path), "/tmp/sonic_setup.%ld.%ld.txt",
             (long)getpid(), token_time);
    setup = setup_path;
    if (setenv("SONIC_SETUP_FILE", setup, 1) != 0)
      return -1;
  }
  if (own_stop) {
    snprintf(stop_path, sizeof(stop_path), "/tmp/sonic_setup_stop.%ld.%ld",
             (long)getpid(), token_time);
    stop = stop_path;
    if (setenv("SONIC_SETUP_STOP", stop, 1) != 0) {
      if (own_setup)
        unsetenv("SONIC_SETUP_FILE");
      return -1;
    }
  }
  unlink(stop);
  if (!extractor || !*extractor)
    extractor = "tools/sonic4ep2_extract.sh";

  memset(&child, 0, sizeof(child));
  child.pid = fork();
  if (child.pid == 0) {
    setenv("SONIC_NO_SPLASH", "1", 1);
    execlp("bash", "bash", extractor, (char *)NULL);
    _exit(127);
  }
  if (child.pid < 0) {
    if (own_setup) {
      unlink(setup);
      unsetenv("SONIC_SETUP_FILE");
    }
    if (own_stop) {
      unlink(stop);
      unsetenv("SONIC_SETUP_STOP");
    }
    return -1;
  }

  splash_rc = splash_loop(&child);
  extract_rc = child_wait(&child);
  unlink(stop);
  if (own_setup) {
    unlink(setup);
    unsetenv("SONIC_SETUP_FILE");
  }
  if (own_stop)
    unsetenv("SONIC_SETUP_STOP");
  fprintf(stderr, "[setup] extractor status=%d, UI status=%d\n", extract_rc,
          splash_rc);
  return extract_rc;
}
