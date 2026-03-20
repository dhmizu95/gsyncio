"""
gsyncio Benchmarks

This package contains comprehensive benchmarks for comparing:
- Python asyncio
- gsyncio (task/sync and async/await models)
- Go routines

Files:
- asyncio_benchmark.py    : Pure asyncio benchmarks
- gsyncio_benchmark.py    : gsyncio-specific benchmarks
- go_benchmark.go         : Go routine benchmarks
- benchmark_runner.py      : Comprehensive runner with comparison
- pytest_benchmark.py      : Pytest-benchmark compatible benchmarks

Usage:
    # Run standalone benchmarks
    python benchmarks/asyncio_benchmark.py
    python benchmarks/gsyncio_benchmark.py
    go run benchmarks/go_benchmark.go
    
    # Run comprehensive comparison
    python benchmarks/benchmark_runner.py
    python benchmarks/benchmark_runner.py --run-go  # Include Go
    
    # Run with pytest-benchmark
    pytest benchmarks/pytest_benchmark.py --benchmark-only
"""

__version__ = "0.1.0"

from .benchmark_runner import BenchmarkRunner

__all__ = ["BenchmarkRunner"]
