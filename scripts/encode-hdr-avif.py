#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Encode a SCRYHDR1 linear binary16 frame as real 10-bit BT.2020/PQ HDR AVIF.
Requires NumPy, ImageMagick and avifenc. No 8-bit image intermediate is used.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import numpy as np

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('frame',type=Path); p.add_argument('output',type=Path)
a=p.parse_args()
raw=a.frame.read_bytes()
if raw[:8]!=b'SCRYHDR1' or len(raw)<16: raise ValueError('invalid HDR frame')
w,h=struct.unpack_from('<II',raw,8)
if not w or not h or len(raw)!=16+w*h*8: raise ValueError('invalid HDR frame dimensions')
rgba=np.frombuffer(raw,dtype='<f2',offset=16).reshape(h,w,4).astype(np.float32)
if not np.isfinite(rgba).all(): raise ValueError('non-finite HDR samples')
# Composite transparent window edges onto black, then sRGB -> BT.2020 linear.
rgb=rgba[...,:3]*np.clip(rgba[...,3:],0,1)
luminance=rgb@np.array([.2126,.7152,.0722])
if luminance.max() <= 1:
    raise ValueError('sample contains no luminance above SDR white; recapture with HDR active')
matrix=np.array([[.627403896,.329283038,.043313066],
                 [.069097289,.919540395,.011362316],
                 [.016391439,.088013308,.895595253]],dtype=np.float32)
wide=np.maximum(rgb@matrix.T,0)
nits=wide*203
if nits.max()>10000: raise ValueError('image exceeds the 10000-nit PQ range')
power=(nits/10000)**(2610/16384)
pq=((3424/4096+(2413/128)*power)/(1+(2392/128)*power))**(2523/32)
encoded=np.rint(np.clip(pq,0,1)*65535).astype('<u2')
max_cll=int(np.ceil(nits.max())); max_pall=int(np.ceil((nits@np.array([.2627,.6780,.0593])).mean()))
with tempfile.TemporaryDirectory(prefix='scry-hdr-pq-') as directory:
    png=Path(directory)/'pq16.png'
    subprocess.run(['magick','-size',f'{w}x{h}','-depth','16','-endian','LSB','RGB:-',
                    '-strip','-define','png:color-type=2',str(png)],input=encoded.tobytes(),check=True)
    # Verify the temporary PNG did not apply an implicit gamma transform.
    check=subprocess.check_output(['magick',str(png),'-depth','16','-endian','LSB','RGB:-'])
    if check!=encoded.tobytes(): raise ValueError('PNG intermediate changed the PQ samples')
    subprocess.run(['avifenc','--depth','10','--yuv','444','--range','full','--cicp','9/16/9',
                    '--clli',f'{max_cll},{max_pall}','--ignore-icc','--ignore-exif','--ignore-xmp',
                    '--qcolor','90','--speed','6',str(png),str(a.output)],check=True)
report={'width':w,'height':h,'depth':10,'primaries':'BT.2020','transfer':'PQ / SMPTE ST 2084',
        'matrix':'BT.2020 non-constant luminance','reference_white_nits':203,
        'max_cll':max_cll,'max_pall':max_pall,'maximum_linear_srgb':float(rgb.max()),
        'pixels_above_sdr_white':int(np.any(rgb>1,axis=2).sum()),
        'maximum_relative_luminance':float(luminance.max()),
        'pixels_with_luminance_above_sdr_white':int((luminance>1).sum())}
a.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
