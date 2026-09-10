#!/usr/bin/env python3
"""Targeted read-only disassembly of the supplied ARM64 guest; no guest execution."""
import argparse
from pathlib import Path
from inspect_runtime import MachO

p=argparse.ArgumentParser()
p.add_argument('address',help='Original Mach-O virtual address, e.g. 0x100094904')
p.add_argument('--macho',required=True,type=Path,help='Owner-supplied ARM64 Mach-O executable')
p.add_argument('--bytes',type=lambda x:int(x,0),default=256)
a=p.parse_args()
m=MachO(a.macho.read_bytes())
address=int(a.address,0)
_,methods=m.objc_methods()
previous=[x for x in methods if x['imp']<=address]
if previous:
    x=previous[-1]
    print(f"Nearest method: {x['kind']} [{x['class_name']} {x['selector']}] {x['imp']:#x}")
for line in m.disassemble(address,address+a.bytes):
    annotation=''
    if 'target' in line:annotation+=' target='+line['target']
    if 'selector' in line:annotation+=' selector='+line['selector']
    if line.get('known_integer_arguments'):annotation+=' '+str(line['known_integer_arguments'])
    print(line['address'],line['instruction'],annotation)
