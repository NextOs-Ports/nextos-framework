#!/bin/bash
# Writable owner-data stays outside the NXExtract payload seal.
set -euo pipefail
umask 077

cd "$(dirname -- "${BASH_SOURCE[0]}")"
[ ! -L data ] || { echo "ERROR: unsafe Forager data path"; exit 1; }
mkdir -p data

# One-way migration from the proven Mali-only layout. Never overwrite a save
# already created by the universal package.
if [ -d saves ] && [ ! -L saves ] && [ ! -e data/.legacy-saves-imported ]; then
  if [ -z "$(find data -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
    cp -a saves/. data/
  fi
  : > data/.legacy-saves-imported
fi

# O perfil Android de fabrica grava musica/SFX em valores que o proprio jogo
# converte para dezenas de dB de atenuacao. Instale o config aprovado do Mali
# apenas quando ainda nao existir um config do usuario; mudancas posteriores no
# menu OPTIONS continuam persistentes e nunca sao reescritas a cada boot.
if [ ! -e data/config.txt ]; then
  [ -f defaults/config.txt ] && [ ! -L defaults/config.txt ] || {
    echo "ERROR: missing safe Forager default configuration"
    exit 1
  }
  cp defaults/config.txt data/config.txt
fi
