# SPDX-License-Identifier: GPL-3.0-only
"""Exercise Android guide discovery and synthetic native-library detection."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


finder = load_tool('find_reference')
inventory = load_tool('inventory_apk')


class AndroidReferenceTests(unittest.TestCase):
    def test_default_search_excludes_other_source_platforms(self):
        catalog = json.loads((ROOT / 'catalog/ports.json').read_text())
        expected = {p['id'] for p in catalog['ports'] if p['platform'] == 'Android'}
        found = finder.search()
        self.assertEqual({p['id'] for p in found}, expected)
        self.assertTrue(all(p['platform'] == 'Android' for p in found))
        self.assertEqual({p['id'] for p in finder.search(platform='all')},
                         {p['id'] for p in catalog['ports']})
        self.assertTrue(all(not p['guides'] for p in finder.search(platform='GameCube')))

    def test_all_android_families_have_discoverable_source_cases(self):
        examples = {
            'unity': 'fp2-nextos', 'mono-android': 'blossomtales-nextos',
            'godot': 'tearscape-nextos', 'cocos2d-x': 'chrono-nextos',
            'gamemaker': 'forager-nextos', 'renpy': 'summertimesaga-nextos',
            'haxe-lime': 'tightrope-nextos', 'native-android': 'actionsquad-nextos',
        }
        for family, expected in examples.items():
            with self.subTest(family=family):
                matches = finder.search(runtime=family)
                self.assertIn(expected, {p['id'] for p in matches})
                self.assertTrue(all(p['runtime_family'] == family for p in matches))
                self.assertTrue(all(p['guides'] for p in matches))

    def test_abi_filter_does_not_invent_aarch64_support(self):
        self.assertEqual(finder.search(runtime='gamemaker', abi='arm64-v8a'), [])
        self.assertEqual([p['id'] for p in finder.search(runtime='gamemaker', abi='armeabi-v7a')],
                         ['forager-nextos'])

    def test_existing_engine_and_renderer_filters_still_combine(self):
        matches = finder.search(engine='uNiTy', abi='arm64-v8a', renderer='gLeS2')
        self.assertIn('fp2-nextos', {p['id'] for p in matches})
        self.assertTrue(all('Unity' in p['engine'] and 'arm64-v8a' in p['abis'] for p in matches))
        self.assertEqual(finder.search(engine='Unity', runtime='renpy'), [])

    def test_family_routing_keeps_unknown_engine_and_hardware_limits(self):
        native = {p['id']: p for p in finder.search(runtime='native-android')}
        self.assertEqual(native['actionsquad-nextos']['engine'], 'unknown')
        for p in finder.search():
            self.assertEqual(p['tested_devices'], [])
            self.assertEqual(p['build_status'], 'source-selection-not-built')
            self.assertEqual(p['hardware_status'], 'not-revalidated-in-this-collection')
        candidate = finder.search(runtime='haxe-lime')[0]
        self.assertIn('1.0.5-test.1', candidate['reference_notes'])
        self.assertIn('pending', candidate['reference_notes'])

    def test_classification_evidence_matches_immutable_source_maps(self):
        catalog = {p['id']: p for p in json.loads((ROOT / 'catalog/ports.json').read_text())['ports']}
        for p in finder.search(platform='all'):
            self.assertEqual(p['platform'], catalog[p['id']]['platform'])
            manifest = json.loads((ROOT / p['manifest']).read_text())
            pinned = {f['path']: f['sha256'] for f in manifest['included']}
            base = Path('ports') / p['id'] / 'upstream'
            for evidence in p.get('classification_evidence', []):
                path = Path(evidence['path'])
                relative = str(path.relative_to(base))
                self.assertEqual(evidence['sha256'], pinned[relative])
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(),
                                 evidence['sha256'])

    def test_registry_routes_to_registered_bilingual_guides(self):
        pairs = json.loads((ROOT / 'publication/languages.json').read_text())['pairs']
        tracks = finder.runtime_guides()
        ids = [r['id'] for r in tracks]
        self.assertEqual(len(ids), len(set(ids)))
        for track in tracks:
            self.assertIn(track['guides'], pairs)
            self.assertTrue(all((ROOT / path).is_file() for path in track['guides'].values()))
        self.assertTrue(all(p['runtime_family'] in ids for p in finder.search()))

    def test_cli_emits_guides_and_rejects_unknown_runtime(self):
        command = [sys.executable, str(ROOT / 'tools/find_reference.py'), '--runtime']
        result = subprocess.run(command + ['renpy'], check=True, capture_output=True, text=True)
        self.assertEqual(json.loads(result.stdout)[0]['guides']['pt-BR'],
                         'docs/pt-BR/RENPY-ANDROID.md')
        result = subprocess.run(command + ['typo'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        with self.assertRaises(ValueError):
            finder.search(runtime='typo')


@unittest.skipUnless(shutil.which('readelf'), 'requires readelf for synthetic ELF inventory')
class AndroidRuntimeInventoryTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'work/tests'
        scratch.mkdir(parents=True, exist_ok=True)
        area = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(area.cleanup)
        self.root = Path(area.name)

    def report(self, members):
        """Header-only original ELF fixtures identify names/ABIs, never execute."""
        path = self.root / 'input.apk'
        with zipfile.ZipFile(path, 'w') as apk:
            apk.writestr('AndroidManifest.xml',
                '<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                'package="org.nextos.training" android:versionCode="1" android:versionName="1.0"/>')
            for name in members:
                is_arm32 = name.startswith('lib/armeabi-v7a/')
                ident = b'\x7fELF' + bytes([1 if is_arm32 else 2, 1, 1]) + bytes(9)
                if is_arm32:
                    data = struct.pack('<16sHHIIIIIHHHHHH', ident, 3, 40, 1,
                                       0, 0, 0, 0x05000000, 52, 32, 0, 40, 0, 0)
                else:
                    data = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, 183, 1,
                                       0, 0, 0, 0, 64, 56, 0, 64, 0, 0)
                apk.writestr(name, data)
        return inventory.inventory([path], self.root / 'scratch')['containers'][0]

    def test_gamemaker_and_renpy_native_libraries_are_detected(self):
        for name, expected in [('libyoyo.so', 'GameMaker'), ('librenpython.so', "Ren'Py")]:
            with self.subTest(library=name):
                report = self.report(['lib/arm64-v8a/' + name])
                self.assertEqual(report['engine_hints'], [expected])
                self.assertEqual(report['preferred_abi'], 'arm64-v8a')

    def test_haxe_requires_paired_native_libraries_in_one_abi(self):
        report = self.report(['lib/arm64-v8a/liblime.so', 'lib/arm64-v8a/libApplicationMain.so'])
        self.assertEqual(report['engine_hints'], ['Haxe/hxcpp/Lime'])

    def test_cross_abi_pair_does_not_claim_hxcpp_application(self):
        report = self.report(['lib/arm64-v8a/liblime.so', 'lib/armeabi-v7a/libApplicationMain.so'])
        self.assertEqual(report['engine_hints'], ['Lime (Haxe family; hxcpp application unconfirmed)'])

    def test_lime_alone_keeps_application_unconfirmed(self):
        report = self.report(['lib/arm64-v8a/liblime.so'])
        self.assertEqual(report['engine_hints'], ['Lime (Haxe family; hxcpp application unconfirmed)'])

    def test_assets_and_backup_names_cannot_impersonate_runtimes(self):
        report = self.report(['assets/libyoyo.so', 'assets/librenpython.so', 'assets/libunity.so',
                              'assets/libgodot_android.so', 'assets/libcocos2dcpp.so',
                              'assets/liblime.so', 'assets/libApplicationMain.so',
                              'lib/arm64-v8a/libyoyo.so.backup'])
        self.assertEqual(report['engine_hints'], [])
        self.assertEqual(report['libraries'], [])

    def test_generic_libraries_do_not_establish_an_engine(self):
        report = self.report(['lib/arm64-v8a/libApplicationMain.so',
                              'lib/arm64-v8a/libpython3.11.so', 'lib/arm64-v8a/libcustom.so'])
        self.assertEqual(report['engine_hints'], [])

    def test_existing_runtime_hints_remain_available(self):
        for name, expected in [('libunity.so', 'Unity'), ('libil2cpp.so', 'Unity IL2CPP'),
                               ('libmonodroid.so', 'Mono Android'),
                               ('libgodot_android.so', 'Godot'), ('libcocos2dcpp.so', 'Cocos family')]:
            with self.subTest(library=name):
                self.assertEqual(self.report(['lib/arm64-v8a/' + name])['engine_hints'], [expected])


if __name__ == '__main__':
    unittest.main()
