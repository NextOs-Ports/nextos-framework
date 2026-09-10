// vkbd — teclado virtual on-screen (ver vkbd.h).
//
// Renderiza um painel com o nome digitado + um grid de teclas (A-Z, a-z, 0-9,
// simbolos, DEL, OK). Navega com d-pad, confirma com A/R3. O texto sai por
// vkbd_text() (o getEditText devolve isto). Fonte via stb_truetype (atlas ASCII
// baked numa textura GL_ALPHA). GLES1 fixed-function, estado salvo/restaurado.
#include "nxcompat_system_font.h"
#include "vkbd.h"

#include <GLES/gl.h>

#include "nxgl_gles1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"  // implementacao vem do game.c (STB_TRUETYPE_IMPLEMENTATION)

// --- atlas de fonte ---------------------------------------------------------
#define ATLAS_W 256
#define ATLAS_H 256
#define FONT_PX 28.0f
static GLuint g_tex;
static stbtt_bakedchar g_cdata[96];  // ASCII 32..127
static int g_ready;

// --- teclas -----------------------------------------------------------------
// grid de KB_COLS colunas + 2 teclas especiais (DEL, OK) na ultima linha.
static const char g_keys[] =
    "ABCDEFGHIJKLM"
    "NOPQRSTUVWXYZ"
    "abcdefghijklm"
    "nopqrstuvwxyz"
    "0123456789 -.";
#define KB_COLS 13
#define NKEYS  (int)(sizeof(g_keys) - 1)  // 65
#define IDX_DEL (NKEYS)
#define IDX_OK  (NKEYS + 1)
#define NTOTAL  (NKEYS + 2)

// --- estado -----------------------------------------------------------------
static int  g_active;
static char g_buf[32];
static int  g_len;
static int  g_max = 5;
static int  g_sel;   // tecla selecionada [0, NTOTAL)

