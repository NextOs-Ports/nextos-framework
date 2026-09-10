# Recipe reference

Every port ships one trusted JSON recipe. The recipe describes valid content;
it never names the user’s external APK file.

## Root fields

| Field | Required | Meaning |
|---|---:|---|
| `schema` | yes | recipe schema, currently `1` |
| `id` | yes | stable safe identifier used for workspace names |
| `version` | yes | bump when accepted data or bake output changes |
| `title` | no | text displayed by the UI |
| `abi_order` | no | preferred Android ABIs |
| `input` | no | discovery directories and safety limits |
| `extract` | yes | payload-selection rules |
| `hooks` | no | bake commands executed without a shell |
| `validate` | no | additional final-output checks |
| `commit` | yes | non-overlapping roots published together |
| `marker` | no | completion marker path |
| `space` | no | free-space safety reserve |
| `log` | no | relative log path |
| `detail_log` | no | lossless per-file/hook log; default `nxextract-detail.log` |
| `result` | no | reserved; if present it must be fixed `nxextract-result.json` |

`id` and `title` identify the port and UI. Neither is an expected package
filename.

`commit` may not overlap `.nxextract`, `marker`, either log or the terminal
result; those protected outputs may not overlap one another. A recipe reached
through a symlink/hardlink is refused.
These checks, including every template field, finish before a log or workspace
is created.

## Input discovery

Common `input` fields:

| Field | Default | Meaning |
|---|---|---|
| `search_dirs` | `["gamedata", "."]` | relative directories searched in order |
| `prefer_first_nonempty` | `true` | stop after the first directory with candidates |
| `sniff_all_in_primary` | `true` | inspect extensionless files in the first directory |
| `extensions` | APK/bundle/ZIP/OBB set | additional candidate extensions |
| `max_files` | `128` | candidate-file limit |
| `max_bundle_apks` | `128` | inner-APK limit |
| `max_member_bytes` | 8 GiB | maximum expanded inner APK |
| `max_bundle_bytes` | 16 GiB | maximum total inner APK bytes |
| `packages` | unset | Android package names this recipe accepts |

`packages` is what keeps two games of the same studio apart. Sibling titles
share an engine, a build pipeline and even asset filenames, so content rules
alone accept the wrong package and install the wrong game without a complaint.
A package the recipe does not list is refused by name, and the log says which
package arrived and which one the port wants. An APK whose manifest cannot be
read is still considered: what decides then is content validation, never a
missing piece of metadata.

An explicit repeated `--input` bypasses directory discovery but not content
classification or validation.

## Extraction rules

Each `extract` item has a unique `id`, a `source`, a `destination` and optional
`validate`, `source_validate`, `output_validate`, `required` and `mode`.

`validate` is checked twice: on the freshly extracted payload and again on the
final result, after any hook. That is fine until a hook legitimately changes the
payload — recompressing a texture, downmixing audio — because then no single
rule can describe both states, and the recipe has to widen until it fits them
both. A widened rule also accepts a stage interrupted *mid-transformation*,
which is exactly the state it should reject.

`source_validate` and `output_validate` split the two phases, so each side can
declare exact sizes and fingerprints and the intermediate state satisfies
neither. Either may be omitted, and an item that declares only `validate`
behaves exactly as before.

Source kinds:

| `kind` | Selects |
|---|---|
| `entry` | one member inside an APK or bundle archive |
| `entries` | a member tree |
| `file` | one loose file |
| `entry_or_file` | either archive member or loose file |
| `container` | the selected APK itself, copied as one file |

Useful source fields:

- `patterns`: ordered glob patterns; `{abi}` is expanded;
- `scopes`: `apk`, `bundle` or both;
- `strip_prefix`: prefix removed from a selected tree;
- `flatten`: discard source directories;
- `file_extensions`: candidate loose-file extensions;
- `case_sensitive`: matching policy;
- `split` (`container` only): which APK of the set to copy. Omitted means the
  base APK — the one that carries the app's own `assets/`. Name a split
  (`config.arm64_v8a`) to copy that one instead.

`container` exists for ports whose payload is the package itself rather than a
subset of its members: an engine that reads its resources straight out of the
APK at run time needs that file, and extracting the whole tree only to zip it
again would cost twice the card space. `container` defaults to the `apk` scope;
the bundle that wraps the APK is not a candidate.

Every `container` recipe requires `input.packages` and must never bind the APK
to external SHA-256, CRC32, exact size or filename values -- in ANY quantity
and in `validate`, `source_validate` or `output_validate` (V3, codes
NXA0001..NXA0006).
`container.source.patterns` remains the name-agnostic `["*"]`; a split is
selected by its manifest `split` value, and its destination is fixed rather
than derived from `{basename}`. Direct output and hook-checkpoint validation of
that destination obey the same ban. Signing, alignment, packaging tool, member order and
compression can change those values without changing compatible content, so
container identity is never compatibility. Combine package identity with
bounded size/ZIP magic and critical internal payload, tree or
transactional-hook checks. Record the tested copy's size, SHA-256, version
and versionCode only in the documentation-only `reference_build` block and in
`INSTALLATION.md`; those values identify the reference build for receipts and
support, never an extraction allowlist. Internal payload hashes may select a
`patch_profiles` entry that really depends on those bytes, and every profile
declares a `fallback` so an unknown compatible build follows the
generic/symbolic path instead of being rejected.

