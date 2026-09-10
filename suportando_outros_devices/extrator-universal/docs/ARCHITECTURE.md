# NXExtract architecture

NXExtract separates policy, extraction and presentation so a new game changes
data, not extractor code.

## Components

| Component | Responsibility |
|---|---|
| `nxextract.py` | discovery, planning, validation, staging and transactions |
| `extractor.json` | per-port paths, ABIs, validators, commit roots and hooks |
| `nxextract-ui` | mandatory SDL2/direct-framebuffer graphical progress renderer in packaged ports |
| hook | optional game-specific bake that writes only to the stage |
| `nxextract-runtime-env.sh` | process-scoped firmware-first native library boundary |
| `run-extractor.sh` | generic foreground launcher |

The Python core uses only the standard library and supports Python 3.7 or
newer. The UI loads the firmware’s SDL2 dynamically. Its release manifest
pins separate AArch64, ARMv7, x86_64 and i386 ELFs, all requiring at most
GLIBC 2.17; a port vendors only the artifact matching its declared ABI.

## Pipeline

```text
discover by content
  → classify APK / bundle / archive / loose file
  → parse Android package and split identity
  → evaluate recipe for each ABI
  → reject zero or multiple different payload plans
  → preflight only bytes still missing from the stage
  → copy with partial files, CRC and fsync
  → run resumable hooks in the stage (transactional hooks prepare in a
    shadow workspace, seal a journal and publish by atomic rename)
  → fully validate the staged result
  → publish commit roots with write-ahead journal + durable backup/rename
  → fully validate live payload + publish sealed marker
  → best-effort cleanup of backup/stage/cache/journal
  → atomically publish one sanitized terminal result
```

External filenames are metadata for logs only. They are never a game identity.

## Candidate identity

Direct split APKs are grouped only when their parsed
`AndroidManifest.xml` package matches. A bundle is expanded into a
same-filesystem cache, then its inner APKs pass through the same planner.

Each successful plan is fingerprinted from rule, destination, size and CRC.
Equivalent copies are harmless; two different fingerprints are an ambiguity
and stop the install.

## Transaction states

The live payload is untouched while extraction and baking run.

| Crash point | Next-run action |
|---|---|
| while copying | valid staged files resume; partial file is replaced |
| while running a hook | validated hook checkpoint resumes, otherwise hook reruns |
| while a transactional hook prepares | shadow and journal are discarded; hook reruns over pristine inputs |
| after a transactional hook seals its journal | publication rolls forward from the journal to the same fingerprint |
| before publication | stage remains; live payload is unchanged |
| after backing up a live root | journal restores the backup |
| after installing some roots | installed roots return to stage, backups return live |
| after marker publication | transaction cleanup completes; published data remains |

Journal format 2 records `backup-intent` and `install-intent` before their
respective rename, fsyncs both parent directories after the rename, and then
fsyncs the confirmed state. Commit roots may not overlap the private workspace,
marker, log or one another. The sealed marker—not a journal boolean—is the
publication boundary; a matching marker is fully revalidated during recovery.

Version 1.2.21 keeps the cheap path/type/size/mtime metadata seal as the normal
launch gate and adds a stable content seal over immutable paths, object kinds,
sizes and bytes. A metadata-only drift reads that content seal once and
atomically reseals the marker; a byte mismatch fails closed. A metadata-drifted
1.2.20 marker has no prior strong root, so it requires an externally expected
content seal after full declared validation. `--reuse-only`
does not invoke transaction recovery: if `transaction.json` exists, it aborts
before recovery, UI startup, source discovery or extraction. This gives binary
update tests a strictly narrower authority than a normal install.

Every staged/published object must be regular and privately linked. Nested
symlinks, hardlinks, special objects and Unicode/case-fold collisions fail
validation before publication.

## Progress protocol

The core atomically writes:

```text
STATE OVERALL 1000
MESSAGE
NXEXTRACT_V1 PHASE OVERALL PHASE_PROGRESS DONE_BYTES TOTAL_BYTES
DETAIL
```

Hooks report:

```text
NXEXTRACT_PROGRESS DONE TOTAL OPTIONAL DETAIL
```

## Terminal result and logs

Every completed install attempt atomically replaces `nxextract-result.json`
with the strict document defined by
[`terminal-result-schema-v1.json`](terminal-result-schema-v1.json). It records
the last phase, outcome, stable `NXE####` code, recipe, package ID, ABI,
validated item/byte totals, critical rule summaries and relative log paths.
The `container` object contains only a finite source kind and an identity
derived from the validated plan; external filenames, URLs and source origins
are never copied into it. A failed rename leaves the previous complete JSON
unchanged, never a partial document.
The filename is fixed so a launcher can consume it without interpreting the
trusted recipe or depending on `jq`; recipes cannot redirect it.

