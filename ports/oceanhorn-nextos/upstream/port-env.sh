# ============================================================================
#  OCEANHORN CHRONOS DUNGEON — PERFIL DE DESEMPENHO / PERFORMANCE PROFILE
#
#  Escolha UM: auto | low | medium | high      (padrão: auto)
#  Pick ONE:   auto | low | medium | high      (default: auto)
#
#    auto   = low em aparelho de ~1 GB, high nos demais (padrão).
#    low    = aparelhos de ~1 GB: render na METADE da resolução com upscale
#             inteiro pixel-perfect (o remédio do lag: fill-rate) + economia
#             de textura 2x (teto 512 + RGBA4444).
#    medium = economia de textura 1,5x (teto 768), render nativo.
#    high   = tudo nativo (1x): resolução e texturas cheias.
#
#  Como mudar / How to change:
#    1) Edite a linha OCEANHORN_PROFILE abaixo; OU
#    2) exporte OCEANHORN_PROFILE=medium no ambiente antes de abrir; OU
#    3) ajuste fino: qualquer CUP_* exportada no ambiente VENCE este arquivo
#       (ex.: CUP_RENDERSCALE=0 desliga só a redução de render no perfil low).
# ============================================================================
OCEANHORN_PROFILE="${OCEANHORN_PROFILE:-auto}"
case "$OCEANHORN_PROFILE" in
  auto|low|medium|high) ;;
  *) OCEANHORN_PROFILE=auto ;;
esac

# Ajustes do usuário/testador em userdata/ (a atualização do port nunca
# sobrescreve): pode trocar o perfil ou qualquer knob sem editar arquivos
# distribuídos.
if [ -r "userdata/ocean-env.sh" ] && [ ! -L "userdata/ocean-env.sh" ]; then
  # shellcheck disable=SC1091
  . "userdata/ocean-env.sh"
fi

# ---- fatos do aparelho (RAM e DRM) usados pelo auto e pelos tetos --------
_oc_mem_kb=$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo 2>/dev/null || true)
case "${_oc_mem_kb:-}" in ''|*[!0-9]*) _oc_mem_kb=0 ;; esac
_oc_drm=0
for _oc_card in /dev/dri/card[0-9]*; do
  [ -e "$_oc_card" ] && { _oc_drm=1; break; }
done

if [ "$OCEANHORN_PROFILE" = auto ]; then
  if [ "$_oc_mem_kb" -gt 0 ] && [ "$_oc_mem_kb" -lt 1250000 ]; then
    OCEANHORN_PROFILE=low
  else
    OCEANHORN_PROFILE=high
  fi
fi
export OCEANHORN_PROFILE

case "$OCEANHORN_PROFILE" in
  low)
    # Render reduzido (2.0.0: no ES3/KMSDRM o loader entra em modo LO-REPORT
    # — a engine enxerga a resolução baixa desde o 1º frame e o blit sobe
    # pixel-perfect para o painel) + economia de textura 2x.
    CUP_RENDERSCALE="${CUP_RENDERSCALE:-2}"
    CUP_TEXHALF="${CUP_TEXHALF:-512}"
    [ "$_oc_drm" -eq 1 ] && CUP_TEX16="${CUP_TEX16:-1}"
    ;;
  medium)
    # Economia de textura 1,5x: teto 768, render nativo.
    CUP_RENDERSCALE="${CUP_RENDERSCALE:-0}"
    CUP_TEXHALF="${CUP_TEXHALF:-768}"
    ;;
  high)
    CUP_RENDERSCALE="${CUP_RENDERSCALE:-0}"
    CUP_TEXHALF="${CUP_TEXHALF:-0}"
    ;;
esac
[ "${CUP_RENDERSCALE:-0}" != 0 ] && export CUP_RENDERSCALE || unset CUP_RENDERSCALE
[ "${CUP_TEXHALF:-0}" != 0 ] && export CUP_TEXHALF || unset CUP_TEXHALF
[ -n "${CUP_TEX16:-}" ] && [ "${CUP_TEX16}" != 0 ] && export CUP_TEX16 || unset CUP_TEX16

# ---- resto do runtime (herdado do launcher antigo, sem swap e sem GC) ----
# Unity/FMOD criam muitas threads curtas; duas arenas evitam reter um heap
# fragmentado por thread no alvo de 1 GB.
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
if [ "$_oc_mem_kb" -gt 0 ] && [ "$_oc_mem_kb" -lt 1250000 ]; then
  export MALLOC_TRIM_THRESHOLD_=${MALLOC_TRIM_THRESHOLD_:-131072}
  export MALLOC_MMAP_THRESHOLD_=${MALLOC_MMAP_THRESHOLD_:-65536}
fi

# O UnityPlayer deste título é guiado pelo Choreographer a 30 Hz; manter a
# cadência nativa em todo backend (host livre só desperdiça CPU/GPU).
export OCEAN_FRAME_LIMIT=${OCEAN_FRAME_LIMIT:-30}

