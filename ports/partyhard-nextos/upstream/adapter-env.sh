#!/bin/sh
# Party Hard GO — sealed adapter hook sourced by nxbootstrap after NXExtract
# and the mandatory NXSplash. Owner-editable settings live separately in
# NEXTOSSETTINGS.txt and port-env.sh.

# dArkOS exposes both Mesa's versioned EGL dispatcher and the native Mali
# provider. SDL/KMSDRM needs the portable unversioned provider names used by
# the proven NXSplash fallback; preserve any explicit firmware choices.
case "${CFW_NAME:-}" in
  dArkOSRE|dArkOS*|ArkOS*)
    : "${SDL_VIDEO_EGL_DRIVER:=libEGL.so}"
    : "${SDL_VIDEO_GL_DRIVER:=libGLESv2.so}"
    export SDL_VIDEO_EGL_DRIVER SDL_VIDEO_GL_DRIVER
    ;;
esac

# V5 provider-aware C6 seam. The runtime decides against the SDL actually
# mapped, never against the firmware name.
export NXC6_SEAM=1
export NXC6_RECEIPT="$GAMEDIR/nxc6-receipt.log"

# Runtime evidence for the editable NEXTOS_CONTROLLERS/4 map.
export NXGPTK_RECEIPT="$GAMEDIR/nxgptk-receipt.jsonl"
: > "$NXGPTK_RECEIPT" 2>/dev/null || true
