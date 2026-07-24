"""
gsyncio._select — Select statement for channel multiplexing.

Works with the C SelectState (fast path) or provides a pure-Python
non-blocking fallback.
"""
import asyncio
from typing import Any, Callable, Optional

from .core import _HAS_CYTHON, Channel as _CChannel
from ._channel import Chan

if _HAS_CYTHON:
    from ._gsyncio_core import SelectState as _SelectState
else:
    _SelectState = None


def _unwrap_channel(ch):
    """Get the underlying C/Python Channel from a Chan wrapper or bare Channel."""
    if isinstance(ch, Chan):
        return ch._channel
    return ch


# ── Case descriptors ──────────────────────────────────────────────────────────

class SelectCase:
    """Represents one arm of a select statement."""

    __slots__ = ("channel", "is_send", "value", "is_default", "func")

    def __init__(
        self,
        channel: Optional[Any] = None,
        is_send: bool = False,
        value: Any = None,
        is_default: bool = False,
        func: Optional[Callable] = None,
    ):
        self.channel    = channel
        self.is_send    = is_send
        self.value      = value
        self.is_default = is_default
        self.func       = func


class SelectResult:
    """Result returned by :func:`select`."""

    __slots__ = ("channel", "value", "case_index", "success")

    def __init__(
        self,
        channel: Optional[Any] = None,
        value: Any = None,
        case_index: int = -1,
        success: bool = False,
    ):
        self.channel    = channel
        self.value      = value
        self.case_index = case_index
        self.success    = success

    def __repr__(self) -> str:
        return (f"<SelectResult channel={self.channel!r} "
                f"value={self.value!r} success={self.success}>")


# ── Case constructors ─────────────────────────────────────────────────────────

def select_recv(channel) -> SelectCase:
    """Create a *receive* case for :func:`select`."""
    return SelectCase(channel=channel, is_send=False)


def select_send(channel, value: Any) -> SelectCase:
    """Create a *send* case for :func:`select`."""
    return SelectCase(channel=channel, is_send=True, value=value)


def default(func: Optional[Callable] = None) -> SelectCase:
    """Create a *default* (non-blocking) case for :func:`select`."""
    return SelectCase(is_default=True, func=func)


# ── select() ─────────────────────────────────────────────────────────────────

async def select(*cases: SelectCase) -> SelectResult:
    """
    Execute a Go-style select on multiple channel operations.

    Blocks until one case is ready, or returns immediately if a default
    case is present.

    Example::

        result = await gs.select(
            gs.select_recv(ch1),
            gs.select_recv(ch2),
            gs.default(lambda: "nothing ready"),
        )
    """
    if not cases:
        return SelectResult(success=False)

    n           = len(cases)
    default_idx = -1
    default_fn  = None

    # Find default case
    for i, case in enumerate(cases):
        if case.is_default:
            default_idx = i
            default_fn  = case.func
            break

    if _HAS_CYTHON and _SelectState is not None:
        return await _select_c(cases, n, default_idx, default_fn)
    else:
        return await _select_python(cases, n, default_idx, default_fn)


# ── C-backed select ───────────────────────────────────────────────────────────

async def _select_c(cases, n, default_idx, default_fn):
    sel = _SelectState(n)

    for i, case in enumerate(cases):
        if case.is_default:
            sel.set_default(i)
        elif case.channel is not None:
            raw = _unwrap_channel(case.channel)
            if case.is_send:
                sel.set_send(i, raw, case.value)
            else:
                sel.set_recv(i, raw)

    result = sel.execute()

    if result and result.get("success"):
        idx          = result.get("case_index", -1)
        selected_val = result.get("value")
        if 0 <= idx < n:
            case = cases[idx]
            if case.is_default and case.func:
                selected_val = case.func()
            return SelectResult(
                channel    = case.channel if not case.is_default else None,
                value      = selected_val,
                case_index = idx,
                success    = True,
            )

    # Fell through to default
    if default_idx >= 0:
        val = default_fn() if default_fn else None
        return SelectResult(channel=None, value=val,
                            case_index=default_idx, success=True)

    return SelectResult(success=False)


# ── Pure-Python select (non-blocking poll) ────────────────────────────────────

async def _select_python(cases, n, default_idx, default_fn):
    """
    Pure-Python select: try each non-default case once without blocking.
    If nothing is ready and there is a default case, run it.
    Otherwise loop with asyncio.sleep(0) until a case is ready.
    """
    while True:
        for i, case in enumerate(cases):
            if case.is_default:
                continue
            if case.channel is None:
                continue
            raw = _unwrap_channel(case.channel)
            if case.is_send:
                # Try to send without blocking
                if raw.send_nowait(case.value):
                    return SelectResult(channel=case.channel,
                                        value=case.value,
                                        case_index=i, success=True)
            else:
                # Try to receive without blocking
                val = raw.recv_nowait()
                if val is not None:
                    return SelectResult(channel=case.channel,
                                        value=val,
                                        case_index=i, success=True)

        if default_idx >= 0:
            val = default_fn() if default_fn else None
            return SelectResult(channel=None, value=val,
                                case_index=default_idx, success=True)

        await asyncio.sleep(0)   # yield and retry


__all__ = [
    "SelectCase", "SelectResult",
    "select_recv", "select_send", "default", "select",
]
