#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Verify a package-manager installation and save a release verification report."""
import argparse
import datetime
import json
from pathlib import Path
import shutil
import sys

from release import ROOT, output, run, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-info', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--terminal-test', type=Path, default=ROOT / 'tests/terminal_test.py')
    args = parser.parse_args()
    info = json.loads(args.build_info.read_text())
    binary = shutil.which('goblin-scry')
    if not binary:
        raise RuntimeError('goblin-scry is not installed on PATH')
    if info['format'] == 'deb':
        installed = output('dpkg-query', '-W', '-f=${Version}', 'goblin-scry')
        architecture = output('dpkg-query', '-W', '-f=${Architecture}', 'goblin-scry')
        if installed != info['package_version'] or architecture != info['architecture']:
            raise RuntimeError('Installed Debian package differs from the release')
        if Path(binary).resolve() != Path('/usr/bin/goblin-scry').resolve():
            raise RuntimeError('PATH selects a different executable from the Debian package')
    else:
        installed = output('brew', 'list', '--versions', 'goblin-scry')
        if info['version'] not in installed.split()[1:]:
            raise RuntimeError('Installed Homebrew version differs from the release')
        prefix = Path(output('brew', '--prefix', 'goblin-scry'))
        if Path(binary).resolve() != (prefix / 'bin/goblin-scry').resolve():
            raise RuntimeError('PATH selects a different executable from the Homebrew package')
        run('brew', 'test', 'goblinreactor/tap/goblin-scry')
    if output(binary, '--version') != 'goblin-scry ' + info['version']:
        raise RuntimeError('Unexpected executable version')
    if 'FreeRDP3 LibVNCClient' not in output(binary, '--help'):
        raise RuntimeError('A required backend is missing')
    run(sys.executable, args.terminal_test, binary)
    write_json(args.output, {
        'build_host': info['build_host'], 'version': info['version'],
        'source_sha256': info['source_sha256'], 'artifact_sha256': info['sha256'],
        'installed_version': installed, 'verified_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'tests': {'package_manager': 'passed', 'backends': 'passed', 'terminal': 'passed'},
    })
    print('Installed package verified:', args.output)


if __name__ == '__main__':
    main()
