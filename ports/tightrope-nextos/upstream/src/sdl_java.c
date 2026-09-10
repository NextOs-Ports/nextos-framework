/*
 * sdl_java.c -- the org.libsdl.app / org.haxe.lime Java surface.
 *
 * liblime embeds SDL 2.26 built for Android, so its video, audio and event
 * code calls up into SDLActivity, SDLControllerManager and SDLAudioManager
 * static methods.  There is no Java here, so these are the native answers a
 * real Activity would have given, arranged so SDL's own state machine runs
 * unchanged: it still creates its window, asks for the native surface, sizes
 * itself from it and pumps its own event loop.
 *
 * Nothing in this file forces a state on SDL.  The one thing it does actively
 * is answer "no" to the Android-only features the box does not have (screen
 * keyboard, relative mouse, custom cursors, DeX, Chromebook), which are all
 * paths SDL already takes on ordinary phones.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_elf.h"
#include "tr.h"

/* The Surface SDL asks the Activity for.  ANativeWindow_fromSurface turns it
 * into the window android.c owns; the object itself only has to be a stable,
 * non-null identity. */
static void *surface_object;

void *tr_jni_surface(void)
{
    if (!surface_object)
        surface_object = tr_jret_obj("android/view/Surface");
    return surface_object;
}

/* --- SDLActivity --------------------------------------------------------- */

static int64_t j_getContext(jctx *c)
{
    (void)c;
    static void *activity;
    if (!activity)
        activity = tr_jret_obj("android/app/Activity");
    return (int64_t)(uintptr_t)activity;
}

static int64_t j_getNativeSurface(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)tr_jni_surface();
}

static int64_t j_false(jctx *c) { (void)c; return 0; }
static int64_t j_void(jctx *c) { (void)c; return 0; }

/* SDL asks the display for its DPI to decide the default window scale.  The
 * fbdev panel reports no physical size, so answer with the Android baseline
 * density, which is the "1x, no scaling" case SDL already handles. */
static int64_t j_getDisplayDPI(jctx *c)
{
    (void)c;
    static void *metrics;
    if (!metrics)
        metrics = tr_jret_obj("android/util/DisplayMetrics");
    return (int64_t)(uintptr_t)metrics;
}

static int64_t j_metrics_float(jctx *c)
{
    (void)c;
    return tr_jret_float(160.0f);
}

static int64_t j_metrics_int(jctx *c)
{
    const char *n = tr_jmethod_name(c);
    if (strcmp(n, "widthPixels") == 0)
        return tr_screen_width();
    if (strcmp(n, "heightPixels") == 0)
        return tr_screen_height();
    return 160;   /* densityDpi */
}

/* SDL routes a few commands (title, orientation, keyboard) through a Handler
 * on the UI thread.  Accepting them is what the Activity does; there is no
 * window manager here to act on any of them. */
static int64_t j_sendMessage(jctx *c)
{
    int32_t command = tr_jarg_int(c);
    int32_t param = tr_jarg_int(c);
    nx_log("SDLActivity.sendMessage(%d, %d)", command, param);
    return 1;
}

static int64_t j_setActivityTitle(jctx *c)
{
    const char *title = tr_jarg_str(c);
    nx_log("SDLActivity.setActivityTitle(\"%s\")", title ? title : "");
    return 1;
}

/* The frontend owns the display mode; the game asking to be rotated or
 * resized does not change a framebuffer that is already the panel. */
static int64_t j_setOrientation(jctx *c)
{
    int32_t w = tr_jarg_int(c);
    int32_t h = tr_jarg_int(c);
    nx_log("SDLActivity.setOrientation(%dx%d)", w, h);
    return 0;
}

static int64_t j_getCurrentOrientation(jctx *c)
{
    (void)c;
    /* SDL_ORIENTATION_LANDSCAPE; the panel is wider than it is tall. */
    return tr_screen_width() >= tr_screen_height() ? 1 : 3;
}

