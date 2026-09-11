# Testing, evidence and delivery

[Português](../pt-BR/TESTES-E-ENTREGA.md)

A useful result states which bytes were tested, with which data and in which environment. This guide distinguishes checks needed while developing a reference from tests that support a game release.

## During development

Run small checks connected to the change: a shim contract, recipe parser, structure conversion, input sequence or synthetic data preparation. Do not run the full framework suite after every adjustment. Do not rebuild an approved executable just to translate its README.

Record failures as testable hypotheses. A countercheck should show why the repair does not apply elsewhere: for example, preserve a valid sampler while fixing one specific texture format.

## Minimum per-port matrix

| Stage | Establishes | Does not establish |
| --- | --- | --- |
| Source/hash verification | Selection origin and integrity | Complete licensing or gameplay |
| Host test | Exercised contracts on the computer | Complete Android ABI or device GPU |
| ARM build and ELF audit | Architecture and declared dependencies | Physical execution |
| CPU emulation | Paths executed in that environment | Mali driver, physical audio or input |
| First real frame | Image at that point with that binary | Progress, saving or every scene |
| Gameplay test | Explicitly exercised actions/scenes | An entire campaign without recorded traversal |
| Clean NXExtract install | Complete input preparation with that ZIP | Another APK, recipe or package |

## Physical evidence

Use only the device authorized for the task. Before launch, confirm no other instance of the same game remains, including an old process whose executable was replaced. Do not put port files in a directory reserved for firmware updates.

Measure the actual context/drawable and pixels immediately before present where instrumentable. Live audio, a PID, context creation or `exit 0` does not prove graphics. Conclusive black output or a dead context invalidates the test; preserve diagnostics and terminate only the exact instance.

Exercise boot, menus, gameplay, transitions, pause, audio, input/contexts, hotplug, save/reload and exit. Report scenes and duration actually observed. Owner feedback complements test data and must stay bound to the correct artifact.

## Freeze before packaging

Complete source, recipe, bilingual documentation, licensing, pins, build and audits. Preserve the physically approved executable hash in a record external to the port; relinking or rebuilding requires new evidence. Do not rename an old ZIP as a new version.

The port's canonical package builder should coordinate one final batch: validate manifests/sources, prepare stage and verify stage **before** creating the single candidate ZIP for that commit. Do not execute those full boundaries separately and then repeat them inside packaging. On failure, collect errors, fix them together and freeze a new commit before the next attempt.

## Release contents

Check the complete framework, graphical NXExtract and real recipe, five-second NXSplash, generated launcher, truthful manifest, PT/EN `<port-id>/INSTALLATION.md`, credits/licenses and required corresponding source. Audit every Linux ELF for GLIBC ≤ 2.30. Use system SDL by default and reject accidental redirection to a private SDL.

Audit executable scripts for dependency on the external `stat` command; reading `/proc/<pid>/stat` is different. Perform pre-runtime launcher-failure checks before release. No APK, asset, original library or prepared game data belongs in the ZIP.

## Accept the exact candidate

After creating the candidate, test [clean installation through NXExtract](NXEXTRACT.md) using complete input and preserve the ZIP without regenerating its bytes. Bind receipts, recipe and output hashes to the same ZIP and executable. Adopting old data is insufficient. Record tested and untested devices/firmwares.

The current collection has source and host-example checks; it has not undergone a new 44-game test campaign. See [publication validation](../../publication/VALIDATION.en.md). Making this collection public still requires NextOS approval.
