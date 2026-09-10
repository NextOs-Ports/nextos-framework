#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""NextOS: verify the source selection, not gameplay or complete license clearance."""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_SUFFIXES = {'.apk', '.apkm', '.apks', '.xapk', '.ipa', '.obb', '.dex', '.zip', '.tar', '.gz', '.xz', '.assets', '.wem', '.bank', '.pck'}
PRIVATE = re.compile(rb'/home/' + b'fel' + b'ipe|/mnt/' + b'ARQUIVOS|fel' + b'c18|fel[.]' + b'c18', re.I)
SECRET = re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)')

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    catalog = json.loads((ROOT / 'catalog/ports.json').read_text())
    baseline = json.loads((ROOT / 'publication/v5-export.json').read_text())
    failures = []
    checked = 0
    allowed_elf = set()
    for record in baseline['included']:
        p = ROOT / record['path']
        if not p.is_file() or p.is_symlink() or digest(p) != record['sha256']:
            failures.append('V5 integrity: ' + record['path'])
        elif p.read_bytes().startswith(b'\x7fELF'):
            allowed_elf.add(record['path'])
        checked += 1
    title_count = sum(len(p['games']) for p in catalog['ports'])
    if len(catalog['ports']) != 41 or title_count != 45:
        failures.append('Expected 41 repositories and 45 titles')
    for port in catalog['ports']:
        m = json.loads((ROOT / port['manifest']).read_text())
        if not m['included']:
            failures.append('Empty source reference: ' + port['id'])
        for record in m['included']:
            p = ROOT / port['source_dir'] / record['path']
            if not p.is_file() or p.is_symlink() or digest(p) != record['sha256']:
                failures.append('Reference integrity: ' + str(p.relative_to(ROOT)))
            checked += 1
    for p in ROOT.rglob('*'):
        rel = p.relative_to(ROOT)
        if any(part in {'.git', 'work', '__pycache__'} for part in rel.parts):
            continue
        if p.is_symlink():
            failures.append('Unexpected symlink: ' + str(rel))
            continue
        if not p.is_file():
            continue
        raw = p.read_bytes()
        if p.suffix.lower() in FORBIDDEN_SUFFIXES:
            failures.append('Forbidden package/data suffix: ' + str(rel))
        if raw.startswith((b'PK\x03\x04', b'PK\x05\x06')):
            failures.append('Unexpected embedded ZIP container: ' + str(rel))
        if raw.startswith(b'\x7fELF') and str(rel) not in allowed_elf:
            failures.append('Unexpected executable: ' + str(rel))
        if PRIVATE.search(raw) or SECRET.search(raw):
            failures.append('Privacy review: ' + str(rel))
    if failures:
        print('\n'.join(failures))
        return 1
    print('PASS: %d source/auxiliary hashes; 41 repositories; 45 titles; %d pinned framework ELFs; recognized privacy/package checks' % (checked, len(allowed_elf)))
    print('Scope: source selection only; public publication, licenses, full port builds and physical claims remain under review.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
