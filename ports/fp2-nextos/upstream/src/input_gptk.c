/* SPDX-License-Identifier: GPL-3.0-only */
/* FP2-CONTROLS-LIVE (1.1.2) — glue puro sobre o runtime vivo do
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

typedef char fp2_assert_none[FP2_GPTK_DECIDE_NONE ==
                            (int)NXINPUT_GPTK_DECIDE_NONE ? 1 : -1];
typedef char fp2_assert_action[FP2_GPTK_DECIDE_ACTION ==
                              (int)NXINPUT_GPTK_DECIDE_ACTION ? 1 : -1];
typedef char fp2_assert_suppress[FP2_GPTK_DECIDE_SUPPRESS ==
                                (int)NXINPUT_GPTK_DECIDE_SUPPRESS ? 1 : -1];
typedef char fp2_assert_native[FP2_GPTK_DECIDE_NATIVE ==
                              (int)NXINPUT_GPTK_DECIDE_NATIVE ? 1 : -1];
typedef char fp2_assert_menu[FP2_GPTK_CONTEXT_MENU ==
                            (int)NXINPUT_GPTK_CONTEXT_MENU ? 1 : -1];
typedef char fp2_assert_gameplay[FP2_GPTK_CONTEXT_GAMEPLAY ==
                                (int)NXINPUT_GPTK_CONTEXT_GAMEPLAY ? 1 : -1];
typedef char fp2_assert_cursor[FP2_GPTK_CONTEXT_CURSOR ==
                              (int)NXINPUT_GPTK_CONTEXT_CURSOR ? 1 : -1];
typedef char fp2_assert_pass[FP2_GPTK_LIVE_PASSTHROUGH ==
                            (int)NXINPUT_GPTK_LIVE_PASSTHROUGH ? 1 : -1];
typedef char fp2_assert_deliv[FP2_GPTK_LIVE_DELIVERED ==
                             (int)NXINPUT_GPTK_LIVE_DELIVERED ? 1 : -1];
typedef char fp2_assert_supp[FP2_GPTK_LIVE_SUPPRESSED ==
                            (int)NXINPUT_GPTK_LIVE_SUPPRESSED ? 1 : -1];
typedef char fp2_assert_fatal[FP2_GPTK_LIVE_FATAL ==
                             (int)NXINPUT_GPTK_LIVE_FATAL ? 1 : -1];
typedef char fp2_assert_mask_fits[NXINPUT_GPTK_CONTROL_COUNT <= 32 ? 1 : -1];

/* Allowlist = input.actions do adapter-contract.json deste port.  O loader
 * rejeita qualquer ação fora dela (NXI1001, fail-closed). */
static const char *const fp2_allowed_actions[] = {
    "fp2.attack",
    "fp2.cancel",
    "fp2.confirm",
    "fp2.guard",
    "fp2.jump",
    "fp2.move",
    "fp2.pause",
    "fp2.special",
};

typedef struct fp2_sink_entry {
    char action[NXINPUT_GPTK_ACTION_MAX + 1u];
    char sink_id[96];
    fp2_gptk_button_sink_fn button_fn;
    fp2_gptk_vector_sink_fn vector_fn;
    void *user;
    unsigned long deliveries;
    int pressed_by; /* controle simbólico que pressionou este sink (-1: nenhum) */
    int vector_active; /* vetor fora do neutro (evidência por BORDA: início e volta ao neutro) */
} fp2_sink_entry;

#define FP2_MAX_SINKS 32

static nxinput_gptk_preinit_result fp2_preinit;
static int fp2_preinit_done;
static nxinput_gptk_live fp2_live;
static int fp2_live_ready;
static fp2_sink_entry fp2_sinks[FP2_MAX_SINKS];
static size_t fp2_sink_count;
static uint32_t fp2_physical_down;
static uint32_t fp2_blocked_until_release;
static unsigned long fp2_deliveries;
static FILE *fp2_receipt;
static int fp2_receipt_tried;

static FILE *fp2_receipt_file(void)
{
    if (fp2_receipt || fp2_receipt_tried)
        return fp2_receipt;
    fp2_receipt_tried = 1;
    const char *path = getenv("NXGPTK_RECEIPT");
    if (!path || !*path)
        return NULL;
    fp2_receipt = fopen(path, "a");
    if (fp2_receipt)
        setvbuf(fp2_receipt, NULL, _IOLBF, 0);
    return fp2_receipt;
}