# Resolução real do painel, sem assumir nada: fb0 mode -> DRM -> virtual_size
# (com detecção de double-buffer). Alimenta o JNI antes da janela existir.
_oc_m=
[ -r /sys/class/graphics/fb0/mode ]  && read -r _oc_m < /sys/class/graphics/fb0/mode  || true
[ -z "$_oc_m" ] && [ -r /sys/class/graphics/fb0/modes ] && read -r _oc_m < /sys/class/graphics/fb0/modes || true
if [ -n "$_oc_m" ]; then
  _oc_p=${_oc_m#*:}; _oc_p=${_oc_p%%[!0-9x]*}
  _oc_w=${_oc_p%x*}; _oc_h=${_oc_p#*x}
  case "$_oc_w" in ''|*[!0-9]*) _oc_w= ;; esac
  case "$_oc_h" in ''|*[!0-9]*) _oc_h= ;; esac
fi
if [ -z "${_oc_w:-}" ] || [ -z "${_oc_h:-}" ]; then
  for _oc_status in /sys/class/drm/card*-*/status; do
    [ -r "$_oc_status" ] || continue
    read -r _oc_conn < "$_oc_status" || true
    [ "$_oc_conn" = connected ] || continue
    _oc_modes=${_oc_status%/status}/modes
    [ -r "$_oc_modes" ] || continue
    read -r _oc_mode < "$_oc_modes" || true
    _oc_w=${_oc_mode%x*}; _oc_h=${_oc_mode#*x}
    case "$_oc_w" in ''|*[!0-9]*) _oc_w= ;; esac
    case "$_oc_h" in ''|*[!0-9]*) _oc_h= ;; esac
    [ -n "${_oc_w:-}" ] && [ -n "${_oc_h:-}" ] && break
  done
fi
if { [ -z "${_oc_w:-}" ] || [ -z "${_oc_h:-}" ]; } && [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _oc_w _oc_vh < /sys/class/graphics/fb0/virtual_size || true
  case "${_oc_w:-}" in ''|*[!0-9]*) _oc_w= ;; esac
  case "${_oc_vh:-}" in ''|*[!0-9]*) _oc_vh= ;; esac
  if [ -n "${_oc_w:-}" ] && [ -n "${_oc_vh:-}" ]; then
    _oc_h=$_oc_vh
    # 1280x1440 e 640x960 são double-buffer; 640x480 não é.
    if [ "$_oc_vh" -gt "$_oc_w" ] && [ $((_oc_vh % 2)) -eq 0 ]; then
      _oc_half=$((_oc_vh / 2))
      if [ "$_oc_half" -le "$_oc_w" ] && [ $((_oc_half * 2)) -ge "$_oc_w" ]; then
        _oc_h=$_oc_half
      fi
    fi
  fi
fi
[ -n "${_oc_w:-}" ] && [ -n "${_oc_h:-}" ] && export TER_SCREEN_W="$_oc_w" TER_SCREEN_H="$_oc_h"

# Log em arquivo é do launcher; o dup2 interno travava a init (lição Terraria).
export TER_NOSTORAGEPATCH=1
export CUP_NOLOGFILE=1
export CUP_FRAMES=${CUP_FRAMES:-0}

# Sem DRM (fbdev/Amlogic) o compositor exige alpha=1 no backbuffer; em KMSDRM
# não há motivo comprovado, então só liga onde é preciso.
if [ -z "${CUP_ALPHAFIX:-}" ] && [ "$_oc_drm" -eq 0 ]; then
  CUP_ALPHAFIX=1
fi
[ -n "${CUP_ALPHAFIX:-}" ] && export CUP_ALPHAFIX || unset CUP_ALPHAFIX

# FMOD: stream NONBLOCKING vira ERR_INTERNAL depois do createSound; converter
# proativamente em sample residente síncrono (FSB original preservado).
export HC_STREAM_FALLBACK=${HC_STREAM_FALLBACK:-1}

# Pulse quando o firmware oferece o socket (ROCKNIX etc.).
for _oc_pulse in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$_oc_pulse" ]; then
    export PULSE_SERVER="unix:$_oc_pulse"
    break
  fi
done

# Identidade única e estável de gamepad para o Android/Unity; o bridge do
# port normaliza o estado físico para o layout Xbox/XInput.
export TER_GAMEPAD=1

# GC do il2cpp NUNCA é desligado nem forçado (o GCEVERY crashou no campo);
# pressão de memória usa o caminho Android nativo (nativeLowMemory, no binário).
[ -n "${CUP_GCOFF:-}" ] && export CUP_GCOFF || unset CUP_GCOFF

# O pin de desempenho da GPU Mali-450 (mpgpu min_freq/min_pp + guarda térmica)
# mora no BINÁRIO desde a 2.0.0 (gpu_perf.c), com restauração garantida na
# saída. OCEAN_GPU_PERFORMANCE=0 desliga.

unset _oc_mem_kb _oc_drm _oc_card _oc_m _oc_p _oc_w _oc_h _oc_vh _oc_half \
      _oc_status _oc_conn _oc_modes _oc_mode _oc_pulse 2>/dev/null || true
