# Estudo de desempenho — Castle of Illusion (NextOS)

Objetivo: subir o framerate de gameplay (v1.0.1 entregava ~15–25 fps),
aceitando perda de qualidade gráfica se preciso.

**Resultado: nenhuma qualidade precisou ser sacrificada.** O gargalo era um
laço de busca de símbolo dentro do shim, não a GPU. A CPU consumida por frame
na thread de render caiu de **30,0 ms para 7,3 ms** (medido no R36T/ArkOS,
Mali-G31, 06/08/2026).

---

## 1. Como o gargalo foi encontrado

### 1.1 Primeira leitura (log de 12 min no Mali-450): frametime quantizado

As linhas `[PERF]` ficavam presas em degraus — `avg` 33,0–33,3 ms (30 fps) ou
50,0–50,7 ms (20 fps), com `max` a menos de 1 ms do `avg` durante minutos. São
2 e 3 vblanks de 60 Hz. Carga de GPU varia; degrau de vsync não. Isso disse
onde mirar: o trabalho do frame estava pouco acima de 33,3 ms, e **cortar
poucos ms pularia um degrau inteiro** — 20 → 30 fps.

### 1.2 Segunda leitura: separar trabalho de espera

O `[PERF]` passou a cronometrar dentro do `SDL_GL_SwapWindow` (espera de
vblank) separado do resto, e a somar **tempo de CPU real** da thread
(`CLOCK_THREAD_CPUTIME_ID`) — sem isso não dá para distinguir "trabalhando" de
"dormindo". No menu, com 21 draw calls:

```
avg=35.4ms  swapwait=5.2ms  busy=30.0ms
```

30 ms de CPU **queimada de verdade** para desenhar 21 quads. O port era limitado
por CPU — não por fill-rate, não por vsync, não por banda de textura. Isso
matou de saída as alavancas óbvias: baixar resolução interna, `COI_TEXSCALE`,
ETC1 e `mediump` não dariam fps nenhum.

### 1.3 Terceira leitura: profiler de amostragem (`COI_PROFILE=1`)

Como não há `perf` no firmware, o port ganhou um profiler próprio
(`src/coi_profile.c`): timer POSIX preso à thread de render, ancorado no
relógio de CPU dela, amostrando o PC a cada 1 ms de CPU. O handler atribui a
amostra ao mapeamento (inclusive os **anônimos** — os módulos do so-loader não
têm caminho no `/proc/self/maps`) e ao balde de 256 B dentro dele.

```
libc-2.30.so                    47.0%
anon (engine libViewer_GP.so)   29.4%
libmali-bifrost-g31             16.4%
castleofillusion (nosso)         5.3%
```

47% da CPU do frame dentro da libc, concentrada num balde: `libc+0x080400` =
**`strcmp`**. Como `strcmp` é folha (não toca o LR), o profiler passou a
registrar também o registrador de retorno — e o chamador saiu com nome:

```
<- castleofillusion+0x009f00   11.8%   => so_find_addr_safe
```

### 1.4 A causa

`pb_try_connect()` (android_shim.c) roda a cada frame pelo
`process_sdl_events()`. Ele procura quatro exports do **Paddleboat** (a
biblioteca de controle do Google, de 2022) na engine — uma engine Sega "oz" de
2013, onde a string "Paddleboat" **não aparece uma única vez**. As buscas
falhavam, o código zerava o cache e no frame seguinte refazia as quatro
varreduras **lineares, com `strcmp`, sobre os 21.324 símbolos dinâmicos** da
engine: cerca de **85 mil `strcmp` por frame**, para sempre.

Um símbolo ausente do `.dynsym` nunca passa a existir depois do load. Uma
tentativa basta, e desistir é definitivo.

## 2. Efeito medido do fix

Mesma cena, mesmo aparelho, antes e depois:

| | antes | depois |
|---|---|---|
| CPU da thread por frame (`busy`) | 30,0 ms | **7,3 ms** |
| CPU do processo (`allcpu`) | 36,0 ms | **11,3 ms** |
| libc no perfil | 47,0% | 9,7% |

O menu continua em ~30 fps porque **a engine se limita a 30 fps** — com o fix,
`fora_do_swap=25 ms` contra `busy=7,3 ms`, ou seja, ~18 ms por frame em que a
thread simplesmente dorme. O ganho aparece onde o frame estourava o orçamento:
o gameplay, que caía para o degrau de 50 ms justamente por causa desses ~23 ms
de CPU desperdiçada.

## 3. Outras correções desta rodada (neutras visualmente)

- **`glGetIntegerv` respondido por shadow state.** A engine relê blend (6
  pnames) + depth do driver ~20× por frame. Passou a ser respondido dos shadows
  que o shim já mantinha (`COI_GLGET_REAL=1` volta ao driver). Medido: neste
  G31 **não era gargalo** — fica como higiene e como seguro no Utgard, onde
  `glGet*` é round-trip síncrono.
- **`glBlendEquation`/`Separate` interceptados**, senão o shadow acima
  envelheceria.
- **Dreno de `glGetError` no `glBufferData`** (2 syncs por upload de geometria,
  todo frame) agora só com `COI_BUF_LOG=1`.
- **`getenv()` cacheado** no caminho de frame (rodava por `glUniform*`, por
  draw, por `glBlendFunc`, `glVertexAttribPointer`…).

## 4. Ferramentas que ficaram no port

