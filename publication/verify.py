#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""NextOS: verify the source selection, not gameplay or complete license clearance."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_SUFFIXES = {'.apk', '.apkm', '.apks', '.xapk', '.ipa', '.obb', '.dex', '.zip', '.tar', '.gz', '.xz', '.assets', '.wem', '.bank', '.pck'}
PRIVATE = re.compile(rb'/home/' + b'fel' + b'ipe|/mnt/' + b'ARQUIVOS|fel' + b'c18|fel[.]' + b'c18', re.I)
SECRET = re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)')
EXCLUDED_PLATFORM = re.compile(r'(?i)(?:^|[^a-z0-9])(?:ios|ipados|iphoneos)(?:$|[^a-z0-9])')
V5_EXPORT_SHA256 = 'c038cd9bb1e9bfedc0f24a8b51639fccf5a464a3dfd474b950463315bcb59662'
PRIVATE_TREES = {'.git', 'work', 'build', 'inputs'}


def source_files(root, ignored_top=()):
    """Prune local build trees before walking; never follow directory symlinks."""
    if root.is_symlink():
        yield root
        return
    for directory, children, files in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in children[:]:
            path = parent / name
            if parent == root and name in ignored_top:
                children.remove(name)
            elif path.is_symlink():
                yield path
                children.remove(name)
            elif name == '__pycache__':
                children.remove(name)
        for name in files:
            if parent != root or name not in ignored_top:
                yield parent / name


def selection_errors(root, records, label, directories=('.',)):
    """A frozen selection is an exact file set, including executable modes."""
    failures, expected = [], {}
    for record in records:
        name = record['path']
        relative = PurePosixPath(name)
        if (not name or relative.is_absolute() or '..' in relative.parts
                or str(relative) != name or '\\' in name
                or not any(d == '.' or name.startswith(d + '/') for d in directories)):
            failures.append(label + ' unsafe manifest path: ' + name)
            continue
        if name in expected:
            failures.append(label + ' duplicate manifest path: ' + name)
        expected[name] = record
    actual = {}
    for directory in directories:
        for path in source_files(root / directory):
            actual[path.relative_to(root).as_posix()] = path
    for name in sorted(actual.keys() - expected.keys()):
        failures.append(label + ' unlisted file: ' + name)
    for name, record in expected.items():
        path = actual.get(name)
        if path is None or path.is_symlink() or not path.is_file():
            failures.append(label + ' missing or unsafe file: ' + name)
            continue
        if digest(path) != record['sha256']:
            failures.append(label + ' hash: ' + name)
        if bool(path.stat().st_mode & 0o111) != bool(int(record['mode'], 8) & 0o111):
            failures.append(label + ' executable mode: ' + name)
    return failures


def tracking_errors(paths):
    """Ignored workspace data must also fail if force-added to the Git index."""
    return ['Tracked private/generated path: ' + name for name in paths
            if (PurePosixPath(name).parts[0] in PRIVATE_TREES
                or '__pycache__' in PurePosixPath(name).parts
                or PurePosixPath(name).suffix == '.pyc'
                or any(part == '.env' or part.startswith('.env.')
                       for part in PurePosixPath(name).parts))]


def baseline_errors(raw):
    # The manifest itself is frozen: editing a file AND its listed hash is not
    # permission to replace V5. This distribution does not admit V6 runtime.
    if hashlib.sha256(raw).hexdigest() != V5_EXPORT_SHA256:
        return ['V5 export manifest differs from the frozen publication baseline']
    return []

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
    baseline_raw = (ROOT / 'publication/v5-export.json').read_bytes()
    failures = baseline_errors(baseline_raw)
    if failures:
        print('\n'.join(failures))
        return 1
    baseline = json.loads(baseline_raw)
    failures = port_scope_errors(catalog, ROOT)
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    failures.extend(tracking_errors([name for name in tracked if name]))
    failures.extend(selection_errors(ROOT, baseline['included'], 'V5',
        ('framework', 'suportando_outros_devices/extrator-universal')))
    checked = 0
    allowed_elf = set()
    for record in baseline['included']:
        p = ROOT / record['path']
        if p.is_file() and not p.is_symlink() and p.read_bytes().startswith(b'\x7fELF'):
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
        if len(m['included']) != port['source_file_count']:
            failures.append('Source/catalog file count mismatch: ' + port['id'])
        failures.extend(selection_errors(ROOT / port['source_dir'], m['included'], port['id']))
        checked += len(m['included'])
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
    for p in source_files(ROOT, PRIVATE_TREES):
        rel = p.relative_to(ROOT)
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
    print('PASS: %d source/auxiliary hashes; exact frozen file sets and executable modes; 40 repositories / 44 source titles; 2 community-only titles; 15 Unity source cases; %d pinned framework ELFs; recognized privacy/package and port-scope checks' % (checked, len(allowed_elf)))
    print('Scope: source selection only; public publication, licenses, full port builds and physical claims remain under review.')
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        sys.exit('PUBLICATION ERROR: ' + str(error))
