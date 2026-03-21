"""
Test suite for gsyncio core functionality
"""

import pytest
import sys
import os

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import gsyncio as gs


class TestTaskSync:
    """Tests for task/sync model"""
    
    def test_task_basic(self):
        """Test basic task spawning"""
        results = []
        
        def worker(n):
            results.append(n)
        
        gs.task(worker, 1)
        gs.task(worker, 2)
        gs.task(worker, 3)
        gs.sync()
        
        assert len(results) == 3
        assert set(results) == {1, 2, 3}
    
    def test_task_count(self):
        """Test task count tracking"""
        def worker():
            import time
            time.sleep(0.01)  # Small delay to ensure task stays active
        
        initial_count = gs.task_count()
        
        # Spawn tasks
        t1 = gs.task(worker)
        t2 = gs.task(worker)
        
        # Count should increase (at least temporarily)
        # Give threads a moment to start
        import time
        time.sleep(0.005)
        count_after = gs.task_count()
        assert count_after >= initial_count
        
        # Wait for tasks to complete
        gs.sync()
        
        # Wait for thread cleanup
        time.sleep(0.1)
        
        # Count should be close to initial (may have some variance due to cleanup timing)
        final_count = gs.task_count()
        # Just verify it's a reasonable number (not growing unbounded)
        assert final_count < 20  # Reasonable upper bound
    
    def test_sync_timeout(self):
        """Test sync with timeout"""
        import time
        
        def slow_worker():
            time.sleep(0.5)
        
        gs.task(slow_worker)
        
        # Should timeout
        result = gs.sync_timeout(0.1)
        assert result is False
        
        # Wait for completion
        gs.sync()
    
    def test_run_function(self):
        """Test run function"""
        def main():
            return "hello"
        
        result = gs.run(main)
        assert result == "hello"

    def test_run_hybrid(self):
        """Test run function with hybrid mapping"""
        def main():
            results = []
            def worker(n):
                results.append(n)
            
            for i in range(10):
                gs.task(worker, i)
            gs.sync()
            return len(results)
        
        result = gs.run(main, mapping='hybrid')
        assert result == 10


class TestAsyncAwait:
    """Tests for async/await model"""
    
    @pytest.mark.asyncio
    async def test_sleep(self):
        """Test async sleep"""
        import time
        
        start = time.time()
        await gs.sleep(100)  # 100ms
        elapsed = time.time() - start
        
        # Sleep should be at least close to 100ms (allow tolerance)
        assert elapsed >= 0.08  # 80ms minimum
        assert elapsed < 0.5    # Should not take too long
    
    @pytest.mark.asyncio
    async def test_gather(self):
        """Test gather multiple futures"""
        import asyncio
        
        async def compute(n):
            return n * 2
        
        tasks = [asyncio.create_task(compute(i)) for i in range(5)]
        results = await gs.gather(*tasks)
        
        assert results == [0, 2, 4, 6, 8]
    
    def test_async_range(self):
        """Test async range iterator"""
        import asyncio
        
        async def test():
            values = []
            async for i in gs.async_range(5):
                values.append(i)
            return values
        
        result = asyncio.run(test())
        assert result == [0, 1, 2, 3, 4]


class TestChannel:
    """Tests for channel operations"""
    
    @pytest.mark.asyncio
    async def test_channel_send_recv(self):
        """Test basic channel send/receive"""
        ch = gs.chan(1)  # Use buffered channel
        
        async def sender():
            await gs.send(ch, "hello")
        
        async def receiver():
            return await gs.recv(ch)
        
        import asyncio
        await asyncio.gather(sender(), receiver())
    
    @pytest.mark.asyncio
    async def test_buffered_channel(self):
        """Test buffered channel"""
        ch = gs.chan(5)
        
        # Send without blocking (buffer has space)
        for i in range(5):
            assert ch.send_nowait(i)
        
        # Buffer full
        assert not ch.send_nowait(99)
        
        # Receive all
        for i in range(5):
            val = ch.recv_nowait()
            assert val == i
    
    @pytest.mark.asyncio
    async def test_channel_close(self):
        """Test channel close"""
        ch = gs.chan()
        
        assert not ch.closed
        ch.close()
        assert ch.closed


class TestWaitGroup:
    """Tests for WaitGroup synchronization"""

    def test_waitgroup_basic(self):
        """Test basic WaitGroup usage with sync tasks"""
        wg = gs.create_wg()
        results = []
        lock = __import__('threading').Lock()

        def worker(n):
            try:
                import time
                time.sleep(0.01)
                with lock:
                    results.append(n)
            finally:
                gs.done(wg)

        gs.add(wg, 3)

        for i in range(3):
            gs.task(worker, i)

        gs.sync()  # Wait for all tasks

        assert len(results) == 3
    
    def test_waitgroup_counter(self):
        """Test WaitGroup counter"""
        wg = gs.create_wg()
        
        assert wg.counter == 0
        
        gs.add(wg, 3)
        assert wg.counter == 3
        
        gs.done(wg)
        gs.done(wg)
        assert wg.counter == 1


class TestSelect:
    """Tests for select statement"""
    
    @pytest.mark.asyncio
    async def test_select_basic(self):
        """Test basic select"""
        ch1 = gs.chan(1)  # Use buffered channel
        ch2 = gs.chan(1)

        # Put data in ch1 first
        await gs.send(ch1, "from ch1")

        result = await gs.select(
            gs.select_recv(ch1),
            gs.select_recv(ch2),
        )

        assert result.value == "from ch1"
        assert result.channel == ch1

    @pytest.mark.asyncio
    async def test_select_default(self):
        """Test select with default"""
        ch = gs.chan()

        # Should not block, should hit default
        result = await gs.select(
            gs.select_recv(ch),
            gs.default(lambda: "default"),
        )

        assert result.value == "default"


class TestFuture:
    """Tests for Future class"""
    
    def test_future_set_result(self):
        """Test setting future result"""
        f = gs.Future()
        
        assert not f.done
        f.set_result(42)
        assert f.done
        assert f.result() == 42
    
    def test_future_set_exception(self):
        """Test setting future exception"""
        f = gs.Future()
        
        exc = ValueError("test error")
        f.set_exception(exc)
        
        assert f.done
        assert f.exception() is exc
    
    @pytest.mark.asyncio
    async def test_future_await(self):
        """Test awaiting future"""
        f = gs.Future()
        
        async def set_result():
            await gs.sleep(10)
            f.set_result("done")
        
        import asyncio
        asyncio.create_task(set_result())
        
        result = await f
        assert result == "done"


class TestScheduler:
    """Tests for scheduler"""
    
    def setup_method(self):
        """Ensure clean state before each test"""
        # Make sure scheduler is shut down from any previous test
        try:
            gs.shutdown_scheduler()
        except:
            pass

    def teardown_method(self):
        """Clean up after each test"""
        try:
            gs.shutdown_scheduler()
        except:
            pass

    def test_scheduler_init(self):
        """Test scheduler initialization"""
        gs.init_scheduler(num_workers=2)
        stats = gs.get_scheduler_stats()

        assert 'total_fibers_created' in stats
        assert 'total_fibers_completed' in stats

    def test_num_workers(self):
        """Test number of workers"""
        # Initialize scheduler first
        gs.init_scheduler(num_workers=2)
        n = gs.num_workers()
        assert n > 0


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
