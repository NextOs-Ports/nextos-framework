/* SPDX-License-Identifier: GPL-3.0-only */
/* BLOSSOMTALES-CONTROLS-LIVE (1.4.0) — glue puro sobre o runtime vivo do
 * nxinput 0.10.2.  Ver input_gptk.h para o contrato.
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
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char bt_assert_none[BT_GPTK_DECIDE_NONE ==
                            (int)NXINPUT_GPTK_DECIDE_NONE ? 1 : -1];
typedef char bt_assert_action[BT_GPTK_DECIDE_ACTION ==
                              (int)NXINPUT_GPTK_DECIDE_ACTION ? 1 : -1];
typedef char bt_assert_suppress[BT_GPTK_DECIDE_SUPPRESS ==
                                (int)NXINPUT_GPTK_DECIDE_SUPPRESS ? 1 : -1];
typedef char bt_assert_native[BT_GPTK_DECIDE_NATIVE ==
                              (int)NXINPUT_GPTK_DECIDE_NATIVE ? 1 : -1];
typedef char bt_assert_menu[BT_GPTK_CONTEXT_MENU ==
                            (int)NXINPUT_GPTK_CONTEXT_MENU ? 1 : -1];
typedef char bt_assert_gameplay[BT_GPTK_CONTEXT_GAMEPLAY ==
                                (int)NXINPUT_GPTK_CONTEXT_GAMEPLAY ? 1 : -1];
typedef char bt_assert_cursor[BT_GPTK_CONTEXT_CURSOR ==
                              (int)NXINPUT_GPTK_CONTEXT_CURSOR ? 1 : -1];
typedef char bt_assert_pass[BT_GPTK_LIVE_PASSTHROUGH ==
                            (int)NXINPUT_GPTK_LIVE_PASSTHROUGH ? 1 : -1];
typedef char bt_assert_deliv[BT_GPTK_LIVE_DELIVERED ==
                             (int)NXINPUT_GPTK_LIVE_DELIVERED ? 1 : -1];
typedef char bt_assert_supp[BT_GPTK_LIVE_SUPPRESSED ==
                            (int)NXINPUT_GPTK_LIVE_SUPPRESSED ? 1 : -1];
typedef char bt_assert_fatal[BT_GPTK_LIVE_FATAL ==
                             (int)NXINPUT_GPTK_LIVE_FATAL ? 1 : -1];
typedef char bt_assert_mask_fits[NXINPUT_GPTK_CONTROL_COUNT <= 32 ? 1 : -1];

/* Allowlist = input.actions do adapter-contract.json deste port.  O loader
 * rejeita qualquer ação fora dela (NXI1001, fail-closed). */
/* NEXTOS_CONTROLLERS/4 (V5, nxinput 0.11.0): adapter contract for the owner
 * file; the schema-4 owner is projected onto the live V3 dispatch by
 * nxinput_gptk4_bridge (sinks and contexts unchanged). */
static const nxinput_gptk4_action_decl bt_gptk4_actions[] = {
    {"menu.accept", NXINPUT_GPTK4_V_DIGITAL},
    {"menu.back", NXINPUT_GPTK4_V_DIGITAL},
    {"player.attack", NXINPUT_GPTK4_V_DIGITAL},
    {"player.open_journal", NXINPUT_GPTK4_V_DIGITAL},
    {"player.open_menu", NXINPUT_GPTK4_V_DIGITAL},
    {"player.select_next", NXINPUT_GPTK4_V_DIGITAL},
    {"player.select_previous", NXINPUT_GPTK4_V_DIGITAL},
    {"player.use_item", NXINPUT_GPTK4_V_DIGITAL},
    {"player.use_tool", NXINPUT_GPTK4_V_DIGITAL},
    {"system.back", NXINPUT_GPTK4_V_DIGITAL},
    {"system.pause", NXINPUT_GPTK4_V_DIGITAL},
};
static const char *const bt_gptk4_contexts[] = {"menu", "cursor"};
static nxinput_gptk4 bt_gptk4_map;
static nxinput_gptk4_bridge_receipt bt_gptk4_receipt;

