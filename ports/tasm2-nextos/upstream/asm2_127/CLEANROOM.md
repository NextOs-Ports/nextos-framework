# ASM2 1.2.7d runtime clean-room boundary

This port is rebuilt from the following inputs only:

- the audited user-supplied Android 1.2.7d/1.2.8d containers that carry the
  exact supported native runtime;
- the matching user-supplied expansion files;
- generic, game-independent loader components in this repository;
- public platform documentation and conceptual lessons from completed ports.

The previous ASM2/TASM2 port implementation, TASMHD/SMHD work and all other
unfinished game ports are explicitly out of scope. Their source, generated
tables, JNI behavior, patches, offsets, logs, binaries and device files must
not be copied or consulted while developing this port. Completed, 100%-working
ports may contribute game-independent loader components or conceptual platform
lessons only; every ASM2-specific decision is re-derived from the exact
supported 1.2.7d native library. Input containers are accepted only by explicit
size and SHA-256 profiles, and every extracted native library must match the
expected SHA-256 before publication.

All game binaries and data are BYO and stay outside Git.
