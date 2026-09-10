#!/usr/bin/env python3
"""Index direct ARM64 branches to Mach-O import stubs for targeted diagnosis."""
from bisect import bisect_right
import argparse
import json
from pathlib import Path
import struct
from inspect_runtime import MachO

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--macho',required=True,type=Path,help='Owner-supplied ARM64 Mach-O executable')
args=parser.parse_args()
m=MachO(args.macho.read_bytes())
_,methods=m.objc_methods()
starts=[x['imp'] for x in methods]
section=m.sections['__text']
result={}
for offset in range(0,section['size']-3,4):
    ins=struct.unpack_from('<I',m.data,section['offset']+offset)[0]
    if ins&0x7c000000 != 0x14000000: continue
    displacement=ins&0x03ffffff
    if displacement&0x02000000:displacement-=0x04000000
    pc=section['addr']+offset
    symbol=m.stubs.get(pc+displacement*4)
    if not symbol:continue
    nearest=bisect_right(starts,pc)-1
    entry={'address':hex(pc),'kind':'bl' if ins&0x80000000 else 'b'}
    if nearest>=0:
        method=methods[nearest]
        entry['nearest_preceding_method']={key:method[key] for key in ('class_name','selector','kind')}
        entry['offset_from_method']=pc-method['imp']
    result.setdefault(symbol,[]).append(entry)
print(json.dumps(result,indent=2))
