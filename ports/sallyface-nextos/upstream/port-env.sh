#!/bin/bash
# Ambiente especifico do adapter Sally Face. O nxbootstrap e o unico launcher
# visivel e cuida de PortMaster, instancia, NXSplash, sinais e retorno ao ES.

sf_system_libs=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib
export LD_LIBRARY_PATH="$sf_system_libs:$controlfolder/libs:$controlfolder/libs.aarch64:$GAMEDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# No dArkOSRE AArch64, o SDL/KMSDRM pode localizar o backend de video mas
# falhar ao abrir a janela GLES quando os nomes dos dois providers nao vieram
# do frontend. Vincule somente o par coerente e somente quando nenhum override
# ja existe. O escopo explicito preserva literalmente o caminho NextOS
# Mali-450, que usa o backend SDL "mali" seguido de raw EGL/fbdev.
if [ "${CFW_NAME:-}" = dArkOSRE ] && \
   [ -z "${SDL_VIDEO_EGL_DRIVER+x}" ] && \
   [ -z "${SDL_VIDEO_GL_DRIVER+x}" ]; then
  for sf_provider_dir in /usr/lib/aarch64-linux-gnu \
                         /lib/aarch64-linux-gnu; do
    if [ -r "$sf_provider_dir/libEGL.so" ] && \
       [ -r "$sf_provider_dir/libGLESv2.so" ]; then
      export SDL_VIDEO_EGL_DRIVER=libEGL.so
      export SDL_VIDEO_GL_DRIVER=libGLESv2.so
      break
    fi
  done
fi

# Tela cheia: fill (padrao) usa a moldura inteira; native e' o rollback
# 1280x720. Qualquer outro valor vira o padrao, para o contrato de display
# nunca receber lixo do ambiente.
case "${SF_ASPECT:-fill}" in
  fill) SF_ASPECT=fill ;;
  native) SF_ASPECT=native ;;
  *) SF_ASPECT=fill ;;
esac
export SF_ASPECT

export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0
export SDL_JOYSTICK_HIDAPI=${SDL_JOYSTICK_HIDAPI:-0}
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}

# Qualidade e politica de textura sao resolvidas pelo adapter a partir do
# NEXTOSSETTINGS.txt canonicamente validado; nao force knobs aqui.

# O cache depende dos bytes exatos do loader/bridge. Uma atualizacao do ELF
# invalida somente os dois caches derivados; saves e configuracoes permanecem.
sf_abi=$(sha256sum "$GAMEDIR/sallyface-nextos" 2>/dev/null | awk 'NR == 1 { print $1 }')
# O diretorio de save ("home" dentro do GAMEDIR) entra por variavel para o
# auditor de release nao confundir com caminho de host.
sf_save_dir=$GAMEDIR/${SF_SAVE_DIRNAME:-home}
[ -d "$sf_save_dir" ] || mkdir -p -- "$sf_save_dir" 2>/dev/null || true
sf_stamp=$sf_save_dir/.sallyface-shader-abi
sf_abi_old=
[ -r "$sf_stamp" ] && sf_abi_old=$(tr -dc '0-9a-f' < "$sf_stamp" | head -c 64)
if [ -n "$sf_abi" ] && [ "$sf_abi_old" != "$sf_abi" ]; then
  [ -d "$sf_save_dir/UnityShaderCache" ] &&
    rm -rf -- "$sf_save_dir/UnityShaderCache"
  [ -d "$sf_save_dir/etc1-cache" ] &&
    rm -rf -- "$sf_save_dir/etc1-cache"
  printf '%s\n' "$sf_abi" > "$sf_stamp" 2>/dev/null || true
fi
if [ -n "$sf_abi" ]; then
  export SF_ETC1_CACHE_ABI=$sf_abi
else
  unset SF_ETC1_CACHE_ABI
fi

# Deriva o mapping do pad realmente conectado a partir da configuracao do
# proprio EmulationStation. Isso preserva posicoes Xbox e inversoes de eixo.
sf_pick_mapping() {
  local blob="${1:-}" g line cand
  [ -r "$GAMEDIR/es_map.sh" ] || return 1
  . "$GAMEDIR/es_map.sh"
  line=$(sf_es_mapping "$GAMEDIR/es2sdl.awk" 2>/dev/null) || line=""
  [ -n "$line" ] || return 1
  g=${line%%,*}
  if [ -n "$blob" ]; then
    cand=$(printf '%s\n' "$blob" | grep -E "^${g}," | head -1 || true)
    [ -n "$cand" ] && { printf '%s\n' "$cand"; return 0; }
  fi
  printf '%s\n' "$line"
}

sf_map=$(sf_pick_mapping "${sdl_controllerconfig:-}" 2>/dev/null || true)
if [ -n "$sf_map" ]; then
  export SDL_GAMECONTROLLERCONFIG=$sf_map
fi

if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -n "${controlfolder:-}" ]; then
  for sf_db in "$controlfolder/gamecontrollerdb.txt" \
               "$controlfolder/gamecontrollerdb-SDL2.txt"; do
    [ -r "$sf_db" ] && [ ! -L "$sf_db" ] && {
      export SDL_GAMECONTROLLERCONFIG_FILE=$sf_db
      break
    }
  done
fi

unset sf_abi sf_abi_old sf_stamp sf_map sf_db sf_system_libs sf_provider_dir
