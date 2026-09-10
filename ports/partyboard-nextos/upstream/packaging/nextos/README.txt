PartyBoard — Mario Party 4 on NextOS Elite
==========================================

Native AArch64 port of Mario Party 4 for NextOS Elite.
Source: https://github.com/NextOs-Ports/partyboard-nextos


HARDWARE — READ THIS FIRST
--------------------------

This port targets ONE class of hardware and nothing else:

  * Amlogic device running NextOS Elite
  * ARM Mali-450 MP (Utgard) GPU, fbdev path
  * OpenGL ES 2.0

It was written, built and tested only there. There is no OpenGL ES 3.x,
desktop GL, Vulkan or WebGPU path in this build. The bundled Mali
program-binary cache is tied to that exact driver, and the performance
tuning assumes a Cortex-A53 CPU with a fixed-function Mali-450.

On any other GPU, driver, kernel or distribution this may run, may render
incorrectly, or may not start at all. None of that is supported.


NO GAME DATA INCLUDED
---------------------

This archive contains no disc image, ROM or any other Nintendo/Hudson
asset. You must supply your own legally obtained copy of the game.

  Game ID:    GMPE01  (Mario Party 4, USA)
  Revisions:  Rev. 0 and Rev. 1
  Formats:    .rvz, .iso, .gcm


INSTALL
-------

1. Copy the two folders in this archive to your NextOS storage, merging
   them with what is already there:

     ports/         ->  /storage/roms/ports/
     ports_scripts/ ->  /storage/roms/ports_scripts/

2. Put your disc image in:

     /storage/roms/ports/partyboard/assets/

   The launcher picks the first supported image it finds and writes the
   runtime configuration automatically.

3. Restart EmulationStation (or refresh the game list). Launch
   "PartyBoard" from the Ports menu.


CONTROLS
--------

  A / B / X / Y        South / East / West / North face button
  Main stick           Left analog stick
  C-stick              Right analog stick
  L / R                Left / right analog trigger
  Z                    Right shoulder
  D-pad                D-pad
  Start                Start
  Exit the port        Hold Select + Start

PortMaster's controller configuration takes priority. A mapping for the
Twin USB PS2 adapter (0810:0001) is bundled as a fallback. Up to four
gamepads are enumerated independently.


NOTES
-----

  * Default internal render scale is 0.50, upscaled to 1280x720 by the
    Mali fbdev backend.
  * Saves, settings and generated caches stay inside
    ports/partyboard/runtime/ — nothing is written outside the port.
  * Shader and pipeline caches ship pre-warmed. They validate their GL
    fingerprint on startup and rebuild themselves if the driver differs.


CREDITS
-------

This port is built on other people's work:

  * Mario Party R&D and the PartyBoard contributors — the decompilation
    and PC port this fork is based on.
    https://github.com/mariopartyrd/partyboard
  * Luke Street (encounter) and the Aurora contributors — the
    GameCube/Wii source-level compatibility layer. MIT.
    https://github.com/encounter/aurora
  * Brian Degenhardt (bmdhacks) — the GLES/GLES3 backend for Aurora,
    which cut the renderer over from Dawn/WebGPU and built the
    GX-to-GLES translation this port stands on. Our OpenGL ES 2.0 path
    for Mali-450 extends his work; it does not replace it.
    https://github.com/bmdhacks/aurora
  * The Dusklight team, and the Twilight Princess decompilation team
    behind it — that GLES renderer was built and proven on Dusklight,
    their reimplementation of The Legend of Zelda: Twilight Princess.
    Mario Party 4 renders on this hardware because that groundwork
    already existed. Dusklight is released under CC0 1.0.
    https://github.com/TwilitRealm/dusklight
    https://github.com/zeldaret/tp
  * Axiomatic Data Laboratories (AxioDL) — the MusyX reconstruction the
    software synthesizer plugs into. MIT.
    https://github.com/AxioDL/musyx
  * byuu and the higan team — libco. ISC.
    https://github.com/higan-emu/libco
  * The SDL contributors — SDL3, with the Mali fbdev video backend.
  * NextOS Elite — the OpenGL ES 2.0 backend for Mali-450 (Utgard),
    the MusyX software synthesizer and endianness work, four-player
    input, launcher, packaging and performance tuning.

License notices for the bundled components are in
ports/partyboard/licenses/.


LEGAL
-----

Nintendo, Mario Party and related names and assets are trademarks or
copyrights of their respective owners. This is an unaffiliated community
port, not endorsed by Nintendo or Hudson Soft, and it distributes no
game data.
