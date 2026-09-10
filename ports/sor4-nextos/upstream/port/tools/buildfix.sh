#!/bin/bash
# buildfix.sh - reconstroi o SOR4.dll de CONTROLES a partir do DLL-base do device
# (que ja tem patchgam+noopm(AndroidServices/save)+skipvideo+verstub+noopm(EOSManager.PollMessage)).
# Aplica POR CIMA: titleprobe (debug, remover no final) + rettrue(load_save) + noopm(promos/android NREs do menu).
# uso: buildfix.sh [--noprobe]   -> gera /tmp/SOR4.fix.dll
set -e
export DOTNET_ROOT=$HOME/.dotnet; export PATH=$HOME/.dotnet:$PATH
T=$HOME/nextos_ports_android/ports/sor4/port/tools
BASE=/tmp/SOR4.device.dll
OUT=/tmp/SOR4.fix.dll
cp "$BASE" "$OUT"
if [ "$1" != "--noprobe" ]; then
  dotnet $T/titleprobe/bin/titleprobe.dll "$OUT"
fi
# fix decisivo: titulo nunca sai de loading pq DidFinishLoadingCloudFiles (Android stub) = false p/ sempre
dotnet $T/rettrue/bin/rettrue.dll "$OUT" "CommonLib.platform::load_save_and_config_is_finished"
# NREs de servicos Android/Firebase no caminho do menu (promo "more games" etc) -> no-op
dotnet $T/noopm/bin/noopm.dll "$OUT" \
  MoreGamesNotificationUpdate
# TEMP/scaffold: video_exists()=false -> pula intro de stage (InterStageVideoScreen)
# p/ ALCANCAR e VALIDAR o gameplay. SERA SUBSTITUIDO por player de video real.
if [ "$2" = "--skipvideo" ] || [ "$1" = "--skipvideo" ]; then
  dotnet $T/noopm/bin/noopm.dll "$OUT" "platform.video_exists"
fi
# CO-OP LOCAL 2/3/4P (default ON, desliga com SOR4_COOP=0): trunca update_game_pad_state_array
# apos a Fase 1 + fia o handler de join -> cada pad fisico = 1 player. Validado; seguro p/ 1P.
if [ "${SOR4_COOP:-1}" = "1" ]; then
  dotnet $T/coopsplit/bin/coopsplit.dll "$OUT"
fi
echo "OK -> $OUT"
