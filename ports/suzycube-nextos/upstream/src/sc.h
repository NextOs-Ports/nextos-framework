/* sc.h -- declaracoes compartilhadas do port do Suzy Cube. */

#ifndef SC_H
#define SC_H

#include <stddef.h>
#include <stdint.h>

/* Onde os dados do jogo vivem em execucao (argv[1] ou o cwd do launcher). */
extern char sc_gamedir[1024];
extern char sc_datadir[1024];   /* <gamedir>/assets */
extern char sc_apk[1024];       /* <gamedir>/assets -- o APK base extraido */
extern char sc_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Chaves de debug, lidas uma vez do ambiente no arranque e todas desligadas
 * por padrao, para o binario distribuido ficar quieto. */
extern int sc_log_level;    /* SC_LOGCAT  : espelha o log do proprio jogo  */
extern int sc_trace_jni;    /* SC_JNILOG  : toda chamada JNI               */
extern int sc_trace_gl;     /* SC_GLLOG   : chamadas GL e fontes de shader */
extern long sc_max_frames;  /* SC_FRAMES=N: para depois de N quadros       */
extern int sc_capture_mode; /* sempre zero; mantido pela abstracao EGL     */

void sc_bionic_init(void);
size_t sc_bionic_count(void);
void sc_pthread_init(void);
void sc_android_init(void);
void sc_egl_init(void);
void sc_jni_init(void);

void *sc_android_sym(const char *name);
void *sc_egl_sym(const char *name);
void *sc_gl_sym(const char *name);
void *sc_jni_sym(const char *name);
void *sc_jni_env(void);
void *sc_jni_vm(void);
void *sc_jni_activity(void);
void *sc_jni_native(const char *cls, const char *name);
void *sc_jret_obj(const char *cls);
void *sc_jret_class(const char *cls);
void *sc_jret_str(const char *text);
void sc_jni_set_unity_player(void *player);
void sc_jni_input_device_info(const char *name, int vendor, int product,
                              const char *descriptor);
extern int sc_jni_key_trace;
void *sc_jni_key_event(int action, int keycode, int scancode);
void *sc_jni_motion_event(float lx, float ly, float rx, float ry,
                          float lt, float rt, float hat_x, float hat_y);
void *sc_native_window(void);

/* O backend FMOD do Unity Android normalmente alimenta um AudioTrack a partir
 * de FMODAudioDevice.run().  O shim JNI mantem o contrato original
 * fmodGetInfo/fmodProcess e audio.c fornece a thread Java que falta pela
 * saida nativa do NextOS via SDL. */
void *sc_jni_fmod_device(void);
void *sc_jni_fmod_bytebuffer(void);
void *sc_jni_fmod_pcm(void);
int sc_jni_fmod_pcm_capacity(void);
void sc_jni_fmod_set_buffer_size(int bytes);
int sc_jni_fmod_should_run(void);
int sc_audio_start(void *env);
void sc_audio_stop(void);

/* Ponte controle Linux -> KeyEvent/MotionEvent do Android.  Os eventos sao
 * injetados na thread de render do Unity, exatamente como o UnityPlayer
 * encaminha a entrada da View no Android.  O Suzy Cube usa InControl de
 * fabrica: nada de cursor, nada de toque sintetico. */
int sc_input_init(void);
void sc_input_poll(void *env, void *player, unsigned long frame);
void sc_input_close(void);
int sc_input_exit_requested(void);
void sc_input_request_exit(void);
void sc_input_set_screen_size(int width, int height);

/* PlayerPrefs persistentes (jni.c). */
int sc_prefs_get_string(const char *key, char *out, size_t size);
int sc_prefs_set_string(const char *key, const char *value);

/* Os tres objetos arm64, na ordem de carga. */
int sc_load_modules(void);
void sc_arm_frame_watchdog(void);
void sc_watchdog_frame(void);

int sc_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* SC_H */
