/*
 * mediandk_shim.c -- Android NDK media API (libmediandk.so) for NextOS.
 *
 * Freedom Planet 2 opens with a NodeCanvas splash FSM whose PlayVideo state only
 * leaves when the VideoPlayer reports the clip finished.  On Android Unity
 * demuxes and decodes that clip through libmediandk; here the library does not
 * exist, so Unity logged "could not load symbol AMediaFormat_setInt64", fell
 * back to its JNI path, found "No tracks in sharedassets1.resource" and never
 * posted a completion.  The splash then stayed on screen forever -- the render
 * loop, audio and the FSM all healthy, waiting for a transition nobody would
 * ever send.
 *
 * Video is a peripheral: the rule for peripherals is a stub that answers "ok"
 * so the game's own flow CONTINUES.  This shim is exactly that and nothing
 * more.  It is an honest empty container: the extractor reports one video
 * track with the clip's real container geometry defaults, and the first read
 * reports end-of-stream, so the decoder immediately hands Unity an EOS output
 * buffer.  Unity sees a clip that played to its end, fires the completion the
 * FSM is waiting for, and the game moves to the menu.
 *
 * What it deliberately does NOT do is decode H.264.  Nothing in the game needs
 * the intro footage to be visible, and a fake decoder that produced garbage
 * frames would be worse than a clean, instant end-of-stream.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nx_elf.h"
#include "mediandk_shim.h"

/* media_status_t values from NdkMediaError.h. */
#define AMEDIA_OK                    0
#define AMEDIA_ERROR_UNSUPPORTED  (-10005)
#define AMEDIA_ERROR_END_OF_STREAM (-10007)

/* AMediaCodec info codes (NdkMediaCodec.h). */
#define AMEDIACODEC_INFO_TRY_AGAIN_LATER        (-1)
#define AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED  (-2)
#define AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED (-3)
#define AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM    4

#define MEDIA_KEY_MAX 24

typedef struct {
    char key[48];
    int kind;                 /* 'i' int32, 'l' int64, 'f' float, 's' string */
    int32_t i32;
    int64_t i64;
    float f;
    char text[64];
} media_entry;

struct AMediaFormat {
    media_entry entries[MEDIA_KEY_MAX];
    int count;
    char printed[256];
};

struct AMediaExtractor {
    AMediaFormat *track;
    int selected;
    int exhausted;
};

typedef struct {
    uint8_t bytes[4096];
} codec_buffer;

struct AMediaCodec {
    AMediaFormat *output;
    codec_buffer input[2];
    codec_buffer scratch;
    int started;
    int input_eos;
    int output_eos;
    int format_reported;
};

typedef struct {
    int64_t offset;
    int64_t size;
    int fd;
} media_source;

/* Set the moment a decoder is created and immediately reports end of stream:
 * from then on the port knows, as a measured fact and not a guess, that this
 * clip will never produce a frame on this device. */
static int video_gave_up;

int fp2_media_video_gave_up(void)
{
    return __atomic_load_n(&video_gave_up, __ATOMIC_ACQUIRE);
}

static int media_log_on(void)
{
#ifdef FP2_RELEASE_BUILD
    return 0;
#else
    static int cached = -1;
    if (cached < 0)
        cached = getenv("FP2_MEDIALOG") != NULL;
    return cached;
#endif
}

#define MEDIA_LOG(...)                                                        \
    do {                                                                      \
        if (media_log_on())                                                   \
            fprintf(stderr, "[fp2/media] " __VA_ARGS__);                      \
    } while (0)

/* ------------------------------------------------------------- AMediaFormat */

static media_entry *format_find(AMediaFormat *f, const char *name)
{
    if (!f || !name)
        return NULL;
    for (int i = 0; i < f->count; i++)
        if (strcmp(f->entries[i].key, name) == 0)
            return &f->entries[i];
    return NULL;
}

static media_entry *format_slot(AMediaFormat *f, const char *name, int kind)
{
    media_entry *e = format_find(f, name);
    if (!e) {
        if (!f || f->count >= MEDIA_KEY_MAX || !name)
            return NULL;
        e = &f->entries[f->count++];
        snprintf(e->key, sizeof e->key, "%s", name);
    }
    e->kind = kind;
    return e;
}

