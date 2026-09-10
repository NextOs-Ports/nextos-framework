#!/bin/sh
systemctl stop emustation 2>/dev/null; pkill -9 emulationstatio 2>/dev/null; pkill -9 sor4host 2>/dev/null; sleep 2
cd /storage/roms/sor4-test/host_pkg
mkdir -p /storage/roms/sor4-test/texcache
export LD_LIBRARY_PATH=$PWD/libs:/usr/lib SDL_VIDEODRIVER=mali DOTNET_EnableWriteXorExecute=0
export SOR4_ASSETS=/storage/roms/sor4-test/gameassets SOR4_TEXSCALE=2 SOR4_SHOT=120
export SOR4_TEXCACHE=/storage/roms/sor4-test/texcache
exec ./sor4host
