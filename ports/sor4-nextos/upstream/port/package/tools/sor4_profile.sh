#!/bin/bash
# Hardware probe and bounded-memory profiles shared by launcher and first-run setup.
# This file is sourced; it never sources the user-writable configuration file.

sor4_cfg_trim() {
    local value=$1
    value=${value#"${value%%[![:space:]]*}"}
    value=${value%"${value##*[![:space:]]}"}
    printf '%s' "$value"
}

sor4_read_config() {
    local gamedir=$1 cfg line key value

    SOR4_CFG_PROFILE=auto
    SOR4_CFG_STREAMING=auto
    SOR4_CFG_BUDGET=auto
    SOR4_CFG_QUALITY=auto
    SOR4_CFG_STREAMING_LOG=auto
    cfg="$gamedir/sor4.cfg"
    [ -f "$cfg" ] || cfg="$gamedir/sor4.cfg.default"
    [ -f "$cfg" ] || return 0

    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%%#*}
        line=$(sor4_cfg_trim "$line")
        [ -n "$line" ] || continue
        case "$line" in
            *=*) key=$(sor4_cfg_trim "${line%%=*}")
                 value=$(sor4_cfg_trim "${line#*=}") ;;
            *)   continue ;;
        esac
        case "$key:$value" in
            profile:auto|profile:1gb|profile:2gb|profile:high)
                SOR4_CFG_PROFILE=$value ;;
            texture_streaming:auto|texture_streaming:on|texture_streaming:off)
                SOR4_CFG_STREAMING=$value ;;
            texture_budget_mb:auto)
                SOR4_CFG_BUDGET=auto ;;
            texture_budget_mb:*)
                case "$value" in
                    ''|*[!0-9]*) ;;
                    *) [ "$value" -ge 32 ] 2>/dev/null &&
                       [ "$value" -le 1024 ] 2>/dev/null &&
                       SOR4_CFG_BUDGET=$value ;;
                esac ;;
            texture_quality:auto|texture_quality:full|texture_quality:half|texture_quality:third)
                SOR4_CFG_QUALITY=$value ;;
            streaming_log:auto|streaming_log:on|streaming_log:off)
                SOR4_CFG_STREAMING_LOG=$value ;;
        esac
    done < "$cfg"
}

# One pure SDL policy shared by the probe, setup splash and game runtime.
sor4_sdl3_available() {
    [ -e /usr/lib/libSDL3.so.0 ] || [ -e /lib/libSDL3.so.0 ] ||
        { command -v ldconfig >/dev/null 2>&1 &&
          ldconfig -p 2>/dev/null | grep -q 'libSDL3\.so\.0'; }
}

sor4_sdl_compat_dir() {
    local gamedir=$1 compat="$1/host_pkg/sdl3compat"
    sor4_sdl3_available || return 1
    [ -f "$compat/libSDL2-2.0.so.0" ] || return 2
    printf '%s\n' "$compat"
}

# Runs the capability probe once with the environment exactly as given.  Sets
# SOR4_PROBE_* from its shell-safe contract; a probe that cannot start leaves them 0.
sor4_run_probe() {
    local gamedir=$1 output line compat search

    SOR4_GLES=2
    SOR4_PROBE_ASTC=0
    SOR4_PROBE_ETC2=0
    SOR4_PROBE_OK=0
    [ -x "$gamedir/tools/sor4probe" ] || return 1

    compat=$(sor4_sdl_compat_dir "$gamedir" 2>/dev/null || true)
    [ -z "$compat" ] || compat="$compat:"
    search="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib"
    [ -n "${SOR4_CONTROLFOLDER:-}" ] &&
        search="$search:$SOR4_CONTROLFOLDER/libs:$SOR4_CONTROLFOLDER/libs.aarch64"
    # Mesa/Panfrost firmwares hand back a desktop-GL context unless SDL is told to
    # load the ES driver; the probe rejects non-ES contexts, so without this hint a
    # perfectly capable device would be demoted to the ES2/ETC1 recipe.
    output=$(LD_LIBRARY_PATH="${compat}${search}" SDL_OPENGL_ES_DRIVER=1 \
        "$gamedir/tools/sor4probe" 2>>"$gamedir/log.txt") || output=
    while IFS= read -r line; do
        case "$line" in
            SOR4_PROBE_OK=1) SOR4_PROBE_OK=1 ;;
            SOR4_GLES=2|SOR4_GLES=3) SOR4_GLES=${line#*=} ;;
            SOR4_ASTC=0|SOR4_ASTC=1) SOR4_PROBE_ASTC=${line#*=} ;;
            SOR4_ETC2=0|SOR4_ETC2=1) SOR4_PROBE_ETC2=${line#*=} ;;
        esac
    done <<< "$output"
    [ "$SOR4_PROBE_OK" = 1 ]
}

