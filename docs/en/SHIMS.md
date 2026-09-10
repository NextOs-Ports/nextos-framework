# Shims: from imports to tested contracts

[Português](../pt-BR/SHIMS.md)

A shim adapts an interface expected by an Android guest to its Linux host. Correct implementations preserve signature, calling convention, layout, ownership, lifetime, errors and callbacks. Finding a symbol name does not demonstrate compatibility.

## 1. Study the executable example

Start with [examples/shims-reference](../../examples/shims-reference/README.en.md). `shims.h` defines signatures and a function-pointer union; `shims.c` registers explicit implementations only; `main.c` exercises their contracts.

| Contract | Demonstrated behavior | Limit |
| --- | --- | --- |
| `__errno` | Returns the calling thread's `errno` address | Does not cover all Bionic TLS |
| `__android_log_write` | stderr diagnostics; 1 on delivery, negative error on failure | Does not implement Android filtering/logd |
| `nx_demo_property` | Only `demo.name`, with explicit capacity | Not `__system_property_get` |
| Resolver | Missing name or wrong enumerated signature returns NULL | The enumeration cannot infer an ELF ABI |

The teaching resolver does not perform relocations. Before integrating with a real loader, the assistant must identify the ELF symbol type and signature from the library/engine contract. An object such as `__stack_chk_guard` must not become a function address.

## 2. Inventory imports

With an owner library prepared in a private area, use `readelf -Ws` for symbols, `readelf -d` for dependencies and `readelf -r` for relocations. Separate required undefined symbols, weak symbols, objects and TLS. Check imports obtained at runtime through `dlsym`, `eglGetProcAddress` and JNI: the static list may not include them.

A useful work table contains: name; symbol version; type; ABI; signature; caller; expected result/error; buffer owner; thread; selected implementation; source/hash/license; tests; limitation. This table belongs to the new port.

## 3. Choose the adaptation type

| Case | Decision |
| --- | --- |
| Proven identical ABI and semantics | Forward to the host with the correct type |
| Different layout/enum/error | Explicitly translate inputs and outputs |
| Asynchronous callback or stateful object | Implement lifecycle and synchronization |
| Missing optional service | Return the contract's defined absence, with evidence for that path |
| Unknown required service | Fail with a precise diagnostic and implement before acceptance |

Do not blindly cast `pthread_mutex_t`, `FILE`, signal structures, `dirent` or JNI objects. Do not force every mutex to be recursive. Managed pointers, JNI handles and native pointers are not interchangeable.

## 4. Implement a new contract

First write cases that distinguish correct behavior from a stub: valid input, buffer boundary, real error, NULL where allowed, concurrency, ownership and destruction. Then register the typed function and integrate a known caller. Do not hide incompatibility behind generic casts.

For JNI, register classes, methods and fields by exact signature. Preserve local/global references, exceptions, string conversions and thread attachment. `JNIEnv` belongs to its thread; do not share an arbitrary pointer among workers. [Official JNI guidance](https://developer.android.com/ndk/guides/jni-tips).

## 5. Find larger implementations

| Boundary | Starting source |
| --- | --- |
| ELF and relocations | [nxloader](../../framework/nxloader/README.md) |
| Android/lifecycle | [nxandroid](../../framework/nxandroid/README.md) |
| Context/present | [nxgl](../../framework/nxgl/README.md) |
| Mixer/output | [nxaudio](../../framework/nxaudio/README.md) |
| Input and contexts | [nxinput](../../framework/nxinput/README.md) |
| Unity | [Public cases and diagnostics](../../portando_unity/README.en.md) |
| Mono/.NET, Godot, Cocos | [Mono](MONO-ANDROID.md), [Godot](GODOT.md), [Cocos2d-x](COCOS2D-X.md) |

Read manifests and licenses before adapting code. Upstream code can contain specific solutions that must not become framework defaults.

## 6. Measure coverage honestly

Classify each contract as `implemented-tested`, `adapter-required`, `optional-absent` or `unsupported`. Record the data profile/ABI exercised. “All imports resolved” measures resolution; it does not measure gameplay, concurrency, saving or graphics fidelity.

Broader coverage must develop around families and repeated contracts in the catalog. The current example teaches structure; it is not a nearly complete Android shim or a promise to run most Android games. Shared V5 changes require separate V6 development.
