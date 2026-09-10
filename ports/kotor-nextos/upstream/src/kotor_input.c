/*
 * KOTOR controller normalization.
 *
 * The Android KOTOR binary consumes SDL_JOY* events directly and assumes the
 * standardized Android order (buttons: A=0 B=1 X=2 Y=3 BACK=4 GUIDE=5 START=6
 * L3=7 R3=8 LB=9 RB=10 DPAD=11..14; axes: LX=0 LY=1 RX=2 RY=3 LT=4 RT=5).
 * SDL_GAMECONTROLLERCONFIG alone cannot change raw joystick event indices, so
 * this linked boundary translates them before the events reach the game.
 *
 * v1.0.3: the physical layout is no longer guessed from VID/PID profiles.
 * Every CFW ships the authoritative layout of its own pad (PortMaster
 * get_controls -> SDL_GAMECONTROLLERCONFIG), so the translation table is
 * built at runtime from SDL_GameControllerMappingForGUID().  The proven
 * TWIN/GO static profiles remain as fallback for pads without a mapping.
 *
 * v1.0.4: three rules imported from ports that already solved this.
 *
 * 1. A physical button with no binding MUST NOT reach the game.  The proven
 *    reference is ports/asm2_127 (TASM2): it consumes the SDL_GameController
 *    API, where an unbound button simply never produces an event.  We consume
 *    raw SDL_JOY* (the Aspyr binary demands it), so the same guarantee has to
 *    be enforced here -- previously an unbound button was forwarded with
 *    button = OUT_NONE (0xFF), i.e. a garbage index into the game's tables.
 *    On a pad whose hotkey emits two codes (e.g. BTN_TL2 + KEY_GOTO, both
 *    enumerated by SDL because KEY_GOTO falls inside [BTN_JOYSTICK, KEY_MAX)),
 *    that is two garbage events per press.
 * 2. `guide:` is never honoured, and a physical button already claimed by one
 *    key is never re-claimed by a later one.  CFW mapping generators translate
 *    an es_input.cfg where `hotkeyenable` shares a button with another
 *    function into two bindings on the same bN; since the mapping is written
 *    in alphabetical order, the later key silently wins.  (Lesson from the
 *    Minecraft Bedrock fix, 2026-07-20: dropping `guide:` was what restored
 *    the exit combo.)
 * 3. SELECT+START is also read straight from evdev, so the exit combo survives
 *    a mapping that has no back/start at all -- SDL computes a GUID with a CRC
 *    field that often fails to match the gamecontrollerdb line, and the
 *    automatic mapping it falls back to declares only a0-a3/b0-b11.  Port of
 *    ports/geometrydash/src/pad_evdev.c (originally from ports/chrono), which
 *    also covers BTN_TRIGGER_HAPPY1..4.
 *
 * MIT license.
 */
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "kotor_framework.h"

typedef int (*sdl_poll_event_fn)(SDL_Event *);

/* Perfis estaticos de fallback (pads sem mapping no SDL). */
enum pad_profile {
  PAD_NONE = 0,
  PAD_TWIN,   /* 0810:0001 Twin PS2/USB (NextOS Mali-450) */
  PAD_GO,     /* 484b:1100/1101 GO-Super / retrogame_joypad */
  PAD_DYNAMIC /* tabela montada do mapping do proprio CFW */
};

#define DYN_MAX_BUTTONS 32
#define DYN_MAX_AXES    16
#define OUT_NONE 0xFF

struct dyn_map {
  Uint8 button_out[DYN_MAX_BUTTONS];      /* botao fisico -> botao Android */
  Uint8 button_trigger_axis[DYN_MAX_BUTTONS]; /* botao fisico -> eixo 4/5 */
  Uint8 axis_out[DYN_MAX_AXES];           /* eixo fisico -> eixo Android */
  Uint8 hat_dir_button[4];                /* up/down/left/right -> botao */
  int has_hat_dpad;
  int back_button;                        /* fisicos, p/ SELECT+START exit */
  int start_button;
};

static sdl_poll_event_fn real_SDL_PollEvent;
static SDL_JoystickID cached_instance = -1;
static int cached_profile;
static struct dyn_map dyn;
static SDL_JoystickID hat_instance = -1;
static Uint8 hat_state;
static SDL_Event queued_hat_events[8];
static unsigned queued_hat_head;
static unsigned queued_hat_count;
static int input_fifo_fd = -2;
static int injected_release_button = -1;
static int injected_center_axis = -1;
static Uint32 injected_release_at;
static Uint8 back_down;
static Uint8 start_down;

