# Stardew Valley v1.1.6 — instalação multi-device

So-loader próprio que roda o **Stardew Valley de Android** (Mono/.NET 8 AOT + MonoGame)
direto no Linux do handheld, sem Android e sem container.

Este pacote traz **somente o nosso código**. Os dados do jogo (`assets/` e `libs/`) saem do
**seu próprio APK** — nada de jogo é distribuído aqui.

---

## Onde já roda

| Aparelho | Vídeo | Estado |
|---|---|---|
| Mali-450 / Amlogic-old (EmuELEC, fbdev) | ES2, fbdev | jogável, save real, é a bancada canônica |
| R36S / ArkOS (RK3326, Mali-G31) | ES2 sobre contexto ES3, KMSDRM | jogável; teclado, scroll, áudio, vídeo e save validados em 01/08/2026 |
| RG 40XX-H / muOS | firmware nativo | confirmado pela comunidade na v1.1.1 |
| RG-DS / ROCKNIX (Panfrost, Wayland, Mesa 26.1.2) | GLES 3.1/FBO core | correção v1.1.2; aguardando reteste pós-release |

O launcher escolhe o binário certo. No NextOS/NextOS Elite usa `stardewvalley`, compilado
obrigatoriamente no sysroot atual do projeto (glibc 2.43 nesta release). Nos demais CFWs
usa `stardewvalley.multi`, compilado em Debian Buster (artefato atual e teto auditado:
GLIBC 2.17).

---

## Montagem no aparelho

```
<roms>/ports/Stardew Valley (NextOS).sh      <- launcher (deste zip: arkos/)
<roms>/ports/sdvnextos/
    run.sh                                  <- runtime único chamado pelos dois launchers
    stardewvalley / stardewvalley.multi      <- loaders deste zip
    nxextract.py / nxextract-ui              <- preparação visual e transacional
    extractor.json                           <- receita exata do Stardew 1.6.15.3
    tools/liblz4.so.1                         <- LZ4 AArch64 incluído (GLIBC 2.17)
    alsoft.conf                              <- negociação OpenAL
    gamedata/
        <seu pacote Android>                  <- VOCÊ põe aqui, e só
```

Abra o port uma vez. O **NXExtract 1.1.2** abre uma interface, valida a origem e prepara
`libs/` e `assets/Content/` numa transação recuperável (~420 MB, alguns minutos). APK,
APKM, APKS, XAPK e ZIP de bundle são aceitos; em pacotes divididos ele resolve a variante
arm64. O arquivo de origem não faz parte do port e pode ser removido depois que a
instalação terminar com sucesso.

**APK necessário** — sua cópia legal:

| pacote | versão | ABI |
|---|---|---|
| `com.chucklefish.stardewvalley` | **1.6.15.3** | arm64-v8a |

O extrator confere versão, ABI, árvore, tamanhos e fingerprint antes do commit. Um
marcador verificado evita reextrações; instalações antigas do v1.0 são migradas e
adotadas sem mexer nos saves. A v1.1.2 também retoma stages da v1.1.0 que pararam em 65%
no ROCKNIX por falta de `liblz4.so`; não é necessário extrair os 394 MB novamente. Se
os dados existentes estiverem inválidos, basta manter uma origem 1.6.15.3 suportada em
`gamedata/` para o reparo transacional.

Os launchers mínimos em `ports/` e `ports_scripts/` acham `sdvnextos/run.sh` pela própria
localização, com fallback para `/roms`, `/roms2` e `/storage/roms`. O runtime usa sua
própria pasta como `GAMEDIR`, e o binário confirma a localização por `/proc/self/exe` —
não há caminho de aparelho cravado em lugar nenhum. As libs do Mono são procuradas tanto
na raiz do port quanto em `libs/`.

---

## Se algo não funcionar num aparelho novo

Todos os escapes são variáveis de ambiente, **desligadas por padrão**. Servem pra descobrir
o que negociar; achado o acerto, ele vira automático no binário.

| Variável | Para que serve |
|---|---|
| `SDV_TILES_X=15` | enquadramento da câmera (largura em tiles). Menos = mais perto |
| `SDV_DPI=142` | crava o DPI reportado (sobrepõe o `SDV_TILES_X`) |
| `SDV_WIDTH` / `SDV_HEIGHT` | crava a resolução (a cadeia automática já cobre o normal) |
| `SDV_NO_FORCE_GLES=1` | não pedir a lib GLES/EGL (só se o aparelho já for GLES nativo) |
| `SDV_EXCL_FS=1` | fullscreen exclusivo em vez de borderless |
| `AUDIO_DRIVER=pulse\|pipewire\|alsa` | backend do OpenAL, quando ficar mudo |
| `SDV_GL_TRACE=1` | conta frames no log (`swap #N`) |
| `SDV_RIGHT_CURSOR=0` | desliga o cursor independente do analógico direito |
| `SDV_INPUT_TRACE=1` | diagnóstico detalhado de input; não usar no dia a dia |

Fazendas novas usam zoom inicial 0,75; saves existentes preservam sua preferência
serializada. O analógico direito mostra uma seta pixel-art cuja ponta é o hotspot de R3.

O log de cada sessão fica em `sdvnextos/debug.log` (a anterior em `debug.prev.log`). A
primeira coisa a ler é a linha `[sdv-egl] ready ... GL='...'`: ela diz com qual GPU e qual
contexto você está falando.

**Tela preta na primeira preparação?** Enquanto aparecer `extracted ...`, o NXExtract
ainda está trabalhando. Aguarde `=== installation complete ===`; SELECT+START só existe
depois que o jogo entra no loop. A v1.1.2 isola a SDL do extrator e inclui seu diagnóstico
visual no `debug.log`.

**Tela preta depois da instalação?** Confira se houve `[sdv-egl] ready` e se
`GL_VERSION` contém `OpenGL ES`. O log ruim do RG-DS continuava com
`MonoGame requires either ARB_framebuffer_object or EXT_framebuffer_object`: o Mesa
aceitava um probe desktop e o MonoGame classificava errado o contexto GLES real. A
v1.1.2 oculta só esse probe e usa o fallback GLES nativo; procure depois por `swap #1`.

**Mudo?** O jogo toca por OpenAL; quem negocia o backend é o `alsoft.conf`
(`drivers = pipewire,pulse,alsa`). Tente `AUDIO_DRIVER=alsa` ou `pulse`.

**Precisa capturar o backbuffer real?** Com o jogo aberto, crie o arquivo vazio
`/dev/shm/sdv-shot`. O port grava `/dev/shm/sdv-shot.ppm` em poucos frames e remove o
gatilho. Esse recurso é apenas diagnóstico e fica completamente inativo no uso normal.

Sair do jogo: **SELECT + START**.

---

## Compilar

```bash
bash ./build.sh                    # NextOS atual -> stardewvalley

SR=<sysroot NextOS aarch64 atual>
docker run --rm -v "$PWD":/repo -v "$SR":/nxsr:ro \
  gtactw-arm64-builder:debian-buster bash /repo/build_buster.sh
                                    # CFWs externos -> stardewvalley.multi

./package/build-package.sh         # release pública determinística
```

Cada build grava `.build-provenance/*.json`. O empacotador recusa binário sem procedência,
GLIBC fora do perfil, dado proprietário, log, endereço de teste ou arquivo fora da
allowlist.
