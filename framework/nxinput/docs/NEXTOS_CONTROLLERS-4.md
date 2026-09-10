# NEXTOS_CONTROLLERS/4 — owner file contract (nxinput 0.11.0, V5 controls)

Owner: `<port>/NEXTOSCONTROLLERS.gptk`. Model: `<port>/defaults/NEXTOSCONTROLLERS.gptk`
(first materialization only; updates offer `.new` + diff, never overwrite the owner).
Parser: `include/nxinput_gptk4.h` / `src/nxinput_gptk4.c`. Fixture: `tests/v5/corpus/fp2-complete.gptk`.

## Header (all required)
```
format = NEXTOS_CONTROLLERS/4
port = <id>
CONTROL_STANDARD = xbox          # positional aliases; the only value
GLYPH_STYLE = xbox               # xbox | nintendo | playstation (drawing only)
AUTHORITY = nextos | engine | synchronized
CONTEXT_POLICY = unified
```

## Vocabulary (positional; the plastic legend is evidence only)
| control | alias | position |
|---|---|---|
| FACE_SOUTH | `A` | south |
| FACE_EAST | `B` | east |
| FACE_WEST | `X` | west |
| FACE_NORTH | `Y` | north |

Core set (every one must appear): `A B X Y L1 R1 L3 R3 START SELECT GUIDE UP DOWN LEFT RIGHT`
in `[base]`; `[stick.left]`/`[stick.right]` own `mode`, `vector`, `up/down/left/right`;
`[trigger.left]`/`[trigger.right]` own `mode`, `analog`, `digital`. Duplicates across
sections and omissions are errors (`NXI4003`, `NXI4001`). Nothing becomes `null` silently.

## Grammar (typed)
```
digital_binding := native | null | action:<id> | action:<id>@key:<key_chord>
analog_binding  := native | null | action:<id>
vector2_binding := native | null | action:<id>
key_chord       := [MODIFIER '+']* KEYSYM     (serialized LCTRL/RCTRL,LALT/RALT,LSHIFT/RSHIFT,LGUI/RGUI+KEYSYM)
```
- `action:<id>` must be declared by the adapter contract with a `value_kind`
  (`digital|scalar|vector2`); a mismatch is `NXI4005`.
- `@key:` needs a proved keyboard backend (`NXI4009` otherwise); bare `key:` is invalid.
- `native` is invalid on derived directions (`NXI4006`): use `vector = native`.
- `[override.<ctx>]` is sparse and only for contexts the adapter declares; an override that
  repeats the base without divergence is refused (`NXI4003`). Unknown context = base passthrough.

## Sticks (6.5)
`mode = vector | digital | split`. `vector` requires the four directions `null`; `digital`
requires `vector = null`; `split` requires `split_policy = zones`,
`direction_enter/exit_threshold`, `digital_enter/exit_threshold`, an action vector AND action
directions on two distinct families, and `direction_enter <= digital_exit/sqrt(2)`.
`enter_threshold`/`exit_threshold` (default 0.55/0.40), `diagonal = 8way | 4way-dominant`,
`tie_break = horizontal | vertical`. Directions are derived after calibration, exact zero and
the radial deadzone (`nxinput_axis_calib`).

## Triggers
`mode = analog | digital`; `analog` (scalar, `[0,1]`) and `digital` are exclusive: the other
channel must be `null`. Thresholds `0 <= exit < enter <= 1`. No implicit split.

## Extensions (E4a, capability-gated)
Physical buttons beyond the core set (MISC, paddles, touchpad click, …) exist only when the
adapter declares them from the provider/capability descriptor, under the stable namespace
`EXT.` + `[A-Z0-9_]+` (`EXT.MISC1`, `EXT.PADDLE1`, `EXT.TOUCHPAD`). They are bound in `[base]`
with the digital grammar. A declared extension omitted from the owner is `NXI4001`; an `EXT.*`
key the adapter did not declare is `NXI4004` (no improvised public names). The generated
default lists every declared extension explicitly as `null` inside `[base]` so the owner sees
every bindable control; a core control the device lacks stays in the schema and the runtime
registry reports it `unavailable` (never a second owner file per device).

## Keyboard as a source
```
[keyboard.base]
SPACE = action:player.jump
LCTRL+S = action:game.save
[keyboard.override.menu]
ESCAPE = action:menu.cancel
```
Keys are symbols from the allowlist; a physical key bound to an action that also carries
`@key` is refused as a loop.

## Resolution
1. merge `base` with the active overlay for the control → exactly one binding;
2. if `action:<family>`, the adapter projects the family for the active context
   (`native`/`null` stop at step 1). Context switch releases the old route, bumps the
   context epoch and inhibits the new binding until the control returns to neutral.

## Errors
`NXI4000` magic, `4001` omitted, `4002` malformed, `4003` duplicate, `4004` unknown,
`4005` kind, `4006` native on derived, `4007` mode, `4008` threshold, `4009` keyboard,
`4010` too large, `4011` header, `4012` mixed-owner stick — with line and column; the owner
text is never rewritten and the last valid generation stays active.

## Migration
V1–V3 owners are read-only under their own parsers; nothing is converted silently. Only the
opt-in successor port materializes schema 4 from `nxinput_gptk4_default()` (single generator
source; a checked-in default must be byte-equal to it).


## 0.11.1 notes
- `AUTHORITY` is executed by `nxinput_authority_v5` (E3): `nextos` = the editable owner file is
  the authority (a `native` binding delegates only that edge); `engine` = there is NO owner GPTK
  presented as editable — the port exposes a readback mirror named `<port>.engine-readback.mirror`
  and prompts come from the engine readback; `synchronized` = both sides kept equal by the CAS
  transaction and REQUIRES the engine hooks (apply/readback/rollback/owner CAS) — without them
  the election fails closed and the port stays native (never a silent `nextos`).
- `[keyboard.*]` loops are refused across sections too: a chord that feeds an action whose
  gamepad binding emits `@key` of that same chord is NXI4009 (the 0.11.0 parser only caught the
  in-line case). The runtime keyboard source (`nxinput_keyboard`) refuses it again.
- The vector gesture of `[stick.*] mode = vector` has a NEUTRAL FLOOR in the runtime
  (`nxinput_gptk_live`, default 1/64, adapter may raise it to its deadzone): a pad whose rest
  normalizes to +0.0039 never opens a gesture by itself.