static int remap_enabled(void) {
  const char *value = getenv("KOTOR_INPUT_REMAP");
  return !value || strcmp(value, "0") != 0;
}

/* ------------------------------------------------------------------- evdev */
/* Port of ports/geometrydash/src/pad_evdev.c (from ports/chrono, measured on
 * the R36S/ArkOS).  SELECT/START also arrive as BTN_TRIGGER_HAPPY1/2 on the
 * RG351/GO-Super family, which is exactly what SDL's automatic mapping drops.
 * Reading the kernel codes works on ANY pad and hardcodes no button index. */

/* BTN_* on new headers, KEY_* on old ones; the codes are stable kernel ABI. */
#define PAD_KEY_TRIGGER_HAPPY1 0x2c0 /* SELECT on these handhelds */
#define PAD_KEY_TRIGGER_HAPPY2 0x2c1 /* START */

#define EVDEV_MAX_DEVICES 8

static int evdev_fd[EVDEV_MAX_DEVICES];
static int evdev_count = -1;
static Uint8 evdev_select, evdev_start;

#define EVDEV_BIT_SET(bits, code)                                             \
  ((bits[(code) / (8 * sizeof(unsigned long))] >>                             \
    ((code) % (8 * sizeof(unsigned long)))) & 1UL)

static int evdev_is_gamepad(int fd) {
  /* The bitmap size is the READER's (our process' unsigned long). */
  unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
  memset(keys, 0, sizeof keys);
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
    return 0;
  return EVDEV_BIT_SET(keys, BTN_SOUTH) || EVDEV_BIT_SET(keys, BTN_A) ||
         EVDEV_BIT_SET(keys, PAD_KEY_TRIGGER_HAPPY1);
}

static void evdev_open(void) {
  if (evdev_count >= 0)
    return;
  evdev_count = 0;
  const char *value = getenv("KOTOR_INPUT_EVDEV");
  if (!remap_enabled() || (value && strcmp(value, "0") == 0)) {
    fprintf(stderr, "kotor_input: evdev exit chord disabled by env\n");
    return;
  }
  for (int index = 0; index < 32 && evdev_count < EVDEV_MAX_DEVICES; ++index) {
    char path[64];
    snprintf(path, sizeof path, "/dev/input/event%d", index);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    if (!evdev_is_gamepad(fd)) {
      close(fd);
      continue;
    }
    char name[128] = "?";
    (void)ioctl(fd, EVIOCGNAME(sizeof name), name);
    fprintf(stderr, "kotor_input: evdev %s (%s) -- SELECT+START also read here\n",
            path, name);
    evdev_fd[evdev_count++] = fd;
  }
  if (!evdev_count)
    fprintf(stderr, "kotor_input: no evdev pad readable; exit chord relies on"
                    " the SDL mapping alone\n");
}

/*
 * Returns 1 while both are held. Never blocks: every fd is non-blocking.
 *
 * Called on EVERY poll, so the state is current from the first frame.  Opening
 * the descriptors lazily, only once the gate below opens, arrives too late:
 * the gate needs the mapping, the mapping is only known after the first
 * joystick event, and that first event IS the chord press -- so the press was
 * missed and only the release reached us (measured on the R36S: the game
 * stayed up, log showed the evdev node being opened between the press and the
 * release).  Reading is free and does not consume events for SDL: each open
 * descriptor gets its own copy of the stream.
 *
 * Whether we ACT on it is gated: only when the SDL mapping failed to declare
 * back/start.  The
 * reference ports OR the two paths unconditionally, which is safe on the pads
 * they were measured on; it is NOT safe in general.  Measured counter-example
 * (RG34XX-SP / Knulli): that driver reports the physical LT as BTN_SELECT and
 * the physical RT as BTN_START, so an unconditional OR would turn "both
 * triggers" -- an ordinary KOTOR input -- into "quit the game".  When the
 * mapping is complete the SDL path is already authoritative and evdev is not
 * read at all.
 */
