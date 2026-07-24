"""
gsyncio._channel — Typed channel helpers.

Wraps the C Channel (or fallback Channel) in a cleaner Python API
that mirrors Go channels.
"""
from typing import Any, Generic, Optional, TypeVar

from .core import Channel as _CChannel, _HAS_CYTHON

T = TypeVar("T")


class Chan(Generic[T]):
    """
    Typed buffered/unbuffered channel.

    Args:
        capacity: Buffer size. 0 = unbuffered (synchronous handoff).

    Example::

        ch = Chan(5)
        ch.send_nowait(42)
        val = ch.recv_nowait()
    """

    def __init__(self, capacity: int = 0):
        self._channel = _CChannel(capacity)

    # ── Properties ────────────────────────────────────────────────────────────

    @property
    def capacity(self) -> int:
        return self._channel.capacity

    @property
    def size(self) -> int:
        return self._channel.size

    @property
    def closed(self) -> bool:
        return self._channel.closed

    # ── Async operations ──────────────────────────────────────────────────────

    async def send(self, value: T) -> None:
        """Send *value*; blocks if buffer full."""
        await self._channel.send(value)

    async def recv(self) -> T:
        """Receive the next value; blocks until available."""
        return await self._channel.recv()

    # ── Non-blocking operations ───────────────────────────────────────────────

    def send_nowait(self, value: T) -> bool:
        """Try to send without blocking. Returns True on success."""
        return self._channel.send_nowait(value)

    def recv_nowait(self) -> Optional[T]:
        """Try to receive without blocking. Returns None if empty."""
        return self._channel.recv_nowait()

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def close(self) -> None:
        """Close the channel; further sends will fail."""
        self._channel.close()

    # ── Iteration ─────────────────────────────────────────────────────────────

    def __aiter__(self):
        return self

    async def __anext__(self) -> T:
        """Async for — drains channel until closed."""
        try:
            return await self.recv()
        except StopAsyncIteration:
            raise

    def __len__(self) -> int:
        return self._channel.size

    def __bool__(self) -> bool:
        return not self._channel.closed

    def __repr__(self) -> str:
        return (f"<Chan capacity={self.capacity} size={self.size} "
                f"closed={self.closed}>")


# ── Factory functions (mirror Go's make(chan T, n)) ───────────────────────────

def chan(capacity: int = 0) -> Chan:
    """Create a channel with the given buffer *capacity*."""
    return Chan(capacity)

create_chan = chan   # alias used in README examples


# ── Functional API ────────────────────────────────────────────────────────────

async def send(ch: Chan, value: Any) -> None:
    """Send *value* to *ch*."""
    await ch.send(value)


async def recv(ch: Chan) -> Any:
    """Receive from *ch*."""
    return await ch.recv()


def close(ch: Chan) -> None:
    """Close *ch*."""
    ch.close()


__all__ = ["Chan", "chan", "create_chan", "send", "recv", "close"]