sor4_probe_hardware() {
    local gamedir=$1

    # SOR4_CLEAR_VIDEODRIVER is a contract with the starter, never a driver choice:
    # the port picks no backend and only reports that an INHERITED SDL_VIDEODRIVER
    # is unusable here, so SDL is allowed to auto-detect instead of failing blind.
    SOR4_CLEAR_VIDEODRIVER=0
    if ! sor4_run_probe "$gamedir" && [ -n "${SDL_VIDEODRIVER:-}" ]; then
        printf '[probe] SDL_VIDEODRIVER=%s nao inicializa aqui; testando auto-deteccao\n' \
            "$SDL_VIDEODRIVER"
        if ( unset SDL_VIDEODRIVER; sor4_run_probe "$gamedir" ); then
            SOR4_CLEAR_VIDEODRIVER=1
            unset SDL_VIDEODRIVER
            sor4_run_probe "$gamedir" || true
        fi
    fi

    # A failed probe must choose the broadest safe path, never claim support.
    if [ "$SOR4_PROBE_OK" != 1 ]; then
        SOR4_GLES=2
        SOR4_PROBE_ASTC=0
        SOR4_PROBE_ETC2=0
    fi
    export SOR4_GLES SOR4_CLEAR_VIDEODRIVER
}

sor4_select_profile() {
    local gamedir=$1 mem_kb cores requested

    sor4_read_config "$gamedir"
    mem_kb=$(awk '/^MemTotal:/{print $2; exit}' /proc/meminfo 2>/dev/null)
    case "$mem_kb" in ''|*[!0-9]*) mem_kb=700000 ;; esac
    SOR4_MEMTOTAL_KB=$mem_kb

    requested=$SOR4_CFG_PROFILE
    if [ "$requested" = auto ]; then
        # Leave real headroom for firmware/CMA. Many nominal 1 GiB handhelds
        # report 930-990 MiB here and must not fall into the 2 GiB recipe.
        if [ "$mem_kb" -lt 1250000 ]; then
            requested=1gb
        elif [ "$mem_kb" -lt 2500000 ]; then
            requested=2gb
        else
            requested=high
        fi
    fi
    SOR4_PROFILE_EFFECTIVE=$requested

    case "$requested" in
        1gb)
            SOR4_PAGE_CAP_MB=80
            SOR4_PAGE_FLOOR_MB=64
            SOR4_PAGE_MIN_KB=16
            SOR4_PAGE_UPLOADS=2
            SOR4_CONV_THREADS=1
            DOTNET_GCConserveMemory=9
            DOTNET_TieredPGO=0 ;;
        2gb)
            SOR4_PAGE_CAP_MB=144
            SOR4_PAGE_FLOOR_MB=96
            SOR4_PAGE_MIN_KB=32
            SOR4_PAGE_UPLOADS=3
            SOR4_CONV_THREADS=2
            DOTNET_GCConserveMemory=5
            DOTNET_TieredPGO=0 ;;
        high)
            SOR4_PAGE_CAP_MB=320
            SOR4_PAGE_FLOOR_MB=160
            SOR4_PAGE_MIN_KB=48
            SOR4_PAGE_UPLOADS=4
            cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
            case "$cores" in ''|*[!0-9]*) cores=2 ;; esac
            SOR4_CONV_THREADS=$((cores > 4 ? 4 : cores))
            DOTNET_GCConserveMemory=0
            DOTNET_TieredPGO=1 ;;
    esac
    [ "$SOR4_CFG_BUDGET" = auto ] || SOR4_PAGE_CAP_MB=$SOR4_CFG_BUDGET

    case "$SOR4_CFG_STREAMING" in
        off) SOR4_PAGE=0 ;;
        *)   SOR4_PAGE=1 ;;
    esac
    # A texture requested by the current draw must be resident before it is bound.
    # Async faults exposed the 1x1 eviction placeholder for a frame (visible as a
    # flashing rectangle on hit effects). XNB reads still stream only level 0.
    SOR4_PAGE_ASYNC=0
    SOR4_PAGE_SWAP="$gamedir/texswap"
    case "$SOR4_CFG_STREAMING_LOG" in on) SOR4_PAGELOG=1 ;; *) SOR4_PAGELOG=0 ;; esac

    if [ "$SOR4_PROBE_ASTC" = 1 ]; then
        SOR4_TEXTURE_MODE=astc
        SOR4_NATIVE_ASTC=1
        SOR4_ETC1=0
        SOR4_BAKE_SCALE=1
    elif [ "$SOR4_PROBE_ETC2" = 1 ] && [ "$SOR4_GLES" -ge 3 ]; then
        SOR4_TEXTURE_MODE=etc2
        SOR4_NATIVE_ASTC=0
        SOR4_ETC1=0
        case "$requested" in 1gb) SOR4_BAKE_SCALE=2 ;; *) SOR4_BAKE_SCALE=1 ;; esac
    else
        SOR4_TEXTURE_MODE=etc1
        SOR4_NATIVE_ASTC=0
        SOR4_ETC1=1
        case "$requested" in 1gb) SOR4_BAKE_SCALE=3 ;; 2gb) SOR4_BAKE_SCALE=2 ;; *) SOR4_BAKE_SCALE=1 ;; esac
    fi
    case "$SOR4_CFG_QUALITY" in
        full) SOR4_BAKE_SCALE=1 ;;
        half)
            SOR4_BAKE_SCALE=2
            if [ "$SOR4_TEXTURE_MODE" = astc ]; then
                SOR4_NATIVE_ASTC=0
                if [ "$SOR4_PROBE_ETC2" = 1 ]; then
                    SOR4_TEXTURE_MODE=etc2
                else
                    SOR4_TEXTURE_MODE=etc1
                    SOR4_ETC1=1
                fi
            fi ;;
        third)
            SOR4_BAKE_SCALE=3
            if [ "$SOR4_TEXTURE_MODE" = astc ]; then
                SOR4_NATIVE_ASTC=0
                if [ "$SOR4_PROBE_ETC2" = 1 ]; then
                    SOR4_TEXTURE_MODE=etc2
                else
                    SOR4_TEXTURE_MODE=etc1
                    SOR4_ETC1=1
                fi
            fi ;;
    esac
    SOR4_TEXSCALE=$SOR4_BAKE_SCALE

    export SOR4_MEMTOTAL_KB SOR4_PROFILE_EFFECTIVE SOR4_TEXTURE_MODE
    export SOR4_PAGE SOR4_PAGE_ASYNC SOR4_PAGE_SWAP SOR4_PAGE_CAP_MB
    export SOR4_PAGE_FLOOR_MB SOR4_PAGE_MIN_KB SOR4_PAGE_UPLOADS SOR4_PAGELOG
    MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
    export SOR4_NATIVE_ASTC SOR4_ETC1 SOR4_BAKE_SCALE SOR4_TEXSCALE
    export SOR4_CONV_THREADS MALLOC_ARENA_MAX
    export DOTNET_GCConserveMemory DOTNET_TieredPGO

    printf '[config] profile=%s ram=%skB gles=%s texture=%s scale=1/%s pager=%s/%sMB\n' \
        "$SOR4_PROFILE_EFFECTIVE" "$SOR4_MEMTOTAL_KB" "$SOR4_GLES" \
        "$SOR4_TEXTURE_MODE" "$SOR4_BAKE_SCALE" "$SOR4_PAGE" "$SOR4_PAGE_CAP_MB"
}

