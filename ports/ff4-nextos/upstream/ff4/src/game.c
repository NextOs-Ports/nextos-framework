// game — implementacoes em C dos metodos estaticos do MainActivity.
// Cada funcao espelha o Java decompilado; divergir daqui = bug de fluxo.
#include "nxcompat_system_font.h"
#include "game.h"
#include "ff_language.h"
#include "jni_shim.h"  // jni_get_fps() — o FF4 usa fps dinamico no frame limiter

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int g_language = GAME_LANG_EN;  // 🚨 JAMAIS japones

// Resolve o idioma a partir de NXPORT_LANGUAGE (bloco do launcher) na
// primeira leitura. Prefixo pt do FF4 e' pt_BR (medido no container).
void game_init_language(void) {
    ff_language sel = ff_language_select("pt_BR");
    g_language = sel.index;
    fprintf(stderr, "[ff4] idioma index=%d (%s.lproj)\n",
            g_language, sel.lproj);
}
int g_pad_mask = 0;
int g_ab_swapped = 0;
int g_back_enabled = 0;

// ===========================================================================
// VFS ARC1 (formato validado no host por tools/ff3arc.c — ver STUDY.md §3)
// ===========================================================================
#define ARC_SEED 0x19000000u   // FF4 base (ff3=84861466, ff4a=98910408) — smali e()/encode
#define ARC_MAGIC 0x31435241u  // "ARC1"

static FILE *g_obb;
static uint8_t *g_dir;
static size_t g_dir_len;
static uint32_t g_dir_count;

static void arc_decode(uint8_t *buf, size_t len, uint32_t seed) {
    // LCG identico ao rand() da glibc — desassemblado de libjniproxy.so:encode.
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] ^= (uint8_t)(seed >> 24);
    }
}

static uint32_t le32(const uint8_t *p, size_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

static uint8_t *arc_read_raw(uint32_t off, uint32_t len) {
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return NULL;
    if (fseek(g_obb, off, SEEK_SET) != 0 || fread(buf, 1, len, g_obb) != len) {
        free(buf);
        return NULL;
    }
    return buf;
}

// Metodo f() do Java: int32 BIG-ENDIAN com o tamanho descomprimido + stream gzip.
static uint8_t *arc_gunzip(const uint8_t *in, size_t in_len, size_t *out_len) {
    if (in_len < 4) return NULL;
    size_t want = ((size_t)in[0] << 24) | ((size_t)in[1] << 16) | ((size_t)in[2] << 8) | in[3];
    uint8_t *out = malloc(want ? want : 1);
    if (!out) return NULL;

    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        free(out);
        return NULL;
    }
    zs.next_in = (Bytef *)in + 4;
    zs.avail_in = in_len - 4;
    zs.next_out = out;
    zs.avail_out = want;
    int r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (r != Z_STREAM_END && r != Z_OK) {
        free(out);
        return NULL;
    }
    *out_len = zs.total_out;
    return out;
}

int game_vfs_open(const char *obb_path) {
    g_obb = fopen(obb_path, "rb");
    if (!g_obb) {
        fprintf(stderr, "[vfs] nao abriu %s\n", obb_path);
        return -1;
    }
    uint8_t *head = arc_read_raw(0, 16);
    if (!head) return -1;
    arc_decode(head, 16, ARC_SEED);
    if (le32(head, 0) != ARC_MAGIC) {
        fprintf(stderr, "[vfs] magic ARC1 invalido\n");
        free(head);
        return -1;
    }
    uint32_t dir_off = le32(head, 8), dir_size = le32(head, 12);
    free(head);

    uint8_t *raw = arc_read_raw(dir_off, dir_size);
    if (!raw) return -1;
    arc_decode(raw, dir_size, dir_off + ARC_SEED);
    g_dir = arc_gunzip(raw, dir_size, &g_dir_len);
    free(raw);
    if (!g_dir) {
        fprintf(stderr, "[vfs] gunzip do diretorio falhou\n");
        return -1;
    }
    g_dir_count = le32(g_dir, 0);
    fprintf(stderr, "[vfs] ARC1 ok: %u arquivos\n", g_dir_count);
    return 0;
}

