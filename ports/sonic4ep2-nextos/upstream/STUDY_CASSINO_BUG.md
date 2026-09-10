# Sonic 4 EP2 — bug do fundo estourado (Episode Metal / "Electric Road") — estudo completo

Status: **NÃO RESOLVIDO** (2026-06-30). Investigação longa, muitas hipóteses testadas e
DESCARTADAS. Este doc consolida TUDO pra retomar rápido.

## SINTOMA (confirmado pelo usuário, com vídeo de referência)
- Fase: **Episode Metal → "Electric Road"** (música `ep1_sng_z2a1`; é Casino Street do EP1 com
  Metal Sonic; a fase é "à esquerda" no world map do Episode Metal).
- **Original:** fundo PRETO, luzes/feixes de holofote FINOS e pequenos.
- **No nosso port (GAMEPLAY):** o **FUNDO INTEIRO estoura pra BRANCO** + objetos (Metal Sonic,
  plataformas) **replicam/borram horizontalmente** ("replicado mil vezes"). Faixas de luz brancas
  gigantes que não existem no original.
- **No PAUSE: fica CORRETO** (fundo preto, feixes finos). Despausa → re-estoura. O usuário diz
  "o pause faz a limpeza e volta preto, mas a luz logo passa e buga tudo".
- => é um efeito que **só acumula/roda no gameplay** (frame advancing); no pause (lógica
  congelada) fica correto.

## HIPÓTESES TESTADAS E DESCARTADAS (nenhuma resolveu)
1. **Framebuffer não-limpo (FBCLEAR)** — glClear no back-buffer antes do DrawFrame. NÃO resolveu.
2. **FBO 0 nunca limpo** — FBO-STATS provou que o engine limpa só o FBO 2; FBO 0 (tela) tem
   draws=1683 clears=0. Liguei FBO 0 + glClear (SONIC_FBCLEAR corrigido + SONIC_FBO0CLEAR). NÃO resolveu.
3. **Light-mask post-effect (NOLIGHTMASK)** — desliga `amPostEFLightMaskDraw`+distortion. NÃO resolveu.
4. **Post-effect inteiro (NOPOSTFX)** — mata `_amPostEFExecEffect`+`amPostEFUpdate`. NÃO resolveu.
5. **Bloom/tone-map (FULLFX)** — liga bloom+tonemap (achei que desligar bloom matava o tone-map
   HDR->LDR junto). NÃO resolveu.
6. **God-ray (NOGODRAY)** — desliga `gm::mapfar::C_MGR::FuncDrawGodray` (god-ray radial blur do
   fundo). NÃO resolveu (e o usuário diz que é o FUNDO INTEIRO, não só os raios).
7. **Thread de draw (THREADDRAW=1)** — `amThreadCheckDraw` retorna 1 sempre (single-thread = draw
   thread). **QUEBROU a seleção de mundos** (revertido). Não é o valor certo.

## ACHADOS-CHAVE (RE da libfox, capstone)
- **FBO-STATS (instrumentação própria, imports.c):** num gameplay, FBO 2 limpo todo frame (~20
  draws), FBO 0 NUNCA limpo, FBO 3 (46860 draws/357 clears = ~1/5 frames) e FBO 5 sub-limpos.
  O smear NÃO é acúmulo de draws (count normal ~20/frame) -> é SHADER ou camada (FBO) específica.
- **Shaders achados:** `gsGxGetGodRayRadialBlur_VS/PS`, `gsGxGetGodRayRadialBlur2`,
  `gsGxGetToneMapLog/Small/Adapte` (tone-map adaptativo = auto-exposição!), `gsGxGetBloomGauss`,
  `gsGxGetBloomDrawHiLuminance`, `gsGxGetGaussBlur`.
- **`ToneMapAdapte`** (auto-exposição adaptativa) = SUSPEITO FORTE não-testado: se a adaptação de
  exposição está bugada (adapta pro errado durante o gameplay), estoura tudo pra branco; no pause
  a adaptação congela -> correto. **PRÓXIMO A TESTAR.**
- **mapfar (fundo):** `gm::mapfar::C_MGR` desenha o fundo (DrawGodray, GetStageId@0x3c499c). O
  fundo inteiro bugando -> o RT do mapfar (talvez FBO 3) ou a exposição.
- Tone-map é config POR FASE: `SsConstTonemapIsEnable` lê config+0x34, `SsConstBloomIsEnable`
  config+0x40 (struct de 0x2b4 bytes por fase). `ChangeToneMapParam(float,float)`,
  `SsConstTonemapMidgray/Lwhite` (Reinhard).

## SUSPEITO PRINCIPAL ATUAL: TONE-MAP ADAPTATIVO (auto-exposição)
A teoria que melhor explica "gameplay estoura / pause correto": a **auto-exposição (ToneMapAdapte)**
mede o brilho da cena e adapta a exposição. Se a medição/adaptação está bugada no port (ex.: lê
luminância errada, ou o histórico de adaptação acumula), durante o gameplay a exposição vai pro
máximo -> tudo branco. No pause a adaptação para -> correto.
**TESTAR:** desligar a adaptação (forçar exposição fixa) — achar o gate do ToneMapAdapte ou
`SsGraphicsToneMapSetParam` e fixar. OU patchar a func de adaptação pra no-op.

## FERRAMENTAS PRONTAS (flags de debug no binário, gated)
- `SONIC_UNLOCK_ALL=1` — libera todas as fases (FUNCIONA, validado).
- `SONIC_WARP_STAGE=N` — TENTATIVA de warp (força sm_select_stage_id + confirm); **NÃO funcionou**
  com NENHUM confirm (testei FOX_A_MENU 0x8020 E decide FOX_A_GAME 0x20; IDs 0/3/8 ficam todos no
  `ep2_sng_worldmap`, started=0). CONCLUSÃO: Episode Metal é **modo separado** — não basta forçar
  o stage_id no world map do EP2. O load entra por outro caminho (provável `GmGameDatLoadInit`/
  troca de mode global). BECO: abandonar warp por enquanto; usuário navega manual com UNLOCK_ALL.
  Próxima ideia (se retomar warp): achar o global de "game mode"/"episode" e setar p/ Episode Metal
  ANTES de forçar o stage, ou hookar o stage-load direto.
- `SONIC_CLEARLOG` — FBO-STATS (draws/clears por FBO).
- `SONIC_FBCLEAR / FBO0CLEAR / NOLIGHTMASK / NOPOSTFX / NOGODRAY / FULLFX / FORCETONEMAP / THREADDRAW` — testes.
- Screenshot ao vivo: `touch /dev/shm/sonic_shot` -> /dev/shm/sonic_shot.raw (640x480 RGBA, flip V).
- Crash handler: grava crash.log (não usado — o bug não é crash).

## PRÓXIMOS PASSOS
1. **Testar a auto-exposição (ToneMapAdapte)** — desligar/fixar. Mais provável raiz.
2. **Reach-stage:** o warp falhou; precisa achar como entrar no Episode Metal (modo separado).
   Alternativa: hookar `GmGameDatLoadInit` ou o stage-load direto. OU o usuário navega manual
   (launcher com UNLOCK) e eu capturo/comparo com a imagem de referência da Electric Road.
3. Capturar GL trace de UM frame do cassino (binds/clears/draws/program/blend) pra ver o pass exato.
4. Comparar screenshot PAUSE (correto, salvo em scratchpad/bands.png) vs GAMEPLAY (estourado,
   casino.png/casino2.png) — a diferença É o efeito bugado.
