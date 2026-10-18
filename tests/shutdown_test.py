#!/usr/bin/env python3
"""Isolated real-process SIGINT/SIGTERM and WebSocket sample regression tests.
Never reads the normal settings or initializes a Linux audio backend.
"""
import array
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
MAX_EXIT = 5.0


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Child:
    def __init__(self, directory, tls=False, invalid_tls=False):
        self.http_port = free_port()
        self.ws_port = free_port()
        while self.ws_port == self.http_port:
            self.ws_port = free_port()
        self.tls = tls
        self.settings = Path(directory) / 'settings.json'
        self.log = tempfile.TemporaryFile()
        env = dict(os.environ, CM5AUDIO_SETTINGS=str(self.settings),
                   CM5AUDIO_HTTP_PORT=str(self.http_port), CM5AUDIO_WS_PORT=str(self.ws_port),
                   CM5AUDIO_BIND_ADDRESS='127.0.0.1')
        if tls or invalid_tls:
            env['CM5AUDIO_TLS_CERT'] = str(Path(directory) / ('invalid.crt' if invalid_tls else 'server.crt'))
            env['CM5AUDIO_TLS_KEY'] = str(Path(directory) / ('invalid.key' if invalid_tls else 'server.key'))
        args = [str(ROOT / 'cm5audio'), '--no-audio']
        if not tls and not invalid_tls:
            args.append('--plain-text')
        self.process = subprocess.Popen(args, cwd=ROOT, env=env, stdout=self.log, stderr=self.log)

    def request(self, path, body=None, method='GET', expected=200):
        connection_type = http.client.HTTPSConnection if self.tls else http.client.HTTPConnection
        kwargs = {'timeout': 3}
        if self.tls:
            kwargs['context'] = ssl._create_unverified_context()
        conn = connection_type('127.0.0.1', self.http_port, **kwargs)
        try:
            data = json.dumps(body) if body is not None else ''
            conn.request(method, path, data, {'Content-Type': 'application/json'})
            result = conn.getresponse()
            content = result.read()
            assert result.status == expected, (result.status, content)
            return json.loads(content)
        finally:
            conn.close()

    def ready(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError('Startup failed: ' + self.logs())
            try:
                self.request('/api/graph')
                return
            except (ConnectionError, OSError):
                time.sleep(0.01)
        raise AssertionError('Not ready: ' + self.logs())

    def logs(self):
        # Popen shares this open-file offset with its stdout/stderr. pread must
        # not seek that offset while the child is writing startup diagnostics.
        return os.pread(self.log.fileno(), os.fstat(self.log.fileno()).st_size, 0).decode(errors='replace')

    def stop(self, sig):
        started = time.monotonic()
        self.process.send_signal(sig)  # Only the exact Popen-owned child.
        try:
            rc = self.process.wait(timeout=MAX_EXIT)
        except subprocess.TimeoutExpired:
            self.process.kill()  # Test cleanup only; always a failed test.
            self.process.wait()
            raise AssertionError('Shutdown exceeded deadline: ' + self.logs())
        elapsed = time.monotonic() - started
        assert rc == 0, (rc, self.logs())
        for port in (self.http_port, self.ws_port):
            with socket.socket() as check:
                check.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                check.bind(('127.0.0.1', port))
        return elapsed

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.log.close()


class WebSocket:
    def __init__(self, child, key):
        self.socket = socket.create_connection(('127.0.0.1', child.ws_port), timeout=3)
        if child.tls:
            self.socket = ssl._create_unverified_context().wrap_socket(self.socket, server_hostname='localhost')
        self.pending = bytearray()
        token = 'MDEyMzQ1Njc4OWFiY2RlZg=='
        self.socket.sendall((f'GET /ws/audio HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {token}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        data = bytearray()
        while b'\r\n\r\n' not in data:
            data.extend(self.socket.recv(4096))
        headers, remainder = bytes(data).split(b'\r\n\r\n', 1)
        assert b'101' in headers.split(b'\r\n')[0], headers
        self.pending.extend(remainder)
        self.text('assigned')
        self.send_json({'type': 'hello', 'client_id': key, 'client_name': key, 'input_channels': 1, 'output_channels': 1})
        self.text('identity_accepted')

    def read(self, count):
        while len(self.pending) < count:
            block = self.socket.recv(max(4096, count - len(self.pending)))
            if not block:
                raise ConnectionError('WebSocket closed')
            self.pending.extend(block)
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    def receive(self):
        first, length = self.read(2)
        masked = length & 128
        length &= 127
        if length == 126:
            length = struct.unpack('!H', self.read(2))[0]
        elif length == 127:
            length = struct.unpack('!Q', self.read(8))[0]
        mask = self.read(4) if masked else None
        data = self.read(length)
        if mask:
            data = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
        return first & 15, data

    def send(self, opcode, data):
        mask = os.urandom(4)
        header = bytes([128 | opcode])
        if len(data) < 126:
            header += bytes([128 | len(data)])
        elif len(data) < 65536:
            header += bytes([128 | 126]) + struct.pack('!H', len(data))
        else:
            header += bytes([128 | 127]) + struct.pack('!Q', len(data))
        self.socket.sendall(header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(data)))

    def send_json(self, value):
        self.send(1, json.dumps(value).encode())

    def text(self, kind):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            opcode, data = self.receive()
            if opcode == 1:
                value = json.loads(data)
                if value.get('type') == kind:
                    return value
        raise AssertionError('Expected ' + kind)

    def amplitude(self):
        # Drain old transport/ring buffering before observing the accepted state.
        samples = []
        end = time.monotonic() + 0.18
        while time.monotonic() < end:
            opcode, data = self.receive()
            if opcode == 2:
                values = array.array('f')
                values.frombytes(data)
                samples = list(values)
        assert samples, 'No sample output'
        return max(abs(value) for value in samples)

    def close(self):
        self.socket.close()


def main():
    times = []
    with tempfile.TemporaryDirectory(prefix='cm5audio-shutdown-') as directory:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-keyout', directory + '/server.key',
                        '-out', directory + '/server.crt'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        Path(directory, 'invalid.crt').write_text('not a certificate')
        Path(directory, 'invalid.key').write_text('not a key')
        for tls in (False, True):
            for sig in (signal.SIGINT, signal.SIGTERM):
                child = Child(directory, tls=tls)
                sockets = []
                done = threading.Event()
                workers = []
                try:
                    child.ready()
                    ws = WebSocket(child, 'test/client/capture/name')
                    sockets.append(ws.socket)
                    restored_routes = child.request('/api/graph')['routes']
                    if restored_routes:
                        # Each following launch restores the previous saved route.
                        route_id = str(restored_routes[0]['id'])
                        child.request('/api/routes/' + route_id + '/apply', {'gain': 1, 'enabled': True, 'dsp': {'active': False, 'gain': 1, 'chain': []}}, 'POST')
                    else:
                        route = child.request('/api/routes?source_node=tone%2Fgenerator&destination_node=client%2Ftest%2Fclient%2Fcapture%2Fname%2Fplayback&source_channel=0&destination_channel=0', method='POST')
                        route_id = route['id']
                    child.request('/api/tone?enabled=1&frequency=1000&amplitude=0.2', method='POST')
                    assert abs(ws.amplitude() - 0.2) < 0.003
                    draft = {'gain': 0.5, 'enabled': True, 'dsp': {'active': True, 'gain': 0.5, 'chain': [{'type': 'gain', 'params': {'gain': 0.5}}]}}
                    applied = child.request('/api/routes/' + route_id + '/apply', draft, 'POST')
                    assert applied['adopted'] is True
                    assert abs(ws.amplitude() - 0.025) < 0.003
                    draft['dsp']['active'] = False
                    child.request('/api/routes/' + route_id + '/apply', draft, 'POST')
                    assert abs(ws.amplitude() - 0.1) < 0.003
                    draft['enabled'] = False
                    child.request('/api/routes/' + route_id + '/apply', draft, 'POST')
                    assert ws.amplitude() < 0.00001
                    draft['enabled'] = True
                    child.request('/api/routes/' + route_id + '/apply', draft, 'POST')
                    saved = child.request('/api/settings/save', method='POST')
                    assert saved['adopted'] and saved['persisted']
                    # Repeated hello cannot replace buffers used by running threads.
                    ws.send_json({'type': 'hello', 'client_id': 'test/client/capture/name', 'input_channels': 2, 'output_channels': 2})
                    ws.text('identity_conflict')
                    # Abrupt close exercises callback -> deferred sender retirement.
                    ws.close()
                    deadline = time.monotonic() + 2
                    while child.request('/api/clients')['clients']:
                        assert time.monotonic() < deadline, 'Closed client was not retired'
                        time.sleep(0.005)
                    ws = WebSocket(child, 'test/client/capture/name')
                    sockets.append(ws.socket)
                    assert abs(ws.amplitude() - 0.1) < 0.003

                    def traffic():
                        while not done.is_set():
                            try:
                                child.request('/api/meters')
                                child.request('/api/routes/update?id=' + route_id + '&gain=0.5&enabled=1', method='POST')
                            except Exception:
                                return
                    for _ in range(3):
                        thread = threading.Thread(target=traffic)
                        thread.start(); workers.append(thread)
                    # Accepted connections at multiple handshake phases. TLS raw
                    # silent peer specifically exercises the patched SSL accept.
                    for _ in range(3):
                        sockets.append(socket.create_connection(('127.0.0.1', child.ws_port), timeout=2))
                    partial = socket.create_connection(('127.0.0.1', child.http_port), timeout=2)
                    if tls:
                        partial = ssl._create_unverified_context().wrap_socket(partial, server_hostname='localhost')
                    partial.sendall(b'POST /api/settings/save HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n{')
                    sockets.append(partial)
                    times.append(child.stop(sig))
                    print(f'PASS: {"TLS" if tls else "plain"} signal {sig.name}, active samples/routes, clients, HTTP traffic and stalled handshakes ({times[-1]:.3f}s)', flush=True)
                finally:
                    done.set()
                    for sock in sockets:
                        sock.close()
                    for worker in workers:
                        worker.join(timeout=4)
                        assert not worker.is_alive()
                    child.cleanup()
        # Race accepted-worker registration and closes repeatedly. Each case
        # owns every connection and child; no production ports/processes involved.
        for index in range(10):
            child = Child(directory)
            done = threading.Event()
            threads = []
            def churn():
                while not done.is_set():
                    try:
                        with socket.create_connection(('127.0.0.1', child.ws_port), timeout=0.2) as sock:
                            sock.sendall(b'GET /ws/audio HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: MDEyMzQ1Njc4OWFiY2RlZg==\r\nSec-WebSocket-Version: 13\r\n\r\n')
                    except OSError:
                        return
            try:
                child.ready()
                threads = [threading.Thread(target=churn) for _ in range(4)]
                for thread in threads: thread.start()
                time.sleep(0.005)
                times.append(child.stop(signal.SIGINT if index % 2 else signal.SIGTERM))
            finally:
                done.set()
                for thread in threads:
                    thread.join(timeout=2)
                    assert not thread.is_alive()
                child.cleanup()
        # Repeated idle launches ensure cleanup and next-launch operation.
        for sig in (signal.SIGINT, signal.SIGTERM) * 3:
            child = Child(directory)
            try:
                child.ready(); times.append(child.stop(sig))
            finally:
                child.cleanup()
        # Signal during early startup after handler installation (no listeners).
        child = Child(directory)
        try:
            deadline = time.monotonic() + 2
            while 'Starting cm5audio' not in child.logs():
                assert time.monotonic() < deadline
                time.sleep(0.001)
            times.append(child.stop(signal.SIGINT))
        finally:
            child.cleanup()
        invalid = Child(directory, invalid_tls=True)
        try:
            assert invalid.process.wait(timeout=5) == 1
        finally:
            invalid.cleanup()
        # A listener owned by this test forces partial-startup bind failure.
        occupied = socket.socket(); occupied.bind(('127.0.0.1', 0)); occupied.listen()
        env = dict(os.environ, CM5AUDIO_HTTP_PORT=str(occupied.getsockname()[1]),
                   CM5AUDIO_WS_PORT=str(free_port()), CM5AUDIO_SETTINGS=directory + '/settings.json',
                   CM5AUDIO_BIND_ADDRESS='127.0.0.1')
        failure = subprocess.run([str(ROOT / 'cm5audio'), '--no-audio', '--plain-text'], env=env, cwd=ROOT,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
        occupied.close()
        assert failure.returncode == 1, failure.stdout.decode()
    print(f'PASS: {len(times)} graceful exits, all ports reusable, next launch works; max {max(times):.3f}s. No physical audio backend initialized.')


if __name__ == '__main__':
    main()
