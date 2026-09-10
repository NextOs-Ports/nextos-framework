#!/usr/bin/env bash
# V3-AUDIO-01 standalone host gate: builds the receipt/liveness/safe-exit
# module as strict C99 and proves the capability-only selection rule by
# grepping the nxaudio sources for brand tokens. Hermetic: no device, no
# network, no guest code.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
source_root="$repo_root/framework/nxaudio"
work_root=$(mktemp -d /tmp/nxaudio-receipt-host.XXXXXX)
trap 'find "$work_root" -depth -delete' EXIT HUP INT TERM

# Safe-exit contract: the helper itself never terminates the process and
# never creates a thread; the port owns its own final _exit(0).
if grep -En '(_exit|abort|pthread_create)[[:space:]]*\(' \
    "$source_root/src/nxaudio_receipt.c" | grep -vE '^\s*[0-9]+:\s*\*'; then
  echo "nxaudio_receipt.c must not call _exit/abort or create threads" >&2
  exit 1
fi

# Static rule: nxaudio never selects or branches by device/brand name.
# Selection is inherited env -> measured open success -> declared fallback
# (nxaudio_backend_probe_order). Any brand token appearing in the C sources
# fails this gate before anything is compiled.
brand_regex='\b(mali|libmali|rockchip|amlogic|allwinner|anbernic|powkiddy|odroid|miyoo|rk3[0-9]+|s905|h700|r36s|rg35[0-9a-z]*|retroid)\b'
if grep -riEn "$brand_regex" \
    "$source_root/src" "$source_root/include"; then
  echo "brand token found in nxaudio sources: selection must stay capability-measured" >&2
  exit 1
fi

for compiler in gcc clang; do
  command -v "$compiler" >/dev/null
  bin="$work_root/test-receipt-$compiler"
  # The new module and its test are strict C99; the existing core (linked
  # only for nxaudio_classify_granted_format) keeps its own C11 build.
  "$compiler" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -c \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$source_root/include" \
    "$source_root/src/nxaudio.c" -o "$bin-core.o"
  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$source_root/include" \
    "$source_root/src/nxaudio_receipt.c" \
    "$source_root/tests/test_receipt.c" \
    "$bin-core.o" -o "$bin"
  if nm -u "$bin" | grep -Eq 'SDL_|snd_|pulse|pipewire|alc?Open|FMOD|Wwise'; then
    echo "receipt fixture imports a real audio provider" >&2
    exit 1
  fi
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
    "$bin"
done

echo "nxaudio_receipt_host=PASS gcc=1 clang=1 c99=1 asan=1 ubsan=1 brand_grep=0"
echo "guest_code_executed=0 hardware_ran=0 device_access=0 network_access=0"
