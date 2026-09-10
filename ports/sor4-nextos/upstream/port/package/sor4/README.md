# Streets of Rage 4 — native AArch64 PortMaster port

**Language / Idioma:** [English](#english) · [Português](#português)

This is a BYO-data package. It never includes runnable Streets of Rage 4 game
code/assets, audio, fonts, or Android middleware. The cover, screenshot, and setup
placard are store-style preview metadata identified in `licenses/`; a legally
obtained complete ARM64 Android v1.4.5 source is required for all runnable data.

---

## English

Streets of Rage 4 runs as a native Linux AArch64 process through a self-contained
.NET 9 host and a GLES build of MonoGame. It is not an Android emulator and not a
shared-object loader. The Android managed game assembly is validated, extracted,
patched for the Linux host, and executed directly by CoreCLR.

### Device compatibility

The 2.0 release targets any AArch64 handheld with about 1 GB of RAM, not one reference
device. What that required, all of it verified rather than assumed:

- **Native libraries.** FreeType and HarfBuzz are taken from the firmware whenever it has
  them, including from PortMaster's own `libs` directory, which is where lean firmwares
  keep theirs. Only when a firmware has neither does the port fall back to the
  dependency-free AArch64 pair it ships in `host_pkg/libs/fallback` (built against
  GLIBC 2.17, static libstdc++, no zlib/png/glib/icu/graphite2). Before this, such a
  firmware refused to start the port at all.
- **Graphics API, never a display backend.** The port asks SDL for the OpenGL ES driver
  (`SDL_OPENGL_ES_DRIVER`), because Mesa/Panfrost firmwares otherwise answer an ES request
  with a desktop-GL context whose GLSL ES shaders do not compile. It still names no video
  backend: fbdev, `mali`, KMSDRM and Wayland are all chosen by SDL itself. If the frontend
  exported an `SDL_VIDEODRIVER` that this device's SDL cannot initialise, the port removes
  that variable and lets auto-detection work instead of failing blind.
- **Audio.** The game's `HOME` is redirected to `save/` for its settings, which used to
  hide `~/.asoundrc` — exactly where Knulli, Batocera and muOS define the working PCM and
  the software volume control. The firmware's ALSA configuration is made visible again
  under the new `HOME`, by symlink or, on FAT/exFAT cards that reject symlinks, by copy.
  OpenAL still cascades PulseAudio (including PipeWire-Pulse) then ALSA, forced nowhere.
- **Frontend lifecycle.** `CUR_TTY` comes from the firmware's `control.txt`. If the
  frontend kills the launcher when the user leaves the menu, a trap takes the host down
  with it, so no orphan keeps the framebuffer or DRM master and makes the next launch open
  black. Only processes running this port's own binary are ever touched.

### What this release changes

- First setup now accepts one complete merged APK, every split from one complete
  Play Store installation, or a complete `.apks`, `.apkm`, or `.xapk` bundle. Base,
  ARM64, and data splits are validated as one collision-safe virtual asset tree.
- The public PortMaster launcher is now a thin entry point. The compiled
  `sor4host --starter` owns single-instance locking, profile selection, dependency
  preflight, setup, runtime aliases, controls, child lifecycle, and cleanup.
- Settings and language are persisted locally in `save/Config.txt`; the in-game Quit
  action now completes the normal shutdown and exits the Linux process.
- Silent Wwise timeline markers no longer replace an audible music segment, fixing the
  temporary loss of music around the Stage 2 prison sirens.
- Existing 2.0 RC installations are upgraded atomically without requiring the APK or
  rebuilding assets.
- Automatic `1gb`, `2gb`, and `high` profiles based on physical RAM. Zram is an
  emergency safety net and is deliberately not counted as physical memory.
- A real GPU capability probe: ES3 is attempted first, with ES2 fallback. GPU and
  firmware names are never used as capability guesses.
- Full-resolution native ASTC on supported GPUs. No texture conversion is done on
  that path.
- ETC2 bake for ES3 devices without ASTC and ETC1 bake for ES2/Mali-450-class
  devices. Low-memory conversion uses one worker and resumes after interruption.
- Bounded texture streaming. Cold GPU allocations are replaced by 1x1 placeholders;
  their level-0 payload is streamed again from the original XNB/LZ4 file before the
  current draw uses it. Native assets are not duplicated into a giant swap file.
- Transactional first-run setup with a lock, resumable staging, exact data
  validation, atomic commit, and rollback after an interrupted commit.
- The original APK is preserved. The installer never silently deletes user data.
- SDL video remains automatic for fbdev, KMSDRM, and Wayland. Audio remains automatic
  through PulseAudio (including PipeWire-Pulse) with an ALSA fallback.

### Profiles

| Profile | Physical RAM | Texture budget | Minimum paged texture | Bake fallback | Workers |
|---|---:|---:|---:|---|---:|
| `1gb` | reported `MemTotal` below ~1.22 GiB | 80 MiB | 16 KiB | ETC2 1/2 or ETC1 1/3 | 1 |
| `2gb` | ~1.22–2.44 GiB | 144 MiB | 32 KiB | full ETC2 or ETC1 1/2 | 2 |
| `high` | ~2.44 GiB and above | 320 MiB | 48 KiB | full resolution | up to 4 |

Native ASTC keeps full texture resolution in every automatic profile. The budget
limits residency, not source quality. Copy `sor4.cfg.default` to `sor4.cfg` to set
`profile`, `texture_streaming`, `texture_budget_mb`, `texture_quality`, or diagnostic
streaming logs. The package never overwrites `sor4.cfg`.

### First-run installation

1. Put one legal Android v1.4.5 source in `ports/sor4/gamedata/` (or directly in
   `ports/sor4/`): either one complete merged ARM64 APK, every APK returned for the
   same complete Play Store installation, or one complete `.apks`, `.apkm`, or
   `.xapk` export. A lone base or ABI split is not enough.
2. Launch Streets of Rage 4 from Ports.
3. The installer validates the complete split set against the exact v1.4.5
   fingerprints. It accepts either all 25,905 assets or the exact supported tree
   missing one XNB texture; code, libraries, all non-XNB data, and all 613 WEM remain
   mandatory. It then extracts into staging, patches `SOR4.dll`, and prepares in-bank audio.
4. Only a fully validated stage is committed. If power is lost, launch again to
   resume or roll back safely.

For the one-missing-XNB compatibility case, SHA-256 sum/XOR/square accumulators prove
that the remaining tree is the official v1.4.5 set minus one record. If the game asks
for it, the bridge returns the APK's own validated `blank.xnb` and logs
`[asset COMPAT]`. This fallback is never enabled for a complete installation.

Before extraction or bake starts, the launcher/runtime helper verifies the bundled audio libraries
and the firmware's versioned or unversioned FreeType/HarfBuzz libraries. A firmware missing
either system library stops immediately with a clear error instead of failing after
a long conversion.

The installer measures free space before writing. A bundle needs additional temporary
space equal to its inner APKs while setup is active; that resumable cache is deleted
after the validated commit. After the legal source has been copied, the port's
conservative minima are about 2.1 million KiB for native ASTC, 3.2 million KiB for
full ETC2, and 4.5 million KiB for full ETC1; half/third fallbacks need less. Keep
5 GiB free after copying the APK to cover every automatic recipe and forced full ETC1.
ASTC avoids conversion but still extracts the complete validated asset tree.

### Architecture

```text
PortMaster launcher
  -> GLES/ASTC probe + physical-RAM profile
  -> transactional BYO-data setup (first run only)
  -> compiled starter (SDL2/SDL3, native deps, save/audio environment, lifecycle)
  -> self-contained .NET 9 AArch64 host
  -> patched MonoGame GLES2/GLES3
  -> patched SOR4 managed assembly
  -> XNB-backed bounded texture pager
```

The pager stores metadata per `Texture2D`, not decoded art. For native ASTC and
pre-baked ETC textures, a page fault reopens the original XNB, stream-decompresses
LZ4 only as far as level 0, uploads it on the render thread, and trims the least
recently used resident texture. Runtime-converted edge cases use a small fallback
page pack.

### Controls and audio

The game reads SDL GameController input directly; `gptokeyb` is disabled by default
to avoid double input. Set `SOR4_USE_GPTK=1` only on a firmware that needs keyboard
mapping. The usual layout is A attack, B jump, X special, Y pickup/run, Start menu,
and D-pad or analog movement. Quit exits through the game's own menu; Select+Start is
also available on supported mappings.

Music, voices, streamed effects, and embedded-bank effects use the port's Wwise/OpenAL
bridge. The launcher/runtime helper selects no fixed SDL audio driver. `alsoft.conf` lists automatic
PulseAudio (including PipeWire-Pulse) and ALSA fallback order. Portable AArch64 OpenAL and Opus
fallbacks are bundled; FreeType/HarfBuzz versioned system libraries are resolved
through writable runtime aliases before the game starts.

### Building and packaging

The release ZIP is built from a strict allowlist and contains no APK, extracted game
assembly, Wwise runtime, game assets, raw logs, cache, or local test information.

```bash
ports/sor4/port/tools/build-native-tools.sh
ports/sor4/port/tools/build-managed-release.sh
ports/sor4/package/build-package.sh
```

`build-native-tools.sh` produces AArch64 probe/splash binaries with a GLIBC 2.17
ceiling. The package builder checks architecture, ABI ceiling, metadata schema,
scripts, Python syntax, proprietary-data exclusions, deterministic timestamps,
manifest hashes, ZIP order, and reproducibility.

### Source map and licenses

- `port/host/` — compiled starter and self-contained CoreCLR game/tool host.
- `port/monogame-gles-patches/` — GLES, ASTC, compatibility, and pager patches.
- `port/tools/texconv/` — low-memory ETC1/ETC2 XNB bake.
- `port/package/tools/sor4_setup.sh` — transactional first-run installer.
- `port/package/tools/sor4_profile.sh` — safe config parser and memory profiles.
- `port/package/tools/sor4_apkextract.py` — streaming APK/assembly-store extractor.
- `wwise-native/` — Wwise/OpenAL compatibility bridge.

Port code is Apache-2.0. MonoGame is Ms-PL/MIT, .NET and Mono.Cecil are MIT, SDL is
zlib, and LZ4 is BSD-2-Clause. Full notices are in `licenses/`. Streets of Rage 4,
its data, and extracted Android middleware remain the property of their owners and
are supplied only by the user.

---

## Português

Streets of Rage 4 roda como processo Linux AArch64 nativo por um host .NET 9
self-contained e uma build GLES do MonoGame. Não é emulador Android nem so-loader.
O assembly gerenciado do jogo é validado, extraído, adaptado para o host Linux e
executado diretamente pelo CoreCLR.

### Compatibilidade de aparelhos

A versão 2.0 mira qualquer portátil AArch64 de ~1 GB de RAM, e não um aparelho de
referência. O que isso exigiu, tudo verificado e não presumido:

- **Bibliotecas nativas.** FreeType e HarfBuzz vêm do firmware sempre que ele as tem —
  inclusive do diretório `libs` do próprio PortMaster, onde firmwares enxutos guardam as
  suas. Só quando o firmware não tem nenhuma das duas o port cai no par AArch64 sem
  dependências que ele embarca em `host_pkg/libs/fallback` (GLIBC 2.17, libstdc++ estática,
  sem zlib/png/glib/icu/graphite2). Antes disso, um firmware assim recusava o port antes
  mesmo de abrir.
- **API gráfica, nunca backend de tela.** O port pede ao SDL o driver OpenGL ES
  (`SDL_OPENGL_ES_DRIVER`), porque firmwares Mesa/Panfrost respondem a um pedido de ES com
  um contexto desktop-GL cujos shaders GLSL ES não compilam. Ele continua não nomeando
  backend de vídeo nenhum: fbdev, `mali`, KMSDRM e Wayland são escolha do próprio SDL. Se o
  frontend exportou um `SDL_VIDEODRIVER` que o SDL deste aparelho não inicializa, o port
  **remove** a variável e deixa a auto-detecção resolver, em vez de falhar às cegas.
- **Áudio.** O `HOME` do jogo é redirecionado para `save/` por causa das configurações, e
  isso escondia o `~/.asoundrc` — justamente onde Knulli, Batocera e muOS definem o PCM que
  funciona e o volume por software. A configuração ALSA do firmware volta a ficar visível
  no novo `HOME`, por link ou, em cartões FAT/exFAT que recusam links, por cópia. O OpenAL
  continua na cascata PulseAudio (incluindo PipeWire-Pulse) e depois ALSA, sem forçar nada.
- **Ciclo com o frontend.** O `CUR_TTY` vem do `control.txt` do firmware. Se o frontend
  mata o launcher quando o usuário sai do menu, um `trap` leva o host junto, para que nenhum
  órfão fique segurando o framebuffer ou o DRM master e faça a próxima abertura ficar preta.
  Só processos do binário deste port são tocados.

### O que esta versão muda

- A primeira instalação agora aceita APK completo mesclado, todos os splits de uma
  instalação completa da Play Store ou bundle `.apks`, `.apkm` ou `.xapk`. Base,
  ARM64 e dados formam uma única árvore virtual com detecção de conflitos.
- O launcher público agora é apenas a entrada PortMaster. O modo compilado
  `sor4host --starter` cuida de single-instance, perfil, dependências, setup, aliases,
  controles, ciclo do processo e limpeza.
- Configurações e idioma ficam em `save/Config.txt`; Quit conclui o shutdown normal e
  encerra o processo Linux.
- Marcadores de linha de tempo Wwise sem áudio não substituem mais uma música audível,
  corrigindo o silêncio temporário nas sirenes da prisão da fase 2.
- Instalações do 2.0 RC migram atomicamente, sem exigir novamente o APK nem refazer assets.
- Perfis automáticos `1gb`, `2gb` e `high` baseados na RAM física. A zram é só rede
  de segurança e não é somada à RAM física.
- Sonda real de GPU: tenta ES3 e cai para ES2. O port não adivinha capacidade pelo
  nome da GPU ou do firmware.
- ASTC nativo em resolução cheia nas GPUs compatíveis, sem conversão de texturas.
- Bake ETC2 para ES3 sem ASTC e ETC1 para ES2/Mali-450. Em pouca RAM a conversão usa
  um trabalhador e continua de onde parou após queda de energia.
- Streaming limitado de texturas. Alocações frias viram placeholders 1x1 e voltam
  diretamente do XNB/LZ4 original quando usadas; não existe cópia gigante dos assets.
- Instalação transacional com lock, staging retomável, validação exata, commit atômico
  e rollback de commit interrompido.
- O APK original é preservado. O instalador não apaga dados do usuário silenciosamente.
- O vídeo SDL continua automático entre fbdev, KMSDRM e Wayland. O áudio usa
  PulseAudio (inclusive PipeWire-Pulse) automaticamente, com fallback ALSA.

### Perfis

| Perfil | RAM física | Orçamento de textura | Textura paginada a partir de | Fallback de bake | Workers |
|---|---:|---:|---:|---|---:|
| `1gb` | `MemTotal` abaixo de ~1,22 GiB | 80 MiB | 16 KiB | ETC2 1/2 ou ETC1 1/3 | 1 |
| `2gb` | ~1,22–2,44 GiB | 144 MiB | 32 KiB | ETC2 cheio ou ETC1 1/2 | 2 |
| `high` | ~2,44 GiB ou mais | 320 MiB | 48 KiB | resolução cheia | até 4 |

Com ASTC nativo, todos os perfis automáticos mantêm a resolução original. O orçamento
limita o que fica residente, não a qualidade da fonte. Copie `sor4.cfg.default` para
`sor4.cfg` para alterar perfil, streaming, orçamento, qualidade ou logs. Atualizações
do pacote nunca sobrescrevem `sor4.cfg`.

### Primeira execução

1. Coloque uma fonte legal Android v1.4.5 em `ports/sor4/gamedata/` (ou diretamente
   em `ports/sor4/`): um APK ARM64 completo mesclado, todos os APKs retornados para
   a mesma instalação completa da Play Store, ou um bundle `.apks`, `.apkm` ou
   `.xapk` completo. Um split base ou de ABI sozinho não basta.
2. Abra o jogo em Ports.
3. O instalador confere fingerprints exatos da v1.4.5, extrai assets e assemblies
   necessários para uma área de staging, corrige `SOR4.dll` e prepara o áudio. Ele aceita
   os 25.905 assets completos ou a árvore oficial menos uma única textura XNB; código,
   bibliotecas, todos os não-XNB e os 613 WEM continuam obrigatórios.
4. Apenas um staging totalmente validado é instalado. Se faltar energia, abra de
   novo para continuar ou restaurar o estado anterior.

Na compatibilidade de uma XNB ausente, acumuladores SHA-256 de soma/XOR/quadrado provam
que todo o restante é o conjunto oficial v1.4.5 menos um registro. Se o jogo pedir essa
textura, o bridge devolve o `blank.xnb` validado do próprio APK e registra
`[asset COMPAT]`. Instalações completas nunca ativam esse fallback.

Antes de extrair ou converter, o launcher/helper de runtime confere as bibliotecas de áudio do pacote
e as bibliotecas FreeType/HarfBuzz, versionadas ou não, do firmware. Se alguma biblioteca
do sistema faltar, ele para imediatamente com erro claro, antes do bake demorado.

O instalador mede o espaço antes de escrever. Um bundle exige espaço temporário igual
aos APKs internos enquanto o setup está ativo; o cache retomável é removido depois do
commit validado. Depois de copiar a fonte legal, os mínimos conservadores do port
são cerca de 2,1 milhões de KiB para ASTC, 3,2 milhões para ETC2 cheio e
4,5 milhões para ETC1 cheio; os fallbacks 1/2 e 1/3 usam menos. Mantenha 5 GiB livres
para cobrir qualquer receita automática e ETC1 cheio forçado. ASTC evita a conversão,
mas ainda extrai a árvore completa de assets validada.

### Arquitetura

```text
launcher PortMaster
  -> sonda GLES/ASTC + perfil pela RAM física
  -> setup BYO-data transacional (somente na primeira vez)
  -> starter compilado (SDL2/SDL3, dependências, save/áudio e lifecycle)
  -> host .NET 9 AArch64 self-contained
  -> MonoGame GLES2/GLES3 corrigido
  -> assembly gerenciado SOR4 corrigido
  -> pager limitado que relê os XNB originais
```

O pager guarda metadados, não arte decodificada. Num page fault ele reabre o XNB,
descomprime LZ4 em fluxo até o nível 0, envia a textura antes do desenho atual e remove
a residente menos usada. Isso evita exibir o placeholder por um quadro. Só casos
convertidos em runtime usam o page pack de fallback.

### Controles e áudio

O gamepad é lido diretamente pelo SDL; `gptokeyb` fica desligado para não duplicar
entrada. Use `SOR4_USE_GPTK=1` apenas se o firmware exigir teclado. Layout usual:
A ataque, B pulo, X especial, Y pegar/correr, Start menu e direcional/analógico para
movimento. Quit sai pelo menu normal do jogo; Select+Start também funciona nos
mapeamentos compatíveis.

Música, vozes e efeitos usam o bridge Wwise/OpenAL do port. O launcher/helper não força
driver SDL de áudio; `alsoft.conf` tenta PulseAudio (inclusive PipeWire-Pulse) e ALSA.
Fallbacks AArch64 de OpenAL e Opus vêm no pacote; FreeType/HarfBuzz versionadas do
CFW são resolvidas por aliases graváveis antes de iniciar o jogo.

### Build, mapa de fontes e licenças

```bash
ports/sor4/port/tools/build-native-tools.sh
ports/sor4/port/tools/build-managed-release.sh
ports/sor4/package/build-package.sh
```

O ZIP usa allowlist estrita e exclui APK, código/dados extraídos, Wwise proprietário,
logs, caches e informações locais de teste. O builder valida arquitetura, GLIBC,
schema, scripts, Python, manifesto, ordem e reprodutibilidade.

- `port/host/`: starter compilado e host CoreCLR do jogo/ferramentas.
- `port/monogame-gles-patches/`: GLES, ASTC, compatibilidade e streaming.
- `port/tools/texconv/`: bake ETC1/ETC2 de baixa memória.
- `port/package/tools/`: perfil, validação, extração e setup transacional.
- `wwise-native/`: bridge de áudio Wwise/OpenAL.

O código do port é Apache-2.0. MonoGame é Ms-PL/MIT, .NET e Mono.Cecil são MIT,
SDL é zlib e LZ4 é BSD-2-Clause. Os avisos completos ficam em `licenses/`. O jogo,
seus dados e middleware Android extraído continuam pertencendo aos respectivos donos
e são fornecidos exclusivamente pelo usuário.
