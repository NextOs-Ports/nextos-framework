/* SPDX-License-Identifier: GPL-3.0-only */
/* NAMELESSCAT-CONTROLS-LIVE 1.2.6 — casos dirigidos do runtime vivo.
 *
 * Cada cenário roda num PROCESSO próprio (o módulo é one-shot por processo,
 * como no jogo).  Nenhum SDL, nenhum teclado, nenhum evdev: só o glue do port
 * (src/input_gptk.c) + o nxinput 0.10.1 vendorizado, com sinks de teste que
 * registram cada entrega.
 *
 *   uso: test_gptk_live <cenário> <workdir> <default.gptk>
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "input_gptk.h"
#include "nxinput_gptk.h"

static int failures;
#define CHECK(cond, name) do { \
    if (cond) { printf("ok    %s\n", (name)); } \
    else { printf("FALHA %s (%s:%d)\n", (name), __FILE__, __LINE__); failures++; } \
} while (0)

/* ---- sinks de teste: contam press/release e vetores por ação ---- */
typedef struct { int press, release; float last_value; } counter;
static counter jump, interact, down, accept, pause_, quit, click;
static int move_calls, cursor_calls;
static float move_x, move_y, cursor_x, cursor_y;
static int ack_fail_action; /* 1: o sink de jump recusa o ACK */

static int sink_button(void *user, const char *action, int pressed, float v)
{
    (void)action;
    counter *c = user;
    if (pressed) c->press++; else c->release++;
    c->last_value = v;
    if (ack_fail_action && c == &jump)
        return -1;
    return 0;
}

static int sink_move(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    move_calls++; move_x = x; move_y = y;
    return 0;
}

static int sink_cursor(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    cursor_calls++; cursor_x = x; cursor_y = y;
    return 0;
}

static int register_all(int skip_jump)
{
    int rc = 0;
    if (!skip_jump)
        rc |= nc_gptk_register_button("player.jump", "engine.input.jump", sink_button, &jump);
    rc |= nc_gptk_register_button("player.interact", "engine.input.interact", sink_button, &interact);
    rc |= nc_gptk_register_button("player.down", "engine.input.down", sink_button, &down);
    rc |= nc_gptk_register_button("menu.accept", "engine.ui_accept", sink_button, &accept);
    rc |= nc_gptk_register_button("system.pause", "engine.input.pause", sink_button, &pause_);
    rc |= nc_gptk_register_button("system.quit", "adapter.system.quit", sink_button, &quit);
    rc |= nc_gptk_register_button("cursor.click", "adapter.pointer.click", sink_button, &click);
    rc |= nc_gptk_register_vector("player.move", "engine.input.move", sink_move, NULL);
    rc |= nc_gptk_register_vector("cursor.move", "adapter.pointer.move", sink_cursor, NULL);
    if (rc)
        return rc;
    return nc_gptk_seal();
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

/* Cópia do dono derivada do default: troca linhas dentro de UMA seção. */
static char *owner_with(const char *def, const char *section,
                        const char *const *pairs, size_t npairs)
{
    size_t cap = strlen(def) + 256;
    char *out = calloc(cap, 1);
    const char *p = def;
    int in_section = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        char line[256];
        size_t copy = line_len < sizeof line - 1 ? line_len : sizeof line - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';
        if (line[0] == '[')
            in_section = strncmp(line + 1, section, strlen(section)) == 0 &&
                         line[1 + strlen(section)] == ']';
        int replaced = 0;
        if (in_section) {
            for (size_t i = 0; i < npairs; i++) {
                size_t klen = strlen(pairs[2 * i]);
                if (strncmp(line, pairs[2 * i], klen) == 0 &&
                    line[klen] == ' ' && line[klen + 1] == '=') {
                    strcat(out, pairs[2 * i]);
                    strcat(out, " = ");
                    strcat(out, pairs[2 * i + 1]);
                    strcat(out, "\n");
                    replaced = 1;
                    break;
                }
            }
        }
        if (!replaced)
            strcat(out, line);
        p += line_len;
    }
    return out;
}

static void boot(const char *workdir, const char *def_text,
                 const char *owner_text)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/defaults", workdir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/defaults/NEXTOSCONTROLLERS.gptk", workdir);
    write_file(path, def_text);
    if (owner_text) {
        snprintf(path, sizeof path, "%s/NEXTOSCONTROLLERS.gptk", workdir);
        write_file(path, owner_text);
    }
    CHECK(nc_gptk_preinit(workdir) == 0, "preinit roda uma vez");
}

