#!/usr/bin/env bash
# V4-AUDIO-05 owner-local runtime-attestation gate. Hermetic host fixture:
# no provider/device, guest, network or PHYSICAL claim.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
source_root="$repo_root/framework/nxaudio"
work_root=$(mktemp -d /tmp/nxaudio-runtime-host.XXXXXX)
trap 'find "$work_root" -depth -delete' EXIT HUP INT TERM

runtime_source="$source_root/src/nxaudio_runtime.c"
realtime_source="$work_root/realtime-source.c"

for helper_name in \
  load_u32 \
  store_u32 \
  compare_u32 \
  session_valid \
  identity_equal \
  identity_event \
  callback_time \
  producer_event \
  saturating_add \
  peak_update \
  claim_recovery_fault \
  release_recovery_fault \
  callback_enter \
  callback_leave; do
  sed -n "/^static .* ${helper_name}(/,/^}/p" "$runtime_source" \
    >>"$realtime_source"
done

for function_name in \
  nxaudio_runtime_update_callback \
  nxaudio_runtime_report_underrun \
  nxaudio_runtime_report_silence \
  nxaudio_runtime_report_device_lost; do
  sed -n "/^nxaudio_result ${function_name}(/,/^}/p" "$runtime_source" \
    >>"$realtime_source"
done

for required in \
  callback_enter \
  callback_leave \
  nxaudio_runtime_update_callback \
  nxaudio_runtime_report_underrun \
  nxaudio_runtime_report_silence \
  nxaudio_runtime_report_device_lost; do
  grep -q "$required" "$realtime_source"
done

forbidden_call='(^|[^[:alnum:]_])(malloc|calloc|realloc|free|snprintf|printf|fprintf|fopen|open|close|read|write|sleep|usleep|getenv|setenv|putenv|system|popen|pthread_mutex_lock|pthread_cond_wait|nxaudio_runtime_format_receipt|nxaudio_backend_recovery_run|SDL_[[:alnum:]_]*|snd_[[:alnum:]_]*|alc?Open[[:alnum:]_]*|FMOD_[[:alnum:]_]*|AK_[[:alnum:]_]*)[[:space:]]*\('
if grep -En "$forbidden_call" "$realtime_source"; then
  echo "realtime callback path contains a forbidden call" >&2
  exit 1
fi

provider_symbol='(SDL_|snd_|pa_[[:alnum:]_]*|pulse|pipewire|alc?Open|OpenAL|FMOD|Wwise|AK::)'
common_flags=(
  -D_POSIX_C_SOURCE=200809L
  -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
  -pedantic-errors -O1 -g -fno-omit-frame-pointer -pthread
  -I "$source_root/include"
)
sanitizers=(-fsanitize=address,undefined)

for compiler in gcc clang; do
  command -v "$compiler" >/dev/null
  core="$work_root/nxaudio-core-$compiler.o"
  binary="$work_root/test-runtime-$compiler"

  "$compiler" -std=c11 "${common_flags[@]}" "${sanitizers[@]}" -c \
    "$source_root/src/nxaudio.c" -o "$core"
  "$compiler" -std=c99 "${common_flags[@]}" "${sanitizers[@]}" \
    "$source_root/src/nxaudio_receipt.c" \
    "$source_root/src/nxaudio_runtime.c" \
    "$source_root/tests/test_runtime.c" "$core" -lm -o "$binary"

  if nm -u "$binary" | grep -Ei "$provider_symbol"; then
    echo "runtime fixture imports a real audio provider" >&2
    exit 1
  fi
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
    "$binary"
done

tsan=SKIP
tsan_reason=compiler-unavailable
if command -v gcc >/dev/null; then
  tsan_reason=build-unavailable
fi
if command -v gcc >/dev/null && \
   gcc -std=c11 "${common_flags[@]}" -fsanitize=thread -c \
     "$source_root/src/nxaudio.c" -o "$work_root/nxaudio-core-tsan.o" \
     >/dev/null 2>&1 && \
   gcc -std=c99 "${common_flags[@]}" -fsanitize=thread \
     "$source_root/src/nxaudio_receipt.c" \
     "$source_root/src/nxaudio_runtime.c" \
     "$source_root/tests/test_runtime.c" "$work_root/nxaudio-core-tsan.o" \
     -lm -o "$work_root/test-runtime-tsan" >/dev/null 2>&1; then
  set +e
  tsan_output=$(TSAN_OPTIONS=halt_on_error=1 "$work_root/test-runtime-tsan" 2>&1)
  tsan_rc=$?
  set -e
  if [[ $tsan_rc -eq 0 ]]; then
    tsan=PASS
    tsan_reason=clean
  elif grep -Eqi \
      'unexpected memory mapping|ThreadSanitizer.*(unsupported|not supported)|failed to allocate|cannot mmap' \
      <<<"$tsan_output"; then
    tsan=SKIP
    tsan_reason=runtime-unavailable
  else
    printf '%s\n' "$tsan_output" >&2
    echo "TSAN detected a defect" >&2
    exit 1
  fi
fi

echo "nxaudio_runtime_host=PASS guards=13 gcc=1 clang=1 c99=1 asan=1 ubsan=1 tsan=$tsan tsan_reason=$tsan_reason"
echo "provider_imports=0 realtime_forbidden_calls=0 class=FIXTURE physical=0 human=0 guest_code_executed=0 device_access=0 network_access=0"
