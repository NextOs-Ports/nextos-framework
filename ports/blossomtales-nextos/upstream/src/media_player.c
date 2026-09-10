/*
 * media_player.c -- ver media_player.h.
 *
 * Estado compartilhado (sob `lock`) e apenas o DESEJO do jogo: fonte, volume,
 * loop e want_state. Quem abre o decodificador, enfileira no OpenAL e mede a
 * posicao e a thread alimentadora, dona exclusiva do pipe e dos objetos AL.
 * Assim nenhuma chamada vinda do jogo bloqueia no `read` do decodificador.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <dlfcn.h>

extern char **environ;

#include "media_player.h"
#include "aac_decoder.h"

#define MP_RATE      48000
#define MP_CHANNELS  2
#define MP_BYTES_PER_FRAME (MP_CHANNELS * 2)
#define MP_BUFFERS   6
#define MP_CHUNK     (16384) /* ~85 ms; 6 buffers ~= 0,5 s de folga */

enum { MP_STATE_IDLE = 0, MP_STATE_STOPPED, MP_STATE_PLAYING, MP_STATE_PAUSED };

/* --- OpenAL carregado do sistema (o mesmo que o MonoGame ja usa) --------- */

#define AL_FORMAT_STEREO16   0x1103
#define AL_BUFFERS_QUEUED    0x1015
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_SOURCE_STATE      0x1010
#define AL_PLAYING           0x1012
#define AL_GAIN              0x100A
#define AL_BUFFER            0x1009
#define AL_SAMPLE_OFFSET     0x1025

struct al_api {
    void (*GenSources)(int, unsigned *);
    void (*DeleteSources)(int, const unsigned *);
    void (*GenBuffers)(int, unsigned *);
    void (*DeleteBuffers)(int, const unsigned *);
    void (*BufferData)(unsigned, int, const void *, int, int);
    void (*SourceQueueBuffers)(unsigned, int, const unsigned *);
    void (*SourceUnqueueBuffers)(unsigned, int, unsigned *);
    void (*SourcePlay)(unsigned);
    void (*SourcePause)(unsigned);
    void (*SourceStop)(unsigned);
    void (*GetSourcei)(unsigned, int, int *);
    void (*Sourcef)(unsigned, int, float);
    void (*Sourcei)(unsigned, int, int);
    int  (*GetError)(void);
    void *(*GetCurrentContext)(void);
};

static struct al_api g_al;
static int g_al_ready;
static pthread_once_t g_al_once = PTHREAD_ONCE_INIT;

static void al_load(void) {
    void *h = dlopen("libopenal.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libopenal.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[nx-music] OpenAL do sistema indisponivel: %s\n",
                dlerror());
        return;
    }
#define SYM(field, name) \
    do { *(void **)&g_al.field = dlsym(h, name); \
         if (!g_al.field) { \
             fprintf(stderr, "[nx-music] OpenAL sem %s\n", name); return; } \
    } while (0)
    SYM(GenSources, "alGenSources");
    SYM(DeleteSources, "alDeleteSources");
    SYM(GenBuffers, "alGenBuffers");
    SYM(DeleteBuffers, "alDeleteBuffers");
    SYM(BufferData, "alBufferData");
    SYM(SourceQueueBuffers, "alSourceQueueBuffers");
    SYM(SourceUnqueueBuffers, "alSourceUnqueueBuffers");
    SYM(SourcePlay, "alSourcePlay");
    SYM(SourcePause, "alSourcePause");
    SYM(SourceStop, "alSourceStop");
    SYM(GetSourcei, "alGetSourcei");
    SYM(Sourcef, "alSourcef");
    SYM(Sourcei, "alSourcei");
    SYM(GetError, "alGetError");
    SYM(GetCurrentContext, "alcGetCurrentContext");
#undef SYM
    g_al_ready = 1;
}

/* --- estrutura ---------------------------------------------------------- */

struct nx_media_player {
    pthread_mutex_t lock;

    /* desejo do jogo */
    char *path;
    int   looping;
    float gain;
    int   want_state;
    int   seek_ms;
    unsigned gen;      /* muda a cada nova fonte/seek: manda a thread religar */

    /* medido */
    int duration_ms;
    int position_ms;
    int finished;

    /* thread */
    pthread_t thread;
    int thread_alive;
    int quit;
};

