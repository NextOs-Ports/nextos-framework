/* SPDX-License-Identifier: GPL-3.0-only */
/* PARTYHARD-CONTROLS-LIVE (1.0.0) — NEXTOSCONTROLLERS.gptk como runtime.
 *
 * Módulo PURO: nada de SDL, nada de nós de dispositivo, nada de teclado.
 * Ele embrulha a fronteira viva canônica do nxinput 0.10.2:
 *
 *   nxinput_gptk_preinit_load()  -> mapa owner/default lido UMA vez, antes de
 *                                   bundle/staging/SDL_Init (FACE_LAYOUT V3)
 *   nxinput_gptk_live_*          -> registro de sinks com ACK, selo, contexto
 *                                   PROVADO pela engine, feed edge-triggered
 *
 * Política fail-safe do contrato nxinput-gptk-live/1: até o selo e um contexto
 * provado, TODO controle é PASSTHROUGH e o caminho nativo Android é a
 * autoridade. `null` só suprime dentro de contexto provado. Um sink que falha
 * o ACK é FATAL: nunca é reproduzido nativamente.
 */
#ifndef ST_INPUT_GPTK_H
#define ST_INPUT_GPTK_H

#include <stddef.h>

/* Espelhos dos valores públicos do framework (verificados em tempo de
 * compilação em input_gptk.c). */
#define ST_GPTK_DECIDE_NONE 0
#define ST_GPTK_DECIDE_ACTION 1
#define ST_GPTK_DECIDE_SUPPRESS 2
#define ST_GPTK_DECIDE_NATIVE 3

#define ST_GPTK_CONTEXT_MENU 0
#define ST_GPTK_CONTEXT_GAMEPLAY 1
#define ST_GPTK_CONTEXT_CURSOR 2

#define ST_GPTK_LIVE_PASSTHROUGH 0
#define ST_GPTK_LIVE_DELIVERED 1
#define ST_GPTK_LIVE_SUPPRESSED 2
#define ST_GPTK_LIVE_FATAL (-1)

/* Sinks do adapter. Devolvem 0 SOMENTE quando a fronteira aceitou o evento
 * (ACK); qualquer outro valor é falha terminal do runtime vivo. */
typedef int (*st_gptk_button_sink_fn)(void *user, const char *action,
                                      int pressed, float value);
typedef int (*st_gptk_vector_sink_fn)(void *user, const char *action,
                                      float x, float y);

/* 1) Pré-init: lê owner/default do gamedir exatamente uma vez. Devolve 0
 * quando a fronteira rodou (mesmo sem mapa válido: o port fica nativo) e -1
 * só em argumento estruturalmente inválido. */
int st_gptk_preinit(const char *gamedir);
int st_gptk_loaded(void);
/* Layout do preâmbulo V3 (NXC6_FACE_LAYOUT_*); AUTO quando não carregado. */
int st_gptk_face_layout(void);
unsigned st_gptk_schema(void);
const char *st_gptk_selected_sha256(void);
const char *st_gptk_source_name(void);

/* 2) Sinks: um por (ação, sink-id do adapter-contract). */
int st_gptk_register_button(const char *action, const char *sink_id,
                            st_gptk_button_sink_fn fn, void *user);
int st_gptk_register_vector(const char *action, const char *sink_id,
                            st_gptk_vector_sink_fn fn, void *user);
/* 3) Selo: só depois de TODAS as ações mapeadas terem sink do tipo certo.
 * Publica marcador de runtime e recibos. Sem selo o runtime não consome. */
int st_gptk_seal(void);
int st_gptk_sealed(void);

/* 4) Contexto provado pela engine (rótulo curto, sem caminho). Contexto
 * desconhecido = clear = passthrough. Toda troca solta ações latched. */
void st_gptk_set_context(int context, const char *source);
void st_gptk_clear_context(const char *reason);
int st_gptk_context(void);          /* -1 quando não provado */
const char *st_gptk_context_source(void);

/* 5) Feed. Botões: só a TRANSIÇÃO física chega ao runtime; um botão ainda
 * segurado numa troca de contexto fica em quarentena até a soltura real.
 * Devolve ST_GPTK_LIVE_*. */
int st_gptk_feed_button(int control, int pressed, float value);
int st_gptk_feed_vector(int control, float x, float y);
/* Solta tudo que estiver fisicamente pressionado (hotplug, foco, saída). */
void st_gptk_release_all(const char *reason);

/* Consulta única antes de suprimir o caminho nativo: verdadeiro só com selo +
 * contexto provado e decisão ACTION/SUPPRESS. */
int st_gptk_should_consume(int control);
int st_gptk_decision(int control, const char **action_out);
int st_gptk_fatal(void);

/* Tuning do [cursor] do mapa selecionado (defaults quando ausente); -1 sem
 * mapa carregado. O tipo é o do framework (declaração adiantada). */
struct nxinput_gptk_cursor_tuning;
int st_gptk_cursor_tuning_copy(struct nxinput_gptk_cursor_tuning *out);

/* Contagem de entregas por (contexto, controle, ação, sink) — evidência
 * externa nxinput-gptk-event-evidence/1, escrita no NXGPTK_RECEIPT. */
unsigned long st_gptk_delivery_count(void);

#endif /* ST_INPUT_GPTK_H */
