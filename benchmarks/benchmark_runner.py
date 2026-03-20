#!/usr/bin/env python3
"""
Comprehensive Benchmark Runner

This script runs all benchmarks (asyncio, gsyncio, Go) and generates
a comprehensive comparison report.

Usage:
    python benchmark_runner.py [--iterations N] [--tasks N,N,N] [--output FILE]
    
Options:
    --iterations N    Number of iterations per benchmark (default: 10)
    --tasks N,N,N     Comma-separated list of task counts (default: 100,1000,10000)
    --output FILE     Output file for results (default: benchmark_results.txt)
    --run-go          Include Go benchmarks (requires Go installed)
    --quick           Run quick benchmarks only (fewer iterations)
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from typing import Dict, List, Any

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gsyncio as gs


class BenchmarkRunner:
    """Comprehensive benchmark runner."""
    
    def __init__(self, iterations: int = 10, task_counts: List[int] = None):
        self.iterations = iterations
        self.task_counts = task_counts or [100, 1000, 10000]
        self.results = {}
        self.start_time = None
        self.end_time = None
    
    def print_header(self, title: str):
        """Print a formatted header."""
        print("\n" + "=" * 60)
        print(f" {title}")
        print("=" * 60)
    
    def print_section(self, title: str):
        """Print a formatted section."""
        print(f"\n--- {title} ---")
    
    def run_asyncio_benchmarks(self):
        """Run Python asyncio benchmarks."""
        self.print_section("Python asyncio")
        
        import asyncio
        
        # Task spawn benchmark
        async def simple_task(n):
            return sum(range(n))
        
        for num_tasks in self.task_counts:
            key = f"asyncio_task_spawn_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                start = time.perf_counter()
                tasks = [asyncio.create_task(simple_task(100)) for _ in range(num_tasks)]
                asyncio.get_event_loop().run_until_complete(asyncio.gather(*tasks))
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Task spawn ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Sleep benchmark
        for num_tasks in self.task_counts:
            key = f"asyncio_sleep_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                start = time.perf_counter()
                tasks = [asyncio.create_task(asyncio.sleep(0.01)) for _ in range(num_tasks)]
                asyncio.get_event_loop().run_until_complete(asyncio.gather(*tasks))
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Sleep ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Gather benchmark
        for num_tasks in self.task_counts:
            key = f"asyncio_gather_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                start = time.perf_counter()
                asyncio.get_event_loop().run_until_complete(
                    asyncio.gather(*[simple_task(100) for _ in range(num_tasks)])
                )
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Gather ({num_tasks} tasks): {avg_time:.4f}s")
    
    def run_gsyncio_benchmarks(self):
        """Run gsyncio benchmarks."""
        self.print_section("gsyncio")
        
        # Task/Sync model - task spawn
        def simple_sync_task(n):
            return sum(range(n))
        
        for num_tasks in self.task_counts:
            key = f"gsyncio_task_spawn_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                for _ in range(num_tasks):
                    gs.task(simple_sync_task, 100)
                
                start = time.perf_counter()
                gs.sync()
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Task spawn ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Task/Sync model - batch
        for num_tasks in self.task_counts:
            key = f"gsyncio_task_batch_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                tasks = [(simple_sync_task, (100,)) for _ in range(num_tasks)]
                
                start = time.perf_counter()
                gs.task_batch(tasks)
                gs.sync()
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Task batch ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Task/Sync model - fast
        for num_tasks in self.task_counts:
            key = f"gsyncio_task_fast_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                start = time.perf_counter()
                for _ in range(num_tasks):
                    gs.task_fast(simple_sync_task, 100)
                gs.sync()
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Task fast ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Async/Await model
        async def simple_async_task(n):
            return sum(range(n))
        
        for num_tasks in self.task_counts:
            key = f"gsyncio_async_spawn_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                async def run_tasks():
                    tasks = [gs.create_task(simple_async_task(100)) for _ in range(num_tasks)]
                    return await gs.gather(*tasks)
                
                start = time.perf_counter()
                gs.run(run_tasks())
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Async spawn ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Async/Await - sleep
        async def sleep_task(delay_ms):
            await gs.sleep(delay_ms)
            return delay_ms
        
        for num_tasks in self.task_counts:
            key = f"gsyncio_async_sleep_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                async def run_tasks():
                    tasks = [gs.create_task(sleep_task(10)) for _ in range(num_tasks)]
                    return await gs.gather(*tasks)
                
                start = time.perf_counter()
                gs.run(run_tasks())
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Async sleep ({num_tasks} tasks): {avg_time:.4f}s")
        
        # Async/Await - gather
        for num_tasks in self.task_counts:
            key = f"gsyncio_async_gather_{num_tasks}"
            times = []
            
            for _ in range(self.iterations):
                async def run_gather():
                    return await gs.gather(*[simple_async_task(100) for _ in range(num_tasks)])
                
                start = time.perf_counter()
                gs.run(run_gather())
                end = time.perf_counter()
                times.append(end - start)
            
            avg_time = sum(times) / len(times)
            self.results[key] = avg_time
            print(f"  Async gather ({num_tasks} tasks): {avg_time:.4f}s")
    
    def run_go_benchmarks(self) -> bool:
        """Run Go benchmarks if Go is available."""
        self.print_section("Go Routines")
        
        # Check if Go is installed
        try:
            result = subprocess.run(["go", "version"], capture_output=True, text=True)
            if result.returncode != 0:
                print("  Go not installed, skipping Go benchmarks")
                return False
            print(f"  Found: {result.stdout.strip()}")
        except FileNotFoundError:
            print("  Go not found, skipping Go benchmarks")
            return False
        
        # Run Go benchmark
        go_benchmark_file = os.path.join(os.path.dirname(__file__), "go_benchmark.go")
        
        if not os.path.exists(go_benchmark_file):
            print(f"  Go benchmark file not found: {go_benchmark_file}")
            return False
        
        print("  Running Go benchmarks...")
        
        # Run Go benchmark with output parsing
        try:
            result = subprocess.run(
                ["go", "run", go_benchmark_file],
                capture_output=True,
                text=True,
                timeout=300  # 5 minute timeout
            )
            
            if result.returncode == 0:
                # Parse Go output
                for line in result.stdout.split("\n"):
                    if "Task spawn" in line or "Sleep" in line or "Gather" in line or "Chain" in line or "Batch" in line or "Mutex" in line or "Atomic" in line or "Channel" in line:
                        print(f"    {line.strip()}")
                
                # Try to parse key metrics from output
                self._parse_go_results(result.stdout)
                return True
            else:
                print(f"  Go benchmark failed: {result.stderr}")
                return False
        except subprocess.TimeoutExpired:
            print("  Go benchmark timed out")
            return False
        except Exception as e:
            print(f"  Error running Go benchmark: {e}")
            return False
    
    def _parse_go_results(self, output: str):
        """Parse Go benchmark results from output."""
        # This is a simplified parser - in production, you'd want more robust parsing
        import re
        
        # Extract timing results
        patterns = {
            "go_task_spawn_100": r"Task spawn \(100 tasks\): ([\d.]+)s",
            "go_task_spawn_1000": r"Task spawn \(1000 tasks\): ([\d.]+)s",
            "go_task_spawn_10000": r"Task spawn \(10000 tasks\): ([\d.]+)s",
            "go_sleep_100": r"Sleep \(100 tasks\): ([\d.]+)s",
            "go_sleep_1000": r"Sleep \(1000 tasks\): ([\d.]+)s",
            "go_sleep_10000": r"Sleep \(10000 tasks\): ([\d.]+)s",
            "go_gather_100": r"Gather \(100 tasks\): ([\d.]+)s",
            "go_gather_1000": r"Gather \(1000 tasks\): ([\d.]+)s",
            "go_gather_5000": r"Gather \(5000 tasks\): ([\d.]+)s",
        }
        
        for key, pattern in patterns.items():
            match = re.search(pattern, output)
            if match:
                self.results[key] = float(match.group(1))
    
    def generate_comparison_report(self) -> str:
        """Generate a comparison report between all frameworks."""
        report = []
        report.append("\n" + "=" * 70)
        report.append(" BENCHMARK COMPARISON REPORT")
        report.append("=" * 70)
        report.append(f"\nGenerated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"Iterations per test: {self.iterations}")
        report.append(f"Task counts tested: {self.task_counts}")
        
        # Performance comparison tables
        report.append("\n" + "-" * 70)
        report.append(" TASK SPAWN PERFORMANCE (lower is better)")
        report.append("-" * 70)
        
        for num_tasks in self.task_counts:
            asyncio_time = self.results.get(f"asyncio_task_spawn_{num_tasks}", 0)
            gsyncio_time = self.results.get(f"gsyncio_task_spawn_{num_tasks}", 0)
            gsyncio_fast_time = self.results.get(f"gsyncio_task_fast_{num_tasks}", 0)
            go_time = self.results.get(f"go_task_spawn_{num_tasks}", 0)
            
            report.append(f"\n{num_tasks} tasks:")
            report.append(f"  asyncio:     {asyncio_time:.4f}s")
            report.append(f"  gsyncio:     {gsyncio_time:.4f}s")
            report.append(f"  gsyncio fast:{gsyncio_fast_time:.4f}s")
            if go_time > 0:
                report.append(f"  Go:          {go_time:.4f}s")
            
            # Calculate speedups
            if asyncio_time > 0:
                report.append(f"  gsyncio speedup vs asyncio: {asyncio_time/gsyncio_time:.2f}x")
                if go_time > 0:
                    report.append(f"  Go speedup vs asyncio: {asyncio_time/go_time:.2f}x")
        
        report.append("\n" + "-" * 70)
        report.append(" ASYNC SLEEP PERFORMANCE (lower is better)")
        report.append("-" * 70)
        
        for num_tasks in self.task_counts:
            asyncio_time = self.results.get(f"asyncio_sleep_{num_tasks}", 0)
            gsyncio_time = self.results.get(f"gsyncio_async_sleep_{num_tasks}", 0)
            go_time = self.results.get(f"go_sleep_{num_tasks}", 0)
            
            report.append(f"\n{num_tasks} tasks (10ms sleep each):")
            report.append(f"  asyncio:  {asyncio_time:.4f}s")
            report.append(f"  gsyncio:  {gsyncio_time:.4f}s")
            if go_time > 0:
                report.append(f"  Go:       {go_time:.4f}s")
            
            if asyncio_time > 0 and gsyncio_time > 0:
                report.append(f"  gsyncio speedup vs asyncio: {asyncio_time/gsyncio_time:.2f}x")
        
        report.append("\n" + "-" * 70)
        report.append(" GATHER PERFORMANCE (lower is better)")
        report.append("-" * 70)
        
        for num_tasks in self.task_counts:
            asyncio_time = self.results.get(f"asyncio_gather_{num_tasks}", 0)
            gsyncio_time = self.results.get(f"gsyncio_async_gather_{num_tasks}", 0)
            go_time = self.results.get(f"go_gather_{num_tasks}", 0)
            
            report.append(f"\n{num_tasks} tasks:")
            report.append(f"  asyncio:  {asyncio_time:.4f}s")
            report.append(f"  gsyncio:  {gsyncio_time:.4f}s")
            if go_time > 0:
                report.append(f"  Go:       {go_time:.4f}s")
            
            if asyncio_time > 0 and gsyncio_time > 0:
                report.append(f"  gsyncio speedup vs asyncio: {asyncio_time/gsyncio_time:.2f}x")
        
        report.append("\n" + "=" * 70)
        report.append(" SUMMARY")
        report.append("=" * 70)
        
        # Calculate average speedups
        asyncio_times = [v for k, v in self.results.items() if k.startswith("asyncio_")]
        gsyncio_times = [v for k, v in self.results.items() if k.startswith("gsyncio_")]
        go_times = [v for k, v in self.results.items() if k.startswith("go_")]
        
        if asyncio_times and gsyncio_times:
            avg_asyncio = sum(asyncio_times) / len(asyncio_times)
            avg_gsyncio = sum(gsyncio_times) / len(gsyncio_times)
            report.append(f"\nAverage execution time:")
            report.append(f"  asyncio:  {avg_asyncio:.4f}s")
            report.append(f"  gsyncio:  {avg_gsyncio:.4f}s")
            report.append(f"  gsyncio speedup: {avg_asyncio/avg_gsyncio:.2f}x")
        
        if go_times:
            avg_go = sum(go_times) / len(go_times)
            report.append(f"  Go:       {avg_go:.4f}s")
        
        return "\n".join(report)
    
    def run_all(self, run_go: bool = False):
        """Run all benchmarks."""
        self.start_time = time.time()
        
        self.print_header("Comprehensive Benchmark Suite")
        print(f"\nIterations: {self.iterations}")
        print(f"Task counts: {self.task_counts}")
        
        # Run Python asyncio benchmarks
        try:
            self.run_asyncio_benchmarks()
        except Exception as e:
            print(f"Error running asyncio benchmarks: {e}")
        
        # Run gsyncio benchmarks
        try:
            self.run_gsyncio_benchmarks()
        except Exception as e:
            print(f"Error running gsyncio benchmarks: {e}")
        
        # Run Go benchmarks if requested
        if run_go:
            try:
                self.run_go_benchmarks()
            except Exception as e:
                print(f"Error running Go benchmarks: {e}")
        
        # Generate and print comparison report
        report = self.generate_comparison_report()
        print(report)
        
        self.end_time = time.time()
        total_time = self.end_time - self.start_time
        print(f"\nTotal benchmark time: {total_time:.2f}s")
        
        return self.results


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Comprehensive Benchmark Runner for asyncio vs gsyncio vs Go"
    )
    parser.add_argument(
        "--iterations", "-i",
        type=int,
        default=10,
        help="Number of iterations per benchmark (default: 10)"
    )
    parser.add_argument(
        "--tasks", "-t",
        type=str,
        default="100,1000,10000",
        help="Comma-separated list of task counts (default: 100,1000,10000)"
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default="",
        help="Output file for results (default: print to stdout)"
    )
    parser.add_argument(
        "--run-go",
        action="store_true",
        help="Include Go benchmarks (requires Go installed)"
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run quick benchmarks (fewer iterations)"
    )
    
    args = parser.parse_args()
    
    # Parse task counts
    task_counts = [int(x.strip()) for x in args.tasks.split(",")]
    
    # Adjust iterations for quick mode
    iterations = args.iterations
    if args.quick:
        iterations = max(3, iterations // 3)
    
    # Run benchmarks
    runner = BenchmarkRunner(iterations=iterations, task_counts=task_counts)
    results = runner.run_all(run_go=args.run_go)
    
    # Save results to file if requested
    if args.output:
        # Save as JSON
        with open(args.output + ".json", "w") as f:
            json.dump(results, f, indent=2)
        
        # Save as text report
        with open(args.output + ".txt", "w") as f:
            f.write(runner.generate_comparison_report())
        
        print(f"\nResults saved to {args.output}.json and {args.output}.txt")


if __name__ == "__main__":
    main()
