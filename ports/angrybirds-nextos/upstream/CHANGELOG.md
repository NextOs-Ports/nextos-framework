# Changelog

## 1.1.9 — 2026-09-03

- Vídeo: nxgl 0.3.5 e nxcompat 0.4.0 (Framework V4 congelado, tag `framework-v4`,
  tags `nxgl-v0.3.5`/`nxcompat-v0.4.0`) vendorizados no lugar do nxgl 0.2.8 e do
  nxcompat 0.2.1. O portão de "promoção do provedor SDL" (dladdr + dlopen
  RTLD_NOLOAD + dlsym) foi substituído pela prova de vida do conjunto ES2
  (`nxgl_gles2`, 142 pontos de entrada pelo resolver do SDL, glGetString do
  próprio provedor). Motivo: no ROCKNIX/Panfrost (Mesa com GLVND) o endereço
  devolvido por eglGetProcAddress mora em libGLdispatch, que não exporta os nomes
  gl*, e o jogo morria em "provedor SDL validado nao pode ser promovido" /
  "negociacao de video/GLES2 falhou" logo após abrir um contexto ES 3.1 válido.
  Nenhum handle passa a ser promovido ao namespace global; os 69 imports gl* do
  convidado continuam vindo de SDL_GL_GetProcAddress (ab_gl.c).
- O próprio loader deixou de importar `gl*`/`egl*` como símbolos indefinidos de
  resolução preguiçosa (era isso que exigia a promoção global): as chamadas
  `gl*` do loader passam pelos ponteiros provados do `nxgl_gles2` e as onze
  entradas EGL do EGLWrapper (contextos compartilhados) são ligadas uma vez pelo
  `nxgl_egl_binding` (V4-GRAPHICS-03: resolver do SDL, dladdr, prova de todos os
  imports e do contexto corrente). `nm -D --undefined-only` do ELF não mostra
  mais nenhum `gl*`/`egl*`. Regressão no dArkOS/K36S (KMSDRM, Mali-G31):
  ES2 142/142 vivo, EGL ligado, áudio ALSA, ~54 fps, saída limpa.
- Sem mudança de controles, áudio, launcher (nxbootstrap 0.6.26), NXExtract
  (1.2.12) ou receita. Quem vem de versão anterior: instalação limpa.

## 1.1.8 — 2026-08-19

- Launcher regenerado com nxbootstrap 0.6.26: o validador do resultado do
  NXExtract passou a ser compativel com engines futuros (crise dos updates
  hibridos no muOS — launcher antigo + engine novo matava a instalacao com
  "unknown terminal result schema"). Sem mudanca de gameplay.
- Quem vem de versao anterior: faca instalacao LIMPA (apague a pasta
  angrybirds E o "Angry Birds Classic.sh" antes de extrair).

## 1.1.2-test.3

- Audio path revision exercised on ArkOS RK3326 / Mali-G31 with the exact
  release executable: native boot, ALSA output, first content transition and
  clean exit.
- Universal cursor kept from 1.1.1: right stick moves the pointer, `A` and `R3`
  tap, simultaneous `A`+`R3` still counts as a single touch, the pointer hides
  after two idle seconds and returns on movement.
- Left stick keeps driving the slingshot; D-pad and triggers keep their native
  actions.
- Public executable: ARMv7 hard-float, `GLIBC_2.17` ceiling, no RPATH/RUNPATH,
  no executable stack.

## 1.1.1

- Universal controller fix: polished pointer in menus and in gameplay, powers
  usable during a level, launcher no longer depends on the external `stat`.
- Star persistence confirmed physically (level 1 went from one to two stars).

## 1.1.0

- First universal PortMaster release of the ARMv7 port.
