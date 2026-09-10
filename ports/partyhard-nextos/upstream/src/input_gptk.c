/* SPDX-License-Identifier: GPL-3.0-only */
/* PARTYHARD-CONTROLS-LIVE (1.0.1) — glue puro sobre o runtime vivo do
 * nxinput 0.11.8.  Ver input_gptk.h para o contrato.
 *
 * Cadeia: pad físico -> nxinput normalizado -> GPTK decide action/null/native
 * (nxinput_gptk_decide, por controle e por contexto) -> runtime vivo com ACK
 * -> sink real do adapter -> fluxo nativo Android/Unity do jogo.
 */
#define _POSIX_C_SOURCE 200809L

#include "input_gptk.h"

#include "nxinput_gptk.h"
#include "nxinput_gptk4.h"
#include "nxinput_gptk4_bridge.h"
#include "nxinput_gptk_live.h"
#include "nxinput_gptk_loader.h"
#include "nxinput_gptk_preinit.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef char st_assert_none[ST_GPTK_DECIDE_NONE ==
                            (int)NXINPUT_GPTK_DECIDE_NONE ? 1 : -1];
typedef char st_assert_action[ST_GPTK_DECIDE_ACTION ==
                              (int)NXINPUT_GPTK_DECIDE_ACTION ? 1 : -1];
typedef char st_assert_suppress[ST_GPTK_DECIDE_SUPPRESS ==
                                (int)NXINPUT_GPTK_DECIDE_SUPPRESS ? 1 : -1];
typedef char st_assert_native[ST_GPTK_DECIDE_NATIVE ==
                              (int)NXINPUT_GPTK_DECIDE_NATIVE ? 1 : -1];
typedef char st_assert_menu[ST_GPTK_CONTEXT_MENU ==
                            (int)NXINPUT_GPTK_CONTEXT_MENU ? 1 : -1];
typedef char st_assert_gameplay[ST_GPTK_CONTEXT_GAMEPLAY ==
                                (int)NXINPUT_GPTK_CONTEXT_GAMEPLAY ? 1 : -1];
typedef char st_assert_cursor[ST_GPTK_CONTEXT_CURSOR ==
                              (int)NXINPUT_GPTK_CONTEXT_CURSOR ? 1 : -1];
typedef char st_assert_pass[ST_GPTK_LIVE_PASSTHROUGH ==
                            (int)NXINPUT_GPTK_LIVE_PASSTHROUGH ? 1 : -1];
typedef char st_assert_deliv[ST_GPTK_LIVE_DELIVERED ==
                             (int)NXINPUT_GPTK_LIVE_DELIVERED ? 1 : -1];
typedef char st_assert_supp[ST_GPTK_LIVE_SUPPRESSED ==
                            (int)NXINPUT_GPTK_LIVE_SUPPRESSED ? 1 : -1];
typedef char st_assert_fatal[ST_GPTK_LIVE_FATAL ==
                             (int)NXINPUT_GPTK_LIVE_FATAL ? 1 : -1];
typedef char st_assert_mask_fits[NXINPUT_GPTK_CONTROL_COUNT <= 32 ? 1 : -1];

/* NEXTOS_CONTROLLERS/4: contrato tipado do arquivo editável do dono.  A
 * ponte V5 projeta o mapa /4 sobre o dispatcher vivo já aprovado pelo jogo. */
static const nxinput_gptk4_action_decl st_gptk4_actions[] = {
    {"partyhard.action1", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.action2", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.action3", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.action4", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.bumper_left", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.bumper_right", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.click", NXINPUT_GPTK4_V_DIGITAL},
    {"partyhard.cursor", NXINPUT_GPTK4_V_VECTOR2},
    {"partyhard.move", NXINPUT_GPTK4_V_VECTOR2},
    {"partyhard.pause", NXINPUT_GPTK4_V_DIGITAL},
};
static const char *const st_gptk4_contexts[] = {"menu", "gameplay"};
static nxinput_gptk4 st_gptk4_map;
static nxinput_gptk4_bridge_receipt st_gptk4_receipt;

