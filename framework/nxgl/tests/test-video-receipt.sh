#!/usr/bin/env bash
# Directed nxgl 0.3.2 video-proof gate. Fake GL only: no device, display,
# window, network or full framework battery.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nx-video-receipt.XXXXXX")
trap 'rm -rf "$TEST_ROOT"' EXIT
BIN=$TEST_ROOT/harness

fail() { printf 'video-receipt test FAIL: %s\n' "$1" >&2; exit 1; }

cc -Wall -Wextra -Werror -O1 -o "$BIN" \
  "$HERE/test_video_receipt.c" "$HERE/../adapters/nxgl_frame_proof_adapter.c" \
  -ldl || fail "harness did not compile"

run_case() { # $1 scenario
  env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT NXLAUNCH_FRONTEND=1 \
    "$BIN" "$1"
}

out=$(run_case ok)
grep -q 'frame_proof=100.0% verdict=OK reason=none sample_point=before-present' <<<"$out" || \
  fail "OK before-present ausente: $out"
grep -q 'GL-ERROR-QUEUE: untouched' <<<"$out" || \
  fail "OK consumiu a fila GL: $out"
grep -q 'PROOF-LIFECYCLE: fatal=0 close=0 close_again=0' <<<"$out" || \
  fail "OK solicitou fechamento: $out"

# glReadPixels sem escrita e escrita parcial nunca classificam o sentinel
# inicializado. A fila de erro preexistente do jogo permanece intocada.
out=$(run_case gl-error)
grep -q 'verdict=UNMEASURED reason=readback-unproven' <<<"$out" || \
  fail "read sem escrita nao ficou UNMEASURED: $out"
grep -q 'pending=0x506 reads=2' <<<"$out" || \
  fail "glGetError foi consumido ou retry sentinel faltou: $out"
grep -q 'verdict=OK\|verdict=BLACK' <<<"$out" && \
  fail "read sem escrita recebeu veredito inventado: $out"

out=$(run_case pending-error-visible)
grep -q 'verdict=OK' <<<"$out" || \
  fail "erro anterior impediu read comprovado: $out"
grep -q 'pending=0x502 reads=1' <<<"$out" || \
  fail "fila anterior foi drenada numa leitura valida: $out"

out=$(run_case partial-read)
grep -q 'verdict=UNMEASURED reason=readback-unproven' <<<"$out" || \
  fail "read parcial foi aceito: $out"

out=$(run_case overflow-read)
grep -q 'verdict=UNMEASURED reason=readback-overflow' <<<"$out" || \
  fail "overflow no guard nao foi recusado: $out"

# RGB colorido com alpha zero e exatamente o caso Amlogic: compositor mostra
# preto. Uma amostra nao mata; tres before-present sequenciais matam.
out=$(run_case alpha-zero)
grep -q 'rgb_non_black=100.0% visible_non_black=0.0%' <<<"$out" || \
  fail "alpha-zero nao separou RGB de pixel visivel: $out"
grep -q 'verdict=BLACK reason=alpha-zero' <<<"$out" || \
  fail "alpha-zero nao foi BLACK: $out"

out=$(run_case alpha-one)
grep -q 'rgb_non_black=100.0% visible_non_black=100.0%.*alpha0=0.0%' <<<"$out" || \
  fail "fronteira literal alpha!=0 mudou sem contrato: $out"
grep -q 'verdict=OK' <<<"$out" || \
  fail "alpha=1 nao seguiu o contrato literal nao-zero: $out"

out=$(run_case one-black)
grep -q 'verdict=INCONCLUSIVE reason=insufficient-black-streak' <<<"$out" || \
  fail "uma amostra preta virou fatal: $out"
grep -q 'verdict=BLACK\|verdict=DEAD-CONTEXT' <<<"$out" && \
  fail "publish manual matou com uma amostra: $out"

out=$(run_case manual-three-black)
grep -q 'verdict=BLACK reason=all-black' <<<"$out" || \
  fail "tres amostras manuais nao fecharam BLACK: $out"

out=$(run_case dead-context)
grep -q 'GL_RENDERER nulo em 3 amostras consecutivas' <<<"$out" || \
  fail "DEAD-CONTEXT nao exigiu sequencia: $out"
