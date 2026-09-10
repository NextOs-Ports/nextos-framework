/* SPDX-License-Identifier: GPL-3.0-only */
/* NAMELESSCAT-CONTROLS-LIVE (1.2.6) — glue puro sobre o runtime vivo do
 * nxinput 0.10.1.  Ver input_gptk.h para o contrato.
 *
 * Cadeia: pad físico -> nxinput normalizado -> GPTK decide action/null/native
 * (nxinput_gptk_decide, por controle e por contexto) -> runtime vivo com ACK
 * -> sink real do adapter -> fluxo nativo Android/Unity do jogo.
 */
#define _POSIX_C_SOURCE 200809L

#include "input_gptk.h"

#include "nxinput_gptk.h"
#include "nxinput_gptk_live.h"
#include "nxinput_gptk_loader.h"
#include "nxinput_gptk_preinit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char nc_assert_none[NC_GPTK_DECIDE_NONE ==
                            (int)NXINPUT_GPTK_DECIDE_NONE ? 1 : -1];
typedef char nc_assert_action[NC_GPTK_DECIDE_ACTION ==
                              (int)NXINPUT_GPTK_DECIDE_ACTION ? 1 : -1];
typedef char nc_assert_suppress[NC_GPTK_DECIDE_SUPPRESS ==
                                (int)NXINPUT_GPTK_DECIDE_SUPPRESS ? 1 : -1];
typedef char nc_assert_native[NC_GPTK_DECIDE_NATIVE ==
                              (int)NXINPUT_GPTK_DECIDE_NATIVE ? 1 : -1];
typedef char nc_assert_menu[NC_GPTK_CONTEXT_MENU ==
                            (int)NXINPUT_GPTK_CONTEXT_MENU ? 1 : -1];
typedef char nc_assert_gameplay[NC_GPTK_CONTEXT_GAMEPLAY ==
                                (int)NXINPUT_GPTK_CONTEXT_GAMEPLAY ? 1 : -1];
typedef char nc_assert_cursor[NC_GPTK_CONTEXT_CURSOR ==
                              (int)NXINPUT_GPTK_CONTEXT_CURSOR ? 1 : -1];
typedef char nc_assert_pass[NC_GPTK_LIVE_PASSTHROUGH ==
                            (int)NXINPUT_GPTK_LIVE_PASSTHROUGH ? 1 : -1];
typedef char nc_assert_deliv[NC_GPTK_LIVE_DELIVERED ==
                             (int)NXINPUT_GPTK_LIVE_DELIVERED ? 1 : -1];
typedef char nc_assert_supp[NC_GPTK_LIVE_SUPPRESSED ==
                            (int)NXINPUT_GPTK_LIVE_SUPPRESSED ? 1 : -1];
typedef char nc_assert_fatal[NC_GPTK_LIVE_FATAL ==
                             (int)NXINPUT_GPTK_LIVE_FATAL ? 1 : -1];
typedef char nc_assert_mask_fits[NXINPUT_GPTK_CONTROL_COUNT <= 32 ? 1 : -1];

/* Allowlist = input.actions do adapter-contract.json deste port.  O loader
 * rejeita qualquer ação fora dela (NXI1001, fail-closed). */
static const char *const nc_allowed_actions[] = {
    "cursor.click",
    "cursor.move",
    "menu.accept",
    "player.down",
    "player.interact",
    "player.jump",
    "player.move",
    "system.pause",
    "system.quit",
};

typedef struct nc_sink_entry {
    char action[NXINPUT_GPTK_ACTION_MAX + 1u];
    char sink_id[96];
    nc_gptk_button_sink_fn button_fn;
    nc_gptk_vector_sink_fn vector_fn;
    void *user;
    unsigned long deliveries;
    int pressed_by; /* controle simbólico que pressionou este sink (-1: nenhum) */
    int vector_active; /* vetor fora do neutro (evidência por BORDA: início e volta ao neutro) */
} nc_sink_entry;

#define NC_MAX_SINKS 32

static nxinput_gptk_preinit_result nc_preinit;
static int nc_preinit_done;
static nxinput_gptk_live nc_live;
static int nc_live_ready;
static nc_sink_entry nc_sinks[NC_MAX_SINKS];
static size_t nc_sink_count;
static uint32_t nc_physical_down;
static uint32_t nc_blocked_until_release;
static unsigned long nc_deliveries;
static FILE *nc_receipt;
static int nc_receipt_tried;

