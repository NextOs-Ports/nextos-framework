# Plano: rodar/garantir o Sonic 4 EP2 em mais devices

> Baseado em `STUDY_WINDOWING_AUDIO.md` (comparação Sonic vs Crazy Taxi vs Bully).
> **Objetivo:** consertar os 2 modos de falha (vídeo no Mesa/Panfrost; áudio no muOS) SEM
> regredir os devices que já funcionam. **Validação obrigatória:** Mali-450 `.79` + R36S `.104`.

## Contexto (por que mexer)

Dois devices falham de formas opostas, ambos onde Crazy Taxi/Bully funcionam:
- **Device A (Mesa/Panfrost, ex-ROCKNIX):** pedimos **ES3 primeiro** → o Mesa devolve um contexto
  **desktop-GL "3.1 Mesa"** que passa como sucesso (não-NULL) → shaders GLSL ES não compilam →
  **tela preta**. Crazy Taxi e Bully pedem **ES2 e só ES2** → GLES real → funciona.
- **Device B (muOS):** pipewire não sobe (falta runtime-dir) → sobra ALSA cru → speaker
  `audiocodec` busy → cai no HDMI → **mudo**. (log é PRÉ-fix; a v4.2 já tem prefere-speaker.)

⚠️ **Importante sobre teste:** os 2 devices que TEMOS (`.79` Mali-450 Utgard ES2; `.104` R36S
ArkOS libmali) **já renderizam e tocam** — servem de **regressão** (garantir que não quebramos).
Os devices que FALHAM (Mesa/muOS) são de tester; a validação lá é indireta (design pelo estudo +
tester), a menos que se rode ROCKNIX no R36S pra reproduzir o Mesa.

## Princípios
- **Cada mudança gated por env** (liga/desliga) e default seguro → reversível, testável isolada.
- **Regra #6 mantida:** nada de forçar SDL_VIDEODRIVER/AUDIODRIVER.
- **Nada de regressão** em `.79`/`.104` — critério de aceite de cada fase.

---

## FASE 1 — VÍDEO: garantir contexto GLES real (o fix do Device A)

### 1a. ES2 primeiro, com fallback pra ES3 (inverter a preferência) — RAIZ
- **Arquivo:** `src/egl_shim.c:114-118` (a ordem `vers`).
- **Hoje:** sem env → `{3, 2}` (ES3 primeiro). **Mudança:** sem env → `{2, 3}` (ES2 primeiro).
- **Motivo:** a engine libfox é ES2/GLSL-ES-1.00; ES2 é o que Crazy Taxi/Bully pedem e é GLES
  real no Mesa. Devices ES3-reais (libmali) rodam ES2 sem problema (compat pra trás).
- `SONIC_GLVER` continua permitindo forçar (=2/=3) p/ teste.

### 1b. Rejeitar contexto DESKTOP-GL e recriar em ES2 (defesa dupla)
- **Arquivo:** `src/egl_shim.c:164-194` (já lê `GL_VERSION` e detecta "sem ES", mas só avisa).
- **Mudança:** se o contexto criado **não** tiver "ES"/"es" no `GL_VERSION`, **destruir**
  (`SDL_GL_DeleteContext`) e **recriar** pedindo ES2 na MESMA janela; se ainda vier desktop,
  aí sim seguir (melhor que nada) com o aviso atual.
- **Motivo:** blindagem — mesmo que 1a não cubra algum backend, isto garante GLES.

### 1c. Fullscreen DESKTOP em vez de exclusivo
- **Arquivo:** `src/egl_shim.c:140` (`SDL_WINDOW_FULLSCREEN` → `SDL_WINDOW_FULLSCREEN_DESKTOP`).
- **Motivo:** Bully usa desktop; exclusivo faz modeset que pode dar EGL_BAD_MATCH em kmsdrm/
  wayland. Já setamos `SDL_VIDEO_FULLSCREEN_DESKTOP=1` no env — alinhar a flag da janela.
- **Gate:** `SONIC_EXCL_FS=1` reverte pro exclusivo (segurança p/ o fbdev Mali .79).

### 1d. (opcional) linkar+preload libEGL/libGLESv2 RTLD_GLOBAL — como o Bully
- **Arquivos:** `build*.sh` (`-Wl,--no-as-needed -lEGL`), e um `dlopen(RTLD_GLOBAL)` de
  libEGL/libGLESv2 no início (espelhar `bully2/src/main.c:133-142`).
