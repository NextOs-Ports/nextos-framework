/*
 * audio.c -- saida de audio do FMOD Studio nativo em NextOS.
 *
 * Blasphemous nao usa o FMOD embutido da Unity: ele carrega o libfmod.so /
 * libfmodstudio.so proprios e escolhe o output AudioTrack.  Do lado nativo
 * esse output e' inteiramente JNI: o FMOD instancia org/fmod/AudioDevice,
 * chama init(canais, taxa, frames, buffers), e a thread do mixer entrega cada
 * bloco por write(byte[], bytes).  Aqui a mesma sequencia termina numa fila do
 * SDL, sem forcar driver (regra #6): quem escolhe o backend e' o sistema.
 *
 * O write do AudioTrack e' BLOQUEANTE no Android -- e' ele que dita o ritmo da
 * thread de mixagem.  Uma fila que so' cresce faria o FMOD mixar tao rapido
 * quanto a CPU permite e o audio chegaria com segundos de atraso, entao o
 * write daqui tambem segura o chamador enquanto a fila estiver cheia.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include "nx_elf.h"
#include "sf.h"

static SDL_AudioDeviceID output;
static int out_channels;
static int out_rate;
static Uint32 queue_target;
static int audio_subsystem;
static unsigned long blocks_written;
static int trace;

static int silent_driver(const char *name)
{
    return !name || strcmp(name, "dummy") == 0 || strcmp(name, "disk") == 0;
}

static int pcm_peak_s16(const int16_t *samples, int count)
{
    int peak = 0;
    for (int i = 0; i < count; i++) {
        int value = samples[i];
        if (value < 0)
            value = value == -32768 ? 32768 : -value;
        if (value > peak)
            peak = value;
    }
    return peak;
}

static int ensure_subsystem(void)
{
    if (audio_subsystem)
        return 1;
    const char *requested = getenv("SF_AUDIO_DRIVER");
    if (requested && *requested)
        setenv("SDL_AUDIODRIVER", requested, 1);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    audio_subsystem = 1;
    trace = getenv("SF_AUDIO_TRACE") != NULL;

    fprintf(stderr, "[audio] SDL drivers:");
    for (int i = 0, n = SDL_GetNumAudioDrivers(); i < n; i++) {
        const char *name = SDL_GetAudioDriver(i);
        if (name)
            fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n[audio] backend selected: %s\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                        : "(none)");
    return 1;
}

/* org/fmod/AudioDevice.init(int channels, int rate, int frames, int buffers) */
int sf_audio_device_init(int channels, int rate, int frames, int buffers)
{
    if (getenv("SF_NO_AUDIO"))
        return 0;
    if (!ensure_subsystem())
        return 0;
    if (silent_driver(SDL_GetCurrentAudioDriver()))
        return 0;
    if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000) {
        fprintf(stderr, "[audio] formato recusado: %d canais, %d Hz\n",
                channels, rate);
        return 0;
    }
    if (frames < 128)
        frames = 128;
    if (frames > 4096)
        frames = 4096;
    if (buffers < 2)
        buffers = 2;
    if (buffers > 16)
        buffers = 16;

    sf_audio_device_close();

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    memset(&have, 0, sizeof have);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = (Uint16)frames;

    output = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!output) {
        fprintf(stderr, "[audio] SDL_OpenAudioDevice falhou: %s\n",
                SDL_GetError());
        return 0;
    }
    out_channels = channels;
    out_rate = rate;
    blocks_written = 0;
    queue_target = (Uint32)frames * (Uint32)channels *
                   (Uint32)sizeof(int16_t) * (Uint32)buffers;
    SDL_PauseAudioDevice(output, 0);
    fprintf(stderr,
            "[audio] AudioDevice.init: %d Hz, %d canal(is), frames=%d, "
            "buffers=%d (SDL %d Hz/%d, driver=%s)\n",
            rate, channels, frames, buffers, have.freq, have.channels,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return 1;
}

