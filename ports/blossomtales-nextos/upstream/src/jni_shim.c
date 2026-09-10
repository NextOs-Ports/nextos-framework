/*
 * jni_shim.c -- fake JNI (JNIEnv + JavaVM) para Stardew Valley Mono-Android.
 *
 * Estrategia: vtable plana nos offsets Bionic arm64 LP64 (mesmos do gtalcs2,
 * verificados contra o NDK). FindClass/GetMethodID/GetFieldID devolvem tokens
 * estaveis nao-nulos para qualquer nome (Mono aborta se receber NULL). As
 * chamadas Call*Method caem num default seguro e sao logadas na 1a ocorrencia
 * — assim o boot nao morre por causa de uma chamada imprevista, e vemos o que
 * precisa de resposta real (filosofia nx_jni).
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>

#include "jni_shim.h"
#include "bt_input.h"
#include "sdv_egl_bridge.h"
#include "media_player.h"

static char fake_vm[0x1000];
static char fake_env[0x1000];

static int g_verbose = 1;

/* AssetManager.Open e Texture2DReader rodam sincronamente na mesma worker.
 * TLS evita uma carga paralela marcar a textura da thread errada. */
static __thread int g_last_asset_paletted_texture;
static __thread int g_pending_font_texture;
/* AssetManager.Open e o upload GL nem sempre acontecem na mesma worker no
 * preload do Paris. Os marcadores de formato continuam TLS, mas o caminho
 * precisa atravessar essa troca de thread para o pager conseguir reabrir o
 * XNB. O preload e serial neste ponto; o mutex ainda torna a copia atomica. */
static char g_last_texture_asset_path[4096];
static pthread_mutex_t g_last_texture_asset_path_mutex =
    PTHREAD_MUTEX_INITIALIZER;

void jni_set_verbose(int on) { g_verbose = on; }
int jni_last_asset_is_paletted_texture(void) {
    return g_last_asset_paletted_texture;
}
int jni_take_pending_paletted_texture(void) {
    int value = g_last_asset_paletted_texture;
    g_last_asset_paletted_texture = 0;
    return value;
}
int jni_take_pending_font_texture(void) {
    int value = g_pending_font_texture;
    g_pending_font_texture = 0;
    return value;
}
int jni_copy_last_texture_asset_path(char *out, size_t capacity) {
    int present = 0;
    if (!out || !capacity) return 0;
    pthread_mutex_lock(&g_last_texture_asset_path_mutex);
    if (g_last_texture_asset_path[0]) {
        snprintf(out, capacity, "%s", g_last_texture_asset_path);
        present = 1;
    } else {
        out[0] = '\0';
    }
    pthread_mutex_unlock(&g_last_texture_asset_path_mutex);
    return present;
}

/* ---- tokens estaveis ---------------------------------------------------- */
/* FindClass devolve um "jclass" != NULL. Reutilizamos o mesmo token p/ todos:
 * a maioria dos usos so compara/armazena. Logamos o nome. */
static long g_class_token = 0xC1A500;   /* sentinelo nao-nulo */

/* Algumas partes do bridge precisam distinguir Class.getName() e
 * GetObjectClass() para tipos gerados pelo Xamarin. O restante do bootstrap
 * continua usando g_class_token; estes handles nomeados sao opt-in. */
#define MAX_FAKE_CLASSES 256
#define MAX_FAKE_OBJECTS 65536
#define MAX_FAKE_ARRAYS 512
struct fake_class {
    char *dot_name;
};
struct fake_object {
    void *klass;
    void *payload;
};
struct fake_motion_event {
    int action;
    int source;
    int device_id;
    float x;
    float y;
    float axes[24];
};
struct fake_stream {
    FILE *file;
    long length;
    long xwb_metadata_end;
    long read_limit;
    int xwb_skip_phase;
    uint64_t xwb_fast_skipped;
    char *path;
    unsigned reads;
    uint64_t read_total;
    uint64_t read_hash;
};

/* AndroidMusicStreamer decodifica os OGG por MediaExtractor/MediaCodec. No
 * NextOS nao existe o framework Java do Android, portanto emulamos somente o
 * subconjunto usado pelo jogo e entregamos PCM16 pelo mesmo ByteBuffer que o
 * binding Xamarin ja espera. libsndfile fica dinamico para o executavel nao
 * ganhar uma dependencia obrigatoria fora do aparelho. */
typedef int64_t sf_count_t;
typedef struct SNDFILE_tag SNDFILE;
struct sb_sf_info {
    sf_count_t frames;
    int samplerate;
    int channels;
    int format;
    int sections;
    int seekable;
};

#define AUDIO_FD_MAGIC        UINT32_C(0x41464431)
#define AUDIO_EXTRACTOR_MAGIC UINT32_C(0x41455831)
#define AUDIO_CODEC_MAGIC     UINT32_C(0x41434431)
#define AUDIO_BUFFER_MAGIC    UINT32_C(0x41424231)
#define AUDIO_INFO_MAGIC      UINT32_C(0x41424931)
#define AUDIO_MP_MAGIC        UINT32_C(0x414D5031)
#define AUDIO_FDESC_MAGIC     UINT32_C(0x41464431 + 1)

struct fake_audio_fd {
    uint32_t magic;
    char *path;
};
struct fake_audio_extractor {
    uint32_t magic;
    char *path;
    SNDFILE *snd;
    struct sb_sf_info info;
    int eof;
};
struct fake_audio_buffer {
    uint32_t magic;
    unsigned char *data;
    size_t capacity;
    size_t size;
    size_t position;
};
struct fake_audio_buffer_info {
    uint32_t magic;
    int flags;
    int size;
};
struct fake_audio_codec {
    uint32_t magic;
    struct fake_audio_extractor *extractor;
    struct fake_audio_buffer input;
    struct fake_audio_buffer output;
    void *input_object;
    void *output_object;
    int input_queued;
    int input_eos;
    uint64_t decoded_bytes;
};

static pthread_once_t g_sndfile_once = PTHREAD_ONCE_INIT;
static void *g_sndfile_lib;
static SNDFILE *(*g_sf_open)(const char *, int, struct sb_sf_info *);
static sf_count_t (*g_sf_readf_short)(SNDFILE *, short *, sf_count_t);
static sf_count_t (*g_sf_seek)(SNDFILE *, sf_count_t, int);
static int (*g_sf_close)(SNDFILE *);

static int asset_verbose(void) {
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("SB_ASSET_VERBOSE");
        enabled = value && value[0] && value[0] != '0';
        initialized = 1;
    }
    return enabled;
}
enum fake_array_kind {
    FAKE_ARRAY_OBJECT,
    FAKE_ARRAY_BOOLEAN,
    FAKE_ARRAY_INT,
    FAKE_ARRAY_BYTE,
};
struct fake_array {
    int active;
    int refs;
    enum fake_array_kind kind;
    int len;
    void *element_class;
    union {
        void **objects;
        unsigned char *booleans;
        int *ints;
        signed char *bytes;
    } data;
};
static struct fake_class g_fake_classes[MAX_FAKE_CLASSES];
static struct fake_object g_fake_objects[MAX_FAKE_OBJECTS];
static struct fake_array g_fake_arrays[MAX_FAKE_ARRAYS];
static int g_fake_classes_n;
static int g_fake_objects_n;
static int g_fake_arrays_n;
static pthread_mutex_t g_fake_classes_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fake_objects_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fake_arrays_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fake_singletons_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_activity;
static void *g_main_looper;
static volatile int g_main_looper_ready;
static void *g_egl_default_display;
static void *g_egl_no_display;
static void *g_egl_no_context;
static void *g_egl_no_surface;
static void *g_egl_display;
static void *g_egl_context;
static void *g_egl_surface;
static void *g_input_devices[4];
static void *g_input_vibrator;
static void *g_input_vibrator_manager;
/* Slot 0 e touch; slots 1..4 sao MotionEvents persistentes dos gamepads. */
static struct fake_motion_event g_motion_events[5];

static const char *fake_class_name(void *token) {
    const char *name = NULL;
    pthread_mutex_lock(&g_fake_classes_lock);
    for (int i = 0; i < g_fake_classes_n; i++) {
        if (token == &g_fake_classes[i]) {
            name = g_fake_classes[i].dot_name;
            break;
        }
    }
    pthread_mutex_unlock(&g_fake_classes_lock);
    return name;
}

static void *fake_object_class(void *token) {
    void *klass = NULL;
    pthread_mutex_lock(&g_fake_objects_lock);
    for (int i = 0; i < g_fake_objects_n; i++) {
        if (token == &g_fake_objects[i]) {
            klass = g_fake_objects[i].klass;
            break;
        }
    }
    pthread_mutex_unlock(&g_fake_objects_lock);
    return klass;
}

static void *fake_object_payload(void *token) {
    void *payload = NULL;
    pthread_mutex_lock(&g_fake_objects_lock);
    for (int i = 0; i < g_fake_objects_n; i++) {
        if (token == &g_fake_objects[i]) {
            payload = g_fake_objects[i].payload;
            break;
        }
    }
    pthread_mutex_unlock(&g_fake_objects_lock);
    return payload;
}

static void fake_object_set_payload(void *token, void *payload) {
    pthread_mutex_lock(&g_fake_objects_lock);
    for (int i = 0; i < g_fake_objects_n; i++) {
        if (token == &g_fake_objects[i]) {
            g_fake_objects[i].payload = payload;
            break;
        }
    }
    pthread_mutex_unlock(&g_fake_objects_lock);
}

static struct fake_array *fake_array_find(void *token) {
    for (int i = 0; i < g_fake_arrays_n; i++)
        if (token == &g_fake_arrays[i] && g_fake_arrays[i].active)
            return &g_fake_arrays[i];
    return NULL;
}

static struct fake_array *fake_array_new(enum fake_array_kind kind, int len) {
    if (len < 0) return NULL;
    pthread_mutex_lock(&g_fake_arrays_lock);
    struct fake_array *a = NULL;
    for (int i = 0; i < g_fake_arrays_n; i++) {
        if (!g_fake_arrays[i].active) {
            a = &g_fake_arrays[i];
            break;
        }
    }
    if (!a) {
        if (g_fake_arrays_n >= MAX_FAKE_ARRAYS) {
            pthread_mutex_unlock(&g_fake_arrays_lock);
            return NULL;
        }
        a = &g_fake_arrays[g_fake_arrays_n++];
    }
    memset(a, 0, sizeof(*a));
    a->active = 1;
    a->refs = 1;
    a->kind = kind;
    a->len = len;
    a->element_class = NULL;
    if (kind == FAKE_ARRAY_OBJECT)
        a->data.objects = calloc((size_t)(len ? len : 1), sizeof(void *));
    else if (kind == FAKE_ARRAY_BOOLEAN)
        a->data.booleans = calloc((size_t)(len ? len : 1), 1);
    else if (kind == FAKE_ARRAY_INT)
        a->data.ints = calloc((size_t)(len ? len : 1), sizeof(int));
    else
        a->data.bytes = calloc((size_t)(len ? len : 1), sizeof(signed char));
    pthread_mutex_unlock(&g_fake_arrays_lock);
    return a;
}

static void fake_array_add_ref(void *token) {
    pthread_mutex_lock(&g_fake_arrays_lock);
    for (int i = 0; i < g_fake_arrays_n; i++)
        if (token == &g_fake_arrays[i] && g_fake_arrays[i].active) {
            g_fake_arrays[i].refs++;
            break;
        }
    pthread_mutex_unlock(&g_fake_arrays_lock);
}

static void fake_array_release(void *token) {
    pthread_mutex_lock(&g_fake_arrays_lock);
    struct fake_array *a = NULL;
    for (int i = 0; i < g_fake_arrays_n; i++)
        if (token == &g_fake_arrays[i] && g_fake_arrays[i].active) {
            a = &g_fake_arrays[i];
            break;
        }
    if (!a || --a->refs > 0) {
        pthread_mutex_unlock(&g_fake_arrays_lock);
        return;
    }
    if (a->kind == FAKE_ARRAY_OBJECT)
        free(a->data.objects);
    else if (a->kind == FAKE_ARRAY_BOOLEAN)
        free(a->data.booleans);
    else if (a->kind == FAKE_ARRAY_INT)
        free(a->data.ints);
    else
        free(a->data.bytes);
    memset(a, 0, sizeof(*a));
    pthread_mutex_unlock(&g_fake_arrays_lock);
}

void *jni_make_class(const char *dot_name) {
    if (!dot_name || !*dot_name) return (void *)g_class_token;
    char *normalized = strdup(dot_name);
    for (char *p = normalized; *p; p++)
        if (*p == '/') *p = '.';
    pthread_mutex_lock(&g_fake_classes_lock);
    for (int i = 0; i < g_fake_classes_n; i++)
        if (strcmp(g_fake_classes[i].dot_name, normalized) == 0) {
            free(normalized);
            void *existing = &g_fake_classes[i];
            pthread_mutex_unlock(&g_fake_classes_lock);
            return existing;
        }
    if (g_fake_classes_n >= MAX_FAKE_CLASSES) {
        free(normalized);
        pthread_mutex_unlock(&g_fake_classes_lock);
        return (void *)g_class_token;
    }
    struct fake_class *c = &g_fake_classes[g_fake_classes_n++];
    c->dot_name = normalized;
    pthread_mutex_unlock(&g_fake_classes_lock);
    return c;
}

void *jni_make_object(void *klass) {
    static int capacity_warning_logged;
    static int saturation_logged;

    pthread_mutex_lock(&g_fake_objects_lock);
    if (g_fake_objects_n >= MAX_FAKE_OBJECTS) {
        if (!saturation_logged) {
            fprintf(stderr,
                    "[jni] ERRO: tabela de objetos fake saturou em %d entradas\n",
                    MAX_FAKE_OBJECTS);
            saturation_logged = 1;
        }
        pthread_mutex_unlock(&g_fake_objects_lock);
        return (void *)0xC1A501;
    }
    if (!capacity_warning_logged &&
        g_fake_objects_n >= MAX_FAKE_OBJECTS * 3 / 4) {
        fprintf(stderr,
                "[jni] aviso: tabela de objetos fake em 75%% (%d/%d)\n",
                g_fake_objects_n, MAX_FAKE_OBJECTS);
        capacity_warning_logged = 1;
    }
    struct fake_object *o = &g_fake_objects[g_fake_objects_n++];
    o->klass = klass;
    o->payload = NULL;
    pthread_mutex_unlock(&g_fake_objects_lock);
    return o;
}

void jni_set_key_event_keycode(void *event, int keycode, int device_id) {
    uintptr_t payload;

    if (device_id < 1 || device_id > 4) device_id = 1;
    /* +1 preserva o keycode zero sem confundi-lo com payload NULL. */
    payload = ((uintptr_t)(unsigned int)device_id << 16) |
              ((uintptr_t)(unsigned int)(keycode + 1) & 0xffffu);
    fake_object_set_payload(event, (void *)payload);
}

void jni_set_motion_event(void *event, int action, float x, float y) {
    struct fake_motion_event *motion = &g_motion_events[0];

    memset(motion, 0, sizeof(*motion));
    motion->action = action;
    motion->source = 0x00001002; /* SOURCE_TOUCHSCREEN */
    motion->x = x;
    motion->y = y;
    fake_object_set_payload(event, motion);
}

void jni_set_gamepad_motion_event(void *event, int device_id,
                                  float lx, float ly,
                                  float rx, float ry, float left_trigger,
                                  float right_trigger, float hat_x,
                                  float hat_y) {
    struct fake_motion_event *motion;

    if (device_id < 1 || device_id > 4) device_id = 1;
    motion = &g_motion_events[device_id];
    memset(motion, 0, sizeof(*motion));
    motion->action = 2;             /* ACTION_MOVE */
    motion->source = 0x01000611;    /* JOYSTICK | GAMEPAD | DPAD */
    motion->device_id = device_id;
    motion->axes[0] = lx;           /* AXIS_X */
    motion->axes[1] = ly;           /* AXIS_Y */
    motion->axes[11] = rx;          /* AXIS_Z */
    motion->axes[14] = ry;          /* AXIS_RZ */
    /* O AndroidGamePad do MonoGame le os gatilhos SO' de AXIS_LTRIGGER(17) e
     * AXIS_RTRIGGER(18) (OnGenericMotionEvent: _leftTrigger = GetAxisValue(17),
     * _rightTrigger = GetAxisValue(18)). Publicar apenas BRAKE/GAS deixa L2/R2
     * mortos no jogo. Mandar nos quatro: o pad Android tipico anuncia os dois
     * pares e o jogo so' consome 17/18. */
    motion->axes[17] = left_trigger;  /* AXIS_LTRIGGER */
    motion->axes[18] = right_trigger; /* AXIS_RTRIGGER */
    motion->axes[23] = left_trigger;  /* AXIS_BRAKE */
    motion->axes[22] = right_trigger; /* AXIS_GAS */
    motion->axes[15] = hat_x;       /* AXIS_HAT_X */
    motion->axes[16] = hat_y;       /* AXIS_HAT_Y */
    fake_object_set_payload(event, motion);
}

static int key_event_keycode(void *event) {
    uintptr_t payload = (uintptr_t)fake_object_payload(event);
    unsigned int encoded = (unsigned int)(payload & 0xffffu);
    return encoded ? (int)encoded - 1 : 0;
}

static int key_event_device_id(void *event) {
    uintptr_t payload = (uintptr_t)fake_object_payload(event);
    return (int)((payload >> 16) & 0xffffu);
}

static void *input_device(int device_id) {
    void *result;

    if (device_id < 1 || device_id > 4) return NULL;
    pthread_mutex_lock(&g_fake_singletons_lock);
    if (!g_input_devices[device_id - 1]) {
        g_input_devices[device_id - 1] = jni_make_object(
            jni_make_class("android.view.InputDevice"));
        fake_object_set_payload(g_input_devices[device_id - 1],
                                (void *)(intptr_t)device_id);
    }
    result = g_input_devices[device_id - 1];
    pthread_mutex_unlock(&g_fake_singletons_lock);
    return result;
}

static int input_device_id(void *device) {
    intptr_t id = (intptr_t)fake_object_payload(device);
    return id >= 1 && id <= 4 ? (int)id : 0;
}

static void *connected_input_device(int device_id) {
    if (device_id < 1 || device_id > 4 ||
        !(sdv_egl_gamepad_mask() & (1u << (device_id - 1))))
        return NULL;
    return input_device(device_id);
}

static void *input_device_ids_array(void) {
    unsigned int mask = sdv_egl_gamepad_mask();
    int count = 0;

    for (int i = 0; i < 4; ++i)
        if (mask & (1u << i)) ++count;
    struct fake_array *ids = fake_array_new(FAKE_ARRAY_INT, count);
    if (!ids) return NULL;
    for (int i = 0, out = 0; i < 4; ++i)
        if (mask & (1u << i)) ids->data.ints[out++] = i + 1;
    return ids;
}

