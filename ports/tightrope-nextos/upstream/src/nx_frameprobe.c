/* nx_frameprobe — a única testemunha que a captura não consegue enganar.
 *
 * Lê o backbuffer com glReadPixels ANTES do swap e loga quanto da tela está
 * aceso. Como roda dentro do processo, não passa por /dev/fb0, nem pelo `pan`,
 * nem pelo compositor, nem por SSH — as quatro coisas que já inventaram "tela
 * preta" em port que estava rodando.
 *
 * ⚠️ glReadPixels no Mali-450 é STALL (sincroniza a pipeline). Por isso:
 *   - DESLIGADO por padrão. Só liga com NX_FRAMEPROBE=1 (regra: experimento
 *     fora do binário de release, gate OFF).
 *   - Amostra 1 frame a cada NX_FRAMEPROBE_EVERY (default 120) — ~2s a 60fps.
 *   - Lê blocos pequenos (5 pontos de 16x16), não a tela inteira.
 * Ainda assim custa fps enquanto ligado. É instrumento de diagnóstico, não de
 * release: com ele ligado, NÃO meça performance.
 *
 * Uso no loader, imediatamente antes do swap:
 *     nx_frameprobe_before_swap();
 *     eglSwapBuffers(dpy, surf);   // ou SDL_GL_SwapWindow(win)
 *
 * Saída (stderr, uma linha por amostra):
 *     [frameprobe] frame=1200 lit=37.4% luma=88.1 -> IMAGEM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GLES2/gl2.h>

#define NX_FP_BLOCK 16
#define NX_FP_POINTS 5

static int   fp_state = -1;   /* -1 nao inicializado, 0 desligado, 1 ligado */
static long  fp_every = 120;
static long  fp_frame = 0;

static void fp_init(void)
{
    const char *on = getenv("NX_FRAMEPROBE");
    fp_state = (on && *on == '1') ? 1 : 0;
    const char *ev = getenv("NX_FRAMEPROBE_EVERY");
    if (ev && *ev) {
        long v = strtol(ev, NULL, 10);
        if (v > 0) fp_every = v;
    }
    if (fp_state)
        fprintf(stderr, "[frameprobe] LIGADO (a cada %ld frames). "
                        "glReadPixels custa fps no Mali — nao meca performance agora.\n",
                fp_every);
}

/* Chame imediatamente ANTES do swap. */
/* Despejo de UM frame inteiro, de dentro do processo.
 *
 * Num device KMSDRM/GBM (R36S) a saida do jogo NAO passa pelo /dev/fb0 -- ler
 * o framebuffer devolve tela preta e faz parecer que o jogo nao desenha.  A
 * unica captura confiavel ali e esta: glReadPixels antes do swap.
 *
 * NX_FRAMEDUMP=<caminho> grava um frame e para (custa caro; e instrumento).
 * Formato cru RGBA, com as dimensoes no proprio nome para o leitor nao
 * precisar adivinhar. */
static void fp_dump_once(void)
{
    static int done;
    static long seen;
    const char *path = getenv("NX_FRAMEDUMP");
    if (done || !path || !*path)
        return;
    /* Esperar o jogo ter o que mostrar: o PRIMEIRO swap e' quase sempre a tela
     * limpa, e capturar ali faz qualquer jogo parecer preto.  NX_FRAMEDUMP_AT
     * escolhe o frame (default 300, ~5s a 60fps). */
    const char *at = getenv("NX_FRAMEDUMP_AT");
    long want = at && *at ? strtol(at, NULL, 10) : 300;
    if (++seen < want)
        return;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0)
        return;
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char name[1024];
    snprintf(name, sizeof name, "%s.%dx%d.raw", path, w, h);
    FILE *f = fopen(name, "wb");
    if (f) {
        /* glReadPixels devolve de baixo para cima; grava ja invertido para o
         * leitor nao ter de saber disso. */
        for (int y = h - 1; y >= 0; y--)
            fwrite(px + (size_t)y * w * 4, 1, (size_t)w * 4, f);
        fclose(f);
        fprintf(stderr, "[framedump] %s (%dx%d)\n", name, w, h);
    }
    free(px);
    done = 1;
}

void nx_frameprobe_before_swap(void)
{
    fp_dump_once();
    if (fp_state < 0) fp_init();
    if (!fp_state) return;
    if (++fp_frame % fp_every) return;

    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "[frameprobe] frame=%ld viewport invalido (%dx%d)\n", fp_frame, w, h);
        return;
    }

    /* 4 cantos + centro: pega HUD, fundo e cena sem varrer a tela toda. */
    const int b = NX_FP_BLOCK;
    int px[NX_FP_POINTS], py[NX_FP_POINTS];
    px[0] = w / 8;         py[0] = h / 8;
    px[1] = w - w / 8 - b; py[1] = h / 8;
    px[2] = w / 8;         py[2] = h - h / 8 - b;
    px[3] = w - w / 8 - b; py[3] = h - h / 8 - b;
    px[4] = w / 2 - b / 2; py[4] = h / 2 - b / 2;

    unsigned char buf[NX_FP_BLOCK * NX_FP_BLOCK * 4];
    long total = 0, lit = 0;
    double luma_sum = 0.0, luma_sq = 0.0;

    /* Estado do leitor: alinhamento de 1 evita surpresa com bloco 16x16 RGBA. */
    GLint old_align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_align);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for (int i = 0; i < NX_FP_POINTS; i++) {
        int x = px[i] < 0 ? 0 : px[i];
        int y = py[i] < 0 ? 0 : py[i];
        if (x + b > w || y + b > h) continue;
        memset(buf, 0, sizeof(buf));
        glReadPixels(x, y, b, b, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        for (int k = 0; k < b * b; k++) {
            unsigned char r = buf[k * 4 + 0], g = buf[k * 4 + 1], bl = buf[k * 4 + 2];
            double luma = 0.299 * r + 0.587 * g + 0.114 * bl;
            luma_sum += luma;
            luma_sq += luma * luma;
            total++;
            if (luma > 32.0) lit++;   /* brilho e area, NUNCA contagem de cor */
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, old_align);
    /* glReadPixels pode deixar erro pendente; drena pra nao contaminar o jogo. */
    while (glGetError() != GL_NO_ERROR) { }

    if (!total) {
        fprintf(stderr, "[frameprobe] frame=%ld nenhum bloco lido\n", fp_frame);
        return;
    }
    double frac = (double)lit / (double)total;
    double mean = luma_sum / (double)total;
    double var = luma_sq / (double)total - mean * mean;
    double sd = var > 0.0 ? sqrt(var) : 0.0;
    /* CHAPADO antes de IMAGEM: tela branca uniforme e' 100% acesa e nao e' jogo
     * nenhum (textura que nao carregou, shader que falhou, clear sem geometria). */
    const char *cls;
    if (frac >= 0.05 && sd < 6.0)      cls = "CHAPADO";
    else if (frac >= 0.05)             cls = "IMAGEM";
    else if (frac < 0.01)              cls = "PRETO";
    else                               cls = "ESCURO";
    fprintf(stderr, "[frameprobe] frame=%ld lit=%.1f%% luma=%.1f sd=%.1f -> %s\n",
            fp_frame, frac * 100.0, mean, sd, cls);
    fflush(stderr);
}
