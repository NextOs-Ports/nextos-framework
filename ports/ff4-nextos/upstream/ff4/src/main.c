// ff4 — so-loader do Final Fantasy IV (3D remake base) para Mali-450 / GLES1.
//
// Engine cuore/Matrix (mesma familia do ff3 e do FF4 After Years). Diferente do
// ff3, o FF4 PUXA o estado por upcalls (getKeyEvent/getResWidth/Height/setFPS) em
// vez de receber pad no touch(). Entry-points: resume/render(cutout)/touch/pause/
// resumeFont — chamados direto (bypass do libff4proxy, Alternativa B).
//
// 🎮 META: controle 100% NATIVO (d-pad + A/B navegam mundo/batalha/menus).
// O cursor (analogico direito + R3) e' apenas um fallback contextual para UI.
#include "game.h"
#include "jni_shim.h"
#include "so_util.h"

#include <SDL2/SDL.h>
#include <GLES/gl.h>

#include "nxgl_frame_proof_adapter.h"
#include "nxgl_provider_discovery_adapter.h"
#include "nxgl_gles1.h"
/* Ver o comentario em audio.c: piso de glibc por symver, nao por sorte. */
#include "nx_symver.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

/* Receita "ordinal pad fix" do framework. A copia local do port foi
 * aposentada: ela nao tinha o gate de barramento externo, entao aplicava o
 * remendo tambem em pad USB de verdade, que nao precisa dele. */
#include "nxinput_pad_ordinal_fix.h"
#include "vkbd.h"

extern const so_import_t g_ff4_imports[];
extern const size_t g_ff4_imports_count;

// ABI JNI: (JNIEnv*, jobject, ...). FF4: render(cutout),
// touch(currentCount, previousOrPeakCount, x0, y0, x1, y1).
typedef void (*ff4_render_t)(void *env, void *thiz, int cutout);
typedef void (*ff4_touch_t)(void *env, void *thiz, int current_count,
                            int previous_or_peak_count, float x0, float y0,
                            float x1, float y1);
typedef void (*ff4_void_t)(void *env, void *thiz);

static ff4_render_t ff4_render;
static ff4_touch_t ff4_touch;
static ff4_void_t ff4_resume, ff4_pause, ff4_resumeFont;

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

// --- mascara de tecla do engine — TABELA R REAL do MainActivity (smali) -------
// 0x60 BUTTON_A->0x1 | 0x61 BUTTON_B->0x2 | dpad 0x10/20/40/80 | R1->0x100 |
// L1->0x200 | BUTTON_Y/MENU->0x400 | BUTTON_X->0x800 | BUTTON_SELECT->0x4000 |
// THUMBL/THUMBR/DPAD_CENTER->0x10000 | START (0x6c) NAO gera bit (consumido).
// 0x8000/0x4000 do BACK sao SO' da tecla BACK do Android (g(): modo 1/2); o B
// do gamepad nunca os envia. O port usa esses bits apenas para transformar
// START num abre/fecha-menu sem alterar o B nativo.
#define K_A      0x0001
#define K_B      0x0002
#define K_RIGHT  0x0010
#define K_LEFT   0x0020
#define K_UP     0x0040
#define K_DOWN   0x0080
#define K_R      0x0100
#define K_L      0x0200
#define K_Y      0x0400
#define K_X      0x0800
#define K_SELECT 0x4000
#define K_BACK    0x8000
#define K_STICK  0x10000

// --- cursor FALLBACK contextual (analogico direito + R3) ---------------------
static int cursor_on = 1;
static int cursor_always = 0;  // apenas diagnostico; gameplay fica sem cursor
// Cursor no padrao da casa: MEXEU no analogico direito, a seta aparece;
// PAROU, some em 3s. R3 clica enquanto ela esta' visivel. Nao ha gesto para
// acender/apagar -- o proprio movimento decide, como nos outros ports.
static Uint32 g_cursor_show_until = 0;   // seta visivel/ativa ate este tick
#define CURSOR_HOLD_MS 3000
static float cur_x = 0.5f, cur_y = 0.5f;
static float cur_vx = 0.0f, cur_vy = 0.0f;

static void gl_restore_cap(GLenum cap, GLboolean enabled) {
    if (enabled) glEnable(cap);
    else glDisable(cap);
}

static void gl_restore_client_cap(GLenum cap, GLboolean enabled) {
    if (enabled) glEnableClientState(cap);
    else glDisableClientState(cap);
}

