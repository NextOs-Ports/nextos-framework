# 🦔 Sonic the Hedgehog 4: Episode II → Mali-450 (so-loader) — ESTUDO

**Veredito: 🟢 VIÁVEL (verde).** Engine GLES2 aberta + dados completos em mãos. Sem muro
arquitetural. Padrão so-loader que já dominamos (Shantae/DuckTales/SOR4/Chrono/LEGO).
**Não existe PortMaster dele** (os Sonic do PortMaster = 1/2/CD via decompilação RSDK e SRB2;
a engine NN/fox não tem decompilação) → seria inédito.

## Arquivos (temos os 2 pedaços)
- **APK** `sonic-the-hedgehog-4-episode-ii-2.0.0.apk` (20MB) — versão F2F/lite
  (`com.sega.sonic4ep2lite`), só engine + ads. Engine: `lib/armeabi-v7a/**libfox.so**` (8.8MB).
- **DADOS** `cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip` (433MB) → contém
  **`com.sega.sonic4episode2/main.22.com.sega.sonic4episode2.obb`** (594MB, formato **LPK**) =
  o jogo COMPLETO (episode2, não lite). Magic `LPK\0`.

## Engine
- **`libfox.so`** = middleware **NN "Ninja" da Sega** (funções `nn*`: nnSetMaterialControl…,
  nn_AndVer…) + wrapper "fox" (mineloader). **ELF32 ARM (armeabi-v7a)**, stripped, BuildID
  2cbe8b35. **GLES2 PURO** (`nnSetMaterialControlSpecularGLES20`, shaders `precision mediump
  float`, sem ES3; só 1 `glUniformMatrix3fv`). NEEDED: libGLESv2, libEGL, libandroid, liblog,
  libz, libdl, libstdc++, libm, libc. (sem OpenSLES — áudio é via Java AudioHelper).
- **Modelo GLSurfaceView** (igual LEGO Batman/Chrono): a Activity Java dirige a engine por JNI.
  NÃO usamos NativeActivity; **replicamos o driver Java no nosso main.c**.

## 🔌 JNI — entry points da engine (`com.mineloader.fox.foxJniLib`) — NÓS CHAMAMOS
Sequência típica (a partir do disasm dos stubs em 0x4bd2xx):
- `init(...)` (0x4bd254) — init mestre.
- `SetGamePath(path)` (0x4bd41c) — diz onde estão os dados (LPK/OBB). Path esperado:
  **`/mnt/sdcard/Sonic4EP2DL/`** (no port: apontar pro nosso dir; redirecionar).
- `coreGetLPKFileInfo(...)` (0x4bd4b0) — registra/abre o LPK (índice do OBB).
- `SetLanguageId(id)` (0x4bd544) — **forçar INGLÊS** (NextOS prefere evitar idioma errado).
- `DrawEGLCreated()` (0x4bd2a8) — chamar quando a surface/contexto EGL está pronto (GL init).
- **Loop por frame:** `FileProcess()` (load streaming) + `GameProcess()` (lógica) + `DrawFrame()`
  (render; a engine desenha — modelo GLSurfaceView, nós damos o present/swap como nos outros).
- Input: `SetPadData(...)` (0x4bd2e4, joypad), `SetTPData(...)` (0x4bd2d4, touch),
  `SetSensorData(...)`, `HasController()` (0x4bd3d8 → forçar 1).
- `nativePlaySe(...)` (0x4bd5a4) — SFX.
- ciclo de vida: `pauseEvent/resumeEvent/Release/quitAndWait/IsExitBlock`.
- flags: `GetBuildTarget/GetMarketTarget/SetContinueFlag/GetTweetClearRings`.
- 🔑 **`IsLiteVersion()` (0x4bd55c) → `GsTrialIsTrial()` (0x4be8d4)** e
  **`IsTegra3Version()` (0x4bd570) → `nn_AndVerIsTegra3()`** — ver DRM/GPU abaixo.