static int mp_autoloop(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SB_MUSIC_AUTOLOOP");
        cached = (v && *v) ? atoi(v) : 1;
    }
    return cached;
}

/* --- decodificador ------------------------------------------------------ */

/* A trilha decodifica DENTRO do processo (`aac_decoder.c`: minimp4 + FDK-AAC).
 * A versao NextOS deste port chamava `ffmpeg`/`ffprobe` do sistema; isso e'
 * especifico daquele CFW e num pacote universal significava "abre sem musica".
 * Nao ha processo externo, pipe nem `system()` neste caminho. */

struct decoder {
    nx_aac *d;
};

static void decoder_close(struct decoder *dec) {
    if (dec->d) { nx_aac_close(dec->d); dec->d = NULL; }
}

static int decoder_open(struct decoder *dec, const char *path, int start_ms) {
    dec->d = nx_aac_open(path, start_ms, MP_RATE, MP_CHANNELS);
    if (!dec->d) {
        fprintf(stderr, "[nx-music] nao consegui abrir %s\n", path);
        return -1;
    }
    return 0;
}

/* >0 bytes, 0 = fim da faixa, -1 = erro. */
static int decoder_read(struct decoder *dec, void *buf, int bytes) {
    if (!dec->d) return -1;
    return nx_aac_read(dec->d, buf, bytes);
}

/* Duracao pelo proprio container MP4 (substitui o `ffprobe`). */
static int probe_duration_ms(const char *path) {
    return nx_aac_duration_ms(path);
}

/* --- thread alimentadora ------------------------------------------------ */

struct feeder {
    unsigned source;
    unsigned buffers[MP_BUFFERS];
    int free_n;
    unsigned free_buffers[MP_BUFFERS];
    int ready;
};

static int feeder_init(struct feeder *f) {
    if (f->ready) return 1;
    pthread_once(&g_al_once, al_load);
    if (!g_al_ready) return 0;
    /* O MonoGame precisa ter criado o contexto antes; sem ele alGenSources
     * nao produz fonte nenhuma e o erro so apareceria como silencio. */
    if (!g_al.GetCurrentContext()) return 0;
    g_al.GetError();
    g_al.GenSources(1, &f->source);
    if (g_al.GetError() != 0 || f->source == 0) return 0;
    g_al.GenBuffers(MP_BUFFERS, f->buffers);
    if (g_al.GetError() != 0) {
        g_al.DeleteSources(1, &f->source);
        f->source = 0;
        return 0;
    }
    for (int i = 0; i < MP_BUFFERS; i++) f->free_buffers[i] = f->buffers[i];
    f->free_n = MP_BUFFERS;
    f->ready = 1;
    fprintf(stderr, "[nx-music] fonte OpenAL propria pronta (source=%u)\n",
            f->source);
    return 1;
}

static void feeder_recycle(struct feeder *f) {
    int processed = 0;
    g_al.GetSourcei(f->source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0 && f->free_n < MP_BUFFERS) {
        unsigned b = 0;
        g_al.SourceUnqueueBuffers(f->source, 1, &b);
        if (!b) break;
        f->free_buffers[f->free_n++] = b;
    }
}

static void feeder_flush(struct feeder *f) {
    if (!f->ready) return;
    g_al.SourceStop(f->source);
    g_al.Sourcei(f->source, AL_BUFFER, 0);
    for (int i = 0; i < MP_BUFFERS; i++) f->free_buffers[i] = f->buffers[i];
    f->free_n = MP_BUFFERS;
}

static void feeder_release(struct feeder *f) {
    if (!f->ready) return;
    feeder_flush(f);
    g_al.DeleteBuffers(MP_BUFFERS, f->buffers);
    g_al.DeleteSources(1, &f->source);
    f->ready = 0;
    f->source = 0;
}

/* --- recibo de audio ----------------------------------------------------
 *
 * Acumula pico e RMS do PCM entregue ao OpenAL e imprime um resumo a cada
 * MP_RECEIPT_MS. E' a unica prova aceitavel de que a trilha nao esta muda:
 * o log "tocando <faixa>" sai igual com um decodificador quebrado. */
#define MP_RECEIPT_MS 5000

struct mp_receipt {
    double  acc;
    long long n;
    int     peak;
    long long frames;
    unsigned last_ms;
};

