# Estudo: Windowing + Áudio — Sonic 4 EP2 vs Crazy Taxi vs Bully

> Estudo comparativo (2026-06-30). **Só análise — nada implementado.** Motivado por 2
> devices que falham de formas OPOSTAS, ambos onde o Crazy Taxi FUNCIONA.

## 0. Os dois modos de falha (fatos dos logs do usuário)

| Device | GPU/Stack | Vídeo | Áudio |
|--------|-----------|-------|-------|
| **A** (ROCKNIX?) | Mali-G31 **Mesa/Panfrost** | ❌ **SEM VÍDEO** (tela preta) | ✅ OK (pulseaudio) |
| **B** (**muOS**) | Mali-G31 **libmali (ARM)** | ✅ OK (`OpenGL ES 3.2`) | ❌ **SEM ÁUDIO** (foi pro HDMI) |

- **Device A** `GL_VERSION=3.1 Mesa 26.1.0` → contexto **DESKTOP GL**, não "OpenGL ES" → shaders
  GLSL ES não compilam → preto. (áudio pulseaudio automático abriu certo)
- **Device B** `GL_VERSION=OpenGL ES 3.2 v1.r20p0` → GLES real, vídeo perfeito. Áudio: só ALSA
  disponível, pipewire falhou (`pw.loop can't make support.system handle`), speaker `audiocodec`
  **busy** → caiu no `ahubhdmi` (HDMI) → mudo no handheld.

**Crazy Taxi roda nos DOIS (ROCKNIX + muOS + ArkOS + TrimUI).** Então a comparação abaixo é o
caminho pra achar o que estamos fazendo de errado.

---

## 1. Tabela comparativa (os 3 ports)