## 🪝 JNI — callbacks que a ENGINE chama (NÓS PROVEMOS no jni_shim)
Classes: `com/mineloader/fox/{AudioHelper, APKFileHelper(+APKFile), ADHelper, VibHelper,
OpenFeintHelper, WallPaper, FoxActivity_Core, bluetoothservice}` + `com/sega/f2fextension/
f2fextensionInterface` + `net/gogame/GoGameJniAdapter`.
- 🔊 **AudioHelper** (CRÍTICO p/ som): `DmSoundPlayBGM/PlaySE/SetVolumeBGM/SetVolumeSE`,
  `GmSoundPlayBGM/PlaySE/PlayBGMJingle`, `PlaySound`, `SetVolume` → **bridge SDL/pulse**
  (decodificar ogg/wav/mp3 do LPK e tocar; igual Chrono/RE4 audio shim).
- 📦 **APKFileHelper / APKFile** (CRÍTICO p/ dados): `getAPKFile(String)→APKFile`, `read(APKFile,
  int)→int`, etc. = a engine lê o LPK/arquivos por aqui. Implementar: abrir do nosso dir/OBB.
  (alternativa: usar AAssetManager nativo — `setassetmanager` (netlib::FileUtilsAndroid) existe).
- 🚫 **ADHelper** (`isShowAds`/`IsTrialShowAdBanner`→**false**), **f2fextension**
  (`isUserRemoveAds`→**true**, `isShowAds`→false, GDPR/consent/age → valores neutros,
  `callBackIntroVideo` etc → no-op), **GoGameJniAdapter** (stub), **VibHelper/OpenFeintHelper/
  WallPaper/bluetoothservice** (stub/no-op).

## 🗂️ Dados (LPK / OBB)
- `main.22.com.sega.sonic4episode2.obb` = archive **LPK** (header `4C 50 4B 00` + tabela de
  offsets). Conteúdo: stages (`stg0`, `ACT7`, `DEMO/STGSLCT`), personagens
  (`CHR_SONIC_BODY01_DIF.DDS`, `CHR_SONIC_SPIN`), modelos **NN** (`.NN/.UV/.NNTC`, 2023 `.AMB`),
  **texturas `.DDS`(1792) + `.PVR`(298)**, **áudio `.OGG`(286)/.WAV(281)/.MP3(45)**, vídeos
  `.MP4`(65), shaders `NNGLES20SHADER/*.pb`, `.ZIP`(772 sub-arquivos), `.TXB/.LNM/.LNO/.LNV`.
- Engine espera os dados em **`/mnt/sdcard/Sonic4EP2DL/`** (SetGamePath) — no port redirecionamos
  pro nosso GAMEDIR. O LPK é lido nativamente (não precisamos desempacotar; só servir o arquivo).

## 🔐 DRM / gates (todos solúveis)
- 🔑 **Trial/Lite → FULL:** `IsLiteVersion` retorna `GsTrialIsTrial()`. **Patch `GsTrialIsTrial`
  (0x4be8d4) p/ retornar 0** (`mov r0,#0; bx lr`) → jogo completo (usa `c_create_act_table`
  cheio, não `_trial`). Confirmar tб `GsTrialIsTrial_VerTwo` (0x4be8e4) e
  `AoEp1LicenseIsEnabled`. (Os dados completos JÁ estão no OBB episode2.)
- 🎮 **GPU/textura:** `IsTegra3Version → nn_AndVerIsTegra3` = **falso** no Mali (não spoofar
  Tegra3) → engine evita o set DXT (`DM_LTEGRA3T_MAIN`, `D_LOGO_TEGRA.AMB`). Mali-450 suporta
  **ETC1** → engine deve pegar o set ETC/genérico. ⚠️CONFIRMAR no 1º boot que escolhe ETC e não
  PVRTC (PVR não roda no Mali). Se pegar PVR/DXT, forçar a detecção pro set ETC.
- 🌐 Online (gogame/puyosega.com/SSL) e ads = stub (sem rede).

## 🧱 Plano de port (scaffold)
1. Base = **framework lswtcs-src** (`docs/reference/lswtcs-src/`): so_util/egl_shim/jni_shim/
   imports/android_shim REUSADOS. main.c novo p/ o driver fox (init→SetGamePath→SetLanguageId
   (inglês)→loop{FileProcess,GameProcess,DrawFrame} + input SetPadData/SetTPData).
2. Carregar `libfox.so` (ELF32-ARM; usar so_util armv7 do Shantae/DuckTales, não o arm64).
3. EGL/GL no Mali: surface 1280x720(?) → DrawEGLCreated → DrawFrame; present igual aos outros
   (a engine pode não dar eglSwapBuffers → damos nós; **lição LEGO Batman: cuidado com
   single-buffer/tearing → usar FBCOPY se precisar**).