static void fp2_receipt_line(const char *line)
{
    FILE *out = fp2_receipt_file();
    if (out) {
        fputs(line, out);
        fputc('\n', out);
    }
}

int fp2_gptk_preinit(const char *gamedir)
{
    if (fp2_preinit_done)
        return 0;
    memset(&fp2_preinit, 0, sizeof fp2_preinit);
    fp2_preinit_done = 1;
    int rc = nxinput_gptk_preinit_load(
        gamedir && *gamedir ? gamedir : ".", fp2_allowed_actions,
        sizeof fp2_allowed_actions / sizeof *fp2_allowed_actions, &fp2_preinit);
    if (rc != 0) {
        fprintf(stderr, "[fp2/gptk] preinit: argumentos inválidos (rc=%d)\n",
                rc);
        fp2_preinit.loaded = 0;
        return -1;
    }
    char json[1024];
    if (nxinput_gptk_load_receipt_json(&fp2_preinit.receipt, json,
                                       sizeof json) == 0) {
        fprintf(stderr, "[fp2/gptk] selection receipt: %s\n", json);
        fp2_receipt_line(json);
    }
    if (!fp2_preinit.loaded) {
        fprintf(stderr,
                "[fp2/gptk] NXI%04d: sem NEXTOSCONTROLLERS válido "
                "(owner_err=%d default_err=%d) — port permanece nativo\n",
                fp2_preinit.rc, fp2_preinit.receipt.owner_error_code,
                fp2_preinit.receipt.default_error_code);
        return 0;
    }
    /* Objeto vivo nasce UNPROVEN sobre o mesmo mapa do preinit; registros e
     * selo vêm depois, na ordem imposta pelos guards. */
    nxinput_gptk_live_init(&fp2_live, &fp2_preinit.map);
    fprintf(stderr,
            "[fp2/gptk] preinit: NEXTOS_CONTROLLERS/%u source=%s layout=%s "
            "sha256=%.16s...\n",
            fp2_preinit.map.schema_version, fp2_gptk_source_name(),
            nxinput_gptk_face_layout_name(fp2_preinit.face_layout),
            fp2_preinit.receipt.selected_sha256);
    return 0;
}

int fp2_gptk_loaded(void)
{
    return fp2_preinit_done && fp2_preinit.loaded;
}

int fp2_gptk_face_layout(void)
{
    return fp2_preinit_done ? (int)fp2_preinit.face_layout : 0;
}

unsigned fp2_gptk_schema(void)
{
    return fp2_gptk_loaded() ? fp2_preinit.map.schema_version : 0u;
}

const char *fp2_gptk_selected_sha256(void)
{
    return fp2_gptk_loaded() ? fp2_preinit.receipt.selected_sha256 : "";
}

const char *fp2_gptk_source_name(void)
{
    if (!fp2_preinit_done)
        return "none";
    return nxinput_gptk_load_source_name(
        (nxinput_gptk_load_source)fp2_preinit.receipt.source);
}

static fp2_sink_entry *fp2_sink_new(const char *action, const char *sink_id)
{
    if (!action || !sink_id || fp2_sink_count >= FP2_MAX_SINKS ||
        strlen(action) > NXINPUT_GPTK_ACTION_MAX ||
        strlen(sink_id) >= sizeof fp2_sinks[0].sink_id)
        return NULL;
    fp2_sink_entry *e = &fp2_sinks[fp2_sink_count];
    memset(e, 0, sizeof *e);
    e->pressed_by = -1;
    strcpy(e->action, action);
    strcpy(e->sink_id, sink_id);
    return e;
}

static int fp2_current_control = -1;
static void fp2_log_delivery(fp2_sink_entry *e, const char *event,
                            int pressed, int control)
{
    e->deliveries++;
    fp2_deliveries++;
    char line[512];
    const char *ctx = nxinput_gptk_context_name(fp2_live.context);
    const char *src = nxinput_gptk_live_context_source(&fp2_live);
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"delivery\",\"context\":\"%s\","
             "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"%s\","
             "\"decision\":\"ACTION\",\"action\":\"%s\",\"sink\":\"%s\","
             "\"pressed\":%d,\"delivery_count\":1}",
             nxinput_gptk_event_evidence_schema(), ctx ? ctx : "?",
             src ? src : "",
             control >= 0 ? nxinput_gptk_control_name(control) : "",
             event, e->action, e->sink_id, pressed ? 1 : 0);
    fp2_receipt_line(line);
}

