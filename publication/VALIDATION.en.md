# Bilingual edition validation

[Português](VALIDATION.md)

Date: 2026-09-10. Scope: documentation, catalog and selected Unity import.

- Original collection: 7,715 source/helper hashes, 41 repositories/45 titles and eight pinned V5 helper ELFs.
- Expanded catalog: 47 NextOS titles, including two community cards without source snapshots.
- Shim example: C99 host build and CTest 1/1 PASS from the previous preparation; source and binary preserved without a documentation rebuild.
- Current integrity: 7,725 hashes checked, including ten generic Unity files/fixtures; no V5 runtime or upstream source was changed.
- Documentation: 86 PT/EN pairs, 1,344 local links and 32 valid shell blocks; executable examples match in both languages. Structural checks do not replace semantic translation review.
- Unity tools: 15 texture-planner tests and 20 touch-checker tests passed; synthetic CLI examples exited 0 without claiming physical evidence.
- nxgenerator, nxrelease and NXExtract recipe-check CLI interfaces checked with `--help`; no generation, installation or release batch was started.
- Recognized privacy/package checks and `git diff --check`: PASS. All 15 Unity cases bind to selected public-repository commits/files; visibility was queried during this review.
- Not run in this revision: ARM/NDK/Godot build, commercial game, on-device extraction, physical testing, port rebuilds or the complete historical V5 suite.

Source integrity, documentation consistency and synthetic tests do not certify gameplay or every file's license. [Pending work](REVIEW.en.md).

The subsequent first-port simulation ran the host example from a clean export, generated a skeleton and checked recipes/pins. Its results and limits are in the [separate report](ONBOARDING-SIMULATION.en.md); the counts above describe the earlier editorial review.

The later public SDK and exercise implementation has separate results in [ONBOARDING-UPDATE.en.md](ONBOARDING-UPDATE.en.md), including ARM builds, an Android guest in QEMU, clean extraction, shaders and Godot logic. The exclusions above apply only to this earlier editorial review.