// Busca binaria identica ao metodo d() do Java (tabela ordenada por nome).
uint8_t *game_vfs_read(const char *name, size_t *len) {
    if (!g_dir) return NULL;
    size_t name_len = strlen(name);
    uint32_t lo = 0, hi = g_dir_count, found = 0;

    while (hi > lo) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t entry = mid * 12;
        uint32_t name_off = le32(g_dir, entry + 4);

        int cmp = 0;
        for (size_t i = 0; i < name_len && cmp == 0; i++) {
            cmp = (int)g_dir[name_off + i] - (int)(uint8_t)name[i];
        }
        if (cmp == 0) cmp = g_dir[name_off + name_len];  // exige o NUL terminador

        if (cmp == 0) {
            found = entry + 8;
            break;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (!found) return NULL;

    uint32_t off = le32(g_dir, found + 0);
    uint32_t size = le32(g_dir, found + 4);
    uint8_t *raw = arc_read_raw(off, size);
    if (!raw) return NULL;
    arc_decode(raw, size, off + ARC_SEED);
    uint8_t *out = arc_gunzip(raw, size, len);
    free(raw);
    return out;
}

// ===========================================================================
// getCurrentFrame — o relogio do jogo (STUDY.md §2.4)
// ===========================================================================
// FF4 (smali): t = currentTimeMillis() * S; bloqueia enquanto t/1000 == last;
// retorna t/1000. S = fps (setFPS). Tick de 1000/S ms. Aqui com CLOCK_MONOTONIC
// (imune a ajuste de hora). Diferente do ff3, que era fixo em 30 Hz (*3/100).
int64_t game_getCurrentFrame(int64_t last) {
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        int fps = jni_get_fps();
        if (fps <= 0) fps = 30;
        int64_t tick = (ms * fps) / 1000;
        if (tick != last) return tick;
        // O Java usa Thread.yield(); dormir 1ms evita queimar CPU sem perder o tick.
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
}

// ===========================================================================
// loadFile / loadRawFile
// ===========================================================================
// FF4 base: 12 idiomas (loadFile smali). Index 1 = "en". 🚨 JAMAIS japones (0).
static const char *k_lang_dirs[] = {"ja", "en", "fr", "de", "it", "es",
                                    "zh_CN", "zh_TW", "ko", "pt_BR", "ru", "th"};

// FF4_VFSLOG=1 mostra cada acesso; FF4_VFSLOG=2 so os que FALHAM.
static int vfs_log_level(void) {
    static int lvl = -1;
    if (lvl < 0) {
        const char *e = getenv("FF4_VFSLOG");
        lvl = e ? atoi(e) : 0;
    }
    return lvl;
}

// FF4 loadSound(name) (smali): "voice/xxx" -> "xxx" senao name+".akb"; tenta
// files/SOUND/BGM/ -> SE/ -> VOICE/. Devolve os bytes akb (o engine decodifica).
uint8_t *game_loadSound(const char *name, size_t *len) {
    char akb[512];
    if (strncmp(name, "voice/", 6) == 0)
        snprintf(akb, sizeof akb, "%s", name + 6);
    else
        snprintf(akb, sizeof akb, "%s.akb", name);

    static const char *dirs[] = {"files/SOUND/BGM/", "files/SOUND/SE/", "files/SOUND/VOICE/"};
    for (size_t i = 0; i < 3; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s%s", dirs[i], akb);
        uint8_t *d = game_vfs_read(path, len);
        if (d) return d;
    }
    if (vfs_log_level()) fprintf(stderr, "[vfs] sound %-36s *** FALTOU ***\n", akb);
    return NULL;
}

uint8_t *game_loadRawFile(const char *name, size_t *len) {
    uint8_t *d = game_vfs_read(name, len);
    int lvl = vfs_log_level();
    if (lvl == 1 || (lvl && !d))
        fprintf(stderr, "[vfs] raw %-40s %s\n", name, d ? "ok" : "*** FALTOU ***");
    return d;
}

uint8_t *game_loadFile(const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof path, "%s.lproj/%s", k_lang_dirs[g_language], name);
    uint8_t *data = game_vfs_read(path, len);
    const char *via = "lproj";
    if (!data) {
        snprintf(path, sizeof path, "files/%s", name);
        data = game_vfs_read(path, len);
        via = "files";
    }
    int lvl = vfs_log_level();
    if (lvl == 1 || (lvl && !data))
        fprintf(stderr, "[vfs] %-40s %s%s\n", name, data ? via : "*** FALTOU ***",
                data ? "" : "");
    if (!data) return NULL;

    // FF4 loadFile (smali): extensao ".msd" e primeiro char != 'e' -> decodeString.
    // (FF4 NAO tem o check "noa" do ff3.)
    const char *dot = strrchr(name, '.');
    if (dot && !strcmp(dot, ".msd") && name[0] != 'e') {
        size_t out_len = 0;
        uint8_t *conv = game_decodeString(data, *len, &out_len);
        if (getenv("FF4_FONTLOG"))
            fprintf(stderr, "[decodeString] %s: %zu -> %zu bytes\n", name, *len, out_len);
        if (conv) {
            free(data);
            *len = out_len;
            return conv;
        }
    }
    return data;
}