`nxextract.log` is compact by default. Milestones, the first miss of each
class, repetition summaries and the terminal cause remain visible there.
Per-file extraction records and hook stdout go to `nxextract-detail.log`.
`--verbose-log` or `NXEXTRACT_VERBOSE_LOG=1` mirrors detail back into the
compact stream for an explicit diagnostic run. Neither logging path changes
the progress protocol or presentation process.

The UI remains a separate process, but the packaged runner treats visible
readiness as a pre-extraction contract. It publishes a private proof only after
the first complete frame from an approved SDL or direct-framebuffer graphical
renderer is presented. A TTY/ASCII proof is not accepted. Missing or malformed
proof fails before source scanning or payload mutation. The bounded deadline is
40 seconds: a late approved renderer is accepted, while reaching the boundary
without an exact proof still fails closed. Developers invoking `nxextract.py`
directly may omit `--require-ui` for isolated headless automation.

The UI control plane never lives on any filesystem. FAT, exFAT and some FUSE
mounts can report every object as `0777` regardless of `chmod`, and a firmware
login session with `Linger=no` recycles `/run/user/<uid>` the moment the
session ends — which killed long extractions that kept their handshake under
`XDG_RUNTIME_DIR`. The engine now opens two pipes in the parent before the UI
spawn, validates each descriptor with `fstat` (FIFO type, effective owner,
private mode, exact dev/inode identity) and passes only inherited descriptors
to the UI as `fd:N` tokens in the historical argv slots. The readiness proof is
sealed by the UI closing its write end; the stop request is a byte plus EOF, so
an engine crash also ends the UI instead of orphaning it. Every later use
revalidates the descriptor identity, and a substituted descriptor fails closed.
Removing any directory mid-extraction — including the exported
`XDG_RUNTIME_DIR`, which stays untouched for Wayland — cannot invalidate a data
transaction that already validated. The diagnostic `ui.log` remains in the
persistent workspace but carries no readiness authority. Persistent
installation state remains in `.nxextract`, but carries no UI-readiness
authority and must not acquire a POSIX-mode requirement that FAT/exFAT/FUSE
cannot represent.

## Native runtime boundary

`run-extractor.sh` re-executes itself once through
`nxextract-runtime-env.sh`. The helper builds a child-only native search path
in this order:

1. architecture-specific firmware directories;
2. optional, explicitly supplied firmware/runtime directories;
3. safe inherited absolute directories.

Entries resolving inside `NXEXTRACT_GAME_DIR`, relative entries and duplicate
entries are discarded. The parent launcher environment is not changed, so the
game can establish its own compatibility-library scope later. The boundary
does not set or clear SDL backend variables by default; a valid firmware
selection and normal SDL autodetection both remain available. A launcher that
has already detected an invalid inherited override may set
`NXEXTRACT_SDL_AUTODETECT=1`. Only in that child scope, the helper unsets the
video, legacy-video and audio SDL driver variables without choosing a
replacement backend.

## Flexible APK-container boundary

The outer APK is not implicitly a game identity. Every `container` source
requires `input.packages`; whole-APK SHA-256, CRC32 (in any quantity), exact
size and filename filters are rejected at recipe load in `validate`,
`source_validate`, `output_validate` and `source.patterns` (V3,
NXA0001..NXA0006) -- identity of
the tested copy lives in the documentation-only `reference_build` block.
Compatibility is established by package, bounded size/magic, internal
payload/tree and/or transactional-hook checks; `patch_profiles` may key a
patch on an internal payload hash with a mandatory generic fallback.
NXExtract resolves core/optional/ABI-variant members over base+splits and
authenticates every patch-selector decision. That bounded result is part of
the plan fingerprint, install marker and reserved hook environment, preventing
a checkpoint produced for one profile/fallback from serving another. The
canonical rule is the shared module `framework/contracts/apkcompat/`,
embedded verbatim in the engine and consumed by NXGenerator and NXRelease.

## Display negotiation

The UI first honors a valid backend inherited from the launcher, then lets
SDL2 auto-select. `dummy`, `offscreen` and non-display backends are rejected.
If automatic selection is invisible, NXExtract enumerates only backends
advertised by that SDL build and tries those compatible with the current
session:

- Wayland only when its socket exists;
- X11 only when `DISPLAY` exists;
- KMSDRM only when a DRM card exists;
- fbdev/Mali-class drivers only when a framebuffer exists.

Any environment adjustment exists only inside the UI child process.

After the normal SDL attempt fails, NXExtract may reuse the NXSplash 0.1.2
portable-provider recovery: `libEGL.so` and `libGLESv2.so` are tried only when
the firmware/user did not explicitly set either provider. A failed portable
attempt is removed before the normal bounded retry sequence continues. This
does not select a backend by firmware/device identity.

If every plausible SDL window path genuinely fails but `/dev/fb0` is available,
the UI wraps the active framebuffer in an SDL software surface and calls the
same `draw_screen()` implementation. This preserves the approved pixels without
choosing an EGL/GLES provider or borrowing game-private libraries. The old
ASCII/TTY renderer is diagnostic-only and cannot satisfy public readiness.
