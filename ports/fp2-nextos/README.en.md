# Freedom Planet 2 — NextOS reference

[Português](README.md)

[Catalog](../../catalog/README.en.md) · [AI guide](../../docs/en/AI-PORTING.md) · [Selected sources](upstream/)

Origin: [fp2-nextos](https://github.com/NextOs-Ports/fp2-nextos), commit `f8caab9d946c39041fa5cc25cb52621590f82adc`. Platform: Android.

## How to use this reference

This directory contains selected public sources from a port created/integrated by NextOS. It is not a complete game or installable package. No commercial data accompanies this selection; a new port needs the owner's compatible copy.

Read [SOURCE-MAP.json](SOURCE-MAP.json) before copying code: 227 files are included and 2 additional privacy/content omissions are recorded. Upstream recipes may require files outside this selection; cloning this directory does not guarantee a standalone build.

Compare engine/build, ABI, signatures, data, renderer, audio and input consumer. Write the new adapter in a different directory. Record reused source/hash/license and test its contract in the destination.

## Status and limitations

The snapshot is the observed public commit, not a new physical certification. Read the exact version's status before claiming gameplay, installation or device support. The reference's own pins remain preserved: inclusion beside V5 neither migrates V3/V4/V6 nor rebuilds approved binaries.

## License and authorship

GitHub detects MIT but NOTICE.md explicitly says loader/hooks/GLES are GPL-3.0-only, NXExtract/MIT plus embedded GPL module, splash MIT. Do not treat entire port as MIT.

Integration/collection author: **NextOS** — [official GitHub](https://github.com/NextOs-Ports). Preserve third-party license notices and credits. Game data, original code and trademarks remain with their respective rights holders. [Collection terms](../../LICENSING.en.md).

Files inside `upstream/` retain their original bytes and languages. This card and the outer guides provide bilingual navigation without modifying the audited reference.