static void cursor_draw(int win_w, int win_h, float px, float py, int pressed) {
    float scale = win_h / 720.0f;
    if (scale < 0.85f) scale = 0.85f;
    if (scale > 1.80f) scale = 1.80f;
    static const GLfloat base_fill[] = {
        0, 0,   0, 25,  23, 17,          // cabeca da seta
        7, 18, 12, 31,  18, 28,          // haste, triangulo 1
        7, 18, 18, 28,  13, 16           // haste, triangulo 2
    };
    static const GLfloat base_outline[] = {
        0, 0,  0, 25,  7, 18,  12, 31,  18, 28,  13, 16,  23, 17
    };
    GLfloat fill[sizeof base_fill / sizeof base_fill[0]];
    GLfloat shadow[sizeof base_fill / sizeof base_fill[0]];
    GLfloat outline[sizeof base_outline / sizeof base_outline[0]];
    for (size_t i = 0; i < sizeof fill / sizeof fill[0]; i += 2) {
        fill[i] = px + base_fill[i] * scale;
        fill[i + 1] = py + base_fill[i + 1] * scale;
        shadow[i] = fill[i] + 2.5f * scale;
        shadow[i + 1] = fill[i + 1] + 3.0f * scale;
    }
    for (size_t i = 0; i < sizeof outline / sizeof outline[0]; i += 2) {
        outline[i] = px + base_outline[i] * scale;
        outline[i + 1] = py + base_outline[i + 1] * scale;
    }

    // O overlay roda depois do render nativo, mas o contexto e' o mesmo. Salvar
    // tudo que tocamos e' obrigatorio: menus do FF4 reutilizam estado/pointers
    // entre quadros e uma seta que deixa vertex pointer, scissor ou blend para
    // tras faz a UI desaparecer no quadro seguinte.
    GLint old_matrix_mode = GL_MODELVIEW, old_viewport[4];
    GLint old_blend_src = GL_ONE, old_blend_dst = GL_ZERO;
    GLint old_array_buffer = 0, old_vertex_size = 4;
    GLint old_vertex_type = GL_FLOAT, old_vertex_stride = 0;
    GLfloat old_color[4], old_line_width = 1.0f;
    GLvoid *old_vertex_pointer = NULL;
    GLboolean old_depth, old_texture, old_lighting, old_cull, old_fog;
    GLboolean old_alpha, old_scissor, old_blend;
    GLboolean old_vertex, old_texcoord, old_color_array, old_normal;
    glGetIntegerv(GL_MATRIX_MODE, &old_matrix_mode);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetIntegerv(GL_BLEND_SRC, &old_blend_src);
    glGetIntegerv(GL_BLEND_DST, &old_blend_dst);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &old_vertex_size);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &old_vertex_type);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &old_vertex_stride);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &old_vertex_pointer);
    glGetFloatv(GL_CURRENT_COLOR, old_color);
    glGetFloatv(GL_LINE_WIDTH, &old_line_width);
    glGetBooleanv(GL_DEPTH_TEST, &old_depth);
    glGetBooleanv(GL_TEXTURE_2D, &old_texture);
    glGetBooleanv(GL_LIGHTING, &old_lighting);
    glGetBooleanv(GL_CULL_FACE, &old_cull);
    glGetBooleanv(GL_FOG, &old_fog);
    glGetBooleanv(GL_ALPHA_TEST, &old_alpha);
    glGetBooleanv(GL_SCISSOR_TEST, &old_scissor);
    glGetBooleanv(GL_BLEND, &old_blend);
    glGetBooleanv(GL_VERTEX_ARRAY, &old_vertex);
    glGetBooleanv(GL_TEXTURE_COORD_ARRAY, &old_texcoord);
    glGetBooleanv(GL_COLOR_ARRAY, &old_color_array);
    glGetBooleanv(GL_NORMAL_ARRAY, &old_normal);

    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrthof(0, (GLfloat)win_w, (GLfloat)win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE); glDisable(GL_FOG); glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(2, GL_FLOAT, 0, shadow);
    glColor4f(0.0f, 0.0f, 0.0f, 0.48f);
    glDrawArrays(GL_TRIANGLES, 0, 9);
    glVertexPointer(2, GL_FLOAT, 0, fill);
    if (pressed) glColor4f(1.0f, 0.78f, 0.16f, 1.0f);
    else         glColor4f(0.96f, 0.98f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 9);
    glVertexPointer(2, GL_FLOAT, 0, outline);
    glLineWidth(2.0f * scale);
    glColor4f(0.05f, 0.08f, 0.16f, 1.0f);
    glDrawArrays(GL_LINE_LOOP, 0, 7);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_array_buffer);
    glVertexPointer(old_vertex_size, (GLenum)old_vertex_type, old_vertex_stride,
                    old_vertex_pointer);
    gl_restore_client_cap(GL_VERTEX_ARRAY, old_vertex);
    gl_restore_client_cap(GL_TEXTURE_COORD_ARRAY, old_texcoord);
    gl_restore_client_cap(GL_COLOR_ARRAY, old_color_array);
    gl_restore_client_cap(GL_NORMAL_ARRAY, old_normal);
    glBlendFunc((GLenum)old_blend_src, (GLenum)old_blend_dst);
    glLineWidth(old_line_width);
    glColor4f(old_color[0], old_color[1], old_color[2], old_color[3]);
    gl_restore_cap(GL_DEPTH_TEST, old_depth);
    gl_restore_cap(GL_TEXTURE_2D, old_texture);
    gl_restore_cap(GL_LIGHTING, old_lighting);
    gl_restore_cap(GL_CULL_FACE, old_cull);
    gl_restore_cap(GL_FOG, old_fog);
    gl_restore_cap(GL_ALPHA_TEST, old_alpha);
    gl_restore_cap(GL_SCISSOR_TEST, old_scissor);
    gl_restore_cap(GL_BLEND, old_blend);
    glMatrixMode((GLenum)old_matrix_mode);
}

/* SDL_JoystickGetDeviceInstanceID so' existe a partir da SDL 2.0.6, e o piso do
 * pacote universal e' 2.0.4: em firmware antigo o simbolo simplesmente nao esta'
 * la'. Resolvemos em tempo de execucao e, quando falta, lemos o instance id
 * abrindo o joystick (a SDL conta referencias, entao isso nao fecha um pad que
 * ja' esteja aberto como controller). O resultado e' cacheado por indice porque
 * a amostragem roda todo frame; o cache e' invalidado quando um pad entra ou
 * sai. */
#define FF4_PAD_MAX 16
static SDL_JoystickID g_pad_iid[FF4_PAD_MAX];
static char g_pad_iid_valid[FF4_PAD_MAX];

static void pad_instance_cache_flush(void) {
    memset(g_pad_iid_valid, 0, sizeof(g_pad_iid_valid));
}

