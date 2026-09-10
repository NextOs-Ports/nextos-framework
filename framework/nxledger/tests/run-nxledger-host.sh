#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# nxledger 0.2.3 directed host battery: fixture Git repos only, read-only
# against the real repository, no network, no device.  ResourceWarning is an
# error: fixtures must close every descriptor.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 -B -W error::ResourceWarning "$HERE/test_nxledger.py"
printf 'nxledger 0.2.3 directed host cases: PASS\n'
