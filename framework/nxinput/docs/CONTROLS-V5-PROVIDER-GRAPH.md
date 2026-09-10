# V5 controls — provider descriptor, physical translation and proof graph (nxinput 0.11.1)

## The defect (P0, 2026-09-03)
V4 decided the C6 rewrite target with `nxinput_sdl_api_domain(API_2) = sdl2-evdev`
(upstream high-first). Knulli, Batocera and muOS ship an SDL2 with the RetroArch/Batocera
input patches: `ConfigJoystick` walks every EV_KEY in ONE ascending sweep
(`tests/providers/patches/sdl2_input_as_retroarch_udev.patch` + `p007`), axes stop at
`ABS_MISC` (`p005`). Their own mappings are native to that provider; V4 rewrote them into the
upstream order and every ordinal preceded by a low key moved (A→volume, L1→START,
SELECT→L3, START→L2). `tests/v5/test_v5_incident_red.c` reproduces both field receipts
exactly (`rewritten_bindings=15 volume_markers=2` / `13 / 0`).

## The graph
```
event node (EVIOCGBIT/EVIOCGABS)  +  provider descriptor (bytes the process mapped)
  -> source ordinal domain      (declared by producer/bundle, or proved by exclusion)
  -> typed physical identity    (EV_KEY / EV_ABS / hat pair)
  -> provider ordinal domain    (MEASURED in-process by the ById API > pinned sha256
                                 (compiled manifest or runtime pin file) > UNKNOWN)
  -> mapping line               (byte-intact when source == provider; rewritten when proved
                                 different; provider UNKNOWN = STOCK MODE: nothing translated,
                                 nothing external in the store, the pad is announced with what
                                 the provider itself holds -- never mute)
  -> C3 sovereign order + real setter + readback (unchanged)
  -> NEXTOS_CONTROLLERS/4 owner -> one primary typed route -> sink -> registry/prompt
```

## Provider descriptor (`nxinput_provider.h`, `nxinput_provider_linux.c`)
- entry = `dlsym(RTLD_DEFAULT, "SDL_Init")` (what the loader bound for THIS process);
- `/proc/self/maps` → dev/inode/offset of the mapping containing it;
- `/proc/self/map_files/<range>` FD (or the path FD whose fstat inode/dev match; inode-only on
  major-0 virtual devices = `sha_bound=2`, reduced confidence); FD held across the decision;
- sha256 of those bytes, ELF Build ID, DT_SONAME, `SDL_GetVersion/GetRevision` from the same
  object (`dladdr` base equality);
- method order (0.11.1): `measured-inprocess` (the ById API of the mapped object CALLED for the
  opened instance, table matched against the transcribed plans; a table no plan reproduces =
  UNKNOWN with `measurement_conflict=1`) > `pinned-elf` (`tests/providers/provider-manifest-v5.json`
  compiled table gated by `tests/v5/test_v5_provider_pins.py`, plus a runtime pin file
  `NXINPUT_PROVIDER_PINS`) > `exported-api` (the symbols exist: measurable, domain still
  UNDECLARED) > `unknown` (no rewrite, ever; stock mode). The presence of a symbol never infers a
  domain. `compat_over_sdl3=1` marks sdl2-compat (the SDL2 ABI over the SDL3 core in the same
  process): one provider behind two majors.
- per DEVICE (`nxinput_provider_probe_device`): driver evdev / hidapi (hidraw or a char device
  without EVIOCGBIT) / virtual / unknown, bus/vid/pid/phys/uniq/caps digest. HIDAPI devices are
  admitted in stock mode with the provider's own mapping (`source=provider-native-hidapi`).
- receipts land in `NXC6_RECEIPT`: `NXC6-PROVIDER`, `NXC6-DEVICE`, `NXC6-MEASURE`, `NXC6-STAGE`,
  `NXC6-ROUTE`, `NXC6-COEXIST`.
- receipt: `NXC6-PROVIDER api= version= soname= path_class= sha256= build_id= sha_bound=
  static= bytable= method= domain= pin= driver= confidence= generation=`.
- majors never share a table/instance (`nxinput_provider_same_instance`).