typedef struct bt_sink_entry {
    char action[NXINPUT_GPTK_ACTION_MAX + 1u];
    char sink_id[96];
    bt_gptk_button_sink_fn button_fn;
    bt_gptk_vector_sink_fn vector_fn;
    void *user;
    unsigned long deliveries;
    int pressed_by; /* controle simbólico que pressionou este sink (-1: nenhum) */
    int vector_active; /* vetor fora do neutro (evidência por BORDA: início e volta ao neutro) */
} bt_sink_entry;

#define BT_MAX_SINKS 32

static nxinput_gptk_preinit_result bt_preinit;
static int bt_preinit_done;
static nxinput_gptk_live bt_live;
static int bt_live_ready;
static bt_sink_entry bt_sinks[BT_MAX_SINKS];
static size_t bt_sink_count;
static uint32_t bt_physical_down;
static uint32_t bt_blocked_until_release;
static unsigned long bt_deliveries;
static FILE *bt_receipt;
static int bt_receipt_tried;

static FILE *bt_receipt_file(void)
{
    if (bt_receipt || bt_receipt_tried)
        return bt_receipt;
    bt_receipt_tried = 1;
    const char *path = getenv("NXGPTK_RECEIPT");
    if (!path || !*path)
        return NULL;
    bt_receipt = fopen(path, "a");
    if (bt_receipt)
        setvbuf(bt_receipt, NULL, _IOLBF, 0);
    return bt_receipt;
}

static void bt_receipt_line(const char *line)
{
    FILE *out = bt_receipt_file();
    if (out) {
        fputs(line, out);
        fputc('\n', out);
    }
}

