# Streets of Rage 4 — aarch64 / Linux (PortMaster) — Community Test

**First NATIVE port of Streets of Rage 4 to aarch64 / Linux.** This is **not** a so-loader —
it runs the actual game code (MonoGame / .NET 9) on a bundled **.NET 9 CoreCLR** runtime + a
**MonoGame build compiled for OpenGL ES 2.0**. Developed/tested on **Mali-450 (Utgard, fbdev,
832 MB RAM)**.

## Status — Playable
- ✅ Full flow: title → stage select → gameplay, **with audio**
- ✅ Original **Wwise** audio (music, voices, combat SFX) via a light OpenAL reimpl; music
  switches cleanly between scenes/tracks (no overlap)
- ✅ Textures converted **ASTC → ETC1** on first run (Mali reads ETC1 in hardware, ~400 MB lighter)
- ✅ Native gamepad (SDL); **SELECT + START quits**
- ✅ **Universal binaries** (all native libs ≤ glibc 2.27 / GLIBCXX 3.4.11) — runs on glibc 2.30+

## Install — BYO-data (bring your own APK)
This port ships **no game data**. You provide your own legal copy:
1. Copy your **Streets of Rage 4 v1.4.5 APK** into the `sor4/` folder.
2. Open the port. On first run a progress window asks (on the controller):
   - **Texture downscale**: `3` (recommended, ~1 GB RAM) / `2` (sharper) / `1` (full)
   - **Mode**: FAST (all cores) / LOW MEMORY
3. It extracts everything from your APK, patches the game code, converts textures to ETC1, then
   **deletes the APK**. Roughly **~20 min on a Mali-450**. The game opens automatically.
   Subsequent launches are instant (runs once).

## Requirements
- aarch64 Linux, **glibc 2.30+**, OpenGL ES 2.0 (Mali fbdev or KMS)
- ~4 GB free space **temporarily** during first run (freed after — APK is deleted)
- Your own **SOR4 1.4.5 APK**

## Controls (editable in `sor4.gptk`)
Native pad: **A** = attack, **B** = jump, **X** = special, **Y** = run, **Start/Select** = menus,
**D-pad/stick** = move, **SELECT + START** = quit.

## Please test / known
- Confirmed on **Mali-450 (Utgard, GLES2, fbdev)**. Reports welcome on other CFWs / GPUs
  (Bifrost / Valhall) and resolutions.
- **Known (Valhall/S905X5M):** an occasional mid-stage HDMI re-sync freeze (image freezes, audio
  continues) — under investigation; not seen on Mali-450/Bifrost.
- First-run texture conversion is one-time and CPU-heavy; on very low-end devices use downscale 3
  + LOW MEMORY.

## Credits / licenses
Porter: **NextOs-Ports**, on the `nextos_ports_android` framework. Bundles .NET 9 runtime (MIT),
MonoGame (Ms-PL), SDL2 (zlib); OpenAL/opus from the CFW. See `sor4/licenses/`.
Game © DotEmu / Guard Crush Games / Lizardcube — **not distributed here** (BYO-data).