4. JNI shim: prover AudioHelper(bridge SDL) + APKFileHelper(ler LPK) + stubs (ads/vib/etc).
5. Patch GsTrialIsTrial→0 (full). Forçar HasController→1, inglês.
6. Input: SetPadData (mapear SDL gamepad) + SELECT+START sair. Touch via SetTPData se preciso
   nos menus.
7. Áudio: decodificar ogg/wav/mp3 → SDL/pulse.

## ⚖️ Riscos (todos táticos, nenhum muro)
- Textura: garantir set ETC1 (não PVR/DXT) no Mali-450. [médio]
- Áudio bridge (AudioHelper) + formatos ogg/wav/mp3. [médio, já fizemos]
- Amplitude do jni_shim (muitos stubs F2F) — trivial mas chato. [baixo]
- Present/tearing no fbdev Mali (usar FBCOPY da lição LEGO Batman). [baixo]
- Confirmar que o LPK episode2 casa com a libfox da APK lite (mesma versão 2.0.0/main.22). [baixo]

## Régua
Mais fácil que LEGO Batman/Elderand: **GLES2 aberto, sem pairip, sem IL2CPP, sem job-system-GL
deadlock à vista, dados completos em mãos.** Bom candidato. Device alvo provável: Mali-450 .79/.164.
Bancada Android p/ comparar fluxo: instalar o APK lite + OBB num device arm (ex. Moto G100
<IP-DO-DEVICE> via adb rede) e observar o fluxo correto.

---
## ⚡ PROGRESSO s1 (2026-06-26) — SCAFFOLD BOOTA, CARREGA LPK, RODA ENGINE ATÉ O INTRO

🟢 **so-loader funciona:** so_load libfox.so (ELF32-ARM, framework Shantae), resolve JNI,
roda init → GameProcess → loop. Binário `sonic4` (build.sh, toolchain armhf). Device .79,
em `/storage/roms/ports/sonic4/` (libfox em lib/armeabi-v7a/, OBB em data/).

**Fixes que destravaram (na ordem):**
1. patch GsTrialIsTrial/_VerTwo → 0 (full). ⚠️**ARM mode, não Thumb** (detectar pelo bit do símbolo;
   patch Thumb numa func ARM = SIGSEGV).
2. 🔑 **LPK/OBB:** `SetGamePath(env,thiz, id=255, jstring path_do_OBB)` → tsSetFileRootPath(255,..)
   → **tsInitFileRootLPK → fopen(OBB)** = abre+indexa o LPK. (id<254 só guarda o path, NÃO abre LPK!)
   +`SetGamePath(0, <dir>)` p/ root de arquivos soltos. ANTES do init (init lê font.nft do LPK).
   O OBB = LPK (magic `LPK\0`), lido via fread NATIVO (tsFRead tipo 1). "Read error" no log é
   ESPÚRIO (check de flag bionic [FILE*+12]&0x40 no FILE glibc) — fread retorna certo (32 bytes OK).
3. 🔑 **threading:** FileProcess = `amFS_proc` = loop da THREAD de file-system (cond_wait quando
   ocioso) → roda em pthread DEDICADA, não no loop. Main loop = GameProcess + DrawFrame.
4. 🔑 **JNI_OnLoad:** libfox exporta `JNI_OnLoad` (0x4fe988) — chamar com nosso fake VM (o Android
   chamaria ao carregar o .so). Seta o global JavaVM que `F2FExtension::GetJNIEnv` lê (NULL deref sem isso).
5. **f2fextension setup:** chamar `nativeSetContext`/`SetJavaObj`/`nativeSetApkPath` (JNI) antes do init.

🔴 **ONDE PAROU (próximos passos):**
- Engine roda GameProcess: getBundlePath, getRegionCode, **Android_playIntroVideo**, NeConInit
  (rede, completou). **Trava esperando o intro video** (SEGA logo mp4) — callBackIntroVideo não
  destravou (achar o callback/flag certo, ou skip nativo do estado intro).
- 🔴 **`ShaderProfile Num : -1`** — engine não detectou o profile de shader do GPU → provável
  bloqueio de RENDER (tela PRETA, frames rodam mas 0 pixels). Investigar a detecção (glGetString
  RENDERER/EXTENSIONS no Mali) — pode precisar spoof do profile/extensões.