static FILE *nc_receipt_file(void)
{
    if (nc_receipt || nc_receipt_tried)
        return nc_receipt;
    nc_receipt_tried = 1;
    const char *path = getenv("NXGPTK_RECEIPT");
    if (!path || !*path)
        return NULL;
    nc_receipt = fopen(path, "a");
    if (nc_receipt)
        setvbuf(nc_receipt, NULL, _IOLBF, 0);
    return nc_receipt;
}

static void nc_receipt_line(const char *line)
{
    FILE *out = nc_receipt_file();
    if (out) {
        fputs(line, out);
        fputc('\n', out);
    }
}

int nc_gptk_preinit(const char *gamedir)
{
    if (nc_preinit_done)
        return 0;
    memset(&nc_preinit, 0, sizeof nc_preinit);
    nc_preinit_done = 1;
    int rc = nxinput_gptk_preinit_load(
        gamedir && *gamedir ? gamedir : ".", nc_allowed_actions,
        sizeof nc_allowed_actions / sizeof *nc_allowed_actions, &nc_preinit);
    if (rc != 0) {
        fprintf(stderr, "[nc/gptk] preinit: argumentos inválidos (rc=%d)\n",
                rc);
        nc_preinit.loaded = 0;
        return -1;
    }
    char json[1024];
    if (nxinput_gptk_load_receipt_json(&nc_preinit.receipt, json,
                                       sizeof json) == 0) {
        fprintf(stderr, "[nc/gptk] selection receipt: %s\n", json);
        nc_receipt_line(json);
    }
    if (!nc_preinit.loaded) {
        fprintf(stderr,
                "[nc/gptk] NXI%04d: sem NEXTOSCONTROLLERS válido "
                "(owner_err=%d default_err=%d) — port permanece nativo\n",
                nc_preinit.rc, nc_preinit.receipt.owner_error_code,
                nc_preinit.receipt.default_error_code);
        return 0;
    }
    /* Objeto vivo nasce UNPROVEN sobre o mesmo mapa do preinit; registros e
     * selo vêm depois, na ordem imposta pelos guards. */
    nxinput_gptk_live_init(&nc_live, &nc_preinit.map);
    fprintf(stderr,
            "[nc/gptk] preinit: NEXTOS_CONTROLLERS/%u source=%s layout=%s "
            "sha256=%.16s...\n",
            nc_preinit.map.schema_version, nc_gptk_source_name(),
            nxinput_gptk_face_layout_name(nc_preinit.face_layout),
            nc_preinit.receipt.selected_sha256);
    return 0;
}

int nc_gptk_loaded(void)
{
    return nc_preinit_done && nc_preinit.loaded;
}

int nc_gptk_face_layout(void)
{
    return nc_preinit_done ? (int)nc_preinit.face_layout : 0;
}

unsigned nc_gptk_schema(void)
{
    return nc_gptk_loaded() ? nc_preinit.map.schema_version : 0u;
}

const char *nc_gptk_selected_sha256(void)
{
    return nc_gptk_loaded() ? nc_preinit.receipt.selected_sha256 : "";
}

const char *nc_gptk_source_name(void)
{
    if (!nc_preinit_done)
        return "none";
    return nxinput_gptk_load_source_name(
        (nxinput_gptk_load_source)nc_preinit.receipt.source);
}

static nc_sink_entry *nc_sink_new(const char *action, const char *sink_id)
{
    if (!action || !sink_id || nc_sink_count >= NC_MAX_SINKS ||
        strlen(action) > NXINPUT_GPTK_ACTION_MAX ||
        strlen(sink_id) >= sizeof nc_sinks[0].sink_id)
        return NULL;
    nc_sink_entry *e = &nc_sinks[nc_sink_count];
    memset(e, 0, sizeof *e);
    e->pressed_by = -1;
    strcpy(e->action, action);
    strcpy(e->sink_id, sink_id);
    return e;
}

