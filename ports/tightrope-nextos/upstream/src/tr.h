/* tr.h -- shared declarations for the Tightrope Theatre port. */

#ifndef TR_H
#define TR_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char tr_gamedir[1024];
extern char tr_datadir[1024];   /* <gamedir>/assets  -- the extracted base APK */
extern char tr_home[1024];      /* <gamedir>/home    -- internal data dir      */

/* Debug switches, read once at start-up, all off by default. */
extern int tr_log_level;    /* TR_LOGCAT : mirror the game's own log        */
extern int tr_trace_jni;    /* TR_JNILOG : every JNI call                   */
extern int tr_trace_gl;     /* TR_GLLOG  : GL calls and shader sources      */
extern int tr_trace_plt;
extern int tr_trace_files;   /* TR_FILELOG: every file the game opens     */    /* TR_PLTLOG : hidden-PLT slot resolution       */
extern long tr_max_frames;  /* TR_FRAMES=N: stop after N swaps              */
extern int tr_capture_mode; /* TR_VM_CAPTURE: one-shot native extraction    */

void tr_bionic_init(void);
/* Takes pthread key 0 before any other library can; see bionic.c. */
void tr_reserve_zero_key(void);
size_t tr_bionic_count(void);
void tr_pthread_init(void);
void tr_android_init(void);
void tr_egl_init(void);
void tr_jni_init(void);

void *tr_android_sym(const char *name);
void *tr_egl_sym(const char *name);
void *tr_jni_sym(const char *name);
void *tr_protected_sym(const char *name);
void *tr_audio_sym(const char *name);

/* Handlers registered with tr_jni_bind() receive this context and return the
 * raw 64-bit slot the JNI call site expects. */
typedef struct jctx jctx;
typedef int64_t (*tr_jni_handler)(jctx *);

void *tr_jarg_obj(jctx *c);
int32_t tr_jarg_int(jctx *c);
int64_t tr_jarg_long(jctx *c);
const char *tr_jarg_str(jctx *c);
void *tr_jret_str(const char *s);
void *tr_jret_class(const char *s);
void *tr_jret_bytes(const void *p, int n);
int64_t tr_jret_float(float v);
const char *tr_jmethod_name(jctx *c);

/* The SDL/Lime Java surface, registered on top of the generic Android one. */
void tr_jni_bind_sdl(void);
/* The Surface object SDLActivity.getNativeSurface() answers with. */
void *tr_jni_surface(void);

void *tr_jni_env(void);
void *tr_jni_vm(void);
void *tr_jni_native(const char *cls, const char *name);
void *tr_jret_obj(const char *cls);
void tr_jni_bind(const char *cls, const char *name, const char *sig, void *fn);

/* The window liblime's SDL video driver draws into. */
void *tr_native_window(void);
int tr_screen_width(void);
int tr_screen_height(void);
unsigned long tr_egl_swap_count(void);

/* Native input: evdev pads are translated into the exact SDL Android
 * joystick/key callbacks that liblime already exports, so SDL's own device
 * and GameController layers run unchanged. */
int tr_input_init(void);
void tr_input_register(void);
void tr_input_poll(void);
void tr_input_close(void);
int tr_input_should_quit(void);
void tr_input_request_quit(void);
void tr_input_scene_loaded(const char *scene_name);
/* Window coordinates of the pad-driven finger; 0 when it should not be drawn. */
int tr_input_cursor(float *x, float *y);
int tr_input_cursor_pressed(void);
void tr_jni_bind_ads(void);
void tr_metronome_tick(void);
void tr_input_cursor_test(int dx, int dy, int press);

/* OpenSL ES surface used by SDL's openslES audio backend. */
void tr_audio_init(void);
void tr_audio_shutdown(void);

/* Module loading and the PairIP-protected ranges. */
int tr_load_modules(void);
int tr_apply_text_overlays(void);
int tr_patch_protected_plt(void);
int tr_pairip_capture(void);
void *tr_capture_execute_program(const char *name, void **args, long nargs);

int tr_iterate_mods(int (*cb)(void *, size_t, void *), void *data);
const char *tr_mod_at(const void *addr, void **base_out);

#endif /* TR_H */
