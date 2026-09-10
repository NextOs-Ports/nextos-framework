#include "media_ndk_fail.h"

#include <stdint.h>
#include <stdio.h>

/* NdkMediaError.h: AMEDIA_ERROR_UNKNOWN is the base MediaNDK error. */
#define HD_AMEDIA_ERROR_UNKNOWN (-10000)
#define HD_AMEDIACODEC_TRY_AGAIN_LATER (-1)

static void hd_media_log_unavailable(void) {
  static int logged;
  if (!logged++)
    fprintf(stderr,
            "[HD-MEDIA] Android MediaNDK unavailable; reporting decoder "
            "creation failure to Unity\n");
}

/* The Android entry points are reached through ELF relocations, not C calls
 * from this executable.  These small ABI endpoints intentionally ignore the
 * arguments supplied in x0..x7 and return only the value class expected by
 * the imported symbol. */
static void *hd_media_null(void) {
  hd_media_log_unavailable();
  return NULL;
}

static int hd_media_error(void) {
  hd_media_log_unavailable();
  return HD_AMEDIA_ERROR_UNKNOWN;
}

static int hd_media_ok_cleanup(void) { return 0; }
static int hd_media_false(void) { return 0; }
static int64_t hd_media_minus_one(void) {
  return HD_AMEDIACODEC_TRY_AGAIN_LATER;
}
static void hd_media_noop(void) {}

/* AMEDIAFORMAT_KEY_* are imported OBJECTS (const char *), not functions.  A
 * function-shaped stub here makes the engine dereference instruction bytes as
 * a pointer.  Keep real pointer objects with Android's canonical key strings. */
static const char *hd_key_channel_count = "channel-count";
static const char *hd_key_color_format = "color-format";
static const char *hd_key_color_range = "color-range";
static const char *hd_key_color_standard = "color-standard";
static const char *hd_key_duration = "durationUs";
static const char *hd_key_encoder_delay = "encoder-delay";
static const char *hd_key_frame_rate = "frame-rate";
static const char *hd_key_height = "height";
static const char *hd_key_language = "language";
static const char *hd_key_mime = "mime";
static const char *hd_key_rotation = "rotation-degrees";
static const char *hd_key_sample_rate = "sample-rate";
static const char *hd_key_slice_height = "slice-height";
static const char *hd_key_stride = "stride";
static const char *hd_key_width = "width";

#define HD_SYM(name, function) { name, (void *)(function) }
#define HD_KEY(name, object) { name, (void *)&(object) }

