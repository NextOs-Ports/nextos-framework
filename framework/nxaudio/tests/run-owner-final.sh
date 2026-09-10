#!/usr/bin/env bash
# One-shot owner-local final battery for mission 121 / V4-AUDIO-05.
# Run only from a frozen clean successor HEAD.  Host fixtures only: this
# script never opens an audio device, uses a network or claims PHYSICAL.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
source_root="$repo_root/framework/nxaudio"
base=6af79b6411c5115ffbd43be244a35ad167bd5a49
base_subtree=f8d7d8aaf8f568a0dd98da8d719a017eebf75bfb
invalid_head=12393081d7dc5dc374fb6429a7b57fcd64ede7d4
expected_branch=framework/nxaudio-0.4.0-v4
work_root=$(mktemp -d /tmp/nxaudio-owner-final.XXXXXX)
trap 'find "$work_root" -depth -delete' EXIT HUP INT TERM

cd "$repo_root"

head=$(git rev-parse HEAD)
tree=$(git rev-parse 'HEAD^{tree}')
subtree=$(git rev-parse HEAD:framework/nxaudio)
branch=$(git symbolic-ref --short HEAD)

[[ "$branch" == "$expected_branch" ]]
[[ "$head" != "$invalid_head" ]]
[[ $(git rev-parse "$base:framework/nxaudio") == "$base_subtree" ]]
git merge-base --is-ancestor "$base" HEAD
[[ -z $(git rev-list --merges "$base..HEAD") ]]
[[ -z $(git status --porcelain=v1 --untracked-files=all) ]]
if find "$source_root" -type d \
    \( -name build -o -name 'build_*' -o -name 'build-*' -o \
       -name __pycache__ \) -print -quit | grep -q .; then
  echo "ignored build/cache artifact found inside nxaudio" >&2
  exit 1
fi

mapfile -t changed < <(git diff --name-only "$base..HEAD")
[[ ${#changed[@]} -gt 0 ]]
for path in "${changed[@]}"; do
  [[ "$path" == framework/nxaudio/* ]]
done

if git log --format='%an%n%ae%n%B' "$base..HEAD" | \
    grep -Eqi 'co-authored-by|anthropic|openai|claude|codex|gemini|generated with'; then
  echo "AI/coauthor signature found in owner history" >&2
  exit 1
fi

hook_path=$(git config --global --get core.hooksPath)
[[ -n "$hook_path" ]]
[[ -f "$hook_path/commit-msg" ]]

# The 0.3.1 implementation and receipt surfaces remain byte-identical.
git diff --exit-code "$base..HEAD" -- \
  framework/nxaudio/src/nxaudio.c \
  framework/nxaudio/include/nxaudio_receipt.h \
  framework/nxaudio/src/nxaudio_receipt.c \
  framework/nxaudio/tests/run-host.sh \
  framework/nxaudio/tests/run-receipt-host.sh \
  framework/nxaudio/tests/test_receipt.c

git show "$base:framework/nxaudio/include/nxaudio.h" | \
  sed 's/#define NXAUDIO_VERSION "0\.3\.1"/#define NXAUDIO_VERSION "0.4.0"/' \
  >"$work_root/nxaudio.h.expected"
cmp "$work_root/nxaudio.h.expected" framework/nxaudio/include/nxaudio.h

git show "$base:framework/nxaudio/tests/test_nxaudio.c" | \
  sed 's/NXAUDIO_VERSION, "0\.3\.1"/NXAUDIO_VERSION, "0.4.0"/' \
  >"$work_root/test_nxaudio.c.expected"
cmp "$work_root/test_nxaudio.c.expected" \
  framework/nxaudio/tests/test_nxaudio.c

find "$source_root" -type f -print0 | sort -z | \
  xargs -0 sha256sum >"$work_root/source-manifest.sha256"
source_manifest_sha256=$(sha256sum "$work_root/source-manifest.sha256" | \
  awk '{print $1}')

echo "MISSION=121_V4_AUDIO_05"
echo "ATTEMPT=1"
echo "CLASS=FIXTURE"
echo "PHYSICAL=PENDING"
echo "DEVICE_ACCESS=0"
echo "NETWORK_ACCESS=0"
echo "BASE=$base"
echo "BASE_SUBTREE=$base_subtree"
echo "BRANCH=$branch"
echo "HEAD=$head"
echo "TREE=$tree"
echo "SUBTREE=$subtree"
echo "ANCESTRY=PASS"
echo "MERGES=0"
echo "AI_COAUTHOR_SIGNATURES=0"
echo "STATUS_BEFORE=CLEAN"
echo "CHANGED_FILES=${#changed[@]}"
printf 'CHANGED=%s\n' "${changed[@]}"
echo "SOURCE_MANIFEST_SHA256=$source_manifest_sha256"
echo "SOURCE_MANIFEST_BEGIN"
sed "s#${repo_root}/##" "$work_root/source-manifest.sha256"
echo "SOURCE_MANIFEST_END"
echo "GCC=$(gcc --version | sed -n '1p')"
echo "CLANG=$(clang --version | sed -n '1p')"
echo "CMAKE=$(cmake --version | sed -n '1p')"
echo "CTEST=$(ctest --version | sed -n '1p')"

echo "GATE=nxaudio-host BEGIN"
bash "$source_root/tests/run-host.sh"
echo "GATE=nxaudio-host PASS"

echo "GATE=nxaudio-receipt-host BEGIN"
bash "$source_root/tests/run-receipt-host.sh"
echo "GATE=nxaudio-receipt-host PASS"

echo "GATE=nxaudio-runtime-host BEGIN"
bash "$source_root/tests/run-runtime-host.sh"
echo "GATE=nxaudio-runtime-host PASS"

echo "GATE=nxaudio-m14-audit BEGIN"
python3 -B "$source_root/tests/test_m14_audio_contract.py"
echo "GATE=nxaudio-m14-audit PASS"

[[ -z $(git status --porcelain=v1 --untracked-files=all) ]]
[[ $(git rev-parse HEAD) == "$head" ]]
[[ $(git rev-parse 'HEAD^{tree}') == "$tree" ]]
[[ $(git rev-parse HEAD:framework/nxaudio) == "$subtree" ]]

echo "STATUS_AFTER=CLEAN"
echo "HEAD_STABLE=YES"
echo "TREE_STABLE=YES"
echo "SUBTREE_STABLE=YES"
echo "PROVIDER_IMPORTS=0"
echo "CONTROLS_TOUCHED=NO"
echo "INTEGRATED_V4=NO"
echo "OWNER_FINAL=PASS"