# Uma execucao como root deixa o semaforo SysV do dmix (ipc_key do ~/.asoundrc,
# tipicamente 1024, ou 5678293, o padrao do ALSA) com dono root e modo 600; a
# partir dai todo processo de usuario abre MUDO com "unable to create IPC
# semaphore".  O semaforo sobrevive ao processo que o criou, entao um semaforo
# de dmix que este usuario nao consegue abrir so atrapalha -- remover devolve o
# audio e o proximo cliente recria na hora.  Roda a cada launch, antes do jogo.
sor4_heal_dmix_ipc() {
    command -v ipcs >/dev/null 2>&1 || return 0
    command -v ipcrm >/dev/null 2>&1 || return 0
    local me sudo_cmd keys k t nome key sid owner
    me=$(id -un 2>/dev/null) || return 0
    sudo_cmd=${ESUDO:-}
    [ -z "$sudo_cmd" ] && command -v sudo >/dev/null 2>&1 && sudo_cmd="sudo -n"
    # Cada configuracao de dmix usa ipc_key para o semaforo/controle e ipc_key+1
    # para o segmento do buffer de som — os dois entram na varredura.
    keys=$({ awk '$1 == "ipc_key" { gsub(/[^0-9]/, "", $2); if ($2 != "") { print $2; print $2 + 1 } }' \
        "$HOME/.asoundrc" /etc/asound.conf 2>/dev/null; echo 5678293; echo 5678294; } | sort -u)
    for k in $keys; do
        printf -v k '0x%08x' "$k" 2>/dev/null || continue
        # O dmix usa DOIS recursos SysV com a mesma key: um semaforo (-s) e um
        # segmento de memoria compartilhada (-m). Limpar so um deixa mudo do
        # mesmo jeito ("unable to create IPC shm instance").
        for t in s m; do
            [ "$t" = s ] && nome=semaforo || nome=shm
            while read -r key sid owner; do
                [ "$key" = "$k" ] && [ -n "$owner" ] && [ "$owner" != "$me" ] || continue
                if ${sudo_cmd:-} ipcrm -$t "$sid" 2>/dev/null || ipcrm -$t "$sid" 2>/dev/null; then
                    echo "SOR4 audio: $nome dmix orfao $key (dono $owner) removido" >&2
                else
                    echo "SOR4 audio: $nome dmix $key (dono $owner) inacessivel e nao removivel; audio pode sair mudo" >&2
                fi
            done < <(ipcs -$t 2>/dev/null | awk '$1 ~ /^0x/ { print $1, $2, $3 }')
        done
    done
    return 0
}

