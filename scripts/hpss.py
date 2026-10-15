#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Private, framed HPSS transport helper. See docs/HPSS.md for dependencies.

Uses the separately installed AGPL-3.0-or-later iShareScreen transport.
Its decoder integration is pinned to 9ab40d3a3151524f954cff9d6239d891197c1705.
The Scry framebuffer and IPC are always RGBA binary16, even for SDR hosts.
"""
import logging
import os
import struct
import sys
import threading


def read_packet(stream):
    def exact(count):
        result = bytearray()
        while len(result) < count:
            part = stream.read(count - len(result))
            if not part:
                raise EOFError('HPSS owner closed the connection')
            result.extend(part)
        return bytes(result)
    length, = struct.unpack('>Q', exact(8))
    if not length or length > sys.maxsize:
        raise ValueError('invalid HPSS IPC length')
    return exact(length)


class Writer:
    def __init__(self):
        self.lock = threading.Lock()
    def send(self, payload):
        with self.lock:
            sys.stdout.buffer.write(struct.pack('>Q', len(payload)))
            sys.stdout.buffer.write(payload)
            sys.stdout.buffer.flush()


def configuration(packet):
    if len(packet) < 21 or packet[:1] != b'C':
        raise ValueError('invalid HPSS configuration')
    version, port, width, height, hdr = struct.unpack_from('>5I', packet, 1)
    if version != 1 or not 0 < port <= 65535 or not width or not height or hdr not in (0, 1):
        raise ValueError('unsupported HPSS configuration')
    offset, strings = 21, []
    for _ in range(3):
        if offset + 8 > len(packet): raise ValueError('truncated HPSS configuration')
        length, = struct.unpack_from('>Q', packet, offset); offset += 8
        if length > len(packet) - offset: raise ValueError('truncated HPSS credential')
        strings.append(packet[offset:offset + length].decode('utf-8')); offset += length
    if offset != len(packet): raise ValueError('trailing HPSS configuration data')
    return port, width, height, bool(hdr), strings


# Key values are Scry's Key enum; wire output uses X11 keysyms.
FUNCTION_KEYS = [0xff1b, 0xff0d, 0xff09, 0xff08, 0xff52, 0xff54, 0xff51, 0xff53,
                 0xff50, 0xff57, 0xff55, 0xff56, 0xff63, 0xffff]
FUNCTION_KEYS += list(range(0xffbe, 0xffca)) + [0xffe1, 0xffe3, 0xffe9, 0xffeb]
MODIFIERS = (0xffe1, 0xffe9, 0xffe3, 0xffeb)  # Shift, Alt, Ctrl, Super.


def keysym(key):
    if 0x110000 <= key < 0x110000 + len(FUNCTION_KEYS): return FUNCTION_KEYS[key - 0x110000]
    if key <= 0xff: return key
    if key <= 0x10ffff and not 0xd800 <= key <= 0xdfff: return 0x01000000 | key
    raise ValueError('invalid HPSS key')


class Controls:
    def __init__(self, controller):
        self.controller = controller; self.held = {}; self.modifiers = 0; self.pointer = (0, 0)
    def mods(self, desired):
        for bit, symbol in enumerate(MODIFIERS):
            if (self.modifiers ^ desired) & (1 << bit): self.controller.key_event(bool(desired & (1 << bit)), symbol)
        self.modifiers = desired
    def packet(self, data):
        if data[:1] == b'K' and len(data) == 17:
            key, physical, modifiers, action = struct.unpack_from('>4I', data, 1)
            if action > 3 or modifiers & ~15: raise ValueError('invalid HPSS key flags')
            symbol = self.held.get(physical, keysym(key))
            is_modifier = symbol in MODIFIERS
            if not is_modifier: self.mods(modifiers)
            self.controller.key_event(action != 3, symbol)
            if action == 0: self.controller.key_event(False, symbol)
            elif action == 3: self.held.pop(physical, None)
            else: self.held[physical] = symbol
            if is_modifier:
                bit = 1 << MODIFIERS.index(symbol)
                self.modifiers = self.modifiers & ~bit if action in (0, 3) else self.modifiers | bit
            if action == 0: self.mods(0)
        elif data[:1] == b'P' and len(data) == 21:
            x, y, mask, wheel, modifiers = struct.unpack_from('>IIIiI', data, 1)
            if mask & ~7 or modifiers & ~15 or wheel not in (0, 1, -1, 2, -2): raise ValueError('invalid HPSS pointer flags')
            self.mods(modifiers | (self.modifiers & 8))
            self.pointer = x, y
            # iShareScreen's encrypted pointer uses Apple's L/R/M bit order.
            buttons = (mask & 1) | ((mask & 2) << 1) | ((mask & 4) >> 1)
            self.controller.pointer_event(buttons, x, y)
            if wheel:
                bit = {1: 8, -1: 16, -2: 32, 2: 64}[wheel]
                self.controller.pointer_event(buttons | bit, x, y)
                self.controller.pointer_event(buttons, x, y)
        else: raise ValueError('invalid HPSS input packet')
    def close(self):
        for symbol in set(self.held.values()): self.controller.key_event(False, symbol)
        self.held.clear(); self.mods(0)
        self.controller.pointer_event(0, *self.pointer)


def install_decoder():
    from isharescreen.proxy.media.hevc import HevcDecoder
    from isharescreen.proxy.media import registry

    class LinearDecoder(HevcDecoder):
        def __init__(self, num_tiles, **options):
            options['prefer_hwaccel'] = False
            options['enable_quality_gate'] = False
            super().__init__(num_tiles, **options)
        def get_frame(self, tile_idx):
            # Take a native-depth AVFrame before the upstream 8-bit TileFrame
            # conversion. Retain its reference after releasing the slot lock.
            if not self._dpb_has_idr or tile_idx in self._tiles_await_idr: return None
            slot = self._tiles[tile_idx]
            with slot.lock:
                if slot.good_count == slot.last_evaluated_count: return None
                slot.last_evaluated_count = slot.good_count
                frame = slot.raw_frame
            if frame is not None:
                if getattr(frame, 'is_corrupt', False): self._gate.mark_decode_error(tile_idx)
                else: self._gate.mark_clean(tile_idx)
            return frame

    registry._REGISTRY.insert(0, registry.DecoderSpec(
        'scry-linear16', 'hevc', '444', ('*',), 'software', 1000,
        available=lambda: True, build=lambda num_tiles, **options: LinearDecoder(num_tiles, **options),
        note='Scry native-depth frames; no 8-bit conversion'))
    return LinearDecoder


def run(writer):
    port, width, height, hdr, (host, username, password) = configuration(read_packet(sys.stdin.buffer))
    os.environ.update(ISS_VIDEO_CODEC='hevc', ISS_DECODER='scry-linear16', ISS_FORCE_SW_HEVC='0', ISS_CLIP='0')
    try:
        import av
        from hpss_color import rgba16
        from isharescreen.proxy.session import Session, SessionConfig
        from isharescreen.proxy.protocol.negotiation import AdvertiseDims
    except ImportError as error:
        raise RuntimeError('install the HPSS runtime using scripts/setup-hpss.py and set GOBLIN_HPSS_PYTHON to its bin/python3') from error
    if not hasattr(av.VideoFrame, 'color_trc'):
        raise RuntimeError('HPSS requires PyAV with frame color metadata (tested with PyAV 19)')
    LinearDecoder = install_decoder()
    session = Session(SessionConfig(host=host, username=username, password=password, port=port,
        advertise=AdvertiseDims(width=width, height=height, hidpi_scale=1.0), hdr=hdr,
        audio=False, hidpi='off', dynamic_resolution=False))
    stop = threading.Event(); failure = []; controls = None; reader = None
    try:
        session.connect()
        if not isinstance(session._decoder, LinearDecoder):
            raise RuntimeError('HPSS did not select the native-depth decoder; refusing an 8-bit fallback')
        controls = Controls(session.input)
        writer.send(b'R' + struct.pack('>II', *session.canvas_dims))
        def cursor(image):
            if image is None: writer.send(b'U' + struct.pack('>4I', 0, 0, 0, 0))
            else: writer.send(b'U' + struct.pack('>4I', image.width, image.height, image.hotspot_x, image.hotspot_y) + image.rgba)
        session.set_cursor_callback(cursor)
        def receive():
            try:
                while not stop.is_set():
                    packet = read_packet(sys.stdin.buffer)
                    if packet == b'Q': break
                    controls.packet(packet)
            except EOFError: pass
            except Exception as error: failure.append(error)
            finally: stop.set()
        reader = threading.Thread(target=receive, daemon=True, name='scry-input'); reader.start()
        canvas, slot_height = None, None
        while not stop.is_set():
            if not session.is_connected: raise ConnectionError('HPSS server disconnected')
            session.wait_for_fresh_tile(timeout=.02)
            current = session.canvas_dims
            if current != canvas: canvas, slot_height = current, None
            for tile in range(session.num_tiles):
                frame = session.get_frame(tile)
                if frame is None: continue
                if slot_height is None: slot_height = frame.height
                if frame.height != slot_height:
                    raise ValueError('HPSS tile height changed without a canvas update')
                x, y = 0, tile * slot_height
                rows = min(frame.height, canvas[1] - y)
                columns = min(frame.width, canvas[0])
                if rows <= 0 or columns <= 0: raise ValueError('HPSS tile lies outside its advertised canvas')
                pixels = rgba16(frame, hdr)[:rows, :columns]
                writer.send(b'F' + struct.pack('>6I', x, y, columns, rows, *canvas) + pixels.tobytes(order='C'))
        if failure: raise failure[0]
    finally:
        stop.set()
        try:
            if controls: controls.close()
        finally: session.close()


if __name__ == '__main__':
    logging.disable(logging.CRITICAL)
    writer = Writer()
    try: run(writer)
    except (BrokenPipeError, EOFError): pass
    except Exception as error:
        try: writer.send(b'E' + str(error).encode('utf-8', errors='replace'))
        except BrokenPipeError: pass
        raise SystemExit(1)
