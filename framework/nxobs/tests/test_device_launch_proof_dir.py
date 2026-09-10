#!/usr/bin/env python3
"""Directed gate for nx-device-launch's private frame-proof directory."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "nx-device-launch.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


source = LAUNCHER.read_text(encoding="utf-8")

# Exercise the exact remote creation fragment carried by the production
# launcher. Keeping this extraction strict makes a weakening mutation fail the
# gate instead of testing a hand-written approximation.
match = re.search(
    r'^if ! ssh_device "(umask 077 && mkdir \'\$PROOF_DIR\')"; then$',
    source,
    flags=re.MULTILINE,
)
require(match is not None, "exclusive umask+mkdir production fragment missing")
command_template = match.group(1)

require("mkdir -p '$PROOF_DIR'" not in source, "proof directory must never use mkdir -p")
require("chmod 700 '$PROOF_DIR'" not in source, "post-create chmod race returned")
require(
    re.search(r"(^|[;&|()\s])stat(\s|$)", source) is None,
    "launcher must not depend on external stat",
)

create_pos = source.index(match.group(0))
owned_pos = source.index("PROOF_DIR_OWNED=1", create_pos)
launch_pos = source.index('LAUNCH_CMD="cd ', owned_pos)
require(create_pos < owned_pos < launch_pos, "ownership may be marked only after exclusive creation")
require(
    'if [ "$PROOF_DIR_OWNED" = 1 ]; then' in source,
    "cleanup must be limited to the directory this invocation created",
)


def create_command(path: Path) -> str:
    return command_template.replace("'$PROOF_DIR'", shlex.quote(str(path)))


with tempfile.TemporaryDirectory(prefix="nxobs-proof-dir-") as temp:
    root = Path(temp)

    fresh = root / "fresh"
    result = subprocess.run(
        ["/bin/sh", "-c", create_command(fresh)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    require(result.returncode == 0, f"fresh creation failed: {result.stderr}")
    info = fresh.lstat()
    require(stat.S_ISDIR(info.st_mode), "fresh proof path is not a directory")
    require(stat.S_IMODE(info.st_mode) == 0o700, "fresh proof directory is not mode 0700")
    require(info.st_uid == os.getuid(), "fresh proof directory is not owned by the caller")

    existing = root / "existing"
    existing.mkdir(mode=0o700)
    sentinel = existing / "keep"
    sentinel.write_text("preserve", encoding="ascii")
    result = subprocess.run(
        ["/bin/sh", "-c", create_command(existing)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    require(result.returncode != 0, "pre-existing directory was accepted")
    require(sentinel.read_text(encoding="ascii") == "preserve", "pre-existing directory was changed")

    target = root / "target"
    target.mkdir(mode=0o700)
    target_sentinel = target / "keep"
    target_sentinel.write_text("preserve", encoding="ascii")
    link = root / "link"
    link.symlink_to(target, target_is_directory=True)
    result = subprocess.run(
        ["/bin/sh", "-c", create_command(link)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    require(result.returncode != 0, "pre-existing symlink was accepted")
    require(link.is_symlink(), "pre-existing symlink was removed")
    require(target_sentinel.read_text(encoding="ascii") == "preserve", "symlink target was changed")

print("nxobs-device-launch-proof-dir: PASS (0700, exclusive, collision/symlink fail-closed)")
