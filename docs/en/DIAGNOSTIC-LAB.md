# Guided diagnostics with reproducible failures

[Português](../pt-BR/DIAGNOSTICO-GUIADO.md)

Run the [original training game's](../../examples/first-port/README.en.md) tests first. Each case below has an intentional failure and a verifiable condition, without commercial input.

## Read the symptom and choose a measurement

| Case/log | Expected result | Next action in a real port |
| --- | --- | --- |
| `cpu.log` | Constructor before JNI; contract PASS | Preserve ordering and hashes |
| `missing-import.log` | `FAIL resolve`; no constructor | Identify and implement the import signature; never continue with zero |
| `wrong-package.log` | `NXE3001`; no marker | Check the manifest and split set |
| `wrong-payload.log` | `NXE3001`; rejected payload | Compare internal build/hash before offsets |
| `missing-payload.log` | `NXE3001`; missing seed | Recover complete input; do not weaken the recipe |
| `wrong-abi.log` | `NXE3001`; rejected library | Check ELF, ABI and internal path |
| `hook-rollback.log` | `NXE7001`; previous data unchanged | Correct transformation while preserving approved state |
| shader/compute laboratory | `TRANSLATION REJECTED`; no output | Implement the operation or declare incompatibility |

## Distinguish other boundaries

An unaccepted JNI version requires checking the runtime contract; sharing JNIEnv between threads is incorrect even when the pointer is nonnull. TLS/IFUNC/RELR should appear early in [inventory](ANDROID-INVENTORY.md). V5 rejects operations outside its contract; do not conceal that with a permissive resolver.

On hardware, audio without graphics requires measuring the context and pixels at present. The laboratory's RAM framebuffer test is not that evidence. Missing Cocos text requires examining bitmap/alpha/stride; a stuck Unity transition may involve synchronization rather than shaders. Consult engine guides and record one hypothesis per measurement.

## Record a useful correction

Record the command, file/commit, technical input, reached boundary, error, hypothesis, minimal change, passing test, negative case that must still fail and physical result when available. Logs remain private until reviewed. New source or binaries do not inherit another artifact's approval.
