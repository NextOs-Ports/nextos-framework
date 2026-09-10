/* aac_decoder.c -- ver aac_decoder.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include <aacdecoder_lib.h>

#include "aac_decoder.h"

/* Um quadro AAC-LC sao 1024 amostras; HE-AAC dobra. Folga de 8 quadros
 * estereo cobre qualquer caso que apareca num .m4a de trilha. */
#define NX_AAC_PCM_MAX (8 * 2048 * 2)

struct nx_aac {
    unsigned char *file;      /* arquivo inteiro em memoria (max ~2 MB aqui) */
    size_t         file_size;
    MP4D_demux_t   mp4;
    int            mp4_open;
    unsigned       track;
    unsigned       nsample;   /* proximo sample do MP4 a entregar ao FDK */

    HANDLE_AACDECODER aac;

    int out_rate, out_ch;     /* formato pedido pelo chamador */
    int src_rate, src_ch;     /* formato real do arquivo */

    /* PCM decodificado do ultimo quadro, ja convertido para out_ch canais e
     * ainda na taxa src_rate. */
    short pcm[NX_AAC_PCM_MAX];
    int   pcm_frames;         /* quadros validos em `pcm` */
    int   pcm_pos;            /* quadros ja consumidos */

    /* reamostragem linear: posicao fracionaria em 16.16 dentro de `pcm` */
    unsigned resamp_frac;
    short    last_frame[8];   /* ultimo quadro emitido, para interpolar */
    int      have_last;

    /* Alinhamento exato da saida.
     *
     * O fluxo que sai do FDK vem adiantado de duas quantidades que o ffmpeg
     * descontava por nos e que um decodificador interno precisa descontar
     * sozinho, sob pena de tocar ~58 ms de silencio no comeco de cada volta
     * do loop:
     *   - `outputDelay` do proprio decodificador (CStreamInfo, 1744 quadros
     *     nestes arquivos);
     *   - `media_time` da edit list (`elst`) do MP4 (1024 quadros aqui).
     * E o fim da faixa e' o que a edit list declara, nao o ultimo quadro AAC
     * codificado (que traz o padding do codificador). */
    long long elst_media_time;   /* quadros a descartar, do container */
    long long limit_frames;      /* total a emitir; 0 = sem limite */
    long long skip_pending;      /* quadros ainda por descartar */
    long long emitted;           /* quadros ja entregues ao chamador */
    int       skip_ready;        /* 1 depois que outputDelay foi conhecido */
    long long start_frames;      /* seek pedido, em quadros */
    long long out_delay;         /* CStreamInfo.outputDelay do FDK */

    int flushes;
    int eof;
};

/* --- edit list (elst) ---------------------------------------------------
 *
 * O minimp4 nao expoe a `elst`, entao a lemos aqui com um passeio curto pelas
 * caixas moov > trak > edts > elst. Ausencia de `elst` nao e' erro: o arquivo
 * simplesmente nao pede corte. */
struct nx_elst { long long media_time; long long segment_duration; int found; };

static void nx_scan_elst(const unsigned char *p, const unsigned char *end,
                         struct nx_elst *out, int depth)
{
    while (end - p >= 8 && depth < 8) {
        unsigned long long size = ((unsigned long long)p[0] << 24) |
                                  ((unsigned long long)p[1] << 16) |
                                  ((unsigned long long)p[2] << 8) | p[3];
        const unsigned char *type = p + 4;
        const unsigned char *body = p + 8;
        if (size == 1) {
            if (end - p < 16) return;
            size = 0;
            for (int i = 0; i < 8; i++) size = (size << 8) | p[8 + i];
            body = p + 16;
        } else if (size == 0) {
            size = (unsigned long long)(end - p);
        }
        if (size < (unsigned long long)(body - p) ||
            size > (unsigned long long)(end - p)) return;
        const unsigned char *next = p + size;

        if (!memcmp(type, "elst", 4)) {
            if (next - body >= 8) {
                int ver = body[0];
                unsigned cnt = ((unsigned)body[4] << 24) | ((unsigned)body[5] << 16) |
                               ((unsigned)body[6] << 8) | body[7];
                const unsigned char *e = body + 8;
                if (cnt >= 1) {
                    if (ver == 1 && next - e >= 20) {
                        unsigned long long dur = 0, mt = 0;
                        for (int i = 0; i < 8; i++) dur = (dur << 8) | e[i];
                        for (int i = 0; i < 8; i++) mt = (mt << 8) | e[8 + i];
                        out->segment_duration = (long long)dur;
                        out->media_time = (long long)(int64_t)mt;
                        out->found = 1;
                    } else if (ver == 0 && next - e >= 12) {
                        unsigned dur = ((unsigned)e[0] << 24) | ((unsigned)e[1] << 16) |
                                       ((unsigned)e[2] << 8) | e[3];
                        int mt = (int)(((unsigned)e[4] << 24) | ((unsigned)e[5] << 16) |
                                       ((unsigned)e[6] << 8) | e[7]);
                        out->segment_duration = (long long)dur;
                        out->media_time = (long long)mt;
                        out->found = 1;
                    }
                }
            }
            return;
        }
        if (!memcmp(type, "moov", 4) || !memcmp(type, "trak", 4) ||
            !memcmp(type, "edts", 4)) {
            nx_scan_elst(body, next, out, depth + 1);
            if (out->found) return;
        }
        p = next;
    }
}