static void *fake_singleton(void **slot, const char *dot_class_name) {
    pthread_mutex_lock(&g_fake_singletons_lock);
    if (!*slot)
        *slot = jni_make_object(jni_make_class(dot_class_name));
    void *result = *slot;
    pthread_mutex_unlock(&g_fake_singletons_lock);
    return result;
}

static uint32_t read_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* O ctor do WaveBank streaming precisa apenas dos segmentos anteriores ao
 * WaveData. O MonoGame Android, porem, forca TitleContainer.OpenStream para um
 * MemoryStream e faz CopyTo do banco inteiro. Em music.xwb isso materializa
 * ~247 MiB no heap. Guardamos aqui o inicio do WaveData para reconhecer e
 * limitar somente esse CopyTo; as reaberturas da worker de audio seguem com o
 * arquivo fisico completo. */
static long xwb_music_metadata_end(FILE *file, const char *relative,
                                   long file_length) {
    const char *base = strrchr(relative, '/');
    base = base ? base + 1 : relative;
    if (strcmp(base, "music.xwb") != 0 || file_length < 0x34)
        return 0;

    unsigned char header[0x34];
    size_t got = fread(header, 1, sizeof(header), file);
    rewind(file);
    if (got != sizeof(header) || memcmp(header, "WBND", 4) != 0)
        return 0;

    /* XACT v42+ possui cinco segmentos de 8 bytes a partir de 0x0c;
     * o quinto e WaveData. Alguns bancos desta versao trazem Length obsoleto
     * nesse ultimo descritor, entao validamos o Offset contra os quatro
     * segmentos de metadados e contra o tamanho fisico. */
    uint32_t version = read_le32(header + 4);
    uint32_t wave_offset = read_le32(header + 0x2c);
    if (version < 42 || wave_offset < sizeof(header) ||
        wave_offset >= (uint64_t)file_length || wave_offset > 16u * 1024u * 1024u)
        return 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t offset = read_le32(header + 0x0c + i * 8);
        uint32_t length = read_le32(header + 0x10 + i * 8);
        if (offset > wave_offset || length > wave_offset - offset)
            return 0;
    }
    return (long)wave_offset;
}

static long asset_stream_logical_end(const struct fake_stream *stream) {
    return stream->read_limit > 0 ? stream->read_limit : stream->length;
}

/* Os atlases indexados seguem a convencao FooTexture*.xnb + FooPalette.xnb.
 * Ex.: FootTextureWrench.xnb compartilha FootPalette.xnb. Detectar pelo arquivo
 * companheiro e mais seguro que uma lista de personagens e cobre HUD/inimigos
 * adicionados pelo jogo sem tratar texturas RGBA normais como indexadas. */
static int asset_texture_has_palette(const char *root, const char *relative) {
    const char *base = strrchr(relative, '/');
    base = base ? base + 1 : relative;
    const char *texture = strstr(base, "Texture");
    if (!texture || !strstr(relative, "2d/Animations/"))
        return 0;

    char palette[4096];
    size_t prefix = (size_t)(texture - relative);
    int n = snprintf(palette, sizeof(palette), "%s/%.*sPalette.xnb",
                     root, (int)prefix, relative);
    return n > 0 && n < (int)sizeof(palette) && access(palette, R_OK) == 0;
}

static struct fake_stream *asset_stream_open(const char *asset_name) {
    static unsigned opened;
    const char *root = getenv("SB_ASSET_DIR");
    char relative[2048];
    char full[4096];

    if (!root || !*root)
        root = "/storage/roms/ports/stardewvalley/assets";
    if (!asset_name || !*asset_name || asset_name[0] == '/' ||
        strstr(asset_name, ".."))
        return NULL;

    size_t n = strlen(asset_name);
    if (n >= sizeof(relative)) return NULL;
    for (size_t i = 0; i <= n; i++)
        relative[i] = asset_name[i] == '\\' ? '/' : asset_name[i];
    if (snprintf(full, sizeof(full), "%s/%s", root, relative) >= (int)sizeof(full))
        return NULL;

    /* SFXPack compacto (repack offline: sons reais ate o orcamento + resto
     * silencio 2KB, MESMA contagem de entradas). O LoadSFXPack copia o pack
     * inteiro pra RAM e a barra de loading so fecha no ultimo som — capar por
     * read_limit congelava o progresso. Redireciona se o .small existir. */
    if (strstr(relative, "SFXPack.pbn")) {
        char small[4096];
        size_t len = strlen(full);
        if (len > 4 && len + 7 < sizeof(small)) {
            memcpy(small, full, len - 4);
            memcpy(small + len - 4, ".small.pbn", 11);
            if (access(small, R_OK) == 0) {
                fprintf(stderr, "[asset] %s -> pack compacto\n", relative);
                memcpy(full, small, len + 7);
            }
        }
    }

    FILE *file = fopen(full, "rb");
    if (!file) {
        if (asset_verbose())
        fprintf(stderr, "[asset] MISS %s\n", full);
        return NULL;
    }
    /* Nao apague a marca ao abrir .pbn, paleta ou em uma sonda que falhou:
     * Texture2DReader pode fazer essas aberturas entre ler o atlas e criar a
     * textura GL. Uma proxima Texture*.xnb bem-sucedida substitui a marca. */
    {
        const char *base = strrchr(relative, '/');
        base = base ? base + 1 : relative;
        if (strstr(base, "Texture") && strstr(base, ".xnb") &&
            asset_texture_has_palette(root, relative))
            g_last_asset_paletted_texture = 1;
        if (strstr(base, "Texture") && strstr(base, ".xnb") &&
            (strstr(relative, "/Fonts/") == relative ||
             strstr(relative, "Content/Fonts/")))
            g_pending_font_texture = 1;
        if (strstr(base, "Texture") && strstr(base, ".xnb")) {
            pthread_mutex_lock(&g_last_texture_asset_path_mutex);
            snprintf(g_last_texture_asset_path,
                     sizeof(g_last_texture_asset_path), "%s", full);
            pthread_mutex_unlock(&g_last_texture_asset_path_mutex);
        }
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    rewind(file);

    struct fake_stream *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        fclose(file);
        return NULL;
    }
    stream->file = file;
    stream->length = length >= 0 ? length : 0;
    stream->xwb_metadata_end =
        xwb_music_metadata_end(file, relative, stream->length);
    /* SFXPack.pbn (318MB): AudioManager.LoadSFXPack copia o pack INTEIRO pra
     * um MemoryStream (contiguo, com doubling ~2x) — OOM fatal em 832MB de
     * RAM. Capar o stream trunca a copia; o parse falha no corte e o catch
     * interno faz sounds.Clear() e retorna limpo (jogo segue SEM SFX).
     * SB_SFXPACK_LIMIT=bytes ajusta; 0 desliga o cap. */
    if (strstr(relative, "SFXPack.pbn") && stream->length > (100l << 20)) {
        /* fallback: pack ORIGINAL (318MB) sem o .small do lado — capar evita
         * o OOM (sem SFX e com o loading preso no progresso; gerar o .small
         * e o caminho certo). */
        const char *lim = getenv("SB_SFXPACK_LIMIT");
        long cap = lim ? atol(lim) : (4l << 20);
        if (cap > 0 && stream->length > cap) {
            stream->read_limit = cap;
            fprintf(stderr, "[asset] %s capado a %ld bytes (RAM; sem SFX)\n",
                    relative, cap);
        }
    }
    if (asset_verbose()) {
        stream->path = strdup(full);
        stream->read_hash = UINT64_C(1469598103934665603);
        opened++;
        if (opened <= 30 || opened % 500 == 0)
            fprintf(stderr, "[asset] open #%u %s (%ld bytes)\n",
                    opened, relative, stream->length);
    }
    return stream;
}

static void asset_stream_close(void *obj) {
    struct fake_stream *stream = fake_object_payload(obj);
    if (!stream) return;
    if (asset_verbose())
        fprintf(stderr, "[asset-close] %s reads=%u bytes=%llu hash=%016llx\n",
                stream->path ? stream->path : "?", stream->reads,
                (unsigned long long)stream->read_total,
                (unsigned long long)stream->read_hash);
    if (stream->file) fclose(stream->file);
    free(stream->path);
    free(stream);
    fake_object_set_payload(obj, NULL);
}

static int asset_stream_read_byte(void *obj) {
    struct fake_stream *stream = fake_object_payload(obj);
    if (!stream || !stream->file) return -1;
    long position = ftell(stream->file);
    if (position < 0 || position >= asset_stream_logical_end(stream))
        return -1;
    int value = fgetc(stream->file);
    return value == EOF ? -1 : (value & 0xff);
}

static int asset_stream_read_array(void *obj, void *array, int offset, int count) {
    struct fake_stream *stream = fake_object_payload(obj);
    struct fake_array *bytes = fake_array_find(array);
    if (!stream || !stream->file || !bytes || bytes->kind != FAKE_ARRAY_BYTE ||
        offset < 0 || count < 0 || offset > bytes->len || count > bytes->len - offset)
        return -1;
    if (count == 0) return 0;

    long position = ftell(stream->file);
    if (position < 0) return -1;

    /* Stream.CopyTo pede 81920 bytes ao ArrayPool; o array alugado pode ser o
     * bucket seguinte (normalmente 131072), e Stream.Read recebe Length. A
     * worker streaming abre outro handle e pula o offset com blocos de 0x8000,
     * portanto pos=0/count>=81920 ainda identifica sem contador global apenas
     * a copia gigante feita durante o ctor do banco. */
    if (!stream->read_limit && stream->xwb_metadata_end > 0 &&
        position == 0 && offset == 0 && count >= 81920) {
        stream->read_limit = stream->xwb_metadata_end;
        fprintf(stderr,
                "[asset-xact] music CopyTo(%d) limitado: %ld/%ld bytes\n",
                count, stream->read_limit, stream->length);
    }

    /* WaveBank streaming reabre music.xwb e, como o wrapper Android declara
     * CanSeek=false, descarta bytes em blocos de ate 0x8000 antes de ler a
     * faixa. Esses bytes nunca sao inspecionados. Convertemos somente essa
     * fase de descarte em seek fisico; a primeira leitura curta encerra o
     * skip, e todas as leituras seguintes (payload real) continuam em fread.
     * Os 137 offsets deste banco foram validados: nenhum limite de payload e
     * multiplo de 0x8000, logo sempre existe o bloco final curto. */
    if (!stream->read_limit && stream->xwb_metadata_end > 0 &&
        stream->xwb_skip_phase == 0 && position == 0 && offset == 0 &&
        count > 0 && count <= 0x8000) {
        stream->xwb_skip_phase = 1;
    }
    if (stream->xwb_skip_phase == 1) {
        long physical_remaining = stream->length - position;
        if ((long)count <= physical_remaining &&
            fseek(stream->file, (long)count, SEEK_CUR) == 0) {
            stream->xwb_fast_skipped += (uint64_t)count;
            if (count < 0x8000) {
                stream->xwb_skip_phase = 2;
                fprintf(stderr,
                        "[asset-xact] music seek rapido: %.1f MiB descartados\n",
                        (double)stream->xwb_fast_skipped / (1024.0 * 1024.0));
            }
            return count;
        }
        /* Se o seek falhar, preserve a semantica original por fread. */
        stream->xwb_skip_phase = 2;
    }

    long logical_end = asset_stream_logical_end(stream);
    if (position >= logical_end)
        return -1;
    int requested = count;
    long remaining = logical_end - position;
    if ((long)count > remaining)
        count = (int)remaining;
    size_t got = fread(bytes->data.bytes + offset, 1, (size_t)count, stream->file);
    if (asset_verbose()) {
        stream->reads++;
        for (size_t i = 0; i < got; i++) {
            stream->read_hash ^= (unsigned char)bytes->data.bytes[offset + (int)i];
            stream->read_hash *= UINT64_C(1099511628211);
        }
        stream->read_total += got;
        if (stream->reads <= 4)
            fprintf(stderr, "[asset-read] %s call=%u array=%d off=%d want=%d got=%zu pos=%ld\n",
                    stream->path ? stream->path : "?", stream->reads, bytes->len,
                    offset, requested, got, ftell(stream->file));
    }
    return got ? (int)got : -1;
}

static int asset_stream_available(void *obj) {
    struct fake_stream *stream = fake_object_payload(obj);
    if (!stream || !stream->file) return 0;
    long position = ftell(stream->file);
    long remaining = position >= 0
        ? asset_stream_logical_end(stream) - position : 0;
    if (remaining < 0) remaining = 0;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static int64_t asset_stream_skip(void *obj, int64_t count) {
    struct fake_stream *stream = fake_object_payload(obj);
    if (!stream || !stream->file || count <= 0) return 0;
    long before = ftell(stream->file);
    if (before < 0) return 0;
    int64_t remaining = asset_stream_logical_end(stream) - before;
    if (remaining <= 0) return 0;
    if (count > remaining) count = remaining;
    if (count > LONG_MAX || fseek(stream->file, (long)count, SEEK_CUR) != 0)
        return 0;
    return count;
}

/* ---- MediaExtractor/MediaCodec PCM bridge ----------------------------- */
static int audio_trace(void) {
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("SB_AUDIO_TRACE");
        enabled = value && value[0] && value[0] != '0';
        initialized = 1;
    }
    return enabled;
}

static void sb_sndfile_init(void) {
    g_sndfile_lib = dlopen("libsndfile.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_sndfile_lib)
        g_sndfile_lib = dlopen("libsndfile.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_sndfile_lib) {
        fprintf(stderr, "[android-audio] libsndfile indisponivel: %s\n",
                dlerror());
        return;
    }
    *(void **)(&g_sf_open) = dlsym(g_sndfile_lib, "sf_open");
    *(void **)(&g_sf_readf_short) =
        dlsym(g_sndfile_lib, "sf_readf_short");
    *(void **)(&g_sf_seek) = dlsym(g_sndfile_lib, "sf_seek");
    *(void **)(&g_sf_close) = dlsym(g_sndfile_lib, "sf_close");
    if (!g_sf_open || !g_sf_readf_short || !g_sf_seek || !g_sf_close) {
        fprintf(stderr, "[android-audio] API libsndfile incompleta\n");
        dlclose(g_sndfile_lib);
        g_sndfile_lib = NULL;
        g_sf_open = NULL;
        g_sf_readf_short = NULL;
        g_sf_seek = NULL;
        g_sf_close = NULL;
    }
}

/* ---- marshal methods (.NET for Android 9+) ----
 *
 * Esta build nao chama mono.android.Runtime.register(): o libxamarin-app.so
 * exporta o corpo nativo de cada metodo do ACW com o nome JNI curto. Para
 * achar um handler basta montar o nome e procurar no snapshot do modulo.
 *
 *   crc64fb467d66f1819016.PlayGamesManager_AuthListener + n_onComplete
 *   -> Java_crc64fb467d66f1819016_PlayGamesManager_1AuthListener_n_1onComplete
 *
 * Regra de escape JNI: '.'/'/' viram '_' e '_' vira '_1'. Procuramos por
 * PREFIXO para nao ter de reproduzir tambem o sufixo de sobrecarga (`__` mais
 * a assinatura mangled), que so existe quando ha ambiguidade. */
void *sb_xam_sym_prefix(const char *prefix);

static void *jni_marshal_method(const char *class_dot, const char *n_name) {
    char prefix[512];
    size_t k = 0;
    const char *lead = "Java_";
    if (!class_dot || !n_name) return NULL;
    for (const char *p = lead; *p && k + 8 < sizeof prefix; p++) prefix[k++] = *p;
    for (const char *p = class_dot; *p && k + 8 < sizeof prefix; p++) {
        if (*p == '.' || *p == '/') prefix[k++] = '_';
        else if (*p == '_') { prefix[k++] = '_'; prefix[k++] = '1'; }
        else prefix[k++] = *p;
    }
    if (k + 8 < sizeof prefix) prefix[k++] = '_';
    for (const char *p = n_name; *p && k + 8 < sizeof prefix; p++) {
        if (*p == '_') { prefix[k++] = '_'; prefix[k++] = '1'; }
        else prefix[k++] = *p;
    }
    prefix[k] = 0;
    return sb_xam_sym_prefix(prefix);
}

/* Handler de um objeto fake: usa a classe registrada nele. */
static void *jni_marshal_for_object(void *obj, const char *n_name);

static int fake_object_is_class(void *obj, const char *dot_name) {
    const char *name = fake_class_name(fake_object_class(obj));
    return name && dot_name && strcmp(name, dot_name) == 0;
}

static void *jni_marshal_for_object(void *obj, const char *n_name) {
    const char *name = fake_class_name(fake_object_class(obj));
    if (!name) return NULL;
    return jni_marshal_method(name, n_name);
}

static void *audio_payload(void *obj, uint32_t magic) {
    void *payload = fake_object_payload(obj);
    if (!payload || (uintptr_t)payload < UINT64_C(0x100000000)) return NULL;
    return *(const uint32_t *)payload == magic ? payload : NULL;
}

static char *audio_asset_path(const char *asset_name) {
    const char *root = getenv("SB_ASSET_DIR");
    char relative[2048];
    char full[4096];
    if (!root || !*root) root = "/storage/roms/ports/blossomtales/assets";
    if (!asset_name || !*asset_name || asset_name[0] == '/' ||
        strstr(asset_name, ".."))
        return NULL;
    size_t length = strlen(asset_name);
    if (length >= sizeof(relative)) return NULL;
    for (size_t i = 0; i <= length; ++i)
        relative[i] = asset_name[i] == '\\' ? '/' : asset_name[i];
    if (snprintf(full, sizeof(full), "%s/%s", root, relative) >=
        (int)sizeof(full))
        return NULL;
    return strdup(full);
}

static void *audio_open_fd(const char *asset_name) {
    char *path = audio_asset_path(asset_name);
    if (!path || access(path, R_OK) != 0) {
        fprintf(stderr, "[android-audio] OpenFd MISS: %s\n",
                path ? path : (asset_name ? asset_name : "?"));
        free(path);
        return NULL;
    }
    struct fake_audio_fd *fd = calloc(1, sizeof(*fd));
    if (!fd) {
        free(path);
        return NULL;
    }
    fd->magic = AUDIO_FD_MAGIC;
    fd->path = path;
    void *object = jni_make_object(
        jni_make_class("android.content.res.AssetFileDescriptor"));
    fake_object_set_payload(object, fd);
    if (audio_trace())
        fprintf(stderr, "[android-audio] OpenFd %s\n", asset_name);
    return object;
}

static void audio_close_fd(void *obj) {
    struct fake_audio_fd *fd = audio_payload(obj, AUDIO_FD_MAGIC);
    if (!fd) return;
    free(fd->path);
    free(fd);
    fake_object_set_payload(obj, NULL);
}

static void audio_extractor_set_source(void *obj, void *fd_object) {
    struct fake_audio_fd *fd = audio_payload(fd_object, AUDIO_FD_MAGIC);
    if (!fd || !fd->path) return;
    pthread_once(&g_sndfile_once, sb_sndfile_init);

    struct fake_audio_extractor *old =
        audio_payload(obj, AUDIO_EXTRACTOR_MAGIC);
    if (old) {
        if (old->snd && g_sf_close) g_sf_close(old->snd);
        free(old->path);
        free(old);
    }
    struct fake_audio_extractor *extractor = calloc(1, sizeof(*extractor));
    if (!extractor) return;
    extractor->magic = AUDIO_EXTRACTOR_MAGIC;
    extractor->path = strdup(fd->path);
    if (g_sf_open)
        extractor->snd = g_sf_open(extractor->path, 0x10 /* SFM_READ */,
                                   &extractor->info);
    if (!extractor->snd) {
        fprintf(stderr, "[android-audio] falha abrindo OGG: %s\n",
                extractor->path);
        extractor->eof = 1;
        extractor->info.samplerate = 48000;
        extractor->info.channels = 2;
    } else {
        fprintf(stderr,
                "[android-audio] OGG pronto: %s (%d Hz, %d ch, %.1f s)\n",
                strrchr(extractor->path, '/')
                    ? strrchr(extractor->path, '/') + 1 : extractor->path,
                extractor->info.samplerate, extractor->info.channels,
                extractor->info.samplerate > 0
                    ? (double)extractor->info.frames /
                          extractor->info.samplerate
                    : 0.0);
    }
    fake_object_set_payload(obj, extractor);
}

static void *audio_get_track_format(void *extractor_object) {
    struct fake_audio_extractor *extractor =
        audio_payload(extractor_object, AUDIO_EXTRACTOR_MAGIC);
    if (!extractor) return NULL;
    void *format = jni_make_object(jni_make_class("android.media.MediaFormat"));
    fake_object_set_payload(format, extractor);
    return format;
}

static void *audio_format_get_string(void *format_object, const char *key) {
    struct fake_audio_extractor *extractor =
        audio_payload(format_object, AUDIO_EXTRACTOR_MAGIC);
    if (!extractor || !key) return NULL;
    if (strcmp(key, "mime") == 0) return strdup("audio/vorbis");
    return strdup("");
}

static int audio_format_get_integer(void *format_object, const char *key) {
    struct fake_audio_extractor *extractor =
        audio_payload(format_object, AUDIO_EXTRACTOR_MAGIC);
    if (!extractor || !key) return 0;
    if (strcmp(key, "sample-rate") == 0) return extractor->info.samplerate;
    if (strcmp(key, "channel-count") == 0) return extractor->info.channels;
    return 0;
}

static void *audio_create_codec(void) {
    struct fake_audio_codec *codec = calloc(1, sizeof(*codec));
    if (!codec) return NULL;
    codec->magic = AUDIO_CODEC_MAGIC;
    codec->input.magic = AUDIO_BUFFER_MAGIC;
    codec->input.capacity = 64u * 1024u;
    codec->input.data = malloc(codec->input.capacity);
    codec->output.magic = AUDIO_BUFFER_MAGIC;
    codec->output.capacity = 256u * 1024u;
    codec->output.data = malloc(codec->output.capacity);
    if (!codec->input.data || !codec->output.data) {
        free(codec->input.data);
        free(codec->output.data);
        free(codec);
        return NULL;
    }
    void *object = jni_make_object(jni_make_class("android.media.MediaCodec"));
    fake_object_set_payload(object, codec);
    return object;
}

static void audio_codec_configure(void *codec_object, void *format_object) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    struct fake_audio_extractor *extractor =
        audio_payload(format_object, AUDIO_EXTRACTOR_MAGIC);
    if (!codec) return;
    codec->extractor = extractor;
}

