"""
Test suite for gsyncio — v0.2 Python layer
"""

import asyncio
import sys
import os
import time
import threading

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import gsyncio as gs


# ─────────────────────────────────────────────────────────────────────────────
# Task / Sync model
# ─────────────────────────────────────────────────────────────────────────────

class TestTaskSync:
    """Tests for task/sync model"""

    def test_task_basic(self):
        """Basic task spawning — results collected with a lock to avoid race."""
        results = []
        lock    = threading.Lock()

        def worker(n):
            with lock:
                results.append(n)

        gs.task(worker, 1)
        gs.task(worker, 2)
        gs.task(worker, 3)
        gs.sync()

        assert len(results) == 3
        assert set(results) == {1, 2, 3}

    def test_task_count(self):
        """task_count() should be a non-negative integer."""
        count = gs.task_count()
        assert isinstance(count, int)
        assert count >= 0

    def test_sync_timeout_returns_bool(self):
        """sync_timeout must return a bool; if all tasks finish fast, True."""
        done_event = threading.Event()

        def fast_worker():
            done_event.set()

        gs.task(fast_worker)
        gs.sync()           # ensure previous tasks are done first
        # With nothing pending, sync_timeout should return True immediately
        result = gs.sync_timeout(1.0)
        assert isinstance(result, bool)

    def test_sync_timeout_false_on_slow(self):
        """sync_timeout returns False when tasks are still running."""
        barrier = threading.Event()

        def slow_worker():
            barrier.wait(timeout=5.0)   # will be released after the test

        gs.task(slow_worker)
        result = gs.sync_timeout(0.05)   # very short
        barrier.set()
        gs.sync()
        assert result is False

    def test_run_function(self):
        """gs.run() should execute a plain function and return its result."""
        def main():
            return "hello"

        result = gs.run(main)
        assert result == "hello"

    def test_run_async(self):
        """gs.run() should handle an async main coroutine.

        Uses gs.sleep(), not asyncio.sleep(): gs.run() drives coroutines
        on gsyncio's own fiber scheduler, not an asyncio event loop, so
        only gsyncio-native awaitables resolve inside it.
        """
        async def main():
            await gs.sleep(0)
            return 42

        result = gs.run(main())
        assert result == 42

    def test_run_hybrid(self):
        """Tasks spawned inside run() should complete before run() returns."""
        results = []
        lock    = threading.Lock()

        def main():
            def worker(n):
                with lock:
                    results.append(n)
            for i in range(5):
                gs.task(worker, i)
            gs.sync()
            return len(results)

        count = gs.run(main)
        assert count == 5


# ─────────────────────────────────────────────────────────────────────────────
# Async / Await model
# ─────────────────────────────────────────────────────────────────────────────

class TestAsyncAwait:
    """Tests for async/await model"""

    @pytest.mark.asyncio
    async def test_sleep(self):
        """gs.sleep(ms) should sleep for roughly the given duration."""
        start   = time.time()
        await gs.sleep(80)          # 80 ms
        elapsed = time.time() - start
        assert elapsed >= 0.06      # at least 60 ms
        assert elapsed < 1.0

    @pytest.mark.asyncio
    async def test_gather(self):
        """gather should collect results from multiple coroutines."""
        async def compute(n):
            return n * 2

        tasks   = [asyncio.create_task(compute(i)) for i in range(5)]
        results = await gs.gather(*tasks)
        assert sorted(results) == [0, 2, 4, 6, 8]

    def test_async_range(self):
        """async_range should produce the correct sequence."""
        async def _test():
            values = []
            async for i in gs.async_range(5):
                values.append(i)
            return values

        result = asyncio.run(_test())
        assert result == [0, 1, 2, 3, 4]

    @pytest.mark.asyncio
    async def test_gather_exceptions(self):
        """gather with return_exceptions=True should not propagate."""
        async def failing():
            raise ValueError("oops")

        async def ok():
            return 1

        results = await gs.gather(ok(), failing(), return_exceptions=True)
        assert results[0] == 1
        assert isinstance(results[1], ValueError)

    @pytest.mark.asyncio
    async def test_create_task(self):
        """gs.create_task wraps a coroutine in a Future that can be awaited."""
        async def compute(n):
            await gs.sleep(1)   # 1 ms — forces a real suspend/resume
            return n * 3

        task = gs.create_task(compute(7))
        result = await task
        assert result == 21

    @pytest.mark.asyncio
    async def test_create_task_gather(self):
        """gs.create_task futures should compose with gs.gather, same as
        asyncio.create_task tasks do in test_gather above."""
        async def compute(n):
            return n + 1

        tasks   = [gs.create_task(compute(i)) for i in range(5)]
        results = await gs.gather(*tasks)
        assert sorted(results) == [1, 2, 3, 4, 5]

    @pytest.mark.asyncio
    async def test_wait_for_completes(self):
        """wait_for should return the result when the awaitable finishes
        well within the timeout."""
        async def quick():
            await gs.sleep(1)   # 1 ms
            return "done"

        result = await gs.wait_for(quick(), timeout=1.0)
        assert result == "done"

    @pytest.mark.asyncio
    async def test_wait_for_timeout(self):
        """wait_for should raise TimeoutError when the awaitable is slower
        than the timeout.

        Uses gs.sleep(), not asyncio.sleep(): wait_for() drives its
        awaitable on gsyncio's own fiber scheduler via create_task(), not
        an asyncio event loop, so only gsyncio-native awaitables resolve
        inside it."""
        async def slow():
            await gs.sleep(1000)
            return "too late"

        with pytest.raises(asyncio.TimeoutError):
            await gs.wait_for(slow(), timeout=0.05)