static int fp2_button_trampoline(void *user, const char *action, int pressed,
                                float value)
{
    fp2_sink_entry *e = user;
    int rc = e->button_fn(e->user, action, pressed, value);
    if (rc == 0) {
        /* A soltura pode nascer de uma troca de contexto (release do runtime),
         * fora de qualquer feed: ela pertence ao controle que PRESSIONOU. */
        if (pressed)
            e->pressed_by = fp2_current_control;
        fp2_log_delivery(e, "press", pressed, e->pressed_by);
        if (!pressed)
            e->pressed_by = -1;
    }
    else
        fprintf(stderr, "[fp2/gptk] sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    return rc;
}

static fp2_sink_entry *fp2_active_vector[NXINPUT_GPTK_CONTROL_COUNT];

static int fp2_vector_trampoline(void *user, const char *action, float x,
                                float y)
{
    fp2_sink_entry *e = user;
    int rc = e->vector_fn(e->user, action, x, y);
    if (rc == 0) {
        /* Um vetor chega todo quadro: a evidência é por BORDA — uma linha
         * quando o vetor sai do neutro (pressed=1) e uma quando volta
         * (pressed=0, em fp2_gptk_feed_vector), por gesto; a contagem por
         * entrega segue em e->deliveries para o diagnóstico. */
        /* Um vetor NULO entregue ao sink não é gesto: só uma deflexão real
         * abre o gesto (senão idle alternaria press/release todo quadro). */
        if (!e->vector_active && (x != 0.0f || y != 0.0f)) {
            fp2_log_delivery(e, "motion", 1, fp2_current_control);
            e->vector_active = 1;
            if (fp2_current_control >= 0 && fp2_current_control < (int)NXINPUT_GPTK_CONTROL_COUNT)
                fp2_active_vector[fp2_current_control] = e;
        }
        e->deliveries++;
    } else {
        fprintf(stderr, "[fp2/gptk] vector sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    }
    return rc;
}

int fp2_gptk_register_button(const char *action, const char *sink_id,
                            fp2_gptk_button_sink_fn fn, void *user)
{
    if (!fp2_gptk_loaded() || !fn || fp2_live_ready)
        return -1;
    fp2_sink_entry *e = fp2_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->button_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register(&fp2_live, action, fp2_button_trampoline,
                                   e) != 0)
        return -1;
    fp2_sink_count++;
    return 0;
}

int fp2_gptk_register_vector(const char *action, const char *sink_id,
                            fp2_gptk_vector_sink_fn fn, void *user)
{
    if (!fp2_gptk_loaded() || !fn || fp2_live_ready)
        return -1;
    fp2_sink_entry *e = fp2_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->vector_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register_vector(&fp2_live, action,
                                          fp2_vector_trampoline, e) != 0)
        return -1;
    fp2_sink_count++;
    return 0;
}

int fp2_gptk_seal(void)
{
    char error[160];
    if (!fp2_gptk_loaded() || fp2_live_ready)
        return -1;
    if (nxinput_gptk_live_seal(&fp2_live, error, sizeof error) != 0) {
        fprintf(stderr, "[fp2/gptk] selo recusado: %s — runtime fica "
                        "nativo (fail-safe)\n", error);
        return -1;
    }
    fp2_live_ready = 1;
    fp2_physical_down = 0;
    fp2_blocked_until_release = 0;
    /* Marcador do contrato controls.runtime_mapping=nxinput-gptk: a string é
     * referenciada por código vivo (nxinput_gptk_runtime_marker), nunca um
     * literal solto no binário. */
    fprintf(stderr,
            "[fp2/gptk] runtime=%s evidence=%s authority=NEXTOS_CONTROLLERS/%u "
            "source=%s sinks=%zu sha256=%.16s...\n",
            nxinput_gptk_runtime_marker(),
            nxinput_gptk_event_evidence_schema(),
            fp2_preinit.map.schema_version, fp2_gptk_source_name(),
            fp2_sink_count, fp2_preinit.receipt.selected_sha256);
    char line[640];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"runtime\",\"marker\":\"%s\","
             "\"mapping_sha256\":\"%s\",\"source\":\"%s\",\"gptk_schema\":%u,"
             "\"face_layout\":\"%s\",\"sinks\":%zu}",
             nxinput_gptk_event_evidence_schema(),
             nxinput_gptk_runtime_marker(),
             fp2_preinit.receipt.selected_sha256, fp2_gptk_source_name(),
             fp2_preinit.map.schema_version,
             nxinput_gptk_face_layout_name(fp2_preinit.face_layout),
             fp2_sink_count);
    fp2_receipt_line(line);
    return 0;
}

