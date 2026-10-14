#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
"""Local RFB peer: real client decoding, resize, input, and ARD credential exchange."""
import hashlib
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from terminal_test import Terminal

# RFC 3526 group 14. Deterministic server private key is for this local test only.
PRIME = int('''FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1
29024E088A67CC74020BBEA63B139B22514A08798E3404DDEF
9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E4
85B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE
386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC200
7CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655
D23DCA3AD961C62F356208552BB9ED529077096966D670C354
E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C
180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF
6955817183995497CEA956AE515D2261898FA051015728E5A8
AACAA68FFFFFFFFFFFFFFFF'''.replace('\n', ''), 16)

def receive(connection, length):
    data = bytearray()
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise EOFError()
        data.extend(chunk)
    return bytes(data)

def rectangle(x, y, width, height, encoding, payload=b''):
    return struct.pack('!HHHHi', x, y, width, height, encoding) + payload

class Server:
    def __init__(self, ard=False, reject=False, initial_frame=True):
        self.initial_frame = initial_frame
        self.ard, self.reject = ard, reject
        self.connection, self.client_socket = socket.socketpair()
        self.error = None
        self.ready = threading.Event()
        self.requests = 0
        self.keys = []
        self.mouse = []
        self.credentials = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            connection = self.connection
            connection.settimeout(8)
            connection.sendall(b'RFB 003.889\n' if self.ard else b'RFB 003.008\n')
            assert receive(connection, 12) == b'RFB 003.008\n'
            if self.ard:
                connection.sendall(b'\x01\x1e')
                assert receive(connection, 1) == b'\x1e'
                private = 123456789
                public = pow(2, private, PRIME)
                connection.sendall(struct.pack('!HH', 2, 256) + PRIME.to_bytes(256, 'big') + public.to_bytes(256, 'big'))
                encrypted = receive(connection, 128)
                peer = int.from_bytes(receive(connection, 256), 'big')
                shared = pow(peer, private, PRIME).to_bytes(256, 'big')
                key = hashlib.md5(shared).hexdigest()
                result = subprocess.run([shutil.which('openssl'), 'enc', '-aes-128-ecb', '-d',
                                         '-K', key, '-nopad'], input=encrypted, capture_output=True, check=True)
                data = result.stdout
                self.credentials = (data[:64].split(b'\0')[0], data[64:].split(b'\0')[0])
            else:
                connection.sendall(b'\x01\x01')
                assert receive(connection, 1) == b'\x01'
            if self.reject:
                message = b'denied\x1b[31m'
                connection.sendall(struct.pack('!II', 1, len(message)) + message)
                self.ready.set()
                return
            connection.sendall(b'\0\0\0\0')
            assert receive(connection, 1) == b'\x01' # shared desktop
            pixel_format = struct.pack('!BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255, 0, 8, 16)
            name = b'goblin test desktop'
            connection.sendall(struct.pack('!HH', 7, 5) + pixel_format + struct.pack('!I', len(name)) + name)
            while True:
                message = receive(connection, 1)[0]
                if message == 0:
                    data = receive(connection, 19)
                    assert data[3:] == pixel_format, data
                elif message == 2:
                    _, count = struct.unpack('!BH', receive(connection, 3))
                    self.encodings = struct.unpack('!' + 'i'*count, receive(connection, count*4))
                    assert -239 not in self.encodings and -240 not in self.encodings, 'remote cursor must stay server-rendered'
                elif message == 3:
                    receive(connection, 9)
                    self.requests += 1
                    if self.requests == 1 and self.initial_frame:
                        self.update(rectangle(0, 0, 7, 5, 0, bytes([10, 20, 30, 0]) * 35))
                    self.ready.set()
                elif message == 4:
                    down, symbol = struct.unpack('!B2xI', receive(connection, 7))
                    self.keys.append((symbol, down))
                elif message == 5:
                    mask, x, y = struct.unpack('!BHH', receive(connection, 5))
                    self.mouse.append((mask, x, y))
                else:
                    raise AssertionError(f'unexpected RFB message: {message}')
        except (EOFError, ConnectionResetError, OSError):
            if not self.ready.is_set():
                self.error = sys.exc_info()[1]
        except Exception as error:
            self.error = error
        finally:
            self.ready.set()

    def update(self, *rectangles):
        self.connection.sendall(struct.pack('!BBH', 0, 0, len(rectangles)) + b''.join(rectangles))

    def close(self):
        if self.connection:
            self.connection.close()
        self.client_socket.close()
        self.thread.join(timeout=2)
        assert self.error is None, repr(self.error)

def exercise(executable, ard):
    server = Server(ard=ard)
    env = dict(os.environ, GOBLIN_PASSWORD='test-password')
    arguments = [str(server.client_socket.fileno())]
    if ard:
        arguments += ['test-user']
    terminal = Terminal(executable, arguments, env=env, pass_fds=(server.client_socket.fileno(),))
    server.client_socket.close()
    try:
        terminal.until(lambda: len(terminal.images) == 8 and terminal.images[1][1][:3] == bytes([10,20,30]))
        assert terminal.resizes[0] == (4,2)
        if ard:
            assert server.credentials == (b'test-user', b'test-password'), server.credentials
        # Let any resize replies settle before checking idle / redundant updates.
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            terminal.pump(0.01)
        terminal.updates.clear()
        before = server.requests
        server.update(rectangle(0, 0, 7, 5, 0, bytes([10,20,30,0]) * 35))
        terminal.until(lambda: server.requests > before)
        terminal.pump()
        assert not terminal.updates, terminal.updates
        server.update(rectangle(2, 3, 1, 1, 0, bytes([200,100,50,0])))
        terminal.until(lambda: terminal.updates)
        assert terminal.updates == [6], terminal.updates
        terminal.send(b'\x1b[99;5:1u\x1b[99;5:3u\x1b[57442;1:3u')
        terminal.send(b'\x1b[<0;2;1M\x1b[<0;2;1m\x1b[<64;2;1M')
        terminal.until(lambda: len(server.keys) >= 4 and len(server.mouse) >= 5)
        assert server.keys[:4] == [(0xffe3,1), (99,1), (99,0), (0xffe3,0)], server.keys
        before_keys = len(server.keys)
        terminal.send(b'\x1b[97:65;2:1u\x1b[97;1:3u')
        terminal.until(lambda: len(server.keys) >= before_keys + 4)
        assert server.keys[before_keys:before_keys+4] == [(0xffe1,1), (65,1), (0xffe1,0), (65,0)], server.keys
        assert server.mouse[:2] == [(1,3,1), (0,3,1)], server.mouse
        assert server.mouse[-2:] == [(8,3,1), (0,3,1)], server.mouse
        terminal.updates.clear()
        server.update(rectangle(0,0,5,4,-223)) # DesktopSize pseudo-encoding
        terminal.until(lambda: terminal.resizes[-1] == (3,2))
        terminal.pump()
        assert not terminal.images, 'resize allocation was painted before pixels arrived'
        server.update(rectangle(0,0,5,4,0,bytes([30,20,10,0])*20))
        terminal.until(lambda: len(terminal.images) == 6 and terminal.images[1][1][:3] == bytes([30,20,10]))
        terminal.send(b'\x1b[120;1:1u\x1b[<0;2;1M')
        terminal.until(lambda: server.keys[-1:] == [(120,1)] and server.mouse[-1:][0][0] == 1)
        terminal.close()
        server.thread.join(timeout=1)
        assert server.keys[-1:] == [(120,0)], server.keys
        assert server.mouse[-1:][0][0] == 0, server.mouse
    finally:
        terminal.destroy()
        server.close()

def pointer_exercise(executable, factor, pixel_mouse):
    server = Server()
    terminal = Terminal(executable, [str(server.client_socket.fileno()), '--magnification', str(factor)],
                        pass_fds=(server.client_socket.fileno(),), pixel_mouse=pixel_mouse)
    server.client_socket.close()
    columns, rows = (7 * factor + 1) // 2, (5 * factor + 2) // 3
    def position(x, y):
        dx, dy = (x - 1, y - 1) if pixel_mouse else ((x - 1) * 2 + 1, (y - 1) * 3 + 1)
        return min(dx // factor, 6), min(dy // factor, 4)
    try:
        terminal.until(lambda: len(terminal.images) == columns * rows and terminal.images[1][1][:3] == bytes([10,20,30]))
        assert terminal.resizes[0] == (columns, rows)
        assert (b'\x1b[?1016h' in terminal.stream) == pixel_mouse
        assert b'\x1b[?1003h' in terminal.stream and b'\x1b[?1006h' in terminal.stream
        # Hover, left drag, simultaneous right press, separate releases, and middle click.
        operations = [(35,1,1,'M',0), (0,2,2,'M',1), (32,3,2,'M',1),
                      (2,3,2,'M',5), (0,3,2,'m',4), (34,4,3,'M',4),
                      (2,4,3,'m',0), (35,999,999,'M',0), (1,1,1,'M',2), (1,1,1,'m',0)]
        expected = [(mask, *position(x,y)) for _,x,y,_,mask in operations]
        for code,x,y,action,_ in operations:
            terminal.send(f'\x1b[<{code};{x};{y}{action}'.encode())
        for code, mask in [(64,8), (65,16), (66,32), (67,64)]:
            terminal.send(f'\x1b[<{code};2;2M'.encode())
            expected.extend([(0,*position(2,2)), (mask,*position(2,2)), (0,*position(2,2))])
        terminal.until(lambda: len(server.mouse) >= len(expected))
        assert server.mouse == expected, (server.mouse, expected)
        # SGR modifiers must reach the server even without keyboard modifier reports.
        terminal.send(b'\x1b[<16;2;2M\x1b[<48;3;2M\x1b[<16;3;2m\x1b[<35;3;2M')
        terminal.until(lambda: len(server.mouse) == len(expected) + 4 and len(server.keys) == 2)
        assert server.keys == [(0xffe3,1), (0xffe3,0)], server.keys
        assert [event[0] for event in server.mouse[-4:]] == [1,1,0,0]
        # Unsupported extra buttons must not become phantom left clicks.
        terminal.send(b'\x1b[<128;2;2M\x1b[<128;2;2m')
        server.update(rectangle(0,0,5,4,-223))
        terminal.until(lambda: terminal.resizes[-1] == ((5*factor+1)//2, (4*factor+2)//3))
        terminal.close()
        server.thread.join(timeout=1)
        assert len(server.mouse) == len(expected) + 4, server.mouse
    finally:
        terminal.destroy()
        server.close()

def tests(executable):
    server = Server(initial_frame=False)
    terminal = Terminal(executable, [str(server.client_socket.fileno())],
                        pass_fds=(server.client_socket.fileno(),))
    server.client_socket.close()
    try:
        terminal.until(lambda: server.ready.is_set())
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:
            terminal.pump(0.01)
        assert not terminal.images, 'VNC sent a blank desktop before its first framebuffer update'
        server.update(rectangle(0,0,7,5,0,bytes([10,20,30,0])*35))
        terminal.until(lambda: len(terminal.images) == 8)
        assert terminal.images[1][1][:3] == bytes([10,20,30])
        terminal.close()
    finally:
        terminal.destroy()
        server.close()
    exercise(executable, False)
    if shutil.which('openssl'):
        exercise(executable, True)
    else:
        print('ARD test skipped: openssl CLI unavailable')
    pointer_exercise(executable, 3, False)
    pointer_exercise(executable, 2, True)
    server = Server()
    terminal = Terminal(executable, [str(server.client_socket.fileno()), '--view-only', '--magnification', '2'],
                        pass_fds=(server.client_socket.fileno(),), pixel_mouse=True)
    server.client_socket.close()
    try:
        terminal.until(lambda: len(terminal.images) == 28 and terminal.images[1][1][:3] == bytes([10,20,30]))
        terminal.send(b'x\x1b[99;5:1u\x1b[<0;2;1M')
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            terminal.pump(0.01)
        terminal.close()
        server.thread.join(timeout=1)
        assert not server.keys and not server.mouse, (server.keys, server.mouse)
    finally:
        terminal.destroy()
        server.close()
    server = Server(reject=True)
    terminal = Terminal(executable, [str(server.client_socket.fileno())],
                        env=dict(os.environ, GOBLIN_PASSWORD='test-password'), pass_fds=(server.client_socket.fileno(),))
    server.client_socket.close()
    try:
        terminal.until(lambda: terminal.process.poll() is not None)
        error = terminal.close(expected=1)
        assert 'denied' in error and '\x1b' not in error, error
    finally:
        terminal.destroy()
        server.close()
    print('VNC integration tests passed')

if __name__ == '__main__':
    tests(sys.argv[1])
