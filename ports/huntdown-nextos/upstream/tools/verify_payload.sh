#!/bin/sh
# Verify one supported, internally correlated Huntdown AArch64 payload.
set -eu

usage() {
  echo "Usage: verify_payload.sh RUNTIME_DIR" >&2
  exit 2
}

[ "$#" -eq 1 ] || usage
runtime=${1%/}
[ -d "$runtime" ] || {
  echo "Runtime directory not found: $runtime" >&2
  exit 1
}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

for tool in python3 unzip; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "Missing required host tool: $tool" >&2
    exit 1
  }
done

python3 "$script_dir/validate_huntdown_data.py" --stage "$runtime"

pack_entries=$(unzip -Z1 "$runtime/UnityDataAssetPack.apk")
[ "$pack_entries" = assets/bin/Data/datapack.unity3d ] || {
  echo "UnityDataAssetPack.apk has an unexpected layout" >&2
  exit 1
}
unzip -tq "$runtime/UnityDataAssetPack.apk" >/dev/null

if command -v ffprobe >/dev/null 2>&1; then
  for video in CoffeeIntro.mp4 EasyTriggerVignette.mp4; do
    video_info=$(ffprobe -v error -select_streams v:0 \
      -show_entries stream=codec_name,width,height \
      -of csv=p=0 "$runtime/video/$video")
    [ "$video_info" = h264,1920,1080 ] || {
      echo "Unexpected video stream in $video: $video_info" >&2
      exit 1
    }
    audio_info=$(ffprobe -v error -select_streams a:0 \
      -show_entries stream=codec_name,sample_rate,channels \
      -of csv=p=0 "$runtime/video/$video")
    [ "$audio_info" = aac,48000,2 ] || {
      echo "Unexpected audio stream in $video: $audio_info" >&2
      exit 1
    }
  done
fi

echo "Huntdown payload verified: AArch64 profile and bridge signatures intact"
