#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Gate do reparo de provedor grafico por CRITERIO OBSERVADO.
#
# O bug que isto impede: com os SONAMEs cruzados, o binario se liga a uma Mesa
# sem driver, ganha um contexto que aceita toda chamada e nao desenha nada.
# Audio, input e o laco seguem vivos, o processo sai 0 e o painel fica preto,
# sem erro em lugar nenhum. A unica condicao segura e observavel e' a string de
# renderer VAZIA medida no contexto real.
#
# Por isso o cenario mais importante aqui e' o NEGATIVO: renderer saudavel tem
# de sair sem tocar em nada. Um reparo disparado por engano num aparelho que ja'
# funcionava seria pior que o bug.
#
# Nenhum cenario toca biblioteca do sistema: tudo em diretorio temporario.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXGL=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

SDL_CFLAGS=$(pkg-config --cflags sdl2 2>/dev/null || true)
SDL_LIBS=$(pkg-config --libs sdl2 2>/dev/null || true)
if [ -z "$SDL_LIBS" ]; then
  echo "nxgl_provider_discovery=SKIP sem SDL2 no host" >&2
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

$CC -shared -fPIC -o "$WORK/lib/libmali.so" "$HERE/fake_unified_provider.c" \
  2>/dev/null || { mkdir -p "$WORK/lib"; \
  $CC -shared -fPIC -o "$WORK/lib/libmali.so" "$HERE/fake_unified_provider.c"; }
mkdir -p "$WORK/so-egl"
$CC -shared -fPIC -o "$WORK/so-egl/libGLESv1_CM.so.1" \
  "$HERE/fake_split_provider.c"
mkdir -p "$WORK/vazio"

$CC -std=gnu99 -D_GNU_SOURCE $SDL_CFLAGS \
  -I "$NXGL/include" -I "$NXGL/adapters" -I "$NXGL/src" \
  -o "$WORK/probe" "$HERE/test_provider_discovery.c" \
  "$NXGL/adapters/nxgl_provider_discovery_adapter.c" \
  "$NXGL/src/nxgl_provider_recovery.c" "$NXGL/src/nxgl_arbiter.c" \
  "$NXGL/src/nxgl_logic.c" "$NXGL/src/nxgl_sdl2.c" \
  "$NXGL/src/nxgl_diagnostics.c" "$NXGL/src/nxgl_metrics.c" \
  "$NXGL/src/nxgl_present.c" "$NXGL/src/nxgl_sdl_hint.c" \
  $SDL_LIBS -ldl

fail() { echo "nxgl_provider_discovery=FAIL $*" >&2; exit 1; }
expect() { [ "$2" = "$1" ] || fail "$3: esperado [$1], veio [$2]"; }

# 1. criterio puro: string vazia e NULL sao "quebrado"; qualquer renderer real
#    nao e'.
expect "broken=1" "$("$WORK/probe" broken '')"          "renderer vazio"
expect "broken=0" "$("$WORK/probe" broken 'Mali-G31')"  "renderer real"
expect "broken=0" "$("$WORK/probe" broken 'Mali-450 MP')" "renderer Utgard"

# 2. descoberta: o objeto que traz EGL **e** GLES no mesmo arquivo e' aceito
expect "discover=1 name=libmali.so" "$("$WORK/probe" discover "$WORK/lib")" \
  "blob unificado"

# 3. o objeto que so' tem GLES -- o formato da Mesa sem driver -- e' RECUSADO,
#    mesmo tendo nome de familia grafica
expect "discover=0 name=-" "$("$WORK/probe" discover "$WORK/so-egl")" \
  "so GLES, sem EGL"

# 4. diretorio sem nada
expect "discover=0 name=-" "$("$WORK/probe" discover "$WORK/vazio")" "vazio"

# 5. RENDERER SAUDAVEL NAO E' TOCADO -- nem teardown, nem reparo. Este e' o
#    cenario que protege todo aparelho que ja' funciona.
expect "outcome=0 teardown=0" \
  "$("$WORK/probe" repair "$WORK/lib" 'Mali-450 MP')" "renderer saudavel"

# 6. renderer vazio e nenhum candidato: sai sem tocar em nada
expect "outcome=1 teardown=0" \
  "$("$WORK/probe" repair "$WORK/vazio" '')" "sem candidato"

