# Contributing to NextOS

[Português](CONTRIBUTING.md)

Read [AGENTS.md](AGENTS.md), choose the contract to improve and work in your own directory/branch. The repository remains private until NextOS approves publication.

## Bilingual documentation

Update Portuguese and English in the same change, including links, examples, commands, limitations and numbers. A short English overview does not replace a full guide translation. Register the pair in [publication/languages.json](publication/languages.json).

Keep API identifiers, paths, enums and schemas identical between languages. Translate explanations and prompts. Use existing relative links and declare command placeholders before showing them. Distinguish tested commands, checked syntax and recipes requiring unavailable SDKs/inputs.

Do not translate over pinned historical trees. Add external explanations; preserve hashes, license files and upstream notices. Normative legal texts remain original; bilingual licensing explanations do not change their terms.

## Catalog and Unity

The catalog contains ports created/integrated by NextOS. For a public repository, record origin, commit and source selection with hashes. For community distribution without public source, say so explicitly and do not invent URLs or snapshots. Every title needs inclusion authorization and a defined evidence scope.

`portando_unity` only accepts admitted cases and reviewed generic tools. Do not copy local indexes, galleries or guides containing other games. Partial repairs retain their limitations; they do not inherit gameplay from another build of the same title.

## Code and data

V5 and reference sources remain preserved. Shared behavior changes follow a separate V6 line; a new adapter does not modify neighboring ports. Keep proprietary data in a private area outside commits, issues and CI artifacts. Do not reproduce personal attribution, device addresses, credentials or APK download origins.

Use targeted tests for new code. For documentation, check links, command parity, language and reference integrity without rebuilding approved games. Run:

```sh
python3 publication/verify.py
python3 publication/verify-docs.py
git diff --check
```

## Commits and review

Collection credit: **NextOS**, with required third-party notices. Never add AI signatures/coauthorship to commits. Review files and messages before pushing. A PR/commit should explain the result, motivation, verification and material limitations.

Publishing a game ZIP requires its own [delivery gates](docs/en/TESTING.md). Editing a guide does not authorize visibility changes, pin migrations or a game release.
