#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

// Create a fake jstring handle usable with this env (for nativeRunMain args).
void *jni_shim_make_jstring(const char *s);

void *AAssetManager_fromJava(void *env, void *assetManager);

// Audio sink callbacks (SDL 2.0.8 android AudioTrack path -> host sink).
typedef int (*jni_audio_open_cb)(int sampleRate, int audioFormat, int channels,
                                 int desiredFrames);
typedef void (*jni_audio_write_cb)(void *data, int len);
typedef void (*jni_audio_close_cb)(void);
void jni_shim_set_audio_cb(jni_audio_open_cb open_cb,
                           jni_audio_write_cb write_short_cb,
                           jni_audio_write_cb write_byte_cb,
                           jni_audio_close_cb close_cb);

#endif
