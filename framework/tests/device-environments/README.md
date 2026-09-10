# Device environments

This directory defines optional, local PC environments for firmware-specific
tests. Each device gets one isolated subdirectory with:

- a small source contract committed to the framework;
- a transactional preparer that consumes an owner-supplied firmware archive;
- a verifier that performs only the device-specific checks;
- no firmware binary, image, credential, address or personal path in Git.

These environments complement the synthetic firmware matrix. They do not claim
physical GPU, display, audio or input evidence.

Currently implemented (seven):

- `spruce`: Miyoo Flip, spruce 4.3.4 — AArch64 host plus ARMHF runtime, the
  alternate loader, 32/64 isolation and the two SDL hint cases.
- `muos`: RG40XX-H, muOS 2601.1 — both roots with their own interpreters,
  isolation, the Python the image ships, renderer providers and the three SDL
  hint cases per ABI.
- `knulli`: RG CubeXX, Knulli 20250813 — viewport only. Each geometry is read
  from the branch that defines it, and the pure resolution policy is exercised
  with the parsed values.
- `arkos`: R36S, ArkOS v2.0 — multiarch roots, the Debian `libc.so` linker
  script, proven soname chains kept apart from providers without `DT_SONAME`,
  KMSDRM on both ABIs, and the profile routes cross-checked (including the
  mixed-ABI ARMHF route of dArkOS).
- `crossmix`: TrimUI, CrossMix-OS v1.3.0 — card tree in a ZIP: declared roots,
  the control file the firmware actually executes, an ABI audit of the whole
  library directory and the controller-discovery chain.
- `amberelec`: RG351P, AmberELEC 20230203 — the OpenAL library and the
  `alsoft.conf` that forces a driver, ALSA/Pulse as real ELFs, and the login
  profile proven to load its fragments.
- `rocknix`: RK3566-Specific 20260801 — image identity, GPT/FAT32/SquashFS,
  AArch64 userspace inventory, the target SDL and late-bound EGL/GLES symbols.

Every preparer keeps the image and the output outside the repository, contains
each path under the prepared environment, refuses symlinks before resolving,
runs external tools with a clean environment and a timeout, and writes a
per-file receipt with size and SHA-256. Every verifier refuses a changed,
missing, extra or symlinked file. `framework/tests/test_device_environments_negative.py`
proves those refusals without needing any firmware image.