grep -q 'verdict=DEAD-CONTEXT reason=renderer-null-driverless-provider' <<<"$out" || \
  fail "DEAD-CONTEXT sem motivo exato: $out"

out=$(run_case broken-dead-sequence)
grep -q 'verdict=UNMEASURED' <<<"$out" || \
  fail "amostra recusada nao quebrou a sequencia DEAD: $out"
grep -q 'verdict=DEAD-CONTEXT\|verdict=BLACK' <<<"$out" && \
  fail "sequencia DEAD nao consecutiva virou fatal: $out"

# Somente o default framebuffer imediatamente before-present autoriza OK.
for scenario in unspecified-visible after-visible; do
  out=$(run_case "$scenario")
  grep -q 'verdict=INCONCLUSIVE' <<<"$out" || \
    fail "$scenario alegou framebuffer apresentado: $out"
  grep -q 'verdict=OK' <<<"$out" && \
    fail "$scenario vazou OK: $out"
done

for spec in \
  'fbo-bound:non-default-framebuffer' \
  'read-fbo-bound:non-default-framebuffer' \
  'pbo-bound:pixel-pack-buffer-bound' \
  'pack-state:incompatible-pack-state' \
  'pack-skip-rows:incompatible-pack-state' \
  'pack-skip-pixels:incompatible-pack-state' \
  'query-unproven:non-default-framebuffer'; do
  scenario=${spec%%:*}
  reason=${spec#*:}
  out=$(run_case "$scenario")
  grep -q "verdict=UNMEASURED reason=$reason" <<<"$out" || \
    fail "$scenario nao recusou state incompatível: $out"
  grep -q 'reads=0' <<<"$out" || \
    fail "$scenario chamou glReadPixels antes do preflight: $out"
done

out=$(run_case pack8)
grep -q 'frame probe 65x63.*visible_non_black=100.0%' <<<"$out" || \
  fail "PACK_ALIGNMENT=8 seguro nao respeitou stride: $out"
grep -q 'verdict=OK' <<<"$out" || fail "PACK_ALIGNMENT=8 valido falhou: $out"

out=$(run_case gles1-default)
grep -q 'verdict=OK' <<<"$out" || \
  fail "GLES1 sem extensao FBO nao reconheceu default implicito: $out"

out=$(run_case pack-alignment-invalid)
grep -q 'verdict=UNMEASURED reason=pack-alignment-unproven' <<<"$out" || \
  fail "PACK_ALIGNMENT invalido nao fechou: $out"
grep -q 'reads=0' <<<"$out" || fail "pack invalido chegou ao driver: $out"

out=$(run_case readback-cap)
grep -q 'verdict=UNMEASURED reason=readback-size-limit' <<<"$out" || \
  fail "cap de 64 MiB nao fechou antes da alocacao: $out"
grep -q 'reads=0' <<<"$out" || fail "readback >64 MiB chegou ao driver: $out"

out=$(run_case oversize)
grep -q 'verdict=UNMEASURED reason=invalid-drawable' <<<"$out" || \
  fail "limite de alocacao nao fechou: $out"
grep -q 'reads=0' <<<"$out" || fail "oversize chegou ao driver: $out"

out=$(run_case reentrant 2>&1)
grep -q 'refused (concurrent/reentrant call)' <<<"$out" || \
  fail "reentrada nao falhou fechado: $out"
grep -q 'verdict=OK' <<<"$out" || \
  fail "guard de reentrada corrompeu a chamada proprietaria: $out"

# Continuous sparse schedule still catches the audio-alive black loop.
out=$(run_case streak-black)
grep -q 'IMAGE PROOF: black-streak=3' <<<"$out" || \
  fail "streak automatico nao gritou: $out"
grep -q '"reason_code":6304' <<<"$out" || \
  fail "NXEVENT 6304 ausente: $out"

out=$(run_case streak-ok)
grep -q 'IMAGE PROOF' <<<"$out" && fail "imagem valida gritou BLACK: $out"
grep -q 'verdict=OK' <<<"$out" || fail "streak OK ausente: $out"

out=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT NXLAUNCH_FRONTEND=1 \
  NXGL_IMAGE_PROOF=0 "$BIN" streak-off)