static SDL_JoystickID pad_device_instance_id_raw(int idx) {
    static SDL_JoystickID (*fn)(int);
    static int probed;
    SDL_Joystick *js;
    SDL_JoystickID id;

    if (!probed) {
        probed = 1;
        *(void **)&fn = dlsym(RTLD_DEFAULT, "SDL_JoystickGetDeviceInstanceID");
    }
    if (fn) return fn(idx);
    js = SDL_JoystickOpen(idx);
    if (!js) return -1;
    id = SDL_JoystickInstanceID(js);
    SDL_JoystickClose(js);
    return id;
}

static SDL_JoystickID pad_device_instance_id(int idx) {
    if (idx < 0 || idx >= FF4_PAD_MAX) return pad_device_instance_id_raw(idx);
    if (!g_pad_iid_valid[idx]) {
        g_pad_iid[idx] = pad_device_instance_id_raw(idx);
        g_pad_iid_valid[idx] = 1;
    }
    return g_pad_iid[idx];
}

static void pad_open_index(int idx) {
    pad_instance_cache_flush();
    nxinput_pad_ordinal_fix_apply(idx, "FF4", NXINPUT_PAD_ORDINAL_LAYOUT_HID);
    if (!SDL_IsGameController(idx)) {
        fprintf(stderr, "[pad] joystick %d (%s) sem mapping — ignorado\n", idx,
                SDL_JoystickNameForIndex(idx));
        return;
    }
    SDL_JoystickID id = pad_device_instance_id(idx);
    if (id >= 0 && SDL_GameControllerFromInstanceID(id)) return;
    SDL_GameController *gc = SDL_GameControllerOpen(idx);
    fprintf(stderr, "[pad] controller %d %s: %s\n", idx, gc ? "aberto" : "FALHOU",
            gc ? SDL_GameControllerName(gc) : SDL_GetError());
}

// ===========================================================================
// AMOSTRAGEM DO PAD — espelha o MainActivity real (tabela R, mascara de estado)
// ===========================================================================
// Chamada 1x por volta do render-loop E a cada tick logico (getCurrentFrame,
// via jni_set_pad_poll) — no app real o Java atualiza `n` num thread proprio,
// entao o input fica VIVO ate dentro dos loops internos da engine (dialogo/
// batalha). Com a mascara congelada durante esses loops, um toque curto virava
// "segurado por 100 ticks" -> repeat() disparava varias vezes (o "B aperta
// sozinho varias vezes").
static int g_raw_mask = 0;          // estado cru (p/ teclado virtual)
static int g_rs_x = 0, g_rs_y = 0;  // analogico direito (cursor fallback)
static int g_r3 = 0;                // R3 cru (vkbd/cursor)
static int g_r3_prev = 0;
static int g_r3_touch_route = 0;    // rota escolhida no DOWN; segura ate o UP
static int g_start_prev = 0;
static int g_start_route = 0;       // evita abrir e fechar menu no mesmo hold
static long g_frame = 0;            // frame de render (envs de teste)
static int g_padlog = 0;

static int cursor_context_active(void) {
    int wake = SDL_GetTicks() < g_cursor_show_until;
    return cursor_on && !vkbd_is_active() &&
           (cursor_always || jni_get_back_mode() > 0 || wake);
}

