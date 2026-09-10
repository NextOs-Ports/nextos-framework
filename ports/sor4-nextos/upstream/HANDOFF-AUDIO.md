# HANDOFF — ÁUDIO do Streets of Rage 4 (Mali-450 / NextOS)

> Para a PRÓXIMA sessão. Leia também `HANDOFF.md` e a memória
> `project_sor4_streets_of_rage4_mali450`. O **progressor/BYO/render já estão PRONTOS e
> validados** (jogo abre, roda, renderiza). Esta sessão foi 100% **áudio + controle**.

Device: **<device-ip>** (senha <senha>). Install: `/storage/roms/ports/sor4`.
Launcher (o ES lê daqui): `/storage/roms/ports_scripts/StreetsOfRage4.sh`.
Fonte do áudio: `~/nextos_ports_android/ports/sor4wwise/src/{audioout.c, wwise_native.c}`.
Parser de soundbank: `~/nextos_ports_android/ports/sor4/port/tools/wwise_extract.py`.
APK backup (não re-transferir): `/storage/roms/SOR4-keep.apk`.

---

## COMO O ÁUDIO FUNCIONA (arquitetura híbrida)

A `libWwise.so` é um wrapper (so-loader) que carrega a **Wwise real do APK**
(`libWwise.real.so`) SÓ para a LÓGICA (selecionar música por contexto, set_state/switch).
O som de verdade é tocado por um **reimpl OpenAL+opusfile** (`audioout.c`). Dois caminhos:

- **MÚSICA**: a Wwise abre um `.wem` streamed (path em `gameassets/NNN.wem`); o wrapper
  intercepta `AAssetManager_open` → `ao_music_request(path)` → thread de streaming OpenAL
  (24 bufs ~8s, loopa via `op_pcm_seek`).
- **SFX in-bank** (menu/voz/golpe): `post_event(name)` → FNV-1 → `manifest.txt` → `<id>.opus`
  (em `SOR4_AUDIO`). Tocado em 1 de 28 fontes (`ao_post_event`).
- **SFX streamed** (one-shot .wem pequeno): `ao_play_streamed_sfx`.

### Envs (já no baseline do launcher — NÃO mexer sem motivo)
- `SOR4_BANKDIR=gameassets`  ← os `.bnk` vivem aqui. A Wwise real PRECISA achar o `Init.bnk`
  senão `real init=0` e **o áudio inteiro nem liga** (foi o "mudo total" desta sessão).
- `SOR4_AUDIO=audioout`      ← os `.opus` + `manifest.txt` (SFX in-bank). **≠ BANKDIR!**
- `WWISE_TICK_US=33333`      ← pump LIGADO @30Hz (a Wwise dirige as TRANSIÇÕES de música em
  tempo real). Antes era `WWISE_NOPUMP=1` (transição lenta). 30Hz alivia a CPU do decode.
- `SOR4_MUSIC_GRACE=3600`    ← música não para no fechamento de segmento (loopa até pedirem outra).
- `SOR4_SFXGAIN=1.1` (era 0.85; golpe tava baixo) `SOR4_MUSICGAIN=0.6`.
- `music_ids.txt` (em `SOR4_AUDIO`) ← lista de wem-IDs de MÚSICA (HIRC); ver fix 2026-06-18.

---

## ✅ JÁ RESOLVIDO NESTA SESSÃO
1. **Mudo total** = `SOR4_BANKDIR` apontava p/ audioout (sem .bnk). FIX: `=gameassets`.
2. **Música da seleção cortava ~10s** = `ao_music_close` parava após grace de 8s. FIX:
   `SOR4_MUSIC_GRACE=3600` (o reimpl loopa sozinho; só troca quando o jogo pede OUTRA faixa).
3. **Controle pulo/menu DOBRADO** = gptokeyb + pad nativo SDL os dois lendo o controle.
   FIX: launcher NÃO roda gptokeyb (pad nativo só; `SOR4_USE_GPTK=1` religa se precisar).
