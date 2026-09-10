/* Shared declarations for the Freedom Planet 2 port. */

#ifndef FP2_H
#define FP2_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char fp2_gamedir[1024];
extern char fp2_datadir[1024];   /* <gamedir>/assets */
extern char fp2_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char fp2_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
#ifdef FP2_RELEASE_BUILD
#define fp2_log_level 0
#define fp2_trace_jni 0
#define fp2_trace_gl 0
#else
extern int fp2_log_level;    /* FP2_LOGCAT   : mirror the game's own log     */
extern int fp2_trace_jni;    /* FP2_JNILOG   : every JNI call                */
extern int fp2_trace_gl;     /* FP2_GLLOG    : GL calls and shader sources   */
#endif
extern long fp2_max_frames;  /* FP2_FRAMES=N : stop after N frames           */
extern int fp2_capture_mode; /* always zero; retained by the EGL abstraction */

void fp2_bionic_init(void);
size_t fp2_bionic_count(void);
void fp2_pthread_init(void);
void fp2_android_init(void);
void fp2_egl_init(void);
void fp2_jni_init(void);
#ifndef FP2_RELEASE_BUILD
int fp2_jni_netflix_selftest(void);
#endif

void *fp2_android_sym(const char *name);
void *fp2_egl_sym(const char *name);
void *fp2_gl_sym(const char *name);
void *fp2_jni_sym(const char *name);
void *fp2_jni_env(void);
void *fp2_jni_vm(void);
void *fp2_jni_activity(void);
void *fp2_jni_native(const char *cls, const char *name);
const char *fp2_jni_native_sig(const char *cls, const char *name);
void *fp2_jret_obj(const char *cls);
void *fp2_jret_class(const char *cls);
void *fp2_jret_str(const char *text);
void fp2_jni_set_unity_player(void *player);
void fp2_jni_pump_callbacks(void);
void fp2_jni_finish_callbacks(void);
void fp2_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *fp2_jni_key_event(int action, int keycode, int scancode);
void *fp2_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *fp2_jni_touch_event(int action, float x, float y);
void *fp2_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *fp2_jni_fmod_device(void);
void *fp2_jni_fmod_bytebuffer(void);
void *fp2_jni_fmod_pcm(void);
int fp2_jni_fmod_pcm_capacity(void);
void fp2_jni_fmod_set_buffer_size(int bytes);
int fp2_jni_fmod_should_run(void);
int fp2_audio_start(void *env);
void fp2_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
/* Pré-init: lê NEXTOSCONTROLLERS.gptk (owner/default, FACE_LAYOUT) UMA vez,
 * antes de qualquer SDL_Init (vídeo incluso). */
int fp2_input_preinit(void);
int fp2_input_init(void);
/* 1 quando o runtime vivo de controles falhou de forma terminal (ACK). */
int fp2_input_fatal(void);
void fp2_input_poll(void *env, void *player, unsigned long frame);
void fp2_input_close(void);
int fp2_input_exit_requested(void);
void fp2_input_request_exit(void);
/* Right-stick pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int fp2_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void fp2_input_set_screen_size(int width, int height);
/* Select the game's own hidden CanvasGroup state for its Android-only
 * joystick/buttons.  The normal HUD and physical-controller input remain. */
void fp2_hide_mobile_controls(void);

enum {
    FP2_KEY_CHARACTER,
    FP2_KEY_BACKSPACE,
    FP2_KEY_SHIFT,
    FP2_KEY_SPACE,
    FP2_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} fp2_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void fp2_input_keyboard_open(const char *initial, int character_limit);
void fp2_input_keyboard_set(const char *text);
void fp2_input_keyboard_hide(void);
int fp2_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const fp2_keyboard_key **keys,
                                size_t *key_count);
void fp2_jni_soft_input_text(const char *text);
void fp2_jni_soft_input_selection(int start, int length);
void fp2_jni_soft_input_visible(int visible);
void fp2_jni_soft_input_closed(int canceled);

/* PlayerPrefs do jogo, para o conserto do fim de fase (input.c). */
int fp2_prefs_get_string(const char *key, char *out, size_t size);
int fp2_prefs_set_string(const char *key, const char *value);

/* The three arm64 objects, in load order. */
int fp2_load_modules(void);
/* Opt-in physical-device diagnostics.  All are inert unless FP2_PERSIST_LOG,
 * FP2_STARTUP_TIMEOUT, FP2_WATCHDOG or FP2_HEARTBEAT is set. */
#ifdef FP2_RELEASE_BUILD
static inline void fp2_diag_open_persistent_log(void) {}
static inline void fp2_diag_watchdog_start(void) {}
static inline void fp2_diag_phase(const char *phase) { (void)phase; }
static inline void fp2_diag_render_ready(void) {}
static inline void fp2_diag_frame_complete(unsigned long frame) { (void)frame; }
static inline void fp2_diag_sync(void) {}
#else
void fp2_diag_open_persistent_log(void);
void fp2_diag_watchdog_start(void);
void fp2_diag_phase(const char *phase);
void fp2_diag_render_ready(void);
void fp2_diag_frame_complete(unsigned long frame);
void fp2_diag_sync(void);
#endif

int fp2_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* FP2_H */
