# Review before publication

[Português](REVIEW.md)

The maintainer must explicitly approve changing visibility. This preparation remains private.

## Included in this edition

Preserved V5; sources from 41 repositories/45 titles; two additional community cards; complete PT/EN guides for AI, ARM, shims, NXExtract, Mono Android, Godot, Cocos2d-x and testing; selected Unity edition with 15 cases and two generic tools; FP2 Vulkan→GLES2 study.

## Publication work still open

- Review both-language guides editorially and test cross recipes independently with a pinned public SDK/sysroot.
- Implement complete Android-loader, graphics/audio/input and NXExtract demonstrations. The existing shim example proves only its small contract; the documented NDK exercise was not built in this revision.
- Review per-file licenses and omissions, especially Goblin Sword, PartyBoard, Pikmin, Forager and FP2; decide new-text terms without removing existing GPL/MIT rights.
- Recover exact source/license before importing the two community ports' code; no public download URL is verified in this collection.
- Bind historical references to physically approved versions when used as final proof; an observed public HEAD is insufficient.
- Measure shim coverage by contract/engine; do not promise universal compatibility.
- Prepare independent public CI if desired. Twelve private-dependent V5 tests were excluded; the historical suite requires omitted resources.

Reviewing source/documentation does not require rebuilding all 45 ports. New executables, pins or support claims need corresponding specific validation. This edition's checks are in [VALIDATION.en.md](VALIDATION.en.md).
