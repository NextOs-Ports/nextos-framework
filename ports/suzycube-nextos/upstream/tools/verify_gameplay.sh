#!/usr/bin/env bash
# Prova de campo do Suzy Cube: abre o jogo NO DEVICE, deixa ele chegar sozinho
# na fase pelo fluxo nativo (menu -> slot -> mapa -> fase) e mede com o
# nx-verify enquanto a gameplay esta rodando.
#
# Regras respeitadas aqui:
#   - matar e CONFIRMAR o jogo anterior PELO DIRETORIO, com ps e fuser de root;
#   - jogo aberto a pedido sobrevive ao ssh (nohup ... < /dev/null &);
#   - nenhuma espera cega: cada laco tem TETO e mensagem de desistencia;
#   - o veredito vem do nx-verify, que se auto-testa antes de julgar.
set -uo pipefail
PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=framework-source.sh
source "$PORT_DIR/tools/framework-source.sh"
REPO=$(sc_resolve_framework_repository "$PORT_DIR")
DEV=${DEV:?set DEV to the IP authorized for this session}
SSH_USER=${SSH_USER:-root}
REMOTE=${SSH_USER}@${DEV}
DST=/storage/roms/ports/suzycube
WARMUP=${WARMUP:-150}      # segundos ate' a gameplay estar de pe'
# Roteiro de laboratorio: leva o jogo ate' a fase pelo FLUXO NATIVO.
SCRIPT=${SCRIPT:-700:start:10,1200:dpright:8,1500:dpleft:8,1900:a:8,2500:l1:8,2900:a:10,3600:dpright:2400}
SHOTS=${SHOTS:-4200,5200,6200}
# Entrada do EmulationStation por padrao: e' o caminho que o usuario usa.
ENTRY=${ENTRY:-/storage/roms/ports/Suzy Cube.sh}

say() { printf '[verify] %s\n' "$*"; }

say "matando instancia anterior (pelo diretorio) e confirmando"
timeout 90 ssh -o BatchMode=yes "$REMOTE" "
  for p in /proc/[0-9]*; do
    exe=\$(readlink \"\$p/exe\" 2>/dev/null)
    case \"\$exe\" in $DST/suzycube-nextos*) kill -TERM \${p##*/} 2>/dev/null ;; esac
  done
  n=0
  while [ \$n -lt 20 ]; do
    vivo=0
    for p in /proc/[0-9]*; do
      exe=\$(readlink \"\$p/exe\" 2>/dev/null)
      case \"\$exe\" in $DST/suzycube-nextos*) vivo=1 ;; esac
    done
    [ \$vivo -eq 0 ] && break
    sleep 0.5; n=\$((n+1))
  done
  for p in /proc/[0-9]*; do
    exe=\$(readlink \"\$p/exe\" 2>/dev/null)
    case \"\$exe\" in $DST/suzycube-nextos*) kill -KILL \${p##*/} 2>/dev/null ;; esac
  done
  echo '--- ps ---'; ps -o pid=,args= 2>/dev/null | grep -i suzycube | grep -v grep || echo 'nenhum suzycube'
  echo '--- fuser /dev/snd ---'; fuser /dev/snd/* 2>&1 || echo 'snd livre'
"

say "abrindo o jogo pela entrada do EmulationStation (sobrevive ao ssh)"
scp -q -o BatchMode=yes "$PORT_DIR/tools/launch_device.sh" \
        "$PORT_DIR/tools/watch_device.sh" "$REMOTE:/tmp/" || exit 1
timeout 60 ssh -o BatchMode=yes "$REMOTE" \
  "chmod +x /tmp/launch_device.sh /tmp/watch_device.sh && \
   /tmp/launch_device.sh '$SCRIPT' '$SHOTS' && \
   nohup /tmp/watch_device.sh $DST 60 10 < /dev/null > /dev/null 2>&1 & sleep 2"

say "aquecendo $WARMUP s ate' a gameplay (teto fixo, sem laco cego)"
timeout $((WARMUP + 60)) ssh -o BatchMode=yes "$REMOTE" "sleep $WARMUP; \
  ps -o pid=,args= 2>/dev/null | grep -i '$DST/suzycube-nextos' | grep -v grep || echo 'PROCESSO SUMIU'"

say "medindo com nx-verify"
"$REPO/tools/nx-verify" "$DEV" suzycube \
  --game "$DST/suzycube-nextos" --samples 4
rc=$?

say "recolhendo provas"
mkdir -p "$PORT_DIR/verify"
scp -q "$REMOTE:$DST/verify_shot.*.ppm" "$PORT_DIR/verify/" 2>/dev/null || true
scp -q "$REMOTE:$DST/log.txt" "$PORT_DIR/verify/gameplay.log" 2>/dev/null || true
say "nx-verify saiu com $rc"
exit $rc