int fp2_gptk_sealed(void)
{
    return fp2_live_ready && !nxinput_gptk_live_is_fatal(&fp2_live);
}

void fp2_gptk_set_context(int context, const char *source)
{
    if (!fp2_gptk_sealed())
        return;
    int was = fp2_gptk_context();
    const char *was_source = nxinput_gptk_live_context_source(&fp2_live);
    if (was == context && was_source && source &&
        strcmp(was_source, source) == 0)
        return;
    /* Quarentena: o mesmo botão ainda segurado não pode nascer de novo no
     * contexto seguinte.  O runtime solta as ações latched do contexto
     * antigo em set_context (clear interno). */
    fp2_blocked_until_release |= fp2_physical_down;
    fp2_current_control = -1;
    if (nxinput_gptk_live_set_context(&fp2_live, (nxinput_gptk_context)context,
                                      source) != 0) {
        fprintf(stderr, "[fp2/gptk] contexto %d (%s) recusado; passthrough\n",
                context, source ? source : "");
        return;
    }
    fprintf(stderr, "[fp2/gptk] context=%s source=%s\n",
            nxinput_gptk_context_name(context), source ? source : "");
    char line[320];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"context\",\"context\":\"%s\","
             "\"source\":\"%s\",\"observed\":true}",
             nxinput_gptk_event_evidence_schema(),
             nxinput_gptk_context_name(context), source ? source : "");
    fp2_receipt_line(line);
}

void fp2_gptk_clear_context(const char *reason)
{
    if (!fp2_live_ready)
        return;
    if (!nxinput_gptk_live_context_proven(&fp2_live))
        return;
    fp2_blocked_until_release |= fp2_physical_down;
    fp2_current_control = -1;
    nxinput_gptk_live_clear_context(&fp2_live);
    fprintf(stderr, "[fp2/gptk] context=unproven reason=%s (passthrough)\n",
            reason ? reason : "");
}

int fp2_gptk_context(void)
{
    if (!fp2_live_ready || !nxinput_gptk_live_context_proven(&fp2_live))
        return -1;
    return (int)fp2_live.context;
}

const char *fp2_gptk_context_source(void)
{
    const char *s = fp2_live_ready
                  ? nxinput_gptk_live_context_source(&fp2_live) : NULL;
    return s ? s : "";
}