/* --- leitura do MP4 a partir da memoria ---------------------------------- */

static int nx_read_cb(int64_t offset, void *buffer, size_t size, void *token)
{
    struct nx_aac *d = (struct nx_aac *)token;
    if (offset < 0 || (uint64_t)offset > d->file_size) return 1;
    if (size > d->file_size - (size_t)offset) return 1;
    memcpy(buffer, d->file + offset, size);
    return 0;
}

static unsigned char *nx_slurp(const char *path, size_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return NULL;
    /* Trilha de jogo: o maior arquivo do Blossom Tales tem 1,6 MB. O teto
     * evita que um caminho errado tente carregar um asset gigante. */
    if (st.st_size > 64 * 1024 * 1024) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) { free(buf); return NULL; }
    *out_size = got;
    return buf;
}

/* Acha a primeira trilha de audio ('soun'). -1 quando nao houver. */
static int nx_find_audio_track(MP4D_demux_t *mp4)
{
    for (unsigned i = 0; i < mp4->track_count; i++)
        if (mp4->track[i].handler_type == MP4D_HANDLER_TYPE_SOUN)
            return (int)i;
    return -1;
}

/* --- duracao (substitui o ffprobe) --------------------------------------- */

int nx_aac_duration_ms(const char *path)
{
    struct nx_aac tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.file = nx_slurp(path, &tmp.file_size);
    if (!tmp.file) return 0;

    int ms = 0;
    if (MP4D_open(&tmp.mp4, nx_read_cb, &tmp, (int64_t)tmp.file_size)) {
        /* A duracao do FILME ja reflete a edit list; a da TRILHA inclui o
         * padding do codificador. O jogo so exibe este numero, mas ele tem
         * de casar com o que realmente toca. */
        if (tmp.mp4.timescale)
            ms = (int)((unsigned long long)tmp.mp4.duration_lo * 1000ULL
                       / tmp.mp4.timescale);
        if (!ms) {
            int t = nx_find_audio_track(&tmp.mp4);
            if (t >= 0) {
                MP4D_track_t *tr = &tmp.mp4.track[t];
                if (tr->timescale)
                    ms = (int)((unsigned long long)tr->duration_lo * 1000ULL
                               / tr->timescale);
            }
        }
        MP4D_close(&tmp.mp4);
    }
    free(tmp.file);
    return ms > 0 ? ms : 0;
}

/* --- abertura ------------------------------------------------------------ */

nx_aac *nx_aac_open(const char *path, int start_ms, int rate_hz, int channels)
{
    if (!path || rate_hz <= 0 || channels < 1 || channels > 2) return NULL;

    struct nx_aac *d = (struct nx_aac *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->out_rate = rate_hz;
    d->out_ch   = channels;

    d->file = nx_slurp(path, &d->file_size);
    if (!d->file) { free(d); return NULL; }

    if (!MP4D_open(&d->mp4, nx_read_cb, d, (int64_t)d->file_size)) {
        fprintf(stderr, "[nx-music] %s: nao e um MP4 valido\n", path);
        nx_aac_close(d);
        return NULL;
    }
    d->mp4_open = 1;

    int t = nx_find_audio_track(&d->mp4);
    if (t < 0) {
        fprintf(stderr, "[nx-music] %s: sem trilha de audio\n", path);
        nx_aac_close(d);
        return NULL;
    }
    d->track = (unsigned)t;
    MP4D_track_t *tr = &d->mp4.track[d->track];

    if (!tr->dsi || !tr->dsi_bytes) {
        fprintf(stderr, "[nx-music] %s: sem AudioSpecificConfig\n", path);
        nx_aac_close(d);
        return NULL;
    }

    d->aac = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!d->aac) {
        fprintf(stderr, "[nx-music] aacDecoder_Open falhou\n");
        nx_aac_close(d);
        return NULL;
    }

    UCHAR *conf[1]  = { (UCHAR *)tr->dsi };
    UINT   conf_l[1] = { (UINT)tr->dsi_bytes };
    if (aacDecoder_ConfigRaw(d->aac, conf, conf_l) != AAC_DEC_OK) {
        fprintf(stderr, "[nx-music] %s: AudioSpecificConfig recusado\n", path);
        nx_aac_close(d);
        return NULL;
    }

    /* Formato de origem: o que o container declara; corrigido pelo
     * CStreamInfo assim que o primeiro quadro sair. */
    d->src_rate = (int)tr->SampleDescription.audio.samplerate_hz;
    d->src_ch   = (int)tr->SampleDescription.audio.channelcount;
    if (d->src_rate <= 0) d->src_rate = d->out_rate;
    if (d->src_ch <= 0)   d->src_ch = d->out_ch;

    /* Edit list: quanto cortar no comeco e onde a faixa realmente termina. */
    struct nx_elst el; memset(&el, 0, sizeof(el));
    nx_scan_elst(d->file, d->file + d->file_size, &el, 0);
    if (el.found && el.media_time > 0) d->elst_media_time = el.media_time;
    if (el.found && el.segment_duration > 0 && d->mp4.timescale)
        d->limit_frames = (long long)((double)el.segment_duration
                          * (double)d->src_rate / (double)d->mp4.timescale);
    if (d->limit_frames <= 0)
        d->limit_frames = (long long)tr->duration_lo - d->elst_media_time;

    /* Seek: o descarte e' feito no fluxo JA decodificado, comecando do sample
     * 0. E' o unico jeito exato -- `outputDelay` so aparece depois do primeiro
     * quadro, e o AAC nao tem quadro-chave para saltar sem erro. Uma faixa
     * inteira decodifica em fracao de segundo e o seek e' raro. */
    d->nsample = 0;
    d->start_frames = (long long)start_ms * d->src_rate / 1000;
    if (d->start_frames < 0) d->start_frames = 0;
    return d;
}