int bt_gptk_preinit(const char *gamedir)
{
    if (bt_preinit_done)
        return 0;
    memset(&bt_preinit, 0, sizeof bt_preinit);
    bt_preinit_done = 1;
    bt_preinit.api_version = 1u;
    bt_preinit.struct_size = sizeof bt_preinit;
    bt_preinit.receipt.api_version = 1u;
    const char *dir = gamedir && *gamedir ? gamedir : ".";
    int owner_fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int defaults_fd = owner_fd >= 0
        ? openat(owner_fd, "defaults", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        : -1;
    nxinput_gptk4_contract contract;
    memset(&contract, 0, sizeof contract);
    contract.actions = bt_gptk4_actions;
    contract.count = sizeof bt_gptk4_actions / sizeof *bt_gptk4_actions;
    contract.contexts = bt_gptk4_contexts;
    contract.context_count = sizeof bt_gptk4_contexts / sizeof *bt_gptk4_contexts;
    int rc = nxinput_gptk4_load_project_at(owner_fd, defaults_fd, &contract,
                                           &bt_gptk4_map, &bt_preinit.map,
                                           &bt_gptk4_receipt);
    if (defaults_fd >= 0)
        close(defaults_fd);
    if (owner_fd >= 0)
        close(owner_fd);
    char json[1024];
    if (nxinput_gptk4_bridge_receipt_json(&bt_gptk4_receipt, json,
                                          sizeof json) == 0) {
        fprintf(stderr, "[bt/gptk] selection receipt: %s\n", json);
        bt_receipt_line(json);
    }
    bt_preinit.loaded = rc == 0;
    bt_preinit.rc = bt_gptk4_receipt.rc;
    bt_preinit.face_layout = 0;
    bt_preinit.receipt.source = (uint8_t)(
        bt_gptk4_receipt.source == NXINPUT_GPTK4_SRC_OWNER ? NXINPUT_GPTK_LOAD_OWNER
        : bt_gptk4_receipt.source == NXINPUT_GPTK4_SRC_DEFAULT
            ? (bt_gptk4_receipt.owner_rejected ? NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED
                                               : NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING)
            : NXINPUT_GPTK_LOAD_NONE);
    bt_preinit.receipt.owner_present = bt_gptk4_receipt.owner_present;
    bt_preinit.receipt.owner_error_code = bt_gptk4_receipt.owner_rejected ? bt_gptk4_receipt.rc : 0;
    bt_preinit.receipt.default_error_code = bt_gptk4_receipt.default_rejected ? bt_gptk4_receipt.rc : 0;
    bt_preinit.receipt.selected_gptk_schema = NXINPUT_GPTK_SCHEMA_V4;
    snprintf(bt_preinit.receipt.selected_sha256,
             sizeof bt_preinit.receipt.selected_sha256, "%s",
             bt_gptk4_receipt.sha256);
    if (!bt_preinit.loaded) {
        fprintf(stderr,
                "[bt/gptk] NXI%04d: sem NEXTOSCONTROLLERS/4 válido "
                "(%s:%u:%u %s) — port permanece nativo\n",
                bt_gptk4_receipt.rc, "NEXTOSCONTROLLERS.gptk",
                bt_gptk4_receipt.line, bt_gptk4_receipt.column,
                bt_gptk4_receipt.what);
        return 0;
    }
    /* Objeto vivo nasce UNPROVEN sobre o mesmo mapa do preinit; registros e
     * selo vêm depois, na ordem imposta pelos guards. */
    nxinput_gptk_live_init(&bt_live, &bt_preinit.map);
    fprintf(stderr,
            "[bt/gptk] preinit: NEXTOS_CONTROLLERS/%u source=%s layout=%s "
            "sha256=%.16s...\n",
            bt_preinit.map.schema_version, bt_gptk_source_name(),
            nxinput_gptk_face_layout_name(bt_preinit.face_layout),
            bt_preinit.receipt.selected_sha256);
    return 0;
}

int bt_gptk_loaded(void)
{
    return bt_preinit_done && bt_preinit.loaded;
}

int bt_gptk_face_layout(void)
{
    return bt_preinit_done ? (int)bt_preinit.face_layout : 0;
}

unsigned bt_gptk_schema(void)
{
    return bt_gptk_loaded() ? bt_preinit.map.schema_version : 0u;
}

const char *bt_gptk_selected_sha256(void)
{
    return bt_gptk_loaded() ? bt_preinit.receipt.selected_sha256 : "";
}

const char *bt_gptk_source_name(void)
{
    if (!bt_preinit_done)
        return "none";
    return nxinput_gptk_load_source_name(
        (nxinput_gptk_load_source)bt_preinit.receipt.source);
}

static bt_sink_entry *bt_sink_new(const char *action, const char *sink_id)
{
    if (!action || !sink_id || bt_sink_count >= BT_MAX_SINKS ||
        strlen(action) > NXINPUT_GPTK_ACTION_MAX ||
        strlen(sink_id) >= sizeof bt_sinks[0].sink_id)
        return NULL;
    bt_sink_entry *e = &bt_sinks[bt_sink_count];
    memset(e, 0, sizeof *e);
    e->pressed_by = -1;
    strcpy(e->action, action);
    strcpy(e->sink_id, sink_id);
    return e;
}

static int bt_current_control = -1;
static void bt_log_delivery(bt_sink_entry *e, const char *event,
                            int pressed, int control)
{
    e->deliveries++;
    bt_deliveries++;
    char line[512];
    const char *ctx = nxinput_gptk_context_name(bt_live.context);
    const char *src = nxinput_gptk_live_context_source(&bt_live);
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"delivery\",\"context\":\"%s\","
             "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"%s\","
             "\"decision\":\"ACTION\",\"action\":\"%s\",\"sink\":\"%s\","
             "\"pressed\":%d,\"delivery_count\":1}",
             nxinput_gptk_event_evidence_schema(), ctx ? ctx : "?",
             src ? src : "",
             control >= 0 ? nxinput_gptk_control_name(control) : "",
             event, e->action, e->sink_id, pressed ? 1 : 0);
    bt_receipt_line(line);
}