# 7. renderer vazio COM candidato: autoriza, faz o teardown e tenta o re-exec.
#    Sem argv o exec nao acontece, entao o resultado observavel e' a falha
#    controlada com o ambiente restaurado -- que e' o que o gate pode provar
#    sem trocar o proprio processo.
out=$(env -u NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED \
      "$WORK/probe" repair "$WORK/lib" '')
case $out in
  "outcome=3 teardown=1") ;;
  "outcome=1 teardown=0") fail "candidato valido foi recusado: $out" ;;
  *) fail "reparo: $out" ;;
esac

# 8. marcador presente: uma tentativa por processo, nunca duas
expect "outcome=0 teardown=0" \
  "$(NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED=1 "$WORK/probe" repair "$WORK/lib" '')" \
  "segunda tentativa"

# 9. PRE-CONTEXTO: a janela nem existiu, entao nao ha renderer para medir.
#    Com um candidato valido e sem hint herdado, o plano estreito autoriza.
out=$(env -u NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED \
      -u SDL_VIDEO_EGL_DRIVER -u SDL_VIDEO_GL_DRIVER \
      "$WORK/probe" precontext "$WORK/lib" '')
case $out in
  "outcome=3 teardown=1") ;;
  *) fail "pre-contexto com candidato: $out" ;;
esac

# 10. HINT HERDADO MANDA MAIS QUE O REPARO. Se a CFW (ou o usuario) ja' escolheu
#     um provedor, respeitar essa escolha vem antes -- reparar por cima seria
#     quebrar um aparelho que talvez ja' estivesse certo.
out=$(env -u NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED \
      SDL_VIDEO_EGL_DRIVER=libEGL.so \
      "$WORK/probe" precontext "$WORK/lib" '')
expect "outcome=0 teardown=0" "$out" "hint herdado respeitado"

# 11. ROLLBACK NO PROCESSO RE-EXECUTADO (caso de campo FF4 3D/ROCKNIX): o
#     re-exec aplicou o par e a TENTATIVA REAL falhou. O rollback desamarra os
#     dois envs UMA vez (a segunda chamada nao faz nada) e o chamador repete
#     pela pilha da firmware -- o erro final volta a ser o verdadeiro. A sonda
#     EGL propria da 0.2.11 foi removida: reprovava blob -gbm BOM (display
#     nulo no default display, medido no aparelho dArkOS em 22/08/2026).
out=$(env NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED=1 \
      SDL_VIDEO_EGL_DRIVER=/x/libmali.so SDL_VIDEO_GL_DRIVER=/x/libmali.so \
      "$WORK/probe" rollback)
expect "rollback=1 again=0 egl=- gl=-" "$out" "rollback desamarra uma vez"

# 12. sem marcador de reparo aplicado, o rollback NAO toca no ambiente (um
#     hint herdado da CFW jamais pode ser removido por engano)
out=$(env -u NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED -u SDL_VIDEO_GL_DRIVER \
      SDL_VIDEO_EGL_DRIVER=libEGL.so "$WORK/probe" rollback)
expect "rollback=0 again=0 egl=libEGL.so gl=-" "$out" "sem marcador, sem rollback"

# 13. marcador presente mas par ausente: nada a desfazer
out=$(env -u SDL_VIDEO_EGL_DRIVER -u SDL_VIDEO_GL_DRIVER \
      NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED=1 "$WORK/probe" rollback)
expect "rollback=0 again=0 egl=- gl=-" "$out" "marcador sem par"

# 14. HINT DESPROVADO (caso de campo dArkOS/ES-launch): a firmware exporta
#     SDL_VIDEO_EGL_DRIVER no unit do frontend; o contexto criado sob esse
#     hint devolve renderer NULO -- medicao viva de que o hint esta' errado
#     NESTE boot. O reparo reativo entao desamarra o hint e re-executa com o
#     par coerente (aqui sem argv o exec falha controlado: outcome=3
#     teardown=1) e o hint volta ao ambiente para o post-mortem. Sem isso,
#     todo lancamento pelo menu ficava preto com som.
out=$(env -u NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED \
      SDL_VIDEO_EGL_DRIVER=libEGL.so \
      "$WORK/probe" repair-hint "$WORK/lib" '')
expect "outcome=3 teardown=1 egl=libEGL.so gl=-" "$out" "hint desprovado sobrescrito e restaurado"

# 15. o PRE-CONTEXTO continua respeitando o hint (sem criterio vivo, a
#     escolha da CFW e' soberana -- cenario 10 acima segue de guarda).

echo "nxgl_provider_discovery=PASS 14 cenarios"