/* getManifestEnvironmentVariables() normally copies <meta-data> entries into
 * the process environment.  There are none to copy, and answering true is the
 * "done, nothing to do" result -- false makes SDL log a warning every start. */
static int64_t j_getManifestEnvironmentVariables(jctx *c)
{
    (void)c;
    return 1;
}

/* Runtime permissions: SDL blocks until the result callback fires.  Nothing
 * here is gated behind a permission, so grant it immediately through SDL's own
 * native callback rather than leaving the wait unanswered. */
static int64_t j_requestPermission(jctx *c)
{
    const char *permission = tr_jarg_str(c);
    int32_t request = tr_jarg_int(c);
    void *fn = tr_jni_native("org/libsdl/app/SDLActivity",
                             "nativePermissionResult");
    nx_log("SDLActivity.requestPermission(\"%s\", %d) -> granted",
           permission ? permission : "", request);
    if (fn)
        ((void (*)(void *, void *, int32_t, uint8_t))fn)(
            tr_jni_env(), tr_jret_class("org/libsdl/app/SDLActivity"),
            request, 1);
    return 0;
}

static int64_t j_getAudioDevices(jctx *c)
{
    (void)c;
    /* An empty int[] is "no addressable devices"; SDL then uses the default
     * output, which is the only one on this box. */
    return (int64_t)(uintptr_t)tr_jret_bytes(NULL, 0);
}

static int64_t j_clipboardGetText(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)tr_jret_str("");
}

static int64_t j_openAPKExpansionInputStream(jctx *c)
{
    (void)c;
    /* There is no expansion file: everything ships inside the base APK, and
     * SDL treats null as "fall through to the normal asset path". */
    return 0;
}

/* --- SDLControllerManager ------------------------------------------------ */

static int64_t j_pollInputDevices(jctx *c)
{
    (void)c;
    /* This is where Android hands SDL the devices it found, and the first
     * point at which SDL's joystick subsystem is ready to receive them. */
    tr_input_register();
    return 0;
}

