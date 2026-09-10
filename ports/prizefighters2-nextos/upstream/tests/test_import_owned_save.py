#!/usr/bin/env python3
"""Small end-to-end test for the owner-save importer."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
IMPORTER = REPOSITORY / "tools" / "import_owned_android_save.py"
FIXTURE = REPOSITORY / "tests" / "fixtures" / "android-save"


def run(*arguments):
    return subprocess.run(
        [sys.executable, str(IMPORTER)] + list(arguments),
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    ).stdout


def main():
    with tempfile.TemporaryDirectory(prefix="pf2-import-test-") as temporary:
        game_dir = Path(temporary) / "pf2"
        game_dir.mkdir()
        dry_output = run(
            "--source", str(FIXTURE), "--game-dir", str(game_dir), "--dry-run"
        )
        assert "device-local skipped: 2" in dry_output
        assert not (game_dir / "home").exists()

        output = run("--source", str(FIXTURE), "--game-dir", str(game_dir))
        assert "import complete" in output
        preferences = game_dir / "home" / "shared-preferences.bin"
        career = game_dir / "home" / "Career Saves" / "CAREER_SAVE_FILE_TEST"
        assert preferences.read_bytes().startswith(b"PF2PREF1")
        assert career.read_text(encoding="utf-8").startswith("PF2 importer")

        sys.path.insert(0, str(REPOSITORY / "tools"))
        import import_owned_android_save as importer

        entries = importer.decode_pf2_preferences(preferences)
        values = {key: value for key, _value_type, value in entries}
        assert values["tutorialComplete"] is True
        assert values["owned-save-test"] == "owner-value"
        assert "Screenmanager%20Resolution%20Width" not in values
        assert "Screenmanager%20Resolution%20Height" not in values
    print("owner-save importer test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
