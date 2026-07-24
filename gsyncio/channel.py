from ._gsyncio_core import Channel as _CChannel
from .core import is_future
from typing import Any, Optional

class Chan:
    """Channel for sending/receiving values between fibers."""
    def __init__(self, size: int = 0):
        self._chan = _CChannel(size)
    
    async def send(self, value: Any):
        """Send a value to the channel."""
        res = self._chan.send(value)
        if is_future(res):
            await res
    
    async def recv(self) -> Any:
        """Receive a value from the channel."""
        res = self._chan.recv()
        if is_future(res):
            return await res
        return res

    def close(self):
        """Close the channel."""
        self._chan.close()

def chan(size: int = 0) -> Chan:
    """Create a new channel."""
    return Chan(size)

__all__ = ['Chan', 'chan']