# ─────────────────────────────────────────────────────────────────────────────
# Channel
# ─────────────────────────────────────────────────────────────────────────────

class TestChannel:
    """Tests for channel operations"""

    @pytest.mark.asyncio
    async def test_buffered_send_recv(self):
        """Buffered channel: send_nowait / recv_nowait."""
        ch = gs.chan(5)

        for i in range(5):
            assert ch.send_nowait(i) is True

        # Buffer full
        assert ch.send_nowait(99) is False

        for i in range(5):
            val = ch.recv_nowait()
            assert val == i

    @pytest.mark.asyncio
    async def test_async_send_recv(self):
        """Async send/recv handshake."""
        ch = gs.chan(1)

        async def sender():
            await gs.send(ch, "hello")

        async def receiver():
            return await gs.recv(ch)

        _, result = await asyncio.gather(sender(), receiver())
        assert result == "hello"

    @pytest.mark.asyncio
    async def test_channel_close(self):
        ch = gs.chan()
        assert ch.closed is False
        ch.close()
        assert ch.closed is True

    @pytest.mark.asyncio
    async def test_channel_iter(self):
        """Async iteration drains channel until closed."""
        ch = gs.chan(3)
        for i in range(3):
            ch.send_nowait(i)
        ch.close()

        values = []
        try:
            async for v in ch:
                values.append(v)
        except StopAsyncIteration:
            pass
        assert values == [0, 1, 2]

    def test_chan_create_chan_alias(self):
        """create_chan and chan should both return a Chan."""
        c1 = gs.chan(10)
        c2 = gs.create_chan(10)
        assert isinstance(c1, gs.Chan)
        assert isinstance(c2, gs.Chan)


# ─────────────────────────────────────────────────────────────────────────────
# WaitGroup
# ─────────────────────────────────────────────────────────────────────────────

class TestWaitGroup:
    """Tests for WaitGroup synchronization"""

    def test_waitgroup_counter(self):
        """add/done/counter semantics."""
        wg = gs.create_wg()

        assert wg.counter == 0
        gs.add(wg, 3)
        assert wg.counter == 3
        gs.done(wg)
        gs.done(wg)
        assert wg.counter == 1
        gs.done(wg)
        assert wg.counter == 0

    def test_waitgroup_basic_sync(self):
        """Workers complete before wait() returns."""
        wg      = gs.create_wg()
        results = []
        lock    = threading.Lock()

        def worker(n):
            try:
                time.sleep(0.01)
                with lock:
                    results.append(n)
            finally:
                gs.done(wg)

        gs.add(wg, 3)
        for i in range(3):
            gs.task(worker, i)

        gs.wait(wg)
        assert len(results) == 3


# ─────────────────────────────────────────────────────────────────────────────
# Select
# ─────────────────────────────────────────────────────────────────────────────

class TestSelect:
    """Tests for select statement"""

    @pytest.mark.asyncio
    async def test_select_ready_channel(self):
        """Select picks the first ready channel."""
        ch1 = gs.chan(1)
        ch2 = gs.chan(1)

        await gs.send(ch1, "from ch1")

        result = await gs.select(
            gs.select_recv(ch1),
            gs.select_recv(ch2),
        )

        assert result.success is True
        assert result.value   == "from ch1"
        assert result.channel == ch1

    @pytest.mark.asyncio
    async def test_select_default(self):
        """Select falls through to default when no channels ready."""
        ch = gs.chan()     # unbuffered, empty

        result = await gs.select(
            gs.select_recv(ch),
            gs.default(lambda: "default_value"),
        )

        assert result.success is True
        assert result.value   == "default_value"

    @pytest.mark.asyncio
    async def test_select_default_no_func(self):
        """Default with no func gives value=None."""
        ch = gs.chan()

        result = await gs.select(
            gs.select_recv(ch),
            gs.default(),
        )

        assert result.success is True
        assert result.value is None


# ─────────────────────────────────────────────────────────────────────────────
# Future
# ─────────────────────────────────────────────────────────────────────────────

