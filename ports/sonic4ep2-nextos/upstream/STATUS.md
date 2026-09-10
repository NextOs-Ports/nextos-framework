# Sonic 4 EP2 - STATUS

## 🏆 2026-07-02 — TESTERS: TODOS os devices OK (relato do usuario)
muOS OK, ROCKNIX (Mesa/Panfrost) OK, R36S OK — "tudo ok". As apostas de
compatibilidade sem ter o device na mao funcionaram: audio adaptativo (scan de
driver SDL, caso muOS/pipewire), rejeicao de contexto desktop-GL (Mesa/Panfrost),
resolucao nativa automatica (paineis 640x480), CLEARALL default. 

## Sessao 2026-07-02 — v5 ARM64-ONLY empacotado (bake estilo Bully) + validado nos devices
Zip: `Sonic4EP2 PortMaster v5 arm64.zip` (Area de trabalho, md5 13380bb1). Commits ate 65c6950.
- **v5 = so arm64** (pedido do usuario): sonic4.arm64 + launcher unico + extrator do .apkm.
- **BAKE estilo Bully v11** (setup_splash.c): o proprio binario desenha (SDL renderer, fonte
  5x7) titulo "SONIC 4 EP2 V5", mensagem, barra de MB e **NEXTOS no canto inferior direito**.
  Gate SONIC_SETUPSPLASH + protocolo /tmp/sonic_setup.txt + stop-file. RETRY de 15s na
  criacao da janela (DRM demora a soltar apos parar a ES).
- **Extrator (tools/sonic4ep2_extract.sh)**: fontes = .apkm (ou splits); extrai
  split_config.arm64_v8a.apk->libfox + split_packs.apk->data.obb com barra por poll de
  tamanho; APAGA apkm/temporarios no fim. Erro = barra vermelha + mensagem.
- **Fluxo validado pelo usuario**: R36S cabeado ("teste 1 ok"), Mali-450 .79 ("deu certo"),
  X5M ("agora sim") — bake visivel + extracao + jogo + apkm some. R36S WiFi pronto (apkm la).
- 🔑 **LICOES DO X5M (3 telas pretas ate acertar, todas com raiz achada):**
  1. ES do X5M lanca ports por **gamelist.xml** (nao ports_scripts como o .79) — entrada add.
  2. **cgroup suicida**: launcher roda DENTRO do cgroup da essway (runemu); `systemctl stop
     essway` mata o proprio launcher+extracao. FIX = first-run inteiro delegado a um
     **systemd-run transiente** (ExecStartPre para a essway; wrapper roda bake+extracao+jogo
     e restaura a ES no fim) — mesmo padrao do pm_platform_helper do mod_NextOS.
  3. **LD_LIBRARY_PATH tem que ser exportado ANTES do wrapper do first-run** — senao o
     splash/jogo nao acham a libmpg123 bundled (libs.aarch64) e morrem no loader.
- **pm_platform_helper** (mod_NextOS do X5M): e ASSIM que Bully abre la — service com
  SDL_VIDEODRIVER=kmsdrm + stop essway (libera o DRM) + gptokeyb no cgroup + ES de volta.
  Nosso launcher chama o helper igual o Bully; defaults criticos foram pro BINARIO
  (LPK=data/data.obb se existir; SONIC_CLEARALL=1 arm64) porque systemd-run nao repassa envs.
