#!/bin/sh
# The public contract is measured through the system SDL-owned context on every
# supported device.  A silent raw-EGL fallback would bypass that proof.
FP2_PURE_SDL_CONTEXTS=1
export FP2_PURE_SDL_CONTEXTS
unset FP2_RAW_EGL_CONTEXTS

# dArkOS exposes both Mesa's versioned EGL dispatcher and the native Mali
# provider.  SDL/KMSDRM needs the portable unversioned provider names used by
# the proven NXSplash fallback; preserve any explicit firmware choices.
case "${CFW_NAME:-}" in
  dArkOSRE|dArkOS*|ArkOS*)
    : "${SDL_VIDEO_EGL_DRIVER:=libEGL.so}"
    : "${SDL_VIDEO_GL_DRIVER:=libGLESv2.so}"
    FP2_PURE_SDL_CONTEXTS=1
    export SDL_VIDEO_EGL_DRIVER SDL_VIDEO_GL_DRIVER
    ;;
esac

# nxinput C6 seam (V4): the firmware SDL2 path admits the pad before
# announcing it, resolving the sovereign PortMaster mapping (authority 1 = the
# launcher's get_controls line, staged out of SDL_GAMECONTROLLERCONFIG by the
# loader before SDL_Init; authority 2 = the CFW database; authority 3 = the
# controllers.nxb bundle pinned in this ZIP).  Receipts land next to the log.
export NXC6_SEAM=1
export NXC6_RECEIPT="$GAMEDIR/nxc6-receipt.log"
# Evidência do runtime GPTK vivo (nxinput-gptk-event-evidence/1): um arquivo por
# execução, na pasta do jogo; é a única fonte aceita para o candidate-lock (botões
# físicos, nunca estímulo sintético).
export NXGPTK_RECEIPT="$GAMEDIR/nxgptk-receipt.jsonl"
: > "$NXGPTK_RECEIPT" 2>/dev/null || true