void nx_aac_close(nx_aac *d)
{
    if (!d) return;
    if (d->aac) aacDecoder_Close(d->aac);
    if (d->mp4_open) MP4D_close(&d->mp4);
    free(d->file);
    free(d);
}

/* --- decodificacao ------------------------------------------------------- */

/* Mapeia `frames` quadros de `src` (src_ch canais) para d->pcm (out_ch). */
static void nx_map_channels(struct nx_aac *d, const INT_PCM *src, int frames)
{
    if (d->src_ch == d->out_ch) {
        memcpy(d->pcm, src, (size_t)frames * d->out_ch * sizeof(short));
    } else if (d->src_ch == 1 && d->out_ch == 2) {
        for (int i = 0; i < frames; i++)
            d->pcm[i * 2] = d->pcm[i * 2 + 1] = src[i];
    } else if (d->out_ch == 1) {
        for (int i = 0; i < frames; i++) {
            int acc = 0;
            for (int c = 0; c < d->src_ch; c++) acc += src[i * d->src_ch + c];
            d->pcm[i] = (short)(acc / d->src_ch);
        }
    } else {
        /* >2 canais para estereo: pega os dois primeiros. */
        for (int i = 0; i < frames; i++) {
            d->pcm[i * 2]     = src[i * d->src_ch];
            d->pcm[i * 2 + 1] = src[i * d->src_ch + 1];
        }
    }
}

/* Decodifica o proximo quadro do MP4 para d->pcm, ja com out_ch canais.
 * 1 = quadro pronto, 0 = fim, -1 = erro irrecuperavel. */
static int nx_decode_frame(struct nx_aac *d)
{
    MP4D_track_t *tr = &d->mp4.track[d->track];

    for (;;) {
        if (d->nsample >= tr->sample_count) {
            /* Fim dos quadros AAC: ainda restam `outputDelay` amostras dentro
             * do decodificador. Sem drenar, a faixa termina ~36 ms cedo e o
             * loop ganha uma costura audivel. */
            if (d->flushes < 2) {
                d->flushes++;
                INT_PCM fout[NX_AAC_PCM_MAX];
                AAC_DECODER_ERROR fe = aacDecoder_DecodeFrame(
                    d->aac, fout, NX_AAC_PCM_MAX, AACDEC_FLUSH);
                CStreamInfo *fsi = aacDecoder_GetStreamInfo(d->aac);
                if (fe == AAC_DEC_OK && fsi && fsi->frameSize > 0) {
                    int frames = fsi->frameSize;
                    if (frames > NX_AAC_PCM_MAX / d->out_ch)
                        frames = NX_AAC_PCM_MAX / d->out_ch;
                    nx_map_channels(d, fout, frames);
                    d->pcm_frames = frames;
                    d->pcm_pos = 0;
                    return 1;
                }
                continue;
            }
            return 0;
        }

        unsigned bytes = 0, ts = 0, dur = 0;
        MP4D_file_offset_t off =
            MP4D_frame_offset(&d->mp4, d->track, d->nsample, &bytes, &ts, &dur);
        d->nsample++;
        if (off == 0 || bytes == 0) continue;
        if ((size_t)off + bytes > d->file_size) return 0;

        UCHAR *pkt[1]   = { d->file + off };
        UINT   pkt_l[1] = { (UINT)bytes };
        UINT   valid    = (UINT)bytes;

        if (aacDecoder_Fill(d->aac, pkt, pkt_l, &valid) != AAC_DEC_OK)
            continue;

        INT_PCM out[NX_AAC_PCM_MAX];
        AAC_DECODER_ERROR err =
            aacDecoder_DecodeFrame(d->aac, out, NX_AAC_PCM_MAX, 0);
        if (err == AAC_DEC_NOT_ENOUGH_BITS) continue;
        if (err != AAC_DEC_OK) {
            /* Quadro corrompido isolado nao derruba a faixa. */
            if (IS_OUTPUT_VALID(err) == 0) continue;
        }

        CStreamInfo *si = aacDecoder_GetStreamInfo(d->aac);
        if (!si || si->frameSize <= 0 || si->numChannels <= 0) continue;

        d->src_rate = si->sampleRate > 0 ? si->sampleRate : d->src_rate;
        d->src_ch   = si->numChannels;
        d->out_delay = (long long)si->outputDelay;

        int frames = si->frameSize;                 /* por canal */
        if (frames > NX_AAC_PCM_MAX / d->out_ch)
            frames = NX_AAC_PCM_MAX / d->out_ch;

        nx_map_channels(d, out, frames);

        d->pcm_frames = frames;
        d->pcm_pos    = 0;
        return 1;
    }
}

