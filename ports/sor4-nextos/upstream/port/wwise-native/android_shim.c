/*
 * android_shim.c -- fake Android NDK for Linux ARM64
 *
 * Implements enough of the android_native_app_glue + Android NDK
 * to let libsyberia1.so's android_main() run on Linux.
 *
 * Input handling:
 *   SDL gamepad events are converted to fake AInputEvent structs
 *   (key events for buttons, motion events for analog stick cursor).
 *   The game's onInputEvent callback receives them through the
 *   standard AInputQueue_getEvent flow.
 */

#include <SDL2/SDL.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "error.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "util.h"

/* ---- Screen resolution (Trimui Smart Pro) ---- */
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

/* ---- Input event queue ---- */
#define MAX_INPUT_EVENTS 64

static FakeInputEvent g_input_queue[MAX_INPUT_EVENTS];
static int g_input_head = 0; // next write position
static int g_input_tail = 0; // next read position
static FakeInputEvent *g_current_event = NULL; // event being processed

// Virtual cursor for analog stick → touch mapping
static float g_cursor_x = SCREEN_WIDTH / 2.0f;
static float g_cursor_y = SCREEN_HEIGHT / 2.0f;
static int g_cursor_down = 0; // whether virtual "finger" is down

// Last sent joystick axis values (to avoid flooding)
static float g_last_lx = 0, g_last_ly = 0, g_last_rx = 0, g_last_ry = 0;

// SDL gamepad
static SDL_GameController *g_gamecontroller = NULL;

/* ---- Globals ---- */
static struct android_app g_app;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static SDL_Window *g_sdl_window = NULL;

// Fake window handle - we just use a pointer to distinguish it from NULL
static int g_fake_native_window = 1;

// Fake input queue handle
static int g_fake_input_queue = 1;

/* ---- Input event queue helpers ---- */

static int input_queue_count(void) {
  return (g_input_head - g_input_tail + MAX_INPUT_EVENTS) % MAX_INPUT_EVENTS;
}

static int input_queue_push(const FakeInputEvent *ev) {
  int next = (g_input_head + 1) % MAX_INPUT_EVENTS;
  if (next == g_input_tail)
    return 0; // full
  g_input_queue[g_input_head] = *ev;
  g_input_head = next;
  return 1;
}

static FakeInputEvent *input_queue_pop(void) {
  if (g_input_tail == g_input_head)
    return NULL; // empty
  FakeInputEvent *ev = &g_input_queue[g_input_tail];
  g_input_tail = (g_input_tail + 1) % MAX_INPUT_EVENTS;
  return ev;
}

/* ---- Push key event ---- */

static void push_key_event(int action, int keycode) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  input_queue_push(&ev);
}

/* ---- Push motion (touch) event ---- */

static void push_motion_event(int action, float x, float y) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = action;
  ev.source = AINPUT_SOURCE_TOUCHSCREEN;
  ev.x = x;
  ev.y = y;
  ev.pointer_count = 1;
  ev.pointer_id = 0;
  input_queue_push(&ev);
}

/* ---- Push joystick motion event (axis values) ---- */

static void push_joystick_event(float lx, float ly, float rx, float ry) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.pointer_count = 1;
  ev.axes[AMOTION_EVENT_AXIS_X] = lx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = ly;
  ev.axes[AMOTION_EVENT_AXIS_Z] = rx;
  ev.axes[AMOTION_EVENT_AXIS_RZ] = ry;
  input_queue_push(&ev);
}

/* ---- SDL button → Android keycode mapping ---- */

static int sdl_button_to_keycode(int sdl_button) {
  switch (sdl_button) {
  case SDL_CONTROLLER_BUTTON_A:
    return AKEYCODE_BUTTON_B; // Trimui: A/B swapped (Nintendo layout)
  case SDL_CONTROLLER_BUTTON_B:
    return AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_X:
    return AKEYCODE_BUTTON_X;
  case SDL_CONTROLLER_BUTTON_Y:
    return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_BACK:
    return AKEYCODE_BACK;
  case SDL_CONTROLLER_BUTTON_START:
    return AKEYCODE_BUTTON_START;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return AKEYCODE_BUTTON_R1;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return AKEYCODE_BUTTON_THUMBL;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return AKEYCODE_BUTTON_THUMBR;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return AKEYCODE_DPAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return AKEYCODE_DPAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return AKEYCODE_DPAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return AKEYCODE_DPAD_RIGHT;
  default:
    return -1;
  }
}

/* ---- Initialize gamepad ---- */

