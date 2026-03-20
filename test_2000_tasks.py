"""
Test suite for gsync with large task counts

This test suite verifies that gsync can handle large numbers of concurrent tasks.
"""

import pytest
import sys
import os
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import gsyncio as gs


class TestLargeTaskCounts:
    """Tests for large concurrent task counts"""
    
    @pytest.fixture(autouse=True)
    def setup_scheduler(self):
        """Ensure fresh scheduler for each test"""
        try:
            gs.shutdown_scheduler()
        except:
            pass
        gs.init_scheduler(num_workers=4, max_fibers=100000)
        yield
        try:
            gs.shutdown_scheduler()
        except:
            pass
    
    def test_100_tasks(self):
        """Test spawning and completing 100 tasks"""
        results = []
        lock = threading.Lock()
        
        def worker(n):
            with lock:
                results.append(n)
        
        for i in range(100):
            gs.task(worker, i)
        
        gs.sync()
        
        assert len(results) == 100
        assert set(results) == set(range(100))
    
    def test_500_tasks(self):
        """Test spawning and completing 500 tasks"""
        counter = [0]
        
        def worker():
            counter[0] += 1
        
        for i in range(500):
            gs.task(worker)
        
        gs.sync()
        
        assert counter[0] == 500
    
    def test_1000_tasks(self):
        """Test spawning and completing 1000 tasks"""
        counter = [0]
        
        def worker():
            counter[0] += 1
        
        for i in range(1000):
            gs.task(worker)
        
        gs.sync()
        
        assert counter[0] == 1000
    
    def test_2000_tasks(self):
        """Test spawning and completing 2000 tasks"""
        counter = [0]
        
        def worker():
            counter[0] += 1
        
        for i in range(2000):
            gs.task(worker)
        
        gs.sync()
        
        assert counter[0] == 2000


class TestPerformance:
    """Performance tests for tasks"""
    
    def test_2000_tasks_throughput(self):
        """Test throughput of 2000 tasks"""
        count = [0]
        
        def worker():
            count[0] += 1
        
        start = time.time()
        for i in range(2000):
            gs.task(worker)
        gs.sync()
        elapsed = time.time() - start
        
        assert count[0] == 2000
        print(f"\n2000 tasks completed in {elapsed:.3f}s ({2000/elapsed:.0f} tasks/sec)")


if __name__ == '__main__':
    pytest.main([__file__, '-v', '-s'])