static AMediaFormat *sh_AMediaFormat_new(void)
{
    return calloc(1, sizeof(AMediaFormat));
}

static int sh_AMediaFormat_delete(AMediaFormat *f)
{
    free(f);
    return AMEDIA_OK;
}

static void sh_AMediaFormat_setInt32(AMediaFormat *f, const char *n, int32_t v)
{
    media_entry *e = format_slot(f, n, 'i');
    if (e)
        e->i32 = v;
}

static void sh_AMediaFormat_setInt64(AMediaFormat *f, const char *n, int64_t v)
{
    media_entry *e = format_slot(f, n, 'l');
    if (e)
        e->i64 = v;
}

static void sh_AMediaFormat_setFloat(AMediaFormat *f, const char *n, float v)
{
    media_entry *e = format_slot(f, n, 'f');
    if (e)
        e->f = v;
}

static void sh_AMediaFormat_setString(AMediaFormat *f, const char *n,
                                      const char *v)
{
    media_entry *e = format_slot(f, n, 's');
    if (e)
        snprintf(e->text, sizeof e->text, "%s", v ? v : "");
}

static void sh_AMediaFormat_setBuffer(AMediaFormat *f, const char *n,
                                      const void *data, size_t size)
{
    (void)f; (void)n; (void)data; (void)size;
}

static int sh_AMediaFormat_getInt32(AMediaFormat *f, const char *n, int32_t *o)
{
    media_entry *e = format_find(f, n);
    if (!e || e->kind != 'i' || !o)
        return 0;
    *o = e->i32;
    return 1;
}

static int sh_AMediaFormat_getInt64(AMediaFormat *f, const char *n, int64_t *o)
{
    media_entry *e = format_find(f, n);
    if (!e || e->kind != 'l' || !o)
        return 0;
    *o = e->i64;
    return 1;
}

static int sh_AMediaFormat_getFloat(AMediaFormat *f, const char *n, float *o)
{
    media_entry *e = format_find(f, n);
    if (!e || e->kind != 'f' || !o)
        return 0;
    *o = e->f;
    return 1;
}

static int sh_AMediaFormat_getString(AMediaFormat *f, const char *n,
                                     const char **o)
{
    media_entry *e = format_find(f, n);
    if (!e || e->kind != 's' || !o)
        return 0;
    *o = e->text;
    return 1;
}

static int sh_AMediaFormat_getBuffer(AMediaFormat *f, const char *n,
                                     void **data, size_t *size)
{
    (void)f; (void)n;
    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    return 0;
}

static const char *sh_AMediaFormat_toString(AMediaFormat *f)
{
    if (!f)
        return "";
    snprintf(f->printed, sizeof f->printed, "mediandk-shim(%d keys)",
             f->count);
    return f->printed;
}

/* ---------------------------------------------------------- AMediaExtractor */

/* The clip geometry only has to be self-consistent: Unity sizes its target
 * texture from it and then receives end-of-stream before a single frame is
 * decoded. */
static AMediaFormat *make_video_track(void)
{
    AMediaFormat *f = sh_AMediaFormat_new();
    if (!f)
        return NULL;
    sh_AMediaFormat_setString(f, "mime", "video/avc");
    sh_AMediaFormat_setInt32(f, "width", 1280);
    sh_AMediaFormat_setInt32(f, "height", 720);
    sh_AMediaFormat_setInt32(f, "frame-rate", 30);
    sh_AMediaFormat_setInt32(f, "color-format", 19); /* YUV420Planar */
    sh_AMediaFormat_setInt64(f, "durationUs", 0);
    return f;
}

static AMediaExtractor *sh_AMediaExtractor_new(void)
{
    AMediaExtractor *e = calloc(1, sizeof(AMediaExtractor));
    if (e)
        e->track = make_video_track();
    MEDIA_LOG("extractor %p created\n", (void *)e);
    return e;
}

static int sh_AMediaExtractor_delete(AMediaExtractor *e)
{
    if (e)
        sh_AMediaFormat_delete(e->track);
    free(e);
    return AMEDIA_OK;
}

