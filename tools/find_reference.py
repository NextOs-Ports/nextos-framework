#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Find Android source references and guides; unknown fields are not support."""
import argparse
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


def runtime_guides():
    return json.loads((ROOT / 'catalog/android-runtimes.json').read_text())['runtimes']


def search(engine=None, abi=None, renderer=None, runtime=None, platform='Android'):
    profiles = json.loads((ROOT / 'catalog/profiles.json').read_text())['profiles']
    guides = {item['id']: item['guides'] for item in runtime_guides()}
    if runtime is not None and runtime not in guides:
        raise ValueError('unknown Android runtime family: ' + runtime)
    found = []
    for profile in profiles:
        if platform.casefold() != 'all' and platform.casefold() != profile['platform'].casefold():
            continue
        if engine and engine.casefold() not in profile['engine'].casefold():
            continue
        if abi and abi not in profile['abis']:
            continue
        if renderer and renderer.casefold() not in profile['renderer'].casefold():
            continue
        if runtime and runtime != profile['runtime_family']:
            continue
        result = dict(profile)
        result['guides'] = guides.get(profile['runtime_family'], {})
        found.append(result)
    return found


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine')
    parser.add_argument('--abi')
    parser.add_argument('--renderer')
    parser.add_argument('--runtime', choices=[item['id'] for item in runtime_guides()])
    parser.add_argument('--platform', default='Android',
                        help='source platform, Android by default; all includes historical non-Android profiles')
    args = parser.parse_args()
    print(json.dumps(search(args.engine, args.abi, args.renderer, args.runtime, args.platform),
                     indent=2, ensure_ascii=False))
