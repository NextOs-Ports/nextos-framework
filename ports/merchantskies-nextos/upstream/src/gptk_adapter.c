/* Adapter GPTK do Merchant of the Skies.
 *
 * Fecha a cadeia arquivo -> parser -> adapter -> acao real no jogo:
 *   1. le a copia editavel NEXTOSCONTROLLERS.gptk do GAMEDIR;
 *   2. parseia com o parser CANONICO nxinput_gptk (framework);
 *   3. valida cada acao contra adapter-contract.input.actions (allowlist);
 *   4. serve, por (contexto, controle fisico), o keycode Android REAL que o
 *      Rewired do jogo interpreta -- a TABELA acao -> sink abaixo.
 *
 * Autoridade unica: um controle que o GPTK governa (tem acao de botao mapeada
 * no contexto ativo) e servido SO por aqui; o caminho legado do input.c pula
 * esse controle. Controles nativos (analogicos, cursor, pause) ficam com a
 * autoridade nativa e nao sao remapeados.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxinput_gptk.h"

extern void nx_log(const char *fmt, ...);

/* Allowlist = exatamente adapter-contract.input.actions do Merchant. Uma acao
 * fora desta lista faz o parse falhar fechado (NXI1001). */
static const char *const MS_ACTIONS[] = {
    "ms.confirm", "ms.back", "ms.click", "ms.pause",
    "ms.move", "cursor.move", "cursor.recenter",
};

/* TABELA acao -> sink REAL do Merchant (keycode Android que o Rewired le).
 * Confirmar/voltar/clicar sao botoes digitais com keycode direto; pausa e
 * vetores ficam com a autoridade nativa (retornam 0 = "GPTK nao governa"). */
static int ms_action_keycode(const char *action)
{
    if (!action || !action[0])
        return 0;
    if (strcmp(action, "ms.confirm") == 0)
        return 96;  /* KEYCODE_BUTTON_A -- aceitar/confirmar */
    if (strcmp(action, "ms.back") == 0)
        return 97;  /* KEYCODE_BUTTON_B -- voltar/cancelar */
    if (strcmp(action, "ms.click") == 0)
        return 96;  /* clique do cursor entra como confirmar */
    /* ms.pause (START), ms.move/cursor.* (vetores) -> autoridade nativa. */
    return 0;
}

/* SDL_CONTROLLER_BUTTON_* -> nxinput_gptk_control. -1 para o que o GPTK nao
 * enderaca como botao digital (o SDL enum vem de SDL2/SDL.h no input.c; aqui
 * usamos os valores estaveis para manter este arquivo desacoplado). */
static int sdl_to_gptk_control(int sdl_button)
{
    switch (sdl_button) {
    case 0:  return NXINPUT_GPTK_A;      /* SDL_CONTROLLER_BUTTON_A */
    case 1:  return NXINPUT_GPTK_B;      /* B */
    case 2:  return NXINPUT_GPTK_X;      /* X */
    case 3:  return NXINPUT_GPTK_Y;      /* Y */
    case 9:  return NXINPUT_GPTK_L1;     /* LEFTSHOULDER */
    case 10: return NXINPUT_GPTK_R1;     /* RIGHTSHOULDER */
    default: return -1;
    }
}

static nxinput_gptk g_gptk;
static int g_gptk_ready;   /* 1 quando ha um arquivo valido carregado */
static int g_gptk_disabled;

/* Carrega e valida o gptk uma vez. Silencioso e fail-safe: qualquer problema
 * (arquivo ausente, magic errada, acao desconhecida) deixa g_gptk_ready=0 e o
 * input.c segue 100%% pelo caminho legado -- nunca pior que antes. */
void ms_gptk_load(const char *gamedir)
{
    static int done;
    char path[4096];
    char *text;
    long n;
    FILE *f;
    char err[128];

    if (done)
        return;
    done = 1;
    if (getenv("MS_NO_GPTK")) {
        g_gptk_disabled = 1;
        nx_log("gptk: desligado por MS_NO_GPTK; input pelo caminho nativo");
        return;
    }
    if (!gamedir || !gamedir[0])
        gamedir = ".";
    (void)snprintf(path, sizeof(path), "%s/NEXTOSCONTROLLERS.gptk", gamedir);
    f = fopen(path, "rb");
    if (!f) {
        nx_log("gptk: %s ausente; input nativo", path);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        n > (long)NXINPUT_GPTK_MAX_BYTES) {
        fclose(f);
        nx_log("gptk: tamanho invalido; input nativo");
        return;
    }
    rewind(f);
    text = malloc((size_t)n + 1u);
    if (!text) {
        fclose(f);
        return;
    }
    if (fread(text, 1u, (size_t)n, f) != (size_t)n) {
        free(text);
        fclose(f);
        return;
    }
    text[n] = '\0';
    fclose(f);

    if (nxinput_gptk_parse(text, (size_t)n, &g_gptk, err, sizeof(err)) != 0) {
        nx_log("gptk: parse falhou (%s); input nativo", err);
        free(text);
        return;
    }
    free(text);
    if (nxinput_gptk_validate_actions(&g_gptk, MS_ACTIONS,
                                      sizeof(MS_ACTIONS) / sizeof(MS_ACTIONS[0]),
                                      err, sizeof(err)) != 0) {
        nx_log("gptk: acao fora do contrato (%s); input nativo", err);
        return;
    }
    g_gptk_ready = 1;
    nx_log("gptk: %s carregado e validado; GPTK governa os botoes mapeados",
           path);
}

/* Keycode que o botao fisico `sdl_button` deve entregar no contexto
 * `gameplay` (1) ou menu (0), segundo o gptk. Retorna 0 quando o GPTK NAO
 * governa esse controle (o input.c mantem a autoridade nativa/legada). */
int ms_gptk_button_keycode(int sdl_button, int gameplay)
{
    int control, keycode;
    const char *action;
    nxinput_gptk_context ctx;

    if (!g_gptk_ready || g_gptk_disabled)
        return 0;
    control = sdl_to_gptk_control(sdl_button);
    if (control < 0)
        return 0;
    ctx = gameplay ? NXINPUT_GPTK_CONTEXT_GAMEPLAY
                   : NXINPUT_GPTK_CONTEXT_MENU;
    if (!g_gptk.context_present[ctx])
        return 0;
    action = nxinput_gptk_action(&g_gptk, ctx, control);
    keycode = ms_action_keycode(action);
    return keycode;  /* 0 quando a acao mapeada nao tem sink de botao */
}

int ms_gptk_active(void)
{
    return g_gptk_ready && !g_gptk_disabled;
}
