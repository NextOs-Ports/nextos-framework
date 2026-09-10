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
#include <sys/statvfs.h>

#include "jni_shim.h"
/* util.h nao entra aqui: este arquivo tem um `ret0` estatico proprio (handler
 * JNI) que conflita com o `ret0` do util.h. Basta declarar o que se usa. */
#include <stddef.h>
const char *sdv_path_in_base(char *buf, size_t bufsz, const char *sub);
#include "sdv_egl_bridge.h"

static char fake_vm[0x1000];
static char fake_env[0x1000];

static int g_verbose = 0;

void jni_set_verbose(int on) { g_verbose = on; }

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
    float x;
    float y;
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

static int asset_verbose(void) {
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("SDV_ASSET_VERBOSE");
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
static void *g_input_device;
static void *g_input_vibrator;
static struct fake_motion_event g_motion_event;
static void *g_motion_event_object;

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

void jni_set_key_event_keycode(void *event, int keycode) {
    /* +1 preserva o keycode zero sem confundi-lo com payload NULL. */
    fake_object_set_payload(event, (void *)(intptr_t)(keycode + 1));
}

void jni_set_motion_event(void *event, int action, float x, float y) {
    g_motion_event.action = action;
    g_motion_event.x = x;
    g_motion_event.y = y;
    g_motion_event_object = event;
    fake_object_set_payload(event, &g_motion_event);
}

static void *input_device(void) {
    if (!g_input_device)
        g_input_device = jni_make_object(jni_make_class("android.view.InputDevice"));
    return g_input_device;
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

/* Dir de dados do jogo (saves). No Android real vem de
 * Context.getFilesDir().getCanonicalPath(); aqui e' derivado do HOME que o
 * launcher aponta pro dir do port. Cravar o caminho do NextOS fazia o
 * SaveGame.Save estourar DirectoryNotFoundException em QUALQUER outro CFW
 * (no R36S/ArkOS o port mora em /roms/ports/sdvnextos) — o jogo fechava
 * exatamente no "Saving..." ao entrar no gameplay.
 * No Mali-450 HOME = /storage/roms/ports/stardewvalley, entao o resultado e'
 * identico ao de antes e os saves existentes continuam valendo. */
#include <sys/stat.h>
#include <sys/types.h>
static const char *sdv_data_dir(void) {
    static char dir[4096];
    if (dir[0]) return dir;
    const char *e = getenv("SDV_DATA_DIR");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else         sdv_path_in_base(dir, sizeof dir, "data");
    /* NAO usar $HOME aqui: o Mono REESCREVE HOME durante o Runtime_init (aponta
     * pro dir de libs nativas do "app"), e o save ia parar em <port>/libs/data. */
    mkdir(dir, 0777);   /* o jogo assume que o Android ja criou */
    fprintf(stderr, "[sdv] data dir = %s\n", dir);
    return dir;
}

/* ---- Enquadramento da camera (o "zoom gigante" do celular) ----------------
 *
 * O jogo chama MobileDisplay.SetDisplaySettings(w, h, dpi) e tira dali o
 * ZoomScale. Como o DisplayMetrics era 160 dpi CRAVADO, o zoom saia sempre o
 * de celular, independente da tela: no painel 640x480 do R36S sobravam ~13
 * tiles de largura contra ~20 do PC.
 *
 * CURVA MEDIDA no aparelho (640x480, um boot por ponto, leitura validada
 * contra o PPI que o proprio jogo imprime):
 *
 *   dpi  |  80     96     120    142       160   200      240     320    420
 *   zoom | .5625  .5625  .5625  .665625   .75   .703125  .84375  1.125  1.4765625
 *
 * Sao DOIS regimes lineares, com um degrau de 3/4 entre eles:
 *   dpi <= 160:  zoom = dpi * 3/640      (exato em 120, 142 e 160)
 *   dpi >= 200:  zoom = dpi * 9/2560     (exato em 200, 240, 320 e 420)
 * e o jogo aplica PISO de 0.5625 (por isso 80 e 96 empatam com 120).
 *
 * O zoom mais aberto possivel e' esse piso, alcancado com dpi <= 120.
 *
 * Regra de portabilidade: fixar a LARGURA EM TILES, nao o dpi. O tile desenha
 * a 64 px, entao ver `tiles` tiles pede zoom = w/(64*tiles), e o dpi que
 * produz esse zoom sai invertendo o modelo. Assim 640x480, 854x480 e 1280x720
 * enquadram a mesma fatia de fazenda.
 *
 * Knobs: `SDV_DPI` crava o dpi; `SDV_TILES_X` muda o enquadramento a gosto. */

/* 15 tiles = o enquadramento APROVADO pelo NextOS no R36S (640x480 -> dpi 142,
 * zoom 0.665625). O padrao do celular era 13.3 ("zoom gigante"); o do PC, ~20.
 * Fixar tiles e nao dpi faz 854x480 e 1280x720 mostrarem essa mesma fatia. */
#define SDV_DEFAULT_TILES_X 15.0
#define SDV_ZOOM_FLOOR      0.5625    /* piso do proprio jogo */

/* Inverte a curva medida: do zoom desejado para o dpi que o produz. */
static double sdv_dpi_for_zoom(double zoom) {
    /* regime baixo (dpi <= 160): zoom = dpi * 3/640 */
    double d = zoom * 640.0 / 3.0;
    if (d <= 160.0) return d;
    /* regime alto (dpi >= 200): zoom = dpi * 9/2560 */
    return zoom * 2560.0 / 9.0;
}
static double sdv_zoom_for_dpi(double dpi) {
    double z = dpi <= 160.0 ? dpi * 3.0 / 640.0 : dpi * 9.0 / 2560.0;
    return z < SDV_ZOOM_FLOOR ? SDV_ZOOM_FLOOR : z;
}

static int sdv_screen_dpi(void) {
    static int dpi;
    if (dpi) return dpi;
    const char *env = getenv("SDV_DPI");
    if (env && *env) {
        dpi = atoi(env);
        if (dpi > 0) {
            fprintf(stderr, "[sdv] dpi=%d (SDV_DPI)\n", dpi);
            return dpi;
        }
    }

    int w = sdv_egl_width();
    if (w <= 0) w = 640;
    const char *t = getenv("SDV_TILES_X");
    double tiles = t && *t ? atof(t) : 0.0;
    if (tiles < 8.0 || tiles > 40.0) tiles = SDV_DEFAULT_TILES_X;

    double zoom = (double)w / (64.0 * tiles);
    dpi = (int)(sdv_dpi_for_zoom(zoom) + 0.5);
    if (dpi < 80) dpi = 80;
    if (dpi > 640) dpi = 640;

    double zoom_real = sdv_zoom_for_dpi(dpi);
    fprintf(stderr, "[sdv] dpi=%d -> zoom %.6f (%.1f tiles em %d px; alvo %.1f)\n",
            dpi, zoom_real, (double)w / (64.0 * zoom_real), w, tiles);
    return dpi;
}

static struct fake_stream *asset_stream_open(const char *asset_name) {
    static unsigned opened;
    const char *root = getenv("SDV_ASSET_DIR");
    char relative[2048];
    char full[4096];

    static char asset_root[4096];
    if (!root || !*root)
        root = sdv_path_in_base(asset_root, sizeof asset_root, "assets");
    if (!asset_name || !*asset_name || asset_name[0] == '/' ||
        strstr(asset_name, ".."))
        return NULL;

    size_t n = strlen(asset_name);
    if (n >= sizeof(relative)) return NULL;
    for (size_t i = 0; i <= n; i++)
        relative[i] = asset_name[i] == '\\' ? '/' : asset_name[i];
    if (snprintf(full, sizeof(full), "%s/%s", root, relative) >= (int)sizeof(full))
        return NULL;

    FILE *file = fopen(full, "rb");
    if (!file) {
        if (asset_verbose())
        fprintf(stderr, "[asset] MISS %s\n", full);
        return NULL;
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
void jni_set_main_looper_ready(int ready) { g_main_looper_ready = !!ready; }

static void *main_looper(void) {
    if (!g_main_looper)
        g_main_looper = jni_make_object(jni_make_class("android.os.Looper"));
    return g_main_looper;
}

/* Diretorio de libs do app (nativeLibraryDir) — devolvido como unico elemento
 * do array fake libFolders (token 0x4001). Default vazio ate jni_set_libdir. */
#define LIBFOLDERS_TOKEN ((void *)0x4001)
static char *g_libdir = NULL;
void jni_set_libdir(const char *path) { g_libdir = path ? strdup(path) : NULL; }

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
    int n = a ? a->len : ((array == LIBFOLDERS_TOKEN && g_libdir) ? 3 : 0);
    if (g_verbose) fprintf(stderr, "[jni] GetArrayLength(array=%p) -> %d\n", array, n);
    return n;
}
static void *GetObjectArrayElement(void *e, void *array, int i) {
    (void)e;
    if (array == LIBFOLDERS_TOKEN && g_libdir) {
        if (g_verbose) fprintf(stderr, "[jni] GetObjectArrayElement(libFolders, %d) -> \"%s\"\n", i, g_libdir);
        return g_libdir;   /* jstring = proprio path C (GetStringUTFChars trata) */
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
    return obj == g_motion_event_object ? &g_motion_event : NULL;
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
    if (strcmp(method, "getActionIndex") == 0 ||
        strcmp(method, "getPointerId") == 0 ||
        strcmp(method, "GetPointerId") == 0 ||
        strcmp(method, "getDeviceId") == 0) {
        *handled = 1;
        return 0;
    }
    if (strcmp(method, "getPointerCount") == 0) {
        *handled = 1;
        return 1;
    }
    if (strcmp(method, "getSource") == 0) {
        *handled = 1;
        return 0x00001002; /* Android SOURCE_TOUCHSCREEN */
    }
    return 0;
}

static float motion_event_float(void *obj, const char *method, int *handled) {
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
    return 0.0f;
}

static void *CallObjectMethodCore(void *obj, void *mid) {
    LOG_CALL("CallObjectMethod", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "getDevice") == 0) {
        void *klass = fake_object_class(obj);
        const char *name = fake_class_name(klass);
        if (name && strcmp(name, "android.view.KeyEvent") == 0)
            return input_device();
    }
    if (method && strcmp(method, "getDescriptor") == 0)
        return strdup("nextos-sdl-gamepad-0");
    if (method && strcmp(method, "getName") == 0) {
        void *klass = fake_object_class(obj);
        const char *class_name = fake_class_name(klass);
        if (class_name && strcmp(class_name, "android.view.InputDevice") == 0)
            return strdup("NextOS SDL Gamepad");
    }
    if (method && strcmp(method, "getVibrator") == 0) {
        if (!g_input_vibrator)
            g_input_vibrator = jni_make_object(jni_make_class("android.os.Vibrator"));
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
        return strdup("com.chucklefish.stardewvalley");
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
        return strdup(sdv_data_dir());
    if (method && strcmp(method, "getApplicationContext") == 0)
        return g_activity ? g_activity : obj;
    if (method && strcmp(method, "getBaseContext") == 0)
        return g_activity ? g_activity : obj;
    if (method && strcmp(method, "edit") == 0)
        return jni_make_object(jni_make_class("android.content.SharedPreferences$Editor"));
    if (method && strcmp(method, "registerReceiver") == 0)
        return NULL; /* nenhum sticky broadcast */
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
    return strdup("java.lang.Object");
}
static void *CallObjectMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; (void)a; return CallObjectMethodCore(obj, mid);
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
    if (method && strcmp(method, "getDevice") == 0) {
        (void)va_arg(a, int);
        return input_device();
    }
    if (method && (strcmp(method, "deviceHasKeys") == 0 ||
                   strcmp(method, "hasKeys") == 0))
        return all_keys_supported_array(va_arg(a, void *));
    if (method && strcmp(method, "getExternalStoragePublicDirectory") == 0)
        return jni_make_object(jni_make_class("java.io.File"));
    if (method && strcmp(method, "getDefaultSharedPreferences") == 0)
        return jni_make_object(jni_make_class("android.content.SharedPreferences"));
    if (method && strcmp(method, "myLooper") == 0)
        sdv_promote_current_mono_thread();
    if (method && (strcmp(method, "getMainLooper") == 0 ||
                   strcmp(method, "myLooper") == 0))
        return g_main_looper_ready ? main_looper() : NULL;
    if (method && strcmp(method, "getEGL") == 0)
        return jni_make_object(jni_make_class("javax.microedition.khronos.egl.EGL10"));
    return NULL;
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
    if (method && strcmp(method, "getDevice") == 0)
        return input_device();
    if (method && (strcmp(method, "deviceHasKeys") == 0 ||
                   strcmp(method, "hasKeys") == 0)) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return all_keys_supported_array(jv ? jv[0].l : NULL);
    }
    if (method && strcmp(method, "getExternalStoragePublicDirectory") == 0)
        return jni_make_object(jni_make_class("java.io.File"));
    if (method && strcmp(method, "getDefaultSharedPreferences") == 0)
        return jni_make_object(jni_make_class("android.content.SharedPreferences"));
    if (method && strcmp(method, "myLooper") == 0)
        sdv_promote_current_mono_thread();
    if (method && (strcmp(method, "getMainLooper") == 0 ||
                   strcmp(method, "myLooper") == 0))
        return g_main_looper_ready ? main_looper() : NULL;
    if (method && strcmp(method, "getEGL") == 0)
        return jni_make_object(jni_make_class("javax.microedition.khronos.egl.EGL10"));
    return NULL;
}
static unsigned char CallBooleanMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; (void)obj; (void)a; LOG_CALL("CallBooleanMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
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
    if (method && (strcmp(method, "requestWindowFeature") == 0 ||
                   strcmp(method, "requestFocus") == 0 ||
                   strcmp(method, "mkdirs") == 0 ||
                   strcmp(method, "commit") == 0 ||
                   strcmp(method, "apply") == 0)) return 1;
    if (method && strncmp(method, "egl", 3) == 0) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
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
static unsigned char CallStaticBooleanMethodV(void *e, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; (void)a; LOG_CALL("CallStaticBooleanMethodV", mid); return 0;
}
static unsigned char CallStaticBooleanMethod(void *e, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); unsigned char r = CallStaticBooleanMethodV(e, cls, mid, a); va_end(a); return r;
}
static unsigned char CallStaticBooleanMethodA(void *e, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; (void)args; LOG_CALL("CallStaticBooleanMethodA", mid); return 0;
}
static int CallIntMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; (void)obj; (void)a; LOG_CALL("CallIntMethodV", mid);
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
        if (strcmp(method, "getId") == 0) return 1;
    }
    if (method && class_name && strcmp(class_name, "android.view.KeyEvent") == 0) {
        if (strcmp(method, "getKeyCode") == 0) {
            intptr_t payload = (intptr_t)fake_object_payload(obj);
            return payload ? (int)payload - 1 : 0;
        }
        if (strcmp(method, "getDeviceId") == 0) return 1;
        if (strcmp(method, "getAction") == 0) return 0;
    }
    if (method && strcmp(method, "available") == 0)
        return asset_stream_available(obj);
    if (method && strcmp(method, "read") == 0 && sig && strcmp(sig, "()I") == 0)
        return asset_stream_read_byte(obj);
    if (method && strcmp(method, "getWidth") == 0) return sdv_egl_width();
    if (method && strcmp(method, "getHeight") == 0) return sdv_egl_height();
    if (method && strcmp(method, "eglGetError") == 0) return 0x3000;
    return 0;
}
static int CallIntMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); int r = CallIntMethodV(e, obj, mid, a); va_end(a); return r;
}
static int CallIntMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; (void)obj; (void)args; LOG_CALL("CallIntMethodA", mid);
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
        if (strcmp(method, "getId") == 0) return 1;
    }
    if (method && class_name && strcmp(class_name, "android.view.KeyEvent") == 0) {
        if (strcmp(method, "getKeyCode") == 0) {
            intptr_t payload = (intptr_t)fake_object_payload(obj);
            return payload ? (int)payload - 1 : 0;
        }
        if (strcmp(method, "getDeviceId") == 0) return 1;
        if (strcmp(method, "getAction") == 0) return 0;
    }
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
    if (method && (strcmp(method, "e") == 0 || strcmp(method, "w") == 0) && args) {
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
    if (method && strcmp(method, "skip") == 0)
        return (long)asset_stream_skip(obj, va_arg(a, int64_t));
    if (method && strcmp(method, "getUsableSpace") == 0)
        return fake_file_usable_space();
    return 0;
}
static long CallLongMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); long r = CallLongMethodV(e, obj, mid, a); va_end(a); return r;
}
static long CallLongMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; LOG_CALL("CallLongMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "skip") == 0 && args) {
        const union fake_jvalue *jv = (const union fake_jvalue *)args;
        return (long)asset_stream_skip(obj, jv[0].j);
    }
    if (method && strcmp(method, "getUsableSpace") == 0)
        return fake_file_usable_space();
    return 0;
}
static float CallFloatMethodCore(void *obj, void *mid) {
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    int handled = 0;
    float value = motion_event_float(obj, method, &handled);

    return handled ? value : 0.0f;
}
static float CallFloatMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; (void)a; LOG_CALL("CallFloatMethodV", mid);
    return CallFloatMethodCore(obj, mid);
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
    (void)e; (void)args; LOG_CALL("CallFloatMethodA", mid);
    return CallFloatMethodCore(obj, mid);
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
static void CallVoidMethodV(void *e, void *obj, void *mid, va_list a) {
    (void)e; (void)a; LOG_CALL("CallVoidMethodV", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "close") == 0)
        asset_stream_close(obj);
}
static void CallVoidMethod(void *e, void *obj, void *mid, ...) {
    va_list a; va_start(a, mid); CallVoidMethodV(e, obj, mid, a); va_end(a);
}
static void CallVoidMethodA(void *e, void *obj, void *mid, const void *args) {
    (void)e; (void)args; LOG_CALL("CallVoidMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "close") == 0)
        asset_stream_close(obj);
}
static void *CallNonvirtualObjectMethodV(void *e, void *obj, void *cls, void *mid, va_list a) {
    (void)e; (void)cls; (void)a; return CallObjectMethodCore(obj, mid);
}
static void *CallNonvirtualObjectMethod(void *e, void *obj, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid);
    void *r = CallNonvirtualObjectMethodV(e, obj, cls, mid, a);
    va_end(a); return r;
}
static void *CallNonvirtualObjectMethodA(void *e, void *obj, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; (void)args; return CallObjectMethodCore(obj, mid);
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
    (void)e; (void)obj; (void)cls; (void)a; LOG_CALL("CallNonvirtualVoidMethodV", mid);
}
static void CallNonvirtualVoidMethod(void *e, void *obj, void *cls, void *mid, ...) {
    va_list a; va_start(a, mid); CallNonvirtualVoidMethodV(e, obj, cls, mid, a); va_end(a);
}
static void CallNonvirtualVoidMethodA(void *e, void *obj, void *cls, void *mid, const void *args) {
    (void)e; (void)cls; (void)args; LOG_CALL("CallNonvirtualVoidMethodA", mid);
    int id = (int)(intptr_t)mid;
    const char *method = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (method && strcmp(method, "close") == 0)
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
    if (name && (strcmp(name, "x") == 0 || strcmp(name, "widthPixels") == 0))
        return sdv_egl_width();
    if (name && (strcmp(name, "y") == 0 || strcmp(name, "heightPixels") == 0))
        return sdv_egl_height();
    if (name && strcmp(name, "densityDpi") == 0) return sdv_screen_dpi();
    return 0;
}
static float GetFloatField(void *e, void *obj, void *fid) {
    (void)e; (void)obj;
    int id = (int)(intptr_t)fid;
    const char *name = (id > 0 && id <= reg_n) ? reg_names[id - 1] : NULL;
    if (name && (strcmp(name, "xdpi") == 0 || strcmp(name, "ydpi") == 0))
        return (float)sdv_screen_dpi();
    if (name && strcmp(name, "density") == 0) return sdv_screen_dpi() / 160.0f;
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
    const char *v = getenv("SDV_JNI_VERBOSE");
    g_verbose = v && v[0] && v[0] != '0';
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
