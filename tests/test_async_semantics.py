"""
Semantics of the async/await model: cooperative yielding, cancellation,
timeouts and the synchronization primitives.

These cover behaviour that used to be missing or wrong:
  * `await sleep(0)` returned immediately instead of yielding, so
    gathered coroutines ran strictly one after another.
  * There was no cancel() at all, and gs.run() waits for every spawned
    task - so an abandoned task kept the whole program alive.
  * wait_for() raised TimeoutError but only after the work it gave up on
    had finished anyway.
"""
import time

import pytest

import gsyncio as gs


# ── cooperative yielding ─────────────────────────────────────────────────────

def test_sleep_zero_interleaves():
    """sleep(0) must be a real yield point, not a no-op."""
    order = []

    async def worker(tag):
        for i in range(3):
            order.append(f"{tag}{i}")
            await gs.sleep(0)

    async def main():
        await gs.gather(worker("A"), worker("B"))

    gs.run(main())

    assert len(order) == 6
    # Interleaved, not A0 A1 A2 B0 B1 B2.
    assert order[0][0] != order[1][0], f"did not interleave: {order}"


def test_yield_now_interleaves():
    order = []

    async def worker(tag):
        for i in range(3):
            order.append(f"{tag}{i}")
            await gs.yield_now()

    async def main():
        await gs.gather(worker("A"), worker("B"))

    gs.run(main())
    assert order[0][0] != order[1][0], f"did not interleave: {order}"


# ── cancellation ─────────────────────────────────────────────────────────────

def test_cancel_wakes_a_sleeping_task_promptly():
    """Cancelling must not wait out the remaining sleep."""
    async def main():
        async def slow():
            await gs.sleep(5000)
            return "should not finish"

        task = gs.create_task(slow())
        await gs.sleep(20)
        t0 = time.perf_counter()
        assert task.cancel() is True
        await gs.sleep(20)
        return time.perf_counter() - t0, task.cancelled

    elapsed, cancelled = gs.run(main())
    assert cancelled is True
    # Nowhere near the 5 s the coroutine asked to sleep for.
    assert elapsed < 1.0, f"cancel took {elapsed:.3f}s"


def test_cancel_runs_finally_blocks():
    """Cancellation is raised *inside* the coroutine, so cleanup runs."""
    cleaned = []

    async def main():
        async def work():
            try:
                await gs.sleep(5000)
            finally:
                cleaned.append("cleanup ran")

        task = gs.create_task(work())
        await gs.sleep(20)
        task.cancel()
        await gs.sleep(50)

    gs.run(main())
    assert cleaned == ["cleanup ran"]


def test_cancel_is_catchable():
    caught = []

    async def main():
        async def work():
            try:
                await gs.sleep(5000)
            except gs.CancelledError:
                caught.append(True)
                raise

        task = gs.create_task(work())
        await gs.sleep(20)
        task.cancel()
        await gs.sleep(50)

    gs.run(main())
    assert caught == [True]


def test_cancel_returns_false_when_already_done():
    async def main():
        async def quick():
            return 1

        task = gs.create_task(quick())
        await task
        return task.cancel()

    assert gs.run(main()) is False


# ── timeouts ─────────────────────────────────────────────────────────────────

def test_wait_for_returns_at_the_deadline():
    """Must not block until the abandoned work finishes on its own."""
    async def main():
        async def slow():
            await gs.sleep(2000)
            return "finished"

        t0 = time.perf_counter()
        try:
            await gs.wait_for(slow(), 0.05)
            return None
        except TimeoutError:
            return time.perf_counter() - t0

    elapsed = gs.run(main())
    assert elapsed is not None, "expected TimeoutError"
    assert elapsed < 1.0, f"wait_for took {elapsed:.3f}s for a 50ms timeout"


def test_wait_for_returns_value_when_in_time():
    async def main():
        async def quick():
            await gs.sleep(5)
            return 42
        return await gs.wait_for(quick(), 5.0)

    assert gs.run(main()) == 42


# ── synchronization primitives ───────────────────────────────────────────────

def test_semaphore_bounds_concurrency():
    live = []
    peak = []

    async def main():
        sem = gs.Semaphore(3)

        async def worker():
            async with sem:
                live.append(1)
                peak.append(len(live))
                await gs.sleep(5)
                live.pop()

        await gs.gather(*[worker() for _ in range(20)])

    gs.run(main())
    assert max(peak) <= 3, f"semaphore allowed {max(peak)} concurrent"


def test_lock_gives_mutual_exclusion():
    peak = []
    live = []

    async def main():
        lock = gs.Lock()

        async def worker():
            async with lock:
                live.append(1)
                peak.append(len(live))
                await gs.sleep(2)
                live.pop()

        await gs.gather(*[worker() for _ in range(10)])

    gs.run(main())
    assert max(peak) == 1


def test_bounded_semaphore_rejects_over_release():
    async def main():
        sem = gs.BoundedSemaphore(1)
        await sem.acquire()
        sem.release()
        with pytest.raises(ValueError):
            sem.release()
        return True

    assert gs.run(main()) is True


def test_event_wakes_waiters():
    async def main():
        ev = gs.Event()
        seen = []

        async def waiter():
            await ev.wait()
            seen.append(True)

        async def setter():
            await gs.sleep(10)
            ev.set()

        await gs.gather(waiter(), waiter(), setter())
        return seen

    assert gs.run(main()) == [True, True]
