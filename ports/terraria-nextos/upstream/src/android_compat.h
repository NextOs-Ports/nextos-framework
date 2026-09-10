#ifndef TERRARIA_ANDROID_COMPAT_H
#define TERRARIA_ANDROID_COMPAT_H

/* Return 1 and store the Android ActivityInfo value when field_name is one
 * of the orientation constants used by UnityPlayer. Return 0 otherwise. */
int ter_android_orientation_constant(const char *field_name, int *value);

/* Decide whether SDL must own the native window. The explicit CUP_VIDEO
 * override wins. Without an override, fbdev-class SDL driver names keep the
 * proven vendor-EGL path only when the EGL provider is not PowerVR/IMG. */
int ter_video_use_sdl_owner(const char *sdl_driver, const char *video_override,
                            int force_shim, const char *egl_vendor);

/* Read the exact package_id value from an NXExtract marker. Only Terraria's
 * two accepted Android identities are returned; malformed or foreign input
 * fails closed with NULL. The returned pointer has static storage duration. */
const char *ter_android_package_from_marker(const char *marker_json);

/* Authorize the canonical portable provider-name retry only after a measured
 * SDL window/context failure and only when neither provider was explicitly
 * supplied by the firmware or user. A non-NULL empty string is still an
 * explicit setting and remains sovereign. */
int ter_allow_portable_provider_retry(const char *egl_provider,
                                      const char *gles_provider,
                                      int initial_attempt_failed);

#endif