typedef struct st_sink_entry {
    char action[NXINPUT_GPTK_ACTION_MAX + 1u];
    char sink_id[96];
    st_gptk_button_sink_fn button_fn;
    st_gptk_vector_sink_fn vector_fn;
    void *user;
    unsigned long deliveries;
    int pressed_by; /* controle simbólico que pressionou este sink (-1: nenhum) */
    int vector_active; /* vetor fora do neutro (evidência por BORDA: início e volta ao neutro) */
} st_sink_entry;

#define ST_MAX_SINKS 32

static nxinput_gptk_preinit_result st_preinit;
static int st_preinit_done;
static nxinput_gptk_live st_live;
static int st_live_ready;
static st_sink_entry st_sinks[ST_MAX_SINKS];
static size_t st_sink_count;
static uint32_t st_physical_down;
static uint32_t st_blocked_until_release;
static unsigned long st_deliveries;
static FILE *st_receipt;
static int st_receipt_tried;

static FILE *st_receipt_file(void)
{
    if (st_receipt || st_receipt_tried)
        return st_receipt;
    st_receipt_tried = 1;
    const char *path = getenv("NXGPTK_RECEIPT");
    if (!path || !*path)
        return NULL;
    st_receipt = fopen(path, "a");
    if (st_receipt)
        setvbuf(st_receipt, NULL, _IOLBF, 0);
    return st_receipt;
}

static void st_receipt_line(const char *line)
{
    FILE *out = st_receipt_file();
    if (out) {
        fputs(line, out);
        fputc('\n', out);
    }
}