static int evdev_exit_chord(void) {
  evdev_open();
  for (int i = 0; i < evdev_count; ++i) {
    struct input_event event;
    while (read(evdev_fd[i], &event, sizeof event) == (ssize_t)sizeof event) {
      if (event.type != EV_KEY)
        continue;
      const Uint8 down = event.value != 0; /* 1 press, 2 autorepeat */
      switch (event.code) {
        case BTN_SELECT:
        case PAD_KEY_TRIGGER_HAPPY1: evdev_select = down; break;
        case BTN_START:
        case PAD_KEY_TRIGGER_HAPPY2: evdev_start = down; break;
        default: break;
      }
    }
  }
  return evdev_select && evdev_start;
}

static int dynamic_enabled(void) {
  const char *value = getenv("KOTOR_INPUT_DYNAMIC");
  return !value || strcmp(value, "0") != 0;
}

/* ------------------------------------------------------------------ dynamic */

static const struct {
  const char *key;
  Uint8 android_button;
} button_keys[] = {
    {"a", SDL_CONTROLLER_BUTTON_A},
    {"b", SDL_CONTROLLER_BUTTON_B},
    {"x", SDL_CONTROLLER_BUTTON_X},
    {"y", SDL_CONTROLLER_BUTTON_Y},
    {"back", SDL_CONTROLLER_BUTTON_BACK},
    {"guide", SDL_CONTROLLER_BUTTON_GUIDE},
    {"start", SDL_CONTROLLER_BUTTON_START},
    {"leftstick", SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"rightstick", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"leftshoulder", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"rightshoulder", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"dpup", SDL_CONTROLLER_BUTTON_DPAD_UP},
    {"dpdown", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"dpleft", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {"dpright", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
};

static const struct {
  const char *key;
  Uint8 android_axis;
} axis_keys[] = {
    {"leftx", 0}, {"lefty", 1}, {"rightx", 2}, {"righty", 3},
    {"lefttrigger", 4}, {"righttrigger", 5},
};

static int hat_mask_to_dir(int mask) {
  switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_DOWN: return 1;
    case SDL_HAT_LEFT: return 2;
    case SDL_HAT_RIGHT: return 3;
    default: return -1;
  }
}

/* Um campo "chave:valor" do mapping. valor: bN, hH.M, aN, +aN, -aN, aN~ */
static void dyn_apply_field(struct dyn_map *map, const char *key,
                            size_t key_len, const char *value) {
  char clean[16];
  size_t n = 0;
  const char *v = value;
  if (*v == '+' || *v == '-') v++;
  while (*v && *v != ',' && n + 1 < sizeof(clean)) clean[n++] = *v++;
  clean[n] = '\0';
  if (n && clean[n - 1] == '~') clean[n - 1] = '\0';
  if (!clean[0]) return;

  for (unsigned i = 0; i < SDL_arraysize(button_keys); ++i) {
    if (strlen(button_keys[i].key) != key_len ||
        strncmp(button_keys[i].key, key, key_len) != 0)
      continue;
    const Uint8 out = button_keys[i].android_button;
    /* `guide` is the CFW hotkey wearing a costume: generators translate an
     * es_input.cfg whose `hotkeyenable` shares a button with another function
     * into guide:bN plus that other binding on the SAME bN.  KOTOR has no use
     * for GUIDE, and honouring it is what ate the exit combo in the Minecraft
     * Bedrock case (2026-07-20).  Never bind it. */
    if (out == SDL_CONTROLLER_BUTTON_GUIDE)
      return;
    if (clean[0] == 'b') {
      const int phys = atoi(clean + 1);
      if (phys >= 0 && phys < DYN_MAX_BUTTONS) {
        /* First key to claim a physical button keeps it.  The mapping is
         * written in alphabetical order, so without this a duplicate lets the
         * later key (e.g. `start`) silently steal a button already bound. */
        if (map->button_out[phys] != OUT_NONE ||
            map->button_trigger_axis[phys] != OUT_NONE) {
          fprintf(stderr,
                  "kotor_input: b%d already bound; ignoring %.*s\n",
                  phys, (int)key_len, key);
          return;
        }
        map->button_out[phys] = out;
        if (out == SDL_CONTROLLER_BUTTON_BACK) map->back_button = phys;
        if (out == SDL_CONTROLLER_BUTTON_START) map->start_button = phys;
      }
    } else if (clean[0] == 'h') {
      const char *dot = strchr(clean, '.');
      if (dot) {
        const int dir = hat_mask_to_dir(atoi(dot + 1));
        if (dir >= 0) {
          map->hat_dir_button[dir] = out;
          map->has_hat_dpad = 1;
        }
      }
    } else {
      fprintf(stderr, "kotor_input: unsupported binding %.*s:%s\n",
              (int)key_len, key, clean);
    }
    return;
  }

  for (unsigned i = 0; i < SDL_arraysize(axis_keys); ++i) {
    if (strlen(axis_keys[i].key) != key_len ||
        strncmp(axis_keys[i].key, key, key_len) != 0)
      continue;
    const Uint8 out = axis_keys[i].android_axis;
    if (clean[0] == 'a') {
      const int phys = atoi(clean + 1);
      if (phys >= 0 && phys < DYN_MAX_AXES)
        map->axis_out[phys] = out;
    } else if (clean[0] == 'b' && (out == 4 || out == 5)) {
      const int phys = atoi(clean + 1);
      /* same first-wins rule as the button path, across BOTH tables */
      if (phys >= 0 && phys < DYN_MAX_BUTTONS) {
        if (map->button_out[phys] != OUT_NONE ||
            map->button_trigger_axis[phys] != OUT_NONE) {
          fprintf(stderr,
                  "kotor_input: b%d already bound; ignoring %.*s\n",
                  phys, (int)key_len, key);
          return;
        }
        map->button_trigger_axis[phys] = out;
      }
    }
    return;
  }
}

/* Monta a tabela a partir do mapping SDL ("guid,nome,chave:valor,..."). */
static int dyn_build(struct dyn_map *map, const char *mapping) {
  memset(map, OUT_NONE, sizeof(*map));
  map->has_hat_dpad = 0;
  map->back_button = -1;
  map->start_button = -1;

  const char *cursor = strchr(mapping, ',');       /* pula guid */
  if (cursor) cursor = strchr(cursor + 1, ',');    /* pula nome */
  if (!cursor) return 0;
  cursor++;

  int fields = 0;
  while (*cursor) {
    const char *colon = strchr(cursor, ':');
    const char *comma = strchr(cursor, ',');
    if (!colon || (comma && colon > comma)) {
      if (!comma) break;
      cursor = comma + 1;
      continue;
    }
    if (strncmp(cursor, "platform", (size_t)(colon - cursor)) != 0) {
      dyn_apply_field(map, cursor, (size_t)(colon - cursor), colon + 1);
      fields++;
    }
    if (!comma) break;
    cursor = comma + 1;
  }
  return fields > 0;
}

/* ------------------------------------------------------------------ profile */

static int pad_profile(SDL_JoystickID instance) {
  const char *forced = getenv("KOTOR_INPUT_REMAP");
  if (forced && strcmp(forced, "force") == 0)
    return PAD_TWIN;
  if (!remap_enabled())
    return PAD_NONE;

  if (instance == cached_instance)
    return cached_profile;

  cached_instance = instance;
  cached_profile = PAD_NONE;

  SDL_Joystick *joystick = SDL_JoystickFromInstanceID(instance);
  if (!joystick)
    return PAD_NONE;

  const SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
  char guid_str[64] = "?";
  SDL_JoystickGetGUIDString(guid, guid_str, sizeof(guid_str));
  const char *name = SDL_JoystickName(joystick);

  /* O jogo so inicializa o subsistema de joystick; o de gamecontroller e
   * quem carrega os mappings do ambiente. Refcounted: seguro repetir. */
  if (dynamic_enabled()) {
    static int gc_ready = -1;
    if (gc_ready == -1)
      gc_ready = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    char *mapping = SDL_GameControllerMappingForGUID(guid);
    if (mapping) {
      if (dyn_build(&dyn, mapping)) {
        cached_profile = PAD_DYNAMIC;
        fprintf(stderr,
                "kotor_input: %s (%s) -> dynamic mapping"
                " (hat_dpad=%d back=b%d start=b%d)\n",
                name ? name : "?", guid_str, dyn.has_hat_dpad,
                dyn.back_button, dyn.start_button);
        fprintf(stderr, "kotor_input: mapping = %s\n", mapping);
      }
      SDL_free(mapping);
      if (cached_profile == PAD_DYNAMIC)
        return cached_profile;
    }
  }

  /* GUID do SDL no Linux: vendor em 4..5 e product em 8..9, little-endian.
   * Os quatro primeiros bytes variam entre builds do SDL e nao servem. */
  if (guid.data[4] == 0x10 && guid.data[5] == 0x08 &&
      guid.data[8] == 0x01 && guid.data[9] == 0x00)
    cached_profile = PAD_TWIN;
  else if (guid.data[4] == 0x4b && guid.data[5] == 0x48 &&
           guid.data[9] == 0x11 &&
           (guid.data[8] == 0x00 || guid.data[8] == 0x01))
    cached_profile = PAD_GO;
  fprintf(stderr, "kotor_input: %s (%s) -> fallback profile %d\n",
          name ? name : "?", guid_str, cached_profile);
  return cached_profile;
}

/*
 * The SDL path owns the exit chord whenever the mapping actually declares both
 * ends of it.  It is only when the mapping is incomplete -- SDL's automatic
 * fallback declares a0-a3/b0-b11 and no back/start, the case measured on the
 * R36S -- that evdev has to fill in.  cached_profile is only meaningful after
 * the first pad event, so before that we stay off.
 */
static int exit_chord_needs_evdev(void) {
  if (!remap_enabled())
    return 0;
  if (cached_instance < 0)
    return 0;
  if (cached_profile == PAD_DYNAMIC)
    return dyn.back_button < 0 || dyn.start_button < 0;
  return cached_profile == PAD_NONE;
}

static int hat_needs_translation(SDL_JoystickID instance) {
  const int profile = pad_profile(instance);
  if (profile == PAD_TWIN)
    return 1;
  return profile == PAD_DYNAMIC && dyn.has_hat_dpad;
}

static Uint8 hat_dir_output(int profile, int dir) {
  static const Uint8 defaults[4] = {
      SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
      SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
  if (profile == PAD_DYNAMIC && dyn.hat_dir_button[dir] != OUT_NONE)
    return dyn.hat_dir_button[dir];
  return defaults[dir];
}

/* The only indices the Aspyr/Android SDL path understands. Anything else is a
 * garbage index into the game's own tables and must never be delivered. */
static int android_button_valid(Uint8 button) {
  return button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
}

static Uint8 normalized_button(Uint8 physical) {
  static const Uint8 map[12] = {
      SDL_CONTROLLER_BUTTON_Y,              /* b0: Y */
      SDL_CONTROLLER_BUTTON_B,              /* b1: B */
      SDL_CONTROLLER_BUTTON_A,              /* b2: A */
      SDL_CONTROLLER_BUTTON_X,              /* b3: X */
      SDL_CONTROLLER_BUTTON_INVALID,        /* b4: LT -> axis 4 */
      SDL_CONTROLLER_BUTTON_INVALID,        /* b5: RT -> axis 5 */
      SDL_CONTROLLER_BUTTON_LEFTSHOULDER,   /* b6: LB */
      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,  /* b7: RB */
      SDL_CONTROLLER_BUTTON_BACK,           /* b8: Select/Back */
      SDL_CONTROLLER_BUTTON_START,          /* b9: Start */
      SDL_CONTROLLER_BUTTON_LEFTSTICK,      /* b10: L3 */
      SDL_CONTROLLER_BUTTON_RIGHTSTICK,     /* b11: R3 */
  };

  return physical < SDL_arraysize(map) ? map[physical] : physical;
}

static void button_to_trigger_axis(SDL_Event *event, Uint8 axis) {
  const Uint32 type = event->type;
  const Uint32 timestamp = event->jbutton.timestamp;
  const SDL_JoystickID which = event->jbutton.which;
  const Sint16 value = type == SDL_JOYBUTTONDOWN ? 32767 : 0;

  memset(event, 0, sizeof(*event));
  event->jaxis.type = SDL_JOYAXISMOTION;
  event->jaxis.timestamp = timestamp;
  event->jaxis.which = which;
  event->jaxis.axis = axis;
  event->jaxis.value = value;
}

/*
 * "GO-Super Gamepad" (R36S/R36T, driver odroidgo3-joypad, 484b:1100).
 * Sem HAT: o D-pad vem como botao. A ordem abaixo e a declarada pelo proprio
 * firmware em gamecontrollerdb (a:b1,b:b0,dpup:b8,...), entao o port se
 * comporta igual a qualquer outro jogo do aparelho.
 */
static Uint8 go_normalized_button(Uint8 physical) {
  static const Uint8 map[17] = {
      SDL_CONTROLLER_BUTTON_B,              /* b0  BTN_SOUTH  -> B */
      SDL_CONTROLLER_BUTTON_A,              /* b1  BTN_EAST   -> A */
      SDL_CONTROLLER_BUTTON_X,              /* b2  BTN_NORTH  -> X */
      SDL_CONTROLLER_BUTTON_Y,              /* b3  BTN_WEST   -> Y */
      SDL_CONTROLLER_BUTTON_LEFTSHOULDER,   /* b4  BTN_TL     -> L1 */
      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,  /* b5  BTN_TR     -> R1 */
      SDL_CONTROLLER_BUTTON_INVALID,        /* b6  BTN_TL2    -> eixo 4 */
      SDL_CONTROLLER_BUTTON_INVALID,        /* b7  BTN_TR2    -> eixo 5 */
      SDL_CONTROLLER_BUTTON_DPAD_UP,        /* b8 */
      SDL_CONTROLLER_BUTTON_DPAD_DOWN,      /* b9 */
      SDL_CONTROLLER_BUTTON_DPAD_LEFT,      /* b10 */
      SDL_CONTROLLER_BUTTON_DPAD_RIGHT,     /* b11 */
      SDL_CONTROLLER_BUTTON_BACK,           /* b12 TH1 -> SELECT */
      SDL_CONTROLLER_BUTTON_START,          /* b13 TH2 -> START */
      SDL_CONTROLLER_BUTTON_LEFTSTICK,      /* b14 TH3 -> L3 */
      SDL_CONTROLLER_BUTTON_RIGHTSTICK,     /* b15 TH4 -> R3 */
      SDL_CONTROLLER_BUTTON_GUIDE,          /* b16 TH5 -> guide */
  };

  return physical < SDL_arraysize(map) ? map[physical]
                                       : SDL_CONTROLLER_BUTTON_INVALID;
}

static void queue_hat_button(const SDL_JoyHatEvent *source, Uint8 button,
                             Uint32 type) {
  if (queued_hat_count >= SDL_arraysize(queued_hat_events))
    return;
  const unsigned slot =
      (queued_hat_head + queued_hat_count) % SDL_arraysize(queued_hat_events);
  SDL_Event *event = &queued_hat_events[slot];
  memset(event, 0, sizeof(*event));
  event->jbutton.type = type;
  event->jbutton.timestamp = source->timestamp;
  event->jbutton.which = source->which;
  event->jbutton.button = button;
  event->jbutton.state =
      type == SDL_JOYBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
  queued_hat_count++;
}

static int translate_hat_event(SDL_Event *event) {
  if (event->type != SDL_JOYHATMOTION ||
      !hat_needs_translation(event->jhat.which))
    return 0;

  const int profile = pad_profile(event->jhat.which);

  if (hat_instance != event->jhat.which) {
    hat_instance = event->jhat.which;
    hat_state = SDL_HAT_CENTERED;
  }

  static const Uint8 direction_masks[4] = {SDL_HAT_UP, SDL_HAT_DOWN,
                                           SDL_HAT_LEFT, SDL_HAT_RIGHT};

  const Uint8 old_state = hat_state;
  const Uint8 new_state = event->jhat.value;
  hat_state = new_state;

  /* Release old directions before pressing new ones so diagonals and direct
   * up<->down transitions have the same ordering as Android gamepads. */
  for (int dir = 0; dir < 4; ++dir) {
    if ((old_state & direction_masks[dir]) &&
        !(new_state & direction_masks[dir]))
      queue_hat_button(&event->jhat, hat_dir_output(profile, dir),
                       SDL_JOYBUTTONUP);
  }
  for (int dir = 0; dir < 4; ++dir) {
    if (!(old_state & direction_masks[dir]) &&
        (new_state & direction_masks[dir]))
      queue_hat_button(&event->jhat, hat_dir_output(profile, dir),
                       SDL_JOYBUTTONDOWN);
  }

  if (!queued_hat_count)
    return 0;
  *event = queued_hat_events[queued_hat_head];
  queued_hat_head =
      (queued_hat_head + 1) % SDL_arraysize(queued_hat_events);
  queued_hat_count--;
  return 1;
}

/* Optional test boundary. It is completely dormant in normal launches and
 * lets an SSH test drive the native menu without attaching a debugger or
 * creating a second SDL joystick. Each byte written to the named FIFO is one
 * standardized Android-controller button: a/b/x/y/u/d/l/r. */
static int poll_injected_event(SDL_Event *event) {
  const char *path = getenv("KOTOR_INPUT_FIFO");
  if (!path || !path[0] || !event)
    return 0;

  const SDL_JoystickID which = SDL_JoystickGetDeviceInstanceID(0);
  if (which < 0)
    return 0;

  Uint8 button;
  Uint32 type;
  if (injected_release_button >= 0 || injected_center_axis >= 0) {
    if (!SDL_TICKS_PASSED(SDL_GetTicks(), injected_release_at))
      return 0;
    if (injected_center_axis >= 0) {
      const Uint8 axis = (Uint8)injected_center_axis;
      injected_center_axis = -1;
      memset(event, 0, sizeof(*event));
      event->jaxis.type = SDL_JOYAXISMOTION;
      event->jaxis.timestamp = SDL_GetTicks();
      event->jaxis.which = which;
      event->jaxis.axis = axis;
      event->jaxis.value = 0;
      return 1;
    }
    button = (Uint8)injected_release_button;
    injected_release_button = -1;
    type = SDL_JOYBUTTONUP;
  } else {
    if (input_fifo_fd == -2)
      input_fifo_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (input_fifo_fd < 0)
      return 0;

    char command;
    for (;;) {
      if (read(input_fifo_fd, &command, 1) != 1)
        return 0;
      Sint16 axis_value = 0;
      Uint8 axis = 0;
      if (command == 'U' || command == 'D') {
        axis = 1;
        axis_value = command == 'U' ? -32767 : 32767;
      } else if (command == 'L' || command == 'R') {
        axis = 0;
        axis_value = command == 'L' ? -32767 : 32767;
      }
      if (axis_value) {
        memset(event, 0, sizeof(*event));
        event->jaxis.type = SDL_JOYAXISMOTION;
        event->jaxis.timestamp = SDL_GetTicks();
        event->jaxis.which = which;
        event->jaxis.axis = axis;
        event->jaxis.value = axis_value;
        injected_center_axis = axis;
        injected_release_at = SDL_GetTicks() + 300;
        return 1;
      }
      switch (command) {
        case 'a': button = SDL_CONTROLLER_BUTTON_A; break;
        case 'b': button = SDL_CONTROLLER_BUTTON_B; break;
        case 'x': button = SDL_CONTROLLER_BUTTON_X; break;
        case 'y': button = SDL_CONTROLLER_BUTTON_Y; break;
        case 'u': button = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
        case 'd': button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
        case 'l': button = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
        case 'r': button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
        case 's': button = SDL_CONTROLLER_BUTTON_START; break;
        case 'k': button = SDL_CONTROLLER_BUTTON_BACK; break;
        default: continue;
      }
      break;
    }
    injected_release_button = button;
    injected_release_at = SDL_GetTicks() + 120;
    type = SDL_JOYBUTTONDOWN;
  }

  memset(event, 0, sizeof(*event));
  event->jbutton.type = type;
  event->jbutton.timestamp = SDL_GetTicks();
  event->jbutton.which = which;
  event->jbutton.button = button;
  event->jbutton.state =
      type == SDL_JOYBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
  return 1;
}

/* Returns 1 to deliver the event, 0 to drop it (see rule 1 in the header). */
static int normalize_event(SDL_Event *event) {
  if (translate_hat_event(event))
    return 1;

  int profile;
  if (event->type == SDL_JOYAXISMOTION)
    profile = pad_profile(event->jaxis.which);
  else if (event->type == SDL_JOYBUTTONDOWN ||
           event->type == SDL_JOYBUTTONUP)
    profile = pad_profile(event->jbutton.which);
  else
    return 1;

  if (profile == PAD_NONE)
    return 1;

  if (event->type == SDL_JOYAXISMOTION) {
    /* TWIN: a2/a3 sao right-Y/right-X e o KOTOR Android espera X/Y.
     * GO: o firmware ja declara leftx:a0 lefty:a1 rightx:a2 righty:a3,
     * que e exatamente a ordem do Android -> nao mexer.
     * DYNAMIC: segue o mapping do CFW; eixo sem binding passa intacto. */
    if (profile == PAD_TWIN) {
      if (event->jaxis.axis == 2)
        event->jaxis.axis = 3;
      else if (event->jaxis.axis == 3)
        event->jaxis.axis = 2;
    } else if (profile == PAD_DYNAMIC &&
               event->jaxis.axis < DYN_MAX_AXES &&
               dyn.axis_out[event->jaxis.axis] != OUT_NONE) {
      event->jaxis.axis = dyn.axis_out[event->jaxis.axis];
    }
    return 1;
  }

  const Uint8 physical = event->jbutton.button;
  const Uint8 pressed = event->type == SDL_JOYBUTTONDOWN;
  int back_physical, start_physical;
  if (profile == PAD_DYNAMIC) {
    back_physical = dyn.back_button;
    start_physical = dyn.start_button;
  } else if (profile == PAD_GO) {
    back_physical = 12;
    start_physical = 13;
  } else {
    back_physical = 8;
    start_physical = 9;
  }

  if (back_physical >= 0 && physical == back_physical)
    back_down = pressed;
  else if (start_physical >= 0 && physical == start_physical)
    start_down = pressed;

  if (pressed && back_down && start_down) {
    memset(event, 0, sizeof(*event));
    event->type = SDL_QUIT;
    back_down = 0;
    start_down = 0;
    return 1;
  }

  /* L2/R2 como botao fisico viram o eixo de gatilho 4/5 que o caminho SDL
   * do Android expoe para o KOTOR. */
  if (profile == PAD_DYNAMIC) {
    if (physical < DYN_MAX_BUTTONS &&
        dyn.button_trigger_axis[physical] != OUT_NONE) {
      button_to_trigger_axis(event, dyn.button_trigger_axis[physical]);
      return 1;
    }
    const Uint8 out = physical < DYN_MAX_BUTTONS ? dyn.button_out[physical]
                                                 : OUT_NONE;
    if (!android_button_valid(out)) {
      static unsigned dropped;
      if (dropped++ < 16)
        fprintf(stderr, "kotor_input: b%u has no binding; event dropped\n",
                physical);
      return 0;
    }
    event->jbutton.button = out;
    return 1;
  }

  if (profile == PAD_GO) {
    if (physical == 6 || physical == 7) {
      button_to_trigger_axis(event, physical == 6 ? 4 : 5);
      return 1;
    }
    const Uint8 out = go_normalized_button(physical);
    if (!android_button_valid(out))
      return 0;
    event->jbutton.button = out;
    return 1;
  }

  if (physical == 4 || physical == 5) {
    button_to_trigger_axis(event, physical);
    return 1;
  }

  const Uint8 out = normalized_button(physical);
  if (!android_button_valid(out))
    return 0;
  event->jbutton.button = out;
  return 1;
}

int SDL_PollEvent(SDL_Event *event) {
  if (!real_SDL_PollEvent)
    real_SDL_PollEvent =
        (sdl_poll_event_fn)dlsym(RTLD_NEXT, "SDL_PollEvent");
  if (!real_SDL_PollEvent)
    return 0;

  if (poll_injected_event(event)) {
    kotor_framework_observe_event(event);
    return 1;
  }

  if (event && queued_hat_count) {
    *event = queued_hat_events[queued_hat_head];
    queued_hat_head =
        (queued_hat_head + 1) % SDL_arraysize(queued_hat_events);
    queued_hat_count--;
    kotor_framework_observe_event(event);
    return 1;
  }

  /* Ler SEMPRE (estado atual desde o primeiro frame), agir so' quando o
   * mapping da SDL nao consegue entregar o combo. */
  const int chord_held = evdev_exit_chord();
  /* Trinco: o combo fica "apertado" por dezenas de frames, e sem isto cada
   * poll dentro da janela reemitia SDL_QUIT e uma linha de log -- medido no
   * R36S, milhares de linhas e 766 KB de debug.log num unico aperto.
   * Um SDL_QUIT por aperto; rearma quando soltar. */
  static int chord_fired;
  if (!chord_held)
    chord_fired = 0;
  if (event && chord_held && !chord_fired && exit_chord_needs_evdev()) {
    chord_fired = 1;
    memset(event, 0, sizeof(*event));
    event->type = SDL_QUIT;
    fprintf(stderr, "kotor_input: SELECT+START (evdev) -> quit\n");
    kotor_framework_observe_event(event);
    return 1;
  }

  /* A dropped event must not surface as "no event": keep pulling until the
   * queue gives us something the game can actually consume, or runs dry. */
  for (;;) {
    const int result = real_SDL_PollEvent(event);
    if (!result || !event) {
      kotor_framework_poll_input();
      return result;
    }
    if (normalize_event(event)) {
      kotor_framework_observe_event(event);
      return 1;
    }
  }
}
