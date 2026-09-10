#ifndef HUNTDOWN_VIDEO_H
#define HUNTDOWN_VIDEO_H

#include <stdint.h>

/* Unity 2022's Android VideoPlayer requires MediaNDK, which no Linux firmware
 * provides.  This bridge keeps the managed VideoPlayer lifecycle and presents
 * the two original APK movies through whichever route the running backend
 * allows: the proven fbdev path when the loader owns /dev/fb0, and a GL quad
 * in Unity's own context when SDL owns the scanout (KMSDRM, Wayland, X11).
 *
 * `fbdev_backend` is the capability answer, not a device name. */
int hd_video_bridge_install(uintptr_t il2cpp_base, int fbdev_backend);
int hd_video_bridge_owns_screen(void);

/* Present one movie frame.  Returns 1 when the loader drew into the current
 * GL backbuffer and the caller still has to swap; 0 when the decoder owns the
 * scanout directly and the caller must not swap. */
int hd_video_bridge_present_tick(void);
void hd_video_bridge_shutdown(void);

/* The authored movie sink may need exclusive access to a firmware ALSA PCM.
 * These calls pause only the host FMOD AudioTrack pump while that sink lives;
 * Unity's managed VideoPlayer order and callbacks remain unchanged. */
void hd_audio_movie_begin(void);
void hd_audio_movie_end(void);

#endif