// ===========================================================================
// decodeString — REPACK das tabelas .msd (espelha o Java do FF4, NAO converte!)
// ===========================================================================
// ⚠️ O ff3 fazia cp1252->UTF-8 + split no NUL. O FF4 NAO converte encoding: as
// strings ja sao **UTF-16LE**; decodeString so COMPACTA — copia cada bloco
// [thisOffset, nextOffset) VERBATIM, reescreve o offset da entrada e poe um
// terminador 0x0000. Converter/splitar destruia o UTF-16 (o engine lia
// desalinhado -> chars CJK: "S",一,"N",䠀,...). Layout: header 16B (count@8),
// entradas 12B a partir de 16 (offset@+8). Passthrough p/ zh (lang 6/7).
uint8_t *game_decodeString(const uint8_t *in, size_t in_len, size_t *out_len) {
    if (g_language == 6 || g_language == 7) {  // zh_CN/zh_TW: sem repack
        uint8_t *copy = malloc(in_len ? in_len : 1);
        memcpy(copy, in, in_len);
        *out_len = in_len;
        return copy;
    }
    if (in_len < 16) return NULL;

    uint32_t count = le32(in, 8);
    size_t header = (size_t)count * 12 + 16;
    if (header > in_len) return NULL;

    // saida = header + soma(len+2). Pior caso ~ in_len + count*2 + header.
    uint8_t *out = calloc(in_len * 2 + (size_t)count * 2 + 64, 1);
    if (!out) return NULL;
    memcpy(out, in, header);

    size_t write = header;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = 16 + (size_t)i * 12;
        uint32_t this_off = le32(in, entry + 8);
        // proximo bloco = offset da PROXIMA entrada (assume ordenado); ultimo = fim.
        uint32_t next_off = (i + 1 < count) ? le32(in, entry + 12 + 8) : (uint32_t)in_len;
        if (this_off > in_len) this_off = (uint32_t)in_len;
        if (next_off > in_len) next_off = (uint32_t)in_len;
        size_t len = (next_off >= this_off) ? (size_t)(next_off - this_off) : 0;

        // reescreve o offset da entrada no header copiado -> nova posicao compactada
        out[entry + 8 + 0] = (uint8_t)(write >> 0);
        out[entry + 8 + 1] = (uint8_t)(write >> 8);
        out[entry + 8 + 2] = (uint8_t)(write >> 16);
        out[entry + 8 + 3] = (uint8_t)(write >> 24);

        memcpy(out + write, in + this_off, len);  // VERBATIM (UTF-16LE preservado)
        write += len;
        out[write++] = 0;  // terminador UTF-16 (0x0000)
        out[write++] = 0;
    }
    *out_len = write;
    return out;
}

