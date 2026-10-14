#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Reject binaries, HTML and agent instructions in the Git index before a commit."""
from pathlib import PurePosixPath
import subprocess
import sys


def main():
    names = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
    forbidden_parts = {'html', 'AGENTS.md', 'SKILL.md', 'CLAUDE.md', '.agents', '.codex', '.claude',
                       '__pycache__', 'dist', 'release-work'}
    binary_suffixes = {'.html', '.htm', '.o', '.a', '.so', '.dylib', '.deb', '.rpm', '.exe',
                       '.zip', '.gz', '.xz', '.png', '.jpg', '.jpeg', '.gif', '.pdf', '.pyc'}
    rejected = []
    for name in filter(None, names):
        path = PurePosixPath(name)
        if forbidden_parts.intersection(path.parts) or (len(path.parts) > 1 and path.parts[0].startswith('build')) or path.suffix in binary_suffixes:
            rejected.append(name)
            continue
        data = subprocess.check_output(['git', 'show', ':' + name])
        if b'\0' in data:
            rejected.append(name)
    if rejected:
        print('Files prohibited from the source repository:\n' + '\n'.join(rejected), file=sys.stderr)
        return 1
    print('Git index contains source and documentation only; no HTML, binaries or agent instructions.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
