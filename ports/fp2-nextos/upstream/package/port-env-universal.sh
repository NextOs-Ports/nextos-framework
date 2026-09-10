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
