# Prizefighters 2 1.09.3 — recon and PairIP analysis for NextOS Mali-450

Recon 2026-07-29, extended the same day with the PairIP import analysis. This
file contains no game data. The XAPK, APKs, native libraries, metadata, assets
and compiled binaries are BYO-data and never enter the public repository.

## Identity and source

- Game: Prizefighters 2, Koality Game
- Package: `com.koalitygame.prizefighters2`
- Version: 1.09.3, versionCode 49
- XAPK SHA-256:
  `d62d2d60f8e8dac6537aeb9e1949138afbd1c8ab580b195538d5aa9dda1f8cbf`
- Splits: `arm64-v8a` and `armeabi-v7a`; the port uses arm64.
- No OBB and no separate asset pack. The Unity content is in the base APK.
- The authorised device and the local XAPK path live only in the port's private
  memory, never here, in the README, in a commit or in a package.

## Engine and content

- Unity `2022.3.62f2`, IL2CPP arm64, metadata v31 in the clear (`AF 1B B1 FA`).
- arm64 libraries: `libmain.so`, `libunity.so`, `libil2cpp.so`,
  `lib_burst_generated.so`, `libpairipcore.so`.
- Two scenes in BuildSettings: `Assets/Scenes/Menu.unity` and
  `Assets/Scenes/Game.unity`.
- Built-in Render Pipeline, Gamma color space; not URP.
- `data.unity3d` holds the scenes and assets. No remote catalogue and no CDN URL
  is needed for content. Billing and Unity Services are peripheral.
- Inventory: 325 `Texture2D`, 45 shaders, 45 `AudioClip`, 640 sprites,
  331 animation clips, 14,823 `MonoBehaviour`.

## Render: known wall, small scope

- The manifest requires GLES 3.0 (`glEsVersion=0x00030000`).
- BuildSettings has `m_GraphicsAPIs=[11]`, i.e. OpenGLES3.
- All 45 shaders carry platform 9 only, with `#version 300 es` source; there is
  no ready GLES2 variant.
- `m_MTRendering=true`. Reproduce the native flow first; only change that
  setting with evidence of a job-system deadlock during loading.
- The game is 2D on the Built-in pipeline, which makes a GLES3→GLES2 transpiler
  specific to this game more realistic than it would be under URP.
- Textures: 192 RGBA32, 120 RGB24, 10 ETC2_RGBA8, 2 Alpha8, 1 ETC_RGB4.
  Mali-450 takes ETC1 but not ETC2. Convert the ten ETC2 to RGBA or
  ETC1+alpha before use; do not switch on a global `npot_fix`.
- Largest atlases are 1024×1024. No ASTC in the main inventory.

## PairIP: what it actually does to the native libraries

This is the part the first recon pass understated, and it changes the plan.

`libunity.so` calls **336 external functions through its PLT**, but the dynamic
tables PairIP ships describe only **136** of them (139 undefined symbols in a
144-entry `.dynsym`). The other **200 `.got.plt` slots have no relocation at
all**: they still hold the link-time value, PLT0's address, and PLT0 branches
through `.got.plt[2]`, which bionic never fills. So the first call to any hidden
import branches to 0.

What fills them is `libunity.so`'s `DT_INIT`, a stub PairIP appended in an extra
executable page (`0x12d0000`) whose entire body is:

```
    x0 = "qFzzC3MvCdTy6brw"      (an asset name)
    x1 = &module_base            (one argument)
    x2 = 1                       (argument count)
    ldr x17, [ExecuteProgram GOT slot at 0x12c8000]
    blr x17
```

`libil2cpp.so` has the same shape and hides **199** of its 569 PLT slots;
`libmain.so` and `lib_burst_generated.so` hide none.

So a stub for `ExecuteProgram` cannot be the whole answer: something has to put
336 (and 569) real addresses into those GOTs.

### The Java side, from `classes.dex`

- Application is `com.pairip.application.Application`; its static initialiser
  calls `StartupLauncher.launch()`, which is
  `VMRunner.invoke("3H0fStvCiQxkzzNR", null)` — the licence/integrity program.
- `VMRunner`'s static initialiser is `System.loadLibrary("pairipcore")`.
- `VMRunner.invoke(name, args)` opens the base APK as a `ZipFile`, reads
  `assets/<name>`, passes it through `VmDecryptor.decrypt(bytes, 49, false)` —
  **which is a no-op, it returns its argument unchanged** — and hands the bytes
  to the native `executeVM([B[Ljava/lang/Object;)Ljava/lang/Object;`.
- `libpairipcore.so` imports no file I/O at all, which is why the program bytes
  have to come back through JNI like this.
- The `.IAP` assets are `\0IAP` + version 2 + payload at byte entropy 7.92, so
  the payload is encrypted and the key is derived natively.

### Recovering the hidden imports offline

PairIP rebuilds `.dynsym`, the head of `.dynstr` and the head of `.rela.plt`,
but it leaves three things in place, and together they are enough:

| Survives | What it gives |
|---|---|
| the original SysV `.hash` (libunity: at 0x26c0, `nbucket = nchain = 343`) | name → symbol index for any candidate name |
| the original `.dynsym` (libunity: at 0x320) | the original `st_name` offset of every symbol, hence its **exact name length** from the gaps |
| the tail of the original `.dynstr` (libunity: from offset 2011) | 155 names outright |
| the tail of the original `.rela.plt` (libunity: entries 137..335) | slot → symbol index for 199 slots |

