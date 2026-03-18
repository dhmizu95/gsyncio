#!/usr/bin/env python3
"""
Compare asyncio vs gsyncio vs Go coroutines performance
"""

import subprocess
import sys
import math

def run_benchmark(name, command):
    """Run a benchmark and return parsed results"""
    print(f"\nRunning {name} benchmark...")
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=60,
            cwd=".",
            shell=isinstance(command, str)
        )
        if result.returncode != 0:
            print(f"  Error running {name}: {result.stderr}")
            return {}
        return parse_results(result.stdout)
    except Exception as e:
        print(f"  Exception running {name}: {e}")
        return {}

def parse_results(output):
    """Parse benchmark results from output"""
    results = {}
    lines = output.split('\n')

    # Look for benchmark sections
    current_section = None
    for line in lines:
        line = line.strip()

        # Check for benchmark section headers
        if 'Task Spawn Benchmark' in line:
            current_section = 'task_spawn'
        elif 'Sleep Benchmark' in line or 'Goroutine Sleep Benchmark' in line:
            current_section = 'sleep'
        elif 'WaitGroup Benchmark' in line:
            current_section = 'waitgroup'
        elif 'Context Switch Benchmark' in line:
            current_section = 'context_switch'

        # Look for total time in the current section
        if current_section and 'Total time:' in line:
            try:
                if ':' in line:
                    time_part = line.split(':')[1].strip()
                    if 'ms' in time_part:
                        time_str = time_part.replace('ms', '').strip()
                        results[current_section] = float(time_str)
                    elif 'µs' in time_part:
                        time_str = time_part.replace('µs', '').strip()
                        results[current_section] = float(time_str) / 1000.0  # Convert to ms
            except (ValueError, IndexError):
                pass

    return results

def main():
    print("=" * 100)
    print("Performance Comparison: asyncio vs gsyncio vs Go Coroutines")
    print("=" * 100)
    print()

    # Run benchmarks
    asyncio_results = run_benchmark("asyncio", [sys.executable, "benchmark_asyncio.py"])
    gsyncio_results = run_benchmark("gsyncio", [sys.executable, "benchmark.py"])
    go_results = run_benchmark("Go", ["./benchmark_go"])

    # Display comparison
    print("\n" + "=" * 100)
    print("RESULTS COMPARISON")
    print("=" * 100)
    print(f"{'Benchmark':<25} {'asyncio (ms)':<12} {'gsyncio (ms)':<12} {'Go (ms)':<12} {'vs Go':<10}")
    print("-" * 100)

    benchmarks = [
        ('Task Spawn (1000)', 'task_spawn'),
        ('Sleep (100)', 'sleep'),
        ('WaitGroup (10×100)', 'waitgroup'),
        ('Context Switch (10000)', 'context_switch')
    ]

    slowdowns_asyncio = []
    slowdowns_gsyncio = []

    for name, key in benchmarks:
        py_time = asyncio_results.get(key, 0)
        gs_time = gsyncio_results.get(key, 0)
        go_time = go_results.get(key, 0)

        if go_time > 0:
            asyncio_slowdown = py_time / go_time
            gsyncio_slowdown = gs_time / go_time
            slowdowns_asyncio.append(asyncio_slowdown)
            slowdowns_gsyncio.append(gsyncio_slowdown)
            print(f"{name:<25} {py_time:<12.2f} {gs_time:<12.2f} {go_time:<12.2f} {asyncio_slowdown:.1f}x / {gsyncio_slowdown:.1f}x")
        else:
            print(f"{name:<25} {py_time:<12.2f} {gs_time:<12.2f} {go_time:<12.2f} {'N/A':<10}")

    print("-" * 100)

    # Calculate geometric means
    if slowdowns_asyncio:
        geo_mean_asyncio = math.exp(sum(math.log(s) for s in slowdowns_asyncio) / len(slowdowns_asyncio))
        print(f"{'Geometric Mean (vs Go)':<25} {'-':<12} {'-':<12} {'-':<12} {geo_mean_asyncio:.1f}x (asyncio)")
    if slowdowns_gsyncio:
        geo_mean_gsyncio = math.exp(sum(math.log(s) for s in slowdowns_gsyncio) / len(slowdowns_gsyncio))
        print(f"{'Geometric Mean (vs Go)':<25} {'-':<12} {'-':<12} {'-':<12} {geo_mean_gsyncio:.1f}x (gsyncio)")

    print("\n" + "=" * 100)
    print("ANALYSIS")
    print("=" * 100)
    print("1. Task Spawn: Go is fastest due to lightweight goroutines; asyncio is slowest due to Task object overhead")
    print("2. Sleep: Go is fastest due to efficient runtime scheduler")
    print("3. WaitGroup: Go is fastest due to optimized synchronization primitives")
    print("4. Context Switch: gsyncio can be faster due to C fibers, asyncio is slowest")
    print()
    print("Performance Ranking (Fastest to Slowest):")
    print("  1. Go (native runtime, assembly-optimized)")
    print("  2. gsyncio (C fibers with Cython - requires compiled extension)")
    print("  3. asyncio (single-threaded event loop, pure Python)")
    print()
    print("Key Differences:")
    print("- Go: M:N:P scheduling, assembly-optimized, no GIL")
    print("- gsyncio: M:N fibers with work-stealing, C extension, Python GIL applies")
    print("- asyncio: 1:M scheduling, single-threaded, pure Python, Python GIL applies")
    print()
    print("Recommendations:")
    print("- Use Go for maximum performance and true parallelism")
    print("- Use gsyncio for Go-like concurrency in Python (with C extension)")
    print("- Use asyncio for standard Python async applications with rich ecosystem")

    # Save results to file
    with open('benchmark_comparison_all.txt', 'w') as f:
        f.write("asyncio vs gsyncio vs Go Benchmark Comparison\n")
        f.write("=" * 60 + "\n\n")
        f.write(f"Python: {sys.version}\n")
        f.write(f"Go: {go_results.get('version', 'Unknown')}\n\n")
        f.write("Results (in milliseconds):\n")
        f.write("-" * 60 + "\n")
        for name, key in benchmarks:
            py_time = asyncio_results.get(key, 0)
            gs_time = gsyncio_results.get(key, 0)
            go_time = go_results.get(key, 0)
            f.write(f"{name}:\n")
            f.write(f"  asyncio: {py_time:.2f}ms\n")
            f.write(f"  gsyncio: {gs_time:.2f}ms\n")
            f.write(f"  Go:      {go_time:.2f}ms\n")
            if go_time > 0:
                f.write(f"  vs Go:   asyncio={py_time/go_time:.1f}x, gsyncio={gs_time/go_time:.1f}x\n\n")

    print(f"\nDetailed results saved to: benchmarks/benchmark_comparison_all.txt")

if __name__ == '__main__':
    main()