static void *audio_codec_buffer(void *codec_object, int output) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    if (!codec) return NULL;
    struct fake_audio_buffer *buffer = output ? &codec->output : &codec->input;
    void **slot = output ? &codec->output_object : &codec->input_object;
    if (!*slot) {
        *slot = jni_make_object(jni_make_class("java.nio.ByteBuffer"));
        fake_object_set_payload(*slot, buffer);
    }
    return *slot;
}

static int audio_dequeue_input(void *codec_object) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    if (!codec || !codec->extractor || codec->input_queued) return -1;
    codec->input.size = 0;
    codec->input.position = 0;
    return 0;
}

static int audio_read_sample_data(void *extractor_object, void *buffer_object,
                                  int offset) {
    struct fake_audio_extractor *extractor =
        audio_payload(extractor_object, AUDIO_EXTRACTOR_MAGIC);
    struct fake_audio_buffer *buffer =
        audio_payload(buffer_object, AUDIO_BUFFER_MAGIC);
    if (!extractor || !buffer || extractor->eof || !extractor->snd)
        return -1;
    if (offset < 0 || (size_t)offset >= buffer->capacity) return -1;
    /* O payload comprimido nao passa pelo codec fake; um byte apenas sinaliza
     * ao loop gerenciado que existe uma amostra para enfileirar. */
    buffer->data[offset] = 0;
    buffer->size = (size_t)offset + 1;
    buffer->position = 0;
    return 1;
}

static void audio_queue_input(void *codec_object, int size, int flags) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    if (!codec) return;
    codec->input_queued = 1;
    codec->input_eos = (flags & 4) != 0 || size <= 0;
}

static struct fake_audio_buffer_info *audio_buffer_info(void *info_object) {
    struct fake_audio_buffer_info *info =
        audio_payload(info_object, AUDIO_INFO_MAGIC);
    if (info) return info;
    info = calloc(1, sizeof(*info));
    if (!info) return NULL;
    info->magic = AUDIO_INFO_MAGIC;
    fake_object_set_payload(info_object, info);
    return info;
}

static int audio_dequeue_output(void *codec_object, void *info_object) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    struct fake_audio_buffer_info *info = audio_buffer_info(info_object);
    if (!codec || !info || !codec->extractor || !codec->input_queued)
        return -1;
    info->flags = 0;
    info->size = 0;
    codec->output.size = 0;
    codec->output.position = 0;
    if (codec->input_eos) {
        info->flags = 4; /* BUFFER_FLAG_END_OF_STREAM */
        return 0;
    }

    struct fake_audio_extractor *extractor = codec->extractor;
    int channels = extractor->info.channels;
    if (!extractor->snd || channels <= 0) {
        extractor->eof = 1;
        codec->input_queued = 0;
        return -1;
    }
    sf_count_t frames_capacity =
        (sf_count_t)(codec->output.capacity / ((size_t)channels * 2u));
    sf_count_t frames = g_sf_readf_short
        ? g_sf_readf_short(extractor->snd, (short *)codec->output.data,
                           frames_capacity)
        : 0;
    if (frames <= 0) {
        extractor->eof = 1;
        codec->input_queued = 0;
        return -1;
    }
    codec->output.size = (size_t)frames * (size_t)channels * 2u;
    info->size = (int)codec->output.size;
    codec->decoded_bytes += codec->output.size;
    if (audio_trace() && codec->decoded_bytes == codec->output.size)
        fprintf(stderr, "[android-audio] primeiro PCM: %zu bytes\n",
                codec->output.size);
    return 0;
}

static void audio_release_output(void *codec_object) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    if (!codec) return;
    codec->input_queued = 0;
    codec->input_eos = 0;
    codec->output.size = 0;
    codec->output.position = 0;
}

static void audio_seek(void *extractor_object, int64_t position_us) {
    struct fake_audio_extractor *extractor =
        audio_payload(extractor_object, AUDIO_EXTRACTOR_MAGIC);
    if (!extractor || !extractor->snd || !g_sf_seek) return;
    sf_count_t frame = 0;
    if (position_us > 0 && extractor->info.samplerate > 0)
        frame = (sf_count_t)((position_us * extractor->info.samplerate) /
                             INT64_C(1000000));
    g_sf_seek(extractor->snd, frame, SEEK_SET);
    extractor->eof = 0;
}

static int audio_buffer_remaining(void *buffer_object) {
    struct fake_audio_buffer *buffer =
        audio_payload(buffer_object, AUDIO_BUFFER_MAGIC);
    if (!buffer || buffer->position >= buffer->size) return 0;
    size_t remaining = buffer->size - buffer->position;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static void *audio_buffer_get(void *buffer_object, void *array_object,
                              int offset, int length) {
    struct fake_audio_buffer *buffer =
        audio_payload(buffer_object, AUDIO_BUFFER_MAGIC);
    struct fake_array *array = fake_array_find(array_object);
    if (!buffer || !array || array->kind != FAKE_ARRAY_BYTE || offset < 0 ||
        length < 0 || offset > array->len || length > array->len - offset)
        return buffer_object;
    size_t remaining = buffer->position < buffer->size
        ? buffer->size - buffer->position : 0;
    if ((size_t)length > remaining) length = (int)remaining;
    memcpy(array->data.bytes + offset, buffer->data + buffer->position,
           (size_t)length);
    buffer->position += (size_t)length;
    return buffer_object;
}

static int audio_buffer_flags(void *info_object) {
    struct fake_audio_buffer_info *info =
        audio_payload(info_object, AUDIO_INFO_MAGIC);
    return info ? info->flags : 0;
}

static void audio_codec_release(void *codec_object) {
    struct fake_audio_codec *codec = audio_payload(codec_object,
                                                   AUDIO_CODEC_MAGIC);
    if (!codec) return;
    if (codec->extractor && codec->extractor->snd && g_sf_close) {
        g_sf_close(codec->extractor->snd);
        codec->extractor->snd = NULL;
    }
    if (audio_trace())
        fprintf(stderr, "[android-audio] decoder liberado: %.1f MiB PCM\n",
                (double)codec->decoded_bytes / (1024.0 * 1024.0));
    free(codec->input.data);
    free(codec->output.data);
    free(codec);
    fake_object_set_payload(codec_object, NULL);
}

/* AssetManager.list(path): nomes (nao paths) dos filhos diretos em
 * SB_ASSET_DIR/path, ordenados (semantica do AssetManager real; path
 * inexistente ou arquivo -> array vazio). jstring = char* strdup'd, mesmo
 * modelo do NewStringUTF. ParisContentManager.FileExists/ListFiles dependem
 * disso — com o default antigo (jstring solta) o managed via 0 entradas. */
static void *asset_dir_list(const char *asset_name) {
    const char *root = getenv("SB_ASSET_DIR");
    char relative[2048];
    char full[4096];

    if (!root || !*root) root = "/storage/roms/ports/blossomtales/assets";
    if (!asset_name) asset_name = "";
    if (asset_name[0] == '/' || strstr(asset_name, ".."))
        return fake_array_new(FAKE_ARRAY_OBJECT, 0);
    size_t n = strlen(asset_name);
    if (n >= sizeof(relative))
        return fake_array_new(FAKE_ARRAY_OBJECT, 0);
    for (size_t i = 0; i <= n; i++)
        relative[i] = asset_name[i] == '\\' ? '/' : asset_name[i];
    if (snprintf(full, sizeof(full), "%s/%s", root, relative) >= (int)sizeof(full))
        return fake_array_new(FAKE_ARRAY_OBJECT, 0);

    struct dirent **entries = NULL;
    int count = scandir(full, &entries, NULL, alphasort);
    if (count < 0) {
        if (asset_verbose())
            fprintf(stderr, "[asset-list] MISS %s\n", full);
        return fake_array_new(FAKE_ARRAY_OBJECT, 0);
    }
    int kept = 0;
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i]->d_name, ".") != 0 &&
            strcmp(entries[i]->d_name, "..") != 0)
            kept++;
    struct fake_array *a = fake_array_new(FAKE_ARRAY_OBJECT, kept);
    int j = 0;
    for (int i = 0; i < count; i++) {
        if (a && j < kept && strcmp(entries[i]->d_name, ".") != 0 &&
            strcmp(entries[i]->d_name, "..") != 0)
            a->data.objects[j++] = strdup(entries[i]->d_name);
        free(entries[i]);
    }
    free(entries);
    if (asset_verbose())
        fprintf(stderr, "[asset-list] %s -> %d entradas\n",
                relative[0] ? relative : "(raiz)", kept);
    return a;
}

/* ---- Netflix Games SDK fake (offline) -----------------------------------
 * O engine Paris gateia o boot em RequestPlayerAccess(ICallback): so limpa
 * _requestProcessing quando onResult entrega um NetflixResult cujo getData e
 * instancia (typemap por nome de classe Java!) de PlayerAccessInfo. Criamos
 * o par result+data com as classes Java EXATAS dos [Register] do Netflix.dll. */
static void *netflix_result_with(const char *data_class_dot) {
    void *result = jni_make_object(jni_make_class("com.netflix.games.NetflixResult"));
    void *data = data_class_dot
        ? jni_make_object(jni_make_class(data_class_dot)) : NULL;
    fake_object_set_payload(result, data);
    return result;
}

void *jni_find_object(const char *dot_class_name) {
    void *klass = jni_make_class(dot_class_name);
    void *result = NULL;
    pthread_mutex_lock(&g_fake_objects_lock);
    for (int i = g_fake_objects_n - 1; i >= 0; i--)
        if (g_fake_objects[i].klass == klass) {
            result = &g_fake_objects[i];
            break;
        }
    pthread_mutex_unlock(&g_fake_objects_lock);
    return result;
}

void jni_set_activity(void *activity) { g_activity = activity; }
void *jni_activity(void) { return g_activity; }
void jni_set_main_looper_ready(int ready) { g_main_looper_ready = !!ready; }

static void *main_looper(void) {
    if (!g_main_looper)
        g_main_looper = jni_make_object(jni_make_class("android.os.Looper"));
    return g_main_looper;
}

/* Diretorio de libs do app (nativeLibraryDir) — devolvido como unico elemento
 * do array fake libFolders (token 0x4001). Default vazio ate jni_set_libdir. */
int sb_aaudio_available(void);

#define LIBFOLDERS_TOKEN ((void *)0x4001)
static char *g_libdir = NULL;
void jni_set_libdir(const char *path) { g_libdir = path ? strdup(path) : NULL; }

/* appDirs[0]/[1] sao dataDir e cacheDir. Devolver o dir das LIBS neles faz o
 * IsolatedStorage do .NET gravar o save DENTRO de libs/ -- ou seja, dentro do
 * payload selado pelo NXExtract: o marker do segundo boot passa a ser
 * recusado por "payload content seal mismatch" e uma reinstalacao de dados
 * levaria o save junto. O dir de dados e' separado. */
static char *g_datadir = NULL;
static char *g_cachedir = NULL;
void jni_set_datadir(const char *path) {
    if (!path || !*path) return;
    g_datadir = strdup(path);
    size_t n = strlen(path) + sizeof("/cache");
    g_cachedir = (char *)malloc(n);
    if (g_cachedir) snprintf(g_cachedir, n, "%s/cache", path);
}

/* runtimeApks (token 0x4002): lista de APKs que o monodroid abre por mmap para
 * localizar assemblies/assemblies.blob. Neste jogo o assembly store nao vem em
 * libs (arquivos .so), e sim dentro do proprio APK. */
#define APKS_TOKEN ((void *)0x4002)
static char *g_apk_path = NULL;
void jni_set_apk_path(const char *path) { g_apk_path = path ? strdup(path) : NULL; }

/* Registro de metodos/campos: nome -> id incremental. IDs sao pequenos ints
 * nao-nulos (jmethodID/jfieldID). */
#define MAX_REG 1024
static char *reg_names[MAX_REG];
static char *reg_sigs[MAX_REG];
static int   reg_n = 0;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static int reg_id(const char *kind, const char *name, const char *sig) {
    if (!name) return 1;
    pthread_mutex_lock(&g_reg_lock);
    for (int i = 0; i < reg_n; i++)
        if (reg_names[i] && strcmp(reg_names[i], name) == 0 &&
            ((!reg_sigs[i] && !sig) ||
             (reg_sigs[i] && sig && strcmp(reg_sigs[i], sig) == 0))) {
            pthread_mutex_unlock(&g_reg_lock);
            return i + 1;
        }
    if (reg_n < MAX_REG) {
        reg_names[reg_n] = strdup(name);
        reg_sigs[reg_n] = sig ? strdup(sig) : NULL;
        if (g_verbose) fprintf(stderr, "[jni] %s id=%d  %s  %s\n",
                               kind, reg_n + 1, name, sig ? sig : "");
        int result = ++reg_n;
        pthread_mutex_unlock(&g_reg_lock);
        return result;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return 1;
}

/* ---- handlers basicos --------------------------------------------------- */
static void *ret0(void) { return NULL; }
/* Default pra slots JNI NAO populados: devolve um sentinel nao-NULL em vez de
 * NULL. Mono trata muitos retornos como objetos/strings; NULL vira ponteiro NULL
 * pra strlen/strcmp e crasha o SIMD. Sentinel nao-nulo deixa o boot avancar. */
static void *ret_obj(void) { return (void *)0xC1A501; }

static void *FindClass(void *e, const char *name) {
    (void)e;
    if (g_verbose) fprintf(stderr, "[jni] FindClass(%s)\n", name ? name : "?");
    return jni_make_class(name);
}

static void *GetMethodID(void *e, void *cls, const char *name, const char *sig) {
    (void)cls;
    return (void *)(long)reg_id("GetMethodID", name, sig);
}
static void *GetStaticMethodID(void *e, void *cls, const char *name, const char *sig) {
    (void)cls;
    return (void *)(long)reg_id("GetStaticMethodID", name, sig);
}
static void *GetFieldID(void *e, void *cls, const char *name, const char *sig) {
    (void)e; (void)cls;
    return (void *)(long)reg_id("GetFieldID", name, sig);
}
static void *GetStaticFieldID(void *e, void *cls, const char *name, const char *sig) {
    (void)e; (void)cls;
    return (void *)(long)reg_id("GetStaticFieldID", name, sig);
}

/* strings: alocadas no heap, nunca liberadas (simples, suficiente pro boot) */
static const char *safe_cstr(void *str) {
    /* Handles JNI fake pequenos (0x4000/0xC1A500/0xC1A501) nao apontam para
     * memoria. Java.Interop pode tentar formata-los ao construir uma excecao;
     * nesse caso uma string vazia preserva o erro original. */
    if (!str || (uintptr_t)str < 0x100000000ULL) return "";
    return (const char *)str;
}
static void *NewStringUTF(void *e, const char *str) {
    (void)e;
    return str ? strdup(str) : strdup("");
}
static void *NewString(void *e, const unsigned short *chars, int len) {
    (void)e;
    if (!chars || len <= 0) return strdup("");
    char *s = malloc((size_t)len + 1);
    if (!s) return NULL;
    for (int i = 0; i < len; i++)
        s[i] = chars[i] <= 0x7f ? (char)chars[i] : '?';
    s[len] = '\0';
    return s;
}
static const char *GetStringUTFChars(void *e, void *str, unsigned char *isCopy) {
    (void)e;
    if (isCopy) *isCopy = 0;
    const char *r = safe_cstr(str);
    if (g_verbose) fprintf(stderr, "[jni] GetStringUTFChars(str=%p) -> \"%s\"\n", str, r);
    return r;
}
static void ReleaseStringUTFChars(void *e, void *str, const char *chars) {
    (void)e; (void)str; (void)chars;
}
static int GetStringUTFLength(void *e, void *str) {
    (void)e; return (int)strlen(safe_cstr(str));
}

/* UTF-16 string access (idx 164/165/166). Nossas "jstring" sao char* C (ASCII —
 * nomes de classe tipo "java.lang.Object"). Java.Interop.Strings.ToString usa a
 * via UTF-16 (GetStringChars), NAO a UTF-8. Sem isso o slot cai no default
 * (sentinel 0xC1A501) e o managed faz new string((jchar*)0xC1A501, len) ->
 * memcpy -> SIGSEGV. Convertemos byte->jchar (zero-extend, valido p/ ASCII). */
static int GetStringLength(void *e, void *str) {
    (void)e; return (int)strlen(safe_cstr(str));
}
static const unsigned short *GetStringChars(void *e, void *str, unsigned char *isCopy) {
    (void)e;
    const char *s = safe_cstr(str);
    size_t n = strlen(s);
    unsigned short *buf = malloc((n + 1) * sizeof(unsigned short));
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)s[i];
    buf[n] = 0;
    if (isCopy) *isCopy = 1;
    return buf;
}
static void ReleaseStringChars(void *e, void *str, const unsigned short *chars) {
    (void)e; (void)str; free((void *)chars);
}

