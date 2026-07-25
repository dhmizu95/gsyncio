"""
Proof that gsyncio's async/await support runs without ever creating or
running an asyncio event loop.

This must run as a standalone subprocess (not via a shared pytest
process) so the patches below are guaranteed to see every call gsyncio
makes, with nothing else in the process able to interfere.

Note: `import gsyncio` does transitively import the `asyncio` module
itself (gsyncio/_select.py has an unconditional top-level `import
asyncio` - its Cython-backed select() path doesn't need it, but the
import statement is there regardless; out of scope for this change).
That's harmless and not what's being tested here - the actual claim is
that gsyncio's async/await drives coroutines on its own fiber scheduler
without ever spinning up an asyncio event loop, which is what these
patches catch.
"""
import subprocess
import sys
import textwrap


def test_gsyncio_async_await_never_runs_an_event_loop():
    script = textwrap.dedent("""
        import sys
        sys.path.insert(0, "/mnt/new_volume/Workspace/Projects/gsyncio")

        import asyncio

        # Patch every "drive a coroutine" entry point asyncio provides.
        # If gsyncio's native path is genuinely asyncio-free, none of
        # these should ever be called during the run below.
        calls = []

        def _poison(name):
            def _fn(*a, **kw):
                calls.append(name)
                raise AssertionError(f"asyncio.{name}() was called - gsyncio's native driver should never need it")
            return _fn

        asyncio.run = _poison("run")
        asyncio.new_event_loop = _poison("new_event_loop")
        asyncio.get_event_loop = _poison("get_event_loop")
        asyncio.BaseEventLoop.run_forever = _poison("BaseEventLoop.run_forever")
        asyncio.BaseEventLoop.run_until_complete = _poison("BaseEventLoop.run_until_complete")

        import gsyncio as gs

        async def fetch(x):
            await gs.sleep(1)
            return x * 2

        async def main():
            tasks = [gs.create_task(fetch(i)) for i in range(20)]
            results = await gs.gather(*tasks)

            f = gs.Future()

            async def completer():
                await gs.sleep(1)
                f.set_result("completed")

            gs.create_task(completer())
            completed = await f

            try:
                await gs.wait_for(gs.sleep(500), timeout=0.02)
                timed_out = False
            except TimeoutError:
                timed_out = True

            return results, completed, timed_out

        results, completed, timed_out = gs.run(main())
        assert results == [i * 2 for i in range(20)], results
        assert completed == "completed", completed
        assert timed_out is True, "wait_for should have timed out"
        assert calls == [], f"asyncio event-loop entry points were called: {calls}"
        print("OK")
    """)
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, (
        f"stdout:\\n{result.stdout}\\nstderr:\\n{result.stderr}"
    )
    assert result.stdout.strip() == "OK"
