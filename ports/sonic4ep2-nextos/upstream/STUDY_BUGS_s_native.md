# Sonic 4 EP2 — Estudo de bugs + fluxo NATIVO de input/áudio (2026-06-29)

Estudo pedido pelo NextOS: "estude tudo primeiro, veja o fluxo nativo, resolva nativamente".
Device de teste: R36S **ArchR** (RK3326, Mali-G31, ES3.2), root@169.254.170.2.
libfox.so analisada = md5 `d77489abf54523046ade35425e537782` (= a do device).

## BUGS REPORTADOS PELO TESTER (luis)
1. **Y = Left no level select.** No menu de seleção de fase, o botão Y age como Left.
2. **Score preso na tela (Oil Desert Act 3).** Depois do mini-game de apertar A pra score,
   os números do score ficam na tela o resto da fase.
3. **Volume 100% no Knulli/ROCKNIX.** Áudio sai sempre no volume cheio nesses CFW.

---

## FLUXO NATIVO DE INPUT (RE confirmada da libfox.so)

Estrutura do pad que o engine lê (de `GmPadUpdate` @0x3cda24):
- `[pad+0]` = máscara DIGITAL de botões (halfword)
- `[pad+2]` = analog LX (signed), `[pad+4]` = LY, `[pad+6]` = RX, `[pad+8]` = RY

**Verdade dos bits de menu** (lidos de `.data`, símbolos `g_gs_env_key_*` @0x866a4c–56):
- UP=0x0001  DOWN=0x0002  LEFT=0x0004  RIGHT=0x0008
- DECIDE=0x0020 (A)  CANCEL=0x0080 (B)
- → **batem 100% com o nosso FOX mask** (main.c:851-868). FOX_Y=0x0100 é bit separado.

`GmPadDirect` (0x3cde58): retorna `gmPaddirectFromPlayer0` (gm-level, que main.c escreve direto)
SE flag `[ptr+4]&0x800`; senão cai em `AoPadDirect` (0x2112f0). `OUYAGetPauseKey`=0x8000 (confirmado).

**Caminho de input do EP2 = ÚNICO:** main.c monta o FOX mask e faz
`SetPadData(mask,0,0,0,0,0)` + escreve `gmPaddirectFromPlayer0` / `gmPadAnalogLX/LY` direto.
- **Paddleboat NÃO existe na libfox** (readelf: 0 símbolos) → `pb_send_key` é no-op no EP2.
- push_key_event/AKEYCODE_* (android_shim) NÃO alimentam o engine do EP2 (sem Paddleboat;
  a Java foi substituída pelo nosso shim). Só o FOX mask conta.

