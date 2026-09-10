# Sonic 4 EP2 — 2 bugs de compatibilidade de device (2026-06-30)

Logs de testers: ROCKNIX (sem vídeo) e muOS (sem áudio).

## ✅ AMBOS IMPLEMENTADOS 2026-06-30 (default ON, defensivos) — PENDENTE teste do tester
- **BUG A (ROCKNIX sem vídeo):** `egl_shim_create_window` agora seta `SDL_OPENGL_ES_DRIVER=1`
  + `SDL_VIDEO_X11_FORCE_EGL=1` (escolhe libGLESv2/EGL em vez de desktop-GL/GLX) + detecção:
  se GL_VERSION não tiver "ES", loga AVISO de tela-preta. Desliga com `SONIC_NO_FORCE_GLES`.
  Validado no nosso R36S (ArchR/libmali): continua `GL_VERSION=OpenGL ES 3.2`, sem regressão.
- **BUG B (muOS som no HDMI):** `sa_try_open` agora faz 2 passes — pass 1 IGNORA devices "hdmi"
  + RETRY (5×400ms) no device "busy" (espera o falante liberar); pass 2 = fallback qualquer
  (incl. HDMI). Desliga com `SONIC_NO_PREFER_SPEAKER`. Nosso R36S usa "default" → não afetado.
- Falta: tester rodar em ROCKNIX (mandar GL_VERSION — deve virar "OpenGL ES") e muOS (confirmar
  som no alto-falante). Não temos esses devices p/ validar local.

## 📚 PESQUISA do stack de vídeo da ROCKNIX (2026-06-30, valida o fix A)
Fontes: github.com/ROCKNIX/distribution (release notes 20241029), rocknix.org/systems/ports,
portmaster.games/porting.html, docs.mesa3d.org/drivers/panfrost.html, Collabora Panfrost blogs,
SDL2 wiki (hints OPENGL_ES_DRIVER / VIDEO_X11_FORCE_EGL), SDL issue #5386, gamescope #1245.
- **Display:** ROCKNIX roda os ports sob **Wayland (compositor `sway`)** → SDL driver `wayland`
  (EGL). Sem suporte Wayland no SDL do app → cai pro **Xwayland → driver `x11` → GLX**.
  (release 20241029: "fixed fullscreen rendering of PortMaster ports with sway".)
- **GPU RK3326/Mali-G31 = Mesa/PANFROST** (open-source), NÃO libmali. 🔑Panfrost expõe **OS DOIS**:
  OpenGL DESKTOP 3.1 **E** OpenGL ES 3.1 no mesmo device. (libmali só dá GLES → por isso o bug
  só aparece em Mesa/Panfrost; Mali-450 Utgard/.79 e R36S-libmali nunca pegam isso.)
- **RAIZ do desktop-GL:** com os 2 APIs disponíveis, o caminho **X11/GLX entrega desktop GL por
  default** se não forçar EGL. Sem profile-ES (ou sem EGL no X11) → contexto desktop 3.1, GLSL
  1.40 → shaders GLSL ES não compilam → tela preta (áudio independe de GL).