- **Motivo:** garante o driver EGL/GLES do device carregado antes do SDL escolher. Menor
  prioridade (1a+1b devem bastar); avaliar só se persistir desktop-GL.

**Aceite Fase 1 (regressão):**
- `.79` (Utgard ES2): abre, renderiza Zone 1, título — igual hoje. (ES2-first é natural lá.)
- `.104` (ArkOS libmali): abre, renderiza — confirmar que ES2-first não regride o device ES3.
- Log deve mostrar `GL_VERSION=OpenGL ES ...` (com "ES") nos dois.

---

## FASE 2 — ÁUDIO: garantir saída no speaker (o fix do Device B/muOS)

### 2a. Retestar o que JÁ existe (v4.2) — passo zero
- A v4.2 já tem prefere-speaker (pula HDMI + retry 5×400ms no card busy) e
  `sonic_detect_session_runtime` (acha XDG_RUNTIME_DIR/WAYLAND_DISPLAY). O log do muOS era
  PRÉ-fix. **Antes de codar nada novo, confirmar no muOS (tester) se a v4.2 já resolve.**

### 2b. Preferir servidor de som na ordem certa (espelhar o Bully)
- **Arquivo:** `src/sonic_audio.c:1194-1214` (a varredura de drivers, hoje na ordem do SDL).
- **Mudança:** ao varrer, **priorizar `pipewire` → `pulse` → `alsa`** (ordem do `alsoft.conf`
  do Bully), em vez da ordem crua do SDL. Servidor de som → default sink = speaker → nada de
  enumerar card por nome.
- **Gate:** `SONIC_AUDIO_ORDER` opcional p/ override.

### 2c. Reforçar a subida do pipewire/pulse antes do ALSA cru
- **Arquivo:** `src/main.c:188` (`sonic_detect_session_runtime`).
- **Investigar/robustecer:** no muOS o `pw.loop` falhou por runtime-dir. Garantir
  XDG_RUNTIME_DIR válido (com permissão) ANTES do primeiro `SDL_InitSubSystem(AUDIO)`, pra o
  pipewire/pulse iniciar e virar o default. Se subir, o caso "card busy/HDMI" nem acontece.

**Aceite Fase 2 (regressão):**
- `.79` (Mali-450): áudio toca (título+gameplay+special stage) igual hoje.
- `.104` (ArkOS): áudio toca igual hoje (não quebrar a seleção atual que funciona).
- muOS (tester): som sai no **speaker** (não HDMI).

---

## Estratégia de implementação e teste
1. **Um branch de trabalho no master por fase** (commits pequenos, sem co-autor). Cada alavanca
   é um commit isolado e gated por env → dá pra bissetar se algo regredir.
2. **Ciclo por fase:** editar → `build.sh` (native .79) + `build_compat_gcc.sh` (compat .104) →
   deploy com kill→rm→scp→confere md5 (lição HLM2) → rodar harness → screenshot + log.
3. **Ferramentas já prontas:** `runsonic.sh`/harness de screenshot (`/dev/shm/sonic_shot`),
   `SONIC_GLVER`, `SONIC_AUDIODRIVER`, `SONIC_WARP_STAGE`, `SONIC_AUTOSTART`.
4. **Ordem:** Fase 1 (vídeo) primeiro — é a de maior impacto e a raiz mais clara. Depois Fase 2.
5. **Empacotar v4.3** só depois das duas fases validadas em `.79` + `.104` (regressão) + parecer
   do tester nos devices Mesa/muOS.

## Riscos / cuidados
- **ES2-first pode, em tese, tirar algum ganho ES3 num device ES3-real** — mas a engine é ES2, e
  1b garante GLES; risco baixo. `SONIC_GLVER=3` fica p/ quem quiser forçar.
- **Fullscreen_desktop no fbdev Mali .79** — testar bem; `SONIC_EXCL_FS=1` como escape.
- **Não temos Mesa/muOS na bancada** — validação final desses depende de tester OU de subir
  ROCKNIX no R36S pra reproduzir o Mesa/Panfrost localmente (recomendado p/ fechar a Fase 1).

## Arquivos que serão tocados
- `src/egl_shim.c` (ordem ES, retry-ES2, flag de janela) — Fase 1.
- `src/main.c` (runtime-dir, ordem de áudio hook) — Fase 2.
- `src/sonic_audio.c` (ordem de drivers) — Fase 2.
- `build.sh` / `build_compat_gcc.sh` (só se 1d) — Fase 1 opcional.
