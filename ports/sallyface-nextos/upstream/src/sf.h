/* sf.h -- shared declarations for the Sally Face port. */

#ifndef SF_H
#define SF_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char sf_gamedir[1024];
extern char sf_datadir[1024];   /* <gamedir>/assets */
extern char sf_apk[1024];       /* <gamedir>/assets -- a arvore do pacote */
/* A raiz que a Unity enxerga como "o pacote instalado" e o diretorio corrente
 * do processo: ela abre a base por caminho relativo a partir dai. */
extern char sf_apkdir[1024];    /* == sf_gamedir */
extern char sf_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int sf_log_level;    /* SF_LOGCAT   : mirror the game's own log     */
extern int sf_trace_jni;    /* SF_JNILOG   : every JNI call                */
extern int sf_trace_gl;     /* SF_GLLOG    : GL calls and shader sources   */
extern long sf_max_frames;  /* SF_FRAMES=N : stop after N frames           */
extern int sf_capture_mode; /* always zero; retained by the EGL abstraction */

void sf_bionic_init(void);
size_t sf_bionic_count(void);
void sf_pthread_init(void);
void sf_android_init(void);
void sf_egl_init(void);
void sf_jni_init(void);

void *sf_android_sym(const char *name);
void *sf_egl_sym(const char *name);
void *sf_gl_sym(const char *name);
void *sf_jni_sym(const char *name);
void *sf_jni_env(void);
void *sf_jni_vm(void);
void *sf_jni_activity(void);
void *sf_jni_native(const char *cls, const char *name);
void sf_jni_dump_natives(const char *filter);
void *sf_jret_obj(const char *cls);
void *sf_jret_class(const char *cls);
void *sf_jret_str(const char *text);
void sf_jni_set_unity_player(void *player);
void sf_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *sf_jni_key_event(int action, int keycode, int scancode);
void *sf_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *sf_jni_touch_event(int action, float x, float y);
void *sf_native_window(void);

/* One display contract is published before Unity's initJni and remains
 * coherent across EGL, ANativeWindow and the JNI display queries.  SDL-owned
 * backends call configure after measuring the real drawable; raw fbdev keeps
 * discovering /dev/fb0 exactly as before. */
void sf_window_configure(int width, int height, int lock_dimensions);
void sf_window_get_size(int *width, int *height);

/* UnityEngine.Video keeps its Android Surface/SurfaceTexture lifecycle in
 * Java while the decoder lives in libmediandk.  These narrow callbacks join
 * both halves without replacing the native VideoPlayer state machine. */
void *sf_jni_media_window(void *surface);
void *sf_jni_surface_for_window(void *window);
void sf_jni_surface_frame_available(void *listener,
                                    void *surface_texture_object);
int sf_gl_video_upload(unsigned texture, const unsigned char *rgba,
                       int width, int height, int stride);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *sf_jni_fmod_device(void);
void *sf_jni_fmod_bytebuffer(void);
void *sf_jni_fmod_pcm(void);
int sf_jni_fmod_pcm_capacity(void);
void sf_jni_fmod_set_buffer_size(int bytes);
int sf_jni_fmod_should_run(void);
int sf_audio_start(void *env);
void sf_audio_stop(void);

/* Saida do FMOD Studio NATIVO (libfmod.so): o output AudioTrack conversa por
 * JNI com org/fmod/AudioDevice, e cada bloco do mixer termina numa fila SDL. */
int sf_audio_device_init(int channels, int rate, int frames, int buffers);
void sf_audio_device_write(const void *data, int bytes);
void sf_audio_device_close(void);
int sf_audio_started(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int sf_input_init(void);
void sf_input_poll(void *env, void *player, unsigned long frame);
void sf_input_close(void);
int sf_input_exit_requested(void);
void sf_input_request_exit(void);

/* Observador de RAM: somente telemetria, nunca encerra o jogo. */
void sf_mem_guard_start(void);
/* Right-stick pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int sf_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void sf_input_set_screen_size(int width, int height);

enum {
    SF_KEY_CHARACTER,
    SF_KEY_BACKSPACE,
    SF_KEY_SHIFT,
    SF_KEY_SPACE,
    SF_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} sf_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void sf_input_keyboard_open(const char *initial, int character_limit);
void sf_input_keyboard_set(const char *text);
void sf_input_keyboard_hide(void);
int sf_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const sf_keyboard_key **keys,
                                size_t *key_count);
void sf_jni_soft_input_text(const char *text);
void sf_jni_soft_input_selection(int start, int length);
void sf_jni_soft_input_visible(int visible);
void sf_jni_soft_input_closed(int canceled);

/* PlayerPrefs do jogo, para o conserto do fim de fase (input.c). */
int sf_prefs_get_string(const char *key, char *out, size_t size);
int sf_prefs_set_string(const char *key, const char *value);

/* The three arm64 objects, in load order. */
int sf_load_modules(void);
void sf_arm_frame_watchdog(void);
void sf_watchdog_frame(void);

int sf_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* SF_H */
