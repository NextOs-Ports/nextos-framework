# NOTICE

This repository combines work from several sources under different terms. This
file records who owns what, so anyone reusing the code knows exactly what they
are allowed to take and who to credit.

**Short version:** the NextOS Elite work is MIT — take it, use it commercially,
modify it, ship it, just keep the copyright notice. The rest is not ours to
give away.

---

## Component map

| Component | Where | Copyright | License |
|---|---|---|---|
| NextOS Elite contributions | commits on top of upstream (see below) | 2026 NextOS Elite contributors | **MIT** — see [LICENSE](LICENSE) |
| PartyBoard | this repository's upstream history | Mario Party R&D and the PartyBoard contributors | **None declared** — all rights reserved by default |
| Reconstructed game code and assets | `src/`, `orig/` (not distributed) | Nintendo / Hudson Soft | Not licensed to anyone here |
| Aurora | `extern/aurora/` | 2022 Luke Street | MIT — `extern/aurora/LICENSE` |
| Aurora GLES/GLES3 backend | `extern/aurora/lib/gl/`, `lib/gfx/`, `lib/gx/` | Brian Degenhardt (`bmdhacks`), for the Dusklight project | MIT, as part of Aurora |
| MusyX reconstruction | `extern/musyx/` | 2023 Axiomatic Data Laboratories | MIT — `extern/musyx/LICENSE` |
| libco | `extern/libco/` | byuu and the higan team | ISC — `extern/libco/LICENSE` |
| SDL3 | linked at runtime | The SDL contributors | Zlib |

---

## What the NextOS Elite MIT grant covers

These are ours, and you may take them under the MIT terms in [LICENSE](LICENSE):

- **The OpenGL ES 2.0 backend for Mali-450 (Utgard).** Aurora's GLES backend, by
  Brian Degenhardt, targets a Mali G31 with OpenGL ES 3.x. Bringing it down to
  ES 2.0 is our work: the ES2 EGL configuration, the GLSL ES 1.00
  (`#version 100`) shader emission path, and the ES2-level rework of the GL
  loader, device and buffer layers, texture-copy and palette conversion, clears,
  pad handling, timing and the fbdev present path. Roughly 1,700 lines across 39
  files. **The GLES/GLES3 backend it builds on is not ours** — see
  [What the grant does NOT cover](#what-the-grant-does-not-cover).
- **The dynamic-palette width fix**, and the **GX FIFO drain optimization** with
  memoized shader/pipeline state and optimized vertex expansion and hashing.
- **The MusyX software synthesizer** — ADPCM decode, per-voice volume and pan,
  ADSR envelopes, 32 kHz stereo mixing into the AI destination buffer, the ARAM
  sample path, and the restored `AIRegisterDMACallback` chain. Plus the
  eight-layer MSM/MusyX endianness work and the `msmSysInit` pointer fix.
- **The four-player input layer** and its controller mappings.
- **`packaging/nextos/`** — the NextOS launcher, the package builder, and the
  cache/metadata tooling.
- **The Mali-450 performance tuning** in the port itself — the shadow and
  particle reduction paths and the render-scale work.
- **The documentation** in `README.md` and `HANDOFF.md`.

### How to credit us

Keep the copyright line from [LICENSE](LICENSE) in your source or
documentation. That is the whole requirement. A link back to
<https://github.com/NextOs-Ports/partyboard-nextos> is appreciated but not
required.

### Reusing the ES 2.0 work

The ES 2.0 path is the most broadly useful piece here — it is not specific to
Mario Party 4, and it should work for any Aurora-based GameCube port that has to
run on an ES 2.0-only GPU. It lives in our Aurora fork, on the branch
`nextos-mali450-gles2`:

<https://github.com/NextOs-Ports/aurora-mali450-es2>

Because Aurora is MIT and our changes are MIT, that work can be merged back into
Aurora — by Brian Degenhardt, by upstream, or by anyone else — without asking us
for anything. That is deliberate. The same is true of the synthesizer in our
MusyX fork, branch `nextos-partyboard`:

<https://github.com/NextOs-Ports/musyx-nextos>

---

<a id="what-the-grant-does-not-cover"></a>
## What the grant does NOT cover

**Upstream PartyBoard.** The project this repository forks
(<https://github.com/mariopartyrd/partyboard>) declares no license. Under
default copyright law that means all rights are reserved by its authors. We
cannot sublicense it, and neither can anyone downstream of us. Our MIT grant
applies only to our own commits, not to the tree as a whole.

**The game.** The reconstructed Mario Party 4 code is derived from a Nintendo
and Hudson Soft work. Nintendo, Mario Party, and all related names, characters
and assets are trademarks or copyrights of their respective owners. Nothing
here grants any right to that material, and no game data is distributed in this
repository or in its release packages.

**Aurora's GLES backend.** The GLES/GLES3 renderer our ES 2.0 work extends was
written by **Brian Degenhardt (`bmdhacks`)** for the
[Dusklight](https://github.com/TwilitRealm/dusklight) project — the cutover from
Dawn/WebGPU, the GX-to-GLES shader emitter, EFB copy/resolve, the ring buffers,
UI compositing, the program-binary cache and the GPU-object leak fixes. It is
MIT as part of Aurora, and it is his, not ours. Without it this port would not
render at all.

**The bundled dependencies.** Aurora, MusyX, libco and SDL each keep their own
license file in their own source directory, and the release package ships the
applicable notices under `licenses/`. Our changes to Aurora and MusyX are
offered under the same MIT terms as those projects, so nothing is more
restricted than it was upstream.

---

## Credits

Beyond the license terms, credit where it is due:

- **[Mario Party R&D](https://github.com/mariopartyrd) and the PartyBoard
  contributors** — the decompilation and PC port this fork is built on. Without
  their reconstruction of the game code there is nothing to port.
- **Luke Street (`encounter`) and the Aurora contributors** — the source-level
  GameCube/Wii compatibility layer that models the GX pipeline.
- **Brian Degenhardt (`bmdhacks`)** — the GLES/GLES3 backend for Aurora. Our
  ES 2.0 path is an extension of his work, not a replacement for it.
- **The [Dusklight](https://github.com/TwilitRealm/dusklight) team and the
  [Twilight Princess decompilation](https://github.com/zeldaret/tp) team** — that
  GLES renderer was built and proven on Dusklight, their reimplementation of
  *The Legend of Zelda: Twilight Princess*. Dusklight is released under CC0 1.0.
- **Axiomatic Data Laboratories (AxioDL)** — the MusyX reconstruction. MusyX
  itself is Factor 5 middleware, and the documented SAL abstraction layer it
  ships is what made a PC-side synthesizer possible at all.
- **byuu and the higan team** — libco.
- **The SDL contributors** — SDL3 and its Mali fbdev video backend.

This port is not affiliated with, authorized by, or endorsed by Nintendo or
Hudson Soft.