static void init_gamecontroller(void) {
  if (g_gamecontroller)
    return;
  int num = SDL_NumJoysticks();
  debugPrintf("android_shim: %d joysticks found\n", num);
  for (int i = 0; i < num; i++) {
    if (SDL_IsGameController(i)) {
      g_gamecontroller = SDL_GameControllerOpen(i);
      if (g_gamecontroller) {
        debugPrintf("android_shim: Opened gamepad: %s\n",
                    SDL_GameControllerName(g_gamecontroller));
        return;
      }
    }
  }
}

/* ---- Process SDL events into input queue ---- */

#define STICK_DEADZONE 8000
#define CURSOR_SPEED 12.0f

static void process_sdl_events(void) {
  // Try to open a gamepad if we don't have one yet
  init_gamecontroller();

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_QUIT:
      g_app.destroyRequested = 1;
      break;

    case SDL_CONTROLLERBUTTONDOWN: {
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_DOWN, kc);
        debugPrintf("android_shim: button DOWN keycode=%d\n", kc);
      }
      // D-pad also sends HAT axis joystick events
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        float hat_x = 0, hat_y = 0;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  hat_x = -1.0f;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) hat_x = 1.0f;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)    hat_y = -1.0f;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  hat_y = 1.0f;
        FakeInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AINPUT_EVENT_TYPE_MOTION;
        ev.action = AMOTION_EVENT_ACTION_MOVE;
        ev.source = AINPUT_SOURCE_JOYSTICK;
        ev.pointer_count = 1;
        ev.axes[AMOTION_EVENT_AXIS_HAT_X] = hat_x;
        ev.axes[AMOTION_EVENT_AXIS_HAT_Y] = hat_y;
        input_queue_push(&ev);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONUP: {
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_UP, kc);
      }
      // D-pad release: reset HAT axes to 0
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        FakeInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AINPUT_EVENT_TYPE_MOTION;
        ev.action = AMOTION_EVENT_ACTION_MOVE;
        ev.source = AINPUT_SOURCE_JOYSTICK;
        ev.pointer_count = 1;
        input_queue_push(&ev);
      }
      break;
    }

    case SDL_CONTROLLERDEVICEADDED:
      debugPrintf("android_shim: Controller added: %d\n", e.cdevice.which);
      init_gamecontroller();
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      debugPrintf("android_shim: Controller removed\n");
      if (g_gamecontroller) {
        SDL_GameControllerClose(g_gamecontroller);
        g_gamecontroller = NULL;
      }
      break;

    default:
      break;
    }
  }

  // Send analog stick values as joystick motion events
  if (g_gamecontroller) {
    int raw_lx = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_LEFTX);
    int raw_ly = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_LEFTY);
    int raw_rx = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_RIGHTX);
    int raw_ry = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_RIGHTY);

    // Apply deadzone
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE)
      lx = (float)raw_lx / 32767.0f;
    if (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE)
      ly = (float)raw_ly / 32767.0f;
    if (raw_rx > STICK_DEADZONE || raw_rx < -STICK_DEADZONE)
      rx = (float)raw_rx / 32767.0f;
    if (raw_ry > STICK_DEADZONE || raw_ry < -STICK_DEADZONE)
      ry = (float)raw_ry / 32767.0f;

    // Send joystick event only when values change
    if (lx != g_last_lx || ly != g_last_ly ||
        rx != g_last_rx || ry != g_last_ry) {
      push_joystick_event(lx, ly, rx, ry);
      g_last_lx = lx;
      g_last_ly = ly;
      g_last_rx = rx;
      g_last_ry = ry;
    }

    // Also update virtual cursor for touch simulation
    if (lx != 0 || ly != 0) {
      g_cursor_x += lx * CURSOR_SPEED;
      g_cursor_y += ly * CURSOR_SPEED;
      if (g_cursor_x < 0)
        g_cursor_x = 0;
      if (g_cursor_x >= SCREEN_WIDTH)
        g_cursor_x = SCREEN_WIDTH - 1;
      if (g_cursor_y < 0)
        g_cursor_y = 0;
      if (g_cursor_y >= SCREEN_HEIGHT)
        g_cursor_y = SCREEN_HEIGHT - 1;
    }
  }
}

/* ---- ALooper ---- */

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  static int fake_looper;
  return (ALooper *)&fake_looper;
}