/* refs: identidade (o objeto "global" e o mesmo ponteiro) */
static void *NewGlobalRef(void *e, void *obj) {
    (void)e; fake_array_add_ref(obj); return obj;
}
static void *NewLocalRef(void *e, void *obj) {
    (void)e; fake_array_add_ref(obj); return obj;
}
static void DeleteGlobalRef(void *e, void *obj) {
    (void)e; fake_array_release(obj);
}
static void DeleteLocalRef(void *e, void *obj) {
    (void)e; fake_array_release(obj);
}

/* Arrays Java reais o bastante para os bindings EGL10. Java.Interop copia
 * int[] gerenciados via NewIntArray/SetIntArrayRegion e le os out params com
 * GetIntArrayRegion; EGLConfig[] usa a familia ObjectArray. */
static void *NewObjectArray(void *e, int len, void *cls, void *init) {
    (void)e; (void)cls;
    struct fake_array *a = fake_array_new(FAKE_ARRAY_OBJECT, len);
    if (!a) return NULL;
    a->element_class = cls;
    for (int i = 0; i < len; i++) a->data.objects[i] = init;
    return a;
}
static void SetObjectArrayElement(void *e, void *array, int i, void *value) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (a && a->kind == FAKE_ARRAY_OBJECT && i >= 0 && i < a->len)
        a->data.objects[i] = value;
}
static int GetArrayLength(void *e, void *array) {
    (void)e;
    /* EXPERIMENTO: setup_app_library_directories so processa libFolders se
     * [wrapper+0x10] (count) > 2 (cmp x9,2; b.ls skip). Devolvemos 3 p/ forcar
     * o branch e popular app_lib_directories. GetObjectArrayElement devolve o
     * mesmo path p/ qualquer indice. */
    struct fake_array *a = fake_array_find(array);
    int n = a ? a->len : ((array == LIBFOLDERS_TOKEN && g_libdir) ? 3 :
                          ((array == APKS_TOKEN && g_apk_path) ? 1 : 0));
    if (g_verbose) fprintf(stderr, "[jni] GetArrayLength(array=%p) -> %d\n", array, n);
    return n;
}
static void *GetObjectArrayElement(void *e, void *array, int i) {
    (void)e;
    if (array == APKS_TOKEN && g_apk_path) {
        if (g_verbose) fprintf(stderr, "[jni] GetObjectArrayElement(apks, %d) -> \"%s\"\n", i, g_apk_path);
        return g_apk_path;
    }
    if (array == LIBFOLDERS_TOKEN && g_libdir) {
        /* appDirs = [dataDir, cacheDir, nativeLibraryDir] */
        const char *pick = g_libdir;
        if (i == 0 && g_datadir) pick = g_datadir;
        else if (i == 1 && g_cachedir) pick = g_cachedir;
        if (g_verbose) fprintf(stderr, "[jni] GetObjectArrayElement(libFolders, %d) -> \"%s\"\n", i, pick);
        return (void *)pick;   /* jstring = proprio path C (GetStringUTFChars trata) */
    }
    struct fake_array *a = fake_array_find(array);
    if (a && a->kind == FAKE_ARRAY_OBJECT && i >= 0 && i < a->len)
        return a->data.objects[i];
    if (g_verbose) fprintf(stderr, "[jni] GetObjectArrayElement(array=%p, %d) -> NULL\n", array, i);
    return NULL;
}

static void *NewIntArray(void *e, int len) {
    (void)e;
    return fake_array_new(FAKE_ARRAY_INT, len);
}
static void *NewBooleanArray(void *e, int len) {
    (void)e;
    return fake_array_new(FAKE_ARRAY_BOOLEAN, len);
}
static unsigned char *GetBooleanArrayElements(void *e, void *array,
                                               unsigned char *isCopy) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (isCopy) *isCopy = 0;
    return (a && a->kind == FAKE_ARRAY_BOOLEAN) ? a->data.booleans : NULL;
}
static void ReleaseBooleanArrayElements(void *e, void *array,
                                        unsigned char *elems, int mode) {
    (void)e; (void)array; (void)elems; (void)mode;
}
static int *GetIntArrayElements(void *e, void *array, unsigned char *isCopy) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (isCopy) *isCopy = 0;
    return (a && a->kind == FAKE_ARRAY_INT) ? a->data.ints : NULL;
}
static void ReleaseIntArrayElements(void *e, void *array, int *elems, int mode) {
    (void)e; (void)array; (void)elems; (void)mode;
}
static void *NewByteArray(void *e, int len) {
    (void)e;
    return fake_array_new(FAKE_ARRAY_BYTE, len);
}
static signed char *GetByteArrayElements(void *e, void *array, unsigned char *isCopy) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (isCopy) *isCopy = 0;
    return (a && a->kind == FAKE_ARRAY_BYTE) ? a->data.bytes : NULL;
}
static void ReleaseByteArrayElements(void *e, void *array, signed char *elems, int mode) {
    (void)e; (void)array; (void)elems; (void)mode;
}
static void GetIntArrayRegion(void *e, void *array, int start, int len, int *buf) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (!a || a->kind != FAKE_ARRAY_INT || !buf || start < 0 || len < 0 ||
        start + len > a->len) return;
    memcpy(buf, a->data.ints + start, (size_t)len * sizeof(int));
}
static void GetBooleanArrayRegion(void *e, void *array, int start, int len,
                                  unsigned char *buf) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (!a || a->kind != FAKE_ARRAY_BOOLEAN || !buf || start < 0 || len < 0 ||
        start + len > a->len) return;
    memcpy(buf, a->data.booleans + start, (size_t)len);
}
static void SetBooleanArrayRegion(void *e, void *array, int start, int len,
                                  const unsigned char *buf) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (!a || a->kind != FAKE_ARRAY_BOOLEAN || !buf || start < 0 || len < 0 ||
        start + len > a->len) return;
    memcpy(a->data.booleans + start, buf, (size_t)len);
}
static void SetIntArrayRegion(void *e, void *array, int start, int len, const int *buf) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (!a || a->kind != FAKE_ARRAY_INT || !buf || start < 0 || len < 0 ||
        start + len > a->len) return;
    memcpy(a->data.ints + start, buf, (size_t)len * sizeof(int));
}
static void GetByteArrayRegion(void *e, void *array, int start, int len, signed char *buf) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (!a || a->kind != FAKE_ARRAY_BYTE || !buf || start < 0 || len < 0 ||
        start + len > a->len) return;
    memcpy(buf, a->data.bytes + start, (size_t)len);
}
static void SetByteArrayRegion(void *e, void *array, int start, int len,
                               const signed char *buf) {
    (void)e;
    struct fake_array *a = fake_array_find(array);
    if (!a || a->kind != FAKE_ARRAY_BYTE || !buf || start < 0 || len < 0 ||
        start + len > a->len) return;
    memcpy(a->data.bytes + start, buf, (size_t)len);
}
static void *GetPrimitiveArrayCritical(void *e, void *array, unsigned char *isCopy) {
    struct fake_array *a = fake_array_find(array);
    if (a && a->kind == FAKE_ARRAY_BOOLEAN)
        return GetBooleanArrayElements(e, array, isCopy);
    if (a && a->kind == FAKE_ARRAY_BYTE)
        return GetByteArrayElements(e, array, isCopy);
    return GetIntArrayElements(e, array, isCopy);
}
static void ReleasePrimitiveArrayCritical(void *e, void *array, void *elems, int mode) {
    (void)e; (void)array; (void)elems; (void)mode;
}

/* RegisterNatives: Mono registra os metodos nativos do Mono.Android aqui.
 * Capturamos para inspecao (o ponteiro fn aponta pro codigo AOT do Mono). */
struct JNINativeMethod64 { const char *name; const char *sig; void *fn; };
static struct JNINativeMethod64 g_natives[2048];
static int g_natives_n = 0;
static int RegisterNatives(void *e, void *cls, void *methods, int n) {
    (void)e; (void)cls;
    struct JNINativeMethod64 *m = (struct JNINativeMethod64 *)methods;
    if (g_verbose) fprintf(stderr, "[jni] RegisterNatives: %d methods\n", n);
    for (int i = 0; i < n && g_natives_n < (int)(sizeof(g_natives)/sizeof(g_natives[0])); i++) {
        if (g_verbose && i < 40)
            fprintf(stderr, "  [%d] %s %s -> %p\n", i, m[i].name, m[i].sig, m[i].fn);
        /* O runtime libera a tabela/string temporaria ao retornar. Precisamos
         * de copia propria para localizar o handler e chama-lo depois. */
        g_natives[g_natives_n].name = m[i].name ? strdup(m[i].name) : NULL;
        g_natives[g_natives_n].sig = m[i].sig ? strdup(m[i].sig) : NULL;
        g_natives[g_natives_n].fn = m[i].fn;
        g_natives_n++;
    }
    return 0;   /* JNI_OK — o managed checa o retorno; nao-zero = erro */
}

/* ---- Call*Method: default seguro + log ---------------------------------- */
#define LOG_CALL(tag, mid) do { \
    if (g_verbose) { \
        int _id = (int)(intptr_t)(mid); \
        const char *_n = (_id>0 && _id<=reg_n && reg_names[_id-1]) ? reg_names[_id-1] : "?"; \
        fprintf(stderr, "[jni] %s id=%d %s\n", tag, _id, _n); \
    } \
} while (0)

/* JNI jvalue tem sempre 8 bytes no arm64. */
union fake_jvalue {
    unsigned char z;
    signed char b;
    unsigned short c;
    short s;
    int i;
    int64_t j;
    float f;
    double d;
    void *l;
};

static void fake_int_array_put(void *array, int index, int value) {
    struct fake_array *a = fake_array_find(array);
    if (a && a->kind == FAKE_ARRAY_INT && index >= 0 && index < a->len)
        a->data.ints[index] = value;
}

static void fake_object_array_put(void *array, int index, void *value) {
    struct fake_array *a = fake_array_find(array);
    if (a && a->kind == FAKE_ARRAY_OBJECT && index >= 0 && index < a->len)
        a->data.objects[index] = value;
}

static void *all_keys_supported_array(void *requested) {
    struct fake_array *source = fake_array_find(requested);
    int length = source ? source->len : 16;
    struct fake_array *result = fake_array_new(FAKE_ARRAY_BOOLEAN, length);
    if (!result) return NULL;
    memset(result->data.booleans, 1, (size_t)length);
    return result;
}

static struct fake_motion_event *motion_event_payload(void *obj) {
    void *payload = fake_object_payload(obj);

    for (int i = 0; i < 5; ++i)
        if (payload == &g_motion_events[i]) return &g_motion_events[i];
    return NULL;
}

static int motion_event_int(void *obj, const char *method, int *handled) {
    struct fake_motion_event *event = motion_event_payload(obj);

    *handled = 0;
    if (!event || !method) return 0;
    if (strcmp(method, "getAction") == 0 ||
        strcmp(method, "getActionMasked") == 0) {
        *handled = 1;
        return event->action;
    }
    if (strcmp(method, "getDeviceId") == 0) {
        *handled = 1;
        return event->device_id;
    }
    if (strcmp(method, "getActionIndex") == 0 ||
        strcmp(method, "getPointerId") == 0 ||
        strcmp(method, "GetPointerId") == 0) {
        *handled = 1;
        return 0;
    }
    if (strcmp(method, "getPointerCount") == 0) {
        *handled = 1;
        return 1;
    }
    if (strcmp(method, "getSource") == 0) {
        *handled = 1;
        return event->source;
    }
    return 0;
}

static float motion_event_float(void *obj, const char *method, int axis,
                                int *handled) {
    struct fake_motion_event *event = motion_event_payload(obj);

    *handled = 0;
    if (!event || !method) return 0.0f;
    if (strcmp(method, "getX") == 0 || strcmp(method, "GetX") == 0) {
        *handled = 1;
        return event->x;
    }
    if (strcmp(method, "getY") == 0 || strcmp(method, "GetY") == 0) {
        *handled = 1;
        return event->y;
    }
    if (strcmp(method, "getAxisValue") == 0 ||
        strcmp(method, "GetAxisValue") == 0) {
        *handled = 1;
        return axis >= 0 && axis < (int)(sizeof event->axes /
                                         sizeof event->axes[0])
            ? event->axes[axis] : 0.0f;
    }
    return 0.0f;
}


/* ---- Google Play Games: caminho OFFLINE do proprio jogo ----
 *
 * O BlossomTales so libera a tela de titulo depois que
 * AndroidPlayStoreHelper.CheckIfSignedIn() responde true, e isso so acontece
 * quando o helper chega ao estado NotConnected. O caminho que o proprio jogo
 * usa para chegar la e:
 *
 *   SignInSilently() -> GoogleSignInClient.silentSignIn() -> Task
 *   Task.addOnCompleteListener(l) -> l.onComplete(task)
 *   -> a Task gerenciada completa -> Update() -> LoadCloudSave()
 *   -> getLastSignedInAccount() == null -> FinishLoading(NotConnected)
 *
 * Reproduzimos exatamente isso: o cliente de sign-in existe, a task completa
 * na hora (como faz uma Task ja concluida no Android real) e nao ha conta
 * assinada. O jogo entra sozinho no modo offline com save local, sem patch no
 * codigo gerenciado.
 */
/* O jogo so sai da tela de titulo depois que o helper de Play Games chega ao
 * estado NotConnected. Sem Google Play Services a Task de silentSignIn nunca
 * completa (o listener e um ACW generico que nenhuma JVM instancia aqui), mas
 * o jogo tem um segundo caminho, o mesmo de quando o usuario fecha a janela de
 * login: Activity.onActivityResult(0x2329, RESULT_CANCELED) -> ProcessSignIn()
 * -> conta nula -> FinishLoading(NotConnected), que ainda carrega os saves
 * locais. Disparamos esse evento na segunda consulta de conta assinada, ou
 * seja, exatamente quando o jogo esta esperando o login terminar. */
static int g_signin_queries;
static int g_signin_cancel_done;

/* O evento e entregue NA MESMA THREAD que fez a consulta, ou seja, a thread do
 * game loop. Entregar de fora (da thread de input) fazia FinishLoading abrir os
 * arquivos de IsolatedStorage em paralelo com o UserPreferences.Save do loop e
 * estourar IO_SharingViolation. */
static void play_note_account_query(void *env) {
    if (g_signin_cancel_done) return;
    /* Na 1a rodada o jogo consulta a conta duas vezes (CheckIfSignedIn e, logo
     * dentro dela, SignInSilently). Entregar o cancelamento ali faria o
     * SignInSilently sobrescrever NotConnected com Connecting logo depois.
     * A partir da 3a consulta ja existe _signInTask, o SignInSilently sai cedo
     * e o CheckIfSignedIn le NotConnected na mesma passagem. */
    if (++g_signin_queries < 3) return;
    g_signin_cancel_done = 1;
    /* A Activity1 deste jogo NAO declara n_onActivityResult (o Program do
     * scourgebringer declarava). Se o simbolo nao existir, este caminho e
     * inerte e o modo offline entra pelo n_onComplete do AuthListener. */
    void *activity_result = jni_marshal_method(
        "com.castlepixel.blossomtales.Activity1", "n_onActivityResult");
    if (!activity_result)
        activity_result = jni_find_registered_native(
            "n_onActivityResult", "(IILandroid/content/Intent;)V");
    fprintf(stderr, "[play] sem conta Google apos %d consultas; entregando "
                    "onActivityResult(0x2329, CANCELED) handler=%p\n",
            g_signin_queries, activity_result);
    if (!activity_result) return;
    typedef void (*activity_result_t)(void *, void *, int, int, void *);
    ((activity_result_t)activity_result)(env, g_activity, 0x2329,
                                         0 /* RESULT_CANCELED */, NULL);
    fprintf(stderr, "[play] onActivityResult RETORNOU\n");
}

static void *play_task_object(void) {
    return jni_make_object(jni_make_class("com.google.android.gms.tasks.Task"));
}

static int is_play_task(void *obj) {
    return fake_object_is_class(obj, "com.google.android.gms.tasks.Task");
}

/* addOnCompleteListener/addOnSuccessListener/addOnFailureListener: no Android
 * uma Task ja concluida dispara o listener imediatamente. */
static void *play_task_add_listener(void *env, void *task, const char *method,
                                    void *listener) {
    if (!listener) return task;
    if (strcmp(method, "addOnCompleteListener") == 0) {
        /* Marshal methods: o corpo do n_onComplete de CADA listener e um
         * simbolo proprio do libxamarin-app. Resolvemos pela classe do
         * proprio listener (AuthListener / ShowUiListener / UnlockListener /
         * ReviewManager_*), nunca por um n_onComplete generico — sao cinco
         * classes distintas com o mesmo nome de metodo. */
        void *on_complete = jni_marshal_for_object(listener, "n_onComplete");
        if (!on_complete)
            on_complete = jni_find_registered_native(
                "n_onComplete", "(Lcom/google/android/gms/tasks/Task;)V");
        if (!on_complete)
            on_complete = jni_find_registered_native("n_onComplete", NULL);
        fprintf(stderr, "[play] addOnCompleteListener(%s) -> onComplete=%p\n",
                fake_class_name(fake_object_class(listener)), on_complete);
        if (on_complete) {
            typedef void (*on_complete_t)(void *, void *, void *);
            ((on_complete_t)on_complete)(env, listener, task);
        }
    }
    return task;
}