### Bug #1 (Y=Left / Y sem função) — ✅ RAIZ REAL ACHADA E CORRIGIDA (commit 1b1075e)
**RAIZ DEFINITIVA: nosso MAPA DE BITS do pad estava ERRADO.** Decompilei `foxJniLib.s_remapKey`
(DEX do APK via androguard 4.1.4) = a tabela keycode→bitindex do PRÓPRIO jogo. O input real =
`foxJniLib.gmPadValue` (máscara) montada por `onKeyEvnet(keycode)`; `gmPadValue=(1<<bitindex)`.
SetPadData(gmPadValue) é o que o engine lê. **Bits REAIS (Xbox):**
`Y(kc100)=bit4=0x10`, `A(96)=0x20`, `X(99)=0x40`, `B(97)=0x80`, `L1(102)=0x100`, `L2(104)=0x200`,
`L3/THUMBL(106)=0x400`, `R1(103)=0x800`, `R2(105)=0x1000`, `R3/THUMBR(107)=0x2000`,
`SELECT/BACK(109)=0x4000`, `START(108)=0x8000`. Dir: UP=0x1 DOWN=0x2 LEFT=0x4 RIGHT=0x8.
**O bug:** nosso `FOX_Y=0x100` era na verdade **L1**! Pressionar Y mandava L1 → no world map L1
rola/pagina (= "Y vira Left") e a função PRÓPRIA do Y (bit4=0x10) nunca disparava (= "Y sem função
no jogo inteiro", reportado pelo usuário). **FIX:** corrigi o mapa inteiro pros valores reais de
s_remapKey; removi o hack de supressão de Y/X. **VALIDADO no R36S:** injetar 0x10 (Y novo) NÃO
rola o mapa (dispara a função do Y, HUD muda); antes 0x100 rolava ~1900 samples.
`isPadConfirmButton`=ENTER/START/A, `isPadCancelButton`=BACK/SELECT/ESC/B (Java foxJniLib).
Ferramenta: androguard no DEX (`dex/`, scratchpad). 

#### (histórico) tentativa anterior — supressão (substituída pelo fix de raiz acima)
Os bits de menu NORMAL (g_gs_env_key: left=0x4 etc.) estão certos e estáveis (keydump confirmou,
sem remap). MAS o "level select" do luis = **WORLD MAP** (atrás de Main Menu → Continue), e o
world map lê input por um caminho DIFERENTE dos menus normais.
**REPRODUZIDO** com injetor determinístico `SONIC_TESTBIT=0xNNNN` (main.c): no world map, injetar
`FOX_Y(0x100)` move o mapa IGUAL a `FOX_LEFT(0x004)` — before/after de screenshot idênticos entre
Left e Y (diff 361; cada um move ~1900 samples vs base). Confirmado visualmente.
**RAIZ:** `FOX_Y=0x100` (e `FOX_X=0x40`) são bits "provisórios" (chute antigo, ver nota main.c:849)
que no input-read do world map aliasam pra LEFT. Y/X não são ações no Sonic 4 fora do gameplay.
**FIX (main.c, commit 529ef91):** suprime `FOX_Y|FOX_X` quando `!sonic_in_gameplay` antes do
SetPadData. **VALIDADO:** com fix, injetar Y NÃO move o mapa (before==after, ~410=ruído de animação
vs ~1900 antes); LEFT(0x004) segue navegando (mask preservado no input-change). Regressão zero.
**Infra reusável:** launch compat glibc2.27 + `timeout -s KILL` no R36S + `SONIC_TESTBIT` (injeta bit
determinístico) + screenshot via `/dev/shm/sonic_shot` + diff de imagem. AUTOSTART chega ao world map
(~30s): título→Main Menu→Continue→world map.

### Bug agente #1 (ALTA): `sonic_game_started` PRESO em 1
imports.c:59-93. Único reset = log "Create World Map". Game-over→título, "Exit to Title" do pause
NÃO logam isso → flag fica 1 no título → A vira FOX_A_GAME(0x20) em vez de FOX_A_MENU(0x8020);
o título lê 0x8000 (AoPadSomeoneStand) → **A não passa do título depois de jogar 1 fase** (soft-lock).
### Bug agente #2 (MÉD): `strcasestr(msg,"spstage")` falso-positivo
Qualquer linha de log com "spstage" (ícone/disponibilidade de special no world map, results,
nome de asset SpStage0X) re-seta started=1 num MENU → A perde o bit 0x8000 de confirm.
### Bug agente #4 (BAIXA): aliasing de bits no FOX mask
FOX_PAUSE(0x4000)==FOX_L3 ; FOX_R3(0x8000) compartilha bit do confirm de título. Clicar L3/R3
na special stage injeta bit em 0xC000 → pausa indevida (mesma classe do fix de pulo-pausa).

**FIX NATIVO p/ #1/#2 (o que o NextOS pediu):** trocar a heurística de string por leitura do
ESTADO REAL do engine. Sinais nativos candidatos: `g_gs_main_sys_info` (0x99f658), o estado
do state-machine de gameplay, `ss::CMain::s_main` (special) já usado. "Em gameplay de fase" =
existe stage/player ativo. Substituir `sonic_game_started` por essa leitura mata #1, #2 e a
fragilidade toda de uma vez.

---

## ÁUDIO — fluxo nativo + bug #3 (volume)
EP2 usa `AudioHelper` (Java) → nosso `sonic_audio.c` (SDL2). **opensles_shim.c NÃO é usado pelo
EP2** (engine não chama OpenSL) → os achados de concorrência do agente em opensles_shim
(conv_buf static, ring SPSC) são DEAD CODE pro EP2; não priorizar pro EP2.

### Bug #3 (volume 100% Knulli/ROCKNIX) — ROOT CAUSE CONFIRMADO
- ArchR (R36S): SDL abre **pulseaudio**, e o tracking de volume do sistema FUNCIONA
  (log: `sys volume -> 0.80/0.50/0.20/0.05/1.00/0.35` seguindo os botões). OK.
- Knulli/ROCKNIX: pulse/pipewire FALHAM (`pw.loop ... can't make support.system handle`,
  `ALSA: Couldn't open audio device`) → SDL cai no **card CRU "audiocodec"** (sonic_audio.c
  "Plano C"). Com card cru os botões de volume controlam o mixer do OS mas nosso stream
  escreve direto no hw → sempre cheio. O `sa_read_sys_volume` (sonic_audio.c:1006-) lê
  `/var/run/batocera-pending-volume` + `/userdata/system/batocera.conf` — **esses paths não
  existem no ROCKNIX/Knulli** → volume fica no default 0.80 (~"100%").
**FIX NATIVO:** adicionar em `sa_read_sys_volume` os paths/chaves de volume do ROCKNIX e do
Knulli (e/ou ler via `amixer get`/control do card). Não mexer no path pulse (ArchR já OK).

### Achados sonic_audio.c que VALEM pro EP2 (agente)
- #4 cache key: `CacheEntry.key[96]` vs lookup 128/256 → truncamento em 95 → cache miss p/
  chaves longas → re-decode. Latente.
- #5 voice steal: 32 vozes cheias → rouba slot 0 (corta som + órfã o handle). Glitch audível.

### Bug #2 (score preso Oil Desert Act3) — não reproduzido
Hipótese: camada de UI/efeito do mini-game (contador de score do "press A") não é limpa pelo
engine OU é agravada pelos nossos patches LOWFX (`SsDrawObjectShadow*`→0, bloom off) que
desligam draws. Precisa repro on-device pra confirmar. Lead: rodar a fase SEM LOWFX
(SONIC_LOWFX off) e ver se o score some — isola se é nosso patch ou o engine.
**Leads de símbolo (libfox) p/ quando houver repro:** o "apertar A pra score" é o gimmick de
SLOT/reel — `GmGmkSlotInit`@0x392e28, `GmGmkSlotStartRequest`@0x392de4, `GmGmkSlotIsStatus`@0x392e0c,
`GmGmkSlotBuild`@0x393094, **`GmGmkSlotFlush`@0x3930ec (=limpa/destrói o display)**; +`GmGmkStopper*`,
`GmPlayerAddScore`@0x3dc7ac, `OFPostScore`@0x2112ac. **Persistência do score = `GmGmkSlotFlush`
não é chamado ao fim do gimmick** (ou um cleanup pulado). Patches suspeitos: `videoIsPlaying`→0 /
`MediaPlayerisPlaying`→0 / `clMovie::isEnd`→1 (se o clear for disparado por transição/movie).
🔑 **HIPÓTESE NOVA (do usuário): pode ter sido o ERRO DE MAPEAMENTO** — se o slot/score espera um
botão pra dispensar e esse botão tinha bit errado (não registrava), o score ficava preso.
Com o fix de bits (commit 1b1075e) o botão volta a funcionar → **testar se o score-stuck sumiu junto**.
Senão: repro on-device, LOWFX off, e checar se GmGmkSlotFlush roda.

---

## PRÓXIMOS PASSOS (ordem)
1. Build instrumentado: dump runtime de `g_gs_env_key_*` + mask por botão → fecha Y=Left.
2. Fix nativo `sonic_game_started` (ler estado real) → mata bugs agente #1/#2 + ajuda menus.
3. Fix volume Knulli/ROCKNIX em `sa_read_sys_volume` (paths desses CFW / amixer).
4. Repro Oil Desert Act3 c/ e sem LOWFX → score preso.
Device R36S ArchR online; ES rodando; lançar com cuidado (matar sonic antes, regra do device).

## Tester ROCKNIX 2026-07-01 (relato do usuário)

- Sonic 4 EP2 no ROCKNIX: **sem som** e **sem vídeos** (ainda), mesmo após
  s5 (áudio adaptativo 7e263ad + contexto ES3->ES2 3ff310b + volume 72f8fb5).
- Investigar próxima sessão sonic4: ROCKNIX = pipewire 1.4.6 + sway/wayland
  (matriz CFW no bully2/STUDY-RAM-STREAMING.md); driver "pipewire" no SDL do
  ROCKNIX pode abrir e ficar mudo (checar sa_driver_silent/varredura) e o
  caminho de vídeo pode precisar do mesmo tratamento do contexto adaptativo.
