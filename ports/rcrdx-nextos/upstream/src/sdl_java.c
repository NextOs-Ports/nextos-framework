/*
 * sdl_java.c -- the org.libsdl.app and com.vblank Java surface.
 *
 * libRCRDX.so carries an old Android build of SDL2 (2.0.4/2.0.5 era) linked
 * statically, so its video, audio, event and joystick code calls up into
 * static methods on ONE class, org.libsdl.app.SDLActivity -- there is no
 * SDLAudioManager and no SDLControllerManager in this vintage.  On top of that
 * the game itself calls three methods on com.vblank.RCRDX.Activity, and the
 * objects those return (com.vblank.Social, com.vblank.Cloud).
 *
 * The exact list below was read out of the binary, not guessed: every
 * GetStaticMethodID/GetMethodID site was resolved back to its name and
 * signature string.
 *
 * Nothing here forces a state on SDL.  It answers the way a real Activity
 * would, so SDL still creates its own window, asks for the native surface,
 * sizes itself from it and pumps its own event loop; and no stub ever returns
 * an error, so the game's flow always continues.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_elf.h"
#include "rcr.h"

/* The Surface SDL asks the Activity for.  ANativeWindow_fromSurface turns it
 * into the window android.c owns; the object itself only has to be a stable,
 * non-null identity. */
static void *surface_object;

void *rcr_jni_surface(void)
{
    if (!surface_object)
        surface_object = rcr_jret_obj("android/view/Surface");
    return surface_object;
}

/* --- SDLActivity: window and platform ------------------------------------ */

static int64_t j_getContext(jctx *c)
{
    (void)c;
    static void *activity;
    if (!activity)
        activity = rcr_jret_obj("android/app/Activity");
    return (int64_t)(uintptr_t)activity;
}

static int64_t j_getNativeSurface(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)rcr_jni_surface();
}

static int64_t j_false(jctx *c) { (void)c; return 0; }
static int64_t j_true(jctx *c) { (void)c; return 1; }
static int64_t j_void(jctx *c) { (void)c; return 0; }

/* The fbdev panel reports no physical size; 160 dpi is Android's "1x, no
 * scaling" baseline, which is the path SDL already takes on such a device. */
static int64_t j_dpi(jctx *c)
{
    (void)c;
    return rcr_jret_float(160.0f);
}

static int64_t j_setActivityTitle(jctx *c)
{
    const char *title = rcr_jarg_str(c);
    nx_log("SDLActivity.setActivityTitle(\"%s\")", title ? title : "");
    return 1;
}

/* SDL routes a few commands (title, orientation, keyboard) through a Handler
 * on the UI thread.  Accepting them is what the Activity does; there is no
 * window manager here to act on any of them. */
static int64_t j_sendMessage(jctx *c)
{
    int32_t command = rcr_jarg_int(c);
    int32_t param = rcr_jarg_int(c);
    nx_log("SDLActivity.sendMessage(%d, %d)", command, param);
    return 1;
}

static int64_t j_getSystemService(jctx *c)
{
    const char *name = rcr_jarg_str(c);
    /* The only service this SDL asks for is the clipboard; a plain object is
     * enough, the calls on it are answered below. */
    return (int64_t)(uintptr_t)rcr_jret_obj(name && strcmp(name, "clipboard") == 0
                                            ? "android/content/ClipboardManager"
                                            : "java/lang/Object");
}

static int64_t j_clipboardGetText(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)rcr_jret_str("");
}

static int64_t j_openAPKExpansionInputStream(jctx *c)
{
    (void)c;
    /* No expansion file: everything ships inside the base APK, and SDL treats
     * null as "fall through to the normal asset path". */
    return 0;
}

/* messageboxShowMessageBox is only reached by SDL_ShowMessageBox, which this
 * game does not use on its own path; -1 means "no button chosen", the same
 * answer a dismissed dialog gives. */
static int64_t j_messagebox(jctx *c)
{
    (void)c;
    return -1;
}

/* Android hands SDL the devices it found here, and this is the first point at
 * which SDL's joystick subsystem is ready to receive them. */
static int64_t j_pollInputDevices(jctx *c)
{
    (void)c;
    rcr_input_register();
    return 0;
}

/* SDL enumerates Android's input devices before it trusts pollInputDevices;
 * an empty list is the honest answer -- our single pad is announced through
 * nativeAddJoystick, exactly as the Java code would have done. */
