# Unity: input, audio and lifecycle

[Português](../pt-BR/INPUT-E-AUDIO.md)

SDL discovers devices and delivers events; the engine must consume them through the build's contract. A filled queue does not establish that a button reached an action. The same distinction applies to produced PCM and audible sound.

## 1. Find the input consumer

Identify InControl, Rewired, Input System, custom IL2CPP methods or native touch. Record enums, signatures, device ID, timing window and consumer thread. Normalize the gamepad using firmware/PortMaster before game semantic actions.

| Reference | Contract worth studying |
| --- | --- |
| [Suzy Cube](../cases/suzycube.en.md) | InControl and axes already transformed by SDL |
| [Oceanhorn](../cases/oceanhorn.en.md) | Rewired through KeyEvent/MotionEvent |
| [Prizefighters 2](../cases/pf2.en.md) | Managed Input System, StateEvent and contextual Mouse |
| [Huntdown](../cases/huntdown.en.md) | Profile-specific GamePad methods |
| [Party Hard GO](../cases/partyhard.en.md) | Single D-pad authority, context/focus release |
| [Nameless Cat](../cases/namelesscat.en.md) | Canonical admission before SDL_Init |

Do not copy game keycodes as a universal table. One physical A must not simultaneously confirm and cancel. Menu/gameplay transitions must release held states and preserve player identity.

## 2. Preserve short clicks

DOWN and UP can occur between two native frames. Keeping only the final state loses the click. Observe SDL origin, Android delivery and game consumer over complete windows including at least the frame following the event.

Use the [short-press analyzer](../diagnostico/toque/README.en.md) to detect loss, duplication, missing release and insufficient coverage in explicit traces. The analyzer neither injects events nor turns an incomplete log into proof of a game defect.

Implement queues/latches according to the consumer API, preserving order, duration and ownership. Test quick clicks, holds, drags, releases, cancellation, focus loss and context changes. Adding extra DOWN events to “ensure” clicks can create duplicates.

## 3. Contextual cursor

When the interface needs touch, use a clear arrow and continuous right-stick movement with radial deadzone, progressive response and per-frame timing. R3 clicks in menu context; gameplay preserves native actions/camera. Do not take over the D-pad or primary button for cursor movement without demonstrated need.

Transform coordinates using the actual content rectangle; convert Y position and delta coherently. A cursor drawn in the right place can deliver a click in the wrong place when the sink uses another coordinate space.

## 4. Reconstruct actual audio

Record backend, objects/vtables, init/start/pause/stop, callbacks, sample rate, channels, format and frame count. FMOD remains the mixer; the adapter transports PCM through OpenSL, AudioTrack, AAudio or the actual interface used.

Do not fake successful opening of a missing library. Do not choose a “typical” sample rate based on Unity version. Rate mismatch speeds up or slows down sound; confusing bytes and frames can cause underruns, repetition or excessive mixer advancement.

[Sally Face](../cases/sallyface.en.md) requires the FMODAudioDevice.run thread; [Terraria](../cases/terraria.en.md) ties fmodGetInfo to DirectByteBuffer; [Merchant of the Skies](../cases/merchantskies.en.md) shows why falsely available AAudio leaves the wrong path active.

## 5. Validate the complete lifecycle

Test focus, pause/resume, scene changes, reconnect, preferences, save/reload and exit. SELECT+START must exit through the approved flow, with persistence before a terminal deadline where necessary. Do not use `_exit` to skip saving or initialization.

Queue logs, JNI calls or PCM amplitude alone do not establish a complete experience. Bind consumed events, audible sound, image and exit to the same executable/data profile. Preserve references and V5 while working on the new adapter.