- **VEREDITO do nosso fix = CORRETO E SUFICIENTE:**
  - `SDL_GL_CONTEXT_PROFILE_ES` (já tínhamos, setado ANTES do SDL_CreateWindow ✓) → resolve o
    caminho Wayland. 🔑GOTCHA confirmado OK no código: no Wayland a EGLConfig/EGLSurface é
    criada no SDL_CreateWindow (não no CreateContext, SDL #5386) → profile-ES TEM que vir antes
    da janela; o nosso vem (no loop, antes do CreateWindow).
  - `SDL_VIDEO_X11_FORCE_EGL=1` (fix novo) → resolve o caminho Xwayland/X11: força EGL no lugar
    do GLX, daí o profile-ES entrega GLES. **Era o que faltava.** No-op no Wayland.
  - `SDL_OPENGL_ES_DRIVER=1` (fix novo) → carrega libGLESv2 por nome em vez de puxar de libGL
    desktop. Reforço do caminho X11; inerte onde GLES é nativo.
  - NÃO forçar `SDL_VIDEODRIVER` (sway auto-seleciona `wayland`; hardcodar quebraria Xwayland +
    viola regra #6). gl4es NÃO serve aqui (ele é desktop-GL→GLES; nosso problema é o inverso).
- **Opção de robustez (não aplicada, não-necessária):** setar as 2 hints ANTES de
  SDL_Init(VIDEO) garante ordenação; hoje setamos antes do 1º SDL_CreateWindow OPENGL, que é
  quando o SDL lê essas hints (no GL load) — também em tempo. Mali-450/R36S validados sem regressão.
- **Pendente:** confirmar no device do tester (GL_VERSION deve virar "OpenGL ES x.x" e o vídeo
  aparecer). Aguardando teste antes de fechar o v4.1.

---
## (estudo original abaixo)

## BUG A — ROCKNIX: áudio OK, SEM VÍDEO (tela preta)

### Sintoma (log)
```
Warning: Could not create EGL context (... EGL_BAD_MATCH)
egl_shim: GL context OK -> ES3 depth24 stencil8 (window 1280x720)
egl_shim: GL_VENDOR=Mesa
egl_shim: GL_RENDERER=Mali-G31 MC1 (Panfrost)
egl_shim: GL_VERSION=3.1 Mesa 26.1.0      <-- DESKTOP GL, não GLES!
egl_shim: GL_GLSL=1.40                     <-- GLSL desktop, não "ES"
```
Engine roda (loop, áudio do título), mas nada renderiza.

### RAIZ
A ROCKNIX usa **Mesa/Panfrost** (driver open-source). Pedimos contexto **ES**
(`SDL_GL_CONTEXT_PROFILE_MASK = SDL_GL_CONTEXT_PROFILE_ES`, egl_shim.c:113) mas o SDL
devolveu um contexto **OpenGL DESKTOP 3.1** (GL_VERSION="3.1 Mesa", GLSL "1.40"). Um
contexto GLES reportaria "OpenGL ES 3.x" / "OpenGL ES GLSL ES 3.x". Como o jogo usa
**shaders GLSL ES** (precision qualifiers, etc.), eles **não compilam** no contexto
desktop 3.1 → nada desenha → tela preta. O `EGL_BAD_MATCH` inicial é o SDL falhando
a 1ª tentativa ES e caindo no **GLX/desktop-GL** em vez do EGL/GLES.
Causa: egl_shim **não força o driver GLES** — o SDL nessa config (X11/gamescope?) usa
GLX (desktop) por default em vez de EGL+libGLESv2.

### SOLUÇÃO (proposta, NÃO aplicada)
1. **Forçar o driver GLES via EGL** antes de criar a janela/contexto:
   `SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1")` (ou env `SDL_OPENGL_ES_DRIVER=1`).
   Isso faz o SDL carregar **libGLESv2 via EGL** (Panfrost expõe GLES) em vez de desktop GL.
   ⚠️ NÃO é forçar SDL_VIDEODRIVER (regra #6) — é só escolher GL-lib (GLES vs desktop),
   que o jogo EXIGE. Mas é env de driver GL → confirmar com o NextOS antes.
2. **Detecção defensiva**: depois de criar o contexto, ler `GL_VERSION`; se NÃO contiver
   "OpenGL ES" (= veio desktop GL), **destruir e recriar** forçando o ES driver / EGL.
   Hoje o egl_shim já loga a identidade — falta agir sobre ela.
3. Alternativa: `SDL_VIDEO_X11_FORCE_EGL=1` (se for X11) força EGL → daí o profile ES pega GLES.
- Validação: precisa de um device ROCKNIX/Panfrost (não temos; é tester). Pedir ao tester
  rodar com `SDL_OPENGL_ES_DRIVER=1` no launcher e mandar o `GL_VERSION` (deve virar "OpenGL ES").

## BUG B — muOS: vídeo OK, SEM ÁUDIO (sai no HDMI)

### Sintoma (log)
```
sonic_audio: default falhou (ALSA: Couldn't open audio device: No such file or directory)
sonic_audio:   [0] "audiocodec, " -> ALSA: Couldn't open audio device: Device or resource busy
sonic_audio:   [1] "ahubhdmi, " -> ABRIU
sonic_audio: volume por software ON (card cru) vol=0.68
```
Áudio abriu o **`ahubhdmi`** (HDMI) → som vai pro HDMI, não pro **alto-falante** (`audiocodec`).

### RAIZ
`sa_try_open` (sonic_audio.c:956): tenta "default"→falha; depois itera os devices nomeados
e **pega o PRIMEIRO que abre** (linha 978-985). Na muOS o `audiocodec` (alto-falante) está
**"Device or resource busy"** (algo segura ele — provável o frontend/processo anterior não
liberou o card), então o loop cai no próximo: `ahubhdmi` (HDMI), que abre → som no HDMI.

### SOLUÇÃO (proposta, NÃO aplicada)
No `sa_try_open`, melhorar a escolha do device nomeado:
1. **Despriorizar HDMI**: pular (ou deixar por último) devices cujo nome contenha "hdmi"
   (`strcasestr(dn,"hdmi")`) — num handheld o alvo é o alto-falante, não HDMI.
2. **Retry do device busy**: se o `audiocodec` (speaker) deu "Device or resource busy",
   tentar de novo algumas vezes com pequeno sleep (o frontend pode liberar o card logo após
   o launch). Preferir o speaker mesmo que demore 1-2s.
3. **Ordem de preferência**: 1º os nomes "fala"/"codec"/"speaker"/"audiocodec"/"default-like";
   HDMI só se nada mais abrir.
   Pseudo: 2 passes — pass 1 abre o 1º não-HDMI que abrir (com N retries no busy);
   pass 2 (fallback) abre qualquer um (incl. HDMI).
- ⚠️ Por que busy: investigar no launcher se há processo segurando o card no muOS (o
  frontend muOS pode não soltar o ALSA). Se for isso, o launcher poderia liberar antes.
- Validação: device muOS (tester). Pedir log com a nova ordem.

## Notas
- Ambos são devices de TESTER (ROCKNIX=Mesa/Panfrost, muOS). Não temos pra validar local;
  precisa do tester rodar a versão de teste. R36S nosso = ArchR (libmali, GLES real) e 
  Mali-450 (.79) não têm esses 2 problemas.
- Nenhuma mudança aplicada (só estudo, a pedido).
