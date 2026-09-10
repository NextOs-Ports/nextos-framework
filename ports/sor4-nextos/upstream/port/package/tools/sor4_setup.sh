#!/bin/bash
# Transactional, resumable first-run setup for the SOR4 PortMaster package.
set -o pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
GAMEDIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P) || exit 1
PKG="$GAMEDIR/host_pkg"
STAGE="$GAMEDIR/.setup.stage"
BACKUP="$GAMEDIR/.setup.backup"
LOCK="$GAMEDIR/.setup.lock"
COMMIT="$GAMEDIR/.setup.commit"
COMMIT_NEW="$COMMIT.new"
COMMITTED="$GAMEDIR/.setup.committed"
LOG="$GAMEDIR/setup.log"
DONE="$GAMEDIR/.setup_done"
DONE_CANDIDATE="$DONE.new"
SETUP_VERSION=3
APK_ASSET_FINGERPRINT=
APK_ASSET_COUNT=
APK_MISSING_XNB_TOKEN=
APK_FILES=()
APK_BUNDLES=()
APK_ERROR=
LOCK_HELD=
UI_CTL="$GAMEDIR/.setup.ui.ctl"
UI_STOP="$GAMEDIR/.setup.ui.stop"
UI_SPLASH=
UI_POLLER=

log() {
    printf '[setup] %s\n' "$*" | tee -a "$LOG"
}

fail() {
    log "ERROR: $*"
    return 1
}

durable_sync() {
    sync || {
        log "ERROR: storage sync failed"
        return 1
    }
}

file_size() {
    [ -f "$1" ] || return 1
    wc -c < "$1" 2>/dev/null
}

require_setup_tools() {
    local tool payload
    for tool in python3 sha256sum find awk wc df du sync sed tail tee grep head tr; do
        command -v "$tool" >/dev/null 2>&1 || {
            fail "required system tool is missing: $tool"
            return 1
        }
    done
    for payload in sor4_profile.sh sor4_apkset.py validate-sor4-apk.py sor4_apkextract.py wwise_extract.py \
                   patchgam.dll noopm.dll skipcall.dll fixplatform.dll \
                   skipvideo.dll verstub.dll rettrue.dll coopsplit.dll Mono.Cecil.dll; do
        [ -r "$SCRIPT_DIR/$payload" ] || {
            fail "package payload is missing: tools/$payload"
            return 1
        }
    done
    [ -x "$PKG/sor4host" ] && [ -r "$PKG/SOR4Bridge.dll" ] || {
        fail "packaged managed host/bridge is missing"
        return 1
    }
    if declare -F sor4_sdl3_available >/dev/null 2>&1 && sor4_sdl3_available; then
        [ -r "$PKG/sdl3compat/libSDL2-2.0.so.0" ] || {
            fail "SDL3 firmware requires packaged host_pkg/sdl3compat"
            return 1
        }
    fi
    if [ "${SOR4_TEXTURE_MODE:-etc1}" != astc ]; then
        [ -r "$SCRIPT_DIR/sor4texconv.dll" ] && [ -r "$PKG/libs/libsor4astc.so" ] || {
            fail "texture conversion payload is missing"
            return 1
        }
    fi
}

process_running() {
    local pid=$1 state command_line
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$pid" 2>/dev/null || return 1
    state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)
    [ "$state" != Z ] || return 1
    command_line=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null) || return 1
    case "$command_line" in *sor4_setup.sh*) return 0 ;; *) return 1 ;; esac
}

release_lock() {
    [ -n "$LOCK_HELD" ] || return 0
    [ "$(sed -n '1p' "$LOCK/pid" 2>/dev/null)" = "$$" ] && rm -rf -- "$LOCK"
    LOCK_HELD=
}

stop_ui() {
    local pid attempt
    [ -n "$UI_SPLASH" ] || return 0
    touch "$UI_STOP"
    for pid in "$UI_POLLER" "$UI_SPLASH"; do
        [ -n "$pid" ] || continue
        kill "$pid" 2>/dev/null || true
        for attempt in 1 2 3; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -f -- "$UI_CTL" "$UI_STOP"
    UI_SPLASH=
    UI_POLLER=
}

start_ui() {
    local image="$SCRIPT_DIR/sor4bake.rgba" splash_ld="${LD_LIBRARY_PATH:-}" compat=""
    [ -x "$SCRIPT_DIR/sor4splash" ] || return 0
    [ "$(file_size "$image")" = 1228800 ] || return 0
    # Use the exact same SDL2/SDL3 policy as the hardware probe and game.
    declare -F sor4_sdl_compat_dir >/dev/null 2>&1 &&
        compat=$(sor4_sdl_compat_dir "$GAMEDIR" 2>/dev/null || true)
    [ -z "$compat" ] || splash_ld="$compat${splash_ld:+:$splash_ld}"
    rm -f -- "$UI_STOP"
    printf '1 0 100\n' > "$UI_CTL"
    LD_LIBRARY_PATH="$splash_ld" \
      "$SCRIPT_DIR/sor4splash" "$image" 640 480 "$UI_CTL" "$UI_STOP" \
        >/dev/null 2>&1 &
    UI_SPLASH=$!
    (
        while kill -0 "$UI_SPLASH" 2>/dev/null && [ ! -e "$UI_STOP" ]; do
            pair=$(tail -n 200 "$LOG" 2>/dev/null | grep -aoE '[0-9]+/[0-9]+' | tail -n 1)
            if [ -n "$pair" ]; then
                current=${pair%/*}; total=${pair#*/}
                printf '1 %s %s\n' "$current" "$total" > "$UI_CTL"
            fi
            sleep 1
        done
    ) &
    UI_POLLER=$!
}

cleanup() {
    stop_ui
    release_lock
}

run_logged() {
    "$@" 2>&1 | tee -a "$LOG"
    return "${PIPESTATUS[0]}"
}

acquire_lock() {
    local owner
    if ! mkdir -- "$LOCK" 2>/dev/null; then
        owner=$(sed -n '1p' "$LOCK/pid" 2>/dev/null || true)
        if [ -n "$owner" ] && process_running "$owner"; then
            fail "another setup is active (pid=$owner)"
            return 1
        fi
        rm -rf -- "$LOCK" || return 1
        mkdir -- "$LOCK" || return 1
    fi
    printf '%s\n' "$$" > "$LOCK/pid" || return 1
    LOCK_HELD=1
}