// ===========================================================================
// loadTexture — PNG -> int[]{w, h, pixels ARGB_8888}
// ===========================================================================
int32_t *game_loadTexture(const uint8_t *png, size_t png_len, size_t *out_ints) {
    int w = 0, h = 0, comp = 0;
    stbi_uc *rgba = stbi_load_from_memory(png, (int)png_len, &w, &h, &comp, 4);
    if (!rgba) {
        fprintf(stderr, "[tex] PNG invalido (%s)\n", stbi_failure_reason());
        return NULL;
    }
    int32_t *out = malloc(((size_t)w * h + 2) * sizeof(int32_t));
    if (!out) {
        stbi_image_free(rgba);
        return NULL;
    }
    out[0] = w;
    out[1] = h;
    // Bitmap.getPixels devolve ARGB_8888 empacotado como 0xAARRGGBB.
    for (int i = 0; i < w * h; i++) {
        out[2 + i] = ((int32_t)rgba[i * 4 + 3] << 24) | ((int32_t)rgba[i * 4 + 0] << 16) |
                     ((int32_t)rgba[i * 4 + 1] << 8) | (int32_t)rgba[i * 4 + 2];
    }
    stbi_image_free(rgba);
    *out_ints = (size_t)w * h + 2;
    return out;
}

// ===========================================================================
// drawFont — rasteriza texto num bitmap quadrado (espelha Canvas/Paint)
// ===========================================================================
// Retorno: int[]{advance, xOff, yOff, top, bottom, pixels ARGB size*size}.
static stbtt_fontinfo g_font;
static uint8_t *g_font_data;
static int g_font_ok;

static void font_init(void) {
    if (g_font_ok) return;
    /* A lista fixa nao cobre todo aparelho: no dArkOS do R36S so' existe
     * /usr/share/fonts/truetype/dejavu/, e um caminho ausente deixa o jogo
     * inteiro SEM TEXTO, sem erro nenhum. nx_find_system_font termina numa
     * varredura de diretorio justamente para isso. */
    char found[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
    const char *candidates[] = {
        nxcompat_system_font_find(NULL, "FF4_FONT", NULL, 0, found, sizeof found) ? found : NULL,
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (!candidates[i]) continue;
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_font_data = malloc(sz);
        if (fread(g_font_data, 1, sz, f) == (size_t)sz &&
            stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0))) {
            fclose(f);
            g_font_ok = 1;
            fprintf(stderr, "[font] usando %s\n", candidates[i]);
            return;
        }
        fclose(f);
        free(g_font_data);
        g_font_data = NULL;
    }
    fprintf(stderr, "[font] NENHUMA fonte encontrada — texto sairia vazio\n");
    g_font_ok = -1;
}

