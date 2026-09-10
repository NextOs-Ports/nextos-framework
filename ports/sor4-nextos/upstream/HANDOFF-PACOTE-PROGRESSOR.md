# HANDOFF — Empacotar SOR4 no padrão PortMaster (progressor na tela + abrir o jogo)

## OBJETIVO (o que a próxima sessão TEM que entregar)
Um ZIP do port no **padrão PortMaster** que:
1. É lançado **PELO PortMaster/ES** (não por ssh na mão).
2. Na **1ª execução**, o `.sh` chama o **progressor** (binário que desenha NA TELA com %),
   que: extrai do APK (BYO-DATA) → patcha o SOR4.dll → **converte texturas ASTC→ETC1**
   (com escolha de escala + modo) → no fim apaga o APK (só após sucesso).
3. Nas próximas vezes, **abre o jogo** direto (sor4host) com gptokeyb.

⚠️ ERRO DA SESSÃO ANTERIOR (NÃO repetir): eu fiquei **bypassando o progressor** (rodando o
texconv direto via ssh) e tentando lançar o progressor por ssh cru. ISSO NÃO É O MODELO.
O modelo é: **`bash StreetsOfRage4.sh`** (lançado pelo PortMaster) → o .sh chama o progressor.
O progressor SÓ funciona/sobrevive quando o port é lançado pela sessão do PortMaster (que
suspende o ES e entrega o display). Por ssh cru o ES (Restart=always) volta e mata o progressor.

## O TEMPLATE CERTO (do Bully.sh de referência — COPIAR a estrutura)
O `.sh` DEVE:
- `source $controlfolder/control.txt` + `get_controls`  → define `$ESUDO`, `$GPTOKEYB`,
  `$directory`, `$sdl_controllerconfig`, `pm_finish`, `$CUR_TTY`.
- `GAMEDIR="/$directory/ports/sor4"` (NÃO /storage/roms/sor4-test).
- 1ª execução (gate por marcador, ex: `[ ! -f "$GAMEDIR/.setup_done" ]`):
  ```
  "$GAMEDIR/tools/progressor" --log "$GAMEDIR/tools/setup.log" \
     --font "$GAMEDIR/tools/FiraCode-Regular.ttf" --title "Streets of Rage 4" \
     "$GAMEDIR/tools/sor4_setup.src"
  ```
- Ambiente: `export LD_LIBRARY_PATH="$GAMEDIR/host_pkg/libs:/usr/lib:$GAMEDIR"`.
  **NUNCA setar SDL_VIDEODRIVER nem SDL_AUDIODRIVER** (auto-detect SEMPRE — regra do projeto).
  Pode usar `SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1` + `SDL_VIDEO_FULLSCREEN_DESKTOP=1` (Bully usa).
- gptokeyb via `$GPTOKEYB "sor4host" -c "$GAMEDIR/sor4.gptk" &` (ou fallback `gptokeyb -1`).
- `./host_pkg/sor4host` (o jogo).
- No fim: matar gptokeyb + `printf "\033c" >> $CUR_TTY` + `pm_finish`.
- NÃO chamar `systemctl stop emustation` na mão — o PortMaster cuida da sessão.
- TESTAR colocando em `/roms/ports/sor4/` (ou onde o $directory aponta) e abrindo PELO MENU do
  ES/PortMaster — NÃO por ssh (ssh cru = ES volta e mata o progressor).

## O QUE JÁ ESTÁ PRONTO E FUNCIONA (não refazer)
- **Conversor ETC1 `texconv`** (`port/tools/texconv/`): lê XNB (LZ4 via Lz4DecoderStream do
  MonoGame, copiado) → decodifica ASTC (P/Invoke `sor4_astc_decode`) → aplica downscale (escala
  1/2/3, box-filter) → opaca=ETC1 (P/Invoke `sor4_etc1_encode`)/alpha=RGBA8 → reescreve o XNB
  comprimido em LZ4 (Encode32HC) no LUGAR. Tem auto-teste LZ4 (round-trip) no início. Saída só
  `%` (sem barra). Multi-thread; `SOR4_CONV_THREADS=N` controla o modo (low-mem=poucos threads).
  VALIDADO: bash-direto escala 3 / 2 threads converteu ~100% sem morrer, 2.2G→1.9G, 600MB+ livres.
  Build: `cd port/tools/texconv && dotnet publish -c Release -r linux-arm64 --self-contained -o out`.
- **Encoder ETC1** em `libsor4astc.so` (`sor4_etc1_encode`, reusado do Dysmantle etc1_encode.c).
- **`sor4host --run-dll <dll> <args>`** (host Program.cs): roda QUALQUER ferramenta .NET pelo
  runtime JÁ embutido no host → **sem dotnet de sistema, qualquer device**. VALIDADO (verstub rodou).
- **Ferramentas de patch Cecil** buildadas em `port/tools/{patchgam,noopm,skipvideo,verstub,rettrue}/bin/`
  + Mono.Cecil.dll. Rodam via `sor4host --run-dll noopm.dll SOR4.dll AndroidServices.* ...` etc.
