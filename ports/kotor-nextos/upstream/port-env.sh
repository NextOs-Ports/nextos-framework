# Star Wars KOTOR — port adapter sourced by the generated launcher.
# May export environment and replace $BIN / $BIN_PRELOAD. Nothing else.

# One public low-glibc runtime on every supported firmware. Input
# normalization is linked into this executable; no preload or host-version
# branch may select a different control path.
BIN="$GAMEDIR/kotor-nextos"

export KOTOR_HIGH_RES="${KOTOR_HIGH_RES:-1}"
# The Android build asks for Java AudioTrack, absent here: the loader wires
# FMOD's native OpenSL backend into the SDL bridge.
export KOTOR_FMOD_OPENSL="${KOTOR_FMOD_OPENSL:-1}"
export KOTOR_INPUT_REMAP="${KOTOR_INPUT_REMAP:-1}"
export KOTOR_INPUT_DYNAMIC="${KOTOR_INPUT_DYNAMIC:-1}"
export KOTOR_INPUT_EVDEV="${KOTOR_INPUT_EVDEV:-1}"

# Known external pads appended after the CFW's own mapping.
KOTOR_CONTROLLER_MAPPINGS="03000000100800000100000010010000,Twin PS2 Adapter,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b6,leftstick:b10,lefttrigger:b4,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b11,righttrigger:b5,rightx:a3,righty:a2,start:b9,x:b3,y:b0,platform:Linux,
0300605b100800000100000010010000,USB Gamepad,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b6,leftstick:b10,lefttrigger:b4,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b11,righttrigger:b5,rightx:a3,righty:a2,start:b9,x:b3,y:b0,platform:Linux,"
if [ -n "$sdl_controllerconfig" ]; then
  export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig}
${KOTOR_CONTROLLER_MAPPINGS}"
else
  export SDL_GAMECONTROLLERCONFIG="$KOTOR_CONTROLLER_MAPPINGS"
fi
