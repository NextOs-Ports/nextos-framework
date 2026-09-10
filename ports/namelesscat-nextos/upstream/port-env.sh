#!/usr/bin/env bash
# Runtime contract proven on NextOS Elite. The generated launcher sources this
# only after the mandatory NXSplash and immediately before the game.
export ST_NONNULL_OBJECT_FALLBACK=1
export ST_PLAY_GAMES_OFFLINE=1
export ST_CURSOR=1
export ST_NO_ADS=1
# nxinput C6 seam (V4-CONTROLLERS-03): the firmware SDL2 path admits the pad before
# announcing it, resolving the sovereign PortMaster mapping (authority 1 = the
# launcher's get_controls line, staged out of SDL_GAMECONTROLLERCONFIG by the
# loader before SDL_Init; authority 2 = SDL_GAMECONTROLLERCONFIG_FILE, the CFW
# database).  Receipts are appended next to the game log.
export NXC6_SEAM=1
export NXC6_RECEIPT="$GAMEDIR/nxc6-receipt.log"
# Evidência do runtime GPTK vivo (nxinput-gptk-event-evidence/1): um arquivo por
# execução, na pasta do jogo; é a única fonte aceita para o candidate-lock (botões
# físicos, nunca estímulo sintético).
export NXGPTK_RECEIPT="$GAMEDIR/nxgptk-receipt.jsonl"
: > "$NXGPTK_RECEIPT" 2>/dev/null || true