/* org/fmod/AudioDevice.write(byte[] data, int length) */
void sf_audio_device_write(const void *data, int bytes)
{
    if (!output || !data || bytes <= 0)
        return;
    /* Contrapressao com TETO: se a fila nao drenar (device sumindo, backend
     * travado) a thread de mixagem do FMOD nao pode ficar presa para sempre. */
    for (int spin = 0; spin < 500 &&
                       SDL_GetQueuedAudioSize(output) >= queue_target; spin++)
        usleep(1000);
    if (SDL_QueueAudio(output, data, (Uint32)bytes) != 0) {
        fprintf(stderr, "[audio] SDL_QueueAudio falhou: %s\n", SDL_GetError());
        return;
    }
    blocks_written++;
    if (trace && (blocks_written <= 8 || blocks_written % 512 == 0))
        fprintf(stderr, "[audio] bloco %lu (%d bytes) peak=%d queued=%u\n",
                blocks_written, bytes,
                pcm_peak_s16(data, bytes / (int)sizeof(int16_t)),
                (unsigned)SDL_GetQueuedAudioSize(output));
}

/* org/fmod/AudioDevice.close() */
void sf_audio_device_close(void)
{
    if (!output)
        return;
    SDL_PauseAudioDevice(output, 1);
    SDL_ClearQueuedAudio(output);
    SDL_CloseAudioDevice(output);
    output = 0;
    fprintf(stderr, "[audio] AudioDevice.close (%lu blocos entregues)\n",
            blocks_written);
}

int sf_audio_started(void)
{
    return output != 0;
}

/* ------------------------------------------------------------------ FMOD */
/*
 * O FMOD EMBUTIDO na Unity (fmod_output_audiotrack.cpp) nao abre saida nenhuma
 * sozinho: quem faz isso, no Android, e' a thread JAVA de
 * org/fmod/FMODAudioDevice.run().  Ela e' que descobre o formato por
 * fmodGetInfo(), cria o AudioTrack, e depois fica pedindo bloco por bloco com
 * fmodProcess(ByteBuffer) e escrevendo no AudioTrack.  O nativo so' expoe
 * fmodGetInfo/fmodProcess e espera ser chamado.
 *
 * Sem essa thread o jogo roda MUDO e nao ha um unico erro no log -- foi
 * exatamente o que aconteceu aqui: `FMODAudioDevice.start` chegava, marcava
 * "rodando", e ninguem mais pedia um bloco. Nenhum descritor de /dev/snd era
 * aberto pelo processo.
 *
 * A sequencia abaixo e' a do proprio jogo, lida do `run()` de
 * org/fmod/FMODAudioDevice no classes.dex desta build -- nao e' invencao nem
 * heranca de outro port.  Os indices do fmodGetInfo sao os que o bytecode usa:
 *
 *     0 = taxa de amostragem      1 = quadros por bloco
 *     2 = numero de blocos        3 = "ha' mixagem para entregar"
 *     4 = canais
 *
 * O AudioTrack vira a fila do SDL (sf_audio_device_*), que ja faz a
 * contrapressao com teto que o write() bloqueante do Android fazia.
 */
enum {
    FMOD_INFO_RATE = 0,
    FMOD_INFO_BLOCK_FRAMES = 1,
    FMOD_INFO_BLOCK_COUNT = 2,
    FMOD_INFO_RUNNING = 3,
    FMOD_INFO_CHANNELS = 4,
};

typedef int (*fmod_get_info_fn)(void *env, void *thiz, int32_t info);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *byte_buffer);

static pthread_t fmod_thread;
static int fmod_thread_live;
static int fmod_shutdown;

