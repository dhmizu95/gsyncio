"""
gsyncio.net - Native socket operations (fallback to asyncio)

This module provides socket operations. Currently uses asyncio as fallback.
"""

import asyncio


async def connect(host: str, port: int):
    """Connect to a host:port"""
    reader, writer = await asyncio.open_connection(host, port)
    return Socket(reader, writer)


async def listen(host: str = "0.0.0.0", port: int = 8080, backlog: int = 128):
    """Create a listening socket"""
    server = await asyncio.start_server(
        lambda r, w: None, host, port, reuse_address=True
    )
    return ServerSocket(server)


async def accept(server_sock):
    """Accept a connection"""
    reader, writer = await server_sock._server.serve_forever.__self__.wait_connection()
    return Socket(reader, writer)


async def recv(sock, n: int = 4096) -> bytes:
    """Receive data from socket"""
    return await sock._reader.read(n)


async def send(sock, data: bytes) -> int:
    """Send data to socket"""
    sock._writer.write(data)
    await sock._writer.drain()
    return len(data)


class Socket:
    """Socket wrapper"""
    def __init__(self, reader, writer):
        self._reader = reader
        self._writer = writer
    
    @property
    def fd(self):
        return self._writer.get_extra_info('socket').fileno()
    
    def close(self):
        self._writer.close()


class ServerSocket:
    def __init__(self, server):
        self._server = server
    
    def close(self):
        self._server.close()


def init():
    """Initialize native networking"""
    pass


def shutdown():
    """Shutdown native networking"""
    pass


__all__ = [
    'Socket',
    'connect',
    'listen',
    'accept',
    'recv',
    'send',
    'init',
    'shutdown',
]
