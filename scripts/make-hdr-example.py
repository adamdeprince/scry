#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Convert a linear Radiance HDR image to a SCRYHDR1 sample frame.

Requires NumPy and an HDRI-enabled ImageMagick. Pixels stay floating point;
this does not promote an SDR screenshot to HDR. Exposure is a linear multiplier.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--width', type=int, default=1136)
p.add_argument('--exposure', type=float, default=.10)
a = p.parse_args()
if not 0 < a.width <= 0xffffffff or not np.isfinite(a.exposure) or a.exposure <= 0:
    p.error('positive uint32 width and finite positive exposure required')
version = subprocess.check_output(['magick', '-version'], text=True)
if 'HDRI' not in version:
    raise ValueError('ImageMagick must retain floating-point HDR samples')
w, h = map(int, subprocess.check_output([
    'magick', str(a.image), '-resize', str(a.width), '-format', '%w %h', 'info:'
], text=True).split())
raw = subprocess.check_output([
    'magick', str(a.image), '-resize', str(a.width),
    '-define', 'quantum:format=floating-point', '-depth', '32', '-endian', 'LSB', 'RGB:-'
])
photo = np.frombuffer(raw, dtype='<f4').reshape(h, w, 3) * a.exposure
if not np.isfinite(photo).all() or np.abs(photo).max() > np.finfo(np.float16).max:
    raise ValueError('sample cannot be represented as binary16')
rgba = np.ones((h, w, 4), dtype='<f2')
rgba[..., :3] = photo
with a.output.open('wb') as stream:
    stream.write(b'SCRYHDR1' + struct.pack('<II', w, h))
    stream.write(rgba.tobytes())
report = {'width': w, 'height': h, 'format': 'linear extended sRGB RGBA binary16',
          'maximum': float(photo.max()),
          'pixels_above_sdr_white': int(np.any(photo > 1, axis=2).sum())}
a.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
