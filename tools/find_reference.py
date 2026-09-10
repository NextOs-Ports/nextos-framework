#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Search curated NextOS profiles without treating unknown fields as support."""
import argparse
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def search(engine=None,abi=None,renderer=None):
    profiles=json.loads((ROOT/'catalog/profiles.json').read_text())['profiles']
    return [p for p in profiles if
        (not engine or engine.casefold() in p['engine'].casefold()) and
        (not abi or abi in p['abis']) and
        (not renderer or renderer.casefold() in p['renderer'].casefold())]
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--engine');p.add_argument('--abi');p.add_argument('--renderer');a=p.parse_args()
    print(json.dumps(search(a.engine,a.abi,a.renderer),indent=2,ensure_ascii=False))