int st_gptk_preinit(const char *gamedir)
{
    if (st_preinit_done)
        return 0;
    memset(&st_preinit, 0, sizeof st_preinit);
    st_preinit_done = 1;
    st_preinit.api_version = 1u;
    st_preinit.struct_size = sizeof st_preinit;
    st_preinit.receipt.api_version = 1u;
    const char *dir = gamedir && *gamedir ? gamedir : ".";
    int owner_fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int defaults_fd = owner_fd >= 0
        ? openat(owner_fd, "defaults", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        : -1;
    nxinput_gptk4_contract contract;
    memset(&contract, 0, sizeof contract);
    contract.actions = st_gptk4_actions;
    contract.count = sizeof st_gptk4_actions / sizeof *st_gptk4_actions;
    contract.contexts = st_gptk4_contexts;
    contract.context_count = sizeof st_gptk4_contexts / sizeof *st_gptk4_contexts;
    int rc = nxinput_gptk4_load_project_at(owner_fd, defaults_fd, &contract,
                                           &st_gptk4_map, &st_preinit.map,
                                           &st_gptk4_receipt);
    if (defaults_fd >= 0)
        close(defaults_fd);
    if (owner_fd >= 0)
        close(owner_fd);
    char json[1024];
    if (nxinput_gptk4_bridge_receipt_json(&st_gptk4_receipt, json,
                                          sizeof json) == 0) {
        fprintf(stderr, "[st/gptk] selection receipt: %s\n", json);
        st_receipt_line(json);
    }
    st_preinit.loaded = rc == 0;
    st_preinit.rc = st_gptk4_receipt.rc;
    st_preinit.face_layout = 0;
    st_preinit.receipt.source = (uint8_t)(
        st_gptk4_receipt.source == NXINPUT_GPTK4_SRC_OWNER ? NXINPUT_GPTK_LOAD_OWNER
        : st_gptk4_receipt.source == NXINPUT_GPTK4_SRC_DEFAULT
            ? (st_gptk4_receipt.owner_rejected ? NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED
                                               : NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING)
            : NXINPUT_GPTK_LOAD_NONE);
    st_preinit.receipt.owner_present = st_gptk4_receipt.owner_present;
    st_preinit.receipt.owner_error_code =
        st_gptk4_receipt.owner_rejected ? st_gptk4_receipt.rc : 0;
    st_preinit.receipt.default_error_code =
        st_gptk4_receipt.default_rejected ? st_gptk4_receipt.rc : 0;
    st_preinit.receipt.selected_gptk_schema = NXINPUT_GPTK_SCHEMA_V4;
    snprintf(st_preinit.receipt.selected_sha256,
             sizeof st_preinit.receipt.selected_sha256, "%s",
             st_gptk4_receipt.sha256);
    if (!st_preinit.loaded) {
        fprintf(stderr,
                "[st/gptk] NXI%04d: sem NEXTOSCONTROLLERS/4 válido "
                "(%s:%u:%u %s) — port permanece nativo\n",
                st_gptk4_receipt.rc, "NEXTOSCONTROLLERS.gptk",
                st_gptk4_receipt.line, st_gptk4_receipt.column,
                st_gptk4_receipt.what);
        return 0;
    }
    /* Objeto vivo nasce UNPROVEN sobre o mesmo mapa do preinit; registros e
     * selo vêm depois, na ordem imposta pelos guards. */
    nxinput_gptk_live_init(&st_live, &st_preinit.map);
    fprintf(stderr,
            "[st/gptk] preinit: NEXTOS_CONTROLLERS/%u source=%s layout=%s "
            "sha256=%.16s...\n",
            st_preinit.map.schema_version, st_gptk_source_name(),
            nxinput_gptk_face_layout_name(st_preinit.face_layout),
            st_preinit.receipt.selected_sha256);
    return 0;
}

int st_gptk_loaded(void)
{
    return st_preinit_done && st_preinit.loaded;
}

int st_gptk_face_layout(void)
{
    return st_preinit_done ? (int)st_preinit.face_layout : 0;
}

unsigned st_gptk_schema(void)
{
    return st_gptk_loaded() ? st_preinit.map.schema_version : 0u;
}

const char *st_gptk_selected_sha256(void)
{
    return st_gptk_loaded() ? st_preinit.receipt.selected_sha256 : "";
}

const char *st_gptk_source_name(void)
{
    if (!st_preinit_done)
        return "none";
    return nxinput_gptk_load_source_name(
        (nxinput_gptk_load_source)st_preinit.receipt.source);
}

static st_sink_entry *st_sink_new(const char *action, const char *sink_id)
{
    if (!action || !sink_id || st_sink_count >= ST_MAX_SINKS ||
        strlen(action) > NXINPUT_GPTK_ACTION_MAX ||
        strlen(sink_id) >= sizeof st_sinks[0].sink_id)
        return NULL;
    st_sink_entry *e = &st_sinks[st_sink_count];
    memset(e, 0, sizeof *e);
    e->pressed_by = -1;
    strcpy(e->action, action);
    strcpy(e->sink_id, sink_id);
    return e;
}

static int st_current_control = -1;
static void st_log_delivery(st_sink_entry *e, const char *event,
                            int pressed, int control)
{
    e->deliveries++;
    st_deliveries++;
    char line[512];
    const char *ctx = nxinput_gptk_context_name(st_live.context);
    const char *src = nxinput_gptk_live_context_source(&st_live);
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"delivery\",\"context\":\"%s\","
             "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"%s\","
             "\"decision\":\"ACTION\",\"action\":\"%s\",\"sink\":\"%s\","
             "\"pressed\":%d,\"delivery_count\":1}",
             nxinput_gptk_event_evidence_schema(), ctx ? ctx : "?",
             src ? src : "",
             control >= 0 ? nxinput_gptk_control_name(control) : "",
             event, e->action, e->sink_id, pressed ? 1 : 0);
    st_receipt_line(line);
}

