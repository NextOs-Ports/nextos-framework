#!/bin/bash
# The Amazing Spider-Man 2 (1.2.7d/1.2.8d inputs) — ARMHF clean-room loader
# Launcher MULTI-DEVICE no padrao PortMaster (mesma base de SMHD / Bully /
# Sonic 4 EP2 / Horizon Chase).
#
# Alvos: ArkOS, ROCKNIX, muOS, Knulli, Batocera, AmberELEC e afins (qualquer
# CFW com PortMaster) + NextOS. Nada aqui e' especifico de um aparelho:
# resolucao, backend de video/audio e caminho de libs sao NEGOCIADOS em
# runtime (regra de ouro: negocie tudo, hardcode nada).
#
# Roda em FOREGROUND: o frontend volta sozinho quando o processo termina.
# Saida do jogo = SELECT+START.
#
# ⚠️ SEM `set -u`: o control.txt do PortMaster referencia variaveis nao
# definidas (ex. OS_NAME) e aborta o launcher inteiro sob nounset.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/storage/.config/PortMaster/" ]; then
  controlfolder="/storage/.config/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

# PortMaster e' OPCIONAL: o port roda standalone se control.txt nao existir.
if [ -f "$controlfolder/control.txt" ]; then
  # shellcheck source=/dev/null
  source "$controlfolder/control.txt"
  case "${CFW_NAME:-}" in
    ""|*[!A-Za-z0-9_.-]*) ;;
    *)
      # shellcheck disable=SC1090
      if [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ]; then
        source "${controlfolder}/mod_${CFW_NAME}.txt"
      fi
      ;;
  esac
  command -v get_controls >/dev/null 2>&1 && get_controls
fi

# raiz dos dados: $directory vem do PortMaster; senao tenta os layouts conhecidos
if [ -n "${directory:-}" ] && [ -d "/${directory#/}/ports/asm2_127" ]; then
  GAMEDIR="/${directory#/}/ports/asm2_127"
elif [ -d "/roms/ports/asm2_127" ]; then
  GAMEDIR="/roms/ports/asm2_127"
elif [ -d "/roms2/ports/asm2_127" ]; then
  GAMEDIR="/roms2/ports/asm2_127"
elif [ -d "/storage/roms/ports/asm2_127" ]; then
  GAMEDIR="/storage/roms/ports/asm2_127"
else
  GAMEDIR="$(dirname "$0")/asm2_127"
fi

cd "$GAMEDIR" || exit 1
GAMEDIR=$(pwd -P) || exit 1

asm2_finish() {
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}

asm2_abort() {
  echo "ERRO ASM2: $*" >&2
  asm2_finish
  exit 1
}

# A trava acompanha todo o launcher por um descritor. O fallback usa mkdir
# atomico e so recupera uma trava cujo PID+starttime comprovadamente morreu.
# Assim, dois launchers simultaneos nunca chegam juntos a /proc, ao extrator ou
# ao framebuffer.
ASM2_LOCK_FILE="$GAMEDIR/.asm2-launch.lock"
ASM2_LOCK_DIR="$GAMEDIR/.asm2-launch.lock.d"
ASM2_LOCK_KIND=

asm2_proc_start() {
  local stat_line stat_fields
  local -a field

  stat_line=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
  stat_fields=${stat_line#*) }
  read -r -a field <<< "$stat_fields"
  [ "${#field[@]}" -ge 20 ] || return 1
  case "${field[19]}" in
    ""|*[!0-9]*) return 1 ;;
  esac
  echo "${field[19]}"
}

ASM2_LOCK_START=$(asm2_proc_start "$$")
case "$ASM2_LOCK_START" in
  ""|*[!0-9]*) asm2_abort "nao foi possivel identificar o processo do launcher" ;;
esac

