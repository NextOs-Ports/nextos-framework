/* hgo.h -- shared declarations for the Hitman GO port. */

#ifndef HGO_H
#define HGO_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char hgo_gamedir[1024];
extern char hgo_datadir[1024];   /* <gamedir>/assets */
extern char hgo_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char hgo_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int hgo_log_level;    /* HGO_LOGCAT   : mirror the game's own log     */
extern int hgo_trace_jni;    /* HGO_JNILOG   : every JNI call                */
extern int hgo_trace_gl;     /* HGO_GLLOG    : GL calls and shader sources   */
extern long hgo_max_frames;  /* HGO_FRAMES=N : stop after N frames           */
extern int hgo_capture_mode; /* always zero; retained by the EGL abstraction */

void hgo_bionic_init(void);
size_t hgo_bionic_count(void);
void hgo_pthread_init(void);
void hgo_android_init(void);
void hgo_egl_init(void);
void hgo_jni_init(void);

void *hgo_android_sym(const char *name);
void *hgo_egl_sym(const char *name);
void *hgo_gl_sym(const char *name);
void *hgo_jni_sym(const char *name);
void *hgo_jni_env(void);
void *hgo_jni_vm(void);
void *hgo_jni_activity(void);
void *hgo_jni_native(const char *cls, const char *name);
void *hgo_jret_obj(const char *cls);
void *hgo_jret_class(const char *cls);
void *hgo_jret_str(const char *text);
void hgo_jni_set_unity_player(void *player);
void hgo_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *hgo_jni_key_event(int action, int keycode, int scancode);
void *hgo_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *hgo_jni_touch_event(int action, float x, float y);
void *hgo_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *hgo_jni_fmod_device(void);
void *hgo_jni_fmod_bytebuffer(void);
void *hgo_jni_fmod_pcm(void);
int hgo_jni_fmod_pcm_capacity(void);
void hgo_jni_fmod_set_buffer_size(int bytes);
int hgo_jni_fmod_should_run(void);
int hgo_audio_start(void *env);
void hgo_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int hgo_input_init(void);
void hgo_input_poll(void *env, void *player, unsigned long frame);
void hgo_input_close(void);
int hgo_input_exit_requested(void);
void hgo_input_request_exit(void);
void hgo_input_cancel_touch(void *env, void *player);
/* Gamepad pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int hgo_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void hgo_input_set_screen_size(int width, int height);

enum {
    HGO_KEY_CHARACTER,
    HGO_KEY_BACKSPACE,
    HGO_KEY_SHIFT,
    HGO_KEY_SPACE,
    HGO_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} hgo_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void hgo_input_keyboard_open(const char *initial, int character_limit);
void hgo_input_keyboard_set(const char *text);
void hgo_input_keyboard_hide(void);
int hgo_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const hgo_keyboard_key **keys,
                                size_t *key_count);
void hgo_jni_soft_input_text(const char *text);
void hgo_jni_soft_input_selection(int start, int length);
void hgo_jni_soft_input_visible(int visible);
void hgo_jni_soft_input_closed(int canceled);

/* The three arm64 objects, in load order. */
int hgo_load_modules(void);
void hgo_arm_frame_watchdog(void);
void hgo_watchdog_frame(void);

int hgo_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* HGO_H */
