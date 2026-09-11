# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
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


if __name__ == '__main__':
    unittest.main()
