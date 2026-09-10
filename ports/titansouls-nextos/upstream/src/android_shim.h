/*
 * android_shim.h -- Android NDK surface for the Titan Souls loader (ARM32).
 *
 * A engine (Abstraction Games, libTestSuite.so) e' uma NativeActivity de
 * verdade: ela traz o android_native_app_glue ESTATICO e o android_main dela
 * roda o proprio laco em cima de ALooper_pollAll + AInputQueue. O loader,
 * portanto, nao inventa fluxo: ele MONTA o `struct android_app` no layout
 * exato do NDK 32-bit e chama android_main(app), deixando a engine andar pela
 * maquina de estados dela.
 *
 * O layout foi CONFIRMADO no binario: ANativeActivity_onCreate faz
 * malloc(148) — 0x94 — que e' exatamente sizeof(struct android_app) no NDK
 * ARM32 (bionic: pthread_mutex_t e pthread_cond_t = 4 bytes cada).
 */

#ifndef __ANDROID_SHIM_H__
#define __ANDROID_SHIM_H__

#include <stdint.h>
#include <stddef.h>

/* ---------- Forward declarations / opaque types ---------- */

typedef struct ANativeWindow ANativeWindow;
typedef struct ANativeActivity ANativeActivity;
typedef struct AInputQueue AInputQueue;
typedef struct ALooper ALooper;
typedef struct AConfiguration AConfiguration;
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
  void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *w);
  void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *w);
  void (*onNativeWindowRedrawNeeded)(ANativeActivity *a, ANativeWindow *w);
  void (*onNativeWindowDestroyed)(ANativeActivity *a, ANativeWindow *w);
  void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
  void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *q);
  void (*onContentRectChanged)(ANativeActivity *activity, const void *rect);
  void (*onConfigurationChanged)(ANativeActivity *activity);
  void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void *vm;           /* JavaVM*  (jni_shim) */
  void *env;          /* JNIEnv*  (jni_shim) */
  void *clazz;        /* jobject  (fake)     */
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;
  void *assetManager; /* AAssetManager* (asset_shim) */
  const char *obbPath;
};

/* ---------- android_app (native_app_glue, layout NDK ARM32) ---------- */

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

enum {
  LOOPER_ID_MAIN = 1,
  LOOPER_ID_INPUT = 2,
  LOOPER_ID_USER = 3,
};

struct android_poll_source {
  int32_t id;                                            /* +0  */
  struct android_app *app;                               /* +4  */
  void (*process)(struct android_app *, struct android_poll_source *); /* +8 */
};

struct android_app {
  void *userData;                                        /* 0   */
  void (*onAppCmd)(struct android_app *app, int32_t cmd);/* 4   */
  int32_t (*onInputEvent)(struct android_app *app, AInputEvent *ev); /* 8 */
  ANativeActivity *activity;                             /* 12  */
  AConfiguration *config;                                /* 16  */
  void *savedState;                                      /* 20  */
  size_t savedStateSize;                                 /* 24  */
  ALooper *looper;                                       /* 28  */
  AInputQueue *inputQueue;                               /* 32  */
  ANativeWindow *window;                                 /* 36  */
  int32_t contentRect[4];                                /* 40  */
  int activityState;                                     /* 56  */
  int destroyRequested;                                  /* 60  */
  char mutex[4];  /* bionic ARM32 pthread_mutex_t */     /* 64  */
  char cond[4];   /* bionic ARM32 pthread_cond_t  */     /* 68  */
  int msgread;                                           /* 72  */
  int msgwrite;                                          /* 76  */
  uint32_t thread;                                       /* 80  */
  struct android_poll_source cmdPollSource;              /* 84  */
  struct android_poll_source inputPollSource;            /* 96  */
  int running;                                           /* 108 */
  int stateSaved;                                        /* 112 */
  int destroyed;                                         /* 116 */
  int redrawNeeded;                                      /* 120 */
  AInputQueue *pendingInputQueue;                        /* 124 */
  ANativeWindow *pendingWindow;                          /* 128 */
  int32_t pendingContentRect[4];                         /* 132 */
};

/* ---------- ALooper ---------- */
ALooper *ALooper_prepare(int opts);
void ALooper_addFd(void *looper, int fd, int ident, int events,
                   void *callback, void *data);
int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData);
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents,
                     void **outData);
int ALooper_removeFd(void *looper, int fd);
ALooper *ALooper_forThread(void);
void ALooper_acquire(void *looper);
void ALooper_release(void *looper);
int ALooper_wake(void *looper);

/* ---------- AInputQueue ---------- */
void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data);
void AInputQueue_detachLooper(void *queue);
int AInputQueue_getEvent(void *queue, AInputEvent **outEvent);
int AInputQueue_hasEvents(void *queue);
int AInputQueue_preDispatchEvent(void *queue, void *event);
void AInputQueue_finishEvent(void *queue, void *event, int handled);