Two further cross-checks fall out for free: 80 slots appear in both the leftover
original table and PairIP's rewritten one, which pins 80 more
(symbol index → name) pairs directly, and every one of the 176 ground-truth
pairs agrees with the length derived from the offset gaps — 0 mismatches.

`analysis/pf2_imports.py` recovers the tables and `analysis/pf2_symnames.py` names the
symbols, constraining each candidate by both its hash bucket and its exact
length and rejecting glibc-only aliases (bionic has no `*f32`/`_IO_*`/`__libc_*`).
Result for libunity: **323 of 342 symbols named, 250 of 336 slots named**, of
which 113 are hidden slots the port now fills itself. The 86 that remain are
slots whose original `.rela.plt` entry fell inside the range PairIP overwrote,
so their symbol index is simply gone; the ARMv7 split is no help because PairIP
hides the same 200-slot positional range there.

### libil2cpp needs a different lever

The same search does find libil2cpp's surviving tables, and they check out:

- the original SysV `.hash` is at **0x14fcc**, `nbucket = nchain = 2657`, ending
  exactly where `.dynstr` begins — so the original symbol count was 2657 against
  the 2458 PairIP ships, and the leftover `.rela.plt` does reference indices up
  to 2591;
- the original `.dynstr` is **fully intact** (PairIP appended its own strings
  after offset 109690 instead of overwriting the head, as it did in libunity);
- the leftover tail of the original `.rela.plt` gives slot → symbol index for
  199 slots (373..571);
- 55 ground-truth pairs, **0 disagreements**, so the tables are the right ones.

But the original `.dynsym` itself is **gone** here — no offset in the file passes
the "st_name points at a string start, st_shndx UNDEF, st_value 0" test (best
score 4 of 199). Without it there is no `st_name` to read, so the length
constraint that carried libunity is unavailable, and hash inversion alone is
ambiguous: 2657 symbols against 2456 candidate strings, with the candidate pool
mixing imports and libil2cpp's own 2371 exports. It names only 6 of the 200
hidden slots, and some of those are exports reached through the PLT rather than
imports.

So libil2cpp's hidden slots have to come from the runtime probe instead: each
unnamed slot gets a stub that reports its slot number and x0..x7 on first call,
and its identity follows from the arguments. The loader already installs those;
what is missing is the iteration.

### The other half: 50 KB of encrypted .text per library

Hiding imports is only one of the two things PairIP does here, and the second one
is what actually decides the port.

Each protected library ships with a **50 KB block at the very start of `.text`
that is not code**. The test is an encoding one rather than entropy: in AArch64
the top-level group is bits 28:25, and 0b0000..0b0011 are reserved, SVE and SME,
which a compiler targeting armv8-a never emits. Real code scores 0.00 on that
measure; these blocks score about 0.25, which is what random bytes give.

| Library | encrypted block | encrypted call targets | share of direct calls |
|---|---|---|---|
| `libunity.so` | `0x397710..0x3a3f10` (50 KB, from the ELF entry point) | 21 | 673 of 167,616 (0.40%) |
| `libil2cpp.so` | `0x118bf7c..0x119877c` (50 KB) | 1 | 512 of 19,170 (2.67%) |
| `lib_burst_generated.so` | its whole 18 KB `.text` | 0 | -- |

The rest of libunity's 14 MB of `.text` is plaintext, so this is a bounded block,
not a packed binary. But the functions inside it are not peripheral. Checked
against the callers: 208 of 208 call sites for `0x399ecc` are in clean code, and
one of them reads

```
    add x0, sp, #0x50
    bl  0x399ecc            ; <- encrypted
    mov x0, x21
    bl  free@plt
```

Three of the 21 are called 73, 107 and 208 times and their return value is used
as a pointer. That is core allocator/container code, and it cannot be stubbed.
`0x398ffc` (14 calls) is the exception: its result is used as
`and wN, w0, #1` and stored in a global, i.e. a bool, which is the shape of a
feature or licence flag.

### What the cipher is, and what that rules out

Measured on libunity's block:

- index of coincidence 1.003, i.e. indistinguishable from random (real code is 2-6);
- **not a repeating-key XOR**: per-position IC stays at ~1.0 for every period from
  8 to 1024, so there is no keystream to fold out;
- 36 distinct 16-byte blocks repeat, 2 to 4 times each, in a 3200-block sample.
  In random data the expected number is zero, so the mode is **ECB** with a
  16-byte block. The repeats sit only 256 or 480 bytes apart, which is the
  plaintext's own structure showing through;
- the **armeabi-v7a** build is protected identically (its first 50 KB of `.text`
  reads entropy 7.97 with 0.40% zero bytes, against 6.0 and 15% for the rest), so
  there is no plaintext copy of those functions in the other ABI either.

ECB without the key gives away repetition and nothing else, so there is no
short-cut here: the key has to come from the VM.

### What the VM's container parser turned out to be