void vkbd_init(const char *ttf_path) {
    /* mesma busca do resto do port: caminho fixo nao cobre toda CFW */
    char found[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
    const char *cands[2]; int nc = 0;
    if (nxcompat_system_font_find(ttf_path, "FF4_FONT", NULL, 0, found, sizeof found))
        cands[nc++] = found;
    unsigned char *ttf = NULL; long sz = 0;
    for (int i = 0; i < nc && !ttf; i++) {
        FILE *f = fopen(cands[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        ttf = malloc(sz);
        if (fread(ttf, 1, sz, f) != (size_t)sz) { free(ttf); ttf = NULL; }
        fclose(f);
    }
    if (!ttf) { fprintf(stderr, "[vkbd] sem fonte TTF — teclado sem letras\n"); return; }

    unsigned char *bmp = calloc(ATLAS_W * ATLAS_H, 1);
    stbtt_BakeFontBitmap(ttf, 0, FONT_PX, bmp, ATLAS_W, ATLAS_H, 32, 96, g_cdata);
    free(ttf);

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, ATLAS_H, 0, GL_ALPHA, GL_UNSIGNED_BYTE, bmp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(bmp);
    g_ready = 1;
    fprintf(stderr, "[vkbd] atlas pronto (tex=%u)\n", g_tex);
}

void vkbd_activate(const char *def, int maxlen) {
    g_active = 1;
    g_max = (maxlen > 0 && maxlen < (int)sizeof g_buf) ? maxlen : 5;
    snprintf(g_buf, sizeof g_buf, "%s", def ? def : "");
    g_len = (int)strlen(g_buf);
    if (g_len > g_max) { g_len = g_max; g_buf[g_len] = 0; }
    g_sel = 0;
    fprintf(stderr, "[vkbd] ativado (def=\"%s\" max=%d)\n", g_buf, g_max);
}

void vkbd_deactivate(void) { g_active = 0; }
int  vkbd_is_active(void) { return g_active; }
const char *vkbd_text(void) { return g_buf; }

// input por edge; retorna 1 quando confirma OK.
int vkbd_handle(int left, int right, int up, int down, int confirm, int cancel) {
    if (!g_active) return 0;
    if (left)  g_sel = (g_sel - 1 + NTOTAL) % NTOTAL;
    if (right) g_sel = (g_sel + 1) % NTOTAL;
    if (up)    { g_sel -= KB_COLS; if (g_sel < 0) g_sel += NTOTAL; }
    if (down)  { g_sel += KB_COLS; if (g_sel >= NTOTAL) g_sel -= NTOTAL; }
    if (cancel) {  // backspace rapido no B (nao fecha)
        if (g_len > 0) g_buf[--g_len] = 0;
    }
    if (confirm) {
        if (g_sel == IDX_OK) { g_active = 0; return 1; }
        if (g_sel == IDX_DEL) { if (g_len > 0) g_buf[--g_len] = 0; }
        else if (g_sel >= 0 && g_sel < NKEYS && g_len < g_max) {
            g_buf[g_len++] = g_keys[g_sel];
            g_buf[g_len] = 0;
        }
    }
    return 0;
}

// --- render -----------------------------------------------------------------
static void rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(r, g, b, a);
    GLfloat v[] = {x, y, x + w, y, x, y + h, x + w, y + h};
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static float text_w(const char *s, float sc) {
    float w = 0;
    for (; *s; s++) if (*s >= 32 && *s < 128) w += g_cdata[*s - 32].xadvance * sc;
    return w;
}

static void text(float x, float y, float sc, const char *s, float r, float g, float b, float a) {
    if (!g_ready) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(r, g, b, a);
    for (; *s; s++) {
        if (*s < 32 || *s >= 128) continue;
        stbtt_bakedchar *c = &g_cdata[*s - 32];
        float x0 = x + c->xoff * sc, y0 = y + c->yoff * sc;
        float x1 = x0 + (c->x1 - c->x0) * sc, y1 = y0 + (c->y1 - c->y0) * sc;
        float u0 = c->x0 / (float)ATLAS_W, v0 = c->y0 / (float)ATLAS_H;
        float u1 = c->x1 / (float)ATLAS_W, v1 = c->y1 / (float)ATLAS_H;
        GLfloat vv[] = {x0, y0, x1, y0, x0, y1, x1, y1};
        GLfloat uv[] = {u0, v0, u1, v0, u0, v1, u1, v1};
        glVertexPointer(2, GL_FLOAT, 0, vv);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        x += c->xadvance * sc;
    }
}

void vkbd_render(int win_w, int win_h) {
    if (!g_active) return;

    // salva estado / ortho em pixels
    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrthof(0, (GLfloat)win_w, (GLfloat)win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
    glDisable(GL_FOG); glDisable(GL_ALPHA_TEST); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    // painel (metade de baixo)
    float pw = win_w * 0.72f, ph = win_h * 0.52f;
    float px = (win_w - pw) / 2, py = win_h * 0.44f;
    rect(px, py, pw, ph, 0.10f, 0.12f, 0.28f, 0.92f);
    rect(px, py, pw, 4, 0.6f, 0.7f, 1.0f, 1.0f);

    float sc = win_h / 720.0f;  // escala relativa

    // caixa do nome digitado
    float nbx = px + pw * 0.5f - 150 * sc, nby = py + 16 * sc;
    rect(nbx, nby, 300 * sc, 42 * sc, 0.02f, 0.03f, 0.10f, 1.0f);
    char shown[40];
    snprintf(shown, sizeof shown, "%s_", g_buf);
    text(nbx + 12 * sc, nby + 30 * sc, 1.1f * sc, shown, 1, 1, 1, 1);

    // grid de teclas
    float gx = px + 30 * sc, gy = py + 78 * sc;
    float cw = (pw - 60 * sc) / KB_COLS, chh = 40 * sc;
    for (int i = 0; i < NTOTAL; i++) {
        int row, col;
        char label[8];
        if (i < NKEYS) { row = i / KB_COLS; col = i % KB_COLS; label[0] = g_keys[i]; label[1] = 0; }
        else { row = NKEYS / KB_COLS; col = (i == IDX_DEL) ? 0 : 4;
               snprintf(label, sizeof label, "%s", i == IDX_DEL ? "DEL" : "OK"); }
        float kx = gx + col * cw, ky = gy + row * (chh + 6 * sc);
        float kw = (i >= NKEYS) ? cw * 3.5f : cw - 4 * sc;
        int seld = (i == g_sel);
        if (label[0] == ' ') snprintf(label, sizeof label, "SP");
        rect(kx, ky, kw, chh, seld ? 0.95f : 0.20f, seld ? 0.8f : 0.24f,
             seld ? 0.2f : 0.42f, seld ? 1.0f : 0.85f);
        float lw = text_w(label, 1.0f * sc);
        text(kx + (kw - lw) / 2, ky + 28 * sc, 1.0f * sc, label,
             seld ? 0.05f : 1.0f, seld ? 0.05f : 1.0f, seld ? 0.05f : 1.0f, 1.0f);
    }
    // dica
    text(px + 16 * sc, py + ph - 12 * sc, 0.8f * sc,
         "d-pad move  A/R3 escolhe  B apaga  (OK finaliza)", 0.8f, 0.85f, 1.0f, 1.0f);

    // restaura
    glColor4f(1, 1, 1, 1);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}
