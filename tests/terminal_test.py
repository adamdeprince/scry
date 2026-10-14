#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Exercise real terminal I/O and decode the emitted Kitty images independently."""
import base64
import errno
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

class Terminal:
    def __init__(self, executable, arguments, cell=(2, 3), report=True, env=None, pass_fds=(), pixel_mouse=False, pixels=None):
        self.master, self.slave = pty.openpty()
        self.saved = termios.tcgetattr(self.slave)
        self.cell = cell
        self.pixels = pixels
        self.report = report
        self.pixel_mouse = pixel_mouse
        self.resizes = []
        self.images = {}
        self.updates = []
        self.pending_image = None
        self.stream = bytearray()
        self.parse_at = 0
        self.done = False
        self.set_size(8, 4)
        self.process = subprocess.Popen([executable, *arguments], stdin=self.slave,
                                        stdout=self.slave, stderr=subprocess.PIPE,
                                        env=env, pass_fds=pass_fds)

    def set_size(self, columns, rows):
        width, height = self.pixels or (0, 0)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, width, height))
        self.columns, self.rows = columns, rows

    def send(self, data):
        os.write(self.master, data)

    def parse(self):
        while self.parse_at < len(self.stream):
            start = self.parse_at
            if self.stream[start] != 27:
                self.parse_at += 1
                continue
            if start + 1 == len(self.stream):
                return
            if self.stream[start + 1] == ord('['):
                match = re.match(rb'\x1b\[([0-9;:?<>]*[ -/]*)([@-~])', self.stream[start:])
                if not match:
                    return
                parameters, final = match.groups()
                self.parse_at += len(match[0])
                if final == b'p' and parameters == b'?1016$' and self.pixel_mouse:
                    self.send(b'\x1b[?1016;2$y')
                if final != b't' or not self.report:
                    continue
                if parameters == b'16':
                    self.send(f"\x1b[6;{self.cell[1]};{self.cell[0]}t".encode())
                elif parameters == b'18':
                    self.send(f"\x1b[8;{self.rows};{self.columns}t".encode())
                elif parameters == b'14':
                    width, height = self.pixels or (self.columns*self.cell[0], self.rows*self.cell[1])
                    self.send(f"\x1b[4;{height};{width}t".encode())
                elif parameters.startswith(b'8;'):
                    _, rows, columns = map(int, parameters.split(b';'))
                    self.resizes.append((columns, rows))
                    if (columns, rows) != (self.columns, self.rows):
                        self.set_size(columns, rows)
                        self.send(f"\x1b[8;{rows};{columns}t".encode())
                        os.kill(self.process.pid, signal.SIGWINCH)
            elif self.stream[start + 1] == ord('_'):
                end = self.stream.find(b'\x1b\\', start + 2)
                if end < 0:
                    return
                body = bytes(self.stream[start + 2:end])
                self.parse_at = end + 2
                assert body.startswith(b'G'), body
                header, payload = body[1:].split(b';', 1)
                fields = dict(item.split(b'=', 1) for item in header.split(b',') if item)
                if fields.get(b'a') == b'd':
                    self.images.pop(int(fields[b'i']), None)
                    continue
                if fields.get(b'a') == b'T':
                    assert fields[b'f'] == b'24' and fields[b'c'] == b'1' and fields[b'r'] == b'1'
                    assert fields[b'C'] == b'1' and fields[b'q'] == b'2'
                    self.pending_image = (fields, bytearray())
                assert self.pending_image is not None
                assert len(payload) <= 4096
                fields, encoded = self.pending_image
                encoded.extend(payload)
                if dict(item.split(b'=', 1) for item in header.split(b',') if item).get(b'm') == b'0':
                    image_id = int(fields[b'i'])
                    pixels = base64.b64decode(encoded, validate=True)
                    dimensions = (int(fields[b's']), int(fields[b'v']))
                    assert len(pixels) == dimensions[0] * dimensions[1] * 3
                    self.images[image_id] = (dimensions, pixels)
                    self.updates.append(image_id)
                    self.pending_image = None
            else:
                self.parse_at += 2

    def pump(self, timeout=0.05):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    return
                raise
            self.stream.extend(data)
            self.parse()

    def until(self, condition, timeout=5):
        deadline = time.monotonic() + timeout
        while not condition():
            if self.process.poll() is not None:
                raise AssertionError(f"terminal process exited: {self.process.returncode}: {self.process.stderr.read().decode()}")
            if time.monotonic() > deadline:
                raise AssertionError(f"terminal wait timed out: exit={self.process.poll()}, resizes={self.resizes}, updates={self.updates}")
            self.pump()

    def close(self, expected=0):
        if self.process.poll() is None:
            self.send(b'\x1d')
        self.until(lambda: self.process.poll() is not None)
        self.pump(0)
        stderr = self.process.stderr.read().decode()
        assert self.process.returncode == expected, (self.process.returncode, stderr)
        restored = termios.tcgetattr(self.slave)
        # macOS sets PENDIN when canonical mode is restored with unread replies.
        restored[3] &= ~getattr(termios, 'PENDIN', 0)
        self.saved[3] &= ~getattr(termios, 'PENDIN', 0)
        assert restored == self.saved, f"termios not restored: {restored!r} != {self.saved!r}"
        assert b'\x1b[<u' in self.stream and b'\x1b[?1049l' in self.stream, "terminal modes not restored"
        self.done = True
        return stderr

    def destroy(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.process.stderr.close()
        os.close(self.master)
        os.close(self.slave)

def tests(executable):
    terminal = Terminal(executable, ['demo', '--size', '7x5'])
    try:
        terminal.until(lambda: len(terminal.images) == 8)
        assert terminal.resizes[0] == (4, 2), terminal.resizes
        dimensions, pixels = terminal.images[8]
        assert dimensions == (2, 3)
        # Bottom-right cell contains one source column, two source rows, black padding.
        expected = bytes([6*255//7, 3*255//5, 80, 0, 0, 0,
                          6*255//7, 4*255//5, 80, 0, 0, 0,
                          0, 0, 0, 0, 0, 0])
        assert pixels == expected, (pixels, expected)
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            terminal.pump(0.01)
        terminal.updates.clear()
        terminal.until(lambda: len(terminal.updates) > 0)
        assert set(terminal.updates) == {1}, terminal.updates
        terminal.cell = (3, 2)
        terminal.send(b'\x1b[6;2;3t')
        terminal.until(lambda: terminal.resizes[-1] == (3, 3) and len(terminal.images) == 9)
        terminal.close()
    finally:
        terminal.destroy()
    terminal = Terminal(executable, ['demo', '--size', '3x2', '--magnification', '3'], cell=(4,5))
    try:
        terminal.until(lambda: len(terminal.images) == 6)
        assert terminal.resizes[0] == (3,2), terminal.resizes
        for image_id, (dimensions, pixels) in terminal.images.items():
            assert dimensions == (4,5)
            column, row = (image_id - 1) % 3, (image_id - 1) // 3
            expected = bytearray()
            for y in range(5):
                for x in range(4):
                    dx, dy = column * 4 + x, row * 5 + y
                    expected.extend([dx//3*255//3, dy//3*255//2, 80] if dx < 9 and dy < 6 else [0,0,0])
            assert pixels == expected, (image_id, pixels, expected)
        terminal.cell = (3,2)
        terminal.send(b'\x1b[6;2;3t')
        terminal.until(lambda: terminal.resizes[-1] == (3,3) and len(terminal.images) == 9)
        terminal.close()
    finally:
        terminal.destroy()
    for factor in ['0', '-1', '1.5', '2147483648', '2147483647']:
        result = subprocess.run([executable, 'demo', '--magnification', factor], capture_output=True)
        assert result.returncode == 1 and b'TTY' not in result.stderr, (factor, result.stderr)
    for arguments, message in [(['demo', '--size', '7x5', '--max-tiles', '4'], 'exceeding --max-tiles'),
                               (['demo'], 'terminal did not report cell pixels')]:
        terminal = Terminal(executable, arguments, report=arguments == ['demo', '--size', '7x5', '--max-tiles', '4'])
        try:
            terminal.until(lambda: terminal.process.poll() is not None)
            assert message in terminal.close(expected=1)
        finally:
            terminal.destroy()
    terminal = Terminal(executable, ['demo', '--size', '7x5', '--cell-size', '2x3'], report=False)
    try:
        terminal.until(lambda: len(terminal.images) == 8)
        os.kill(terminal.process.pid, signal.SIGTERM)
        terminal.close()
    finally:
        terminal.destroy()
    print('terminal tests passed')

if __name__ == '__main__':
    tests(sys.argv[1])
