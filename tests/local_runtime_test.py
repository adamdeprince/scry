#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Smoke-test actual TigerVNC/Sway/wayvnc servers without launching Kitty.

Requires the local runtime programs, xterm, and foot. Run on Linux with:
  python3 tests/local_runtime_test.py build/goblin-scry
"""
import os
from pathlib import Path
import signal
import sys
from terminal_test import Terminal


def exercise(executable, mode, application):
    before = set(Path('/tmp').glob('goblin-scry-*'))
    # A deterministic shell makes readiness observable before sending input.
    command = ("printf '\\033[48;2;11;37;79m\\033[2J\\033[Hready\\n'; "
               "read -r reply; test \"$reply\" = paint && "
               "printf '\\033[48;2;17;83;149m\\033[2J\\033[Hcapture-test\\n'; cat")
    terminal = Terminal(executable, [mode, '--', application, '-e' if mode == 'x11' else '--',
                                   '/bin/sh', '-c', command], cell=(80, 120), pixels=(640, 480))
    try:
        terminal.until(lambda: len(terminal.images) == 32 and
                       sum(pixels.count(bytes([11,37,79])) for _, pixels in terminal.images.values()) > 100, timeout=15)
        # Require actual captured application content and a visible response to
        # keyboard input; counting tiles also accepts an all-black allocation.
        terminal.send(b'paint\r')
        terminal.until(lambda: any(bytes([17,83,149]) * 100 in pixels
                                   for _, pixels in terminal.images.values()), timeout=15)
        assert terminal.resizes == [], terminal.resizes
        # Verify both growth and shrinkage of the real server framebuffer.
        for columns, rows, pixels in [(10, 5, (800, 600)), (7, 3, (560, 360))]:
            terminal.pixels = pixels
            terminal.set_size(columns, rows)
            os.kill(terminal.process.pid, signal.SIGWINCH)
            terminal.until(lambda: len(terminal.images) == columns * rows, timeout=15)
            assert terminal.resizes == [], terminal.resizes
        # A font change retains the text-area pixels but rebuilds its cell grid.
        terminal.cell = (70, 120)
        terminal.set_size(8, 3)
        terminal.send(b'\x1b[48;3;8;360;560t')
        terminal.until(lambda: len(terminal.images) == 24 and terminal.images[1][0] == (70, 120), timeout=15)
        terminal.close()
    finally:
        terminal.destroy()
    assert set(Path('/tmp').glob('goblin-scry-*')) == before, 'local display directory survived shutdown'
    print(f'{mode}: real server startup, application, growth, shrinkage, font change, and cleanup passed')


if __name__ == '__main__':
    exercise(sys.argv[1], 'x11', 'xterm')
    exercise(sys.argv[1], 'wayland', 'foot')
