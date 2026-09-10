#!/usr/bin/env python3
"""Exercise and lock Party Hard's run-bound nxbootstrap health boundary."""
from pathlib import Path
import json
import os
import stat
import subprocess
import tempfile

port = Path(__file__).resolve().parents[1]
health = (port / "src/health.c").read_text()
sdl = (port / "src/egl_sdl.c").read_text()
raw = (port / "src/egl.c").read_text()

for token in (
    'getenv("NXBOOTSTRAP_HEALTH_FILE")',
    'getenv("NXBOOTSTRAP_HEALTH_RUN_ID")',
    'getenv("NXBOOTSTRAP_HEALTH_GENERATION")',
    'getenv("NXBOOTSTRAP_HEALTH_PORT_ID")',
    "O_EXCL | O_NOFOLLOW | O_CLOEXEC",
    '"status\\\":\\\"ready\\\"}\\n"',
    "fchmod(fd, 0600)",
    "fsync(fd)",
    "rename(temporary, file)",
):
    assert token in health, f"health receipt contract missing: {token}"

assert "frame_count >= 30" in sdl
assert "SDL_GL_SwapWindow(video_window);" in sdl
assert sdl.index("SDL_GL_SwapWindow(video_window);") < sdl.index(
    "st_health_publish_once();"
), "SDL health must follow a real page flip"

assert "presented == EGL_TRUE && st_swap_count >= 30" in raw
assert raw.index("EGLBoolean presented = p_eglSwapBuffers") < raw.index(
    "st_health_publish_once();"
), "raw-EGL health must follow a successful real present"

fixture = r'''
#include <stdlib.h>
void st_health_publish_once(void);
int main(void) {
    st_health_publish_once();
    st_health_publish_once();
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="partyhard-health-",
                                 dir=os.getenv("TMPDIR")) as temporary:
    root = Path(temporary)
    source = root / "test.c"
    binary = root / "test"
    receipt = root / "health.json"
    source.write_text(fixture)
    subprocess.run([
        os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        str(source), str(port / "src/health.c"), "-o", str(binary),
    ], check=True)

    environment = os.environ.copy()
    environment.update({
        "NXBOOTSTRAP_HEALTH_FILE": str(receipt),
        "NXBOOTSTRAP_HEALTH_SCHEMA": "org.nextos.nxruntime.health",
        "NXBOOTSTRAP_HEALTH_SCHEMA_VERSION": "1",
        "NXBOOTSTRAP_HEALTH_RUN_ID": "partyhard-health-test",
        "NXBOOTSTRAP_HEALTH_GENERATION": "a" * 64,
        "NXBOOTSTRAP_HEALTH_PORT_ID": "partyhard",
    })
    completed = subprocess.run([str(binary)], env=environment, check=True,
                               text=True, capture_output=True)
    assert completed.stderr.count("receipt ready published") == 1
    assert stat.S_IMODE(receipt.stat().st_mode) == 0o600
    assert receipt.stat().st_nlink == 1
    assert receipt.read_text().count("\n") == 1
    assert json.loads(receipt.read_text()) == {
        "schema": "org.nextos.nxruntime.health",
        "schema_version": 1,
        "run_id": "partyhard-health-test",
        "generation": "a" * 64,
        "port_id": "partyhard",
        "status": "ready",
    }

print("PASS: health receipt is exact, atomic and gated behind real presents")
