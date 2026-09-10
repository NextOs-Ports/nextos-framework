/* rcr.h -- shared declarations for the Retro City Rampage DX port. */

#ifndef RCR_H
#define RCR_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char rcr_gamedir[1024];
extern char rcr_datadir[1024];   /* <gamedir>/gamedata -- the APK's assets/ */
extern char rcr_home[1024];      /* <gamedir>/home     -- internal data dir */

/* SDL_AUDIODRIVER as the launcher's environment had it, taken out of the way
 * before the game's own SDL can choke on it (see main.c); audio.c gives it
 * back to the system SDL when it really names one of its drivers. */
extern const char *rcr_sys_audiodriver;

/* Debug switches, read once at start-up, all off by default. */
extern int rcr_log_level;     /* RCR_LOGCAT : mirror the game's own log      */
extern int rcr_trace_jni;     /* RCR_JNILOG : every JNI call                 */
extern int rcr_trace_gl;      /* RCR_GLLOG  : GL calls and shader sources    */
extern int rcr_trace_plt;
extern int rcr_trace_files;   /* RCR_FILELOG: every file the game opens      */
extern long rcr_max_frames;   /* RCR_FRAMES=N: stop after N swaps            */
extern int rcr_verbose_audio; /* RCR_AUDIOLOG: periodic audio queue report   */

void rcr_bionic_init(void);
/* Takes pthread key 0 before any other library can; see bionic.c. */
void rcr_reserve_zero_key(void);
size_t rcr_bionic_count(void);
void rcr_pthread_init(void);
void rcr_android_init(void);
void rcr_egl_init(void);
void rcr_jni_init(void);

void *rcr_android_sym(const char *name);
void *rcr_egl_sym(const char *name);
void *rcr_jni_sym(const char *name);
void *rcr_audio_sym(const char *name);

/* Handlers registered with rcr_jni_bind() receive this context and return the
 * raw 64-bit slot the JNI call site expects. */
typedef struct jctx jctx;
typedef int64_t (*rcr_jni_handler)(jctx *);

void *rcr_jarg_obj(jctx *c);
int32_t rcr_jarg_int(jctx *c);
int64_t rcr_jarg_long(jctx *c);
const char *rcr_jarg_str(jctx *c);
void *rcr_jarray_data(void *obj, int *len);
void *rcr_jret_str(const char *s);
void *rcr_jret_class(const char *s);
void *rcr_jret_bytes(const void *p, int n);
int64_t rcr_jret_float(float v);
const char *rcr_jmethod_name(jctx *c);

/* The SDL/vblank Java surface, registered on top of the generic Android one. */
void rcr_jni_bind_sdl(void);
/* The Surface object SDLActivity.getNativeSurface() answers with. */
void *rcr_jni_surface(void);

void *rcr_jni_env(void);
void *rcr_jni_vm(void);
void *rcr_jni_native(const char *cls, const char *name);
void *rcr_jret_obj(const char *cls);
void rcr_jni_bind(const char *cls, const char *name, const char *sig, void *fn);

/* The window the game's SDL video driver draws into. */
void *rcr_native_window(void);
int rcr_screen_width(void);
int rcr_screen_height(void);
unsigned long rcr_egl_swap_count(void);

/* Native input: evdev pads are translated into the exact SDL Android
 * joystick/key callbacks the game already exports, so SDL's own device and
 * GameController layers run unchanged. */
int rcr_input_init(void);
void rcr_input_register(void);
void rcr_input_poll(void);
void rcr_input_close(void);
int rcr_input_should_quit(void);
void rcr_input_request_quit(void);
/* Kept so the shared modules that call it stay unchanged; this game paces
 * itself from inside SDL_main. */
void rcr_metronome_tick(void);

/* The AudioTrack surface the game's SDL audio driver writes into. */
int rcr_audio_open(int rate, int is16bit, int stereo, int frames);
void rcr_audio_write(const void *data, size_t bytes);
void rcr_audio_close(void);
unsigned long rcr_audio_writes(void);

int rcr_load_modules(void);
int rcr_iterate_mods(int (*cb)(void *, size_t, void *), void *data);
const char *rcr_mod_at(const void *addr, void **base_out);

#endif /* RCR_H */