static const HdMediaNdkSymbol hd_media_symbols[] = {
    HD_SYM("AHardwareBuffer_acquire", hd_media_noop),
    HD_SYM("AHardwareBuffer_describe", hd_media_noop),
    HD_SYM("AHardwareBuffer_release", hd_media_noop),
    HD_SYM("AImageReader_acquireLatestImage", hd_media_error),
    HD_SYM("AImageReader_delete", hd_media_noop),
    HD_SYM("AImageReader_getWindow", hd_media_error),
    HD_SYM("AImageReader_newWithUsage", hd_media_error),
    HD_SYM("AImageReader_setBufferRemovedListener", hd_media_error),
    HD_SYM("AImageReader_setImageListener", hd_media_error),
    HD_SYM("AImage_delete", hd_media_noop),
    HD_SYM("AImage_deleteAsync", hd_media_noop),
    HD_SYM("AImage_getHardwareBuffer", hd_media_error),
    HD_SYM("AImage_getTimestamp", hd_media_error),
    HD_SYM("AImage_getWidth", hd_media_error),

    HD_KEY("AMEDIAFORMAT_KEY_CHANNEL_COUNT", hd_key_channel_count),
    HD_KEY("AMEDIAFORMAT_KEY_COLOR_FORMAT", hd_key_color_format),
    HD_KEY("AMEDIAFORMAT_KEY_COLOR_RANGE", hd_key_color_range),
    HD_KEY("AMEDIAFORMAT_KEY_COLOR_STANDARD", hd_key_color_standard),
    HD_KEY("AMEDIAFORMAT_KEY_DURATION", hd_key_duration),
    HD_KEY("AMEDIAFORMAT_KEY_ENCODER_DELAY", hd_key_encoder_delay),
    HD_KEY("AMEDIAFORMAT_KEY_FRAME_RATE", hd_key_frame_rate),
    HD_KEY("AMEDIAFORMAT_KEY_HEIGHT", hd_key_height),
    HD_KEY("AMEDIAFORMAT_KEY_LANGUAGE", hd_key_language),
    HD_KEY("AMEDIAFORMAT_KEY_MIME", hd_key_mime),
    HD_KEY("AMEDIAFORMAT_KEY_ROTATION", hd_key_rotation),
    HD_KEY("AMEDIAFORMAT_KEY_SAMPLE_RATE", hd_key_sample_rate),
    HD_KEY("AMEDIAFORMAT_KEY_SLICE_HEIGHT", hd_key_slice_height),
    HD_KEY("AMEDIAFORMAT_KEY_STRIDE", hd_key_stride),
    HD_KEY("AMEDIAFORMAT_KEY_WIDTH", hd_key_width),

    HD_SYM("AMediaCodec_configure", hd_media_error),
    HD_SYM("AMediaCodec_createDecoderByType", hd_media_null),
    HD_SYM("AMediaCodec_delete", hd_media_ok_cleanup),
    HD_SYM("AMediaCodec_dequeueInputBuffer", hd_media_minus_one),
    HD_SYM("AMediaCodec_dequeueOutputBuffer", hd_media_minus_one),
    HD_SYM("AMediaCodec_flush", hd_media_error),
    HD_SYM("AMediaCodec_getInputBuffer", hd_media_null),
    HD_SYM("AMediaCodec_getOutputBuffer", hd_media_null),
    HD_SYM("AMediaCodec_getOutputFormat", hd_media_null),
    HD_SYM("AMediaCodec_queueInputBuffer", hd_media_error),
    HD_SYM("AMediaCodec_releaseOutputBuffer", hd_media_error),
    HD_SYM("AMediaCodec_setOutputSurface", hd_media_error),
    HD_SYM("AMediaCodec_start", hd_media_error),
    HD_SYM("AMediaCodec_stop", hd_media_error),

    HD_SYM("AMediaDataSource_delete", hd_media_ok_cleanup),
    HD_SYM("AMediaDataSource_new", hd_media_null),
    HD_SYM("AMediaDataSource_setClose", hd_media_noop),
    HD_SYM("AMediaDataSource_setGetSize", hd_media_noop),
    HD_SYM("AMediaDataSource_setReadAt", hd_media_noop),
    HD_SYM("AMediaDataSource_setUserdata", hd_media_noop),

    HD_SYM("AMediaExtractor_advance", hd_media_false),
    HD_SYM("AMediaExtractor_delete", hd_media_ok_cleanup),
    HD_SYM("AMediaExtractor_getSampleTime", hd_media_minus_one),
    HD_SYM("AMediaExtractor_getSampleTrackIndex", hd_media_minus_one),
    HD_SYM("AMediaExtractor_getTrackCount", hd_media_false),
    HD_SYM("AMediaExtractor_getTrackFormat", hd_media_null),
    HD_SYM("AMediaExtractor_new", hd_media_null),
    HD_SYM("AMediaExtractor_readSampleData", hd_media_minus_one),
    HD_SYM("AMediaExtractor_seekTo", hd_media_error),
    HD_SYM("AMediaExtractor_selectTrack", hd_media_error),
    HD_SYM("AMediaExtractor_setDataSource", hd_media_error),
    HD_SYM("AMediaExtractor_setDataSourceCustom", hd_media_error),
    HD_SYM("AMediaExtractor_setDataSourceFd", hd_media_error),

    HD_SYM("AMediaFormat_delete", hd_media_ok_cleanup),
    HD_SYM("AMediaFormat_getFloat", hd_media_false),
    HD_SYM("AMediaFormat_getInt32", hd_media_false),
    HD_SYM("AMediaFormat_getInt64", hd_media_false),
    HD_SYM("AMediaFormat_getString", hd_media_false),
    HD_SYM("AMediaFormat_setInt32", hd_media_noop),
};

const HdMediaNdkSymbol *hd_media_ndk_fail_symbols(size_t *count) {
  if (count)
    *count = sizeof(hd_media_symbols) / sizeof(hd_media_symbols[0]);
  return hd_media_symbols;
}
