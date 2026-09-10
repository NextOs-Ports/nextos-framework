/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput-doctor -- V4-CONTROLLERS-03 / C2 read-only live input matrix.
 *
 * Shows, for a pad, the complete 18-control matrix: which controls were
 * never pressed, first press/release, stick center/min/max and trigger
 * min/max -- without ever grabbing the device, injecting input or requiring
 * root when the input node is readable.
 *
 * HARD RULES: never grab the device, never exclusive open, never write to the
 * device, never print a free-form device name, path, IP or hostname --
 * only standardized BTN_/ABS_ symbolic names and pad ordinals.
 *
 * Modes:
 *   --replay FILE   hermetic: read a synthetic event script (one event per
 *                   line: `key <code> <0|1>` or `abs <code> <value>`), feed
 *                   the same matrix and print it. Used by the host gate.
 *   --evdev PATH    live: read-only O_NONBLOCK poll of one event node for
 *                   --seconds N (default 10), then print the matrix. The
 *                   PATH is consumed, redacted and never echoed.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#define DOCTOR_ABS_MAX 64
#define DOCTOR_KEY_BASE 0x100
#define DOCTOR_KEY_MAX 0x300

struct key_state {
  unsigned int presses;
  unsigned int releases;
};

struct abs_state {
  int seen;
  int first;
  int min;
  int max;
  int last;
};

static struct key_state g_keys[DOCTOR_KEY_MAX - DOCTOR_KEY_BASE];
static struct abs_state g_abs[DOCTOR_ABS_MAX];

/* Standard Linux gamepad symbolic names; anything else prints as KEY_0x%x,
 * which is symbolic by construction and never a free-form device string. */
static const struct {
  int code;
  const char *name;
  const char *canonical;
} key_names[] = {
    {BTN_SOUTH, "BTN_SOUTH", "A"},
    {BTN_EAST, "BTN_EAST", "B"},
    {BTN_NORTH, "BTN_NORTH", "X"},
    {BTN_WEST, "BTN_WEST", "Y"},
    {BTN_TL, "BTN_TL", "L1"},
    {BTN_TR, "BTN_TR", "R1"},
    {BTN_TL2, "BTN_TL2", "L2"},
    {BTN_TR2, "BTN_TR2", "R2"},
    {BTN_THUMBL, "BTN_THUMBL", "L3"},
    {BTN_THUMBR, "BTN_THUMBR", "R3"},
    {BTN_START, "BTN_START", "START"},
    {BTN_SELECT, "BTN_SELECT", "SELECT"},
    {BTN_MODE, "BTN_MODE", "GUIDE"},
    {BTN_DPAD_UP, "BTN_DPAD_UP", "UP"},
    {BTN_DPAD_DOWN, "BTN_DPAD_DOWN", "DOWN"},
    {BTN_DPAD_LEFT, "BTN_DPAD_LEFT", "LEFT"},
    {BTN_DPAD_RIGHT, "BTN_DPAD_RIGHT", "RIGHT"},
    {BTN_TRIGGER_HAPPY1, "BTN_TRIGGER_HAPPY1", "SELECT?"},
    {BTN_TRIGGER_HAPPY2, "BTN_TRIGGER_HAPPY2", "START?"},
    {BTN_TRIGGER_HAPPY3, "BTN_TRIGGER_HAPPY3", "-"},
    {BTN_TRIGGER_HAPPY4, "BTN_TRIGGER_HAPPY4", "-"},
    {BTN_TRIGGER_HAPPY5, "BTN_TRIGGER_HAPPY5", "-"},
};

static const struct {
  int code;
  const char *name;
  const char *canonical;
} abs_names[] = {
    {ABS_X, "ABS_X", "LEFT_STICK.x"},
    {ABS_Y, "ABS_Y", "LEFT_STICK.y"},
    {ABS_RX, "ABS_RX", "RIGHT_STICK.x"},
    {ABS_RY, "ABS_RY", "RIGHT_STICK.y"},
    {ABS_Z, "ABS_Z", "L2/RIGHT_STICK.x?"},
    {ABS_RZ, "ABS_RZ", "R2/RIGHT_STICK.y?"},
    {ABS_HAT0X, "ABS_HAT0X", "LEFT/RIGHT"},
    {ABS_HAT0Y, "ABS_HAT0Y", "UP/DOWN"},
    {ABS_GAS, "ABS_GAS", "R2?"},
    {ABS_BRAKE, "ABS_BRAKE", "L2?"},
};

static const char *key_name(int code, const char **canonical) {
  static char fallback[24];
  size_t i;
  for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
    if (key_names[i].code == code) {
      *canonical = key_names[i].canonical;
      return key_names[i].name;
    }
  }
  (void)snprintf(fallback, sizeof fallback, "KEY_0x%x", (unsigned int)code);
  *canonical = "-";
  return fallback;
}

static const char *abs_name(int code, const char **canonical) {
  static char fallback[24];
  size_t i;
  for (i = 0; i < sizeof(abs_names) / sizeof(abs_names[0]); i++) {
    if (abs_names[i].code == code) {
      *canonical = abs_names[i].canonical;
      return abs_names[i].name;
    }
  }
  (void)snprintf(fallback, sizeof fallback, "ABS_0x%x", (unsigned int)code);
  *canonical = "-";
  return fallback;
}

static void feed_key(int code, int value) {
  if (code < DOCTOR_KEY_BASE || code >= DOCTOR_KEY_MAX) {
    return;
  }
  if (value == 1) {
    g_keys[code - DOCTOR_KEY_BASE].presses++;
  } else if (value == 0) {
    g_keys[code - DOCTOR_KEY_BASE].releases++;
  }
}

