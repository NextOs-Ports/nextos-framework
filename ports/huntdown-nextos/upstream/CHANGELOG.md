# Changelog

## 1.0.7 — 2026-08-31

- Replaced independent size guesses with one display resolver shared by the
  window, EGL, JNI, video and render-scale paths. It prefers the launcher's
  visible-display contract, then connected DRM geometry and finally visible
  framebuffer geometry, never the double-buffered virtual height.
- Made the two authored opening movies presentation-free by default while
  preserving Huntdown's native Unity `loopPointReached` callbacks and scene
  order. External playback remains an explicit diagnostic opt-in.
- Pinned the release to the immutable `framework-v3` baseline: nxbootstrap
  0.6.37, NXExtract 1.2.21, nxgenerator 0.2.20 and NXRelease 0.2.43.
- Physically validated the exact release executable at 1280×720 on two NextOS
  Mali-450/fbdev devices, including visible geometry, audio stream, controller
  discovery and both opening-video callbacks.
- Supersedes the unpublished 1.0.6 candidate.

## 1.0.5 — 2026-08-26

- Fixed Unity 6 on legacy Mali-450/fbdev after the first correct logo by
  negotiating a bounded GLES3-to-GLES2 EGL contract only when native ES3 is
  rejected.
- Preferred the measured GLES3 emulation table over unusable Utgard dispatch
  thunks and translated Unity 6 pixel-store and sized-texture uploads at the
  physical GLES2 boundary.
- Preserved native ES3 on capable fbdev/KMS/PowerVR paths and kept the
  framework, extraction UI and mandatory five-second NXSplash unchanged.
- Prevented controller rescans from repeatedly applying the same legacy
  ordinal correction to an already-open device.
- Physically validated the Unity 6 profile through both authored videos and a
  stable title/menu image on NextOS Mali-450/fbdev; audio and controller
  discovery were also confirmed.

## 1.0.4 — 2026-08-26

- Added a second correlated owner-data profile for Huntdown version code
  `200036`, built with Unity `6000.2.6f2`, while retaining the validated
  `200023` / Unity `2022.3.47f1` profile.
- Reproduced the Unity 6 `NativeLoader`, Choreographer/HandlerThread, AAudio,
  GLES3-on-GLES2 and bounded shader paths without changing the native game
  sequence.
- Fixed PowerVR handhelds whose SDL backend is labelled `mali` by selecting
  graphics ownership from the real GL vendor and renderer.
- Normalized controller semantics across firmware mappings and added the
  canonical legacy ordinal correction only for external USB/Bluetooth
  `BTN_C`/`BTN_Z` controllers. Internal controls and explicit SDL or
  `HUNTDOWN_PAD_MAP` mappings remain authoritative.
- Updated the BYO-data recipe to accept APK, split APK sets, APKM, APKS, XAPK
  and harmlessly repacked containers by internal package/ABI/payload identity,
  never by a single whole-container hash.
- Updated the generated public stack to nxbootstrap 0.6.30, NXExtract 1.2.18,
  nxgenerator 0.2.12 and NXRelease 0.2.30. The canonical five-second NXSplash
  remains unchanged.