void ALooper_addFd(void *looper, int fd, int ident, int events,
                   void *callback, void *data) {
  (void)looper;
  (void)fd;
  (void)ident;
  (void)events;
  (void)callback;
  (void)data;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
  (void)outFd;
  (void)outEvents;

  // Fire pending audio callbacks first (before any blocking)
  opensles_shim_pump_callbacks();

  // Check for pending commands on the pipe (don't block long)
  struct pollfd pfd;
  pfd.fd = g_app.msgread;
  pfd.events = POLLIN;
  pfd.revents = 0;

  // Cap poll timeout to 5ms to keep audio flowing
  int timeout = timeoutMillis;
  if (timeout < 0 || timeout > 5)
    timeout = 5;

  int ret = poll(&pfd, 1, timeout);
  if (ret > 0 && (pfd.revents & POLLIN)) {
    if (outData)
      *outData = &g_app.cmdPollSource;
    return LOOPER_ID_MAIN;
  }

  // Only poll SDL when input queue is empty (avoids flooding)
  if (input_queue_count() == 0) {
    process_sdl_events();
  }

  // If there are input events queued, return LOOPER_ID_INPUT
  if (input_queue_count() > 0) {
    if (outData)
      *outData = &g_app.inputPollSource;
    return LOOPER_ID_INPUT;
  }

  // Fire audio callbacks again after poll
  opensles_shim_pump_callbacks();

  return -1; // no events
}

/* ---- AInputQueue ---- */

void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data) {
  (void)queue;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
}

void AInputQueue_detachLooper(void *queue) { (void)queue; }

int AInputQueue_getEvent(void *queue, AInputEvent **outEvent) {
  (void)queue;
  FakeInputEvent *ev = input_queue_pop();
  if (!ev) {
    if (outEvent)
      *outEvent = NULL;
    return -1; // no events
  }
  g_current_event = ev;
  if (outEvent)
    *outEvent = (AInputEvent *)ev;
  return 0; // success
}

int AInputQueue_preDispatchEvent(void *queue, void *event) {
  (void)queue;
  (void)event;
  return 0; // don't consume
}

void AInputQueue_finishEvent(void *queue, void *event, int handled) {
  (void)queue;
  (void)event;
  (void)handled;
  g_current_event = NULL;
}

/* ---- AInputEvent getters ---- */

int AInputEvent_getType(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->type;
}

int AKeyEvent_getAction(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->action;
}

int AKeyEvent_getKeyCode(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->keycode;
}

float AMotionEvent_getX(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->x;
}

float AMotionEvent_getY(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->y;
}

int AMotionEvent_getAction(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->action;
}

int AMotionEvent_getPointerCount(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->pointer_count;
}

int AMotionEvent_getPointerId(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->pointer_id;
}

float AMotionEvent_getAxisValue(void *event, int axis, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  if (axis >= 0 && axis < AMOTION_EVENT_AXIS_MAX)
    return ev->axes[axis];
  return 0.0f;
}

int AInputEvent_getSource(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->source;
}

/* ---- AConfiguration stubs ---- */

static int g_fake_config = 0;

AConfiguration *AConfiguration_new(void) {
  return (AConfiguration *)&g_fake_config;
}

void AConfiguration_delete(void *config) { (void)config; }

void AConfiguration_fromAssetManager(void *config, void *assetManager) {
  (void)config;
  (void)assetManager;
}

void AConfiguration_setLocale(void *config, const char *locale) {
  (void)config;
  (void)locale;
}

int AConfiguration_getLanguage(void *config, char *outLanguage) {
  (void)config;
  if (outLanguage) {
    outLanguage[0] = 'e';
    outLanguage[1] = 'n';
  }
  return 2;
}

int AConfiguration_getCountry(void *config, char *outCountry) {
  (void)config;
  if (outCountry) {
    outCountry[0] = 'U';
    outCountry[1] = 'S';
  }
  return 2;
}

int AConfiguration_getDensity(void *config) {
  (void)config;
  return 240; // ACONFIGURATION_DENSITY_HIGH (hdpi)
}

int AConfiguration_getOrientation(void *config) {
  (void)config;
  return 2; // ACONFIGURATION_ORIENTATION_LAND
}

void AConfiguration_setOrientation(void *config, int orientation) {
  (void)config;
  (void)orientation;
}

int AConfiguration_getScreenSize(void *config) {
  (void)config;
  return 3; // ACONFIGURATION_SCREENSIZE_LARGE
}

/* ---- ASensorManager stubs ---- */

ASensorManager *ASensorManager_getInstance(void) {
  static int fake_sensor_mgr;
  return (ASensorManager *)&fake_sensor_mgr;
}

void *ASensorManager_getDefaultSensor(void *manager, int type) {
  (void)manager;
  (void)type;
  return NULL;
}

ASensorEventQueue *ASensorManager_createEventQueue(void *manager,
                                                    void *looper, int ident,
                                                    void *callback,
                                                    void *data) {
  (void)manager;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
  static int fake_event_queue;
  return (ASensorEventQueue *)&fake_event_queue;
}

int ASensorEventQueue_enableSensor(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}

