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
EXCLUDED_PLATFORM = re.compile(r'(?i)(?:^|[^a-z0-9])(?:ios|ipados|iphoneos)(?:$|[^a-z0-9])')

def port_scope_errors(catalog, root):
    failures = []
    for port in catalog['ports'] + catalog.get('community_ports', []):
        if (EXCLUDED_PLATFORM.search(port.get('platform', ''))
                or EXCLUDED_PLATFORM.search(port['id'])):
            failures.append('Excluded port platform: ' + port['id'])
    admitted = {port['id'] for port in catalog['ports']}
    directory = root / 'ports'
    if directory.is_dir():
        for path in directory.iterdir():
            if path.is_dir() and path.name not in admitted:
                failures.append('Source directory outside catalog scope: ' + path.name)
    return failures

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    catalog = json.loads((ROOT / 'catalog/ports.json').read_text())
    baseline = json.loads((ROOT / 'publication/v5-export.json').read_text())
    failures = port_scope_errors(catalog, ROOT)
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
    if (len(catalog['ports']) != 40 or title_count != 44
            or catalog.get('repository_count') != len(catalog['ports'])):
        failures.append('Expected 40 repositories and 44 source titles')
    for port in catalog['ports']:
        m = json.loads((ROOT / port['manifest']).read_text())
        if m.get('platform') != port.get('platform'):
            failures.append('Source/catalog platform mismatch: ' + port['id'])
        if not m['included']:
            failures.append('Empty source reference: ' + port['id'])
        for record in m['included']:
            p = ROOT / port['source_dir'] / record['path']
            if not p.is_file() or p.is_symlink() or digest(p) != record['sha256']:
                failures.append('Reference integrity: ' + str(p.relative_to(ROOT)))
            checked += 1
    community = catalog.get('community_ports', [])
    if {p['id'] for p in community} != {'strangerthings3-nextos', 'avgn12deluxe-nextos'}:
        failures.append('Community catalog differs from explicitly authorized selection')
    if (catalog.get('source_title_count') != title_count
            or catalog.get('community_title_count') != sum(len(p['games']) for p in community)
            or catalog.get('title_count') != title_count + sum(len(p['games']) for p in community)):
        failures.append('Catalog title counts disagree')
    for port in community:
        if (port.get('author') != 'NextOS' or port.get('repository') is not None
                or port.get('source_dir') is not None
                or port.get('status') != 'community-distributed-catalog-only'):
            failures.append('Unsupported community source claim: ' + port['id'])
        for key in ('documentation', 'documentation_en'):
            if not (ROOT / port[key]).is_file():
                failures.append('Missing community card: ' + port['id'])
    unity = json.loads((ROOT / 'portando_unity/SOURCE-MAP.json').read_text())
    references = {p['id']: p for p in catalog['ports']}
    if len(unity['case_sources']) != 15:
        failures.append('Unity public-source case count changed')
    for case in unity['case_sources']:
        port = references.get(case['repository_id'])
        if (port is None or case['public_repository'] != port['repository']
                or case['public_source_commit'] != port['source_commit']):
            failures.append('Unity case is not bound to an admitted public source: ' + case['id'])
            continue
        for record in case['public_source_files']:
            p = ROOT / port['source_dir'] / record['path']
            if not p.is_file() or digest(p) != record['sha256']:
                failures.append('Unity code reference hash: ' + case['id'] + '/' + record['path'])
    for record in unity['generic_tool_sources']:
        p = ROOT / record['path']
        if not p.is_file() or p.is_symlink() or digest(p) != record['sha256']:
            failures.append('Unity generic tool hash: ' + record['path'])
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
    print('PASS: %d source/auxiliary hashes; 40 repositories / 44 source titles; 2 community-only titles; 15 Unity source cases; %d pinned framework ELFs; recognized privacy/package and port-scope checks' % (checked, len(allowed_elf)))
    print('Scope: source selection only; public publication, licenses, full port builds and physical claims remain under review.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
