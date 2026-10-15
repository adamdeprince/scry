#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Native video precision and colorimetry; requires the optional HPSS runtime."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import av
import numpy as np
from hpss_color import rgba16
from hpss import install_decoder, Controls


def frame(samples, transfer=13, primaries=1, matrix=1):
    f = av.VideoFrame(len(samples), 2, 'yuv444p10le')
    f.color_range = 2; f.color_trc = transfer; f.color_primaries = primaries; f.colorspace = matrix
    for index, p in enumerate(f.planes):
        data = np.zeros((p.height, p.line_size // 2), dtype='<u2')
        data[:, :p.width] = samples if index == 0 else 512
        p.update(data.tobytes())
    return f


def tests():
    result = rgba16(frame([511,512]))
    assert result.dtype == np.dtype('<f2')
    assert result[0,0,0] != result[0,1,0], '10-bit detail lost before binary16 output'
    assert result[0,0,0] < result[0,1,0] and np.all(result[...,3] == 1)
    pq = rgba16(frame([1023,512],16,9,9), True)
    assert 49 < float(pq[0,0,0]) < 50, 'PQ peak must preserve 10000/203 SDR whites'
    assert float(pq[0,1,0]) > .4
    hlg = rgba16(frame([1023,512],18,9,9), True)
    assert 4.9 < float(hlg[0,0,0]) < 5
    bad = frame([512],2,2,2)
    try: rgba16(bad,True)
    except ValueError: pass
    else: raise AssertionError('HDR metadata was guessed')
    # Exercise the pinned upstream decoder boundary, not just NumPy conversion.
    Decoder = install_decoder(); decoder = Decoder(1)
    decoder._dpb_has_idr = True
    native = frame([511,512])
    slot = decoder._tiles[0]; slot.raw_frame = native; slot.good_count = 1
    assert decoder.get_frame(0) is native
    assert decoder.get_frame(0) is None
    decoder.close()
    # Real 10-bit HEVC encode/decode verifies native depth and CICP preservation.
    from fractions import Fraction
    source = frame([400+i for i in range(64)],16,9,9).reformat(width=64,height=64,format='yuv444p10le')
    encoder = av.CodecContext.create('libx265','w'); encoder.width=64; encoder.height=64
    encoder.pix_fmt='yuv444p10le'; encoder.time_base=Fraction(1,30)
    encoder.color_trc=16; encoder.color_primaries=9; encoder.colorspace=9; encoder.color_range=2
    encoder.options={'x265-params':'lossless=1:log-level=error:pools=none:frame-threads=1'}
    packets=encoder.encode(source)+encoder.encode(None)
    decoder=av.CodecContext.create('hevc','r')
    decoded=[]
    for packet in packets: decoded.extend(decoder.decode(packet))
    decoded.extend(decoder.decode(None))
    assert decoded and decoded[0].format.name == 'yuv444p10le'
    assert decoded[0].color_trc == 16 and decoded[0].color_primaries == 9
    assert np.array_equal(rgba16(decoded[0],True),rgba16(source,True))
    print('Native 10-bit HEVC, HDR metadata, PQ, HLG, precision and decoder boundary passed')

if __name__ == '__main__': tests()
