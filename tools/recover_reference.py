#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Recover one public code file at a catalog commit into a new ignored work file."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys
from urllib.parse import quote
import urllib.request
ROOT=Path(__file__).resolve().parents[1]
def main(repo_id,relative,destination):
    catalog=json.loads((ROOT/'catalog/ports.json').read_text())
    port=next((p for p in catalog['ports'] if p['id']==repo_id),None)
    if port is None:raise ValueError('not an admitted public-source repository')
    logical=PurePosixPath(relative)
    if (not relative or str(logical)!=relative or logical.is_absolute()
            or '..' in logical.parts or '\\' in relative
            or any(ord(c)<32 for c in relative)):
        raise ValueError('unsafe source path')
    if logical.suffix not in {'.c','.cpp','.h','.hpp','.py','.sh','.cmake','.txt','.md','.json'} and logical.name not in {'LICENSE','NOTICE','CMakeLists.txt','Makefile'}:
        raise ValueError('only reviewable source/text files can be recovered')
    destination=Path(destination).resolve();destination.relative_to((ROOT/'work').resolve())
    target=destination/relative
    target.resolve().relative_to((ROOT/'work').resolve())
    provenance=target.with_name(target.name+'.provenance.json')
    if any(p.exists() or p.is_symlink() for p in (target,provenance)):
        raise ValueError('destination or provenance already exists')
    repository=port['repository'].removeprefix('https://github.com/')
    if repository!='NextOs-Ports/'+repo_id:raise ValueError('unexpected catalog repository')
    url='https://raw.githubusercontent.com/'+repository+'/'+port['source_commit']+'/'+quote(relative,safe='/')
    with urllib.request.urlopen(url,timeout=30) as response:data=response.read(4*1024*1024+1)
    if len(data)>4*1024*1024 or data.startswith((b'\x7fELF',b'PK')):raise ValueError('not a bounded text source')
    data.decode('utf-8')
    manifest=json.loads((ROOT/port['manifest']).read_text())
    record=next((item for item in manifest['included'] if item['path']==relative),None)
    if record is not None and hashlib.sha256(data).hexdigest()!=record['sha256']:
        raise ValueError('download does not match the pinned source manifest')
    target.parent.mkdir(parents=True,exist_ok=True)
    fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as out:out.write(data)
    report={'repository':port['repository'],'commit':port['source_commit'],'path':relative,
            'sha256':hashlib.sha256(data).hexdigest(),'status':'private review copy; not executed or imported into the frozen snapshot'}
    fd=os.open(provenance,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'w',encoding='utf-8') as out:out.write(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('repository_id');p.add_argument('--path',required=True);p.add_argument('--destination',required=True);a=p.parse_args()
    try:main(a.repository_id,a.path,a.destination)
    except (OSError,ValueError) as e:sys.exit('RECOVERY ERROR: '+str(e))