static int st_button_trampoline(void *user, const char *action, int pressed,
                                float value)
{
    st_sink_entry *e = user;
    int rc = e->button_fn(e->user, action, pressed, value);
    if (rc == 0) {
        /* A soltura pode nascer de uma troca de contexto (release do runtime),
         * fora de qualquer feed: ela pertence ao controle que PRESSIONOU. */
        if (pressed)
            e->pressed_by = st_current_control;
        st_log_delivery(e, "press", pressed, e->pressed_by);
        if (!pressed)
            e->pressed_by = -1;
    }
    else
        fprintf(stderr, "[st/gptk] sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    return rc;
}

static st_sink_entry *st_active_vector[NXINPUT_GPTK_CONTROL_COUNT];

static int st_vector_trampoline(void *user, const char *action, float x,
                                float y)
{
    st_sink_entry *e = user;
    int rc = e->vector_fn(e->user, action, x, y);
    if (rc == 0) {
        /* Um vetor chega todo quadro: a evidência é por BORDA — uma linha
         * quando o vetor sai do neutro (pressed=1) e uma quando volta
         * (pressed=0, em st_gptk_feed_vector), por gesto; a contagem por
         * entrega segue em e->deliveries para o diagnóstico. */
        /* Um vetor NULO entregue ao sink não é gesto: só uma deflexão real
         * abre o gesto (senão idle alternaria press/release todo quadro). */
        if (!e->vector_active && (x != 0.0f || y != 0.0f)) {
            st_log_delivery(e, "motion", 1, st_current_control);
            e->vector_active = 1;
            if (st_current_control >= 0 && st_current_control < (int)NXINPUT_GPTK_CONTROL_COUNT)
                st_active_vector[st_current_control] = e;
        }
        e->deliveries++;
    } else {
        fprintf(stderr, "[st/gptk] vector sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    }
    return rc;
}

int st_gptk_register_button(const char *action, const char *sink_id,
                            st_gptk_button_sink_fn fn, void *user)
{
    if (!st_gptk_loaded() || !fn || st_live_ready)
        return -1;
    st_sink_entry *e = st_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->button_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register(&st_live, action, st_button_trampoline,
                                   e) != 0)
        return -1;
    st_sink_count++;
    return 0;
}

int st_gptk_register_vector(const char *action, const char *sink_id,
                            st_gptk_vector_sink_fn fn, void *user)
{
    if (!st_gptk_loaded() || !fn || st_live_ready)
        return -1;
    st_sink_entry *e = st_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->vector_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register_vector(&st_live, action,
                                          st_vector_trampoline, e) != 0)
        return -1;
    st_sink_count++;
    return 0;
}

int st_gptk_seal(void)
{
    char error[160];
    if (!st_gptk_loaded() || st_live_ready)
        return -1;
    if (nxinput_gptk_live_seal(&st_live, error, sizeof error) != 0) {
        fprintf(stderr, "[st/gptk] selo recusado: %s — runtime fica "
                        "nativo (fail-safe)\n", error);
        return -1;
    }
    st_live_ready = 1;
    st_physical_down = 0;
    st_blocked_until_release = 0;
    /* Marcador do contrato controls.runtime_mapping=nxinput-gptk: a string é
     * referenciada por código vivo (nxinput_gptk_runtime_marker), nunca um
     * literal solto no binário. */
    fprintf(stderr,
            "[st/gptk] runtime=%s evidence=%s authority=NEXTOS_CONTROLLERS/%u "
            "source=%s sinks=%zu sha256=%.16s...\n",
            NXINPUT_GPTK4_RUNTIME_MARKER,
            nxinput_gptk_event_evidence_schema(),
            st_preinit.map.schema_version, st_gptk_source_name(),
            st_sink_count, st_preinit.receipt.selected_sha256);
    char line[640];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"runtime\",\"marker\":\"%s\","
             "\"mapping_sha256\":\"%s\",\"source\":\"%s\",\"gptk_schema\":%u,"
             "\"face_layout\":\"%s\",\"sinks\":%zu}",
             nxinput_gptk_event_evidence_schema(),
             NXINPUT_GPTK4_RUNTIME_MARKER,
             st_preinit.receipt.selected_sha256, st_gptk_source_name(),
             st_preinit.map.schema_version,
             nxinput_gptk_face_layout_name(st_preinit.face_layout),
             st_sink_count);
    st_receipt_line(line);
    return 0;
}

