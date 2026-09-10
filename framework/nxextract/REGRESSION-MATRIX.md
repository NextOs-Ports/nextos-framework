# NXExtract 1.3.0 regression matrix

| Boundary | Gate | Required result |
|---|---|---|
| Kill por grupo | filho de hook que ignora TERM não sobrevive ao orçamento; `kill` no lugar de `killpg` reprova | `tests/test_nxextract.py` |
| Fixture legal externa ponta a ponta | o pipeline gerado aceita a cópia legal de referência; rejeitá-la reprova o gate | `tests/test_legal_fixture_e2e.py` |
| Identidade do container não decide | renomeado, reempacotado (ordem/compressão/timestamps/extra fields) e outro formato resolvem o MESMO conjunto de payloads | `tests/test_legal_fixture_e2e.py` |
| Negativos reais | outro package, payload obrigatório ausente e ABI não declarada falham fechado nomeando a propriedade técnica | `tests/test_legal_fixture_e2e.py` |
| Fixture ausente | sai 77 declarando limite de claim; nunca um PASS inventado | `tests/test_legal_fixture_e2e.py` |
| Cerca de recursos fail-closed | limite que não pode ser estabelecido recusa o hook; controle positivo prova que o hook roda sem a falha injetada | `tests/test_nxextract.py` |
| Full install | `test_terminal_result_covers_install_fast_path_and_adoption` | `success/NXE0000`, package, ABI, sanitized content identity and exact totals |
| Valid marker | same test, second run without source | `success/NXE0001`, same validated totals and identity |
| Metadata-only drift | `test_metadata_only_drift_reseals_by_content_without_ui_or_extraction` | content-authenticated marker reseal; no UI, scan or extraction |
| Same-size corruption | `test_reuse_only_rejects_same_size_corruption_before_ui` | fail closed before UI; installed bytes are not replaced |
| Marker migration | `test_legacy_120_marker_migrates_without_ui_or_extraction` | metadata drift fails without an expected seal; the matching seal promotes 1.2.20 to 1.2.21 without UI/extraction |
| Fast path | `test_exact_current_marker_keeps_metadata_fast_path` | exact metadata never reads the full payload content |
| Existing-data adoption | same test, marker and source absent | `success/NXE0002`, `existing` identity and full tree-file totals |
| Failure | `test_terminal_error_is_stable_compact_and_sanitized` | finite error code, last phase, zero unvalidated totals, terminal cause |
| Visible UI receipt | `test_required_ui_accepts_private_sdl_readiness_proof`, `test_required_ui_accepts_private_fbdev_readiness_proof` | cleanup removes private runtime controls while terminal JSON retains `visible` plus exact approved renderer |
| Atomicity | `test_terminal_result_atomic_failure_preserves_previous_document` | failed rename leaves the prior complete JSON and no temporary member |
| Redaction | terminal result/error tests | no external container filename, URL or host path |
| Compact default | 2,000-record logger fixture | file chatter absent, first miss + repeat count + terminal cause present; detail >100x compact |
| Verbose opt-in | logger fixture | detail is mirrored only when explicitly enabled |
| Transactions | complete 104-case Python suite | recovery, rollback, dual marker seal, bundle/container and filesystem gates unchanged |
| Pending transaction in reuse-only | `test_reuse_only_refuses_pending_transaction_without_recovery` | fail closed without rollback, roll-forward, UI or extraction |
| Runtime isolation | `tests/test_runtime_env.sh` | firmware-first child boundary unchanged |
| Visual identity | `tests/test_visual_identity.sh` | `draw_screen()` plus 640x480 and 1280x720 hashes exact |
| UI release | release manifest + ELF gate | four immutable GLIBC 2.17-or-lower artifacts exact |
| Public release gate | `tools/check-release.sh --require-ui` | version 1.2.21, 104 cases, 4 immutable UI ELFs, all gates green |
| Generated-port compatibility | nxbootstrap manifest contract | component is 1.2.21; existing ports remain pinned until explicit opt-in |
