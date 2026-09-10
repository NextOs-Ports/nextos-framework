# Oceanhorn: Chronos Dungeon 4.0b54 — fechamento NextOS Elite

Estado em 30/07/2026: **CONCLUÍDO, JOGÁVEL E PUBLICADO**.

Alvo validado: NextOS Elite AArch64, Amlogic-old, Mali-450/GLES2, framebuffer
1280×720 e aproximadamente 1 GB de RAM. O port preserva o fluxo Android do
Unity 2022.3.61f1 IL2CPP: construtores, `JNI_OnLoad`, registro dos nativos,
`initJni`, superfície, resume/foco e `nativeRender`.

## Resultado final

| Frente | Estado |
|---|---|
| Boot | Título e gameplay pelo fluxo nativo |
| Vídeo | 1280×720 nativo, tela completa, sem render-scale |
| Performance | 25–30 FPS em gameplay |
| Áudio | FMOD/OpenSL → SDL/Pulse, música e efeitos ativos, ganho 1,5× |
| Controle | Direcional e analógico esquerdo movem; botões de gameplay ativos |
| Save | Persistência e flush na saída |
| Saída | `SELECT+START` confirmado fisicamente |
| Frontend | Retorno limpo ao `emustation.service` |
| Release | Full-data privado publicado no R2 |

## Correções determinantes

### Proxy JNI do Unity

As duas pontes de proxy não são intercambiáveis:

- `bitter.jnibridge.JNIBridge.invoke` recebe ponteiro C++ nativo;
- `ReflectionHelper.nativeProxyInvoke` recebe GCHandle gerenciado.

O scaffold entregava GCHandle pequeno ao JNIBridge e causava SIGSEGV. O bridge
agora preserva a origem do proxy e chama o caminho correto.

### Dados Unity/IL2CPP

Os caminhos Android inexistentes são redirecionados para `bin/Data`. O loader
abre o `data.unity3d`, resources, metadata e resources gerenciados originais.
O APK inteiro não é necessário em runtime.

### Áudio definitivo

O `createSound` do FMOD aceitava `CREATESTREAM|NONBLOCKING`, mas o worker Android
mudava o estado para `ERR_INTERNAL`. O hook rederivado para esta `libunity`
converte o modo antecipadamente para sample residente síncrono. A saída OpenSL
é entregue a SDL/Pulse em estéreo, com ganho configurável, padrão 1,5×, e
limitador suave.

### Controle e saída

`src/ocean_input.c` publica o InputDevice Android completo e injeta
KeyEvent/MotionEvent aceitos pelo Rewired. Como esta configuração associa
movimento ao direcional, o analógico esquerdo é espelhado no mesmo caminho com
zona morta, histerese e diagonais.

`SELECT+START` é detectado antes do próximo frame. O loop então executa perda
de foco, `nativePause`, flush das preferências, parada do áudio e o encerramento
seguro do so-loader. O launcher restaura a GPU ao valor original.

### Performance sem degradar gráficos

O render-scale experimental foi rejeitado e fica obrigatoriamente desligado.
O ganho final veio de:

- três pixel processors do Mali ativos;
- nível oficial de 666 MHz durante o jogo;
- recuo térmico para o nível anterior a 82 °C e retorno abaixo de 76 °C;
- `GL_EXT_discard_framebuffer` exposto ao Unity;
- cadência nativa de 30 FPS;
- remoção de logs síncronos de diagnóstico no caminho normal.

O launcher restaura `min_freq` e `min_pp` ao sair, inclusive por sinal.

## Build e release

`./build.sh` usa exclusivamente o toolchain/sysroot atual do NextOS
Amlogic-old. No fechamento, o sysroot fornecia glibc 2.43; a maior versão de
símbolo exigida pelo loader era GLIBC_2.38.

O pacote R2 fica em:

```text
ports_aio/nextos_elite_exclusivos/
Oceanhorn Chronos Dungeon (NextOS Elite).tar.gz
```

O full-data contém loader stripado, launcher, três bibliotecas ARM64, 17
arquivos Unity, licença/aviso, capa 1280×720 e `userdata` vazio. Não contém APK
redundante, logs, saves pessoais, capturas, backups, fontes ou binários antigos.

## Limitações conhecidas

- O bridge publica o primeiro controle SDL; multiplayer com controles
  adicionais não foi validado.
- Em temperatura alta, a proteção reduz um nível de GPU e o FPS pode variar
  conforme a cena.
- Ads, billing, serviços Google e recursos online Android não fazem parte do
  runtime offline.

## Não regredir

- Não ativar `CUP_RENDERSCALE` em produção.
- Não reutilizar RVA de outra versão do Unity.
- Não trocar a ponte ReflectionHelper pela JNIBridge.
- Não voltar o áudio para streaming assíncrono Android.
- Não lançar uma segunda instância.
- Não remover o flush nativo usado por `SELECT+START`.

