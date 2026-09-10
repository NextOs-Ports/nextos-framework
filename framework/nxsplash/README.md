# nxsplash 0.1.2

`nxsplash` is the framework-owned pre-runtime screen used by ports generated
with nxbootstrap 0.6.9 or later. It runs only after NXExtract, the optional
prepare adapter and the required-payload gate have completed, and exits before
private game libraries or native game entry points are loaded.

The public helper is always named `nxsplash-nextos` and receives only the
manifest-owned game title. Its duration is compiled to five seconds; launchers
do not expose a skip switch or a per-port disable option. A missing or unsafe
helper is a malformed package and fails before the game starts. A renderer
failure is different: the helper falls back from SDL to a code-native software
surface copied safely to a compatible `/dev/fb0`, using the exact same layout,
font and RGBA palette. If neither graphical path is available, it preserves the
fixed handoff delay headlessly and returns success so a firmware display
limitation never prevents an otherwise compatible game from running.

The framebuffer path accepts only a real, non-symlink character device with a
packed true-color 16-, 24- or 32-bit layout whose visible bounds fit the
reported framebuffer storage. It uses a shared mapping when supported and a
bounded row-safe writeback when the driver rejects `mmap`. Unsupported metadata
fails closed to the timed headless handoff rather than risking an out-of-bounds
or wrongly formatted write. Every drawn pixel, including the final black
handoff frame, is opaque.

The old real-TTY representation is not an automatic public fallback. It is
available only for explicit diagnostics when both `NX_SPLASH_TTY_DIAGNOSTIC=1`
and a valid `NX_SPLASH_TTY=/dev/ttyN` are supplied. The `NX_SPLASH_TTY` value
already emitted by older launchers does not opt into it by itself.

## Compatibility contract

- The inherited/default SDL selection is attempted first.
- `SDL_VIDEODRIVER` is never selected by a device or firmware name.
- Fallback considers only drivers advertised by the loaded SDL and only when
  their required display endpoint exists.
- If window creation still fails and the firmware did not publish explicit
  EGL/GLES providers, the helper retries the portable unversioned runtime
  names once. Explicit provider choices are never replaced.
- If SDL remains unavailable, `/dev/fb0` receives the canonical `draw_screen`
  output through a bounded software surface and the advertised framebuffer
  channel layout; `mmap` and writeback use the same frame and no second visual
  design exists.
- No compositor, display server, audio service or input service is started.
- A real TTY is diagnostic-only and requires a separate explicit opt-in; it is
  never inferred from an active VT or selected by firmware name.
- No game library directory enters the splash process.
- No game input is read and no native lifecycle function is invoked.
- SDL state lives in a separate process and is destroyed before game launch.
- The generated package pins the helper byte hash and executable mode.
- Existing ports pinned to older framework versions remain unchanged.

## Build

Run `./tools/build-release.sh`. The build downloads the official Zig 0.16.0
toolchain only when it is absent, verifies its pinned SHA-256 before use, and
targets glibc 2.17 for AArch64, ARMv7, x86_64 and i386. It then audits every ELF
against the public `GLIBC_2.30` ceiling and writes
`release/manifest-v1.json`. Re-running the build with the same source and
toolchain must reproduce the same hashes.

Run `./tests/run.sh` for the source, manifest, architecture, runtime,
low-glibc, framebuffer-layout and deterministic visual-golden gates. The
goldens cover 320x240, 640x480 and 1280x720 and pin the unchanged canonical
`draw_screen` body.

The 0.1.2 portable-provider recovery was physically validated on an ArkOS
Mali-G31 device with SDL KMSDRM in the pre-fbdev candidate whose AArch64 helper
SHA-256 was
`94d6df5bb65f2453544f9831cc2aea33cd175bcf4fa336fb1808121a51510118`;
the SDL `mali` renderer remains the physically validated path on the NextOS
Mali-450 family. Because adding fbdev changes the rebuilt helper bytes, the
current candidate still requires an exact-artifact physical acceptance gate
before 0.1.2 is tagged. These device facts validate the screen only, not any
individual game's support claim.

## License and provenance

The component is MIT licensed. Its backend-neutral SDL loading and firmware
session discovery derive from the published NXExtract universal UI 1.2.6; the
splash renderer and runtime contract are independent and do not reuse game
code or proprietary data.