# Invocada indiretamente pelo trap EXIT do fallback.
# shellcheck disable=SC2329
asm2_lock_cleanup() {
  local owner_pid owner_start

  if [ "$ASM2_LOCK_KIND" = "mkdir" ] &&
     [ -r "$ASM2_LOCK_DIR/owner" ]; then
    read -r owner_pid owner_start < "$ASM2_LOCK_DIR/owner" || return
    if [ "$owner_pid" = "$$" ] && [ "$owner_start" = "$ASM2_LOCK_START" ]; then
      rm -f "$ASM2_LOCK_DIR/owner"
      rmdir "$ASM2_LOCK_DIR" 2>/dev/null || true
    fi
  fi
}

if command -v flock >/dev/null 2>&1; then
  exec 9>"$ASM2_LOCK_FILE" ||
    asm2_abort "nao foi possivel abrir a trava $ASM2_LOCK_FILE"
  flock -n 9 ||
    asm2_abort "outro launcher do The Amazing Spider-Man 2 ja esta ativo"
  ASM2_LOCK_KIND=flock
else
  while ! mkdir "$ASM2_LOCK_DIR" 2>/dev/null; do
    if [ ! -r "$ASM2_LOCK_DIR/owner" ]; then
      asm2_abort "trava concorrente sem dono legivel em $ASM2_LOCK_DIR"
    fi

    read -r lock_pid lock_start < "$ASM2_LOCK_DIR/owner" ||
      asm2_abort "trava concorrente invalida em $ASM2_LOCK_DIR"
    case "$lock_pid" in
      ""|*[!0-9]*) asm2_abort "PID invalido na trava concorrente" ;;
    esac
    case "$lock_start" in
      ""|*[!0-9]*) asm2_abort "starttime invalido na trava concorrente" ;;
    esac

    live_start=$(asm2_proc_start "$lock_pid")
    if [ "$live_start" = "$lock_start" ]; then
      asm2_abort "outro launcher do The Amazing Spider-Man 2 ja esta ativo"
    fi

    stale_dir="${ASM2_LOCK_DIR}.stale.$$.$ASM2_LOCK_START"
    if mv "$ASM2_LOCK_DIR" "$stale_dir" 2>/dev/null; then
      rm -f "$stale_dir/owner"
      rmdir "$stale_dir" 2>/dev/null || true
    fi
  done

  printf '%s %s\n' "$$" "$ASM2_LOCK_START" > "$ASM2_LOCK_DIR/owner" ||
    {
      rm -f "$ASM2_LOCK_DIR/owner"
      rmdir "$ASM2_LOCK_DIR" 2>/dev/null || true
      asm2_abort "nao foi possivel registrar o dono da trava"
    }
  ASM2_LOCK_KIND="mkdir"
  trap asm2_lock_cleanup EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
fi

: > "$GAMEDIR/debug.log"
exec > >(tee "$GAMEDIR/debug.log") 2>&1
echo "=== The Amazing Spider-Man 2 | $(date -Is) | dir=$GAMEDIR ==="

# 1 INSTANCIA SO: enumera por executable (inclusive "(deleted)"), comm,
# command line e cwd. TERM vem primeiro; KILL so depois do prazo e uma nova
# comprovacao evita atingir um PID que tenha sido reciclado.
asm2_pids() {
  local p pid exe comm args cwd

  for p in /proc/[0-9]*; do
    pid=${p##*/}
    [ "$pid" = "$$" ] && continue

    exe=$(readlink "$p/exe" 2>/dev/null)
    case "$exe" in
      "$GAMEDIR"/asm2_127*)
        echo "$pid"
        continue
        ;;
    esac

    comm=$(cat "$p/comm" 2>/dev/null)
    args=$(tr '\0' ' ' 2>/dev/null < "$p/cmdline")
    cwd=$(readlink "$p/cwd" 2>/dev/null)

    case "$cwd:$comm" in
      "$GAMEDIR:asm2_127"|"$GAMEDIR:asm2_127-unive"*)
        echo "$pid"
        continue
        ;;
    esac

    case " $args " in
      *" $GAMEDIR/asm2_127 "*|*" $GAMEDIR/asm2_127-universal "*|\
      *" ./asm2_127 "*|*" ./asm2_127-universal "*)
        [ "$cwd" = "$GAMEDIR" ] && echo "$pid"
        ;;
    esac
  done
}