`compatibility.required_members` is evaluated against the logical base+split
APK set:

- a string is an implicit `core_required`;
- `{"member": "...", "role": "optional"}` records presence but never
  rejects;
- `variant_required` also names `variant`, which must be an exact
  `abi_order` token and gates only that resolved ABI;
- `patch_selector` links by member path to one or more `patch_profiles`.
  Profiles require an internal-payload SHA-256 and one common fallback. A
  unique match selects its profile; absent/unknown bytes choose the fallback.

Android/JAR signing members in `META-INF` cannot be compatibility members,
required extract anchors or patch selectors.

Single-file destinations support `{abi}` and `{basename}`. Tree destinations
support `{abi}`.

## Validators

File validators:

- exact `size`, `min_size`, `max_size`;
- `crc32` or `sha256`, each accepting one value or a list;
- `magic_ascii`, `magic_hex`, `magic_offset`;
- `elf_machine`: `arm64-v8a`, `armeabi-v7a`, `armeabi`, `x86_64`, `x86` or
  `{abi}`. The template resolves to the ABI currently being evaluated, so one
  recipe can strongly validate both ARM64 and ARMv7 payloads.

Tree validators:

- `exact_files`, `min_files`, `max_files`;
- `exact_entries`, `min_entries`, `max_entries` as compatibility aliases for
  the corresponding file-count fields;
- `exact_bytes`, `min_bytes`, `max_bytes`;
- `required_paths`;
- `tree_fingerprint`.

Use SHA-256 for critical libraries, OBBs and bake sentinels. Count-only
validation is appropriate only when version drift is harmless.

## Hooks

A hook is an argv array, not shell text:

```json
{
  "id": "texture-bake",
  "argv": [
    "{game_dir}/tools/texture-bake",
    "--source",
    "{stage}/assets"
  ],
  "cwd": "{game_dir}",
  "checkpoint": [
    {
      "path": "assets/.texture-bake-ok",
      "type": "file",
      "sha256": "REAL_SHA256"
    }
  ]
}
```

Templates:

- `{game_dir}`;
- `{stage}`;
- `{workspace}`;
- `{recipe_dir}`;
- `{abi}`.

Only those exact fields are accepted in hook `argv`, `cwd` and `env`; attribute
lookups, conversions and format specifications are forbidden. Extraction
patterns/strip prefixes/commit paths accept only `{abi}`. Single-file
destinations additionally accept `{basename}`. An unknown field is a recipe
error before extraction starts.

The hook also receives `NXEXTRACT_GAME_DIR`, `NXEXTRACT_STAGE`,
`NXEXTRACT_WORKSPACE`, `NXEXTRACT_ABI`, `NXEXTRACT_PROGRESS_FILE` and the
bounded canonical JSON receipt `NXEXTRACT_COMPATIBILITY_JSON`. It contains
member presence and authenticated profile/fallback decisions and is included
in the plan fingerprint. Every `NXEXTRACT_*` name is engine-reserved and
cannot be overridden through hook `env`.

### Transactional hooks (1.2.17+)

A hook that replaces or creates several files should declare
`"transactional": true`. The contract:

- the engine provides an empty shadow workspace in `NXEXTRACT_HOOK_SHADOW`;
  the hook writes **every** output there, mirroring the stage-relative paths,
  and must not modify the stage (the engine proves the checkpoint inputs
  unchanged by SHA-256 and refuses the hook if they moved);
- after the hook exits 0, the engine validates the checkpoint on the
  shadow+stage overlay, seals a journal (per-output SHA-256, counters,
  integral fingerprint) and only then publishes, one atomic rename per
  target, with `fsync`;
- interruption before the seal discards the temporaries; interruption after
  the seal rolls forward deterministically from the journal. A partially
  transformed stage is never adopted, and the retry of the same container
  ends at the same fingerprint;
- transactional hooks run with `PYTHONUNBUFFERED=1`, and on failure the last
  detail line reaches the summary sanitized (no absolute paths, no owner
  container names);
- outputs replace or create files; a transactional hook cannot delete stage
  files. The shadow needs free space for the outputs while they are staged.

Hooks without the member behave exactly as before.

Every compiled AArch64 hook shipped with a public port must pass:

```bash
./tools/check-glibc.sh path/to/hook
```

## Versioning

The marker contains a digest of the whole recipe. Any recipe edit invalidates
the fast path. Bump `version` when users or maintainers need that change to be
visible in UI and support reports.

NXExtract 1.2.21 markers also contain a stable content seal and content-byte
count for every immutable object under `commit`. Recipe-declared `mutable`
paths are excluded from both metadata and content seals. Only a 1.2.20 marker
with the exact current recipe/plan contract can migrate; it first passes full
declared output validation. `--reuse-only` may perform that atomic marker
migration, but metadata drift additionally requires `--expected-content-seal`.
The read-only `content-seal` command computes that value without changing the
marker. Reuse-only never recovers a pending transaction or starts installation.