safe_relative_path() {
    case "$1" in
        ''|/*|../*|*/../*|*/..|*/./*|./*) return 1 ;;
        *) return 0 ;;
    esac
}

remove_path() {
    [ -e "$1" ] || [ -L "$1" ] || return 0
    if [ -d "$1" ] && [ ! -L "$1" ]; then rm -rf -- "$1"; else rm -f -- "$1"; fi
}

metadata_value() {
    local marker=$1 wanted=$2
    [ -f "$marker" ] || return 1
    awk -F= -v wanted="$wanted" '
        $1 == wanted {
            value = substr($0, length($1) + 2)
            if (value ~ /^[A-Za-z0-9._-]+$/) {
                print value
                found = 1
            }
            exit
        }
        END { if (!found) exit 1 }
    ' "$marker"
}

metadata_is_well_formed() {
    local marker=$1 version data mode scale fingerprint count missing
    version=$(metadata_value "$marker" setup_version 2>/dev/null) || return 1
    data=$(metadata_value "$marker" data_version 2>/dev/null) || return 1
    mode=$(metadata_value "$marker" texture_mode 2>/dev/null) || return 1
    scale=$(metadata_value "$marker" texture_scale 2>/dev/null) || return 1
    fingerprint=$(metadata_value "$marker" asset_fingerprint 2>/dev/null) || return 1
    [ "$version" = "$SETUP_VERSION" ] && [ "$data" = 1.4.5 ] || return 1
    case "$mode" in astc|etc2|etc1) ;; *) return 1 ;; esac
    case "$scale" in 1|2|3|4|5|6|7|8) ;; *) return 1 ;; esac
    [ -n "$fingerprint" ] || return 1

    # Markers written before 2.0.1 have neither field and always describe the
    # complete 25,905-asset tree. New compatibility markers must carry both.
    count=$(metadata_value "$marker" asset_count 2>/dev/null || true)
    missing=$(metadata_value "$marker" missing_xnb_token 2>/dev/null || true)
    if [ -z "$count" ] && [ -z "$missing" ]; then
        return 0
    fi
    case "$count:$missing" in
        25905:none) return 0 ;;
        25904:*)
            case "$missing" in ''|*[!0-9a-f]*) return 1 ;; esac
            [ "${#missing}" = 64 ]
            ;;
        *) return 1 ;;
    esac
}

metadata_asset_count() {
    local marker=$1 count
    metadata_is_well_formed "$marker" || return 1
    count=$(metadata_value "$marker" asset_count 2>/dev/null || true)
    case "$count" in
        '') printf '25905\n' ;;
        25904|25905) printf '%s\n' "$count" ;;
        *) return 1 ;;
    esac
}

metadata_matches_current() {
    local marker=$1 mode scale fingerprint
    metadata_is_well_formed "$marker" || return 1
    mode=$(metadata_value "$marker" texture_mode) || return 1
    scale=$(metadata_value "$marker" texture_scale) || return 1
    fingerprint=$(metadata_value "$marker" asset_fingerprint) || return 1
    [ "$mode" = "${SOR4_TEXTURE_MODE:-}" ] &&
        [ "$scale" = "${SOR4_BAKE_SCALE:-}" ] || return 1
    [ -z "$APK_ASSET_FINGERPRINT" ] || [ "$fingerprint" = "$APK_ASSET_FINGERPRINT" ]
}

metadata_same_recipe() {
    local left=$1 right=$2 key a b
    metadata_is_well_formed "$left" || return 1
    metadata_is_well_formed "$right" || return 1
    for key in data_version texture_mode texture_scale asset_fingerprint; do
        a=$(metadata_value "$left" "$key") || return 1
        b=$(metadata_value "$right" "$key") || return 1
        [ "$a" = "$b" ] || return 1
    done
}

write_setup_metadata() {
    local output=$1 temporary="$1.tmp"
    {
        printf 'setup_version=%s\n' "$SETUP_VERSION"
        printf 'data_version=1.4.5\n'
        printf 'profile=%s\n' "${SOR4_PROFILE_EFFECTIVE:-unknown}"
        printf 'texture_mode=%s\n' "$SOR4_TEXTURE_MODE"
        printf 'texture_scale=%s\n' "$SOR4_BAKE_SCALE"
        printf 'asset_fingerprint=%s\n' "${APK_ASSET_FINGERPRINT:-native-v145-verified}"
        printf 'asset_count=%s\n' "${APK_ASSET_COUNT:-25905}"
        printf 'missing_xnb_token=%s\n' "${APK_MISSING_XNB_TOKEN:-none}"
    } > "$temporary" || return 1
    mv -f -- "$temporary" "$output"
}

finish_committed_setup() {
    [ -f "$COMMITTED" ] || return 0
    log "finishing an installation whose payload commit was already durable"
    if [ -f "$DONE_CANDIDATE" ]; then
        metadata_is_well_formed "$DONE_CANDIDATE" || return 1
        [ ! -f "$STAGE/.setup_recipe" ] ||
            metadata_same_recipe "$DONE_CANDIDATE" "$STAGE/.setup_recipe" || return 1
        rm -f -- "$COMMIT"
        durable_sync || return 1
        mv -f -- "$DONE_CANDIDATE" "$DONE" || return 1
        durable_sync || return 1
    else
        metadata_is_well_formed "$DONE" || return 1
        [ ! -f "$STAGE/.setup_recipe" ] ||
            metadata_same_recipe "$DONE" "$STAGE/.setup_recipe" || return 1
        rm -f -- "$COMMIT"
        durable_sync || return 1
    fi
    rm -f -- "$COMMITTED"
    durable_sync || return 1
    rm -rf -- "$BACKUP" "$STAGE"
    durable_sync || return 1
}

rollback_commit() {
    local rel
    [ -f "$COMMIT" ] || return 0
    log "recovering an interrupted installation commit"
    validate_commit_manifest "$COMMIT" || {
        log "refusing unsafe or incomplete commit manifest"
        return 1
    }
    while IFS= read -r rel || [ -n "$rel" ]; do
        safe_relative_path "$rel" || return 1
        if [ -e "$BACKUP/$rel" ] || [ -L "$BACKUP/$rel" ]; then
            if { [ -e "$GAMEDIR/$rel" ] || [ -L "$GAMEDIR/$rel" ]; } &&
               [ ! -e "$STAGE/$rel" ] && [ ! -L "$STAGE/$rel" ]; then
                mkdir -p -- "$(dirname -- "$STAGE/$rel")" || return 1
                mv -- "$GAMEDIR/$rel" "$STAGE/$rel" || return 1
            else
                remove_path "$GAMEDIR/$rel"
            fi
            mkdir -p -- "$(dirname -- "$GAMEDIR/$rel")" || return 1
            mv -- "$BACKUP/$rel" "$GAMEDIR/$rel" || return 1
        elif [ ! -e "$STAGE/$rel" ] && [ ! -L "$STAGE/$rel" ]; then
            # This was a newly installed path. Put it back in the stage so a
            # multi-hour low-memory bake can resume instead of being discarded.
            if [ -e "$GAMEDIR/$rel" ] || [ -L "$GAMEDIR/$rel" ]; then
                mkdir -p -- "$(dirname -- "$STAGE/$rel")" || return 1
                mv -- "$GAMEDIR/$rel" "$STAGE/$rel" || return 1
            fi
        fi
    done < "$COMMIT"
    durable_sync || return 1
    rm -f -- "$COMMIT" "$COMMIT_NEW" "$COMMITTED" "$DONE_CANDIDATE"
    durable_sync || return 1
    rm -rf -- "$BACKUP"
}

recover_setup_state() {
    if [ -f "$COMMITTED" ]; then
        finish_committed_setup
    elif [ -f "$COMMIT" ]; then
        rollback_commit
    else
        # A candidate without a commit manifest was never allowed to publish.
        rm -f -- "$DONE_CANDIDATE" "$DONE_CANDIDATE.tmp" "$COMMIT_NEW"
        rm -rf -- "$BACKUP"
    fi
}

add_apk_source() {
    local candidate=$1 current
    [ -f "$candidate" ] && [ ! -L "$candidate" ] || return 0
    for current in "${APK_FILES[@]}"; do
        [ "$current" = "$candidate" ] && return 0
    done
    APK_FILES+=("$candidate")
}

add_apk_bundle() {
    local candidate=$1 current
    [ -f "$candidate" ] && [ ! -L "$candidate" ] || return 0
    for current in "${APK_BUNDLES[@]}"; do
        [ "$current" = "$candidate" ] && return 0
    done
    APK_BUNDLES+=("$candidate")
}

find_apk_sources() {
    local dir candidate found=0
    APK_FILES=()
    APK_BUNDLES=()
    APK_ERROR=
    shopt -s nullglob
    for dir in "$GAMEDIR/gamedata" "$GAMEDIR"; do
        [ -d "$dir" ] || continue
        for candidate in "$dir"/*.apk "$dir"/*.APK; do
            add_apk_source "$candidate"; found=1
        done
        for candidate in "$dir"/*.apks "$dir"/*.APKS \
                         "$dir"/*.apkm "$dir"/*.APKM \
                         "$dir"/*.xapk "$dir"/*.XAPK; do
            add_apk_bundle "$candidate"; found=1
        done
    done
    shopt -u nullglob
    [ "$found" = 1 ] && return 0
    APK_ERROR="copy the legal SOR4 Android 1.4.5 APK, complete split set, APKS, APKM or XAPK into sor4/gamedata"
    return 1
}

preflight_bundle_space() {
    local bytes=$1 cache=$2 available_kb cache_kb required_kb total_kb
    case "$bytes" in ''|*[!0-9]*) return 1 ;; esac
    [ "$bytes" -gt 0 ] || return 0
    available_kb=$(df -Pk "$GAMEDIR" 2>/dev/null | awk 'END { print $4 }')
    case "$available_kb" in
        ''|*[!0-9]*) log "warning: could not measure free storage for APK bundle cache"; return 0 ;;
    esac
    cache_kb=$(du -sk "$cache" 2>/dev/null | awk '{ print $1 }')
    case "$cache_kb" in ''|*[!0-9]*) cache_kb=0 ;; esac
    required_kb=$(((bytes + 1023) / 1024 + 131072))
    total_kb=$((available_kb + cache_kb))
    log "bundle preflight: available+resumable=${total_kb}KiB cache+safety=${required_kb}KiB"
    [ "$total_kb" -ge "$required_kb" ] ||
        fail "not enough free storage to expand the APK bundle safely"
}

prepare_apk_set() {
    local cache="$STAGE/.apkset" list="$STAGE/.apkset.sources" bytes path
    local extracted=()
    if [ "${#APK_BUNDLES[@]}" -gt 0 ]; then
        bytes=$(python3 "$SCRIPT_DIR/sor4_apkset.py" bundle-bytes "${APK_BUNDLES[@]}") || return 1
        preflight_bundle_space "$bytes" "$cache" || return 1
        mkdir -p -- "$cache" || return 1
        log "expanding and CRC-validating ${#APK_BUNDLES[@]} APK bundle(s) transactionally"
        if ! run_logged python3 "$SCRIPT_DIR/sor4_apkset.py" \
                expand-bundles "$cache" "${APK_BUNDLES[@]}"; then
            return 1
        fi
        shopt -s nullglob
        extracted=("$cache"/*.apk)
        shopt -u nullglob
        [ "${#extracted[@]}" -gt 0 ] || return 1
        for path in "${extracted[@]}"; do add_apk_source "$path"; done
    fi
    [ "${#APK_FILES[@]}" -gt 0 ] || return 1
    : > "$list.new" || return 1
    for path in "${APK_FILES[@]}"; do
        printf '%s\n' "$path" >> "$list.new" || return 1
    done
    mv -f -- "$list.new" "$list" || return 1
    log "APK source set prepared (${#APK_FILES[@]} archive(s))"
}

validate_supported_apk() {
    local output status fingerprint count missing_token compatibility
    output=$(python3 "$SCRIPT_DIR/validate-sor4-apk.py" "${APK_FILES[@]}" 2>&1)
    status=$?
    printf '%s\n' "$output" | tee -a "$LOG"
    [ "$status" -eq 0 ] || return "$status"
    fingerprint=$(printf '%s\n' "$output" | sed -n 's/^asset_fingerprint=//p' | tail -n 1)
    case "$fingerprint" in ''|*[!0-9a-f]*) return 1 ;; esac
    [ "${#fingerprint}" = 64 ] || return 1
    count=$(printf '%s\n' "$output" | sed -n 's/^asset_count=//p' | tail -n 1)
    case "$count" in 25904|25905) ;; *) return 1 ;; esac
    compatibility=$(printf '%s\n' "$output" | sed -n 's/^compatibility=//p' | tail -n 1)
    missing_token=$(printf '%s\n' "$output" | sed -n 's/^missing_xnb_token=//p' | tail -n 1)
    if [ "$count" = 25904 ]; then
        [ "$compatibility" = one-missing-xnb ] || return 1
        case "$missing_token" in ''|*[!0-9a-f]*) return 1 ;; esac
        [ "${#missing_token}" = 64 ] || return 1
    else
        [ -z "$compatibility" ] && [ -z "$missing_token" ] || return 1
        missing_token=none
    fi
    APK_ASSET_FINGERPRINT=$fingerprint
    APK_ASSET_COUNT=$count
    APK_MISSING_XNB_TOKEN=$missing_token
}

revalidate_apk_identity() {
    local expected=$APK_ASSET_FINGERPRINT expected_count=$APK_ASSET_COUNT
    local expected_missing=$APK_MISSING_XNB_TOKEN
    validate_supported_apk || return 1
    [ "$APK_ASSET_FINGERPRINT" = "$expected" ] &&
        [ "$APK_ASSET_COUNT" = "$expected_count" ] &&
        [ "$APK_MISSING_XNB_TOKEN" = "$expected_missing" ] || {
        fail "APK identity changed while setup was running"
        return 1
    }
}

run_dll() {
    run_logged "$PKG/sor4host" --run-dll "$1" "${@:2}"
}

validate_raw_code() {
    [ "$(file_size "$STAGE/SOR4.dll")" = 1491968 ] || return 1
    [ "$(sha256sum "$STAGE/SOR4.dll" 2>/dev/null | awk '{print $1}')" = \
      45b06ce7e8f51ef7c21cb35c45597d8ac868dde9ee091819a25810830d4a7205 ]
}

validate_gameassets_dir() {
    local assets=$1 expected=${2:-${APK_ASSET_COUNT:-25905}} count hash wem_count
    case "$expected" in 25904|25905) ;; *) return 1 ;; esac
    [ -d "$assets" ] && [ ! -L "$assets" ] || return 1
    [ -f "$assets/bigfile" ] && [ ! -L "$assets/bigfile" ] || return 1
    hash=$(sha256sum "$assets/bigfile" 2>/dev/null | awk '{print $1}')
    [ "$hash" = 98489edb6bbe65c24ff7620de7e96292c65ea232f34742f13bdedac986190ccb ] || return 1
    count=$(find "$assets" -type f ! -name '*.sor4-part' 2>/dev/null | wc -l)
    [ "$count" = "$expected" ] || return 1
    wem_count=$(find "$assets" -maxdepth 1 -type f -iname '*.wem' 2>/dev/null | wc -l)
    [ "$wem_count" = 613 ] || return 1
    [ "$(file_size "$assets/353312695.wem")" = 5432250 ] || return 1
    [ "$(file_size "$assets/163823816.wem")" = 3573152 ] || return 1
    ! find "$assets" -name '*.sor4-part' -print -quit 2>/dev/null | grep -q .
}

validate_native_asset_fingerprints() {
    local assets=$1 rel expected actual
    while read -r expected rel; do
        actual=$(sha256sum "$assets/$rel" 2>/dev/null | awk '{print $1}') || return 1
        [ "$actual" = "$expected" ] || return 1
    done <<'EOF'
78c0969bbc298a926435cd83dfcc0d289e9284c35a2f675e96fe7c300e6c6fa1 animatedsprites/sprdummy/dummy02.xnb
dffc1db2663035a8bcd842a40da297b00e58cd31d08555e2e4478ec50d5067f4 animatedsprites/sprdummy/weapon/weapondummy01.xnb
1ddc25791f377296991a6fa3626cdfe3b9dd442085d30fdacab859f07bb8f668 gui/preload/title_screen.xnb
EOF
}

managed_dependency_names() {
    printf '%s\n' \
        _Microsoft.Android.Resource.Designer Xamarin.Android.Google.BillingClient \
        Xamarin.AndroidX.Activity Xamarin.AndroidX.Core Xamarin.AndroidX.Fragment \
        Xamarin.AndroidX.Lifecycle.Common.Jvm Xamarin.AndroidX.Lifecycle.LiveData.Core \
        Xamarin.AndroidX.Lifecycle.ViewModel.Android Xamarin.AndroidX.Loader \
        Xamarin.AndroidX.SavedState.SavedState.Android Xamarin.Firebase.Common \
        Xamarin.Firebase.Config Xamarin.Google.Android.Play.Core \
        Xamarin.GooglePlayServices.Auth Xamarin.GooglePlayServices.Base \
        Xamarin.GooglePlayServices.Basement Xamarin.GooglePlayServices.Drive \
        Xamarin.GooglePlayServices.Games Xamarin.GooglePlayServices.Measurement.Api \
        Xamarin.GooglePlayServices.Tasks Xamarin.Kotlin.StdLib \
        Xamarin.KotlinX.Coroutines.Core.Jvm EOSSDK.Android HelpshiftSDKx.Android \
        SharpFont.Core StandaloneTypeModel.Android.Retail
}

# Minimal set proven necessary by the native Linux runtime. Older community
# installs intentionally omitted Android-only assemblies removed by the patch.
runtime_dependency_names() {
    printf '%s\n' _Microsoft.Android.Resource.Designer \
        Xamarin.Android.Google.BillingClient Xamarin.Firebase.Config \
        Xamarin.Google.Android.Play.Core Xamarin.GooglePlayServices.Auth \
        Xamarin.GooglePlayServices.Base Xamarin.GooglePlayServices.Basement \
        Xamarin.GooglePlayServices.Games Xamarin.GooglePlayServices.Measurement.Api \
        Xamarin.GooglePlayServices.Tasks EOSSDK.Android HelpshiftSDKx.Android \
        SharpFont.Core StandaloneTypeModel.Android.Retail
}

font_payload_names() {
    printf '%s\n' FreeMono.otf NotoSans-Bold.ttf NotoSansJP-Bold.otf \
        NotoSansJP-Light.otf NotoSansKR-Bold.otf NotoSansKR-Light.otf \
        NotoSansSC-Bold.otf NotoSansSC-Light.otf Oswald-Bold.ttf \
        Oswald-BoldItalic.ttf Oswald-LightItalic.ttf OswaldJP-LightItalic.ttf
}

validate_managed_dependencies_at() {
    local root=$1 assembly
    while IFS= read -r assembly; do
        [ -f "$root/host_pkg/$assembly.dll" ] &&
            [ ! -L "$root/host_pkg/$assembly.dll" ] &&
            [ "$(head -c 2 "$root/host_pkg/$assembly.dll" 2>/dev/null)" = MZ ] || return 1
    done < <(managed_dependency_names)
}

validate_runtime_dependencies_at() {
    local root=$1 assembly
    while IFS= read -r assembly; do
        [ -f "$root/host_pkg/$assembly.dll" ] &&
            [ ! -L "$root/host_pkg/$assembly.dll" ] &&
            [ "$(head -c 2 "$root/host_pkg/$assembly.dll" 2>/dev/null)" = MZ ] || return 1
    done < <(runtime_dependency_names)
}

validate_font_payloads_at() {
    local root=$1 font
    while IFS= read -r font; do
        [ -f "$root/host_pkg/$font" ] && [ ! -L "$root/host_pkg/$font" ] &&
            [ "$(file_size "$root/host_pkg/$font")" -gt 0 ] 2>/dev/null || return 1
    done < <(font_payload_names)
}

validate_apk_support_at() {
    local root=$1 assembly hash
    [ "$(file_size "$root/SOR4.dll")" = 1491968 ] || return 1
    [ "$(sha256sum "$root/SOR4.dll" 2>/dev/null | awk '{print $1}')" = \
      45b06ce7e8f51ef7c21cb35c45597d8ac868dde9ee091819a25810830d4a7205 ] || return 1
    validate_managed_dependencies_at "$root" || return 1
    validate_font_payloads_at "$root" || return 1
    [ -f "$root/host_pkg/libs/libWwise.real.so" ] &&
        [ ! -L "$root/host_pkg/libs/libWwise.real.so" ] || return 1
    hash=$(sha256sum "$root/host_pkg/libs/libWwise.real.so" 2>/dev/null | awk '{print $1}')
    [ "$hash" = 4db3d430cde5525f3eeaa99af6e46c44feb554d89d76955b1391dfd5dd2cf0f2 ]
}

validate_live_support_at() {
    local root=$1 hash
    validate_patched_code_at "$root" || return 1
    validate_runtime_dependencies_at "$root" || return 1
    validate_font_payloads_at "$root" || return 1
    [ -f "$root/host_pkg/libs/libWwise.real.so" ] &&
        [ ! -L "$root/host_pkg/libs/libWwise.real.so" ] || return 1
    hash=$(sha256sum "$root/host_pkg/libs/libWwise.real.so" 2>/dev/null | awk '{print $1}')
    [ "$hash" = 4db3d430cde5525f3eeaa99af6e46c44feb554d89d76955b1391dfd5dd2cf0f2 ]
}

validate_current_patched_dll() {
    local dll=$1 hash
    [ -f "$dll" ] && [ ! -L "$dll" ] || return 1
    [ "$(wc -c < "$dll" 2>/dev/null)" = 1484288 ] || return 1
    hash=$(sha256sum "$dll" 2>/dev/null | awk '{print $1}')
    [ "$hash" = 41694ca016b3098f60ebacea5e76505d1c2b67c61f565f90bf791b5c6471595f ]
}

validate_rc_v2_patched_dll() {
    local dll=$1 hash
    [ -f "$dll" ] && [ ! -L "$dll" ] || return 1
    [ "$(wc -c < "$dll" 2>/dev/null)" = 1484288 ] || return 1
    hash=$(sha256sum "$dll" 2>/dev/null | awk '{print $1}')
    [ "$hash" = 3aff450035b905a1176a0fe5878be9acbec24cb7100e9d33c323acf115a788bc ]
}

validate_patched_code_at() {
    local root=$1 hash bridge_hash packaged_bridge_hash
    validate_current_patched_dll "$root/host_pkg/SOR4.dll" || return 1
    [ -f "$root/host_pkg/SOR4Bridge.dll" ] && [ ! -L "$root/host_pkg/SOR4Bridge.dll" ] || return 1
    [ "$(head -c 2 "$root/host_pkg/SOR4Bridge.dll" 2>/dev/null)" = MZ ] || return 1
    bridge_hash=$(sha256sum "$root/host_pkg/SOR4Bridge.dll" 2>/dev/null | awk '{print $1}')
    packaged_bridge_hash=$(sha256sum "$PKG/SOR4Bridge.dll" 2>/dev/null | awk '{print $1}')
    [ -n "$bridge_hash" ] && [ "$bridge_hash" = "$packaged_bridge_hash" ]
}

validate_audio_dir() {
    local audio=$1 files opus manifest_lines music_lines
    [ -f "$audio/manifest.txt" ] && [ -f "$audio/music_ids.txt" ] || return 1
    files=$(find "$audio" -maxdepth 1 -type f 2>/dev/null | wc -l)
    opus=$(find "$audio" -maxdepth 1 -type f -name '*.opus' 2>/dev/null | wc -l)
    manifest_lines=$(wc -l < "$audio/manifest.txt" 2>/dev/null) || return 1
    music_lines=$(awk 'END { print NR }' "$audio/music_ids.txt" 2>/dev/null) || return 1
    [ "$files" = 1443 ] && [ "$opus" = 1441 ] &&
        [ "$manifest_lines" = 794 ] && [ "$music_lines" = 423 ]
}

validate_assets_stage() {
    validate_gameassets_dir "$STAGE/gameassets" &&
        validate_apk_support_at "$STAGE"
}

discard_invalid_assets_stage() {
    log "discarding a completed-but-invalid asset checkpoint to avoid a retry loop"
    rm -rf -- "$STAGE/gameassets" "$STAGE/host_pkg" "$STAGE/audioout" \
        "$STAGE/.audioout.new" || return 1
    rm -f -- "$STAGE/SOR4.dll" "$STAGE/.bake_progress" \
        "$STAGE/.assets_done" "$STAGE/.code_done" "$STAGE/.audio_done" || return 1
    mkdir -p -- "$STAGE/host_pkg/libs"
}

validate_live_install() {
    local expected=${1:-${APK_ASSET_COUNT:-25905}}
    validate_gameassets_dir "$GAMEDIR/gameassets" "$expected" &&
        validate_live_support_at "$GAMEDIR" &&
        validate_audio_dir "$GAMEDIR/audioout"
}

validate_committed_install() {
    validate_live_install && validate_managed_dependencies_at "$GAMEDIR"
}

patch_code() {
    local output="$STAGE/host_pkg/SOR4.dll"
    mkdir -p -- "$STAGE/host_pkg"
    cp -f -- "$STAGE/SOR4.dll" "$output" || return 1
    cp -f -- "$PKG/SOR4Bridge.dll" "$STAGE/host_pkg/SOR4Bridge.dll" || return 1

    run_dll "$SCRIPT_DIR/patchgam.dll" "$output" "$PKG/SOR4Bridge.dll" || return 1
    run_dll "$SCRIPT_DIR/noopm.dll" "$output" "AndroidServices.*" || return 1
    run_dll "$SCRIPT_DIR/skipcall.dll" "$output" initialize save_save_game || return 1
    run_dll "$SCRIPT_DIR/skipcall.dll" "$output" save_save_game Exit || return 1
    run_dll "$SCRIPT_DIR/fixplatform.dll" "$output" || return 1
    run_dll "$SCRIPT_DIR/skipvideo.dll" "$output" || return 1
    run_dll "$SCRIPT_DIR/verstub.dll" "$output" || return 1
    run_dll "$SCRIPT_DIR/noopm.dll" "$output" EOSManager.PollMessage || return 1
    run_dll "$SCRIPT_DIR/rettrue.dll" "$output" \
        'CommonLib.platform::load_save_and_config_is_finished' || return 1
    run_dll "$SCRIPT_DIR/noopm.dll" "$output" MoreGamesNotificationUpdate || return 1
    run_dll "$SCRIPT_DIR/noopm.dll" "$output" platform.video_exists || return 1
    run_dll "$SCRIPT_DIR/coopsplit.dll" "$output" || return 1
    [ "$(head -c 2 "$output" 2>/dev/null)" = MZ ] || return 1
    [ "$(file_size "$output")" -gt 1400000 ] 2>/dev/null || return 1
}

extract_assets() {
    local resume="$STAGE/.bake_progress" apk args=()
    for apk in "${APK_FILES[@]}"; do args+=(--apk "$apk"); done
    if [ "$SOR4_TEXTURE_MODE" = astc ]; then
        log "extracting original ASTC assets with atomic per-file resume"
        run_logged python3 "$SCRIPT_DIR/sor4_apkextract.py" \
            --game-dir "$STAGE" "${args[@]}" || return 1
    else
        log "extracting runtime libraries and managed game code"
        run_logged python3 "$SCRIPT_DIR/sor4_apkextract.py" \
            --game-dir "$STAGE" "${args[@]}" --libs-only || return 1
        validate_raw_code || return 1
        log "baking $SOR4_TEXTURE_MODE textures at scale 1/$SOR4_BAKE_SCALE ($SOR4_CONV_THREADS worker)"
        if [ "$SOR4_TEXTURE_MODE" = etc2 ]; then
            run_dll "$SCRIPT_DIR/sor4texconv.dll" "${args[@]}" \
                "$STAGE/gameassets" "$SOR4_BAKE_SCALE" --etc2 --resume "$resume" || return 1
        else
            run_dll "$SCRIPT_DIR/sor4texconv.dll" "${args[@]}" \
                "$STAGE/gameassets" "$SOR4_BAKE_SCALE" --noetc2 --resume "$resume" || return 1
        fi
    fi
    validate_raw_code || return 1
}

prepare_audio() {
    local banks=() bank output="$STAGE/.audioout.new"
    shopt -s nullglob
    for bank in "$STAGE/gameassets"/*.bnk; do banks+=("$bank"); done
    shopt -u nullglob
    [ "${#banks[@]}" -gt 0 ] || return 1
    rm -rf -- "$output"
    mkdir -p -- "$output" || return 1
    local joined
    joined=$(IFS=,; printf '%s' "${banks[*]}")
    run_logged python3 "$SCRIPT_DIR/wwise_extract.py" "$joined" "$output" || return 1
    validate_audio_dir "$output" || return 1
    rm -rf -- "$STAGE/audioout"
    mv -- "$output" "$STAGE/audioout"
}

validate_stage() {
    validate_assets_stage && validate_patched_code_at "$STAGE" &&
        validate_audio_dir "$STAGE/audioout" &&
        ! find "$STAGE" -name '*.sor4-part' -print -quit 2>/dev/null | grep -q .
}

mark_stage_done() {
    local marker=$1 temporary="$1.tmp"
    durable_sync || return 1
    : > "$temporary" || return 1
    mv -f -- "$temporary" "$marker" || return 1
    durable_sync || return 1
}

prepare_stage_recipe() {
    if [ -d "$STAGE" ]; then
        if [ -f "$STAGE/.setup_recipe" ] &&
           metadata_matches_current "$STAGE/.setup_recipe"; then
            mkdir -p -- "$STAGE/host_pkg/libs"
            return
        else
            log "discarding staged data made for a different texture recipe"
            rm -rf -- "$STAGE" || return 1
        fi
    fi
    mkdir -p -- "$STAGE/host_pkg/libs" || return 1
    write_setup_metadata "$STAGE/.setup_recipe" || return 1
    durable_sync || return 1
}

preflight_disk_space() {
    local required_kb available_kb staged_kb source_cache_kb total_kb
    case "$SOR4_TEXTURE_MODE:$SOR4_BAKE_SCALE" in
        astc:*) required_kb=2100000 ;;
        etc2:1) required_kb=3200000 ;;
        etc2:2) required_kb=1600000 ;;
        etc2:*) required_kb=1100000 ;;
        etc1:1) required_kb=4500000 ;;
        etc1:2) required_kb=2300000 ;;
        etc1:*) required_kb=1300000 ;;
    esac
    available_kb=$(df -Pk "$GAMEDIR" 2>/dev/null | awk 'END { print $4 }')
    staged_kb=$(du -sk "$STAGE" 2>/dev/null | awk '{ print $1 }')
    source_cache_kb=$(du -sk "$STAGE/.apkset" 2>/dev/null | awk '{ print $1 }')
    case "$available_kb" in ''|*[!0-9]*) log "warning: could not measure free storage"; return 0 ;; esac
    case "$staged_kb" in ''|*[!0-9]*) staged_kb=0 ;; esac
    case "$source_cache_kb" in ''|*[!0-9]*) source_cache_kb=0 ;; esac
    [ "$staged_kb" -ge "$source_cache_kb" ] && staged_kb=$((staged_kb - source_cache_kb))
    total_kb=$((available_kb + staged_kb))
    log "storage preflight: available+resumable=${total_kb}KiB required=${required_kb}KiB"
    [ "$total_kb" -ge "$required_kb" ] ||
        fail "not enough free storage for this texture recipe; free space and run again"
}

legacy_marker_describes_current() {
    local version data mode scale
    [ -f "$DONE" ] || return 1
    version=$(metadata_value "$DONE" setup_version 2>/dev/null) || return 1
    data=$(metadata_value "$DONE" data_version 2>/dev/null) || return 1
    mode=$(metadata_value "$DONE" texture_mode 2>/dev/null) || return 1
    scale=$(metadata_value "$DONE" texture_scale 2>/dev/null) || return 1
    [ "$version" = 1 ] && [ "$data" = 1.4.5 ] &&
        [ "$mode" = "$SOR4_TEXTURE_MODE" ] && [ "$scale" = "$SOR4_BAKE_SCALE" ]
}

adopt_legacy_install() {
    validate_live_install || return 1
    if legacy_marker_describes_current; then
        :
    elif { [ ! -e "$DONE" ] || [ ! -s "$DONE" ]; } &&
         [ "$SOR4_TEXTURE_MODE" = astc ] && [ "$SOR4_BAKE_SCALE" = 1 ] &&
         validate_native_asset_fingerprints "$GAMEDIR/gameassets"; then
        :
    else
        return 1
    fi
    APK_ASSET_FINGERPRINT=native-v145-verified
    write_setup_metadata "$DONE_CANDIDATE" || return 1
    durable_sync || return 1
    mv -f -- "$DONE_CANDIDATE" "$DONE" || return 1
    durable_sync || return 1
    rm -rf -- "$STAGE" "$BACKUP"
    log "adopted and versioned a validated legacy installation without rebaking it"
}

rc_v2_marker_describes_current() {
    local version data mode scale
    [ -f "$DONE" ] || return 1
    version=$(metadata_value "$DONE" setup_version 2>/dev/null) || return 1
    data=$(metadata_value "$DONE" data_version 2>/dev/null) || return 1
    mode=$(metadata_value "$DONE" texture_mode 2>/dev/null) || return 1
    scale=$(metadata_value "$DONE" texture_scale 2>/dev/null) || return 1
    [ "$version" = 2 ] && [ "$data" = 1.4.5 ] &&
        [ "$mode" = "$SOR4_TEXTURE_MODE" ] && [ "$scale" = "$SOR4_BAKE_SCALE" ]
}

upgrade_rc_v2_install() {
    local dll="$PKG/SOR4.dll" candidate="$PKG/SOR4.dll.v3-part"
    local rollback="$PKG/SOR4.dll.v2-rollback" fingerprint hash
    rc_v2_marker_describes_current || return 1
    validate_gameassets_dir "$GAMEDIR/gameassets" || return 1
    validate_runtime_dependencies_at "$GAMEDIR" || return 1
    validate_font_payloads_at "$GAMEDIR" || return 1
    validate_audio_dir "$GAMEDIR/audioout" || return 1
    [ -f "$PKG/libs/libWwise.real.so" ] && [ ! -L "$PKG/libs/libWwise.real.so" ] || return 1
    hash=$(sha256sum "$PKG/libs/libWwise.real.so" 2>/dev/null | awk '{print $1}')
    [ "$hash" = 4db3d430cde5525f3eeaa99af6e46c44feb554d89d76955b1391dfd5dd2cf0f2 ] || return 1
    [ -f "$PKG/SOR4Bridge.dll" ] && [ ! -L "$PKG/SOR4Bridge.dll" ] || return 1

    if validate_current_patched_dll "$dll"; then
        log "finishing an interrupted RC v2 managed-code upgrade"
    else
        validate_rc_v2_patched_dll "$dll" || return 1
        log "upgrading RC v2 save/config/Quit support without re-extracting game data"
        rm -f -- "$candidate" "$rollback"
        cp -f -- "$dll" "$rollback" || return 1
        cp -f -- "$dll" "$candidate" || { rm -f -- "$rollback"; return 1; }
        if ! run_dll "$SCRIPT_DIR/fixplatform.dll" "$candidate" ||
           ! validate_current_patched_dll "$candidate"; then
            rm -f -- "$candidate" "$rollback"
            return 1
        fi
        durable_sync || { rm -f -- "$candidate"; return 1; }
        mv -f -- "$candidate" "$dll" || return 1
        durable_sync || return 1
    fi

    if ! validate_live_install; then
        if [ -f "$rollback" ]; then
            mv -f -- "$rollback" "$dll"
            durable_sync || true
        fi
        return 1
    fi
    fingerprint=$(metadata_value "$DONE" asset_fingerprint 2>/dev/null) ||
        fingerprint=native-v145-verified
    APK_ASSET_FINGERPRINT=$fingerprint
    write_setup_metadata "$DONE_CANDIDATE" || return 1
    durable_sync || return 1
    mv -f -- "$DONE_CANDIDATE" "$DONE" || return 1
    durable_sync || return 1
    rm -f -- "$rollback" "$candidate"
    rm -rf -- "$STAGE" "$BACKUP"
    log "RC v2 installation upgraded to setup recipe v3"
}

expected_commit_paths() {
    local name
    printf '%s\n' gameassets host_pkg/libs/libWwise.real.so audioout || return 1
    printf '%s\n' host_pkg/SOR4.dll host_pkg/SOR4Bridge.dll || return 1
    while IFS= read -r name; do
        printf 'host_pkg/%s.dll\n' "$name" || return 1
    done < <(managed_dependency_names)
    while IFS= read -r name; do
        printf 'host_pkg/%s\n' "$name" || return 1
    done < <(font_payload_names)
}

commit_path_allowed() {
    local wanted=$1 allowed found=
    while IFS= read -r allowed; do
        [ "$wanted" != "$allowed" ] || found=1
    done < <(expected_commit_paths)
    [ "$found" = 1 ]
}

validate_commit_manifest() {
    local manifest=$1 actual expected
    [ -f "$manifest" ] && [ ! -L "$manifest" ] || return 1
    actual=$(sha256sum "$manifest" 2>/dev/null | awk '{print $1}') || return 1
    expected=$(expected_commit_paths | sha256sum | awk '{print $1}') || return 1
    [ "$actual" = "$expected" ]
}

validate_commit_payload() {
    local rel
    while IFS= read -r rel || [ -n "$rel" ]; do
        safe_relative_path "$rel" && commit_path_allowed "$rel" || return 1
        case "$rel" in
            gameassets|audioout)
                [ -d "$STAGE/$rel" ] && [ ! -L "$STAGE/$rel" ] || return 1 ;;
            *)
                [ -f "$STAGE/$rel" ] && [ ! -L "$STAGE/$rel" ] || return 1 ;;
        esac
    done < "$STAGE/commit.paths"
}

build_commit_manifest() {
    local temporary="$STAGE/commit.paths.tmp"
    rm -f -- "$temporary"
    expected_commit_paths > "$temporary" || { rm -f -- "$temporary"; return 1; }
    validate_commit_manifest "$temporary" || { rm -f -- "$temporary"; return 1; }
    mv -f -- "$temporary" "$STAGE/commit.paths" || return 1
    durable_sync || return 1
    validate_commit_manifest "$STAGE/commit.paths" && validate_commit_payload
}

commit_stage() {
    local rel
    rm -rf -- "$BACKUP"
    mkdir -p -- "$BACKUP" || return 1
    validate_commit_manifest "$STAGE/commit.paths" && validate_commit_payload || return 1
    cp -- "$STAGE/commit.paths" "$COMMIT_NEW" || return 1
    durable_sync || return 1
    mv -f -- "$COMMIT_NEW" "$COMMIT" || return 1
    validate_commit_manifest "$COMMIT" || return 1
    write_setup_metadata "$DONE_CANDIDATE" || return 1
    durable_sync || return 1

    while IFS= read -r rel || [ -n "$rel" ]; do
        safe_relative_path "$rel" && commit_path_allowed "$rel" || return 1
        if [ ! -e "$STAGE/$rel" ] && [ ! -L "$STAGE/$rel" ]; then
            [ "$rel" = audioout ] && continue
            return 1
        fi
        if [ -e "$GAMEDIR/$rel" ] || [ -L "$GAMEDIR/$rel" ]; then
            mkdir -p -- "$(dirname -- "$BACKUP/$rel")" || return 1
            mv -- "$GAMEDIR/$rel" "$BACKUP/$rel" || return 1
        fi
        mkdir -p -- "$(dirname -- "$GAMEDIR/$rel")" || return 1
        mv -- "$STAGE/$rel" "$GAMEDIR/$rel" || return 1
    done < "$COMMIT"

    durable_sync || return 1
    validate_committed_install || return 1
    : > "$COMMITTED" || return 1
    durable_sync || return 1
    rm -f -- "$COMMIT"
    durable_sync || return 1
    mv -f -- "$DONE_CANDIDATE" "$DONE" || return 1
    durable_sync || return 1
    rm -f -- "$COMMITTED"
    durable_sync || return 1
    rm -rf -- "$BACKUP" "$STAGE"
    durable_sync || return 1
}

main() {
    local installed_asset_count
    trap cleanup EXIT
    trap 'exit 130' INT TERM HUP
    acquire_lock || return 1
    : > "$LOG"
    recover_setup_state || { fail "automatic transaction recovery failed"; return 1; }

    if [ -f "$SCRIPT_DIR/sor4_profile.sh" ]; then
        # shellcheck source=/dev/null
        source "$SCRIPT_DIR/sor4_profile.sh"
        [ -n "${SOR4_PROFILE_EFFECTIVE:-}" ] || {
            sor4_probe_hardware "$GAMEDIR"
            sor4_select_profile "$GAMEDIR"
        }
    fi
    : "${SOR4_PROFILE_EFFECTIVE:=1gb}"
    : "${SOR4_TEXTURE_MODE:=etc1}"
    : "${SOR4_BAKE_SCALE:=3}"
    : "${SOR4_CONV_THREADS:=1}"
    export SOR4_CONV_THREADS
    export LD_LIBRARY_PATH="$PKG/libs:/usr/lib:/lib"
    export DOTNET_EnableWriteXorExecute=0
    export DOTNET_gcServer=0
    export SDL_NO_SIGNAL_HANDLERS=1
    require_setup_tools || return 1

    if upgrade_rc_v2_install; then
        return 0
    elif metadata_matches_current "$DONE"; then
        installed_asset_count=$(metadata_asset_count "$DONE") || {
            fail "installed setup marker is malformed"
            return 1
        }
        if validate_live_install "$installed_asset_count"; then
            rm -rf -- "$STAGE" "$BACKUP"
            log "setup already complete for mode=$SOR4_TEXTURE_MODE scale=1/$SOR4_BAKE_SCALE"
            return 0
        fi
        log "installed payload no longer validates; rebuilding transactionally"
    elif adopt_legacy_install; then
        return 0
    fi

    start_ui
    find_apk_sources || { fail "$APK_ERROR"; return 1; }
    [ -x "$PKG/sor4host" ] || { fail "missing packaged .NET host"; return 1; }

    prepare_stage_recipe || { fail "could not initialize a recipe-safe staging area"; return 1; }
    prepare_apk_set || { fail "APK/split/bundle preparation failed"; return 1; }
    log "validating the exact supported Android data"
    validate_supported_apk || return 1
    write_setup_metadata "$STAGE/.setup_recipe" || return 1
    durable_sync || return 1
    preflight_disk_space || return 1

    if [ -f "$STAGE/.assets_done" ] && ! validate_assets_stage; then
        discard_invalid_assets_stage || return 1
    fi
    if [ ! -f "$STAGE/.assets_done" ]; then
        extract_assets || { fail "asset extraction/bake failed; resume data was preserved"; return 1; }
        revalidate_apk_identity || {
            discard_invalid_assets_stage || true
            return 1
        }
        validate_assets_stage || {
            fail "asset extraction checkpoint did not validate"
            discard_invalid_assets_stage || true
            return 1
        }
        mark_stage_done "$STAGE/.assets_done" || return 1
    fi
    if [ -f "$STAGE/.code_done" ] && ! validate_patched_code_at "$STAGE"; then
        log "stale managed-code checkpoint rejected"
        rm -f -- "$STAGE/.code_done"
    fi
    if [ ! -f "$STAGE/.code_done" ]; then
        patch_code || { fail "managed-code patch failed; staged data was preserved"; return 1; }
        validate_patched_code_at "$STAGE" || { fail "patched managed code did not validate"; return 1; }
        mark_stage_done "$STAGE/.code_done" || return 1
    fi
    if [ -f "$STAGE/.audio_done" ] && ! validate_audio_dir "$STAGE/audioout"; then
        log "stale audio checkpoint rejected"
        rm -f -- "$STAGE/.audio_done"
    fi
    if [ ! -f "$STAGE/.audio_done" ]; then
        prepare_audio || { fail "audio preparation failed; staged data was preserved"; return 1; }
        mark_stage_done "$STAGE/.audio_done" || return 1
    fi
    validate_stage || { fail "staged data validation failed"; return 1; }
    build_commit_manifest || { fail "could not build a safe commit manifest"; return 1; }
    commit_stage || {
        fail "installation commit failed; recovering its last durable state"
        recover_setup_state || fail "recovery incomplete; staged data and backup were preserved"
        return 1
    }
    printf '1 100 100\n' > "$UI_CTL" 2>/dev/null || true
    sleep 1
    log "complete: profile=$SOR4_PROFILE_EFFECTIVE mode=$SOR4_TEXTURE_MODE scale=1/$SOR4_BAKE_SCALE"
}

if [ "${SOR4_SETUP_LIBRARY:-0}" != 1 ]; then
    main "$@"
fi
