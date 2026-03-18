"""
gsyncio.select - Select statement for channel multiplexing

This module provides select functionality for multiplexing on
multiple channel operations (like Go's select statement).

Usage:
    import gsyncio as gs
    
    async def server():
        auth_chan = gs.chan(10)
        data_chan = gs.chan(10)
        
        while True:
            result = await gs.select(
                gs.recv(auth_chan),
                gs.recv(data_chan),
                gs.default(lambda: None)
            )
            
            if result.channel == auth_chan:
                print(f"Auth: {result.value}")
            elif result.channel == data_chan:
                print(f"Data: {result.value}")
    
    gs.run(server())
"""

from typing import Any, Optional, Callable, List
from .core import Channel


class SelectCase:
    """
    Represents a single case in a select statement.
    """

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
    """
    Result of a select operation.
    """
    
    def __init__(self, channel: Optional[Channel] = None,
                 value: Any = None,
                 case_index: int = -1,
                 success: bool = False):
        self.channel = channel
        self.value = value
        self.case_index = case_index
        self.success = success


def recv(channel: Channel) -> SelectCase:
    """
    Create a receive case for select.
    
    Args:
        channel: Channel to receive from
    
    Returns:
        SelectCase for receiving
    """
    return SelectCase(channel=channel, is_send=False)


def send(channel: Channel, value: Any) -> SelectCase:
    """
    Create a send case for select.
    
    Args:
        channel: Channel to send to
        value: Value to send
    
    Returns:
        SelectCase for sending
    """
    return SelectCase(channel=channel, is_send=True, value=value)


def default(func: Optional[Callable] = None) -> SelectCase:
    """
    Create a default case for select (non-blocking).

    Args:
        func: Optional function to call if default is selected

    Returns:
        SelectCase for default
    """
    return SelectCase(is_default=True, func=func)


async def select(*cases: SelectCase) -> SelectResult:
    """
    Execute a select statement on multiple channel operations.
    
    Blocks until one of the cases can proceed, or returns immediately
    if a default case is present.
    
    Args:
        *cases: Select cases (recv, send, or default)
    
    Returns:
        SelectResult with the result of the selected case
    """
    import asyncio
    
    if not cases:
        return SelectResult(success=False)
    
    # Check for default case
    default_case = None
    for i, case in enumerate(cases):
        if case.is_default:
            default_case = (i, case)
            break
    
    # Try each case non-blocking
    for i, case in enumerate(cases):
        if case.is_default:
            continue
        
        if case.channel is None:
            continue
        
        if not case.is_send:
            # Try receive
            value = case.channel.recv_nowait()
            if value is not None or not case.channel.closed:
                if value is not None:
                    return SelectResult(
                        channel=case.channel,
                        value=value,
                        case_index=i,
                        success=True
                    )
        else:
            # Try send
            if case.channel.send_nowait(case.value):
                return SelectResult(
                    channel=case.channel,
                    value=None,
                    case_index=i,
                    success=True
                )
    
    # If default case exists, execute it
    if default_case:
        i, case = default_case
        if case.func is not None:
            result = case.func()
        else:
            result = None
        return SelectResult(
            channel=None,
            value=result,
            case_index=i,
            success=True
        )
    
    # No case ready - need to wait
    # Create asyncio events for each channel
    async def wait_for_case(i: int, case: SelectCase) -> SelectResult:
        if case.is_default:
            return SelectResult(
                channel=None,
                value=case.func() if case.func else None,
                case_index=i,
                success=True
            )
        
        if not case.is_send:
            # Wait for receive
            try:
                value = await case.channel.recv()
                return SelectResult(
                    channel=case.channel,
                    value=value,
                    case_index=i,
                    success=True
                )
            except StopAsyncIteration:
                return SelectResult(
                    channel=case.channel,
                    value=None,
                    case_index=i,
                    success=False
                )
        else:
            # Wait for send
            await case.channel.send(case.value)
            return SelectResult(
                channel=case.channel,
                value=None,
                case_index=i,
                success=True
            )
    
    # Wait for first case to complete
    tasks = [
        asyncio.create_task(wait_for_case(i, case))
        for i, case in enumerate(cases)
    ]
    
    done, pending = await asyncio.wait(
        tasks,
        return_when=asyncio.FIRST_COMPLETED
    )
    
    # Cancel pending tasks with proper awaiting
    for task in pending:
        try:
            await asyncio.gather(task, return_exceptions=True)
        except Exception:
            pass
    
    # Get result from first completed task
    for task in done:
        try:
            return task.result()
        except Exception:
            continue
    
    return SelectResult(success=False)


__all__ = ['SelectCase', 'SelectResult', 'recv', 'send', 'default', 'select']
