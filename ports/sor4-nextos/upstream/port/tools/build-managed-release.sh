#!/usr/bin/env bash
# Rebuild every SOR4-managed release artifact without workstation paths or PDBs.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export NUGET_XMLDOC_MODE=skip
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1783900800}

fail() {
    printf 'managed build error: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '[managed build] %s\n' "$*"
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
BUILD_DIR="$PORT_DIR/build"
PATCH_DIR="$PORT_DIR/port/monogame-gles-patches"
HOST_PROJECT="$PORT_DIR/port/host/host.csproj"
BRIDGE_PROJECT="$SCRIPT_DIR/bridge/bridge.csproj"
STUBS_DIR=${SOR4_STUBS_DIR:-"$BUILD_DIR/stubs"}
PACKAGE_TOOLS="$PORT_DIR/port/package/tools"
CACHE_DIR="$BUILD_DIR/cache"
WORK_DIR="$BUILD_DIR/managed-release-work"
OUT_DIR="$BUILD_DIR/managed-release"
HOST_PACKAGE="$BUILD_DIR/host_pkg"

MONOGAME_COMMIT=caaa1170c48b8d340690014423e1c8bbc0777899
MONOGAME_SHA256=691831ff43146119a98a5d649062a91d029acec21975b6e2891104f7e6003967
MONOGAME_URL="https://codeload.github.com/MonoGame/MonoGame/tar.gz/$MONOGAME_COMMIT"
MONOGAME_TARBALL=${SOR4_MONOGAME_TARBALL:-"$CACHE_DIR/monogame-$MONOGAME_COMMIT.tar.gz"}
DOTNET_SDK_VERSION=9.0.315

PATCH_TOOLS=(
    coopsplit
    fixplatform
    noopm
    patchgam
    rettrue
    skipcall
    skipvideo
    verstub
)

for command in basename cat cmp curl dirname find grep install mkdir mv python3 \
               readelf rm sed sha256sum strings tar; do
    command -v "$command" >/dev/null 2>&1 || fail "missing host command: $command"
done

DOTNET=${DOTNET:-}
if [[ -z "$DOTNET" ]]; then
    DOTNET=$(command -v dotnet || true)
fi
[[ -n "$DOTNET" && -x "$DOTNET" ]] || \
    fail "set DOTNET to the executable from .NET SDK $DOTNET_SDK_VERSION"
[[ "$($DOTNET --version)" == "$DOTNET_SDK_VERSION" ]] || \
    fail "expected .NET SDK $DOTNET_SDK_VERSION, found $($DOTNET --version)"
export DOTNET_ROOT=$(cd -- "$(dirname -- "$DOTNET")" && pwd -P)
export DOTNET_CLI_HOME="$BUILD_DIR/.dotnet-home"

for input in \
    "$STUBS_DIR/Mono.Android.dll" \
    "$STUBS_DIR/Java.Interop.dll" \
    "$PATCH_DIR/apply.py" \
    "$PATCH_DIR/MonoGame.Framework.SOR4GLES.csproj" \
    "$HOST_PROJECT" \
    "$BRIDGE_PROJECT"; do
    [[ -f "$input" ]] || fail "missing build input: $input"
done

mkdir -p -- "$CACHE_DIR" "$DOTNET_CLI_HOME" "$(dirname -- "$MONOGAME_TARBALL")"
if [[ ! -f "$MONOGAME_TARBALL" ]]; then
    note "downloading pinned MonoGame source"
    curl -fL "$MONOGAME_URL" -o "$MONOGAME_TARBALL.part"
    printf '%s  %s\n' "$MONOGAME_SHA256" "$MONOGAME_TARBALL.part" | sha256sum -c -
    mv -f -- "$MONOGAME_TARBALL.part" "$MONOGAME_TARBALL"
fi
printf '%s  %s\n' "$MONOGAME_SHA256" "$MONOGAME_TARBALL" | sha256sum -c -

# Only these two private build directories are replaced. Existing APK-derived
# data and native wrappers in build/host_pkg are deliberately left alone.
rm -rf -- "$WORK_DIR" "$OUT_DIR"
mkdir -p -- \
    "$WORK_DIR/MonoGame" \
    "$OUT_DIR/bridge" \
    "$OUT_DIR/host-publish" \
    "$OUT_DIR/tools"

tar -xzf "$MONOGAME_TARBALL" --strip-components=1 \
    --no-same-owner -C "$WORK_DIR/MonoGame"
MONOGAME_FRAMEWORK="$WORK_DIR/MonoGame/MonoGame.Framework"
MONOGAME_PROJECT="$MONOGAME_FRAMEWORK/MonoGame.Framework.SOR4GLES.csproj"

note "applying maintained GLES/ASTC/streaming patches to pristine MonoGame"
python3 "$PATCH_DIR/apply.py" "$MONOGAME_FRAMEWORK"
install -m 0644 -- "$PATCH_DIR/MonoGame.Framework.SOR4GLES.csproj" "$MONOGAME_PROJECT"

# No portable/embedded PDB is emitted. PathMap is still applied so compiler
# generated metadata cannot expose the checkout if a future source adds it.
PATH_MAP="$PORT_DIR=/_/src"
BUILD_PROPS=(
    "-p:DebugType=None"
    "-p:DebugSymbols=false"
    "-p:PathMap=$PATH_MAP"
    "-p:ContinuousIntegrationBuild=true"
    "-p:Deterministic=true"
    "-p:DeterministicSourcePaths=true"
    "-p:UseSharedCompilation=false"
)

note "building SOR4Bridge"
"$DOTNET" build "$BRIDGE_PROJECT" -c Release -o "$OUT_DIR/bridge" --tl:off \
    "${BUILD_PROPS[@]}" \
    "-p:Sor4BuildDir=$BUILD_DIR" \
    "-p:Sor4ReferenceDir=$STUBS_DIR"
BRIDGE_DLL="$OUT_DIR/bridge/SOR4Bridge.dll"
[[ -f "$BRIDGE_DLL" ]] || fail "bridge build did not produce SOR4Bridge.dll"

note "publishing self-contained AArch64 host and patched MonoGame"
"$DOTNET" publish "$HOST_PROJECT" -c Release -r linux-arm64 --self-contained true --tl:off \
    -o "$OUT_DIR/host-publish" \
    "${BUILD_PROPS[@]}" \
    "-p:Sor4BuildDir=$BUILD_DIR" \
    "-p:Sor4ReferenceDir=$STUBS_DIR" \
    "-p:Sor4BridgeAssembly=$BRIDGE_DLL" \
    "-p:Sor4MonoGameProject=$MONOGAME_PROJECT"

for output in \
    "$OUT_DIR/host-publish/sor4host" \
    "$OUT_DIR/host-publish/sor4host.dll" \
    "$OUT_DIR/host-publish/sor4host.deps.json" \
    "$OUT_DIR/host-publish/sor4host.runtimeconfig.json" \
    "$OUT_DIR/host-publish/MonoGame.Framework.dll" \
    "$OUT_DIR/host-publish/SOR4Bridge.dll"; do
    [[ -f "$output" ]] || fail "host publish is missing: $(basename -- "$output")"
done
cmp -s -- "$BRIDGE_DLL" "$OUT_DIR/host-publish/SOR4Bridge.dll" || \
    fail "published SOR4Bridge differs from the validated bridge build"
readelf -h "$OUT_DIR/host-publish/sor4host" | \
    grep -Fq 'Machine:                           AArch64' || \
    fail "published sor4host is not AArch64"

CECIL_DLL=
for tool in "${PATCH_TOOLS[@]}"; do
    project="$SCRIPT_DIR/$tool/$tool.csproj"
    output="$OUT_DIR/tools/$tool"
    [[ -f "$project" ]] || fail "missing tool project: $project"
    mkdir -p -- "$output"
    note "building $tool"
    "$DOTNET" build "$project" -c Release -o "$output" --tl:off "${BUILD_PROPS[@]}"
    [[ -f "$output/$tool.dll" ]] || fail "$tool build did not produce $tool.dll"
    [[ -f "$output/Mono.Cecil.dll" ]] || fail "$tool build did not resolve Mono.Cecil.dll"
    if [[ -z "$CECIL_DLL" ]]; then
        CECIL_DLL="$output/Mono.Cecil.dll"
    else
        cmp -s -- "$CECIL_DLL" "$output/Mono.Cecil.dll" || \
            fail "tools resolved different Mono.Cecil payloads"
    fi
done

note "building the streaming texture converter"
mkdir -p -- "$OUT_DIR/tools/sor4texconv"
"$DOTNET" build "$SCRIPT_DIR/texconv/texconv.csproj" -c Release --tl:off \
    -o "$OUT_DIR/tools/sor4texconv" "${BUILD_PROPS[@]}"
[[ -f "$OUT_DIR/tools/sor4texconv/sor4texconv.dll" ]] || \
    fail "texture converter build did not produce sor4texconv.dll"

CUSTOM_DLLS=(
    "$OUT_DIR/host-publish/MonoGame.Framework.dll"
    "$BRIDGE_DLL"
    "$OUT_DIR/host-publish/sor4host.dll"
)
for tool in "${PATCH_TOOLS[@]}"; do
    CUSTOM_DLLS+=("$OUT_DIR/tools/$tool/$tool.dll")
done
CUSTOM_DLLS+=("$OUT_DIR/tools/sor4texconv/sor4texconv.dll")
[[ ${#CUSTOM_DLLS[@]} -eq 12 ]] || fail "internal custom DLL inventory is not 12"

UNSAFE_PATTERN='/home/|/root/|/Users/|/mnt/|[A-Za-z]:\\Users\\|192\.168\.|sshpass|ark@|root@|felipe@'
scan_safe() {
    local candidate=$1 matches
    matches=$(strings -a "$candidate" 2>/dev/null | grep -E "$UNSAFE_PATTERN" || true)
    [[ -z "$matches" ]] || {
        printf '%s\n' "$matches" >&2
        fail "workstation/private string in $candidate"
    }
}

note "scanning generated payloads for workstation/private paths"
while IFS= read -r -d '' candidate; do
    scan_safe "$candidate"
done < <(find "$OUT_DIR/host-publish" -maxdepth 1 -type f -print0)
for candidate in "${CUSTOM_DLLS[@]}" "$CECIL_DLL"; do
    scan_safe "$candidate"
done
if find "$OUT_DIR" -type f -name '*.pdb' -print -quit | grep -q .; then
    fail "a PDB was emitted despite the release build properties"
fi

# Publish only the clean self-contained host files; XML/PDB/build metadata and
# every unrelated file type remain outside the runtime package directory.
mkdir -p -- "$HOST_PACKAGE" "$PACKAGE_TOOLS"
while IFS= read -r -d '' source; do
    name=$(basename -- "$source")
    case "$name" in
        *.dll|*.json|*.so|createdump|sor4host)
            mode=0644
            case "$name" in createdump|sor4host) mode=0755 ;; esac
            install -m "$mode" -- "$source" "$HOST_PACKAGE/$name"
            ;;
    esac
done < <(find "$OUT_DIR/host-publish" -maxdepth 1 -type f -print0)

install -m 0644 -- "$CECIL_DLL" \
    "$HOST_PACKAGE/Mono.Cecil.dll"
install -m 0644 -- "$CECIL_DLL" \
    "$PACKAGE_TOOLS/Mono.Cecil.dll"
for tool in "${PATCH_TOOLS[@]}"; do
    install -m 0644 -- "$OUT_DIR/tools/$tool/$tool.dll" \
        "$PACKAGE_TOOLS/$tool.dll"
done
install -m 0644 -- "$OUT_DIR/tools/sor4texconv/sor4texconv.dll" \
    "$PACKAGE_TOOLS/sor4texconv.dll"

# Stale symbols from earlier local builds are never release inputs, but removing
# these known files also prevents accidental manual packaging of them.
rm -f -- \
    "$HOST_PACKAGE/MonoGame.Framework.pdb" \
    "$HOST_PACKAGE/SOR4Bridge.pdb" \
    "$HOST_PACKAGE/sor4host.pdb"

FINAL_DLLS=(
    "$HOST_PACKAGE/MonoGame.Framework.dll"
    "$HOST_PACKAGE/SOR4Bridge.dll"
    "$HOST_PACKAGE/sor4host.dll"
)
for tool in "${PATCH_TOOLS[@]}"; do
    FINAL_DLLS+=("$PACKAGE_TOOLS/$tool.dll")
done
FINAL_DLLS+=("$PACKAGE_TOOLS/sor4texconv.dll")

for candidate in "${FINAL_DLLS[@]}" \
                 "$HOST_PACKAGE/Mono.Cecil.dll" \
                 "$PACKAGE_TOOLS/Mono.Cecil.dll"; do
    [[ -f "$candidate" ]] || fail "missing installed release artifact: $candidate"
    scan_safe "$candidate"
done

MANIFEST="$OUT_DIR/CUSTOM-DLLS.sha256"
for candidate in "${FINAL_DLLS[@]}"; do
    sha256sum -- "$candidate"
done | sed "s#$PORT_DIR/#ports/sor4/#" > "$MANIFEST"

note "clean 12-DLL manifest"
cat -- "$MANIFEST"
note "managed release artifacts are ready"
