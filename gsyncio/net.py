"""
gsyncio.net - Native socket operations using C backend

This module provides high-performance socket operations using gsyncio's
native C implementation with fiber-aware async I/O.
"""

from ._gsyncio_core import GSocket
from .core import _HAS_CYTHON
from ._gsyncio_core import (
    net_init_ as _net_init,
    net_shutdown_ as _net_shutdown,
)

if _HAS_CYTHON:
    _net_init()


class Socket:
    """High-performance socket wrapper using native C backend"""
    def __init__(self, reader=None, writer=None, sock=None):
        self._reader = reader
        self._writer = writer
        self._sock = sock

    @classmethod
    def create_tcp(cls, host="0.0.0.0", port=0):
        """Create a TCP socket connected to host:port"""
        sock = GSocket.tcp()
        if host and port:
            sock.connect(host, port)
        return cls(sock=sock)

    @classmethod
    def create_server(cls, host="0.0.0.0", port=8080, backlog=128):
        """Create a listening TCP server socket"""
        sock = GSocket.tcp()
        sock.bind(host, port)
        sock.listen(backlog)
        return ServerSocket(sock)

    @property
    def fd(self):
        """File descriptor"""
        return self._sock.fd if self._sock else -1

    @property
    def closed(self):
        """Check if socket is closed"""
        return self._sock.closed if self._sock else True

    async def accept(self):
        """Accept a connection (async)"""
        client = await self._sock.accept()
        if client:
            return Socket(sock=client), None
        return None, None

    async def connect(self, host, port):
        """Connect to remote host (async)"""
        self._sock.connect(host, port)
        return self

    async def recv(self, n=4096):
        """Receive data from socket (async)"""
        return await self._sock.recv(n)

    async def send(self, data):
        """Send data to socket (async)"""
        return await self._sock.send(data)

    def send_nowait(self, data):
        """Send data without blocking"""
        return self._sock.send_sync(data)

    def recv_nowait(self, n=4096):
        """Receive data without blocking"""
        return self._sock.recv_sync(n)

    def close(self):
        """Close the socket"""
        if self._sock:
            self._sock.close()


class ServerSocket:
    """TCP server socket wrapper"""
    def __init__(self, sock):
        self._sock = sock
        self._closed = False

    @property
    def fd(self):
        return self._sock.fd if self._sock else -1

    @property
    def closed(self):
        return self._closed or (self._sock.closed if self._sock else True)

    async def accept(self):
        """Accept a connection (async)"""
        client = await self._sock.accept()
        if client:
            return Socket(sock=client), None
        return None, None

    def close(self):
        """Close the server socket"""
        self._closed = True
        if self._sock:
            self._sock.close()


async def connect(host: str, port: int):
    """Connect to a remote host (async)"""
    sock = GSocket.tcp()
    sock.connect(host, port)
    return Socket(sock=sock)


async def listen(host: str = "0.0.0.0", port: int = 8080, backlog: int = 128):
    """Create a listening socket (async)"""
    sock = GSocket.tcp()
    sock.bind(host, port)
    sock.listen(backlog)
    return ServerSocket(sock)


async def accept(server_sock):
    """Accept a connection from server socket (async)"""
    return await server_sock.accept()


async def recv(sock, n: int = 4096) -> bytes:
    """Receive data from socket (async)"""
    return await sock.recv(n)


async def send(sock, data: bytes) -> int:
    """Send data to socket (async)"""
    return await sock.send(data)


def init():
    """Initialize native networking"""
    if _HAS_CYTHON:
        _net_init()


def shutdown():
    """Shutdown native networking"""
    if _HAS_CYTHON:
        _net_shutdown()


__all__ = [
    'Socket',
    'ServerSocket',
    'connect',
    'listen',
    'accept',
    'recv',
    'send',
    'init',
    'shutdown',
]
