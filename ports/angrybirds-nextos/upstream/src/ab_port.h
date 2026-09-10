/* ab_port.h — contrato interno do port Angry Birds Classic 8.0.3 (Mali-450).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_PORT_H
#define AB_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "nxloader.h"

typedef struct nxinput_context nxinput_context;
typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;

/* ---------- log persistente (O_SYNC na pasta do port) ---------- */
void ab_log_open(const char *gamedir);
void ab_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ab_log_close(void);
int ab_env_int(const char *name, int fallback);

/* ---------- caminhos ---------- */
const char *ab_gamedir(void);   /* pasta do executável */
const char *ab_filesdir(void);  /* "filesDir" do Android → saves/ */
const char *ab_cachedir(void);
const char *ab_apk_path(void);

/* ---------- APK / assets ---------- */
int ab_apk_open(const char *path);
void ab_apk_close(void);
/* Nome relativo a "assets/" dentro do APK. Devolve ponteiro estável e tamanho.
 * STORED = ponteiro direto pro mmap (zero cópia); DEFLATE = buffer inflado. */
const void *ab_apk_asset(const char *name, size_t *out_size, int *out_owned);
int ab_apk_has(const char *name);
/* Entrada crua do zip pelo caminho completo; o chamador libera com free(). */
void *ab_apk_entry(const char *zip_path, size_t *out_size);

/* ---------- providers de símbolo (bionic) ---------- */
nxloader_result ab_add_bionic_provider(nxloader_registry *registry);
nxloader_result ab_add_pthread_provider(nxloader_registry *registry);
nxloader_result ab_add_stdio_provider(nxloader_registry *registry);
nxloader_result ab_add_gl_provider(nxloader_registry *registry);
int ab_gl_provider_exports(void *handle);

/* ---------- fake JNI ---------- */
void *ab_jni_env(void);
void *ab_jni_vm(void);
void ab_jni_init(void);
/* callbacks que o falso-JNI precisa pedir ao main */
typedef struct ab_jni_hooks {
  int (*display_width)(void);
  int (*display_height)(void);
  int (*ppi)(void);
  void (*audio_create)(int64_t mixer, int rate, int channels, int bits, int bufbytes);
  int (*audio_start)(void);
  void (*audio_stop)(void);
  void (*quit_requested)(void);
  /* EGLWrapper: no APK ele é Java, então o contexto é NOSSO */
  int (*egl_current_context)(void);
  int (*egl_create_shared)(int handle);
  void (*egl_destroy_shared)(int handle);
  int (*egl_register_thread)(int handle);
  void (*egl_unregister_thread)(void);
} ab_jni_hooks;
void ab_jni_set_hooks(const ab_jni_hooks *hooks);

/* ---------- módulo do jogo ---------- */
extern nxloader_module *ab_guest;
uintptr_t ab_sym(const char *name);

/* ---------- adaptador Lua do controle (API privada desta lib 8.0.3) ---------- */
int ab_lua_control_install(void);
void ab_lua_control_set_stick(float x, float y, int blocked);
void ab_lua_control_press_launch(void);
void ab_lua_control_press_l1(void);
void ab_lua_control_press_r1(void);
void ab_lua_control_press_triangle(void);
void ab_lua_control_clear_buttons(void);
int ab_lua_control_is_gameplay(void);

/* ---------- áudio ---------- */
void ab_audio_shutdown(void);

/* ---------- input ---------- */
void ab_input_init(void);
void ab_input_pump(void);
void ab_input_draw_cursor(void);
void ab_input_shutdown(void);
int ab_input_quit_requested(void);
nxinput_context *ab_input_context(void);

/* ---------- framework universal ---------- */
int ab_framework_preflight(const char *game_dir);
int ab_framework_open_graphics(SDL_Window **window, SDL_GLContext *context,
                               int *width, int *height);
int ab_framework_publish_input(nxinput_context *input);
int ab_framework_require_ready(void);
int ab_framework_present(void);

#endif /* AB_PORT_H */
