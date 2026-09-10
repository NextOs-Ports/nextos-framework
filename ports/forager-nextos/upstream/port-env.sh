#!/bin/bash
# port-env.sh — adaptador do Forager (carregado pelo launcher nxbootstrap).
# O locale global do firmware continua neutro. A escolha validada abaixo e
# entregue diretamente aos helpers Android do runner, sem alterar ferramentas,
# NXExtract ou caminhos do host.
export LANG=C LC_ALL=C
# core dump de processo com centenas de MB de textura enche o cartao.
ulimit -c 0

forager_language_valid() {
  case "$1" in
    en|pt-br|es|fr|de|ru|tr|th|ja|ko|zh-cn|zh-tw) return 0 ;;
    *) return 1 ;;
  esac
}

forager_language_register() {
  local code=$1 title=$2 description=$3
  PortMasterDialog "register_set_info" forager-language "$code" \
    "title:$title" "description:$description"
}

# GAME_LANGUAGE=auto abre a lista nativa do PortMaster. A ultima escolha fica
# em data/, fora da arvore runtime selada, e portanto nunca invalida o marker do
# NXExtract. Um override explicito no launcher/ambiente pula a interface.
forager_language_file="$GAMEDIR/data/language.txt"
forager_language_cached=en
if [ -f "$forager_language_file" ] && [ ! -L "$forager_language_file" ]; then
  IFS= read -r forager_language_cached < "$forager_language_file" || true
  forager_language_valid "$forager_language_cached" || forager_language_cached=en
fi

forager_language_selected=${NXPORT_LANGUAGE:-auto}
if [ "$forager_language_selected" = auto ]; then
  forager_language_selected=$forager_language_cached
  case "${PM_FUNCS_VERSION:-}" in
    3|4|5|6|7|8|9|[1-9][0-9]*)
      if [ -x "${controlfolder:-}/pugwash" ] && \
         declare -F PortMasterDialogInit >/dev/null 2>&1 && \
         declare -F PortMasterDialog >/dev/null 2>&1 && \
         declare -F PortMasterDialogResult >/dev/null 2>&1 && \
         declare -F PortMasterDialogExit >/dev/null 2>&1; then
        PortMasterDialogInit "no-harbour"
        if [ -p "${PM_PIPE:-}" ] && [ ! -L "${PM_PIPE:-}" ]; then
          PortMasterDialog "register_clear" forager-language
          case "$forager_language_cached" in
            pt-br) forager_language_register pt-br "Português (Brasil) ✓" "Idioma atual / Current language" ;;
            en)    forager_language_register en "English ✓" "Idioma atual / Current language" ;;
            es)    forager_language_register es "Español ✓" "Idioma atual / Current language" ;;
            fr)    forager_language_register fr "Français ✓" "Idioma atual / Current language" ;;
            de)    forager_language_register de "Deutsch ✓" "Idioma atual / Current language" ;;
            ru)    forager_language_register ru "Русский ✓" "Idioma atual / Current language" ;;
            tr)    forager_language_register tr "Türkçe ✓" "Idioma atual / Current language" ;;
            th)    forager_language_register th "ไทย ✓" "Idioma atual / Current language" ;;
            ja)    forager_language_register ja "日本語 ✓" "Idioma atual / Current language" ;;
            ko)    forager_language_register ko "한국어 ✓" "Idioma atual / Current language" ;;
            zh-cn) forager_language_register zh-cn "简体中文 ✓" "Idioma atual / Current language" ;;
            zh-tw) forager_language_register zh-tw "繁體中文 ✓" "Idioma atual / Current language" ;;
          esac
          [ "$forager_language_cached" = pt-br ] || forager_language_register pt-br "Português (Brasil)" "Portuguese / Português"
          [ "$forager_language_cached" = en ]    || forager_language_register en "English" "English / Inglês"
          [ "$forager_language_cached" = es ]    || forager_language_register es "Español" "Spanish / Espanhol"
          [ "$forager_language_cached" = fr ]    || forager_language_register fr "Français" "French / Francês"
          [ "$forager_language_cached" = de ]    || forager_language_register de "Deutsch" "German / Alemão"
          [ "$forager_language_cached" = ru ]    || forager_language_register ru "Русский" "Russian / Russo"
          [ "$forager_language_cached" = tr ]    || forager_language_register tr "Türkçe" "Turkish / Turco"
          [ "$forager_language_cached" = th ]    || forager_language_register th "ไทย" "Thai / Tailandês"
          [ "$forager_language_cached" = ja ]    || forager_language_register ja "日本語" "Japanese / Japonês"
          [ "$forager_language_cached" = ko ]    || forager_language_register ko "한국어" "Korean / Coreano"
          [ "$forager_language_cached" = zh-cn ] || forager_language_register zh-cn "简体中文" "Chinese (Simplified)"
          [ "$forager_language_cached" = zh-tw ] || forager_language_register zh-tw "繁體中文" "Chinese (Traditional)"
          forager_language_result=$(PortMasterDialogResult \
            "selection_list" "--register=forager-language" "--want-description")
          if forager_language_valid "$forager_language_result"; then
            forager_language_selected=$forager_language_result
          fi
        fi
        PortMasterDialogExit
      fi
      ;;
  esac
