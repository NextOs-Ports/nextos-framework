# Terraria adapter environment. This file is sourced by the generated
# nxbootstrap launcher after NXExtract and the mandatory five-second splash.

export TER_GAMEDIR="$GAMEDIR"

# Keep the Android Unity 2021 lifecycle intact while avoiding its incremental
# collector deadlock on constrained Linux handhelds.
export GC_DISABLE_INCREMENTAL=${GC_DISABLE_INCREMENTAL:-1}
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}

# The public launcher owns log.txt; the loader must not install a second dup2
# target. Native InControl, the name-entry keyboard and FMOD bridge remain the
# proven Terraria paths.
export CUP_NOLOGFILE=1
export TER_NATPAD=${TER_NATPAD:-1}
export TER_OSK=${TER_OSK:-1}
export TER_VK_DEFAULT=${TER_VK_DEFAULT:-Player}
export TER_AUDIO=${TER_AUDIO:-1}
export TER_STREAMFALLBACK=${TER_STREAMFALLBACK:-1}

# Saves and owner data are deliberately outside the generated files so an
# update never replaces them.
mkdir -p "$GAMEDIR/userdata" "$GAMEDIR/Players" "$GAMEDIR/Worlds"

# Use Pulse only when the firmware exposes a real native socket. SDL still
# chooses the video and audio drivers; this adapter never pins either one.
for ter_pulse in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$ter_pulse" ]; then
    export PULSE_SERVER="unix:$ter_pulse"
    break
  fi
done
unset ter_pulse