int ASensorEventQueue_setEventRate(void *queue, void *sensor,
                                   int32_t usec) {
  (void)queue;
  (void)sensor;
  (void)usec;
  return 0;
}

/* ---- ANativeActivity stubs ---- */

void ANativeActivity_finish(void *activity) {
  (void)activity;
  debugPrintf("ANativeActivity_finish called\n");
  g_app.destroyRequested = 1;
}

/* ---- android_app command processing ---- */

static void process_cmd(struct android_app *app,
                        struct android_poll_source *source) {
  (void)source;
  int8_t cmd;
  if (read(app->msgread, &cmd, sizeof(cmd)) == sizeof(cmd)) {
    if (app->onAppCmd)
      app->onAppCmd(app, cmd);
  }
}

/* ---- Input processing (called by game via inputPollSource.process) ---- */

static void process_input(struct android_app *app,
                          struct android_poll_source *source) {
  (void)source;
  AInputEvent *event = NULL;
  while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
    if (AInputQueue_preDispatchEvent(app->inputQueue, event))
      continue;
    int handled = 0;
    if (app->onInputEvent) {
      handled = app->onInputEvent(app, event);
      FakeInputEvent *fe = (FakeInputEvent *)event;
      if (fe->type == AINPUT_EVENT_TYPE_KEY) {
        debugPrintf("android_shim: KEY type=%d action=%d keycode=%d handled=%d\n",
                    fe->type, fe->action, fe->keycode, handled);
      } else if (fe->type == AINPUT_EVENT_TYPE_MOTION) {
        debugPrintf("android_shim: MOTION action=%d x=%.0f y=%.0f handled=%d\n",
                    fe->action, fe->x, fe->y, handled);
      }
    }
    AInputQueue_finishEvent(app->inputQueue, event, handled);
  }
}

/* ---- Public API ---- */

struct android_app *android_shim_init(void) {
  debugPrintf("android_shim: Initializing fake Android environment\n");

  memset(&g_app, 0, sizeof(g_app));
  memset(&g_activity, 0, sizeof(g_activity));
  memset(&g_callbacks, 0, sizeof(g_callbacks));

  // Create command pipe
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    fatal_error("android_shim: Failed to create pipe");
  }
  g_app.msgread = pipefd[0];
  g_app.msgwrite = pipefd[1];

  // Setup JNI
  void *fake_vm = NULL;
  void *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);

  // Setup activity
  g_activity.callbacks = &g_callbacks;
  g_activity.vm = fake_vm;
  g_activity.env = fake_env;
  g_activity.sdkVersion = 24; // Android 7.0
  g_activity.internalDataPath = "./gamedata";
  g_activity.externalDataPath = "./gamedata";
  g_activity.obbPath = ".";

  // Setup app
  g_app.activity = &g_activity;
  g_app.config = AConfiguration_new();
  g_app.looper = ALooper_prepare(0);
  g_app.window = (ANativeWindow *)&g_fake_native_window;
  g_app.inputQueue = (AInputQueue *)&g_fake_input_queue;

  // Command poll source
  g_app.cmdPollSource.id = LOOPER_ID_MAIN;
  g_app.cmdPollSource.app = &g_app;
  g_app.cmdPollSource.process = process_cmd;

  // Input poll source
  g_app.inputPollSource.id = LOOPER_ID_INPUT;
  g_app.inputPollSource.app = &g_app;
  g_app.inputPollSource.process = process_input;

  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fatal_error("android_shim: SDL_Init failed: %s\n", SDL_GetError());
  }
  debugPrintf("android_shim: SDL initialized\n");

  // Try to open a gamepad early
  init_gamecontroller();

  debugPrintf("android_shim: Fake android_app ready at %p\n", &g_app);
  return &g_app;
}

void android_shim_send_cmd(struct android_app *app, int8_t cmd) {
  if (write(app->msgwrite, &cmd, sizeof(cmd)) != sizeof(cmd)) {
    debugPrintf("android_shim: Failed to write command %d\n", cmd);
  }
}

ANativeWindow *android_shim_get_window(void) {
  return (ANativeWindow *)&g_fake_native_window;
}

void android_shim_cleanup(void) {
  debugPrintf("android_shim: Cleaning up\n");
  if (g_app.msgread >= 0)
    close(g_app.msgread);
  if (g_app.msgwrite >= 0)
    close(g_app.msgwrite);

  if (g_gamecontroller) {
    SDL_GameControllerClose(g_gamecontroller);
    g_gamecontroller = NULL;
  }

  if (g_sdl_window) {
    SDL_DestroyWindow(g_sdl_window);
    g_sdl_window = NULL;
  }
  SDL_Quit();
}