/* ---- Context.getSystemService ----
 *
 * O binding do Java.Interop faz o cast do retorno consultando a CLASSE do
 * objeto Java (getClass/getName). Devolver um java.lang.Object generico
 * derrubava o OnCreate do jogo com InvalidCastException dentro de
 * BlossomTales.Platform.HapticFeedback.Initialize. Cada servico precisa vir
 * com a classe exata que o binding espera. */
static void *system_service(const char *service) {
    static const struct { const char *name; const char *klass; } map[] = {
        { "vibrator",         "android.os.Vibrator" },
        { "vibrator_manager", "android.os.VibratorManager" },
        { "audio",            "android.media.AudioManager" },
        { "window",           "android.view.WindowManager" },
        { "input",            "android.hardware.input.InputManager" },
        { "sensor",           "android.hardware.SensorManager" },
        { "power",            "android.os.PowerManager" },
        { "activity",         "android.app.ActivityManager" },
        { "connectivity",     "android.net.ConnectivityManager" },
        { "layout_inflater",  "android.view.LayoutInflater" },
        { "notification",     "android.app.NotificationManager" },
        { "wifi",             "android.net.wifi.WifiManager" },
    };
    if (!service) return NULL;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(service, map[i].name) != 0) continue;
        if (strcmp(map[i].name, "vibrator") == 0) {
            if (!g_input_vibrator)
                g_input_vibrator = jni_make_object(jni_make_class("android.os.Vibrator"));
            return g_input_vibrator;
        }
        if (strcmp(map[i].name, "vibrator_manager") == 0) {
            if (!g_input_vibrator_manager)
                g_input_vibrator_manager = jni_make_object(
                    jni_make_class("android.os.VibratorManager"));
            return g_input_vibrator_manager;
        }
        return jni_make_object(jni_make_class(map[i].klass));
    }
    fprintf(stderr, "[jni] getSystemService('%s') sem mapeamento; objeto generico\n",
            service);
    return jni_make_object(jni_make_class("java.lang.Object"));
}

/* ---- objeto tipado a partir da assinatura do metodo ----
 *
 * O Java.Interop decide o tipo gerenciado do retorno perguntando a CLASSE do
 * jobject (getClass/getName). Devolver sempre um java.lang.Object generico
 * fazia o cast do binding falhar (InvalidCastException) ou o valor chegar
 * nulo (NullReferenceException, caso do BillingClient.newBuilder). Como o
 * GetMethodID guarda a assinatura, extraimos dela a classe de retorno e
 * fabricamos um objeto fake JA COM a identidade certa.
 *
 * Deliberadamente NAO cobre `Ljava/lang/String;` (tem caminho proprio) nem
 * arrays/primitivos, e nunca sobrepoe os ramos explicitos que devolvem NULL de
 * proposito — sao eles que levam o jogo para o modo offline. */
static void *typed_object_from_signature(void *mid) {
    int id = (int)(intptr_t)mid;
    const char *sig = (id > 0 && id <= reg_n) ? reg_sigs[id - 1] : NULL;
    const char *ret;
    char dot[256];
    size_t k = 0;
    if (!sig) return NULL;
    ret = strchr(sig, ')');
    if (!ret || *++ret != 'L') return NULL;
    for (const char *p = ret + 1; *p && *p != ';' && k + 1 < sizeof dot; p++)
        dot[k++] = (*p == '/') ? '.' : *p;
    dot[k] = 0;
    if (!k || strcmp(dot, "java.lang.String") == 0 ||
        strcmp(dot, "java.lang.CharSequence") == 0 ||
        strcmp(dot, "java.lang.Object") == 0)
        return NULL;
    return jni_make_object(jni_make_class(dot));
}

/* ---- metodos de instancia com um unico argumento String ----
 *
 * Concentra os casos que precisam LER o argumento: o Java.Interop chama uns
 * pela via virtual e outros pela NAO-virtual (metodos herdados de Context),
 * entao os quatro despachos delegam aqui em vez de duplicar regra.
 *
 * getProperty: o OpenALSoundController do MonoGame Android le a taxa de
 * amostragem e o tamanho de buffer do AudioManager e faz int.Parse no
 * resultado. Devolver a string generica do shim ("java.lang.Object") fazia o
 * parse estourar e o audio inteiro morria com "Failed to init
 * OpenALSoundController" — som nenhum, sem erro visivel de OpenAL (o device
 * pulse abria normalmente). NULL faria o MonoGame usar os proprios defaults;
 * devolvemos valores reais para nao depender disso. */
static int object_string_arg_call(void *obj, const char *method,
                                  const char *arg, void **out) {
    (void)obj;
    if (!method || !out) return 0;
    if (strcmp(method, "getSystemService") == 0) {
        *out = system_service(arg);
        return 1;
    }
    if (strcmp(method, "getProperty") == 0) {
        if (arg && strcmp(arg, "android.media.property.OUTPUT_SAMPLE_RATE") == 0)
            *out = strdup("48000");
        else if (arg && strcmp(arg, "android.media.property.OUTPUT_FRAMES_PER_BUFFER") == 0)
            *out = strdup("512");
        else
            *out = NULL;   /* propriedade desconhecida: MonoGame usa o default */
        return 1;
    }
    return 0;
}


/* --- MediaPlayer de verdade: a musica do MonoGame ----------------------- */

/* Song (Microsoft.Xna.Framework.Media) nao passa pelo OpenAL: o caminho e
 * Reset -> setDataSource(FileDescriptor,off,len) -> prepare -> setLooping ->
 * setVolume -> start, tudo no android.media.MediaPlayer. O stub antigo so
 * respondia getVideoWidth/Height para o video de intro e engolia o resto,
 * entao o jogo rodava com SFX e sem trilha nenhuma, sem erro no log. */

struct fake_media_player {
    uint32_t magic;
    nx_media_player *player;
};
struct fake_file_desc {
    uint32_t magic;
    char *path;
};

static int is_media_player(void *obj) {
    return fake_object_is_class(obj, "android.media.MediaPlayer");
}

static nx_media_player *media_player_for(void *obj) {
    if (!is_media_player(obj)) return NULL;
    struct fake_media_player *slot = audio_payload(obj, AUDIO_MP_MAGIC);
    if (slot) return slot->player;
    if (fake_object_payload(obj)) return NULL; /* payload de outro dono */
    slot = calloc(1, sizeof(*slot));
    if (!slot) return NULL;
    slot->magic = AUDIO_MP_MAGIC;
    slot->player = nx_mp_create();
    fake_object_set_payload(obj, slot);
    fprintf(stderr, "[nx-music] MediaPlayer real criado\n");
    return slot->player;
}

/* AssetFileDescriptor.getFileDescriptor(): o descritor precisa carregar o
 * caminho ja resolvido, porque e ele que o MonoGame entrega ao MediaPlayer. */
static void *audio_file_descriptor(void *afd_object) {
    struct fake_audio_fd *fd = audio_payload(afd_object, AUDIO_FD_MAGIC);
    if (!fd || !fd->path) return NULL;
    struct fake_file_desc *desc = calloc(1, sizeof(*desc));
    if (!desc) return NULL;
    desc->magic = AUDIO_FDESC_MAGIC;
    desc->path = strdup(fd->path);
    void *object = jni_make_object(jni_make_class("java.io.FileDescriptor"));
    fake_object_set_payload(object, desc);
    return object;
}

static const char *file_desc_path(void *obj) {
    struct fake_file_desc *desc = audio_payload(obj, AUDIO_FDESC_MAGIC);
    return desc ? desc->path : NULL;
}

static int64_t audio_fd_length(void *afd_object) {
    struct fake_audio_fd *fd = audio_payload(afd_object, AUDIO_FD_MAGIC);
    struct stat st;
    if (!fd || !fd->path || stat(fd->path, &st) != 0) return 0;
    return (int64_t)st.st_size;
}

/* Devolve 1 quando a chamada era do MediaPlayer e ja foi atendida. */
static int media_player_void_call(void *obj, const char *method,
                                  const union fake_jvalue *jv) {
    if (!method || !is_media_player(obj)) return 0;
    nx_media_player *mp = media_player_for(obj);
    if (!mp) return 0;
    if (strcmp(method, "reset") == 0 || strcmp(method, "release") == 0) {
        nx_mp_reset(mp);
        return 1;
    }
    if (strcmp(method, "prepare") == 0 || strcmp(method, "prepareAsync") == 0) {
        nx_mp_prepare(mp);
        return 1;
    }
    if (strcmp(method, "start") == 0) { nx_mp_start(mp); return 1; }
    if (strcmp(method, "pause") == 0) { nx_mp_pause(mp); return 1; }
    if (strcmp(method, "stop") == 0)  { nx_mp_stop(mp);  return 1; }
    if (strcmp(method, "setDataSource") == 0) {
        if (!jv) return 1;
        const char *path = file_desc_path(jv[0].l);
        if (!path) path = safe_cstr(jv[0].l); /* overload de String */
        if (path && *path) {
            const char *file = strncmp(path, "file://", 7) == 0 ? path + 7
                                                                : path;
            nx_mp_set_source(mp, file);
        }
        return 1;
    }
    if (strcmp(method, "setLooping") == 0) {
        nx_mp_set_looping(mp, jv ? jv[0].z : 0);
        return 1;
    }
    if (strcmp(method, "setVolume") == 0) {
        nx_mp_set_volume(mp, jv ? jv[0].f : 1.0f, jv ? jv[1].f : 1.0f);
        return 1;
    }
    if (strcmp(method, "seekTo") == 0) {
        nx_mp_seek_ms(mp, jv ? jv[0].i : 0);
        return 1;
    }
    /* setOnCompletionListener, setAudioStreamType, setWakeMode e afins nao
     * tem efeito util aqui, mas precisam ser engolidos com dono conhecido. */
    if (strncmp(method, "set", 3) == 0) return 1;
    return 0;
}

static int media_player_void_call_v(void *obj, const char *method, va_list a) {
    if (!method || !is_media_player(obj)) return 0;
    union fake_jvalue jv[2];
    memset(jv, 0, sizeof(jv));
    if (strcmp(method, "setDataSource") == 0) {
        jv[0].l = va_arg(a, void *);
    } else if (strcmp(method, "setLooping") == 0) {
        jv[0].z = (unsigned char)va_arg(a, int);
    } else if (strcmp(method, "setVolume") == 0) {
        jv[0].f = (float)va_arg(a, double);
        jv[1].f = (float)va_arg(a, double);
    } else if (strcmp(method, "seekTo") == 0) {
        jv[0].i = va_arg(a, int);
    }
    return media_player_void_call(obj, method, jv);
}

static int media_player_int_call(void *obj, const char *method, int *out) {
    if (!method || !is_media_player(obj)) return 0;
    nx_media_player *mp = media_player_for(obj);
    if (!mp) return 0;
    if (strcmp(method, "getDuration") == 0) {
        *out = nx_mp_duration_ms(mp);
        return 1;
    }
    if (strcmp(method, "getCurrentPosition") == 0) {
        *out = nx_mp_position_ms(mp);
        return 1;
    }
    return 0;
}

static int media_player_bool_call(void *obj, const char *method,
                                  unsigned char *out) {
    if (!method || !is_media_player(obj)) return 0;
    nx_media_player *mp = media_player_for(obj);
    if (!mp) return 0;
    if (strcmp(method, "isPlaying") == 0) {
        *out = (unsigned char)(nx_mp_is_playing(mp) ? 1 : 0);
        return 1;
    }
    if (strcmp(method, "isLooping") == 0) {
        *out = (unsigned char)(nx_mp_get_looping(mp) ? 1 : 0);
        return 1;
    }
    return 0;
}

