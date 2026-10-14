#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Exercise the launcher with isolated server processes and a real RFB transport.

The fake runtime executables validate argv/environment and emit actual pixel and
DesktopSize messages, so tests cover terminal -> server -> renderer end to end.
No host X11 or Wayland session is needed.
"""
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
from terminal_test import Terminal
from vnc_test import receive, rectangle


def record(kind, **values):
    data = (json.dumps(dict(kind=kind, role=Path(sys.argv[0]).name, pid=os.getpid(), **values)) + '\n').encode()
    fd = os.open(os.environ['GOBLIN_TEST_RECORD'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    os.write(fd, data)
    os.close(fd)


def argument(name):
    return sys.argv[sys.argv.index(name) + 1]


def write_size(directory, size):
    target = directory / 'size.json'
    temporary = directory / 'size.tmp'
    temporary.write_text(json.dumps(size))
    temporary.replace(target)


def read_size(directory):
    return tuple(json.loads((directory / 'size.json').read_text()))


def fake_runtime():
    role = Path(sys.argv[0]).name
    record('start', arguments=sys.argv[1:], environment={key: os.environ.get(key) for key in
           ['DISPLAY', 'WAYLAND_DISPLAY', 'SWAYSOCK', 'XDG_RUNTIME_DIR', 'XAUTHORITY',
            'WLR_BACKENDS', 'WLR_RENDERER', 'GOBLIN_PASSWORD']})
    def stop(*_):
        record('stop')
        sys.exit(0)
    signal.signal(signal.SIGTERM, stop)
    if os.environ.get('GOBLIN_TEST_FAIL_SERVER') == role:
        print('intentional startup failure', file=sys.stderr)
        return 1
    if role == 'application':
        if os.environ.get('GOBLIN_TEST_APP_EXIT'):
            return int(os.environ['GOBLIN_TEST_APP_EXIT'])
        while True:
            time.sleep(0.1)
    if role == 'sway':
        directory = Path(os.environ['XDG_RUNTIME_DIR'])
        assert os.environ['WLR_BACKENDS'] == 'headless' and os.environ['WLR_RENDERER'] == 'pixman'
        assert not os.environ.get('DISPLAY') and not os.environ.get('SWAYSOCK')
        config = Path(argument('--config')).read_text()
        assert 'xwayland disable' in config
        size = tuple(map(int, re.search(r'mode --custom (\d+)x(\d+) scale 1', config).groups()))
        write_size(directory, size)
        ready_env = dict(os.environ, WAYLAND_DISPLAY='wayland-7', SWAYSOCK=str(directory / 'sway-ipc.fake.sock'))
        subprocess.run(['/bin/sh', str(directory / 'ready.sh')], env=ready_env, check=True)
        while True:
            time.sleep(0.1)
    if role in ['swaymsg', 'xrandr']:
        if os.environ.get('GOBLIN_TEST_FAIL_CONTROL'):
            print('intentional resize failure', file=sys.stderr)
            return 1
        if role == 'swaymsg':
            directory = Path(os.environ['XDG_RUNTIME_DIR'])
            assert argument('-s') == os.environ['SWAYSOCK']
            size = tuple(map(int, re.search(r'mode --custom (\d+)x(\d+)$', sys.argv[-1]).groups()))
            write_size(directory, size)
            record('resize', size=size)
        else:
            directory = Path(os.environ['XAUTHORITY']).parent
            assert os.environ['DISPLAY'] == ':42'
            if '--output' in sys.argv:
                assert argument('--output') == 'VNC-0'
                size = tuple(map(int, argument('--mode').removeprefix('goblin-').split('x')))
                write_size(directory, size)
                record('resize', size=size)
        return 0
    if role == 'Xvnc':
        directory = Path(argument('-rfbunixpath')).parent
        assert argument('-rfbport') == '-1' and argument('-nolisten') == 'tcp'
        assert argument('-SecurityTypes') == 'None' and argument('-rfbunixmode') == '0600'
        assert '--port' not in sys.argv and '-ac' not in sys.argv
        auth = Path(argument('-auth')).read_bytes()
        assert auth[:8] == b'\xff\xff\0\0\0\0\0\x12'
        assert auth[8:26] == b'MIT-MAGIC-COOKIE-1' and auth[26:28] == b'\0\x10' and len(auth) == 44
        assert auth[-16:] != bytes(16)
        write_size(directory, tuple(map(int, argument('-geometry').split('x'))))
        if os.environ.get('GOBLIN_TEST_SLOW_START'):
            time.sleep(10)
        os.write(int(argument('-displayfd')), b'42\n')
        os.close(int(argument('-displayfd')))
    else:
        assert role == 'wayvnc'
        directory = Path(os.environ['XDG_RUNTIME_DIR'])
        assert os.environ['WAYLAND_DISPLAY'] == 'wayland-7'
        assert argument('--output') == 'HEADLESS-1' and '--render-cursor' in sys.argv and '--unix-socket' in sys.argv
        assert Path(argument('--config')).read_text() == 'enable_auth=false\n'
        assert sys.argv[-1] == str(directory / 'rfb')
    assert stat.S_IMODE(directory.stat().st_mode) == 0o700
    record('directory', path=str(directory))
    connection = socket.socket(fileno=int(os.environ['GOBLIN_TEST_SERVER_FD']))
    (directory / 'rfb').touch()  # readiness marker for the test-only connect replacement
    connection.settimeout(5)
    connection.sendall(b'RFB 003.008\n')
    assert receive(connection, 12) == b'RFB 003.008\n'
    connection.sendall(b'\x01\x01')
    assert receive(connection, 1) == b'\x01'
    connection.sendall(bytes(4))
    assert receive(connection, 1) == b'\x01'
    size = read_size(directory)
    record('initial', size=size)
    pixel_format = struct.pack('!BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255, 0, 8, 16)
    connection.sendall(struct.pack('!HH', *size) + pixel_format + struct.pack('!I', 4) + b'test')
    connection.settimeout(0.05)
    pending = bytearray()
    encodings = []
    painted = False
    try:
        while True:
            new_size = read_size(directory)
            if new_size != size:
                assert -223 in encodings and -308 not in encodings, encodings
                size = new_size
                connection.sendall(struct.pack('!BBH', 0, 0, 1) + rectangle(0, 0, *size, -223))
                painted = False
                record('framebuffer', size=size)
            try:
                data = connection.recv(65536)
                if not data:
                    break
                pending.extend(data)
            except socket.timeout:
                continue
            while pending:
                kind = pending[0]
                lengths = {0: 20, 3: 10, 4: 8, 5: 6}
                if kind == 2:
                    if len(pending) < 4:
                        break
                    length = 4 + struct.unpack('!H', pending[2:4])[0] * 4
                else:
                    assert kind in lengths, kind
                    length = lengths[kind]
                if len(pending) < length:
                    break
                message = bytes(pending[:length])
                del pending[:length]
                if kind == 2:
                    requested = list(struct.unpack('!' + 'i' * ((length - 4) // 4), message[4:]))
                    if -258 in encodings:
                        assert -258 in requested, 'QEMU capability removed while its reply may be queued'
                    encodings = requested
                elif kind == 3 and not painted:
                    connection.sendall(struct.pack('!BBH', 0, 0, 1) +
                                       rectangle(0, 0, *size, 0, bytes([10, 20, 30, 0]) * (size[0] * size[1])))
                    painted = True
                elif kind in [4, 5]:
                    record('input', message=list(message))
    except (BrokenPipeError, ConnectionResetError):
        pass
    # Stay alive until the launcher stops us, as an ordinary display server does.
    while True:
        time.sleep(0.1)


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def cleanup_assertions(path):
    for item in records(path):
        if item['kind'] == 'directory':
            assert not Path(item['path']).exists(), item
        if item['kind'] == 'start' and item['role'] in ['sway', 'wayvnc', 'Xvnc', 'application']:
            try:
                os.kill(item['pid'], 0)
            except ProcessLookupError:
                continue
            raise AssertionError(f"child survived cleanup: {item}")


def local_terminal(executable, arguments, env, **kwargs):
    client, server = socket.socketpair()
    try:
        return Terminal(executable, arguments,
                        env=dict(env, GOBLIN_TEST_CONNECT_FD=str(client.fileno()), GOBLIN_TEST_SERVER_FD=str(server.fileno())),
                        pass_fds=(client.fileno(), server.fileno()), **kwargs)
    finally:
        client.close()
        server.close()


def exercise(executable, mode, binaries, record_path, env):
    terminal = local_terminal(executable, [mode, '--', str(binaries / 'application'), 'one argument', '$(literal)'],
                        cell=(8, 16), env=env)
    try:
        terminal.until(lambda: len(terminal.images) == 32 and terminal.images[1][1][:3] == bytes([10, 20, 30]))
        terminal.until(lambda: any(item['role'] == 'application' for item in records(record_path)))
        assert terminal.resizes == [], terminal.resizes
        app = next(item for item in records(record_path) if item['role'] == 'application')
        assert app['arguments'] == ['one argument', '$(literal)']
        assert app['environment']['GOBLIN_PASSWORD'] is None
        if mode == 'x11':
            assert app['environment']['DISPLAY'] == ':42' and app['environment']['WAYLAND_DISPLAY'] is None
        else:
            assert app['environment']['WAYLAND_DISPLAY'] == 'wayland-7' and app['environment']['DISPLAY'] is None
        # SIGWINCH / XTWINOPS: preserve exact pixels, including a partial cell.
        terminal.pixels = (83, 81)
        terminal.set_size(10, 5)
        os.kill(terminal.process.pid, signal.SIGWINCH)
        terminal.until(lambda: any(item['kind'] == 'framebuffer' and item['size'] == [83, 81] for item in records(record_path)))
        terminal.until(lambda: len(terminal.images) == 50)
        # In-band reports carry pixels even when the terminal ioctl has none.
        terminal.set_size(9, 4)
        terminal.pixels = None
        terminal.send(b'\x1b[48;0;0;0;0t\x1b[48;4;9;64;72t')
        terminal.until(lambda: any(item['kind'] == 'framebuffer' and item['size'] == [72, 64] for item in records(record_path)))
        terminal.until(lambda: len(terminal.images) == 36)
        # Repeated dimensions must not create another native resize.
        before = len([item for item in records(record_path) if item['kind'] == 'resize'])
        terminal.send(b'\x1b[48;4;9;64;72t')
        for _ in range(3):
            terminal.pump(0.02)
        assert len([item for item in records(record_path) if item['kind'] == 'resize']) == before
        # Font changes refresh pixel geometry without resizing the outer terminal.
        terminal.cell = (10, 12)
        terminal.send(b'\x1b[6;12;10t')
        terminal.until(lambda: any(item['kind'] == 'framebuffer' and item['size'] == [90, 48] for item in records(record_path)))
        terminal.until(lambda: len(terminal.images) == 36 and terminal.images[1][0] == (10, 12))
        # Ordinary input still crosses the bridge.
        terminal.send(b'x\x1b[<0;2;2M\x1b[<0;2;2m')
        terminal.until(lambda: len([item for item in records(record_path) if item['kind'] == 'input']) >= 4)
        assert terminal.resizes == []
        terminal.close()
    finally:
        terminal.destroy()
    cleanup_assertions(record_path)


def tests(executable):
    with tempfile.TemporaryDirectory(prefix='goblin-local-tests-') as temporary:
        directory = Path(temporary)
        binaries = directory / 'bin'
        binaries.mkdir()
        # Run this file as separate executable runtime roles without a shell.
        for name in ['Xvnc', 'xrandr', 'sway', 'swaymsg', 'wayvnc', 'application']:
            launcher = binaries / name
            launcher.write_text(f'#!{sys.executable}\nimport os, runpy, sys\nos.environ["GOBLIN_TEST_ROLE"]={name!r}\n'
                                f'sys.path.insert(0, {str(Path(__file__).resolve().parent)!r})\n'
                                f'runpy.run_path({str(Path(__file__).resolve())!r}, run_name="__main__")\n')
            launcher.chmod(0o755)
        for mode in ['x11', 'wayland']:
            record_path = directory / (mode + '.jsonl')
            env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ.get('PATH', ''),
                       GOBLIN_TEST_RECORD=str(record_path), GOBLIN_PASSWORD='must-not-leak',
                       DISPLAY=':existing', WAYLAND_DISPLAY='wayland-existing', SWAYSOCK='/old/socket')
            exercise(executable, mode, binaries, record_path, env)
            # Initial size from TIOCGWINSZ works without any geometry replies.
            record_path.unlink()
            terminal = local_terminal(executable, [mode, '--cell-size', '16x32', '--magnification', '2', '--view-only'],
                                cell=(16, 32), report=False, pixels=(128, 128), env=env)
            try:
                terminal.until(lambda: len(terminal.images) == 32)
                assert any(item['kind'] == 'initial' and item['size'] == [64, 64] for item in records(record_path))
                terminal.send(b'x\x1b[<0;2;2M')
                terminal.pixels = (160, 160)
                terminal.set_size(10, 5)
                os.kill(terminal.process.pid, signal.SIGWINCH)
                terminal.until(lambda: any(item['kind'] == 'framebuffer' and item['size'] == [80, 80] for item in records(record_path)))
                terminal.until(lambda: len(terminal.images) == 50)
                os.kill(terminal.process.pid, signal.SIGTERM)
                terminal.close()
                assert not any(item['kind'] == 'input' for item in records(record_path))
            finally:
                terminal.destroy()
            cleanup_assertions(record_path)
            # Closing the launched command is a normal exit; a failing command
            # produces a failure. Both cases must remove all display processes.
            for status in [0, 7]:
                record_path.unlink()
                terminal = local_terminal(executable, [mode, '--', str(binaries / 'application')],
                                          cell=(8, 16), env=dict(env, GOBLIN_TEST_APP_EXIT=str(status)))
                try:
                    terminal.until(lambda: terminal.process.poll() is not None)
                    error = terminal.close(expected=0 if status == 0 else 1)
                    if status:
                        assert 'application exited' in error
                finally:
                    terminal.destroy()
                cleanup_assertions(record_path)
            # Failed native controls surface a useful error and stop all children.
            record_path.unlink()
            terminal = local_terminal(executable, [mode], cell=(8, 16), env=dict(env, GOBLIN_TEST_FAIL_CONTROL='1'))
            try:
                terminal.until(lambda: len(terminal.images) == 32)
                terminal.send(b'\x1b[48;5;10;80;80t')
                terminal.until(lambda: terminal.process.poll() is not None)
                assert 'intentional resize failure' in terminal.close(expected=1)
            finally:
                terminal.destroy()
            cleanup_assertions(record_path)
            # Failed startup also restores the terminal and removes server state.
            record_path.unlink()
            role = 'Xvnc' if mode == 'x11' else 'wayvnc'
            terminal = local_terminal(executable, [mode], cell=(8, 16), env=dict(env, GOBLIN_TEST_FAIL_SERVER=role))
            try:
                terminal.until(lambda: terminal.process.poll() is not None)
                assert 'intentional startup failure' in terminal.close(expected=1)
            finally:
                terminal.destroy()
            cleanup_assertions(record_path)
        # Cancel startup before the X11 server announces its display number.
        record_path = directory / 'cancel.jsonl'
        env.update(GOBLIN_TEST_RECORD=str(record_path), GOBLIN_TEST_SLOW_START='1')
        terminal = local_terminal(executable, ['x11'], cell=(8, 16), env=env)
        try:
            terminal.until(lambda: any(item['role'] == 'Xvnc' for item in records(record_path)))
            terminal.close()
        finally:
            terminal.destroy()
        cleanup_assertions(record_path)
        for mode in ['x11', 'wayland']:
            for flag in ['--size', '--port']:
                result = subprocess.run([executable, mode, flag, '800x600' if flag == '--size' else '5900'], capture_output=True)
                assert result.returncode == 1 and b'TTY' not in result.stderr
    print('local display integration tests passed')


if __name__ == '__main__':
    if 'GOBLIN_TEST_ROLE' in os.environ:
        sys.argv[0] = os.environ['GOBLIN_TEST_ROLE']
        sys.exit(fake_runtime() or 0)
    tests(sys.argv[1])