static void pad_sample(void) {
    SDL_GameControllerUpdate();     // estado fresco (nao consome a fila de eventos)
    int mask = 0, sel = 0, sta = 0;
    g_r3 = 0;
    g_rs_x = 0;
    g_rs_y = 0;
    long long best_rs = -1;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(
            pad_device_instance_id(i));
        if (!gc) continue;
#define BTN(b) SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_##b)
        if (BTN(A)) mask |= K_A;
        if (BTN(B)) mask |= K_B;        // SO' 0x2 (tabela R): sem 0x8000, sem tap
        if (BTN(X)) mask |= K_X;
        if (BTN(Y)) mask |= K_Y;
        if (BTN(DPAD_RIGHT)) mask |= K_RIGHT;
        if (BTN(DPAD_LEFT)) mask |= K_LEFT;
        if (BTN(DPAD_UP)) mask |= K_UP;
        if (BTN(DPAD_DOWN)) mask |= K_DOWN;
        if (BTN(LEFTSHOULDER)) mask |= K_L;
        if (BTN(RIGHTSHOULDER)) mask |= K_R;
        if (BTN(BACK)) { mask |= K_SELECT; sel = 1; }   // SELECT = 0x4000 (tabela R)
        if (BTN(START)) sta = 1;
        if (BTN(LEFTSTICK)) mask |= K_STICK;
        if (BTN(RIGHTSTICK)) g_r3 = 1;
        // analogico esquerdo -> d-pad. Deadzone RADIAL de ~24%% (era 49%% por
        // eixo, que exigia empurrar quase ate' a metade para andar e matava a
        // diagonal). FF4 anda em grade, entao um eixo so' vence quando domina
        // o outro (histerese 5/8), evitando zig-zag na diagonal. Env de ajuste
        // FF4_STICK_DZ (fracao 0..1) para o NextOS calibrar sem rebuild.
        int axx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int axy = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        float dzf = getenv("FF4_STICK_DZ") ? (float)atof(getenv("FF4_STICK_DZ")) : 0.24f;
        if (dzf < 0.05f) dzf = 0.05f; else if (dzf > 0.9f) dzf = 0.9f;
        int dz = (int)(dzf * 32767.0f);
        long ax = axx, ay = axy;
        long amx = ax < 0 ? -ax : ax, amy = ay < 0 ? -ay : ay;
        if (amx * amx + amy * amy > (long)dz * dz) {  // fora do circulo morto
            if (amx * 8 >= amy * 5) { if (ax > 0) mask |= K_RIGHT; else mask |= K_LEFT; }
            if (amy * 8 >= amx * 5) { if (ay > 0) mask |= K_DOWN;  else mask |= K_UP; }
        }
        int rsx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        int rsy = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        long long rs_mag = (long long)rsx * rsx + (long long)rsy * rsy;
        if (rs_mag > best_rs) { g_rs_x = rsx; g_rs_y = rsy; best_rs = rs_mag; }
        // MOVEU o analogico direito -> a seta aparece e o relogio de 3s
        // reinicia. E' o unico gatilho: nada de gesto para acender.
        {
            const long long wake_dz = 8000LL * 8000LL;  /* ~24% */
            if (rs_mag > wake_dz) g_cursor_show_until = SDL_GetTicks() + CURSOR_HOLD_MS;
        }
#undef BTN
    }
    // teclado SO' sem pad (evita fantasma do gptokeyb)
    if (n == 0) {
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        if (ks[SDL_SCANCODE_RIGHT]) mask |= K_RIGHT;
        if (ks[SDL_SCANCODE_LEFT]) mask |= K_LEFT;
        if (ks[SDL_SCANCODE_UP]) mask |= K_UP;
        if (ks[SDL_SCANCODE_DOWN]) mask |= K_DOWN;
        if (ks[SDL_SCANCODE_Z] || ks[SDL_SCANCODE_RETURN]) mask |= K_A;
        if (ks[SDL_SCANCODE_X] || ks[SDL_SCANCODE_BACKSPACE]) mask |= K_B;
    }

    // Entrada remota somente para prova fisica automatizada. Formato do arquivo:
    // mask rs_x rs_y start r3 select. Sem FF4_TEST_INPUT_FILE este caminho nem
    // abre arquivo e nao participa do release/uso normal.
    const char *test_input_path = getenv("FF4_TEST_INPUT_FILE");
    if (test_input_path && test_input_path[0]) {
        FILE *test_input = fopen(test_input_path, "r");
        if (test_input) {
            int test_mask = 0, test_rs_x = 0, test_rs_y = 0;
            int test_start = 0, test_r3 = 0, test_select = 0;
            int fields = fscanf(test_input, "%i %d %d %d %d %d", &test_mask,
                                &test_rs_x, &test_rs_y, &test_start,
                                &test_r3, &test_select);
            fclose(test_input);
            if (fields >= 1) mask |= test_mask;
            if (fields >= 3) { g_rs_x = test_rs_x; g_rs_y = test_rs_y; }
            if (fields >= 4 && test_start) sta = 1;
            if (fields >= 5 && test_r3) g_r3 = 1;
            if (fields >= 6 && test_select) { sel = 1; mask |= K_SELECT; }
        }
    }

    // auto-input p/ teste headless (sem pad fisico).
    if (getenv("FF4_AUTOKEY")) {
        int bit = atoi(getenv("FF4_AUTOKEY"));
        if (bit == 0) bit = K_A;
        if ((g_frame % 24) < 6) mask |= bit;   // pulso ~1x/s
    }
    if (getenv("FF4_HOLD")) mask |= atoi(getenv("FF4_HOLD"));
    if (getenv("FF4_WALKAFTER") && g_frame > atol(getenv("FF4_WALKAFTER"))) {
        int dir = getenv("FF4_WALKDIR") ? atoi(getenv("FF4_WALKDIR")) : K_DOWN;
        if ((g_frame / 30) % 3 < 2) mask |= dir;
    }
    if (getenv("FF4_MENUAFTER") && g_frame > atol(getenv("FF4_MENUAFTER"))) {
        int b = getenv("FF4_MENUBTN") ? atoi(getenv("FF4_MENUBTN")) : K_Y;
        if ((g_frame % 60) < 4) mask |= b;
    }

    // SELECT+START = fechar
    if (sel && sta && !g_quit) {
        fprintf(stderr, "[ff4] SELECT+START -> saindo\n");
        g_quit = 1;
    }

    // O APK consome KEYCODE_BUTTON_START sem bit. No port, START e' o botao
    // ergonomico de menu: fora dele envia MENU/Y (0x400); dentro envia o mesmo
    // BACK Android escolhido por assignBackButton. A rota e' travada no DOWN
    // para um START segurado nao abrir e fechar o menu no mesmo gesto.
    int back_mode = jni_get_back_mode();
    if (sta && !g_start_prev) {
        if (sel) g_start_route = 0;
        else if (back_mode == 0) g_start_route = K_Y;
        else if (back_mode == 1) g_start_route = K_BACK;
        else g_start_route = K_SELECT;
        if (g_padlog && g_start_route)
            fprintf(stderr, "[pad] START route=0x%x back_mode=%d\n",
                    g_start_route, back_mode);
    }
    if (!sta) g_start_route = 0;
    if (sta && !sel) mask |= g_start_route;
    g_start_prev = sta;

    // R3 tambem trava sua rota no DOWN. Em UI ele clica no cursor; no gameplay
    // conserva o bit nativo K_STICK. O analogico direito nunca move o cursor no
    // gameplay (o APK original tambem nao mapeia seus eixos).
    if (g_r3 && !g_r3_prev) {
        // A seta so' aparece por MOVIMENTO do analogico direito; enquanto ela
        // estiver viva, o R3 CLICA nela (e o toque que abre o teclado na tela
        // de nome). Sem seta na tela, o R3 conserva o bit nativo do jogo.
        g_r3_touch_route = cursor_context_active();
    }
    if (!g_r3) g_r3_touch_route = 0;
    if (g_r3 && !g_r3_touch_route && !vkbd_is_active()) mask |= K_STICK;
    g_r3_prev = g_r3;

    g_raw_mask = mask;
    int game_mask = vkbd_is_active() ? 0 : mask;  // digitando: nada vai pro jogo
    static int logged_mask = -1, logged_back = -1;
    if (g_padlog && (game_mask != logged_mask || back_mode != logged_back)) {
        fprintf(stderr, "[pad] mask=0x%x back_mode=%d cursor=%d\n",
                game_mask, back_mode, cursor_context_active());
        logged_mask = game_mask;
        logged_back = back_mode;
    }
    jni_set_keystate(game_mask);
}

