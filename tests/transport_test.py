#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Compare a tiny desktop over SSH and Skiff in an isolated terminal emulator."""
import argparse
import base64
import errno
import fcntl
import os
import pty
import re
import select
import signal
import subprocess
import termios
import time
import struct
import tempfile
from pathlib import Path


class Probe:
    def __init__(self, command, zero_pixels=False, protocol='demo', columns=12, rows=6):
        self.master, self.slave = pty.openpty()
        self.columns, self.rows = columns, rows
        self.cell = (8, 16)
        self.zero_pixels = zero_pixels
        self.expected = 8 if protocol == 'demo' else self.columns * self.rows
        self.protocol = protocol
        self.colored = False
        self.stream = bytearray()
        self.offset = 0
        self.images = set()
        self.placements = set()
        self.upload = None
        self.queries = []
        self.started = time.monotonic()
        self.first_image = None
        self.set_size()
        self.process = subprocess.Popen(command, stdin=self.slave, stdout=self.slave, stderr=subprocess.PIPE,
            env=dict(os.environ, TERM='xterm-256color'), start_new_session=True,
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))

    def set_size(self):
        pixels = (0, 0) if self.zero_pixels else (self.columns * self.cell[0], self.rows * self.cell[1])
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', self.rows, self.columns, *pixels))

    def send(self, data):
        os.write(self.master, data)

    def parse(self):
        while self.offset < len(self.stream):
            start = self.offset
            if self.stream[start] != 27:
                self.offset += 1
                continue
            if start + 1 >= len(self.stream):
                return
            kind = self.stream[start + 1]
            if kind == ord('['):
                match = re.match(rb'\x1b\[([0-9;:?<>=]*[ -/]*)([@-~])', self.stream[start:])
                if not match:
                    return
                params, final = match.groups()
                self.offset += len(match[0])
                reply = None
                if final == b't':
                    if params == b'16':
                        reply = b'\x1b[6;16;8t'
                    elif params == b'18':
                        reply = f'\x1b[8;{self.rows};{self.columns}t'.encode()
                    elif params == b'14':
                        reply = f'\x1b[4;{self.rows*16};{self.columns*8}t'.encode()
                    elif params.startswith(b'8;'):
                        _, self.rows, self.columns = map(int, params.split(b';'))
                        self.set_size()
                        reply = f'\x1b[8;{self.rows};{self.columns}t'.encode()
                        os.kill(self.process.pid, signal.SIGWINCH)
                elif final == b'n' and params in [b'6', b'?6']:
                    reply = b'\x1b[?1;1R' if params == b'?6' else b'\x1b[1;1R'
                elif final == b'c' and params in [b'', b'0']:
                    reply = b'\x1b[?62c'
                elif final == b'u' and params == b'?':
                    reply = b'\x1b[?0u'
                elif final == b'p' and params == b'?1016$':
                    reply = b'\x1b[?1016;2$y'
                elif final == b'p' and params == b'?5522$':
                    reply = b'\x1b[?5522;0$y'
                if reply:
                    self.queries.append(match[0].decode('ascii'))
                    self.send(reply)
            elif kind in [ord('_'), ord(']'), ord('P')]:
                end = self.stream.find(b'\x1b\\', start + 2)
                if kind == ord(']'):
                    bell = self.stream.find(b'\x07', start + 2)
                    if bell >= 0 and (end < 0 or bell < end):
                        self.offset = bell + 1
                        continue
                if end < 0:
                    return
                body = bytes(self.stream[start + 2:end])
                self.offset = end + 2
                if kind != ord('_') or not body.startswith(b'G'):
                    continue
                header, _, payload = body[1:].partition(b';')
                fields = dict(item.split(b'=', 1) for item in header.split(b',') if b'=' in item)
                image_id = int(fields.get(b'i', b'0'))
                action = fields.get(b'a', b't')
                if action == b'q':
                    self.send(f'\x1b_Gi={image_id};OK\x1b\\'.encode())
                elif action in [b'T', b't']:
                    if b's' in fields and b'v' in fields:
                        self.upload = (fields, bytearray())
                    if self.upload is not None:
                        initial, encoded = self.upload
                        encoded.extend(payload)
                        if fields.get(b'm', b'0') == b'0':
                            image_id = int(initial.get(b'i', b'0'))
                            if 1 <= image_id <= self.expected:
                                pixels = base64.b64decode(encoded, validate=True)
                                channels = int(initial.get(b'f', b'32')) // 8
                                assert len(pixels) == int(initial[b's']) * int(initial[b'v']) * channels
                                assert int(initial[b's']) == 8 and int(initial[b'v']) == 16
                                if self.protocol == 'demo' and image_id == 1:
                                    assert pixels[1:3] == bytes([0, 80]), pixels[:4]
                                if any(value for channel in range(3) for value in pixels[channel::channels]):
                                    self.colored = True
                                self.images.add(image_id)
                                if self.first_image is None:
                                    self.first_image = time.monotonic() - self.started
                                if initial.get(b'a') == b'T':
                                    self.placements.add(image_id)
                            self.upload = None
                elif action == b'p' and 1 <= image_id <= self.expected:
                    self.placements.add(image_id)
            else:
                self.offset += 2

    def pump(self, timeout=0.1):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                self.stream.extend(os.read(self.master, 65536))
                self.parse()
            except OSError as error:
                if error.errno != errno.EIO:
                    raise

    def run(self, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.process.poll() is None:
            self.pump()
            if len(self.images) == self.expected and len(self.placements) == self.expected and self.colored:
                break
        successful = len(self.images) == self.expected and len(self.placements) == self.expected and self.colored
        status = {'images': len(self.images), 'placements': len(self.placements),
                  'first_image_seconds': round(self.first_image, 3) if self.first_image is not None else None,
                  'exit': self.process.poll(), 'queries': [repr(q) for q in self.queries]}
        if self.process.poll() is None:
            self.send(b'\x1d')  # exit goblin-scry; the containing Skiff command also exits
            deadline = time.monotonic() + 5
            while self.process.poll() is None and time.monotonic() < deadline:
                self.pump()
        if self.process.poll() is None:
            self.send(b'\x1e.')  # Skiff escape, if the application never started
            deadline = time.monotonic() + 3
            while self.process.poll() is None and time.monotonic() < deadline:
                self.pump()
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
        status['stderr'] = self.process.stderr.read().decode(errors='replace')[-2000:]
        # Preserve diagnostic text while stripping escape sequences in reports.
        status['output_tail'] = repr(bytes(self.stream[-1500:])) if not successful else ''
        return successful, status

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.process.stderr.close()
        os.close(self.master)
        os.close(self.slave)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('transport', choices=['direct', 'ssh', 'skiff'])
    parser.add_argument('--host', default='naamah.tail4d442.ts.net')
    parser.add_argument('--wrapper', default='/opt/homebrew/bin/goblin-skiff')
    parser.add_argument('--client', default='/opt/homebrew/bin/goblin-skiff-client')
    parser.add_argument('--server', default='/usr/bin/goblin-skiff-server')
    parser.add_argument('--cell-size', action='store_true')
    parser.add_argument('--local', action='store_true', help='run Skiff through its local UDP server')
    parser.add_argument('--zero-pixels', action='store_true', help='emulate a terminal that reports cell pixels only through queries')
    parser.add_argument('--delay-client', type=float, default=0, help='delay attachment after Skiff starts the remote command')
    parser.add_argument('--protocol', choices=['demo', 'x11', 'wayland'], default='demo')
    parser.add_argument('--columns', type=int, default=12)
    parser.add_argument('--rows', type=int, default=6)
    args = parser.parse_args()
    fixture = tempfile.TemporaryDirectory(prefix='goblin-scry-transport-')
    if args.delay_client:
        executable = Path(fixture.name) / 'client'
        executable.write_text(f'#!{os.sys.executable}\nimport os, sys, time\n'
            f'if len(sys.argv) > 2: time.sleep({args.delay_client!r})\n'
            f'os.execv({args.client!r}, [{args.client!r}, *sys.argv[1:]])\n')
        executable.chmod(0o755)
        args.client = str(executable)
    remote = ['goblin-scry', 'demo', '--size', '32x32']
    if args.protocol != 'demo':
        remote = ['goblin-scry', args.protocol, '--', 'xterm' if args.protocol == 'x11' else 'foot']
    if args.cell_size:
        remote += ['--cell-size', '8x16']
    if args.transport == 'direct':
        command = remote
    elif args.transport == 'ssh':
        command = ['ssh', '-S', 'none', '-o', 'BatchMode=yes', '-tt', args.host, *remote]
    else:
        command = [args.wrapper, '--ssh=ssh -S none -o BatchMode=yes', '--predict=never', '--connect-timeout=10',
                   '--client=' + args.client, '--server=' + args.server]
        if args.local:
            command += ['--local']
        command += ['--', args.host, *remote]
    probe = Probe(command, args.zero_pixels, args.protocol, args.columns, args.rows)
    try:
        ok, result = probe.run()
        print(args.transport, 'PASS' if ok else 'FAIL', result)
    finally:
        probe.close()
        fixture.cleanup()