static unsigned mp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void mp_receipt(struct mp_receipt *r, const short *pcm, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int v = pcm[i];
        r->acc += (double)v * v;
        if (v < 0) v = -v;
        if (v > r->peak) r->peak = v;
    }
    r->n += (long long)n;
    r->frames += (long long)(n / MP_CHANNELS);

    unsigned now = mp_now_ms();
    if (!r->last_ms) { r->last_ms = now; return; }
    if (now - r->last_ms < MP_RECEIPT_MS) return;

    double rms = r->n ? sqrt(r->acc / (double)r->n) : 0.0;
    fprintf(stderr,
            "[nx-music][recibo] pcm_frames=%lld rms=%.1f pico=%d de 32767 %s\n",
            r->frames, rms, r->peak,
            (r->peak > 512) ? "AUDIVEL" : "SILENCIO");
    r->acc = 0.0; r->n = 0; r->peak = 0; r->last_ms = now;
}

static void *mp_thread(void *arg) {
    nx_media_player *mp = (nx_media_player *)arg;
    struct feeder f;
    struct decoder dec = { NULL };
    unsigned cur_gen = 0;
    char *cur_path = NULL;
    long long frames_fed = 0;
    int base_ms = 0;
    int eof = 0;
    unsigned char chunk[MP_CHUNK];
    size_t chunk_len = 0;
    struct mp_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));

    memset(&f, 0, sizeof(f));

    for (;;) {
        pthread_mutex_lock(&mp->lock);
        int quit = mp->quit;
        int want = mp->want_state;
        unsigned gen = mp->gen;
        int looping = mp->looping;
        float gain = mp->gain;
        char *path = mp->path ? strdup(mp->path) : NULL;
        int seek_ms = mp->seek_ms;
        pthread_mutex_unlock(&mp->lock);

        if (quit) { free(path); break; }

        /* fonte nova ou seek: derruba o decodificador e a fila do AL */
        if (gen != cur_gen) {
            decoder_close(&dec);
            feeder_flush(&f);
            free(cur_path);
            cur_path = path ? strdup(path) : NULL;
            cur_gen = gen;
            base_ms = seek_ms;
            frames_fed = 0;
            chunk_len = 0;
            eof = 0;
        }
        free(path);

        if (want != MP_STATE_PLAYING) {
            if (f.ready && want == MP_STATE_PAUSED) g_al.SourcePause(f.source);
            if (f.ready && want <= MP_STATE_STOPPED) {
                feeder_flush(&f);
                decoder_close(&dec);
                frames_fed = 0;
                chunk_len = 0;
                eof = 0;
            }
            usleep(15000);
            continue;
        }

        if (!cur_path) { usleep(15000); continue; }
        if (!feeder_init(&f)) { usleep(30000); continue; }

        /* Controle negativo de bancada: SB_MUSIC_MUTE=1 zera SO a fonte da
         * trilha. O que sobrar na saida e' necessariamente SFX, e e' assim
         * que se prova SFX sem placa de loopback. Desligado por padrao e
         * nunca ligado por um pacote publicado. */
        {
            static int mute = -1;
            if (mute < 0) {
                const char *v = getenv("SB_MUSIC_MUTE");
                mute = (v && v[0] && v[0] != '0') ? 1 : 0;
                if (mute)
                    fprintf(stderr, "[nx-music] SB_MUSIC_MUTE=1: trilha muda "
                                    "(controle negativo de medicao)\n");
            }
            g_al.Sourcef(f.source, AL_GAIN, mute ? 0.0f : gain);
        }

        if (!dec.d && !eof) {
            if (decoder_open(&dec, cur_path, base_ms) != 0) {
                pthread_mutex_lock(&mp->lock);
                mp->want_state = MP_STATE_STOPPED;
                mp->finished = 1;
                pthread_mutex_unlock(&mp->lock);
                continue;
            }
            const char *leaf = strrchr(cur_path, '/');
            fprintf(stderr, "[nx-music] tocando %s%s\n",
                    leaf ? leaf + 1 : cur_path, looping ? " (loop)" : "");
        }

        feeder_recycle(&f);

        /* enche um buffer inteiro antes de enfileirar: o AL nao gosta de
         * pedacos de 200 bytes e o custo aqui e irrelevante para musica. */
        while (f.free_n > 0 && !eof) {
            if (chunk_len < sizeof(chunk) && dec.d) {
                int got = decoder_read(&dec, chunk + chunk_len,
                                       (int)(sizeof(chunk) - chunk_len));
                if (got > 0) chunk_len += (size_t)got;
                else eof = 1; /* 0 = fim da faixa, -1 = erro */
            }
            if (chunk_len == sizeof(chunk) || (eof && chunk_len > 0)) {
                size_t usable = chunk_len - (chunk_len % MP_BYTES_PER_FRAME);
                if (usable > 0) {
                    unsigned b = f.free_buffers[--f.free_n];
                    /* Recibo de audio: mede o PCM REAL que vai para o OpenAL.
                     * "Abre sem musica" e' falha, e log de "tocando" nao prova
                     * nada -- so o pico/RMS do bloco enfileirado prova. */
                    mp_receipt(&receipt, (const short *)chunk,
                               usable / sizeof(short));
                    g_al.BufferData(b, AL_FORMAT_STEREO16, chunk, (int)usable,
                                    MP_RATE);
                    g_al.SourceQueueBuffers(f.source, 1, &b);
                    frames_fed += (long long)(usable / MP_BYTES_PER_FRAME);
                }
                chunk_len = 0;
            }
            if (!eof && chunk_len < sizeof(chunk)) break;
        }

        int state = 0;
        g_al.GetSourcei(f.source, AL_SOURCE_STATE, &state);
        int queued = 0;
        g_al.GetSourcei(f.source, AL_BUFFERS_QUEUED, &queued);
        if (state != AL_PLAYING && queued > 0) g_al.SourcePlay(f.source);
        {   /* Estado da fonte OpenAL junto do recibo: PCM audivel com a fonte
             * parada continua sendo silencio no alto-falante. */
            static unsigned last_state_ms;
            unsigned now = mp_now_ms();
            if (now - last_state_ms > MP_RECEIPT_MS) {
                last_state_ms = now;
                int processed = 0;
                g_al.GetSourcei(f.source, AL_BUFFERS_PROCESSED, &processed);
                fprintf(stderr,
                        "[nx-music][recibo] fonte_al=%s enfileirados=%d "
                        "reciclados=%d\n",
                        state == AL_PLAYING ? "PLAYING" : "PARADA",
                        queued, processed);
            }
        }

        pthread_mutex_lock(&mp->lock);
        mp->position_ms = base_ms +
            (int)(frames_fed * 1000 / MP_RATE) -
            (int)(queued * (MP_CHUNK / MP_BYTES_PER_FRAME) * 1000 / MP_RATE);
        if (mp->position_ms < 0) mp->position_ms = 0;
        pthread_mutex_unlock(&mp->lock);

        /* fim da faixa: so quando o AL drenou tudo que foi enfileirado */
        if (eof && queued == 0 && state != AL_PLAYING) {
            decoder_close(&dec);
            if (looping || mp_autoloop()) {
                if (!looping)
                    fprintf(stderr, "[nx-music] fim da faixa; repetindo "
                                    "(SB_MUSIC_AUTOLOOP)\n");
                base_ms = 0;
                frames_fed = 0;
                chunk_len = 0;
                eof = 0;
                feeder_flush(&f);
            } else {
                pthread_mutex_lock(&mp->lock);
                mp->want_state = MP_STATE_STOPPED;
                mp->finished = 1;
                mp->position_ms = mp->duration_ms;
                pthread_mutex_unlock(&mp->lock);
                fprintf(stderr, "[nx-music] fim da faixa\n");
            }
        }

        usleep(5000);
    }

    decoder_close(&dec);
    feeder_release(&f);
    free(cur_path);
    return NULL;
}