Documentação pública e bilíngue: `README.md`.

---

## Fase 2 — multi-device: R36S/ArkOS (03/08/2026)

Plano: `docs/plans/2026-08-03-001-feat-oceanhorn-universal-multidevice-plan.md`.

### U1 — binário universal: CONCLUÍDO

`build_universal.sh` (Docker `debian:buster`, gcc 8.3 aarch64, sysroot NextOS montado
somente-leitura e apenas para headers) produz `oceanhorn-universal` exigindo **GLIBC_2.27**
— dentro do teto público de 2.30. O `build.sh` (glibc corrente do NextOS Elite) continua
sendo artefato separado e não entra em pacote universal.

Gates automáticos dentro do próprio script: teto de glibc e layout do TLS
(`g_bionic_guard_pad` = primeiro bloco PT_TLS, 256 bytes) — se o canary bionic sair do pad,
a build falha em vez de gerar um binário que quebra em runtime.

### Primeiro boot no R36S/ArkOS: JOGO RODANDO

Aparelho: ArkOS (imagem RK3326, hostname `rg351mp`), Ubuntu 19.10, glibc 2.30,
kernel 4.4.189, Mali-G31, painel 640×480, ~640 MiB de RAM.

| Frente | Resultado no primeiro boot |
|---|---|
| Vídeo | SDL **KMSDRM**, 640×480 (fonte: SDL desktop), contexto **ES3.0** depth24/stencil8 |
| GPU | `GL_VENDOR=ARM`, `GL_RENDERER=Mali-G31`, ES 3.2 r13p0, GLSL ES 3.20 |
| Swappy | fallback EGL nativo forçado automaticamente no host KMS |
| Áudio | SDL **ALSA** 44100 Hz 2ch 1024 samples |
| Controle | pad reconhecido (`Add controller [Xbox 360 Controller] to player [0]`); **A** confirma no título |
| Progressão | título → START GAME → cutscene de abertura renderizando |
| RAM | RSS 277 MB no título, **pico 352 MB** na cutscene; MemAvailable 257 MB |
| FPS | 24 durante o loading, subindo para **42–55** |

Provas: `shots-arkos/shot-title.png`, `shots-arkos/shot-late.png` (captura kmsgrab no
próprio aparelho).

O código adaptativo herdado do Horizon Chase resolveu vídeo/áudio sem alteração: nada de
backend SDL forçado, nada de resolução cravada.

### Armadilha de harness (não é bug do port) — custou duas rodadas

Rodar o binário **sem** o `run.sh` não é o mesmo teste: os gates do port ficam no default.
Em teste manual por SSH, exportar o mesmo conjunto que o `run.sh` exporta, no mínimo:
`CUP_FRAMES=0` (o default é 600 frames → o jogo "sai sozinho" em ~20 s),
`HC_STREAM_FALLBACK=1` (sem ele o áudio fica **mudo**), `TER_GAMEPAD=1`,
`TER_NOSTORAGEPATCH=1`.

### SELECT+START: RESOLVIDO

O GO-Super Gamepad do R36S não tem `BTN_SELECT` nem `BTN_START`; os dois chegam como
`BTN_TRIGGER_HAPPY1/2`, que a base do SDL não converte em BACK/START. O combo era
invisível e o jogo só terminava por sinal.

`ocean_input.c` agora calcula os ordinais SDL desses dois códigos a partir do bitmap
`EV_KEY` do próprio nó de evento (contagem de bits a partir de `BTN_JOYSTICK`). Neste pad
eles são **12/13** — um índice fixo 8/9 estaria errado. Pads com SELECT/START físicos
sondam -1 e seguem o caminho intocado, então o Mali-450 não muda.

Validado no aparelho: combo injetado → `[OCEANPAD] SELECT+START -> saída limpa solicitada`
→ processo encerrado em 1 s, com focus-loss/pause/flush.

### Áudio: TOCANDO (o mudo era do harness)

Medido com o novo `OCEAN_AUDIOLOG=N` (nível real entregue ao SDL, desligado por padrão):
picos de **2571 → 7368 de 32767** no título e em gameplay, `vol=1.000`, zero
`FAIL createSound`, zero erro de FSB, zero underrun.

O silêncio das primeiras rodadas era **falta do env `HC_STREAM_FALLBACK=1`**, que é quem
instala o hook do `FMOD::System::createSound` (`0xc7e168`). Sem ele o FMOD entrega buffers
de silêncio com o OpenSL inteiro aparentemente saudável.

### Pendências desta fase
- FPS em gameplay real (dungeon) ainda não medido; só título/cutscene.
- Launcher PortMaster do ArkOS, NXExtract e empacotamento seguem pendentes (U6/U7).

### Não regredir

- Mali-450 continua sendo o caminho padrão daquele hardware: toda adaptação de G31/KMSDRM
  entra por detecção de capacidade, nunca substituindo o caminho fbdev/GLES2/ETC1.