grep -q 'frame probe' <<<"$out" && fail "NXGL_IMAGE_PROOF=0 amostrou: $out"

# Private machine receipt helpers.
receipt_case() { # $1 scenario $2 path; optional extra env follows
  local scenario=$1 path=$2
  shift 2
  env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT \
    NXLAUNCH_FRONTEND=1 \
    NXBOOTSTRAP_VIDEO_FILE="$path" \
    NXBOOTSTRAP_HEALTH_RUN_ID=nameless-cat-4242-1700000000-7 \
    NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef \
    NXBOOTSTRAP_HEALTH_PORT_ID=nameless-cat \
    "$@" "$BIN" "$scenario"
}

expected_receipt() { # $1 verdict $2 reason
  printf '{"schema":"org.nextos.nxruntime.video-proof","schema_version":1,"run_id":"nameless-cat-4242-1700000000-7","generation":"0123456789abcdef","port_id":"nameless-cat","verdict":"%s","reason":"%s"}\n' "$1" "$2"
}

ok_file=$TEST_ROOT/video-ok.json
ok_proof_dir=$TEST_ROOT/ok-proof
mkdir "$ok_proof_dir"
chmod 0700 "$ok_proof_dir"
out=$(receipt_case ok "$ok_file" NXLAUNCH_PROOF_DIR="$ok_proof_dir")
[ "$(cat "$ok_file")" = "$(expected_receipt OK non-black)" ] || \
  fail "OK receipt nao e contrato exato"
[ "$(stat -c %a "$ok_file")" = 600 ] || fail "OK receipt nao e 0600"
[ "$(stat -c %h "$ok_file")" = 1 ] || fail "OK receipt tem hardlink"
grep -q 'frame proof image written .* (640x480 RGBA)' <<<"$out" || \
  fail "PNG exato nao confirmou escrita: $out"
python3 -B "$HERE/inspect-proof-png.py" \
  "$ok_proof_dir/frame-proof.png" non-black >/dev/null || \
  fail "PNG RGBA da primeira amostra OK nao e visivel"

# PNG requested but unwritable/unsafe blocks machine OK. A planted temp is
# preserved, the next sample retries, and exactly one atomic PNG is claimed.
png_retry_file=$TEST_ROOT/video-png-retry.json
png_retry_dir=$TEST_ROOT/png-retry
mkdir "$png_retry_dir"
chmod 0700 "$png_retry_dir"
out=$(receipt_case png-retry "$png_retry_file" \
  NXLAUNCH_PROOF_DIR="$png_retry_dir" 2>&1)
grep -q 'png-retry: failed-write-not-claimed foreign-temp=preserved' <<<"$out" || \
  fail "falha PNG apagou temp alheio ou alegou sucesso: $out"
[ "$(grep -c 'frame proof image written' <<<"$out")" = 1 ] || \
  fail "PNG foi alegado mais de uma vez: $out"
[ "$(cat "$png_retry_file")" = "$(expected_receipt OK non-black)" ] || \
  fail "retry PNG nao concluiu o mesmo OK"
python3 -B "$HERE/inspect-proof-png.py" \
  "$png_retry_dir/frame-proof.png" non-black >/dev/null || \
  fail "retry PNG nao preservou pixels"

bad_png_file=$TEST_ROOT/video-bad-png.json
bad_png_dir=$TEST_ROOT/public-proof
mkdir "$bad_png_dir"
chmod 0755 "$bad_png_dir"
out=$(receipt_case ok "$bad_png_file" NXLAUNCH_PROOF_DIR="$bad_png_dir" 2>&1)
[ ! -e "$bad_png_file" ] && [ ! -L "$bad_png_file" ] || \
  fail "falha de PNG publicou OK"
grep -q 'frame proof image write failed' <<<"$out" || \
  fail "falha PNG nao ficou verificavel: $out"
grep -q 'VIDEO-PROOF: verdict=OK' <<<"$out" && \
  fail "falha PNG alegou OK: $out"

one_file=$TEST_ROOT/video-one-black.json
out=$(receipt_case one-black "$one_file")
[ ! -e "$one_file" ] && [ ! -L "$one_file" ] || \
  fail "uma amostra preta publicou fatal"