static int bt_button_trampoline(void *user, const char *action, int pressed,
                                float value)
{
    bt_sink_entry *e = user;
    int rc = e->button_fn(e->user, action, pressed, value);
    if (rc == 0) {
        /* A soltura pode nascer de uma troca de contexto (release do runtime),
         * fora de qualquer feed: ela pertence ao controle que PRESSIONOU. */
        if (pressed)
            e->pressed_by = bt_current_control;
        bt_log_delivery(e, "press", pressed, e->pressed_by);
        if (!pressed)
            e->pressed_by = -1;
    }
    else
        fprintf(stderr, "[bt/gptk] sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    return rc;
}

static bt_sink_entry *bt_active_vector[NXINPUT_GPTK_CONTROL_COUNT];

static int bt_vector_trampoline(void *user, const char *action, float x,
                                float y)
{
    bt_sink_entry *e = user;
    int rc = e->vector_fn(e->user, action, x, y);
    if (rc == 0) {
        /* Um vetor chega todo quadro: a evidência é por BORDA — uma linha
         * quando o vetor sai do neutro (pressed=1) e uma quando volta
         * (pressed=0, em bt_gptk_feed_vector), por gesto; a contagem por
         * entrega segue em e->deliveries para o diagnóstico. */
        /* Um vetor NULO entregue ao sink não é gesto: só uma deflexão real
         * abre o gesto (senão idle alternaria press/release todo quadro). */
        if (!e->vector_active && (x != 0.0f || y != 0.0f)) {
            bt_log_delivery(e, "motion", 1, bt_current_control);
            e->vector_active = 1;
            if (bt_current_control >= 0 && bt_current_control < (int)NXINPUT_GPTK_CONTROL_COUNT)
                bt_active_vector[bt_current_control] = e;
        }
        e->deliveries++;
    } else {
        fprintf(stderr, "[bt/gptk] vector sink %s recusou ACK (rc=%d)\n",
                e->sink_id, rc);
    }
    return rc;
}

int bt_gptk_register_button(const char *action, const char *sink_id,
                            bt_gptk_button_sink_fn fn, void *user)
{
    if (!bt_gptk_loaded() || !fn || bt_live_ready)
        return -1;
    bt_sink_entry *e = bt_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->button_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register(&bt_live, action, bt_button_trampoline,
                                   e) != 0)
        return -1;
    bt_sink_count++;
    return 0;
}

int bt_gptk_register_vector(const char *action, const char *sink_id,
                            bt_gptk_vector_sink_fn fn, void *user)
{
    if (!bt_gptk_loaded() || !fn || bt_live_ready)
        return -1;
    bt_sink_entry *e = bt_sink_new(action, sink_id);
    if (!e)
        return -1;
    e->vector_fn = fn;
    e->user = user;
    if (nxinput_gptk_live_register_vector(&bt_live, action,
                                          bt_vector_trampoline, e) != 0)
        return -1;
    bt_sink_count++;
    return 0;
}

int bt_gptk_seal(void)
{
    char error[160];
    if (!bt_gptk_loaded() || bt_live_ready)
        return -1;
    if (nxinput_gptk_live_seal(&bt_live, error, sizeof error) != 0) {
        fprintf(stderr, "[bt/gptk] selo recusado: %s — runtime fica "
                        "nativo (fail-safe)\n", error);
        return -1;
    }
    bt_live_ready = 1;
    bt_physical_down = 0;
    bt_blocked_until_release = 0;
    /* Marcador do contrato controls.runtime_mapping=nxinput-gptk: a string é
     * referenciada por código vivo (nxinput_gptk_runtime_marker), nunca um
     * literal solto no binário. */
    fprintf(stderr,
            "[bt/gptk] runtime=%s evidence=%s authority=NEXTOS_CONTROLLERS/%u "
            "source=%s sinks=%zu sha256=%.16s...\n",
            NXINPUT_GPTK4_RUNTIME_MARKER,
            nxinput_gptk_event_evidence_schema(),
            bt_preinit.map.schema_version, bt_gptk_source_name(),
            bt_sink_count, bt_preinit.receipt.selected_sha256);
    char line[640];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"runtime\",\"marker\":\"%s\","
             "\"mapping_sha256\":\"%s\",\"source\":\"%s\",\"gptk_schema\":%u,"
             "\"face_layout\":\"%s\",\"sinks\":%zu}",
             nxinput_gptk_event_evidence_schema(),
             NXINPUT_GPTK4_RUNTIME_MARKER,
             bt_preinit.receipt.selected_sha256, bt_gptk_source_name(),
             bt_preinit.map.schema_version,
             nxinput_gptk_face_layout_name(bt_preinit.face_layout),
             bt_sink_count);
    bt_receipt_line(line);
    return 0;
}

