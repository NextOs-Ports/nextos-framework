#!/bin/sh
# port-env.sh -- ambiente do Blossom Tales, lido pelo launcher do framework.
#
# O launcher gerado ja resolve GAMEDIR, HOME, LD_LIBRARY_PATH do host e o
# mapeamento de controle da CFW. Aqui entra so o que e' deste port.
#
# A resolucao NAO e' cravada: o loader usa o modo de video que o SDL reportar.
# SB_WIDTH/SB_HEIGHT ficam de fora de proposito.

# Onde o so-loader acha as libs Android e os dados extraidos do APK do dono.
export SB_LIBDIR="$GAMEDIR/libs"
export SB_ASSET_DIR="$GAMEDIR/assets"

# Saves e preferencias do jogador ficam FORA da arvore selada pelo NXExtract.
# Dentro de libs/ o save quebrava o selo do marker no segundo boot e uma
# reinstalacao de dados levaria o save junto.
export SB_DATADIR="$GAMEDIR/home"

# Contrato do bootstrap Mono/.NET-for-Android desta build (ver README).
export SB_RUNTIME_INIT=1
export SB_START_ACTIVITY=1
export SB_HOLD=1
export SB_JNI_VERBOSE=0

# Apresentacao por FBO proprio: e' o que o Mali-450 Utgard exige. Medido tambem
# num Mali Bifrost/ES3.2, onde nao custa quadro (59,9 fps, 0/300 atrasados).
export SB_PRESENT_FBO=1
export SB_FBO_TRACK=1
export SB_RIGHT_CURSOR=0

# O Mono e' classe de OOM conhecida em aparelho de 1 GB; limitar as arenas do
# malloc segura o RSS sem custo de quadro.
export MALLOC_ARENA_MAX=2

# As libs Android do jogo ficam ao lado do binario. A SDL, o EGL e o OpenAL
# continuam sendo os do sistema: nada aqui os sombreia.
export LD_LIBRARY_PATH="$GAMEDIR/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ArkOS/dArkOS nao acham o EGL do aparelho sem esta dica ao SDL. So e'
# definida quando ninguem definiu antes -- a CFW continua tendo a palavra.
[ -n "${SDL_VIDEO_EGL_DRIVER:-}" ] || export SDL_VIDEO_EGL_DRIVER=libEGL.so

# Saida de audio: SFX pelo OpenAL e musica pelo MediaPlayer do so-loader, os
# dois no mesmo servidor de som do sistema. Sem servidor, o OpenAL usa a ALSA.
for nx_socket in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$nx_socket" ]; then
    export PULSE_SERVER="unix:$nx_socket"
    break
  fi
done
unset nx_socket

# nxinput C6 seam (V5): provider conhecido encena a linha do launcher antes do
# SDL_Init para tradução tipada; provider desconhecido deixa a linha no ambiente
# para a importação stock da própria SDL. Authority 2 = database do CFW;
# authority 3 = controllers.nxb pinado neste ZIP. Receipts são diagnóstico.
export NXC6_SEAM=1
export NXC6_RECEIPT="$GAMEDIR/nxc6-receipt.log"
# Evidência diagnóstica do runtime GPTK vivo: um arquivo por execução.
export NXGPTK_RECEIPT="$GAMEDIR/nxgptk-receipt.jsonl"
: > "$NXGPTK_RECEIPT" 2>/dev/null || true
export NXVIDEO_RECEIPT="$GAMEDIR/nxvideo-receipt.log"
: > "$NXVIDEO_RECEIPT" 2>/dev/null || true
