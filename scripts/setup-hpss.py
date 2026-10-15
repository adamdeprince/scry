#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Install the optional, pinned HPSS transport without GUI/audio dependencies."""
import argparse
from pathlib import Path
import subprocess
import sys
import venv

PIN = '9ab40d3a3151524f954cff9d6239d891197c1705'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path, help='dedicated Python virtual environment to create')
args = parser.parse_args()
if sys.version_info < (3, 10):
    parser.error('HPSS requires Python 3.10 or later')
root = args.directory.expanduser().resolve()
venv.EnvBuilder(with_pip=True).create(root)
python = root / 'bin/python3'
subprocess.run([str(python), '-m', 'pip', 'install', 'av==19.0.1', 'numpy>=1.26',
                'cryptography>=42', 'pycryptodome>=3.20', 'pyperclip>=1.9'], check=True)
subprocess.run([str(python), '-m', 'pip', 'install', '--no-deps',
                f'git+https://github.com/renegadelink/iShareScreen.git@{PIN}'], check=True)
print(f'HPSS runtime installed. Set GOBLIN_HPSS_PYTHON={python}')
print('iShareScreen is AGPL-3.0-or-later. See docs/HPSS.md for source and licensing.')