static void *CallObjectMethodCore(void *obj, void *mid) {
    LOG_CALL("CallObjectMethod", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "getFileDescriptor") == 0 &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor"))
        return audio_file_descriptor(obj);
    if (method && (strcmp(method, "silentSignIn") == 0 ||
                   strcmp(method, "signOut") == 0 ||
                   strcmp(method, "revokeAccess") == 0))
        return play_task_object();
    if (method && strcmp(method, "getResult") == 0 && is_play_task(obj))
        return NULL;   /* sem conta assinada: resultado nulo */
    if (method && strcmp(method, "getException") == 0 && is_play_task(obj))
        return NULL;
    if (method && strcmp(method, "getDevice") == 0) {
        void *klass = fake_object_class(obj);
        const char *name = fake_class_name(klass);
        int device_id = 0;
        if (name && strcmp(name, "android.view.KeyEvent") == 0)
            device_id = key_event_device_id(obj);
        else if (name && strcmp(name, "android.view.MotionEvent") == 0) {
            struct fake_motion_event *event = motion_event_payload(obj);
            if (event) device_id = event->device_id;
        }
        if (device_id) return input_device(device_id);
    }
    if (method && strcmp(method, "getDescriptor") == 0 &&
        fake_object_is_class(obj, "android.view.InputDevice")) {
        char descriptor[48];
        snprintf(descriptor, sizeof descriptor, "nextos-sdl-gamepad-%d",
                 input_device_id(obj));
        return strdup(descriptor);
    }
    if (method && strcmp(method, "getName") == 0) {
        void *klass = fake_object_class(obj);
        const char *class_name = fake_class_name(klass);
        if (class_name && strcmp(class_name, "android.view.InputDevice") == 0)
            return strdup("Xbox Wireless Controller");
    }
    if (method && strcmp(method, "getVibrator") == 0) {
        if (!g_input_vibrator)
            g_input_vibrator = jni_make_object(jni_make_class("android.os.Vibrator"));
        return g_input_vibrator;
    }
    if (method && strcmp(method, "getVibratorManager") == 0) {
        if (!g_input_vibrator_manager)
            g_input_vibrator_manager = jni_make_object(
                jni_make_class("android.os.VibratorManager"));
        return g_input_vibrator_manager;
    }
    if (method && strcmp(method, "getDefaultVibrator") == 0) {
        if (!g_input_vibrator)
            g_input_vibrator = jni_make_object(
                jni_make_class("android.os.Vibrator"));
        return g_input_vibrator;
    }
    if (method && strcmp(method, "getName") == 0) {
        const char *name = fake_class_name(obj);
        if (name) return strdup(name);
    }
    if (method && strcmp(method, "getClass") == 0) {
        void *klass = fake_object_class(obj);
        if (klass) return klass;
    }
    if (method && strcmp(method, "getWindow") == 0)
        return jni_make_object(jni_make_class("android.view.Window"));
    if (method && strcmp(method, "getPackageManager") == 0)
        return jni_make_object(jni_make_class("android.content.pm.PackageManager"));
    if (method && strcmp(method, "getPackageName") == 0)
        return strdup("com.FDGEntertainment.BlossomTales.gp");
    /* getStackTrace: devolver NULL evita o bug de tradução de array
     * (Java.Interop resolve o elemento como "java.lang.Object" e lanca
     * InvalidOperationException, mascarando/quebrando o handling da excecao
     * original — inclusive Thread.Start do game loop). */
    if (method && strcmp(method, "getStackTrace") == 0)
        return NULL;
    if (method && strcmp(method, "getWindowManager") == 0)
        return jni_make_object(jni_make_class("android.view.WindowManager"));
    if (method && strcmp(method, "getDefaultDisplay") == 0)
        return jni_make_object(jni_make_class("android.view.Display"));
    if (method && strcmp(method, "getResources") == 0)
        return jni_make_object(jni_make_class("android.content.res.Resources"));
    if (method && strcmp(method, "getAssets") == 0)
        return jni_make_object(jni_make_class("android.content.res.AssetManager"));
    if (method && strcmp(method, "getDisplayMetrics") == 0)
        return jni_make_object(jni_make_class("android.util.DisplayMetrics"));
    if (method && strcmp(method, "getHolder") == 0)
        return jni_make_object(jni_make_class("android.view.SurfaceHolder"));
    if (method && (strcmp(method, "getFilesDir") == 0 ||
                   strcmp(method, "getExternalFilesDir") == 0))
        return jni_make_object(jni_make_class("java.io.File"));
    if (method && strcmp(method, "getCanonicalPath") == 0)
        return strdup("/storage/roms/ports/blossomtales/data");
    if (method && strcmp(method, "getApplicationContext") == 0)
        return g_activity ? g_activity : obj;
    if (method && strcmp(method, "getBaseContext") == 0)
        return g_activity ? g_activity : obj;
    if (method && strcmp(method, "edit") == 0)
        return jni_make_object(jni_make_class("android.content.SharedPreferences$Editor"));
    /* ---- Netflix Games SDK (fake offline): getters de API viram objetos
     * fake; idioma SEMPRE ingles (regra do sistema); handle de perfil generico.
     * O ctor do Paris.NetflixManager caia em NRE com getInstance()=NULL. */
    if (method && (strcmp(method, "getAccessApi") == 0 ||
                   strcmp(method, "getProfilesApi") == 0 ||
                   strcmp(method, "getBlobStoreApi") == 0 ||
                   strcmp(method, "getStatsApi") == 0 ||
                   strcmp(method, "getLeaderboardsApi") == 0))
        return jni_make_object(jni_make_class("com.netflix.games.Api"));
    /* get_CurrentProfile devolve NetflixResult; o CurrentProfile fica no
     * getData (SetNetflixProfile le result.Error==null + result.Data). */
    if (method && strcmp(method, "getCurrentProfile") == 0)
        return netflix_result_with(
            "com.netflix.games.player.profiles.CurrentProfile");
    if (method && strcmp(method, "getData") == 0) {
        const char *cn = fake_class_name(fake_object_class(obj));
        if (cn && strcmp(cn, "com.netflix.games.NetflixResult") == 0)
            return fake_object_payload(obj);
    }
    if (method && strcmp(method, "getError") == 0)
        return NULL;   /* Error==null = sucesso p/ o NetflixManager */
    if (method && strcmp(method, "getPlayerId") == 0)
        return strdup("nextos-player-1");
    if (method && (strcmp(method, "getPreferredLanguage") == 0 ||
                   strcmp(method, "getLanguage") == 0))
        return strdup("en");
    if (method && strcmp(method, "getHandle") == 0)
        return strdup("PLAYER");
    if (method && strcmp(method, "registerReceiver") == 0)
        return NULL; /* nenhum sticky broadcast */
    if (method && strncmp(method, "egl", 3) == 0 && getenv("SB_EGL_TRACE"))
        fprintf(stderr, "[egl] objCore %s\n", method);
    if (method && strcmp(method, "eglGetDisplay") == 0) {
        if (!sdv_egl_ready())
            return fake_singleton(&g_egl_no_display,
                                  "javax.microedition.khronos.egl.EGLDisplay");
        return fake_singleton(&g_egl_display,
                              "javax.microedition.khronos.egl.EGLDisplay");
    }
    if (method && strcmp(method, "eglCreateContext") == 0) {
        void *native = sdv_egl_create_context();
        if (!native)
            return fake_singleton(&g_egl_no_context,
                                  "javax.microedition.khronos.egl.EGLContext");
        void *java = fake_singleton(&g_egl_context,
                                    "javax.microedition.khronos.egl.EGLContext");
        fake_object_set_payload(java, native);
        return java;
    }
    if (method && (strcmp(method, "eglCreateWindowSurface") == 0 ||
                   strcmp(method, "eglCreatePbufferSurface") == 0)) {
        void *native = sdv_egl_create_surface();
        if (!native)
            return fake_singleton(&g_egl_no_surface,
                                  "javax.microedition.khronos.egl.EGLSurface");
        void *java = jni_make_object(
            jni_make_class("javax.microedition.khronos.egl.EGLSurface"));
        fake_object_set_payload(java, native);
        g_egl_surface = java;
        return java;
    }
    /* Default nao-NULL: o managed Java.Interop chama getName/toString via
     * CallObjectMethod e usa o resultado como chave (Dictionary.TryGetValue).
     * NULL -> ArgumentNullException (TypeManager.CreateInstance key=null).
     * Devolvemos uma jstring placeholder ("java.lang.Object") — GetStringUTFChars
     * devolve o C string, TypeManager mapeia p/ Java.Lang.Object (tipo conhecido). */
    {
        void *typed = typed_object_from_signature(mid);
        if (typed) return typed;
    }
    return strdup("java.lang.Object");
}
static void *CallObjectMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e;
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "list") == 0)
        return asset_dir_list(safe_cstr(va_arg(a, void *)));
    if (method && strcmp(method, "open") == 0) {
        struct fake_stream *stream = asset_stream_open(safe_cstr(va_arg(a, void *)));
        if (!stream) return NULL;
        void *input = jni_make_object(jni_make_class("java.io.InputStream"));
        fake_object_set_payload(input, stream);
        return input;
    }
    if (method && strncmp(method, "addOn", 5) == 0 && is_play_task(obj))
        return play_task_add_listener(e, obj, method, va_arg(a, void *));
    if (method && strcmp(method, "openFd") == 0)
        return audio_open_fd(safe_cstr(va_arg(a, void *)));
    if (method && strcmp(method, "hasKeys") == 0 &&
        fake_object_is_class(obj, "android.view.InputDevice"))
        return all_keys_supported_array(va_arg(a, void *));
    if (method && strcmp(method, "getTrackFormat") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        (void)va_arg(a, int);
        return audio_get_track_format(obj);
    }
    if (method && strcmp(method, "getString") == 0 &&
        fake_object_is_class(obj, "android.media.MediaFormat"))
        return audio_format_get_string(obj, safe_cstr(va_arg(a, void *)));
    if (method && strcmp(method, "getInputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        (void)va_arg(a, int);
        return audio_codec_buffer(obj, 0);
    }
    if (method && strcmp(method, "getOutputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        (void)va_arg(a, int);
        return audio_codec_buffer(obj, 1);
    }
    if (method && strcmp(method, "get") == 0 &&
        fake_object_is_class(obj, "java.nio.ByteBuffer")) {
        void *array = va_arg(a, void *);
        int offset = va_arg(a, int);
        int length = va_arg(a, int);
        return audio_buffer_get(obj, array, offset, length);
    }
    if (method && (strcmp(method, "getSystemService") == 0 ||
                   strcmp(method, "getProperty") == 0)) {
        void *out = NULL;
        if (object_string_arg_call(obj, method, safe_cstr(va_arg(a, void *)), &out))
            return out;
    }
    return CallObjectMethodCore(obj, mid);
}
static void *CallObjectMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); void *r = CallObjectMethodV(e, obj, mid, a); va_end(a); return r;
}
static void *CallObjectMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e;
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "open") == 0 && args) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        struct fake_stream *stream = asset_stream_open(safe_cstr(jv[0].l));
        if (!stream) return NULL;
        /* A classe exata InputStream faz o binding Xamarin usar
         * InputStreamInvoker (CanSeek=false). FileInputStream tentaria um
         * FileChannel fake com Size/Position=0 e reduziria o CopyTo a blocos
         * de 16 bytes. */
        void *input = jni_make_object(jni_make_class("java.io.InputStream"));
        fake_object_set_payload(input, stream);
        return input;
    }
    if (method && strcmp(method, "list") == 0 && args) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return asset_dir_list(safe_cstr(jv[0].l));
    }
    if (method && strncmp(method, "addOn", 5) == 0 && args && is_play_task(obj)) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return play_task_add_listener(e, obj, method, jv[0].l);
    }
    if (method && strcmp(method, "openFd") == 0 && args) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return audio_open_fd(safe_cstr(jv[0].l));
    }
    if (method && strcmp(method, "hasKeys") == 0 && args &&
        fake_object_is_class(obj, "android.view.InputDevice")) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return all_keys_supported_array(jv[0].l);
    }
    if (method && strcmp(method, "getTrackFormat") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor"))
        return audio_get_track_format(obj);
    if (method && strcmp(method, "getString") == 0 && args &&
        fake_object_is_class(obj, "android.media.MediaFormat")) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return audio_format_get_string(obj, safe_cstr(jv[0].l));
    }
    if (method && strcmp(method, "getInputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec"))
        return audio_codec_buffer(obj, 0);
    if (method && strcmp(method, "getOutputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec"))
        return audio_codec_buffer(obj, 1);
    if (method && strcmp(method, "get") == 0 && args &&
        fake_object_is_class(obj, "java.nio.ByteBuffer")) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return audio_buffer_get(obj, jv[0].l, jv[1].i, jv[2].i);
    }
    if (method && args && (strcmp(method, "getSystemService") == 0 ||
                           strcmp(method, "getProperty") == 0)) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        void *out = NULL;
        if (object_string_arg_call(obj, method, safe_cstr(jv[0].l), &out))
            return out;
    }
    return CallObjectMethodCore(obj, mid);
}
static void *CallStaticObjectMethodV(void *e, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; LOG_CALL("CallStaticObjectMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "forName") == 0) {
        void *name = va_arg(a, void *);
        return jni_make_class(safe_cstr(name));
    }
    if (method && strcmp(method, "getDeviceIds") == 0)
        return input_device_ids_array();
    if (method && strcmp(method, "getDevice") == 0)
        return connected_input_device(va_arg(a, int));
    if (method && (strcmp(method, "deviceHasKeys") == 0 ||
                   strcmp(method, "hasKeys") == 0))
        return all_keys_supported_array(va_arg(a, void *));
    if (method && strcmp(method, "getLastSignedInAccount") == 0) {
        play_note_account_query(e);
        return NULL;
    }
    if (method && strcmp(method, "getClient") == 0)
        return jni_make_object(
            jni_make_class("com.google.android.gms.auth.api.signin.GoogleSignInClient"));
    if (method && strcmp(method, "getExternalStoragePublicDirectory") == 0)
        return jni_make_object(jni_make_class("java.io.File"));
    if (method && strcmp(method, "getDefaultSharedPreferences") == 0)
        return jni_make_object(jni_make_class("android.content.SharedPreferences"));
    if (method && strcmp(method, "myLooper") == 0)
        sdv_promote_current_mono_thread();
    if (method && (strcmp(method, "getMainLooper") == 0 ||
                   strcmp(method, "myLooper") == 0))
        return g_main_looper_ready ? main_looper() : NULL;
    if (method && strcmp(method, "getInstance") == 0)
        return jni_make_object(cls ? cls : jni_make_class("java.lang.Object"));
    if (method && strcmp(method, "getEGL") == 0) {
        if (getenv("SB_EGL_TRACE")) fprintf(stderr, "[egl] getEGL (start CreateGLContext?)\n");
        return jni_make_object(jni_make_class("javax.microedition.khronos.egl.EGL10"));
    }
    if (method && strcmp(method, "createDecoderByType") == 0) {
        (void)va_arg(a, void *);
        return audio_create_codec();
    }
    /* Fabricas estaticas desconhecidas (BillingClient.newBuilder, etc.):
     * devolver NULL derrubava o Initialize do BillingManager com NRE. */
    return typed_object_from_signature(mid);
}
static void *CallStaticObjectMethod(void *e, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); void *r = CallStaticObjectMethodV(e, cls, mid, a); va_end(a); return r;
}
static void *CallStaticObjectMethodA(void *e, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; LOG_CALL("CallStaticObjectMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "forName") == 0 && args) {
        void *name = *(void * const *)args; /* jvalue[0].l */
        return jni_make_class(safe_cstr(name));
    }
    if (method && strcmp(method, "getDeviceIds") == 0)
        return input_device_ids_array();
    if (method && strcmp(method, "getDevice") == 0) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return connected_input_device(jv ? jv[0].i : 0);
    }
    if (method && (strcmp(method, "deviceHasKeys") == 0 ||
                   strcmp(method, "hasKeys") == 0)) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return all_keys_supported_array(jv ? jv[0].l : NULL);
    }
    if (method && strcmp(method, "getLastSignedInAccount") == 0) {
        play_note_account_query(e);
        return NULL;
    }
    if (method && strcmp(method, "getClient") == 0)
        return jni_make_object(
            jni_make_class("com.google.android.gms.auth.api.signin.GoogleSignInClient"));
    if (method && strcmp(method, "getExternalStoragePublicDirectory") == 0)
        return jni_make_object(jni_make_class("java.io.File"));
    if (method && strcmp(method, "getDefaultSharedPreferences") == 0)
        return jni_make_object(jni_make_class("android.content.SharedPreferences"));
    if (method && strcmp(method, "myLooper") == 0)
        sdv_promote_current_mono_thread();
    if (method && (strcmp(method, "getMainLooper") == 0 ||
                   strcmp(method, "myLooper") == 0))
        return g_main_looper_ready ? main_looper() : NULL;
    if (method && strcmp(method, "getInstance") == 0)
        return jni_make_object(cls ? cls : jni_make_class("java.lang.Object"));
    if (method && strcmp(method, "getEGL") == 0) {
        if (getenv("SB_EGL_TRACE")) fprintf(stderr, "[egl] getEGL (start CreateGLContext?)\n");
        return jni_make_object(jni_make_class("javax.microedition.khronos.egl.EGL10"));
    }
    if (method && strcmp(method, "createDecoderByType") == 0)
        return audio_create_codec();
    return typed_object_from_signature(mid);
}
static int play_task_bool(void *obj, const char *method) {
    if (!method || !is_play_task(obj)) return -1;
    if (strcmp(method, "isSuccessful") == 0) return 1;
    if (strcmp(method, "isComplete") == 0) return 1;
    if (strcmp(method, "isCanceled") == 0) return 0;
    return -1;
}
static unsigned char CallBooleanMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; (void)obj; (void)a; LOG_CALL("CallBooleanMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int pt = play_task_bool(obj, method);
    if (pt >= 0) return (unsigned char)pt;
    { unsigned char mp_flag = 0;
      if (media_player_bool_call(obj, method, &mp_flag)) return mp_flag; }
    if (method && strcmp(method, "advance") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor"))
        return 1;
    if (method && strncmp(method, "egl", 3) == 0) return 1;
    return 0;
}
static unsigned char CallBooleanMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); unsigned char r = CallBooleanMethodV(e, obj, mid, a); va_end(a); return r;
}
static unsigned char CallBooleanMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; (void)obj; (void)args; LOG_CALL("CallBooleanMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int pt = play_task_bool(obj, method);
    if (pt >= 0) return (unsigned char)pt;
    { unsigned char mp_flag = 0;
      if (media_player_bool_call(obj, method, &mp_flag)) return mp_flag; }
    if (method && strcmp(method, "advance") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor"))
        return 1;
    if (method && (strcmp(method, "requestWindowFeature") == 0 ||
                   strcmp(method, "requestFocus") == 0 ||
                   strcmp(method, "mkdirs") == 0 ||
                   strcmp(method, "commit") == 0 ||
                   strcmp(method, "apply") == 0)) return 1;
    if (method && strncmp(method, "egl", 3) == 0) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        if (getenv("SB_EGL_TRACE")) fprintf(stderr, "[egl] boolA %s\n", method);
        if (jv && strcmp(method, "eglInitialize") == 0) {
            if (!sdv_egl_ready()) return 0;
            fake_int_array_put(jv[1].l, 0, 1);
            fake_int_array_put(jv[1].l, 1, 4);
        } else if (jv && strcmp(method, "eglGetConfigs") == 0) {
            fake_int_array_put(jv[3].l, 0, 1);
            if (jv[1].l && jv[2].i > 0)
                fake_object_array_put(jv[1].l, 0,
                    jni_make_object(jni_make_class("javax.microedition.khronos.egl.EGLConfig")));
        } else if (jv && strcmp(method, "eglChooseConfig") == 0) {
            fake_int_array_put(jv[4].l, 0, 1);
            if (jv[2].l && jv[3].i > 0)
                fake_object_array_put(jv[2].l, 0,
                    jni_make_object(jni_make_class("javax.microedition.khronos.egl.EGLConfig")));
        } else if (jv && strcmp(method, "eglGetConfigAttrib") == 0) {
            int value = 0;
            switch (jv[2].i) {
            case 0x3020: value = 32; break; /* EGL_BUFFER_SIZE */
            case 0x3021: value = 8; break;  /* alpha */
            case 0x3022: value = 8; break;  /* blue */
            case 0x3023: value = 8; break;  /* green */
            case 0x3024: value = 8; break;  /* red */
            case 0x3025: value = 24; break; /* depth */
            case 0x3026: value = 8; break;  /* stencil */
            case 0x3040: value = 4; break;  /* OpenGL ES 2 */
            }
            fake_int_array_put(jv[3].l, 0, value);
        } else if (jv && strcmp(method, "eglQuerySurface") == 0) {
            int value = jv[2].i == 0x3056 ? sdv_egl_height() : sdv_egl_width();
            fake_int_array_put(jv[3].l, 0, value);
        } else if (jv && strcmp(method, "eglMakeCurrent") == 0) {
            void *surface = fake_object_payload(jv[1].l);
            void *context = fake_object_payload(jv[3].l);
            if (!surface && !context)
                return sdv_egl_make_current(NULL, NULL);
            return sdv_egl_make_current(context, surface);
        } else if (jv && strcmp(method, "eglSwapBuffers") == 0) {
            return sdv_egl_swap(fake_object_payload(jv[1].l));
        } else if (jv && strcmp(method, "eglDestroySurface") == 0) {
            void *native = fake_object_payload(jv[1].l);
            if (native) sdv_egl_destroy_surface(native);
            fake_object_set_payload(jv[1].l, NULL);
            if (jv[1].l == g_egl_surface) g_egl_surface = NULL;
            return 1;
        } else if (jv && strcmp(method, "eglDestroyContext") == 0) {
            void *native = fake_object_payload(jv[1].l);
            if (native) sdv_egl_destroy_context(native);
            fake_object_set_payload(jv[1].l, NULL);
            return 1;
        } else if (strcmp(method, "eglTerminate") == 0) {
            sdv_egl_destroy();
            return 1;
        }
        return 1;
    }
    return 0;
}
/* org.fmod.FMOD: o FMOD consulta a classe Java antes de escolher a saida.
 * `checkInit` = "org.fmod.FMOD.init(context) ja rodou" (aqui o equivalente e o
 * JNI_OnLoad do libfmod, que ja rodou). `supportsAAudio` decide entre AAudio e
 * o fallback AudioTrack (implementado em Java, que nao existe aqui) — dizemos
 * sim para que o FMOD use a AAudio de aaudio_shim.c. `supportsLowLatency`
 * ficaria pedindo bursts minusculos ao driver; num Mali-450 isso so gera
 * underrun, entao fica falso. */