| Aspecto | **Sonic 4 EP2** | **Crazy Taxi** | **Bully** |
|---|---|---|---|
| Arch loader | armhf | aarch64 | aarch64 |
| Engine | libfox.so (armv7) | libgl2jni.so (arm64) | libGame.so (arm64) |
| **VÍDEO — API** | SDL2 (janela+ctx) | SDL2 (janela+ctx) | SDL2 (janela+ctx) |
| **GL version pedida** | **ES3 → ES2 (adaptativo)** | **ES2 só** | **ES2 só** |
| Tentativas de contexto | **multi** (ES3/ES2 × 3 depth/stencil) | **1** (fatal se falhar) | **1** (loga e desiste) |
| Flag da janela | **`SDL_WINDOW_FULLSCREEN`** (exclusivo!) | `FULLSCREEN` + env desktop | **`FULLSCREEN_DESKTOP`** |
| depth/stencil | 24/8 → 16/0 → 0/0 | 16/8 | 24 / sem stencil |
| **EGL** | **shim FALSO** → `SDL_GL_*` | real (driver `mali` do SDL) | EGL cru sobre janela SDL |
| **Linka libEGL?** | ❌ **NÃO** (`--as-needed` dropa) | ✅ sim | ✅ sim (+ dlopen RTLD_GLOBAL) |
| Linka libGLESv2? | ✅ sim | ✅ sim | ✅ sim (+ dlopen RTLD_GLOBAL) |
| Força SDL_VIDEODRIVER | ❌ não (regra #6) | ✅ **`mali`** (só no launcher Mali-450) | ❌ não |
| **ÁUDIO — API** | **SDL audio** (shim OpenSL) | SDL audio (shim OpenSL) | **OpenAL-soft** + mpg123 (do jogo) |
| Força AUDIODRIVER | ❌ não | ✅ **`pulse`** | ❌ não |
| Seleção de device | **6 camadas** (auto→varre→default→enum nomeado→anti-HDMI 2-pass+retry→softvol) | **nenhuma** (abre default) | ordem de backend via `alsoft.conf` (`pipewire,pulse,alsa`) |
| Fallback de driver | sim (varre todos) | não (1 fixo) | via OpenAL-soft |
| Watchdog OOM | não | não | sim (`device-watchdog.sh`) |

---

## 2. VÍDEO — por que o Device A (Mesa/Panfrost) fica sem vídeo

### 2.1 Raiz nº1 (mais provável): pedimos **ES3 primeiro**
- `egl_shim.c:118` — sem env, a ordem é `vers = {3, 2}` (tenta **ES3**, depois ES2).
- No Mesa/Panfrost, pedir um contexto **ES 3.x** via o backend/config que o SDL escolheu
  devolveu um contexto **DESKTOP GL 3.1** (`GL_VERSION="3.1 Mesa"`). Esse contexto **é não-NULL
  → "sucesso"** → o loop `for v` para no ES3 e **nunca tenta o ES2** (`egl_shim.c:146-157`).
- **Crazy Taxi e Bully pedem ES 2.0 e SÓ ES 2.0.** No Mesa, um contexto ES2 vem como GLES real
  (`OpenGL ES 2.0`), shaders compilam, vídeo funciona. É por isso que eles rodam no ROCKNIX.
- **A engine do Sonic é ES2** (libfox armv7, GLSL ES 1.00). Não ganhamos nada pedindo ES3 —
  só arriscamos pegar o desktop-GL do Mesa. O "ES3 primeiro" foi pedido pra devices ES3-reais
  (commit 3ff310b), mas é EXATAMENTE o que quebra no Mesa.
- 🔬 **Teste barato (já existe):** `SONIC_GLVER=2` força só ES2. **Hipótese: resolve o Device A.**

### 2.2 Raiz nº2: detectamos o desktop-GL mas só AVISAMOS
- `egl_shim.c:186` já detecta "contexto sem 'ES'" e imprime `*** AVISO: OpenGL DESKTOP ***`.
- Mas **não recria com ES2** — segue com o contexto ruim → preto. Um fix real destruiria o
  contexto desktop e re-tentaria ES2 na mesma janela.

### 2.3 Raiz nº3 (secundária): EGL virtualizada demais
- Sonic: `egl_shim_CreateContext` (egl_shim.c:327) é EGL **falso** — por dentro chama
  `SDL_GL_CreateContext`. **Toda** decisão GLES-vs-desktop é do SDL a partir de atributos/hints.
- Crazy Taxi: engine chama a `libGLESv2` **real direto** (imports → `&glActiveTexture`…), EGL é
  do driver `mali`. Bully: **linka E faz dlopen(RTLD_GLOBAL) de libEGL+libGLESv2** e usa EGL cru.
- Sonic **nem linka libEGL** (o `-lEGL` cai no `--as-needed` porque nunca chamamos EGL real).
  Só `libGLESv2`. Consequência prática: não há garantia de que o driver EGL/GLES do device seja
  o carregado — depende 100% do SDL. Nos ports que funcionam, o binário **exige** libEGL+libGLESv2.

### 2.4 Raiz nº4 (secundária): fullscreen EXCLUSIVO
- Sonic cria a janela com **`SDL_WINDOW_FULLSCREEN`** (exclusivo, muda modo de vídeo) —
  egl_shim.c:140. Bully usa **`SDL_WINDOW_FULLSCREEN_DESKTOP`** (borderless, sem modeset).
- Em kmsdrm/wayland, o fullscreen exclusivo pode falhar o modeset / dar EGL_BAD_MATCH. Setamos
  `SDL_VIDEO_FULLSCREEN_DESKTOP=1` no env (main.c:645), mas a flag explícita da janela pode
  ganhar. Bully é o mais seguro aqui.

### 2.5 O que NÃO é o problema
- Não é o driver forçado: nem Crazy-Taxi-ROCKNIX nem Bully forçam `SDL_VIDEODRIVER` (o `mali` do
  Crazy Taxi é só do launcher Mali-450). No Mesa não existe driver `mali` — quem funciona lá NÃO
  força nada. Ou seja, **regra #6 está certa**; o problema é o **ES3-first + aceitar desktop-GL**.

---

## 3. ÁUDIO — por que o Device B (muOS) fica sem som

### 3.1 O que aconteceu no log do muOS
```
drivers disponiveis: alsa            <- SÓ alsa (sem pulse/pipewire utilizável)
[E] pw.loop can't make support.system handle: No such file or directory   <- pipewire NÃO inicia
default falhou (ALSA: ... No such file or directory)   <- ALSA sem PCM "default"
  [0] "audiocodec, " -> ... busy      <- SPEAKER ocupado
  [1] "ahubhdmi, "  -> ABRIU          <- caiu no HDMI = mudo no handheld
```
- **O log do muOS é de uma versão ANTIGA** (formato sem `PULADO (HDMI, pass 1)` / `busy, retry
  x/5` que o fix atual imprime). Ou seja, foi **antes** do fix prefere-speaker (v4.0-era).
  ⚠️ **Precisa retestar no muOS com a v4.2** — o retry+skip-HDMI pode já resolver.

### 3.2 Por que cai nessa situação (raiz)
- No muOS, **o servidor de som (pipewire) não inicia** (`pw.loop ... No such file or directory`
  = falta XDG_RUNTIME_DIR/socket). Sem servidor, sobra **ALSA cru**, que:
  1. não tem PCM `default` → o "default" do SDL falha;
  2. expõe cards por nome; o **speaker (`audiocodec`) está BUSY** (o frontend/ES ainda segura o
     card) → abre o **HDMI (`ahubhdmi`)** → mudo.
- Nós já tentamos consertar 2 coisas: (a) `sonic_detect_session_runtime()` (main.c:188) aponta
  XDG_RUNTIME_DIR/WAYLAND_DISPLAY se a sessão não exportou — pra pipewire iniciar; (b) o
  prefere-speaker (sa_try_open, sonic_audio.c:985) pula HDMI e faz retry no card busy 5×400ms.

### 3.3 Comparação com os que funcionam
- **Crazy Taxi:** força `SDL_AUDIODRIVER=pulse` + abre `default`. **Zero** proteção anti-HDMI.
  Só funciona porque nesses devices o **pulse existe e o default aponta pro speaker**. Num device
  onde o default fosse HDMI, o Crazy Taxi ficaria mudo igual — ele não tem defesa.
- **Bully (modelo mais robusto):** **não usa SDL pra áudio** — usa **OpenAL-soft** com
  `alsoft.conf`:
  ```
  [general]
  drivers = pipewire,pulse,alsa     <- ORDEM de preferência: servidor primeiro, alsa cru por último
  [alsa]
  mmap = false                       <- anti broken-pipe/XRUN no modeset HDMI
  ```
  A robustez vem de **preferir servidor de som (pipewire/pulse) → default sink = speaker** e só
  cair no ALSA cru como último recurso. **Não** seleciona device por nome — delega ao servidor.
  ⚠️ No `bully2` o `alsoft.conf` nem existe (só no `bully`), então lá usa defaults do OpenAL-soft.

### 3.4 Diagnóstico do nosso áudio
- Temos **6 camadas de fallback** (override → auto → varredura → default → enum nomeado →
  anti-HDMI 2-pass+retry → softvol). É **muito mais** do que Crazy Taxi (1) e Bully (delegado ao
  alsoft). Complexidade ≠ robustez: o caso muOS é justamente onde a heurística de nome
  (`audiocodec` vs `ahubhdmi` vs "hdmi") tem que acertar, e no log antigo ela errou.
- **O ponto que os dois que funcionam têm e nós tratamos por último: garantir o SERVIDOR de som
  (pulse/pipewire) rodando** → aí o "default" já é o speaker e nada disso importa. Nossa
  `sonic_detect_session_runtime` mira nisso, mas no muOS o pipewire ainda falhou.

---

## 4. Conclusões — "o que estamos errando"

### VÍDEO (Device A / Mesa)
1. 🔴 **ES3-primeiro** é o suspeito nº1: aceita um contexto **desktop-GL** que "funciona"
   (não-NULL) e nunca cai pro ES2. Os 2 ports que funcionam no Mesa pedem **ES2 e só ES2**.
2. 🟠 Detectamos desktop-GL mas só avisamos — não recriamos em ES2.
3. 🟡 Não linkamos/preloadamos libEGL (os que funcionam sim); EGL 100% virtualizada no SDL.
4. 🟡 Janela em fullscreen **exclusivo** vs o `FULLSCREEN_DESKTOP` do Bully.

### ÁUDIO (Device B / muOS)
1. 🔴 O log é **pré-fix** (v4.0). **Retestar com v4.2** (prefere-speaker+retry) é o 1º passo.
2. 🟠 Os que funcionam **garantem/preferem o servidor de som** (pulse no Crazy Taxi; ordem
   `pipewire,pulse,alsa` no Bully). Nosso `sonic_detect_session_runtime` tenta, mas no muOS o
   pipewire não subiu → caímos no ALSA cru + heurística de nome.
3. 🟡 Nossa pilha de 6 fallbacks é frágil; o modelo do Bully (OpenAL-soft + alsoft.conf de ordem
   de backend) é mais limpo — mas trocar nossa OpenSL→SDL por OpenAL seria reescrita grande.

## 5. Alavancas testáveis (para uma PRÓXIMA sessão de implementação — NÃO feito aqui)
- **V-1 (barato, alto valor):** rodar Device A com `SONIC_GLVER=2` → se aparecer vídeo, confirma
  a raiz nº1. Fix definitivo: **pedir ES2 primeiro** (ou: se o contexto criado não tiver "ES" no
  GL_VERSION, destruir e recriar em ES2 na mesma janela).
- **V-2:** trocar `SDL_WINDOW_FULLSCREEN` → `SDL_WINDOW_FULLSCREEN_DESKTOP` (como Bully).
- **V-3:** linkar+`dlopen(RTLD_GLOBAL)` libEGL+libGLESv2 (como Bully) pra garantir o driver GLES.
- **A-1 (barato):** **retestar muOS com a v4.2** (o fix prefere-speaker já está no binário).
- **A-2:** reforçar a subida de pipewire/pulse (XDG_RUNTIME_DIR) antes do fallback ALSA cru;
  espelhar a ordem `pipewire,pulse,alsa` do Bully na nossa varredura de drivers.

## 5.1 Validação (2026-06-30) — FASE 1/2 implementadas e testadas
- **On-screen, sem regressão:** `.79` Mali-450 Utgard (ES2 real "OpenGL ES 2.0", gameplay
  1280x720) e `.104` R36S ArkOS libmali (ES2 ctx, "OpenGL ES 3.2", menu Sylvania). Áudio toca
  nos dois (pulse/alsa auto). Fixes da v4.2 (fase abre/sai, sons) intactos.
- **Edge-cases (.104):** `SONIC_GLVER=3` (força ES3) = contexto ES3 ACEITO (device ES3-only não
  quebra); `SONIC_EXCL_FS=1` (exclusivo) OK.
- **`ctx_is_gles()` provado num contexto DESKTOP-GL REAL** (os devices ARM só dão GLES, nunca
  exercitavam o path de rejeição): teste `tools/test_ctxgles.c` no PC — pedir CORE 3.3 →
  `GL_VERSION="3.3.0 NVIDIA"` → `ctx_is_gles=0` → **REJEITA** (idêntico ao "3.1 Mesa" do Device A);
  pedir ES2 → `"OpenGL ES 3.2"` → `ctx_is_gles=1` → **ACEITA**. Confirma que o Device A (desktop-GL
  "3.1 Mesa") seria rejeitado e o loop cairia p/ um contexto GLES real.
- ⚠️ **Falta só** a validação do pipeline COMPLETO num Panfrost/Mesa ou muOS reais (hardware que
  não temos): tester roda a v4.3, OU subir ROCKNIX no R36S.

## 6. Arquivos-chave
- Sonic vídeo: `src/egl_shim.c` (create_window 76-219, EGL falso 327-355), `src/main.c:644-645`.
- Sonic áudio: `src/sonic_audio.c` (ensure_audio 1131, sa_try_open 956, prefere-speaker 985).
- Crazy Taxi: `docs/reference/crazytaxi-src/main.c` (ES2, single-shot), launcher força mali/pulse.
- Bully: `ports/bully2/src/egl_shim.c` (ES2, FULLSCREEN_DESKTOP), áudio OpenAL-soft +
  `ports/bully/alsoft.conf` (`drivers=pipewire,pulse,alsa`), `device-watchdog.sh`.
