#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""NextOS: materialize exported V5 sources without inventing historical Git objects."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import io
import tarfile

ROOT=Path(__file__).resolve().parents[1]
SOURCE_COMMIT='c5a5c827ed1918572a7cc01fc5a1740341b7a92d'
COMPONENTS=('nxextract','tests','portmaster','nxabi','nxandroid','nxaudio',
            'nxbootstrap','nxcompat','nxgenerator','nxgl','nxinput','nxloader',
            'nxobs','nxrelease','nxsplash')

def run(args):
    print('+ '+' '.join(map(str,args)),flush=True)
    subprocess.run(list(map(str,args)),check=True,cwd=str(ROOT))

def export_tool_source(destination,manifest):
    """A separate complete export includes contracts absent from pin-v1's registry.

    Do not add directories to the canonical materialized component snapshot.
    """
    expected={r['path']:r for r in manifest['included']}
    archive=subprocess.check_output(['git','archive',SOURCE_COMMIT,'framework',
        'suportando_outros_devices/extrator-universal'],cwd=str(ROOT))
    target=Path(destination)/'tool-source'
    if target.exists():raise ValueError('tool-source already exists')
    seen=set();target.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar:
            if member.isdir():continue
            record=expected.get(member.name)
            if not member.isfile() or record is None or member.size>16*1024*1024:
                raise ValueError('unexpected tool export member')
            blob=tar.extractfile(member).read()
            if hashlib.sha256(blob).hexdigest()!=record['sha256']:
                raise ValueError('tool export hash mismatch')
            out=target/member.name;out.parent.mkdir(parents=True,exist_ok=True)
            out.write_bytes(blob);out.chmod(int(record['mode'],8)&0o777)
            seen.add(member.name)
    if seen!=set(expected):raise ValueError('tool export is incomplete')
    (Path(destination)/'TOOL-SOURCE.json').write_text(json.dumps({
        'schema':1,'collection_source_commit':SOURCE_COMMIT,
        'scope':'complete exported V5 selection, including contracts; separate from component pin-v1',
        'files':manifest['included']},indent=2)+'\n')
    return target

def materialize(destination):
    destination=Path(destination).resolve()
    if destination.exists():
        raise ValueError('destination must be new; keep existing pins/snapshots immutable')
    # Verify the helper bytes against the frozen export before executing it.
    manifest=json.loads(subprocess.check_output(
        ['git','show',SOURCE_COMMIT+':publication/v5-export.json'],cwd=str(ROOT)))
    helper=ROOT/'framework/nxgenerator/framework_pin.py'
    record=next(x for x in manifest['included'] if x['path']==str(helper.relative_to(ROOT)))
    if hashlib.sha256(helper.read_bytes()).hexdigest()!=record['sha256']:
        raise ValueError('frozen framework pin helper changed')
    destination.mkdir(parents=True)
    pin=destination/'FRAMEWORK-BUILD-PIN.json'
    args=[sys.executable,helper,'create','--repository',ROOT,'--output',pin]
    for name in COMPONENTS: args+=['--component',name+'='+SOURCE_COMMIT]
    run(args)
    run([sys.executable,helper,'materialize','--repository',ROOT,'--pin',pin,
         '--destination',destination/'source'])
    run([sys.executable,helper,'verify','--pin',pin,'--snapshot',destination/'source'])
    (destination/'COLLECTION-PROVENANCE.json').write_text(json.dumps({
        'schema':1,'collection_source_commit':SOURCE_COMMIT,
        'original_baseline_tag':manifest['baseline_tag'],
        'original_baseline_commit':manifest['baseline_commit'],
        'composition':'exported source selection; historical private-dependent tests omitted',
        'components':list(COMPONENTS),'physical_support_proven':False,
        'pin_sha256':hashlib.sha256(pin.read_bytes()).hexdigest()
    },indent=2)+'\n')
    export_tool_source(destination,manifest)
    return destination/'source'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination',required=True)
    args=parser.parse_args()
    try: materialize(args.destination)
    except (OSError,ValueError,subprocess.CalledProcessError) as error:
        print('PIN ERROR: '+str(error),file=sys.stderr);return 1
    return 0

if __name__=='__main__': sys.exit(main())