// Decodifica UTF-8: devolve o codepoint em *cp e avanca o ponteiro.
static const char *utf8_next(const char *p, int *cp) {
    int c = (uint8_t)*p++;
    int nb = 0;
    if (c >= 0xF0)      { c &= 0x07; nb = 3; }
    else if (c >= 0xE0) { c &= 0x0F; nb = 2; }
    else if (c >= 0xC0) { c &= 0x1F; nb = 1; }
    while (nb-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *cp = c;
    return p;
}

// FF4 drawFont(text, size, textSize, baseline) — espelha o Java: rasteriza a
// STRING INTEIRA (Canvas.drawText) num bitmap size x size. Retorno int[]:
//   [0]=advance do 1o char, [1]=xOff, [2]=yOff, [3]=top, [4]=bottom, [5..]=ARGB.
// (O ff3 desenhava so o 1o glifo; o FF4 passa rotulos de menu inteiros -> tinha
// que renderizar todos os glifos, senao aparece so a 1a letra.)
int32_t *game_drawFont(const char *text, int size, int text_size, int baseline,
                       size_t *out_ints) {
    font_init();
    if (getenv("FF4_FONTLOG") && text && text[0])
        fprintf(stderr, "[drawFont] size=%d ts=%d bl=%d text=\"%s\"\n", size, text_size, baseline, text);
    size_t n = (size_t)size * size + 5;
    int32_t *out = calloc(n, sizeof(int32_t));
    if (!out) return NULL;
    *out_ints = n;
    if (g_font_ok != 1 || !text || !text[0]) return out;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)text_size);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &line_gap);
    int top_px = (int)(ascent * scale), bottom_px = (int)(descent * scale);
    int base_y = baseline - ((-top_px) + (-bottom_px)) / 2;

    // Passo 1: mede a string inteira (bounds do getTextBounds + advance do 1o char).
    float pen = 0.0f;
    int rect_left = 0, rect_top = 0, rect_bottom = 0, have_bounds = 0, first_adv = 0, first = 1;
    for (const char *p = text; *p;) {
        int cp;
        p = utf8_next(p, &cp);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, cp, scale, scale, &x0, &y0, &x1, &y1);
        if (first) { first_adv = (int)(adv * scale); rect_left = (int)(pen) + x0; first = 0; }
        if (x1 > x0 && y1 > y0) {
            if (!have_bounds) { rect_top = y0; rect_bottom = y1; have_bounds = 1; }
            else { if (y0 < rect_top) rect_top = y0; if (y1 > rect_bottom) rect_bottom = y1; }
        }
        pen += adv * scale;
    }
    int x_off = rect_left < 1 ? -rect_left + 1 : 0;
    int y_off = (base_y + rect_top) < 1 ? -(rect_top + base_y) + 1 : 0;
    out[0] = first_adv < 0 ? 0 : first_adv;
    out[1] = x_off;
    out[2] = y_off;
    out[3] = rect_top + base_y + y_off;
    out[4] = rect_bottom + base_y + y_off;

    // Passo 2: rasteriza cada glifo na posicao do pen (branco, alfa=cobertura).
    pen = 0.0f;
    for (const char *p = text; *p;) {
        int cp;
        p = utf8_next(p, &cp);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, cp, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            uint8_t *gray = calloc((size_t)gw * gh, 1);
            stbtt_MakeCodepointBitmap(&g_font, gray, gw, gh, gw, scale, scale, cp);
            int pen_i = (int)(pen + 0.5f);
            for (int y = 0; y < gh; y++) {
                int dy = base_y + y_off + y0 + y;
                if (dy < 0 || dy >= size) continue;
                for (int x = 0; x < gw; x++) {
                    int dx = x_off + pen_i + x0 + x;
                    if (dx < 0 || dx >= size) continue;
                    uint8_t a = gray[y * gw + x];
                    if (a) out[5 + dy * size + dx] = ((int32_t)a << 24) | 0x00FFFFFF;
                }
            }
            free(gray);
        }
        pen += adv * scale;
    }
    return out;
}

// ===========================================================================
// trace / som / saves
// ===========================================================================
void game_trace(const char *msg) {
    fprintf(stderr, "[ff3] %s\n", msg);
}

// ===========================================================================
// FMV (res/raw/opening.mp4, 92MB H.264)
// ===========================================================================
// No Android o playMovie liga MainActivity.y (= movieState) e sobe um
// MediaPlayer POR CIMA do GLSurfaceView; o engine para de desenhar e so
// retoma quando getMovieState() volta 0 (fim do video).
// Aqui nao decodificamos H.264 — mas precisamos honrar o CICLO, senao o
// engine fica esperando um fim que nunca chega e a tela fica parada.
// Entao: liga o estado e desliga sozinho depois de um tempo curto, que e
// exatamente o que o engine espera ver ("o filme rodou e acabou").
static int g_movie_playing;
static int64_t g_movie_until;