4. **SFX existir** (menu, soco no AR, voz do personagem, música) — OK.

## ✅✅ RESOLVIDO+VALIDADO 2026-06-18 (commit 1dbc6e4) — MÚSICA SOBREPOSTA
> **Validado de OUVIDO**: fase completa, troca de música LIMPA (transições E dentro da
> fase com 2/3 faixas). "a fase toda ficou top, música trocou limpa". CASO FECHADO.

**Sintoma (relatado):** ao trocar de cena (título → seleção → loading → fase) a música da
cena ANTERIOR continua tocando POR CIMA da nova; e DENTRO da fase, quando ela tem 2/3 faixas,
"uma música por cima da outra". A nova deveria começar de imediato (a anterior PARA).

**CAUSA RAIZ:** o roteamento música-vs-SFX no `wwise_native.c` (`AAssetManager_open`) decidia
por **TAMANHO** (`.wem >= 1.5MB = música`). Mas **~302 segmentos de música têm <1.5MB** →
eram classificados como **SFX streamed** e tocados em PARALELO (`ao_play_streamed_sfx`, várias
fontes ao mesmo tempo). A música real usa UMA fonte (`g_mus_src`) que SUBSTITUI a anterior —
mas os segmentos mal-roteados escapavam disso → empilhavam.

**FIX:** roteamento por **HIRC** (preciso), não por tamanho.
 - `wwise_extract.py` gera `music_ids.txt` = wem-IDs referenciados por
   **MusicSegment(0x0A)/MusicTrack(0x0B)/MusicRanSeq(0x0D)** ∩ os `.wem` do gameassets.
   Resultado: **423 wems de música, 0 overlap** com os 50 SFX streamed (Sound 0x02).
 - `wwise_native.c`: `load_music_ids()` carrega a lista (busca binária, ~7KB RAM). No
   `AAsset_open`: ID na lista → `ao_music_request` (fonte única, substitui); senão → **fallback
   por tamanho** (compat. devices sem a lista). Custo desprezível, sem CPU extra.
**Validado:** device loga `[wwise] music_ids carregados: 423` + `pump LIGADO`. Lógica conferida
offline (423 música / 50 SFX / 0 overlap / 302 música <1.5MB = a causa). **FALTA:** ouvir
in-game (a música só abre depois do menu — não dá p/ testar sem navegar). Se sobrar sobreposição
residual, são os ~190 wems FORA do HIRC dos 4 bancos (usam fallback por tamanho).

## 🔎 X5M (.103) — FREEZE de imagem no meio da fase (HDMI off/on, som continua) — HIPÓTESE

NÃO testável agora (X5M offline). Diagnóstico: no Amlogic o link HDMI é compartilhado
áudio+vídeo; uma reconfiguração de áudio (re-open/rate) dispara um **modeset** (HDMI re-sync)
e o **blob Valhall congela o present** (a thread de áudio continua → som segue, imagem trava).
R36S (Bifrost) não trava nisso. **Nosso `audioout.c` abre o device UMA vez (linha 386, sem
reabrir)** → o re-open NÃO vem do nosso reimpl; vem do lado PipeWire/HDMI Amlogic OU de um
modeset de vídeo (mudança de modo/refresh in-stage). A INVESTIGAR no device: `dmesg | grep -i
meson_vout/modeset` correlacionado ao freeze; e se `pw-top` mostra o node de áudio
suspendendo/re-negociando. Fix candidato (LEVE, X5M-only via alsoft.conf): fixar `frequency` do
OpenAL = nativo do HDMI (evita renegociação) + impedir suspend-on-idle do node PipeWire. NÃO
mexer no caminho do Mali (que está bom).

## ✅ RESOLVIDO 2026-06-17 (commit bfba090) — aguarda só ouvido em combate

