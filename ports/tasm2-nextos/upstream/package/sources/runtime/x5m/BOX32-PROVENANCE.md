# X5M Box32 runtime provenance

This component is selected only for an AArch64 NextOS host whose device tree
contains the tested `amlogic, s7d` compatible value. It is not a general
Box32 runtime for other devices or older distributions.

`box64` is Box64 built with `BOX32=ON` from upstream commit
`3ec5de03c786333ed8d5a51c5b35a8bd6e22b229`, plus a bounded downstream
patch to `src/custommmap.c`, `src/tools/box64stack.c` and the audited SDL2
wrapper declarations. The patch keeps guest-visible state below 4 GiB,
allows native Mali/DRM mappings above 4 GiB and incrementally exposes the
audited SDL imports used by the i386 loader. The exact downstream patch is
SHA-256
`4d1042b5fadd1a5ad16b47882c62d79aae12f10c450ede84b81bf079f3ef6ad7`.
Box32 v6 passed three repeated 256-cycle condition-variable stress runs. The
final scoped profile explicitly uses `BOX64_DYNAREC=1`,
`BOX64_DYNAREC_BIGBLOCK=0` and `BOX64_DYNAREC_SAFEFLAGS=2`; experimental eager
mode is excluded. On the real X5M it completed 11,034 gameplay frames and a
6,213-frame reopen, both RC0, with save create/update/reload, physical controls
and audio all passing.
Box64 is under the MIT license preserved in `licenses/Box64-MIT.txt`.

`native/libSDL2-2.0.so.0` is sdl2-compat 2.32.71 development source from
upstream commit
`3e1fa90d301428ace65d5c8b371e93d2c59c3d65`, built as AArch64 against the
NextOS system SDL3 headers. Release 1.1.8 builds Box32, sdl2-compat and the
i386 loader deterministically with Debian Buster and
`SOURCE_DATE_EPOCH=1785628800`; their maximum GLIBC requirements are 2.28,
2.17 and 2.17 respectively. The embedded SDL string is
`SDL-2.32.71-no-vcs`, because the hermetic build intentionally omits unrelated
parent-tree metadata. Its zlib license is preserved in
`licenses/SDL2-compat-zlib.txt`.

Exact public runtime files:

| Path | Size | SHA-256 |
| --- | ---: | --- |
| `runtime/x5m/box64` | 25,622,728 | `d73aa019eefd4e553acda8ef7a126f88101cd1cdf46502e4060c192412c3f4dc` |
| `runtime/x5m/native/libSDL2-2.0.so.0` | 451,256 | `798051928c553ee27fde9e2d555be4a9fe4ddcefbbb72f59252e427cbcb0d452` |
| `asm2_127_x86_box32` | 196,508 | `201c2ef029451a005f047090db05f8f0d9f61b43eccf5309b936ba4306a7b110` |

The game library `libtasm2-x86.so` is proprietary and is not included. The
owner-data extractor creates it from the user's validated Android 1.2.7d APK.

The sdl2-compat bytes above remain frozen. The earlier i386 loader
with SHA-256
`fd6b48f4d89b1b9ff67af2cb34a67a4e61d7259eed34e52c781ff91a85481b8d`
is explicitly rejected because its lazy resolver entered C with incorrect
i386 stack alignment. The packaged `201c2e...` loader retains the corrected
ABI boundary in the low-glibc rebuild and is valid only with the exact scoped
profile above. The Box32 host starts under AArch64 QEMU and executes the i386
guest loader to its expected missing-owner-library boundary. The exact rebuilt
X5M bytes have not yet had a new physical-device session. The Box32 host
source, patch and runtime safety profile are unchanged from the physical
gameplay baseline; the i386 loader changes only the shared first-run input
path described by release 1.1.8 and reaches its expected owner-library boundary
under the QEMU/Box32 smoke test.