static int nc_current_control = -1;
static void nc_log_delivery(nc_sink_entry *e, const char *event,
                            int pressed, int control)
{
    e->deliveries++;
    nc_deliveries++;
    char line[512];
    const char *ctx = nxinput_gptk_context_name(nc_live.context);
    const char *src = nxinput_gptk_live_context_source(&nc_live);
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"delivery\",\"context\":\"%s\","
             "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"%s\","
             "\"decision\":\"ACTION\",\"action\":\"%s\",\"sink\":\"%s\","
             "\"pressed\":%d,\"delivery_count\":1}",
             nxinput_gptk_event_evidence_schema(), ctx ? ctx : "?",
             src ? src : "",
             control >= 0 ? nxinput_gptk_control_name(control) : "",
             event, e->action, e->sink_id, pressed ? 1 : 0);
    nc_receipt_line(line);
}

static int nc_button_trampoline(void *user, const char *action, int pressed,
                                float value)
{
    nc_sink_entry *e = user;
    int rc = e->button_fn(e->user, action, pressed, value);
    if (rc == 0) {
        /* A soltura pode nascer de uma troca de contexto (release do runtime),
         * fora de qualquer feed: ela pertence ao controle que PRESSIONOU. */
        if (pressed)
            e->pressed_by = nc_current_control;
        nc_log_delivery(e, "press", pressed, e->pressed_by);
        if (!pressed)
            e->pressed_by = -1;
    }
    else
        fprintf(stderr, "[nc/gptk] sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    return rc;
}

static nc_sink_entry *nc_active_vector[NXINPUT_GPTK_CONTROL_COUNT];

static int nc_vector_trampoline(void *user, const char *action, float x,
                                float y)
{
    nc_sink_entry *e = user;
    int rc = e->vector_fn(e->user, action, x, y);
    if (rc == 0) {
        /* Um vetor chega todo quadro: a evidência é por BORDA — uma linha
         * quando o vetor sai do neutro (pressed=1) e uma quando volta
         * (pressed=0, em nc_gptk_feed_vector), por gesto; a contagem por
         * entrega segue em e->deliveries para o diagnóstico. */
        /* Um vetor NULO entregue ao sink não é gesto: só uma deflexão real
         * abre o gesto (senão idle alternaria press/release todo quadro). */
        if (!e->vector_active && (x != 0.0f || y != 0.0f)) {
            nc_log_delivery(e, "motion", 1, nc_current_control);
            e->vector_active = 1;
            if (nc_current_control >= 0 && nc_current_control < (int)NXINPUT_GPTK_CONTROL_COUNT)
                nc_active_vector[nc_current_control] = e;
        }
        e->deliveries++;
    } else {
        fprintf(stderr, "[nc/gptk] vector sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    }
    return rc;
}

int nc_gptk_register_button(const char *action, const char *sink_id,
                            nc_gptk_button_sink_fn fn, void *user)
{
    if (!nc_gptk_loaded() || !fn || nc_live_ready)
        return -1;
    nc_sink_entry *e = nc_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->button_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register(&nc_live, action, nc_button_trampoline,
                                   e) != 0)
        return -1;
    nc_sink_count++;
    return 0;
}

int nc_gptk_register_vector(const char *action, const char *sink_id,
                            nc_gptk_vector_sink_fn fn, void *user)
{
    if (!nc_gptk_loaded() || !fn || nc_live_ready)
        return -1;
    nc_sink_entry *e = nc_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->vector_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register_vector(&nc_live, action,
                                          nc_vector_trampoline, e) != 0)
        return -1;
    nc_sink_count++;
    return 0;
}

int nc_gptk_seal(void)
{
    char error[160];
    if (!nc_gptk_loaded() || nc_live_ready)
        return -1;
    if (nxinput_gptk_live_seal(&nc_live, error, sizeof error) != 0) {
        fprintf(stderr, "[nc/gptk] selo recusado: %s — runtime fica "
                        "nativo (fail-safe)\n", error);
        return -1;
    }
    nc_live_ready = 1;
    nc_physical_down = 0;
    nc_blocked_until_release = 0;
    /* Marcador do contrato controls.runtime_mapping=nxinput-gptk: a string é
     * referenciada por código vivo (nxinput_gptk_runtime_marker), nunca um
     * literal solto no binário. */
    fprintf(stderr,
            "[nc/gptk] runtime=%s evidence=%s authority=NEXTOS_CONTROLLERS/%u "
            "source=%s sinks=%zu sha256=%.16s...\n",
            nxinput_gptk_runtime_marker(),
            nxinput_gptk_event_evidence_schema(),
            nc_preinit.map.schema_version, nc_gptk_source_name(),
            nc_sink_count, nc_preinit.receipt.selected_sha256);
    char line[640];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"runtime\",\"marker\":\"%s\","
             "\"mapping_sha256\":\"%s\",\"source\":\"%s\",\"gptk_schema\":%u,"
             "\"face_layout\":\"%s\",\"sinks\":%zu}",
             nxinput_gptk_event_evidence_schema(),
             nxinput_gptk_runtime_marker(),
             nc_preinit.receipt.selected_sha256, nc_gptk_source_name(),
             nc_preinit.map.schema_version,
             nxinput_gptk_face_layout_name(nc_preinit.face_layout),
             nc_sink_count);
    nc_receipt_line(line);
    return 0;
}

