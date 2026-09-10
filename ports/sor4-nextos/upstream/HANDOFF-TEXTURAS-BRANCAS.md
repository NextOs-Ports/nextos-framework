# HANDOFF — Texturas brancas (cercas/escadas/sprites) — RESOLVIDO 2026-06-18

> Sessão autônoma. Resolvi o bug das texturas brancas.

## O bug
Cercas, grades, **escadas de fundo** (e na verdade sprites de personagem também) apareciam
BRANCAS — pior a partir do ato 2 da fase 2. Veio do **MASKFIX**.

## Diagnóstico (definitivo, via `texconv --diag`)
Adicionei um modo `--diag` no texconv (lê e categoriza sem escrever). Resultado sobre o APK:
- **785 texturas embranquecidas** pelo MASKFIX: **757 em `animatedsprites/`** (sombras/
  silhuetas de personagem) + **28 em `decors/`** (cercas/escadas de cenário).
- **0 fontes embranquecidas.** Os atlas `gui/fonts/{font_50x50,numbers_62x62}.xnb` são
  **COLORIDOS** (têm cor no pixel opaco) → convertem normal e renderizam legíveis SEM maskfix
  (confirmado de olho que o texto fica legível no build anterior).

**Conclusão:** o MASKFIX (que pintava RGB=A pra "fazer texto aparecer") era uma heurística
equivocada — nenhuma fonte deste jogo depende dele; ele só embranquecia por engano qualquer
textura ESCURA+alpha (sombra, silhueta, fio de cerca, degrau).

## Fix (commit 5e249eb)
**MASKFIX DESLIGADO por padrão**, nos DOIS lugares onde existia:
1. `texconv` (conversão do progressor): `NoMaskFix=true` default; reativa com `--maskfix`.
2. `Texture2DReader.SOR4.cs` (runtime): roda só se `SOR4_MASKFIX=1` (default off). Na prática
   nem roda — o progressor converte 100% das ASTC, então não sobra ASTC pra decodificar em jogo.

Como o MASKFIX antigo já estava ASSADO nas texturas convertidas, foi preciso **RE-CONVERTER
do APK** (feito nesta sessão, ~25905 arquivos, do `Streets-of-Rage-4-v1.4.5...apk`).

## Estado deixado
- DLLs novos deployados no device (.127): `MonoGame.Framework.dll`=fce76d46,
  `sor4texconv.dll`=6c4b517e. Reconversão com MASKFIX off **rodando/concluída**.
- texcache limpo, ES reiniciada.
- **Zip de distribuição atualizado** com os DLLs novos (instalação BYO futura já vem certa).
- Áudio (audioout/music_ids) intacto.

## ✅ VALIDADO DE OLHO pelo porter (2026-06-18)
"arrumou tudo mesmo" — corrigiu cercas/escadas/sombras E **o menu** (preto-transparente voltou
ao certo) E **a barra de vida**. Um fix conceitual resolveu vários bugs de uma vez (o MASKFIX
atacava TODO escuro+alpha, não só as cercas). Fontes/texto seguem legíveis. CASO FECHADO.

- Instalação BYO nova: o progressor do zip chama `texconv --apk ... $SCALE` **sem `--maskfix`**
  → já converte SEM branco automaticamente. Nada a repetir.
- Os 1479 "puladas" são **não-ASTC** (já Color/RGBA) — corretas como estão, não é bug.
- Recuperação (só se algum texto sumir, não deve): `--maskfix` no progressor ou `SOR4_MASKFIX=1`.

## Modo diag (pra reusar)
`sor4host --run-dll sor4texconv.dll --apk <apk> <gameassets> 3 --diag` → loga `[SKIP rN]`
(r1=naoXNB r2=lzx r4=naoASTC r5=dims r6=blocoASTCdesconhecido r7=decodeErr) e `[WHITEN] <path>`.