void tr_jni_bind_sdl(void)
{
    static const struct {
        const char *cls, *name, *sig;
        tr_jni_handler fn;
    } binds[] = {
        /* --- SDLActivity, video and window --- */
        { "org/libsdl/app/SDLActivity", "getContext",
          "()Landroid/content/Context;", j_getContext },
        { "org/libsdl/app/SDLActivity", "getNativeSurface",
          "()Landroid/view/Surface;", j_getNativeSurface },
        { "org/libsdl/app/SDLActivity", "setActivityTitle",
          "(Ljava/lang/String;)Z", j_setActivityTitle },
        { "org/libsdl/app/SDLActivity", "setWindowStyle", "(Z)V", j_void },
        { "org/libsdl/app/SDLActivity", "setOrientation",
          "(IIZLjava/lang/String;)V", j_setOrientation },
        { "org/libsdl/app/SDLActivity", "getCurrentOrientation", "()I",
          j_getCurrentOrientation },
        { "org/libsdl/app/SDLActivity", "minimizeWindow", "()V", j_void },
        { "org/libsdl/app/SDLActivity", "shouldMinimizeOnFocusLoss", "()Z",
          j_false },
        { "org/libsdl/app/SDLActivity", "setSurfaceViewFormat", "(I)V",
          j_void },
        { "org/libsdl/app/SDLActivity", "getDisplayDPI",
          "()Landroid/util/DisplayMetrics;", j_getDisplayDPI },
        { "android/util/DisplayMetrics", "xdpi", "F", j_metrics_float },
        { "android/util/DisplayMetrics", "ydpi", "F", j_metrics_float },
        { "android/util/DisplayMetrics", "densityDpi", "I", j_metrics_int },
        { "android/util/DisplayMetrics", "widthPixels", "I", j_metrics_int },
        { "android/util/DisplayMetrics", "heightPixels", "I", j_metrics_int },
        { "org/libsdl/app/SDLActivity", "sendMessage", "(II)Z", j_sendMessage },

        /* --- SDLActivity, platform capability questions --- */
        { "org/libsdl/app/SDLActivity", "isAndroidTV", "()Z", j_false },
        { "org/libsdl/app/SDLActivity", "isChromebook", "()Z", j_false },
        { "org/libsdl/app/SDLActivity", "isDeXMode", "()Z", j_false },
        { "org/libsdl/app/SDLActivity", "isTablet", "()Z", j_false },
        { "org/libsdl/app/SDLActivity", "manualBackButton", "()Z", j_false },
        { "org/libsdl/app/SDLActivity", "getManifestEnvironmentVariables",
          "()Z", j_getManifestEnvironmentVariables },
        { "org/libsdl/app/SDLActivity", "initTouch", "()V", j_void },

        /* --- SDLActivity, input methods the box does not have --- */
        { "org/libsdl/app/SDLActivity", "isScreenKeyboardShown", "()Z",
          j_false },
        { "org/libsdl/app/SDLActivity", "showTextInput", "(IIII)Z", j_false },
        { "org/libsdl/app/SDLActivity", "supportsRelativeMouse", "()Z",
          j_false },
        { "org/libsdl/app/SDLActivity", "setRelativeMouseEnabled", "(Z)Z",
          j_false },
        { "org/libsdl/app/SDLActivity", "createCustomCursor", "([IIIII)I",
          j_false },
        { "org/libsdl/app/SDLActivity", "setCustomCursor", "(I)Z", j_false },
        { "org/libsdl/app/SDLActivity", "setSystemCursor", "(I)Z", j_false },

        /* --- SDLActivity, services --- */
        { "org/libsdl/app/SDLActivity", "clipboardHasText", "()Z", j_false },
        { "org/libsdl/app/SDLActivity", "clipboardGetText",
          "()Ljava/lang/String;", j_clipboardGetText },
        { "org/libsdl/app/SDLActivity", "clipboardSetText",
          "(Ljava/lang/String;)V", j_void },
        { "org/libsdl/app/SDLActivity", "openURL", "(Ljava/lang/String;)I",
          j_false },
        { "org/libsdl/app/SDLActivity", "showToast",
          "(Ljava/lang/String;IIII)I", j_false },
        { "org/libsdl/app/SDLActivity", "requestPermission",
          "(Ljava/lang/String;I)V", j_requestPermission },
        { "org/libsdl/app/SDLActivity", "openAPKExpansionInputStream",
          "(Ljava/lang/String;)Ljava/io/InputStream;",
          j_openAPKExpansionInputStream },

        /* --- SDLAudioManager --- */
        { "org/libsdl/app/SDLAudioManager", "getAudioOutputDevices", "()[I",
          j_getAudioDevices },
        { "org/libsdl/app/SDLAudioManager", "getAudioInputDevices", "()[I",
          j_getAudioDevices },
        { "org/libsdl/app/SDLAudioManager", "audioSetThreadPriority", "(ZI)V",
          j_void },

        /* --- SDLControllerManager --- */
        { "org/libsdl/app/SDLControllerManager", "pollInputDevices", "()V",
          j_pollInputDevices },
        { "org/libsdl/app/SDLControllerManager", "pollHapticDevices", "()V",
          j_void },
        { "org/libsdl/app/SDLControllerManager", "hapticRun", "(IFI)V",
          j_void },
        { "org/libsdl/app/SDLControllerManager", "hapticRumble", "(IFFI)V",
          j_void },
        { "org/libsdl/app/SDLControllerManager", "hapticStop", "(I)V",
          j_void },
    };

    for (size_t i = 0; i < sizeof binds / sizeof *binds; i++)
        tr_jni_bind(binds[i].cls, binds[i].name, binds[i].sig,
                    (void *)binds[i].fn);
    nx_log("jni: SDL/Lime surface bound (%zu entries)",
           sizeof binds / sizeof *binds);
}