asm2_stop_existing() {
  local old_pids pid wait_round alive remaining

  old_pids=$(asm2_pids)
  if [ -n "$old_pids" ]; then
    for pid in $old_pids; do
      echo "[launcher] encerrando instancia anterior pid=$pid"
      kill "$pid" 2>/dev/null || true
    done

    wait_round=0
    alive=$(asm2_pids)
    while [ -n "$alive" ] && [ "$wait_round" -lt 20 ]; do
      sleep 0.5
      wait_round=$((wait_round + 1))
      alive=$(asm2_pids)
    done

    if [ -n "$alive" ]; then
      for pid in $alive; do
        echo "[launcher] forçando encerramento da instancia anterior pid=$pid"
        kill -9 "$pid" 2>/dev/null || true
      done
      sleep 1
    fi

    remaining=$(asm2_pids)
    [ -z "$remaining" ] ||
      asm2_abort "instancia anterior permaneceu ativa (PID(s): $remaining)"
  fi
}

asm2_stop_existing

# ---- binario por alvo -------------------------------------------------------
# asm2_127-universal: ARMHF glibc<=2.27 (ArkOS/CFWs). asm2_127: build NextOS.
asm2_is_nextos() {
  local system_root=${1:-/} system_label

  system_root=${system_root%/}
  [ -n "$system_root" ] || system_root=/
  system_label="${CFW_NAME:-} ${OS_NAME:-} ${ASM2_CFW_NAME:-} ${ASM2_OS_NAME:-}"
  case "$system_label" in
    *[Nn][Ee][Xx][Tt][Oo][Ss]*|*[Rr][Ee][Tt][Rr][Oo]*[Ee][Ll][Ii][Tt][Ee]*)
      return 0
      ;;
  esac

  [ -x "$system_root/usr/bin/nextos-pulse-health" ] && return 0
  [ -r "$system_root/etc/os-release" ] &&
    grep -Eiq 'nextos|retro[[:space:]_-]*elite' \
      "$system_root/etc/os-release"
}

asm2_select_binary() {
  local system_root=${1:-/}
  local nextos_bin="$GAMEDIR/asm2_127"
  local universal_bin="$GAMEDIR/asm2_127-universal"

  if asm2_is_nextos "$system_root"; then
    if [ -f "$nextos_bin" ]; then
      BIN=$nextos_bin
      echo "[launcher] build ARMHF atual do NextOS selecionado"
    elif [ -f "$universal_bin" ]; then
      BIN=$universal_bin
      echo "[launcher] build NextOS ausente; usando fallback ARMHF universal"
    else
      BIN=$nextos_bin
    fi
  else
    if [ -f "$universal_bin" ]; then
      BIN=$universal_bin
      echo "[launcher] build ARMHF universal de baixa glibc selecionado"
    elif [ -f "$nextos_bin" ]; then
      BIN=$nextos_bin
      echo "[launcher] build universal ausente; usando unico loader ARMHF disponivel"
    else
      BIN=$universal_bin
    fi
  fi
}

asm2_select_binary /