black_file=$TEST_ROOT/video-three-black.json
health_file=$TEST_ROOT/health-must-be-revoked.json
printf 'ready-before-fatal\n' > "$health_file"
chmod 0600 "$health_file"
out=$(receipt_case manual-three-black "$black_file" \
  NXAUDIO_STATE=playing NXBOOTSTRAP_HEALTH_FILE="$health_file")
[ "$(cat "$black_file")" = "$(expected_receipt BLACK black-streak)" ] || \
  fail "audio+3 pretos nao publicou BLACK/streak"
[ ! -e "$health_file" ] && [ ! -L "$health_file" ] || \
  fail "BLACK nao revogou o health preexistente"
grep -q 'PROOF-LIFECYCLE: fatal=1 close=1 close_again=0' <<<"$out" || \
  fail "BLACK nao produziu fatal irreversivel/consume unico: $out"

dead_file=$TEST_ROOT/video-dead.json
out=$(receipt_case dead-context "$dead_file")
[ "$(cat "$dead_file")" = "$(expected_receipt DEAD-CONTEXT dead-context)" ] || \
  fail "dead context nao publicou fatal exato"
grep -q 'PROOF-LIFECYCLE: fatal=1 close=1 close_again=0' <<<"$out" || \
  fail "DEAD-CONTEXT nao produziu fatal irreversivel: $out"

# Fatal after OK: failed replacement revokes the old OK, preserves a foreign
# O_EXCL collision, remains irreversible, then succeeds on a later retry.
retry_file=$TEST_ROOT/video-retry-fatal.json
out=$(receipt_case retry-fatal "$retry_file" NXAUDIO_STATE=playing 2>&1)
grep -q 'retry-safety: prior-ok=revoked foreign-temp=preserved' <<<"$out" || \
  fail "fatal failure nao revogou/retry ou apagou temp alheio: $out"
grep -q 'previous receipt revoked; retry pending' <<<"$out" || \
  fail "revogacao fatal nao ficou explicita: $out"
[ "$(cat "$retry_file")" = "$(expected_receipt BLACK black-streak)" ] || \
  fail "fatal retry nao terminou BLACK"

drift_file=$TEST_ROOT/video-contract-drift.json
out=$(receipt_case contract-drift-fatal "$drift_file")
grep -q 'contract-lock: ambient-drift=ignored original=authoritative' <<<"$out" || \
  fail "tuple/path nao ficaram congelados depois do primeiro OK: $out"
[ "$(cat "$drift_file")" = "$(expected_receipt BLACK black-streak)" ] || \
  fail "fatal seguiu ambiente mutado em vez do contrato original"
[ ! -e "$drift_file.drift" ] && [ ! -L "$drift_file.drift" ] || \
  fail "drift de path recebeu autoridade"

replace_file=$TEST_ROOT/video-replace.json
out=$(receipt_case ok-then-black "$replace_file" NXAUDIO_STATE=playing)
[ "$(cat "$replace_file")" = "$(expected_receipt BLACK black-streak)" ] || \
  fail "fatal posterior nao substituiu OK"
grep -q 'frame proof verdict=OK.*$' <<<"$out" || \
  fail "cenario replace nao exercitou OK inicial"
grep -q 'frame proof verdict=BLACK' <<<"$out" || \
  fail "best historico reemitiu OK depois do fatal: $out"
last_verdict=$(grep 'gl: frame proof verdict=' <<<"$out" | tail -n 1)
grep -q 'verdict=BLACK' <<<"$last_verdict" || \
  fail "ultimo veredito reutilizou best antigo: $out"

irreversible_file=$TEST_ROOT/video-irreversible.json
out=$(receipt_case black-then-ok "$irreversible_file")
[ "$(cat "$irreversible_file")" = "$(expected_receipt BLACK black-streak)" ] || \
  fail "quadro tardio sobrescreveu fatal"

# Frontend explicit 1 mirrors the pure classifier and wins over parallel SSH.
frontend_wins=$TEST_ROOT/video-frontend-wins.json
out=$(env SSH_CONNECTION='10 11 12 13' NXLAUNCH_FRONTEND=1 \
  NXBOOTSTRAP_VIDEO_FILE="$frontend_wins" \
  NXBOOTSTRAP_HEALTH_RUN_ID=nameless-cat-4242-1700000000-7 \
  NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef \
  NXBOOTSTRAP_HEALTH_PORT_ID=nameless-cat "$BIN" manual-three-black)