## Stock mode and staging (0.11.1, review finding 1)
`nxc6_stage_before_init()` resolves the provider BEFORE staging `SDL_GAMECONTROLLERCONFIG`: with
a table it stages (as 0.10.0 did); UNKNOWN leaves the variable for the provider's own import and
the seam later answers `NXINPUT_SDL_SEAM_ADMIT_STOCK` (`source=stock-passthrough
decision=DO_NOT_MUTATE_STORE passthrough=1 translated=0 mapping_present= env_reinstated=`). The
legacy blind staging (`nxinput_sdl_seam_stage_before_init`) under UNKNOWN is repaired by handing
the CFW's own env line back to the setter — the ONE EXISTING_NATIVE_PASSTHROUGH of 1.4; file and
bundle lines never. RED cases: `tests/v5/run-unpinned-provider-oracle.sh` (host, the libSDL2
copied with bytes appended) and `tests/v5/receipts/device-oracle-*-unpinned-stock*.json` (device).

## Domains (`nxinput_sdl.h`)
`sdl2-evdev` (upstream), `sdl2-legacy-evdev` (2.0.10 hats), `sdl3-evdev`,
`sdl2-ascending-patched` (one sweep `0..KEY_MAX`, axes `0..ABS_MISC`), `joydev-legacy`.
Pinned providers: 8c4dc956… (2.30.12 patched, ascending, ById; measured on aarch64),
40d0616f… (muOS 2601.1 2.28.5 patched, ascending, ById; measured on aarch64; 32-bit sibling 4e95cde5… undeclared),
4fd539cd… (2.32.10 upstream; measured on device), 1ac99b5c… (private fork; MEASURED on
device: high-first — symbol absence was not the reason), 5da9fd36… (host 2.32.70),
eceaf5f9… (SDL3 3.5.0, table undeclared).

## Translation (`nxinput_translate.h`)
`nxinput_translate_line(line, key_bits, abs_bits, provider_domain, source_descriptor)`:
- source domain: declared (producer/bundle) → else exactly one coherent candidate → else all
  coherent candidates identical → else unproven (byte-intact);
- `bN/aN/hN.mask` with sign/inversion/half-axis preserved; hats by the DOMAIN's detection
  (`nxinput_sdl_hat_present`: ascending = any present half is a hat, p009; upstream = the pair);
  `guide` may sit on a system key (KEY_MENU/HOMEPAGE/HOME/BACK/ESC/SELECT/OK/POWER/ENTER/SPACE);
- results: `byte-intact-native`, `rewritten`, `byte-intact-unproven`, `rejected` (source yields).
- receipt: `NXC6-DOMAIN guid= matching= rewritten= rewritten_bindings= native= unproven=
  rejected= bindings= source_domain= provider_domain= provider_method= result=`.

## Transaction (`nxinput_sdl_seam.c`)
Same-GUID divergent second instance is refused in `bridge_apply_mapping`, BEFORE
`SDL_GameControllerAddMapping` (`store_mutated=0`). Setter accepted + hostile readback is
still no decision. Provider UNKNOWN (`target_domain=undeclared`): with a V5 ops table no external ordinal line
reaches the setter, even unchanged; the pad continues only through the provider's own built-in
database or a declared raw route (degraded, diagnosable). Never the major's presumption.

## Proof oracle (`tools/nxoracle_v5.py`, `tests/v5/run-host-oracle.py`)
Stimulus = physical profile (position → EV_KEY, trust root DTS/es_input); provider table from
the descriptor/manifest; the mapping under test never produces its own expectation.
`tests/v5/mutation/run_mutants.py`: the V4 tool lets "swap A/B" and "ascending provider"
survive (circularity proved); the V5 oracle kills every listed mutant.
Harness `tests/v5/harness_sdl2_provider.c` = pure SDL2 consumer with the real glue, device-
faithful uinput clones, judged by position; receipts on host, dArkOS K36S and NextOS Elite.

## The 1.4 machine (`nxinput_decision.h`) — one decision, no parallel policy
```
SOURCE_TRUST (translate evidence) × CONSUMER_KIND (SDL2/SDL3/GODOT/RAW_EVDEV/ANDROID/ENGINE_DIRECT)
× CONSUMER_TABLE (provider descriptor)
  PROVED + PROVED + integral equivalence -> KEEP_EXISTING_BYTE_INTACT (setter allowed)
  PROVED + PROVED + divergent            -> TRANSLATE_TYPED (setter allowed, no BYTE_INTACT)
  PROVED + NOT_APPLICABLE                -> ROUTE_TYPED_DIRECT (no setter, no BYTE_INTACT)
  any UNKNOWN                            -> DO_NOT_MUTATE_STORE (+ passthrough only if native/proved)
```
Integral equivalence (C2) = domain label AND backend AND hat/half-axis/inversion/trigger flags
AND complete ordinal table digest for the measured caps AND corpus digest AND physical digest.
The glue calls it per line (`nxc6_decide_line`): unproven → the source yields
(`result=source-unproven-yields`); native but not integrally equivalent → typed translation.

