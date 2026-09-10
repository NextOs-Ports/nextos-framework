/* SPDX-License-Identifier: GPL-3.0-only */
/* BLOSSOMTALES-CONTROLS-LIVE (1.4.0) — controle do jogo governado pelo
 * NEXTOSCONTROLLERS.gptk vivo (nxinput 0.10.2) sobre a SDL do firmware.
 *
 * Substitui o laço de entrada posicional do 1.3.0: o pad físico vira o
 * vocabulário simbólico do nxinput (união dos pads admitidos pela costura C6,
 * chord SELECT+START só no MESMO instance), o GPTK decide action/null/native
 * por contexto PROVADO pela engine (Game1.currentState + PauseScreen.IsPaused
 * lidos pelo embedding do Mono) e cada ação chega ao MonoGame pelo mesmo
 * KeyEvent Android que o caminho nativo usa. O cursor do analógico direito
 * (P1) e o R3 continuam nativos, como aprovado no 1.3.0. */
#ifndef BT_INPUT_H
#define BT_INPUT_H

#include <signal.h>

/* 1) Antes de QUALQUER SDL_Init (o bootstrap gráfico do bridge inicializa o
 * GAMECONTROLLER): lê owner/default do GPTK e encena a costura C6. */
int bt_input_preinit(const char *gamedir);

/* Vendor id apresentado ao jogo pelo InputDevice falso: segue o FACE_LAYOUT
 * do mapa selecionado (retro = Nintendo, modern/auto = Xbox) para os glyphs
 * do próprio jogo obedecerem à mesma autoridade do mapping. */
int bt_input_vendor_id(void);
int bt_input_product_id(void);

/* 2) Depois de SDL_InitSubSystem(GAMECONTROLLER): abre os pads admitidos,
 * registra sinks e sela o runtime vivo. Fail-closed: -1 aborta o port. */
int bt_input_init(void);

/* 3) Laço por quadro (thread principal). Bloqueia até o chord soberano, SIGTERM/
 * SIGINT (flag) ou SDL_QUIT. Os callbacks são os handlers marshalled do
 * MonoGameAndroidGameView já resolvidos pelo main.c. */
void bt_input_run_loop(void *env, void *view, void *down_handler,
                       void *up_handler, void *motion_handler,
                       void *touch_handler, volatile sig_atomic_t *exit_flag);

/* Sinks reais (símbolos exportados de propósito: o nxrelease liga o sink-id do
 * adapter-contract a um símbolo definido neste ELF). */
int bt_sink_android_keyevent(void *user, const char *action, int pressed,
                             float value);

/* Fornecido pelo main.c: leitura do estado da engine pelo embedding do Mono.
 * Devolve 1 quando o estado foi lido (state = nome curto da classe do
 * GameState corrente, paused = PauseScreen.IsPaused), 0 quando ainda não
 * provável (assembly/instância ausentes). Nunca presume. */
int sb_engine_state_probe(char *state, size_t state_cap, int *paused);

/* Fornecido pelo main.c: saída limpa (onPause -> save -> _exit(0)). */
void sb_pause_and_exit(void);

#endif /* BT_INPUT_H */
