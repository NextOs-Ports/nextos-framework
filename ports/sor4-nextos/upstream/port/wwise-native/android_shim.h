/*
 * android_shim.h -- fake Android NDK types and functions for Linux
 *
 * Implements the android_native_app_glue pattern so that android_main()
 * in libsyberia1.so can run on Linux ARM64.
 */

#ifndef __ANDROID_SHIM_H__
#define __ANDROID_SHIM_H__

#include <stdint.h>

/* ---------- Forward declarations / opaque types ---------- */

typedef struct ANativeWindow ANativeWindow;
typedef struct ANativeActivity ANativeActivity;
typedef struct AInputQueue AInputQueue;
typedef struct ALooper ALooper;
typedef struct AConfiguration AConfiguration;
typedef struct ASensorManager ASensorManager;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct AInputEvent AInputEvent;

/* ---------- ANativeActivity ---------- */

typedef struct ANativeActivityCallbacks {
  void (*onStart)(ANativeActivity *activity);
  void (*onResume)(ANativeActivity *activity);
  void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outSize);
  void (*onPause)(ANativeActivity *activity);
  void (*onStop)(ANativeActivity *activity);
  void (*onDestroy)(ANativeActivity *activity);
  void (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
  void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
  void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
  void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
  void (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
  void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
  void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
  void (*onContentRectChanged)(ANativeActivity *activity, const void *rect);
  void (*onConfigurationChanged)(ANativeActivity *activity);
  void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void *vm;          // JavaVM* - fake
  void *env;         // JNIEnv* - fake
  void *clazz;       // jobject - fake
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;    // user data
  void *assetManager; // AAssetManager* - fake
  const char *obbPath;
};

/* ---------- android_app (native_app_glue) ---------- */

// Commands from the app thread
enum {
  APP_CMD_INPUT_CHANGED = 0,
  APP_CMD_INIT_WINDOW,
  APP_CMD_TERM_WINDOW,
  APP_CMD_WINDOW_RESIZED,
  APP_CMD_WINDOW_REDRAW_NEEDED,
  APP_CMD_CONTENT_RECT_CHANGED,
  APP_CMD_GAINED_FOCUS,
  APP_CMD_LOST_FOCUS,
  APP_CMD_CONFIG_CHANGED,
  APP_CMD_LOW_MEMORY,
  APP_CMD_START,
  APP_CMD_RESUME,
  APP_CMD_SAVE_STATE,
  APP_CMD_PAUSE,
  APP_CMD_STOP,
  APP_CMD_DESTROY,
};

// Looper data IDs
enum {
  LOOPER_ID_MAIN = 1,
  LOOPER_ID_INPUT = 2,
  LOOPER_ID_USER = 3,
};

struct android_poll_source {
  int32_t id;
  struct android_app *app;
  void (*process)(struct android_app *app, struct android_poll_source *source);
};

struct android_app {
  void *userData;
  void (*onAppCmd)(struct android_app *app, int32_t cmd);
  int32_t (*onInputEvent)(struct android_app *app, AInputEvent *event);

  ANativeActivity *activity;
  AConfiguration *config;
  void *savedState;
  size_t savedStateSize;

  ALooper *looper;
  AInputQueue *inputQueue;
  ANativeWindow *window;

  int activityState;
  int destroyRequested;

  // Internal pipe for commands
  int msgread;
  int msgwrite;

  struct android_poll_source cmdPollSource;
  struct android_poll_source inputPollSource;
};

/* ---------- ALooper ---------- */
ALooper *ALooper_prepare(int opts);
void ALooper_addFd(void *looper, int fd, int ident, int events,
                   void *callback, void *data);
int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData);

/* ---------- AInputQueue ---------- */
void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data);
void AInputQueue_detachLooper(void *queue);
int AInputQueue_getEvent(void *queue, AInputEvent **outEvent);
int AInputQueue_preDispatchEvent(void *queue, void *event);
void AInputQueue_finishEvent(void *queue, void *event, int handled);

/* ---------- AInputEvent ---------- */

// Input event types
#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2

// Key event actions
#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1

// Motion event actions
#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2

// Android keycodes
#define AKEYCODE_BACK 4
#define AKEYCODE_MENU 82
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_L2 104
#define AKEYCODE_BUTTON_R2 105
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_BUTTON_THUMBL 106
#define AKEYCODE_BUTTON_THUMBR 107
#define AKEYCODE_ENTER 66

// Android motion event axis IDs
#define AMOTION_EVENT_AXIS_X 0
#define AMOTION_EVENT_AXIS_Y 1
#define AMOTION_EVENT_AXIS_Z 11
#define AMOTION_EVENT_AXIS_RZ 14
#define AMOTION_EVENT_AXIS_HAT_X 15
#define AMOTION_EVENT_AXIS_HAT_Y 16
#define AMOTION_EVENT_AXIS_LTRIGGER 17
#define AMOTION_EVENT_AXIS_RTRIGGER 18
#define AMOTION_EVENT_AXIS_MAX 64

// Input source constants
#define AINPUT_SOURCE_TOUCHSCREEN 0x1002
#define AINPUT_SOURCE_JOYSTICK 0x1000010

// Fake input event structure
typedef struct {
  int type;          // AINPUT_EVENT_TYPE_KEY or AINPUT_EVENT_TYPE_MOTION
  int action;        // key or motion action
  int keycode;       // Android keycode (key events)
  int source;        // AINPUT_SOURCE_*
  float x, y;        // Touch coordinates (motion events)
  float axes[AMOTION_EVENT_AXIS_MAX]; // Axis values for joystick motion
  int pointer_count;
  int pointer_id;
} FakeInputEvent;

// Additional input functions
float AMotionEvent_getAxisValue(void *event, int axis, int pointerIndex);
int AInputEvent_getSource(void *event);

int AInputEvent_getType(void *event);
int AKeyEvent_getAction(void *event);
int AKeyEvent_getKeyCode(void *event);
float AMotionEvent_getX(void *event, int pointerIndex);
float AMotionEvent_getY(void *event, int pointerIndex);
int AMotionEvent_getAction(void *event);
int AMotionEvent_getPointerCount(void *event);
int AMotionEvent_getPointerId(void *event, int pointerIndex);

/* ---------- AConfiguration ---------- */
AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(void *config);
void AConfiguration_fromAssetManager(void *config, void *assetManager);
void AConfiguration_setLocale(void *config, const char *locale);
int AConfiguration_getLanguage(void *config, char *outLanguage);
int AConfiguration_getCountry(void *config, char *outCountry);
int AConfiguration_getDensity(void *config);
int AConfiguration_getOrientation(void *config);
void AConfiguration_setOrientation(void *config, int orientation);
int AConfiguration_getScreenSize(void *config);

/* ---------- ASensorManager ---------- */
ASensorManager *ASensorManager_getInstance(void);
void *ASensorManager_getDefaultSensor(void *manager, int type);
ASensorEventQueue *ASensorManager_createEventQueue(void *manager,
                                                    void *looper, int ident,
                                                    void *callback,
                                                    void *data);
int ASensorEventQueue_enableSensor(void *queue, void *sensor);
int ASensorEventQueue_setEventRate(void *queue, void *sensor, int32_t usec);

/* ---------- ANativeActivity ---------- */
void ANativeActivity_finish(void *activity);

/* ---------- Shim initialization ---------- */
struct android_app *android_shim_init(void);
void android_shim_cleanup(void);
void android_shim_send_cmd(struct android_app *app, int8_t cmd);
ANativeWindow *android_shim_get_window(void);

#endif
