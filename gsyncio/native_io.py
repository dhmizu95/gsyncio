"""
gsyncio.native_io - Native I/O operations for gsyncio

This module provides native I/O operations without asyncio dependency.
It uses epoll (Linux) or kqueue (BSD/macOS) for async I/O.

Usage:
    import gsyncio.native_io as nio
    
    # Initialize native I/O
    nio.init()
    
    # Create TCP server socket
    server = nio.NativeSocket.tcp()
    server.bind('0.0.0.0', 8080)
    server.listen()
    
    # Accept connections (fiber-aware, non-blocking)
    client, addr = server.accept()
    print(f"Connected: {addr}")
    
    # Send/recv (fiber-aware, non-blocking)
    client.send("Hello!")
    data = client.recv(1024)
    
    # Cleanup
    client.close()
    server.close()
    nio.shutdown()
"""

try:
    from ._gsyncio_native_io import (
        NativeSocket,
        NativeEventLoop,
        init,
        shutdown,
        is_initialized,
    )
    _HAS_NATIVE_IO = True
except ImportError:
    _HAS_NATIVE_IO = False
    
    # Fallback implementations
    class NativeSocket:
        """Fallback socket wrapper using standard library"""
        def __init__(self, fd=-1):
            self.fd = fd
            self.is_server = False
            self.is_closed = True if fd < 0 else False
        
        @staticmethod
        def tcp():
            import socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setblocking(False)
            return NativeSocket(sock.fileno())
        
        @staticmethod
        def udp():
            import socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setblocking(False)
            return NativeSocket(sock.fileno())
        
        def bind(self, host="0.0.0.0", port=0):
            import socket
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((host, port))
            self.fd = s.fileno()
            self.is_closed = False
        
        def listen(self, backlog=128):
            import socket
            s = socket.fromfd(self.fd, socket.AF_INET, socket.SOCK_STREAM)
            s.listen(backlog)
            self.is_server = True
        
        def accept(self):
            import socket
            s = socket.fromfd(self.fd, socket.AF_INET, socket.SOCK_STREAM)
            client, addr = s.accept()
            return NativeSocket(client.fileno()), addr
        
        def connect(self, host, port):
            import socket
            s = socket.fromfd(self.fd, socket.AF_INET, socket.SOCK_STREAM)
            s.connect((host, port))
        
        def send(self, data):
            import socket
            s = socket.fromfd(self.fd, socket.AF_INET, socket.SOCK_STREAM)
            if isinstance(data, str):
                data = data.encode('utf-8')
            return s.send(data)
        
        def recv(self, size):
            import socket
            s = socket.fromfd(self.fd, socket.AF_INET, socket.SOCK_STREAM)
            return s.recv(size)
        
        def close(self):
            if self.fd >= 0:
                import os
                try:
                    os.close(self.fd)
                except:
                    pass
                self.is_closed = True
    
    class NativeEventLoop:
        """Fallback event loop"""
        def run_once(self, timeout_ms=100):
            return 0
        
        def register(self, fd, events, user_data=None):
            pass
        
        def unregister(self, fd):
            pass
    
    def init():
        pass
    
    def shutdown():
        pass
    
    def is_initialized():
        return False


__all__ = [
    'NativeSocket',
    'NativeEventLoop',
    'init',
    'shutdown',
    'is_initialized',
    '_HAS_NATIVE_IO',
]
