# cython: language_level=3
# cython: boundscheck=False
# cython: wraparound=False

"""
_gsyncio_native_io.pyx - Python bindings for native I/O

Provides Python access to native epoll-based I/O operations.
"""

from libc.stdlib cimport malloc, free
from libc.stdint cimport int64_t, uint64_t, int32_t, uint32_t, uint16_t
from libc.string cimport memset, strcpy, strlen
from cpython.ref cimport PyObject, Py_INCREF, Py_DECREF
from cpython.bytes cimport PyBytes_FromStringAndSize, PyBytes_AS_STRING

cdef extern from "native_io.h":
    ctypedef enum native_io_op_t:
        NATIVE_IO_READ
        NATIVE_IO_WRITE
        NATIVE_IO_ACCEPT
        NATIVE_IO_CONNECT
        NATIVE_IO_CLOSE

    ctypedef struct native_socket_t:
        int fd
        int is_server
        int is_closed

    ctypedef struct native_evloop_t:
        int epoll_fd
        int running
        int event_count

    int native_io_init()
    void native_io_shutdown()
    native_evloop_t* native_io_get_evloop()

    int native_socket_tcp()
    int native_socket_udp()
    int native_socket_bind(int fd, char* host, int port)
    int native_socket_listen(int fd, int backlog)
    int native_socket_accept(int fd, void* client_addr)
    int native_socket_connect(int fd, char* host, int port)
    ssize_t native_socket_send(int fd, void* buffer, size_t len)
    ssize_t native_socket_recv(int fd, void* buffer, size_t len)
    void native_socket_close(int fd)

    ssize_t native_io_read_async(int fd, void* buffer, size_t len)
    ssize_t native_io_write_async(int fd, void* buffer, size_t len)

    int native_evloop_run_once(native_evloop_t* evloop)
    int native_evloop_register(native_evloop_t* evloop, int fd, uint32_t events, void* user_data)
    void native_evloop_unregister(native_evloop_t* evloop, int fd)

cdef extern from "arpa/inet.h":
    char* inet_ntoa(in_addr in_)
    uint16_t ntohs(uint16_t netshort)

cdef extern from "netinet/in.h":
    ctypedef uint32_t in_addr_t
    ctypedef struct in_addr:
        in_addr_t s_addr
    ctypedef struct sockaddr_in:
        uint16_t sin_family
        uint16_t sin_port
        in_addr sin_addr
        char sin_zero[8]

# ============================================
# Python Classes
# ============================================

