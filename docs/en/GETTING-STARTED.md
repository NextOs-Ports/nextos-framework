# Getting started

[Português](../pt-BR/PRIMEIROS-PASSOS.md)

This walkthrough starts with a clone and ends with a C example running on your computer. It then prepares you to ask an AI assistant to implement a new port. The catalog contains source references for 45 titles; each reference retains its own requirements and limitations.

## 1. Prepare your computer

Use Linux with Git, Python 3, CMake 3.20 or newer, a C99 compiler, Make or Ninja, and ELF tools (`readelf`). The C project accepts CMake 3.16, but this guide uses test command features available in later versions. On Windows, run the commands inside a Linux environment; handheld GPU testing remains a separate step.

Check your environment before installing game-specific dependencies:

```sh
git --version
python3 --version
cmake --version
cc --version
readelf --version
```

## 2. Clone and verify the collection

While the repository is private, your GitHub account needs access. Use your normal authentication without pasting tokens into scripts or documentation.

```sh
git clone https://github.com/NextOs-Ports/nextos-framework.git
cd nextos-framework
git rev-parse HEAD
python3 publication/verify.py
```

Record the collection commit. The verifier checks imported files against their manifests and looks for recognized prohibited formats and patterns. A hash mismatch requires recovering the correct source; do not update the manifest just to make the error disappear.

## 3. Build your first example

Run all commands below from the clone root:

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Expected result: `explicit-shim-contracts` passes. The program demonstrates typed resolution, unknown-import errors, per-thread `errno`, and bounded buffer operations. [Read the example contract](../../examples/shims-reference/README.en.md).

A passing host test establishes only this example's behavior. It does not yet load an Android library, render on Mali, or run a game.

## 4. Separate reference material and development

Read [AGENTS.md](../../AGENTS.md) and select a reference from the [catalog](../../catalog/README.en.md). Keep the collection as a reference and write your port in `work/ports/<port-id>/` or a separate repository. Git ignores `work/`, but that does not protect files sent through other channels: keep commercial data out of commits and uploads.

Before coding, define the game/build, ABI, engine, target system/GPU, and the first result to demonstrate. Owner-supplied data belongs in an explicitly private location. An old device address never authorizes a new connection.

## 5. Choose your next track

| Goal | Reading |
| --- | --- |
| Understand the components | [Architecture](ARCHITECTURE.md) |
| Have AI handle implementation | [AI porting](AI-PORTING.md) |
| Build a Linux ARM executable | [ARM compilation](BUILD-ARM.md) |
| Implement Android imports | [Shims](SHIMS.md) |
| Investigate a Unity APK | [Porting Unity](../../portando_unity/README.en.md) |
| Prepare the owner's data | [NXExtract and installation](NXEXTRACT.md) |
| Understand approval claims | [Testing and delivery](TESTING.md) |

Read [troubleshooting](TROUBLESHOOTING.md) if your first command fails. Report the command, error and architecture without attaching an APK or a complete private log.
