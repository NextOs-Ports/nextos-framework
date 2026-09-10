# Vulkan → GLES2 shader laboratory

[Português](README.md)

A reproducible lesson using original shaders, inspired by the method documented for Freedom Planet 2. It contains no game-extracted shaders, Unity files or commercial offsets.

## 1. Prepare the tools

On a Linux host, use Git, Python 3.11+, a C++17 compiler and `glslangValidator`. The verified run used glslang 16.4.0. The script records the actual version; a different version requires reviewing the result. SPIRV-Cross and SMOL-V sources are commit-pinned in [sources.json](sources.json).

```sh
python3 examples/shader-lab/run.py --fetch   --deps work/shader-dependencies --output work/shader-results
```

`--fetch` enables downloads from the two declared public repositories. Without it, missing sources produce an error. Existing directories must be clean at the exact commit. Building does not alter the FP2 translator or install dependencies globally.

## 2. Follow the transformation

`color.vert` and `color.frag` produce Vulkan 1.0 SPIR-V. The [original translator](translate.cpp) compresses to SMOL-V, decodes and compares every byte before translating to ESSL 1.00. The fragment shader preserves alpha and explicitly premultiplies RGB. glslang validates the resulting text.

`unsupported.comp` is the negative case: compute is excluded from this lesson's GLES2 route. The translator returns an error and creates no output. A failed shader is never replaced by a generic one. `RESULT.json` records five checks, tools and pins.

## 3. Distinguish FP2 pins

SMOL-V commit `4b52c165c13763051a18e80ffbc2ee436314ceb2` exactly reproduces the `smolv.cpp`/`.h` hashes required by the historical FP2 script.

Historical SPIRV-Cross commit `eb32b288ea553e938005fcfd819a2290b1c8032d` could not be obtained from Khronos upstream during this review (`not our ref`). The lesson uses a different explicitly pinned public commit. **This does not replace the FP2 pin or prove an identical rebuild of its translator.** Recover that exact pin's origin before rebuilding approved FP2; do not remove its check. The historical script also requires SPIRV-Tools 2026.3.1, which this independent lesson does not use.

## 4. Apply the method to a port

Host translation does not prove driver compilation, Unity bindings, stencil, pixels, variants or gameplay. Add a physical GLES2 test on authorized hardware. The [FP2 study](../../portando_unity/en/FP2-VULKAN-GLES2.md) describes further boundaries. References: [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), [SMOL-V](https://github.com/aras-p/smol-v).
