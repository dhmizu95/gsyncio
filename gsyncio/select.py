"""
gsyncio.select - Native C-based select statement for channel multiplexing

This module provides select functionality for multiplexing on
multiple channel operations (like Go's select statement).
Uses native C implementation for maximum performance.

Usage:
    import gsyncio as gs

    async def server():
        auth_chan = gs.chan(10)
        data_chan = gs.chan(10)

        while True:
            result = await gs.select(
                gs.select_recv(auth_chan),
                gs.select_send(data_chan, "response"),
                gs.default(lambda: None)
            )

            if result.channel == auth_chan:
                print(f"Auth: {result.value}")
            elif result.channel == data_chan:
                print(f"Data: {result.value}")

    gs.run(server())
"""

from typing import Any, Optional, Callable, List
from .core import _HAS_CYTHON, Channel as _CChannel

# Try to import C extension, fall back to pure Python
if _HAS_CYTHON:
    from ._gsyncio_core import SelectState, Channel
else:
    # Pure Python fallback
    SelectState = None
    Channel = _CChannel

from .channel import Chan


def _get_c_channel(ch):
    """Get underlying C channel from Chan wrapper or return as-is"""
    if isinstance(ch, Chan):
        return ch._channel
    return ch


class SelectCase:
    """Represents a single case in a select statement."""

    def __init__(self, channel: Optional[Channel] = None,
                 is_send: bool = False,
                 value: Any = None,
                 is_default: bool = False,
                 func: Optional[Callable] = None):
        self.channel = channel
        self.is_send = is_send
        self.value = value
        self.is_default = is_default
        self.func = func


class SelectResult:
    """Result of a select operation."""

    def __init__(self, channel: Optional[Any] = None,
                 value: Any = None,
                 case_index: int = -1,
                 success: bool = False):
        self.channel = channel
        self.value = value
        self.case_index = case_index
        self.success = success


def recv(channel: Channel) -> SelectCase:
    """Create a receive case for select."""
    return SelectCase(channel=channel, is_send=False)


def send(channel: Channel, value: Any) -> SelectCase:
    """Create a send case for select."""
    return SelectCase(channel=channel, is_send=True, value=value)


def default(func: Optional[Callable] = None) -> SelectCase:
    """Create a default case for select (non-blocking)."""
    return SelectCase(is_default=True, func=func)


async def select(*cases: SelectCase) -> SelectResult:
    """Execute a select statement on multiple channel operations.

    Uses native C implementation for high performance.
    Blocks until one of the cases can proceed, or returns immediately
    if a default case is present.
    """
    if not cases:
        return SelectResult(success=False)

    n = len(cases)
    sel = SelectState(n)
    default_idx = -1
    default_func = None

    for i, case in enumerate(cases):
        if case.is_default:
            sel.set_default(i)
            default_idx = i
            default_func = case.func
        elif case.channel is not None:
            c_ch = _get_c_channel(case.channel)
            if case.is_send:
                sel.set_send(i, c_ch, case.value)
            else:
                sel.set_recv(i, c_ch)

    if _HAS_CYTHON:
        result = sel.execute()
        if result and result.get('success'):
            idx = result.get('case_index', -1)
            if 0 <= idx < n:
                selected_case = cases[idx]
                if selected_case.is_default and selected_case.func:
                    default_value = selected_case.func()
                    return SelectResult(
                        channel=None,
                        value=default_value,
                        case_index=idx,
                        success=True
                    )
                return SelectResult(
                    channel=selected_case.channel if not selected_case.is_default else None,
                    value=result.get('value'),
                    case_index=idx,
                    success=True
                )
            return SelectResult(success=True, case_index=idx)
        if default_idx >= 0:
            if default_func:
                default_value = default_func()
                return SelectResult(
                    channel=None,
                    value=default_value,
                    case_index=default_idx,
                    success=True
                )
            return SelectResult(
                channel=None,
                value=None,
                case_index=default_idx,
                success=True
            )
        return SelectResult(success=False)

    return SelectResult(success=False)


__all__ = ['SelectCase', 'SelectResult', 'recv', 'send', 'default', 'select']
