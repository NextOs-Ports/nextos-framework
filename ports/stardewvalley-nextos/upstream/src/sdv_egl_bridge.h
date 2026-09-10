/*
 * Minimal SDL-backed graphics bridge for the Stardew Valley MonoGame port.
 *
 * SDL is deliberately opaque here: the implementation resolves every SDL
 * entry point at runtime, so users of this header do not need SDL headers or
 * an SDL link dependency.
 */

#ifndef SDV_EGL_BRIDGE_H
#define SDV_EGL_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Must be called on the main thread after SDL2 has been loaded globally.
 * Creates the fullscreen window and the port's single OpenGL ES context, then
 * releases the context so MonoGame's render thread can acquire it.
 */
int sdv_egl_init(void);

/* The bridge owns one context. Repeated calls return that same context. */
void *sdv_egl_create_context(void);

/* Surfaces are lightweight logical handles; the SDL window remains alive. */
void *sdv_egl_create_surface(void);

/* Pass NULL for both arguments to release the context on the calling thread. */
int sdv_egl_make_current(void *context, void *surface);
int sdv_egl_swap(void *surface);

/* Surface recreation does not affect the singleton context or SDL window. */
void sdv_egl_destroy_surface(void *surface);
void sdv_egl_destroy_context(void *context);

/* Full shutdown. This also destroys any still-live singleton context/window. */
void sdv_egl_destroy(void);

int sdv_egl_ready(void);
void *sdv_egl_window(void);
int sdv_egl_width(void);
int sdv_egl_height(void);

typedef struct SdvGamepadState {
    unsigned int buttons;
    short left_x;
    short left_y;
    short right_x;
    short right_y;
    short left_trigger;
    short right_trigger;
} SdvGamepadState;

/* Polling SDL do primeiro controle reconhecido. Os bits de buttons usam os
 * indices SDL_CONTROLLER_BUTTON_* (A=0 ... DPAD_RIGHT=14). */
int sdv_egl_poll_gamepad(SdvGamepadState *state);

/* Cursor auxiliar do analogico direito. Coordenadas em pixels da superficie,
 * com origem no canto superior esquerdo; o desenho ocorre antes do swap e nao
 * altera o cursor/foco gerenciado pelo jogo. */
void sdv_egl_set_right_cursor(float x, float y, int visible);

/* Resolve GL entry points for the exact context created by this bridge. */
void *sdv_egl_get_proc_address(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SDV_EGL_BRIDGE_H */
