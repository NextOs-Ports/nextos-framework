/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared declarations for the Gunbrick adapter. */

#ifndef GB_H
#define GB_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char gb_gamedir[1024];
extern char gb_datadir[1024];   /* <gamedir>/assets */
extern char gb_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char gb_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int gb_log_level;    /* GB_LOGCAT   : mirror the game's own log     */
extern int gb_trace_jni;    /* GB_JNILOG   : every JNI call                */
extern int gb_trace_gl;     /* GB_GLLOG    : GL calls and shader sources   */
extern long gb_max_frames;  /* GB_FRAMES=N : stop after N frames           */

void gb_bionic_init(void);
size_t gb_bionic_count(void);
void gb_pthread_init(void);
void gb_android_init(void);
void gb_egl_init(void);
void gb_jni_init(void);

void *gb_android_sym(const char *name);
void *gb_egl_sym(const char *name);
void *gb_gl_sym(const char *name);
void *gb_jni_sym(const char *name);
void *gb_jni_env(void);
void *gb_jni_vm(void);
void *gb_jni_activity(void);
void *gb_jni_native(const char *cls, const char *name);
void *gb_jret_obj(const char *cls);
void *gb_jret_class(const char *cls);
void *gb_jret_str(const char *text);
void gb_jni_set_unity_player(void *player);
void gb_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *gb_jni_key_event(int action, int keycode, int scancode);
void *gb_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *gb_jni_touch_event(int action, float x, float y);
void *gb_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *gb_jni_fmod_device(void);
void *gb_jni_fmod_bytebuffer(void);
void *gb_jni_fmod_pcm(void);
int gb_jni_fmod_pcm_capacity(void);
void gb_jni_fmod_set_buffer_size(int bytes);
int gb_jni_fmod_should_run(void);
int gb_audio_start(void *env);
void gb_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int gb_input_init(void);
void gb_input_poll(void *env, void *player, unsigned long frame);
void gb_input_close(void);
int gb_input_exit_requested(void);
void gb_input_request_exit(void);
/* Gunbrick currently publishes no cursor context; the drawing bridge retains
 * this read-only hook for a future evidence-backed menu adapter. */
int gb_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void gb_input_set_screen_size(int width, int height);

enum {
    GB_KEY_CHARACTER,
    GB_KEY_BACKSPACE,
    GB_KEY_SHIFT,
    GB_KEY_SPACE,
    GB_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} gb_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void gb_input_keyboard_open(const char *initial, int character_limit);
void gb_input_keyboard_set(const char *text);
void gb_input_keyboard_hide(void);
int gb_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const gb_keyboard_key **keys,
                                size_t *key_count);
void gb_jni_soft_input_text(const char *text);
void gb_jni_soft_input_selection(int start, int length);
void gb_jni_soft_input_visible(int visible);
void gb_jni_soft_input_closed(int canceled);

/* Owner-data PlayerPrefs bridge used by the JNI activity model. */
int gb_prefs_get_string(const char *key, char *out, size_t size);
int gb_prefs_set_string(const char *key, const char *value);

/* The three arm64 objects, in load order. */
int gb_load_modules(void);

int gb_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* GB_H */