- **Reader** (`build/MonoGame/.../Texture2DReader.cs`) lê RgbEtc1 XNB direto (caminho ASTC nem
  dispara qdo já é ETC1) → jogo lê os XNB pré-convertidos sem conversão runtime.
- **Progressor** (binário 335KB) + FiraCode font: vêm do `DYSMANTLE v3.zip`
  (tools/progressor, tools/FiraCode-Regular.ttf). `pbar`/`msgbox` funcionam; **`settitle` CRASHA**
  (não usar — usar --title). `confirm` NÃO testado no nosso progressor (Salt usa um progressor de
  137KB onde confirm funciona — se precisar de confirm, testar ou usar o do Salt).
- Backup do jogo antigo no device: pasta renomeada `/storage/roms/sor4-test.bak` (originais ASTC).
- Áudio nativo (música+SFX) e fix das cercas brancas: JÁ resolvidos em sessões anteriores
  (ver project_sor4_streets_of_rage4_mali450 na memória).

## A PIPELINE DE SETUP (o que o `sor4_setup.src` deve fazer, via run-dll)
Base/ordem (do SOR4.dll cru do APK):
1. patchgam SOR4.dll SOR4Bridge.dll  (get_AssetManager→bridge)
2. noopm SOR4.dll  AndroidServices.* save_save_game save_config
3. skipvideo SOR4.dll  (reset_game StartGameVideoScreen→TitleScreen)
4. verstub SOR4.dll  (get_version_identifier→"1.4.5")
5. noopm SOR4.dll  EOSManager.PollMessage MoreGamesNotificationUpdate
6. rettrue SOR4.dll  CommonLib.platform::load_save_and_config_is_finished
7. noopm SOR4.dll  platform.video_exists   (pula intro de fase)
8. texconv  gameassets  <ESCALA>   (ASTC→ETC1, downscale)
Cada passo = `"$PKG/sor4host" --run-dll "$PKG/<tool>.dll" <args>`. LD_LIBRARY_PATH com host_pkg/libs.

## EXTRAÇÃO DO APK (BYO-DATA) — A FAZER (não foi feito ainda)
O SOR4 é .NET-for-Android. Falta descobrir/implementar a extração do APK:
- gameassets (assets/), libs (lib/arm64-v8a/*.so: libWwise etc), e o **SOR4.dll** (fica no
  assembly store do .NET-Android — pode precisar de tratamento especial, NÃO é só unzip).
- Estudar como o SOR4.dll foi obtido na dev (havia /tmp/SOR4.device.dll). Os 19 arquivos `\x00IAP`
  na raiz do gameassets são minoria (não são as texturas; texturas = 25224 XNB padrão).
- ⚠️ DECISÃO DO PORTER: apagar o APK **só após SUCESSO TOTAL** (extração+patch+conversão+jogo
  abre). Se falhar, MANTER o APK (opção 1).

## UX DO PROGRESSOR (decidido com o porter, inspirado no Salt) — A FAZER
DUAS escolhas separadas (Salt usa `confirm`):
1. **Downscale (escala 1 / 2 / 3)** — tamanho da textura. Recomendado **3.0 p/ 1GB**.
2. **Modo de conversão**: "RÁPIDA (memória alta = muitos threads)" vs "LOW MEMORY (lenta = poucos
   threads, `SOR4_CONV_THREADS` baixo)". Igual o "HQ?" do Salt (que usa ASTC_QUALITY).
Implementar via `confirm` no .src (testar o confirm no progressor primeiro; se crashar, usar o
progressor do Salt 137KB ou um menu por etapas). A escala escolhida vai pro `texconv <dir> <escala>`
e o modo define `SOR4_CONV_THREADS`.

## ARMADILHAS (aprendidas na marra)
- **Lançar PELO PortMaster**, não por ssh cru (ssh: ES Restart=always volta em ~5min e MATA o
  progressor → conversão morre junto. Foi a "morte aos 282s/45%").
- **SDL video E áudio = AUTO-DETECT** (nunca forçar driver; forçar 'mali' deixava tela preta).
- **p3 é vfat** (limite 4GB/arquivo) — não fazer tar gigante; backup = renomear pasta.
- **settitle crasha** o progressor (JSON) — usar --title.
- texconv usa pouca RAM (~121MB); o "low memory" é p/ a RAM do JOGO (ETC1 resolve) E
  p/ o pico DURANTE a conversão (menos threads).
- Limpar no fim: logs de debug; coredumps do DOTNET_DbgEnableMiniDump (o run_diag.sh de debug
  gera ~80MB por crash — o launcher de produção NÃO deve ter isso).

## ESTADO DO DEVICE (<device-ip>)
- `/storage/roms/sor4` = pacote em montagem (host_pkg, gameassets convertido escala-3, tools/).
- `/storage/roms/sor4-test.bak` = backup (originais ASTC).
- Conversão escala-3 bash-direto rodou até ~100% (validar o jogo abrindo com `bash sor4/StreetsOfRage4.sh`).
- Próximo passo concreto: (a) validar o JOGO abre lindo com ETC1; (b) reescrever o .sh no template
  PortMaster (control.txt/get_controls); (c) testar lançando PELO ES; (d) extração do APK; (e) UX confirm.
