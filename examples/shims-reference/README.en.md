# NextOS shim example

[Português](README.md)

This C99 project runs small, explicit contract tests. It does not load Android ELFs or implement a complete runtime. Use it to learn shim organization before integrating real code.

## Files and flow

| File | Purpose |
| --- | --- |
| [shims.h](shims.h) | Signatures, typed registry and property contract |
| [shims.c](shims.c) | Known implementations; errors for absence/mismatch |
| [main.c](main.c) | Exercises TLS, signatures, buffers and logging |
| [CMakeLists.txt](CMakeLists.txt) | C99 build, pthread and host test |

`nx_demo_resolve` requires a name and enumerated signature. The caller supplies that enumeration; it does not read or validate an Android library's ABI. Call the corresponding union member using its exact type.

## Build and test

From the repository root, with a C compiler and CMake installed:

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Expected result: `explicit-shim-contracts` passes and the program exits with status zero. Cases check unknown imports, mismatched signatures, NULL, independent `errno` in two threads, insufficient capacity, absent properties and an invalid logging tag.

## Contracts and limitations

`__errno` forwards to host TLS; it does not implement all Bionic TLS. Logging uses the `__android_log_write` signature, writes to stderr and returns 1 on delivery or a negative error; Android filtering is not implemented. `nx_demo_property` knows only `demo.name` and must never be registered as a substitute for `__system_property_get`.

Do not add a catch-all success stub. An extension needs declared semantics, inputs/outputs, ownership, error behavior and a discriminating test. Read [the shim guide](../../docs/en/SHIMS.md).

For ARM, follow [the build guide](../../docs/en/BUILD-ARM.md). CMake does not register tests in cross mode; an ARM build and ARM execution are separate stages. Full Android-loader, graphics/audio and extraction demonstrations remain separate work.

Code: GPL-3.0-only. Author: **NextOS**.