**FIX (bank v135):** reescrito `wwise_extract.py` + `audioout.c`.
 - Event → segue SÓ ações de **Play** (actionType high-byte == 0x04). Antes seguia
   Stop/SetState e varria a árvore toda.
 - Containers RanSeq(5)/Switch(6)/ActorMixer(7)/Layer(9) → lê a **CHILD-LIST real**
   (`numChildren u32 + numChildren×childID u32`; pega o 1º bloco "N + N ids" cujos ids
   existem no HIRC e são tocáveis — validado contra a playlist/switch-list). Antes o
   scan ingênuo pegava o **DirectParentID** e colhia os IRMÃOS via ActorMixer pai →
   por isso TODO golpe caía no mesmo wem genérico `263540238` ("computando hit").
 - `ao_post_event` → **round-robin** entre variantes (campo `Entry.cur`): cada
   soco/queda/morte soa diferente; eventos de 1 wem (UI/voz) seguem idênticos.

**Resultado:** 794 eventos c/ SFX (321 multi-variante). `hit_heavy`/`hit_ground`/`whoosh`
mapeiam para conjuntos DISTINTOS de wems reais (Ogg Opus 5-8KB). **JÁ DEPLOYADO** no .127
(libWwise.so + wwise_extract.py + audioout regenerado). Boot validado: manifest 794,
`real init=1`, sem crash. **FALTA SÓ:** jogar e confirmar de ouvido.
Se um golpe vier com som de contexto errado (ex: cano em vez de soco) é porque juntamos
TODOS os casos do Switch (não dá p/ saber o switch-value offline) — aceitável p/ variedade.
⚠️ device tem ~268 .opus órfãos da run antiga (inofensivos). bkp: `*.bak-prehit`.

### [HISTÓRICO] Sintoma original que motivou o fix
**Sintoma (relatado):** soco no AR tem som; bater no INIMIGO só dá UM som genérico
"computando hit", IGUAL para todos os personagens; inimigo morrendo/caindo = nada.
Testado com `SOR4_SFXGAIN=1.6` → continua mudo, então **não é ganho**.

**CAUSA RAIZ (confirmada por trace):** o `wwise_extract.py` mapeia vários eventos de
impacto distintos para o **MESMO wem genérico**. No trace (WWISE_TRACE=1):
```
[audioout] SFX OK 'hit_heavy'            wem=263540238
[audioout] SFX OK 'hit_ground'           wem=263540238
[audioout] SFX OK 'Play_fight_prepunch_whoosh' wem=263540238   <- todos no MESMO wem!
[audioout] SFX OK 'Play_vx_player_blaze_punch'  wem=922087847   (voz do soco = OK)
[audioout] SFX OK 'Play_vx_ennemy_..._deathscream' wem=1009379495
```
O `263540238` é o "computando hit" genérico que se ouve. Os sons REAIS (variados,
por golpe/personagem) estão em containers **RanSeq(0x05)/Switch(0x06)** que o parser
**NÃO resolve direito**: `collect_wems_for_object` (em wwise_extract.py) varre o corpo do
container por QUALQUER u32 que seja id do HIRC (scan ingênuo) → pega um filho genérico,
não a child-list real → o evento mapeia pro wem errado.

**O QUE FAZER:** reescrever a resolução de containers no `wwise_extract.py` p/ ler a
**child-list REAL** do RanSeq/Switch (estrutura binária do HIRC, **bank version = 135**):
- Sound 0x02: `parse_sound_source` já OK.
- **RanSeq 0x05 / Switch 0x06 / ActorMixer 0x07**: parsear o NodeBaseParams + a lista de
  filhos de verdade (Children list: `numChildren`(u32) + `numChildren × childID`(u32)) em
  vez do scan por u32. Pegar a estrutura exata da v135 (NodeInitialParams/PositioningParams/
  AuxParams/AdvSettings/StateChunk/RTPC vêm ANTES da Children list — precisa pular tudo).
- Depois, o reimpl (`ao_post_event`, audioout.c) toca `e->ids[0]`; p/ RanSeq, idealmente
  o manifest deveria ter TODAS as variantes e o reimpl sortear uma (variedade de golpe).