`libpairipcore+0x4d164` is not a decryptor with a key -- it is the interpreter,
and it is control-flow flattened. Its state variable is compared against magic
32-bit constants (0x4442cbda, 0x237ce5c1, 0x28baf11b, 0x2c6b873c, 0xc9ac7145,
0xdbfa7a2b, 0xfb9e30c9, ...) to pick the next block, starting from state
0xdf7acf88. The bytecode words themselves are read as
`~(word ^ len)` and then reduced `mod len`, i.e. **obfuscated with the program's
own length and nothing secret**. Combined with the environment trace above --
`getauxval` twice and one property read, no `stat`, no `opendir` -- there is no
sign of device-derived key material anywhere in the path.

That is worth stating plainly because it changes what the remaining work is:
this is not a cryptographic problem to solve, it is a control-flow-flattened
interpreter to reverse, which is a project of its own rather than a step in a
port.

`libunity.so`'s `DT_INIT` therefore has two jobs, not one: decrypt that block
**and** patch the GOT. This is what the loader hits in practice -- with the
imports filled, `libil2cpp`'s init array now completes and the run dies with
SIGILL at `libunity+0x399000`, four bytes into the encrypted function that
`0x3bf9ec` calls.

**Corrected verdict: the offline import reconstruction cannot substitute for the
VM.** It is still needed (the VM has to be given a working GOT, and the recovery
is what proves the loader's tables are right), but the port cannot reach gameplay
until either the VM runs correctly or that 50 KB is decrypted another way.

### State of the VM path

With `PF2_PAIRIP_VM=1` the real VM does come up: `libpairipcore`'s init runs,
its `JNI_OnLoad` registers `VMRunner.executeVM`, `libil2cpp`'s `DT_INIT` calls
`ExecuteProgram("fZ9RXXeCrCFThFOY", …)`, our `VMRunner.invoke` feeds it the
asset, and the interpreter starts and resolves symbols through our `dlsym`
(observed: it asks for `environ`). It then faults reading 0x5a250 bytes past the
end of a 205,286-byte program — consistent with the payload being encrypted with
a key the Java layer does not supply. `executeVM`'s ABI was read off the disassembly and the port matches it, which
narrows where the failure is. At `libpairipcore+0x58be0` it does exactly:

```
    len  = GetArrayLength(env, code)          ; JNIEnv slot 171 ([env+1368])
    buf  = operator new(len)
    memset(buf, 0, len)
    GetByteArrayRegion(env, code, 0, len, buf); JNIEnv slot 200 ([env+1600])
    r = vm_entry(&{buf, len, 0}, args)        ; libpairipcore+0x4d164
    operator delete(buf)
    return r
```

Both JNI slots are ours and both are implemented, and the length the VM reads
back is 205,286, exactly the program's size -- so the VM receives the right bytes
through the right calls. It then dies inside the container parser at
`+0x4d164`, and the shape of the failure is specific: at `+0x67264` it runs an
**FNV-1a-64** loop (the constants 0xcbf29ce484222325 and 0x100000001b3 are both
built inline) over a range whose length it took from the program stream, and that
length comes out as 0xffdee4bb. A container it cannot parse is a container it
cannot decrypt.

Two things were ruled out along the way, both of them our bugs first:
`ExecuteProgram` was resolving to the stub even with the VM enabled, because the
import table is built before the modules are loaded (now a per-call dispatcher);
and the fake JNI dropped constructor arguments, so the `java/lang/Long` that
`ExecuteProgram` boxes the module base into arrived empty (boxed primitives now
carry their value and `longValue()` and friends unbox). Neither changed the
outcome, which is what makes the container the remaining suspect.

Running the startup program first was also tried, because that is the platform's
order -- `Application.<clinit>` runs it before `UnityPlayerActivity` loads
libunity -- and it fails the same way at `+0x53bdc`.

The environment the VM inspects before interpreting is small and now traced:
`getauxval(AT_HWCAP)`, `getauxval(AT_SECURE)` and
`__system_property_get("ro.arch")`. It stats nothing and opens nothing, so the
key is not coming from the filesystem or from a device property.

The VM stays behind an environment switch, with `ExecuteProgram` otherwise
resolving to a small three-argument stub (the signature is fixed, not varargs:
every call site sets exactly x0/x1/x2), so the rest of the loader can be
developed and verified in the meantime.

The build also includes CodeStage Anti-Cheat Toolkit. Do not neutralise its
internal logic without evidence that it blocks the port; stub only licence,
billing, analytics, share/social and other peripherals.

## Input and audio

- Unity Input System 1.14 is present.
- Native `Gamepad` bindings exist: sticks, D-pad, four buttons, shoulders,
  triggers and Start. Start from the native Gamepad path, not touch or mouse.
- `uses-feature touchscreen` is optional in the manifest.
- Audio is Unity's internal FMOD; there is no external `libfmod.so`. `libunity`
  contains the Android AudioTrack/OpenSL backends. Discover the backend, rate
  and format at runtime before building the SDL/PulseAudio bridge.

## Viability

Status: **hard candidate**, not drop-in and not an automatic NO-GO.

In favour: arm64, local content, clean metadata, Built-in 2D, few shaders,
mostly uncompressed textures, Gamepad already defined, and the hidden-import
problem is now understood and largely solved offline.

The wall is PairIP's encrypted `.text`, and it is a hard one: 50 KB per library
holding functions that live code calls hundreds of times, decrypted only by the
VM, whose program payload is itself encrypted with a natively-derived key. Until
that is solved, nothing downstream matters. After it: the remaining unnamed PLT
slots, then Unity 2022 IL2CPP with MT rendering, GLES3-only shaders and the ten
ETC2 textures.

## Order of work

1. Re-extract the XAPK to the BYO area outside the repo and check the hash.
2. Generate a lean arm64 scaffold from `kit_essencial/core`; do not copy a whole
   Unity port.
3. Map this APK's Java order, `RegisterNatives`, init arrays, `JNI_OnLoad` and
   Activity callbacks; reproduce the native order with no shortcuts.
4. Recover and fill the hidden PLT slots of every protected library, then prove
   300–1000 frames of the native loop.
5. Create the GLES2 context and the transpiler for the 45 shaders; prove the
   Menu is visible.
6. Follow Menu → Game through the state machine. If it stalls, find the missing
   callback or job; do not force the scene.
7. Convert the ETC2 textures, close FMOD audio and publish the native Gamepad.
8. Validate a playable fight, English, save, SELECT+START and a clean return to
   EmulationStation.

Solution references: only the kit, the recipes, and ports that are explicitly
approved and playable. WIP ports serve only as negative notes; do not copy code,
offsets, lifecycle, shader shims or workarounds from them.

## Correction, 2026-07-29 (second pass): two earlier conclusions were wrong

The first pass concluded that the `.IAP` programs are encrypted with a natively
derived key and that what remained was to reverse an obfuscated VM. Reading the
container out of the disassembly instead of inferring it from entropy shows both
halves of that to be wrong, and the surface that has to be bypassed to be far
larger than measured. Both corrections matter because they point at different
work.

### The programs are masked, not encrypted

`libpairipcore+0x4d164` (`vm_entry`) fetches every operand like this, straight
out of the disassembly at `+0x4d1ec`:

```
x8 = [x27]            ; the program buffer
w9 = [x27 + 8]        ; its length
w11 = [x8 + w10]      ; a word of the program at the current PC
w11 = eon(w11, w9)    ; = ~(w11 ^ len)
w11 = w11 mod w9      ; reduced by the same length
```

`len` is one constant for the whole run, so this is **XOR with a constant, not a
cipher**. That also disposes of the evidence the first pass relied on: XOR with
a constant preserves entropy exactly, so the payload's 7.92 bits/byte never
distinguished encryption from dense bytecode. Confirmed on the Java side too --
`VmDecryptor.decrypt` decompiles to `return bArr`, literally a no-op, and
`SignatureCheck` is a Java-only base64 comparison with no key material and no
native call.

The entry state is therefore computable offline, and
`analysis/pf2_iap_container.py` does it (transcribed from `+0x4d820`):

| | 3H0fStvCiQxkzzNR | qFzzC3MvCdTy6brw | fZ9RXXeCrCFThFOY |
|---|---|---|---|
| length | 232227 | 205867 | 205286 |
| header | `\0IAP` + version 2 + `08 06` | same | same |
| initial PC | 45 | 45 | 45 |
| `u16@35` | 65530 | 65532 | 65532 |

`u16@35` is `-6/-4/-4` as a signed halfword and is stable across programs, so
bytes 0..44 are a real header, not ciphertext.

### Our side of the ABI is faithful

`executeVM` (`+0x58be0`) is exactly what the port implements: `GetArrayLength`
(slot 171) -> allocate -> `memset` -> `GetByteArrayRegion` (slot 200, start 0,
whole length) -> `vm_entry(&{buf, len, 0}, args)`. The length is the Java array
length, which is `ZipEntry.getSize()`; checked against the base APK's central
directory, it equals the extracted file size for every asset. `bl 0x28728` at
the function entry is not a decryption step -- it is a thread-local arena
(`pthread_getspecific` + `realloc`, descriptor in `.data`).

So the buffer, the length, the args and the slot numbers are all correct, and
the VM still leaves the instruction grid.

### The encrypted `.text` surface is ~60x larger than measured

The first pass counted only direct `BL` and concluded 21 + 1 reachable
functions, i.e. "reimplement the entry points". Counting the other three ways a
reference can reach the region (`analysis/pf2_enc_surface.py`) kills that idea:

| | libunity | libil2cpp | burst |
|---|---|---|---|
| encrypted prefix | first 0xC800 of `.text` | first 0xC800 | all 19244 |
| direct BL targets | 23 (756 calls) | 2 (513 calls) | 0 |
| ADRP+ADD address-taken | **243 distinct** | 0 | 0 |
| `R_AARCH64_RELATIVE` into it | 2 | **1090 distinct** | 2 |
| FDEs inside / reachable | 275 / **267** | 1497 / **1092** | 65 / 2 |

The libil2cpp figure is the decisive one: 1090 relocations, each pointing at a
distinct function, is IL2CPP's method-pointer table -- those are the game's own
C# methods, so there is no external copy of them and nothing to reimplement.
The prefix is also a fixed-size budget (0xC800 in both large libraries), not a
chosen set of sensitive functions.

**Net: the `.text` still has to be decrypted, and it cannot be worked around;
but the decryptor is a masked program we can read, not a cipher we have to
break.**

### Native-fidelity bugs found and fixed this pass

Instrumenting what the VM asks the environment (`PF2_PAIRIP_VM=1
PF2_JNILOG=1`) exposed three of our own bugs, all of them places where the fake
JNI answered in a way ART never would:

1. `dladdr` filled in its `Dl_info` and then returned 0. Zero means failure, so
   a caller that checks the return sees a failure carrying a populated struct.
   Now it locates the address in the real module list and returns 1, or returns
   0 and leaves the struct alone.
2. Constructors discarded their arguments unless the class was a boxed
   primitive, so `new java/io/File(path)` produced a File with no path and
   `length()` could only answer 0. Both the varargs and the `jvalue`-array forms
   now keep a single `String` argument -- the VM builds its `File` through
   `NewObjectA` and its `Intent` through the varargs form, so covering one form
   only is not enough.
3. An unimplemented method returned NULL even when its return type was the
   receiver's own class. Android's builders (`Intent.putExtra`, `setPackage`,
   `setFlags`, `setData`) return `this` so calls chain; returning NULL broke the
   chain and every later link operated on nothing. Unimplemented methods that
   return their own class now return the receiver.

### What the startup program actually does

With those fixed, the trace reads clearly:

```
new java/io/File("/proc/self/maps")   -> length()  -> 0
new java/io/File("/proc/self/status") -> length()  -> 0
new android/content/Intent("com.google.android.gms.play.integrity.autoprotect.LOG_TELEMETRY")
  .setPackage(...).putExtra(...) x4
Context.sendBroadcast(intent)
```

It is Play Integrity autoprotect: an anti-debug probe of procfs followed by a
telemetry broadcast. The zero lengths are *correct* -- `stat()` on
`/proc/self/maps` reports size 0 on Android too -- so they are not the
divergence.

### Where it still breaks, and what the next instrument has to be

The crash is `libpairipcore+0x53bdc`, an FNV-1a-64 loop over the program's own
bytes whose trip count comes from a **signed** halfword operand (`ldrsh` at
`+0x53b84`). A negative count makes `subs`/`b.ne` run ~2^32 times, so it walks
off the end of the heap; that is the SIGSEGV. 48.8% of the payload's signed
halfwords are negative, so a negative there is what reading a random position
looks like: the interpreter is off the instruction grid, not fed a malformed
program.

The state at the fault (`x19=0xd29352ad`, `x20=0x98eab866`, `x24=0xe137e2c2`,
`x26=0x15864cae`, `x27=0x38b23`) is **identical across every run and unchanged
by the JNI fixes**, so the divergence is deterministic and is not caused by an
answer we give through Java.

That makes the next step a specific tool rather than more guessing: decode the
program offline with the verified model (mask `~(w ^ len)`, index `mod len`,
initial PC 45) and walk it until the offset chain leaves the grid, which
localises the handler we are mis-modelling. It runs on the PC with no device in
the loop. `analysis/pf2_iap_container.py` already has the entry state; the
missing piece is the opcode table.

## Third pass, 2026-07-29: a VM tracer, and the licence program is a red herring

Rather than reimplement the VM offline -- which cannot find a handler we model
wrongly, because it would model it wrongly too -- the port now traces the real
interpreter from inside itself. `PF2_VMTRACE=1` patches BRK #0 over two
instructions in libpairipcore, and the SIGTRAP handler records into a ring
buffer and emulates what it displaced. The fault reporter dumps the tail.

  - `+0x4d340` (`mov w26, w8`) is the dispatcher head. Nine branches target it,
    so it is hit once per flattening step.
  - `+0x53ac0` (`ldr x1, [x21]`) is the entry of the opcode that crashes.

### Where the VM's pc actually lives

Not at `[sp+120]`: that is written once during init and only read, and tracing
it shows a constant **45** for the whole run -- worthless as a trace, but a
clean independent confirmation of the offline container model, which predicted
exactly 45.

The running pc is the **third field of the descriptor `executeVM` builds**, the
one it initialises to zero. `+0x53acc` does `ldp w27, w10, [x21, #8]`, i.e. the
length from +8 and the pc from +12, and the handlers write it back to
`[x21, #12]`. It doubles as a read cursor: a handler advances it as it consumes
operands, which is why the dispatcher-head trace shows it moving in +2 steps
mid-instruction. Also note that `x27` is the descriptor pointer at the top of
vm_entry but is **repurposed as the length** inside this handler, which is why
the crash dump shows `x27 = 0x38b23`.

There is no `ret` between `+0x4d164` and `+0x53b40`: vm_entry is a single
flattened function of ~26 KB, which is also why `.eh_frame` covers
`0x28e50..0x7873c` with one FDE.

### The self-hash opcode, decoded and validated against hardware

34 bytes wide, operands at pc+4, +8, +0xc, +0x10, +0x14, +0x1c, +0x1e, +0x22.
It reads four masked words from a pool at `[sp+144]`, unmasks them with the
length, uses each `mod len` to gather four more words out of the program, and
combines them (eor/and/bic) into `w19`, the operand mask -- so the mask is
data-dependent but constant here, because the pool pointer is written once.
That is why `w19 = 0xd29352ad` in every run. The opcode then computes
FNV-1a-64 over `w28` bytes starting at `(w19 ^ word@pc+0x1e) mod len`, where
**`w28` is a raw *signed* halfword at pc+0x1c** and the only guard before the
loop is `cbz`: a negative count therefore runs ~2^32 times and walks off the
heap.

Evaluating that offline at the traced pc predicted `count = -15583` and start
offset `78063`; the crash registers show `x10 - x8 = 0x130EF = 78063`. Model and
hardware agree exactly, so the container model is now verified rather than
assumed.

### Why that crash is probably not ours

The trace is structurally healthy right up to it: 429 flattening steps resolve
to ~66 VM instructions, each with the same shape (cursor at X for four steps,
X+2 for two or three), the initial state is `0xdf7acf88` at pc 0 as expected,
and no pc repeats or leaves the program. The self-hash opcode then runs
**exactly once**, immediately after the program has broadcast
`com.google.android.gms.play.integrity.autoprotect.LOG_TELEMETRY`, and goes away
with a negative count that the surrounding code makes no attempt to guard.

A clean 66-instruction run that ends in a trivially-producible runaway directly
after a tamper-telemetry broadcast reads as a **deliberate bail**, not as our
divergence.

### The program that matters fails differently

`PF2_PAIRIP_SKIP_STARTUP=1` skips the licence program, which separates the two
questions the platform's ordering welds together. With it set the flow advances,
and libil2cpp's `DT_INIT` asks for a *third* program:

```
ExecuteProgram("fZ9RXXeCrCFThFOY", 1 args) -> the VM      # 205286 bytes
```

That is the 205286-byte figure earlier notes attributed to `qFzz`; the
GOT-and-.text program of libil2cpp is `fZ9RXXeCrCFThFOY`, and libunity's is
`qFzzC3MvCdTy6brw`.

It runs 431 flattening steps and faults at `+0x671d8`, which is a different bug
shape entirely -- an out-of-bounds index, not a runaway:

```
671c8: ldr x8,  [x8, #112]            ; table base, out of the thread-local arena
671cc: add x21, x8, w22, uxth #4      ; x21 = base + (w22 & 0xffff) * 16
671d8: ldr q0,  [x21]                 ; faults; w22 = 0xfffd2695 -> index 9877
```

So the VM indexes element 9877 of a 16-element-strided table (offset 158032) and
the table is not that large. The index is an unmasked word truncated to 16 bits
rather than reduced `mod len`, and the table comes from the growable
thread-local arena at `+0x28728` (`pthread_getspecific` + `realloc`).

The arena is **not** a shim problem: `pthread_key_t` is an unsigned int and
`pthread_getspecific` returns void* in both bionic and glibc on arm64, and the
port passes all three straight through to glibc.

So the next question is narrow: is index 9877 legitimate and the arena grown too
small, or is the index itself diverged? The arena's required size comes from
`[descriptor + 16]`, read with `ldar` from `.data` at `0x83cd8` -- so tracing
that descriptor's value, and the arena's actual capacity at `[tls + 8]`, decides
it. Both are readable with the same BRK instrument.

Still no render, no audio, no input, no gameplay.

---

# 2026-07-29, 4th pass: the .text wall is down, and the real wall is in .data

The VM does not have to be reversed to get past `.text`. Two things it does at
`DT_INIT` -- decrypt a `.text` window and fill the hidden PLT slots -- were both
obtained from post-`DT_INIT` memory of a real Android run, and both were then
validated offline against the shipped file.

## The `.text` windows are restored, and the proof is not statistical

`analysis/pf2_decrypt_text.py` accepts a dump only when all three hold:

1. every dump byte *outside* the differing window equals the file. That is what
   fixes the alignment and proves the dump belongs to this build -- it is not an
   assumption, it is a check that fails loudly;
2. the differing run is contiguous and starts at the segment's `.text`;
3. the AArch64 encoding test flips from ciphertext to clean. Bits 28:25 in
   0b0000..0b0011 are reserved/SVE/SME and an armv8-a compiler never emits them:
   real code scores 0.00, ciphertext ~0.25.

| lib | window (vaddr) | size | encoding before -> after |
|---|---|---|---|
| libunity | `0x397710..0x3a3ec0` | 0xc7b0 | 0.2567 -> 0.0000 |
| libil2cpp | `0x118bf7c..0x119872c` | 0xc7b0 | 0.2556 -> 0.0000 |

A sliding scan of both patched images now reports **0 suspicious windows** over
3497 and 1772 windows of `.text`. The only pages that still score high are the
PairIP-appended `DT_INIT` pages, which are 98% zeros -- an artefact of the
zero-fill, not ciphertext.

## Every import is named from the resolved GOT, and it agrees with the offline table

`analysis/pf2_got_names.py` resolves each dumped `.got.plt` value against the
mapped library that owns it (`/proc/pid/maps` plus the platform libraries),
skipping ARM mapping symbols (`$x`, `$d`) and preferring global FUNC symbols:

* libunity 334/336, libil2cpp 567/569. The 3 leftovers are ART's `libsigchain`
  interposers.
* libil2cpp's unnamed slots point mostly at **libil2cpp and libunity**, not at
  libc. The earlier assumption that it only needs libc/libm/libdl/liblog is why
  the offline route stalled at 6 of 200.
* Against the 252-entry offline table: **234 exact matches, 0 real conflicts**.
  All 16 apparent disagreements are bionic aliases of one address --
  `__memmove_aarch64_simd` (which is both `memcpy` *and* `memmove`, confirming
  the call-site ranking), `__strlen_aarch64`, `fopen64`, `siglongjmp`,
  `scalbnf`, `mbsrtowcs_l`. Two independent methods, one answer.

The shipped tables now cover **336/336 and 569/569**, and a run reports
`200 restored, 135 already relocated, 0 missing` for libunity and
`200 restored, 369 already relocated, 0 missing` for libil2cpp.

## What the port does now

Unity boots for real: `JNI_OnLoad` for all three objects, `initJni`,
`nativeRecreateGfxState`, `nativeSendSurfaceChangedEvent`, the `nativeRender`
loop, and then IL2CPP itself -- it resolves its `il2cpp_*` exports, opens
`global-metadata.dat` (magic `0xFAB11BAF`, version 31) and the `Managed`
resources, and prints its own banner:

```
MemoryManager: Using 'Dynamic Heap' Allocator.
SystemInfo CPU = ARM64 FP ASIMD AES, Cores = 4, Memory = 916mb
Built from '2022.3/respin/2022.3.62f2-...' Scripting Backend 'il2cpp'
```

Unity's own crash handler also works, which means every fault from here on
arrives with a full symbolised stack instead of bare registers.

## 🚨 The new wall: PairIP encrypts a window of WRITABLE DATA too

The old note that the encrypted surface is "the first 0xC800 of `.text`" is
**incomplete**. PairIP spends the same budget a second time, on data:

| lib | data window | size | slots |
|---|---|---|---|
| libil2cpp | `0x2f0e7a0..0x2f1af50` | 0xc7b0 | 6390 |
| libunity | `0x11cdbf8..0x11d2408` | 0x4810 | 2306 |

In libil2cpp that window falls inside the **IL2CPP metadata-usage table**: an
array of `(kind << 29) | (index << 1) | 1` tokens.
`il2cpp_codegen_initialize_runtime_metadata` (real entry `0x139cee4`, reached
through the thunk at `0x1363a38`) atomically loads a token, returns immediately
if bit 0 is clear, else takes `kind = bits 31:29`, `index = bits 28:1` and
dispatches through a byte jump table at `0xabb012`.

The first fault was exactly this: `il2cpp_init` -> ... -> a `.cctor`
(`mscorlib.dll` method 6993) whose token read `0x20b9888ffbd0d9e7`, giving
`index = 0xde86cf3` -- 233 million -- so the resolver indexed a metadata table
1.8 GB past its base. The file holds that same garbage, and its neighbours are
high-entropy too.

Acceptance test for this window, for any future attempt: the clear part of the
table is **55% zeros and 97.6% valid tokens**; the encrypted part is 0.4% zeros
and **0%** valid tokens.

Instrument (`PF2_ZERO_ENC_DATA=1`, off by default, measurement only): zeroing
the window turns "wild index" into "this usage stays NULL", because a token with
bit 0 clear makes the resolver return at once. The boot then dies as a clean
NULL dereference at the same `.cctor` -- so at least one hidden slot is needed
during `il2cpp_init`. All **6390 slots are referenced by relocated pointers**
(6419 references), so none of them is padding.

## The cipher is NOT ECB -- corrected with plaintext in hand

With 3195 plaintext/ciphertext block pairs per library, the earlier
characterisation collapses:

* identical plaintext blocks encrypt to **different** ciphertext (290 such cases
  in libunity, 333 in libil2cpp), and the 6 plaintext blocks common to both
  libraries encrypt differently in each. **Not ECB.** The 36 repeated ciphertext
  blocks that suggested ECB were coincidence.
* C -> P is unambiguous (0 conflicts), which fits a position-dependent
  construction.
* `C ^ P` is not a shared keystream: libunity's and libil2cpp's differ at
  99.6% of bytes, and neither is periodic (best autocorrelation ~10% at lag 392
  and 256 respectively -- structure, but not a period).
* `C ^ P` is not splitmix64, not a 64-bit or 32-bit LCG, and has no short period.

## Routes ruled out this pass (do not redo them)

* **Keystream reuse.** XOR-ing the `.text` keystream over the `.data` window at
  every alignment in +/-0x900 never rises above 0% valid tokens.
* **Plaintext stored in the VM programs.** A sliding constant-mask search
  (period 1/2/4/8/16, which covers any `eon(word, len)`-style masking without
  guessing `len`) finds the known 51120-byte plaintext in none of the six
  ~205 KB asset programs.
* **Cross-ABI fill.** `global-metadata.dat` is byte-identical in the arm64 and
  armv7 XAPKs, so the token *values* are shared -- but the tables are emitted in
  different order per ABI (armv7's clear part starts at `kind=6 idx=75027`,
  arm64's at `kind=1 idx=20095`), and armv7's readable set of 28603 tokens is
  almost a *subset* of arm64's 34991: only 3 tokens are unique to armv7. The
  armv7 build hides more, so it adds nothing.

## The offline route that does exist: methodPointers reverse lookup

The token for a given slot is recoverable from the code that consumes it,
without the VM. Proven this pass:

* the token table is **sorted**: after the window it runs `kind=1` with strictly
  increasing indices (20095, 20102, 20104, 20106, ...), so the hidden entries are
  an ascending run bounded above by the first clear one;
* the consuming site names itself. `Il2CppCodeGenModule` structs are readable --
  80 modules, `moduleName` at +0x00, `methodPointerCount` at +0x08,
  `methodPointers` at +0x10 -- and their pointer arrays lie *outside* the
  encrypted window. Reversing them maps **61753 function pointers to
  (module, method index)**. The failing `.cctor` is `mscorlib.dll` 6993 and the
  constructor it calls is `mscorlib.dll` 3355;
* from a method index, `global-metadata.dat` gives the declaring type, which is
  the index a `kind=1`/`kind=2` token needs. The two slots at the first fault
  (`0x2f16140` and `0x2f18d20`, reached through `.got` pointers `0x2e77468` and
  `0x2e77be0`) are both classes: one is constructed with `object_new`, the other
  receives the object in a static field.

So the remaining work is a metadata-driven reconstructor, not cryptanalysis.
The cheap alternative is one more capture of those two windows from a real
Android run, exactly as was done for `.text`.

Still no render, no audio, no input, no gameplay.

## 2026-07-30: the data window closed the PairIP chapter; now it is an ordinary port

One capture with the extended helper (`analysis/pf2_memwatch.c`, which now also
waits until the window reads as a metadata-usage table before freezing the
process) produced the missing plaintext.

**libil2cpp's data window is in, and it validated cleanly**: `0x2f0e7a0..0x2f1af50`,
token rate **0.0000 -> 1.0000**, and every byte of the dump outside the window
equal to the file, which is the alignment proof. With it, the boot no longer
dies inside `il2cpp_init`; the four overlays apply and the PLT still reports
`0 missing` on both libraries.

⚠️ **libunity's data window is deliberately NOT applied.** A capture taken at the
same moment holds 2304 of its 2306 words as live pointers into the donor process:
by then Unity had overwritten the decrypted static content. Splicing them in sent
the player into a modal wait and hung frame 1. Whatever that window holds
statically, the game gets to its first scene without it; a valid copy would need
a capture taken between libunity's own `DT_INIT` and UnityPlayer init.

### What the port does now

Unity initialises fully: 29 threads (job workers, GC finalizer,
`Loading.AsyncRead`), a thread inside `_mali_osk_notification_queue_receive` --
so the Mali driver is live -- and the player reaches

```
Company Name: Koality Game
Product Name: Prizefighters 2
```

### The instrument that made the rest tractable: PF2_WATCHDOG

`PF2_WATCHDOG=<seconds>` starts a thread that, if the frame counter has not
moved, sends `SIGSEGV` **to the render thread specifically** with `tgkill`. Unity
installs its own crash handler, so the result is a fully symbolised backtrace of
the stuck thread. Guessing from `/proc/<pid>/syscall` and hand-walking the stack
found the frame; this prints it in one run.

### Two real port bugs it found

1. **`android.os.Build.MANUFACTURER` was absent.** Unity does
   `strcasecmp(MANUFACTURER, "Amazon")` and a NULL field faults inside
   `strcasecmp`. jni.c now answers the whole `Build`/`Build$VERSION` set, with
   values that match no vendor Unity has a workaround for, and `SDK_INT` 33.

2. **A modal AlertDialog with nobody to click it.** The watchdog stack pointed at
   a wait helper whose caller references `gles-api-check`, `Warning`,
   `'Your device does not match the hardware requirements of this application.'`,
   `Continue`, `Abort`. The JNI trace shows the exact sequence: Unity calls
   `Activity.getPreferences(0).getBoolean("gles-api-check", false)`, and on false
   it builds an AlertDialog (title, message, two buttons, a do-not-show-again
   CheckBox), hands it to `Activity.runOnUiThread(Runnable)` and blocks the
   render thread on a condition variable. There is no Java UI thread here, so
   frame 1 never returns. jni.c now returns a SharedPreferences object and
   answers `true` for that key -- this port runs GLES2 on Mali-450 on purpose,
   so it is a warning we have already read.
   (Note: `androidUseSwappy=0` and `gles-api-check=0` in `boot.config` do **not**
   help. The key is a preference, not a boot.config entry. Both were tried and
   the device's `boot.config` was restored.)

### Where it stands, and the next step

With the dialog suppressed the hang is gone and the failure is now a jump to
**pc=0**: `libunity+0xa33608` does `blr [x20+1848]` with `w0 = 0x1f03`
(`GL_EXTENSIONS`), i.e. **`glGetString` in Unity's own GL function table is
NULL**. Unity does not import GL through the PLT -- it builds that table at
runtime -- and our `dlsym` chain ended at the host process, which links only
`libEGL.so.1`. On this image every `libGLES*` name is a symlink to `libMali.so`,
so `pf2_gl_sym()` (egl.c) now resolves any `gl*` name from that blob and
`pf2_egl_sym` falls back to it, which also covers `eglGetProcAddress` (the Mali
driver answers extensions only).

**That fix is built and committed but not yet run on the device.** Verifying it
is the next step, and after it the expected front is the one the dialog was
warning about: an ES3-only player on a GLES2 driver, i.e. the shader layer.

Still no render, no audio, no input, no gameplay.
