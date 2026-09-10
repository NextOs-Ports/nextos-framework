/*
 * egl_shim.h -- EGL wrapper backed by SDL2
 */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

#define EGL_TRUE 1
#define EGL_FALSE 0
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ACCESS 0x3002
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_CURRENT_SURFACE 0x3007
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_BAD_PARAMETER 0x300C
#define EGL_BAD_SURFACE 0x300D
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DONT_CARE ((EGLint)-1)

/* EGL 1.4 attributes consumed by the Titan Souls guest. */
#define EGL_BUFFER_SIZE 0x3020
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_CAVEAT 0x3027
#define EGL_CONFIG_ID 0x3028
#define EGL_LEVEL 0x3029
#define EGL_MAX_PBUFFER_HEIGHT 0x302A
#define EGL_MAX_PBUFFER_PIXELS 0x302B
#define EGL_MAX_PBUFFER_WIDTH 0x302C
#define EGL_NATIVE_RENDERABLE 0x302D
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_NATIVE_VISUAL_TYPE 0x302F
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_TRANSPARENT_TYPE 0x3034
#define EGL_TRANSPARENT_BLUE_VALUE 0x3035
#define EGL_TRANSPARENT_GREEN_VALUE 0x3036
#define EGL_TRANSPARENT_RED_VALUE 0x3037
#define EGL_NONE 0x3038
#define EGL_BIND_TO_TEXTURE_RGB 0x3039
#define EGL_BIND_TO_TEXTURE_RGBA 0x303A
#define EGL_MIN_SWAP_INTERVAL 0x303B
#define EGL_MAX_SWAP_INTERVAL 0x303C
#define EGL_LUMINANCE_SIZE 0x303D
#define EGL_ALPHA_MASK_SIZE 0x303E
#define EGL_COLOR_BUFFER_TYPE 0x303F
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_CONFORMANT 0x3042
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_DRAW 0x3059
#define EGL_READ 0x305A
#define EGL_BACK_BUFFER 0x3084
#define EGL_SINGLE_BUFFER 0x3085
#define EGL_RENDER_BUFFER 0x3086
#define EGL_CLIENT_APIS 0x308D
#define EGL_RGB_BUFFER 0x308E
#define EGL_SWAP_BEHAVIOR 0x3093
#define EGL_BUFFER_PRESERVED 0x3094
#define EGL_BUFFER_DESTROYED 0x3095

#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_SLOW_CONFIG 0x3050
#define EGL_NON_CONFORMANT_CONFIG 0x3051
#define EGL_TRANSPARENT_RGB 0x3052
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id);
EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean egl_shim_Terminate(EGLDisplay dpy);
EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config);
EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list);
EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list);
EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list);
EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx);
EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value);
EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value);
EGLint egl_shim_GetError(void);
void *egl_shim_GetProcAddress(const char *procname);
EGLBoolean egl_shim_BindAPI(unsigned int api);
const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name);
EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval);
EGLContext egl_shim_GetCurrentContext(void);
EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw);
EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v);

// Access SDL window from outside (for ANativeWindow shim)
struct SDL_Window *egl_shim_get_window(void);

// Pre-create the SDL window+context from main thread
void egl_shim_create_window(void);

// Mutex hooks — call from pthread_mutex_lock/unlock wrappers
// to detect outermost EndCriticalSectionGL and release GL
void egl_shim_on_mutex_post_lock(void *mutex_id);
void egl_shim_on_mutex_pre_unlock(void *mutex_id);
int egl_shim_ensure_current(void);

#endif