| flag | o que faz |
|---|---|
| `COI_PROFILE=1` | profiler de amostragem da thread de render (`[PROF]`) |
| `COI_PROFILE_US=<us>` | intervalo de amostragem (default 1000 µs de CPU) |
| `COI_MEDIUMP=1` | troca `highp` por `mediump` nos shaders (troca qualidade por GPU) |
| `COI_GLGET_REAL=1` | desliga o shadow state e pergunta ao driver |
| `COI_SWAPINT=0` | sem vsync — revela o custo real do frame |

O `[PERF]` sozinho já responde o essencial:

- `swapwait` alto + `busy` baixo → preso no degrau do vsync;
- `busy` alto → gargalo de CPU, ligar `COI_PROFILE=1` e ler quem aparece;
- `fora_do_swap` muito maior que `busy` → a engine está dormindo (limiter).

## 5. Observações que continuam valendo

- **O jogo não usa FBO nenhum** (`fbo=0 screen=...` o run inteiro). A alavanca
  de resolução interna que existe em `my_glViewport`/`my_glRenderbufferStorage`
  só age com `g_cur_fbo != 0` e portanto **nunca agiu neste jogo** — o
  comentário de "89% dos draws no FBO de cena" é herança do scaffold doador.
  Se um dia o gargalo voltar a ser GPU, seria preciso um render target próprio
  do shim (e o blit final é onde o Huntdown universal empacou com
  `GL_INVALID_FRAMEBUFFER_OPERATION` — validar com `glReadPixels`).
- **A engine se limita a 30 fps.** 30 fps estável é o teto do jogo, não do
  aparelho.

---

# Rodada 2 — sessão no aparelho (R36T/ArkOS, 06/08/2026)

Medições feitas com o NextOS jogando e com o profiler novo. **Nada aqui está
concluído**; é o estado em que a investigação parou.

## 6. Gameplay depois do fix do Paddleboat

Fase real, NextOS jogando: **19,1 fps**, `busy=40,4 ms`, `swapwait=10,6 ms`,
46 draw calls e 32 matrizes por frame. Continua **limitado por CPU** — só que
agora a CPU é gasta com trabalho de verdade, não com varredura de símbolo.

Perfil de gameplay (56 amostras por SIGUSR1, relógio de parede, então mistura
espera com trabalho):

| onde | fatia |
|---|---|
| `libpthread` (lock/espera) | 26,8% |
| driver Mali | 12,5% |
| pilhas passando por **física (Bullet)** | 27% |
| pilhas passando por render (`QuadBatcher`) | 16% |
| som | 2% |

Nomes recorrentes: `btGjkEpaSolver2::SignedDistance`,
`btKinematicCharacterController::{stepUp,onGround,playerStep}`,
`btCollisionWorld::{performDiscreteCollisionDetection,updateAabbs}`,
`btBoxShape::getAabb`.

## 7. Loading: é parsing de XML

`[PROFLOAD]` (janela de amostras zerada a cada swap, então isola o frame de
carregamento). Frames de 3,4 s e 6,7 s, com **87–95% da CPU dentro da engine**:

```
tinyxml2::StrPair::ParseName             7,4%
oz::ResourceSystem::GetResource(GUID)    7,1%
tinyxml2::StrPair::CollapseWhitespace    5,1%
tinyxml2::XMLUtil::StringEqual           5,0%
tinyxml2::XMLDocument::ShallowEqual      4,9%
tinyxml2::XMLUtil::SkipWhiteSpace        2,5%
```

Sequência de frames de load numa abertura: 1,5 · 1,2 · 1,3 · 3,4 · 6,7 · 6,2 ·
3,4 · 14,4 · 22,1 · 3,3 · 12,6 · 9,3 · 8,3 · 3,4 · 6,5 · 19,1 · 3,6 s.

Não é o cartão: se fosse I/O, `busy` seria uma fração do relógio de parede, e
ele é ~95% dele.

## 8. Clock do CPU NÃO é alavanca: o aparelho está em throttling térmico

O `scaling_max_freq` estava em **1008 MHz** com `cpuinfo_max_freq` de
**1512 MHz**, o que parecia um teto de firmware para levantar. É throttling:

```
thermal_zone0 = 83,5 °C     cooling_device0 cur_state = 3
```

Teste feito e **descartado**: subir o teto e pôr o governor em `performance`
deu tempos de loading idênticos (1,4 · 3,7 · 7,0 · 15,2 · 22,3 s contra
1,5 · 3,4 · 6,7 · 14,4 · 22,1 s) porque o kernel puxou o clock de volta. Tudo
foi restaurado (`interactive`, GPU em `dmc_ondemand`).

Consequência: **a única saída é o jogo gastar menos CPU** — o que também
esquenta menos e reduz o throttling.

## 9. Próximo passo preparado (não executado)

`btDiscreteDynamicsWorld::stepSimulation` é **virtual**: não há `bl` direto
para ela em lugar nenhum do binário, a chamada passa pela vtable. Isso permite
interceptar **sem patch de código**, trocando o ponteiro no slot:

```
_ZTV23btDiscreteDynamicsWorld          @ 0x87dcb0   (vtable, RW no nosso heap)
_ZN23btDiscreteDynamicsWorld14stepSimulationEfif @ 0x40ad84
```

Ordem certa: primeiro um wrapper que **só registra** `(dt, maxSubSteps,
fixedTimeStep)` nos primeiros frames. Se `maxSubSteps > 1`, existe espiral
clássica do Bullet — a 19 fps o `dt` de 52 ms vira 3 sub-passos de física por
frame, ou seja, quanto mais lento fica, mais física roda. Nesse caso
`maxSubSteps = 0` (passo único com o `dt` real) corta a física para 1/3
mantendo a velocidade do jogo, ao custo de precisão de colisão. **Medir antes
de mexer.**
