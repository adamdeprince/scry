# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Native-depth decoded video -> Silkie little-endian linear RGBA binary16.

PQ uses the BT.2408 diffuse-white reference of 203 cd/m². HLG uses the
BT.2100 reference display (1000 cd/m², system gamma 1.2). No 8-bit RGB
intermediate is used. Unsupported HDR metadata is an error, not guessed.
"""
import re
import numpy as np


def plane(frame, index, bits, endian='le'):
    source = frame.planes[index]
    dtype = np.dtype('u1' if bits <= 8 else ('<' if endian == 'le' else '>') + 'u2')
    samples = np.frombuffer(source, dtype=dtype).reshape(source.height, source.line_size // dtype.itemsize)
    return samples[:, :source.width].astype(np.float32)


def linearize(rgb, transfer, primaries):
    if transfer == 16:  # SMPTE ST 2084 (PQ), absolute luminance.
        p = np.maximum(rgb, 0) ** (1 / (2523 / 32))
        denominator = 2413 / 128 - (2392 / 128) * p
        if np.any(denominator <= 0):
            raise ValueError('invalid PQ sample outside its transfer function')
        return (np.maximum(p - 3424 / 4096, 0) / denominator) ** (1 / (2610 / 16384)) * (10000 / 203)
    if transfer == 18:  # ARIB STD-B67 / BT.2100 HLG reference OOTF.
        a, b, c = .17883277, .28466892, .55991073
        encoded = np.maximum(rgb, 0)
        scene = np.where(encoded <= .5, encoded * encoded / 3, (np.exp((encoded - c) / a) + b) / 12)
        weights = np.array([.2627, .6780, .0593] if primaries == 9 else [.2126, .7152, .0722])
        luminance = np.maximum(scene @ weights, 0)
        return scene * np.power(luminance, .2)[..., None] * (1000 / 203)
    if transfer == 8:
        return rgb
    magnitude = np.abs(rgb)
    if transfer == 13:
        result = np.where(magnitude <= .04045, magnitude / 12.92, ((magnitude + .055) / 1.055) ** 2.4)
    elif transfer in (1, 6, 14, 15):
        # BT.709 / BT.2020 inverse camera transfer (not an 8-bit approximation).
        alpha = 1.09929682680944 if transfer in (14, 15) else 1.099
        beta = .018053968510807 if transfer in (14, 15) else .018
        result = np.where(magnitude < 4.5 * beta, magnitude / 4.5, ((magnitude + alpha - 1) / alpha) ** (1 / .45))
    else:
        raise ValueError(f'unsupported video transfer characteristic {transfer}')
    return np.copysign(result, rgb)


def rgba16(frame, hdr_requested=False):
    """Preserve decoder precision, range, transfer and primaries before packing."""
    fmt = frame.format.name
    transfer, primaries, matrix = int(frame.color_trc), int(frame.color_primaries), int(frame.colorspace)
    if hdr_requested and (transfer in (0, 2) or primaries in (0, 2)):
        raise ValueError('HDR stream lacks transfer/primaries metadata; refusing to guess its colors')
    if transfer in (0, 2): transfer = 1
    if primaries in (0, 2): primaries = 1
    match = re.fullmatch(r'yuv(j?)(420|422|444)p(?:(9|10|12|14|16)(le|be))?', fmt)
    if match:
        bits = int(match[3] or 8)
        y, u, v = [plane(frame, i, bits, match[4] or 'le') for i in range(3)]
        # Software HEVC preserves source 4:4:4. Other chroma layouts have
        # explicitly defined 2x sampling; expand their actual plane extents.
        sx, sy = (2 if match[2] != '444' else 1), (2 if match[2] == '420' else 1)
        u = u.repeat(sy, axis=0).repeat(sx, axis=1)[:frame.height, :frame.width]
        v = v.repeat(sy, axis=0).repeat(sx, axis=1)[:frame.height, :frame.width]
        if int(frame.color_range) == 2 or match[1]:
            maximum = (1 << bits) - 1
            y /= maximum; u = (u - (1 << (bits - 1))) / maximum; v = (v - (1 << (bits - 1))) / maximum
        else:
            unit = 1 << (bits - 8)
            y = (y - 16 * unit) / (219 * unit)
            u = (u - 128 * unit) / (224 * unit); v = (v - 128 * unit) / (224 * unit)
        if matrix in (0, 2):
            if hdr_requested: raise ValueError('HDR YUV stream lacks matrix coefficients')
            matrix = 1
        coefficients = {1: (.2126, .0722), 5: (.299, .114), 6: (.299, .114), 9: (.2627, .0593)}
        if matrix not in coefficients: raise ValueError(f'unsupported YUV matrix {matrix}')
        kr, kb = coefficients[matrix]; kg = 1 - kr - kb
        red = y + 2 * (1 - kr) * v; blue = y + 2 * (1 - kb) * u
        green = y - 2 * kb * (1 - kb) / kg * u - 2 * kr * (1 - kr) / kg * v
        rgb = np.stack((red, green, blue), axis=-1)
    else:
        match = re.fullmatch(r'gbrp(?:(9|10|12|14|16)(le|be))?', fmt)
        if not match: raise ValueError(f'unsupported native HPSS pixel format {fmt}')
        bits = int(match[1] or 8)
        green, blue, red = [plane(frame, i, bits, match[2] or 'le') / ((1 << bits) - 1) for i in range(3)]
        rgb = np.stack((red, green, blue), axis=-1)
    linear = linearize(rgb, transfer, primaries)
    if primaries == 9:
        transform = [[1.660491, -.587641, -.072850], [-.124550, 1.132900, -.008349], [-.018151, -.100579, 1.118730]]
        linear = linear @ np.asarray(transform, dtype=np.float32).T
    elif primaries == 12:
        transform = [[1.224940, -.224940, 0], [-.042057, 1.042057, 0], [-.019638, -.078636, 1.098274]]
        linear = linear @ np.asarray(transform, dtype=np.float32).T
    elif primaries != 1:
        raise ValueError(f'unsupported video color primaries {primaries}')
    if not np.all(np.isfinite(linear)) or np.any(np.abs(linear) > np.finfo(np.float16).max):
        raise ValueError('decoded HDR samples cannot be represented as binary16')
    output = np.ones((frame.height, frame.width, 4), dtype='<f2')
    output[..., :3] = linear
    return output
