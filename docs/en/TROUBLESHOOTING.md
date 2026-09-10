# Troubleshooting and the next diagnostic step

[Português](../pt-BR/PROBLEMAS-COMUNS.md)

Record the command, first meaningful failure, ABI and tool versions. Do not replace diagnosis with multiple global flag changes. Run reference scripts only after reading their paths and effects.

| Symptom | Check first | Next step |
| --- | --- | --- |
| Private clone denied | Account permission and GitHub authentication | Fix access without putting a token in the command |
| CMake cannot find compiler | Executable path and loaded toolchain | Use a new build directory when switching compiler |
| Missing `crt1.o`/`crti.o` or `-lc` | Complete development sysroot and ABI | Fix SDK; do not link x86 files into ARM |
| “file in wrong format” | `readelf -h` on objects and libraries | Separate Linux/Android, ARM32/ARM64 and host |
| “Exec format error” | Executable architecture versus host | Use the correct target or explicit emulation |
| No cross CTest tests | `CMAKE_CROSSCOMPILING` and example CMakeLists | Record successful build; ARM execution still pending |
| `GLIBC_x.y not found` | Required versions across all ELFs | Build only the new candidate with a compatible sysroot |
| Resolved import followed by crash | Type, signature, layout, TLS and ownership | Target the contract; do not use generic casts |
| JNI returns null and fails later | Class/method/field and expected exception | Implement the actual required object/callback |
| Unity stops near the third frame | Present, synchronization and callbacks | Measure the boundary; Swappy patches require a proven profile |
| Audio with a black screen | Pre-present pixels, FBO, shader and compositor | Invalidate graphics; read Unity/Godot diagnostics |
| Text displayed as blocks/squares | Atlas channel, alpha, SDF and sampler | Correct the specific format/material |
| Audio too fast/slow | Actual mixer versus device sample rate | Fix the contract or conversion, preserving frame counts |
| Button confirms and cancels | Duplicate events and competing owners | One route per action/context with actual release |
| Click misses the button | Drawable, viewport and Y origin | Transform position and delta into consumer space |
| NXExtract only accepts prepared data | Recipe, complete input and hooks | Test from scratch; adoption does not establish installation |
| Launcher returns before logging | Early error log and resolved paths | Diagnose the boundary before runtime |

## Report a useful issue

Include collection/port commit, executable SHA, system/ABI/GPU, technical game profile, secret-free command, expected and observed results, and checks already performed. Say whether it occurred on host, emulation or hardware. Do not attach commercial data, dumps or complete private logs.

For a regression, compare against the approved executable and the same data copy. Preserve that artifact and recipe before rebuilding. A data change can explain different behavior even when the game name is unchanged.

Read [shims](SHIMS.md), [Unity](../../portando_unity/README.en.md), [Mono Android](MONO-ANDROID.md), [Godot](GODOT.md) and [Cocos2d-x](COCOS2D-X.md) according to the engine.
