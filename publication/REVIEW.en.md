# Review before publication

[Português](REVIEW.md)

iOS ports, sources, studies and examples were excluded from the current selection. Cross-platform declarations in generic dependencies such as SDL remain in preserved sources. Removal used a normal commit: earlier private history still contains the old selection. Before becoming public, review which history will also be published.

The maintainer must explicitly approve changing visibility. This preparation remains private.

The [first-port update](ONBOARDING-UPDATE.en.md) records implementation of gaps identified by the [earlier simulation](ONBOARDING-SIMULATION.en.md), with explicit checks and limits.

The [V5 tooling review dated 2026-09-12](V5-REVIEW-20260912.en.md) adds exact frozen-tree checks, inventory/recovery fixes and automatic collection CI, with 48 passing host tests. It preserves the V5 runtime and does not incorporate V6. The full SDK workflow remains a separate manual check.

## Included in this edition

Preserved V5; sources from 40 repositories/44 titles; two additional community cards; PT/EN guides; selected Unity edition with 15 cases and generic tools; FP2 Vulkan→GLES2 study. Now includes a public SDK, collection pins, integrated nxloader/NXExtract minigame, executable inventory, searchable profiles, engine exercises and shader laboratory.

## Publication work still open

- Review both-language guides and observe the manual CI workflow's first independent run. The public SDK was built and the example's CPU/extraction tests passed locally.
- Validate the integrated example on authorized hardware: NXExtract UI, NXSplash, pixels, sound, controls, saving and exit. Its generated contracts remain nonrelease. Unity editor builds, Cocos integration and the separate NDK exercise remain pending.
- Recover the exact origin of FP2's historical SPIRV-Cross pin before claiming an identical rebuild. The shader laboratory uses another explicit public pin; it does not replace the approved recipe.
- Review per-file licenses and omissions, especially PartyBoard, Pikmin, Forager and FP2; decide new-text terms without removing existing GPL/MIT rights.
- Recover exact source/license before importing the two community ports' code; no public download URL is verified in this collection.
- Bind historical references to physically approved versions when used as final proof; an observed public HEAD is insufficient.
- Measure shim coverage by contract/engine; do not promise universal compatibility.
- Twelve private-dependent V5 tests were excluded; the new teaching CI does not restore or certify that historical suite.

Reviewing source/documentation does not require rebuilding all 44 ports. New executables, pins or support claims need corresponding specific validation. Read the [update checks](ONBOARDING-UPDATE.en.md) and [earlier editorial validation](VALIDATION.en.md).