static int sh_AMediaExtractor_setDataSourceFd(AMediaExtractor *e, int fd,
                                              int64_t offset, int64_t length)
{
    (void)e;
    MEDIA_LOG("setDataSourceFd(fd=%d, offset=%lld, length=%lld)\n", fd,
              (long long)offset, (long long)length);
    return AMEDIA_OK;
}

static int sh_AMediaExtractor_setDataSource(AMediaExtractor *e,
                                            const char *location)
{
    (void)e;
    MEDIA_LOG("setDataSource(%s)\n", location ? location : "(null)");
    return AMEDIA_OK;
}

static int sh_AMediaExtractor_setDataSourceCustom(AMediaExtractor *e,
                                                  void *source)
{
    (void)e; (void)source;
    MEDIA_LOG("setDataSourceCustom\n");
    return AMEDIA_OK;
}

static size_t sh_AMediaExtractor_getTrackCount(AMediaExtractor *e)
{
    return e && e->track ? 1 : 0;
}

static AMediaFormat *sh_AMediaExtractor_getTrackFormat(AMediaExtractor *e,
                                                       size_t index)
{
    if (!e || !e->track || index != 0)
        return NULL;
    /* Unity owns and deletes the returned format, so hand out a copy. */
    AMediaFormat *copy = sh_AMediaFormat_new();
    if (copy)
        *copy = *e->track;
    return copy;
}

static int sh_AMediaExtractor_selectTrack(AMediaExtractor *e, size_t index)
{
    if (!e || index != 0)
        return AMEDIA_ERROR_UNSUPPORTED;
    e->selected = 1;
    return AMEDIA_OK;
}

static int sh_AMediaExtractor_unselectTrack(AMediaExtractor *e, size_t index)
{
    (void)index;
    if (e)
        e->selected = 0;
    return AMEDIA_OK;
}

static ssize_t sh_AMediaExtractor_readSampleData(AMediaExtractor *e,
                                                 uint8_t *buffer,
                                                 size_t capacity)
{
    (void)buffer; (void)capacity;
    if (e)
        e->exhausted = 1;
    return -1;                       /* end of stream on the first read */
}

static int sh_AMediaExtractor_advance(AMediaExtractor *e)
{
    (void)e;
    return 0;                        /* nothing left to advance to */
}

static int64_t sh_AMediaExtractor_getSampleTime(AMediaExtractor *e)
{
    (void)e;
    return -1;
}

static uint32_t sh_AMediaExtractor_getSampleFlags(AMediaExtractor *e)
{
    (void)e;
    return 0;
}

static int sh_AMediaExtractor_getSampleTrackIndex(AMediaExtractor *e)
{
    (void)e;
    return -1;
}

static int sh_AMediaExtractor_seekTo(AMediaExtractor *e, int64_t time,
                                     int mode)
{
    (void)time; (void)mode;
    if (e)
        e->exhausted = 1;
    return AMEDIA_OK;
}

/* -------------------------------------------------------- AMediaDataSource */

static void *sh_AMediaDataSource_new(void)
{
    return calloc(1, sizeof(media_source));
}

static void sh_AMediaDataSource_delete(void *s) { free(s); }
static void sh_AMediaDataSource_setUserdata(void *s, void *u) { (void)s; (void)u; }
static void sh_AMediaDataSource_setReadAt(void *s, void *f) { (void)s; (void)f; }
static void sh_AMediaDataSource_setGetSize(void *s, void *f) { (void)s; (void)f; }
static void sh_AMediaDataSource_setClose(void *s, void *f) { (void)s; (void)f; }

/* --------------------------------------------------------------- AMediaCodec */

static AMediaCodec *codec_new(void)
{
    AMediaCodec *c = calloc(1, sizeof(AMediaCodec));
    if (c)
        c->output = make_video_track();
    return c;
}

static AMediaCodec *sh_AMediaCodec_createDecoderByType(const char *mime)
{
    MEDIA_LOG("createDecoderByType(%s)\n", mime ? mime : "(null)");
    return codec_new();
}

static AMediaCodec *sh_AMediaCodec_createEncoderByType(const char *mime)
{
    (void)mime;
    return NULL;                     /* the port never encodes */
}