static int fmod_static_bool(const char *method) {
    if (!method) return -1;
    if (strcmp(method, "checkInit") == 0) return 1;
    if (strcmp(method, "supportsAAudio") == 0) return sb_aaudio_available();
    if (strcmp(method, "supportsLowLatency") == 0) return 0;
    return -1;
}
static unsigned char CallStaticBooleanMethodV(void *e, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; (void)a; LOG_CALL("CallStaticBooleanMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int fm = fmod_static_bool(method);
    return fm >= 0 ? (unsigned char)fm : 0;
}
static unsigned char CallStaticBooleanMethod(void *e, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); unsigned char r = CallStaticBooleanMethodV(e, cls, mid, a); va_end(a); return r;
}
static unsigned char CallStaticBooleanMethodA(void *e, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; (void)args; LOG_CALL("CallStaticBooleanMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int fm = fmod_static_bool(method);
    return fm >= 0 ? (unsigned char)fm : 0;
}
static int CallIntMethodV(void *e, void *obj, void *mid, va_list a) {
    { int mp_value = 0;
      int mp_id = (int)(intptr_t)mid;
      const char *mp_method = (mp_id > 0 && mp_id <= reg_n)
                              ? reg_names[mp_id - 1] : NULL;
      if (media_player_int_call(obj, mp_method, &mp_value)) return mp_value; }
    (void)e; LOG_CALL("CallIntMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    const char *sig = (id > 0 && id <= reg_n) ? reg_sigs[id - 1] : NULL;
    int motion_handled = 0;
    int motion_value = motion_event_int(obj, method, &motion_handled);
    if (motion_handled) return motion_value;
    void *klass = fake_object_class(obj);
    const char *class_name = fake_class_name(klass);
    if (method && class_name &&
        strcmp(class_name, "android.view.InputDevice") == 0) {
        if (strcmp(method, "getSources") == 0)
            return 0x01000611; /* JOYSTICK | GAMEPAD | DPAD */
        if (strcmp(method, "getId") == 0) return input_device_id(obj);
        if (strcmp(method, "getVendorId") == 0) return bt_input_vendor_id();
        if (strcmp(method, "getProductId") == 0) return bt_input_product_id();
    }
    if (method && class_name && strcmp(class_name, "android.view.KeyEvent") == 0) {
        if (strcmp(method, "getKeyCode") == 0)
            return key_event_keycode(obj);
        if (strcmp(method, "getDeviceId") == 0)
            return key_event_device_id(obj);
        if (strcmp(method, "getAction") == 0) return 0;
    }
    if (method && strcmp(method, "getInteger") == 0 &&
        fake_object_is_class(obj, "android.media.MediaFormat"))
        return audio_format_get_integer(obj,
                                        safe_cstr(va_arg(a, void *)));
    if (method && strcmp(method, "dequeueInputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        (void)va_arg(a, int64_t);
        return audio_dequeue_input(obj);
    }
    if (method && strcmp(method, "readSampleData") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        void *buffer = va_arg(a, void *);
        int offset = va_arg(a, int);
        return audio_read_sample_data(obj, buffer, offset);
    }
    if (method && strcmp(method, "dequeueOutputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        void *info = va_arg(a, void *);
        (void)va_arg(a, int64_t);
        return audio_dequeue_output(obj, info);
    }
    if (method && strcmp(method, "remaining") == 0 &&
        fake_object_is_class(obj, "java.nio.ByteBuffer"))
        return audio_buffer_remaining(obj);
    if (method && (strcmp(method, "getFlags") == 0 ||
                   strcmp(method, "get_Flags") == 0))
        return audio_buffer_flags(obj);
    if (method && strcmp(method, "available") == 0)
        return asset_stream_available(obj);
    if (method && strcmp(method, "read") == 0 && sig && strcmp(sig, "()I") == 0)
        return asset_stream_read_byte(obj);
    /* MediaPlayer fake (video de intro): dimensoes validas evitam
     * RenderTarget de largura 0. SuperVideoPlayer mantem a posicao em um
     * campo gerenciado que so avanca com SurfaceTexture.FrameAvailable; como
     * nao temos decoder/evento, duracao zero faz PlatformGetState devolver
     * Stopped e o VideoContext seguir ao menu. */
    if (method && strcmp(method, "getVideoWidth") == 0) return 640;
    if (method && strcmp(method, "getVideoHeight") == 0) return 360;
    if (method && class_name &&
        strcmp(class_name, "android.media.MediaPlayer") == 0) {
        if (strcmp(method, "getDuration") == 0) return 0;
        if (strcmp(method, "getCurrentPosition") == 0) return 0;
    }
    if (method && strcmp(method, "getWidth") == 0) return sdv_egl_width();
    if (method && strcmp(method, "getHeight") == 0) return sdv_egl_height();
    if (method && strcmp(method, "eglGetError") == 0) return 0x3000;
    return 0;
}
static int CallIntMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); int r = CallIntMethodV(e, obj, mid, a); va_end(a); return r;
}
static int CallIntMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; LOG_CALL("CallIntMethodA", mid);
    { int mp_value = 0;
      int mp_id = (int)(intptr_t)mid;
      const char *mp_method = (mp_id > 0 && mp_id <= reg_n)
                              ? reg_names[mp_id - 1] : NULL;
      if (media_player_int_call(obj, mp_method, &mp_value)) return mp_value; }
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    const char *sig = (id > 0 && id <= reg_n) ? reg_sigs[id - 1] : NULL;
    int motion_handled = 0;
    int motion_value = motion_event_int(obj, method, &motion_handled);
    if (motion_handled) return motion_value;
    void *klass = fake_object_class(obj);
    const char *class_name = fake_class_name(klass);
    if (method && class_name &&
        strcmp(class_name, "android.view.InputDevice") == 0) {
        if (strcmp(method, "getSources") == 0)
            return 0x01000611;
        if (strcmp(method, "getId") == 0) return input_device_id(obj);
        if (strcmp(method, "getVendorId") == 0) return bt_input_vendor_id();
        if (strcmp(method, "getProductId") == 0) return bt_input_product_id();
    }
    if (method && class_name && strcmp(class_name, "android.view.KeyEvent") == 0) {
        if (strcmp(method, "getKeyCode") == 0)
            return key_event_keycode(obj);
        if (strcmp(method, "getDeviceId") == 0)
            return key_event_device_id(obj);
        if (strcmp(method, "getAction") == 0) return 0;
    }
    if (method && strcmp(method, "getInteger") == 0 && args &&
        fake_object_is_class(obj, "android.media.MediaFormat")) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return audio_format_get_integer(obj, safe_cstr(jv[0].l));
    }
    if (method && strcmp(method, "dequeueInputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec"))
        return audio_dequeue_input(obj);
    if (method && strcmp(method, "readSampleData") == 0 && args &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return audio_read_sample_data(obj, jv[0].l, jv[1].i);
    }
    if (method && strcmp(method, "dequeueOutputBuffer") == 0 && args &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return audio_dequeue_output(obj, jv[0].l);
    }
    if (method && strcmp(method, "remaining") == 0 &&
        fake_object_is_class(obj, "java.nio.ByteBuffer"))
        return audio_buffer_remaining(obj);
    if (method && (strcmp(method, "getFlags") == 0 ||
                   strcmp(method, "get_Flags") == 0))
        return audio_buffer_flags(obj);
    if (method && strcmp(method, "available") == 0)
        return asset_stream_available(obj);
    if (method && strcmp(method, "read") == 0) {
        if (sig && strcmp(sig, "()I") == 0)
            return asset_stream_read_byte(obj);
        if (!args) return -1;
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        struct fake_array *bytes = fake_array_find(jv[0].l);
        int offset = 0;
        int count = bytes ? bytes->len : 0;
        if (sig && strcmp(sig, "([BII)I") == 0) {
            offset = jv[1].i;
            count = jv[2].i;
        }
        return asset_stream_read_array(obj, jv[0].l, offset, count);
    }
    /* MediaPlayer fake — mesmo tratamento do CallIntMethodV acima. */
    if (method && strcmp(method, "getVideoWidth") == 0) return 640;
    if (method && strcmp(method, "getVideoHeight") == 0) return 360;
    if (method && class_name &&
        strcmp(class_name, "android.media.MediaPlayer") == 0) {
        if (strcmp(method, "getDuration") == 0) return 0;
        if (strcmp(method, "getCurrentPosition") == 0) return 0;
    }
    if (method && strcmp(method, "getWidth") == 0) return sdv_egl_width();
    if (method && strcmp(method, "getHeight") == 0) return sdv_egl_height();
    if (method && strcmp(method, "eglGetError") == 0) return 0x3000; /* EGL_SUCCESS */
    return 0;
}

/* Xamarin usa a chamada nonvirtual quando o peer gerenciado representa uma
 * subclasse Java. View.Width/Height passam por esse caminho no GameView; deixar
 * os slots no stub generico devolve o registrador de retorno de uma funcao com
 * assinatura incompatível e transforma o viewport em valores aleatorios. */
static int CallNonvirtualIntMethodV(void *e, void *obj, void *cls,
                                    void *mid, va_list a) {
    (void)cls;
    return CallIntMethodV(e, obj, mid, a);
}
static int CallNonvirtualIntMethod(void *e, void *obj, void *cls,
                                   void *mid, ...) {
    int result;
    va_list a;
    va_start(a, mid);
    result = CallNonvirtualIntMethodV(e, obj, cls, mid, a);
    va_end(a);
    return result;
}
static int CallNonvirtualIntMethodA(void *e, void *obj, void *cls,
                                    void *mid, const void *args) {
    (void)cls;
    return CallIntMethodA(e, obj, mid, args);
}
static int CallStaticIntMethodV(void *e, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; LOG_CALL("CallStaticIntMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "identityHashCode") == 0) {
        uintptr_t p = (uintptr_t)va_arg(a, void *);
        uint32_t h = (uint32_t)(p ^ (p >> 32));
        return h ? (int)h : 1;
    }
    if (method && method[1] == '\0' &&
        (method[0] == 'e' || method[0] == 'w' || method[0] == 'd' ||
         method[0] == 'i' || method[0] == 'v')) {
        void *tag = va_arg(a, void *);
        void *msg = va_arg(a, void *);
        fprintf(stderr, "[java-log.%s] %s: %s\n", method, safe_cstr(tag),
                safe_cstr(msg));
    }
    return 0;
}
static int CallStaticIntMethod(void *e, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); int r = CallStaticIntMethodV(e, cls, mid, a); va_end(a); return r;
}
static int CallStaticIntMethodA(void *e, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; LOG_CALL("CallStaticIntMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "identityHashCode") == 0 && args) {
        uintptr_t p = (uintptr_t)*(void * const *)args;
        uint32_t h = (uint32_t)(p ^ (p >> 32));
        return h ? (int)h : 1;
    }
    /* android.util.Log: o MonoGame reporta a deteccao de controles por aqui
     * ("Found new controller [n] ..."), que e exatamente o que se precisa ver
     * quando o pad nao responde. */
    if (method && method[1] == '\0' && args &&
        (method[0] == 'e' || method[0] == 'w' || method[0] == 'd' ||
         method[0] == 'i' || method[0] == 'v')) {
        const void *const *jv = (const void *const *)args;
        fprintf(stderr, "[java-log.%s] %s: %s\n", method,
                safe_cstr((void *)jv[0]), safe_cstr((void *)jv[1]));
    }
    return 0;
}

static long fake_file_usable_space(void) {
    struct statvfs fs;
    const char *path = getenv("HOME");

    if (!path || !path[0]) path = ".";
    if (statvfs(path, &fs) == 0) {
        uint64_t block_size = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
        uint64_t bytes = (uint64_t)fs.f_bavail * block_size;
        if (bytes > (uint64_t)INT64_MAX) bytes = (uint64_t)INT64_MAX;
        fprintf(stderr, "[jni-file] usable space: %.1f MiB (%s)\n",
                (double)bytes / (1024.0 * 1024.0), path);
        return (long)bytes;
    }
    fprintf(stderr, "[jni-file] statvfs failed for %s\n", path);
    return 0;
}

static long CallLongMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; LOG_CALL("CallLongMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor")) {
        if (strcmp(method, "getStartOffset") == 0) return 0;
        if (strcmp(method, "getLength") == 0 ||
            strcmp(method, "getDeclaredLength") == 0)
            return (long)audio_fd_length(obj);
    }
    if (method && strcmp(method, "skip") == 0)
        return (long)asset_stream_skip(obj, va_arg(a, int64_t));
    if (method && strcmp(method, "getUsableSpace") == 0)
        return fake_file_usable_space();
    if (method && strcmp(method, "getSampleTime") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor"))
        return 0;
    return 0;
}
static long CallLongMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); long r = CallLongMethodV(e, obj, mid, a); va_end(a); return r;
}
static long CallLongMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; LOG_CALL("CallLongMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor")) {
        if (strcmp(method, "getStartOffset") == 0) return 0;
        if (strcmp(method, "getLength") == 0 ||
            strcmp(method, "getDeclaredLength") == 0)
            return (long)audio_fd_length(obj);
    }
    if (method && strcmp(method, "skip") == 0 && args) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return (long)asset_stream_skip(obj, jv[0].j);
    }
    if (method && strcmp(method, "getUsableSpace") == 0)
        return fake_file_usable_space();
    if (method && strcmp(method, "getSampleTime") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor"))
        return 0;
    return 0;
}
static float CallFloatMethodCore(void *obj, void *mid, int axis) {
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int handled = 0;
    float value = motion_event_float(obj, method, axis, &handled);

    return handled ? value : 0.0f;
}
static float CallFloatMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; LOG_CALL("CallFloatMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int axis = method && (strcmp(method, "getAxisValue") == 0 ||
                          strcmp(method, "GetAxisValue") == 0)
        ? va_arg(a, int) : -1;
    return CallFloatMethodCore(obj, mid, axis);
}
static float CallFloatMethod(void *e, void *obj, void *mid, ...) {
    float result;
    va_list a;
    va_start(a, mid);
    result = CallFloatMethodV(e, obj, mid, a);
    va_end(a);
    return result;
}
static float CallFloatMethodA(void *e, void *obj, void *mid,
                              const void *args) {
    (void)e; LOG_CALL("CallFloatMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int axis = -1;
    if (args && method && (strcmp(method, "getAxisValue") == 0 ||
                           strcmp(method, "GetAxisValue") == 0)) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        axis = jv[0].i;
    }
    return CallFloatMethodCore(obj, mid, axis);
}
static float CallNonvirtualFloatMethodV(void *e, void *obj, void *cls,
                                        void *mid, va_list a) {
    (void)cls;
    return CallFloatMethodV(e, obj, mid, a);
}
static float CallNonvirtualFloatMethod(void *e, void *obj, void *cls,
                                       void *mid, ...) {
    float result;
    va_list a;
    va_start(a, mid);
    result = CallNonvirtualFloatMethodV(e, obj, cls, mid, a);
    va_end(a);
    return result;
}
static float CallNonvirtualFloatMethodA(void *e, void *obj, void *cls,
                                        void *mid, const void *args) {
    (void)cls;
    return CallFloatMethodA(e, obj, mid, args);
}
/* requestPlayerAccess(ICallback): SDK real responderia async com onResult.
 * Registramos o binding ICallback do NetflixManager (mesma mecanica do
 * n_onCreate no main.c) e entregamos sincronamente um PlayerAccessInfo fake.
 * Isso limpa _requestProcessing e destrava o boot do jogo. */
extern uintptr_t g_runtime_register;
static void jni_netflix_deliver_access(void *env, void *callback) {
    static int registered;
    if (!callback) {
        fprintf(stderr, "[netflix] requestPlayerAccess sem callback\n");
        return;
    }
    if (!registered) {
        if (!g_runtime_register) {
            fprintf(stderr, "[netflix] Runtime_register indisponivel\n");
            return;
        }
        static const char methods[] =
            "n_onResult:(Lcom/netflix/games/NetflixResult;)V:"
            "GetOnResult_Lcom_netflix_games_NetflixResult_Handler:"
            "Com.Netflix.Games.ICallbackInvoker, Netflix\n";
        typedef void (*runtime_register_t)(void *, void *, void *, void *,
                                           void *);
        ((runtime_register_t)g_runtime_register)(env, (void *)0xC1A500,
            (void *)"Paris.NetflixManager, ParisEngine",
            jni_make_class("crc6411146ee806c650fb.NetflixManager"),
            (void *)methods);
        registered = 1;
        fprintf(stderr, "[netflix] Runtime.register(NetflixManager/ICallback) OK\n");
    }
    void *fn = jni_find_registered_native(
        "n_onResult", "(Lcom/netflix/games/NetflixResult;)V");
    if (!fn) {
        fprintf(stderr, "[netflix] n_onResult nao registrado\n");
        return;
    }
    void *result = netflix_result_with(
        "com.netflix.games.player.access.PlayerAccessInfo");
    fprintf(stderr, "[netflix] entregando PlayerAccessInfo fake via n_onResult\n");
    typedef void (*on_result_t)(void *, void *, void *);
    ((on_result_t)fn)(env, callback, result);
    fprintf(stderr, "[netflix] n_onResult RETORNOU\n");
}

static void CallVoidMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; LOG_CALL("CallVoidMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (media_player_void_call_v(obj, method, a)) return;
    if (method && strcmp(method, "setDataSource") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        audio_extractor_set_source(obj, va_arg(a, void *));
        return;
    }
    if (method && strcmp(method, "selectTrack") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        (void)va_arg(a, int);
        return;
    }
    if (method && strcmp(method, "configure") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        void *format = va_arg(a, void *);
        (void)va_arg(a, void *); /* Surface */
        (void)va_arg(a, void *); /* MediaCrypto */
        (void)va_arg(a, int);    /* flags */
        audio_codec_configure(obj, format);
        return;
    }
    if (method && strcmp(method, "queueInputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        (void)va_arg(a, int); /* index */
        (void)va_arg(a, int); /* offset */
        int size = va_arg(a, int);
        (void)va_arg(a, int64_t); /* presentationTimeUs */
        int flags = va_arg(a, int);
        audio_queue_input(obj, size, flags);
        return;
    }
    if (method && strcmp(method, "releaseOutputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        (void)va_arg(a, int);
        (void)va_arg(a, int);
        audio_release_output(obj);
        return;
    }
    if (method && strcmp(method, "seekTo") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        int64_t position_us = va_arg(a, int64_t);
        (void)va_arg(a, int);
        audio_seek(obj, position_us);
        return;
    }
    if (method && strcmp(method, "release") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        audio_codec_release(obj);
        return;
    }
    if (method && (strcmp(method, "start") == 0 ||
                   strcmp(method, "stop") == 0) &&
        fake_object_is_class(obj, "android.media.MediaCodec"))
        return;
    if (method && strcmp(method, "close") == 0 &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor")) {
        audio_close_fd(obj);
        return;
    }
    if (method && strcmp(method, "close") == 0)
        asset_stream_close(obj);
    if (method && strcmp(method, "requestPlayerAccess") == 0)
        jni_netflix_deliver_access(e, va_arg(a, void *));
}
static void CallVoidMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); CallVoidMethodV(e, obj, mid, a); va_end(a);
}
static void CallVoidMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; LOG_CALL("CallVoidMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    const union fake_jvalue *jv = (const union fake_jvalue *)args;
    if (media_player_void_call(obj, method, jv)) return;
    if (method && strcmp(method, "setDataSource") == 0 && jv &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        audio_extractor_set_source(obj, jv[0].l);
        return;
    }
    if (method && strcmp(method, "selectTrack") == 0 &&
        fake_object_is_class(obj, "android.media.MediaExtractor"))
        return;
    if (method && strcmp(method, "configure") == 0 && jv &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        audio_codec_configure(obj, jv[0].l);
        return;
    }
    if (method && strcmp(method, "queueInputBuffer") == 0 && jv &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        audio_queue_input(obj, jv[2].i, jv[4].i);
        return;
    }
    if (method && strcmp(method, "releaseOutputBuffer") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        audio_release_output(obj);
        return;
    }
    if (method && strcmp(method, "seekTo") == 0 && jv &&
        fake_object_is_class(obj, "android.media.MediaExtractor")) {
        audio_seek(obj, jv[0].j);
        return;
    }
    if (method && strcmp(method, "release") == 0 &&
        fake_object_is_class(obj, "android.media.MediaCodec")) {
        audio_codec_release(obj);
        return;
    }
    if (method && (strcmp(method, "start") == 0 ||
                   strcmp(method, "stop") == 0) &&
        fake_object_is_class(obj, "android.media.MediaCodec"))
        return;
    if (method && strcmp(method, "close") == 0 &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor")) {
        audio_close_fd(obj);
        return;
    }
    if (method && strcmp(method, "close") == 0)
        asset_stream_close(obj);
    if (method && strcmp(method, "requestPlayerAccess") == 0) {
        jni_netflix_deliver_access(e, jv ? jv[0].l : NULL);
    }
}
static void *CallNonvirtualObjectMethodV(void *e, void *obj, void *cls, void *mid, va_list a) {
    (void)e; (void)cls;
    {   /* Activity.getSystemService chega por aqui: o Java.Interop invoca os
         * metodos herdados de Context de forma NAO-virtual. Sem este ramo o
         * servico voltava como java.lang.Object generico e o cast do binding
         * derrubava o OnCreate com InvalidCastException. */
        int id = (int)(intptr_t)mid;
        const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
        void *out = NULL;
        if (method && (strcmp(method, "getSystemService") == 0 ||
                       strcmp(method, "getProperty") == 0) &&
            object_string_arg_call(obj, method, safe_cstr(va_arg(a, void *)), &out))
            return out;
    }
    (void)a; return CallObjectMethodCore(obj, mid);
}
static void *CallNonvirtualObjectMethod(void *e, void *obj, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid);
    void *r = CallNonvirtualObjectMethodV(e, obj, cls, mid, a);
    va_end(a); return r;
}
static void *CallNonvirtualObjectMethodA(void *e, void *obj, void *cls, void *mid, const void *args) {
    (void)e; (void)cls;
    {
        int id = (int)(intptr_t)mid;
        const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
        if (method && args && (strcmp(method, "getSystemService") == 0 ||
                               strcmp(method, "getProperty") == 0)) {
            const union fake_jvalue *jv = (const union fake_jvalue *)args;
            void *out = NULL;
            if (object_string_arg_call(obj, method, safe_cstr(jv[0].l), &out))
                return out;
        }
    }
    (void)args; return CallObjectMethodCore(obj, mid);
}
static unsigned char NonvirtualBooleanCore(void *mid) {
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && (strcmp(method, "requestWindowFeature") == 0 ||
                   strcmp(method, "requestFocus") == 0 ||
                   strcmp(method, "mkdirs") == 0 ||
                   strcmp(method, "canDetectOrientation") == 0)) return 1;
    return 0;
}
static unsigned char CallNonvirtualBooleanMethodV(void *e, void *obj, void *cls, void *mid, va_list a) {
    (void)e; (void)obj; (void)cls; (void)a; LOG_CALL("CallNonvirtualBooleanMethodV", mid);
    return NonvirtualBooleanCore(mid);
}
static unsigned char CallNonvirtualBooleanMethod(void *e, void *obj, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid);
    unsigned char r = CallNonvirtualBooleanMethodV(e, obj, cls, mid, a);
    va_end(a); return r;
}
static unsigned char CallNonvirtualBooleanMethodA(void *e, void *obj, void *cls, void *mid, const void *args) {
    (void)e; (void)obj; (void)cls; (void)args; LOG_CALL("CallNonvirtualBooleanMethodA", mid);
    return NonvirtualBooleanCore(mid);
}
static void CallNonvirtualVoidMethodV(void *e, void *obj, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; LOG_CALL("CallNonvirtualVoidMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (media_player_void_call_v(obj, method, a)) return;
    if (method && strcmp(method, "close") == 0 &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor"))
        audio_close_fd(obj);
    else if (method && strcmp(method, "close") == 0)
        asset_stream_close(obj);
}
static void CallNonvirtualVoidMethod(void *e, void *obj, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); CallNonvirtualVoidMethodV(e, obj, cls, mid, a); va_end(a);
}
static void CallNonvirtualVoidMethodA(void *e, void *obj, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; LOG_CALL("CallNonvirtualVoidMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (media_player_void_call(obj, method,
                               (const union fake_jvalue *)args)) return;
    if (method && strcmp(method, "close") == 0 &&
        fake_object_is_class(obj,
                             "android.content.res.AssetFileDescriptor"))
        audio_close_fd(obj);
    else if (method && strcmp(method, "close") == 0)
        asset_stream_close(obj);
}
static void CallStaticVoidMethodV(void *e, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; (void)a; LOG_CALL("CallStaticVoidMethodV", mid);
}
static void CallStaticVoidMethod(void *e, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); CallStaticVoidMethodV(e, cls, mid, a); va_end(a);
}
static void CallStaticVoidMethodA(void *e, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; (void)args; LOG_CALL("CallStaticVoidMethodA", mid);
}