int st_gptk_sealed(void)
{
    return st_live_ready && !nxinput_gptk_live_is_fatal(&st_live);
}

void st_gptk_set_context(int context, const char *source)
{
    if (!st_gptk_sealed())
        return;
    int was = st_gptk_context();
    const char *was_source = nxinput_gptk_live_context_source(&st_live);
    if (was == context && was_source && source &&
        strcmp(was_source, source) == 0)
        return;
    /* Quarentena: o mesmo botão ainda segurado não pode nascer de novo no
     * contexto seguinte.  O runtime solta as ações latched do contexto
     * antigo em set_context (clear interno). */
    st_blocked_until_release |= st_physical_down;
    st_current_control = -1;
    if (nxinput_gptk_live_set_context(&st_live, (nxinput_gptk_context)context,
                                      source) != 0) {
        fprintf(stderr, "[st/gptk] contexto %d (%s) recusado; passthrough\n",
                context, source ? source : "");
        return;
    }
    fprintf(stderr, "[st/gptk] context=%s source=%s\n",
            nxinput_gptk_context_name(context), source ? source : "");
    char line[320];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"context\",\"context\":\"%s\","
             "\"source\":\"%s\",\"observed\":true}",
             nxinput_gptk_event_evidence_schema(),
             nxinput_gptk_context_name(context), source ? source : "");
    st_receipt_line(line);
}

void st_gptk_clear_context(const char *reason)
{
    if (!st_live_ready)
        return;
    if (!nxinput_gptk_live_context_proven(&st_live))
        return;
    st_blocked_until_release |= st_physical_down;
    st_current_control = -1;
    nxinput_gptk_live_clear_context(&st_live);
    fprintf(stderr, "[st/gptk] context=unproven reason=%s (passthrough)\n",
            reason ? reason : "");
}

int st_gptk_context(void)
{
    if (!st_live_ready || !nxinput_gptk_live_context_proven(&st_live))
        return -1;
    return (int)st_live.context;
}

const char *st_gptk_context_source(void)
{
    const char *s = st_live_ready
                  ? nxinput_gptk_live_context_source(&st_live) : NULL;
    return s ? s : "";
}