/* ---------- AInputEvent ---------- */

#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2

#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1

#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2

#define AKEYCODE_BACK 4
#define AKEYCODE_MENU 82
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_C 98
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_Z 101
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_L2 104
#define AKEYCODE_BUTTON_R2 105
#define AKEYCODE_BUTTON_THUMBL 106
#define AKEYCODE_BUTTON_THUMBR 107
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_BUTTON_MODE 110
#define AKEYCODE_ENTER 66

#define AMOTION_EVENT_AXIS_X 0
#define AMOTION_EVENT_AXIS_Y 1
#define AMOTION_EVENT_AXIS_Z 11
#define AMOTION_EVENT_AXIS_RZ 14
#define AMOTION_EVENT_AXIS_HAT_X 15
#define AMOTION_EVENT_AXIS_HAT_Y 16
#define AMOTION_EVENT_AXIS_LTRIGGER 17
#define AMOTION_EVENT_AXIS_RTRIGGER 18
#define AMOTION_EVENT_AXIS_BRAKE 23
#define AMOTION_EVENT_AXIS_GAS 22
#define AMOTION_EVENT_AXIS_MAX 48

#define AINPUT_SOURCE_TOUCHSCREEN 0x1002
#define AINPUT_SOURCE_JOYSTICK 0x1000010
#define AINPUT_SOURCE_GAMEPAD 0x00000401
#define AINPUT_SOURCE_DPAD 0x00000201

typedef struct {
  int type;
  int action;
  int keycode;
  int source;
  float x, y;
  float axes[AMOTION_EVENT_AXIS_MAX];
  int pointer_count;
  int pointer_id;
} FakeInputEvent;

/* Estes cruzam a fronteira loader->engine devolvendo float: a engine e'
 * SOFTFP (sem Tag_ABI_VFP_args), entao o retorno tem que sair em r0, nao em
 * s0. Sem o pcs("aapcs") o eixo chega como lixo — e em SILENCIO. */
#if defined(__arm__)
#define TS_GUEST_PCS __attribute__((pcs("aapcs")))
#else
#define TS_GUEST_PCS
#endif

float TS_GUEST_PCS AMotionEvent_getAxisValue(void *event, int axis, int ptr);
float TS_GUEST_PCS AMotionEvent_getX(void *event, int pointerIndex);
float TS_GUEST_PCS AMotionEvent_getY(void *event, int pointerIndex);
int AInputEvent_getSource(void *event);
int AInputEvent_getType(void *event);
int AInputEvent_getDeviceId(void *event);
int AKeyEvent_getAction(void *event);
int AKeyEvent_getKeyCode(void *event);
int AKeyEvent_getFlags(void *event);
int AKeyEvent_getMetaState(void *event);
int AKeyEvent_getRepeatCount(void *event);
int AMotionEvent_getAction(void *event);
int AMotionEvent_getPointerCount(void *event);
int AMotionEvent_getPointerId(void *event, int pointerIndex);
int AMotionEvent_getButtonState(void *event);

/* ---------- AConfiguration ---------- */
AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(void *config);
void AConfiguration_fromAssetManager(void *config, void *assetManager);
void AConfiguration_setLocale(void *config, const char *locale);
int AConfiguration_getLanguage(void *config, char *outLanguage);
int AConfiguration_getCountry(void *config, char *outCountry);
int AConfiguration_getDensity(void *config);
int AConfiguration_getOrientation(void *config);
int AConfiguration_getScreenSize(void *config);

/* ---------- ANativeWindow ---------- */
int ANativeWindow_setBuffersGeometry(void *window, int w, int h, int format);
int ANativeWindow_getWidth(void *window);
int ANativeWindow_getHeight(void *window);
int ANativeWindow_getFormat(void *window);
void ANativeWindow_acquire(void *window);
void ANativeWindow_release(void *window);

/* ---------- ANativeActivity ---------- */
void ANativeActivity_finish(void *activity);
void ANativeActivity_setWindowFlags(void *a, unsigned add, unsigned rem);

/* ---------- loader-side API ---------- */

/* Zoom nativo (FlashPunk FP::GetZoom/SetZoom); 0 = indisponivel. */
void android_shim_set_zoom_symbols(unsigned long get_address,
                                   unsigned long set_address);
struct android_app *android_shim_init(void);
void android_shim_send_cmd(struct android_app *app, int8_t cmd);
void android_shim_install_exit_signals(void);
ANativeWindow *android_shim_get_window(void);
void android_shim_pump(void); /* input host -> fila Android (1 volta) */

#endif
