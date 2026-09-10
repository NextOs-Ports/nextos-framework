# Notices

This public BYO-data interoperability project distributes no APK/XAPK, complete game
library, Unity game asset, raw decrypted runtime overlay, user save, receipt or
purchase state.

The four files under `tools/patches/` are version-pinned XOR transformation
masks. Each mask is useful only together with a byte range from the exact
owner-supplied Prizefighters 2 v1.09.3 ARM64 library. The installer validates
both the complete source-library SHA-256 and the reconstructed output SHA-256.
These masks are version-specific release artifacts and are not offered as game
data.

Prizefighters 2 and its original content are property of Koality Game and the
respective rightsholders. Unity is a trademark of Unity Technologies. This
project is unofficial and is not endorsed by Koality Game, Unity or Google.

Source code written for this port is licensed under GPL-3.0. Third-party
components keep their original licences, included in `licenses/`.