static AMediaCodec *sh_AMediaCodec_createCodecByName(const char *name)
{
    MEDIA_LOG("createCodecByName(%s)\n", name ? name : "(null)");
    return codec_new();
}

static int sh_AMediaCodec_delete(AMediaCodec *c)
{
    if (c)
        sh_AMediaFormat_delete(c->output);
    free(c);
    return AMEDIA_OK;
}

static int sh_AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *format,
                                    void *surface, void *crypto, uint32_t flags)
{
    (void)format; (void)surface; (void)crypto; (void)flags;
    return c ? AMEDIA_OK : AMEDIA_ERROR_UNSUPPORTED;
}

static int sh_AMediaCodec_start(AMediaCodec *c)
{
    if (!c)
        return AMEDIA_ERROR_UNSUPPORTED;
    c->started = 1;
    return AMEDIA_OK;
}

static int sh_AMediaCodec_stop(AMediaCodec *c)
{
    if (c)
        c->started = 0;
    return AMEDIA_OK;
}

static int sh_AMediaCodec_flush(AMediaCodec *c)
{
    if (c) {
        c->input_eos = 0;
        c->output_eos = 0;
    }
    return AMEDIA_OK;
}

static ssize_t sh_AMediaCodec_dequeueInputBuffer(AMediaCodec *c,
                                                 int64_t timeout_us)
{
    (void)timeout_us;
    if (!c || !c->started)
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    return 0;
}

static uint8_t *sh_AMediaCodec_getInputBuffer(AMediaCodec *c, size_t index,
                                              size_t *out_size)
{
    if (!c || index >= 2)
        return NULL;
    if (out_size)
        *out_size = sizeof c->input[index].bytes;
    return c->input[index].bytes;
}

static uint8_t *sh_AMediaCodec_getOutputBuffer(AMediaCodec *c, size_t index,
                                               size_t *out_size)
{
    (void)index;
    if (!c)
        return NULL;
    if (out_size)
        *out_size = 0;
    return c->scratch.bytes;
}

static int sh_AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t index,
                                           off_t offset, size_t size,
                                           uint64_t time, uint32_t flags)
{
    (void)index; (void)offset; (void)size; (void)time;
    if (!c)
        return AMEDIA_ERROR_UNSUPPORTED;
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
        c->input_eos = 1;
    return AMEDIA_OK;
}

/* AMediaCodecBufferInfo, laid out exactly as NdkMediaCodec.h declares it. */
typedef struct {
    int32_t offset;
    int32_t size;
    int64_t presentation_time_us;
    uint32_t flags;
} shim_buffer_info;

static ssize_t sh_AMediaCodec_dequeueOutputBuffer(AMediaCodec *c,
                                                  shim_buffer_info *info,
                                                  int64_t timeout_us)
{
    (void)timeout_us;
    if (!c || !c->started)
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    /* Announce the output format once, exactly as a real decoder does, then
     * hand over an empty end-of-stream buffer.  Unity treats that pair as a
     * clip that reached its end and posts the completion the game waits on. */
    if (!c->format_reported) {
        c->format_reported = 1;
        return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    }
    if (c->output_eos)
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    c->output_eos = 1;
    __atomic_store_n(&video_gave_up, 1, __ATOMIC_RELEASE);
    if (info) {
        info->offset = 0;
        info->size = 0;
        info->presentation_time_us = 0;
        info->flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
    }
    MEDIA_LOG("decoder reported end of stream\n");
    return 0;
}

static AMediaFormat *sh_AMediaCodec_getOutputFormat(AMediaCodec *c)
{
    if (!c || !c->output)
        return NULL;
    AMediaFormat *copy = sh_AMediaFormat_new();
    if (copy)
        *copy = *c->output;
    return copy;
}

static int sh_AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t index,
                                              int render)
{
    (void)c; (void)index; (void)render;
    return AMEDIA_OK;
}

static int sh_AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *c,
                                                    size_t index,
                                                    int64_t timestamp)
{
    (void)c; (void)index; (void)timestamp;
    return AMEDIA_OK;
}


