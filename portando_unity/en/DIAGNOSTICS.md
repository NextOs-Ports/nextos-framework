# Unity boundary diagnostics

[Português](../pt-BR/DIAGNOSTICO.md)

Record **symptom → measurement → cause → repair → countercheck → limitation**. A file named “fix” does not authorize enabling that behavior in every game.

| Symptom | Discriminating measurement | Selected reference |
| --- | --- | --- |
| No GL calls | Bootstrap, nativeRender and EGL resolution | Terraria |
| Black after a few frames | Present, timestamps and Swappy selection | Huntdown, 2022 profile |
| Correct RGB, black scanout | Final alpha and compositor; before/after state | Horizon Chase, Hitman GO |
| Sprite with colored rectangle | Swizzle, alpha and premultiplication | Freedom Planet 2 |
| Block-shaped text | R8/SDF channel and physical format | Merchant of the Skies |
| Magenta rain, otherwise correct | Scene programs beyond global programs | Sally Face |
| Cropped scenery/characters | Complete atlas and Sprite rectangles | Sally Face |
| Live engine, level does not start | JNI/managed exception and complete data | Bomb Chicken |
| Live context, image lost after replacing SDL | Actual provider and library bytes | Nameless Cat |
| Stuck direction | Duplicate KeyEvent/HAT, focus and release | Party Hard GO |
| Misplaced click | Managed Mouse coordinate space and Y delta | Prizefighters 2 |
| Accelerated sound | fmodGetInfo, SDL rate and bytes per frame | Terraria |
| Mixer does not start | FMODAudioDevice thread and actually available library | Sally Face, Merchant of the Skies |

Cards and sources are linked from the [index](../README.en.md). This table identifies case-supported hypotheses; it does not establish the same cause in a new game.

## A diagnostic run

Define the comparison result, executable/SHA, input, scene, settings and observation window. Add only necessary instrumentation; record the observer at each boundary. Do not treat a black DRM capture as proof of the game image or a PID as a frame counter.

Where possible, compare pre-present pixels and the actual screen. Keep probes outside default release behavior. End one instance before launching another and use only the task-authorized device.

## Included tools

The [texture planner](../diagnostico/texturas/README.en.md) calculates potential payload from an inventory; it does not convert assets or measure FPS. The [touch checker](../diagnostico/toque/README.en.md) analyzes explicit sequences; it neither injects events nor measures the consumer by itself.

Only generic tools and synthetic examples were included. Galleries, session-bound collectors, reports and sources from excluded games were not imported.

## Close a hypothesis

If measurement contradicts the hypothesis, remove the experiment from the new adapter and record the result. If confirmed, implement the minimal repair and countercheck, then test the previous scene and next transition. Do not delete saves, change data or rebuild an approved binary to manufacture a passing result.

A partial finding remains partial. Mark any gap between historical source and approved artifact; this edition does not manufacture new physical certification.