static void *fmod_pump(void *unused)
{
    (void)unused;
    /* A thread vive o PROCESSO INTEIRO, nao a chamada de start().
     *
     * Medido nesta build: `FMODAudioDevice.start()` chega no frame zero, quando
     * so' existem 45 nativos registrados e NENHUM deles e' do FMOD -- e logo em
     * seguida vem um `stop()`.  Uma thread que nascesse em start() e morresse
     * quando `should_run` cai encontraria a mesa vazia, desistiria, e o jogo
     * ficaria mudo para sempre sem uma unica linha de erro.  Quem manda aqui e'
     * o par (natives registrados + should_run), consultado sempre, nunca uma
     * unica vez na entrada. */
    fmod_get_info_fn get_info = NULL;
    fmod_process_fn process = NULL;
    void *env = NULL, *thiz = NULL, *buffer = NULL;
    unsigned char *pcm = NULL;
    int opened = 0, attempts = 3, block_bytes = 0, announced = 0;

    while (!__atomic_load_n(&fmod_shutdown, __ATOMIC_ACQUIRE)) {
        if (!sf_jni_fmod_should_run()) {
            if (opened) {
                sf_audio_device_close();
                opened = 0;
            }
            usleep(50000);
            continue;
        }
        if (!get_info || !process) {
            get_info = (fmod_get_info_fn)sf_jni_native(
                "org/fmod/FMODAudioDevice", "fmodGetInfo");
            process = (fmod_process_fn)sf_jni_native(
                "org/fmod/FMODAudioDevice", "fmodProcess");
            if (!get_info || !process) {
                usleep(50000);
                continue;
            }
            env = sf_jni_env();
            thiz = sf_jni_fmod_device();
            buffer = sf_jni_fmod_bytebuffer();
            pcm = sf_jni_fmod_pcm();
            fprintf(stderr, "[audio] fmodGetInfo/fmodProcess resolvidos\n");
        }

        if (!opened) {
            if (attempts <= 0) {
                usleep(200000);
                attempts = 3;
                continue;
            }
            int rate = get_info(env, thiz, FMOD_INFO_RATE);
            int channels = get_info(env, thiz, FMOD_INFO_CHANNELS);
            int frames = get_info(env, thiz, FMOD_INFO_BLOCK_FRAMES);
            int blocks = get_info(env, thiz, FMOD_INFO_BLOCK_COUNT);
            if (rate <= 0 || channels <= 0 || frames <= 0) {
                usleep(50000);
                continue;
            }
            block_bytes = frames * channels * (int)sizeof(int16_t);
            if (block_bytes > sf_jni_fmod_pcm_capacity()) {
                fprintf(stderr,
                        "[audio] bloco do FMOD de %d B nao cabe no buffer "
                        "direto de %d B\n",
                        block_bytes, sf_jni_fmod_pcm_capacity());
                usleep(500000);
                continue;
            }
            sf_jni_fmod_set_buffer_size(block_bytes);
            if (!sf_audio_device_init(channels, rate, frames, blocks)) {
                attempts--;
                usleep(200000);
                continue;
            }
            opened = 1;
            attempts = 3;
            if (!announced) {
                announced = 1;
                fprintf(stderr,
                        "[audio] FMOD da Unity entregando %d Hz x %d canal(is), "
                        "bloco de %d quadros\n", rate, channels, frames);
            }
        }

        /* fmodGetInfo(3) e' o mesmo portao do bytecode: quando o mixer nao tem
         * nada a entregar, o Java derruba o AudioTrack em vez de escrever
         * silencio.  Aqui a saida SDL apenas fecha e reabre no proximo bloco. */
        if (get_info(env, thiz, FMOD_INFO_RUNNING) != 1) {
            sf_audio_device_close();
            opened = 0;
            usleep(20000);
            continue;
        }
        process(env, thiz, buffer);
        sf_audio_device_write(pcm, block_bytes);
    }
    sf_audio_device_close();
    return NULL;
}

int sf_audio_start(void *env)
{
    (void)env;
    if (fmod_thread_live)
        return 1;
    if (getenv("SF_NO_AUDIO"))
        return 0;
    int rc = pthread_create(&fmod_thread, NULL, fmod_pump, NULL);
    if (rc != 0) {
        fprintf(stderr, "[audio] pthread_create(FMOD) falhou: %s\n",
                strerror(rc));
        return 0;
    }
    fmod_thread_live = 1;
    fprintf(stderr, "[audio] thread do FMODAudioDevice de pe'\n");
    return 1;
}

void sf_audio_stop(void)
{
    if (fmod_thread_live) {
        __atomic_store_n(&fmod_shutdown, 1, __ATOMIC_RELEASE);
        pthread_join(fmod_thread, NULL);
        fmod_thread_live = 0;
    }
    sf_audio_device_close();
    if (audio_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem = 0;
    }
}