int fp2_gptk_feed_button(int control, int pressed, float value)
{
    if (control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return FP2_GPTK_LIVE_PASSTHROUGH;
    uint32_t bit = UINT32_C(1) << (unsigned)control;
    int down = pressed != 0;
    int was_down = (fp2_physical_down & bit) != 0;

    /* Só a transição física alcança o runtime. */
    if (down == was_down)
        return fp2_gptk_should_consume(control) ? FP2_GPTK_LIVE_DELIVERED
                                               : FP2_GPTK_LIVE_PASSTHROUGH;
    if (down) {
        fp2_physical_down |= bit;
    } else {
        fp2_physical_down &= ~bit;
        if ((fp2_blocked_until_release & bit) != 0) {
            fp2_blocked_until_release &= ~bit;
            /* A soltura já foi entregue pelo runtime na troca de contexto;
             * o caminho nativo tampouco tem nada a soltar (não entregou). */
            return fp2_gptk_should_consume(control) ? FP2_GPTK_LIVE_DELIVERED
                                                   : FP2_GPTK_LIVE_PASSTHROUGH;
        }
    }
    if ((fp2_blocked_until_release & bit) != 0)
        return fp2_gptk_should_consume(control) ? FP2_GPTK_LIVE_DELIVERED
                                               : FP2_GPTK_LIVE_PASSTHROUGH;
    if (!fp2_gptk_sealed())
        return FP2_GPTK_LIVE_PASSTHROUGH;
    fp2_current_control = control;
    int rc = (int)nxinput_gptk_live_feed(&fp2_live, control, down, value);
    if (rc == FP2_GPTK_LIVE_FATAL)
        fprintf(stderr, "[fp2/gptk] FATAL: sink sem ACK para %s — runtime "
                        "invalidado, nada é reproduzido nativamente\n",
                nxinput_gptk_control_name(control));
    else if (rc == FP2_GPTK_LIVE_SUPPRESSED && down) {
        /* `null` provado: a pressão física existiu e NADA foi entregue —
         * evidência tão importante quanto a entrega. */
        char line[320];
        const char *src = nxinput_gptk_live_context_source(&fp2_live);
        snprintf(line, sizeof line,
                 "{\"schema\":\"%s\",\"kind\":\"suppressed\",\"context\":\"%s\","
                 "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"press\","
                 "\"decision\":\"SUPPRESS\",\"delivery_count\":0}",
                 nxinput_gptk_event_evidence_schema(),
                 nxinput_gptk_context_name(fp2_live.context), src ? src : "",
                 nxinput_gptk_control_name(control));
        fp2_receipt_line(line);
        fprintf(stderr, "[fp2/gptk] %s = null: suprimido em %s\n",
                nxinput_gptk_control_name(control),
                nxinput_gptk_context_name(fp2_live.context));
    }
    return rc;
}

int fp2_gptk_feed_vector(int control, float x, float y)
{
    if (!fp2_gptk_sealed())
        return FP2_GPTK_LIVE_PASSTHROUGH;
    fp2_current_control = control;
    if (x == 0.0f && y == 0.0f && control >= 0 && control < (int)NXINPUT_GPTK_CONTROL_COUNT &&
        fp2_active_vector[control]) {
        /* Volta ao neutro: fecha o gesto na evidência (nada latched). */
        fp2_sink_entry *e = fp2_active_vector[control];
        fp2_log_delivery(e, "motion", 0, control);
        e->vector_active = 0;
        fp2_active_vector[control] = NULL;
    }
    int rc = (int)nxinput_gptk_live_feed_vector(&fp2_live, control, x, y);
    if (rc == FP2_GPTK_LIVE_FATAL)
        fprintf(stderr, "[fp2/gptk] FATAL: vector sink sem ACK para %s\n",
                nxinput_gptk_control_name(control));
    return rc;
}

void fp2_gptk_release_all(const char *reason)
{
    if (fp2_live_ready && nxinput_gptk_live_context_proven(&fp2_live)) {
        /* Solta latches do runtime sem trocar o contexto provado: o clear
         * emite release para toda ação latched; o contexto é re-provado no
         * próximo quadro pelo adapter. */
        nxinput_gptk_live_clear_context(&fp2_live);
        fprintf(stderr, "[fp2/gptk] release-all reason=%s\n",
                reason ? reason : "");
    }
    /* Gestos de vetor abertos fecham na evidência (volta ao neutro forçada). */
    for (int c = 0; c < (int)NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (fp2_active_vector[c]) {
            fp2_log_delivery(fp2_active_vector[c], "motion", 0, c);
            fp2_active_vector[c]->vector_active = 0;
            fp2_active_vector[c] = NULL;
        }
    }
    fp2_physical_down = 0;
    fp2_blocked_until_release = 0;
}

int fp2_gptk_should_consume(int control)
{
    if (!fp2_live_ready)
        return 0;
    return nxinput_gptk_live_should_consume(&fp2_live, control);
}

int fp2_gptk_decision(int control, const char **action_out)
{
    if (action_out)
        *action_out = NULL;
    if (!fp2_gptk_sealed() || !nxinput_gptk_live_context_proven(&fp2_live) ||
        control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return FP2_GPTK_DECIDE_NONE;
    return (int)nxinput_gptk_decide(&fp2_preinit.map, fp2_live.context, control,
                                    action_out);
}

int fp2_gptk_fatal(void)
{
    return fp2_live_ready && nxinput_gptk_live_is_fatal(&fp2_live);
}

unsigned long fp2_gptk_delivery_count(void)
{
    return fp2_deliveries;
}

int fp2_gptk_cursor_tuning_copy(struct nxinput_gptk_cursor_tuning *out)
{
    if (!out)
        return -1;
    nxinput_gptk_cursor_tuning_get(fp2_gptk_loaded() ? &fp2_preinit.map : NULL,
                                   out);
    return fp2_gptk_loaded() ? 0 : -1;
}