- Validar com 1 passada de combate + WWISE_TRACE=1: cada hit deve mapear p/ wem DIFERENTE.

⚠️ Possível 2ª frente: alguns impactos podem ser **.wem STREAMED** (não in-bank). Conferir
no trace se ao bater o jogo ABRE um .wem pequeno (deveria cair em `ao_play_streamed_sfx` —
não apareceu "STREAMED-SFX" no log do combate). Se sim, o problema é o roteamento
music-vs-sfx do `AAsset_open` em `wwise_native.c` (threshold `SOR4_MUSIC_MINSIZE`).

### 2) Transição de música LENTA (seleção → loading → fase) — trade-off do pump
Com `WWISE_NOPUMP=1` (necessário p/ não pesar a CPU), a Wwise não dirige as transições em
tempo real → a troca de faixa demora. **A TESTAR:** `pump ON` (tirar WWISE_NOPUMP) + manter
`SOR4_MUSIC_GRACE=3600` — talvez o grace já evite o corte no combate (que antes era o motivo
do pump-off) e o pump-on devolva a transição rápida. Se pump-on cortar a música no combate
pesado (decode duplicado/RAM 832MB), aí volta pump-off. Medir os dois.

---

## COMO TESTAR / DEBUGAR (receita)
Lançar o jogo por ssh com trace (precisa PARAR o ES direito senão tela preta):
```sh
ssh root@<device-ip>
W=/storage/roms/ports/sor4; PKG=$W/host_pkg
systemctl stop emustation; sleep 3; pkill -9 sor4host
export LD_LIBRARY_PATH=$PKG/libs:/usr/lib:/lib SDL_NO_SIGNAL_HANDLERS=1 DOTNET_EnableWriteXorExecute=0
export SOR4_ASSETS=$W/gameassets SOR4_BANKDIR=$W/gameassets SOR4_AUDIO=$W/audioout
export WWISE_REAL=$PKG/libs/libWwise.real.so WWISE_LOG=$W/wwise.log WWISE_NOPUMP=1 SOR4_MUSIC_GRACE=3600
export WWISE_TRACE=1   # << loga post_event/SFX/STREAMED/MUSICA
export SDL_GAMECONTROLLERCONFIG="0300605b100800000100000010010000,USB Gamepad,a:b2,b:b1,x:b3,y:b0,..."
cd $PKG; ./sor4host > $W/log.txt 2>&1 &
```
Jogar; ler `$W/wwise.log`:
- `grep 'SFX OK\|NAO no manifest\|STREAMED-SFX' wwise.log` → o que dispara no combate.
- `grep 'MUSICA tocando\|set_state.*music_manager' wwise.log` → música/transições.
- `grep 'real init=\|init OK' wwise.log` → áudio ligou? (real init=1 obrigatório).

Rebuild da libWwise.so (se mexer no audioout.c/wwise_native.c): ver `ports/sor4wwise/`
(toolchain aarch64; o .so vai p/ `host_pkg/libs/libWwise.so`). Rebuild do `wwise_extract.py`
NÃO precisa (é python; roda no device). Regenerar audioout no device:
`python3 $W/tools/wwise_extract.py "$W/gameassets/Core.bnk,$W/gameassets/Generic.bnk,..." $W/audioout`

## VERIFICAÇÃO SEM OUVIR
`pactl list short sink-inputs` (float32le = OpenAL tocando) + os logs acima. Mas SFX de
combate **precisa de ouvido** (é o que a gente está caçando).

## DADOS ÚTEIS
- bank v135; 4 .bnk: Init(didx=0), Core, Generic(didx=516 hirc=1631), Music(didx=0,hirc=0).
- manifest dev atual = 825 eventos / 1709 .opus (em `$W/audioout`). BYO gera ~1059.
- NSFX=28 fontes; MUS_NBUF=24×16384/48000 ≈ 8s; .opus de hit reais (6-24KB, não vazios).
- RAM device: 832MB total, ~89MB livre + swap (apertado mas ok).
