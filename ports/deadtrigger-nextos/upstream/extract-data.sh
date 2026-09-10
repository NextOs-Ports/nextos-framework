#!/usr/bin/env bash
# Extract the exact complete Dead Trigger 1 v2.1.0 ARM64 payload.
set -euo pipefail

EXPECTED_APK_SHA256=578d4f34cc5b5d2a3d1872f5edda356f2bb1a00c894730f8fa6109d3abf1461b
EXPECTED_FILE_COUNT=1441
EXPECTED_DATA_BYTES=306315658

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "uso: $0 /caminho/dead-trigger-2.1.0.apk [diretorio-destino]" >&2
    exit 2
fi

APK="$1"
DEST="${2:-deadtrigger-v2.1.0-data}"
[[ -f "$APK" ]] || {
    echo "APK ausente: $APK" >&2
    exit 1
}
if [[ -e "$DEST" ]]; then
    echo "Destino já existe; não será sobrescrito: $DEST" >&2
    exit 1
fi

APK_SHA256="$(sha256sum "$APK" | awk '{print $1}')"
if [[ "$APK_SHA256" != "$EXPECTED_APK_SHA256" ]]; then
    echo "APK incompatível." >&2
    echo "esperado: $EXPECTED_APK_SHA256" >&2
    echo "obtido:   $APK_SHA256" >&2
    exit 1
fi

unzip -tq "$APK" >/dev/null
DEST_PARENT="$(dirname "$DEST")"
mkdir -p "$DEST_PARENT"
TEMP="$(mktemp -d "$DEST_PARENT/.deadtrigger-data.XXXXXX")"
cleanup() {
    [[ -n "${TEMP:-}" && -d "$TEMP" ]] && rm -rf -- "$TEMP"
}
trap cleanup EXIT

unzip -qj "$APK" \
    lib/arm64-v8a/libmain.so \
    lib/arm64-v8a/libunity.so \
    lib/arm64-v8a/libil2cpp.so \
    -d "$TEMP"
unzip -q "$APK" 'assets/bin/Data/*' -d "$TEMP"

count="$(find "$TEMP/assets/bin/Data" -type f | wc -l)"
bytes="$(find "$TEMP/assets/bin/Data" -type f -exec stat -c '%s' {} + |
    awk '{total += $1} END {print total + 0}')"
[[ "$count" -eq "$EXPECTED_FILE_COUNT" ]] || {
    echo "Payload incompleto: $count arquivos; esperado $EXPECTED_FILE_COUNT" >&2
    exit 1
}
[[ "$bytes" -eq "$EXPECTED_DATA_BYTES" ]] || {
    echo "Payload incompleto: $bytes bytes; esperado $EXPECTED_DATA_BYTES" >&2
    exit 1
}

check_hash() {
    local expected="$1" path="$2" actual
    actual="$(sha256sum "$path" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] || {
        echo "Hash incorreto: $path" >&2
        echo "esperado: $expected" >&2
        echo "obtido:   $actual" >&2
        exit 1
    }
}
check_hash c32a89d727bb920d125e50f1bdc4663e284af3afb5f737e3482fc06f1c6ebc73 \
    "$TEMP/libmain.so"
check_hash af3519ed16e3fdf5b85afae3b7477aa83ea370c353cd7352fdb77926aac75290 \
    "$TEMP/libunity.so"
check_hash 37c25b8fcdb0387c660fb33554c3e585aa13fd505b12bffff48816d91361efe9 \
    "$TEMP/libil2cpp.so"
check_hash c01abcdbfe75379b898288323fdbe6cf5a0095624ef1ecf4ee313b6c3ff9df9d \
    "$TEMP/assets/bin/Data/Managed/Metadata/global-metadata.dat"
check_hash 8184d79493032a94f565e9b2d11bd4af4a3b49ad379a96c22d397913a9f0ab59 \
    "$TEMP/assets/bin/Data/globalgamemanagers"

mv "$TEMP" "$DEST"
TEMP=
trap - EXIT
echo "DATA OK: $DEST"
echo "APK universal v2.1.0 · ARM64 · $count arquivos · $bytes bytes"