int nc_gptk_sealed(void)
{
    return nc_live_ready && !nxinput_gptk_live_is_fatal(&nc_live);
}

void nc_gptk_set_context(int context, const char *source)
{
    if (!nc_gptk_sealed())
        return;
    int was = nc_gptk_context();
    const char *was_source = nxinput_gptk_live_context_source(&nc_live);
    if (was == context && was_source && source &&
        strcmp(was_source, source) == 0)
        return;
    /* Quarentena: o mesmo botão ainda segurado não pode nascer de novo no
     * contexto seguinte.  O runtime solta as ações latched do contexto
     * antigo em set_context (clear interno). */
    nc_blocked_until_release |= nc_physical_down;
    nc_current_control = -1;
    if (nxinput_gptk_live_set_context(&nc_live, (nxinput_gptk_context)context,
                                      source) != 0) {
        fprintf(stderr, "[nc/gptk] contexto %d (%s) recusado; passthrough\n",
                context, source ? source : "");
        return;
    }
    fprintf(stderr, "[nc/gptk] context=%s source=%s\n",
            nxinput_gptk_context_name(context), source ? source : "");
    char line[320];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"context\",\"context\":\"%s\","
             "\"source\":\"%s\",\"observed\":true}",
             nxinput_gptk_event_evidence_schema(),
             nxinput_gptk_context_name(context), source ? source : "");
    nc_receipt_line(line);
}

void nc_gptk_clear_context(const char *reason)
{
    if (!nc_live_ready)
        return;
    if (!nxinput_gptk_live_context_proven(&nc_live))
        return;
    nc_blocked_until_release |= nc_physical_down;
    nc_current_control = -1;
    nxinput_gptk_live_clear_context(&nc_live);
    fprintf(stderr, "[nc/gptk] context=unproven reason=%s (passthrough)\n",
            reason ? reason : "");
}

int nc_gptk_context(void)
{
    if (!nc_live_ready || !nxinput_gptk_live_context_proven(&nc_live))
        return -1;
    return (int)nc_live.context;
}

const char *nc_gptk_context_source(void)
{
    const char *s = nc_live_ready
                  ? nxinput_gptk_live_context_source(&nc_live) : NULL;
    return s ? s : "";
}