- 🟡 present/Mali fbdev: quando renderizar, aplicar lição LEGO Batman (FBCOPY/pan; fb 1280x1440).
- 🟡 stubar f2fextension/ads direito (muitos Android_* callbacks); áudio (AudioHelper bridge).
- Flags: SONIC_DATADIR, SONIC_LPK, SONIC_IOLOG (log de fread).

### s1 update — GL PIPELINE CONFIRMADO (present OK), engine renderiza PRETO
- 🔑 **egl_shim_bind_main()**: modelo GLSurfaceView = a engine NÃO cria contexto EGL próprio,
  assume o contexto já current. O Shantae egl_shim soltava o contexto (gl_makecurrent NULL) no
  fim do create_window → DrawFrame rodava SEM contexto → preto. Fix = ligar o share-root na
  thread principal após create_window.
- ✅ **Teste glClear(vermelho) após DrawFrame (SONIC_TESTCLEAR=1) → TELA VERMELHA nas 2 metades**
  = contexto GL current OK + present OK + fbdev Mali OK (NÃO precisa FBCOPY/pan; SDL double-buffer
  mostra nas 2 metades). 
- 🔴 **PRÓXIMO = por que a engine renderiza PRETO** (DrawFrame não desenha o título):
  suspeito `ShaderProfile Num : -1` (shaders NN não carregam/registram → glDrawElements sem
  program → nada). Investigar: nnRegistStdShaderProfile / de onde carrega os NNGLES20SHADER
  (LPK? loose .pb?); confirmar se o game chega no DmTitleInit (title) ou trava antes (FS thread
  idle = não enfileira o load do título?). Achar o gate que impede o título de renderizar.

### s1 update 2 — game logic RODA (amTaskExecute), boot task parado antes do título
- `GameProcess`→`fox_FrameUpdate` (0x4ac3fc): se `Sonic4F2F::isGamePause()` → return early
  (pula amTaskExecute). Chamamos `resumeEvent` + patch isGamePause→0 (não era o gate; já era 0).
- fox_FrameUpdate roda: amAlarmWaitTimer, amDrawCloseDisplayList, amPadGetData, amTpExecute,
  gsGxPfxTest::Update, **amTaskExecute** (a STATE MACHINE / tasks do jogo). Então a lógica RODA.
- 🔴 **O boot task NÃO transiciona pro título** (DmTitleInit 0x254340 nunca alcançado via gdb break).
  Fica num estado preto esperando algo: provável (a) asset async que a FS thread não entrega
  (reads param após boot; a fonte carregou SÍNCRONO, o caminho async game→amFS_proc não foi
  exercitado/confirmado), OU (b) render-ready (ShaderProfile -1 → shaders não registram → boot
  gateia no GL), OU (c) condição F2F/online.
- pthread_cond bridge (pthread_bridge.c cnd_real) parece OK (bionic→glibc).
- ✅ Fix do F2F path: getLocalPath/getBundlePath→dir gravável, getRegionCode→"US", getLanguageCode→"en"
  (cria Sonic4ep2.f2f; antes "/Sonic4ep2.f2f" no root falhava).
- 🎯 PRÓXIMO: (1) confirmar se a FS thread (amFS_proc) processa requests async — logar/instrumentar
  o request queue (amFS); (2) BANCADA: instalar Sonic4 no Moto G100 e logcat o foxLog correto
  pra ver o que vem DEPOIS do nosso ponto (NeConInit) e qual estado/asset destrava o título;
  (3) investigar nnRegistStdShaderProfile (shaders carregam do LPK?).

## 🏆 PROGRESSO s2 (2026-06-26) — TELA DE TÍTULO RENDERIZA (imagem na tela!)
Boot: **logo SONIC TEAM** (nítido) → **tela de título "SONIC THE HEDGEHOG 4 EPISODE II"**
(emblema alado, Sonic+Tails 3D, ©SEGA). A tela preta da s1 era 1 BUG TRIVIAL.