// ===========================================================================
// TOUCH NATIVO — exatamente uma chamada antes de cada render, como onDrawFrame
// ===========================================================================
// MainActivity passa (touchCount, touchPeak/previousCount). No DOWN: (1,1),
// segurando: (1,1), no UP: (0,1), e no quadro seguinte: (0,0). Mesmo sem toque,
// touch(0,0,...) e' obrigatorio: ele limpa cont/contF/trg para o render seguinte
// reconstruir pad/edge/repeat/release. O port antigo omitia esta etapa e escrevia
// nesses globais por fora, quebrando justamente o fluxo dos menus.
static int g_touch_previous = 0;
static float g_touch_x = 0.5f, g_touch_y = 0.5f;
static int g_touch_tap_pending = 0;
static float g_touch_tap_x = 0.5f, g_touch_tap_y = 0.5f;

static void touch_queue_tap(float x, float y) {
    g_touch_tap_x = x;
    g_touch_tap_y = y;
    g_touch_tap_pending = 1;
}

static void touch_pump(void *env, void *thiz, int cursor_pressed,
                       int test_pressed, float test_x, float test_y) {
    int current = 0;
    float x = g_touch_x, y = g_touch_y;
    if (g_touch_tap_pending) {
        current = 1;
        x = g_touch_tap_x;
        y = g_touch_tap_y;
        g_touch_tap_pending = 0;  // quadro seguinte produz automaticamente o UP
    } else if (test_pressed) {
        current = 1;
        x = test_x;
        y = test_y;
    } else if (cursor_pressed) {
        current = 1;
        x = cur_x;
        y = cur_y;
    }
    if (current) { g_touch_x = x; g_touch_y = y; }
    int previous_or_peak = g_touch_previous;
    if (current > previous_or_peak) previous_or_peak = current;
    ff4_touch(env, thiz, current, previous_or_peak,
              g_touch_x, g_touch_y, 0.0f, 0.0f);
    g_touch_previous = current;
}

static SDL_Window *g_repair_window;
static SDL_GLContext g_repair_context;

/* Teardown para o reparo de provedor: o framework recusa o re-exec sem a
 * atestacao de que a pilha grafica que falhou foi realmente fechada. */