/* Unity resolves every AMEDIAFORMAT_KEY_* as a DATA symbol and stops
 * loading the NDK at the first one it cannot find, which is why the shim
 * publishes them with the exact key strings the platform uses. */
const char *AMEDIAFORMAT_KEY_AAC_PROFILE = "aac-profile";
const char *AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char *AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char *AMEDIAFORMAT_KEY_CHANNEL_MASK = "channel-mask";
const char *AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char *AMEDIAFORMAT_KEY_COLOR_RANGE = "color-range";
const char *AMEDIAFORMAT_KEY_COLOR_STANDARD = "color-standard";
const char *AMEDIAFORMAT_KEY_DURATION = "durationUs";
const char *AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL = "flac-compression-level";
const char *AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char *AMEDIAFORMAT_KEY_HEIGHT = "height";
const char *AMEDIAFORMAT_KEY_IS_ADTS = "is-adts";
const char *AMEDIAFORMAT_KEY_IS_AUTOSELECT = "is-autoselect";
const char *AMEDIAFORMAT_KEY_IS_DEFAULT = "is-default";
const char *AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE = "is-forced-subtitle";
const char *AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char *AMEDIAFORMAT_KEY_LANGUAGE = "language";
const char *AMEDIAFORMAT_KEY_MAX_HEIGHT = "max-height";
const char *AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";
const char *AMEDIAFORMAT_KEY_MAX_WIDTH = "max-width";
const char *AMEDIAFORMAT_KEY_MIME = "mime";
const char *AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP = "push-blank-buffers-on-shutdown";
const char *AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER = "repeat-previous-frame-after";
const char *AMEDIAFORMAT_KEY_ROTATION = "rotation-degrees";
const char *AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char *AMEDIAFORMAT_KEY_SLICE_HEIGHT = "slice-height";
const char *AMEDIAFORMAT_KEY_STRIDE = "stride";
const char *AMEDIAFORMAT_KEY_WIDTH = "width";

/* ------------------------------------------------------------------- table */

#define M(name) { #name, (void *)(uintptr_t)sh_##name }