- **ports_scripts (.79)**: a ES do NextOS antigo lanca por /storage/roms/ports_scripts/*.sh —
  launcher v5 copiado la tambem.
- 📌 licao de arquitetura (estilo Bully, p/ adotar num v5.x): Bully nao LINKA libs do device
  (dlopen em runtime) — nunca morre no loader por lib faltando. Nosso binario linka
  SDL2/mpg123/vorbisfile/GLESv2 (bundle cobre, mas dlopen seria mais robusto).

## Sessao 2026-07-01 (noite 2) — MULTI-DEVICE: SFX arm64 validado + X5M COMPLETO + R36S parcial
- ✅ **Mali-450 .79 (NextOS): fix SFX arm64 VALIDADO** — tsRead alloc/buf resolvem (fallback
  ...Pm), `LPK read alloc ok` nos OGGs, `play sfx key="Ok" handle=1` tocando. Binario c20d52f.
- 🏆 **S905X5M .103 (NextOS aarch64, Mali-G310 Valhall, 3.7GB RAM): ARM64 COMPLETO VALIDADO** —
  titulo (sprites+logo) -> menu (musica+Ok sfx) -> GAMEPLAY Sylvania Castle (Sonic+Tails+HUD)
  em **1920x1080, GL ES 3.2**, musica+SFX pelo LPK. Screenshots x5m_title.png/x5m_2.png.
  🔑 **Requisitos X5M descobertos (p/ o launcher/pacote arm64):**
  (1) o SDL2 do X5M e **wayland-only** (sem kmsdrm/fbdev/x11) e a ES (essway) roda KMSDRM
  direto SEM sessao wayland -> port precisa subir **weston** (existe em /usr/bin) e rodar
  dentro dele: `XDG_RUNTIME_DIR=/run/user/0 weston &` + jogo (nosso binario acha o socket
  sozinho). Padrao westonpack/SWD2.
  (2) **libmpg123.so.0 NAO existe no sistema** -> bundlar no gamedir + LD_LIBRARY_PATH
  (a copia aarch64 de 28/06 no gamedir funciona).
  Setup deployado: sonic4.arm64 + libfox v3 + data/data.obb v3 (md5 4c7745b1) + sfx_map 730
  + launcher Sonic4EP2-arm64.sh (launcher ainda NAO sobe weston — TODO empacotar).
- ✅ **R36S cabeado .170.2 (ArchR): fluxo de INSTALACAO 100%** — zip v4.5 + APK/cache BYO ->
  extracao first-run perfeita (libfox 8.8MB + OBB 594MB). ⚠️ No LAUNCH o device CAIU da rede
  (sem rota) — suspeita: parei a `essway` (ES+sway = o compositor wayland que o SDL do jogo
  usa la) antes de lancar; sem display + algo derrubou o usb-eth. Instalacao antiga preservada
  em sonic4ep2.prev_s4test_20260701. LICAO: **no ArchR NAO parar a essway** — lancar via
  sessao (ES/PortMaster) ou com o sway vivo. PENDENTE: revalidar quando o device voltar.
- ⏳ **R36S WiFi (ArkOS)**: offline a sessao toda (desligado). PENDENTE.

## Sessao 2026-07-01 (noite) — ARM64 v3: os 2 bugs de render RESOLVIDOS (validado pelo usuario)
Commit `54b7322`. Binario `sonic4.arm64` deployado no .79. Usuario validou: menu abre,
fase abre/retorna sem tela preta, **Electric Road "ficou perfeita"** (fundo preto, luzes finas,
sem duplicacao/rastro), velocidade e som OK pelo launcher.

### BUG 2 (tela preta ao sair/reiniciar fase) — RAIZ + FIX (RELSAFE64)
- Raiz: release de textura DIFERIDO (cmd 2 do ring amDraw). Payload copiado por valor no
  enqueue mas contem ponteiro P -> lista {count,handles} na CENA, que o GameProcess libera
  na MESMA passada -> no exec P esta STALE. No armv7 dava a tempestade de SIGSEGV mascarada
  pelo DEMOGUARD (DecRef em slot errado); no arm64 o RELSAFE antigo rejeitava TUDO
  (sane_engine_ptr com teto 32-bit rejeitava ponteiro >4GB) -> leak -> tabela de 1024 slots
  do amTexMgr esgotava -> reload da fase sem textura = TELA PRETA.
- Fix: hook do `amDrawRegistCommand1` (replica fiel do ring v3; ctx/tabela derivados dos
  BYTES da lib via decode ADRP+LDR/ADD + verificacao table[0]=Nop/table[2]=ReleaseTexture).
  cmd 2 -> SNAPSHOT {count,handles} feito NO ENQUEUE (P valido) e o payload passa a apontar
  pro snapshot; DecRef continua NO EXEC (timing original). ⚠️ DecRef ADIANTADO (1a tentativa)
  quebrava os SPRITES do titulo (texturas do frame corrente liberadas cedo) — o snapshot e
  a forma certa. P nao-rastreado no exec = stale -> pula SEMPRE (nunca DecRef em lista morta).

### Titulo nao ia pro MENU no tap (regressao aparente) — RAIZ + FIX
- Raiz: no v3 a flag de continue (lida por CStateWaitViewPausing::Next, slot re-apontado
  pelo init p/ struct malloc'd) nasce LIXO — no Android o Java a inicializa via
  SetContinueFlag. Next() so trata 1/2; lixo = preso no titulo p/ sempre (music restart).
- Fix: sanitizador por frame (slot derivado das instrucoes do isContinueStart; valor fora
  de 0..2 -> 0) + continue-drive v2 (SetContinueStart(2)) volta a funcionar.
- Gates v3 extras (consent refatorado): Legal::isCompleteAllState->1, Age::haveDoneAgeGate->1,
  haveUserConsent->1, NEED_TO_SHOW_POPUP_IMPROVE->0, resume do PAUSE_INTRO_VIDEO (bit 0x80).

### BUG 1 (Electric Road estourada/duplicada) — resolvido pela combinacao
- CLEARALL blindado no arm64 (scissor OFF + colormask/depthmask FULL durante o clear,
  estado restaurado) + RELSAFE64 (texmgr saudavel). FBO-STATS do v3 na fase: FBO 2 =
  cena (75 draws/frame, 1 clear/frame do engine), FBO 0 nunca limpo pelo engine (CLEARALL
  cobre), FBO 6/7/8 ciclicos. Validado com screenshots no gameplay real (pulos/molas/boost).

### Infra de debug nova (reusavel)
- `SONIC_INPUTFILE=/dev/shm/sonic_pad` — mask FOX dinamico via arquivo (dirigir por ssh).
- `SONIC_INPUTSEQ="F:MASK:LEN,..."` — roteiro deterministico de input.
- `SONIC_STATELOG=1` — gates do fluxo titulo->menu por segundo (setup/enable/upshell/continue).
- ⚠️ ao testar via ssh: PARAR a ES antes (senao compoe por cima do jogo e contamina
  screenshot/fb) e NAO usar SONIC_FRAME_SLEEP_US em teste de jogabilidade (deixa lento).
- Screenshots de referencia: scratchpad da sessao (h1_title=titulo OK, p6/p7=Electric Road OK).

### 🔊 SFX no arm64: eram TODOS mudos — RAIZ achada e FIX pronto (falta deploy+validar)
- Diagnostico do estudo pos-sessao: no run arm64 instrumentado ha **0 linhas "play sfx" e
  1416 "LPK miss"** — ou seja, TODO SFX mudo no arm64 (a musica toca por outro caminho,
  MP3/mpg123, por isso "parece ok"). O usuario ouvia so musica.
- **RAIZ: mangling v3.** `sonic_audio.c` resolve `_Z10tsReadFilePKcPPvPj`/`...PhPj`
  (v2: size = `unsigned int*`); o v3 arm64 exporta `...PPvPm`/`...PhPm` (size =
  `unsigned long*`, 8 bytes) -> alloc=NULL, buf=NULL -> read_lpk_file nunca le nada.
- **FIX (sonic_audio.c):** fallback pros nomes v3 + tipos de size viram `unsigned long`
  (no armv7 long=32-bit, identico ao antigo; no arm64 evita corromper pilha com
  out-param de 8 bytes). Buildado; **PENDENTE deploy+teste** (usuario estava jogando).
- Verificacao do OBB v3 (parser proprio do LPK): indice = hash(nome normalizado)*31<<7,
  2815 slots @0x30, file table 16B @0x2c40; `SOUND/SFX/S4EP2FX_113a_S2_2235_44R.OGG`
  ESTA indexado e aponta p/ OggS valido -> OBB perfeito, era so o reader.
  🔑 normalizador do LPK so UPPERCASEIA 'b'..'y' ('a' e 'z' ficam minusculos — por isso
  os nomes "CaSINO"/"113a"). Hash: h=h*31+c(signed), key=(h<<7)&0xffffff80, strcmp so
  em colisao (low 7 bits = idx na tabela de nomes duplicados).
- Varredura geral de simbolos: unicos usados ausentes do v3 sem cobertura = os 2
  tsReadFile (corrigidos); resto coberto pela tabela de alias/gates novos.

### 🎬 ESTUDO cutscenes (a implementar, viavel)
- Assets: `split_packs.apk` tem **cutscene1..7, 9-1, 9-2.mp4 (~135MB, MPEG4-ASP 800x450
  30fps + AAC — leve p/ decode SW)** + stfrol_tryagain_*.mp4 (staffroll por idioma).
- Engine: `gm::movie::clMovie` — requestMovieBeforeGameStart/AfterClearDemo escolhem o
  id; hoje o port PULA tudo (videoIsPlaying->0, willPlayMovieBeforeGameStart->0,
  isEnd->1). Java receberia o id (`cutsceneSetPlayId`) e tocaria o mp4.
- **Device TEM mpv/ffplay/ffmpeg nativos** (EmuELEC usa p/ preview no ES).
- Plano: modo `SONIC_MOVIES=1` — nao patchar os 3 gates; interceptar o pedido de movie
  (jni_shim/log do metodo Java com o id), pausar o loop, `mpv --fs cutsceneN.mp4`
  (SELECT/START = skip, kill mpv), sinalizar isEnd e resumir. MP4s = BYO via extract
  do apkm (estender tools/sonic4ep2_extract.src). Risco baixo: jogo pausado nao swapa GL.

## Sessao 2026-06-30 (noite) — v4.2: corrige REGRESSAO (fase nao abria) + sons + release limpa
Tudo VALIDADO no R36S/ArkOS (.104) pelo usuario. 4 commits no master (sem co-autor):
- **FASE NAO ABRIA = regressao pos-v4.0 (commit 9a116e1).** O RELSAFE de `_amDrawReleaseTexture`
  passou a validar a registlist com checagem ESTRITA por arena `[heap_base,heap_end)` do so_load.
  Mas as REGISTLIST da engine sao **malloc'd em 0xab.../0xac... — FORA do arena** -> TODA lista
  valida era PULADA -> nenhuma textura liberada -> a free-list do amTexMgr ESGOTAVA -> 
  `amTexMgrCreateTexId` fazia `str r0,[r3]` com r3=head=NULL -> SIGSEGV (libfox+0x2059ec) -> a
  fase nem abria. FIX: `sane_engine_ptr` volta a validade FROUXA (`0x10000<p<0xfffff000`, alinhado),
  aceita malloc (= v4.0 e o 1o fix de saida a03b5fe que funcionavam). O discriminador do caso
  stale (saida) e o `count` fora de [1,65536], NAO o range. Stride-8 fiel ao nativo (confirmado
  por disasm: itera count elems de 8 bytes, DecRef no handle). ⚠️LICAO: NAO usar o arena do
  so_load p/ validar ponteiros da engine — ela aloca via malloc do glibc, fora do arena.
- **"O fix de audio quebrou tudo" foi DIAGNOSTICO ERRADO.** Quem travava era o texmgr acima.
  O `my_MediaPlayerisPlaying` cirurgico (estado real so p/ canais jingle "_jin_") foi REABILITADO
  (default ON, commit 0cd7286). Som do 1up/caixa-de-vida/aneis-suficientes VOLTOU. SONIC_NO_JINGLE1UP=1 reverte.
- **Sons da special stage (SpStage01 espinhos / SpStage06 arco) mudos = `sfx_map.tsv` DEPLOYADO
  VELHO (644 linhas).** O repo/package ja tinham o mapa completo (730 linhas, SpStage01-08 do
  commit 9ba8f16); so a copia no DEVICE estava defasada — deploys de binario nunca atualizavam o
  mapa. ⚠️LICAO (igual HLM2): ao deployar, copiar TAMBEM o sfx_map.tsv, nao so o binario.
- **Unlock all fases (commit b8a4764):** launcher exporta SONIC_UNLOCK_ALL=1 se existir o marcador
  `unlock_all` no gamedir (`touch sonic4ep2/unlock_all`). Release NAO leva o marcador.
- **v4.2 EMPACOTADA** (Area de trabalho, md5 ec958231): binario compat fe4bcae0 (glibc2.27) +
  sfx_map 730 linhas + README enxuto padrao PortMaster + launcher sem debug de audio/video
  (SONIC_AUDIOLOG era opt-in, removido do launcher) + tools/audio_diag.sh removido. Commit 48125b3.
- 🔌 R36S desta sessao = **ArkOS .104** (IP muda no DHCP; era .150/.104), user ark/ark. **RAM REAL
  do device = 768MB** (chip topa em 0x30000000; NAO e 1GB — clone mente spec), Mali = memoria
  unificada (sem VRAM dedicada), CommitLimit ~320MB sem swap -> explica os OOM do heap de 256MB.

## Sessao 2026-06-30 (tarde) — 2 fixes: fundo do cassino + crash ao sair da fase

### 1. FIX fundo estourado/duplicado da Electric Road (Episode Metal) — VALIDADO (usuario: "ficou perfeita")
- **Sintoma:** so na Electric Road (Casino Street c/ Metal Sonic), no GAMEPLAY o fundo (feixes
  de holofote + reflexo) estourava pra BRANCO e os objetos (moedas/molas/Sonic no pulo) DUPLICAVAM
  no fundo. No PAUSE ficava correto (preto, feixes finos). So essa fase; as outras OK.
- **RAIZ:** essa fase usa um FBO off-screen de fundo que o engine espera LIMPO a cada frame. No
  nosso port esse FBO nao era limpo no gameplay -> acumulava (feixes aditivos -> branco; objetos
  -> copias). Pause congela a logica -> para de acumular -> correto.
- **FIX `SONIC_CLEARALL`** (imports.c, em my_glBindFramebuffer): limpa cada FBO 1x/frame no 1o
  bind, preservando passes dentro do mesmo frame (binds seguintes nao re-limpam) e a cor de clear
  do engine (salva/restaura via glGetFloatv). **Validado no R36S: fase perfeita.**
- ⚠️ CLEARALL e GLOBAL. As outras fases ja eram OK; FALTA confirmar que com CLEARALL ligado elas
  NAO regridem. Se alguma quebrar -> estreitar o clear so pro FBO do cassino. Hipoteses
  descartadas antes (NAO eram a raiz): FBO0CLEAR, NOLIGHTMASK, NOPOSTFX, NOGODRAY, FULLFX,
  FREEZETONEMAP(auto-exposicao). Ver STUDY_CASSINO_BUG.md.

### 2. FIX crash ao SAIR da fase ("Return to Stage Select" fechava o jogo) — PENDENTE VALIDACAO
- **Sintoma:** START -> Return to Stage Select fechava o jogo (em todas as fases). As vezes SIGSEGV,
  as vezes OOM-kill no R36S.
- **RAIZ (RE + registradores do crash):** `_amDrawReleaseTexture` libera a lista de texturas da
  cena DIFERIDO (GameProcess enfileira via amDrawRegistCommand, DrawFrame executa via
  amDrawExecRegist). Na saida o GameProcess enfileira o release E libera os dados da cena na MESMA
  passada (loop single-thread GameProcess->DrawFrame) -> quando DrawFrame drena a fila, o ponteiro
  da lista (node+4 = P = {count,array}) ja aponta pra memoria LIBERADA -> count lixo (0xfbfd18f0)
  -> indexa array fora -> SIGSEGV. Crash: PC=_amDrawReleaseTexture+0x38, r5(count)=0xfbfd18f0.
- **FIX `RELSAFE`** (main.c my_amDrawReleaseTexture via patch_arm_jump, DEFAULT ON, SONIC_NO_RELSAFE
  desliga): valida count(0<n<=65536)+ponteiros antes de iterar. Lista valida = libera igual ao
  original (sem vazar); lista corrompida = pula com seguranca (dados ja liberados pela destruicao
  da cena). Crash handler agora loga r0-r12. Builda/instala/boota OK no R36S.
- **PENDENTE:** usuario testar entrar na fase + Return to Stage Select (esperado: volta pro menu
  sem fechar). Logs [RELSAFE] mostram quantas listas foram puladas (se 0 puladas e ainda fecha ->
  outra causa; se pula e volta OK -> resolvido).

## Sessao 2026-06-30 — v4.0 FULLFX default + fix dos RASTROS (Metal Sonic)
- **FULLFX agora e DEFAULT (commit a3d1fda):** bloom + sombras LIGADOS. LOWFX virou opt-in
  (`SONIC_LOWFX=1`); o launcher NAO seta -> LOWFX nunca ativa. Validado no R36S (title limpo, 0 crash).
- **RASTROS do Sonic ao pular no estagio do Metal Sonic = causado pelo LOWFX.** Mecanismo
  confirmado por RE: o present NAO limpa o framebuffer por frame (glClear so sob SONIC_TESTCLEAR)
  e e double-buffered; o jogo conta com o pass full-screen do BLOOM pra reescrever a tela. Com
  LOWFX (bloom off), o buffer antigo (Sonic 2 frames atras) nao e sobrescrito -> rastros. FULLFX
  (bloom on) cobre a tela -> rastros somem. Log do tester confirmou LOWFX ativo durante o bug.
  (Robustez opcional p/ modo adaptativo futuro: glClear por frame no inicio do DrawFrame mataria
  os rastros em qualquer nivel de FX.)
- **v4.0 REEMPACOTADO** (Area de trabalho, md5 7b13215b): binario FULLFX (md5 38954661) +
  version.txt=4.0 + cover novo. Tudo resolvido: Y/mapa, volume, L3/R3, rastros, visual completo.
- Pendencias menores OPCIONAIS: confirmar score-preso Oil Desert (pode ter sido resolvido pelo
  fix de mapeamento); 3 SFX mudos no Metal Sonic (MS_Homing/Kama/Boss2_03 — nao existem na
  AudioDataTbl do jogo, so chute-map possivel).

## Sessao 2026-06-29 (noite) — RELEASE v4.0 + fixes de raiz validados no R36S
- **Y / mapa de botoes (RAIZ, commit 1b1075e):** o input real do engine = `gmPadValue`
  montado por `onKeyEvnet(keycode)` via tabela `s_remapKey` do jogo (decompilada com
  androguard no DEX). Nosso mapa de bits FOX estava ERRADO: `FOX_Y=0x100` era na verdade
  **L1** -> Y rolava o world map ("Y vira Left") e a funcao propria do Y (bit4=0x10) nunca
  disparava. Corrigido o mapa inteiro pros valores reais (Y=0x10, A=0x20, X=0x40, B=0x80,
  L1=0x100, L2=0x200, L3=0x400, R1=0x800, R2=0x1000, R3=0x2000, SELECT=0x4000, START=0x8000).
  VALIDADO no R36S por injecao deterministica (SONIC_TESTBIT) + screenshot: 0x10 nao rola o
  mapa, antes 0x100 rolava. Usuario confirmou "y funcionando".
- **Volume Knulli/ROCKNIX (commit 72f8fb5):** card-cru -> le system.cfg/batocera.conf + fallback
  `amixer Master`. Validado no R36S (sys volume -> 0.30).
- **L3/R3 (commit f978087):** nao colidem mais com pause/confirm.
- **RELEASE: `Sonic4EP2 PortMaster v4.0.zip`** (Area de trabalho, md5 2d9721e9, 3.8M): binario
  novo compat glibc2.27 (md5 470d7680) + cover novo (sonic_4ep2.png) + README v4.0. Layout
  PortMaster classico, sem lixo. Backup do binario original preservado no device.
- Bug ABERTO: score preso no Oil Desert Act3 (pode ter sido resolvido pelo fix de mapeamento;
  lead = GmGmkSlotFlush). Aguardando confirmacao do usuario.

## Sessao 2026-06-29 (FIX: pulo PAUSA no Special Stage)
- **Bug (reportado pelo luis, v3.3 nao corrigia):** no Special Stage o PULO (A) tambem pausa.
  Nos atos normais o pulo funciona. Causa-raiz por disasm do libfox.so:
  - `OUYAGetPauseKey()` (0x3bd904) = `mov r0,#0x8000; bx lr` -> retorna a mascara **0x8000**.
  - TODO checador de pausa (`gmMainStartDemoEndCheck`, `SsUserInputIsPause`, `CPauseMenu::play`,
    `GmPauseMenuGetResult`) faz: `OUYAGetPauseKey()|0x4000` -> testa `pad & 0xC000`.
  - 0x8000 e o MESMO bit do confirm de menu (`FOX_A_MENU=0x8020`, AoPadSomeoneStand). Logo,
    confirm-de-menu e pausa sao indistinguiveis: A com 0x8000 SEMPRE pausa um estado pausavel.
  - Por que so no special stage: `sonic_game_started` e heuristica de log. Atos normais carregam
    por `mapfar` e logam `--- GmGameDatLoadExit ---` -> started=1 -> A=FOX_A_GAME(0x20), sem pausa.
    Special stage carrega por **CSSLoadingTask/GmSpStage_Start** (player GmPlayerSpStage_*, dados
    SpStage01..08), caminho SEPARADO que NAO loga GmGameDatLoadExit -> started fica 0 (vindo de
    "Create World Map") -> A=FOX_A_MENU(0x8020) -> 0x8000 -> pad&0xC000 -> PAUSA no pulo.
- **Fix (imports.c, `sonic_update_gameplay_state_from_log`):** `strcasestr(msg,"spstage")` ->
  sonic_game_started=1. Casa os loads `--- Load Start <...SpStage0X...> ---` (mesmo logger foxLog
  ja confirmado em runtime) + "SPSTAGE BRANCH"/"SpStage Loading"; NAO casa a UI "Special Stage"
  (tem espaco). Acesso ao special stage em EP2 = Red Star Ring (hasRedStarRing/getEmeraldIndex).
- **Estado:** build native (208804, md5 1d8dab51) deployado em .79 dev (`/storage/roms/ports/sonic4`)
  E package (`/storage/roms/ports/sonic4ep2`). Regressao OK: boota 1280x720, chega no gameplay,
  60fps, 0 crash, ZERO falso-positivo "special stage:" no boot/menu/ato normal. Backups na .79:
  sonic4.bak_prespstage. **FALTA: confirmacao on-screen (entrar num special stage via RSR e pular)
  — nao automatizei (RSR + navegacao no world map). Compat glibc2.27 p/ release: rebuildar apos OK.**

## Sessao 2026-06-28 (R36S/ROCKNIX + perf + special-stage audio)
- **Special stage audio FIX (confirmado pelo NextOS):** cues SpStage01-08 (speed/spike/entrada)
  estavam unmapped (so SpStage04 existia) -> mudos; anel/BGM tocavam. Mapeados via AudioDataTbl
  do DEX (dexlib2) com a CAIXA correta do OBB (V maiusculo). +Env01. sfx_map.tsv (runtime). Commit 9ba8f16.
- **Performance:** GPU nao era o gargalo (titulo/gameplay carregado = 60fps); lag real = stalls de
  I/O (streaming do SD, multi-seg) + memoria. Levers aplicados: SONIC_LOWFX (bloom off, ~+15% GPU,
  sem perda visivel; commit 0e5260c) + swap 512MB + SD readahead 4096 (servico systemd persistente
  no ROCKNIX). Bench zone1: avg 27->35 fps (~+30%). Downscale interno NAO feito (mexe na nitidez).
- **R36S agora roda ROCKNIX** (Arch-R panfrost travava o boot; ROCKNIX painel4 via overlay mipi-panel.dtbo
  = tela ok com libmali; panfrost descartado nesse painel). Acesso: root@169.254.170.2 senha rocknix
  (regenera no boot). Build aarch64 (X5M 64-bit puro) = WIP (crash pos-init_array).
- Release: Sonic4EP2 PortMaster v3.2.zip (port.json v5, bin LOWFX e59671d6, audio fix). BYO-data.
- ⚠️ NUNCA rodar harness com `rm foxsave` (apaguei save do NextOS em bench; scripts removidos).

# Sonic 4 EP2 - STATUS (PortMaster/R36S/Mali-450, 2026-06-27)

## Estado atual
O port passa pelo fluxo principal no device `.79`:

- logo/titulo/menu aparecem;
- Start entra no jogo;
- gameplay renderiza com fundo, HUD, Sonic/Tails e cenario;
- BGM funciona no titulo, menu e primeira fase;
- SFX comuns funcionam pelo caminho nativo `AudioHelper` -> SDL audio;
- audio aprovado pela validacao NextOS: menu, gameplay, pulo, mola e sons comuns fluem sem engasgo;
- performance do gameplay corrigida: depois de remover o sleep fixo do loop, gameplay fica em 60 FPS estavel;
- Start/Pause dentro do gameplay abre corretamente e foi aprovado pela validacao NextOS;
- `SELECT+START` foi corrigido no `main.c`, testado no device e aprovado pela validacao NextOS;
- save/continue voltou para o caminho nativo: com save bom, Start nao reinicia Act 1 direto e segue para o
  fluxo de mapa/continue esperado;
- save real validado: foi concluida a primeira fase, o jogo foi fechado/reaberto e apareceu `Continue`.
- fix aplicado para transicoes longas/boss: `sonic_game_started` agora tambem reconhece
  `GmGameDatLoadExit`, `Gimmick set camera scale` e `GmPlySeq`, evitando que A/B sejam tratados
  como entrada de menu depois de carregar fase por continue/boss;
- log normal foi reduzido: `ALOG`, `[frame]`, `[PERF]` e `[MEMCPY-NULL]` ficam desativados por
  padrao e voltam apenas com flags de diagnostico (`SONIC_VERBOSE_LOG`, `SONIC_ALOG`,
  `SONIC_FRAMELOG`, `SONIC_PERFLOG`, `SONIC_MEMCPYLOG`).

Device principal Mali-450 usado: `<IP-DO-DEVICE>`.
Device R36S usado: `169.254.170.2`.
Diretorio de desenvolvimento/testes antigos no device: `/storage/roms/ports/sonic4`.
Diretorio final do pacote no device: `/storage/roms/ports/sonic4ep2`.
Launcher final do pacote: `Sonic4EP2.sh` na raiz do zip, para extrair direto em `roms/ports`.

## Pacote PortMaster v1
Pacote pronto no desktop local:

```sh
Sonic4EP2 PortMaster v1.zip
```

Conteudo do zip:

- `Sonic4EP2.sh`;
- `sonic4ep2/sonic4` binario armhf compat;
- `sonic4ep2/sonic4.gptk`;
- `sonic4ep2/port.json`;
- `sonic4ep2/gameinfo.xml`;
- `sonic4ep2/cover.png` (cover nova do Sonic/Tails);
- `sonic4ep2/screenshot.png`;
- `sonic4ep2/splash.png`;
- `sonic4ep2/sfx_map.tsv`;
- `sonic4ep2/tools/extract-sonic4-data.sh`;
- `sonic4ep2/tools/sonic4ep2_extract.src`;
- `sonic4ep2/tools/progressor`;
- `sonic4ep2/README.md`;
- `sonic4ep2/LICENSE.md`;
- `sonic4ep2/Sonic4ep2.f2f`.

O zip nao inclui `runtime/`, APK, OBB ou arquivos comerciais do jogo.
Zip final validado: md5 `29081438be1b3d3896ee9498c4af7407`, sha256
`68e5ecabb44ab1104a5d25a56c82fb2eabb2e6374d8ee8cc3ff46d5772bcf982`, tamanho `2.4M`.
O zip foi validado sem entradas `ports/` ou `ports_scripts/`.

Build compat:

```sh
cd ports/sonic4
SR=<toolchain-sysroot>
sudo -n docker run --rm --platform linux/amd64 -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_compat_gcc.sh
cp -f sonic4.compat.gcc package/ports/sonic4ep2/sonic4
cd package/ports
zip -r "<output-dir>/Sonic4EP2 PortMaster v1.zip" Sonic4EP2.sh sonic4ep2
```

Resultado do build Docker:

- `sonic4.compat.gcc` / `package/ports/sonic4ep2/sonic4`;
- tamanho: `165196` bytes;
- sha256 validado no pacote: `02d137428af9c63f2941ae0b94a97a3e8b14d5bdea4bb351ba0b6a94cf41f3ea`;
- maior simbolo glibc: `GLIBC_2.27`, abaixo do alvo glibc 2.30;
- sem dependencia `GLIBCXX`;
- dependencias dinamicas esperadas no device: `libSDL2-2.0.so.0`, `libmpg123.so.0`,
  `libvorbisfile.so.3`, `libGLESv2.so`, `libdl.so.2`, `libm.so.6`, `libpthread.so.0`,
  `libc.so.6`.

Starter limpo:

- mata qualquer `sonic4` antigo pelo alvo real de `/proc/*/exe` antes de iniciar;
- fica como `Sonic4EP2.sh` na raiz do zip, padrao PortMaster classico igual Bully;
- usa `GAMEDIR="/$directory/ports/sonic4ep2"`, sem hardcode de `/storage/roms` no launcher;
- extrai `lib/armeabi-v7a/libfox.so` do APK na primeira execucao;
- instala `data/main.22.com.sega.sonic4episode2.obb` a partir do cache ZIP ou OBB direto;
- usa `progressor`/`tools/sonic4ep2_extract.src` para janela de primeira extracao quando disponivel;
- usa `SONIC_DATADIR="$GAMEDIR"`;
- usa `SONIC_AUTOSTART=0`;
- usa `SONIC_NOFAKESOUND=1`;
- usa `SONIC_SWAPINT=1`;
- no R36S/Wayland detecta resolucao real via `wlr-randr` ou `/sys/class/drm` e exporta `SONIC_RES`;
- no Mali-450/fbdev nao exporta `SONIC_RES`, mantendo o caminho SDL/fbdev ja validado;
- nao usa autoplay, `AUTORIGHT`, `AUTOJUMP`, `INPUTLOG` ou flags de teste.

Validacao R36S (`169.254.170.2`):

- device usa `essway.service` ativo, `es-de.service` inativo;
- painel real confirmado: `/sys/class/drm/card0-DSI-1/modes = 640x480`;
- launcher novo detectou `SONIC_RES=640x480`;
- log confirmou `setScreenSize(640 x 480)` e `fox: init(w=640 h=480)`;
- teste pelo ES confirmou tela correta e audio OK;
- pacote final copiado para `/storage/roms/ports/Sonic4EP2_PortMaster_v1.zip`;
- zip final extraido em `/storage/roms/ports`, criando `/storage/roms/ports/Sonic4EP2.sh`
  e `/storage/roms/ports/sonic4ep2/`;
- estado deixado para primeira execucao: `lib/` e `data/` removidos, `foxsave_0.dat`
  removido, APK e OBB na raiz de `sonic4ep2/`;
- backup preservado em `/storage/roms/ports/sonic4ep2/_backup_input/`;
- nenhum processo `sonic4` rodando ao final.

Validacao Mali-450 (`<IP-DO-DEVICE>`):

- instalacao anterior preservada como `/storage/roms/ports/sonic4.devbackup_20260626_183504`;
- zip final copiado para `/storage/roms/Sonic4EP2_PortMaster_v1.zip`;
- teste fbdev com `emustation.service` parado temporariamente e religado ao final;
- log confirmou que no Mali-450 nao houve override R36S: `setScreenSize(1280 x 720)` e
  `fox: init(w=1280 h=720)`;
- jogo entrou no loop principal, renderizou frames e ficou em torno de 60 FPS;
- `emustation.service` voltou `active`; `es-de.service` permaneceu `inactive`;
- nenhum processo `sonic4` rodando ao final.

Validacao limpa anterior no `.79`:

- APK copiado para `/storage/roms/ports/sonic4ep2/sonic-the-hedgehog-4-episode-ii-2.0.0.apk`;
- OBB direto copiado para `/storage/roms/ports/sonic4ep2/main.22.com.sega.sonic4episode2.obb`;
- backup de inputs preservado em `/storage/roms/ports/sonic4ep2/_backup_input/`;
- primeiro launch extraiu `libfox.so` e OBB automaticamente;
- `libfox.so` extraida com md5 `d77489abf54523046ade35425e537782`;
- OBB extraida com md5 `3b8ad5c461014bd94cf227982d49c664`;
- primeiro boot do layout final chegou em `[frame 0]`, criou `foxsave_0.dat` novo e estabilizou em 60 FPS;
- segundo boot nao reextraiu dados, carregou `foxsave_0.dat` com sucesso e estabilizou em 60 FPS;
- apos encerrar, varredura de `/proc` retornou `no_sonic_running`.
- `progressor` corrigido e smoke-testado depois da extracao: saiu `child exited with code 0`, sem apagar APK/OBB restaurados;

Regra operacional do device:

- o frontend correto para devolver a tela ao usuario e `emustation.service`;
- nao iniciar, parar, reiniciar ou alterar `es-de.service` neste fluxo.

## Audio implementado
O caminho real do Sonic 4 EP2 nao e OpenSL neste port. A `libfox.so` chama a ponte Java
`com/mineloader/fox/AudioHelper`:

- `MusicSetDataSource(id, key)`;
- `MusicStart(id)`, `MusicStop(id)`, `MusicPause(id)`, `MusicVolume(id, volume)`;
- `PlaySound(key, volume, arg)`;
- `PauseSound(handle)`, `ResumeSound(handle)`, `StopSound(handle)`, `SetVolume(handle, volume)`;
- `spReset/mpReset`;
- `asyncBuildSpData(...)`, `asyncBuildBgmData(...)`;
- `isDoneBuildSp`, `isDoneBuildBgm`, `GetMusicState`.

O shim JNI encaminha essas chamadas para `src/sonic_audio.c`.

`src/sonic_audio.c`:

- abre audio SDL2 em 44100 Hz stereo S16;
- usa os leitores nativos do proprio `libfox.so` (`tsReadFile`) para buscar arquivos dentro do LPK/OBB;
- decodifica MP3 com `libmpg123`;
- decodifica OGG com `libvorbisfile`;
- mistura BGM e SFX em callback SDL;
- cacheia ate 256 buffers decodificados;
- trata SFX observados como one-shot, porque o terceiro argumento de `PlaySound` veio como `2/3/-1`
  mesmo para sons que nao podem loopar;
- replica o comportamento nativo do `AudioHelper`: `MusicSetDataSource`/`MusicStart` da mesma faixa
  ativa nao reiniciam a posicao, e o loop vem da tabela nativa `AudioDataTbl` (`loopflag`,
  `loopStart`, `loopEnd`) em vez de tratar toda musica como loop total;
- trata jingles de evento como camada sobre o BGM, mas preserva os loops nativos de invencibilidade
  e Super Sonic; jingles curtos (`emerald`, `1up`, `clear`, etc.) ficam one-shot;
- corrige o mapa nativo de bosses: `boss2=FinalA1`, `boss3=MetalSonic`, `boss5=StardustSPDWY`,
  `boss6=DEmk2`;
- aplica headroom/soft limiter no mixer e reaproveita SFX mecanicos ja ativos para ficar mais proximo
  do comportamento do `SoundPool` Android e evitar clipping em mecanismos repetitivos;
- aceita `SONIC_SFX_OVERRIDE`, por exemplo `Jump=S4EP2FX_024_S1C2_44.OGG`, para testar trocas sem
  recompilar;
- carrega `sfx_map.tsv` em runtime, com 644 entradas normalizadas pelo manifesto real do OBB, e usa
  o banco atual vindo de `asyncBuildSpData` (`ep2zone1`, `ep2zone2`, etc.).

## Fonte oficial do mapa SFX
O mapa certo veio do DEX do APK, nao de chute pelo nome dos arquivos:

- APK: `sonic-the-hedgehog-4-episode-ii-2.0.0.apk`;
- DEX extraido: `/tmp/sonic4-classes2.dex`;
- classe: `com/mineloader/fox/AudioDataTbl`;
- dump local: `/tmp/sonic4-dex-sfx-map.tsv`;
- resultado: 772 linhas, 288 cues unicos, 10 cues conflitantes por banco/zona.

Correcoes importantes ja aplicadas em `g_sfx_map`:

- `Ok` -> `S4EP2FX_001_SHSY08_22.OGG`;
- `Pause` -> `S4EP2FX_004_SHSY10_22.OGG`;
- `Jump` -> `S4EP2FX_009_SK62_44.OGG`;
- `Enemy` -> `S4EP2FX_017a_S2_3441_44.OGG`;
- `Spring` -> `S4EP2FX_067_SKB1_44.OGG`;
- `Ring1L` -> `S4EP2FX_112a_S2_2235_44L.OGG`;
- `Ring1R` -> `S4EP2FX_113a_S2_2235_44R.OGG`;
- `LockedOn` -> `S4EP2FX_144a_COLORS_OBJ_LOCKON_22.OGG`.

Detalhe maior em `SFX_MAP.md`.

## Validacao feita
Build local:

```sh
cd ports/sonic4
bash build.sh
```

Deploy feito para `.79` com `scp`.

Regra obrigatoria de launch:

- usar `run.sh` para abrir o jogo no device;
- usar `runsonic.sh N` para teste com tempo/screenshot;
- usar `stop.sh` quando precisar apenas fechar o Sonic sem abrir outro;
- ambos matam qualquer processo existente cujo `/proc/*/exe` seja
  `/storage/roms/ports/sonic4/sonic4*` antes de iniciar outro, incluindo processo antigo com binario
  deletado;
- nao iniciar manualmente com `nohup ./sonic4 ... &` sem rodar a mesma varredura primeiro.
- `SELECT+START` fecha o jogo no binario, igual Bully/Dysmantle/Katana Zero; no Sonic o check tambem
  roda no `main.c`, porque esse loop consome os eventos SDL antes do `android_shim`.

Teste bom de gameplay/SFX:

```sh
cd /storage/roms/ports/sonic4
SONIC_EXTRA='SONIC_NOFAKESOUND=1 SONIC_AUDIOLOG=1 SONIC_AUTORIGHT_AFTER=1150 SONIC_AUTOJUMP_AT=1240 SONIC_INPUTLOG=1' sh ./runsonic.sh 150
```

Artefatos desse teste:

- log: `/tmp/sonic4-input-a-game1.log`;
- screenshot: `/tmp/sonic4-input-a-game1.png`;
- screenshot mostra gameplay com fundo, HUD, Sonic/Tails, nao o menu de pause falso antigo;
- nenhum `unmapped sfx` no trecho validado;
- contagem observada: `Spring` 70, `Ring1L` 5, `Ring1R` 4, `Ok` 2, `Jump` 2, `LockedOn` 1.

Teste aprovado pela validacao NextOS:

- log: `/tmp/sonic4-audio-final-ok.log`;
- audio sem engasgos;
- som da mola repetindo e correto, porque a rota automatica fica quicando em cima da mola;
- som considerado finalizado.

Performance:

- problema raiz: `main.c` tinha `usleep(16000)` fixo apos o present;
- como o swap ja espera vsync, isso criava double pacing e deixava menu 3D/gameplay em camera lenta;
- agora o sleep default e `0`; se precisar testar, usar `SONIC_FRAME_SLEEP_US=N`;
- log bom: `/tmp/sonic4-perf-perfect.log`;
- gameplay apos carregar estabiliza em `[PERF] fps=60.0 avg=16.7ms`.

Start/Pause:

- tentativa antiga por input bruto `FOX_START` nao abria o menu;
- o caminho atual chama a sequencia nativa `GmPauseMenuLoadStart` -> `GmPauseMenuBuildStart` ->
  `GmPauseMenuStart`;
- NextOS confirmou que Start/Pause ficou perfeito;
- o run de teste manual atual foi iniciado no `.79` sem `AUTOPAUSE`, sem `AUTORIGHT`, sem `AUTOJUMP`
  e sem `SONIC_INPUTLOG`, mantendo apenas o `AUTOSTART` inicial do launcher para entrar no jogo.

Save/continue:

- nao forcar mais `_Z18GsUserIsSaveEnablem`; esse flag precisa refletir o load real do save;
- o setup nativo carrega `foxsave_0.dat`, mas no port o caminho observado nao reconstruia sempre os dados
  globais de progresso antes do menu;
- `main.c` agora espera `AoStorageLoadIsFinished()` + `AoStorageLoadIsSuccessed()` e chama
  `DmBuildSysDataFromBackup()` uma vez, seguido de `gs::backup::utility::UpdateStageUnlockState()`;
- validado no device com o save bom: log mostrou `stage 0 clear=1`, `stage 1 unlocked=1` e Start parou de
  cair direto em `G_ZONE1`/Act 1 novo;
- save bom preservado no device como `foxsave_0.dat.codex_keep_20260626_173044`; nao restaurar o backup
  `before_native_load_20260626_175036`, porque ele representava estado quase novo.
- validacao final em jogo real: apos concluir a primeira fase, `foxsave_0.dat` ficou com md5
  `81be5f6252b533ad0cc1024e0b7074e5`; depois de `stop.sh` + `run.sh`, o menu mostrou `Continue`.

Protocolo recomendado para validar save limpo:

```sh
cd /storage/roms/ports/sonic4
cp -p foxsave_0.dat foxsave_0.dat.before_clean_save_test_$(date +%Y%m%d_%H%M%S) 2>/dev/null || true
rm -f foxsave_0.dat
sh ./run.sh
```

Depois passar uma fase inteira. Em seguida fechar pelo guard (`sh ./stop.sh`) e reabrir com
`sh ./run.sh`: Start deve entrar no mapa/continue, nao reiniciar a primeira fase do zero.

## Pendencias atuais
Ordem recomendada:

1. Testar controles reais por mais tempo no pacote v1 limpo.
2. Validar rotas mais longas de gameplay para item box, inimigo/matar bicho, damage,
   spin/dash/homing, gimmicks de zona e coop/Tails. Audio base ja esta aprovado.
3. Depois do teste manual, revisar qualquer visual/audio restante que aparecer fora da primeira rota.

## Auditoria Bully v11 × Sonic v5 (2026-07-02) — video/audio/compat
- Gap real achado e FECHADO: bundle libs.aarch64 agora leva a familia vorbis
  (libvorbisfile.so.3 + libvorbis.so.0 + libogg.so.0) alem da libmpg123 — o
  binario LINKA essas libs (NEEDED) e um device enxuto futuro (padrao X5M)
  poderia nao te-las. Zip v5 final md5 4eb359bd; bundle tambem nos 4 devices.
- Licao estrutural (nao urgente): Bully so linka SDL2/EGL/GLES/libc e faz
  dlopen(RTLD_GLOBAL) de openal/mpg123 — nunca morre no ld.so por lib de audio.
  Migrar mpg123/vorbis p/ dlopen num v5.x deixaria o Sonic igual.
- Resto da comparacao: Sonic v5 >= Bully em video (resolucao com fallback DRM
  sysfs + drawable authority + rejeicao desktop-GL + weston fallback; Bully tem
  MSAA opt-in e alpha8->0 que nao precisamos) e >= em audio (scan adaptativo de
  driver SDL + override do launcher vs OpenAL do device com alsoft.conf fixo).
