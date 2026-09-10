# Suzy Cube compatibility-port notices

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

The port carries its own ELF loader, Bionic shim and JNI bridge, and uses the
repository's universal `nxcompat`, `nxinput` and `nxaudio` components for the
host contract. NXExtract engine 1.2.10, its immutable graphical UI release
1.2.9 and NXSplash 0.1.2 are redistributed unmodified under their respective licenses; see
`licenses/NXExtract-MIT.txt`. SDL2, EGL, GLES and standard system libraries are
supplied by the target firmware and are not bundled.

Suzy Cube, its APK, OBB, Unity/IL2CPP libraries, scenes, artwork, music, sound
effects, saves and all other owner data are proprietary works of Noodlecake
Studios, Louard Games or their respective rightsholders. They are not covered
by this project's license and are never present in the public package. Users
must provide files from their own lawful Android copy.

Physical validation used a user-supplied v1.0.13 payload whose useful files are
content-validated by the installer. The APK itself is not redistributed. This independent
interoperability project is not affiliated with or endorsed by Noodlecake
Studios, Louard Games, Unity Technologies or another rightsholder.