# The compiled starter consumes the same profile chosen by first-run setup.  Only
# a fixed allowlist is emitted; the starter never evaluates shell output.
sor4_emit_profile() {
    local gamedir=${1:-}
    [ -n "$gamedir" ] && [ -d "$gamedir" ] || return 2
    sor4_heal_dmix_ipc
    sor4_probe_hardware "$gamedir"
    sor4_select_profile "$gamedir"
    local key
    for key in \
        SOR4_GLES SOR4_CLEAR_VIDEODRIVER SOR4_MEMTOTAL_KB SOR4_PROFILE_EFFECTIVE SOR4_TEXTURE_MODE \
        SOR4_PAGE SOR4_PAGE_ASYNC SOR4_PAGE_SWAP SOR4_PAGE_CAP_MB \
        SOR4_PAGE_FLOOR_MB SOR4_PAGE_MIN_KB SOR4_PAGE_UPLOADS SOR4_PAGELOG \
        SOR4_NATIVE_ASTC SOR4_ETC1 SOR4_BAKE_SCALE SOR4_TEXSCALE \
        SOR4_CONV_THREADS MALLOC_ARENA_MAX DOTNET_GCConserveMemory \
        DOTNET_TieredPGO; do
        printf 'SOR4_EXPORT %s=%s\n' "$key" "${!key}"
    done
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    case "${1:-}" in
        --emit) sor4_emit_profile "${2:-}" ;;
        *) printf 'usage: sor4_profile.sh --emit GAMEDIR\n' >&2; exit 2 ;;
    esac
fi
