# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic inputs discriminate ABI/split errors without commercial data."""
import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('inventory', ROOT / 'tools/inventory_apk.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


def elf_header(elf_class=2, machine=183):
    ident = b'\x7fELF' + bytes([elf_class, 1, 1]) + bytes(9)
    if elf_class == 1:
        return struct.pack('<16sHHIIIIIHHHHHH', ident, 3, machine, 1,
                           0, 0, 0, 0x05000000, 52, 32, 0, 40, 0, 0)
    return struct.pack('<16sHHIQQQIHHHHHH', ident, 3, machine, 1,
                       0, 0, 0, 0, 64, 56, 0, 64, 0, 0)


class InventoryCompatibilityTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'work/tests'
        scratch.mkdir(parents=True, exist_ok=True)
        area = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(area.cleanup)
        self.root = Path(area.name)

    def apk(self, name, split=None, version='1', library=None):
        manifest = '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.nextos.training"'
        if split is not None:
            manifest += ' split="' + split + '"'
        if version is not None:
            manifest += ' android:versionCode="' + version + '" android:versionName="1.0"'
        manifest += '/>'
        path = self.root / name
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('AndroidManifest.xml', manifest)
            archive.writestr('assets/pad.txt', b'x' * 200)
            if library is not None:
                archive.writestr('lib/arm64-v8a/libtraining.so', library)
        return path

    @unittest.skipUnless(shutil.which('readelf'), 'requires readelf')
    def test_elf64_cannot_be_reported_as_valid_armv7(self):
        result = inventory.library_inventory(elf_header(machine=40),
            'lib/armeabi-v7a/guest.so', self.root)
        self.assertFalse(result['abi_consistent'])
        self.assertIn('directory/ELF ABI mismatch', result['v5_preflight_blockers'])

    @unittest.skipUnless(shutil.which('readelf'), 'requires readelf')
    def test_valid_32_and_64_bit_identities_are_kept(self):
        for elf_class, machine, abi in ((1, 40, 'armeabi-v7a'), (2, 183, 'arm64-v8a')):
            with self.subTest(abi=abi):
                result = inventory.library_inventory(elf_header(elf_class, machine),
                    'lib/' + abi + '/guest.so', self.root)
                self.assertTrue(result['abi_consistent'])
                self.assertEqual(result['elf_class'], elf_class * 32)
                self.assertEqual(result['v5_preflight_blockers'], [])

    def test_bad_headers_rejected_before_readelf(self):
        original = elf_header()
        mutations = [original[:32]]
        for offset, value in ((4, 0), (4, 3), (5, 2), (6, 0), (20, 0), (52, 0)):
            data = bytearray(original)
            data[offset] = value
            mutations.append(bytes(data))
        with mock.patch.object(inventory.subprocess, 'check_output') as readelf:
            for data in mutations:
                with self.subTest(header=data[:24]):
                    with self.assertRaises(ValueError):
                        inventory.library_inventory(data, 'lib/arm64-v8a/a.so', self.root)
            readelf.assert_not_called()

    def test_loader_rejected_dynamic_tags_are_visible(self):
        def readelf(argv, **kwargs):
            return '(TEXTREL)\n(RPATH)\n(RUNPATH)\n' if '-dW' in argv else ''
        with mock.patch.object(inventory.subprocess, 'check_output', side_effect=readelf):
            report = inventory.library_inventory(elf_header(), 'lib/arm64-v8a/a.so', self.root)
        self.assertTrue({'TEXTREL', 'RPATH', 'RUNPATH'} <= set(report['v5_preflight_blockers']))

    @unittest.skipUnless(shutil.which('readelf'), 'requires readelf')
    def test_x86_identity_is_not_v5_loader_support(self):
        result = inventory.library_inventory(elf_header(machine=62),
            'lib/x86_64/guest.so', self.root)
        self.assertTrue(result['abi_consistent'])
        self.assertIn('ABI unsupported by V5 nxloader', result['v5_preflight_blockers'])

    @unittest.skipUnless(shutil.which('readelf'), 'requires readelf')
    def test_wrong_elf_class_cannot_choose_arm64_for_inventory(self):
        apk = self.apk('wrong.apk', library=elf_header(1, 183))
        report = inventory.inventory([apk], self.root / 'scratch')['containers'][0]
        self.assertIsNone(report['preferred_abi'])
        self.assertFalse(report['libraries'][0]['abi_consistent'])

    @unittest.skipUnless(shutil.which('readelf'), 'requires readelf')
    def test_valid_arm64_still_has_priority(self):
        apk = self.apk('good.apk', library=elf_header())
        report = inventory.inventory([apk], self.root / 'scratch')['containers'][0]
        self.assertEqual(report['preferred_abi'], 'arm64-v8a')

    def test_matching_base_and_split_versions_pass(self):
        base = self.apk('base.apk')
        split = self.apk('arm.apk', split='config.arm64_v8a')
        report = inventory.inventory([base, split], self.root / 'scratch')
        self.assertEqual(len(report['containers']), 2)

    def test_mixed_versions_fail_even_with_same_package(self):
        base = self.apk('base.apk')
        split = self.apk('arm.apk', split='config.arm64_v8a', version='2')
        with self.assertRaisesRegex(ValueError, 'conflicting version_code'):
            inventory.inventory([base, split], self.root / 'scratch')

    def test_duplicate_base_or_split_identity_fails(self):
        for split in (None, 'config.arm64_v8a'):
            apk = self.apk('input.apk', split=split)
            with self.subTest(split=split), self.assertRaisesRegex(ValueError, 'duplicate base or split'):
                inventory.inventory([apk, apk], self.root / 'scratch')

    def test_unknown_version_is_not_invented(self):
        base = self.apk('base.apk')
        split = self.apk('split.apk', split='assets', version=None)
        with mock.patch.object(inventory.shutil, 'which', return_value=None):
            report = inventory.inventory([base, split], self.root / 'scratch')
        self.assertIsNone(report['containers'][1]['version_code'])

    def test_uncompressed_budget_applies_to_whole_apk_set(self):
        base = self.apk('base.apk')
        split = self.apk('split.apk', split='assets')
        with mock.patch.object(inventory, 'TOTAL_LIMIT', 500):
            with self.assertRaisesRegex(ValueError, 'APK set exceeds'):
                inventory.inventory([base, split], self.root / 'scratch')

    def test_large_manifest_rejected_before_decompression(self):
        apk = self.root / 'oversize.apk'
        with zipfile.ZipFile(apk, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('AndroidManifest.xml', b' ' * (2 * 1024 * 1024 + 1))
        with mock.patch.object(zipfile.ZipFile, 'read', side_effect=AssertionError('must not decompress')):
            with self.assertRaisesRegex(ValueError, 'manifest exceeds'):
                inventory.inventory([apk], self.root / 'scratch')


if __name__ == '__main__':
    unittest.main()
