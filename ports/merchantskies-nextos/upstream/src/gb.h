/* Shared declarations for the Merchant of the Skies port. */

#ifndef ST_H
#define ST_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char st_gamedir[1024];
extern char st_datadir[1024];   /* <gamedir>/assets */
extern char st_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char st_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int st_log_level;    /* ST_LOGCAT   : mirror the game's own log     */
extern int st_trace_jni;    /* ST_JNILOG   : every JNI call                */
extern int st_trace_gl;     /* ST_GLLOG    : GL calls and shader sources   */
extern long st_max_frames;  /* ST_FRAMES=N : stop after N frames           */
extern int st_capture_mode; /* always zero; retained by the EGL abstraction */

void st_bionic_init(void);
size_t st_bionic_count(void);
void st_pthread_init(void);
void st_android_init(void);
void st_egl_init(void);
void st_jni_init(void);
int st_jni_netflix_selftest(void);

void *st_android_sym(const char *name);
void *st_egl_sym(const char *name);
void *st_gl_sym(const char *name);
void *st_jni_sym(const char *name);
void *st_jni_env(void);
void *st_jni_vm(void);
void *st_jni_activity(void);
void *st_jni_native(const char *cls, const char *name);
void *st_jret_obj(const char *cls);
void *st_jret_class(const char *cls);
void *st_jret_str(const char *text);
void st_jni_set_unity_player(void *player);
void st_jni_pump_callbacks(void);
void st_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *st_jni_key_event(int action, int keycode, int scancode);
void *st_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *st_jni_touch_event(int action, float x, float y);
void *st_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *st_jni_fmod_device(void);
void *st_jni_fmod_bytebuffer(void);
void *st_jni_fmod_pcm(void);
int st_jni_fmod_pcm_capacity(void);
void st_jni_fmod_set_buffer_size(int bytes);
int st_jni_fmod_should_run(void);
int st_audio_start(void *env);
void st_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int st_input_init(void);
void st_input_poll(void *env, void *player, unsigned long frame);
void st_input_close(void);
int st_input_exit_requested(void);
void st_input_request_exit(void);
/* Right-stick pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int st_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void st_input_set_screen_size(int width, int height);

enum {
    ST_KEY_CHARACTER,
    ST_KEY_BACKSPACE,
    ST_KEY_SHIFT,
    ST_KEY_SPACE,
    ST_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} st_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void st_input_keyboard_open(const char *initial, int character_limit);
void st_input_keyboard_set(const char *text);
void st_input_keyboard_hide(void);
int st_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const st_keyboard_key **keys,
                                size_t *key_count);
void st_jni_soft_input_text(const char *text);
void st_jni_soft_input_selection(int start, int length);
void st_jni_soft_input_visible(int visible);
void st_jni_soft_input_closed(int canceled);

/* PlayerPrefs do jogo, para o conserto do fim de fase (input.c). */
int st_prefs_get_string(const char *key, char *out, size_t size);
int st_prefs_set_string(const char *key, const char *value);

/* The three arm64 objects, in load order. */
int st_load_modules(void);
/* Opt-in physical-device diagnostics.  All are inert unless ST_PERSIST_LOG,
 * ST_STARTUP_TIMEOUT, ST_WATCHDOG or ST_HEARTBEAT is set. */
void st_diag_open_persistent_log(void);
void st_diag_watchdog_start(void);
void st_diag_phase(const char *phase);
void st_diag_render_ready(void);
void st_diag_frame_complete(unsigned long frame);
void st_diag_sync(void);

int st_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* ST_H */
