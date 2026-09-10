#ifndef DEADTRIGGER_MEDIA_DT_H
#define DEADTRIGGER_MEDIA_DT_H

/*
 * Installs the NextOS backend at Dead Trigger's own IntroVideoPlayer seam.
 * The managed MoviePlayer coroutine, GUI and completion cleanup remain native.
 */
void dt_media_try_install(void);

/*
 * The SDL/UI thread continues running while the compatible raw-framebuffer
 * presenter owns UnityMain. Publish the physical skip-button level here.
 */
void dt_media_set_skip_requested(int requested);

#endif
