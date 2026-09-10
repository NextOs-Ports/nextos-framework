#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Audit declared Linux outputs without executing them or resolving ldd."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

def audit(path,machine='AArch64'):
    path=Path(path)
    if path.is_symlink() or path.read_bytes()[:4]!=b'\x7fELF':
        raise ValueError('expected a regular ELF: '+str(path))
    report={}
    for flag in ('-h','-l','-d','--version-info'):
        report[flag]=subprocess.check_output(['readelf',flag,str(path)],text=True,
            env={**__import__('os').environ,'LC_ALL':'C'})
    if not re.search(r'Machine:\s+'+re.escape(machine)+r'\s*$',report['-h'],re.M):
        raise ValueError('incorrect ELF machine')
    versions=sorted({tuple(map(int,x.split('.'))) for x in re.findall(r'GLIBC_([0-9.]+)',report['--version-info'])})
    if not versions or versions[-1]>(2,30):
        raise ValueError('Linux GLIBC not established or exceeds 2.30: '+str(versions))
    if '(RPATH)' in report['-d'] or '(RUNPATH)' in report['-d']:
        raise ValueError('unexpected runtime search path')
    return {'machine':machine,'max_glibc':'.'.join(map(str,versions[-1])),
            'needed':re.findall(r'\(NEEDED\).*?\[(.*?)\]',report['-d']),
            'sha256':__import__('hashlib').sha256(path.read_bytes()).hexdigest()}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',nargs='+')
    p.add_argument('--machine',default='AArch64');a=p.parse_args()
    try:
        for name in a.elf: print(json.dumps({'file':Path(name).name,**audit(name,a.machine)},sort_keys=True))
    except (OSError,ValueError,subprocess.CalledProcessError) as e:
        print('ELF AUDIT ERROR: '+str(e),file=sys.stderr);return 1
    return 0
if __name__=='__main__':sys.exit(main())
