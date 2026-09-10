#!/usr/bin/env bash
# Native X5M process scopes shared by the public package and its Git source.
#
# NXExtract runs before the i386 guest exists and must use the firmware's
# proven SDL2/KMSDRM handoff. The packaged sdl2-compat is private to the
# Box32 game process and must never leak into the native extractor UI.

asm2_x5m_run_extractor() {
  local game_dir=$1 control_folder=$2

  LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$control_folder/libs:$control_folder/libs.aarch64" \
    NXEXTRACT_GAME_DIR="$game_dir" \
    "$game_dir/run-extractor.sh" --abi x86
}

asm2_x5m_run_game() {
  local game_dir=$1 native_lib_dir=$2 box32_bin=$3

  (
    cd -- "$game_dir" || exit 1
    LD_LIBRARY_PATH="$native_lib_dir:/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$box32_bin" ./asm2_127_x86_box32 "$game_dir"
  )
}