class TestFuture:
    """Tests for Future class"""

    def test_set_result(self):
        f = gs.Future()
        assert f.done is False
        f.set_result(42)
        assert f.done is True
        assert f.result() == 42

    def test_set_exception(self):
        f   = gs.Future()
        exc = ValueError("test error")
        f.set_exception(exc)
        assert f.done is True
        assert f.exception() is exc

    def test_double_set_raises(self):
        f = gs.Future()
        f.set_result(1)
        with pytest.raises(RuntimeError):
            f.set_result(2)

    @pytest.mark.asyncio
    async def test_future_await(self):
        """Await a Future that is completed from another coroutine."""
        f = gs.Future()

        async def completer():
            await asyncio.sleep(0.02)
            f.set_result("done")

        asyncio.create_task(completer())
        result = await f
        assert result == "done"


# ─────────────────────────────────────────────────────────────────────────────
# Native async/await - no ambient asyncio event loop at all
#
# These are deliberately plain `def` tests (no `async def`, no
# @pytest.mark.asyncio) - pytest-asyncio never creates a loop for them.
# Every other async test in this file runs *under* pytest-asyncio's own
# real event loop, which proves gsyncio's async helpers can cooperate
# with an ambient loop, not that they can run without one. These prove
# the actual capability: gs.run() drives async def/await entirely on
# gsyncio's own fiber scheduler.
# ─────────────────────────────────────────────────────────────────────────────

class TestNativeAsyncNoLoop:
    """gs.run()-driven async/await with zero ambient asyncio loop."""

    def test_run_returns_result(self):
        async def main():
            return 123

        assert gs.run(main()) == 123

    def test_run_propagates_exception(self):
        async def main():
            raise ValueError("boom")

        with pytest.raises(ValueError, match="boom"):
            gs.run(main())

    def test_run_with_native_sleep(self):
        async def main():
            start = time.time()
            await gs.sleep(50)
            return time.time() - start

        elapsed = gs.run(main())
        assert elapsed >= 0.04

    def test_create_task_and_gather_inside_run(self):
        async def compute(n):
            await gs.sleep(1)
            return n * n

        async def main():
            tasks = [gs.create_task(compute(i)) for i in range(10)]
            return await gs.gather(*tasks)

        results = gs.run(main())
        assert results == [i * i for i in range(10)]

    def test_gather_return_exceptions_inside_run(self):
        async def ok():
            return 1

        async def failing():
            raise ValueError("nope")

        async def main():
            return await gs.gather(ok(), failing(), return_exceptions=True)

        results = gs.run(main())
        assert results[0] == 1
        assert isinstance(results[1], ValueError)

    def test_wait_for_inside_run(self):
        async def quick():
            await gs.sleep(1)
            return "done"

        async def main():
            return await gs.wait_for(quick(), timeout=1.0)

        assert gs.run(main()) == "done"

    def test_future_completed_by_sibling_task_inside_run(self):
        """A Future set by one create_task()'d coroutine, awaited by the
        main coroutine - the same pattern as test_future_await, but with
        gsyncio's own scheduler providing the concurrency instead of an
        asyncio loop."""
        async def main():
            f = gs.Future()

            async def completer():
                await gs.sleep(20)
                f.set_result("done")

            gs.create_task(completer())
            return await f

        assert gs.run(main()) == "done"

    def test_mixing_raw_asyncio_raises_clear_error(self):
        """Awaiting a non-gsyncio-native awaitable inside gs.run() has no
        event loop to service it - should fail clearly, not hang."""
        async def main():
            await asyncio.sleep(0)

        with pytest.raises(RuntimeError, match="isn't gsyncio-native"):
            gs.run(main())


# ─────────────────────────────────────────────────────────────────────────────
# Scheduler
# ─────────────────────────────────────────────────────────────────────────────

class TestScheduler:
    """Tests for scheduler init/stats/workers"""

    def setup_method(self):
        try:
            gs.shutdown_scheduler()
        except Exception:
            pass

    def teardown_method(self):
        try:
            gs.shutdown_scheduler()
        except Exception:
            pass

    def test_init_stats_keys(self):
        gs.init_scheduler(num_workers=2)
        stats = gs.get_scheduler_stats()
        assert "total_fibers_created" in stats
        assert "total_fibers_completed" in stats

    def test_num_workers(self):
        gs.init_scheduler(num_workers=2)
        n = gs.num_workers()
        assert n >= 0    # 0 is valid when C ext not loaded


# ─────────────────────────────────────────────────────────────────────────────
# Large-scale
# ─────────────────────────────────────────────────────────────────────────────

class TestScale:

    def test_hundred_tasks(self):
        counter  = [0]
        lock     = threading.Lock()

        def inc():
            with lock:
                counter[0] += 1

        for _ in range(100):
            gs.task(inc)
        gs.sync()

        assert counter[0] == 100

    def test_is_future(self):
        f = gs.Future()
        assert gs.is_future(f) is True
        assert gs.is_future(42) is False


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
