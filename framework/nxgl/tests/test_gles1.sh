#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Gate hermetico do resolvedor GLES1 (nxgl_gles1) -- 0.2.14: selecao por
# MEDICAO (prova de vida por candidato), nunca por ordem de nome.
#
# Motivo dos cenarios espelhados: dArkOS (R36S) tem libmali.so VIVO e a Mesa
# versionada MORTA; ROCKNIX (RK3566) tem exatamente o oposto (libmali.so e'
# blob kbase orfao, a Mesa/Panfrost e' a viva). Ordem fixa quebrava um dos
# dois; o resolvedor tem de escolher o VIVO nos dois mundos.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXGL_ROOT=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

GLES_INCLUDE=${NXGL_GLES1_TEST_INCLUDE:-}
if [ -z "$GLES_INCLUDE" ]; then
  for candidate in \
    "$HOME"/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain/aarch64-libreelec-linux-gnu/sysroot/usr/include \
    /usr/include; do
    if [ -f "$candidate/GLES/gl.h" ]; then
      GLES_INCLUDE=$candidate
      break
    fi
  done
fi
if [ -z "$GLES_INCLUDE" ]; then
  echo "nxgl_gles1=SKIP sem GLES/gl.h no host" >&2
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -shared -fPIC -o "$WORK/live.so" "$HERE/fake_gles1_live.c"
"$CC" -shared -fPIC -o "$WORK/dead.so" "$HERE/fake_gles1_dead.c"

cat > "$WORK/main.c" <<'EOF'
#include "nxgl_gles1.h"
#include <stdio.h>
int main(void) {
  nxgl_gles1_receipt receipt;
  int rc = nxgl_gles1_init(&receipt);
  printf("rc=%d provider=%s resolved=%u total=%u missing=%s liveness=%s\n",
         rc, receipt.provider, receipt.resolved, receipt.total,
         receipt.first_missing[0] ? receipt.first_missing : "-",
         nxgl_gles1_liveness());
  return 0;
}
EOF

cat > "$WORK/main_primary.c" <<'EOF'
#include "nxgl_gles1.h"
#include <dlfcn.h>
#include <stdio.h>
static void *g_handle;
static void *resolver(const char *name) { return dlsym(g_handle, name); }
int main(void) {
  nxgl_gles1_receipt receipt;
  int rc;
  g_handle = dlopen("libprimary.so", RTLD_NOW | RTLD_LOCAL);
  if (g_handle == NULL) {
    printf("dlopen-primary-failed\n");
    return 1;
  }
  nxgl_gles1_set_primary_resolver(resolver);
  rc = nxgl_gles1_init(&receipt);
  printf("rc=%d provider=%s liveness=%s\n", rc, receipt.provider,
         nxgl_gles1_liveness());
  return 0;
}
EOF

for probe in main main_primary; do
  "$CC" -std=c99 -D_POSIX_C_SOURCE=200809L \
    -I "$NXGL_ROOT/include" -I "$GLES_INCLUDE" \
    -o "$WORK/$probe" "$WORK/$probe.c" "$NXGL_ROOT/src/nxgl_gles1.c" -ldl
done

fail() { echo "nxgl_gles1=FAIL $*" >&2; exit 1; }

# 1. provedor classico VIVO (NextOS/EmuELEC: so' o nome versionado existe)
mkdir -p "$WORK/classic"
cp "$WORK/live.so" "$WORK/classic/libGLESv1_CM.so.1"
out=$(LD_LIBRARY_PATH="$WORK/classic" "$WORK/main")
case $out in
  "rc=0 provider=libGLESv1_CM.so.1 resolved=47 total=47 missing=- liveness=ok") ;;
  *) fail "cenario classico vivo: $out" ;;
esac

# 2. espelho dArkOS: blob VIVO + Mesa versionada MORTA -> tem de escolher o blob
mkdir -p "$WORK/darkos"
cp "$WORK/live.so" "$WORK/darkos/libmali.so"
cp "$WORK/dead.so" "$WORK/darkos/libGLESv1_CM.so.1"
out=$(LD_LIBRARY_PATH="$WORK/darkos" "$WORK/main")
case $out in
  "rc=0 provider=libmali.so resolved=47 total=47 missing=- liveness=ok") ;;
  *) fail "espelho dArkOS (blob vivo tem de vencer): $out" ;;
esac

# 3. espelho ROCKNIX: blob MORTO + Mesa versionada VIVA -> tem de escolher a
#    Mesa (a cadeia por nome antiga escolhia o blob = tela preta com som)
mkdir -p "$WORK/rocknix"
cp "$WORK/dead.so" "$WORK/rocknix/libmali.so"
cp "$WORK/live.so" "$WORK/rocknix/libGLESv1_CM.so.1"
out=$(LD_LIBRARY_PATH="$WORK/rocknix" "$WORK/main")
case $out in
  "rc=0 provider=libGLESv1_CM.so.1 resolved=47 total=47 missing=- liveness=ok") ;;
  *) fail "espelho ROCKNIX (o vivo tem de vencer o blob orfao): $out" ;;
esac

# 4. so' o blob unificado VIVO (dArkOS sem Mesa nenhuma)
mkdir -p "$WORK/blob"
cp "$WORK/live.so" "$WORK/blob/libmali.so"
out=$(LD_LIBRARY_PATH="$WORK/blob" "$WORK/main")
case $out in
  "rc=0 provider=libmali.so resolved=47 total=47 missing=- liveness=ok") ;;
  *) fail "cenario so-blob vivo: $out" ;;
esac

# 5. sem provedor nenhum: falhar com recibo, nunca seguir com ponteiro nulo
out=$(LD_LIBRARY_PATH="$WORK/vazio" "$WORK/main")
case $out in
  "rc=-1 provider=nenhum resolved=0 total=47 missing=glAlphaFunc liveness=mixed") ;;
  *) fail "cenario sem provedor: $out" ;;
esac

# 6. todos os candidatos completos MORTOS (init sem contexto corrente, ou
#    firmware toda quebrada): comportamento v1 preservado -- segue com o
#    primeiro completo, mas o recibo DENUNCIA liveness=dead
mkdir -p "$WORK/alldead"
cp "$WORK/dead.so" "$WORK/alldead/libmali.so"
out=$(LD_LIBRARY_PATH="$WORK/alldead" "$WORK/main")
case $out in
  "rc=0 provider=libmali.so resolved=47 total=47 missing=- liveness=dead") ;;
  *) fail "cenario todos-mortos (fallback v1 + denuncia): $out" ;;
esac

# 7. resolvedor primario injetado (SDL_GL_GetProcAddress do port) vence a
#    cadeia inteira quando vivo -- mesmo com um blob vivo presente
mkdir -p "$WORK/primary"
cp "$WORK/live.so" "$WORK/primary/libprimary.so"
cp "$WORK/live.so" "$WORK/primary/libmali.so"
out=$(LD_LIBRARY_PATH="$WORK/primary" "$WORK/main_primary")
case $out in
  "rc=0 provider=primary-resolver liveness=ok") ;;
  *) fail "cenario resolvedor primario: $out" ;;
esac

echo "nxgl_gles1=PASS 7 cenarios (espelhos dArkOS/ROCKNIX inclusos)"
