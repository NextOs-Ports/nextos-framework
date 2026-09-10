# Stardew Valley v1.1.6 (NextOS) — como instalar

Port do **Stardew Valley de Android** rodando direto no Linux do seu portátil, por um
so-loader próprio: sem Android, sem emulador, sem container.

Este pacote traz **só o programa**. Os dados do jogo saem do **seu pacote Android legal**
— nada de conteúdo do jogo é distribuído aqui.

---

## 1. Copie os arquivos

O zip já está no formato do aparelho: **extraia na raiz da sua pasta de ROMs** e pronto.

| Sistema | Extraia em |
|---|---|
| ArkOS, ROCKNIX, muOS, Knulli, batocera | `/roms` |
| EmuELEC / NextOS | `/storage/roms` |

Fica assim:

```
roms/ports/Stardew Valley (NextOS).sh
roms/ports/sdvnextos/…
roms/ports_scripts/Stardew Valley (NextOS).sh
```

> As duas cópias do launcher são de propósito: o EmulationStation do EmuELEC/NextOS lê de
> `ports_scripts/`, e o ArkOS/ROCKNIX/PortMaster lê de `ports/`. Deixe as duas — quem não
> usa uma, ignora.

Desde a v1.1.3 essas duas entradas são wrappers PortMaster mínimos e idênticos. A lógica
multi-device existe uma única vez em `ports/sdvnextos/run.sh`, evitando um launcher
visível enorme e divergências futuras. Na v1.1.4 esse runtime também deixou de mexer no
frontend: parar e devolver o EmulationStation é tarefa do PortMaster e do lançador de
ports de cada sistema, não do jogo. A atualização pode ser instalada por cima e não refaz
os dados nem altera saves.

## 2. Ponha o seu pacote Android

Copie o pacote do jogo para dentro de `roms/ports/sdvnextos/gamedata/`:

| pacote | versão | ABI |
|---|---|---|
| `com.chucklefish.stardewvalley` | **1.6.15.3** | arm64-v8a |

Precisa conter **arm64-v8a** — a versão somente 32 bits não serve. O NXExtract 1.1.2
aceita APK, APKM, APKS, XAPK e ZIP de bundle.

## 3. Abra pelo menu de Ports

Na primeira vez, o NXExtract mostra o progresso, valida a origem e prepara os dados numa
transação segura (~420 MB, alguns minutos). Da segunda em diante, um marcador verificado
abre o jogo diretamente. Depois da confirmação de sucesso, você pode remover o pacote de
origem para liberar espaço.

Quem já usava o v1.0 pode instalar por cima: o novo preparador migra o assembly store e
adota os dados existentes sem apagar ou reescrever os saves.

Se a v1.1.0 parou em 65% no ROCKNIX com erro de `liblz4.so`, instale esta versão por
cima e abra de novo. Os 394 MB já validados são retomados; a v1.1.2 leva seu próprio LZ4
AArch64 compatível com GLIBC 2.17.

**Sair do jogo: SELECT + START.**

---

## Onde já foi testado

| Aparelho | Estado |
|---|---|
| Mali-450 / Amlogic (EmuELEC) | jogável, save real — é a bancada onde o port nasceu |
| R36S / ArkOS (RK3326, Mali-G31) | jogável; teclado, scroll, áudio, vídeo e save validados em 01/08/2026 |
| RG 40XX-H / muOS | comunidade confirmou funcionamento excelente na v1.1.1 |
| RG-DS / ROCKNIX (Panfrost, Wayland) | correção de tela preta na v1.1.2; aguardando reteste do relator |

O runtime interno escolhe entre dois binários auditados: sysroot atual/glibc 2.43 no
NextOS e compatibilidade externa com GLIBC máx. 2.17 no artefato desta release.

Clicar em Nome/Fazenda/Coisa Favorita limpa automaticamente apenas o texto vermelho de
dica e abre o teclado interno. O analógico direito ganhou uma seta pixel-art, e fazendas
novas começam no zoom 0,75 sem alterar a preferência de saves antigos.

**Novo na v1.1.4:** segure **D-pad cima** para aproximar a câmera e **D-pad baixo** para
afastar, entre 0,40× e 2,5×. Andar e navegar em menus ficam no **analógico esquerdo** —
inclusive a rolagem inteira da última aba das Opções. Se preferir o D-pad todo como
movimento, igual à v1.1.3, use `SDV_DPAD_ZOOM=0`.

---

## Se der problema

Todo o diagnóstico está em `sdvnextos/debug.log` (a sessão anterior fica em
`debug.prev.log`). A linha mais importante é esta:

```
[sdv-egl] ready 640x480 driver=KMSDRM ES2 a8 d24 s8 GL='OpenGL ES 3.2 …'
```

Ela diz a resolução, o backend de vídeo e **com qual GPU** você está falando.

**Tela preta enquanto instala** → se o log ainda mostra `extracted ...`, aguarde
`=== installation complete ===`; o jogo ainda não iniciou. A v1.1.2 passa a usar a SDL
AArch64 do firmware no NXExtract e anexa o diagnóstico da interface ao `debug.log`.

**Tela preta depois de instalar** → veja se o log chegou a `[sdv-egl] ready` e se
`GL=` contém `OpenGL ES`. Na v1.1.1, o RG-DS mostrava em seguida
`MonoGame requires either ARB_framebuffer_object or EXT_framebuffer_object`: era uma
classificação errada de GLES como OpenGL desktop, não falta de FBO na GPU. A v1.1.2
oculta somente esse probe do Mesa e deixa o fallback GLES nativo seguir até `swap #1`.

**Sem som** → o jogo toca por OpenAL e a ordem de backends está em `alsoft.conf`
(`pipewire`, depois `pulse`, depois `alsa`). Force um com `AUDIO_DRIVER=alsa` ou
`AUDIO_DRIVER=pulse`.

**Câmera muito perto ou muito longe** → `SDV_TILES_X=15` controla o enquadramento (quantos
quadrados de largura aparecem). Menos = mais perto.

Os ajustes são variáveis de ambiente, todas desligadas por padrão. A lista completa está em
`sdvnextos/INSTALAR.md`.

---

## Créditos e licença

Port por **NextOS Ports**. O jogo é da Chucklefish/ConcernedApe — este pacote
não distribui nem redistribui conteúdo dele; você usa a sua própria cópia.
