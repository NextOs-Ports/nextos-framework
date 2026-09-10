## Gate (sem mudança de versão) — 2026-08-29

- O gate passa a fixar explicitamente a **espera de cinco segundos** da tela
  obrigatória: toda a suíte compila `NXSPLASH_DURATION_MS` para 20 ms para
  ficar rápida, então nada ali notaria o default embarcado sendo trocado. O
  manifesto de release já protege a fonte por hash, mas isso é uma proteção
  cega; agora o contrato é nomeado, e um reseal futuro que mudasse 5000 para
  outro valor falha com a razão certa em vez de passar por ser "só mais um
  hash novo". O gate também recusa o override rápido vazando para a fonte.
- Nenhum byte de artefato mudou: apenas o gate.

# nxsplash changelog

## 0.1.2 - 2026-08-14

- Recover KMSDRM images whose usable GPU provider is exposed only as
  `libEGL.so`/`libGLESv2.so`, after normal discovery fails and without
  overriding an explicit firmware/user provider choice.
- Replace the automatic real-TTY fallback with a bounded true-color `/dev/fb0`
  software surface that reuses the canonical SDL `draw_screen` layout, font,
  colors, progress and opaque-alpha behavior unchanged.
- Preserve that graphical fallback on framebuffer drivers that permit bounded
  reads/writes but reject `mmap`, using the same software frame and explicit
  row-safe writeback instead of falling through to a TTY or changing layout.
- Keep the old ASCII TTY renderer only behind the explicit diagnostic pair
  `NX_SPLASH_TTY_DIAGNOSTIC=1` plus a validated `NX_SPLASH_TTY` path, and restore
  terminal attributes and cursor visibility after that diagnostic.
- Add deterministic RGBA goldens at 320x240, 640x480 and 1280x720, pin the
  unchanged `draw_screen` body, and test framebuffer bounds/channel conversion
  plus the no-automatic-TTY contract.

## 0.1.1 - 2026-08-14

- Resolve the kernel-published active virtual terminal when PortMaster leaves
  `CUR_TTY` empty, so a failed SDL/KMSDRM pre-game renderer still produces a
  visible terminal instead of only preserving the delay headlessly.
- Keep an explicit non-empty console path authoritative and validate the
  discovered `tty<N>` device as a real character device before opening it.
- Record the selected terminal path in diagnostics and cover the fallback in
  the automatic framework matrix.
- Keep the `RETRO ELITE` signature and complete terminal frame visible for the
  full console countdown, including the active-VT fallback.
- Quote the pinned Zig executable path so reproducible builds also work from
  archive locations containing spaces.

## 0.1.0 - 2026-08-14

- Add the mandatory five-second pre-runtime screen for newly generated ports.
- Render a bilingual terminal-style boot screen with game title, verified-data,
  controls-ready, loading and countdown states, plus the Retro Elite signature.
- Resolve SDL2 dynamically and preserve the firmware-selected backend first.
- Retry only SDL-advertised, capability-plausible backends after a real failure.
- Fall back to the active framebuffer console and finally to a timed headless
  handoff without blocking the game's native lifecycle.
- Provide reproducible low-glibc builds for every nxbootstrap architecture.