int nc_gptk_feed_button(int control, int pressed, float value)
{
    if (control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return NC_GPTK_LIVE_PASSTHROUGH;
    uint32_t bit = UINT32_C(1) << (unsigned)control;
    int down = pressed != 0;
    int was_down = (nc_physical_down & bit) != 0;

    /* Só a transição física alcança o runtime. */
    if (down == was_down)
        return nc_gptk_should_consume(control) ? NC_GPTK_LIVE_DELIVERED
                                               : NC_GPTK_LIVE_PASSTHROUGH;
    if (down) {
        nc_physical_down |= bit;
    } else {
        nc_physical_down &= ~bit;
        if ((nc_blocked_until_release & bit) != 0) {
            nc_blocked_until_release &= ~bit;
            /* A soltura já foi entregue pelo runtime na troca de contexto;
             * o caminho nativo tampouco tem nada a soltar (não entregou). */
            return nc_gptk_should_consume(control) ? NC_GPTK_LIVE_DELIVERED
                                                   : NC_GPTK_LIVE_PASSTHROUGH;
        }
    }
    if ((nc_blocked_until_release & bit) != 0)
        return nc_gptk_should_consume(control) ? NC_GPTK_LIVE_DELIVERED
                                               : NC_GPTK_LIVE_PASSTHROUGH;
    if (!nc_gptk_sealed())
        return NC_GPTK_LIVE_PASSTHROUGH;
    nc_current_control = control;
    int rc = (int)nxinput_gptk_live_feed(&nc_live, control, down, value);
    if (rc == NC_GPTK_LIVE_FATAL)
        fprintf(stderr, "[nc/gptk] FATAL: sink sem ACK para %s — runtime "
                        "invalidado, nada é reproduzido nativamente\n",
                nxinput_gptk_control_name(control));
    else if (rc == NC_GPTK_LIVE_SUPPRESSED && down) {
        /* `null` provado: a pressão física existiu e NADA foi entregue —
         * evidência tão importante quanto a entrega. */
        char line[320];
        const char *src = nxinput_gptk_live_context_source(&nc_live);
        snprintf(line, sizeof line,
                 "{\"schema\":\"%s\",\"kind\":\"suppressed\",\"context\":\"%s\","
                 "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"press\","
                 "\"decision\":\"SUPPRESS\",\"delivery_count\":0}",
                 nxinput_gptk_event_evidence_schema(),
                 nxinput_gptk_context_name(nc_live.context), src ? src : "",
                 nxinput_gptk_control_name(control));
        nc_receipt_line(line);
        fprintf(stderr, "[nc/gptk] %s = null: suprimido em %s\n",
                nxinput_gptk_control_name(control),
                nxinput_gptk_context_name(nc_live.context));
    }
    return rc;
}

int nc_gptk_feed_vector(int control, float x, float y)
{
    if (!nc_gptk_sealed())
        return NC_GPTK_LIVE_PASSTHROUGH;
    nc_current_control = control;
    if (x == 0.0f && y == 0.0f && control >= 0 && control < (int)NXINPUT_GPTK_CONTROL_COUNT &&
        nc_active_vector[control]) {
        /* Volta ao neutro: fecha o gesto na evidência (nada latched). */
        nc_sink_entry *e = nc_active_vector[control];
        nc_log_delivery(e, "motion", 0, control);
        e->vector_active = 0;
        nc_active_vector[control] = NULL;
    }
    int rc = (int)nxinput_gptk_live_feed_vector(&nc_live, control, x, y);
    if (rc == NC_GPTK_LIVE_FATAL)
        fprintf(stderr, "[nc/gptk] FATAL: vector sink sem ACK para %s\n",
                nxinput_gptk_control_name(control));
    return rc;
}

void nc_gptk_release_all(const char *reason)
{
    if (nc_live_ready && nxinput_gptk_live_context_proven(&nc_live)) {
        /* Solta latches do runtime sem trocar o contexto provado: o clear
         * emite release para toda ação latched; o contexto é re-provado no
         * próximo quadro pelo adapter. */
        nxinput_gptk_live_clear_context(&nc_live);
        fprintf(stderr, "[nc/gptk] release-all reason=%s\n",
                reason ? reason : "");
    }
    /* Gestos de vetor abertos fecham na evidência (volta ao neutro forçada). */
    for (int c = 0; c < (int)NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (nc_active_vector[c]) {
            nc_log_delivery(nc_active_vector[c], "motion", 0, c);
            nc_active_vector[c]->vector_active = 0;
            nc_active_vector[c] = NULL;
        }
    }
    nc_physical_down = 0;
    nc_blocked_until_release = 0;
}

int nc_gptk_should_consume(int control)
{
    if (!nc_live_ready)
        return 0;
    return nxinput_gptk_live_should_consume(&nc_live, control);
}

int nc_gptk_decision(int control, const char **action_out)
{
    if (action_out)
        *action_out = NULL;
    if (!nc_gptk_sealed() || !nxinput_gptk_live_context_proven(&nc_live) ||
        control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return NC_GPTK_DECIDE_NONE;
    return (int)nxinput_gptk_decide(&nc_preinit.map, nc_live.context, control,
                                    action_out);
}

int nc_gptk_fatal(void)
{
    return nc_live_ready && nxinput_gptk_live_is_fatal(&nc_live);
}

unsigned long nc_gptk_delivery_count(void)
{
    return nc_deliveries;
}

int nc_gptk_cursor_tuning_copy(struct nxinput_gptk_cursor_tuning *out)
{
    if (!out)
        return -1;
    nxinput_gptk_cursor_tuning_get(nc_gptk_loaded() ? &nc_preinit.map : NULL,
                                   out);
    return nc_gptk_loaded() ? 0 : -1;
}

