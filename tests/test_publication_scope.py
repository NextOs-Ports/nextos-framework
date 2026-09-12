# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('publication_verify', ROOT / 'publication/verify.py')
publication = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publication)


class PublicationScopeTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'work/tests'
        scratch.mkdir(parents=True, exist_ok=True)
        self.area = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.area.cleanup)
        self.root = Path(self.area.name)

    def test_rejects_ios_source_even_without_platform_in_name(self):
        catalog = {'ports': [{'id': 'training-nextos', 'platform': 'iOS'}]}
        self.assertTrue(publication.port_scope_errors(catalog, self.root))

    def test_rejects_community_ios_reference(self):
        catalog = {'ports': [], 'community_ports': [
            {'id': 'training-nextos', 'platform': 'iPadOS'}]}
        self.assertTrue(publication.port_scope_errors(catalog, self.root))

    def test_rejects_unlisted_source_directory(self):
        (self.root / 'ports/training-ios-nextos').mkdir(parents=True)
        self.assertTrue(publication.port_scope_errors({'ports': []}, self.root))

    def test_keeps_android_and_gamecube_sources(self):
        catalog = {'ports': [
            {'id': 'android-training', 'platform': 'Android'},
            {'id': 'gamecube-training', 'platform': 'GameCube'}]}
        for port in catalog['ports']:
            (self.root / 'ports' / port['id']).mkdir(parents=True)
        self.assertEqual(publication.port_scope_errors(catalog, self.root), [])

    def frozen_selection(self):
        path = self.root / 'runtime/launcher.sh'
        path.parent.mkdir()
        path.write_bytes(b'#!/bin/sh\nexit 0\n')
        path.chmod(0o755)
        records = [{'path': 'runtime/launcher.sh', 'mode': '100755',
                    'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}]
        return path, records

    def test_accepts_exact_frozen_selection(self):
        _, records = self.frozen_selection()
        self.assertEqual(publication.selection_errors(self.root, records, 'fixture'), [])

    def test_rejects_runtime_added_outside_manifest(self):
        _, records = self.frozen_selection()
        (self.root / 'runtime/new-behavior.py').write_text('print("unapproved")\n')
        errors = publication.selection_errors(self.root, records, 'fixture')
        self.assertTrue(any('unlisted file' in error for error in errors), errors)

    def test_rejects_replaced_runtime(self):
        path, records = self.frozen_selection()
        path.write_bytes(b'#!/bin/sh\nexit 1\n')
        self.assertTrue(publication.selection_errors(self.root, records, 'fixture'))

    def test_rejects_runtime_losing_executable_mode(self):
        path, records = self.frozen_selection()
        path.chmod(0o644)
        errors = publication.selection_errors(self.root, records, 'fixture')
        self.assertTrue(any('executable mode' in error for error in errors), errors)

    def test_rejects_symlink_to_identical_bytes(self):
        path, records = self.frozen_selection()
        target = path.with_name('target.sh')
        path.rename(target)
        path.symlink_to(target)
        self.assertTrue(publication.selection_errors(self.root, records, 'fixture'))

    def test_rejects_directory_symlink(self):
        _, records = self.frozen_selection()
        original = self.root / 'runtime'
        target = self.root / 'moved'
        original.rename(target)
        original.symlink_to(target, target_is_directory=True)
        self.assertTrue(publication.selection_errors(
            self.root, records, 'fixture', ('runtime',)))

    def test_rejects_duplicate_manifest_entry(self):
        _, records = self.frozen_selection()
        self.assertTrue(publication.selection_errors(self.root, records * 2, 'fixture'))

    def test_rejects_manifest_path_outside_selection(self):
        _, records = self.frozen_selection()
        for name in ('../outside.sh', '/outside.sh', 'runtime/../launcher.sh',
                     'runtime//launcher.sh', 'runtime/./launcher.sh', 'different/file.sh'):
            with self.subTest(path=name):
                bad = [dict(records[0], path=name)]
                self.assertTrue(publication.selection_errors(
                    self.root, bad, 'fixture', ('runtime',)))

    def test_manifest_cannot_self_authorize_runtime_change(self):
        raw = (ROOT / 'publication/v5-export.json').read_bytes()
        self.assertEqual(publication.baseline_errors(raw), [])
        changed = json.loads(raw)
        changed['included'][0]['sha256'] = '0' * 64
        self.assertTrue(publication.baseline_errors(json.dumps(changed).encode()))

    def test_git_index_cannot_publish_ignored_work_or_inputs(self):
        for name in ('work/notes.md', 'inputs/game.apk', 'build/program',
                     'tools/__pycache__/cache.pyc', '.env', 'config/.env.local'):
            with self.subTest(path=name):
                self.assertTrue(publication.tracking_errors([name]))
        self.assertEqual(publication.tracking_errors(['tools/inventory_apk.py']), [])

    def test_nested_work_directory_is_not_exempt_from_freeze(self):
        _, records = self.frozen_selection()
        extra = self.root / 'runtime/work/hidden.py'
        extra.parent.mkdir()
        extra.write_text('unlisted\n')
        self.assertTrue(publication.selection_errors(self.root, records, 'fixture'))

    def test_untracked_python_cache_does_not_change_selection(self):
        _, records = self.frozen_selection()
        cache = self.root / 'runtime/__pycache__/generated.pyc'
        cache.parent.mkdir()
        cache.write_bytes(b'generated by Python')
        self.assertEqual(publication.selection_errors(self.root, records, 'fixture'), [])

    def test_local_work_and_git_worktree_pointer_are_not_scanned(self):
        (self.root / '.git').write_text('gitdir: private local location\n')
        (self.root / 'work').mkdir()
        (self.root / 'work/data.apk').write_bytes(b'private input')
        self.assertEqual(list(publication.source_files(self.root, publication.PRIVATE_TREES)), [])


if __name__ == '__main__':
    unittest.main()
