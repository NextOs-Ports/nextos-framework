# The Amazing Spider-Man 2 1.2.7d / 1.2.8d — universal BYO-data port

**Language / Idioma:** [English](#english) · [Português](#português)

**Package release / Versão do pacote:** 1.1.9

This is an independent clean-room compatibility loader. It does not distribute
the APK, either native game library, OBB files, audio or other executable game
data. The catalogue includes one real gameplay screenshot captured from the
validated port.

## English

The loader preserves the original Android lifecycle on both supported routes:

- the original ARMv7 `libtasm2.so` runs natively through the public
  low-glibc ARMHF loader on NextOS and PortMaster-class systems;
- the physically validated AArch64 NextOS X5M route (`amlogic,s7d` /
  Mali-G310) runs the original Android x86 library through a scoped Box32 host
  and `sdl2-compat`. The packaged compatibility SDL is private to that game
  process; the native NXExtract UI uses the firmware SDL2/KMSDRM stack.

The launcher negotiates resolution, SDL, GLES, controller data and memory
settings at runtime. ARMHF systems retain their native backend selection. On
the external low-glibc ARMHF route only, a failed automatic or inherited
PulseAudio initialization gets one scoped ALSA retry; arbitrary explicit
diagnostic drivers remain untouched. The KMSDRM variables required by the X5M
are confined to its exact hardware route. Before startup the launcher holds a
single-instance lock, stops and confirms stale processes, validates the
selected ELF, interpreter, dependencies and packaged hashes, then runs the
game in the foreground. The original `RestartGame()` contract permits one
controlled relaunch.

### Compatibility

Physical route baselines:

- NextOS R2 on Mali-450, using the ARMHF runtime source retained here;
- ArkOS on R36S / Mali-G31, including the exact public v1.1.8 ARMHF loader and
  its complete clean-profile/reopen sequence;
- muOS on RG 40XX-H, using the low-glibc ARMHF build and the firmware's
  32-bit ALSA, PipeWire and SPA modules. Gameplay, clear audio and clean
  shutdown passed with zero reported audio underruns, missing bytes or
  failures;
- ROCKNIX on RG-DS with Panfrost, Wayland and Mesa 26.1.2. When the inherited
  PulseAudio service was unavailable to the ARMHF process, the scoped ALSA
  retry opened the RK817 output; gameplay, clear audio and clean shutdown
  passed;
- NextOS on AArch64 X5M / Mali-G310 completed 11,034 gameplay frames and a
  6,213-frame reopen, both RC0, with save create/update/reload, physical
  controls and audio passing. This route requires the scoped Box64 profile
  `DYNAREC=1`, `BIGBLOCK=0`, `SAFEFLAGS=2`; experimental eager mode is not
  used.

The v1.1.8 X5M components are deterministic low-glibc rebuilds. Their Box32
host source, downstream patch and safety profile match that physical baseline;
the exact rebuilt host starts under AArch64 QEMU and carries the i386 loader to
its expected owner-library boundary. Those exact X5M bytes were not physically
retested for this release.

The ARMHF route is also structured for other PortMaster-class firmware that
provides an ARM hard-float runtime, SDL2 and GLES2/GLES3. Those other firmware
and device combinations are compatible targets, not claims of physical
testing. The X5M runtime is intentionally rejected on other AArch64 SoCs.

### Install with your own Android data

NXExtract accepts the audited 1.2.7d/1.2.8d inputs documented in
`INSTALLATION.md`:

1. Extract `asm2.zip` at the storage root that contains `ports/`.
2. Put one supported APK in `ports/asm2_127/gamedata/`.
3. For a loose APK, also put the validated companion cache ZIP there, or these
   intact OBB files:
   - `main.12032.com.gameloft.android.ANMP.GloftASHM.obb`
   - `patch.12723.com.gameloft.android.ANMP.GloftASHM.obb`
   - optionally,
     `patch.12438.com.gameloft.android.ANMP.GloftASHM.obb`
4. Launch **The Amazing Spider-Man 2** and let NXExtract finish.

On ROCKNIX/Wayland, the first greater-than-1-GiB preparation can remain black
instead of displaying the NXExtract progress screen. Extraction is still
active: do not power off the device; wait for the game to start. Detailed
progress remains in `debug.log`.

The validated self-contained 1.2.8d installer already carries the two required
expansions and needs no separate cache. It is ARM32/multilib-only; the X5M
route requires a supported APK that contains the exact x86 game library.
External filenames do not matter.

NXExtract validates the supported container and OBB hashes, verifies the exact
native-library bytes, repairs the known damaged ZIP layout, extracts valid
layouts without executing Android installer code, creates the two offline shop
catalogs and validates every output before publishing them together.
If an APK candidate is rejected, the log records its size, SHA-256 and exact
rejection reason without exposing the owner's filename or local path.

The rebuilt APK stores its recovered members without compression. This keeps
the result byte-identical across firmware with different zlib versions while
the full 622-member CRC and SHA-256 verification still runs before publication.

An incomplete, wrong, truncated or corrupt input cannot replace working game
data. The owner's source files are never deleted. Updates publish only
validated runtime outputs, so saves, preferences and cache remain untouched.

The first clean run follows the original sequence: legal disclaimer, update
log, the native cloud-data notice, controls, progressive loading and gameplay.
Release 1.1.8 routes one face-button action for each of the first three
prompts: drawable-aware touch for legal, native HID for the update log, then a
centered touch for the cloud notice. Near-square panels such as 720×720 use
the photographed legal-button row instead of the old 4:3 coordinate. Progress
is migrated from older markers, but only the game's own completed controls
profile is final proof; interrupted setup is offered again after restart. The
update-log step always emits the guest's logical A action, independent of
Nintendo-style A/B face-button labels.

### Controls

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Camera |
| R2 | Jump / hold to swing |
| L2 | Special combo |
| X | Attack |
| B | Web |
| Y | Dodge |
| A | Confirm / next dialogue |
| Start | Pause |
| Select + Start | Save and exit |

On a clean profile, wait for each prompt and press one face button once for
legal, update log and cloud notice. Input then returns to normal handling.

## Português

O loader preserva o ciclo de vida Android original nas duas rotas suportadas:

- a `libtasm2.so` ARMv7 original executa nativamente pelo loader público ARMHF
  de glibc baixa no NextOS e em sistemas da classe PortMaster;
- a rota NextOS AArch64 X5M validada fisicamente (`amlogic,s7d` / Mali-G310)
  executa a biblioteca Android x86 original pelo host Box32 específico e pelo
  `sdl2-compat`. A SDL de compatibilidade empacotada fica restrita ao processo
  do jogo; a interface nativa do NXExtract usa a SDL2/KMSDRM do firmware.

O launcher negocia resolução, SDL, GLES, controles e memória durante a
execução. Sistemas ARMHF mantêm a seleção nativa de backend. Somente na rota
ARMHF externa de glibc baixa, uma falha na inicialização automática ou no
PulseAudio herdado recebe uma tentativa restrita via ALSA; escolhas explícitas
de diagnóstico continuam intactas. As variáveis KMSDRM exigidas pelo X5M ficam
restritas à identificação exata desse aparelho. Antes de iniciar, o launcher
trava uma única instância, encerra e confirma processos residuais, valida ELF,
interpretador, dependências e hashes, e executa o jogo em primeiro plano.

### Compatibilidade

Validação física concluída no NextOS R2 com Mali-450, no ArkOS/R36S com
Mali-G31, no muOS/RG 40XX-H e no ROCKNIX/RG-DS com Panfrost, Wayland e Mesa
26.1.2. No muOS, o launcher usou os módulos ALSA, PipeWire e SPA de 32 bits do
firmware; gameplay, áudio claro e encerramento limpo passaram sem underruns,
bytes ausentes ou falhas de áudio registradas. No ROCKNIX, o PulseAudio herdado
não abriu no processo ARMHF; a tentativa restrita via ALSA abriu a saída RK817,
com gameplay, áudio claro e encerramento limpo aprovados. No NextOS/X5M com
Mali-G310, a rota final completou 11.034 frames de gameplay e 6.213 frames após
reabrir, ambos RC0, com criação/atualização/carga de save, áudio e controle
físico aprovados. Ela exige o perfil Box64 restrito `DYNAREC=1`, `BIGBLOCK=0`,
`SAFEFLAGS=2`; o modo eager experimental não é usado.
A rota ARMHF também foi estruturada para outros firmwares da classe PortMaster
que forneçam runtime ARM hard-float, SDL2 e GLES2/GLES3; esses outros alvos são
compatíveis, mas não são anunciados como testes físicos. A rota X5M é recusada
em outros SoCs AArch64.

### Instalação com seus próprios dados Android

O NXExtract aceita os insumos 1.2.7d/1.2.8d auditados em `INSTALLATION.md`:

1. Extraia `asm2.zip` na raiz do armazenamento que contém `ports/`.
2. Coloque um APK suportado em `ports/asm2_127/gamedata/`.
3. Para um APK solto, coloque também o cache ZIP validado ou estes OBBs
   intactos:
   - `main.12032.com.gameloft.android.ANMP.GloftASHM.obb`
   - `patch.12723.com.gameloft.android.ANMP.GloftASHM.obb`
   - opcionalmente,
     `patch.12438.com.gameloft.android.ANMP.GloftASHM.obb`
4. Abra **The Amazing Spider-Man 2** e aguarde o NXExtract terminar.

No ROCKNIX/Wayland, a primeira preparação de mais de 1 GiB pode permanecer com
a tela preta em vez de mostrar o progresso do NXExtract. A extração continua
ativa: não desligue o aparelho; aguarde o jogo iniciar. O progresso detalhado
permanece em `debug.log`.

O instalador 1.2.8d autocontido validado já traz as duas expansões obrigatórias
e dispensa cache separado. Ele funciona somente em ARM32/multilib; a rota X5M
exige um APK suportado com a biblioteca x86 exata. Nomes externos não importam.

O NXExtract valida os hashes dos contêineres e OBBs suportados, confirma os
bytes exatos das bibliotecas nativas, repara o layout ZIP danificado conhecido,
extrai layouts válidos sem executar o instalador Android, cria os dois
catálogos da loja offline e só publica os resultados depois da validação.
Quando um APK candidato é rejeitado, o log registra tamanho, SHA-256 e motivo
exato sem expor o nome do arquivo do usuário nem seu caminho local.

O APK reconstruído armazena os membros recuperados sem compressão. Assim o
resultado permanece byte a byte idêntico entre firmwares com versões diferentes
de zlib, mantendo a verificação de CRC e SHA-256 dos 622 membros antes da
publicação.

Dados ausentes, de outra versão, truncados ou corrompidos não substituem uma
instalação funcional. Os arquivos-fonte do dono nunca são apagados. A
atualização troca somente resultados de runtime validados, preservando saves,
preferências e cache.

Uma instalação limpa segue a ordem original: termos, log de atualização, aviso
nativo de dados na nuvem, controles, carregamento progressivo e gameplay. Na
versão 1.1.8, uma ação de botão frontal é tratada para cada um dos três avisos:
toque ajustado ao drawable nos termos, HID nativo no log e toque centralizado
no aviso de nuvem. Telas quase quadradas, como 720×720, usam a linha real do
botão fotografado em vez da antiga coordenada 4:3. Marcadores antigos são
migrados e uma sequência interrompida reaparece até o jogo criar o perfil de
controles. No log de atualização, a ação enviada é sempre o A lógico do jogo,
independentemente das etiquetas A/B do controle.

Em um perfil novo, aguarde cada tela e pressione uma vez um botão frontal nos
termos, no log e no aviso de nuvem. Depois, os controles voltam ao normal.

## Licenses / Licenças

The compatibility loader and its helpers are GPL-3.0. NXExtract and Box64 are
MIT; `sdl2-compat` uses the zlib license. Full texts and notices are under
`licenses/`. The adjacent corresponding-source archive reproduces all bundled
GPL/MIT/zlib components. The game and owner-supplied data remain separate
proprietary works of their respective rightsholders.