static int64_t j_inputGetInputDeviceIds(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)rcr_jret_bytes(NULL, 0);
}

/* --- SDLActivity: audio (this SDL's driver is the Java AudioTrack one) ---- */

static int64_t j_audioInit(jctx *c)
{
    int32_t rate = rcr_jarg_int(c);
    int32_t is16 = rcr_jarg_int(c);
    int32_t stereo = rcr_jarg_int(c);
    int32_t frames = rcr_jarg_int(c);
    return rcr_audio_open(rate, is16, stereo, frames);
}

static int64_t j_audioWriteShort(jctx *c)
{
    int len = 0;
    void *data = rcr_jarray_data(rcr_jarg_obj(c), &len);
    rcr_audio_write(data, (size_t)len * sizeof(int16_t));
    return 0;
}

static int64_t j_audioWriteByte(jctx *c)
{
    int len = 0;
    void *data = rcr_jarray_data(rcr_jarg_obj(c), &len);
    rcr_audio_write(data, (size_t)len);
    return 0;
}

static int64_t j_audioQuit(jctx *c)
{
    (void)c;
    rcr_audio_close();
    return 0;
}

/* --- com.vblank: language, achievements, leaderboards, cloud -------------- */

/* The game asks the Activity which language to run in.  0 is English, and
 * English is the house rule -- never Japanese. */
static int64_t j_getLanguage(jctx *c)
{
    (void)c;
    nx_log("RCRDX.Activity.getLanguage() -> 0 (English)");
    return 0;
}

static int64_t j_initSocial(jctx *c)
{
    (void)c;
    static void *social;
    if (!social)
        social = rcr_jret_obj("com/vblank/Social");
    return (int64_t)(uintptr_t)social;
}

static int64_t j_initCloud(jctx *c)
{
    (void)c;
    static void *cloud;
    if (!cloud)
        cloud = rcr_jret_obj("com/vblank/Cloud");
    return (int64_t)(uintptr_t)cloud;
}

/* Play Games / GameCircle are not present.  Every answer here is a calm "yes,
 * done, nothing happened" -- never an error, so the game's flow continues:
 * isAvailable/isSignedIn say no service, and the actions accept and return.
 * Cloud.read returning an empty array is "no cloud save", which is exactly the
 * state of a player who never signed in; the real save is the local file the
 * game writes through stdio. */
static int64_t j_cloud_read(jctx *c)
{
    const char *key = rcr_jarg_str(c);
    nx_log("Cloud.read(\"%s\") -> vazio (sem serviço de nuvem)", key ? key : "");
    return (int64_t)(uintptr_t)rcr_jret_bytes(NULL, 0);
}

static int64_t j_cloud_write(jctx *c)
{
    (void)rcr_jarg_obj(c);
    const char *key = rcr_jarg_str(c);
    nx_log("Cloud.write(\"%s\") aceito", key ? key : "");
    return 0;
}

static int64_t j_unlockAchievement(jctx *c)
{
    const char *id = rcr_jarg_str(c);
    nx_log("Social.unlockAchievement(\"%s\") aceito", id ? id : "");
    return 0;
}

static int64_t j_updateLeaderboard(jctx *c)
{
    const char *id = rcr_jarg_str(c);
    int64_t score = rcr_jarg_long(c);
    nx_log("Social.updateLeaderboard(\"%s\", %lld) aceito", id ? id : "",
           (long long)score);
    return 0;
}

/* secondsToScore(double) -> long: the service's own encoding of a time.  With
 * no service, milliseconds is the honest identity conversion and keeps the
 * game's own comparisons monotonic. */
static int64_t j_secondsToScore(jctx *c)
{
    int64_t bits = rcr_jarg_long(c);
    double seconds;
    memcpy(&seconds, &bits, sizeof seconds);
    return (int64_t)(seconds * 1000.0);
}

