# Spruce/Miyoo Flip PC environment

This environment mirrors the mixed-ABI topology proven on a physical Miyoo
Flip:

- NXExtract runs as AArch64;
- NXSplash and the Titan Souls guest run as ARMHF;
- the default ARMHF interpreter path is absent;
- spruce provides an alternate ARMHF interpreter;
- the persistent 32-bit chroot and the reduced muOS root provide the ARMHF
  closure;
- the selected ARMHF SDL reports KMSDRM and dummy.

The firmware is never copied into Git. Preparation requires the matching
spruce 4.3.4 archive and checks its exact size and SHA-256 before extraction.
The output is created transactionally outside the repository.

Prepare:

    python3 framework/tests/device-environments/spruce/prepare.py \
      --archive /path/to/spruceV4.3.4.7z \
      --output /path/to/firmware-environments/spruce-4.3.4

Verify the prepared environment:

    python3 framework/tests/device-environments/spruce/verify.py \
      --environment /path/to/firmware-environments/spruce-4.3.4

The verifier checks source receipts, file hashes, ELF classes, the mixed-ABI
path order and, when clang plus qemu-arm-static are available, asks the real
ARMHF SDL for its compiled video drivers. It does not initialize SDL video and
does not touch DRM, Mali, framebuffer, ALSA or input devices.

## Os dois casos SDL

Com o ambiente preparado, os dois casos que faltavam para a promoção mixed-ABI
rodam contra a lista de drivers que a **SDL 32-bit da própria firmware**
publica — medida ao vivo pelo probe ARMHF sob `qemu-arm-static` com o loader
alternativo da imagem, nunca lida de um JSON:

    python3 framework/tests/device-environments/spruce/sdl-hint-cases.py \
      --environment /path/to/firmware-environments/spruce-4.3.4

- **sem hint**: nada é injetado e a autodetecção da SDL fica intacta
  (`action=no-hint`, `SDL_VIDEODRIVER` ausente);
- **hint incompatível injetado** (`x11`, que essa SDL não compilou): é
  removido (`action=cleared-unsupported`), e a SDL volta a escolher entre os
  drivers dela;
- **controle**: um driver que a firmware publica (`KMSDRM`) é preservado
  (`action=preserved-supported`).

A decisão é do sanitizador REAL do nxgl (`nxgl_sanitize_sdl_video_hint_v2`),
compilado com o seam de teste apenas para receber a lista medida. O gate não
inicializa vídeo e não toca DRM, Mali, framebuffer, áudio ou input.
