/* pf2.h -- shared declarations for the Prizefighters 2 port. */

#ifndef PF2_H
#define PF2_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char pf2_gamedir[1024];
extern char pf2_datadir[1024];   /* <gamedir>/assets */
extern char pf2_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char pf2_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int pf2_log_level;    /* PF2_LOGCAT   : mirror the game's own log     */
extern int pf2_trace_jni;    /* PF2_JNILOG   : every JNI call                */
extern int pf2_trace_gl;     /* PF2_GLLOG    : GL calls and shader sources   */
extern int pf2_trace_plt;    /* PF2_PLTLOG   : hidden-PLT slot resolution    */
extern long pf2_max_frames;  /* PF2_FRAMES=N : stop after N frames           */
extern int pf2_capture_mode; /* PF2_VM_CAPTURE: one-shot native extraction   */

void pf2_bionic_init(void);
size_t pf2_bionic_count(void);
void pf2_pthread_init(void);
void pf2_android_init(void);
void pf2_egl_init(void);
void pf2_jni_init(void);

void *pf2_android_sym(const char *name);
void *pf2_egl_sym(const char *name);
void *pf2_gl_sym(const char *name);
void *pf2_jni_sym(const char *name);
void *pf2_protected_sym(const char *name);
void *pf2_jni_env(void);
void *pf2_jni_vm(void);
void *pf2_jni_native(const char *cls, const char *name);
void *pf2_jret_obj(const char *cls);
void pf2_jni_set_unity_player(void *player);
void pf2_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *pf2_jni_key_event(int action, int keycode, int scancode);
void *pf2_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *pf2_jni_touch_event(int action, float x, float y);
void *pf2_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *pf2_jni_fmod_device(void);
void *pf2_jni_fmod_bytebuffer(void);
void *pf2_jni_fmod_pcm(void);
int pf2_jni_fmod_pcm_capacity(void);
void pf2_jni_fmod_set_buffer_size(int bytes);
int pf2_jni_fmod_should_run(void);
int pf2_audio_start(void *env);
void pf2_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int pf2_input_init(void);
void pf2_input_poll(void *env, void *player, unsigned long frame);
void pf2_input_close(void);
/* Right-stick pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int pf2_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void pf2_input_set_screen_size(int width, int height);

enum {
    PF2_KEY_CHARACTER,
    PF2_KEY_BACKSPACE,
    PF2_KEY_SHIFT,
    PF2_KEY_SPACE,
    PF2_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} pf2_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void pf2_input_keyboard_open(const char *initial, int character_limit);
void pf2_input_keyboard_set(const char *text);
void pf2_input_keyboard_hide(void);
int pf2_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const pf2_keyboard_key **keys,
                                size_t *key_count);
void pf2_jni_soft_input_text(const char *text);
void pf2_jni_soft_input_selection(int start, int length);
void pf2_jni_soft_input_visible(int visible);
void pf2_jni_soft_input_closed(int canceled);

/* The three arm64 objects, in load order. */
int pf2_load_modules(void);
/* Restore the version-pinned protected text and hidden PLT slots without
 * loading the Android licence/bootstrap library. */
int pf2_apply_text_overlays(void);
int pf2_neutralise_encrypted_data(void);
void pf2_arm_frame_watchdog(void);
void pf2_watchdog_frame(void);
int pf2_patch_protected_plt(void);

/* Analysis-only, one-shot recovery of PairIP-protected writable ranges.  This
 * runs solely on the arm64 NextOS target and exits before the game runtime.
 * The shipped path never loads libpairipcore. */
int pf2_pairip_capture(void);
void *pf2_capture_execute_program(const char *name, void **args, long nargs);

int pf2_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* PF2_H */
