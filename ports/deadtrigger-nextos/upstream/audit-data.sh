#!/usr/bin/env bash
# Fail-closed audit of an extracted or installed Dead Trigger v2.1.0 tree.
set -euo pipefail

ROOT="${1:-/storage/roms/ports/deadtrigger}"
DATA="$ROOT/assets/bin/Data"
[[ -d "$DATA" ]] || {
    echo "Data directory ausente: $DATA" >&2
    exit 1
}

check_hash() {
    local expected="$1" path="$2" actual
    [[ -f "$path" ]] || {
        echo "Arquivo ausente: $path" >&2
        exit 1
    }
    actual="$(sha256sum "$path" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] || {
        echo "Hash incorreto: $path" >&2
        echo "esperado: $expected" >&2
        echo "obtido:   $actual" >&2
        exit 1
    }
}

count="$(find "$DATA" -type f | wc -l)"
bytes="$(find "$DATA" -type f -exec stat -c '%s' {} + |
    awk '{total += $1} END {print total + 0}')"
[[ "$count" -eq 1441 ]] || {
    echo "Contagem incorreta: $count (esperado 1441)" >&2
    exit 1
}
[[ "$bytes" -eq 306315658 ]] || {
    echo "Tamanho incorreto: $bytes (esperado 306315658)" >&2
    exit 1
}

check_hash c32a89d727bb920d125e50f1bdc4663e284af3afb5f737e3482fc06f1c6ebc73 \
    "$ROOT/libmain.so"
check_hash af3519ed16e3fdf5b85afae3b7477aa83ea370c353cd7352fdb77926aac75290 \
    "$ROOT/libunity.so"
check_hash 37c25b8fcdb0387c660fb33554c3e585aa13fd505b12bffff48816d91361efe9 \
    "$ROOT/libil2cpp.so"
check_hash c01abcdbfe75379b898288323fdbe6cf5a0095624ef1ecf4ee313b6c3ff9df9d \
    "$DATA/Managed/Metadata/global-metadata.dat"
check_hash 8184d79493032a94f565e9b2d11bd4af4a3b49ad379a96c22d397913a9f0ab59 \
    "$DATA/globalgamemanagers"

echo "DATA OK: $ROOT"
echo "Dead Trigger 1 v2.1.0 ARM64 · $count arquivos · $bytes bytes"