/* Um quadro: alimenta um botão com o estado físico corrente. */
static int frame_button(int control, int down)
{
    return nc_gptk_feed_button(control, down, down ? 1.0f : 0.0f);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "uso: %s <cenario> <workdir> <default.gptk>\n", argv[0]);
        return 2;
    }
    const char *scenario = argv[1];
    const char *workdir = argv[2];
    size_t def_len = 0;
    char *def = read_file(argv[3], &def_len);
    if (!def) { perror(argv[3]); return 2; }
    CHECK(strstr(def, "format = NEXTOS_CONTROLLERS/3") != NULL,
          "default gerado é NEXTOS_CONTROLLERS/3");
    CHECK(strstr(def, "FACE_LAYOUT = auto") != NULL,
          "default gerado declara FACE_LAYOUT = auto");
    CHECK(strstr(def, "[cursor]") == NULL,
          "default não tem contexto cursor (cursor vive dentro de [menu])");

    if (!strcmp(scenario, "default")) {
        /* 1: default fiel — A pula no gameplay, A confirma no menu, R3 clica
         * no menu, START pausa nos dois, B interage, Y agacha; UMA entrega
         * por press e UMA por release. */
        boot(workdir, def, NULL);
        CHECK(nc_gptk_loaded(), "mapa carregado");
        CHECK(nc_gptk_schema() == 3, "schema 3 selecionado");
        CHECK(!strcmp(nc_gptk_source_name(), "default_owner_missing"),
              "sem cópia do dono: default imutável selecionado");
        CHECK(register_all(0) == 0, "sinks registrados e selados");
        CHECK(nc_gptk_context() == -1, "contexto nasce UNPROVEN");
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_PASSTHROUGH,
              "sem contexto provado, A é PASSTHROUGH");
        CHECK(jump.press == 0, "…e o sink de pulo NÃO foi chamado");
        frame_button(NXINPUT_GPTK_A, 0);
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        CHECK(nc_gptk_context() == NC_GPTK_CONTEXT_GAMEPLAY, "gameplay provado");
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_DELIVERED, "A press entregue");
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_DELIVERED, "A segurado: sem nova borda");
        CHECK(jump.press == 1 && jump.release == 0, "pulo: exatamente 1 press");
        CHECK(frame_button(NXINPUT_GPTK_A, 0) == NC_GPTK_LIVE_DELIVERED, "A release entregue");
        CHECK(jump.press == 1 && jump.release == 1, "pulo: exatamente 1 release");
        CHECK(accept.press == 0, "no gameplay A não vira menu.accept");
        CHECK(click.press == 0, "no gameplay A não vira clique");
        frame_button(NXINPUT_GPTK_B, 1); frame_button(NXINPUT_GPTK_B, 0);
        CHECK(interact.press == 1 && interact.release == 1, "B = player.interact (1/1)");
        frame_button(NXINPUT_GPTK_Y, 1); frame_button(NXINPUT_GPTK_Y, 0);
        CHECK(down.press == 1 && down.release == 1, "Y = player.down (1/1)");
        frame_button(NXINPUT_GPTK_START, 1); frame_button(NXINPUT_GPTK_START, 0);
        CHECK(pause_.press == 1 && pause_.release == 1, "START = system.pause (1/1)");
        CHECK(frame_button(NXINPUT_GPTK_L2, 1) == NC_GPTK_LIVE_SUPPRESSED, "L2 = null suprimido");
        frame_button(NXINPUT_GPTK_L2, 0);
        CHECK(frame_button(NXINPUT_GPTK_R2, 1) == NC_GPTK_LIVE_SUPPRESSED, "R2 = null suprimido");
        frame_button(NXINPUT_GPTK_R2, 0);
        CHECK(frame_button(NXINPUT_GPTK_X, 1) == NC_GPTK_LIVE_PASSTHROUGH, "X = native passa");
        frame_button(NXINPUT_GPTK_X, 0);
        CHECK(frame_button(NXINPUT_GPTK_R3, 1) == NC_GPTK_LIVE_PASSTHROUGH, "R3 nativo no gameplay");
        frame_button(NXINPUT_GPTK_R3, 0);
        CHECK(!nc_gptk_should_consume(NXINPUT_GPTK_UP) &&
              !nc_gptk_should_consume(NXINPUT_GPTK_DOWN) &&
              !nc_gptk_should_consume(NXINPUT_GPTK_LEFT) &&
              !nc_gptk_should_consume(NXINPUT_GPTK_RIGHT),
              "D-pad permanece nativo no gameplay");
        CHECK(nc_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 0.5f, -0.25f) ==
              NC_GPTK_LIVE_DELIVERED && move_calls == 1 && move_x == 0.5f,
              "LEFT_STICK = player.move (vetor entregue)");
        CHECK(nc_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, 1.0f, 0.0f) ==
              NC_GPTK_LIVE_PASSTHROUGH && cursor_calls == 0,
              "RIGHT_STICK nativo no gameplay: cursor nunca vaza");
        /* menu */
        nc_gptk_set_context(NC_GPTK_CONTEXT_MENU, "scene:touch-ui");
        frame_button(NXINPUT_GPTK_A, 1); frame_button(NXINPUT_GPTK_A, 0);
        CHECK(accept.press == 1 && accept.release == 1 && jump.press == 1,
              "menu: A = menu.accept (1/1), não pulo");
        CHECK(click.press == 0, "menu: A NÃO clica o cursor");
        frame_button(NXINPUT_GPTK_R3, 1); frame_button(NXINPUT_GPTK_R3, 0);
        CHECK(click.press == 1 && click.release == 1, "menu: R3 = cursor.click (1/1)");
        CHECK(frame_button(NXINPUT_GPTK_R1, 1) == NC_GPTK_LIVE_PASSTHROUGH &&
              click.press == 1, "menu: R1 nativo, não clica");
        frame_button(NXINPUT_GPTK_R1, 0);
        CHECK(nc_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, 1.0f, 0.0f) ==
              NC_GPTK_LIVE_DELIVERED && cursor_calls == 1,
              "menu: RIGHT_STICK = cursor.move");
        CHECK(nc_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 1.0f, 0.0f) ==
              NC_GPTK_LIVE_PASSTHROUGH && move_calls == 1,
              "menu: LEFT_STICK nativo");
        CHECK(!nc_gptk_fatal(), "sem fatal");
    } else if (!strcmp(scenario, "owner-remap")) {
        /* 2: o dono edita [gameplay]: A = null, R2 = player.jump.  O jogo deixa
         * de pular no A e passa a pular no R2; null não vaza. */
        const char *pairs[] = { "A", "null", "R2", "player.jump" };
        char *owner = owner_with(def, "gameplay", pairs, 2);
        boot(workdir, def, owner);
        CHECK(!strcmp(nc_gptk_source_name(), "owner"), "cópia do dono selecionada");
        CHECK(strcmp(nc_gptk_selected_sha256(), "") != 0, "sha do mapa selecionado disponível");
        CHECK(register_all(0) == 0, "selado");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_SUPPRESSED, "A = null: SUPPRESSED");
        CHECK(nc_gptk_should_consume(NXINPUT_GPTK_A), "A consumido (nada nativo)");
        frame_button(NXINPUT_GPTK_A, 0);
        CHECK(jump.press == 0 && accept.press == 0 && click.press == 0,
              "A = null não alcança pulo, accept nem clique");
        CHECK(nc_gptk_feed_button(NXINPUT_GPTK_R2, 1, 0.9f) == NC_GPTK_LIVE_DELIVERED,
              "R2 = player.jump: entregue");
        CHECK(jump.press == 1 && jump.last_value == 0.9f, "pulo pelo R2 (1 press, magnitude do gatilho)");
        nc_gptk_feed_button(NXINPUT_GPTK_R2, 0, 0.0f);
        CHECK(jump.release == 1, "pulo pelo R2 (1 release)");
        /* o dono continua intacto byte a byte */
        char path[1024];
        snprintf(path, sizeof path, "%s/NEXTOSCONTROLLERS.gptk", workdir);
        size_t n = 0; char *after = read_file(path, &n);
        CHECK(after && !strcmp(after, owner), "arquivo do dono nunca reescrito");
        free(after); free(owner);
    } else if (!strcmp(scenario, "owner-invalid")) {
        /* 3: dono inválido (código evdev) é preservado; default selecionado. */
        const char *bad = "format = NEXTOS_CONTROLLERS/3\nport = namelesscat\n"
                          "FACE_LAYOUT = auto\n[menu]\n304 = player.jump\n[gameplay]\n";
        boot(workdir, def, bad);
        CHECK(nc_gptk_loaded(), "default selecionado apesar do dono inválido");
        CHECK(!strcmp(nc_gptk_source_name(), "default_owner_rejected"),
              "source = default_owner_rejected");
        char path[1024];
        snprintf(path, sizeof path, "%s/NEXTOSCONTROLLERS.gptk", workdir);
        size_t n = 0; char *after = read_file(path, &n);
        CHECK(after && !strcmp(after, bad), "dono inválido preservado byte a byte");
        free(after);
    } else if (!strcmp(scenario, "start-latch")) {
        /* 4: START segurado atravessando gameplay -> menu (pause abre): a
         * troca solta o pause do gameplay; a MESMA pressão não renasce no
         * menu; só uma soltura física seguida de nova pressão gera borda. */
        boot(workdir, def, NULL);
        CHECK(register_all(0) == 0, "selado");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        frame_button(NXINPUT_GPTK_START, 1);
        CHECK(pause_.press == 1 && pause_.release == 0, "START press no gameplay");
        nc_gptk_set_context(NC_GPTK_CONTEXT_MENU, "ui:modal");
        CHECK(pause_.release == 1, "troca de contexto soltou o START latched");
        for (int i = 0; i < 32; i++)
            frame_button(NXINPUT_GPTK_START, 1);
        CHECK(pause_.press == 1, "32 quadros segurado no menu: nenhuma 2ª borda");
        frame_button(NXINPUT_GPTK_START, 0);
        CHECK(pause_.release == 1, "soltura física após quarentena: sem release duplicado");
        frame_button(NXINPUT_GPTK_START, 1);
        CHECK(pause_.press == 2, "nova pressão física: nova borda (fecha o pause)");
        frame_button(NXINPUT_GPTK_START, 0);
        CHECK(pause_.release == 2, "…e sua soltura");
        /* voltar ao gameplay solta latches e re-arma */
        frame_button(NXINPUT_GPTK_A, 1);
        CHECK(accept.press == 1, "A no menu = accept");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        CHECK(accept.release == 1 && jump.press == 0,
              "A segurado ao voltar ao gameplay: accept solto, pulo NÃO nasce");
        frame_button(NXINPUT_GPTK_A, 0);
        frame_button(NXINPUT_GPTK_A, 1);
        CHECK(jump.press == 1, "após soltura real, A pula de novo");
    } else if (!strcmp(scenario, "unknown-context")) {
        /* 5: contexto desconhecido (clear) = passthrough total; null não
         * suprime fora de contexto provado. */
        boot(workdir, def, NULL);
        CHECK(register_all(0) == 0, "selado");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        frame_button(NXINPUT_GPTK_A, 1);
        nc_gptk_clear_context("player:control-locked");
        CHECK(jump.release == 1, "clear soltou o pulo latched");
        CHECK(nc_gptk_context() == -1, "contexto UNPROVEN");
        frame_button(NXINPUT_GPTK_A, 0);
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_PASSTHROUGH &&
              jump.press == 1, "A em contexto desconhecido: passthrough, sem sink");
        CHECK(!nc_gptk_should_consume(NXINPUT_GPTK_L2), "L2 = null NÃO suprime sem contexto provado");
        CHECK(nc_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 1.f, 0.f) == NC_GPTK_LIVE_PASSTHROUGH,
              "vetor em contexto desconhecido: passthrough");
    } else if (!strcmp(scenario, "missing-sink")) {
        /* 6: sem sink para player.jump o selo falha e NADA é consumido. */
        boot(workdir, def, NULL);
        CHECK(register_all(1) != 0, "selo recusado: ação mapeada sem sink");
        CHECK(!nc_gptk_sealed(), "runtime não selado");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        CHECK(nc_gptk_context() == -1, "sem selo não há contexto provado");
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_PASSTHROUGH &&
              !nc_gptk_should_consume(NXINPUT_GPTK_L2),
              "fail-safe: passthrough, null não suprime");
    } else if (!strcmp(scenario, "ack-fatal")) {
        /* 7: sink recusa ACK -> FATAL, runtime invalidado, nunca reproduz. */
        boot(workdir, def, NULL);
        CHECK(register_all(0) == 0, "selado");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        ack_fail_action = 1;
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_FATAL, "ACK recusado = FATAL");
        CHECK(nc_gptk_fatal(), "fatal registrado");
        CHECK(!nc_gptk_sealed(), "runtime invalidado");
        frame_button(NXINPUT_GPTK_A, 0);
        CHECK(frame_button(NXINPUT_GPTK_B, 1) == NC_GPTK_LIVE_PASSTHROUGH &&
              interact.press == 0, "após fatal: nada mais é entregue");
    } else if (!strcmp(scenario, "evidence")) {
        /* 8: NXGPTK_RECEIPT recebe linhas nxinput-gptk-event-evidence/1
         * ligando contexto, fonte, ação e sink de cada entrega. */
        char receipt[1024];
        snprintf(receipt, sizeof receipt, "%s/receipt.jsonl", workdir);
        setenv("NXGPTK_RECEIPT", receipt, 1);
        boot(workdir, def, NULL);
        CHECK(register_all(0) == 0, "selado");
        nc_gptk_set_context(NC_GPTK_CONTEXT_GAMEPLAY, "player:allow-control");
        frame_button(NXINPUT_GPTK_A, 1); frame_button(NXINPUT_GPTK_A, 0);
        nc_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 1.f, 0.f);
        frame_button(NXINPUT_GPTK_L2, 1); frame_button(NXINPUT_GPTK_L2, 0);
        /* START segurado atravessa a troca: a soltura nasce do runtime e tem
         * de ser atribuída ao START, não ao último vetor alimentado. */
        frame_button(NXINPUT_GPTK_START, 1);
        nc_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 0.5f, 0.f);
        nc_gptk_set_context(NC_GPTK_CONTEXT_MENU, "scene:touch-ui");
        frame_button(NXINPUT_GPTK_START, 0);
        frame_button(NXINPUT_GPTK_R3, 1); frame_button(NXINPUT_GPTK_R3, 0);
        size_t n = 0; char *text = read_file(receipt, &n);
        CHECK(text != NULL, "recibo escrito");
        if (text) {
            CHECK(strstr(text, "\"schema\":\"nxinput-gptk-load-evidence/1\"") != NULL,
                  "recibo de seleção owner/default presente");
            CHECK(strstr(text, "\"kind\":\"runtime\",\"marker\":\"nxinput-gptk-runtime/3\"") != NULL,
                  "marcador de runtime /3 no recibo");
            CHECK(strstr(text, "\"kind\":\"context\",\"context\":\"gameplay\",\"source\":\"player:allow-control\"") != NULL,
                  "contexto gameplay observado");
            CHECK(strstr(text, "\"context\":\"gameplay\",\"context_source\":\"player:allow-control\",\"control\":\"A\",\"event\":\"press\",\"decision\":\"ACTION\",\"action\":\"player.jump\",\"sink\":\"engine.input.jump\",\"pressed\":1") != NULL,
                  "entrega A->player.jump->engine.input.jump registrada");
            CHECK(strstr(text, "\"control\":\"LEFT_STICK\",\"event\":\"motion\",\"decision\":\"ACTION\",\"action\":\"player.move\",\"sink\":\"engine.input.move\"") != NULL,
                  "entrega vetorial player.move registrada");
            CHECK(strstr(text, "\"context\":\"menu\",\"context_source\":\"scene:touch-ui\",\"control\":\"R3\",\"event\":\"press\",\"decision\":\"ACTION\",\"action\":\"cursor.click\",\"sink\":\"adapter.pointer.click\"") != NULL,
                  "entrega R3->cursor.click registrada no menu");
            CHECK(strstr(text, "nxinput-gptk-runtime/2") == NULL, "nunca o marcador /2");
            CHECK(strstr(text, "\"context_source\":\"player:allow-control\",\"control\":\"START\",\"event\":\"press\",\"decision\":\"ACTION\",\"action\":\"system.pause\",\"sink\":\"engine.input.pause\",\"pressed\":0") != NULL,
                  "soltura por troca de contexto atribuída ao START");
            CHECK(strstr(text, "\"control\":\"LEFT_STICK\",\"event\":\"press\"") == NULL,
                  "nenhuma soltura atribuída ao vetor");
            CHECK(strstr(text, "\"kind\":\"suppressed\",\"context\":\"gameplay\",\"context_source\":\"player:allow-control\",\"control\":\"L2\",\"event\":\"press\",\"decision\":\"SUPPRESS\",\"delivery_count\":0") != NULL,
                  "supressão L2 = null registrada");
            free(text);
        }
    } else if (!strcmp(scenario, "no-map")) {
        /* 9: sem defaults/ o port fica nativo (loaded=0), sem palpite. */
        CHECK(nc_gptk_preinit(workdir) == 0, "preinit roda sem mapa");
        CHECK(!nc_gptk_loaded(), "nenhum mapa carregado");
        CHECK(register_all(0) != 0, "registro recusado sem mapa");
        CHECK(frame_button(NXINPUT_GPTK_A, 1) == NC_GPTK_LIVE_PASSTHROUGH, "passthrough puro");
    } else {
        fprintf(stderr, "cenário desconhecido: %s\n", scenario);
        return 2;
    }
    free(def);
    printf("%s: %s\n", scenario, failures ? "FALHOU" : "OK");
    return failures ? 1 : 0;
}