/* --- API ---------------------------------------------------------------- */

nx_media_player *nx_mp_create(void) {
    nx_media_player *mp = calloc(1, sizeof(*mp));
    if (!mp) return NULL;
    pthread_mutex_init(&mp->lock, NULL);
    mp->gain = 1.0f;
    mp->want_state = MP_STATE_IDLE;
    if (pthread_create(&mp->thread, NULL, mp_thread, mp) == 0)
        mp->thread_alive = 1;
    else
        fprintf(stderr, "[nx-music] nao consegui criar a thread de musica\n");
    return mp;
}

void nx_mp_destroy(nx_media_player *mp) {
    if (!mp) return;
    pthread_mutex_lock(&mp->lock);
    mp->quit = 1;
    pthread_mutex_unlock(&mp->lock);
    if (mp->thread_alive) pthread_join(mp->thread, NULL);
    free(mp->path);
    pthread_mutex_destroy(&mp->lock);
    free(mp);
}

void nx_mp_reset(nx_media_player *mp) {
    if (!mp) return;
    pthread_mutex_lock(&mp->lock);
    free(mp->path);
    mp->path = NULL;
    mp->want_state = MP_STATE_IDLE;
    mp->duration_ms = 0;
    mp->position_ms = 0;
    mp->seek_ms = 0;
    mp->finished = 0;
    mp->gen++;
    pthread_mutex_unlock(&mp->lock);
}