// MEDIDO NO DEVICE: sinalizar "filme tocando" e depois "terminou" leva o engine
// para a rota de fim-de-filme, que desreferencia um ponteiro nulo e derruba o
// jogo no frame ~471. Com movieState SEMPRE 0 o engine entende que nao ha filme
// e segue o fluxo normal ate o menu — comportamento validado por captura de tela.
// Ou seja: nao ha handshake pendente; o FMV simplesmente nao entra em cena.
// Para exibir o video de verdade seria preciso decodificar res/raw/opening.mp4
// (92MB H.264) e compor por cima do GL, como o VideoView faz no Android.
void game_playMovie(void) {
    g_movie_playing = 0;
    fprintf(stderr, "[fmv] playMovie ignorado (sem decodificador; ver comentario)\n");
}

void game_stopMovie(void) {
    g_movie_playing = 0;
    fprintf(stderr, "[fmv] stopMovie\n");
}

int game_getMovieState(void) {
    return 0;  // ver game_playMovie
}

// O audio real e servido pelo shim de OpenSLES (o engine usa SdSoundSystem +
// slCreateEngine). Estes callbacks sao o caminho de UI/BGM do lado Java.
void game_playSound(int ch, const char *name) {
    fprintf(stderr, "[snd] play ch=%d %s\n", ch, name);
}
void game_pauseSound(int ch, int flag) { (void)ch; (void)flag; }
void game_stopSound(int ch) { (void)ch; }
void game_setSoundVolume(int ch, float vol) { (void)ch; (void)vol; }
int game_getSoundState(int ch) { (void)ch; return 0; }

// ===========================================================================
// Saves — espelham getFilesDir()/openFileOutput do Android
// ===========================================================================
// ⚠️ O nome engana: getSaveFileName() devolve o **DIRETORIO** (getFilesDir()),
// nao um arquivo. E createSaveFile(n) PRE-CRIA "save.bin" com n bytes zerados
// dentro dele. O nativo depois abre <dir>/save.bin direto com fopen.
// Sem esses dois casando, o fopen falha e o jogo nao passa do New Game.
static const char *save_dir(void) {
    static char dir[512];
    if (!dir[0]) {
        const char *base = getenv("FF4_GAMEDIR");
        snprintf(dir, sizeof dir, "%s/saves", base ? base : ".");
        mkdir(dir, 0755);
    }
    return dir;
}

void game_createSaveFile(int size) {
    char path[600];
    snprintf(path, sizeof path, "%s/save.bin", save_dir());

    // Se ja existe com tamanho suficiente, nao destruir o save do jogador.
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= size) {
        fprintf(stderr, "[save] save.bin ja existe (%lld bytes), preservado\n",
                (long long)st.st_size);
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[save] NAO criou %s\n", path);
        return;
    }
    static const uint8_t zeros[4096] = {0};
    int left = size;
    while (left > 0) {
        int n = left > (int)sizeof zeros ? (int)sizeof zeros : left;
        if (fwrite(zeros, 1, n, f) != (size_t)n) break;
        left -= n;
    }
    fclose(f);
    fprintf(stderr, "[save] save.bin criado com %d bytes em %s\n", size, path);
}

uint8_t *game_getSaveFileName(size_t *len) {
    const char *dir = save_dir();
    *len = strlen(dir);
    uint8_t *out = malloc(*len + 1);
    memcpy(out, dir, *len + 1);
    fprintf(stderr, "[save] diretorio de saves: %s\n", dir);
    return out;
}

// FF4: getSaveDataPath() devolve o CAMINHO COMPLETO do save.bin (getFilesDir()
// + "/save.bin"), como byte[]. O engine faz fopen direto nesse caminho.
uint8_t *game_getSaveDataPath(size_t *len) {
    char path[600];
    snprintf(path, sizeof path, "%s/save.bin", save_dir());
    *len = strlen(path);
    uint8_t *out = malloc(*len + 1);
    memcpy(out, path, *len + 1);
    fprintf(stderr, "[save] getSaveDataPath: %s\n", path);
    return out;
}
