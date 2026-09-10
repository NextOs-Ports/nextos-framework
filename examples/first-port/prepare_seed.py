#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""NXExtract transactional hook for the original training input only."""
import os
from pathlib import Path
import sys
stage=Path(os.environ['NXEXTRACT_STAGE'])
shadow=Path(os.environ['NXEXTRACT_HOOK_SHADOW'])
if (stage/'seed.txt').read_bytes()!=b'seed=7\n':
    sys.exit('training seed input does not match the documented contract')
(shadow/'seed.txt').write_bytes(b'7\n')
print('NXEXTRACT_PROGRESS 1 1 TRAINING SEED PREPARED',flush=True)
