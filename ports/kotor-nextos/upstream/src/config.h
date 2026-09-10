/* config.h -- KOTOR (Aspyr, Odyssey engine) so-loader config, Mali-450.
 *
 * libKOTOR.so is a standard Android SDL2 + GLES2 game (Aspyr port of the 2003
 * Odyssey engine). Unlike the LEGO/Fusion GLSurfaceView ports, KOTOR OWNS its
 * own run loop: its entry is SDL_main and it drives window/GL/events/audio
 * through SDL2 itself. We therefore relink it against the device's NATIVE SDL2
 * (fbdev/GLES2) instead of the Android backend, mount the OBBs via the Aspyr
 * JNI handshake (mountObb/mountPatchObb), and just call SDL_main.
 *
 * MIT license. See LICENSE.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* per-module RWX heaps are sized in main.c; this is unused here but kept for
 * the shared shim sources that reference it. */
#define SO_REGION_MB 160

#define SO_NAME     "libKOTOR.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME    "debug.log"

/* Android identity (mostly cosmetic here; the engine is not GLSurfaceView). */
#define DEVICE_MODEL        "SM-G950F"
#define DEVICE_PRODUCT      "dreamlte"
#define DEVICE_MANUFACTURER "samsung"
#define DEVICE_HARDWARE     "exynos8895"
#define ANDROID_VERSION_RELEASE "7.0"
#define ANDROID_SDK_INT 24

/* --- game data -------------------------------------------------------------
 * Two APK-Expansion OBBs staged next to the binary. mountObb/mountPatchObb
 * (Aspyr JNI natives) take the path and index the OBB (ObbFile). We deploy them
 * renamed to main.obb / patch.obb. */
#define PACKAGE   "com.aspyr.swkotor"
#define OBB_MAIN  "main.obb"
#define OBB_PATCH "patch.obb"
/* APK assets/ (shaders, fonts) extracted next to the binary. */
#define GAMEDATA_DIR "assets"

/* --- filesystem paths (collapsed onto cwd by fix_path) --------------------- */
#define SAVE_PATH   "/data/user/0/com.aspyr.swkotor/files"
#define CACHE_PATH  "/data/user/0/com.aspyr.swkotor/cache"
#define WRITE_PATH  "/storage/emulated/0/Android/data/com.aspyr.swkotor/files"

#define SAVE_GAME_FILE   "savegame.dat"

/* actual render/surface size (picked at runtime) */
extern int screen_width;
extern int screen_height;

#endif
