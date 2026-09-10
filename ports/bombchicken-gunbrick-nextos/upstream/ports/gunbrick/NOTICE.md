# Gunbrick Reloaded compatibility-port notices

The compatibility loader and packaging integration in this directory are part
of `nextos_ports_android`, Copyright 2026 NextOS Project contributors, and are
distributed under GNU General Public License version 3 only. The complete text
is in `LICENSE`.

The loader follows interoperability techniques developed in the free-software
Android native-porting community. Required acknowledgements include mtojek's
Apache-2.0 Android native loader work, initdream's Crazy Taxi work and
Producdevity's MIT-licensed Call of Duty: Black Ops Zombies work. Their
copyrights and licenses remain their own; this notice does not relicense those
upstream projects.

The loader uses the repository's universal `nxloader`, `nxcompat`, `nxinput`
and `nxaudio` components. NXExtract 1.2.6, including its canonical low-glibc
AArch64 setup UI, is redistributed unmodified under the MIT license; see
`licenses/NXExtract-MIT.txt`. SDL2, EGL, GLES and standard
system libraries are supplied by the target firmware and are not bundled.

Gunbrick Reloaded, its APK, Unity/IL2CPP libraries, scenes, videos, artwork,
music, sound effects, saves and all other owner data are proprietary works of
Nitrome or their respective rightsholders. They are not covered by this
project's license and are never present in the public package. Users must
provide files from their own lawful Android copy.

Physical validation used a user-supplied v10 payload whose useful files are
content-pinned by the installer. The APK itself is not redistributed and no
claim is made that it is an original store artifact. This independent
interoperability project is not affiliated with or endorsed by Nitrome, Unity
Technologies, Google or another rightsholder.