# Le o ELF sem depender de `file`/`readelf`, normalmente ausentes nos CFWs
# minimos. Alem de ELF32 little-endian/EM_ARM, exige EF_ARM_ABI_FLOAT_HARD e
# extrai o PT_INTERP declarado pelo proprio executavel.
asm2_u16le_at() {
  local raw
  local -a byte

  raw=$(LC_ALL=C od -An -v -t u1 -j "$2" -N 2 "$1" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a byte <<< "$raw"
  [ "${#byte[@]}" -eq 2 ] || return 1
  echo $((byte[0] + (byte[1] << 8)))
}

asm2_u32le_at() {
  local raw
  local -a byte

  raw=$(LC_ALL=C od -An -v -t u1 -j "$2" -N 4 "$1" 2>/dev/null) ||
    return 1
  raw=${raw//$'\n'/ }
  read -r -a byte <<< "$raw"
  [ "${#byte[@]}" -eq 4 ] || return 1
  echo $((byte[0] + (byte[1] << 8) + (byte[2] << 16) + (byte[3] << 24)))
}

asm2_is_armhf_elf() {
  local raw flags
  local -a header

  raw=$(LC_ALL=C od -An -v -t u1 -N 20 "$1" 2>/dev/null) || return 1
  raw=${raw//$'\n'/ }
  read -r -a header <<< "$raw"
  [ "${#header[@]}" -eq 20 ] || return 1
  [ "${header[0]}" -eq 127 ] &&
    [ "${header[1]}" -eq 69 ] &&
    [ "${header[2]}" -eq 76 ] &&
    [ "${header[3]}" -eq 70 ] &&
    [ "${header[4]}" -eq 1 ] &&
    [ "${header[5]}" -eq 1 ] &&
    { [ "${header[16]}" -eq 2 ] || [ "${header[16]}" -eq 3 ]; } &&
    [ "${header[17]}" -eq 0 ] &&
    [ "${header[18]}" -eq 40 ] &&
    [ "${header[19]}" -eq 0 ] || return 1

  flags=$(asm2_u32le_at "$1" 36) || return 1
  [ $((flags & 0x400)) -ne 0 ]
}

asm2_elf_interp() {
  local file=$1 file_size phoff phentsize phnum index entry
  local p_type p_offset p_filesz interp

  file_size=$(wc -c < "$file" 2>/dev/null) || return 1
  phoff=$(asm2_u32le_at "$file" 28) || return 1
  phentsize=$(asm2_u16le_at "$file" 42) || return 1
  phnum=$(asm2_u16le_at "$file" 44) || return 1
  [ "$phentsize" -ge 32 ] && [ "$phnum" -gt 0 ] && [ "$phnum" -le 128 ] ||
    return 1

  index=0
  while [ "$index" -lt "$phnum" ]; do
    entry=$((phoff + index * phentsize))
    p_type=$(asm2_u32le_at "$file" "$entry") || return 1
    if [ "$p_type" -eq 3 ]; then
      p_offset=$(asm2_u32le_at "$file" $((entry + 4))) || return 1
      p_filesz=$(asm2_u32le_at "$file" $((entry + 16))) || return 1
      [ "$p_filesz" -gt 1 ] && [ "$p_filesz" -le 4096 ] &&
        [ $((p_offset + p_filesz)) -le "$file_size" ] || return 1
      interp=$(dd if="$file" bs=1 skip="$p_offset" count="$p_filesz" \
        2>/dev/null | LC_ALL=C tr -d '\000') || return 1
      case "$interp" in
        /*)
          echo "$interp"
          return 0
          ;;
      esac
      return 1
    fi
    index=$((index + 1))
  done
  return 1
}

[ -f "$BIN" ] || asm2_abort "executavel ausente: $BIN"
chmod +x "$BIN" 2>/dev/null
[ -x "$BIN" ] || asm2_abort "executavel sem permissao de execucao: $BIN"
for required_tool in od dd tr wc; do
  command -v "$required_tool" >/dev/null 2>&1 ||
    asm2_abort "ferramenta obrigatoria ausente para validar ARMHF: $required_tool"
done
asm2_is_armhf_elf "$BIN" ||
  asm2_abort "o loader nao e um executavel Linux ARMHF valido: $BIN"
ASM2_INTERP=$(asm2_elf_interp "$BIN") ||
  asm2_abort "nao foi possivel validar o interpreter dinamico do loader ARMHF"

# ---- libs -------------------------------------------------------------------
# A libmali/SDL2 ARMHF que casa com o kernel costuma morar FORA do path padrao
# (/usr/local/lib/<triplet>). Sem isso o binario nao acha libEGL/libGLESv2.
LD_EXTRA=""
for d in /usr/local/lib/arm-linux-gnueabihf /usr/local/lib32 /usr/local/lib \
         /usr/lib32 /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf \
         "$controlfolder/libs" "$controlfolder/libs.armhf"; do
  [ -d "$d" ] && LD_EXTRA="$LD_EXTRA:$d"
done
if [ -n "$LD_EXTRA" ]; then
  export LD_LIBRARY_PATH="${LD_EXTRA#:}:$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

[ -x "$ASM2_INTERP" ] ||
  asm2_abort "runtime ARMHF ausente: $ASM2_INTERP (habilite o multilib ARMHF deste firmware)"
if ! "$ASM2_INTERP" --verify "$BIN" >/dev/null 2>&1; then
  asm2_abort "o kernel/runtime deste firmware nao consegue executar ARMHF ($ASM2_INTERP)"
fi
ASM2_RUNTIME_REPORT=$(LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$ASM2_INTERP" --list "$BIN" 2>&1)
ASM2_RUNTIME_STATUS=$?
if [ "$ASM2_RUNTIME_STATUS" -ne 0 ]; then
  printf '%s\n' "$ASM2_RUNTIME_REPORT" >&2
  asm2_abort "runtime ARMHF incompleto; dependencias do loader nao puderam ser resolvidas"
fi
case "$ASM2_RUNTIME_REPORT" in
  *"not found"*)
    printf '%s\n' "$ASM2_RUNTIME_REPORT" | awk '/not found/' >&2
    asm2_abort "runtime ARMHF incompleto; uma ou mais bibliotecas estao ausentes"
    ;;
esac

# ---- video ------------------------------------------------------------------
# NUNCA forcar SDL_VIDEODRIVER: cada CFW autodetecta (fbdev/KMSDRM/Wayland).
# Resolucao: o binario le o modo real via SDL_GetDesktopDisplayMode — nada a
# cravar aqui.
export SDL_VIDEO_FULLSCREEN_DESKTOP=1

# Nunca anunciar Wayland por nome presumido: mantem ou descobre
# WAYLAND_DISPLAY somente quando o caminho correspondente e' um socket real.
wayland_socket=
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
  case "$WAYLAND_DISPLAY" in
    /*) wayland_candidate=$WAYLAND_DISPLAY ;;
    *) wayland_candidate="${XDG_RUNTIME_DIR:-}/$WAYLAND_DISPLAY" ;;
  esac
  [ -S "$wayland_candidate" ] && wayland_socket=$wayland_candidate
  if [ -z "$wayland_socket" ]; then
    echo "[launcher] ignorando WAYLAND_DISPLAY sem socket real: $WAYLAND_DISPLAY"
    unset WAYLAND_DISPLAY
  fi
fi

if [ -z "$wayland_socket" ]; then
  for runtime_dir in "${XDG_RUNTIME_DIR:-}" "/run/user/$(id -u)" \
                     "/var/run/user/$(id -u)" /run/user/0 /var/run/user/0; do
    [ -n "$runtime_dir" ] && [ -d "$runtime_dir" ] || continue
    for socket in "$runtime_dir"/wayland-*; do
      if [ -S "$socket" ]; then
        export XDG_RUNTIME_DIR=$runtime_dir
        export WAYLAND_DISPLAY=${socket##*/}
        wayland_socket=$socket
        break 2
      fi
    done
  done
fi

if [ -z "$wayland_socket" ] &&
   { [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ] ||
     [ ! -w "$XDG_RUNTIME_DIR" ]; }; then
  for runtime_dir in "/run/user/$(id -u)" "/var/run/user/$(id -u)" \
                     /run/user/0 /var/run/user/0; do
    if [ -d "$runtime_dir" ] && [ -w "$runtime_dir" ]; then
      export XDG_RUNTIME_DIR=$runtime_dir
      break
    fi
  done
fi
if [ -z "$wayland_socket" ] &&
   { [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ] ||
     [ ! -w "$XDG_RUNTIME_DIR" ]; }; then
  mkdir -p /tmp/asm2-runtime &&
    chmod 700 /tmp/asm2-runtime &&
    export XDG_RUNTIME_DIR=/tmp/asm2-runtime
fi
[ -n "$wayland_socket" ] || unset WAYLAND_DISPLAY

# Compositor Amlogic (fbdev) usa o alpha por pixel do framebuffer -> backbuffer
# precisa sair opaco. Em scanout KMS (XRGB) isso e' inocuo mas desnecessario.
if [ -r /sys/class/display/mode ]; then
  export ASM2_FORCE_OPAQUE=${ASM2_FORCE_OPAQUE:-1}
fi

# ---- audio ------------------------------------------------------------------
# NUNCA forcar SDL_AUDIODRIVER. So aponta o socket Pulse quando ele existe
# (NextOS); nos ALSA-cru o SDL negocia sozinho.
for pulse_socket in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$pulse_socket" ]; then
    export PULSE_SERVER="unix:$pulse_socket"
    break
  fi
done

# ---- memoria ----------------------------------------------------------------
MEMKB=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo 0)
if [ "${MEMKB:-0}" -gt 0 ] && [ "$MEMKB" -lt 1200000 ]; then
  export MALLOC_ARENA_MAX=2
  export MALLOC_TRIM_THRESHOLD_=131072
  export MALLOC_MMAP_THRESHOLD_=65536
  echo "[launcher] RAM curta ($((MEMKB/1024))MB) -> malloc enxuto"
fi

# ---- controles --------------------------------------------------------------
# gamecontrollerdb do sistema quando existir; mapping do PortMaster ganha.
for db in "$controlfolder/gamecontrollerdb.txt" \
          /opt/system/Tools/PortMaster/gamecontrollerdb.txt \
          /roms/ports/PortMaster/gamecontrollerdb.txt \
          /usr/share/SDL2/gamecontrollerdb.txt; do
  [ -f "$db" ] && { export SDL_GAMECONTROLLERCONFIG_FILE="$db"; break; }
done
[ -n "${sdl_controllerconfig:-}" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# governor em performance quando der (inocuo onde nao existe)
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo performance > "$g" 2>/dev/null
done

export ASM2_RUN=1

# ---- NXExtract (primeiro boot, BYO-data) ------------------------------------
# Quando o pacote publico traz o extrator universal + receita, o preparo dos
# dados (APK/OBB do dono -> gamefiles/) acontece aqui, transacional e retomavel.
if [ -x "$GAMEDIR/run-extractor.sh" ] && [ -f "$GAMEDIR/extractor.json" ]; then
  "$GAMEDIR/run-extractor.sh" || {
    status=$?
    echo "ASM2: preparo de dados falhou ($status)"
    command -v pm_finish >/dev/null 2>&1 && pm_finish
    exit "$status"
  }
fi

chmod +x "$BIN" 2>/dev/null

command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$BIN" >/dev/null

# RestartGame do lifecycle Android termina deliberadamente o processo com 75.
# O novo processo precisa nascer pelo mesmo launcher e no mesmo foreground.
# Uma unica repeticao reproduz esse contrato sem permitir um loop de restart.
asm2_run_foreground() {
  local restart_count=0 game_status

  while :; do
    "$BIN" "$GAMEDIR"
    game_status=$?

    if [ "$game_status" -eq 75 ] && [ "$restart_count" -eq 0 ]; then
      restart_count=1
      echo "[launcher] RestartGame solicitado; relançando uma vez em foreground"
      sleep 1
      asm2_stop_existing
      continue
    fi

    if [ "$game_status" -eq 75 ]; then
      echo "[launcher] RestartGame repetido; limite de uma repeticao atingido"
    fi
    return "$game_status"
  done
}

asm2_run_foreground
ASM2_GAME_STATUS=$?
asm2_finish
exit "$ASM2_GAME_STATUS"
