#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Build a named macOS 26 app for explicitly selected HDR window capture.

Grant Screen & System Audio Recording permission after building. Rebuilding an
ad-hoc signed app can require removing and re-adding its permission entry.
"""
import argparse
from pathlib import Path
import plistlib
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, default=Path('release-work/Scry HDR Capture.app'))
a = p.parse_args()
root = Path(__file__).resolve().parent.parent
contents = a.output.resolve() / 'Contents'
(contents / 'MacOS').mkdir(parents=True, exist_ok=True)
with (contents / 'Info.plist').open('wb') as output:
    plistlib.dump({
        'CFBundleIdentifier': 'com.goblinreactor.scry.hdr-capture',
        'CFBundleName': 'Scry HDR Capture', 'CFBundleDisplayName': 'Scry HDR Capture',
        'CFBundleExecutable': 'capture-hdr-window', 'CFBundlePackageType': 'APPL',
        'CFBundleVersion': '1', 'LSUIElement': True,
        'NSScreenCaptureUsageDescription': 'Capture only a selected public HDR example window for the Goblin Scry website.',
    }, output)
subprocess.run(['xcrun', 'swiftc', '-parse-as-library', '-module-cache-path',
                str(contents.parent.parent / 'swift-module-cache'),
                str(root / 'scripts/capture-hdr-window.swift'),
                '-o', str(contents / 'MacOS/capture-hdr-window')], check=True)
subprocess.run(['codesign', '--force', '--sign', '-', str(contents.parent)], check=True)
print(contents.parent)
