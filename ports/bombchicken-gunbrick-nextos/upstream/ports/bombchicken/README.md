# Bomb Chicken v44 — universal compatibility port (AArch64 / Unity IL2CPP)

**Language / Idioma:** [English](#english) · [Português](#português)

Bomb Chicken is proprietary software. This repository and the public release
contain the free compatibility loader and integration only. A lawful Android
copy of the game is required; no game executable, library, artwork, audio or
asset is redistributed.

## English

### Community

Questions, device reports and bug reports:
<https://discord.gg/DHfY62eDNN>

### Status and support boundary

The original 1.0 port was physically proven playable on a NextOS AArch64
Mali-450/GLES2 handheld: image, audio, controller, pause/resume, level
progression and orderly exit all worked. Version 1.1.7 fixes an ABI mismatch
in the Bomb Chicken adapter's two IL2CPP `PlayerPrefs.GetString` overloads.
The bug was hidden by populated saves but returned the hidden `MethodInfo*` as
a managed string when a clean install requested a missing key.

The 1.1.7 loader was physically exercised with both states on an authorized
AArch64 Mali-G31 test device: an existing save loaded phase 3; after that save
was moved to a recoverable backup, a clean launch reached group 0, completed
the tutorial, saved progress and loaded `R1G1` without a black screen or a
managed exception. The fix belongs to this opt-in game adapter, not to a
framework default. The deterministic loader requires at most `GLIBC_2.27`,
below the public `GLIBC_2.30` ceiling. This evidence does not claim that every
AArch64 firmware or GPU is supported.

### Architecture

The self-contained nxbootstrap 0.6.8 launcher is the package's only launcher.
It resolves host capabilities, exports only the quirks declared in
`nxport.json`, runs NXExtract as a separate foreground phase, and then starts
`bombchicken-nextos` directly. Its single-instance lock uses Bash `-ef` and
portable numeric `ls`, with no external `stat` dependency. The package contains
no bootstrap companion, deployment receipt, nested `run.sh`, firmware-name
branch or process-table scan.

The compiled adapter remains game-specific. It maps the owner's original
AArch64 `libmain.so`, `libunity.so` and `libil2cpp.so`, supplies the Android/JNI,
EGL/GLES, audio and input surface they actually use, and drives the APK's
observed Android lifecycle in its original order:

1. map and relocate `libmain` → `libunity` → `libil2cpp`;
2. run `libmain` constructors, then its `JNI_OnLoad`;
3. call the registered `NativeLoader.load`, which runs the real `libunity` and
   `libil2cpp` constructors/`JNI_OnLoad` in that order;
4. call `initJni`, surface-created, surface-changed, focus and resume;
5. run the real `nativeRender` loop;
6. on exit, send focus-lost and pause before closing input and audio.

No entry point, constructor or lifecycle phase is skipped. This migration does
not pretend that the proven adapter has already been replaced by every shared
runtime component; the universal framework owns the pre-runtime contract and
launcher boundary, while the Unity compatibility code remains in `src/`.

### Game-specific quirks

All compatibility corrections default off and are enabled only by exact
newline-delimited tokens from the generated contract:

| Contract token | Evidence-backed purpose |
|---|---|
| `game.bombchicken.menu-cursor-v44` | polished right-stick pointer for the v44 touch-only menu; R3 clicks |
| `game.bombchicken.native-pause-v44` | invokes v44 `LevelStart.PausePressed()` and the native resume path |
| `game.bombchicken.nonnull-play-games-v44` | supplies a live JNI object where v44 Play Games code dereferences an optional service |
| `game.bombchicken.playerprefs-getstring-v44` | separates the two exact v44 IL2CPP `GetString` RVAs/ABIs and returns a managed empty string for a missing clean-save key |
| `game.bombchicken.present-alpha-one` | preserves visible RGB on compositors that blend the default framebuffer by alpha |
| `game.bombchicken.progress-parser-v44` | safely handles malformed/trailing `Progress` records at level completion |
| `game.bombchicken.stencil8-v44` | supplies the stencil format requested by the proven v44 render path |
| `adapter.gl-provider-probe-init-reexec` | re-executes with a coherent EGL/GLES provider only after the system provider fails its real initialization probe |

Graphics selection is capability-first. The adapter tries an SDL-owned GLES2
context and uses the raw EGL path only when that probe fails or an explicit
diagnostic override requests it. It does not select a path by firmware, device
or GPU name.

### Controls

| Control | Action |
|---|---|
| D-pad / left stick | move |
| A | confirm in menus; place a bomb in a level |
| B | place a bomb |
| Start | start from the title; pause/resume in a level |
| Right stick | move the pointer in touch-only menus |
| R3 | click the menu pointer |
| Select + Start | orderly lifecycle exit |

Controller mappings come from the host framework/SDL. The port does not ship a
device GUID or controller-name override. A capability-based raw-joystick and
evdev fallback remains for handhelds whose real controls are not exposed by
SDL's controller database.

### Owner-provided data with NXExtract 1.2.6

Only **Bomb Chicken Android v44 / build 45**, package
`com.nitrome.bombchicken`, ABI `arm64-v8a`, APK size **133,951,858 bytes** and
APK SHA-256
`0501f71e90412502dfc7c74a0d81adbe822daa3c11fe8f667c0e4e9e6016b32b`
matches the accepted evidence. Place your lawful APK in the installed port's
`gamedata/` directory and start the visible launcher. NXExtract 1.2.6 validates
the package, all three native-library hashes and the complete Unity asset-tree
fingerprint before it atomically installs `assets/` and `lib/`.

Do not unpack files manually and do not add them to the source or public ZIP.
The accepted payload evidence and unmodified NXExtract hashes are documented
in `nxextract-version.txt`; the executable recipe is `extractor.json`.

### Build, test and release

The public loader is built inside a pinned, offline Debian Buster image. The
target sysroot is mounted read-only for SDL2/EGL/GLES headers only; no
high-glibc target library is linked into the output.

```sh
cd ports/bombchicken
./build.sh
./tests/run-host.sh
```

The gate performs the exact NXExtract recipe check, contract/privacy checks,
GCC and Clang ASan/UBSan tests for both `GetString` ABIs (including a missing
clean-save key), two clean builds and a byte-for-byte reproducibility
comparison. It audits every project-built ELF for architecture, interpreter,
`DT_NEEDED`, RPATH/RUNPATH and symbol-version requirements. It never launches
the game or touches a device. Public packaging is produced only by the
deterministic NXRelease recipe after framework-generated artifacts are pinned.

Useful opt-in diagnostics include `BC_VERBOSE=1`, `BC_LOGCAT=1`,
`BC_JNILOG=1`, `BC_GLLOG=1`, `BC_AUDIO_TRACE=1`, `BC_FPS=1` and the bounded
frame limit `BC_FRAMES=N`. They are not enabled by the public contract. The
deterministic public bundle is audited and re-opened by NXRelease 0.2.6.

### Source map

- `src/main.c` — contract-to-quirk bridge and exact Unity lifecycle.
- `src/nx_elf.c` — AArch64 Android ELF mapping, relocation and init arrays.
- `src/jni.c` / `src/android.c` — JNI and Android service compatibility.
- `src/egl.c` / `src/egl_sdl.c` — EGL/GLES2 surface, presentation and ETC2 hooks.
- `src/input.c` — SDL/Android input bridge and exact v44 gameplay corrections.
- `src/playerprefs_fix.c` — separately typed/tested v44 `GetString` overloads.
- `src/audio.c` — Unity FMOD/OpenSL-to-SDL audio path.
- `nxport.json` — universal capability and quirk contract.
- `extractor.json` — exact BYO-data recipe.
- `tests/` — process-free host contract, extraction and ELF gates.

### License and acknowledgements

The compatibility code and integration are GPL-3.0-only; see `LICENSE` and
`NOTICE.md`. NXExtract 1.2.6 is MIT-licensed; see
`licenses/NXExtract-MIT.txt`. Upstream interoperability acknowledgements are
recorded in `NOTICE.md`. Bomb Chicken and all owner data remain the property of
their respective rightsholders. This independent project is not affiliated
with or endorsed by Nitrome, Unity or Google.

---

## Português

### Comunidade

Dúvidas, relatos de aparelho e bugs:
<https://discord.gg/DHfY62eDNN>

### Estado e limite de suporte

O port 1.0 original foi comprovado fisicamente como jogável em um portátil
NextOS AArch64 com Mali-450/GLES2: imagem, áudio, controle, pausa/retomada,
progresso de fase e saída ordenada funcionaram. A versão 1.1.7 corrige uma ABI
errada entre os dois overloads IL2CPP de `PlayerPrefs.GetString` no adapter do
Bomb Chicken. Saves preenchidos escondiam o erro; num save limpo, uma chave
ausente fazia o hook devolver o `MethodInfo*` oculto como string managed.

O loader 1.1.7 foi exercitado fisicamente nos dois estados em um aparelho de
teste AArch64 Mali-G31 autorizado: o save existente abriu a fase 3; depois de
movê-lo para backup recuperável, a abertura limpa chegou ao grupo 0, concluiu
o tutorial, gravou progresso e carregou `R1G1`, sem tela preta nem exceção
managed. O conserto pertence ao adapter opt-in deste jogo, não ao default do
framework. O loader determinístico exige no máximo `GLIBC_2.27`, abaixo do
teto público `GLIBC_2.30`. Isso não declara suporte a todo firmware AArch64 ou
toda GPU.

### Arquitetura

O nxbootstrap 0.6.8 autocontido é o único launcher do pacote. Ele resolve as
capacidades do host, exporta somente os quirks declarados em `nxport.json`, roda
o NXExtract numa fase separada em foreground e depois chama diretamente
`bombchicken-nextos`. O lock de instância usa `-ef` do Bash e `ls` numérico
portável, sem depender de `stat` externo. O pacote não contém bootstrap auxiliar,
receipt de deployment, `run.sh` interno, desvio por firmware nem varredura da
tabela global de processos.

O adaptador compilado continua específico do jogo. Ele mapeia `libmain.so`,
`libunity.so` e `libil2cpp.so` AArch64 originais do dono, oferece a superfície
Android/JNI, EGL/GLES, áudio e input realmente usada e reproduz a ordem nativa:

1. mapear e realocar `libmain` → `libunity` → `libil2cpp`;
2. rodar construtores de `libmain` e depois seu `JNI_OnLoad`;
3. chamar o `NativeLoader.load` registrado, que executa construtores e
   `JNI_OnLoad` reais de `libunity` e `libil2cpp`, nessa ordem;
4. chamar `initJni`, surface-created, surface-changed, foco e resume;
5. executar o loop real de `nativeRender`;
6. na saída, enviar perda de foco e pause antes de fechar input e áudio.

Nenhum entry point, construtor ou estágio nativo é pulado. Esta manutenção não finge
que o adaptador comprovado já foi substituído por todos os módulos
compartilhados: o framework universal controla o contrato e o pré-runtime; o
código de compatibilidade Unity permanece em `src/`.

### Quirks específicos e comprovados

Todas as correções nascem desligadas e só são ativadas pelos tokens exatos,
separados por nova linha, que vêm do contrato gerado:

| Token do contrato | Finalidade comprovada |
|---|---|
| `game.bombchicken.menu-cursor-v44` | seta polida no analógico direito para o menu touch-only v44; R3 clica |
| `game.bombchicken.native-pause-v44` | chama `LevelStart.PausePressed()` v44 e o caminho nativo de retomada |
| `game.bombchicken.nonnull-play-games-v44` | entrega objeto JNI vivo onde o Play Games v44 desreferencia serviço opcional |
| `game.bombchicken.playerprefs-getstring-v44` | separa os dois RVAs/ABIs IL2CPP exatos de `GetString` v44 e devolve string managed vazia para chave ausente no save limpo |
| `game.bombchicken.present-alpha-one` | preserva RGB visível em compositores que misturam o framebuffer padrão pelo alpha |
| `game.bombchicken.progress-parser-v44` | trata registros `Progress` malformados/finais sem quebrar a troca de fase |
| `game.bombchicken.stencil8-v44` | fornece o stencil pedido pelo caminho de render v44 comprovado |
| `adapter.gl-provider-probe-init-reexec` | reexecuta com provider EGL/GLES coerente somente depois de o provider do sistema falhar na inicialização real |

A seleção gráfica é por capacidade. O adaptador tenta um contexto GLES2 de
propriedade do SDL e só usa EGL cru quando a tentativa falha ou quando um
override explícito de diagnóstico pede isso. Nome de firmware, aparelho ou GPU
não decide o caminho.

### Controles

| Controle | Ação |
|---|---|
| Direcional / analógico esquerdo | mover |
| A | confirmar nos menus; botar bomba na fase |
| B | botar bomba |
| Start | iniciar no título; pausar/despausar na fase |
| Analógico direito | mover a seta nos menus touch-only |
| R3 | clicar com a seta |
| Select + Start | sair pela sequência normal de lifecycle |

Os mapeamentos vêm do framework/SDL do host. O port não inclui GUID fixo nem
override por nome de controle. Permanece um fallback por capacidades reais de
joystick/evdev para portáteis que não aparecem na base GameController do SDL.

### Dados do dono com NXExtract 1.2.6

Somente **Bomb Chicken Android v44 / build 45**, pacote
`com.nitrome.bombchicken`, ABI `arm64-v8a`, APK com **133.951.858 bytes** e
SHA-256
`0501f71e90412502dfc7c74a0d81adbe822daa3c11fe8f667c0e4e9e6016b32b`
corresponde à evidência aceita. Coloque seu APK legal em `gamedata/` dentro do
port instalado e abra o launcher visível. O NXExtract 1.2.6 confere pacote,
hashes das três bibliotecas e o fingerprint completo da árvore Unity antes de
instalar `assets/` e `lib/` atomicamente.

Não extraia manualmente e não inclua esses arquivos no código-fonte ou ZIP
público. `nxextract-version.txt` registra a evidência aceita e os hashes do
NXExtract sem patches; `extractor.json` é a receita executável.

### Compilar, testar e publicar

O loader público é compilado numa imagem Debian Buster offline e fixada. O
sysroot do alvo é montado somente-leitura para headers SDL2/EGL/GLES; nenhuma
biblioteca de glibc alta do firmware entra no link.

```sh
cd ports/bombchicken
./build.sh
./tests/run-host.sh
```

O gate valida receita NXExtract, contrato e privacidade, testa com GCC e Clang
mais ASan/UBSan as duas ABIs de `GetString` (incluindo chave ausente/save
limpo), faz dois builds e compara os bytes. Todos os ELFs construídos são
auditados por arquitetura, interpretador, `DT_NEEDED`, RPATH/RUNPATH e versões
de símbolos. Ele não abre o jogo nem acessa aparelho. O pacote público só é
produzido pela receita determinística do NXRelease depois que os artefatos
gerados pelo framework forem fixados.

Diagnósticos opt-in úteis: `BC_VERBOSE=1`, `BC_LOGCAT=1`, `BC_JNILOG=1`,
`BC_GLLOG=1`, `BC_AUDIO_TRACE=1`, `BC_FPS=1` e o limite de teste
`BC_FRAMES=N`. Nenhum deles é ligado pelo contrato público. O bundle público
determinístico é auditado e reaberto pelo NXRelease 0.2.6.

### Mapa do código

- `src/main.c` — ponte contrato→quirks e lifecycle Unity exato.
- `src/nx_elf.c` — mapeamento ELF Android AArch64, realocações e init arrays.
- `src/jni.c` / `src/android.c` — compatibilidade JNI e serviços Android.
- `src/egl.c` / `src/egl_sdl.c` — superfície EGL/GLES2, present e hooks ETC2.
- `src/input.c` — ponte SDL/Android e correções exatas do gameplay v44.
- `src/playerprefs_fix.c` — overloads v44 de `GetString` tipados/testados separadamente.
- `src/audio.c` — caminho de áudio Unity FMOD/OpenSL→SDL.
- `nxport.json` — contrato universal de capacidades e quirks.
- `extractor.json` — receita exata de dados BYO.
- `tests/` — gates host de contrato, extração e ELF sem executar o jogo.

### Licença e créditos

O código de compatibilidade e a integração são GPL-3.0-only; veja `LICENSE` e
`NOTICE.md`. O NXExtract 1.2.6 usa licença MIT; veja
`licenses/NXExtract-MIT.txt`. Créditos das referências de interoperabilidade
estão em `NOTICE.md`. Bomb Chicken e todos os dados do dono continuam sendo
dos respectivos titulares. Este projeto independente não é afiliado nem
endossado por Nitrome, Unity ou Google.
