#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Decode an example AVIF and verify its HDR metadata and highlight luminance.

Requires PyAV and NumPy, also used by the optional HPSS runtime.
"""
import argparse
import json
import av
import numpy as np
from hpss_color import rgba16

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image')
a = p.parse_args()
with av.open(a.image) as container:
    frame = next(container.decode(video=0))
    if frame.format.name != 'yuv444p10le':
        raise ValueError(f'expected 10-bit 4:4:4, got {frame.format.name}')
    if (frame.color_primaries, frame.color_trc, frame.colorspace) != (9, 16, 9):
        raise ValueError('expected BT.2020/PQ CICP 9/16/9')
    rgb = rgba16(frame, hdr_requested=True)[..., :3].astype(np.float32)
    luminance = rgb @ np.array([.2126, .7152, .0722])
    if luminance.max() <= 1:
        raise ValueError('decoded image contains no luminance above SDR white')
    print(json.dumps({'width': frame.width, 'height': frame.height,
                      'format': frame.format.name, 'cicp': [9, 16, 9],
                      'maximum_relative_luminance': float(luminance.max()),
                      'pixels_with_luminance_above_sdr_white': int((luminance > 1).sum())}, indent=2))
