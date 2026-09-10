# nxdoctor — Regression Matrix (v0.3.0)

Gates owner-local: `python3 -B tests/test_nxdoctor.py` e
`python3 -B tests/test_evidence.py`.

| Guarantee | Test (in `tests/test_nxdoctor.py`) |
| --- | --- |
| Default run performs zero writes | healthy tree whole-tree hash (names+modes+contents) identical before/after `--json` and human runs |
| State reported: active/pending/previous_healthy/activation_seq/prehealth_failures + v2 health fields | healthy and real-shape v2 report assertions; `failure_generation` blocks GC |
| Legacy install (no state.json) reported, not treated as an error | `legacy-port` → `state.mode == "legacy"` |
| Generation verification uses commit marker + strict manifest + sha256/mode only | GEN_A/GEN_B complete; GEN_D hash-mismatch → corrupt; GEN_E missing commit → incomplete; unparseable manifest → corrupt |
| Clock independence (V3-CLOCK-01): never mtime/ctime/filename | mtimes deliberately inverted (older/healthy generation stamped newest, broken ones oldest) before re-verifying; statuses unchanged |
| Unreferenced generations listed, never deleted by the report | `unreferenced_generations == {C,D,E}` and GEN_C dir still present |
| Sanitized output: no absolute personal paths | `abspath(port) not in stdout` on the JSON report |
| gptk validation ok/invalid, never rewritten | positive fixture + negatives (bad magic, NUL byte, oversize, unknown section) with byte-identity re-check of the file |
| settings validation ok/invalid | negatives: missing magic, duplicate key, oversize, NUL byte |
| restore-previous happy path is atomic and bumps activation_seq | state rewritten to {active=previous, pending=null, seq+1}; rerun → `already-clear` (idempotent) |
| restore-previous refusals | wrong id refused; incomplete previous (commit removed) refused |
| complete-gc removes exactly the unreferenced complete generation | GEN_C removed; A/B/D/E/9 all still present |
| complete-gc collects corrupt-but-unreferenced | GEN_D removed with `status_before == "corrupt"` |
| complete-gc refusals | referenced active/previous refused; pending refused; traversal id `../../evil` refused; symlinked generation dir refused (target untouched); planted `game.apk` refused (apk survives); incomplete refused; rerun on absent id → `already-clear` |
| discard-staging confined to `.nxruntime/staging*` | outside-staging path refused; `..` traversal refused |
| discard-staging owner/link guards | symlink target refused; committed-generation lookalike refused; `save.dat` owner pattern refused; hardlink (st_nlink>1) refused; rerun → `already-clear` |
| clear-lock only for proved dead/reused owners | historic starttime live/refused and reused/cleared; real launcher token dead/cleared, token-live/refused; mismatch refused; rerun → `already-clear` |
| Owner data byte-for-byte preserved across every action | hash of `gamedata/` + `NEXTOSCONTROLLERS.gptk` + `NEXTOSSETTINGS.txt` identical before/after the whole action battery |
| Interruption safety of the atomic state write | monkeypatched `os.replace` raising after tempfile creation → `state.json` byte-identical, no `nxdoctor-tmp` leftovers, idempotent successful rerun |
| Negação do filesystem em toda ação | restore recusa sem trocar state; discard/clear desanexam atomicamente e registram quarentena quando a limpeza tardia falha |
| Fase publicada pelo launcher | fase/fronteira/status/child_status/sequence recuperados; ilegível, schema estrangeiro e ausente não derrubam relatório |
| Inventário de rotações | `events.prev.jsonl`, `nxextract-detail.log` e `.prev` aparecem no relatório |
| Mídia chmodless (exFAT) | geração 0777 continua verificada por hash; modos POSIX errados e bytes adulterados reprovam |
| Kit de evidência: quatro ações | execução real de restore/discard/gc/clear-lock, idempotência e receipts/status coerentes |
| v1/v2, ids 32/64 e chmodless | fixture moderna v2/64 com health fields e fixture legada v1/32 |
| Snapshot e owner-data | state, gerações, staging, lock, superfície protegida e owner-data; mutação byte a byte reprova |
| Recusas | quatro ações recusadas sem falso sucesso; negações injetadas independem de root |
| Preflight/confinamento/concorrência | pais symlink, EACCES/EIO, lock concorrente, state trocado e owner inválido recusam antes de mutar |
| Claim GC autenticada | conteúdo exato, hash, nlink e modo ligados ao snapshot; claim errada/hardlink recusada |
| Integridade fail-closed | hash/action/target/receipt/exit, JSON duplicado/truncado/recursivo, run stale e path hostil reprovam |
| Privacidade e arquivo externo | sem path absoluto/output cru; O_EXCL, no-follow, 0600 e proibição dentro do port |
| Separação física | host sempre `HOST_FIXTURE`; qualquer tentativa `PHYSICAL` é `PENDING` até existir trust anchor externo |
