# First-accept seed

These two files are the minimal first-run acceptance state of The Amazing
Spider-Man 2 1.2.7d. They contain no game code and no game assets: they are
the settings records the game engine itself writes after the owner completes
the first-run sequence (legal notice, update log and cloud notice).

They were generated on 2026-08-02 on project-owned NextOS Mali-450 hardware
by completing the game's own first-run flow once and copying the resulting
records unchanged:

- `ud_Control.sav` (266 bytes,
  sha256 74cd086d52aad0f5f6a1e5ef5e84c2c892110eafc81493621c3f277ef7b58de9)
- `ud_OObjects.sav` (1586 bytes,
  sha256 ccc19cdf67b80d98944b764e30e155da12703e3a01279abd9b91ef766a1b8b8b)

At launch, `run.sh` copies each file into
`gamefiles/Android/data/com.gameloft.android.ANMP.GloftASHM/files/` only when
that file does not exist yet. Existing owner data is never overwritten. This
removes the fragile screen-coordinate recovery as the primary mechanism: the
first-run modals are touch-driven and their layout varies per panel aspect, so
handhelds without a touchscreen could get stuck on the legal notice. The
touch recovery in the loaders remains as a fallback for the case where the
seed cannot be written.

Physical validation (NextOS Mali-450, 1280x720): a clean install with only
these two files planted boots with no first-run modals, reaches the title
screen and starts the campaign from the beginning.
