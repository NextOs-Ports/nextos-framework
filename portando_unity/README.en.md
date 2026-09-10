# Porting Unity — selected NextOS edition

[Português](README.md)

This bilingual edition adapts the `portando_unity` study for **15 cases with public repositories**, plus two generic tools and synthetic fixtures. Two additional ports have explicitly authorized community-distribution cards. Sources and recipes for other local games were not included.

## Reading order

1. [Build triage](en/TRIAGE.md): identity, runtime, contracts and reference selection.
2. [Graphics and textures](en/GRAPHICS-AND-TEXTURES.md): GLES2, ETC1/dual, alpha, scaling and memory.
3. [Input and audio](en/INPUT-AND-AUDIO.md): actual consumers, short presses and lifecycle.
4. [Boundary diagnostics](en/DIAGNOSTICS.md): symptom, measurement, cause and countercheck.
5. [Freedom Planet 2: Vulkan to GLES2](en/FP2-VULKAN-GLES2.md): in-depth study with public sources.

## Cases with public sources

| NextOS port | Unity version in selected notes | Reading |
| --- | --- | --- |
| Suzy Cube | 2017.4.40f1 | [Case](cases/suzycube.en.md) |
| Freedom Planet 2 | 2018.4.36f1 | [Case](cases/fp2.en.md) |
| Dead Trigger | 2019.4 (patch a confirmar / patch to confirm) | [Case](cases/deadtrigger.en.md) |
| Terraria | 2021.3.56f2 | [Case](cases/terraria.en.md) |
| Horizon Chase | 2022.3.33f1 | [Case](cases/horizonchase.en.md) |
| Bomb Chicken | 2022.3.39f1 | [Case](cases/bombchicken.en.md) |
| Prizefighters 2 | 2022.3.62f2 | [Case](cases/pf2.en.md) |
| Oceanhorn: Chronos Dungeon | 2022.3.61f1 | [Case](cases/oceanhorn.en.md) |
| Hitman GO | 2022.3.67f2 | [Case](cases/hitmango.en.md) |
| Huntdown | 2022.3.47f1 / 6000.2.6f2 | [Case](cases/huntdown.en.md) |
| Skateboard Party 3 | 2022.3.45f1 | [Case](cases/skate3.en.md) |
| Sally Face | 2022.3.62f3 | [Case](cases/sallyface.en.md) |
| Merchant of the Skies | 6000.4.2f1 | [Case](cases/merchantskies.en.md) |
| Nameless Cat | 6000.3.11f1 | [Case](cases/namelesscat.en.md) |
| Party Hard GO | 6000.3.10f1 | [Case](cases/partyhard.en.md) |

Each card points to the catalog's selected public commit. Historical note results do not automatically certify that HEAD. Huntdown keeps its 2022 and 6000.x profiles separate; title/menu evidence for the second does not become approved gameplay.

## Cases authorized through community distribution

- [Stranger Things 3: The Game](../catalog/community/strangerthings3.en.md): Unity 6000.2.6f2, technical card without a source snapshot.
- [AVGN I & II Deluxe](../catalog/community/avgn12deluxe.en.md): Unity 2019.4.16f1 and BYO-data V6 reference; does not change V5.

No other titles were imported by analogy or merely because they exist locally. All are NextOS ports; third-party components retain their own credits and licenses.

## Tools and limitations

[ETC1 planning](diagnostico/texturas/README.en.md) and [short-press analysis](diagnostico/toque/README.en.md) are offline tools. They do not connect to devices, run games or produce physical approval. Code and fixtures preserve hashes; guides have complete PT/EN versions.

[SOURCE-MAP.json](SOURCE-MAP.json) records selected note origins/hashes, public-code correspondence and copied generic files. Cross-cutting guides were adapted to remove excluded-game references. Galleries, data, logs and private evidence are not included.

To start a port, read [the AI guide](../docs/en/AI-PORTING.md) and [ARM build guide](../docs/en/BUILD-ARM.md). Preserve V5, firmware SDL, native flow and NXExtract/NXSplash interfaces. Shared behavior changes belong to V6, not this documentation edition.