🔑🔑 **FIX RAIZ DA TELA PRETA:** a JNI `Java_..._foxJniLib_init(env, thiz, r2=WIDTH, r3=HEIGHT)`
repassa os args 3/4 → `fox_Init(w,h)` (0x4ac104) → `amDrawInitVideo(w,h)` (0x1f4f54) que dimensiona
`_am_draw_video` (0x87ba38, struct de display: w@+0, h@+4, formatos FBO @+56=GL_RGB/+60=DEPTH16).
Chamávamos `fox.init(env,thiz, NULL, NULL)` = **0,0** → `amRenderCreate` cria FBO **0x0** → INCOMPLETE
(`glCheckFramebufferStatus=0x8cd6` GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT) → todo `glDraw*` falha com
`GL_INVALID_FRAMEBUFFER_OPERATION (0x506)` → **TUDO PRETO**. Fix em main.c:
`fox.init(env, thiz, (void*)(intptr_t)dys_screen_w, (void*)(intptr_t)dys_screen_h)`. **1 linha.**

🔑 **gate do título (CStateInitialize::Next 0x2525a4):** 3 condições p/ sair do Initialize→Opening:
(1)`SJni_IsUpshellShow()`==0 — JNI CallBooleanMethod (nosso shim devolvia 1=upsell aberto→trava);
fix=`patch_ret0("_Z18SJni_IsUpshellShowv")`. (2)`GsTrialCheckIsFinished()`!=0 (já é 1 fixo). (3)
`CDemoResourceManager::IsValid(evtType=3)` (thunk 0x23514c)!=0 — recurso do attract-demo do evento 3
(id 2) NUNCA valida (arquivos DEMO/* abrem OK no LPK via LPK_GetFile, mas o recurso do evt 3 não
completa o build). BYPASS = NOP no `beq` em `CStateInitialize::Next+0x5c` (patch_word_at→0xe1a00000):
avança Initialize→FadeIn→Opening→LogoMainFadeIn→LogoSubFadeIn→Animation→Waiting (TÍTULO VISÍVEL).
⚠️Forçar `IsValid->1` GLOBAL (thunk) CRASHA (over-advance pro save load "foxsave.dat failed").

🔬 **DEBUG (método):** binário não-stripped → gdb no device: `break egl_shim_bind_main; run;
set $b=(unsigned long)text_base; break *($b+OFFSET)`. Capturar tela: `dd if=/dev/fb0` (1280x1440 =
2 metades de 720, 32bpp BGRA→RGBA) → PNG. Instrumentei glCheckFramebufferStatus/glGetError +
glRenderbufferStorage/glTexImage2D/glFramebufferTexture2D nos wrappers de imports.c (env SONIC_GLERR)
→ revelou FBO 0x0. ⚠️rodar sonic4 SEMPRE detached (`nohup ./sonic4 >log 2>&1 &`); foreground over ssh
trava o canal — ler logs em ssh separado.

🔧 **Flags diag (main.c/imports.c):** SONIC_GLERR (FBO status+glerror+formatos attachment),
SONIC_PIXDIAG (lê pixel central de FB0..3 por frame), SONIC_KEEPDEMOGATE (desliga bypass gate3),
SONIC_FORCEDEMOGATE (força IsValid->1, crasha), SONIC_TESTCLEAR (glClear vermelho = testa present).

🔴 **MUROS s3 (próximo):** (a) **attract-demo evt3 não builda** — círculo verde no centro do emblema
durante o load = render-target do demo de attract; achar pq id 2 do evt 3 não valida (`SyGetEvtInfo()->
[12]`=3, `CResourceManagerTask::IsValid` 0x235034). (b) **possível swap R/B nas texturas dos chars**
(Sonic vermelho/Tails azul no t_16, mas o LOGO tem cores certas → não é swap global; checar decode
DDS/PVR dos personagens). (c) **áudio** (AudioHelper bridge SDL/pulse). (d) **controle** SDL gamepad
+ SELECT+START. (e) **empacotar** launcher PortMaster (sem forçar SDL driver). (f) present Mali: já
funciona via SDL_GL_SwapWindow nas 2 metades (NÃO precisou FBCOPY).

## PROGRESSO s4 (2026-06-26) - AudioHelper nativo funcionando

Foco desta etapa: audio. Lentidao percebida no teste ficou para depois.

Confirmacao importante: neste jogo o caminho real de audio nao e OpenSL; a `libfox.so` chama a
classe Java `com/mineloader/fox/AudioHelper`. O shim JNI agora registra e implementa os callbacks
`PlaySound`, `MusicSetDataSource`, `MusicStart`, `MusicStop`, `MusicVolume`, `SetVolume`,
`StopSound`, `spReset/mpReset`, `asyncBuildSpData`, `asyncBuildBgmData` e `GetMusicState`.

Implementacao nova:

- `src/sonic_audio.c` / `src/sonic_audio.h`;
- build linka `libmpg123`, `libvorbisfile`, `libvorbis`, `libogg`;
- audio SDL2 44100 Hz stereo S16;
- leitura de assets pelo proprio `tsReadFile` da engine, entao nao precisa extrair OBB;
- MP3 via mpg123, OGG via vorbisfile;
- mixer simples para BGM + SFX;
- cache de buffers decodificados;
- SFX observados tratados como one-shot (o 3o argumento de `PlaySound` nao e loop confiavel: `Ok`,
  ring e spring chegaram como `2/3/-1`).

Validado no device `.79`:

- `SONIC_NOFAKESOUND=1 SONIC_AUDIOLOG=1` passa por titulo/menu/gameplay;
- SDL audio abriu (`44100Hz 2ch samples=1024 driver=pulseaudio`);
- BGM titulo: `SOUND/MUSIC/MIXED00_EP2_TITLE.MP3`;
- BGM menu: `SOUND/MUSIC/SNG01_EP2_SNG_MENU_V00_EP1.MP3`;
- BGM fase: `SOUND/MUSIC/MIXED03_EP2_Z1A1.MP3`;
- SFX `Ok`, `Ring1L`, `Ring1R`, `LockedOn`, `Spring` carregam e decodificam como OGG.

Tabela de BGM foi expandida para zonas, speedups existentes, bosses, special/endroll/act-clear,
jingles e faixas EP1 usadas pelo conteudo extra.

## PROGRESSO s5 (2026-06-26) - SFX oficial via DEX + input de gameplay corrigido

O mapa correto dos SFX veio do DEX do APK, nao do nome bruto dos arquivos e nao de CSB/CPK. A classe
`com/mineloader/fox/AudioDataTbl` em `/tmp/sonic4-classes2.dex` foi parseada e gerou
`/tmp/sonic4-dex-sfx-map.tsv`: 772 linhas, 288 cues unicos e 10 cues conflitantes por banco/zona.

Correcoes importantes aplicadas em `src/sonic_audio.c`:

- `Ok` agora e `S4EP2FX_001_SHSY08_22.OGG` (antes estava confundido com `Damage1`);
- `Pause` agora e `S4EP2FX_004_SHSY10_22.OGG`;
- `Jump` agora e `S4EP2FX_009_SK62_44.OGG`;
- `Enemy` agora e `S4EP2FX_017a_S2_3441_44.OGG`;
- `Spring` agora e `S4EP2FX_067_SKB1_44.OGG` (antes estava confundido com som curto de resultado);
- `Ring1L/R`, `LockedOn`, `Spin`, `Dash*`, `Damage*`, `Homing`, `Barrier`, `Coop*`, `ItemBox_Dbl`
  e outros cues comuns entraram no mapa base.

Tambem foi adicionado `SONIC_SFX_OVERRIDE` para testar `Cue=Arquivo.OGG` sem recompilar. O cache de
audio usa `cue+arquivo`, entao override troca o som de verdade.

Input: o bit A foi separado em menu e gameplay:

- `FOX_A_MENU = 0x8020` para confirmar titulo/menu;
- `FOX_A_GAME = 0x0020` para pulo/acao em gameplay.

Isso removeu o efeito antigo onde o auto-clique de A em gameplay abria uma tela azul de Pause/menu.
`sonic_game_started` e detectado pelos logs Android da engine com "game start"; depois disso
`SONIC_AUTOJUMP_AT` usa somente `FOX_A_GAME`.

Validacao boa no device `.79`:

```sh
SONIC_EXTRA='SONIC_NOFAKESOUND=1 SONIC_AUDIOLOG=1 SONIC_AUTORIGHT_AFTER=1150 SONIC_AUTOJUMP_AT=1240 SONIC_INPUTLOG=1' sh ./runsonic.sh 150
```

Artefatos:

- `/tmp/sonic4-input-a-game1.log`
- `/tmp/sonic4-input-a-game1.png`

Resultado:

- gameplay visivel com fundo/HUD/Sonic/Tails;
- nenhum `unmapped sfx` no trecho testado;
- `Spring` disparou 70 vezes usando `S4EP2FX_067_SKB1_44.OGG`;
- `Ring1L/R`, `Ok`, `Jump` e `LockedOn` tambem decodificaram dos arquivos oficiais.

Pendencias atuais:

- validar por rota sons que ainda nao foram acionados: item box, matar inimigo, damage,
  spin/dash/homing, gimmicks de zona e coop/Tails;
- implementar resolucao por banco/zona se algum dos 10 cues conflitantes aparecer;
- investigar o fundo que ainda falta no menu Start/Pause real. `SONIC_IOLOG=1` via `fopen` nao
  mostra leituras internas do LPK; para esse caso precisa logar/hookar `tsReadFile`;
- performance/FPS fica para a proxima etapa, depois do mapa de audio e fundo. O `egl_shim.c` ja
  possui log `[PERF]` e `SONIC_SWAPINT`; o `jni_shim.c` ja responde bateria 100%/carregando
  para evitar cap de power-save.

## PROGRESSO s6 (2026-06-26) - Audio finalizado e gameplay 60 FPS

Audio:

- NextOS aprovou o som como finalizado: menu, gameplay, pulo, mola e sons comuns sem engasgo;
- `sfx_map.tsv` foi gerado a partir do DEX + manifesto do OBB e enviado para o device;
- `sonic_audio.c` agora carrega esse mapa externo e usa o banco atual de `asyncBuildSpData`
  (`ep2zone1`, `ep2zone2`, etc.) antes de cair no fallback hardcoded;
- `SONIC_SFX_OVERRIDE` continua disponivel para teste rapido.

Performance:

- causa principal da lentidao: `main.c` fazia `usleep(16000)` no fim de todo frame;
- como o present via SDL/GL ja fica em vsync, isso criava double pacing e deixava menu 3D/gameplay
  com movimento lento;
- fix: sleep default virou `0`; `SONIC_FRAME_SLEEP_US=N` existe apenas como override de teste;
- `egl_shim_present()` agora tambem emite `[PERF]`, porque Sonic usa o present proprio do loop e
  nao o caminho `eglSwapBuffers`;
- log bom: `/tmp/sonic4-perf-perfect.log`;
- resultado em gameplay: 60 FPS estavel, `avg=16.7ms`, confirmado pela validacao NextOS como perfeito.

Proximo foco daquela etapa:

- Start/Pause dentro do gameplay.

## PROGRESSO s7 (2026-06-26) - Start/Pause resolvido e run manual limpo

O input bruto nao era o melhor caminho para reproduzir o pause:

- `SONIC_AUTOPAUSE_AT=1500` mandando apenas `FOX_START` nao abriu o menu;
- tentativa visual anterior ficou em gameplay normal, entao a automacao nao estava acionando o fluxo real.

Fix aplicado em `src/main.c`:

- resolver os entry points nativos:
  - `_Z20GmPauseMenuLoadStartv`;
  - `_Z25GmPauseMenuLoadIsFinishedv`;
  - `_Z21GmPauseMenuBuildStartv`;
  - `_Z26GmPauseMenuBuildIsFinishedv`;
  - `_Z16GmPauseMenuStartm`;
- quando `SONIC_AUTOPAUSE_AT=N` e o gameplay ja iniciou, rodar a sequencia
  `LoadStart` -> espera `LoadIsFinished` -> `BuildStart` -> espera `BuildIsFinished` ->
  `GmPauseMenuStart(0)`.

Resultado:

- NextOS confirmou que Start/Pause ficou perfeito;
- estado atual aprovado: audio final, performance final e pause/start funcionando.

Run manual deixado no device para teste NextOS:

- device: `<IP-DO-DEVICE>`;
- processo: `/storage/roms/ports/sonic4/sonic4`;
- flags ativas: `SONIC_NOFAKESOUND=1 SONIC_SWAPINT=1`;
- launcher manteve `SONIC_AUTOSTART=1` apenas para entrar no jogo;
- sem `SONIC_AUTOPAUSE_AT`, sem `SONIC_AUTORIGHT_AFTER`, sem `SONIC_AUTOJUMP_AT` e sem
  `SONIC_INPUTLOG`, devolvendo o controle real ao jogador;
- log confirmou `--- game start` e gameplay em ~60 FPS.