/* fields: default 0/NULL, mas ObjectField devolve token de classe nao-nulo
 * (Mono le campos Class via GetStaticObjectField p/ carregar classes Java como
 * mono.android.GCUserPeer; NULL => abort "Failed to load"). */
static int GetIntField(void *e, void *obj, void *fid) {
    (void)e; (void)obj;
    int id = (int)(intptr_t)fid;
    const char *name = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (name && strcmp(name, "flags") == 0)
        return audio_buffer_flags(obj);
    if (name && (strcmp(name, "x") == 0 || strcmp(name, "widthPixels") == 0))
        return sdv_egl_width();
    if (name && (strcmp(name, "y") == 0 || strcmp(name, "heightPixels") == 0))
        return sdv_egl_height();
    if (name && strcmp(name, "densityDpi") == 0) return 160;
    return 0;
}
static float GetFloatField(void *e, void *obj, void *fid) {
    (void)e; (void)obj;
    int id = (int)(intptr_t)fid;
    const char *name = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (name && (strcmp(name, "xdpi") == 0 || strcmp(name, "ydpi") == 0)) return 160.0f;
    if (name && strcmp(name, "density") == 0) return 1.0f;
    return 0.0f;
}
static void *GetObjectField(void *e, void *obj, void *fid) { (void)e; (void)obj; (void)fid; return (void *)g_class_token; }
static void *GetStaticObjectField(void *e, void *cls, void *fid) {
    (void)e; (void)cls;
    int id = (int)(intptr_t)fid;
    const char *name = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (name && strcmp(name, "Context") == 0 && g_activity) return g_activity;
    if (name && strcmp(name, "DIRECTORY_PICTURES") == 0) return strdup("Pictures");
    if (name && strcmp(name, "EGL_DEFAULT_DISPLAY") == 0)
        return fake_singleton(&g_egl_default_display, "java.lang.Object");
    if (name && strcmp(name, "EGL_NO_DISPLAY") == 0)
        return fake_singleton(&g_egl_no_display,
                              "javax.microedition.khronos.egl.EGLDisplay");
    if (name && strcmp(name, "EGL_NO_CONTEXT") == 0)
        return fake_singleton(&g_egl_no_context,
                              "javax.microedition.khronos.egl.EGLContext");
    if (name && strcmp(name, "EGL_NO_SURFACE") == 0)
        return fake_singleton(&g_egl_no_surface,
                              "javax.microedition.khronos.egl.EGLSurface");
    if (name && strcmp(name, "mono_android_GCUserPeer") == 0)
        return jni_make_class("mono.android.GCUserPeer");
    if (name && strcmp(name, "mono_android_IGCUserPeer") == 0)
        return jni_make_class("mono.android.IGCUserPeer");
    return (void *)g_class_token;
}
static int GetStaticIntField(void *e, void *cls, void *fid) { (void)e; (void)cls; (void)fid; return 0; }
static void SetStaticIntField(void *e, void *cls, void *fid, int v) { (void)e; (void)cls; (void)fid; (void)v; }
static void *GetObjectClass(void *e, void *obj) {
    (void)e;
    struct fake_array *a = fake_array_find(obj);
    if (a) {
        if (a->kind == FAKE_ARRAY_BOOLEAN)
            return jni_make_class("[Z");
        if (a->kind == FAKE_ARRAY_INT)
            return jni_make_class("[I");
        if (a->kind == FAKE_ARRAY_BYTE)
            return jni_make_class("[B");
        const char *element = fake_class_name(a->element_class);
        if (element) {
            size_t n = strlen(element) + 4;
            char *name = malloc(n);
            snprintf(name, n, "[L%s;", element);
            void *klass = jni_make_class(name);
            free(name);
            return klass;
        }
        return jni_make_class("[Ljava.lang.Object;");
    }
    void *klass = fake_object_class(obj);
    return klass ? klass : (void *)g_class_token;
}

/* lifecycle de refs/objetos. Os objetos Java sao apenas handles opacos, mas
 * precisam manter a classe para o typemap e a ativacao do peer gerenciado. */
static int Throw(void *e, void *obj) { (void)e; (void)obj; return 0; }
static int ThrowNew(void *e, void *cls, const char *msg) { (void)e; (void)cls; (void)msg; return 0; }
static unsigned char IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
static int EnsureLocalCapacity(void *e, int cap) { (void)e; (void)cap; return 0; }
static void *AllocObject(void *e, void *cls) { (void)e; return jni_make_object(cls); }
static void *NewObjectV(void *e, void *cls, void *mid, va_list a) {
    (void)e; (void)mid; (void)a; return jni_make_object(cls);
}
static void *NewObject(void *e, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); void *r = NewObjectV(e, cls, mid, a); va_end(a); return r;
}
static void *NewObjectA(void *e, void *cls, void *mid, const void *args) {
    (void)e; (void)mid; (void)args; return jni_make_object(cls);
}
static unsigned char IsInstanceOf(void *e, void *obj, void *cls) {
    (void)e; (void)cls; return obj != NULL;
}

/* exception: sempre "nenhuma". CRITICO: ExceptionOccurred/ExceptionCheck DEVEM
 * cair nos offsets corretos (idx 15 = 0x78, idx 228 = 0x720). Se ficarem no
 * default ret_obj (0xC1A501 nao-NULL), o managed .NET acha que SEMPRE ha excecao
 * pendente apos cada FindClass -> GetExceptionForThrowable -> embrulha Throwable
 * -> typemap falha ("Could not determine Java type ... Java.Lang.Throwable"). */
static void ExceptionClear(void *e) { (void)e; }
static int  ExceptionCheck(void *e) { (void)e; return 0; }
static void *ExceptionOccurred(void *e) { (void)e; return NULL; }

/* frame local (idx 19/20): no-op, sucesso. PushLocalFrame DEVE devolver 0
 * (jint), senao o managed trata como falha de alocacao de frame. */
static int PushLocalFrame(void *e, int cap) { (void)e; (void)cap; return 0; }
static void *PopLocalFrame(void *e, void *result) { (void)e; return result; }

/* ---- JavaVM ------------------------------------------------------------ */
static int GetEnv(void *vm, void **env, int version) {
    (void)vm; (void)version; *env = fake_env; return 0;
}
static int AttachCurrentThread(void *vm, void **env, void *args) {
    (void)vm; (void)args; *env = fake_env; return 0;
}
static int DetachCurrentThread(void *vm) { (void)vm; return 0; }
/* GetJavaVM (JNIEnv slot 0x6D8): Mono le o JavaVM via env->GetJavaVM(&vm) p/
 * passar a JNIEnvInit (args.javaVm). Slot default nao seta o out-param -> vm=0
 * -> "No JavaVM registered with handle 0x0". Setamos *vm = fake_vm. */
static int GetJavaVM(void *e, void **vm) { (void)e; *vm = (void *)fake_vm; return 0; }


/* ---- build vtables (offsets Bionic arm64 LP64) ------------------------- */
#define SET(off, fn) *(uintptr_t *)(fake_env + (off)) = (uintptr_t)(fn)
static void build_env(void) {
    for (unsigned i = 0; i < sizeof(fake_env)/sizeof(uintptr_t); i++)
        ((uintptr_t *)fake_env)[i] = (uintptr_t)ret_obj;  /* default nao-NULL */
    *(uintptr_t *)fake_env = (uintptr_t)fake_env;   /* JNIEnv -> itself */
    SET(0x30,  FindClass);
    SET(0x68,  Throw);              /* idx 13 */
    SET(0x70,  ThrowNew);           /* idx 14 */
    SET(0x78,  ExceptionOccurred);  /* idx 15 (ANTES 0x90=FatalError — BUG: caia no default nao-NULL) */
    SET(0x88,  ExceptionClear);     /* idx 17 */
    SET(0x98,  PushLocalFrame);     /* idx 19 */
    SET(0xA0,  PopLocalFrame);      /* idx 20 */
    SET(0x720, ExceptionCheck);     /* idx 228 (ANTES 0x98=PushLocalFrame — BUG) */
    SET(0xA8,  NewGlobalRef);
    SET(0xB0,  DeleteGlobalRef);
    SET(0xB8,  DeleteLocalRef);
    SET(0xC0,  IsSameObject);       /* idx 24 */
    SET(0xC8,  NewLocalRef);        /* idx 25 (ANTES em 0xE0=NewObject) */
    SET(0xD0,  EnsureLocalCapacity);
    SET(0xD8,  AllocObject);
    SET(0xE0,  NewObject);
    SET(0xE8,  NewObjectV);
    SET(0xF0,  NewObjectA);
    SET(0xF8,  GetObjectClass);       /* idx 31 */
    SET(0x100, IsInstanceOf);
    SET(0x108, GetMethodID);
    SET(0x110, CallObjectMethod);
    SET(0x118, CallObjectMethodV);
    SET(0x120, CallObjectMethodA);
    SET(0x128, CallBooleanMethod);
    SET(0x130, CallBooleanMethodV);
    SET(0x138, CallBooleanMethodA);
    SET(0x188, CallIntMethod);
    SET(0x190, CallIntMethodV);
    SET(0x198, CallIntMethodA);
    SET(0x1A0, CallLongMethod);
    SET(0x1A8, CallLongMethodV);     /* ANTES 0x1C8=CallFloatMethodA */
    SET(0x1B0, CallLongMethodA);
    SET(0x1B8, CallFloatMethod);
    SET(0x1C0, CallFloatMethodV);
    SET(0x1C8, CallFloatMethodA);
    SET(0x1E8, CallVoidMethod);
    SET(0x1F0, CallVoidMethodV);
    SET(0x1F8, CallVoidMethodA);
    SET(0x200, CallNonvirtualObjectMethod);
    SET(0x208, CallNonvirtualObjectMethodV);
    SET(0x210, CallNonvirtualObjectMethodA);
    SET(0x218, CallNonvirtualBooleanMethod);
    SET(0x220, CallNonvirtualBooleanMethodV);
    SET(0x228, CallNonvirtualBooleanMethodA);
    SET(0x278, CallNonvirtualIntMethod);
    SET(0x280, CallNonvirtualIntMethodV);
    SET(0x288, CallNonvirtualIntMethodA);
    SET(0x2A8, CallNonvirtualFloatMethod);
    SET(0x2B0, CallNonvirtualFloatMethodV);
    SET(0x2B8, CallNonvirtualFloatMethodA);
    SET(0x2D8, CallNonvirtualVoidMethod);
    SET(0x2E0, CallNonvirtualVoidMethodV);
    SET(0x2E8, CallNonvirtualVoidMethodA);
    SET(0x2F0, GetFieldID);
    SET(0x2F8, GetObjectField);      /* ANTES 0x308=GetByteField */
    SET(0x320, GetIntField);
    SET(0x330, GetFloatField);       /* idx 102 */
    SET(0x388, GetStaticMethodID);
    SET(0x390, CallStaticObjectMethod);
    SET(0x398, CallStaticObjectMethodV);
    SET(0x3A0, CallStaticObjectMethodA);
    SET(0x3A8, CallStaticBooleanMethod);
    SET(0x3B0, CallStaticBooleanMethodV);
    SET(0x3B8, CallStaticBooleanMethodA);
    SET(0x408, CallStaticIntMethod);
    SET(0x410, CallStaticIntMethodV);
    SET(0x418, CallStaticIntMethodA);
    SET(0x468, CallStaticVoidMethod);
    SET(0x470, CallStaticVoidMethodV);
    SET(0x478, CallStaticVoidMethodA);
    SET(0x480, GetStaticFieldID);
    SET(0x488, GetStaticObjectField);
    SET(0x4B0, GetStaticIntField);
    SET(0x4F8, SetStaticIntField);
    SET(0x518, NewString);             /* idx 163 (UTF-16) */
    SET(0x520, GetStringLength);       /* idx 164 (UTF-16) */
    SET(0x528, GetStringChars);        /* idx 165 (UTF-16) */
    SET(0x530, ReleaseStringChars);    /* idx 166 */
    SET(0x538, NewStringUTF);          /* idx 167 */
    SET(0x540, GetStringUTFLength);    /* idx 168 (ANTES 0x53C — desalinhado) */
    SET(0x548, GetStringUTFChars);     /* idx 169 */
    SET(0x550, ReleaseStringUTFChars); /* idx 170 */
    SET(0x558, GetArrayLength);        /* idx 171 */
    SET(0x560, NewObjectArray);        /* idx 172 */
    SET(0x568, GetObjectArrayElement);
    SET(0x570, SetObjectArrayElement);
    SET(0x578, NewBooleanArray);         /* idx 175 */
    SET(0x580, NewByteArray);            /* idx 176 */
    SET(0x598, NewIntArray);             /* idx 179 */
    SET(0x5B8, GetBooleanArrayElements); /* idx 183 */
    SET(0x5C0, GetByteArrayElements);    /* idx 184 */
    SET(0x5D8, GetIntArrayElements);     /* idx 187 */
    SET(0x5F8, ReleaseBooleanArrayElements); /* idx 191 */
    SET(0x600, ReleaseByteArrayElements); /* idx 192 */
    SET(0x618, ReleaseIntArrayElements); /* idx 195 */
    SET(0x638, GetBooleanArrayRegion);   /* idx 199 */
    SET(0x640, GetByteArrayRegion);      /* idx 200 */
    SET(0x658, GetIntArrayRegion);       /* idx 203 */
    SET(0x678, SetBooleanArrayRegion);   /* idx 207 */
    SET(0x680, SetByteArrayRegion);      /* idx 208 */
    SET(0x698, SetIntArrayRegion);       /* idx 211 */
    SET(0x6F0, GetPrimitiveArrayCritical);     /* idx 222 */
    SET(0x6F8, ReleasePrimitiveArrayCritical); /* idx 223 */
    SET(0x6B8, RegisterNatives);
    SET(0x6D8, GetJavaVM);
}
#undef SET

static void build_vm(void) {
    for (unsigned i = 0; i < sizeof(fake_vm)/sizeof(uintptr_t); i++)
        ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
    *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
    *(uintptr_t *)(fake_vm + 0x20) = (uintptr_t)AttachCurrentThread;
    *(uintptr_t *)(fake_vm + 0x28) = (uintptr_t)DetachCurrentThread;
    *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)GetEnv;
    *(uintptr_t *)(fake_vm + 0x38) = (uintptr_t)AttachCurrentThread;
}

void *jni_build_env(void) {
    const char *v = getenv("SB_JNI_VERBOSE");
    if (v && (*v == '0')) g_verbose = 0;
    build_env();
    build_vm();
    return (void *)fake_vm;
}

void *jni_env_ptr(void) { return (void *)fake_env; }

void *jni_find_registered_native(const char *name, const char *sig) {
    if (!name) return NULL;
    for (int i = g_natives_n - 1; i >= 0; i--) {
        if (!g_natives[i].name || strcmp(g_natives[i].name, name) != 0) continue;
        if (sig && (!g_natives[i].sig || strcmp(g_natives[i].sig, sig) != 0)) continue;
        return g_natives[i].fn;
    }
    return NULL;
}