static void feed_abs(int code, int value) {
  struct abs_state *state;
  if (code < 0 || code >= DOCTOR_ABS_MAX) {
    return;
  }
  state = &g_abs[code];
  if (!state->seen) {
    state->seen = 1;
    state->first = value;
    state->min = value;
    state->max = value;
  }
  if (value < state->min) state->min = value;
  if (value > state->max) state->max = value;
  state->last = value;
}

static void print_matrix(void) {
  static const char *const canonical[] = {
      "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
      "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
      "LEFT_STICK", "RIGHT_STICK",
  };
  int touched[18];
  int code;
  size_t i;

  memset(touched, 0, sizeof touched);
  printf("NXINPUT-DOCTOR: pad=0 read-only matrix (no grab, no injection)\n");
  printf("-- keys observed --\n");
  for (code = DOCTOR_KEY_BASE; code < DOCTOR_KEY_MAX; code++) {
    const struct key_state *state = &g_keys[code - DOCTOR_KEY_BASE];
    const char *canon;
    const char *name;
    if (state->presses == 0u && state->releases == 0u) {
      continue;
    }
    name = key_name(code, &canon);
    printf("key %-20s canonical=%-8s press=%u release=%u\n", name, canon,
           state->presses, state->releases);
    for (i = 0; i < 18u; i++) {
      size_t len = strlen(canonical[i]);
      /* A trailing '?' marks a family guess (TRIGGER_HAPPY select/start);
       * the guess still lights the matrix, the '?' stays in the listing. */
      if (strncmp(canon, canonical[i], len) == 0 &&
          (canon[len] == '\0' || canon[len] == '?')) {
        touched[i] = 1;
      }
    }
  }
  printf("-- axes observed (first=center) --\n");
  for (code = 0; code < DOCTOR_ABS_MAX; code++) {
    const struct abs_state *state = &g_abs[code];
    const char *canon;
    const char *name;
    if (!state->seen) {
      continue;
    }
    name = abs_name(code, &canon);
    printf("abs %-12s canonical=%-18s center=%d min=%d max=%d last=%d\n",
           name, canon, state->first, state->min, state->max, state->last);
    if (strncmp(canon, "LEFT_STICK", 10) == 0) touched[16] = 1;
    if (strncmp(canon, "RIGHT_STICK", 11) == 0) touched[17] = 1;
    /* Ambiguous trigger axes are marked on their first-named guess only;
     * the trailing '?' in the listing keeps the ambiguity visible. */
    if (strncmp(canon, "L2", 2) == 0) touched[6] = 1;
    if (strncmp(canon, "R2", 2) == 0) touched[7] = 1;
  }
  printf("-- canonical matrix --\n");
  for (i = 0; i < 18u; i++) {
    printf("control %-12s %s\n", canonical[i],
           touched[i] ? "seen" : "never-pressed");
  }
}

static int run_replay(const char *path) {
  FILE *stream = fopen(path, "r");
  char kind[16];
  int code, value;
  if (stream == NULL) {
    fprintf(stderr, "replay script unavailable\n");
    return 2;
  }
  while (fscanf(stream, "%15s %i %i", kind, &code, &value) == 3) {
    if (strcmp(kind, "key") == 0) {
      feed_key(code, value);
    } else if (strcmp(kind, "abs") == 0) {
      feed_abs(code, value);
    }
  }
  fclose(stream);
  print_matrix();
  return 0;
}

static int run_evdev(const char *path, int seconds) {
  struct input_event events[64];
  struct pollfd pfd;
  time_t deadline;
  int fd;

  /* Read-only, non-exclusive, no grab ever. */
  fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "input node unavailable (readable evdev required); "
                    "the path is not echoed\n");
    return 2;
  }
  printf("NXINPUT-DOCTOR: observing pad=0 for %ds (read-only)...\n", seconds);
  deadline = time(NULL) + seconds;
  while (time(NULL) < deadline) {
    ssize_t got;
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 500) <= 0) {
      continue;
    }
    got = read(fd, events, sizeof events);
    if (got <= 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue;
      }
      break;
    }
    {
      size_t count = (size_t)got / sizeof(events[0]);
      size_t i;
      for (i = 0; i < count; i++) {
        if (events[i].type == EV_KEY) {
          feed_key((int)events[i].code, (int)events[i].value);
        } else if (events[i].type == EV_ABS) {
          feed_abs((int)events[i].code, (int)events[i].value);
        }
      }
    }
  }
  close(fd);
  print_matrix();
  return 0;
}

int main(int argc, char **argv) {
  int seconds = 10;
  const char *replay = NULL;
  const char *evdev = NULL;
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
      replay = argv[++i];
    } else if (strcmp(argv[i], "--evdev") == 0 && i + 1 < argc) {
      evdev = argv[++i];
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = atoi(argv[++i]);
      if (seconds < 1 || seconds > 600) {
        seconds = 10;
      }
    } else {
      fprintf(stderr,
              "usage: nxinput-doctor --replay FILE | --evdev PATH "
              "[--seconds N]\n");
      return 2;
    }
  }
  if (replay != NULL) {
    return run_replay(replay);
  }
  if (evdev != NULL) {
    return run_evdev(evdev, seconds);
  }
  fprintf(stderr, "usage: nxinput-doctor --replay FILE | --evdev PATH "
                  "[--seconds N]\n");
  return 2;
}