grep -q 'launch: context=frontend can-prove-image=yes' <<<"$out" || \
  fail "frontend nao venceu SSH: $out"
[ "$(cat "$frontend_wins")" = "$(expected_receipt BLACK black-streak)" ] || \
  fail "frontend+SSH nao publicou fatal"

frontend_zero=$TEST_ROOT/video-frontend-zero.json
out=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT NXLAUNCH_FRONTEND=0 \
  NXBOOTSTRAP_VIDEO_FILE="$frontend_zero" \
  NXBOOTSTRAP_HEALTH_RUN_ID=nameless-cat-4242-1700000000-7 \
  NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef \
  NXBOOTSTRAP_HEALTH_PORT_ID=nameless-cat "$BIN" manual-three-black)
grep -q 'launch: context=unknown can-prove-image=no' <<<"$out" || \
  fail "NXLAUNCH_FRONTEND=0 foi tratado como frontend: $out"
grep -q 'verdict=INCONCLUSIVE' <<<"$out" || \
  fail "preto em launch desconhecido acusou o port: $out"
grep -q 'verdict=BLACK\|verdict=DEAD-CONTEXT' <<<"$out" && \
  fail "launch desconhecido recebeu fatal humano: $out"
[ ! -e "$frontend_zero" ] && [ ! -L "$frontend_zero" ] || \
  fail "frontend=0 publicou fatal conclusivo"

# Unsafe contract boundaries remain fail-closed.
bad_tuple=$TEST_ROOT/video-bad-tuple.json
out=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT NXLAUNCH_FRONTEND=1 \
  NXBOOTSTRAP_VIDEO_FILE="$bad_tuple" \
  NXBOOTSTRAP_HEALTH_RUN_ID='bad/run' \
  NXBOOTSTRAP_HEALTH_GENERATION=0123456789abcdef \
  NXBOOTSTRAP_HEALTH_PORT_ID=nameless-cat "$BIN" ok 2>&1)
[ ! -e "$bad_tuple" ] && [ ! -L "$bad_tuple" ] || \
  fail "tuple insegura publicou recibo"
grep -q 'receipt refused (unsafe contract)' <<<"$out" || \
  fail "tuple insegura nao falhou explicitamente"

unsafe_file=$TEST_ROOT/video-unsafe-mode.json
printf 'sentinel\n' > "$unsafe_file"
chmod 0644 "$unsafe_file"
out=$(receipt_case ok "$unsafe_file" 2>&1)
[ "$(cat "$unsafe_file")" = sentinel ] || fail "target 0644 sobrescrito"
[ "$(stat -c %a "$unsafe_file")" = 644 ] || fail "target 0644 alterado"

victim=$TEST_ROOT/victim
printf 'keep\n' > "$victim"
symlink_file=$TEST_ROOT/video-symlink.json
ln -s "$victim" "$symlink_file"
out=$(receipt_case ok "$symlink_file" 2>&1)
[ -L "$symlink_file" ] || fail "symlink substituido"
[ "$(cat "$victim")" = keep ] || fail "alvo de symlink alterado"

public_parent=$TEST_ROOT/public-parent
mkdir "$public_parent"
chmod 0755 "$public_parent"
public_file=$public_parent/video.json
out=$(receipt_case ok "$public_file" 2>&1)
[ ! -e "$public_file" ] && [ ! -L "$public_file" ] || \
  fail "diretorio publico recebeu autoridade"

if find "$TEST_ROOT" -name '*.tmp.*' -print -quit | grep -q .; then
  fail "temporario criado pelo adapter sobreviveu"
fi

if grep -Eq 'resolve_gl\("gl(BindFramebuffer|BindBuffer|PixelStorei)' \
    "$HERE/../adapters/nxgl_frame_proof_adapter.c"; then
  fail "adapter passou a alterar binding/pixel-store observado"
fi

echo "nxgl video-receipt test: PASS cases=44 captures=2 machine-contract=17 readback=sentinel-no-glGetError-64MiB alpha=zero-closed-nonzero-literal default-fbo=proved fatal=3-sequential-retry-safe-consume-health-revoked"