int nx_mp_set_source(nx_media_player *mp, const char *path) {
    if (!mp || !path || !*path) return 0;
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "[nx-music] fonte ilegivel: %s\n", path);
        return 0;
    }
    pthread_mutex_lock(&mp->lock);
    free(mp->path);
    mp->path = strdup(path);
    mp->want_state = MP_STATE_STOPPED;
    mp->duration_ms = 0;
    mp->position_ms = 0;
    mp->seek_ms = 0;
    mp->finished = 0;
    mp->gen++;
    pthread_mutex_unlock(&mp->lock);
    return 1;
}

int nx_mp_prepare(nx_media_player *mp) {
    if (!mp) return 0;
    pthread_mutex_lock(&mp->lock);
    char *path = mp->path ? strdup(mp->path) : NULL;
    pthread_mutex_unlock(&mp->lock);
    if (!path) return 0;
    int duration = probe_duration_ms(path);
    free(path);
    pthread_mutex_lock(&mp->lock);
    mp->duration_ms = duration;
    pthread_mutex_unlock(&mp->lock);
    return 1;
}

void nx_mp_set_looping(nx_media_player *mp, int looping) {
    if (!mp) return;
    pthread_mutex_lock(&mp->lock);
    mp->looping = looping ? 1 : 0;
    pthread_mutex_unlock(&mp->lock);
}

int nx_mp_get_looping(nx_media_player *mp) {
    if (!mp) return 0;
    pthread_mutex_lock(&mp->lock);
    int looping = mp->looping;
    pthread_mutex_unlock(&mp->lock);
    return looping;
}

void nx_mp_set_volume(nx_media_player *mp, float left, float right) {
    if (!mp) return;
    float gain = (left + right) * 0.5f;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    pthread_mutex_lock(&mp->lock);
    mp->gain = gain;
    pthread_mutex_unlock(&mp->lock);
}

void nx_mp_start(nx_media_player *mp) {
    if (!mp) return;
    pthread_mutex_lock(&mp->lock);
    if (mp->path) {
        mp->finished = 0;
        mp->want_state = MP_STATE_PLAYING;
    }
    pthread_mutex_unlock(&mp->lock);
}

void nx_mp_pause(nx_media_player *mp) {
    if (!mp) return;
    pthread_mutex_lock(&mp->lock);
    if (mp->want_state == MP_STATE_PLAYING) mp->want_state = MP_STATE_PAUSED;
    pthread_mutex_unlock(&mp->lock);
}

void nx_mp_stop(nx_media_player *mp) {
    if (!mp) return;
    pthread_mutex_lock(&mp->lock);
    mp->want_state = MP_STATE_STOPPED;
    mp->position_ms = 0;
    mp->seek_ms = 0;
    mp->gen++;
    pthread_mutex_unlock(&mp->lock);
}

void nx_mp_seek_ms(nx_media_player *mp, int position_ms) {
    if (!mp) return;
    if (position_ms < 0) position_ms = 0;
    pthread_mutex_lock(&mp->lock);
    mp->seek_ms = position_ms;
    mp->position_ms = position_ms;
    mp->gen++;
    pthread_mutex_unlock(&mp->lock);
}

int nx_mp_duration_ms(nx_media_player *mp) {
    if (!mp) return 0;
    pthread_mutex_lock(&mp->lock);
    int duration = mp->duration_ms;
    pthread_mutex_unlock(&mp->lock);
    return duration;
}

int nx_mp_position_ms(nx_media_player *mp) {
    if (!mp) return 0;
    pthread_mutex_lock(&mp->lock);
    int position = mp->position_ms;
    pthread_mutex_unlock(&mp->lock);
    return position;
}

int nx_mp_is_playing(nx_media_player *mp) {
    if (!mp) return 0;
    pthread_mutex_lock(&mp->lock);
    int playing = mp->want_state == MP_STATE_PLAYING;
    pthread_mutex_unlock(&mp->lock);
    return playing;
}
