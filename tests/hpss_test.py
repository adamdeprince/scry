#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""HPSS private transport and Silkie negotiation over a real pseudo-terminal."""
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from hpss import Controls, Writer, configuration, read_packet, keysym
from terminal_test import Terminal


def fixture():
    port, w, h, hdr, credentials = configuration(read_packet(sys.stdin.buffer))
    assert credentials == ['example.invalid', 'test-user', 'test-password']
    assert 'GOBLIN_PASSWORD' not in os.environ
    assert (w, h) == (7, 5)
    writer = Writer()
    log = Path(os.environ['SCRY_TEST_LOG'])
    log.write_text(json.dumps({'hdr': hdr, 'pid': os.getpid()})+'\n')
    mode = os.environ.get('SCRY_TEST_MODE', '')
    if mode == 'error': writer.send(b'Efixture authentication failure'); return
    writer.send(b'R' + struct.pack('>II', w, h))
    pixels = b''.join(struct.pack('<eeee', 4, .5 + (x % 2) / 2048, -.25, 1) for y in range(h) for x in range(w))
    payload = b'F' + struct.pack('>6I', 0, 0, w, h, w, h) + pixels
    if mode == 'malformed': payload = payload[:-1]
    writer.send(payload)
    try:
        while True:
            packet = read_packet(sys.stdin.buffer)
            if packet == b'Q': break
            with log.open('a') as stream: stream.write(packet.hex()+'\n')
            if packet[:1] == b'K' and struct.unpack_from('>I', packet, 1)[0] == ord('g'):
                # Many complete updates, with no fixed frame/history limit.
                for index in range(600):
                    writer.send(b'F'+struct.pack('>6I', 6,4,1,1,w,h)+struct.pack('<eeee',5+index/1024, .5, -.25, 1))
    except (EOFError, BrokenPipeError): pass


def tests(executable):
    class Controller:
        def __init__(self): self.events = []
        def key_event(self, down, symbol): self.events.append(('key', down, symbol))
        def pointer_event(self, buttons, x, y): self.events.append(('pointer', buttons, x, y))
    controller = Controller(); controls = Controls(controller)
    controls.packet(b'K' + struct.pack('>4I', ord('A'), ord('a'), 1, 1))
    controls.packet(b'K' + struct.pack('>4I', ord('a'), ord('a'), 0, 3))
    assert controller.events == [('key',True,0xffe1), ('key',True,ord('A')),
                                 ('key',False,0xffe1), ('key',False,ord('A'))]
    controller.events.clear()
    controls.packet(b'P' + struct.pack('>IIIiI', 8, 9, 4, -1, 4))
    assert controller.events == [('key',True,0xffe3), ('pointer',2,8,9),
                                 ('pointer',18,8,9), ('pointer',2,8,9)]
    controls.close()
    assert controller.events[-2:] == [('key',False,0xffe3), ('pointer',0,8,9)]
    assert keysym(0x1f600) == 0x0101f600 and keysym(0x110000) == 0xff1b
    for hdr in (False, True):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'events'
            env = dict(os.environ, GOBLIN_PASSWORD='test-password', GOBLIN_HPSS_HELPER=str(Path(__file__).resolve()),
                       GOBLIN_HPSS_PYTHON=sys.executable, SCRY_TEST_LOG=str(log))
            terminal = Terminal(executable, ['hpss', 'example.invalid', '--username', 'test-user', '--size', '7x5'], hdr=hdr, env=env)
            pid = None
            try:
                terminal.until(lambda: len(terminal.images) == 8)
                meta = json.loads(log.read_text().splitlines()[0]); pid = meta['pid']
                assert meta['hdr'] == hdr
                assert set(terminal.formats.values()) == {64 if hdr else 24}
                if hdr:
                    _, data = terminal.images[1]
                    assert struct.unpack_from('<eeee', data, 0) == (4,.5,-.25,1)
                    assert struct.unpack_from('<eeee', data, 8) == (4,.5+1/2048,-.25,1)
                else:
                    assert terminal.images[1][1][0] == 255
                terminal.send(b'\x1b[103;1:1u\x1b[103;1:3u')
                terminal.until(lambda: (struct.unpack_from('<e', terminal.images[8][1], 16)[0] >= 5.58
                    if hdr else len(terminal.updates) > 8), timeout=15)
                terminal.send(b'\x1b[<0;2;1M\x1b[<0;2;1m')
                terminal.until(lambda: len(log.read_text().splitlines()) >= 5)
                terminal.close()
                try: os.kill(pid, 0)
                except ProcessLookupError: pass
                else: raise AssertionError('HPSS helper was not reaped')
            finally: terminal.destroy()
    for mode, message in [('error', 'fixture authentication failure'), ('malformed', 'invalid HPSS frame geometry')]:
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, GOBLIN_PASSWORD='test-password', GOBLIN_HPSS_HELPER=str(Path(__file__).resolve()),
                       GOBLIN_HPSS_PYTHON=sys.executable, SCRY_TEST_LOG=str(Path(directory)/'events'), SCRY_TEST_MODE=mode)
            terminal = Terminal(executable, ['hpss','example.invalid','--username','test-user','--size','7x5'], env=env)
            try:
                terminal.until(lambda: terminal.process.poll() is not None)
                assert message in terminal.close(expected=1)
            finally: terminal.destroy()
    terminal = Terminal(executable, ['hpss','example.invalid','--username','test-user','--hdr','on'],
                        env=dict(os.environ,GOBLIN_PASSWORD='test-password'))
    try:
        terminal.until(lambda: terminal.process.poll() is not None)
        assert 'did not confirm' in terminal.close(expected=1)
    finally: terminal.destroy()
    print('HPSS transport, 16-bit precision, HDR negotiation, SDR fallback, input, sustained updates and cleanup passed')


if __name__ == '__main__':
    if len(sys.argv) == 1: fixture()
    else: tests(sys.argv[1])