void rcr_jni_bind_sdl(void)
{
    static const struct {
        const char *cls, *name, *sig;
        rcr_jni_handler fn;
    } binds[] = {
        /* --- SDLActivity: window --- */
        { "org/libsdl/app/SDLActivity", "getContext",
          "()Landroid/content/Context;", j_getContext },
        { "org/libsdl/app/SDLActivity", "getNativeSurface",
          "()Landroid/view/Surface;", j_getNativeSurface },
        { "org/libsdl/app/SDLActivity", "setActivityTitle",
          "(Ljava/lang/String;)Z", j_setActivityTitle },
        { "org/libsdl/app/SDLActivity", "sendMessage", "(II)Z", j_sendMessage },
        { "org/libsdl/app/SDLActivity", "getDisplayHDPI", "()F", j_dpi },
        { "org/libsdl/app/SDLActivity", "getDisplayVDPI", "()F", j_dpi },

        /* --- SDLActivity: audio --- */
        { "org/libsdl/app/SDLActivity", "audioInit", "(IZZI)I", j_audioInit },
        { "org/libsdl/app/SDLActivity", "audioWriteShortBuffer", "([S)V",
          j_audioWriteShort },
        { "org/libsdl/app/SDLActivity", "audioWriteByteBuffer", "([B)V",
          j_audioWriteByte },
        { "org/libsdl/app/SDLActivity", "audioQuit", "()V", j_audioQuit },

        /* --- SDLActivity: input --- */
        { "org/libsdl/app/SDLActivity", "pollInputDevices", "()V",
          j_pollInputDevices },
        { "org/libsdl/app/SDLActivity", "inputGetInputDeviceIds", "(I)[I",
          j_inputGetInputDeviceIds },
        { "org/libsdl/app/SDLActivity", "isScreenKeyboardShown", "()Z",
          j_false },
        { "org/libsdl/app/SDLActivity", "showTextInput", "(IIII)Z", j_false },

        /* --- SDLActivity: services --- */
        { "org/libsdl/app/SDLActivity", "getSystemServiceFromUiThread",
          "(Ljava/lang/String;)Ljava/lang/Object;", j_getSystemService },
        { "org/libsdl/app/SDLActivity", "messageboxShowMessageBox",
          "(ILjava/lang/String;Ljava/lang/String;[I[I[Ljava/lang/String;[I)I",
          j_messagebox },
        { "org/libsdl/app/SDLActivity", "openAPKExpansionInputStream",
          "(Ljava/lang/String;)Ljava/io/InputStream;",
          j_openAPKExpansionInputStream },

        /* --- the clipboard object SDL gets from getSystemService --- */
        { "android/content/ClipboardManager", "hasText", "()Z", j_false },
        { "android/content/ClipboardManager", "getText",
          "()Ljava/lang/CharSequence;", j_clipboardGetText },
        { "android/content/ClipboardManager", "setText",
          "(Ljava/lang/CharSequence;)V", j_void },

        /* --- com.vblank.RCRDX.Activity --- */
        { "com/vblank/RCRDX/Activity", "getLanguage", "()I", j_getLanguage },
        { "com/vblank/RCRDX/Activity", "initSocial", "()Lcom/vblank/Social;",
          j_initSocial },
        { "com/vblank/RCRDX/Activity", "initCloud", "()Lcom/vblank/Cloud;",
          j_initCloud },

        /* --- com.vblank.Social (Play Games / GameCircle) --- */
        { "com/vblank/Social", "isAvailable", "()Z", j_false },
        { "com/vblank/Social", "isSignedIn", "()Z", j_false },
        { "com/vblank/Social", "userSignIn", "()V", j_void },
        { "com/vblank/Social", "viewAchievements", "()V", j_void },
        { "com/vblank/Social", "viewLeaderboards", "()V", j_void },
        { "com/vblank/Social", "unlockAchievement", "(Ljava/lang/String;ZF)V",
          j_unlockAchievement },
        { "com/vblank/Social", "updateLeaderboard", "(Ljava/lang/String;J)V",
          j_updateLeaderboard },
        { "com/vblank/Social", "secondsToScore", "(D)J", j_secondsToScore },

        /* --- com.vblank.Cloud --- */
        { "com/vblank/Cloud", "isAvailable", "()Z", j_false },
        { "com/vblank/Cloud", "read", "(Ljava/lang/String;)[B", j_cloud_read },
        { "com/vblank/Cloud", "write", "([BLjava/lang/String;)V",
          j_cloud_write },
    };

    for (size_t i = 0; i < sizeof binds / sizeof *binds; i++)
        rcr_jni_bind(binds[i].cls, binds[i].name, binds[i].sig,
                     (void *)binds[i].fn);
    nx_log("jni: SDL/vblank surface bound (%zu entries)",
           sizeof binds / sizeof *binds);
    (void)j_true;
}