static const nx_import media_tab[] = {
    M(AMediaCodec_configure),
    M(AMediaCodec_createCodecByName),
    M(AMediaCodec_createDecoderByType),
    M(AMediaCodec_createEncoderByType),
    M(AMediaCodec_delete),
    M(AMediaCodec_dequeueInputBuffer),
    M(AMediaCodec_dequeueOutputBuffer),
    M(AMediaCodec_flush),
    M(AMediaCodec_getInputBuffer),
    M(AMediaCodec_getOutputBuffer),
    M(AMediaCodec_getOutputFormat),
    M(AMediaCodec_queueInputBuffer),
    M(AMediaCodec_releaseOutputBuffer),
    M(AMediaCodec_releaseOutputBufferAtTime),
    M(AMediaCodec_start),
    M(AMediaCodec_stop),
    M(AMediaDataSource_delete),
    M(AMediaDataSource_new),
    M(AMediaDataSource_setClose),
    M(AMediaDataSource_setGetSize),
    M(AMediaDataSource_setReadAt),
    M(AMediaDataSource_setUserdata),
    M(AMediaExtractor_advance),
    M(AMediaExtractor_delete),
    M(AMediaExtractor_getSampleFlags),
    M(AMediaExtractor_getSampleTime),
    M(AMediaExtractor_getSampleTrackIndex),
    M(AMediaExtractor_getTrackCount),
    M(AMediaExtractor_getTrackFormat),
    M(AMediaExtractor_new),
    M(AMediaExtractor_readSampleData),
    M(AMediaExtractor_seekTo),
    M(AMediaExtractor_selectTrack),
    M(AMediaExtractor_setDataSource),
    M(AMediaExtractor_setDataSourceCustom),
    M(AMediaExtractor_setDataSourceFd),
    M(AMediaExtractor_unselectTrack),
    M(AMediaFormat_delete),
    M(AMediaFormat_getBuffer),
    M(AMediaFormat_getFloat),
    M(AMediaFormat_getInt32),
    M(AMediaFormat_getInt64),
    M(AMediaFormat_getString),
    M(AMediaFormat_new),
    M(AMediaFormat_setBuffer),
    M(AMediaFormat_setFloat),
    M(AMediaFormat_setInt32),
    M(AMediaFormat_setInt64),
    M(AMediaFormat_setString),
    M(AMediaFormat_toString),
    { "AMEDIAFORMAT_KEY_AAC_PROFILE", (void *)&AMEDIAFORMAT_KEY_AAC_PROFILE },
    { "AMEDIAFORMAT_KEY_BIT_RATE", (void *)&AMEDIAFORMAT_KEY_BIT_RATE },
    { "AMEDIAFORMAT_KEY_CHANNEL_COUNT", (void *)&AMEDIAFORMAT_KEY_CHANNEL_COUNT },
    { "AMEDIAFORMAT_KEY_CHANNEL_MASK", (void *)&AMEDIAFORMAT_KEY_CHANNEL_MASK },
    { "AMEDIAFORMAT_KEY_COLOR_FORMAT", (void *)&AMEDIAFORMAT_KEY_COLOR_FORMAT },
    { "AMEDIAFORMAT_KEY_COLOR_RANGE", (void *)&AMEDIAFORMAT_KEY_COLOR_RANGE },
    { "AMEDIAFORMAT_KEY_COLOR_STANDARD", (void *)&AMEDIAFORMAT_KEY_COLOR_STANDARD },
    { "AMEDIAFORMAT_KEY_DURATION", (void *)&AMEDIAFORMAT_KEY_DURATION },
    { "AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL", (void *)&AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL },
    { "AMEDIAFORMAT_KEY_FRAME_RATE", (void *)&AMEDIAFORMAT_KEY_FRAME_RATE },
    { "AMEDIAFORMAT_KEY_HEIGHT", (void *)&AMEDIAFORMAT_KEY_HEIGHT },
    { "AMEDIAFORMAT_KEY_IS_ADTS", (void *)&AMEDIAFORMAT_KEY_IS_ADTS },
    { "AMEDIAFORMAT_KEY_IS_AUTOSELECT", (void *)&AMEDIAFORMAT_KEY_IS_AUTOSELECT },
    { "AMEDIAFORMAT_KEY_IS_DEFAULT", (void *)&AMEDIAFORMAT_KEY_IS_DEFAULT },
    { "AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE", (void *)&AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE },
    { "AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", (void *)&AMEDIAFORMAT_KEY_I_FRAME_INTERVAL },
    { "AMEDIAFORMAT_KEY_LANGUAGE", (void *)&AMEDIAFORMAT_KEY_LANGUAGE },
    { "AMEDIAFORMAT_KEY_MAX_HEIGHT", (void *)&AMEDIAFORMAT_KEY_MAX_HEIGHT },
    { "AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", (void *)&AMEDIAFORMAT_KEY_MAX_INPUT_SIZE },
    { "AMEDIAFORMAT_KEY_MAX_WIDTH", (void *)&AMEDIAFORMAT_KEY_MAX_WIDTH },
    { "AMEDIAFORMAT_KEY_MIME", (void *)&AMEDIAFORMAT_KEY_MIME },
    { "AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP", (void *)&AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP },
    { "AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER", (void *)&AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER },
    { "AMEDIAFORMAT_KEY_ROTATION", (void *)&AMEDIAFORMAT_KEY_ROTATION },
    { "AMEDIAFORMAT_KEY_SAMPLE_RATE", (void *)&AMEDIAFORMAT_KEY_SAMPLE_RATE },
    { "AMEDIAFORMAT_KEY_SLICE_HEIGHT", (void *)&AMEDIAFORMAT_KEY_SLICE_HEIGHT },
    { "AMEDIAFORMAT_KEY_STRIDE", (void *)&AMEDIAFORMAT_KEY_STRIDE },
    { "AMEDIAFORMAT_KEY_WIDTH", (void *)&AMEDIAFORMAT_KEY_WIDTH },
};

const nx_import *fp2_media_table(size_t *n)
{
    *n = sizeof media_tab / sizeof *media_tab;
    return media_tab;
}

void *fp2_media_sym(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof media_tab / sizeof *media_tab; i++)
        if (strcmp(media_tab[i].name, name) == 0)
            return media_tab[i].addr;
    return NULL;
}