cdef class NativeSocket:
    """Native socket wrapper"""
    cdef int fd
    cdef bint is_server
    cdef bint is_closed
    cdef object _fileno

    def __cinit__(self):
        self.fd = -1
        self.is_server = 0
        self.is_closed = 1
        self._fileno = None

    @property
    def fileno(self):
        if self._fileno is None:
            self._fileno = self.fd
        return self._fileno

    @staticmethod
    cdef NativeSocket from_fd(int fd, bint is_server=0):
        cdef NativeSocket sock = NativeSocket()
        sock.fd = fd
        sock.is_server = is_server
        sock.is_closed = 0
        sock._fileno = fd
        return sock

    @staticmethod
    def tcp():
        """Create a TCP socket"""
        cdef NativeSocket sock = NativeSocket()
        sock.fd = native_socket_tcp()
        if sock.fd < 0:
            raise OSError("Failed to create TCP socket")
        sock.is_closed = 0
        sock._fileno = sock.fd
        return sock

    @staticmethod
    def udp():
        """Create a UDP socket"""
        cdef NativeSocket sock = NativeSocket()
        sock.fd = native_socket_udp()
        if sock.fd < 0:
            raise OSError("Failed to create UDP socket")
        sock.is_closed = 0
        sock._fileno = sock.fd
        return sock

    def bind(self, host="0.0.0.0", port=0):
        """Bind socket to address"""
        cdef bytes host_bytes = host.encode('utf-8')
        cdef char* host_ptr = host_bytes
        if native_socket_bind(self.fd, host_ptr, port) < 0:
            raise OSError(f"Failed to bind to {host}:{port}")

    def listen(self, int backlog=128):
        """Listen for connections (TCP server)"""
        if native_socket_listen(self.fd, backlog) < 0:
            raise OSError("Failed to listen")
        self.is_server = 1

    def accept(self):
        """Accept a connection (fiber-aware)"""
        cdef char[128] client_addr_buf
        cdef int client_fd
        
        memset(&client_addr_buf, 0, sizeof(client_addr_buf))

        client_fd = native_socket_accept(self.fd, <void*>&client_addr_buf)
        if client_fd < 0:
            raise OSError("Accept failed")

        cdef NativeSocket client = NativeSocket.from_fd(client_fd, 0)
        return client, ("0.0.0.0", 0)  # Simplified - address not extracted

    def connect(self, host, port):
        """Connect to remote host (fiber-aware)"""
        cdef bytes host_bytes = host.encode('utf-8')
        cdef char* host_ptr = host_bytes
        if native_socket_connect(self.fd, host_ptr, port) < 0:
            raise OSError(f"Failed to connect to {host}:{port}")

    def send(self, data):
        """Send data (fiber-aware)"""
        cdef bytes data_bytes
        if isinstance(data, str):
            data_bytes = data.encode('utf-8')
        else:
            data_bytes = data
        
        cdef const char* buf = data_bytes
        cdef size_t length = len(data_bytes)
        
        cdef ssize_t sent = native_socket_send(self.fd, buf, length)
        if sent < 0:
            raise OSError("Send failed")
        return sent

    def recv(self, size):
        """Receive data (fiber-aware)"""
        cdef char* buffer = <char*>malloc(size)
        cdef ssize_t received
        if not buffer:
            raise MemoryError()

        try:
            received = native_socket_recv(self.fd, buffer, size)
            if received < 0:
                raise OSError("Recv failed")
            if received == 0:
                return b''  # Connection closed
            return PyBytes_FromStringAndSize(buffer, received)
        finally:
            free(buffer)

    def sendall(self, data):
        """Send all data"""
        cdef bytes data_bytes
        cdef const char* buf
        cdef size_t total, sent
        cdef ssize_t n
        
        if isinstance(data, str):
            data_bytes = data.encode('utf-8')
        else:
            data_bytes = data

        buf = data_bytes
        total = len(data_bytes)
        sent = 0

        while sent < total:
            n = native_socket_send(self.fd, buf + sent, total - sent)
            if n < 0:
                raise OSError("Send failed")
            sent += n

    def recvall(self, size):
        """Receive exactly size bytes"""
        cdef char* buffer = <char*>malloc(size)
        cdef size_t received = 0
        cdef ssize_t n
        
        if not buffer:
            raise MemoryError()

        try:
            while received < size:
                n = native_socket_recv(self.fd, buffer + received, size - received)
                if n <= 0:
                    raise OSError("Connection closed")
                received += n
            return PyBytes_FromStringAndSize(buffer, size)
        finally:
            free(buffer)

    def close(self):
        """Close socket"""
        if self.fd >= 0:
            native_socket_close(self.fd)
            self.fd = -1
            self.is_closed = 1

    def __dealloc__(self):
        if self.fd >= 0:
            native_socket_close(self.fd)

    def __repr__(self):
        if self.is_closed:
            return "<NativeSocket closed>"
        return f"<NativeSocket fd={self.fd} server={bool(self.is_server)}>"


cdef class NativeEventLoop:
    """Native event loop wrapper"""
    cdef native_evloop_t* evloop
    cdef bint owns_evloop

    def __cinit__(self):
        self.evloop = native_io_get_evloop()
        self.owns_evloop = 0

    @staticmethod
    def create():
        """Create a new event loop"""
        cdef NativeEventLoop loop = NativeEventLoop()
        loop.evloop = <native_evloop_t*>malloc(sizeof(native_evloop_t))
        if not loop.evloop:
            raise MemoryError()
        memset(loop.evloop, 0, sizeof(native_evloop_t))
        loop.owns_evloop = 1
        return loop

    def run_once(self, timeout_ms=100):
        """Run one iteration of the event loop"""
        if not self.evloop:
            return 0
        return native_evloop_run_once(self.evloop)

    def register(self, fd, events, user_data=None):
        """Register FD with event loop"""
        if not self.evloop:
            raise RuntimeError("Event loop not initialized")
        cdef int result = native_evloop_register(self.evloop, fd, events, <void*>user_data)
        if result < 0:
            raise OSError("Failed to register FD")

    def unregister(self, fd):
        """Unregister FD from event loop"""
        if self.evloop:
            native_evloop_unregister(self.evloop, fd)

    def __dealloc__(self):
        if self.owns_evloop and self.evloop:
            free(self.evloop)


# ============================================
# Module initialization
# ============================================

def init():
    """Initialize native I/O subsystem"""
    if native_io_init() < 0:
        raise OSError("Failed to initialize native I/O")

def shutdown():
    """Shutdown native I/O subsystem"""
    native_io_shutdown()

def is_initialized():
    """Check if native I/O is initialized"""
    return native_io_init() == 0


__all__ = [
    'NativeSocket',
    'NativeEventLoop',
    'init',
    'shutdown',
    'is_initialized',
]
