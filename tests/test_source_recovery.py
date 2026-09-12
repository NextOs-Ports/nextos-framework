# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('recovery', ROOT / 'tools/recover_reference.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class SourceRecoveryTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'work/tests'
        scratch.mkdir(parents=True, exist_ok=True)
        area = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(area.cleanup)
        self.root = Path(area.name)
        self.destination = self.root / 'work/recovered'
        (self.root / 'catalog').mkdir()
        (self.root / 'catalog/ports.json').write_text(json.dumps({'ports': [{
            'id': 'training-nextos', 'repository': 'https://github.com/NextOs-Ports/training-nextos',
            'source_commit': '1' * 40, 'manifest': 'manifest.json'}]}))
        (self.root / 'manifest.json').write_text(json.dumps({'included': []}))
        patch = mock.patch.object(recovery, 'ROOT', self.root)
        patch.start()
        self.addCleanup(patch.stop)

    def recover(self, path, data=b'/* original synthetic source */\n'):
        with mock.patch.object(recovery.urllib.request, 'urlopen', return_value=io.BytesIO(data)) as download:
            with contextlib.redirect_stdout(io.StringIO()):
                recovery.main('training-nextos', path, self.destination)
        return download

    def test_url_preserves_literal_space_hash_and_question_mark(self):
        name = 'src/example #1?.c'
        download = self.recover(name)
        self.assertTrue(download.call_args.args[0].endswith('/src/example%20%231%3F.c'))
        target = self.destination / name
        record = json.loads(target.with_name(target.name + '.provenance.json').read_text())
        self.assertEqual(record['path'], name)
        self.assertEqual(record['sha256'], hashlib.sha256(target.read_bytes()).hexdigest())
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_pinned_download_must_match_recorded_bytes(self):
        record = {'path': 'guest.c', 'sha256': hashlib.sha256(b'expected').hexdigest()}
        (self.root / 'manifest.json').write_text(json.dumps({'included': [record]}))
        with self.assertRaisesRegex(ValueError, 'pinned source manifest'):
            self.recover('guest.c', b'wrong source')
        self.assertFalse(self.destination.exists())
        self.recover('guest.c', b'expected')
        self.assertEqual((self.destination / 'guest.c').read_bytes(), b'expected')

    def test_existing_provenance_is_never_overwritten(self):
        self.destination.mkdir(parents=True)
        proof = self.destination / 'guest.c.provenance.json'
        proof.write_text('previous evidence')
        with mock.patch.object(recovery.urllib.request, 'urlopen') as download:
            with self.assertRaisesRegex(ValueError, 'provenance already exists'):
                recovery.main('training-nextos', 'guest.c', self.destination)
            download.assert_not_called()
        self.assertEqual(proof.read_text(), 'previous evidence')
        self.assertFalse((self.destination / 'guest.c').exists())

    def test_noncanonical_paths_never_download(self):
        for path in ('../guest.c', '/guest.c', 'src//guest.c', 'src/./guest.c', 'src\\guest.c'):
            with self.subTest(path=path), mock.patch.object(recovery.urllib.request, 'urlopen') as download:
                with self.assertRaises(ValueError):
                    recovery.main('training-nextos', path, self.destination)
                download.assert_not_called()

    def test_destination_symlink_cannot_escape_work(self):
        self.destination.mkdir(parents=True)
        outside = self.root / 'outside'
        outside.mkdir()
        (self.destination / 'src').symlink_to(outside, target_is_directory=True)
        with mock.patch.object(recovery.urllib.request, 'urlopen') as download:
            with self.assertRaises(ValueError):
                recovery.main('training-nextos', 'src/guest.c', self.destination)
            download.assert_not_called()
        self.assertEqual(list(outside.iterdir()), [])

    def test_existing_source_is_never_overwritten(self):
        self.recover('guest.c')
        original = (self.destination / 'guest.c').read_bytes()
        with self.assertRaises(ValueError):
            self.recover('guest.c', b'new bytes')
        self.assertEqual((self.destination / 'guest.c').read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