int bt_gptk_sealed(void)
{
    return bt_live_ready && !nxinput_gptk_live_is_fatal(&bt_live);
}

void bt_gptk_set_context(int context, const char *source)
{
    if (!bt_gptk_sealed())
        return;
    int was = bt_gptk_context();
    const char *was_source = nxinput_gptk_live_context_source(&bt_live);
    if (was == context && was_source && source &&
        strcmp(was_source, source) == 0)
        return;
    /* Quarentena: o mesmo botão ainda segurado não pode nascer de novo no
     * contexto seguinte.  O runtime solta as ações latched do contexto
     * antigo em set_context (clear interno). */
    bt_blocked_until_release |= bt_physical_down;
    bt_current_control = -1;
    if (nxinput_gptk_live_set_context(&bt_live, (nxinput_gptk_context)context,
                                      source) != 0) {
        fprintf(stderr, "[bt/gptk] contexto %d (%s) recusado; passthrough\n",
                context, source ? source : "");
        return;
    }
    fprintf(stderr, "[bt/gptk] context=%s source=%s\n",
            nxinput_gptk_context_name(context), source ? source : "");
    char line[320];
    snprintf(line, sizeof line,
             "{\"schema\":\"%s\",\"kind\":\"context\",\"context\":\"%s\","
             "\"source\":\"%s\",\"observed\":true}",
             nxinput_gptk_event_evidence_schema(),
             nxinput_gptk_context_name(context), source ? source : "");
    bt_receipt_line(line);
}

void bt_gptk_clear_context(const char *reason)
{
    if (!bt_live_ready)
        return;
    if (!nxinput_gptk_live_context_proven(&bt_live))
        return;
    bt_blocked_until_release |= bt_physical_down;
    bt_current_control = -1;
    nxinput_gptk_live_clear_context(&bt_live);
    fprintf(stderr, "[bt/gptk] context=unproven reason=%s (passthrough)\n",
            reason ? reason : "");
}

int bt_gptk_context(void)
{
    if (!bt_live_ready || !nxinput_gptk_live_context_proven(&bt_live))
        return -1;
    return (int)bt_live.context;
}

const char *bt_gptk_context_source(void)
{
    const char *s = bt_live_ready
                  ? nxinput_gptk_live_context_source(&bt_live) : NULL;
    return s ? s : "";
}