int nx_aac_read(nx_aac *d, void *buf, int bytes)
{
    if (!d || !buf || bytes <= 0) return -1;

    const int fb = d->out_ch * (int)sizeof(short);  /* bytes por quadro */
    int want = bytes / fb;
    if (want <= 0) return 0;

    short *dst = (short *)buf;
    int done = 0;

    while (done < want) {
        if (d->limit_frames > 0 && d->emitted >= d->limit_frames) {
            d->eof = 1;
            break;
        }

        if (d->pcm_pos >= d->pcm_frames) {
            if (d->eof) break;
            int r = nx_decode_frame(d);
            if (r < 0) return done > 0 ? done * fb : -1;
            if (r == 0) { d->eof = 1; break; }
        }

        /* Descarte de alinhamento: outputDelay + media_time + seek. So da
         * para calcular depois do primeiro quadro, quando o CStreamInfo
         * publica o outputDelay. */
        if (!d->skip_ready) {
            d->skip_pending = d->out_delay + d->elst_media_time + d->start_frames;
            d->skip_ready = 1;
        }
        if (d->skip_pending > 0) {
            long long avail = d->pcm_frames - d->pcm_pos;
            long long drop = avail < d->skip_pending ? avail : d->skip_pending;
            d->pcm_pos += (int)drop;
            d->skip_pending -= drop;
            continue;
        }

        if (d->src_rate == d->out_rate) {
            int avail = d->pcm_frames - d->pcm_pos;
            int n = want - done;
            if (n > avail) n = avail;
            if (d->limit_frames > 0 && d->emitted + n > d->limit_frames)
                n = (int)(d->limit_frames - d->emitted);
            if (n <= 0) { d->eof = 1; break; }
            memcpy(dst + (size_t)done * d->out_ch,
                   d->pcm + (size_t)d->pcm_pos * d->out_ch,
                   (size_t)n * fb);
            d->pcm_pos += n;
            done += n;
            d->emitted += n;
            continue;
        }

        /* Reamostragem linear. `step` em 16.16 evita divisao por quadro e
         * mantem a fase entre quadros no proprio `resamp_frac`. */
        unsigned step = (unsigned)(((unsigned long long)d->src_rate << 16)
                                   / (unsigned)d->out_rate);
        while (done < want && d->pcm_pos < d->pcm_frames) {
            const short *a = d->pcm + (size_t)d->pcm_pos * d->out_ch;
            const short *prev = d->have_last ? d->last_frame : a;
            unsigned f = d->resamp_frac & 0xFFFFu;
            for (int c = 0; c < d->out_ch; c++) {
                int v = ((int)prev[c] * (int)(65536u - f)
                         + (int)a[c] * (int)f) >> 16;
                dst[(size_t)done * d->out_ch + c] = (short)v;
            }
            done++;
            d->emitted++;
            d->resamp_frac += step;
            unsigned adv = d->resamp_frac >> 16;
            if (adv) {
                for (unsigned k = 0; k < adv && d->pcm_pos < d->pcm_frames; k++) {
                    const short *cur = d->pcm + (size_t)d->pcm_pos * d->out_ch;
                    for (int c = 0; c < d->out_ch; c++) d->last_frame[c] = cur[c];
                    d->have_last = 1;
                    d->pcm_pos++;
                }
                d->resamp_frac &= 0xFFFFu;
            }
        }
    }

    return done * fb;
}
