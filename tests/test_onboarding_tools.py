# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import io
import json
from pathlib import Path
import stat
import platform
import shutil
import subprocess
import tempfile
import unittest
import zipfile
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('inventory',ROOT/'tools/inventory_apk.py')
inventory=importlib.util.module_from_spec(spec);spec.loader.exec_module(inventory)
class InventoryTests(unittest.TestCase):
    def archive(self,entries):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as archive:
            for name,value in entries:archive.writestr(name,value)
        data.seek(0);return zipfile.ZipFile(data)
    def test_plain_identity(self):
        data=b'<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.nextos.example" android:versionName="1.2" android:versionCode="4"/>'
        result=inventory.manifest_identity(data)
        self.assertEqual(result['package_id'],'org.nextos.example');self.assertEqual(result['version_name'],'1.2')
    def test_entities_rejected(self):
        with self.assertRaises(ValueError):inventory.manifest_identity(b'<!DOCTYPE a [<!ENTITY x "y">]><manifest/>')
    def test_traversal_rejected(self):
        with self.archive([('../file',b'x')]) as a:
            with self.assertRaises(ValueError):inventory.member_index(a)
    def test_absolute_rejected(self):
        with self.archive([('/file',b'x')]) as a:
            with self.assertRaises(ValueError):inventory.member_index(a)
    def test_case_collision_rejected(self):
        with self.archive([('lib/A.so',b'x'),('lib/a.so',b'y')]) as a:
            with self.assertRaises(ValueError):inventory.member_index(a)
    def test_symlink_rejected(self):
        entry=zipfile.ZipInfo('link');entry.create_system=3;entry.external_attr=(stat.S_IFLNK|0o777)<<16
        with self.archive([(entry,b'target')]) as a:
            with self.assertRaises(ValueError):inventory.member_index(a)
    def test_safe_members(self):
        with self.archive([('assets/seed.txt',b'seed=7\n')]) as a:
            self.assertEqual(inventory.member_index(a),['assets/seed.txt'])
    def test_noncanonical_archive_aliases_rejected(self):
        for name in ('./AndroidManifest.xml', 'lib//arm64-v8a/a.so',
                     'lib/./arm64-v8a/a.so', 'assets//'):
            with self.subTest(path=name), self.archive([(name,b'x')]) as a:
                with self.assertRaises(ValueError):inventory.member_index(a)
    def test_normal_directory_entries_allowed(self):
        with self.archive([('assets/',b''),('assets/seed.txt',b'x')]) as a:
            self.assertEqual(inventory.member_index(a),['assets/','assets/seed.txt'])
    def test_import_kinds_and_tls(self):
        text='  1: 00000000 0 FUNC GLOBAL DEFAULT UND puts@LIBC\n  2: 00000000 0 OBJECT WEAK DEFAULT UND data\n  3: 00000000 8 TLS GLOBAL DEFAULT 4 local_tls\n'
        imports,blockers=inventory.parse_symbols(text)
        self.assertEqual([x['kind'] for x in imports],['FUNC','OBJECT'])
        self.assertEqual(imports[0]['version'],'LIBC');self.assertIn('TLS symbol',blockers)
    @unittest.skipUnless(shutil.which('cc') and shutil.which('readelf') and platform.machine()=='x86_64',
                         'requires Linux x86_64 C compiler and readelf')
    def test_long_real_elf_import_is_not_truncated(self):
        symbol='nextos_required_import_name_that_must_not_be_truncated_for_shim_registry'
        scratch=ROOT/'work/tests';scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            temp=Path(directory);source=temp/'long.c';library=temp/'long.so'
            source.write_text('extern void '+symbol+'(void); void entry(void) { '+symbol+'(); }\n')
            subprocess.run(['cc','-shared','-fPIC',str(source),'-o',str(library)],check=True)
            report=inventory.library_inventory(library.read_bytes(),'lib/x86_64/long.so',temp)
            self.assertIn(symbol,{x['name'] for x in report['imports']})
    def test_catalog_profiles_have_real_evidence(self):
        catalog=json.loads((ROOT/'catalog/ports.json').read_text())
        profiles=json.loads((ROOT/'catalog/profiles.json').read_text())['profiles']
        self.assertEqual({p['id'] for p in profiles},{p['id'] for p in catalog['ports']})
        for p in profiles:
            self.assertEqual(p['tested_devices'],[])
            for evidence in p['evidence']:self.assertTrue((ROOT/evidence).is_file(),evidence)
            self.assertEqual(p['source_commit'],next(x['source_commit'] for x in catalog['ports'] if x['id']==p['id']))
if __name__=='__main__':unittest.main()