static void ff4_video_teardown(void) {
    if (g_repair_context) { SDL_GL_DeleteContext(g_repair_context); g_repair_context = NULL; }
    if (g_repair_window) { SDL_DestroyWindow(g_repair_window); g_repair_window = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(int argc, char **argv) {
    (void)argc;
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const char *gamedir = getenv("FF4_GAMEDIR");
    if (!gamedir) gamedir = ".";
    char path[1024];

    // 1. VFS primeiro (gate real, igual ao Android: o OBB precede o init).
    snprintf(path, sizeof path, "%s/data/main.obb", gamedir);
    if (game_vfs_open(path) != 0) {
        fprintf(stderr, "[ff4] sem main.obb — abortando\n");
        return 1;
    }

    // 2. Janela + GLES1. Video/audio vem do sistema: NUNCA forcar SDL_*DRIVER.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[ff4] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    int win_w = dm.w > 0 ? dm.w : 1280;
    int win_h = dm.h > 0 ? dm.h : 720;
    if (getenv("FF4_W")) win_w = atoi(getenv("FF4_W"));
    if (getenv("FF4_H")) win_h = atoi(getenv("FF4_H"));

    SDL_Window *win = NULL;
    int window_attempt;
    for (window_attempt = 0; window_attempt < 2 && win == NULL;
         window_attempt++) {
        win = SDL_CreateWindow("Final Fantasy IV", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, win_w, win_h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (win == NULL && window_attempt == 0) {
            /* 0.2.12: processo re-executado com par aplicado que falhou na
             * TENTATIVA REAL -- desamarrar e repetir pela pilha da firmware,
             * para o erro final ser o verdadeiro. */
            nxgl_provider_repair_receipt rollback_receipt;
            fprintf(stderr, "[ff4] SDL_CreateWindow: %s\n", SDL_GetError());
            if (nxgl_provider_precontext_rollback(&rollback_receipt)) {
                fprintf(stderr, "[ff4] %s\n", rollback_receipt.text);
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                    break;
                }
                continue;
            }
        }
    }
    if (!win) {
        fprintf(stderr, "[ff4] SDL_CreateWindow: %s\n", SDL_GetError());
        {
        /* A janela nem chegou a existir: nao ha renderer para medir. Este e' o
         * outro sintoma do provedor cruzado, e o framework tem um plano
         * proprio para ele -- mais estreito, porque aqui nao ha criterio vivo
         * possivel. Se nao houver reparo autorizado, o erro segue como antes. */
    static const char *const k_gles[] = {"glOrthof", "glDrawArrays",
                                         "glTexImage2D", "glClear"};
        nxgl_provider_repair_options repair;
        nxgl_provider_repair_receipt repair_receipt;

        nxgl_provider_repair_options_init(&repair);
        repair.video_backend = SDL_GetCurrentVideoDriver();
        repair.required_gles_symbols = k_gles;
        repair.required_gles_symbol_count = sizeof k_gles / sizeof k_gles[0];
        repair.teardown = ff4_video_teardown;
        repair.argv = argv;
        nxgl_provider_repair_precontext(&repair, NXGL_OPEN_STAGE_V2_WINDOW_CREATE,
                                        NXGL_OPEN_REASON_V2_WINDOW_FAILED,
                                        &repair_receipt);
        fprintf(stderr, "[ff4] %s\n", repair_receipt.text);
    }
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "[ff4] SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_MakeCurrent(win, ctx);

    /* GLES1 resolvido com o contexto ja' corrente. O binario NAO declara
     * DT_NEEDED libGLESv1_CM.so: esse SONAME nao existe em toda CFW (no
     * dArkOS/ArkOS do R36S, Mali-Bifrost G31, so' ha' o blob libmali.so, que
     * define os 45 pontos de entrada). Ver framework/nxgl. */
    {
        nxgl_gles1_receipt gles1;
        /* 0.2.14: a fonte primaria e' o proprio contexto que a SDL acabou de
         * criar -- no ROCKNIX o dlopen por nome achava o blob kbase ORFAO
         * (libmali.so) e o jogo inteiro virava no-op (tela preta com som).
         * O resolvedor prova cada candidato com glGetString antes de aceitar. */
        nxgl_gles1_set_primary_resolver(
            (nxgl_gles1_resolver_fn)SDL_GL_GetProcAddress);
        if (nxgl_gles1_init(&gles1) != 0) {
            fprintf(stderr, "[ff4] %s\n", gles1.text);
            return 1;
        }
        fprintf(stderr, "[ff4] %s\n", gles1.text);
    }

    /* Prova de imagem medida por dentro do contexto. Um port que nao desenha
     * e' indistinguivel de um saudavel em todo sinal que o launcher enxerga:
     * o laco roda, o audio toca, o input chega e o processo sai 0. O resolvedor
     * do nxgl e' instalado como fonte de simbolos porque, num port so-loader,
     * o glReadPixels real nao existe em mais lugar nenhum. */
    nxgl_frame_proof_set_resolver(nxgl_gles1_lookup);
    nxgl_frame_proof_launch_receipt();
    if (SDL_GL_SetSwapInterval(1) != 0) SDL_GL_SetSwapInterval(0);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_GL_GetDrawableSize(win, &win_w, &win_h);

    const char *(*glGetString_p)(unsigned) = (void *)SDL_GL_GetProcAddress("glGetString");
    const char *gl_renderer = glGetString_p ? glGetString_p(0x1F01) : NULL;
    if (glGetString_p)
        fprintf(stderr, "[ff4] GL_RENDERER=%s  GL_VERSION=%s\n",
                gl_renderer, glGetString_p(0x1F02));

    /* Renderer vazio aqui nao e' curiosidade: e' a assinatura do SONAME
     * cruzado, onde o contexto aceita tudo e nao desenha nada. O framework
     * decide o que fazer -- e nao faz nada quando o renderer esta' saudavel,
     * que e' o caso deste aparelho e da maioria. */
    {
        static const char *const k_gles[] = {"glOrthof", "glDrawArrays",
                                             "glTexImage2D", "glClear"};
        nxgl_provider_repair_options repair;
        nxgl_provider_repair_receipt repair_receipt;

        /* O guardiao mede o renderer PELO CAMINHO DA ENGINE (o resolvedor do
         * nxgl), nao pelo da SDL: no ROCKNIX a SDL respondia "Panfrost"
         * saudavel enquanto a engine desenhava por um provedor morto, e o
         * reparo nunca disparava. */
        const char *engine_renderer =
            nxgl_gles1_pfn_glGetString != NULL
                ? (const char *)nxgl_gles1_pfn_glGetString(0x1F01)
                : NULL;
        g_repair_window = win;
        g_repair_context = ctx;
        nxgl_provider_repair_options_init(&repair);
        repair.renderer = engine_renderer;
        repair.video_backend = SDL_GetCurrentVideoDriver();
        repair.window_opened = 1;
        repair.context_current = 1;
        repair.drawable_positive = (win_w > 0 && win_h > 0);
        repair.required_gles_symbols = k_gles;
        repair.required_gles_symbol_count = sizeof k_gles / sizeof k_gles[0];
        repair.teardown = ff4_video_teardown;
        repair.argv = argv;
        nxgl_provider_repair_if_renderer_broken(&repair, &repair_receipt);
        fprintf(stderr, "[ff4] %s\n", repair_receipt.text);

        /* Recibo VIDEO completo: sem isto a linha final saia window=? driver=?
         * renderer=? -- inutil como primeira leitura de um relato de campo. */
        nxgl_frame_proof_set_video_context(
            win_w, win_h, SDL_GetCurrentVideoDriver(), engine_renderer,
            nxgl_gles1_pfn_glGetString != NULL
                ? (const char *)nxgl_gles1_pfn_glGetString(0x1F02)
                : NULL);
    }
    fprintf(stderr, "[ff4] VIDEO ENV: SDL_VIDEODRIVER=%s SDL_VIDEO_EGL_DRIVER=%s SDL_VIDEO_GL_DRIVER=%s reparo-aplicado=%s\n",
            getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "-",
            getenv("SDL_VIDEO_EGL_DRIVER") ? getenv("SDL_VIDEO_EGL_DRIVER") : "-",
            getenv("SDL_VIDEO_GL_DRIVER") ? getenv("SDL_VIDEO_GL_DRIVER") : "-",
            getenv("NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED") ? "sim" : "nao");
    fprintf(stderr, "[ff4] janela %dx%d\n", win_w, win_h);

    // 3. Shim JNI ANTES de carregar (os ctors ja chamam de volta).
    jni_shim_init();
    game_init_language();
    jni_set_screen(win_w, win_h);
    vkbd_init(getenv("FF4_FONT"));  // teclado virtual (atlas de fonte, ctx GL ativo)
    if (getenv("FF4_VKBD_DEMO")) vkbd_activate("Cecil", 5);  // teste: mostra o teclado no boot

    // 4. Carrega o libff4 (bypass do proxy).
    so_module_t mod;
    snprintf(path, sizeof path, "%s/libff4.so", gamedir);
    if (so_load(&mod, path, g_ff4_imports, g_ff4_imports_count) != 0) {
        fprintf(stderr, "[ff4] falha carregando libff4.so\n");
        return 1;
    }
    so_run_init(&mod);

    ff4_render = (ff4_render_t)so_symbol(&mod, "render");
    ff4_touch = (ff4_touch_t)so_symbol(&mod, "touch");
    ff4_resume = (ff4_void_t)so_symbol(&mod, "resume");
    ff4_pause = (ff4_void_t)so_symbol(&mod, "pause");
    ff4_resumeFont = (ff4_void_t)so_symbol(&mod, "resumeFont");
    // CPad fica integralmente nativo. Hooks antigos de edge/read foram removidos:
    // a chamada touch() na ordem Android ja preserva decide/repeat/release.
    fprintf(stderr, "[ff4] CPad nativo (sem hooks)\n");
    if (!ff4_render || !ff4_touch || !ff4_resume) {
        fprintf(stderr, "[ff4] simbolos render/touch/resume nao encontrados\n");
        return 1;
    }
    fprintf(stderr, "[ff4] render=%p resume=%p touch=%p resumeFont=%p\n",
            (void *)ff4_render, (void *)ff4_resume, (void *)ff4_touch,
            (void *)ff4_resumeFont);

    void *env = jni_shim_env();
    void *thiz = jni_shim_activity();

    // 5. Boot: resume inicializa GL/engine; resumeFont carrega as texturas de fonte.
    fprintf(stderr, "[ff4] resume()...\n");
    ff4_resume(env, thiz);
    if (ff4_resumeFont) ff4_resumeFont(env, thiz);

    for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open_index(i);
    g_padlog = getenv("FF4_PADLOG") ? atoi(getenv("FF4_PADLOG")) : 0;
    // Cursor habilitado por padrao, mas estritamente contextual: so aparece em
    // UI que chamou assignBackButton. FF4_CURSOR_ALWAYS=1 e' apenas diagnostico.
    cursor_on = getenv("FF4_CURSOR") ? atoi(getenv("FF4_CURSOR")) : 1;
    cursor_always = getenv("FF4_CURSOR_ALWAYS") ? atoi(getenv("FF4_CURSOR_ALWAYS")) : 0;
    fprintf(stderr, "[ff4] input nativo; cursor fallback=%s (%s)\n",
            cursor_on ? "on" : "off", cursor_always ? "sempre/diagnostico" : "contextual");
    long frames = getenv("FF4_FRAMES") ? atol(getenv("FF4_FRAMES")) : 0;
    // input vivo por tick logico (como o Java async do app real): getCurrentFrame
    // re-amostra o pad ate dentro dos loops internos da engine.
    jni_set_pad_poll(pad_sample);

    // 6. Loop = onDrawFrame. Ordem original: input -> touch -> render -> swap.
    Uint32 cur_vis_until = 0;   // cursor visivel ate este tick (some sozinho)
    unsigned kbd_prev = 0;      // estado anterior p/ edges do teclado virtual
    int previous_cursor_context = 0;
    Uint64 cursor_clock = SDL_GetPerformanceCounter();
    Uint64 cursor_frequency = SDL_GetPerformanceFrequency();

    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: g_quit = 1; break;
            case SDL_CONTROLLERDEVICEADDED: pad_open_index(e.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED: {
                SDL_GameController *gc = SDL_GameControllerFromInstanceID(e.cdevice.which);
                if (gc) SDL_GameControllerClose(gc);
                pad_instance_cache_flush();
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN:
                if (g_padlog)
                    fprintf(stderr, "[pad] BTN DOWN idx=%d (%s)\n", e.cbutton.button,
                            SDL_GameControllerGetStringForButton(e.cbutton.button));
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) g_quit = 1;
                break;
            default: break;
            }
        }

        pad_sample();   // mascara K da tabela R; touch() cuidara dos acumuladores

        Uint64 new_cursor_clock = SDL_GetPerformanceCounter();
        float cursor_dt = cursor_frequency
            ? (float)(new_cursor_clock - cursor_clock) / (float)cursor_frequency : 0.0f;
        cursor_clock = new_cursor_clock;
        if (cursor_dt < 0.0f) cursor_dt = 0.0f;
        if (cursor_dt > 0.05f) cursor_dt = 0.05f;

        // --- TECLADO VIRTUAL: quando ativo (tela de nome), consome d-pad/A/R3/B
        // (pad_sample ja mandou mask=0 pro jogo) e digita. OK toca o Accept. ---
        if (vkbd_is_active()) {
            unsigned km = (unsigned)g_raw_mask | (g_r3 ? 0x40000000u : 0);
            unsigned ke = km & ~kbd_prev;  // edges
            // SELECT: saida de emergencia da tela de nome. Fecha o teclado
            // e injeta BACK ao jogo, devolvendo o controle -- nunca mais
            // 'tela sem saida' se algo mais falhar.
            int ok = vkbd_handle(ke & K_LEFT, ke & K_RIGHT, ke & K_UP, ke & K_DOWN,
                                 (ke & K_A) || (ke & 0x40000000u), ke & K_B);
            // SELECT confirma pelo MESMO caminho do OK -- atalho e rede de
            // seguranca: o jogador nunca fica preso na tela de nome.
            if (ke & K_SELECT) { ok = 1; fprintf(stderr, "[edit] SELECT confirma o nome\n"); }
            kbd_prev = km;
            if (ok) {  // tap no botao Accept, mapeado pela VIEWPORT letterboxed
                // (getView*), nunca pela janela crua: em 4:3 (34XX-SP) o
                // 0.5/0.62 da janela erra o botao. FF4_ACCEPTX/Y (escape)
                // agora e' relativo a viewport, nao a janela.
                int vx, vy, vw, vh;
                jni_get_view(&vx, &vy, &vw, &vh);
                float rx = getenv("FF4_ACCEPTX") ? atof(getenv("FF4_ACCEPTX")) : 0.5f;
                float ry = getenv("FF4_ACCEPTY") ? atof(getenv("FF4_ACCEPTY")) : 0.62f;
                float ax = (win_w > 0) ? (vx + rx * vw) / (float)win_w : rx;
                float ay = (win_h > 0) ? (vy + ry * vh) / (float)win_h : ry;
                touch_queue_tap(ax, ay);
            }
        } else {
            kbd_prev = 0;
        }

        // --- CURSOR fallback contextual: deadzone radial, curva progressiva,
        // velocidade em pixels/s e suavizacao exponencial independente do FPS. ---
        int cursor_context = cursor_context_active();
        Uint32 ticks_now = SDL_GetTicks();
        if (cursor_context && !previous_cursor_context)
            cur_vis_until = ticks_now + CURSOR_HOLD_MS;
        previous_cursor_context = cursor_context;

        if (cursor_context) {
            float rx = g_rs_x / 32767.0f, ry = g_rs_y / 32767.0f;
            if (rx < -1.0f) rx = -1.0f; else if (rx > 1.0f) rx = 1.0f;
            if (ry < -1.0f) ry = -1.0f; else if (ry > 1.0f) ry = 1.0f;
            float magnitude = sqrtf(rx * rx + ry * ry);
            const float deadzone = 0.22f;
            float target_vx = 0.0f, target_vy = 0.0f;
            if (magnitude > deadzone) {
                float response = (magnitude - deadzone) / (1.0f - deadzone);
                if (response > 1.0f) response = 1.0f;
                response = response * response * (3.0f - 2.0f * response);
                float speed_px = 760.0f * (win_h / 720.0f);
                target_vx = (rx / magnitude) * response * speed_px / win_w;
                target_vy = (ry / magnitude) * response * speed_px / win_h;
                cur_vis_until = ticks_now + CURSOR_HOLD_MS;
                g_cursor_show_until = ticks_now + CURSOR_HOLD_MS;
            }
            float smoothing = 1.0f - expf(-14.0f * cursor_dt);
            cur_vx += (target_vx - cur_vx) * smoothing;
            cur_vy += (target_vy - cur_vy) * smoothing;
            cur_x += cur_vx * cursor_dt;
            cur_y += cur_vy * cursor_dt;
            if (cur_x < 0.0f) { cur_x = 0.0f; cur_vx = 0.0f; }
            else if (cur_x > 1.0f) { cur_x = 1.0f; cur_vx = 0.0f; }
            if (cur_y < 0.0f) { cur_y = 0.0f; cur_vy = 0.0f; }
            else if (cur_y > 1.0f) { cur_y = 1.0f; cur_vy = 0.0f; }
            // clicar tambem conta como uso: renova os 3s.
            if (g_r3 && g_r3_touch_route) {
                cur_vis_until = ticks_now + CURSOR_HOLD_MS;
                g_cursor_show_until = ticks_now + CURSOR_HOLD_MS;
            }
        } else {
            cur_vx = cur_vy = 0.0f;
        }

        int cursor_pressed = cursor_context && g_r3 && g_r3_touch_route;
        int test_pressed = 0;
        float test_x = 0.5f, test_y = 0.5f;
        // Teste opt-in: seis quadros DOWN a cada quarenta; o pump gera o UP.
        if (getenv("FF4_TAPXY")) {
            sscanf(getenv("FF4_TAPXY"), "%f,%f", &test_x, &test_y);
            test_pressed = (g_frame % 40) < 6;
        }

        // Etapa que faltava no port: sempre uma chamada, inclusive (0,0).
        touch_pump(env, thiz, cursor_pressed, test_pressed, test_x, test_y);

        // render(cutout=0) — sem notch no fbdev.
        ff4_render(env, thiz, 0);
        // Cursor e' overlay de fallback; A/B/d-pad continuam 100% nativos.
        // Uma regra so': a seta vive enquanto o relogio de 3s do ultimo uso
        // (movimento ou clique) nao expirar.
        if (cursor_context && SDL_GetTicks() < cur_vis_until)
            cursor_draw(win_w, win_h, cur_x * win_w, cur_y * win_h, cursor_pressed);
        vkbd_render(win_w, win_h);  // teclado virtual por cima (so' quando ativo)

        // Prova de imagem CONTINUA, ANTES do present (pos-swap o backbuffer
        // e' indefinido em Mali tile-based). O adapter amostra no cronograma,
        // publica o veredito e grita IMAGE PROOF se o painel ficar preto com
        // o jogo vivo.
        nxgl_frame_proof_before_present(win_w, win_h);
        // 1 SwapWindow POR FRAME (fb0 Mali = 2 metades).
        SDL_GL_SwapWindow(win);
        g_frame++;
        if (g_frame < 5 || (g_frame % 300) == 0) fprintf(stderr, "[ff4] frame %ld\n", g_frame);
        if (frames > 0 && g_frame >= frames) g_quit = 1;

        // medicao de fps a cada 5s
        {
            static Uint32 last = 0; static long lf = 0;
            Uint32 now = SDL_GetTicks();
            if (!last) last = now;
            if (now - last >= 5000) {
                fprintf(stderr, "[fps] %.1f (frame %ld)\n",
                        (g_frame - lf) * 1000.0 / (now - last), g_frame);
                last = now; lf = g_frame;
            }
        }
    }

    fprintf(stderr, "[ff4] saindo (frames=%ld)\n", g_frame);
    if (ff4_pause) ff4_pause(env, thiz);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
