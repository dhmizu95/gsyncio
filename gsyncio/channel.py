"""
gsyncio.channel - Channel-based communication between fibers

This module provides typed message-passing channels for communication
between concurrent tasks (like Go channels).

Usage:
    import gsyncio as gs
    
    async def worker(chan_id, send_chan, recv_chan):
        data = await gs.recv(recv_chan)
        processed = f"Worker {chan_id}: {data}"
        await gs.send(send_chan, processed)
    
    async def main():
        recv_chan = gs.chan(10)  # Buffered channel
        send_chan = gs.chan(10)
        
        # Spawn workers
        for i in range(5):
            gs.task(worker, i, send_chan, recv_chan)
        
        # Send data
        for i in range(5):
            await gs.send(recv_chan, f"Task {i}")
        
        # Receive results
        for i in range(5):
            result = await gs.recv(send_chan)
            print(result)
    
    gs.run(main)
"""

from typing import Any, Optional, Generic, TypeVar
from .core import Channel as _Channel

T = TypeVar('T')


class Chan(Generic[T]):
    """
    Typed channel for message passing between fibers.
    
    Args:
        capacity: Buffer capacity (0 = unbuffered/synchronous)
    """
    
    def __init__(self, capacity: int = 0):
        self._channel = _Channel(capacity)
    
    @property
    def capacity(self) -> int:
        """Channel buffer capacity"""
        return self._channel.capacity
    
    @property
    def size(self) -> int:
        """Current number of items in buffer"""
        return self._channel.size
    
    @property
    def closed(self) -> bool:
        """Check if channel is closed"""
        return self._channel.closed
    
    async def send(self, value: T) -> None:
        """
        Send a value to the channel.
        
        Blocks if buffer is full (for buffered channels) or until
        a receiver is ready (for unbuffered channels).
        
        Args:
            value: Value to send
        
        Raises:
            RuntimeError: If channel is closed
        """
        await self._channel.send(value)
    
    async def recv(self) -> T:
        """
        Receive a value from the channel.
        
        Blocks until a value is available or channel is closed.
        
        Returns:
            Received value
        
        Raises:
            StopAsyncIteration: If channel is closed and empty
        """
        return await self._channel.recv()

    def __aiter__(self):
        return self

    async def __anext__(self) -> T:
        """For 'async for' iteration (equivalent to Go's for range)"""
        try:
            return await self.recv()
        except StopAsyncIteration:
            raise
    
    def send_nowait(self, value: T) -> bool:
        """
        Try to send without blocking.
        
        Args:
            value: Value to send
        
        Returns:
            True if sent successfully, False if would block
        """
        return self._channel.send_nowait(value)
    
    def recv_nowait(self) -> Optional[T]:
        """
        Try to receive without blocking.
        
        Returns:
            Received value, or None if would block
        """
        return self._channel.recv_nowait()
    
    def close(self) -> None:
        """Close the channel"""
        self._channel.close()
    
    def __len__(self) -> int:
        return self._channel.size
    
    def __bool__(self) -> bool:
        return not self._channel.closed


def chan(capacity: int = 0) -> Chan:
    """
    Create a new channel.
    
    Args:
        capacity: Buffer capacity (0 = unbuffered/synchronous)
    
    Returns:
        New channel
    """
    return Chan(capacity)


async def send(ch: Chan[T], value: T) -> None:
    """
    Send a value to a channel.
    
    Args:
        ch: Channel to send to
        value: Value to send
    """
    await ch.send(value)


async def recv(ch: Chan[T]) -> T:
    """
    Receive a value from a channel.
    
    Args:
        ch: Channel to receive from
    
    Returns:
        Received value
    """
    return await ch.recv()


def close(ch: Chan) -> None:
    """
    Close a channel.
    
    Args:
        ch: Channel to close
    """
    ch.close()


__all__ = ['Chan', 'chan', 'send', 'recv', 'close']
