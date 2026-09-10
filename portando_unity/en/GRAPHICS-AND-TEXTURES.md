# Graphics, ETC1 and transparency

[Português](../pt-BR/GRAFICOS-E-TEXTURAS.md)

The desired result is a correct image on the available physical context. A translated program, error-free upload or high FPS does not replace that evidence. Preserve each port's approved graphics route.

## 1. Separate four decisions

Record the API serialized in data, available programs, logical operations requested by Unity and the physical context opened on the device. A facade can translate a required GLES3 subset to GLES2; it cannot advertise capabilities without implementing their effects.

For native GLES2 Unity, preserve supported variants and formats. If a build uses other programs, identify each stage, parameters, attributes, samplers, constants and incompatible operations. Compile and link, then compare actual scenes. [Freedom Planet 2](FP2-VULKAN-GLES2.md) demonstrates why Vulkan programs need a specific pipeline.

## 2. Measure resident textures

For each object, record file/container + object identity, dimensions, mips, original/upload format, consumer material, actual alpha, updates and lifetime. A texture name or isolated PathID is not sufficient identity across different files.

PNG or compressed bundles measure storage, not GPU memory. Observe creation, redefinition and deletion; cumulative uploads are not simultaneous residency. Separate CPU copies, temporary expansion, encoder/cache, GPU payload and driver overhead.

## 3. Select ETC1/dual by use

ETC1 stores RGB in 4×4 blocks using eight bytes per block; alpha requires another representation. This follows the [Khronos ETC1 specification](https://registry.khronos.org/OpenGL/extensions/OES/OES_compressed_ETC1_RGB8_texture.txt). In a dual strategy, a second compressed image carries alpha for shader reconstruction.

| Use | Initial decision |
| --- | --- |
| Truly opaque static color | Evaluate ETC1 RGB |
| Static color with blend/cutout | Evaluate dual with coherent shader, sampler and mips |
| Premultiplied color | Preserve RGB/alpha convention and blending |
| SDF font, normal, LUT or numerical mask | Specific analysis; do not classify as ordinary color |
| Render target, depth, video or dynamic updates | Outside automatic static conversion |
| Unknown use/alpha | Investigate before conversion |

ETC1 does not require reducing dimensions. Preserve atlases, rectangles, UVs, pivots and sprites. [Sally Face](../cases/sallyface.en.md) shows how cropped atlases can resemble a codec defect; [Horizon Chase](../cases/horizonchase.en.md) preserves existing RGB/alpha layers.

## 4. Calculate bytes without promising FPS

For each level, `ETC1 = ceil(w/4) × ceil(h/4) × 8`; sum every mip, including 2×2 and 1×1, which still occupy one block per plane. Dual uses two matching planes. Very small textures may save nothing.

At 1024×1024 with complete mips: RGBA8888 has 5,592,404 payload bytes; ETC1, 699,064; dual, 1,398,128. These are payload calculations, not measured RSS or total driver allocation. Use the [offline planner](../diagnostico/texturas/README.en.md) with a synthetic or local inventory.

## 5. Prove upload and shader together

Observe full definition, storage and subimage paths. Intercepting TexImage2D alone can allow a later upload to expand or overwrite the texture. An ETC2 enum cannot become ETC1 without proving block content. Keep format/storage/upload coherent.

For dual, validate RGB/alpha identity, dimensions, mips and sidecar ranges. Identical RGB can have different alpha; do not select the first RGB-hash match. Track GL-object redefinition and deletion. Measure available texture units and update every required consumer program.

Preserve shader UVs, filtering/wrap, premultiplication and cutout thresholds. Check edges, small text and distant mips. [Merchant of the Skies](../cases/merchantskies.en.md) illustrates an R8/alpha channel contract that must respect physical context and material.

## 6. Resolution and coordinates

Measure panel/drawable, window, render target, logical viewport and content rectangle separately. Reducing the window may not reduce RenderTextures. A framebuffer's virtual resolution may exceed the visible area.

For letterbox, transform pointer coordinates using content-rectangle offset and scale; handle clicks in bars according to the contract instead of inventing valid positions. Position and delta must share origin and scale. Check center, four corners, dragging and release after resize.

Do not impose a scaling policy on an approved port. [Oceanhorn](../cases/oceanhorn.en.md) preserves framing; [Prizefighters 2](../cases/pf2.en.md) needs attention to managed Mouse Y origin.

## 7. Finish the change

Convert owner data in a transactional stage with pinned tools and parameters. Compare untouched objects and reopen outputs. Then measure quality, memory, loading time and frame time in the same scene. Theoretical reduction does not authorize an FPS-gain claim.