## One policy in front of every setter (0.11.1, C6)
`nxinput_route_policy` decides for each route (V5 seam, SDL3/PortMaster, V3 ordinal fix, Godot
native seam) whether its setter may run: the V5 seam through the 1.4 machine; SDL3/PortMaster and
the V3 ordinal fix are LEGACY (explicit opt-in, provider of the right major with a table, proved
source) and every refusal is receipted (`NXC6-ROUTE`); the Godot native seam is a typed direct
route and, since 0.11.1, accepts a declared `sdl2-ascending-patched` line by TRANSLATING it into
the engine's high-first numbering by physical code.

## Authority, keyboard, coexistence, host gate (0.11.1)
- `nxinput_authority_v5`: AUTHORITY executable — NEXTOS (editable owner; native edges delegate one
  edge), ENGINE (no owner GPTK: a named readback mirror; registry from readback), SYNCHRONIZED
  (requires the four engine hooks, else refused; nothing silently NEXTOS). The chord pre-router
  and a stale mapping generation always suppress.
- `nxinput_keyboard`: physical keyboard → `[keyboard.*]` chords → actions through the SAME
  output router as the gamepad (one press/one release however many sources hold an action);
  event and polling backends produce identical sequences; loops refused in the parser (also the
  cross-section one) and at the source.
- `nxinput_coexist`: SDL2+SDL3 in one process — the global `SDL_GAMECONTROLLERCONFIG` sequenced
  per major (provider-local hint too), concurrent inits refused, arbitration `separate` /
  `shared-core` (sdl2-compat) / `ambiguous` (fail closed); physical owner election in
  `nxinput_prerouter_bind_physical` (observers never double a chord or a pause).
- `nx-gptk4-host-gate`: the universal closure gate for a schema-4 owner (contract + owner dir +
  declared contexts [+ closure.tsv]) emitting `NXGPTK_PROOF` lines; the Tearscape owner is the
  RED fixture.
- `nxinput_padset` (engine glue) now carries the pre-router (START/SELECT retained in the window,
  one exit per chord, generation per instance), the admitted-vs-opened instance check and
  `nxinput_padset_vector_norm()` (5.5 calibration + radial deadzone); `nxinput_gptk_live` carries
  the vector gesture neutral floor (item 2).

## Lifecycle, pre-router, corpus
- `nxinput_prerouter`: sovereign SELECT+START, outside PORT_AUTHORITY_MODE, bounded window,
  exactly-once forwarding, never cross-pad/cross-generation, no latch on focus/unplug.
- `nxinput_lifecycle`: admit → open(generation re-checked) → edges → remove; transitions =
  release-all → epoch++ → neutral gate → one owner (declared fallback for unknown/loading).
  Hot reload and `nxinput_sync` (synchronized CAS) publish only after release + readback.
- `nxinput_corpus`: origin/priority/platform inventory; matching=N collapse; divergent
  duplicates refused before the store; digest feeds the equivalence.

## Static provider (mission 5.1, `nxinput_provider_declare_static`)

An engine whose SDL is BUILT IN (Godot + SDL3 in Tearscape) has no DSO to fingerprint.
It declares the provider explicitly, before `SDL_Init`, with
`nxc6_declare_static_provider(&SDL_Init, pin_id, domain)`: the entry must live in the
main program (a DSO address is refused), the pin must exist under
`tests/providers/provider-manifest-v5.json` → `static_sources` (source tarball + seam
patch, both sha256-pinned) and the domain is the table measured for that source. The
receipt reports `method=declared-static-source`. Nothing else may reach this path; an
undeclared static SDL stays UNKNOWN (`DO_NOT_MUTATE_STORE`).

## Device proof: the clone must publish the pad's resting values

`tools/nx-input-inject-agent.py` creates uinput clones from the measured profile. A
fresh uinput node reports `0` on every axis until its first event: on a `0..255` pad
(NextOS Elite "USB Gamepad") that is full deflection, so the game opened a stick gesture
before any stimulus and every edge-based stick expectation failed. The clone now emits
the `EVIOCGABS.value` measured on the real device at creation and on every `rest()`.
The same lesson on the consumer side: a vector gesture edge must use the cursor
deadzone as its neutral floor, never `!= 0` (asymmetric ranges normalize to +0.0039).

