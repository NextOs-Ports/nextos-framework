#!/bin/sh
# port-env.sh — Tearscape (NextOS) — HOOK DO DONO / OWNER HOOK
#
# PT: Este arquivo é SEU. Ele é semeado uma vez a partir de defaults/port-env.sh,
#     nunca é sobrescrito por atualização, health ou healing (um default novo aparece
#     como port-env.sh.new). A lógica selada do port vive em adapter-env.sh (não edite
#     esse). Aqui você pode exportar variáveis de runtime/diagnóstico. O aspecto da
#     imagem NÃO mora aqui: edite video.aspect em NEXTOSSETTINGS.txt. Variáveis
#     reservadas do framework (GAMEDIR, PORT_ID, NXBOOTSTRAP_*) NÃO podem ser
#     alteradas: o launcher recusa, com o nome da variável, e mantém seus bytes.
#     Se o arquivo não parsear, a abertura aborta visivelmente (linha do erro no log);
#     apague-o para reseed.
# EN: This file is YOURS. It is seeded once from defaults/port-env.sh and never
#     overwritten by updates, health or healing (a new default shows up as
#     port-env.sh.new). The port's sealed logic lives in adapter-env.sh (do not
#     edit that one). Export runtime/diagnostic variables here. The image aspect
#     does NOT live here: edit video.aspect in NEXTOSSETTINGS.txt. Framework
#     reserved variables (GAMEDIR, PORT_ID, NXBOOTSTRAP_*) cannot be changed: the
#     launcher refuses, naming the variable, and keeps your bytes. If the file does
#     not parse the launch aborts visibly (error line in the log); delete it to reseed.
#
# Exemplos / examples (remova o '#' para ativar / remove the '#' to enable):
# export NX_VIDEO_ASPECT=preserve   # overrides NEXTOSSETTINGS.txt for one owner