int st_gptk_feed_button(int control, int pressed, float value)
{
    if (control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return ST_GPTK_LIVE_PASSTHROUGH;
    uint32_t bit = UINT32_C(1) << (unsigned)control;
    int down = pressed != 0;
    int was_down = (st_physical_down & bit) != 0;

    /* Só a transição física alcança o runtime. */
    if (down == was_down)
        return st_gptk_should_consume(control) ? ST_GPTK_LIVE_DELIVERED
                                               : ST_GPTK_LIVE_PASSTHROUGH;
    if (down) {
        st_physical_down |= bit;
    } else {
        st_physical_down &= ~bit;
        if ((st_blocked_until_release & bit) != 0) {
            st_blocked_until_release &= ~bit;
            /* A soltura já foi entregue pelo runtime na troca de contexto;
             * o caminho nativo tampouco tem nada a soltar (não entregou). */
            return st_gptk_should_consume(control) ? ST_GPTK_LIVE_DELIVERED
                                                   : ST_GPTK_LIVE_PASSTHROUGH;
        }
    }
    if ((st_blocked_until_release & bit) != 0)
        return st_gptk_should_consume(control) ? ST_GPTK_LIVE_DELIVERED
                                               : ST_GPTK_LIVE_PASSTHROUGH;
    if (!st_gptk_sealed())
        return ST_GPTK_LIVE_PASSTHROUGH;
    st_current_control = control;
    int rc = (int)nxinput_gptk_live_feed(&st_live, control, down, value);
    if (rc == ST_GPTK_LIVE_FATAL)
        fprintf(stderr, "[st/gptk] FATAL: sink sem ACK para %s — runtime "
                        "invalidado, nada é reproduzido nativamente\n",
                nxinput_gptk_control_name(control));
    else if (rc == ST_GPTK_LIVE_SUPPRESSED && down) {
        /* `null` provado: a pressão física existiu e NADA foi entregue —
         * evidência tão importante quanto a entrega. */
        char line[320];
        const char *src = nxinput_gptk_live_context_source(&st_live);
        snprintf(line, sizeof line,
                 "{\"schema\":\"%s\",\"kind\":\"suppressed\",\"context\":\"%s\","
                 "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"press\","
                 "\"decision\":\"SUPPRESS\",\"delivery_count\":0}",
                 nxinput_gptk_event_evidence_schema(),
                 nxinput_gptk_context_name(st_live.context), src ? src : "",
                 nxinput_gptk_control_name(control));
        st_receipt_line(line);
        fprintf(stderr, "[st/gptk] %s = null: suprimido em %s\n",
                nxinput_gptk_control_name(control),
                nxinput_gptk_context_name(st_live.context));
    }
    return rc;
}

int st_gptk_feed_vector(int control, float x, float y)
{
    if (!st_gptk_sealed())
        return ST_GPTK_LIVE_PASSTHROUGH;
    st_current_control = control;
    if (x == 0.0f && y == 0.0f && control >= 0 && control < (int)NXINPUT_GPTK_CONTROL_COUNT &&
        st_active_vector[control]) {
        /* Volta ao neutro: fecha o gesto na evidência (nada latched). */
        st_sink_entry *e = st_active_vector[control];
        st_log_delivery(e, "motion", 0, control);
        e->vector_active = 0;
        st_active_vector[control] = NULL;
    }
    int rc = (int)nxinput_gptk_live_feed_vector(&st_live, control, x, y);
    if (rc == ST_GPTK_LIVE_FATAL)
        fprintf(stderr, "[st/gptk] FATAL: vector sink sem ACK para %s\n",
                nxinput_gptk_control_name(control));
    return rc;
}

void st_gptk_release_all(const char *reason)
{
    if (st_live_ready && nxinput_gptk_live_context_proven(&st_live)) {
        /* Solta latches do runtime sem trocar o contexto provado: o clear
         * emite release para toda ação latched; o contexto é re-provado no
         * próximo quadro pelo adapter. */
        nxinput_gptk_live_clear_context(&st_live);
        fprintf(stderr, "[st/gptk] release-all reason=%s\n",
                reason ? reason : "");
    }
    /* Gestos de vetor abertos fecham na evidência (volta ao neutro forçada). */
    for (int c = 0; c < (int)NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (st_active_vector[c]) {
            st_log_delivery(st_active_vector[c], "motion", 0, c);
            st_active_vector[c]->vector_active = 0;
            st_active_vector[c] = NULL;
        }
    }
    st_physical_down = 0;
    st_blocked_until_release = 0;
}

int st_gptk_should_consume(int control)
{
    if (!st_live_ready)
        return 0;
    return nxinput_gptk_live_should_consume(&st_live, control);
}

int st_gptk_decision(int control, const char **action_out)
{
    if (action_out)
        *action_out = NULL;
    if (!st_gptk_sealed() || !nxinput_gptk_live_context_proven(&st_live) ||
        control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return ST_GPTK_DECIDE_NONE;
    return (int)nxinput_gptk_decide(&st_preinit.map, st_live.context, control,
                                    action_out);
}

int st_gptk_fatal(void)
{
    return st_live_ready && nxinput_gptk_live_is_fatal(&st_live);
}

unsigned long st_gptk_delivery_count(void)
{
    return st_deliveries;
}

int st_gptk_cursor_tuning_copy(struct nxinput_gptk_cursor_tuning *out)
{
    if (!out)
        return -1;
    nxinput_gptk_cursor_tuning_get(st_gptk_loaded() ? &st_preinit.map : NULL,
                                   out);
    return st_gptk_loaded() ? 0 : -1;
}