int bt_gptk_feed_button(int control, int pressed, float value)
{
    if (control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return BT_GPTK_LIVE_PASSTHROUGH;
    uint32_t bit = UINT32_C(1) << (unsigned)control;
    int down = pressed != 0;
    int was_down = (bt_physical_down & bit) != 0;

    /* Só a transição física alcança o runtime. */
    if (down == was_down)
        return bt_gptk_should_consume(control) ? BT_GPTK_LIVE_DELIVERED
                                               : BT_GPTK_LIVE_PASSTHROUGH;
    if (down) {
        bt_physical_down |= bit;
    } else {
        bt_physical_down &= ~bit;
        if ((bt_blocked_until_release & bit) != 0) {
            bt_blocked_until_release &= ~bit;
            /* A soltura já foi entregue pelo runtime na troca de contexto;
             * o caminho nativo tampouco tem nada a soltar (não entregou). */
            return bt_gptk_should_consume(control) ? BT_GPTK_LIVE_DELIVERED
                                                   : BT_GPTK_LIVE_PASSTHROUGH;
        }
    }
    if ((bt_blocked_until_release & bit) != 0)
        return bt_gptk_should_consume(control) ? BT_GPTK_LIVE_DELIVERED
                                               : BT_GPTK_LIVE_PASSTHROUGH;
    if (!bt_gptk_sealed())
        return BT_GPTK_LIVE_PASSTHROUGH;
    bt_current_control = control;
    int rc = (int)nxinput_gptk_live_feed(&bt_live, control, down, value);
    if (rc == BT_GPTK_LIVE_FATAL)
        fprintf(stderr, "[bt/gptk] FATAL: sink sem ACK para %s — runtime "
                        "invalidado, nada é reproduzido nativamente\n",
                nxinput_gptk_control_name(control));
    else if (rc == BT_GPTK_LIVE_SUPPRESSED && down) {
        /* `null` provado: a pressão física existiu e NADA foi entregue —
         * evidência tão importante quanto a entrega. */
        char line[320];
        const char *src = nxinput_gptk_live_context_source(&bt_live);
        snprintf(line, sizeof line,
                 "{\"schema\":\"%s\",\"kind\":\"suppressed\",\"context\":\"%s\","
                 "\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"press\","
                 "\"decision\":\"SUPPRESS\",\"delivery_count\":0}",
                 nxinput_gptk_event_evidence_schema(),
                 nxinput_gptk_context_name(bt_live.context), src ? src : "",
                 nxinput_gptk_control_name(control));
        bt_receipt_line(line);
        fprintf(stderr, "[bt/gptk] %s = null: suprimido em %s\n",
                nxinput_gptk_control_name(control),
                nxinput_gptk_context_name(bt_live.context));
    }
    return rc;
}

int bt_gptk_feed_vector(int control, float x, float y)
{
    if (!bt_gptk_sealed())
        return BT_GPTK_LIVE_PASSTHROUGH;
    bt_current_control = control;
    if (x == 0.0f && y == 0.0f && control >= 0 && control < (int)NXINPUT_GPTK_CONTROL_COUNT &&
        bt_active_vector[control]) {
        /* Volta ao neutro: fecha o gesto na evidência (nada latched). */
        bt_sink_entry *e = bt_active_vector[control];
        bt_log_delivery(e, "motion", 0, control);
        e->vector_active = 0;
        bt_active_vector[control] = NULL;
    }
    int rc = (int)nxinput_gptk_live_feed_vector(&bt_live, control, x, y);
    if (rc == BT_GPTK_LIVE_FATAL)
        fprintf(stderr, "[bt/gptk] FATAL: vector sink sem ACK para %s\n",
                nxinput_gptk_control_name(control));
    return rc;
}

void bt_gptk_release_all(const char *reason)
{
    if (bt_live_ready && nxinput_gptk_live_context_proven(&bt_live)) {
        /* Solta latches do runtime sem trocar o contexto provado: o clear
         * emite release para toda ação latched; o contexto é re-provado no
         * próximo quadro pelo adapter. */
        nxinput_gptk_live_clear_context(&bt_live);
        fprintf(stderr, "[bt/gptk] release-all reason=%s\n",
                reason ? reason : "");
    }
    /* Gestos de vetor abertos fecham na evidência (volta ao neutro forçada). */
    for (int c = 0; c < (int)NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (bt_active_vector[c]) {
            bt_log_delivery(bt_active_vector[c], "motion", 0, c);
            bt_active_vector[c]->vector_active = 0;
            bt_active_vector[c] = NULL;
        }
    }
    bt_physical_down = 0;
    bt_blocked_until_release = 0;
}

int bt_gptk_should_consume(int control)
{
    if (!bt_live_ready)
        return 0;
    return nxinput_gptk_live_should_consume(&bt_live, control);
}

int bt_gptk_decision(int control, const char **action_out)
{
    if (action_out)
        *action_out = NULL;
    if (!bt_gptk_sealed() || !nxinput_gptk_live_context_proven(&bt_live) ||
        control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
        return BT_GPTK_DECIDE_NONE;
    return (int)nxinput_gptk_decide(&bt_preinit.map, bt_live.context, control,
                                    action_out);
}

int bt_gptk_fatal(void)
{
    return bt_live_ready && nxinput_gptk_live_is_fatal(&bt_live);
}

unsigned long bt_gptk_delivery_count(void)
{
    return bt_deliveries;
}

int bt_gptk_cursor_tuning_copy(struct nxinput_gptk_cursor_tuning *out)
{
    if (!out)
        return -1;
    nxinput_gptk_cursor_tuning_get(bt_gptk_loaded() ? &bt_preinit.map : NULL,
                                   out);
    return bt_gptk_loaded() ? 0 : -1;
}

