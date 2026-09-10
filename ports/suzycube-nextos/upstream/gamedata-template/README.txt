Place a compatible, lawfully acquired Suzy Cube ARM64 Android APK here.

NXExtract checks the package name, both native ABIs, the three native libraries
and the full asset tree before installing anything. Suzy Cube ships its data in
two pieces -- part inside the APK and part inside an OBB -- and both arrive cut
into `.splitN` chunks; the installer rejoins them in numeric order and refuses a
tree with a missing chunk instead of writing a truncated file. If your copy came
with a separate `main.<n>.com.noodlecake.suzycube.obb`, drop it here too.

Unsupported, incomplete and wrong-package APKs are rejected. The public ZIP
never contains game data.