fi
forager_language_valid "$forager_language_selected" || forager_language_selected=en

if [ ! -L "$forager_language_file" ]; then
  forager_language_tmp="$GAMEDIR/data/.language.$$.tmp"
  if [ ! -e "$forager_language_tmp" ] && [ ! -L "$forager_language_tmp" ] && \
     (umask 077; set -C; printf '%s\n' "$forager_language_selected" > "$forager_language_tmp") 2>/dev/null; then
    mv -f -- "$forager_language_tmp" "$forager_language_file"
  fi
  [ ! -e "${forager_language_tmp:-}" ] || rm -f -- "$forager_language_tmp"
fi

case "$forager_language_selected" in
  pt-br) FORAGER_LANGUAGE=pt; FORAGER_REGION=BR; FORAGER_LANGUAGE_ASSET=portuguese ;;
  es)    FORAGER_LANGUAGE=es; FORAGER_REGION=ES; FORAGER_LANGUAGE_ASSET=spanish ;;
  fr)    FORAGER_LANGUAGE=fr; FORAGER_REGION=FR; FORAGER_LANGUAGE_ASSET=french ;;
  de)    FORAGER_LANGUAGE=de; FORAGER_REGION=DE; FORAGER_LANGUAGE_ASSET=german ;;
  ru)    FORAGER_LANGUAGE=ru; FORAGER_REGION=RU; FORAGER_LANGUAGE_ASSET=russian ;;
  tr)    FORAGER_LANGUAGE=tr; FORAGER_REGION=TR; FORAGER_LANGUAGE_ASSET=turkish ;;
  th)    FORAGER_LANGUAGE=th; FORAGER_REGION=TH; FORAGER_LANGUAGE_ASSET=thai ;;
  ja)    FORAGER_LANGUAGE=ja; FORAGER_REGION=JP; FORAGER_LANGUAGE_ASSET=japanese ;;
  ko)    FORAGER_LANGUAGE=ko; FORAGER_REGION=KR; FORAGER_LANGUAGE_ASSET=korean ;;
  zh-cn) FORAGER_LANGUAGE=zh; FORAGER_REGION=CN; FORAGER_LANGUAGE_ASSET=chinese ;;
  zh-tw) FORAGER_LANGUAGE=zh; FORAGER_REGION=TW; FORAGER_LANGUAGE_ASSET=chinese_traditional ;;
  *)     FORAGER_LANGUAGE=en; FORAGER_REGION=US; FORAGER_LANGUAGE_ASSET=english ;;
esac
export FORAGER_LANGUAGE FORAGER_REGION FORAGER_LANGUAGE_ASSET
echo "[forager-language] selected=$forager_language_selected locale=$FORAGER_LANGUAGE-$FORAGER_REGION"
unset forager_language_cached forager_language_file forager_language_result
unset forager_language_selected forager_language_tmp
unset -f forager_language_valid forager_language_register

# Alguns firmwares ARMHF publicam o provider Mali em libEGL.so/libGLESv2.so,
# mas deixam libEGL.so.1 apontando para uma implementacao diferente. O SDL
# pode entao montar um par EGL/GLES cruzado e falhar antes de criar a janela.
# Preserve hints do firmware e aplique o par somente quando os dois links nao
# versionados provarem ser o mesmo objeto e o EGL versionado provar o conflito.
if [ -z "${SDL_VIDEO_EGL_DRIVER:-}" ] && [ -z "${SDL_VIDEO_GL_DRIVER:-}" ]; then
  for nx_provider_dir in \
    /usr/local/lib/arm-linux-gnueabihf \
    /usr/lib/arm-linux-gnueabihf \
    /lib/arm-linux-gnueabihf \
    /usr/local/lib32 /usr/lib32 /lib32; do
    [ -f "$nx_provider_dir/libEGL.so" ] || continue
    [ -f "$nx_provider_dir/libEGL.so.1" ] || continue
    [ -f "$nx_provider_dir/libGLESv2.so" ] || continue
    nx_egl_provider=$(readlink -f "$nx_provider_dir/libEGL.so" 2>/dev/null) || continue
    nx_egl_versioned=$(readlink -f "$nx_provider_dir/libEGL.so.1" 2>/dev/null) || continue
    nx_gles_provider=$(readlink -f "$nx_provider_dir/libGLESv2.so" 2>/dev/null) || continue
    [ -f "$nx_egl_provider" ] && [ "$nx_egl_provider" = "$nx_gles_provider" ] || continue
    [ "$nx_egl_provider" != "$nx_egl_versioned" ] || continue
    export SDL_VIDEO_EGL_DRIVER="$nx_egl_provider"
    export SDL_VIDEO_GL_DRIVER="$nx_egl_provider"
    echo "[forager-gl] coherent ARMHF EGL/GLES provider selected"
    break
  done
  unset nx_provider_dir nx_egl_provider nx_egl_versioned nx_gles_provider
fi
