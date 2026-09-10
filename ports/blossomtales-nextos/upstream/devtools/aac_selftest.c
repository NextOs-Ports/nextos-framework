/* aac_selftest.c -- teste dirigido do decodificador interno (aac_decoder.c).
 *
 * Roda no HOST: decodifica um .m4a com o mesmo codigo do port e imprime
 * duracao declarada, quadros produzidos, RMS e pico. O comparativo e' o
 * `ffmpeg` do host -- que existe SO na bancada, nunca no aparelho.
 *
 *   cc -I src -o /tmp/aac_selftest devtools/aac_selftest.c src/aac_decoder.c -lfdk-aac -lm
 *   /tmp/aac_selftest <arquivo.m4a> [saida.pcm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "aac_decoder.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "uso: %s <m4a> [saida.pcm]\n", argv[0]); return 2; }
    const char *path = argv[1];

    int dur = nx_aac_duration_ms(path);
    nx_aac *d = nx_aac_open(path, 0, 48000, 2);
    if (!d) { fprintf(stderr, "FALHA ao abrir %s\n", path); return 1; }

    FILE *out = argc > 2 ? fopen(argv[2], "wb") : NULL;

    static short buf[8192];
    long long frames = 0;
    double acc = 0.0;
    int peak = 0;
    for (;;) {
        int got = nx_aac_read(d, buf, (int)sizeof(buf));
        if (got <= 0) break;
        int n = got / (int)sizeof(short);
        if (out) fwrite(buf, 1, (size_t)got, out);
        for (int i = 0; i < n; i++) {
            int v = buf[i];
            acc += (double)v * v;
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        frames += n / 2;
    }
    nx_aac_close(d);
    if (out) fclose(out);

    double rms = frames ? sqrt(acc / (double)(frames * 2)) : 0.0;
    printf("%s duration_ms=%d frames=%lld decoded_ms=%lld rms=%.1f peak=%d\n",
           path, dur, frames, frames * 1000 / 48000, rms, peak);
    return frames > 0 ? 0 : 1;
}
